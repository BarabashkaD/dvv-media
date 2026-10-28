// The YUV4MPEG2 source provider (M2 design §5.6, D15, R10, F6): a text header, then frames that
// each start with a parameterless "FRAME\n" marker. Frames have a fixed size, so frame i is found
// by arithmetic (random access without an index scan); its marker is checked on every read.
//
//   <path>[?fmt=<name>][&size=WxH][&fps=]   the query only checks the header's values
//         [&colour=][&range=]   these replace the header's (a standard does not set the range)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
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
#include "math_util.h"
#include "sources/file_read.h"
#include "sources/uri_params.h"
#include "sources/y4m_header.h"

namespace dvv::media {

namespace detail {

namespace {

constexpr std::uint64_t MAX_RATE_TERM =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

// The C tag values and their catalogue formats (D15). A missing C means 420jpeg.
struct Colourspace {
    std::string_view tag;
    std::string_view format;
};
constexpr std::array<Colourspace, 16> COLOURSPACES{{
    {"420jpeg", "yuv420p"},
    {"420paldv", "yuv420p"},
    {"420mpeg2", "yuv420p"},
    {"420", "yuv420p"},
    {"422", "yuv422p"},
    {"444", "yuv444p"},
    {"mono", "gray"},
    {"420p10", "yuv420p10le"},
    {"422p10", "yuv422p10le"},
    {"444p10", "yuv444p10le"},
    {"420p12", "yuv420p12le"},
    {"422p12", "yuv422p12le"},
    {"444p12", "yuv444p12le"},
    {"420p16", "yuv420p16le"},
    {"444p16", "yuv444p16le"},
    {"mono16", "gray16le"},
}};
constexpr std::string_view DEFAULT_COLOURSPACE = "420jpeg";

Result<FormatId> formatOf(std::string_view colourspace) {
    const auto entry = std::ranges::find(COLOURSPACES, colourspace, &Colourspace::tag);
    if (entry == COLOURSPACES.end()) {
        return makeError(Errc::Unsupported,
                         std::format("Y4M colourspace '{}' is not supported", colourspace));
    }
    const std::optional<FormatId> format = findFormat(entry->format);
    if (!format) {
        return makeError(Errc::Internal,
                         std::format("Y4M: format '{}' is not in the catalogue", entry->format));
    }
    return *format;
}

// F<n>:<d>, both > 0; reduced by the gcd like the fps= parameter.
Result<Rational> parseRate(std::string_view value) {
    const std::size_t colon = value.find(':');
    if (colon != std::string_view::npos) {
        const auto num = parseUnsigned("F", value.substr(0, colon));
        const auto den = parseUnsigned("F", value.substr(colon + 1));
        if (num && den && *num > 0 && *den > 0 && *num <= MAX_RATE_TERM && *den <= MAX_RATE_TERM) {
            const std::uint64_t divisor = std::gcd(*num, *den);
            return Rational{static_cast<std::int64_t>(*num / divisor),
                            static_cast<std::int64_t>(*den / divisor)};
        }
    }
    return makeError(Errc::Format, std::format("bad Y4M frame rate 'F{}'", value));
}

// X<name>=<value>: only XCOLORRANGE is read; other X tags are ignored.
Result<void> applyExtension(std::string_view text, ColourRange& range) {
    const std::size_t equals = text.find('=');
    if (text.substr(0, equals) != "COLORRANGE") {
        return {};
    }
    const std::string_view value =
        equals == std::string_view::npos ? std::string_view{} : text.substr(equals + 1);
    if (value == "FULL") {
        range = ColourRange::Full;
    } else if (value == "LIMITED") {
        range = ColourRange::Limited;
    } else {
        return makeError(Errc::Format,
                         std::format("bad Y4M XCOLORRANGE '{}' (FULL, LIMITED)", value));
    }
    return {};
}

}  // namespace

Result<Y4mHeader> parseY4mHeader(std::span<const std::byte> head) {
    const std::string_view text(reinterpret_cast<const char*>(head.data()),
                                std::min(head.size(), Y4M_MAX_HEADER));
    if (!text.starts_with(Y4M_MAGIC)) {
        return makeError(Errc::Format, "not a YUV4MPEG2 file");
    }
    const std::size_t newline = text.find('\n');
    if (newline == std::string_view::npos) {
        return makeError(Errc::Format, "Y4M header longer than 4096 bytes or not terminated");
    }
    if (text[newline - 1] == '\r') {
        return makeError(Errc::Unsupported, "Y4M header ends with CR LF");
    }

    Y4mHeader header;
    header.bytes = newline + 1;
    std::string_view colourspace = DEFAULT_COLOURSPACE;
    bool has_width = false;
    bool has_height = false;
    std::string_view tags = text.substr(Y4M_MAGIC.size(), newline - Y4M_MAGIC.size());
    while (true) {
        const std::size_t space = tags.find(' ');
        const std::string_view tag = tags.substr(0, space);
        if (tag.empty()) {
            return makeError(Errc::Format,
                             "empty Y4M header tag (tags are separated by one space)");
        }
        const std::string_view value = tag.substr(1);
        switch (tag.front()) {
            case 'W': {
                DVV_TRY(header.width, parseDimension("W", value));
                has_width = true;
                break;
            }
            case 'H': {
                DVV_TRY(header.height, parseDimension("H", value));
                has_height = true;
                break;
            }
            case 'F': {
                DVV_TRY(header.fps, parseRate(value));
                break;
            }
            case 'I':  // interlacing and pixel aspect: accepted and ignored (F6)
            case 'A': break;
            case 'C': colourspace = value; break;
            case 'X': {
                DVV_TRY_VOID(applyExtension(value, header.range));
                break;
            }
            default:
                return makeError(Errc::Format,
                                 std::format("unknown Y4M header tag '{}'", tag.front()));
        }
        if (space == std::string_view::npos) {
            break;
        }
        tags.remove_prefix(space + 1);
    }
    if (!has_width || !has_height) {
        return makeError(Errc::Format,
                         std::format("Y4M header has no {} tag", has_width ? 'H' : 'W'));
    }
    DVV_TRY(header.format, formatOf(colourspace));
    return header;
}

}  // namespace detail

namespace {

constexpr std::array<std::string_view, 1> SCHEMES{"file"};
constexpr std::array<std::string_view, 3> OWN_PARAMETERS{"fmt", "size", "fps"};
constexpr auto ALLOWED = detail::withColourParameters(OWN_PARAMETERS);
constexpr std::string_view FRAME_MARKER = "FRAME\n";
constexpr std::uint64_t MARKER_BYTES = FRAME_MARKER.size();
// The smallest host frame alignment (a cache line); unbuffered reads need the reader's.
constexpr std::size_t MIN_FRAME_ALIGNMENT = 64;

// A source as its header describes it, with the file open.
struct Opened {
    SourceInfo info;
    platform::FileReader reader;
    std::uint64_t headerBytes = 0;
    std::uint64_t frameBytes = 0;
};

// The query must agree with the header: fmt, size and fps are checks only.
Result<void> checkQuery(const Uri& uri, const detail::Y4mHeader& header) {
    const std::string_view name = descriptor(header.format).name;
    if (const auto fmt = uri.param("fmt"); fmt && *fmt != name) {
        return makeError(Errc::Format, std::format("Y4M header says {}", name));
    }
    if (const auto text = uri.param("size")) {
        DVV_TRY(const auto size, detail::parseSize(*text));
        if (size != std::pair{header.width, header.height}) {
            return makeError(Errc::Format,
                             std::format("Y4M header says {}x{}", header.width, header.height));
        }
    }
    if (const auto text = uri.param("fps")) {
        DVV_TRY(const Rational fps, detail::parseFps(*text));
        if (fps != header.fps) {
            return makeError(Errc::Format,
                             std::format("Y4M header says {}", detail::formatFps(header.fps)));
        }
    }
    return {};
}

// The header's colour (only its range), with every field the query gives replacing it.
ColourParams mergeColour(const detail::Y4mHeader& header, const ColourParams& query) {
    ColourParams colour{.range = header.range};
    if (query.range != ColourRange::Unspecified) {
        colour.range = query.range;
    }
    if (query.matrix != ColourMatrix::Unspecified) {
        colour.matrix = query.matrix;
        colour.kr = query.kr;
        colour.kb = query.kb;
    }
    if (query.transfer != TransferFn::Unspecified) {
        colour.transfer = query.transfer;
    }
    if (query.primaries != Primaries::Unspecified) {
        colour.primaries = query.primaries;
    }
    return colour;
}

// Blocking: runs on the io executor.
Result<Opened> openFile(const SourceRequest& request) {
    const Uri& uri = request.uri;
    DVV_TRY_VOID(detail::checkParameters(uri, ALLOWED));
    DVV_TRY(const ColourSpec query_spec, colourSpecFromQuery(uri));
    DVV_TRY(platform::FileReader reader,
            platform::FileReader::open(pathFromUtf8(uri.path), request.fileReads));

    // The header lies in the first Y4M_MAX_HEADER bytes; an unbuffered read needs an aligned
    // whole buffer.
    const std::size_t alignment = reader.alignment();
    const auto buffer_bytes =
        static_cast<std::size_t>(detail::alignUp(detail::Y4M_MAX_HEADER, alignment));
    DVV_TRY(const platform::AlignedBuffer buffer,
            platform::allocateAligned(buffer_bytes, alignment));
    DVV_TRY(const std::size_t read, reader.readAt(0, std::span(buffer.get(), buffer_bytes)));
    const std::size_t head_bytes = std::min(read, detail::Y4M_MAX_HEADER);
    DVV_TRY(const detail::Y4mHeader header,
            detail::parseY4mHeader(std::span<const std::byte>(buffer.get(), head_bytes)));
    DVV_TRY_VOID(checkQuery(uri, header));
    const ColourParams query_colour = merged(query_spec, descriptor(header.format).model);

    DVV_TRY(const FrameGeometry frame, geometry(header.format, header.width, header.height));
    const std::uint64_t frame_bytes = frame.bytes;
    const std::uint64_t stride = MARKER_BYTES + frame_bytes;
    const std::uint64_t payload = reader.size() - header.bytes;

    Uri resolved;
    resolved.scheme = "file";
    resolved.path = uri.path;
    resolved.params = {
        {"fmt", std::string(descriptor(header.format).name)},
        {"size", std::format("{}x{}", header.width, header.height)},
        {"fps", detail::formatFps(header.fps)},
    };
    std::ranges::move(colourParameters(query_spec), std::back_inserter(resolved.params));
    resolved.params.emplace_back("impl", "y4m");

    Opened opened{.info = {},
                  .reader = std::move(reader),
                  .headerBytes = header.bytes,
                  .frameBytes = frame_bytes};
    SourceInfo& info = opened.info;
    info.desc = FrameDesc{.format = header.format,
                          .colour = internColour(mergeColour(header, query_colour)),
                          .width = header.width,
                          .height = header.height};
    info.fps = header.fps;
    info.frameCount = static_cast<std::int64_t>(payload / stride);
    info.access = SourceAccess::RandomCheap;
    info.deterministic = true;
    info.resolvedUri = formatUri(resolved);
    info.provider = "y4m";
    if (const std::uint64_t rest = payload % stride; rest > 0) {
        info.warnings.push_back(std::format("{} trailing bytes ignored", rest));
    }
    info.reader = opened.reader.modeText();
    return opened;
}

// produce() only reads: FileReader::readAt is thread-safe and runs one read at a time (U-IO2), so
// concurrent calls share the reader.
class Y4mSource final : public ISource, public Managed<Y4mSource> {
public:
    Y4mSource(CreationKey<SourceFactory> key, SourceInfo info, std::string path,
              platform::FileReader reader, std::uint64_t header_bytes, std::uint64_t frame_bytes)
        : Managed(std::move(key)),
          info_(std::move(info)),
          path_(std::move(path)),
          reader_(std::move(reader)),
          header_bytes_(header_bytes),
          frame_bytes_(frame_bytes) {}

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override {
        const std::int64_t frames = info_.frameCount.value_or(0);
        if (index < 0 || index >= frames) {
            co_return makeError(Errc::Internal,
                                std::format("frame {} is outside [0, {})", index, frames));
        }
        co_await resumeOn(ctx.io());
        const std::uint64_t data =
            header_bytes_ + static_cast<std::uint64_t>(index) * (MARKER_BYTES + frame_bytes_) +
            MARKER_BYTES;
        const std::uint32_t alignment = reader_.alignment();
        const detail::FrameRead plan =
            detail::planFrameRead(data, frame_bytes_, MARKER_BYTES, alignment);
        const std::shared_ptr<HostFrame> frame = co_try(ctx.allocateHost(
            info_.desc, plan.rangeBytes, std::max<std::size_t>(alignment, MIN_FRAME_ALIGNMENT),
            plan.dataOffset));
        const std::span<std::byte> memory = frame->memory();
        co_try(detail::readFrame(reader_, plan, memory, index, path_));
        // planFrameRead puts the marker inside the range: it starts at alignDown(data - 6, a).
        const std::span<const std::byte> marker =
            memory.subspan(plan.dataOffset - MARKER_BYTES, MARKER_BYTES);
        if (std::memcmp(marker.data(), FRAME_MARKER.data(), MARKER_BYTES) != 0) {
            co_return makeError(
                Errc::Unsupported,
                std::format("Y4M frame parameters are not supported (frame {} of '{}')", index,
                            path_));
        }
        co_return ctx.publish(frame);
    }

private:
    SourceInfo info_;
    std::string path_;  // UTF-8, as given, for messages
    platform::FileReader reader_;
    std::uint64_t header_bytes_;
    std::uint64_t frame_bytes_;
};

}  // namespace

class Y4mProvider final : public ISourceProvider, public Managed<Y4mProvider> {
public:
    Y4mProvider(CreationKey<ComponentRegistry> key, const ParameterValues& /*values*/) noexcept
        : Managed(std::move(key)) {}

    static ComponentInfo info() noexcept {
        return {.id = "y4m", .name = "YUV4MPEG2 file", .kind = Kind::Source, .version = 1};
    }
    static const ParameterSchema& schema() {
        static const ParameterSchema SCHEMA;
        return SCHEMA;
    }
    static std::expected<void, Unavailable> probe(const Capabilities& /*caps*/) { return {}; }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        return SCHEMES;
    }

    // The magic decides; the query is checked by open().
    [[nodiscard]] Match match(const SourceRequest& /*request*/,
                              std::span<const std::byte> head) const override {
        const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
        if (!text.starts_with(detail::Y4M_MAGIC)) {
            return Match{.confidence = 0, .guess = {}, .why = "no YUV4MPEG2 magic"};
        }
        Match match{.confidence = 100, .guess = {}, .why = "YUV4MPEG2 magic"};
        if (const auto header = detail::parseY4mHeader(head)) {
            match.guess = FormatGuess{.format = header->format,
                                      .width = header->width,
                                      .height = header->height,
                                      .fps = header->fps};
        }
        return match;
    }

    [[nodiscard]] Task<Result<std::unique_ptr<ISource>>> open(OpenTicket ticket,
                                                              const SourceRequest& request,
                                                              const FormatGuess& /*guess*/,
                                                              const OpenContext& context) override {
        co_await resumeOn(context.io);
        Opened opened = co_try(openFile(request));
        std::unique_ptr<ISource> source = std::make_unique<Y4mSource>(
            SourceFactory::issueKey<Y4mSource>(std::move(ticket)), std::move(opened.info),
            request.uri.path, std::move(opened.reader), opened.headerBytes, opened.frameBytes);
        co_return std::move(source);
    }
};

DVV_REGISTER(ISourceProvider, Y4mProvider);

}  // namespace dvv::media
