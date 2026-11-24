#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "dvv/media/view.h"
#include "dvv/platform/pixel_size.h"

using dvv::media::canPan;
using dvv::media::ImagePixel;
using dvv::media::imagePixelAt;
using dvv::media::imagePointAt;
using dvv::media::panBy;
using dvv::media::PixelOffset;
using dvv::media::PointD;
using dvv::media::resolveView;
using dvv::media::ViewMode;
using dvv::media::ViewRequest;
using dvv::media::ViewTransform;
using dvv::media::zoomAround;
using dvv::platform::PixelSize;

namespace {

double offsetX(const ViewTransform& t) {
    return -t.origin().x * t.zoom();
}
double offsetY(const ViewTransform& t) {
    return -t.origin().y * t.zoom();
}

// offset() is the offset fit() and zoomed() computed, the same one origin() was derived from.
void checkStoredOffset(const ViewTransform& t) {
    CHECK(static_cast<double>(t.offset().x) == doctest::Approx(offsetX(t)));
    CHECK(static_cast<double>(t.offset().y) == doctest::Approx(offsetY(t)));
}

// The image column and row of an output pixel: the image point at the pixel's centre.
double column(const ViewTransform& t, int px) {
    return std::floor(t.origin().x + (px + 0.5) / t.zoom());
}
double row(const ViewTransform& t, int py) {
    return std::floor(t.origin().y + (py + 0.5) / t.zoom());
}

const PixelSize VP{200, 100};
const PixelSize IMG{400, 300};

}  // namespace

TEST_SUITE("view") {
    TEST_CASE("view: pillarbox upscale (37x23 in 211x97)") {
        const auto t = ViewTransform::fit({211, 97}, {37, 23});
        CHECK(t.zoom() == 97.0 / 23.0);
        CHECK(t.origin().x == -27.0 / (97.0 / 23.0));
        CHECK(t.origin().y == 0.0);
        CHECK(std::abs(offsetX(t) - 27.0) <= 1e-9);
        CHECK(offsetY(t) == 0.0);
        checkStoredOffset(t);
        CHECK(t.viewport() == PixelSize{211, 97});
        CHECK(t.image() == PixelSize{37, 23});
    }

    TEST_CASE("view: letterbox upscale (64x48 in 400x400)") {
        const auto t = ViewTransform::fit({400, 400}, {64, 48});
        CHECK(t.zoom() == 400.0 / 64.0);
        CHECK(t.origin().x == 0.0);
        CHECK(t.origin().y == -50.0 / (400.0 / 64.0));
        CHECK(std::abs(offsetY(t) - 50.0) <= 1e-9);
        checkStoredOffset(t);
    }

    TEST_CASE("view: 1:1 and the tie") {
        const auto t = ViewTransform::fit({37, 23}, {37, 23});
        CHECK(t.zoom() == 1.0);
        CHECK(t.origin() == PointD{0, 0});
    }

    TEST_CASE("view: downscale (1920x1080 in 640x480)") {
        const auto t = ViewTransform::fit({640, 480}, {1920, 1080});
        CHECK(t.zoom() == 640.0 / 1920.0);
        CHECK(t.origin().x == 0.0);
        CHECK(t.origin().y == -60.0 / (640.0 / 1920.0));
        CHECK(std::abs(offsetY(t) - 60.0) <= 1e-9);
        checkStoredOffset(t);
    }

    TEST_CASE("view: integer zoom covers whole pixels (37x23 in 74x80)") {
        const auto t = ViewTransform::fit({74, 80}, {37, 23});
        CHECK(t.zoom() == 2.0);
        CHECK(t.origin().x == 0.0);
        CHECK(t.origin().y == -8.5);
        for (int px = 0; px < 74; ++px) {
            CHECK(column(t, px) == px / 2);
        }
        for (int py = 0; py < 80; ++py) {
            if (py < 17 || py >= 63) {
                CHECK((row(t, py) < 0 || row(t, py) >= 23));
            } else {
                CHECK(row(t, py) == (py - 17) / 2);
            }
        }
    }

    TEST_CASE("view: an odd remainder is floored (10x5 in 30x20)") {
        const auto t = ViewTransform::fit({30, 20}, {10, 5});
        CHECK(t.zoom() == 3.0);
        CHECK(t.origin().y == -2.0 / 3.0);
        for (int py = 0; py < 20; ++py) {
            if (py >= 2 && py < 17) {
                CHECK(row(t, py) == (py - 2) / 3);
            } else {
                CHECK((row(t, py) < 0 || row(t, py) >= 5));
            }
        }
        for (int px = 0; px < 30; ++px) {
            CHECK(column(t, px) == px / 3);
        }
    }

    TEST_CASE("view: 64-bit products (70000x70000 in 70000x1)") {
        const auto t = ViewTransform::fit({70000, 1}, {70000, 70000});
        CHECK(t.zoom() == 1.0 / 70000.0);
        CHECK(t.origin().y == 0.0);
        CHECK(t.origin().x == -34999.0 / (1.0 / 70000.0));
    }

    TEST_CASE("view: empty sizes") {
        const ViewTransform none{};
        CHECK(none.zoom() == 0.0);
        CHECK(none.origin() == PointD{0, 0});
        CHECK(none.viewport() == PixelSize{0, 0});
        CHECK(none.image() == PixelSize{0, 0});
        const PixelSize image{37, 23};
        for (const PixelSize viewport : {PixelSize{0, 0}, PixelSize{211, 0}}) {
            const auto t = ViewTransform::fit(viewport, image);
            CHECK(t.zoom() == 0.0);
            CHECK(t.origin() == PointD{0, 0});
            CHECK(t.viewport() == viewport);
            CHECK(t.image() == image);
        }
        const auto t = ViewTransform::fit({211, 97}, {0, 23});
        CHECK(t.zoom() == 0.0);
        CHECK(t.origin() == PointD{0, 0});
        CHECK(t.viewport() == PixelSize{211, 97});
        CHECK(t.image() == PixelSize{0, 23});
    }

    // A view without an image is always Fit.
    TEST_CASE("view: zoomed with an empty size or a zoom <= 0 is Fit at zoom 0") {
        for (const double zoom : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
            CAPTURE(zoom);
            const auto t = ViewTransform::zoomed(VP, IMG, zoom, {200, 150});
            CHECK(t.zoom() == 0.0);
            CHECK(t.origin() == PointD{0, 0});
            CHECK(t.offset() == PixelOffset{});
            CHECK(t.mode() == ViewMode::Fit);
            CHECK(t.viewport() == VP);
            CHECK(t.image() == IMG);
        }
        for (const auto& [viewport, image] :
             {std::pair{PixelSize{0, 0}, IMG}, std::pair{VP, PixelSize{0, 23}}}) {
            const auto t = ViewTransform::zoomed(viewport, image, 2, {200, 150});
            CHECK(t.zoom() == 0.0);
            CHECK(t.origin() == PointD{0, 0});
            CHECK(t.offset() == PixelOffset{});
            CHECK(t.mode() == ViewMode::Fit);
        }
    }

    TEST_CASE("resolve: zoom with overflow on both axes") {
        const auto t = resolveView({ViewMode::Zoom, 1, {200, 150}, IMG}, VP, IMG);
        CHECK(t.zoom() == 1);
        CHECK(t.origin() == PointD{100, 100});
        CHECK(t.mode() == ViewMode::Zoom);
    }

    TEST_CASE("resolve: the clamp at each edge") {
        CHECK(resolveView({ViewMode::Zoom, 1, {0, 0}, IMG}, VP, IMG).origin() == PointD{0, 0});
        CHECK(resolveView({ViewMode::Zoom, 1, {400, 300}, IMG}, VP, IMG).origin() ==
              PointD{200, 200});
    }

    TEST_CASE("resolve: no overflow is centred") {
        const PixelSize img{50, 40};
        CHECK(resolveView({ViewMode::Zoom, 2, {0, 0}, img}, VP, img).origin() == PointD{-25, -5});
    }

    TEST_CASE("resolve: overflow on one axis") {
        const PixelSize img{80, 60};
        CHECK(resolveView({ViewMode::Zoom, 2, {40, 30}, img}, VP, img).origin() == PointD{-10, 5});
    }

    TEST_CASE("resolve: whole-pixel offsets") {
        const PixelSize vp{201, 101};
        const PixelSize img{100, 50};
        const auto t = resolveView({ViewMode::Zoom, 3, {50.2, 25.1}, img}, vp, img);
        CHECK(offsetX(t) == doctest::Approx(-50));
        CHECK(offsetY(t) == doctest::Approx(-25));
        checkStoredOffset(t);
        checkStoredOffset(resolveView({ViewMode::Zoom, 2, {40, 30}, {80, 60}}, VP, {80, 60}));
    }

    TEST_CASE("resolve: a request for another image size is Fit") {
        const PixelSize drawn{401, 300};
        const auto t = resolveView({ViewMode::Zoom, 1, {200, 150}, IMG}, VP, drawn);
        CHECK(t == ViewTransform::fit(VP, drawn));
        CHECK(t.mode() == ViewMode::Fit);
    }

    TEST_CASE("resolve: Fit and empty sizes") {
        CHECK(resolveView(ViewRequest{}, VP, IMG) == ViewTransform::fit(VP, IMG));
        const auto t = resolveView({ViewMode::Zoom, 1, {200, 150}, IMG}, {0, 0}, IMG);
        CHECK(t.zoom() == 0);
        CHECK(t.origin() == PointD{0, 0});
        CHECK(t.mode() == ViewMode::Fit);
    }

    TEST_CASE("imagePointAt inverts resolve") {
        const auto t = resolveView({ViewMode::Zoom, 1, {200, 150}, IMG}, VP, IMG);
        CHECK(imagePointAt(t, {100, 50}) == PointD{200, 150});
        CHECK(imagePointAt(t, {0, 0}) == t.origin());
    }

    TEST_CASE("zoomAround keeps the anchor's image point") {
        const ViewRequest r{ViewMode::Zoom, 1, {200, 150}, IMG};
        const auto z = zoomAround(r, VP, IMG, 2, {50, 25});
        CHECK(z.zoom == 2);
        CHECK(z.centre == PointD{175, 137.5});
        const auto t = resolveView(z, VP, IMG);
        CHECK(imagePointAt(t, {50, 25}) == PointD{150, 125});
        const auto c = resolveView(zoomAround(r, VP, IMG, 2, {100, 50}), VP, IMG);
        CHECK(imagePointAt(c, {100, 50}) == PointD{200, 150});
    }

    TEST_CASE("zoomAround from Fit and at the edges") {
        const auto z = zoomAround(ViewRequest{}, VP, IMG, 0.5, {100, 50});
        CHECK(z.mode == ViewMode::Zoom);
        CHECK(z.zoom == 0.5);
        CHECK(z.centre == PointD{200, 150});
        CHECK(z.image == IMG);
        CHECK(zoomAround({ViewMode::Zoom, 2, {60, 40}, IMG}, VP, IMG, 1, {200, 100}).centre ==
              PointD{100, 50});
    }

    TEST_CASE("zoomAround from a request for another image size starts from the fit") {
        // The shown image is IMG, so the request resolves as Fit (zoom 1/3, origin (-99, 0)).
        const ViewRequest other{ViewMode::Zoom, 4, {10, 10}, {401, 300}};
        const auto z = zoomAround(other, VP, IMG, 1, {100, 50});
        CHECK(z == zoomAround(ViewRequest{}, VP, IMG, 1, {100, 50}));
        CHECK(z.mode == ViewMode::Zoom);
        CHECK(z.image == IMG);
        CHECK(z.centre == PointD{201, 150});
    }

    TEST_CASE("zoomAround with a resolved zoom of 0 keeps the request's centre") {
        const ViewRequest r{ViewMode::Zoom, 2, {-5, 999}, IMG};
        const auto z = zoomAround(r, {0, 0}, IMG, 4, {0, 0});
        CHECK(z.zoom == 4);
        CHECK(z.centre == PointD{-5, 999});
    }

    TEST_CASE("panBy leaves a request for another image size or a zoom of 0") {
        const ViewRequest other{ViewMode::Zoom, 1, {200, 150}, {401, 300}};
        CHECK(panBy(other, VP, IMG, 30, -20) == other);
        const ViewRequest none{ViewMode::Zoom, 0, {200, 150}, IMG};
        CHECK(panBy(none, VP, IMG, 30, -20) == none);
    }

    TEST_CASE("panBy moves the image and clamps") {
        const ViewRequest r{ViewMode::Zoom, 1, {200, 150}, IMG};
        CHECK(panBy(r, VP, IMG, 30, -20).centre == PointD{170, 170});
        CHECK(panBy(r, VP, IMG, 1000, 0).centre.x == 100);
        const PixelSize narrow{150, 300};
        const ViewRequest n{ViewMode::Zoom, 1, {75, 150}, narrow};
        CHECK(panBy(n, VP, narrow, 30, 0).centre.x == 75);
        CHECK(panBy(ViewRequest{}, VP, IMG, 30, 0) == ViewRequest{});
    }

    TEST_CASE("canPan") {
        CHECK_FALSE(canPan(ViewRequest{}, VP, IMG));
        CHECK(canPan({ViewMode::Zoom, 1, {200, 150}, IMG}, VP, IMG));
        const PixelSize small{50, 40};
        CHECK_FALSE(canPan({ViewMode::Zoom, 2, {25, 20}, small}, VP, small));
        CHECK_FALSE(canPan({ViewMode::Zoom, 1, {200, 150}, {401, 300}}, VP, IMG));
    }

    TEST_CASE("view: offset is the stored whole-pixel offset") {
        CHECK(ViewTransform::fit({211, 97}, {37, 23}).offset() ==
              PixelOffset{.x = 27, .y = 0});  // pillarbox
        CHECK(ViewTransform::fit({64, 64}, {32, 16}).offset() ==
              PixelOffset{.x = 0, .y = 16});  // letterbox
        const ViewTransform z = ViewTransform::zoomed({200, 100}, {400, 300}, 2, {.x = 0, .y = 0});
        CHECK(z.offset() == PixelOffset{.x = 0, .y = 0});
        const ViewTransform far =
            ViewTransform::zoomed({200, 100}, {400, 300}, 2, {.x = 400, .y = 300});
        CHECK(far.offset() == PixelOffset{.x = -600, .y = -500});  // ceil(W - s) clamp
        checkStoredOffset(far);
        const ViewTransform centred =
            ViewTransform::zoomed({200, 100}, {50, 40}, 2, {.x = 25, .y = 20});
        CHECK(centred.offset() == PixelOffset{.x = 50, .y = 10});  // floor((W - s) / 2)
        checkStoredOffset(centred);
        CHECK(ViewTransform{}.offset() == PixelOffset{});
    }

    // Without the saturation these convert -inf, NaN or about -2e302 to int64: undefined
    // behaviour that -fsanitize=float-cast-overflow (part of -fsanitize=undefined) reports.
    TEST_CASE("view: zoomed saturates the offset of an infinite or huge zoom and a NaN centre") {
        constexpr std::int64_t LIMIT = 0x4000000000000000;  // 2^62
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK(ViewTransform::zoomed(VP, IMG, inf, {.x = 1, .y = 1}).offset() ==
              PixelOffset{.x = -LIMIT, .y = -LIMIT});
        CHECK(ViewTransform::zoomed(VP, IMG, 1e300, {.x = 200, .y = 150}).offset() ==
              PixelOffset{.x = -LIMIT, .y = -LIMIT});
        CHECK(ViewTransform::zoomed(VP, IMG, 2, {.x = nan, .y = 150}).offset() ==
              PixelOffset{.x = 0, .y = -250});
    }

    TEST_CASE("view: imagePixelAt") {
        const ViewTransform t =
            ViewTransform::zoomed({200, 100}, {400, 300}, 2, {.x = 200, .y = 150});
        CHECK(imagePixelAt(t, {.x = 0.5, .y = 0.5}) == ImagePixel{.x = 150, .y = 125});
        CHECK(imagePixelAt(ViewTransform::fit({64, 64}, {32, 16}), {.x = 1, .y = 1}) ==
              std::nullopt);  // letterbox border
        CHECK(imagePixelAt(ViewTransform{}, {.x = 1, .y = 1}) == std::nullopt);
    }

    // fit({64, 64}, {32, 16}): zoom 2, image point (0, 0) at viewport pixel (0, 16).
    TEST_CASE("view: imagePixelAt floors and excludes the right and bottom edge") {
        const ViewTransform t = ViewTransform::fit({64, 64}, {32, 16});
        CHECK(imagePixelAt(t, {.x = 0, .y = 16}) == ImagePixel{.x = 0, .y = 0});
        CHECK(imagePixelAt(t, {.x = -0.5, .y = 20}) == std::nullopt);  // x = -0.25: floor, not 0
        CHECK(imagePixelAt(t, {.x = 63.998, .y = 47.998}) == ImagePixel{.x = 31, .y = 15});
        CHECK(imagePixelAt(t, {.x = 64, .y = 20}) == std::nullopt);  // x == width
        CHECK(imagePixelAt(t, {.x = 20, .y = 48}) == std::nullopt);  // y == height
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CHECK(imagePixelAt(t, {.x = nan, .y = 20}) == std::nullopt);
        CHECK(imagePixelAt(t, {.x = 20, .y = nan}) == std::nullopt);
    }
}
