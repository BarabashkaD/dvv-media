// Built without FP contraction (CMakeLists.txt): every float statement rounds once, as the shader's
// `precise` does, so a fused multiply-add never changes boundaryAt.

#include "dvv/media/view_lines.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "dvv/media/view.h"
#include "dvv/platform/pixel_size.h"

namespace dvv::media {

namespace {

constexpr std::int64_t INT32_LOW = std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t INT32_HIGH = std::numeric_limits<std::int32_t>::max();

// The largest pixel magnitude: pixels are saturated to [-2^30, 2^30] (int in the shader).
constexpr float PIXEL_LIMIT = 0x1p30F;

// The largest thickness a span uses: with saturated pixels no span end overflows an int32.
constexpr std::uint32_t THICKNESS_LIMIT = 0x20000000U;

bool showsImage(const AxisView& axis) noexcept {
    return axis.size > 0 && axis.extent > 0 && std::isfinite(axis.zoom) && axis.zoom > 0;
}

PixelSpan clipSpan(PixelSpan span, PixelSpan image) noexcept {
    const std::int32_t begin = std::max(span.begin, image.begin);
    const std::int32_t end = std::min(span.end, image.end);
    if (end <= begin) {
        return {};
    }
    return {begin, end};
}

// The unclipped line span; the caller has checked k <= size and t > 0.
PixelSpan lineWide(const AxisView& axis, std::uint32_t k, std::uint32_t thickness) noexcept {
    const std::int32_t p = firstPixelOf(axis, k);
    const auto t = static_cast<std::int32_t>(std::min(thickness, THICKNESS_LIMIT));
    if (k == 0) {
        return {p, p + t};
    }
    if (k == axis.size) {
        return {p - t, p};
    }
    const std::int32_t begin = p - t / 2;
    return {begin, begin + t};
}

// The unclipped line span grown by one pixel on each side, then clipped.
PixelSpan haloOf(PixelSpan line, PixelSpan image) noexcept {
    return clipSpan({line.begin - 1, line.end + 1}, image);
}

// k is a multiple of 1 << step_log2; a step_log2 of 32 or more has the single multiple 0.
bool divides(std::uint32_t step_log2, std::uint32_t k) noexcept {
    if (step_log2 >= 32) {
        return k == 0;
    }
    const std::uint32_t mask = (1U << step_log2) - 1U;
    return (k & mask) == 0;
}

// The step of a level as a 64-bit value for linesInView and nearestTick; 1 << 32 stands for every
// larger step (no multiple but 0 lies in [0, size]).
std::uint64_t stepOf(std::uint32_t step_log2) noexcept {
    return std::uint64_t{1} << std::min<std::uint32_t>(step_log2, 32);
}

// The first n in [low, high) with firstPixelOf(n << step_log2) >= pixel (high if none).
// firstPixelOf(k) does not decrease with k.
std::uint64_t firstAtOrAfter(const AxisView& axis, std::uint32_t step_log2, std::uint64_t low,
                             std::uint64_t high, std::int64_t pixel) noexcept {
    while (low < high) {
        const std::uint64_t mid = low + (high - low) / 2;
        const auto k = static_cast<std::uint32_t>(mid << step_log2);
        if (firstPixelOf(axis, k) >= pixel) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

}  // namespace

AxisView axisX(const ViewTransform& view) noexcept {
    return {.offset = static_cast<std::int32_t>(std::clamp(view.offset().x, INT32_LOW, INT32_HIGH)),
            .zoom = static_cast<float>(view.zoom()),
            .size = view.image().width,
            .extent = view.viewport().width};
}

AxisView axisY(const ViewTransform& view) noexcept {
    return {.offset = static_cast<std::int32_t>(std::clamp(view.offset().y, INT32_LOW, INT32_HIGH)),
            .zoom = static_cast<float>(view.zoom()),
            .size = view.image().height,
            .extent = view.viewport().height};
}

float imageCoordinateAt(const AxisView& axis, std::int32_t pixel) noexcept {
    const float centre = static_cast<float>(pixel) + 0.5F;
    const float shifted = centre - static_cast<float>(axis.offset);
    const float u = shifted / axis.zoom;
    return u;
}

float boundaryAt(const AxisView& axis, std::uint32_t k) noexcept {
    const float product = static_cast<float>(k) * axis.zoom;
    const float b = static_cast<float>(axis.offset) + product;
    return b;
}

std::int32_t firstPixelOf(const AxisView& axis, std::uint32_t k) noexcept {
    const float b = boundaryAt(axis, k);
    const float edge = b - 0.5F;
    const float estimate = std::ceil(edge);
    // Written so that a NaN estimate (no image) saturates too.
    const float raised = estimate >= -PIXEL_LIMIT ? estimate : -PIXEL_LIMIT;
    const float saturated = raised > PIXEL_LIMIT ? PIXEL_LIMIT : raised;
    auto p = static_cast<std::int32_t>(saturated);
    const float target = static_cast<float>(k);
    if (imageCoordinateAt(axis, p - 1) >= target) {
        p -= 1;
    } else if (imageCoordinateAt(axis, p) < target) {
        p += 1;
    }
    return p;
}

PixelSpan imageSpan(const AxisView& axis) noexcept {
    if (!showsImage(axis)) {
        return {};
    }
    const auto extent = static_cast<std::int32_t>(std::min(axis.extent, 0x7FFFFFFFU));
    return clipSpan({firstPixelOf(axis, 0), firstPixelOf(axis, axis.size)}, {0, extent});
}

PixelSpan lineSpan(const AxisView& axis, std::uint32_t k, std::uint32_t thickness) noexcept {
    const PixelSpan image = imageSpan(axis);
    if (image.empty() || k > axis.size || thickness == 0) {
        return {};
    }
    return clipSpan(lineWide(axis, k, thickness), image);
}

PixelSpan haloSpan(const AxisView& axis, std::uint32_t k, std::uint32_t thickness) noexcept {
    const PixelSpan image = imageSpan(axis);
    if (image.empty() || k > axis.size || thickness == 0) {
        return {};
    }
    return haloOf(lineWide(axis, k, thickness), image);
}

PixelSpan cursorSpan(const AxisView& axis, std::uint32_t c) noexcept {
    const PixelSpan image = imageSpan(axis);
    if (image.empty() || c >= axis.size) {
        return {};
    }
    const std::int32_t last = firstPixelOf(axis, axis.size) - 1;
    const std::int32_t begin = std::min(firstPixelOf(axis, c), last);
    const std::int32_t end = std::max(begin + 1, firstPixelOf(axis, c + 1));
    return clipSpan({begin, end}, image);
}

bool levelVisible(std::uint32_t step_log2, float zoom, float minimum) noexcept {
    // 2^step_log2 is exact in float up to 2^127 and infinite beyond.
    const float step = std::ldexp(1.0F, static_cast<int>(std::min<std::uint32_t>(step_log2, 255)));
    const float spacing = step * zoom;
    return spacing >= minimum;
}

// The image span once per call and each candidate's unclipped line once, for its line and halo.
std::optional<LineHit> lineAt(const AxisView& axis,
                              const std::array<LineLevel, LINE_LEVELS>& levels,
                              std::int32_t pixel) noexcept {
    const PixelSpan image = imageSpan(axis);
    if (image.empty()) {
        return std::nullopt;
    }
    const float u = imageCoordinateAt(axis, pixel);
    for (std::size_t l = 0; l < LINE_LEVELS; ++l) {
        const LineLevel& level = levels[l];
        if (!level.drawn || level.thickness == 0) {
            continue;
        }
        const std::uint32_t shift = std::min<std::uint32_t>(level.stepLog2, 31);
        // The largest m with m << stepLog2 <= size.
        const std::uint32_t last = level.stepLog2 >= 32 ? 0 : axis.size >> shift;
        // One float operation per statement, as the shader writes it; the scale is exact.
        const float scale = std::ldexp(1.0F, -static_cast<int>(shift));
        const float q = u * scale;
        // Outside [-3, last + 2] every candidate lies outside [0, last]; the clamp keeps the
        // conversion in range and changes no candidate in it (exact for last < 2^24).
        const float high_bound = static_cast<float>(last) + 2.0F;
        const float high = std::min(high_bound, PIXEL_LIMIT);
        const float n = std::clamp(std::floor(q), -3.0F, high);
        const std::int32_t first = static_cast<std::int32_t>(n) - 1;
        constexpr std::size_t CANDIDATES = 4;
        std::array<bool, CANDIDATES> valid{};
        std::array<std::uint32_t, CANDIDATES> ks{};
        std::array<PixelSpan, CANDIDATES> wide{};
        for (std::size_t i = 0; i < CANDIDATES; ++i) {
            const std::int32_t m = first + static_cast<std::int32_t>(i);
            valid[i] = m >= 0 && static_cast<std::uint32_t>(m) <= last;
            if (valid[i]) {
                ks[i] = static_cast<std::uint32_t>(m) << shift;
                wide[i] = lineWide(axis, ks[i], level.thickness);
            }
        }
        for (std::size_t i = 0; i < CANDIDATES; ++i) {
            if (!valid[i]) {
                continue;
            }
            const PixelSpan line = clipSpan(wide[i], image);
            if (line.begin <= pixel && pixel < line.end) {
                return LineHit{.level = l, .k = ks[i], .halo = false};
            }
        }
        for (std::size_t i = 0; i < CANDIDATES; ++i) {
            if (!level.halo || !valid[i]) {
                continue;
            }
            const PixelSpan halo = haloOf(wide[i], image);
            if (halo.begin <= pixel && pixel < halo.end) {
                return LineHit{.level = l, .k = ks[i], .halo = true};
            }
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> tickLevel(std::uint32_t k,
                                     const std::array<LineLevel, LINE_LEVELS>& levels) noexcept {
    for (std::size_t l = 0; l < LINE_LEVELS; ++l) {
        const LineLevel& level = levels[l];
        if (level.drawn && divides(level.stepLog2, k)) {
            return l;
        }
    }
    return std::nullopt;
}

std::vector<std::uint32_t> linesInView(const AxisView& axis, std::uint32_t step_log2) {
    std::vector<std::uint32_t> lines;
    const PixelSpan image = imageSpan(axis);
    if (image.empty()) {
        return lines;
    }
    const std::uint32_t shift = std::min<std::uint32_t>(step_log2, 32);
    const std::uint64_t count = std::uint64_t{axis.size} / stepOf(shift) + 1;  // n = 0 .. size/step
    // A one-pixel line below k = size is [p, p + 1) with p = firstPixelOf(k): it shows when p lies
    // in the image's span, and p does not decrease with k, so those n form one range.
    const std::uint64_t begin = firstAtOrAfter(axis, shift, 0, count, image.begin);
    const std::uint64_t end = firstAtOrAfter(axis, shift, begin, count, image.end);
    const auto multiple_of_step = [&](std::uint64_t n) {
        return static_cast<std::uint32_t>(n << shift);
    };
    for (std::uint64_t n = begin; n < end; ++n) {
        if (multiple_of_step(n) != axis.size) {
            lines.push_back(multiple_of_step(n));
        }
    }
    // k = size, the largest multiple when the step divides the size, has its own span.
    if (divides(step_log2, axis.size) && !lineSpan(axis, axis.size, 1).empty()) {
        lines.push_back(axis.size);
    }
    return lines;
}

PixelRect pictureRegion(dvv::platform::PixelSize extent, ViewStrips strips) noexcept {
    const std::uint32_t left = std::min(strips.left, extent.width);
    const std::uint32_t top = std::min(strips.top, extent.height);
    return {.x = left, .y = top, .width = extent.width - left, .height = extent.height - top};
}

std::optional<ImagePixel> cursorPixel(const ViewTransform& view, PixelRect picture,
                                      PointD point) noexcept {
    if (!picture.contains(point)) {
        return std::nullopt;
    }
    return imagePixelAt(view, {.x = point.x - static_cast<double>(picture.x),
                               .y = point.y - static_cast<double>(picture.y)});
}

PixelRect visiblePicture(const ViewTransform& view, PixelRect picture) noexcept {
    // The image span of one axis clipped to the region's size on that axis; the span already lies
    // in [0, the view's extent).
    const auto clipped = [](const AxisView& axis, std::uint32_t region) {
        const auto limit = static_cast<std::int32_t>(std::min(region, 0x7FFFFFFFU));
        return clipSpan(imageSpan(axis), {0, limit});
    };
    const PixelSpan x = clipped(axisX(view), picture.width);
    const PixelSpan y = clipped(axisY(view), picture.height);
    if (x.empty() || y.empty()) {
        return {};
    }
    // Non-empty clipped spans have 0 <= begin < end.
    return {.x = picture.x + static_cast<std::uint32_t>(x.begin),
            .y = picture.y + static_cast<std::uint32_t>(y.begin),
            .width = static_cast<std::uint32_t>(x.end - x.begin),
            .height = static_cast<std::uint32_t>(y.end - y.begin)};
}

RulerRects rulerRects(dvv::platform::PixelSize extent, ViewStrips strips,
                      const ViewTransform& view) noexcept {
    const PixelRect p = pictureRegion(extent, strips);
    const PixelRect visible = visiblePicture(view, p);
    const PixelRect h = visible.empty() ? p : visible;
    // h lies in p, so h.x >= p.x and h.y >= p.y.
    return {.hugged = h,
            .top = {.x = h.x, .y = h.y - p.y, .width = h.width, .height = p.y},
            .left = {.x = h.x - p.x, .y = h.y, .width = p.x, .height = h.height},
            .corner = {.x = h.x - p.x, .y = h.y - p.y, .width = p.x, .height = p.y}};
}

std::optional<std::uint32_t> nearestTick(
    std::uint32_t c, std::uint32_t size,
    const std::array<LineLevel, LINE_LEVELS>& levels) noexcept {
    if (c >= size) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> finest;
    for (const LineLevel& level : levels) {
        if (level.drawn && (!finest || level.stepLog2 < *finest)) {
            finest = level.stepLog2;
        }
    }
    if (!finest) {
        return std::nullopt;
    }
    // m = ceil((c + 0.5) / s - 0.5), the nearest multiple with the lower one on a tie, in integers:
    // floor((2c + s) / 2s). A step of 2^32 or more has the single multiple 0 in [0, size].
    const std::uint64_t step = stepOf(*finest);
    const std::uint64_t nearest = (2 * std::uint64_t{c} + step) / (2 * step) * step;
    const std::uint64_t last = std::uint64_t{size} / step * step;
    return static_cast<std::uint32_t>(std::min(nearest, last));
}

}  // namespace dvv::media
