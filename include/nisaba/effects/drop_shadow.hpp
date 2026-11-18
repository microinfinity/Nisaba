#pragma once

#include <cstdint>
#include <optional>
#include "nisaba/color/color.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/rrect.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/canvas/mask.hpp"
#include "nisaba/path/path.hpp"

namespace nisaba::effects {

/// Configuration defining an elevation, ambient, or directional drop shadow.
struct DropShadow {
    float dx{0.0f};                               ///< Horizontal offset
    float dy{4.0f};                               ///< Vertical offset
    float sigma{4.0f};                            ///< Gaussian blur standard deviation (blur radius)
    Color color{Color::from_rgba8(0, 0, 0, 120)}; ///< Shadow color including opacity

    constexpr DropShadow() noexcept = default;
    constexpr DropShadow(float dx, float dy, float sigma, Color color) noexcept
        : dx(dx), dy(dy), sigma(sigma), color(color) {}

    /// Preset for a subtle near-surface shadow (e.g., cards, chips).
    static constexpr DropShadow subtle(float dy = 2.0f, float sigma = 3.0f) noexcept {
        return DropShadow(0.0f, dy, sigma, Color::from_rgba8(0, 0, 0, 60));
    }

    /// Preset for a medium elevation shadow (e.g., dialogs, floating buttons).
    static constexpr DropShadow medium(float dy = 6.0f, float sigma = 8.0f) noexcept {
        return DropShadow(0.0f, dy, sigma, Color::from_rgba8(0, 0, 0, 100));
    }

    /// Preset for high elevation floating panels.
    static constexpr DropShadow elevated(float dy = 12.0f, float sigma = 16.0f) noexcept {
        return DropShadow(0.0f, dy, sigma, Color::from_rgba8(0, 0, 0, 130));
    }

    /// Preset for an omnidirectional glow / neon halo.
    static constexpr DropShadow glow(Color glow_color, float sigma = 8.0f) noexcept {
        return DropShadow(0.0f, 0.0f, sigma, glow_color);
    }
};

/// Composites an 8-bit alpha shadow mask onto dst at (x, y) tinted with shadow color.
void draw_shadow_mask(
    PixmapMut& dst,
    int32_t x,
    int32_t y,
    const MaskRef& mask,
    Color color,
    const Mask* clip_mask = nullptr
);

/// Renders a blurred drop shadow for an arbitrary vector path.
void draw_path_shadow(
    PixmapMut& dst,
    const Path& path,
    const DropShadow& shadow,
    FillRule fill_rule = FillRule::Winding,
    Transform transform = Transform(),
    const Mask* clip_mask = nullptr
);

/// Renders a blurred drop shadow for a rectangle.
void draw_rect_shadow(
    PixmapMut& dst,
    const Rect& rect,
    const DropShadow& shadow,
    Transform transform = Transform(),
    const Mask* clip_mask = nullptr
);

/// Renders a blurred drop shadow for a rounded rectangle.
void draw_round_rect_shadow(
    PixmapMut& dst,
    const Rect& rect,
    float rx,
    float ry,
    const DropShadow& shadow,
    Transform transform = Transform(),
    const Mask* clip_mask = nullptr
);

/// Renders a blurred drop shadow for an asymmetric 4-corner RRect.
void draw_rrect_shadow(
    PixmapMut& dst,
    const RRect& rrect,
    const DropShadow& shadow,
    Transform transform = Transform(),
    const Mask* clip_mask = nullptr
);

/// Renders a blurred drop shadow for a circle.
void draw_circle_shadow(
    PixmapMut& dst,
    float cx,
    float cy,
    float radius,
    const DropShadow& shadow,
    Transform transform = Transform(),
    const Mask* clip_mask = nullptr
);

} // namespace nisaba::effects
