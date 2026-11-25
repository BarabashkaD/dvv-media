#pragma once

// Test sources of packed frames (noise or fixed code values), and helpers for the original-read
// and display tests (host and GPU).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
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

// The SourceInfo of a test source: `frames` frames of `desc` at 25 fps, cheap random access.
[[nodiscard]] inline SourceInfo testSourceInfo(const FrameDesc& desc, std::int64_t frames) {
    SourceInfo info;
    info.desc = desc;
    info.fps = Rational{25, 1};
    info.frameCount = frames;
    info.access = SourceAccess::RandomCheap;
    info.provider = "test";
    return info;
}

// Packs `values` into a host frame of `desc` allocated through `ctx` and publishes it.
[[nodiscard]] inline Result<FrameRef> publishPacked(const FrameDesc& desc, const CodeValues& values,
                                                    ProduceContext& ctx) {
    DVV_TRY(const std::vector<std::byte> bytes, pack(values));
    DVV_TRY(const std::shared_ptr<HostFrame> frame, ctx.allocateHost(desc, bytes.size(), 64, 0));
    const std::span<std::byte> data = frame->data();
    if (data.size() != bytes.size()) {
        return makeError(Errc::Internal, "packed: the host frame has another size");
    }
    std::ranges::copy(bytes, data.begin());
    return ctx.publish(frame);
}

// Frame i is pack(detail::generate(Noise, desc.format, width, height, seed, i)). A test double, not
// a component.
class PackedSource final : public ISource {
public:
    PackedSource(const FrameDesc& desc, std::int64_t frames, std::uint64_t seed)
        : info_(testSourceInfo(desc, frames)), seed_(seed) {}

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override {
        const FrameDesc& desc = info_.desc;
        if (index < 0 || index >= info_.frameCount.value_or(0)) {
            co_return makeError(Errc::Internal,
                                std::format("packed: frame {} out of range", index));
        }
        const CodeValues values = co_try(detail::generate(detail::Pattern::Noise, desc.format,
                                                          desc.width, desc.height, seed_, index));
        co_return publishPacked(desc, values, ctx);
    }

private:
    SourceInfo info_;
    std::uint64_t seed_;
};

// One frame of fixed code values, packed on every produce(). A test double, not a component.
class CodeValuesSource final : public ISource {
public:
    CodeValuesSource(const FrameDesc& desc, CodeValues values)
        : info_(testSourceInfo(desc, 1)), values_(std::move(values)) {}

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Task<Result<FrameRef>> produce(std::int64_t index, ProduceContext& ctx) override {
        if (index != 0) {
            co_return makeError(Errc::Internal, std::format("values: no frame {}", index));
        }
        co_return publishPacked(info_.desc, values_, ctx);
    }

private:
    SourceInfo info_;
    CodeValues values_;
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
