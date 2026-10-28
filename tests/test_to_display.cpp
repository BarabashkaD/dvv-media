#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/display.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv_media_shaders/to_display.h"
#include "spirv_walk.h"
#include "to_display.h"

namespace media = dvv::media;
namespace detail = dvv::media::detail;
using media::ColourMatrix;
using media::ColourParams;
using media::ColourRange;
using media::Primaries;
using media::TransferFn;

namespace {

using Layout = std::array<std::uint32_t, 4>;

media::FormatId idOf(std::string_view name) {
    const auto id = media::findFormat(name);
    REQUIRE_MESSAGE(id.has_value(), name);
    return id.value_or(media::FormatId{0});  // REQUIRE above stops the test case first
}

// The push constants of a 33x17 frame of `name` with this colour, resolved as ToDisplay does.
detail::ToDisplayPush pushOf(std::string_view name, const ColourParams& colour) {
    const media::FrameDesc desc{
        .format = idOf(name), .colour = media::internColour(colour), .width = 33, .height = 17};
    const auto push = detail::toDisplayPush(desc.format, media::resolveColour(desc).params, 33, 17);
    REQUIRE_MESSAGE(push.has_value(), push.error().describe());
    return *push;
}

}  // namespace

TEST_SUITE("to_display") {
    TEST_CASE("ToDisplay push constants") {
        const ColourParams bt709{.matrix = ColourMatrix::Bt709, .range = ColourRange::Limited};
        const detail::ToDisplayPush push = pushOf("yuv420p10le", bt709);
        const media::FrameDesc desc{.format = idOf("yuv420p10le"),
                                    .colour = media::internColour(bt709),
                                    .width = 33,
                                    .height = 17};
        const media::DisplayAffine affine =
            media::displayAffine(desc.format, media::resolveColour(desc).params);
        for (std::size_t r = 0; r < 3; ++r) {
            CAPTURE(r);
            for (std::size_t i = 0; i < 3; ++i) {
                CHECK(push.affine[(4 * r) + i] == affine.matrix[(3 * r) + i]);
            }
            CHECK(push.affine[(4 * r) + 3] == affine.offset[r]);
        }
        CHECK(push.planeLayout == Layout{0x000, 0x111, 0x211, 0});
        CHECK(push.componentCount == 3);
        CHECK(push.transfer == 1);  // the default curve is BT.1886 (D5R section 1.4)
        CHECK(push.pad0 == 0);
        CHECK(push.width == 33);
        CHECK(push.height == 17);
        CHECK(push.pad == std::array<std::uint32_t, 3>{});

        CHECK(pushOf("nv12", {}).planeLayout == Layout{0, 0x111, 0x211, 0});
        CHECK(pushOf("v210", {}).planeLayout == Layout{0, 0x101, 0x201, 0});
        const detail::ToDisplayPush gray = pushOf("gray", {});
        CHECK(gray.planeLayout == Layout{0, 0, 0, 0});
        CHECK(gray.componentCount == 1);
        const detail::ToDisplayPush rgba = pushOf("rgba", {});
        CHECK(rgba.planeLayout == Layout{0, 0x100, 0x200, 0x300});
        CHECK(rgba.componentCount == 4);

        CHECK(pushOf("yuv420p", {.transfer = TransferFn::Bt1886}).transfer == 1);
        CHECK(pushOf("yuv420p", {.transfer = TransferFn::Linear}).transfer == 2);
        CHECK(pushOf("yuv420p", {.transfer = TransferFn::Pq}).transfer == 3);
        CHECK(pushOf("yuv420p", {.transfer = TransferFn::Hlg}).transfer == 4);
        // The primaries are information only (D5R section 2.1): no gamut conversion flag.
        CHECK(pushOf("yuv420p", {.primaries = Primaries::Bt2020}).pad0 == 0);

        const ColourParams resolved{.matrix = ColourMatrix::Bt709,
                                    .range = ColourRange::Limited,
                                    .transfer = TransferFn::Srgb,
                                    .primaries = Primaries::Bt709};
        const auto empty = detail::toDisplayPush(idOf("gray"), resolved, 0, 17);
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == dvv::Errc::Internal);
        const auto unknown = detail::toDisplayPush(media::FormatId{0xFFFFFFF0U}, resolved, 4, 4);
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == dvv::Errc::Internal);
    }

    TEST_CASE("the ToDisplay shader's push block matches ToDisplayPush") {
        const auto reflection = dvv::json::parse(dvv_media_shaders::toDisplay().reflectionJson);
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
              sizeof(detail::ToDisplayPush));
        CHECK(sizeof(detail::ToDisplayPush) == 96);
        std::map<std::string, std::uint32_t> offsets;
        for (const auto& field : type.at("elementType").at("fields")) {
            offsets[dvv::json::get<std::string>(field, "name").value_or("?")] =
                dvv::json::get<std::uint32_t>(field.at("binding"), "offset").value_or(~0U);
        }
        const std::map<std::string, std::uint32_t> expected{
            {"row0", 0},   {"row1", 16},   {"row2", 32},           {"planeLayout", 48},
            {"width", 64}, {"height", 68}, {"componentCount", 72}, {"transfer", 76},
            {"pad0", 80},  {"pad1", 84},   {"pad2", 88},           {"pad3", 92}};
        CHECK(offsets == expected);
    }

    TEST_CASE("the ToDisplay shader reads planes without a format") {
        using dvv::media::test::SPIRV_IMAGE_FORMAT;
        using dvv::media::test::SPIRV_IMAGE_SAMPLED;
        using dvv::media::test::SPIRV_OP_CAPABILITY;
        using dvv::media::test::SPIRV_OP_TYPE_IMAGE;
        using dvv::media::test::SpirvInstruction;
        constexpr std::uint32_t STORAGE_IMAGE_READ_WITHOUT_FORMAT = 55;
        constexpr std::uint32_t SAMPLED_STORAGE = 2;  // read and written without a sampler
        constexpr std::uint32_t FORMAT_UNKNOWN = 0;
        constexpr std::uint32_t FORMAT_RGBA16F = 2;
        int unknown_images = 0;
        int rgba16f_images = 0;
        bool read_without_format = false;
        for (const SpirvInstruction& instruction :
             dvv::media::test::spirvInstructions(dvv_media_shaders::toDisplay().spirv)) {
            if (instruction.opcode == SPIRV_OP_TYPE_IMAGE) {
                REQUIRE(instruction.operands.size() > SPIRV_IMAGE_FORMAT);
                if (instruction.operands[SPIRV_IMAGE_SAMPLED] != SAMPLED_STORAGE) {
                    continue;
                }
                const std::uint32_t format = instruction.operands[SPIRV_IMAGE_FORMAT];
                unknown_images += format == FORMAT_UNKNOWN ? 1 : 0;
                rgba16f_images += format == FORMAT_RGBA16F ? 1 : 0;
            } else if (instruction.opcode == SPIRV_OP_CAPABILITY) {
                REQUIRE(!instruction.operands.empty());
                read_without_format = read_without_format ||
                                      instruction.operands[0] == STORAGE_IMAGE_READ_WITHOUT_FORMAT;
            }
        }
        CHECK(read_without_format);
        CHECK(unknown_images >= 1);
        CHECK(rgba16f_images == 1);
    }
}
