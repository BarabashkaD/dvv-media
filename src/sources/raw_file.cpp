// The raw file source provider (M2 design §4.5, §5.5, §5.6, D8, D9, D19): headerless frames back to
// back. Format, size and fps come from the query, then the request's hint, then clip=@<file.json>,
// then the sidecar <file>.<ext>.json, then the file name (D5 design §4.2).
//
//   <path>[?fmt=<name>|@<file.json>][&size=WxH (or w=&h=)][&fps=][&clip=@<file.json>][&colour=]
//         [&range=]
//   <path> = <dir>/<name>_WxH[_fmt][_fps][_colour].<ext> for detection from the name
//   (file_name.h); the colour standard comes from colour=, then the clip description, then the
//   name's token; the range from range=, then the clip description (D5R §1.3).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/clip_json.h"
#include "dvv/media/file_name.h"
#include "dvv/media/format_json.h"
#include "dvv/media/frame.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/parameters.h"
#include "sources/file_read.h"
#include "sources/uri_params.h"

namespace dvv::media {

namespace {

constexpr std::array<std::string_view, 1> SCHEMES{"file"};
constexpr std::array<std::string_view, 6> OWN_PARAMETERS{"fmt", "size", "w", "h", "fps", "clip"};
constexpr auto ALLOWED = detail::withColourParameters(OWN_PARAMETERS);

constexpr Rational DEFAULT_FPS{25, 1};
// The format assumed when only the size is known.
constexpr std::string_view GUESS_FORMAT = "yuv420p";
// The smallest host frame alignment (a cache line); unbuffered reads need the reader's.
constexpr std::size_t MIN_FRAME_ALIGNMENT = 64;
constexpr std::string_view NO_SIZE =
    "no frame size: name the file <name>_WxH[_fmt] or pass ?size=WxH&fmt=";

using Size = std::pair<std::uint32_t, std::uint32_t>;

// A catalogue format by name (as the synthetic provider: unpacked and custom formats are interned
// only on use, so accepting them would make a URI open or fail depending on the process's history).
Result<FormatId> parseFormat(std::string_view name) {
    const std::optional<FormatId> format = findFormat(name);
    if (!format || std::ranges::find(catalogue(), *format) == catalogue().end()) {
        return makeError(Errc::Format, std::format("unknown pixel format '{}'", name));
    }
    return *format;
}

// A format given as fmt= text: a catalogue name or "@<pixel-format JSON>" (read here: blocking).
Result<FormatId> formatFromText(std::string_view text) {
    if (text.starts_with('@')) {
        DVV_TRY(const std::string json_text, readSmallFile(text.substr(1)));
        DVV_TRY(const json::Value value, json::parse(json_text));
        return formatFromJson(value);
    }
    return parseFormat(text);
}

// A file name without its folder, for notes and warnings.
std::string_view baseName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The request's sidecar (D5 design §3.5): its description when it parses, else the warning
// "<file>.json ignored: <reason>". Nothing with clip= (it replaces the sidecar).
struct SidecarUse {
    std::optional<ClipDescription> clip;
    std::string warning;
    std::string path;  // the sidecar's path (UTF-8); empty without a sidecar
};

// "<file>.json ignored: <reason>", with the sidecar's own path left out of the reason: the
// "clip description '<path>': " prefix is dropped and any other mention becomes the base name.
std::string ignoredWarning(std::string_view path, std::string reason) {
    const std::string prefix = std::format("clip description '{}': ", path);
    if (reason.starts_with(prefix)) {
        reason.erase(0, prefix.size());
    }
    const std::string_view name = baseName(path);
    for (std::size_t at = reason.find(path); !path.empty() && at != std::string::npos;
         at = reason.find(path, at + name.size())) {
        reason.replace(at, path.size(), name);
    }
    return std::format("{} ignored: {}", name, reason);
}

SidecarUse useSidecar(const SourceRequest& request) {
    SidecarUse use{.clip = std::nullopt, .warning = {}, .path = {}};
    if (!request.sidecar || request.uri.param("clip")) {
        return use;
    }
    const Sidecar& sidecar = *request.sidecar;
    use.path = sidecar.path;
    std::string reason = sidecar.error;
    if (reason.empty()) {
        auto parsed = parseClipJson(sidecar.text, sidecar.path);
        if (parsed) {
            use.clip = std::move(*parsed);
            return use;
        }
        reason = std::move(parsed).error().message;
    }
    use.warning = ignoredWarning(sidecar.path, std::move(reason));
    return use;
}

// The colour in use (D5R §1.3, §3.4): the standard from colour=, else the clip description's
// (clip=@ or the sidecar), else the file name's token; the range from range=, else the clip
// description's. Neither: resolveColour() gives the defaults.
ColourSpec withClipColour(ColourSpec query, const ClipDescription* clip,
                          const std::optional<FileNameInfo>& name) {
    const std::optional<ColourSpec> clip_colour = clip ? clip->colour : std::optional<ColourSpec>{};
    if (!query.preset) {
        query.preset = clip_colour ? clip_colour->preset : name ? name->colour : std::nullopt;
    }
    if (query.range == ColourRange::Unspecified && clip_colour) {
        query.range = clip_colour->range;
    }
    return query;
}

// What match() can tell without I/O. A query parameter that is present counts as given even when
// its value is bad, so that open() reports the error.
struct Known {
    bool explicitSize = false;  // from the query or the hint
    bool explicitFormat = false;
    bool clip = false;           // clip= is given (read in open())
    bool sidecar = false;        // the sidecar gave the size or the format
    bool sidecarFormat = false;  // the sidecar gave the format (maybe "@<json>")
    std::string sidecarPath;     // the sidecar's path when `sidecar` is set
    std::optional<Size> size;
    std::optional<FormatId> format;  // nullopt for fmt=@<json> and bad names
    std::optional<Rational> fps;
};

Known gather(const SourceRequest& request) {
    const Uri& uri = request.uri;
    const FormatHint hint = request.hint.value_or(FormatHint{});
    const std::optional<FileNameInfo> name = parseFileName(uri.path);
    const SidecarUse side = useSidecar(request);
    const ClipDescription* const sidecar = side.clip ? &*side.clip : nullptr;
    Known known;
    known.clip = uri.param("clip").has_value();

    if (uri.param("size") || uri.param("w") || uri.param("h")) {
        known.explicitSize = true;
        if (const auto size = detail::sizeFromQuery(uri)) {
            if (const auto& value = *size) {
                known.size = *value;
            }
        }
    } else if (hint.width && hint.height) {
        known.explicitSize = true;
        known.size = Size{*hint.width, *hint.height};
    } else if (sidecar && sidecar->size) {
        known.sidecar = true;
        known.size = sidecar->size;
    } else if (name) {
        known.size = Size{name->width, name->height};
    }

    if (const auto fmt = uri.param("fmt")) {
        known.explicitFormat = true;
        if (!fmt->starts_with('@')) {
            if (const auto format = parseFormat(*fmt)) {
                known.format = *format;
            }
        }
    } else if (hint.format) {
        known.explicitFormat = true;
        known.format = hint.format;
    } else if (sidecar && sidecar->format) {
        known.sidecar = true;
        known.sidecarFormat = true;
        if (!sidecar->format->starts_with('@')) {
            known.format = findFormat(*sidecar->format);
        }
    } else if (name && name->format) {
        known.format = name->format;
    }

    if (const auto text = uri.param("fps")) {
        if (const auto fps = detail::parseFps(*text)) {
            known.fps = *fps;
        }
    } else if (hint.fps) {
        known.fps = hint.fps;
    } else if (sidecar && sidecar->fps) {
        known.fps = sidecar->fps;
    } else if (name && name->fps) {
        known.fps = name->fps;
    }
    if (known.sidecar) {
        known.sidecarPath = side.path;
    }
    return known;
}

// A source as its request describes it, with the file open.
struct Opened {
    SourceInfo info;
    platform::FileReader reader;
    std::uint64_t frameBytes = 0;
};

// Blocking: runs on the io executor.
Result<Opened> openFile(const SourceRequest& request) {
    const Uri& uri = request.uri;
    DVV_TRY_VOID(detail::checkParameters(uri, ALLOWED));
    const FormatHint hint = request.hint.value_or(FormatHint{});
    const std::optional<FileNameInfo> name = parseFileName(uri.path);

    // The clip description: clip=@<file> (an open error when bad), else a good sidecar.
    std::optional<ClipDescription> clip;
    std::string clip_file;
    if (const auto value = uri.param("clip")) {
        if (!value->starts_with('@')) {
            return makeError(Errc::Format, "clip needs @<file.json>");
        }
        clip_file = value->substr(1);
        DVV_TRY(clip, loadClipJson(clip_file));
    }
    SidecarUse side = useSidecar(request);
    // A sidecar's "@" format that is used is read now: when it cannot be read, the sidecar is
    // ignored with a warning, as a bad sidecar (it must not block the open, §3.5).
    std::optional<FormatId> sidecar_format;
    if (side.clip && !uri.param("fmt") && !hint.format) {
        const std::optional<std::string>& text = side.clip->format;
        if (text && text->starts_with('@')) {
            auto read = formatFromText(*text);
            if (read) {
                sidecar_format = *read;
            } else {
                side.warning = ignoredWarning(side.path, std::move(read).error().message);
                side.clip.reset();
            }
        }
    }
    const bool from_sidecar = side.clip.has_value();
    if (from_sidecar) {
        clip = std::move(side.clip);
        clip_file = side.path;
    }

    // The format, and its fmt= form for the resolved URI.
    FormatId format = 0;
    std::string fmt_param;
    if (const auto fmt = uri.param("fmt")) {
        DVV_TRY(format, formatFromText(*fmt));
        fmt_param = *fmt;
    } else if (!hint.format && clip && clip->format) {
        if (sidecar_format) {
            format = *sidecar_format;
        } else {
            DVV_TRY(format, formatFromText(*clip->format));
        }
        fmt_param = *clip->format;
    } else {
        if (hint.format) {
            format = *hint.format;
        } else if (name && name->format) {
            format = *name->format;
        } else {
            DVV_TRY(format, parseFormat(GUESS_FORMAT));
        }
        fmt_param = descriptor(format).name;
    }

    DVV_TRY(const std::optional<Size> query_size, detail::sizeFromQuery(uri));
    Size size;
    if (query_size) {
        size = *query_size;
    } else if (hint.width && hint.height) {
        size = Size{*hint.width, *hint.height};
    } else if (clip && clip->size) {
        size = *clip->size;
    } else if (name) {
        size = Size{name->width, name->height};
    } else if (clip && !from_sidecar) {
        return makeError(Errc::Format, std::format("no frame size: {} has no size; name the file "
                                                   "<name>_WxH[_fmt] or pass ?size=WxH&fmt=",
                                                   clip_file));
    } else {
        return makeError(Errc::Format, std::string(NO_SIZE));
    }
    const auto [width, height] = size;

    Rational fps = DEFAULT_FPS;
    if (const auto text = uri.param("fps")) {
        DVV_TRY(fps, detail::parseFps(*text));
    } else if (hint.fps) {
        fps = *hint.fps;
    } else if (clip && clip->fps) {
        fps = *clip->fps;
    } else if (name && name->fps) {
        fps = *name->fps;
    }
    // The colour (D5R §1.3): the query, then the clip description, then the file name.
    DVV_TRY(const ColourSpec colour_spec, colourSpecFromQuery(uri));
    const ColourParams colour = merged(withClipColour(colour_spec, clip ? &*clip : nullptr, name),
                                       descriptor(format).model);
    DVV_TRY(const FrameGeometry frame, geometry(format, width, height));

    DVV_TRY(platform::FileReader reader,
            platform::FileReader::open(pathFromUtf8(uri.path), request.fileReads));
    const std::uint64_t frame_bytes = frame.bytes;
    const std::uint64_t frames = reader.size() / frame_bytes;

    Uri resolved;
    resolved.scheme = "file";
    resolved.path = uri.path;
    resolved.params = {
        {"fmt", fmt_param},
        {"size", std::format("{}x{}", width, height)},
        {"fps", detail::formatFps(fps)},
    };
    if (const auto value = uri.param("clip")) {
        resolved.params.emplace_back("clip", std::string(*value));
    }
    std::ranges::move(colourParameters(colour_spec), std::back_inserter(resolved.params));

    Opened opened{.info = {}, .reader = std::move(reader), .frameBytes = frame_bytes};
    SourceInfo& info = opened.info;
    info.desc = FrameDesc{
        .format = format, .colour = internColour(colour), .width = width, .height = height};
    info.fps = fps;
    info.frameCount = static_cast<std::int64_t>(frames);
    info.access = SourceAccess::RandomCheap;
    info.deterministic = true;
    info.resolvedUri = formatUri(resolved);
    info.provider = "raw";
    info.clipFile = clip_file;
    if (from_sidecar) {
        std::string note = std::format("{} describes {}", baseName(clip_file), baseName(uri.path));
        if (name) {
            note += " (it wins over the file name)";
        }
        info.notes.push_back(std::move(note));
    }
    if (!side.warning.empty()) {
        info.warnings.push_back(std::move(side.warning));
    }
    if (const std::uint64_t rest = opened.reader.size() % frame_bytes; rest > 0) {
        info.warnings.push_back(std::format("{} trailing bytes ignored", rest));
    }
    info.reader = opened.reader.modeText();
    return opened;
}

// produce() only reads: FileReader::readAt is thread-safe and runs one read at a time (U-IO2), so
// concurrent calls share the reader.
class RawFileSource final : public ISource, public Managed<RawFileSource> {
public:
    RawFileSource(CreationKey<SourceFactory> key, SourceInfo info, platform::FileReader reader,
                  std::uint64_t frame_bytes)
        : Managed(std::move(key)),
          info_(std::move(info)),
          path_(uriPath(info_.resolvedUri)),
          reader_(std::move(reader)),
          frame_bytes_(frame_bytes) {}

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override {
        const std::int64_t frames = info_.frameCount.value_or(0);
        if (index < 0 || index >= frames) {
            co_return makeError(Errc::Internal,
                                std::format("frame {} is outside [0, {})", index, frames));
        }
        co_await resumeOn(ctx.io());
        const std::uint32_t alignment = reader_.alignment();
        const detail::FrameRead plan = detail::planFrameRead(
            static_cast<std::uint64_t>(index) * frame_bytes_, frame_bytes_, 0, alignment);
        const std::shared_ptr<HostFrame> frame = co_try(ctx.allocateHost(
            info_.desc, plan.rangeBytes, std::max<std::size_t>(alignment, MIN_FRAME_ALIGNMENT),
            plan.dataOffset));
        co_try(detail::readFrame(reader_, plan, frame->memory(), index, path_));
        co_return ctx.publish(frame);
    }

private:
    // The UTF-8 path of the resolved URI (the path as given), for messages.
    [[nodiscard]] static std::string uriPath(std::string_view resolved_uri) {
        auto uri = parseUri(resolved_uri);
        return uri ? std::move(uri->path) : std::string(resolved_uri);
    }

    SourceInfo info_;
    std::string path_;
    platform::FileReader reader_;
    std::uint64_t frame_bytes_;
};

}  // namespace

class RawFileProvider final : public ISourceProvider, public Managed<RawFileProvider> {
public:
    RawFileProvider(CreationKey<ComponentRegistry> key, const ParameterValues& /*values*/) noexcept
        : Managed(std::move(key)) {}

    static ComponentInfo info() noexcept {
        return {.id = "raw", .name = "Raw video file", .kind = Kind::Source, .version = 1};
    }
    static const ParameterSchema& schema() {
        static const ParameterSchema SCHEMA;
        return SCHEMA;
    }
    static std::expected<void, Unavailable> probe(const Capabilities& /*caps*/) { return {}; }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        return SCHEMES;
    }

    // No I/O and no validation (open() reports the errors); the head is ignored.
    [[nodiscard]] Match match(const SourceRequest& request,
                              std::span<const std::byte> /*head*/) const override {
        if (request.uri.scheme != "file") {
            return Match{.confidence = 0, .guess = {}, .why = "not a file"};
        }
        const Known known = gather(request);
        Match match{.confidence = 0, .guess = {}, .why = std::string(NO_SIZE)};
        match.guess.format = known.format;
        if (known.size) {
            match.guess.width = known.size->first;
            match.guess.height = known.size->second;
        }
        match.guess.fps = known.fps;

        const bool has_size = known.explicitSize || known.size.has_value();
        const bool has_format =
            known.explicitFormat || known.sidecarFormat || known.format.has_value();
        if (known.explicitSize && known.explicitFormat) {
            match.confidence = 95;
            match.why = "explicit format and size";
        } else if (known.clip) {
            match.confidence = 95;
            match.why = "explicit clip description";
        } else if (has_size && has_format && known.sidecar) {
            match.confidence = 75;
            match.why = std::format("format and size from {}", baseName(known.sidecarPath));
        } else if (has_size && has_format) {
            match.confidence = 70;
            match.why = "size and format from the file name";
        } else if (has_size) {
            match.confidence = 30;
            match.why = "size only; format guessed as yuv420p";
            match.guess.format = findFormat(GUESS_FORMAT);
        }
        return match;
    }

    [[nodiscard]] Task<Result<std::unique_ptr<ISource>>> open(OpenTicket ticket,
                                                              const SourceRequest& request,
                                                              const FormatGuess& /*guess*/,
                                                              const OpenContext& context) override {
        co_await resumeOn(context.io);
        Opened opened = co_try(openFile(request));
        std::unique_ptr<ISource> source = std::make_unique<RawFileSource>(
            SourceFactory::issueKey<RawFileSource>(std::move(ticket)), std::move(opened.info),
            std::move(opened.reader), opened.frameBytes);
        co_return std::move(source);
    }
};

DVV_REGISTER(ISourceProvider, RawFileProvider);

}  // namespace dvv::media
