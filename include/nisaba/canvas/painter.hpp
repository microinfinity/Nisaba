#pragma once

#include <optional>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/canvas/mask.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/math/transform.hpp"

namespace nisaba {

namespace painter {

void fill_rect(
    PixmapMut& dst,
    const Rect& rect,
    const Paint& paint,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void stroke_rect_axis_aligned(
    PixmapMut& dst,
    const Rect& rect,
    float stroke_width,
    const Paint& paint,
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void fill_convex_quad(
    PixmapMut& dst,
    Point p0, Point p1, Point p2, Point p3,
    const Paint& paint,
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void fill_path(
    PixmapMut& dst,
    const Path& path,
    const Paint& paint,
    FillRule fill_rule = FillRule::Winding,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void stroke_path(
    PixmapMut& dst,
    const Path& path,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void fill_circle(
    PixmapMut& dst,
    float cx, float cy, float radius,
    const Paint& paint,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void stroke_circle(
    PixmapMut& dst,
    float cx, float cy, float radius,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void fill_round_rect(
    PixmapMut& dst,
    const Rect& rect,
    float rx, float ry,
    const Paint& paint,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void stroke_round_rect(
    PixmapMut& dst,
    const Rect& rect,
    float rx, float ry,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void stroke_line(
    PixmapMut& dst,
    Point p0, Point p1,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void draw_pixmap(
    PixmapMut& dst,
    int32_t x,
    int32_t y,
    const PixmapRef& src,
    const PixmapPaint& paint = PixmapPaint(),
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void draw_pixmap_rect(
    PixmapMut& dst,
    const PixmapRef& src,
    const Rect& src_rect,
    const Rect& dst_rect,
    const PixmapPaint& paint = PixmapPaint(),
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

void apply_mask(
    PixmapMut& dst,
    const Mask& mask
);

} // namespace painter

} // namespace nisaba
