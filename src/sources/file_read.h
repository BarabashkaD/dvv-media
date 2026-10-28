#pragma once

// Aligned frame reads shared by the file source providers (raw, Y4M; M2 design D8, §5.6).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/platform/file_reader.h"

namespace dvv::media::detail {

struct FrameRead {
    std::uint64_t alignedOffset;  // where the read starts
    std::size_t rangeBytes;       // bytes to read (a multiple of the alignment)
    std::size_t dataOffset;       // where the frame starts inside the range
    std::size_t frameBytes;
};

// Range [alignDown(offset - lead, a), alignUp(offset + frame_bytes, a)); a >= 1 (0 counts as 1).
// `lead` bytes before the frame (a Y4M frame marker) are inside the range; lead > offset counts
// as lead == offset.
[[nodiscard]] FrameRead planFrameRead(std::uint64_t offset, std::uint64_t frame_bytes,
                                      std::uint64_t lead, std::uint32_t alignment) noexcept;

// Blocking (call on the I/O pool). Reads plan.rangeBytes at plan.alignedOffset into `memory`.
// Errc::Io "truncated frame <index> of '<file>'" when fewer than dataOffset + frameBytes bytes
// arrive; Errc::Internal when `memory` is smaller than the range; FileReader errors pass through.
[[nodiscard]] Result<void> readFrame(const platform::FileReader& reader, const FrameRead& plan,
                                     std::span<std::byte> memory, std::int64_t index,
                                     std::string_view file_utf8);

}  // namespace dvv::media::detail
