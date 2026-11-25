// shaders/view_lines.slang against its C++ mirror (dvv/media/view_lines.h): the probe shader
// (tests/shaders/view_lines_probe.slang) writes every function for one axis into an R32_UINT image,
// one column i per value; this file computes the same with the mirror and compares.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "dvv/gpu/command_recorder.h"
#include "dvv/gpu/compute_pipeline.h"
#include "dvv/gpu/context_vulkan.h"
#include "dvv/gpu/execution_context.h"
#include "dvv/gpu/image.h"
#include "dvv/gpu/resources.h"
#include "dvv/media/view_lines.h"
#include "gpu_fixture.h"
#include "media_test_shaders/view_lines_probe.h"

namespace gpu = dvv::gpu;
using dvv::media::AxisView;
using dvv::media::cursorSpan;
using dvv::media::firstPixelOf;
using dvv::media::haloSpan;
using dvv::media::imageCoordinateAt;
using dvv::media::imageSpan;
using dvv::media::levelVisible;
using dvv::media::LINE_LEVELS;
using dvv::media::lineAt;
using dvv::media::LineHit;
using dvv::media::LineLevel;
using dvv::media::lineSpan;
using dvv::media::numberStepLog2;
using dvv::media::pixelCentreAt;
using dvv::media::PixelRange;
using dvv::media::PixelSpan;
using dvv::media::tickLevel;
using dvv::media::visiblePixels;
using dvv::media::test::GpuTest;

namespace {

using Levels = std::array<LineLevel, LINE_LEVELS>;

// The probe's push constants (ProbePush in view_lines_probe.slang).
struct ProbePush {
    std::int32_t offset = 0;
    float zoom = 0;
    std::uint32_t size = 0;
    std::uint32_t extent = 0;
    std::array<std::array<std::uint32_t, 4>, LINE_LEVELS> levels{};  // stepLog2, t, drawn, halo
    std::uint32_t width = 0;
    float minimum = 0;
    std::uint32_t majorStepLog2 = 0;  // numberStepLog2's major step
    float neededSpacing = 0;          // numberStepLog2's needed spacing
};
static_assert(sizeof(ProbePush) == 80);

// The probe's rows. Row r holds, for column i: 0 boundaryAt(k = i) (bits), 1 imageCoordinateAt(
// pixel = i) (bits), 2 firstPixelOf(k = i), 3..10 lineSpan(k = i, t = 1..4) begin/end, 11..12
// haloSpan(k = i, t = 2), 13..14 cursorSpan(c = i), 15..16 imageSpan, 17 lineAt(pixel = i) packed,
// 18 tickLevel(k = i) (3: none), 19 levelVisible(stepLog2 = i, zoom, minimum), 20..22
// visiblePixels of the axis panned by i pixels (offset - i, wrapping): valid, first, last, 23
// numberStepLog2(majorStepLog2, zoom x 2^(i mod 16 - 8), neededSpacing, size), 24
// pixelCentreAt(c = i) (bits).
constexpr std::uint32_t ROWS = 25;
constexpr std::uint32_t PROBE_GROUP = 64;
constexpr float MINIMUM_SPACING = 8;
// The lines lineAt's allowance looks at on each side of the pixel's own (enough for every case's
// spacing and thickness).
constexpr std::int64_t LINE_WINDOW = 8;

std::uint32_t packHit(const std::optional<LineHit>& hit) {
    if (!hit) {
        return 0xFFFFFFFFU;
    }
    return static_cast<std::uint32_t>(hit->level) | hit->k << 2U | (hit->halo ? 1U : 0U) << 31U;
}

bool dyadic(float zoom) {
    int exponent = 0;
    return std::isfinite(zoom) && std::frexp(zoom, &exponent) == 0.5F;
}

// A GPU divide may be 2.5 ulp off (Vulkan), so a GPU u may lie on the other side of `target` than
// the mirror's only when the mirror's u lies within a few ulp of it.
bool nearValue(float value, float target) {
    const float magnitude = std::max(std::fabs(value), std::fabs(target));
    const float ulp = std::nextafter(magnitude, std::numeric_limits<float>::infinity()) - magnitude;
    return std::fabs(value - target) <= 4 * ulp;
}

bool nearInteger(float value) {
    return std::isfinite(value) && nearValue(value, std::round(value));
}

// The distance in ulp between two floats (bit patterns in one ordered integer line); two NaNs: 0.
std::int64_t ulpDistance(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) {
        return 0;
    }
    const auto ordered = [](float v) {
        const auto bits = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(v));
        return bits < 0 ? std::int64_t{std::numeric_limits<std::int32_t>::min()} - bits : bits;
    };
    return std::abs(ordered(a) - ordered(b));
}

struct Tally {
    std::size_t exact = 0;      // values equal to the mirror
    std::size_t allowance = 0;  // values within the non-dyadic allowance, not equal
    std::size_t failures = 0;
    std::size_t coordinatesOff = 0;  // row 1 values that are not the mirror's bit for bit
    std::size_t hitsOpen = 0;        // lineAt pixels open to the allowance
    std::size_t edgesOff = 0;        // visiblePixels values within the allowance
};

class Comparison {
public:
    // strict: no allowance at all (a dyadic zoom, or a case that must compare exactly).
    Comparison(const AxisView& axis, const Levels& levels, std::span<const std::uint32_t> rows,
               std::uint32_t width, bool strict)
        : axis_(axis), levels_(levels), rows_(rows), width_(width), strict_(strict) {}

    Tally run() {
        for (std::uint32_t i = 0; i < width_; ++i) {
            const auto pixel = static_cast<std::int32_t>(i);
            coordinate(0, i, dvv::media::boundaryAt(axis_, i), 0);
            coordinate(1, i, imageCoordinateAt(axis_, pixel), strict_ ? 0 : 3);
            nearly(2, i, std::bit_cast<std::uint32_t>(firstPixelOf(axis_, i)), fragile(i));
            const bool line_fragile = fragile(i) || fragile(0) || fragile(axis_.size);
            for (std::uint32_t t = 1; t <= 4; ++t) {
                span(1 + 2 * t, i, lineSpan(axis_, i, t), line_fragile);
            }
            span(11, i, haloSpan(axis_, i, 2), line_fragile);
            span(13, i, cursorSpan(axis_, i), line_fragile || fragile(i + 1));
            span(15, i, imageSpan(axis_), fragile(0) || fragile(axis_.size));
            hit(i);
            const std::optional<std::size_t> tick = tickLevel(i, levels_);
            exactly(18, i, tick ? static_cast<std::uint32_t>(*tick) : 3U);
            exactly(19, i, levelVisible(i, axis_.zoom, MINIMUM_SPACING) ? 1U : 0U);
            visible(i);
            // Multiplication is correctly rounded on the GPU: these compare exactly at any zoom.
            const float zoom = axis_.zoom * std::ldexp(1.0F, static_cast<int>(i % 16) - 8);
            exactly(23, i, numberStepLog2(major_, zoom, needed_, axis_.size));
            coordinate(24, i, pixelCentreAt(axis_, i), 0);
        }
        return tally_;
    }

    // numberStepLog2's inputs besides the zoom and the size.
    Comparison& numberStep(std::uint32_t major, float needed) {
        major_ = major;
        needed_ = needed;
        return *this;
    }

private:
    [[nodiscard]] std::uint32_t at(std::uint32_t row, std::uint32_t i) const {
        return rows_[(std::size_t{row} * width_) + i];
    }

    void fail(std::uint32_t row, std::uint32_t i, std::uint32_t expected) {
        if (tally_.failures < 8) {
            INFO("row ", row, ", i ", i, ": GPU ", at(row, i), ", mirror ", expected);
            CHECK(at(row, i) == expected);
        }
        ++tally_.failures;
    }

    void exactly(std::uint32_t row, std::uint32_t i, std::uint32_t expected) {
        if (at(row, i) == expected) {
            ++tally_.exact;
        } else {
            fail(row, i, expected);
        }
    }

    // firstPixelOf(k) may move by one on the GPU when u at either pixel it tests is near k.
    [[nodiscard]] bool fragile(std::uint32_t k) const { return fragileOn(axis_, k); }

    [[nodiscard]] bool fragileOn(const AxisView& axis, std::uint32_t k) const {
        if (strict_) {
            return false;
        }
        const std::int32_t p = firstPixelOf(axis, k);
        const auto target = static_cast<float>(k);
        return nearValue(imageCoordinateAt(axis, p - 1), target) ||
               nearValue(imageCoordinateAt(axis, p), target);
    }

    // visiblePixels of the axis panned by i pixels. Its ends test boundaryAt, which is exact; off
    // a dyadic zoom the image point at a cut end, near a whole number, may floor or ceil to the
    // next image pixel (first or last moves by one), and an end of the image span may move by one
    // pixel, so a span of at most one pixel may appear or vanish. These divides take integer
    // numerators that row 1 does not sample: their allowance is tallied apart.
    void visible(std::uint32_t i) {
        AxisView panned = axis_;
        panned.offset = std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(axis_.offset) - i);
        const std::optional<PixelRange> expected = visiblePixels(panned);
        const PixelSpan image = imageSpan(panned);
        const auto edge = [&](std::int32_t pixel) {
            const float shifted = static_cast<float>(pixel) - static_cast<float>(panned.offset);
            return nearInteger(shifted / panned.zoom);
        };
        const bool valid = at(20, i) != 0;
        if (valid == expected.has_value() &&
            (!valid || (at(21, i) == expected->first && at(22, i) == expected->last))) {
            tally_.exact += 3;
            return;
        }
        const auto extent = static_cast<std::int32_t>(std::min(panned.extent, 0x7FFFFFFFU));
        // An end may move only where the viewport cuts it at a near-whole image point.
        const auto near = [&](std::uint32_t row, std::uint32_t want, bool cut, std::int32_t pixel) {
            const std::uint32_t got = at(row, i);
            return got == want || (cut && edge(pixel) && std::abs(std::int64_t{got} - want) <= 1);
        };
        const bool cut_begin = dvv::media::boundaryAt(panned, 0) < 0.0F;
        const bool cut_end =
            dvv::media::boundaryAt(panned, panned.size) > static_cast<float>(extent);
        const std::int64_t length = std::int64_t{image.end} - image.begin;
        const bool allowed =
            !strict_ &&
            (valid == expected.has_value()
                 ? near(21, expected->first, cut_begin, 0) &&
                       near(22, expected->last, cut_end, extent)
                 : (fragileOn(panned, 0) || fragileOn(panned, panned.size)) && length <= 1);
        if (allowed) {
            tally_.allowance += 3;
            tally_.edgesOff += 3;
            return;
        }
        fail(20, i, expected ? 1U : 0U);
        fail(21, i, expected ? expected->first : 0U);
        fail(22, i, expected ? expected->last : 0U);
    }

    // lineAt(pixel) may differ on the GPU only where its window can move (q near an integer for a
    // drawn level), where a line or halo that can reach the pixel starts at a fragile
    // firstPixelOf, or next to a fragile end of the image span.
    [[nodiscard]] bool hitFragile(std::int32_t pixel) const {
        if (strict_ || imageSpan(axis_).empty()) {
            return false;
        }
        const auto reaches = [&](std::uint32_t k, std::int64_t reach) {
            return fragile(k) && std::abs(std::int64_t{firstPixelOf(axis_, k)} - pixel) <= reach;
        };
        if (reaches(0, 1) || reaches(axis_.size, 1)) {
            return true;
        }
        const float u = imageCoordinateAt(axis_, pixel);
        for (const LineLevel& level : levels_) {
            if (!level.drawn || level.thickness == 0) {
                continue;
            }
            const std::uint32_t shift = std::min<std::uint32_t>(level.stepLog2, 31);
            const float q = u * std::ldexp(1.0F, -static_cast<int>(shift));
            if (nearInteger(q)) {
                return true;
            }
            const std::int64_t last = level.stepLog2 >= 32 ? 0 : axis_.size >> shift;
            const auto n = static_cast<std::int64_t>(
                std::clamp(std::floor(q), -2.0F * LINE_WINDOW, static_cast<float>(last)));
            const std::int64_t t = std::min<std::uint32_t>(level.thickness, 0x20000000U);
            for (std::int64_t m = std::max<std::int64_t>(0, n - LINE_WINDOW);
                 m <= std::min(last, n + LINE_WINDOW); ++m) {
                const auto k = static_cast<std::uint32_t>(static_cast<std::uint64_t>(m) << shift);
                const bool edge = k == 0 || k == axis_.size;
                if (reaches(k, (edge ? t : t / 2) + 2)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Rows 0, 1 and 24 (floats): NaNs equal, whatever their bits; row 1 may be `allowed` ulp off.
    void coordinate(std::uint32_t row, std::uint32_t i, float expected, std::int64_t allowed) {
        const float got = std::bit_cast<float>(at(row, i));
        const std::int64_t distance = ulpDistance(got, expected);
        if (row == 1 && distance != 0) {
            ++tally_.coordinatesOff;
        }
        if (distance == 0) {
            ++tally_.exact;
        } else if (distance <= allowed) {
            ++tally_.allowance;
        } else {
            fail(row, i, std::bit_cast<std::uint32_t>(expected));
        }
    }

    void nearly(std::uint32_t row, std::uint32_t i, std::uint32_t expected, bool allowed) {
        const auto got = std::bit_cast<std::int32_t>(at(row, i));
        const auto want = std::bit_cast<std::int32_t>(expected);
        if (got == want) {
            ++tally_.exact;
        } else if (allowed && std::abs(std::int64_t{got} - want) <= 1) {
            ++tally_.allowance;
        } else {
            fail(row, i, expected);
        }
    }

    void span(std::uint32_t row, std::uint32_t i, PixelSpan expected, bool allowed) {
        const PixelSpan got{std::bit_cast<std::int32_t>(at(row, i)),
                            std::bit_cast<std::int32_t>(at(row + 1, i))};
        if (got == expected) {
            tally_.exact += 2;
            return;
        }
        // One endpoint moved by one pixel, or a one-pixel span appeared or vanished at a clip.
        const auto length = [](PixelSpan s) { return std::int64_t{s.end} - s.begin; };
        const bool close = (std::abs(std::int64_t{got.begin} - expected.begin) <= 1 &&
                            std::abs(std::int64_t{got.end} - expected.end) <= 1) ||
                           (got.empty() && length(expected) <= 1) ||
                           (expected.empty() && length(got) <= 1);
        if (allowed && close) {
            tally_.allowance += 2;
            return;
        }
        fail(row, i, std::bit_cast<std::uint32_t>(expected.begin));
        fail(row + 1, i, std::bit_cast<std::uint32_t>(expected.end));
    }

    void hit(std::uint32_t i) {
        const auto pixel = static_cast<std::int32_t>(i);
        const std::uint32_t expected = packHit(lineAt(axis_, levels_, pixel));
        const bool open = hitFragile(pixel);
        tally_.hitsOpen += open ? 1 : 0;
        if (at(17, i) == expected) {
            ++tally_.exact;
            return;
        }
        if (open && (at(17, i) == packHit(lineAt(axis_, levels_, pixel - 1)) ||
                     at(17, i) == packHit(lineAt(axis_, levels_, pixel + 1)))) {
            ++tally_.allowance;
            return;
        }
        fail(17, i, expected);
    }

    AxisView axis_;
    Levels levels_;
    std::span<const std::uint32_t> rows_;
    std::uint32_t width_;
    bool strict_;
    std::uint32_t major_ = 0;
    float needed_ = 0;
    Tally tally_;
};

// Runs the probe for one axis; the rows, read back.
std::vector<std::uint32_t> runProbe(GpuTest& test, const gpu::ComputePipeline& pipeline,
                                    const ProbePush& push) {
    auto created = gpu::createImage(
        test.context(), {.width = push.width,
                         .height = ROWS,
                         .format = VK_FORMAT_R32_UINT,
                         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         .name = "view lines probe"});
    REQUIRE_MESSAGE(created.has_value(),
                    (created.has_value() ? std::string{} : created.error().describe()));
    const std::shared_ptr<gpu::GpuImage> image = std::move(*created);
    const std::uint64_t bytes = std::uint64_t{push.width} * ROWS * 4;
    auto buffer = gpu::createBuffer(test.context(), {.size = bytes,
                                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     .memory = gpu::MemoryUsage::HostReadback,
                                                     .name = "view lines readback"});
    REQUIRE_MESSAGE(buffer.has_value(),
                    (buffer.has_value() ? std::string{} : buffer.error().describe()));
    const std::shared_ptr<gpu::GpuBuffer> readback = std::move(*buffer);
    const std::array<gpu::GpuImage*, 1> bindings{image.get()};
    const auto submitted = test.submitAndWait([&](gpu::CommandRecorder& recorder) {
        recorder.dispatch(pipeline, std::as_bytes(std::span(&push, 1)), bindings,
                          gpu::GroupCount{.x = (push.width + PROBE_GROUP - 1) / PROBE_GROUP});
        recorder.copyImageToBuffer(*image, *readback);
    });
    REQUIRE_MESSAGE(submitted.has_value(),
                    (submitted.has_value() ? std::string{} : submitted.error().describe()));
    const auto read = readback->mapRead();
    REQUIRE_MESSAGE(read.has_value(), (read.has_value() ? std::string{} : read.error().describe()));
    const std::span<const std::byte> mapped = read->bytes();
    REQUIRE(mapped.size() >= bytes);
    std::vector<std::uint32_t> rows(static_cast<std::size_t>(bytes / 4));
    std::memcpy(rows.data(), mapped.data(), static_cast<std::size_t>(bytes));
    return rows;
}

}  // namespace

TEST_CASE_FIXTURE(GpuTest, "view lines: the shader matches the C++ mirror") {
    auto created = gpu::createComputePipeline(context(), media_test_shaders::viewLinesProbe(),
                                              sizeof(ProbePush), {.storageImages = 1});
    REQUIRE_MESSAGE(created.has_value(),
                    (created.has_value() ? std::string{} : created.error().describe()));
    const std::shared_ptr<gpu::ComputePipeline> pipeline = std::move(*created);
    // The levels of the CPU lineAt case: a haloed coarse level, a fine level, an undrawn one.
    const Levels levels{{{.stepLog2 = 5, .thickness = 2, .drawn = true, .halo = true},
                         {.stepLog2 = 3, .thickness = 1, .drawn = true},
                         {.stepLog2 = 1, .thickness = 1, .drawn = false}}};
    // The CPU dense case, below the precondition spacing >= thickness: the shader's window must
    // pick the same k as the mirror's. It catches a window moved up by one candidate; a window
    // moved down or narrowed to three gives the same lines here.
    const Levels dense{{{.stepLog2 = 1, .thickness = 8, .drawn = true, .halo = true}, {}, {}}};
    // The edges: a step of 2^32 (only k = 0; a shift by 32 is undefined in SPIR-V), a step of 2^40
    // (lineAt's shift 31), thickness 0 on a drawn level and a thickness above the 2^29 limit.
    const Levels edges{{{.stepLog2 = 32, .thickness = 0, .drawn = true, .halo = true},
                        {.stepLog2 = 40, .thickness = 3, .drawn = true, .halo = true},
                        {.stepLog2 = 31, .thickness = 0xFFFFFFFFU, .drawn = true, .halo = true}}};
    constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
    constexpr float INFINITE = std::numeric_limits<float>::infinity();
    constexpr std::int32_t INT32_HIGH = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t INT32_LOW = std::numeric_limits<std::int32_t>::min();
    struct Case {
        AxisView axis;
        Levels levels;
        bool strict = false;  // no allowance at all, at any zoom
    };
    const std::array<Case, 17> cases{{
        {{.offset = 10, .zoom = 4, .size = 8, .extent = 100}, levels},
        {{.offset = -6, .zoom = 4, .size = 8, .extent = 20}, levels},
        {{.offset = 3, .zoom = 0.473F, .size = 3840, .extent = 1816}, levels},
        {{.offset = 0, .zoom = 0.125F, .size = 64, .extent = 8}, levels},
        {{.offset = 0, .zoom = 32, .size = 1, .extent = 64}, levels},
        {{.offset = 3, .zoom = 1.9F, .size = 200, .extent = 400}, dense},
        // firstPixelOf's correction moves the estimate down and up here (about 100 and 140 k).
        {{.offset = 3, .zoom = 0.3F, .size = 4096, .extent = 4096}, levels},
        // The whole picture at a Fit of 1/3 whose last pixel starts past the span's last centre.
        {{.offset = 293, .zoom = 1.0F / 3.0F, .size = 4000, .extent = 1920}, levels},
        // No image: zoom 0, NaN or infinite, size 0, extent 0.
        {{.offset = 0, .zoom = 0, .size = 8, .extent = 16}, levels, true},
        {{.offset = 0, .zoom = NOT_A_NUMBER, .size = 8, .extent = 16}, levels, true},
        {{.offset = 0, .zoom = INFINITE, .size = 8, .extent = 16}, levels, true},
        {{.offset = 0, .zoom = 4, .size = 0, .extent = 16}, levels, true},
        {{.offset = 0, .zoom = 4, .size = 8, .extent = 0}, levels, true},
        // Saturation at 2^30: above only, and at both ends with an image between.
        {{.offset = INT32_HIGH, .zoom = 4, .size = 8, .extent = 16}, levels},
        {{.offset = INT32_LOW, .zoom = 0x1p31F, .size = 8, .extent = 16}, levels},
        {{.offset = 10, .zoom = 4, .size = 8, .extent = 100}, edges},
        {{.offset = -3, .zoom = 2, .size = 16, .extent = 40}, edges},
    }};
    Tally total;
    std::uint32_t index = 0;
    for (const auto& [axis, case_levels, strict] : cases) {
        ProbePush push{.offset = axis.offset,
                       .zoom = axis.zoom,
                       .size = axis.size,
                       .extent = axis.extent,
                       .width = std::max(axis.size + 2, axis.extent),
                       .minimum = MINIMUM_SPACING,
                       .majorStepLog2 = index % 6,
                       .neededSpacing = 37.5F + static_cast<float>(index)};
        ++index;
        for (std::size_t l = 0; l < LINE_LEVELS; ++l) {
            push.levels[l] = {case_levels[l].stepLog2, case_levels[l].thickness,
                              case_levels[l].drawn ? 1U : 0U, case_levels[l].halo ? 1U : 0U};
        }
        const std::vector<std::uint32_t> rows = runProbe(*this, *pipeline, push);
        CAPTURE(axis.offset);
        CAPTURE(axis.zoom);
        const Tally tally =
            Comparison(axis, case_levels, rows, push.width, strict || dyadic(axis.zoom))
                .numberStep(push.majorStepLog2, push.neededSpacing)
                .run();
        CHECK(tally.failures == 0);
        // Where the GPU's u equals the mirror's everywhere, every value must equal it too: the
        // allowance is only for a divide that differs. visiblePixels divides integer numerators
        // that row 1 does not sample, so its allowance does not count here.
        if (tally.coordinatesOff == 0) {
            CHECK(tally.allowance == tally.edgesOff);
        }
        MESSAGE("zoom ", axis.zoom, ", offset ", axis.offset, ": ", tally.exact, " exact, ",
                tally.allowance, " within the allowance, ", tally.hitsOpen,
                " lineAt pixels open to it");
        total.exact += tally.exact;
        total.allowance += tally.allowance;
        total.failures += tally.failures;
    }
    MESSAGE("view lines: ", total.exact, " values equal the mirror, ", total.allowance,
            " within the non-dyadic allowance, ", total.failures, " differ");
}
