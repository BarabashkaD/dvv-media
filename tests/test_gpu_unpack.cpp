#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/pixel_format.h"
#include "dvv_media_shaders/unpack.h"
#include "gpu_unpack.h"
#include "spirv_walk.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;

namespace {

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

detail::UnpackPush pushOf(std::string_view name, std::uint32_t width, std::uint32_t height,
                          std::size_t component, std::uint64_t base = 0) {
    const auto push = detail::unpackPush(idOf(name), width, height, component, base);
    REQUIRE_MESSAGE(push.has_value(), push.error().describe());
    return *push;
}

using Slots = std::array<std::uint32_t, 3>;

}  // namespace

TEST_SUITE("gpu_unpack") {
    TEST_CASE("unpack push constants of yuv420p10le at 33x17") {
        const detail::UnpackPush y = pushOf("yuv420p10le", 33, 17, 0, 4096);
        CHECK(y.base == 4096);
        CHECK(y.planeOffset == 0);
        CHECK(y.stride == 66);
        CHECK(y.groupBytes == 2);
        CHECK(y.wordBytes == 2);
        CHECK(y.bigEndian == 0);
        CHECK(y.bits == 10);
        CHECK(y.width == 33);
        CHECK(y.height == 17);
        CHECK(y.samplesPerGroup == 1);
        CHECK(y.slots == Slots{0, 0, 0});
        CHECK(y.pad == 0);

        const detail::UnpackPush u = pushOf("yuv420p10le", 33, 17, 1, 4096);
        CHECK(u.base == 4096);
        CHECK(u.planeOffset == 1122);
        CHECK(u.stride == 34);
        CHECK(u.width == 17);
        CHECK(u.height == 9);
        CHECK(u.bits == 10);
        CHECK(u.samplesPerGroup == 1);

        const detail::UnpackPush v = pushOf("yuv420p10le", 33, 17, 2, 4096);
        CHECK(v.planeOffset == 1428);
        CHECK(v.stride == 34);
        CHECK(v.width == 17);
        CHECK(v.height == 9);
    }

    TEST_CASE("unpack push constants of v210 at 12x1") {
        const detail::UnpackPush y = pushOf("v210", 12, 1, 0);
        CHECK(y.stride == 128);
        CHECK(y.groupBytes == 16);
        CHECK(y.wordBytes == 4);
        CHECK(y.bigEndian == 0);
        CHECK(y.bits == 10);
        CHECK(y.width == 12);
        CHECK(y.height == 1);
        CHECK(y.samplesPerGroup == 6);
        CHECK(y.slots == Slots{0x0100000A, 0x020A0114, 0x03140300});

        const detail::UnpackPush u = pushOf("v210", 12, 1, 1);
        CHECK(u.width == 6);
        CHECK(u.samplesPerGroup == 3);
        CHECK(u.slots == Slots{0x010A0000, 0x00000214, 0});

        const detail::UnpackPush v = pushOf("v210", 12, 1, 2);
        CHECK(v.width == 6);
        CHECK(v.samplesPerGroup == 3);
        CHECK(v.slots == Slots{0x02000014, 0x0000030A, 0});
    }

    TEST_CASE("unpack push constants of p010le and rgb48be") {
        for (std::size_t c = 1; c <= 2; ++c) {
            CAPTURE(c);
            const detail::UnpackPush chroma = pushOf("p010le", 4, 2, c);
            CHECK(chroma.slots[0] == (c == 1 ? 0x0006U : 0x0106U));
            CHECK(chroma.bits == 10);
            CHECK(chroma.wordBytes == 2);
            CHECK(chroma.groupBytes == 4);
            CHECK(chroma.samplesPerGroup == 1);
            CHECK(chroma.width == 2);
            CHECK(chroma.height == 1);
            CHECK(chroma.planeOffset == 16);
            CHECK(chroma.stride == 8);
        }
        for (std::size_t c = 0; c < 3; ++c) {
            CAPTURE(c);
            const detail::UnpackPush rgb = pushOf("rgb48be", 3, 1, c);
            CHECK(rgb.slots[0] == c * 0x0100U);
            CHECK(rgb.bigEndian == 1);
            CHECK(rgb.bits == 16);
            CHECK(rgb.groupBytes == 6);
            CHECK(rgb.wordBytes == 2);
            CHECK(rgb.stride == 18);
        }
    }

    TEST_CASE("the unpack shader's push block matches UnpackPush") {
        const auto reflection = dvv::json::parse(dvv_media_shaders::unpack().reflectionJson);
        REQUIRE(reflection.has_value());
        const dvv::json::Value* block = nullptr;
        for (const auto& parameter : reflection->at("parameters")) {
            if (dvv::json::get<std::string>(parameter.at("binding"), "kind") ==
                "pushConstantBuffer") {
                block = &parameter;
            }
        }
        REQUIRE(block != nullptr);
        const auto& type = block->at("type");
        CHECK(dvv::json::get<std::uint32_t>(type.at("elementVarLayout").at("binding"), "size") ==
              sizeof(detail::UnpackPush));
        std::map<std::string, std::uint32_t> offsets;
        for (const auto& field : type.at("elementType").at("fields")) {
            offsets[dvv::json::get<std::string>(field, "name").value_or("?")] =
                dvv::json::get<std::uint32_t>(field.at("binding"), "offset").value_or(~0U);
        }
        const std::map<std::string, std::uint32_t> expected{
            {"base", 0},       {"planeOffset", 8},      {"stride", 16}, {"groupBytes", 20},
            {"wordBytes", 24}, {"bigEndian", 28},       {"bits", 32},   {"width", 36},
            {"height", 40},    {"samplesPerGroup", 44}, {"slots", 48},  {"pad", 60}};
        CHECK(offsets == expected);
    }

    TEST_CASE("the unpack shader writes an image of unknown format") {
        using dvv::media::test::SPIRV_IMAGE_FORMAT;
        using dvv::media::test::SPIRV_IMAGE_SAMPLED;
        using dvv::media::test::SPIRV_OP_CAPABILITY;
        using dvv::media::test::SPIRV_OP_TYPE_IMAGE;
        using dvv::media::test::SpirvInstruction;
        constexpr std::uint32_t STORAGE_IMAGE_WRITE_WITHOUT_FORMAT = 56;
        constexpr std::uint32_t SAMPLED_STORAGE = 2;  // read and written without a sampler
        constexpr std::uint32_t FORMAT_UNKNOWN = 0;
        int storage_images = 0;
        int images = 0;
        bool write_without_format = false;
        for (const SpirvInstruction& instruction :
             dvv::media::test::spirvInstructions(dvv_media_shaders::unpack().spirv)) {
            if (instruction.opcode == SPIRV_OP_TYPE_IMAGE) {
                REQUIRE(instruction.operands.size() > SPIRV_IMAGE_FORMAT);
                ++images;
                if (instruction.operands[SPIRV_IMAGE_SAMPLED] == SAMPLED_STORAGE &&
                    instruction.operands[SPIRV_IMAGE_FORMAT] == FORMAT_UNKNOWN) {
                    ++storage_images;
                }
            } else if (instruction.opcode == SPIRV_OP_CAPABILITY) {
                REQUIRE(!instruction.operands.empty());
                write_without_format =
                    write_without_format ||
                    instruction.operands[0] == STORAGE_IMAGE_WRITE_WITHOUT_FORMAT;
            }
        }
        CHECK(images == 1);
        CHECK(storage_images == 1);
        CHECK(write_without_format);
    }

    TEST_CASE("unpackBufferBytes follows the size rule") {
        CHECK(detail::unpackBufferBytes(0, 561) == 568);
        CHECK(detail::unpackBufferBytes(260, 1) == 268);
        CHECK(detail::unpackBufferBytes(0, 64) == 68);
    }

    TEST_CASE("unpackPush rejects a component without slots") {
        const auto push = detail::unpackPush(idOf("yuv420p"), 4, 4, 3, 0);
        REQUIRE_FALSE(push.has_value());
        CHECK(push.error().code == dvv::Errc::Internal);
    }
}
