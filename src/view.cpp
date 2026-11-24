#include "dvv/media/view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "dvv/media/view_lines.h"
#include "dvv/platform/pixel_size.h"

namespace dvv::media {

namespace {

// The clamp of the centre on one axis: viewport in viewport pixels, image in image pixels.
double clampCentre(double centre, double viewport, double image, double zoom) noexcept {
    if (image * zoom > viewport) {
        const double half = viewport / (2 * zoom);
        return std::clamp(centre, half, image - half);
    }
    return image / 2;
}

PointD clampCentre(PointD centre, dvv::platform::PixelSize viewport, dvv::platform::PixelSize image,
                   double zoom) noexcept {
    return {clampCentre(centre.x, viewport.width, image.width, zoom),
            clampCentre(centre.y, viewport.height, image.height, zoom)};
}

// The viewport offset of the image on one axis, in whole viewport pixels.
double axisOffset(double centre, double viewport, double image, double zoom) noexcept {
    const double size = image * zoom;
    if (size > viewport) {
        return std::clamp(std::round(viewport / 2 - centre * zoom), std::ceil(viewport - size),
                          0.0);
    }
    return std::floor((viewport - size) / 2);
}

// An axis offset as an integer: saturated to [-2^62, 2^62] (both exact doubles inside the int64
// range) and NaN -> 0, so an infinite or huge zoom or a NaN centre never converts out of range.
std::int64_t wholePixels(double offset) noexcept {
    if (std::isnan(offset)) {
        return 0;
    }
    return static_cast<std::int64_t>(std::clamp(offset, -0x1p62, 0x1p62));
}

// The image pixel on one axis that the viewport pixel under `coordinate` shows, by the pixel-centre
// rule of view_lines.h, so that it always agrees with the cursor marker and the picture.
std::optional<std::uint32_t> imagePixelOn(const AxisView& axis, double coordinate) noexcept {
    // Written so that NaN gives nullopt too; pixels beyond 2^30 are saturated as in view_lines.h.
    if (!(std::isfinite(axis.zoom) && axis.zoom > 0) || std::isnan(coordinate)) {
        return std::nullopt;
    }
    const double pixel = std::floor(std::clamp(coordinate, -0x1p30, 0x1p30));
    const float u = imageCoordinateAt(axis, static_cast<std::int32_t>(pixel));
    const float image_pixel = std::floor(u);
    if (!(image_pixel >= 0.0F && image_pixel < 0x1p32F)) {
        return std::nullopt;
    }
    const auto c = static_cast<std::uint32_t>(image_pixel);
    if (c >= axis.size) {
        return std::nullopt;
    }
    return c;
}

}  // namespace

ViewTransform ViewTransform::fit(dvv::platform::PixelSize viewport,
                                 dvv::platform::PixelSize image) {
    ViewTransform t;
    t.viewport_ = viewport;
    t.image_ = image;
    if (viewport.empty() || image.empty()) {
        return t;
    }
    // 64-bit before any product: (2^32 - 1)^2 fits.
    const std::uint64_t vw = viewport.width;
    const std::uint64_t vh = viewport.height;
    const std::uint64_t iw = image.width;
    const std::uint64_t ih = image.height;
    std::uint64_t offset_x = 0;
    std::uint64_t offset_y = 0;
    if (vw * ih <= vh * iw) {  // width-limited (a tie too)
        t.zoom_ = static_cast<double>(vw) / static_cast<double>(iw);
        offset_y = (vh * iw - ih * vw) / (2 * iw);
    } else {  // height-limited
        t.zoom_ = static_cast<double>(vh) / static_cast<double>(ih);
        offset_x = (vw * ih - iw * vh) / (2 * ih);
    }
    // An offset is below the viewport size (< 2^32), so it fits the signed type.
    t.offset_ = {.x = static_cast<std::int64_t>(offset_x),
                 .y = static_cast<std::int64_t>(offset_y)};
    t.origin_ = {-static_cast<double>(offset_x) / t.zoom_,
                 -static_cast<double>(offset_y) / t.zoom_};
    return t;
}

double ViewTransform::zoom() const noexcept {
    return zoom_;
}
PointD ViewTransform::origin() const noexcept {
    return origin_;
}
PixelOffset ViewTransform::offset() const noexcept {
    return offset_;
}
dvv::platform::PixelSize ViewTransform::viewport() const noexcept {
    return viewport_;
}
dvv::platform::PixelSize ViewTransform::image() const noexcept {
    return image_;
}

ViewTransform ViewTransform::zoomed(dvv::platform::PixelSize viewport,
                                    dvv::platform::PixelSize image, double zoom, PointD centre) {
    ViewTransform t;
    t.viewport_ = viewport;
    t.image_ = image;
    if (viewport.empty() || image.empty() || !(zoom > 0)) {
        return t;  // mode Fit, zoom 0: only the background is drawn
    }
    t.mode_ = ViewMode::Zoom;
    t.zoom_ = zoom;
    const double offset_x = axisOffset(centre.x, viewport.width, image.width, zoom);
    const double offset_y = axisOffset(centre.y, viewport.height, image.height, zoom);
    t.offset_ = {.x = wholePixels(offset_x), .y = wholePixels(offset_y)};
    t.origin_ = {-offset_x / zoom, -offset_y / zoom};
    return t;
}

ViewMode ViewTransform::mode() const noexcept {
    return mode_;
}

ViewTransform resolveView(const ViewRequest& request, dvv::platform::PixelSize viewport,
                          dvv::platform::PixelSize image) {
    if (request.mode == ViewMode::Fit || request.image != image) {
        return ViewTransform::fit(viewport, image);
    }
    return ViewTransform::zoomed(viewport, image, request.zoom, request.centre);
}

PointD imagePointAt(const ViewTransform& view, PointD viewport_point) noexcept {
    return {view.origin().x + viewport_point.x / view.zoom(),
            view.origin().y + viewport_point.y / view.zoom()};
}

std::optional<ImagePixel> imagePixelAt(const ViewTransform& view, PointD viewport_point) noexcept {
    const std::optional<std::uint32_t> x = imagePixelOn(axisX(view), viewport_point.x);
    const std::optional<std::uint32_t> y = imagePixelOn(axisY(view), viewport_point.y);
    if (!x || !y) {
        return std::nullopt;
    }
    return ImagePixel{.x = *x, .y = *y};
}

ViewRequest zoomAround(const ViewRequest& request, dvv::platform::PixelSize viewport,
                       dvv::platform::PixelSize image, double zoom, PointD anchor) {
    const ViewTransform view = resolveView(request, viewport, image);
    ViewRequest result{
        .mode = ViewMode::Zoom, .zoom = zoom, .centre = request.centre, .image = image};
    if (!(view.zoom() > 0) || !(zoom > 0)) {
        return result;
    }
    const PointD point = imagePointAt(view, anchor);
    const PointD centre{point.x - anchor.x / zoom + viewport.width / (2 * zoom),
                        point.y - anchor.y / zoom + viewport.height / (2 * zoom)};
    result.centre = clampCentre(centre, viewport, image, zoom);
    return result;
}

ViewRequest panBy(const ViewRequest& request, dvv::platform::PixelSize viewport,
                  dvv::platform::PixelSize image, double dx, double dy) {
    if (request.mode == ViewMode::Fit || request.image != image || !(request.zoom > 0)) {
        return request;
    }
    ViewRequest result = request;
    result.centre =
        clampCentre({request.centre.x - dx / request.zoom, request.centre.y - dy / request.zoom},
                    viewport, image, request.zoom);
    return result;
}

bool canPan(const ViewRequest& request, dvv::platform::PixelSize viewport,
            dvv::platform::PixelSize image) noexcept {
    return request.mode == ViewMode::Zoom && request.image == image &&
           (image.width * request.zoom > viewport.width ||
            image.height * request.zoom > viewport.height);
}

}  // namespace dvv::media
