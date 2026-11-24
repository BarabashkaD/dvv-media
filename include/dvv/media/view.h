#pragma once

// The view geometry: where an image lies in a viewport. Viewport sizes and offsets are in viewport
// pixels, image sizes in image pixels, and an image point is a position in image pixels; pixel
// (i, j) covers [i, i+1) x [j, j+1). Viewport pixel (px, py) shows the image point
// (originX + (px + 0.5) / zoom, originY + (py + 0.5) / zoom).

#include <cstdint>
#include <optional>

#include "dvv/platform/pixel_size.h"

namespace dvv::media {

struct PointD {
    double x = 0, y = 0;
    bool operator==(const PointD&) const = default;
};

struct PixelOffset {  // whole viewport pixels
    std::int64_t x = 0, y = 0;
    bool operator==(const PixelOffset&) const = default;
};

struct ImagePixel {
    std::uint32_t x = 0, y = 0;
    bool operator==(const ImagePixel&) const = default;
};

// The view a caller asks for; resolveView resolves it against the viewport and image size.
// Fit: the whole image, centred. Zoom: `zoom` viewport pixels per image pixel with image point
// `centre` at the viewport's centre, for images of size `image`; a request for another image size
// resolves as Fit.
enum class ViewMode : std::uint8_t { Fit, Zoom };
struct ViewRequest {
    ViewMode mode = ViewMode::Fit;
    double zoom = 1.0;                 // Zoom: viewport pixels per image pixel
    PointD centre{};                   // Zoom: the image point wanted at the viewport's centre
    dvv::platform::PixelSize image{};  // Zoom: the image size the request was made for
    bool operator==(const ViewRequest&) const = default;
};

class ViewTransform {  // value type
public:
    ViewTransform() = default;  // viewport 0x0, image 0x0, zoom 0, origin (0, 0), offset (0, 0)

    // zoom = min(W/w, H/h) with no clamp. Width-limited iff W·h <= H·w (64-bit integers; a tie is
    // width-limited). Width-limited: zoom = W/w, x offset 0, y offset = (H·w − h·W) / (2·w);
    // height-limited: zoom = H/h, y offset 0, x offset = (W·h − w·H) / (2·h); offsets by integer
    // division (whole viewport pixels). origin = −offset / zoom on each axis.
    // An empty viewport or image -> zoom 0, origin (0, 0) (only the background is drawn).
    [[nodiscard]] static ViewTransform fit(dvv::platform::PixelSize viewport,
                                           dvv::platform::PixelSize image);

    // Per axis, with W the viewport and s = image x zoom: s > W -> offset = round(W/2 - centre x
    // zoom) clamped to [ceil(W - s), 0]; else floor((W - s) / 2). origin = -offset / zoom.
    // offset() holds that offset saturated to [-2^62, 2^62], and 0 for a NaN one (a NaN centre, or
    // an infinite zoom at centre 0).
    // An empty viewport or image, or a zoom <= 0 (or NaN) -> zoom 0, origin (0, 0), mode Fit (only
    // the background is drawn), so a view without an image is always Fit.
    [[nodiscard]] static ViewTransform zoomed(dvv::platform::PixelSize viewport,
                                              dvv::platform::PixelSize image, double zoom,
                                              PointD centre);

    [[nodiscard]] double zoom() const noexcept;
    [[nodiscard]] PointD origin() const noexcept;  // the image point at the viewport's (0, 0)
    // The viewport pixel of image point (0, 0), as fit() and zoomed() computed it (stored, not
    // derived from origin(); saturated as zoomed() states); {0, 0} at zoom 0.
    [[nodiscard]] PixelOffset offset() const noexcept;
    [[nodiscard]] dvv::platform::PixelSize viewport() const noexcept;
    [[nodiscard]] dvv::platform::PixelSize image() const noexcept;
    // Fit from fit() and the default; Zoom from zoomed() with a zoom > 0 (else Fit).
    [[nodiscard]] ViewMode mode() const noexcept;

    bool operator==(const ViewTransform&) const = default;

private:
    dvv::platform::PixelSize viewport_{};
    dvv::platform::PixelSize image_{};
    double zoom_ = 0;
    PointD origin_{};
    PixelOffset offset_{};
    ViewMode mode_ = ViewMode::Fit;
};

// Fit, or Zoom with `request.image != image` -> ViewTransform::fit; else ViewTransform::zoomed.
[[nodiscard]] ViewTransform resolveView(const ViewRequest& request,
                                        dvv::platform::PixelSize viewport,
                                        dvv::platform::PixelSize image);

// The image point under a viewport point: origin + point / zoom.
[[nodiscard]] PointD imagePointAt(const ViewTransform& view, PointD viewport_point) noexcept;

// The image pixel shown at the viewport pixel under viewport_point, by the pixel-centre rule of
// view_lines.h, so it always agrees with the picture and the cursor marker: per axis, with
// p = floor(viewport_point) and u = imageCoordinateAt(axisX(view) or axisY(view), p) (in float),
// floor(u) when it lies in [0, width) x [0, height); nullopt otherwise, and when the view shows no
// image (a zoom that is not a finite number > 0 as a float).
[[nodiscard]] std::optional<ImagePixel> imagePixelAt(const ViewTransform& view,
                                                     PointD viewport_point) noexcept;

// Starts from resolveView (a Fit request works): the image point under `anchor` stays under it at
// the new zoom. The stored centre is clamped, not rounded: on an axis where image x zoom exceeds
// the viewport to [W/(2 zoom), image - W/(2 zoom)], else image/2. Mode Zoom, `image` set; the
// zoom itself is not clamped. When the resolved zoom is 0 (an empty viewport or image) or `zoom`
// <= 0, the centre is the request's own, unclamped.
[[nodiscard]] ViewRequest zoomAround(const ViewRequest& request, dvv::platform::PixelSize viewport,
                                     dvv::platform::PixelSize image, double zoom, PointD anchor);

// Moves the image by (dx, dy) viewport pixels (positive: right, down): centre -= d / zoom,
// clamped as in zoomAround. A Fit request, or one for another image size, is returned unchanged.
[[nodiscard]] ViewRequest panBy(const ViewRequest& request, dvv::platform::PixelSize viewport,
                                dvv::platform::PixelSize image, double dx, double dy);

// A pan can move the image: mode Zoom, `request.image == image`, and image x zoom exceeds the
// viewport on at least one axis.
[[nodiscard]] bool canPan(const ViewRequest& request, dvv::platform::PixelSize viewport,
                          dvv::platform::PixelSize image) noexcept;

}  // namespace dvv::media
