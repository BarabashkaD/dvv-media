#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alpha_formats.h"
#include "canonical_hidden.h"
#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/display.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv_media_shaders/to_display.h"
#include "result_message.h"
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

// The push constants of a 33x17 frame of `format` with this colour and hidden set, resolved as
// ToDisplay does.
detail::ToDisplayPush pushOf(media::FormatId format, const ColourParams& colour,
                             media::HiddenComponents hidden = {}) {
    const media::FrameDesc desc{
        .format = format, .colour = media::internColour(colour), .width = 33, .height = 17};
    const auto push =
        detail::toDisplayPush(desc.format, media::resolveColour(desc).params, 33, 17, hidden);
    REQUIRE_MESSAGE(push.has_value(), (push.has_value() ? std::string{} : push.error().describe()));
    return *push;
}

detail::ToDisplayPush pushOf(std::string_view name, const ColourParams& colour,
                             media::HiddenComponents hidden = {}) {
    return pushOf(idOf(name), colour, hidden);
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
        CHECK(push.transfer == 1);       // the default curve is BT.1886 (D5R section 1.4)
        CHECK(push.alphaScale == 0.0F);  // no alpha component
        CHECK(push.width == 33);
        CHECK(push.height == 17);
        CHECK(push.hiddenMask == 0);
        CHECK(push.chromaFill == 0);
        CHECK(push.pad == 0);

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
        // The alpha scale: 1 / (2^bits - 1) of the A component, 0 without one.
        CHECK(pushOf("yuv420p", {}).alphaScale == 0.0F);
        CHECK(pushOf("gray", {}).alphaScale == 0.0F);
        CHECK(pushOf("rgba", {}).alphaScale == 1.0F / 255.0F);
        CHECK(pushOf("rgba64le", {}).alphaScale == 1.0F / 65535.0F);

        const ColourParams resolved{.matrix = ColourMatrix::Bt709,
                                    .range = ColourRange::Limited,
                                    .transfer = TransferFn::Srgb,
                                    .primaries = Primaries::Bt709};
        const auto empty = detail::toDisplayPush(idOf("gray"), resolved, 0, 17, {});
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == dvv::Errc::Internal);
        const auto unknown =
            detail::toDisplayPush(media::FormatId{0xFFFFFFF0U}, resolved, 4, 4, {});
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
            {"row0", 0},        {"row1", 16},       {"row2", 32},           {"planeLayout", 48},
            {"width", 64},      {"height", 68},     {"componentCount", 72}, {"transfer", 76},
            {"alphaScale", 80}, {"hiddenMask", 84}, {"chromaFill", 88},     {"pad", 92}};
        CHECK(offsets == expected);
    }

    TEST_CASE("hidden: ToDisplay push constants") {
        using media::ChromaFill;
        using media::HiddenComponents;
        static_assert(offsetof(detail::ToDisplayPush, hiddenMask) == 84);
        static_assert(offsetof(detail::ToDisplayPush, chromaFill) == 88);
        static_assert(offsetof(detail::ToDisplayPush, pad) == 92);
        static_assert(sizeof(detail::ToDisplayPush) == 96);
        CHECK(offsetof(detail::ToDisplayPush, hiddenMask) == 84);
        CHECK(offsetof(detail::ToDisplayPush, chromaFill) == 88);
        CHECK(offsetof(detail::ToDisplayPush, pad) == 92);
        CHECK(sizeof(detail::ToDisplayPush) == 96);

        constexpr HiddenComponents Y_AND_V{.components = {true, false, true}};
        constexpr HiddenComponents U_AND_V{.components = {false, true, true}};
        constexpr HiddenComponents U_AND_V_ZERO{.components = {false, true, true},
                                                .chroma = ChromaFill::Zero};
        constexpr HiddenComponents ALL{.components = {true, true, true}};
        constexpr HiddenComponents FIRST{.components = {true, false, false}};

        // Only V of the chroma is hidden, so U's half of the fill is 0.
        const detail::ToDisplayPush y_and_v = pushOf("yuv420p", {}, Y_AND_V);
        CHECK(y_and_v.hiddenMask == 0b101U);
        CHECK(y_and_v.chromaFill == 128U << 16U);
        const detail::ToDisplayPush u_and_v = pushOf("yuv420p", {}, U_AND_V);
        CHECK(u_and_v.hiddenMask == 0b110U);
        CHECK(u_and_v.chromaFill == (128U | 128U << 16U));
        const detail::ToDisplayPush zero = pushOf("yuv420p", {}, U_AND_V_ZERO);
        CHECK(zero.hiddenMask == 0b110U);
        CHECK(zero.chromaFill == 0);
        CHECK(pushOf("yuv420p10le", {}, U_AND_V).chromaFill == (512U | 512U << 16U));
        CHECK(pushOf("yuv444p16le", {}, U_AND_V).chromaFill == (32768U | 32768U << 16U));
        // Review focus: MSB-aligned high-bit YUV. The mid code is in the unpacked value's units,
        // and 32768 fits its 16-bit half.
        CHECK(pushOf("p010le", {}, U_AND_V).chromaFill == (512U | 512U << 16U));
        CHECK(pushOf("p016le", {}, U_AND_V).chromaFill == (32768U | 32768U << 16U));

        // Without chroma (RGB, gray, YUV read with the Rgb matrix) the fill is 0.
        const detail::ToDisplayPush rgb = pushOf("rgb24", {}, ALL);
        CHECK(rgb.hiddenMask == 0b111U);
        CHECK(rgb.chromaFill == 0);
        const detail::ToDisplayPush gray = pushOf("gray", {}, FIRST);
        CHECK(gray.hiddenMask == 1);
        CHECK(gray.chromaFill == 0);
        const detail::ToDisplayPush as_rgb =
            pushOf("yuv444p", {.matrix = ColourMatrix::Rgb}, U_AND_V);
        CHECK(as_rgb.hiddenMask == 0b110U);
        CHECK(as_rgb.chromaFill == 0);

        // Review focus: gray + alpha. The canonical set hides only Y; the alpha is never c1.
        const auto ya8 = media::test::loadFormat(media::test::YA8_JSON);
        REQUIRE_MESSAGE(ya8.has_value(), media::test::messageOf(ya8));
        const detail::ToDisplayPush gray_alpha = pushOf(*ya8, {}, FIRST);
        CHECK(gray_alpha.hiddenMask == 1);
        CHECK(gray_alpha.chromaFill == 0);
        CHECK(gray_alpha.componentCount == 2);
        // A non-canonical set never reaches the alpha: only colour components enter the mask.
        CHECK(pushOf(*ya8, {}, ALL).hiddenMask == 1);
        CHECK(pushOf("rgba", {}, ALL).hiddenMask == 0b111U);

        // Nothing hidden: the three words stay 0 for every format.
        for (const media::FormatId format : media::catalogue()) {
            CAPTURE(media::formatDesc(format).name);
            const detail::ToDisplayPush push = pushOf(format, {});
            CHECK(push.hiddenMask == 0);
            CHECK(push.chromaFill == 0);
            CHECK(push.pad == 0);
        }
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

    TEST_CASE("hidden: canonical form") {
        using media::ChromaFill;
        using media::HiddenComponents;
        const auto desc_of = [](media::FormatId format, const ColourParams& colour = {}) {
            return media::FrameDesc{
                .format = format, .colour = media::internColour(colour), .width = 4, .height = 4};
        };
        CHECK(detail::canonicalHidden(desc_of(idOf("gray")), {.components = {false, true, true}}) ==
              HiddenComponents{});
        CHECK(detail::canonicalHidden(desc_of(idOf("rgb24")), {.components = {true, false, false},
                                                               .chroma = ChromaFill::Zero}) ==
              HiddenComponents{.components = {true, false, false}});
        CHECK(detail::canonicalHidden(
                  desc_of(idOf("yuv444p"), {.matrix = ColourMatrix::Rgb}),
                  {.components = {false, true, false}, .chroma = ChromaFill::Zero}) ==
              HiddenComponents{.components = {false, true, false}});
        CHECK(detail::canonicalHidden(desc_of(idOf("yuv420p")), {.components = {true, false, false},
                                                                 .chroma = ChromaFill::Zero}) ==
              HiddenComponents{.components = {true, false, false}});
        const HiddenComponents chroma_zero{.components = {false, true, false},
                                           .chroma = ChromaFill::Zero};
        CHECK(detail::canonicalHidden(desc_of(idOf("yuv420p")), chroma_zero) == chroma_zero);
        // Review focus: a gray + alpha format keeps only Y; the alpha component is never hidden.
        const auto ya8 = media::test::loadFormat(media::test::YA8_JSON);
        REQUIRE_MESSAGE(ya8.has_value(), media::test::messageOf(ya8));
        CHECK(detail::canonicalHidden(desc_of(*ya8), {.components = {true, true, true}}) ==
              HiddenComponents{.components = {true, false, false}});

        // RGB has no chroma: a hidden G or B with the Zero fill gets Mid (the reset by model).
        CHECK(detail::canonicalHidden(desc_of(idOf("rgb24")), {.components = {false, true, false},
                                                               .chroma = ChromaFill::Zero}) ==
              HiddenComponents{.components = {false, true, false}});
        CHECK(detail::canonicalHidden(desc_of(idOf("rgb24")), {.components = {false, false, true},
                                                               .chroma = ChromaFill::Zero}) ==
              HiddenComponents{.components = {false, false, true}});
        // A hidden V alone counts as hidden chroma: the Zero fill is kept.
        const HiddenComponents v_zero{.components = {false, false, true},
                                      .chroma = ChromaFill::Zero};
        CHECK(detail::canonicalHidden(desc_of(idOf("yuv420p")), v_zero) == v_zero);
        // Four components (colour + alpha): all three colour components stay hidden.
        const HiddenComponents all_zero{.components = {true, true, true},
                                        .chroma = ChromaFill::Zero};
        CHECK(detail::canonicalHidden(desc_of(idOf("rgba")), all_zero) ==
              HiddenComponents{.components = {true, true, true}});
        const auto yuva = media::test::loadFormat(media::test::YUVA10_JSON);
        REQUIRE_MESSAGE(yuva.has_value(), media::test::messageOf(yuva));
        CHECK(detail::canonicalHidden(desc_of(*yuva), all_zero) == all_zero);

        // Idempotent for every catalogue and custom format, every set and both fills.
        std::vector<media::FormatId> formats(media::catalogue().begin(), media::catalogue().end());
        formats.push_back(*ya8);
        formats.push_back(*yuva);
        for (const media::FormatId format : formats) {
            CAPTURE(media::formatDesc(format).name);
            const media::FrameDesc desc = desc_of(format);
            for (unsigned bits = 0; bits < 8; ++bits) {
                for (const ChromaFill fill : {ChromaFill::Mid, ChromaFill::Zero}) {
                    const HiddenComponents hidden{
                        .components = {(bits & 1U) != 0, (bits & 2U) != 0, (bits & 4U) != 0},
                        .chroma = fill};
                    const HiddenComponents once = detail::canonicalHidden(desc, hidden);
                    CHECK(detail::canonicalHidden(desc, once) == once);
                }
            }
        }
    }
}
