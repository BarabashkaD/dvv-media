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
    REQUIRE_MESSAGE(push.has_value(), (push.has_value() ? std::string{} : push.error().describe()));
    return *push;
}

using Slots = std::array<std::uint32_t, 3>;

}  // namespace

TEST_SUITE("gpu_unpack") {
    TEST_CASE("unpack push constants of yuv420p10le at 33x17") {
        const detail::UnpackPush y = pushOf("yuv420p10le", 33, 17, 0, 4096);
        CHECK(y.base == 4096);
        CHECK(y.layout.planeOffset == 0);
        CHECK(y.layout.stride == 66);
        CHECK(y.layout.groupBytes == 2);
        CHECK(y.layout.wordBytes == 2);
        CHECK(y.layout.bigEndian == 0);
        CHECK(y.layout.bits == 10);
        CHECK(y.width == 33);
        CHECK(y.height == 17);
        CHECK(y.layout.samplesPerGroup == 1);
        CHECK(y.layout.slots == Slots{0, 0, 0});
        CHECK(y.layout.pad == 0);

        const detail::UnpackPush u = pushOf("yuv420p10le", 33, 17, 1, 4096);
        CHECK(u.base == 4096);
        CHECK(u.layout.planeOffset == 1122);
        CHECK(u.layout.stride == 34);
        CHECK(u.width == 17);
        CHECK(u.height == 9);
        CHECK(u.layout.bits == 10);
        CHECK(u.layout.samplesPerGroup == 1);

        const detail::UnpackPush v = pushOf("yuv420p10le", 33, 17, 2, 4096);
        CHECK(v.layout.planeOffset == 1428);
        CHECK(v.layout.stride == 34);
        CHECK(v.width == 17);
        CHECK(v.height == 9);
    }

    TEST_CASE("unpack push constants of v210 at 12x1") {
        const detail::UnpackPush y = pushOf("v210", 12, 1, 0);
        CHECK(y.layout.stride == 128);
        CHECK(y.layout.groupBytes == 16);
        CHECK(y.layout.wordBytes == 4);
        CHECK(y.layout.bigEndian == 0);
        CHECK(y.layout.bits == 10);
        CHECK(y.width == 12);
        CHECK(y.height == 1);
        CHECK(y.layout.samplesPerGroup == 6);
        CHECK(y.layout.slots == Slots{0x0100000A, 0x020A0114, 0x03140300});

        const detail::UnpackPush u = pushOf("v210", 12, 1, 1);
        CHECK(u.width == 6);
        CHECK(u.layout.samplesPerGroup == 3);
        CHECK(u.layout.slots == Slots{0x010A0000, 0x00000214, 0});

        const detail::UnpackPush v = pushOf("v210", 12, 1, 2);
        CHECK(v.width == 6);
        CHECK(v.layout.samplesPerGroup == 3);
        CHECK(v.layout.slots == Slots{0x02000014, 0x0000030A, 0});
    }

    TEST_CASE("unpack push constants of p010le and rgb48be") {
        for (std::size_t c = 1; c <= 2; ++c) {
            CAPTURE(c);
            const detail::UnpackPush chroma = pushOf("p010le", 4, 2, c);
            CHECK(chroma.layout.slots[0] == (c == 1 ? 0x0006U : 0x0106U));
            CHECK(chroma.layout.bits == 10);
            CHECK(chroma.layout.wordBytes == 2);
            CHECK(chroma.layout.groupBytes == 4);
            CHECK(chroma.layout.samplesPerGroup == 1);
            CHECK(chroma.width == 2);
            CHECK(chroma.height == 1);
            CHECK(chroma.layout.planeOffset == 16);
            CHECK(chroma.layout.stride == 8);
        }
        for (std::size_t c = 0; c < 3; ++c) {
            CAPTURE(c);
            const detail::UnpackPush rgb = pushOf("rgb48be", 3, 1, c);
            CHECK(rgb.layout.slots[0] == c * 0x0100U);
            CHECK(rgb.layout.bigEndian == 1);
            CHECK(rgb.layout.bits == 16);
            CHECK(rgb.layout.groupBytes == 6);
            CHECK(rgb.layout.wordBytes == 2);
            CHECK(rgb.layout.stride == 18);
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
        // Offsets from the block start; the layout's fields as "layout.<name>".
        std::map<std::string, std::uint32_t> offsets;
        for (const auto& field : type.at("elementType").at("fields")) {
            const std::string name = dvv::json::get<std::string>(field, "name").value_or("?");
            const std::uint32_t offset =
                dvv::json::get<std::uint32_t>(field.at("binding"), "offset").value_or(~0U);
            offsets[name] = offset;
            if (name == "layout") {
                for (const auto& member : field.at("type").at("fields")) {
                    offsets["layout." + dvv::json::get<std::string>(member, "name").value_or("?")] =
                        offset +
                        dvv::json::get<std::uint32_t>(member.at("binding"), "offset").value_or(~0U);
                }
            }
        }
        const std::map<std::string, std::uint32_t> expected{{"base", 0},
                                                            {"layout", 8},
                                                            {"layout.planeOffset", 8},
                                                            {"layout.stride", 16},
                                                            {"layout.groupBytes", 20},
                                                            {"layout.wordBytes", 24},
                                                            {"layout.bigEndian", 28},
                                                            {"layout.bits", 32},
                                                            {"layout.samplesPerGroup", 36},
                                                            {"layout.slots", 40},
                                                            {"layout.paddingMasks", 52},
                                                            {"layout.pad", 76},
                                                            {"width", 80},
                                                            {"height", 84}};
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
