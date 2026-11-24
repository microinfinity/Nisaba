#pragma once

#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/canvas/mask.hpp"
#include "nisaba/mesh/mesh.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include <optional>

namespace nisaba::raster {

/// Renders a 2D mesh of colored and/or textured vertices onto the destination surface
/// with Gouraud shading, bilinear texture sampling, and subpixel edge anti-aliasing.
void draw_vertices(
    PixmapMut dst,
    const Vertices& vertices,
    const PixmapRef* texture = nullptr,
    float opacity = 1.0f,
    BlendMode blend_mode = BlendMode::SourceOver,
    Transform transform = Transform(),
    const Mask* mask = nullptr,
    std::optional<ScreenIntRect> clip = std::nullopt
);

/// Renders a single triangle with per-vertex colors (Gouraud interpolation).
void draw_colored_triangle(
    PixmapMut dst,
    Point p0, Point p1, Point p2,
    Color c0, Color c1, Color c2,
    float opacity = 1.0f,
    BlendMode blend_mode = BlendMode::SourceOver
);

/// Renders a single textured triangle with UV mapping and bilinear filtering.
void draw_textured_triangle(
    PixmapMut dst,
    const PixmapRef& texture,
    Point p0, Point p1, Point p2,
    Point t0, Point t1, Point t2,
    const Color* c0 = nullptr, const Color* c1 = nullptr, const Color* c2 = nullptr,
    float opacity = 1.0f,
    BlendMode blend_mode = BlendMode::SourceOver
);

} // namespace nisaba::raster
