// The synthetic source provider (M2 design D16, §4.5): deterministic gradient and noise frames of
// any catalogue format, made from the shared pattern generators and packed with the CPU reference.
//
//   synthetic:<gradient|noise>?fmt=<name>&size=WxH (or w=&h=)[&fps=][&frames=][&seed=][&colour=]
//                              [&range=]

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/frame.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/parameters.h"
#include "patterns.h"
#include "sources/uri_params.h"

namespace dvv::media {

namespace {

constexpr std::array<std::string_view, 1> SCHEMES{"synthetic"};
constexpr std::array<std::string_view, 7> OWN_PARAMETERS{"fmt", "size",   "w",   "h",
                                                         "fps", "frames", "seed"};
constexpr auto ALLOWED = detail::withColourParameters(OWN_PARAMETERS);

constexpr Rational DEFAULT_FPS{25, 1};
constexpr std::uint64_t DEFAULT_FRAMES = 10;
constexpr std::uint64_t DEFAULT_SEED = 1;
constexpr auto MAX_FRAMES = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
// The host frame alignment (a cache line).
constexpr std::size_t FRAME_ALIGNMENT = 64;

// A source as its URI describes it.
struct Parsed {
    detail::Pattern pattern = detail::Pattern::Gradient;
    std::uint64_t seed = DEFAULT_SEED;
    SourceInfo info;
};

Result<detail::Pattern> parsePattern(std::string_view name) {
    if (name == "gradient") {
        return detail::Pattern::Gradient;
    }
    if (name == "noise") {
        return detail::Pattern::Noise;
    }
    return makeError(Errc::Format,
                     std::format("unknown synthetic pattern '{}' (gradient, noise)", name));
}

// A catalogue format by name: unpacked and custom formats are interned only on use, so accepting
// them would make the same URI open or fail depending on the process's history.
Result<FormatId> parseFormat(std::string_view name) {
    const std::optional<FormatId> format = findFormat(name);
    if (!format || std::ranges::find(catalogue(), *format) == catalogue().end()) {
        return makeError(Errc::Format, std::format("unknown pixel format '{}'", name));
    }
    return *format;
}

Result<Parsed> parse(const Uri& uri) {
    DVV_TRY_VOID(detail::checkParameters(uri, ALLOWED));
    Parsed parsed;
    DVV_TRY(parsed.pattern, parsePattern(uri.path));

    const auto fmt = uri.param("fmt");
    if (!fmt) {
        return makeError(Errc::Format, "synthetic source needs fmt");
    }
    DVV_TRY(const FormatId format, parseFormat(*fmt));
    DVV_TRY(const auto size, detail::sizeFromQuery(uri));
    if (!size) {
        return makeError(Errc::Format, "synthetic source needs size=WxH or w= and h=");
    }
    const auto [width, height] = *size;
    DVV_TRY_VOID(geometry(format, width, height).transform([](const FrameGeometry&) {}));

    Rational fps = DEFAULT_FPS;
    if (const auto text = uri.param("fps")) {
        DVV_TRY(fps, detail::parseFps(*text));
    }
    std::uint64_t frames = DEFAULT_FRAMES;
    if (const auto text = uri.param("frames")) {
        DVV_TRY(frames, detail::parseUnsigned("frames", *text));
        if (frames == 0 || frames > MAX_FRAMES) {
            return makeError(Errc::Format,
                             std::format("bad frames '{}' (expected 1..{})", *text, MAX_FRAMES));
        }
    }
    if (const auto text = uri.param("seed")) {
        DVV_TRY(parsed.seed, detail::parseUnsigned("seed", *text));
    }
    DVV_TRY(const ColourSpec colour_spec, colourSpecFromQuery(uri));
    const ColourParams colour = merged(colour_spec, descriptor(format).model);

    Uri resolved;
    resolved.scheme = "synthetic";
    resolved.path = uri.path;
    resolved.params = {
        {"fmt", descriptor(format).name},         {"size", std::format("{}x{}", width, height)},
        {"fps", detail::formatFps(fps)},          {"frames", std::format("{}", frames)},
        {"seed", std::format("{}", parsed.seed)},
    };
    std::ranges::move(colourParameters(colour_spec), std::back_inserter(resolved.params));

    SourceInfo& info = parsed.info;
    info.desc = FrameDesc{
        .format = format, .colour = internColour(colour), .width = width, .height = height};
    info.fps = fps;
    info.frameCount = static_cast<std::int64_t>(frames);
    info.access = SourceAccess::RandomCheap;
    info.deterministic = true;
    info.resolvedUri = formatUri(resolved);
    info.provider = "synthetic";
    return parsed;
}

// Stateless after construction: produce() only reads, so concurrent calls are safe.
class SyntheticSource final : public ISource, public Managed<SyntheticSource> {
public:
    SyntheticSource(CreationKey<SourceFactory> key, SourceInfo info, detail::Pattern pattern,
                    std::uint64_t seed) noexcept
        : Managed(std::move(key)), info_(std::move(info)), pattern_(pattern), seed_(seed) {}

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override {
        const std::int64_t frames = info_.frameCount.value_or(0);
        if (index < 0 || index >= frames) {
            co_return makeError(Errc::Internal,
                                std::format("frame {} is outside [0, {})", index, frames));
        }
        const FrameDesc& desc = info_.desc;
        // The gradient takes no seed: 0, as the GPU tests use it.
        const std::uint64_t seed = pattern_ == detail::Pattern::Noise ? seed_ : 0;
        const CodeValues values =
            co_try(detail::generate(pattern_, desc.format, desc.width, desc.height, seed, index));
        const std::vector<std::byte> bytes = co_try(pack(values));
        const std::shared_ptr<HostFrame> frame =
            co_try(ctx.allocateHost(desc, bytes.size(), FRAME_ALIGNMENT, 0));
        const std::span<std::byte> data = frame->data();
        if (data.size() != bytes.size()) {
            co_return makeError(Errc::Internal, "synthetic: the host frame has another size");
        }
        std::ranges::copy(bytes, data.begin());
        co_return ctx.publish(frame);
    }

private:
    SourceInfo info_;
    detail::Pattern pattern_;
    std::uint64_t seed_;
};

}  // namespace

class SyntheticProvider final : public ISourceProvider, public Managed<SyntheticProvider> {
public:
    SyntheticProvider(CreationKey<ComponentRegistry> key,
                      const ParameterValues& /*values*/) noexcept
        : Managed(std::move(key)) {}

    static ComponentInfo info() noexcept {
        return {.id = "synthetic",
                .name = "Synthetic test patterns",
                .kind = Kind::Source,
                .version = 1};
    }
    static const ParameterSchema& schema() {
        static const ParameterSchema SCHEMA;
        return SCHEMA;
    }
    static std::expected<void, Unavailable> probe(const Capabilities& /*caps*/) { return {}; }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        return SCHEMES;
    }

    // No validation: open() reports the parameter errors.
    [[nodiscard]] Match match(const SourceRequest& request,
                              std::span<const std::byte> /*head*/) const override {
        const Uri& uri = request.uri;
        if (uri.scheme != "synthetic") {
            return Match{.confidence = 0, .guess = {}, .why = "not a synthetic: URI"};
        }
        Match match{.confidence = 100, .guess = {}, .why = "synthetic source"};
        if (const auto fmt = uri.param("fmt")) {
            match.guess.format = findFormat(*fmt);
        }
        if (const auto size = detail::sizeFromQuery(uri)) {
            if (const auto& value = *size) {
                match.guess.width = value->first;
                match.guess.height = value->second;
            }
        }
        return match;
    }

    [[nodiscard]] Task<Result<std::unique_ptr<ISource>>> open(
        OpenTicket ticket, const SourceRequest& request, const FormatGuess& /*guess*/,
        const OpenContext& /*context*/) override {
        Parsed parsed = co_try(parse(request.uri));
        std::unique_ptr<ISource> source = std::make_unique<SyntheticSource>(
            SourceFactory::issueKey<SyntheticSource>(std::move(ticket)), std::move(parsed.info),
            parsed.pattern, parsed.seed);
        co_return std::move(source);
    }
};

DVV_REGISTER(ISourceProvider, SyntheticProvider);

}  // namespace dvv::media
