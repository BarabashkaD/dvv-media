#include "sources/file_read.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/platform/file_reader.h"

namespace dvv::media::detail {

FrameRead planFrameRead(std::uint64_t offset, std::uint64_t frame_bytes, std::uint64_t lead,
                        std::uint32_t alignment) noexcept {
    const std::uint64_t a = alignment == 0 ? 1 : alignment;
    const std::uint64_t start = offset - std::min(lead, offset);
    const std::uint64_t first = start - (start % a);
    const std::uint64_t end = offset + frame_bytes;
    const std::uint64_t last = end % a == 0 ? end : end + (a - (end % a));
    return FrameRead{.alignedOffset = first,
                     .rangeBytes = static_cast<std::size_t>(last - first),
                     .dataOffset = static_cast<std::size_t>(offset - first),
                     .frameBytes = static_cast<std::size_t>(frame_bytes)};
}

Result<void> readFrame(const platform::FileReader& reader, const FrameRead& plan,
                       std::span<std::byte> memory, std::int64_t index,
                       std::string_view file_utf8) {
    if (memory.size() < plan.rangeBytes) {
        return makeError(Errc::Internal,
                         std::format("readFrame: {} bytes of memory for a {}-byte read",
                                     memory.size(), plan.rangeBytes));
    }
    DVV_TRY(const std::size_t read,
            reader.readAt(plan.alignedOffset, memory.first(plan.rangeBytes)));
    if (read < plan.dataOffset + plan.frameBytes) {
        return makeError(Errc::Io, std::format("truncated frame {} of '{}'", index, file_utf8));
    }
    return {};
}

}  // namespace dvv::media::detail
