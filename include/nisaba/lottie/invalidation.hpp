#pragma once

/// @file invalidation.hpp
/// @brief Dirty-region tracking controller for incremental Lottie compositing and partial redraws.

#include "nisaba/math/rect.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/math/transform.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace nisaba::lottie {

/// Tracks minimal dirty bounding boxes and per-layer invalidated regions across animation frames.
/// Designed to feed directly into `GpuCanvas::flush(damage_rect)`, compositor damage rings,
/// and tile-based partial redraw pipelines.
class InvalidationController {
public:
    InvalidationController() = default;

    /// Invalidates `rect` transformed by `ctm` and merges it into the accumulated damage bounds.
    void inval(const Rect& rect, const Transform& ctm = Transform::identity());

    /// Resets all accumulated damage state.
    void reset() noexcept;

    /// Returns true if no region has been invalidated since construction or the last `reset()`.
    [[nodiscard]] bool is_empty() const noexcept { return !bounds_.has_value(); }

    /// Returns the minimal bounding box enclosing all invalidated regions in composition space.
    [[nodiscard]] std::optional<Rect> bounds() const noexcept { return bounds_; }

    /// Returns the individual invalidated rectangles (useful for multi-rect partial redraws).
    [[nodiscard]] const std::vector<Rect>& rects() const noexcept { return rects_; }

    /// Clamps the accumulated dirty bounds to the provided clipping rectangle (e.g. composition bounds).
    void clamp_to(const Rect& clip_rect) noexcept;

    /// Converts accumulated dirty bounds into a surface-clamped `ScreenIntRect`
    /// ready for `GpuCanvas::flush(damage_rect)`.
    [[nodiscard]] std::optional<ScreenIntRect> to_screen_int_rect(
        uint32_t surface_width,
        uint32_t surface_height,
        const Transform& view_transform = Transform::identity()
    ) const noexcept;

    /// Converts individual dirty rectangles into surface-clamped `ScreenIntRect` entries
    /// ready for `GpuCanvas::flush(damage_rects)`.
    [[nodiscard]] std::vector<ScreenIntRect> to_screen_int_rects(
        uint32_t surface_width,
        uint32_t surface_height,
        const Transform& view_transform = Transform::identity()
    ) const;

    /// Converts accumulated dirty bounds using the exact same fitted destination mapping as
    /// `Animation::render(canvas, frame, dest_rect, preserve_aspect_ratio)`.
    [[nodiscard]] std::optional<ScreenIntRect> to_screen_int_rect_fitted(
        uint32_t surface_width,
        uint32_t surface_height,
        float comp_width,
        float comp_height,
        const Rect& dest_rect,
        bool preserve_aspect_ratio = true
    ) const noexcept;

private:
    std::optional<Rect> bounds_;
    std::vector<Rect> rects_;
};

} // namespace nisaba::lottie

