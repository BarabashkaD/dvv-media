#pragma once

// A test source of packed noise frames, and helpers for the original-read tests (host and GPU).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/task.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "patterns.h"

namespace dvv::media::test {

// Frame i is pack(detail::generate(Noise, desc.format, width, height, seed, i)). A test double, not
// a component.
class PackedSource final : public ISource {
public:
    PackedSource(FrameDesc desc, std::int64_t frames, std::uint64_t seed) : seed_(seed) {
        info_.desc = desc;
        info_.fps = Rational{25, 1};
        info_.frameCount = frames;
        info_.access = SourceAccess::RandomCheap;
        info_.provider = "test";
    }

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override {
        const FrameDesc& desc = info_.desc;
        if (index < 0 || index >= info_.frameCount.value_or(0)) {
            co_return makeError(Errc::Internal,
                                std::format("packed: frame {} out of range", index));
        }
        const CodeValues values = co_try(detail::generate(detail::Pattern::Noise, desc.format,
                                                          desc.width, desc.height, seed_, index));
        const std::vector<std::byte> bytes = co_try(pack(values));
        const std::shared_ptr<HostFrame> frame =
            co_try(ctx.allocateHost(desc, bytes.size(), 64, 0));
        const std::span<std::byte> data = frame->data();
        if (data.size() != bytes.size()) {
            co_return makeError(Errc::Internal, "packed: the host frame has another size");
        }
        std::ranges::copy(bytes, data.begin());
        co_return ctx.publish(frame);
    }

private:
    SourceInfo info_;
    std::uint64_t seed_;
};

// The runs of the converter `name`; 0 when the store has no such converter.
[[nodiscard]] inline std::uint64_t runsOf(const StoreStats& stats, std::string_view name) {
    const auto found = std::ranges::find_if(
        stats.converters,
        [name](const ConverterStats& converter) { return converter.name == name; });
    return found == stats.converters.end() ? 0 : found->runs;
}

// The original bytes of frame `index` of a PackedSource.
[[nodiscard]] inline Result<std::vector<std::byte>> packedOriginal(FormatId format,
                                                                   std::uint32_t width,
                                                                   std::uint32_t height,
                                                                   std::uint64_t seed,
                                                                   std::int64_t index) {
    DVV_TRY(const CodeValues values,
            detail::generate(detail::Pattern::Noise, format, width, height, seed, index));
    return pack(values);
}

// unpackToPlanes() of frame `index` of a PackedSource: the bytes of its unpacked view.
[[nodiscard]] inline Result<std::vector<std::byte>> packedPlanes(FormatId format,
                                                                 std::uint32_t width,
                                                                 std::uint32_t height,
                                                                 std::uint64_t seed,
                                                                 std::int64_t index) {
    DVV_TRY(const std::vector<std::byte> original,
            packedOriginal(format, width, height, seed, index));
    DVV_TRY(const FrameGeometry planes, geometry(unpackedFormat(format), width, height));
    std::vector<std::byte> result(static_cast<std::size_t>(planes.bytes));
    DVV_TRY_VOID(unpackToPlanes(format, width, height, original, result));
    return result;
}

// The planes of a host binding, back to back.
[[nodiscard]] inline std::vector<std::byte> concatenated(const HostPlanesB& planes) {
    std::vector<std::byte> bytes;
    for (std::size_t p = 0; p < planes.planeCount; ++p) {
        bytes.insert(bytes.end(), planes.planes[p].begin(), planes.planes[p].end());
    }
    return bytes;
}

}  // namespace dvv::media::test
