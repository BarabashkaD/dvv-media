#pragma once

// A walk over the instructions of a SPIR-V module, for the shader tests.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dvv::media::test {

inline constexpr std::uint32_t SPIRV_OP_CAPABILITY = 17;
inline constexpr std::uint32_t SPIRV_OP_TYPE_IMAGE = 25;
// OpTypeImage operands (after the opcode word): result, sampled type, dim, depth, arrayed, MS,
// sampled, format.
inline constexpr std::size_t SPIRV_IMAGE_SAMPLED = 6;
inline constexpr std::size_t SPIRV_IMAGE_FORMAT = 7;

struct SpirvInstruction {
    std::uint32_t opcode = 0;
    std::span<const std::uint32_t> operands;  // the words after the opcode word
};

// The instructions after the five-word header; REQUIREs well-formed lengths.
[[nodiscard]] inline std::vector<SpirvInstruction> spirvInstructions(
    std::span<const std::uint32_t> words) {
    REQUIRE(words.size() > 5);
    std::vector<SpirvInstruction> instructions;
    std::size_t at = 5;
    while (at < words.size()) {
        const std::uint32_t length = words[at] >> 16U;
        REQUIRE(length > 0);
        REQUIRE(at + length <= words.size());
        instructions.push_back({.opcode = words[at] & 0xFFFFU,
                                .operands = words.subspan(at + 1, std::size_t{length} - 1)});
        at += length;
    }
    return instructions;
}

}  // namespace dvv::media::test
