/// @file invalidation.cpp
/// @brief Implementation of InvalidationController for incremental Lottie dirty-region tracking.

#include "nisaba/lottie/invalidation.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::lottie {

namespace {

std::optional<ScreenIntRect> rect_to_screen_clamped(
    const Rect& r,
    uint32_t surface_width,
    uint32_t surface_height,
    const Transform& view_transform
) noexcept {
    if (surface_width == 0 || surface_height == 0) return std::nullopt;
    auto mapped = view_transform.is_identity() ? std::optional<Rect>(r) : r.transform(view_transform);
    if (!mapped.has_value()) return std::nullopt;

    const int32_t sw = static_cast<int32_t>(surface_width);
    const int32_t sh = static_cast<int32_t>(surface_height);
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(mapped->left())), 0, sw);
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(mapped->top())), 0, sh);
    const int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(mapped->right())), 0, sw);
    const int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(mapped->bottom())), 0, sh);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;

    return ScreenIntRect::from_xywh(
        static_cast<uint32_t>(x0),
        static_cast<uint32_t>(y0),
        static_cast<uint32_t>(x1 - x0),
        static_cast<uint32_t>(y1 - y0)
    );
}

} // anonymous namespace

void InvalidationController::inval(const Rect& rect, const Transform& ctm) {
    if (rect.is_empty()) return;

    std::optional<Rect> mapped = ctm.is_identity() ? std::optional<Rect>(rect) : rect.transform(ctm);
    if (!mapped.has_value() || mapped->is_empty()) return;

    rects_.push_back(*mapped);
    if (!bounds_.has_value()) {
        bounds_ = *mapped;
    } else if (auto joined = bounds_->join(*mapped)) {
        bounds_ = *joined;
    }
}

void InvalidationController::reset() noexcept {
    bounds_.reset();
    rects_.clear();
}

void InvalidationController::clamp_to(const Rect& clip_rect) noexcept {
    if (!bounds_.has_value()) return;
    bounds_ = bounds_->intersect(clip_rect);
    if (!bounds_.has_value() || bounds_->is_empty()) {
        bounds_.reset();
        rects_.clear();
        return;
    }

    size_t write_idx = 0;
    for (size_t i = 0; i < rects_.size(); ++i) {
        if (auto clipped = rects_[i].intersect(clip_rect); clipped && !clipped->is_empty()) {
            rects_[write_idx++] = *clipped;
        }
    }
    rects_.resize(write_idx);
}

std::optional<ScreenIntRect> InvalidationController::to_screen_int_rect(
    uint32_t surface_width,
    uint32_t surface_height,
    const Transform& view_transform
) const noexcept {
    if (!bounds_.has_value()) return std::nullopt;
    return rect_to_screen_clamped(*bounds_, surface_width, surface_height, view_transform);
}

std::vector<ScreenIntRect> InvalidationController::to_screen_int_rects(
    uint32_t surface_width,
    uint32_t surface_height,
    const Transform& view_transform
) const {
    std::vector<ScreenIntRect> out;
    out.reserve(rects_.size());
    for (const auto& r : rects_) {
        if (auto sr = rect_to_screen_clamped(r, surface_width, surface_height, view_transform)) {
            out.push_back(*sr);
        }
    }
    return out;
}

std::optional<ScreenIntRect> InvalidationController::to_screen_int_rect_fitted(
    uint32_t surface_width,
    uint32_t surface_height,
    float comp_width,
    float comp_height,
    const Rect& dest_rect,
    bool preserve_aspect_ratio
) const noexcept {
    if (!bounds_.has_value() || comp_width <= 0.0f || comp_height <= 0.0f ||
        dest_rect.width() <= 0.0f || dest_rect.height() <= 0.0f) {
        return std::nullopt;
    }

    const float sx = dest_rect.width() / comp_width;
    const float sy = dest_rect.height() / comp_height;
    Transform fit = Transform::identity();
    if (preserve_aspect_ratio) {
        const float s = std::min(sx, sy);
        const float ox = dest_rect.x() + (dest_rect.width() - comp_width * s) * 0.5f;
        const float oy = dest_rect.y() + (dest_rect.height() - comp_height * s) * 0.5f;
        fit = Transform::from_translate(ox, oy).pre_scale(s, s);
    } else {
        fit = Transform::from_translate(dest_rect.x(), dest_rect.y()).pre_scale(sx, sy);
    }

    return to_screen_int_rect(surface_width, surface_height, fit);
}

} // namespace nisaba::lottie

