#pragma once

// Lines on the shown image: which image pixel a viewport pixel shows, where the lines, halos and
// cursor markers are drawn, the nested line levels, the picture region, the rulers' rects and the
// nearest tick. One rule, written once: a GPU shader that draws the same lines or the same picture
// mirrors these functions statement by statement, and the CPU references and tests use exactly
// these. CPU helpers that no shader mirrors: axisX, axisY, linesInView, and the picture region and
// everything after it.
//
// Units as in view.h: an AxisView is one axis of a ViewTransform, in viewport pixels and image
// pixels. Image pixel i covers [i, i + 1) and viewport pixel p covers [p, p + 1). The one mapping
// rule is the pixel centre: viewport pixel p shows the image point u(p) = imageCoordinateAt(p) and
// the image pixel floor(u(p)); the image span, the lines, the halos, the cursor markers and
// imagePixelAt (view.h) all derive from it. A span is a half-open range of viewport pixels; an
// empty span is {0, 0}. "Clipped" means intersected with imageSpan(): nothing is drawn outside the
// image or the viewport. An axis with a zoom that is not a finite number > 0 shows no image: every
// span is empty and every lookup is nullopt.
//
// The arithmetic is 32-bit int and float, as in the shader (shaders/view_lines.slang, the same
// names in the same namespace); only linesInView's counts are 64-bit. Pixels are saturated to
// [-2^30, 2^30] and a span uses a thickness of at most 2^29 (a larger one acts as 2^29), so no span
// end overflows. firstPixelOf is exact while |offset| + k x zoom < 2^22 and lineAt while the image
// size is below 2^24.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "dvv/media/view.h"
#include "dvv/platform/pixel_size.h"

namespace dvv::media {

inline constexpr std::size_t LINE_LEVELS = 3;  // nested levels; see LineLevel for the order

struct AxisView {              // one axis of the shown image
    std::int32_t offset = 0;   // the viewport pixel of image coordinate 0
    float zoom = 0;            // viewport pixels per image pixel (the shaders' float)
    std::uint32_t size = 0;    // image pixels on this axis
    std::uint32_t extent = 0;  // the viewport's pixels on this axis
    bool operator==(const AxisView&) const = default;
};
// offset().x / .y (clamped to int32), float(zoom()), image().width / .height,
// viewport().width / .height.
[[nodiscard]] AxisView axisX(const ViewTransform& view) noexcept;
[[nodiscard]] AxisView axisY(const ViewTransform& view) noexcept;

struct PixelSpan {  // [begin, end) in viewport pixels
    std::int32_t begin = 0, end = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return end <= begin; }
    bool operator==(const PixelSpan&) const = default;
};

// u(p) = (float(pixel) + 0.5 - float(offset)) / zoom, one float operation per statement: the image
// point at the centre of viewport pixel `pixel`. Viewport pixel p shows image pixel floor(u(p))
// when 0 <= u(p) < size. u does not decrease with p.
[[nodiscard]] float imageCoordinateAt(const AxisView& axis, std::int32_t pixel) noexcept;

// b = float(offset) + float(k) * zoom, the multiply and the add as separate statements in a file
// built without FP contraction (the shader marks it precise): where boundary k lies, the estimate
// firstPixelOf starts from.
[[nodiscard]] float boundaryAt(const AxisView& axis, std::uint32_t k) noexcept;

// The first viewport pixel of image pixel k: the smallest p with u(p) >= k (b itself at a dyadic
// zoom >= 1). Computed as e = ceil(b - 0.5), saturated to [-2^30, 2^30], then moved by one pixel
// where u(e - 1) >= k or u(e) < k. Not clipped; meaningful only for a zoom that is a finite
// number > 0.
[[nodiscard]] std::int32_t firstPixelOf(const AxisView& axis, std::uint32_t k) noexcept;

// The pixels with 0 <= u(p) < size, [firstPixelOf(0), firstPixelOf(size)), intersected with
// [0, extent); empty when the axis shows no image.
[[nodiscard]] PixelSpan imageSpan(const AxisView& axis) noexcept;

// With p = firstPixelOf(k): k = 0 -> [p, p + t); k = size -> [p - t, p) (the outer edges are drawn
// inside the image, at its first and last pixels); else [p - t/2, p - t/2 + t) (t/2 rounded down:
// a 1-pixel line is the first pixel of image pixel k). Clipped; empty for k > size or t == 0.
[[nodiscard]] PixelSpan lineSpan(const AxisView& axis, std::uint32_t k,
                                 std::uint32_t thickness) noexcept;

// The unclipped line span grown by one pixel on each side, then clipped; empty where lineSpan is
// empty by k or t.
[[nodiscard]] PixelSpan haloSpan(const AxisView& axis, std::uint32_t k,
                                 std::uint32_t thickness) noexcept;

// The pixels whose image pixel is c: [s, max(s + 1, firstPixelOf(c + 1))) with
// s = min(firstPixelOf(c), firstPixelOf(size) - 1), clipped; empty for c >= size. An image pixel
// narrower than a viewport pixel shows on no pixel; its marker is then the one pixel at s, and the
// min keeps it inside the image at its end, like the outer edge line, so a cursor pixel whose span
// reaches into the viewport is never an empty marker.
[[nodiscard]] PixelSpan cursorSpan(const AxisView& axis, std::uint32_t c) noexcept;

// float(1 << stepLog2) * zoom >= minimum, all in viewport pixels (2^stepLog2 as an exact float).
[[nodiscard]] bool levelVisible(std::uint32_t step_log2, float zoom, float minimum) noexcept;

// The levels of a std::array<LineLevel, LINE_LEVELS> are coarsest first: index 0 wins in lineAt
// and tickLevel. A stepLog2 of 32 or more has the single multiple 0 in [0, size].
struct LineLevel {
    std::uint32_t stepLog2 = 0;   // step = 1 << stepLog2
    std::uint32_t thickness = 1;  // viewport pixels
    bool drawn = false;
    bool halo = false;
    bool operator==(const LineLevel&) const = default;
};
struct LineHit {
    std::size_t level = 0;
    std::uint32_t k = 0;
    bool halo = false;  // the pixel is on the level's halo, not its line
    bool operator==(const LineHit&) const = default;
};

// For each drawn level in index order: q = u(pixel) x 2^-stepLog2 (an exact power-of-two scale),
// n = floor(q) clamped to [-3, last + 2] in float, with last = size >> stepLog2 (0 for a stepLog2
// of 32 or more); the candidates k = m << stepLog2 for m = n - 1 .. n + 2 within [0, last], in
// ascending k; the first whose lineSpan holds `pixel` hits; else, for a halo level, the first
// whose haloSpan holds it hits with halo = true. nullopt: no level. When each drawn level's
// spacing float(step) x zoom is at least its thickness, the result is that of a scan of every line
// in ascending k: only n - 1, n and n + 1 can hold the pixel, and the window n - 1 .. n + 2 still
// holds them when a shader's u is a few ulp off (a GPU divide need not be correctly rounded) and
// floor(q) moves by one. Below that spacing the level, the halo flag and hit-or-miss still match
// the scan, but k may be a neighbouring line's.
[[nodiscard]] std::optional<LineHit> lineAt(const AxisView& axis,
                                            const std::array<LineLevel, LINE_LEVELS>& levels,
                                            std::int32_t pixel) noexcept;

// The first drawn level (index order) whose step divides k (a mask test); nullopt: none.
[[nodiscard]] std::optional<std::size_t> tickLevel(
    std::uint32_t k, const std::array<LineLevel, LINE_LEVELS>& levels) noexcept;

// The multiples of 1 << step_log2 in [0, size] whose one-pixel lineSpan is not empty, ascending.
// A CPU helper (64-bit counts); the shader does not mirror it.
[[nodiscard]] std::vector<std::uint32_t> linesInView(const AxisView& axis, std::uint32_t step_log2);

// The output frame is `extent` pixels; strips along its top and left edge leave the picture
// region, which is the viewport a ViewTransform is resolved against.
struct ViewStrips {  // output pixels
    std::uint32_t top = 0, left = 0;
    bool operator==(const ViewStrips&) const = default;
};
struct PixelRect {  // output pixels
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] constexpr dvv::platform::PixelSize size() const noexcept {
        return {width, height};
    }
    // Whether the point `point` (output pixels) lies in [x, x + width) x [y, y + height): the one
    // containment rule for the cursor and the hit tests. False for a NaN coordinate.
    [[nodiscard]] constexpr bool contains(PointD point) const noexcept {
        return point.x >= static_cast<double>(x) && point.y >= static_cast<double>(y) &&
               point.x < static_cast<double>(x) + static_cast<double>(width) &&
               point.y < static_cast<double>(y) + static_cast<double>(height);
    }
    bool operator==(const PixelRect&) const = default;
};

// The strips clamped to `extent`; x = left, y = top, the rest of the extent (at least 0 x 0).
[[nodiscard]] PixelRect pictureRegion(dvv::platform::PixelSize extent, ViewStrips strips) noexcept;

// The image pixel under the output point `point` (output pixels), as a picture drawn into the
// picture region `picture` shows it: nullopt when the point lies outside the region (over a strip
// or past the output), else imagePixelAt(view, point - the region's origin), with `view` resolved
// against the region's size. The one rule for a cursor readout and a cursor marker.
[[nodiscard]] std::optional<ImagePixel> cursorPixel(const ViewTransform& view, PixelRect picture,
                                                    PointD point) noexcept;

// The pixels of the picture region `picture` that show the image of `view`: imageSpan(axisX(view))
// and imageSpan(axisY(view)), each clipped to [0, min(the view's extent, the region's size)) of its
// axis, so the result stays inside the region even for a view resolved against another size, and
// moved by the region's origin. Empty ({}) when the view shows no image (zoom 0 or an empty image),
// the region is empty, or no image pixel falls inside the clipped range (the image lies past the
// region on an axis).
[[nodiscard]] PixelRect visiblePicture(const ViewTransform& view, PixelRect picture) noexcept;

// Rulers that hug the visible picture, in output pixels.
struct RulerRects {
    PixelRect hugged;  // visiblePicture, or the picture region when that is empty
    PixelRect top, left, corner;
    bool operator==(const RulerRects&) const = default;
};
// With P = pictureRegion(extent, strips) and H = hugged = visiblePicture(view, P), or P when that
// is empty: top {H.x, H.y - P.y, H.width, P.y}, left {H.x - P.x, H.y, P.x, H.height}, corner
// {H.x - P.x, H.y - P.y, P.x, P.y}. P.x and P.y are the strips as clamped to the extent and H lies
// in P, so the rects never leave the output. When H is P (the picture covers it, no image, an
// empty P) they are the reserved strips at the output's edges. Zero strips: all three empty.
[[nodiscard]] RulerRects rulerRects(dvv::platform::PixelSize extent, ViewStrips strips,
                                    const ViewTransform& view) noexcept;

// The tick nearest the centre c + 0.5 of image pixel c on the finest drawn level (the drawn level
// with the smallest step s, whatever its index): the multiple of s nearest to c + 0.5, the lower
// one on a tie (only s = 1 has ties), then at most the last multiple of s in [0, size]. nullopt
// when no level is drawn or c >= size. A CPU helper: the shaders take the tick as a value.
[[nodiscard]] std::optional<std::uint32_t> nearestTick(
    std::uint32_t c, std::uint32_t size, const std::array<LineLevel, LINE_LEVELS>& levels) noexcept;

}  // namespace dvv::media
