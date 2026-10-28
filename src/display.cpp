#include "dvv/media/display.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "format_table.h"

namespace dvv::media {

namespace {

constexpr std::string_view DISPLAY_NAME = "rgba16f@display";

// SMPTE ST 2084 (PQ) constants.
constexpr float PQ_M1 = 0.1593017578125f, PQ_M2 = 78.84375f, PQ_C1 = 0.8359375f,
                PQ_C2 = 18.8515625f, PQ_C3 = 18.6875f;
// ARIB STD-B67 / BT.2100 HLG constants.
constexpr float HLG_A = 0.17883277f, HLG_B = 0.28466892f, HLG_C = 0.55991073f;
constexpr float SOURCE_PEAK_NITS = 1000.0f;  // raw files carry no mastering metadata (D4)
constexpr float TARGET_PEAK_NITS = 100.0f;   // SDR

float pqToNits(float e) {  // ST 2084 EOTF
    e = std::clamp(e, 0.0f, 1.0f);
    const float p = std::pow(e, 1.0f / PQ_M2);
    const float num = std::max(p - PQ_C1, 0.0f);
    return 10000.0f * std::pow(num / (PQ_C2 - PQ_C3 * p), 1.0f / PQ_M1);
}
float nitsToPq(float nits) {  // ST 2084 inverse EOTF
    const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
    const float p = std::pow(y, PQ_M1);
    return std::pow((PQ_C1 + PQ_C2 * p) / (1.0f + PQ_C3 * p), PQ_M2);
}
// BT.2390 EETF in the PQ domain, source peak 1000 nits, target peak 100 nits, black 0 for both
// (so the black-lift step vanishes); values above the source peak clamp to it.
float eetf(float e) {
    const float source = nitsToPq(SOURCE_PEAK_NITS);
    const float max_lum = nitsToPq(TARGET_PEAK_NITS) / source;
    const float ks = 1.5f * max_lum - 0.5f;
    const float e1 = std::min(e / source, 1.0f);
    float e2 = e1;
    if (e1 >= ks) {
        const float t = (e1 - ks) / (1.0f - ks);
        const float t2 = t * t, t3 = t2 * t;
        e2 = (2.0f * t3 - 3.0f * t2 + 1.0f) * ks + (t3 - 2.0f * t2 + t) * (1.0f - ks) +
             (-2.0f * t3 + 3.0f * t2) * max_lum;
    }
    return e2 * source;
}
float pqToLinear(float e) {
    return pqToNits(eetf(e)) / TARGET_PEAK_NITS;
}
float nitsToLinear(float nits) {
    return pqToLinear(nitsToPq(nits));
}
float hlgInverseOetf(float e) {  // BT.2100: signal -> normalised scene light
    return e <= 0.5f ? e * e / 3.0f : (std::exp((e - HLG_C) / HLG_A) + HLG_B) / 12.0f;
}
// HLG: inverse OETF per channel, OOTF with Lw = 1000 and gamma 1.2 (Ys with BT.2020 luma weights),
// then the PQ EETF on the display light.
std::array<float, 3> hlgToLinear(std::array<float, 3> v) {
    const std::array<float, 3> e{hlgInverseOetf(v[0]), hlgInverseOetf(v[1]), hlgInverseOetf(v[2])};
    const float ys = 0.2627f * e[0] + 0.6780f * e[1] + 0.0593f * e[2];
    const float gain = SOURCE_PEAK_NITS * std::pow(ys, 0.2f);
    return {nitsToLinear(gain * e[0]), nitsToLinear(gain * e[1]), nitsToLinear(gain * e[2])};
}

PixelFormatDescriptor displayDescriptor() {
    PixelFormatDescriptor d;
    d.name = std::string(DISPLAY_NAME);
    d.model = ColourModel::Rgb;
    d.componentCount = 4;
    d.components = {ComponentDesc{.kind = ComponentKind::R, .bits = 16},
                    ComponentDesc{.kind = ComponentKind::G, .bits = 16},
                    ComponentDesc{.kind = ComponentKind::B, .bits = 16},
                    ComponentDesc{.kind = ComponentKind::A, .bits = 16}};
    d.planeCount = 1;
    PlaneDesc& plane = d.planes[0];
    plane.groupPixels = 1;
    plane.groupBytes = 8;
    plane.wordBytes = 2;
    plane.order = ByteOrder::Little;
    plane.slotCount = 4;
    for (std::uint8_t c = 0; c < 4; ++c) {
        plane.slots[c] = SampleSlot{.component = c, .index = 0, .word = c, .shift = 0};
    }
    return d;
}

// n = code * scale + offset for one component.
struct Normalisation {
    double scale = 0;
    double offset = 0;
};

// Luma and RGB components: limited (c - 16 s) / (219 s), full c / (2^b - 1), s = 2^(b - 8).
Normalisation lumaNormalisation(unsigned bits, bool limited) {
    if (limited) {
        const double s = std::ldexp(1.0, static_cast<int>(bits) - 8);
        return {.scale = 1.0 / (219.0 * s), .offset = -16.0 * s / (219.0 * s)};
    }
    const double max = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    return {.scale = 1.0 / max, .offset = 0.0};
}

// Chroma components: limited (c - 128 s) / (224 s), full (c - 2^(b - 1)) / (2^b - 1).
Normalisation chromaNormalisation(unsigned bits, bool limited) {
    if (limited) {
        const double s = std::ldexp(1.0, static_cast<int>(bits) - 8);
        return {.scale = 1.0 / (224.0 * s), .offset = -128.0 * s / (224.0 * s)};
    }
    const double max = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
    return {.scale = 1.0 / max, .offset = -std::ldexp(1.0, static_cast<int>(bits) - 1) / max};
}

// The luma weights of a YUV matrix (standardWeights); Unspecified counts as BT.709.
std::array<double, 2> lumaWeights(const ColourParams& colour) {
    if (colour.matrix == ColourMatrix::Custom) {
        return {colour.kr, colour.kb};
    }
    // Unspecified and Rgb fall back to BT.709.
    const std::array<float, 2> weights =
        standardWeights(colour.matrix)
            .or_else([] { return standardWeights(ColourMatrix::Bt709); })
            .value_or(std::array<float, 2>{});
    return {weights[0], weights[1]};
}

using Matrix3 = std::array<std::array<double, 3>, 3>;

DisplayAffine toFloat(const Matrix3& matrix, const std::array<double, 3>& offset) {
    DisplayAffine affine;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            affine.matrix[3 * r + c] = static_cast<float>(matrix[r][c]);
        }
        affine.offset[r] = static_cast<float>(offset[r]);
    }
    return affine;
}

}  // namespace

FormatId displayFormat() {
    static const FormatId ID = [] {
        const PixelFormatDescriptor d = displayDescriptor();
        if (const auto valid = validate(d); !valid) {
            throw std::logic_error("display format: " + valid.error().message);
        }
        const auto id = detail::internCustomFormat(d);
        if (!id) {
            throw std::logic_error("display format: " + id.error().message);
        }
        return *id;
    }();
    return ID;
}

bool isDisplay(FormatId format) noexcept {
    return descriptor(format).name == DISPLAY_NAME;
}

ColourId displayColour() {
    static const ColourId ID = internColour({.matrix = ColourMatrix::Rgb,
                                             .range = ColourRange::Full,
                                             .transfer = TransferFn::Linear,
                                             .primaries = Primaries::Bt709});
    return ID;
}

ViewSpec displayView(const Domain& device_domain) {
    return ViewSpec{.format = displayFormat(),
                    .colour = displayColour(),
                    .layout = LayoutPolicy::Any,
                    .api = Api::VkImage,
                    .domain = device_domain,
                    .downscaleLog2 = 0};
}

ResolvedColour resolveColour(const FrameDesc& desc, FileHint hint) {
    const ColourModel model = descriptor(desc.format).model;
    const ColourParams& given = colourParams(desc.colour);
    ResolvedColour resolved{
        .params = given, .assumed = 0, .standard = standardOf(given, model, hint)};
    // The fields the frame leaves open come from its standard, else from BT.709 (D5R §1.3).
    const ColourParams& fill =
        colourPresetInfo(resolved.standard.value_or(ColourPreset::Bt709)).colour;
    ColourParams& p = resolved.params;
    const auto assume = [&resolved](AssumedColour bit) {
        resolved.assumed = static_cast<std::uint8_t>(resolved.assumed | bit);
    };
    if (p.matrix == ColourMatrix::Unspecified) {
        if (model == ColourModel::Yuv) {
            p.matrix = fill.matrix;
            assume(AssumedMatrix);
        } else {
            p.matrix = ColourMatrix::Rgb;  // the matrix does not apply to RGB and gray
        }
    }
    if (p.range == ColourRange::Unspecified) {
        p.range = defaultRange(model);  // never from the standard (M4-D5-RGBRANGE, all formats)
        assume(AssumedRange);
    }
    if (p.transfer == TransferFn::Unspecified) {
        p.transfer = fill.transfer;
        assume(AssumedTransfer);
    }
    if (p.primaries == Primaries::Unspecified) {
        p.primaries = fill.primaries;
        assume(AssumedPrimaries);
    }
    return resolved;
}

DisplayAffine displayAffine(FormatId format, const ColourParams& resolved) {
    const PixelFormatDescriptor& d = descriptor(format);
    if (d.componentCount == 0) {
        return {};  // unknown id
    }
    const bool limited =
        resolved.range == ColourRange::Limited ||
        (resolved.range == ColourRange::Unspecified && d.model == ColourModel::Yuv);
    const auto bits = [&d](std::size_t c) { return unsigned{d.components[c].bits}; };
    Matrix3 matrix{};
    std::array<double, 3> offset{};

    if (d.model == ColourModel::Gray) {
        const Normalisation y = lumaNormalisation(bits(0), limited);
        for (std::size_t r = 0; r < 3; ++r) {
            matrix[r][0] = y.scale;
            offset[r] = y.offset;
        }
        return toFloat(matrix, offset);
    }
    if (d.model == ColourModel::Rgb) {
        for (std::size_t c = 0; c < 3; ++c) {
            const Normalisation n = lumaNormalisation(bits(c), limited);
            matrix[c][c] = n.scale;
            offset[c] = n.offset;
        }
        return toFloat(matrix, offset);
    }
    if (resolved.matrix == ColourMatrix::Rgb) {
        // FFmpeg's AVCOL_SPC_RGB: the Y, U and V planes hold G, B and R.
        const std::array<std::size_t, 3> source{2, 0, 1};  // R' = n(c2), G' = n(c0), B' = n(c1)
        for (std::size_t r = 0; r < 3; ++r) {
            const Normalisation n = lumaNormalisation(bits(source[r]), limited);
            matrix[r][source[r]] = n.scale;
            offset[r] = n.offset;
        }
        return toFloat(matrix, offset);
    }
    const auto [kr, kb] = lumaWeights(resolved);
    const double kg = 1.0 - kr - kb;
    const Matrix3 m{{{1.0, 0.0, 2.0 * (1.0 - kr)},
                     {1.0, -2.0 * kb * (1.0 - kb) / kg, -2.0 * kr * (1.0 - kr) / kg},
                     {1.0, 2.0 * (1.0 - kb), 0.0}}};
    const std::array<Normalisation, 3> n{lumaNormalisation(bits(0), limited),
                                         chromaNormalisation(bits(1), limited),
                                         chromaNormalisation(bits(2), limited)};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            matrix[r][c] = m[r][c] * n[c].scale;
            offset[r] += m[r][c] * n[c].offset;
        }
    }
    return toFloat(matrix, offset);
}

std::array<float, 4> toDisplayReference(FormatId format, const ColourParams& resolved,
                                        std::span<const std::uint16_t> code_values) noexcept {
    const DisplayAffine affine = displayAffine(format, resolved);
    const std::size_t count = std::min<std::size_t>(
        {descriptor(format).componentCount, std::size_t{3}, code_values.size()});
    std::array<float, 3> c{};
    for (std::size_t k = 0; k < count; ++k) {
        c[k] = static_cast<float>(code_values[k]);
    }
    std::array<float, 3> v{};
    for (std::size_t r = 0; r < 3; ++r) {
        v[r] = std::clamp(affine.matrix[3 * r] * c[0] + affine.matrix[3 * r + 1] * c[1] +
                              affine.matrix[3 * r + 2] * c[2] + affine.offset[r],
                          0.0f, 1.0f);
    }
    switch (resolved.transfer) {
        case TransferFn::Unspecified:
        case TransferFn::Srgb:
            for (float& x : v) {
                x = srgbDecode(x);
            }
            break;
        case TransferFn::Bt1886:
            for (float& x : v) {
                x = std::pow(x, 2.4f);
            }
            break;
        case TransferFn::Linear: break;
        case TransferFn::Pq:
            for (float& x : v) {
                x = pqToLinear(x);
            }
            break;
        case TransferFn::Hlg: v = hlgToLinear(v); break;
    }
    return {std::clamp(v[0], 0.0f, 1.0f), std::clamp(v[1], 0.0f, 1.0f),
            std::clamp(v[2], 0.0f, 1.0f), 1.0f};
}

float srgbEncode(float linear) noexcept {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

float srgbDecode(float encoded) noexcept {
    const float x = std::clamp(encoded, 0.0f, 1.0f);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t encodeSrgb8(float linear) noexcept {
    return static_cast<std::uint8_t>(std::lround(255.0f * srgbEncode(linear)));
}

std::uint16_t floatToHalf(float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const std::uint32_t magnitude = bits & 0x7FFFFFFFU;
    if (magnitude > 0x7F800000U) {
        return static_cast<std::uint16_t>(sign | 0x7E00U);  // NaN: a quiet NaN
    }
    if (magnitude == 0x7F800000U) {
        return static_cast<std::uint16_t>(sign | 0x7C00U);  // infinity
    }
    const int exponent = static_cast<int>(magnitude >> 23U) - 127;
    const std::uint32_t mantissa = magnitude & 0x7FFFFFU;
    if (exponent > 15) {
        return static_cast<std::uint16_t>(sign | 0x7C00U);  // overflow
    }
    // The half without the sign as an integer, and the discarded bits relative to their half unit.
    std::uint32_t half = 0;
    std::uint32_t rest = 0;
    std::uint32_t halfway = 0;
    if (exponent >= -14) {
        // Normal: 10 of the 23 mantissa bits; a carry out of the mantissa raises the exponent
        // (and turns 65520 and above into infinity).
        half = (static_cast<std::uint32_t>(exponent + 15) << 10U) | (mantissa >> 13U);
        rest = mantissa & 0x1FFFU;
        halfway = 0x1000U;
    } else {
        // Subnormal: value / 2^-24 = (1.mantissa) * 2^(exponent + 24).
        const int shift = -exponent - 1;  // 14 for exponent -15, 24 for exponent -25
        if (shift > 24) {
            return sign;  // below half of the smallest subnormal
        }
        const std::uint32_t significand = mantissa | 0x800000U;
        half = significand >> static_cast<unsigned>(shift);
        rest = significand & ((1U << static_cast<unsigned>(shift)) - 1U);
        halfway = 1U << static_cast<unsigned>(shift - 1);
    }
    if (rest > halfway || (rest == halfway && (half & 1U) != 0)) {
        ++half;
    }
    return static_cast<std::uint16_t>(sign | half);
}

float halfToFloat(std::uint16_t half) noexcept {
    const std::uint32_t sign = (static_cast<std::uint32_t>(half) & 0x8000U) << 16U;
    const std::uint32_t exponent = (half >> 10U) & 0x1FU;
    const std::uint32_t mantissa = half & 0x3FFU;
    if (exponent == 0) {
        // Zero or subnormal: mantissa * 2^-24, exact in float.
        const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);
        return sign != 0 ? -magnitude : magnitude;
    }
    if (exponent == 0x1FU) {
        // Infinity, or NaN with the payload's top bits.
        return std::bit_cast<float>(sign | 0x7F800000U | (mantissa << 13U));
    }
    // Normal: rebias the exponent from 15 to 127.
    return std::bit_cast<float>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

}  // namespace dvv::media
