#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "dvv/media/view.h"
#include "dvv/media/view_lines.h"
#include "dvv/platform/pixel_size.h"

using dvv::media::AxisView;
using dvv::media::axisX;
using dvv::media::axisY;
using dvv::media::boundaryAt;
using dvv::media::cursorPixel;
using dvv::media::cursorSpan;
using dvv::media::firstPixelOf;
using dvv::media::haloSpan;
using dvv::media::imageCoordinateAt;
using dvv::media::ImagePixel;
using dvv::media::imagePixelAt;
using dvv::media::imageSpan;
using dvv::media::levelVisible;
using dvv::media::LINE_LEVELS;
using dvv::media::lineAt;
using dvv::media::LineHit;
using dvv::media::LineLevel;
using dvv::media::linesInView;
using dvv::media::lineSpan;
using dvv::media::nearestTick;
using dvv::media::numberStepLog2;
using dvv::media::pictureRegion;
using dvv::media::pixelCentreAt;
using dvv::media::PixelRange;
using dvv::media::PixelRect;
using dvv::media::PixelSpan;
using dvv::media::PointD;
using dvv::media::RulerRects;
using dvv::media::rulerRects;
using dvv::media::tickLevel;
using dvv::media::ViewMode;
using dvv::media::ViewStrips;
using dvv::media::ViewTransform;
using dvv::media::visiblePicture;
using dvv::media::visiblePixels;

namespace {

using Levels = std::array<LineLevel, LINE_LEVELS>;

bool holds(PixelSpan span, std::int32_t pixel) {
    return span.begin <= pixel && pixel < span.end;
}

bool inside(PixelSpan span, PixelSpan outer) {
    return !span.empty() && outer.begin <= span.begin && span.end <= outer.end;
}

// lineAt by brute force: every multiple of the step in [0, size], not four candidates.
std::optional<LineHit> lineAtByScan(const AxisView& axis, const Levels& levels,
                                    std::int32_t pixel) {
    for (std::size_t l = 0; l < LINE_LEVELS; ++l) {
        const LineLevel& level = levels.at(l);
        if (!level.drawn) {
            continue;
        }
        const std::uint32_t step = 1U << level.stepLog2;
        for (std::uint32_t k = 0; k <= axis.size; k += step) {
            if (holds(lineSpan(axis, k, level.thickness), pixel)) {
                return LineHit{.level = l, .k = k, .halo = false};
            }
        }
        for (std::uint32_t k = 0; level.halo && k <= axis.size; k += step) {
            if (holds(haloSpan(axis, k, level.thickness), pixel)) {
                return LineHit{.level = l, .k = k, .halo = true};
            }
        }
    }
    return std::nullopt;
}

// The smallest p with imageCoordinateAt(p) >= k, by bisection over u alone (u does not decrease).
std::int32_t firstPixelByBisection(const AxisView& axis, std::uint32_t k) {
    std::int32_t low = -0x400000;  // -2^22
    std::int32_t high = 0x400000;  // 2^22
    while (low < high) {
        const std::int32_t mid = low + (high - low) / 2;
        if (imageCoordinateAt(axis, mid) >= static_cast<float>(k)) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

// The image pixel that viewport pixel p shows, or -1 for none (outside the image).
std::int64_t shownPixel(const AxisView& axis, std::int32_t p) {
    const float u = imageCoordinateAt(axis, p);
    if (!(u >= 0 && u < static_cast<float>(axis.size))) {
        return -1;
    }
    return static_cast<std::int64_t>(std::floor(u));
}

// The number of places where the spans break the pixel-centre rule, checked pixel by pixel
// against imageCoordinateAt alone.
std::size_t pixelCentreMismatches(const AxisView& a) {
    std::size_t bad = 0;
    const auto extent = static_cast<std::int32_t>(a.extent);
    for (std::int32_t p = -3; p < extent + 3; ++p) {
        const bool shown = p >= 0 && p < extent && shownPixel(a, p) >= 0;
        bad += holds(imageSpan(a), p) != shown ? 1 : 0;
        if (shown && !holds(cursorSpan(a, static_cast<std::uint32_t>(shownPixel(a, p))), p)) {
            ++bad;
        }
    }
    for (std::uint32_t k = 0; k <= a.size; ++k) {
        const std::int32_t first = firstPixelByBisection(a, k);
        bad += firstPixelOf(a, k) != first ? 1 : 0;
        if (k > 0 && k < a.size) {
            const PixelSpan image = imageSpan(a);
            const bool visible = image.begin <= first && first < image.end;
            const PixelSpan expected = visible ? PixelSpan{first, first + 1} : PixelSpan{};
            bad += lineSpan(a, k, 1) != expected ? 1 : 0;
        }
    }
    for (std::uint32_t c = 0; c < a.size; ++c) {
        // A marker covers only pixels that show c, or is the one pixel of a c shown nowhere.
        const PixelSpan marker = cursorSpan(a, c);
        for (std::int32_t p = marker.begin; p < marker.end; ++p) {
            if (shownPixel(a, p) != static_cast<std::int64_t>(c) &&
                marker.end - marker.begin != 1) {
                ++bad;
            }
        }
    }
    return bad;
}

}  // namespace

TEST_CASE("view lines: a boundary on a pixel edge is the first pixel of image pixel k") {
    const AxisView a{.offset = 10, .zoom = 4, .size = 8, .extent = 100};
    CHECK(lineSpan(a, 3, 1) == PixelSpan{22, 23});
    CHECK(lineSpan(a, 3, 2) == PixelSpan{21, 23});
    CHECK(lineSpan(a, 3, 3) == PixelSpan{21, 24});
    CHECK(lineSpan(a, 3, 4) == PixelSpan{20, 24});
}

TEST_CASE("view lines: outer edges are drawn inside the image") {
    const AxisView a{.offset = 10, .zoom = 4, .size = 8, .extent = 100};
    CHECK(imageSpan(a) == PixelSpan{10, 42});
    CHECK(lineSpan(a, 0, 2) == PixelSpan{10, 12});
    CHECK(lineSpan(a, 8, 2) == PixelSpan{40, 42});
    CHECK(lineSpan(a, 9, 1).empty());
    CHECK(haloSpan(a, 0, 2) == PixelSpan{10, 13});  // clipped at the image's start
    CHECK(haloSpan(a, 4, 1) == PixelSpan{25, 28});
    CHECK(haloSpan(a, 8, 2) == PixelSpan{39, 42});  // clipped at the image's end
    CHECK(haloSpan(a, 9, 1).empty());
    CHECK(lineSpan(a, 3, 0).empty());
    CHECK(haloSpan(a, 3, 0).empty());
}

TEST_CASE("view lines: clipped to the viewport (a panned view)") {
    const AxisView a{.offset = -6, .zoom = 4, .size = 8, .extent = 20};
    CHECK(imageSpan(a) == PixelSpan{0, 20});
    CHECK(lineSpan(a, 1, 4).empty());             // b = -2: [-4, 0) is outside the viewport
    CHECK(lineSpan(a, 2, 4) == PixelSpan{0, 4});  // b = 2: [0, 4)
    CHECK(lineSpan(a, 7, 1).empty());             // b = 22: beyond the extent
}

TEST_CASE("view lines: no image shows no span") {
    CHECK(imageSpan({.offset = 0, .zoom = 0, .size = 8, .extent = 100}).empty());
    CHECK(imageSpan({.offset = 0, .zoom = -1, .size = 8, .extent = 100}).empty());
    CHECK(imageSpan({.offset = 0, .zoom = 1, .size = 0, .extent = 100}).empty());
    CHECK(imageSpan({.offset = 0, .zoom = 1, .size = 8, .extent = 0}).empty());
    CHECK(lineSpan({.offset = 0, .zoom = 0, .size = 8, .extent = 100}, 0, 1).empty());
    CHECK(cursorSpan({.offset = 0, .zoom = 0, .size = 8, .extent = 100}, 0).empty());
    CHECK(linesInView({.offset = 0, .zoom = 0, .size = 8, .extent = 100}, 1).empty());
    const Levels levels{{{.stepLog2 = 0, .drawn = true}, {}, {}}};
    CHECK(lineAt({.offset = 0, .zoom = 0, .size = 8, .extent = 100}, levels, 0) == std::nullopt);
}

TEST_CASE("view lines: cursor span is at least one pixel") {
    CHECK(cursorSpan({.offset = 0, .zoom = 0.125F, .size = 64, .extent = 8}, 9) == PixelSpan{1, 2});
    CHECK(cursorSpan({.offset = 0, .zoom = 32, .size = 1, .extent = 64}, 0) == PixelSpan{0, 32});
    CHECK(cursorSpan({.offset = 0, .zoom = 1, .size = 4, .extent = 4}, 4).empty());
}

TEST_CASE("view lines: an image pixel narrower than a viewport pixel at the image's end") {
    // b(37) = 19.501: viewport pixel 19 has u = 17.5 / 0.473 = 36.997, so it shows image pixel 36
    // and the image's span ends after it. Image pixel 35 is shown on no pixel; its marker is the
    // one pixel where image pixel 36 starts.
    const AxisView a{.offset = 2, .zoom = 0.473F, .size = 37, .extent = 40};
    CHECK(imageSpan(a) == PixelSpan{2, 20});
    CHECK(lineSpan(a, 37, 1) == PixelSpan{19, 20});
    CHECK(cursorSpan(a, 36) == PixelSpan{19, 20});
    CHECK(cursorSpan(a, 35) == PixelSpan{19, 20});
    CHECK(cursorSpan(a, 0) == PixelSpan{2, 3});
}

TEST_CASE("view lines: boundaryAt is the two statements, not a fused multiply-add") {
    const AxisView a{.offset = -1000, .zoom = 0.473F, .size = 3840, .extent = 1816};
    std::size_t fused_differs = 0;
    for (std::uint32_t k = 0; k <= a.size; ++k) {
        const float product = static_cast<float>(k) * a.zoom;
        const float b = static_cast<float>(a.offset) + product;
        CHECK(boundaryAt(a, k) == b);
        if (std::fma(static_cast<float>(k), a.zoom, static_cast<float>(a.offset)) != b) {
            ++fused_differs;
        }
    }
    CHECK(fused_differs > 0);  // the inputs can tell a fused result from the two statements
}

TEST_CASE("view lines: imageCoordinateAt is the pixel centre") {
    const AxisView a{.offset = 10, .zoom = 4, .size = 8, .extent = 100};
    CHECK(imageCoordinateAt(a, 10) == 0.125F);
    CHECK(imageCoordinateAt(a, 9) == -0.125F);
    CHECK(imageCoordinateAt(a, 41) == 7.875F);
    const AxisView b{.offset = 0, .zoom = 1.25F, .size = 400, .extent = 300};
    CHECK(imageCoordinateAt(b, 3) == 3.5F / 1.25F);
}

TEST_CASE("view lines: lines and markers sit where the picture changes at a non-dyadic zoom") {
    // u(3) = 2.8 and u(4) = 3.6: image pixel 2 is shown on [2, 4) and the line of k = 3 is the
    // first pixel of image pixel 3, not floor(b(3)) = floor(3.75) = 3.
    const AxisView a{.offset = 0, .zoom = 1.25F, .size = 400, .extent = 300};
    CHECK(firstPixelOf(a, 3) == 4);
    CHECK(lineSpan(a, 3, 1) == PixelSpan{4, 5});
    CHECK(cursorSpan(a, 2) == PixelSpan{2, 4});
    // b(10) = 13.7: viewport pixel 13 (u = 9.85) shows image pixel 9, so the image ends at 14.
    const AxisView c{.offset = 0, .zoom = 1.37F, .size = 10, .extent = 40};
    CHECK(imageSpan(c) == PixelSpan{0, 14});
    CHECK(lineSpan(c, 10, 1) == PixelSpan{13, 14});
    CHECK(cursorSpan(c, 9) == PixelSpan{12, 14});
}

TEST_CASE("view lines: every span follows the pixel-centre rule") {
    const std::array<AxisView, 9> axes{{
        {.offset = 0, .zoom = 1.25F, .size = 400, .extent = 300},
        {.offset = 0, .zoom = 1.37F, .size = 400, .extent = 300},
        {.offset = 2, .zoom = 0.473F, .size = 37, .extent = 40},
        {.offset = -100, .zoom = 1.37F, .size = 400, .extent = 300},      // the image ends beyond
        {.offset = 40, .zoom = 0.473F, .size = 900, .extent = 300},       // the image ends beyond
        {.offset = 0, .zoom = 0x1.aaaaacp-1F, .size = 60, .extent = 80},  // the first-pixel
        {.offset = 3, .zoom = 0x1.638e3ap+0F, .size = 60, .extent = 90},  // estimate is corrected
        {.offset = 7, .zoom = 4, .size = 30, .extent = 200},
        {.offset = 0, .zoom = 0.125F, .size = 400, .extent = 60},
    }};
    for (const AxisView& a : axes) {
        CAPTURE(a.zoom);
        CAPTURE(a.offset);
        CHECK(pixelCentreMismatches(a) == 0);
    }
    const ViewTransform t = ViewTransform::fit({1816, 1022}, {3840, 2160});
    CHECK(pixelCentreMismatches(axisX(t)) == 0);
    CHECK(pixelCentreMismatches(axisY(t)) == 0);
}

TEST_CASE("view lines: the estimate of the first pixel is corrected in both directions") {
    // Found by a search: here ceil(b - 0.5) is one pixel too small (k = 3) or too large (k = 9).
    const AxisView a{.offset = 0, .zoom = 0x1.aaaaacp-1F, .size = 60, .extent = 80};
    CHECK(firstPixelOf(a, 3) == firstPixelByBisection(a, 3));
    CHECK(firstPixelOf(a, 9) == firstPixelByBisection(a, 9));
    CHECK(firstPixelOf(a, 3) != static_cast<std::int32_t>(std::ceil(boundaryAt(a, 3) - 0.5F)));
    CHECK(firstPixelOf(a, 9) != static_cast<std::int32_t>(std::ceil(boundaryAt(a, 9) - 0.5F)));
}

TEST_CASE("view lines: a panned image keeps its outer edges off the viewport's edge") {
    // The image continues past both viewport edges: no edge line and no marker is pulled in.
    const AxisView a{.offset = -100, .zoom = 1.37F, .size = 400, .extent = 300};
    CHECK(imageSpan(a) == PixelSpan{0, 300});
    CHECK(lineSpan(a, 400, 2).empty());
    CHECK(lineSpan(a, 0, 2).empty());
    const auto right = static_cast<std::uint32_t>(std::floor(imageCoordinateAt(a, 299)));
    CHECK(cursorSpan(a, right).end == 300);
    CHECK(cursorSpan(a, right + 1).empty());
    CHECK(cursorSpan(a, a.size - 1).empty());
}

TEST_CASE("view lines: imagePixelAt agrees with the cursor marker") {
    const std::array<ViewTransform, 3> views{
        ViewTransform::fit({1816, 1022}, {3840, 2160}),
        ViewTransform::zoomed({300, 200}, {400, 300}, 1.37, {.x = 150, .y = 100}),
        ViewTransform::zoomed({300, 200}, {400, 300}, 1.25, {.x = 120, .y = 80})};
    for (const ViewTransform& t : views) {
        const AxisView a = axisX(t);
        std::size_t bad = 0;
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(a.extent); ++x) {
            for (const double fraction : {0.1, 0.5, 0.9}) {
                const std::optional<dvv::media::ImagePixel> pixel =
                    imagePixelAt(t, PointD{.x = x + fraction, .y = 100.5});
                const bool agrees =
                    pixel ? holds(cursorSpan(a, pixel->x), x) : !holds(imageSpan(a), x);
                bad += agrees ? 0 : 1;
            }
        }
        CHECK(bad == 0);
    }
}

TEST_CASE("view lines: level visibility and tick levels") {
    CHECK(levelVisible(5, 1.0F, 24.0F));
    CHECK_FALSE(levelVisible(1, 1.0F, 6.0F));  // a spacing of 2 px, the minimum 6 px
    CHECK(levelVisible(2, 1.5F, 6.0F));        // a spacing equal to the minimum is visible
    CHECK(levelVisible(32, 0x1p-32F, 1.0F));   // 2^32 x 2^-32: no shift of 32
    CHECK(levelVisible(300, 1.0F, 6.0F));
    CHECK_FALSE(levelVisible(3, 0.0F, 6.0F));
    const Levels levels{{{.stepLog2 = 5, .drawn = true},
                         {.stepLog2 = 3, .drawn = true},
                         {.stepLog2 = 1, .drawn = false}}};
    CHECK(tickLevel(64, levels) == 0U);
    CHECK(tickLevel(40, levels) == 1U);
    CHECK(tickLevel(2, levels) == std::nullopt);
    CHECK(tickLevel(0, levels) == 0U);
    const Levels huge{{{.stepLog2 = 40, .drawn = true}, {}, {}}};
    CHECK(tickLevel(0, huge) == 0U);
    CHECK(tickLevel(0x80000000U, huge) == std::nullopt);
    const Levels top{{{.stepLog2 = 31, .drawn = true}, {.stepLog2 = 32, .drawn = true}, {}}};
    CHECK(tickLevel(0x80000000U, top) == 0U);
    CHECK(tickLevel(0x40000000U, top) == std::nullopt);
}

TEST_CASE("view lines: lineAt prefers the coarsest level and finds halos") {
    const AxisView a{.offset = 0, .zoom = 1, .size = 256, .extent = 256};
    const Levels levels{{{.stepLog2 = 5, .thickness = 2, .drawn = true, .halo = true},
                         {.stepLog2 = 3, .thickness = 1, .drawn = true},
                         {.stepLog2 = 1, .thickness = 1, .drawn = false}}};
    CHECK(lineAt(a, levels, 31) == LineHit{.level = 0, .k = 32, .halo = false});
    CHECK(lineAt(a, levels, 32) == LineHit{.level = 0, .k = 32, .halo = false});
    CHECK(lineAt(a, levels, 33) == LineHit{.level = 0, .k = 32, .halo = true});
    CHECK(lineAt(a, levels, 30) == LineHit{.level = 0, .k = 32, .halo = true});
    CHECK(lineAt(a, levels, 40) == LineHit{.level = 1, .k = 40, .halo = false});
    CHECK(lineAt(a, levels, 41) == std::nullopt);
    CHECK(lineAt(a, levels, -1) == std::nullopt);
    CHECK(lineAt(a, levels, 256) == std::nullopt);
}

TEST_CASE("view lines: lineAt agrees with a scan of every line at a non-dyadic zoom") {
    const ViewTransform t = ViewTransform::fit({1816, 1022}, {3840, 2160});
    const Levels levels{{{.stepLog2 = 5, .thickness = 2, .drawn = true, .halo = true},
                         {.stepLog2 = 3, .thickness = 1, .drawn = true},
                         {.stepLog2 = 1, .thickness = 1, .drawn = false}}};
    for (const AxisView& a : {axisX(t), axisY(t)}) {
        std::size_t mismatches = 0;
        for (std::int32_t p = -2; p < static_cast<std::int32_t>(a.extent) + 2; ++p) {
            if (lineAt(a, levels, p) != lineAtByScan(a, levels, p)) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("view lines: lineAt's four candidates hold dense lines") {
    // Spacings of 3.4 to 7.8 px under lines 8 px thick with halos, below the precondition
    // spacing >= thickness: the level, the halo flag and hit-or-miss match the scan; k may be a
    // neighbouring line's (view_lines.h), so it is not compared. The window's k at these spacings
    // is pinned by the GPU test (gpu_test_view_lines.cpp), which compares lineAt with the shader.
    const Levels levels{{{.stepLog2 = 1, .thickness = 8, .drawn = true, .halo = true}, {}, {}}};
    for (const float zoom : {1.7F, 1.8F, 1.9F, 2.3F, 3.1F, 3.9F}) {
        const AxisView a{.offset = 3, .zoom = zoom, .size = 200, .extent = 800};
        std::size_t mismatches = 0;
        for (std::int32_t p = -2; p < static_cast<std::int32_t>(a.extent) + 2; ++p) {
            const std::optional<LineHit> window = lineAt(a, levels, p);
            const std::optional<LineHit> scan = lineAtByScan(a, levels, p);
            const bool same =
                window.has_value() == scan.has_value() &&
                (!window || (window->level == scan->level && window->halo == scan->halo));
            if (!same) {
                ++mismatches;
            }
        }
        CAPTURE(zoom);
        CHECK(mismatches == 0);
    }
}

TEST_CASE("view lines: a level of step 2^32 or more has the single line k = 0") {
    const AxisView a{.offset = 10, .zoom = 4, .size = 100, .extent = 500};
    const Levels levels{{{.stepLog2 = 40, .thickness = 2, .drawn = true, .halo = true}, {}, {}}};
    CHECK(lineAt(a, levels, 10) == LineHit{.level = 0, .k = 0, .halo = false});
    CHECK(lineAt(a, levels, 12) == LineHit{.level = 0, .k = 0, .halo = true});
    CHECK(lineAt(a, levels, 409) == std::nullopt);
    CHECK(linesInView(a, 40) == std::vector<std::uint32_t>{0});
    // At a size of 2^31 a shift by 31 would leave a second multiple, k = size: there is none.
    const AxisView wide{.offset = 10, .zoom = 0x1p-24F, .size = 0x80000000U, .extent = 500};
    CHECK(imageSpan(wide) == PixelSpan{10, 138});
    CHECK(lineAt(wide, levels, 10) == LineHit{.level = 0, .k = 0, .halo = false});
    CHECK(lineAt(wide, levels, 137) == std::nullopt);
}

TEST_CASE("view lines: linesInView") {
    const AxisView a{.offset = -100, .zoom = 2, .size = 400, .extent = 300};
    const std::vector<std::uint32_t> ks = linesInView(a, 5);
    CHECK(ks.front() == 64);  // b = 28; k = 32 is at -36
    CHECK(ks.back() == 192);  // b = 284; k = 224 is at 348
    CHECK(ks.size() == 5);
}

TEST_CASE("view lines: linesInView is the multiples with a non-empty one-pixel line") {
    const ViewTransform t = ViewTransform::zoomed({1816, 1022}, {3840, 2160}, 1.37, {1000, 700});
    for (const AxisView& a : {axisX(t), axisY(t)}) {
        for (std::uint32_t s = 0; s <= 8; ++s) {
            std::vector<std::uint32_t> scan;
            for (std::uint32_t k = 0; k <= a.size; k += 1U << s) {
                if (!lineSpan(a, k, 1).empty()) {
                    scan.push_back(k);
                }
            }
            CHECK(linesInView(a, s) == scan);
        }
    }
    CHECK(linesInView({.offset = 0, .zoom = 1, .size = 100, .extent = 200}, 40) ==
          std::vector<std::uint32_t>{0});
}

TEST_CASE("view lines: a thickness above 2^29 acts as 2^29") {
    constexpr std::uint32_t LIMIT = 0x20000000U;
    const AxisView a{.offset = 0, .zoom = 1, .size = 0x40000000U, .extent = 0x40000000U};
    CHECK(lineSpan(a, 0, LIMIT) == PixelSpan{0, 0x20000000});
    const std::int32_t middle = firstPixelOf(a, 0x20000000U);
    CHECK(lineSpan(a, 0x20000000U, LIMIT) == PixelSpan{middle - 0x10000000, middle + 0x10000000});
    const std::int32_t end = imageSpan(a).end;
    CHECK(lineSpan(a, a.size, LIMIT) == PixelSpan{end - 0x20000000, end});
    for (const std::uint32_t thickness : {LIMIT + 1, 0x40000000U, 0x80000000U, 0xFFFFFFFFU}) {
        CAPTURE(thickness);
        for (const std::uint32_t k : {0U, 0x20000000U, a.size}) {
            CAPTURE(k);
            CHECK(lineSpan(a, k, thickness) == lineSpan(a, k, LIMIT));
            CHECK(haloSpan(a, k, thickness) == haloSpan(a, k, LIMIT));
        }
        const AxisView small{.offset = 0, .zoom = 1, .size = 8, .extent = 16};
        const Levels thick{{{.thickness = thickness, .drawn = true, .halo = true}, {}, {}}};
        const Levels clamped{{{.thickness = LIMIT, .drawn = true, .halo = true}, {}, {}}};
        for (std::int32_t pixel = 0; pixel < 16; ++pixel) {
            CAPTURE(pixel);
            CHECK(lineAt(small, thick, pixel) == lineAt(small, clamped, pixel));
        }
    }
}

TEST_CASE("view lines: degenerate images") {
    CHECK(imageSpan({.offset = 0, .zoom = 32, .size = 1, .extent = 1000}) == PixelSpan{0, 32});
    CHECK(lineSpan({.offset = 0, .zoom = 32, .size = 1, .extent = 1000}, 1, 2) ==
          PixelSpan{30, 32});
    const AxisView thin{.offset = 0, .zoom = 0.015625F, .size = 16384, .extent = 256};
    CHECK_FALSE(levelVisible(8, thin.zoom, 6.0F));
    CHECK(linesInView(thin, 8).back() <= thin.size);
    CHECK(lineSpan({.offset = 2, .zoom = 0.473F, .size = 37, .extent = 40}, 37, 1).end ==
          imageSpan({.offset = 2, .zoom = 0.473F, .size = 37, .extent = 40}).end);
}

TEST_CASE("view lines: a 1x1 image at a zoom of 32") {
    const ViewTransform t = ViewTransform::zoomed({1000, 600}, {1, 1}, 32, {.x = 0.5, .y = 0.5});
    const AxisView x = axisX(t);
    CHECK(x == AxisView{.offset = 484, .zoom = 32, .size = 1, .extent = 1000});
    CHECK(imageSpan(x) == PixelSpan{484, 516});
    CHECK(lineSpan(x, 0, 2) == PixelSpan{484, 486});
    CHECK(lineSpan(x, 1, 2) == PixelSpan{514, 516});
    CHECK(lineSpan(x, 2, 2).empty());
    CHECK(cursorSpan(x, 0) == PixelSpan{484, 516});
    CHECK(cursorSpan(x, 1).empty());
    CHECK(linesInView(x, 0) == std::vector<std::uint32_t>{0, 1});
    CHECK(linesInView(x, 1) == std::vector<std::uint32_t>{0});
    const Levels levels{{{.stepLog2 = 5, .thickness = 2, .drawn = true, .halo = true},
                         {.stepLog2 = 3, .thickness = 1, .drawn = true},
                         {.stepLog2 = 1, .thickness = 1, .drawn = true}}};
    CHECK(lineAt(x, levels, 484) == LineHit{.level = 0, .k = 0, .halo = false});
    CHECK(lineAt(x, levels, 486) == LineHit{.level = 0, .k = 0, .halo = true});
    CHECK(lineAt(x, levels, 515) == std::nullopt);  // k = 1 is no multiple of a step
    CHECK(tickLevel(1, levels) == std::nullopt);
    CHECK(axisY(t) == AxisView{.offset = 284, .zoom = 32, .size = 1, .extent = 600});
}

TEST_CASE("view lines: a 16384x1 image at a zoom of 1/64") {
    const ViewTransform t = ViewTransform::fit({256, 256}, {16384, 1});
    const AxisView x = axisX(t);
    const AxisView y = axisY(t);
    CHECK(x == AxisView{.offset = 0, .zoom = 0.015625F, .size = 16384, .extent = 256});
    CHECK(y == AxisView{.offset = 127, .zoom = 0.015625F, .size = 1, .extent = 256});
    CHECK_FALSE(levelVisible(8, x.zoom, 6.0F));
    CHECK(levelVisible(8, x.zoom, 4.0F));
    CHECK(imageSpan(x) == PixelSpan{0, 256});
    const std::vector<std::uint32_t> ks = linesInView(x, 8);
    CHECK(ks.size() == 65);
    CHECK(ks.back() == 16384);
    CHECK(lineSpan(x, 16384, 1) == PixelSpan{255, 256});
    std::size_t bad_cursor = 0;
    for (std::uint32_t c = 0; c < x.size; ++c) {
        if (!inside(cursorSpan(x, c), imageSpan(x)) ||
            cursorSpan(x, c).end - cursorSpan(x, c).begin != 1) {
            ++bad_cursor;
        }
    }
    CHECK(bad_cursor == 0);
    CHECK(cursorSpan(x, 16383) == PixelSpan{255, 256});
    // A sixty-fourth of a pixel high: no viewport pixel shows the image, so nothing is drawn.
    CHECK(imageSpan(y).empty());
    CHECK(cursorSpan(y, 0).empty());
    CHECK(lineSpan(y, 0, 1).empty());
    CHECK(lineSpan(y, 1, 1).empty());
    CHECK(linesInView(y, 0).empty());
}

TEST_CASE("view lines: an image whose size is no multiple of any step") {
    const AxisView a{.offset = 5, .zoom = 2, .size = 999, .extent = 2100};
    CHECK(imageSpan(a) == PixelSpan{5, 2003});
    CHECK(lineSpan(a, 999, 1) == PixelSpan{2002, 2003});
    CHECK(linesInView(a, 1).back() == 998);
    CHECK(linesInView(a, 5).back() == 992);
    const Levels levels{{{.stepLog2 = 5, .thickness = 1, .drawn = true},
                         {.stepLog2 = 3, .thickness = 1, .drawn = true},
                         {.stepLog2 = 1, .thickness = 1, .drawn = true, .halo = true}}};
    CHECK(tickLevel(999, levels) == std::nullopt);
    CHECK(lineAt(a, levels, 2001) == LineHit{.level = 2, .k = 998, .halo = false});
    CHECK(lineAt(a, levels, 2002) == LineHit{.level = 2, .k = 998, .halo = true});
    for (std::int32_t p = 0; p < static_cast<std::int32_t>(a.extent); ++p) {
        const std::optional<LineHit> hit = lineAt(a, levels, p);
        if (hit) {
            REQUIRE(hit->k <= a.size);
        }
    }
}

TEST_CASE("view lines: k = size on a non-dyadic Fit") {
    const ViewTransform t = ViewTransform::fit({1816, 1022}, {3840, 2160});
    for (const AxisView& a : {axisX(t), axisY(t)}) {
        const PixelSpan image = imageSpan(a);
        CHECK(image.end <= static_cast<std::int32_t>(a.extent));
        CHECK(image.end >= static_cast<std::int32_t>(a.extent) - 1);  // one pixel, non-dyadic
        CHECK(lineSpan(a, a.size, 2) == PixelSpan{image.end - 2, image.end});
        CHECK(lineSpan(a, 0, 2) == PixelSpan{image.begin, image.begin + 2});
        std::size_t empty_cursor = 0;
        for (std::uint32_t c = 0; c < a.size; ++c) {
            if (!inside(cursorSpan(a, c), image)) {
                ++empty_cursor;
            }
        }
        CHECK(empty_cursor == 0);
        for (const std::uint32_t k : linesInView(a, 1)) {
            REQUIRE(k <= a.size);
        }
    }
}

TEST_CASE("view lines: pictureRegion") {
    CHECK(pictureRegion({200, 100}, {.top = 30, .left = 30}) == PixelRect{30, 30, 170, 70});
    CHECK(pictureRegion({8, 8}, {.top = 30, .left = 30}) == PixelRect{8, 8, 0, 0});
    CHECK(pictureRegion({200, 100}, {}) == PixelRect{0, 0, 200, 100});
    CHECK(pictureRegion({0, 40}, {.top = 30, .left = 30}) == PixelRect{0, 30, 0, 10});
    CHECK(pictureRegion({0, 0}, {}) == PixelRect{0, 0, 0, 0});
    CHECK(pictureRegion({200, 20}, {.top = 30, .left = 10}) == PixelRect{10, 20, 190, 0});
    CHECK(pictureRegion({200, 100}, {.top = 30, .left = 30}).empty() == false);
    CHECK(pictureRegion({8, 8}, {.top = 30, .left = 30}).empty());
}

TEST_CASE("view lines: PixelRect contains is half-open") {
    static_assert(PixelRect{30, 20, 100, 50}.contains({30, 20}));  // usable in constant expressions
    const PixelRect rect{30, 20, 100, 50};
    CHECK(rect.contains({30, 20}));         // the near edges are inside
    CHECK(rect.contains({129.9, 69.9}));    // just short of the far edges
    CHECK_FALSE(rect.contains({130, 40}));  // the far x edge is outside
    CHECK_FALSE(rect.contains({60, 70}));   // the far y edge is outside
    CHECK_FALSE(rect.contains({29.9, 40}));
    CHECK_FALSE(rect.contains({60, 19.9}));
    CHECK_FALSE(rect.contains({std::nan(""), 40}));
    CHECK_FALSE(PixelRect{30, 20, 0, 50}.contains({30, 20}));  // empty
    const PixelRect far{0xFFFFFFF0U, 0, 0xFFFFFFF0U, 1};       // the far edge past 2^32
    CHECK(far.contains({8589934000.0, 0.5}));
}

TEST_CASE("view lines: cursorPixel only inside the picture region") {
    // 400 % on a 64 x 48 image in the region 100 x 100 at (30, 30): the image starts at x = 19.5
    // at the region's left edge, so a point over the left strip still maps to an image pixel by
    // imagePixelAt alone.
    const PixelRect picture{30, 30, 100, 100};
    const ViewTransform view = dvv::media::resolveView(
        {dvv::media::ViewMode::Zoom, 4, {32, 24}, {64, 48}}, picture.size(), {64, 48});
    REQUIRE(imagePixelAt(view, {-20, 50}).has_value());
    CHECK_FALSE(cursorPixel(view, picture, {10, 80}).has_value());    // over the left strip
    CHECK_FALSE(cursorPixel(view, picture, {29.9, 80}).has_value());  // just left of the region
    CHECK_FALSE(cursorPixel(view, picture, {80, 29.9}).has_value());  // over the top strip
    CHECK_FALSE(cursorPixel(view, picture, {130, 80}).has_value());   // past the right edge
    CHECK_FALSE(cursorPixel(view, picture, {80, 130}).has_value());   // past the bottom edge
    CHECK_FALSE(cursorPixel(view, PixelRect{30, 30, 0, 0}, {30, 30}).has_value());  // empty
    // Inside: imagePixelAt of the region-local point, edges included.
    for (const PointD point : {PointD{30, 30}, PointD{80.5, 60.5}, PointD{129.9, 129.9}}) {
        const std::optional<ImagePixel> pixel = cursorPixel(view, picture, point);
        CHECK(pixel == imagePixelAt(view, {point.x - 30, point.y - 30}));
        CHECK(pixel.has_value());
    }
    // Fit at 100 / 64 = 1.5625, x offset 0: the region's first column shows image column 0, its
    // last, pixel 99 with centre u = 99.5 / 1.5625 = 63.68, the last image column 63.
    const ViewTransform fit = dvv::media::resolveView({}, picture.size(), {64, 48});
    const std::optional<ImagePixel> first = cursorPixel(fit, picture, {30, 80});
    const std::optional<ImagePixel> last = cursorPixel(fit, picture, {129.9, 80});
    REQUIRE(first.has_value());
    REQUIRE(last.has_value());
    CHECK(first.value_or(ImagePixel{}).x == 0);  // REQUIRE above stops the test case first
    CHECK(last.value_or(ImagePixel{}).x == 63);
}

TEST_CASE("view lines: axisX and axisY") {
    const ViewTransform t = ViewTransform::zoomed({200, 100}, {400, 300}, 2, {.x = 400, .y = 300});
    CHECK(axisX(t) == AxisView{.offset = -600, .zoom = 2.0F, .size = 400, .extent = 200});
    CHECK(axisY(t) == AxisView{.offset = -500, .zoom = 2.0F, .size = 300, .extent = 100});
    CHECK(axisX(ViewTransform{}) == AxisView{});
}

TEST_CASE("view lines: a picture region's size resolves a view") {
    const PixelRect region = pictureRegion({200, 100}, {.top = 30, .left = 20});
    CHECK(region.size() == dvv::platform::PixelSize{180, 70});
    CHECK(axisX(ViewTransform::fit(region.size(), {360, 140})).extent == 180);
}

namespace {

// The output of the hugging cases: 200 x 100 with a top strip of 10 and a left strip of 20, so the
// picture region is {20, 10, 180, 90}.
constexpr dvv::platform::PixelSize HUG_EXTENT{200, 100};
constexpr ViewStrips HUG_STRIPS{.top = 10, .left = 20};
constexpr dvv::platform::PixelSize HUG_PICTURE{180, 90};

// A Zoom request for `image` at `zoom` with `centre` (the image's centre when none), resolved
// against `viewport`.
ViewTransform zoomedView(dvv::platform::PixelSize image, double zoom,
                         std::optional<PointD> centre = std::nullopt,
                         dvv::platform::PixelSize viewport = HUG_PICTURE) {
    const PointD at = centre.value_or(PointD{.x = image.width / 2.0, .y = image.height / 2.0});
    return dvv::media::resolveView({ViewMode::Zoom, zoom, at, image}, viewport, image);
}

bool insideOutput(PixelRect rect, dvv::platform::PixelSize extent) {
    return std::uint64_t{rect.x} + rect.width <= extent.width &&
           std::uint64_t{rect.y} + rect.height <= extent.height;
}

// The multiple of `step` in [0, size] nearest to c + 0.5, the lower one on a tie, by a scan.
std::uint32_t nearestMultipleByScan(std::uint32_t c, std::uint32_t size, std::uint32_t step) {
    const double centre = c + 0.5;
    std::uint32_t best = 0;
    for (std::uint32_t k = 0; k <= size; k += step) {
        if (std::abs(static_cast<double>(k) - centre) <
            std::abs(static_cast<double>(best) - centre)) {
            best = k;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("view lines: visiblePicture") {
    const PixelRect picture = pictureRegion(HUG_EXTENT, HUG_STRIPS);
    REQUIRE(picture == PixelRect{20, 10, 180, 90});
    CHECK(visiblePicture(zoomedView({100, 50}, 1), picture) == PixelRect{60, 30, 100, 50});
    CHECK(visiblePicture(zoomedView({100, 50}, 4), picture) == picture);
    CHECK(visiblePicture(zoomedView({100, 200}, 1), picture) == PixelRect{60, 10, 100, 90});
    // No image: zoom 0, an empty image, an empty region.
    CHECK(visiblePicture(ViewTransform{}, picture).empty());
    CHECK(visiblePicture(zoomedView({0, 50}, 1), picture).empty());
    const PixelRect none = pictureRegion({8, 8}, {.top = 30, .left = 30});
    CHECK(visiblePicture(zoomedView({100, 50}, 1, std::nullopt, none.size()), none).empty());
    CHECK(visiblePicture(zoomedView({100, 50}, 1), none).empty());  // a view for another size
}

TEST_CASE("view lines: visiblePicture and rulerRects of a panned picture") {
    // 200 x 80 shown: wider than the region, shorter. Offset x = round(90 - 105) = -15 inside the
    // clamp [-20, 0], offset y = floor((90 - 80) / 2) = 5.
    const ViewTransform view = zoomedView({100, 40}, 2, PointD{.x = 52.5, .y = 20});
    REQUIRE(view.offset().x == -15);
    REQUIRE(view.offset().y == 5);
    const PixelRect hugged{20, 15, 180, 80};
    CHECK(visiblePicture(view, pictureRegion(HUG_EXTENT, HUG_STRIPS)) == hugged);
    const RulerRects rects = rulerRects(HUG_EXTENT, HUG_STRIPS, view);
    CHECK(rects.hugged == hugged);
    CHECK(rects.top == PixelRect{20, 5, 180, 10});
    CHECK(rects.left == PixelRect{0, 15, 20, 80});
    CHECK(rects.corner == PixelRect{0, 5, 20, 10});
}

TEST_CASE("view lines: visiblePicture of a view resolved against a larger size") {
    // Offset (100, 75) against 300 x 200: clipped to the region's own 180 x 90.
    const ViewTransform view = zoomedView({100, 50}, 1, std::nullopt, {300, 200});
    REQUIRE(view.offset().x == 100);
    REQUIRE(view.offset().y == 75);
    const PixelRect hugged = visiblePicture(view, pictureRegion(HUG_EXTENT, HUG_STRIPS));
    CHECK(hugged == PixelRect{120, 85, 80, 15});
    CHECK(insideOutput(hugged, HUG_EXTENT));
    const RulerRects rects = rulerRects(HUG_EXTENT, HUG_STRIPS, view);
    CHECK(rects.hugged == hugged);
    for (const PixelRect rect : {rects.top, rects.left, rects.corner}) {
        CHECK(insideOutput(rect, HUG_EXTENT));
    }
}

TEST_CASE("view lines: rulerRects hug a smaller picture") {
    const RulerRects narrow = rulerRects(HUG_EXTENT, HUG_STRIPS, zoomedView({100, 50}, 1));
    CHECK(narrow == RulerRects{.hugged = {60, 30, 100, 50},
                               .top = {60, 20, 100, 10},
                               .left = {40, 30, 20, 50},
                               .corner = {40, 20, 20, 10}});
    const RulerRects tall = rulerRects(HUG_EXTENT, HUG_STRIPS, zoomedView({100, 200}, 1));
    CHECK(tall.hugged == PixelRect{60, 10, 100, 90});
    CHECK(tall.top == PixelRect{60, 0, 100, 10});  // at the window's top edge
    CHECK(tall.left == PixelRect{40, 10, 20, 90});
    CHECK(tall.corner == PixelRect{40, 0, 20, 10});
}

TEST_CASE("view lines: rulerRects are the reserved strips when the picture covers P") {
    const auto reserved = [](dvv::platform::PixelSize extent, ViewStrips strips) {
        const PixelRect p = pictureRegion(extent, strips);
        return RulerRects{.hugged = p,
                          .top = {p.x, 0, extent.width - p.x, p.y},
                          .left = {0, p.y, p.x, extent.height - p.y},
                          .corner = {0, 0, p.x, p.y}};
    };
    CHECK(rulerRects(HUG_EXTENT, HUG_STRIPS, zoomedView({100, 50}, 4)) ==
          reserved(HUG_EXTENT, HUG_STRIPS));
    CHECK(rulerRects(HUG_EXTENT, HUG_STRIPS, ViewTransform{}) == reserved(HUG_EXTENT, HUG_STRIPS));
    const dvv::platform::PixelSize tiny{8, 8};
    const ViewStrips wide{.top = 30, .left = 30};
    const RulerRects clamped = rulerRects(tiny, wide, zoomedView({100, 50}, 1, std::nullopt, {}));
    CHECK(clamped == reserved(tiny, wide));
    CHECK(clamped.corner == PixelRect{0, 0, 8, 8});
    // Continuity: as the picture grows the rulers move out to the window edges and stay there.
    std::uint32_t last_top = HUG_EXTENT.height;
    std::uint32_t last_left = HUG_EXTENT.width;
    for (int eighths = 8; eighths <= 32; ++eighths) {
        const double zoom = eighths / 8.0;
        CAPTURE(zoom);
        const RulerRects rects = rulerRects(HUG_EXTENT, HUG_STRIPS, zoomedView({100, 50}, zoom));
        CHECK(rects.top.y <= last_top);
        CHECK(rects.left.x <= last_left);
        CHECK((rects.top.y == 0) == (50 * zoom >= 90));
        CHECK((rects.left.x == 0) == (100 * zoom >= 180));
        for (const PixelRect rect : {rects.top, rects.left, rects.corner}) {
            CHECK(insideOutput(rect, HUG_EXTENT));
        }
        last_top = rects.top.y;
        last_left = rects.left.x;
    }
}

TEST_CASE("view lines: rulerRects with the rulers hidden") {
    const RulerRects rects =
        rulerRects(HUG_EXTENT, {}, zoomedView({100, 50}, 1, std::nullopt, HUG_EXTENT));
    CHECK(rects.hugged == PixelRect{50, 25, 100, 50});
    CHECK(rects.top.empty());
    CHECK(rects.left.empty());
    CHECK(rects.corner.empty());
}

TEST_CASE("view lines: nearestTick") {
    std::array<LineLevel, LINE_LEVELS> levels{};
    levels[0] = {.stepLog2 = 5, .drawn = true};  // 32
    levels[1] = {.stepLog2 = 3, .drawn = true};  // 8
    levels[2] = {.stepLog2 = 1, .drawn = true};  // 2
    for (std::uint32_t c = 0; c < 37; ++c) {
        CAPTURE(c);
        CHECK(nearestTick(c, 37, levels) == nearestMultipleByScan(c, 37, 2));
    }
    for (std::uint32_t step_log2 = 0; step_log2 <= 5; ++step_log2) {  // every c at each step
        std::array<LineLevel, LINE_LEVELS> one{};
        one[2] = {.stepLog2 = step_log2, .drawn = true};
        for (const std::uint32_t size : {37U, 101U}) {
            for (std::uint32_t c = 0; c < size; ++c) {
                CAPTURE(step_log2);
                CAPTURE(size);
                CAPTURE(c);
                CHECK(nearestTick(c, size, one) ==
                      nearestMultipleByScan(c, size, std::uint32_t{1} << step_log2));
            }
        }
    }
    CHECK(nearestTick(0, 37, levels) == 0U);
    CHECK(nearestTick(1, 37, levels) == 2U);
    CHECK(nearestTick(5, 37, levels) == 6U);
    std::array<LineLevel, LINE_LEVELS> major = levels;  // only the major drawn
    major[1].drawn = false;
    major[2].drawn = false;
    CHECK(nearestTick(15, 37, major) == 0U);
    CHECK(nearestTick(16, 37, major) == 32U);
    std::array<LineLevel, LINE_LEVELS> eight = levels;  // only 8 drawn
    eight[0].drawn = false;
    eight[2].drawn = false;
    CHECK(nearestTick(100, 101, eight) == 96U);  // at most the last multiple in [0, size]
    CHECK(nearestTick(100, 101, eight) == nearestMultipleByScan(100, 101, 8));
    std::array<LineLevel, LINE_LEVELS> fine = major;  // the finest drawn level, whatever its index
    fine[0].stepLog2 = 3;
    fine[2] = {.stepLog2 = 4, .drawn = true};
    CHECK(nearestTick(9, 37, fine) == 8U);  // step 16 alone would give 16
    CHECK(nearestTick(11, 37, fine) == 8U);
    std::array<LineLevel, LINE_LEVELS> ones{};
    ones[1] = {.stepLog2 = 0, .drawn = true};
    CHECK(nearestTick(7, 37, ones) == 7U);  // 7.5 is midway: the lower
    CHECK(nearestTick(36, 37, ones) == 36U);
    std::array<LineLevel, LINE_LEVELS> huge{};
    huge[0] = {.stepLog2 = 40, .drawn = true};
    CHECK(nearestTick(36, 37, huge) == 0U);  // the single multiple 0
    // Near 2^32 the arithmetic needs 64 bits: 2c + s and 2s overflow 32 bits.
    std::array<LineLevel, LINE_LEVELS> two{};
    two[0] = {.stepLog2 = 1, .drawn = true};
    CHECK(nearestTick(0xFFFFFFFEU, 0xFFFFFFFFU, two) == 0xFFFFFFFEU);
    std::array<LineLevel, LINE_LEVELS> half{};
    half[0] = {.stepLog2 = 31, .drawn = true};
    CHECK(nearestTick(0xFFFFFFF0U, 0xFFFFFFFFU, half) == 0x80000000U);  // 2^32 > size: clamped
    CHECK_FALSE(nearestTick(3, 37, std::array<LineLevel, LINE_LEVELS>{}).has_value());
    CHECK_FALSE(nearestTick(37, 37, levels).has_value());
    CHECK_FALSE(nearestTick(0, 0, levels).has_value());
}

namespace {

// imagePixelAt of every pixel of the view's x image span lies in visiblePixels, and the range's
// ends are shown: at the span's first and last pixel.
void checkVisibleByImagePixelAt(const ViewTransform& view) {
    const AxisView axis = axisX(view);
    const PixelSpan span = imageSpan(axis);
    REQUIRE_FALSE(span.empty());
    const std::optional<PixelRange> shown = visiblePixels(axis);
    REQUIRE(shown.has_value());
    const PixelRange range = shown.value_or(PixelRange{});  // REQUIRE above stops the test case
    const double y = view.viewport().height / 2.0;
    std::size_t outside = 0;
    for (std::int32_t p = span.begin; p < span.end; ++p) {
        const std::optional<ImagePixel> pixel = imagePixelAt(view, PointD{.x = p + 0.5, .y = y});
        outside += pixel && range.first <= pixel->x && pixel->x <= range.last ? 0 : 1;
    }
    CHECK(outside == 0);
}

// By the definition: image pixel c covers [b(c), b(c + 1)) in viewport coordinates (b in double)
// and counts when it overlaps the visible extent [max(0, b(0)), min(extent, b(size))); nullopt
// where the image span is empty.
std::optional<PixelRange> visibleByOverlap(const AxisView& axis) {
    if (imageSpan(axis).empty()) {
        return std::nullopt;
    }
    const auto b = [&](std::uint32_t c) {
        return axis.offset + (static_cast<double>(c) * static_cast<double>(axis.zoom));
    };
    const double begin = std::max(0.0, b(0));
    const double end = std::min(static_cast<double>(axis.extent), b(axis.size));
    std::optional<PixelRange> range;
    for (std::uint32_t c = 0; c < axis.size; ++c) {
        if (b(c + 1) > begin && b(c) < end) {
            if (!range) {
                range = PixelRange{.first = c, .last = c};
            }
            range->last = c;
        }
    }
    return range;
}

}  // namespace

TEST_CASE("view lines: visiblePixels") {
    // The whole 3840 picture at Fit into 1920.
    const ViewTransform fit = ViewTransform::fit({1920, 1080}, {3840, 2160});
    CHECK(visiblePixels(axisX(fit)) == PixelRange{.first = 0, .last = 3839});
    checkVisibleByImagePixelAt(fit);
    // Panned at 100 %: offset round(500 - 1000) = -500.
    const ViewTransform panned =
        ViewTransform::zoomed({1000, 100}, {2000, 100}, 1, {.x = 1000, .y = 50});
    REQUIRE(axisX(panned).offset == -500);
    CHECK(visiblePixels(axisX(panned)) == PixelRange{.first = 500, .last = 1499});
    checkVisibleByImagePixelAt(panned);
    // 800 % with offset round(50 - 6.75 x 8) = -4: image pixel 0 covers [-4, 4), partly visible.
    const ViewTransform close =
        ViewTransform::zoomed({100, 100}, {100, 100}, 8, {.x = 6.75, .y = 50});
    REQUIRE(axisX(close).offset == -4);
    CHECK(visiblePixels(axisX(close)) == PixelRange{.first = 0, .last = 12});
    checkVisibleByImagePixelAt(close);
    // A narrower picture.
    const AxisView narrow{.offset = 100, .zoom = 1, .size = 200, .extent = 1000};
    CHECK(visiblePixels(narrow) == PixelRange{.first = 0, .last = 199});
    CHECK(visiblePixels(narrow) == visibleByOverlap(narrow));
    const ViewTransform centred =
        ViewTransform::zoomed({1000, 100}, {200, 100}, 1, {.x = 100, .y = 50});
    CHECK(visiblePixels(axisX(centred)) == PixelRange{.first = 0, .last = 199});
    checkVisibleByImagePixelAt(centred);
    // A zoom that is not a power of two.
    const ViewTransform odd =
        ViewTransform::zoomed({1000, 100}, {3840, 100}, 0.37, {.x = 1900, .y = 50});
    const std::optional<PixelRange> odd_range = visiblePixels(axisX(odd));
    REQUIRE(odd_range.has_value());
    CHECK(odd_range.value_or(PixelRange{}).first > 0);  // REQUIRE above stops the test case
    CHECK(odd_range.value_or(PixelRange{}).last < 3839);
    checkVisibleByImagePixelAt(odd);
    for (const std::int32_t offset : {-3000, -1203, -7, 0, 5, 999}) {
        const AxisView axis{.offset = offset, .zoom = 0.37F, .size = 3840, .extent = 1000};
        CAPTURE(offset);
        CHECK(visiblePixels(axis) == visibleByOverlap(axis));
    }
    // The whole picture at a Fit that is not a power of two, on both axes: the last pixel's area
    // starts on the last viewport pixel of the image span, but past that pixel's centre.
    const ViewTransform fit_4k = ViewTransform::fit({1920, 1080}, {4000, 3000});
    CHECK(visiblePixels(axisX(fit_4k)) == PixelRange{.first = 0, .last = 3999});
    CHECK(visiblePixels(axisY(fit_4k)) == PixelRange{.first = 0, .last = 2999});
    const AxisView third{.offset = 293, .zoom = 1.0F / 3.0F, .size = 4000, .extent = 1920};
    CHECK(visiblePixels(third) == PixelRange{.first = 0, .last = 3999});
    CHECK(visiblePixels(third) == visibleByOverlap(third));
    // Every picture that fits, at 0.37 and at 1/3: the whole picture at any offset.
    for (const float zoom : {0.37F, 1.0F / 3.0F}) {
        std::size_t differ = 0;
        for (std::int32_t offset = 0; offset <= 100; ++offset) {
            for (std::uint32_t size = 8; size <= 2000; size += 7) {
                const AxisView axis{.offset = offset, .zoom = zoom, .size = size, .extent = 1000};
                differ += visiblePixels(axis) == PixelRange{.first = 0, .last = size - 1} ? 0 : 1;
            }
        }
        CAPTURE(zoom);
        CHECK(differ == 0);
    }
    // Saturated: the far edge's image point rounds below the near one's; the range stays ordered.
    CHECK(visiblePixels({.offset = std::numeric_limits<std::int32_t>::min(),
                         .zoom = 0x1p31F,
                         .size = 8,
                         .extent = 16}) == PixelRange{.first = 1, .last = 1});
    // The largest size at a tiny zoom, the whole picture in the viewport.
    CHECK(visiblePixels({.offset = 0, .zoom = 1e-9F, .size = 0xFFFFFFFFU, .extent = 16}) ==
          PixelRange{.first = 0, .last = 0xFFFFFFFEU});
    // The largest size with the far end cut: its image point rounds to 2^32, kept at size - 1.
    CHECK(
        visiblePixels(
            {.offset = -0x40000000, .zoom = 0x1.000002p-2F, .size = 0xFFFFFFFFU, .extent = 100}) ==
        PixelRange{.first = 4294966784U, .last = 0xFFFFFFFEU});
    // Every power-of-two zoom from 1/16 to 32 and offsets past both ends of the viewport.
    for (int e = -4; e <= 5; ++e) {
        const float zoom = std::ldexp(1.0F, e);
        std::size_t differ = 0;
        for (std::int32_t offset = -2000; offset <= 250; offset += 3) {
            const AxisView axis{.offset = offset, .zoom = zoom, .size = 300, .extent = 200};
            differ += visiblePixels(axis) == visibleByOverlap(axis) ? 0 : 1;
        }
        CAPTURE(zoom);
        CHECK(differ == 0);
    }
    // No image, and an image wholly outside the viewport.
    CHECK_FALSE(visiblePixels(axisX(ViewTransform{})).has_value());
    CHECK_FALSE(visiblePixels({.offset = 0, .zoom = 0, .size = 8, .extent = 16}).has_value());
    CHECK_FALSE(visiblePixels({.offset = 0, .zoom = 1, .size = 0, .extent = 16}).has_value());
    CHECK_FALSE(
        visiblePixels({.offset = 1000, .zoom = 1, .size = 200, .extent = 1000}).has_value());
    CHECK_FALSE(
        visiblePixels({.offset = 1200, .zoom = 1, .size = 200, .extent = 1000}).has_value());
    CHECK_FALSE(
        visiblePixels({.offset = -200, .zoom = 1, .size = 200, .extent = 1000}).has_value());
    CHECK(visiblePixels({.offset = -199, .zoom = 1, .size = 200, .extent = 1000}) ==
          PixelRange{.first = 199, .last = 199});
}

TEST_CASE("view lines: numberStepLog2") {
    constexpr float NEEDED = 37.5F;
    for (std::uint32_t major = 1; major <= 8; ++major) {
        for (int e = -4; e <= 5; ++e) {
            const float zoom = std::ldexp(1.0F, e);
            const std::uint32_t s = numberStepLog2(major, zoom, NEEDED, 0xFFFFFFFFU);
            CAPTURE(major);
            CAPTURE(zoom);
            REQUIRE(s >= major);
            REQUIRE(s < 31);
            CHECK(std::ldexp(zoom, static_cast<int>(s)) >= NEEDED);  // fits
            if (s > major) {
                CHECK(std::ldexp(zoom, static_cast<int>(s) - 1) < NEEDED);  // one below does not
            }
            // A needed spacing of exactly 2^s x zoom fits at s; just above it needs one more.
            const float exact = std::ldexp(zoom, static_cast<int>(s));
            CHECK(numberStepLog2(major, zoom, exact, 0xFFFFFFFFU) == s);
            CHECK(numberStepLog2(major, zoom, std::nextafter(exact, 1e30F), 0xFFFFFFFFU) == s + 1);
        }
    }
    CHECK(numberStepLog2(3, 1, 8, 1000) == 3);  // the major step fits
    CHECK(numberStepLog2(3, 1, 9, 1000) == 4);
    CHECK(numberStepLog2(5, 0.01F, 50, 100) == 7);       // stops past the image size
    CHECK(numberStepLog2(5, 0.01F, 50, 128) == 8);       // a step equal to the size goes on
    CHECK(numberStepLog2(7, 0.01F, 50, 100) == 7);       // the major step already exceeds it
    CHECK(numberStepLog2(1, 0, 50, 0x80000000U) == 31);  // the loop ends at 31
    CHECK(numberStepLog2(1, 0, 50, 0xFFFFFFFFU) == 31);
    CHECK(numberStepLog2(31, 0, 50, 0xFFFFFFFFU) == 31);
    CHECK(numberStepLog2(0, 1, 0, 0) == 0);  // nothing needed
    CHECK(numberStepLog2(0, 1, 5, 0) == 0);  // an empty image: the major step
}

TEST_CASE("view lines: pixelCentreAt") {
    for (const float zoom : {0.125F, 0.5F, 1.0F, 2.0F, 8.0F, 32.0F}) {
        for (const std::int32_t offset : {-500, -4, 0, 3, 100}) {
            const AxisView axis{.offset = offset, .zoom = zoom, .size = 4096, .extent = 1000};
            std::size_t bad = 0;
            for (std::uint32_t c = 0; c < 4096; ++c) {
                const float expected =
                    static_cast<float>(offset) + ((static_cast<float>(c) + 0.5F) * zoom);
                bad += pixelCentreAt(axis, c) == expected ? 0 : 1;
            }
            CAPTURE(zoom);
            CAPTURE(offset);
            CHECK(bad == 0);
        }
    }
    const AxisView odd{.offset = 3, .zoom = 0.37F, .size = 4096, .extent = 1000};
    std::size_t outside = 0;
    for (std::uint32_t c = 0; c < odd.size; ++c) {
        const float centre = pixelCentreAt(odd, c);
        outside += boundaryAt(odd, c) < centre && centre < boundaryAt(odd, c + 1) ? 0 : 1;
    }
    CHECK(outside == 0);
}

TEST_CASE("view lines: pixelCentreAt is the three statements, not a fused multiply-add") {
    const AxisView a{.offset = -1000, .zoom = 0.473F, .size = 3840, .extent = 1816};
    std::size_t fused_differs = 0;
    for (std::uint32_t c = 0; c < a.size; ++c) {
        const float centre = static_cast<float>(c) + 0.5F;
        const float product = centre * a.zoom;
        const float b = static_cast<float>(a.offset) + product;
        CHECK(pixelCentreAt(a, c) == b);
        if (std::fma(centre, a.zoom, static_cast<float>(a.offset)) != b) {
            ++fused_differs;
        }
    }
    CHECK(fused_differs > 0);  // the inputs can tell a fused result from the three statements
}
