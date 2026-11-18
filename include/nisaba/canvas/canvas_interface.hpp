#pragma once

/// @file canvas_interface.hpp
/// @brief Sovereign 2D vector drawing interface shared by CPU Canvas and GPU GpuCanvas.

#include "nisaba/math/transform.hpp"
#include "nisaba/math/transform4x4.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/border_radius.hpp"
#include "nisaba/math/rrect.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/effects/drop_shadow.hpp"
#include <algorithm>
#include <span>

namespace nisaba {

namespace detail {

[[nodiscard]] inline std::optional<ScreenIntRect> clip_layer_bounds(
    const std::optional<Rect>& local_bounds,
    const Transform& ts,
    uint32_t canvas_w,
    uint32_t canvas_h,
    const std::optional<ScreenIntRect>& scissor_clip
) noexcept {
    int32_t cw = static_cast<int32_t>(canvas_w);
    int32_t ch = static_cast<int32_t>(canvas_h);
    if (cw <= 0 || ch <= 0) return std::nullopt;

    std::optional<ScreenIntRect> result = scissor_clip;
    if (!result.has_value()) {
        result = ScreenIntRect::from_xywh(0, 0, canvas_w, canvas_h);
    }
    if (!result.has_value()) return std::nullopt;

    if (local_bounds.has_value()) {
        auto tr = local_bounds->transform(ts);
        if (!tr.has_value()) return std::nullopt;
        int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(tr->left())), 0, cw);
        int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(tr->top())), 0, ch);
        int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(tr->right())), 0, cw);
        int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(tr->bottom())), 0, ch);
        if (x1 <= x0 || y1 <= y0) return std::nullopt;
        auto dev = ScreenIntRect::from_xywh(
            static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
            static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)
        );
        if (!dev.has_value()) return std::nullopt;
        result = result->intersect(*dev);
    }
    return result;
}

} // namespace detail

/// @class ICanvas
/// @brief Abstract 2D vector drawing interface shared by software (Canvas) and hardware (GpuCanvas).
class ICanvas {
public:
    virtual ~ICanvas() = default;

    [[nodiscard]] virtual uint32_t canvas_width() const noexcept { return 0; }
    [[nodiscard]] virtual uint32_t canvas_height() const noexcept { return 0; }

    virtual void save() = 0;
    virtual void save_layer(const Paint& paint = Paint(Color::WHITE)) {
        (void)paint;
        save();
    }
    virtual void save_layer_bounds(const Rect& bounds, const Paint& paint = Paint(Color::WHITE)) {
        save_layer(paint);
        clip_rect(bounds);
    }
    virtual void save_layer_backdrop(
        std::optional<Rect> bounds = std::nullopt,
        const Paint& paint = Paint(Color::WHITE),
        float blur_sigma = 0.0f
    ) {
        (void)blur_sigma;
        if (bounds.has_value()) {
            save_layer_bounds(*bounds, paint);
        } else {
            save_layer(paint);
        }
    }
    void save_layer_alpha(float alpha) {
        Paint p(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)));
        save_layer(p);
    }
    void save_layer_alpha(const Rect& bounds, float alpha) {
        Paint p(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)));
        save_layer_bounds(bounds, p);
    }
    virtual void restore() = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotate(float degrees) {
        concat(Transform::from_rotate(degrees));
    }
    virtual void skew(float kx, float ky) {
        concat(Transform::from_skew(kx, ky));
    }
    virtual void concat(const Transform& ts) = 0;
    [[nodiscard]] virtual Transform current_transform() const noexcept { return Transform::identity(); }

    virtual void fill_rect(const Rect& rect, const Paint& paint) = 0;
    virtual void stroke_rect(const Rect& rect, const Paint& paint, const Stroke& stroke) {
        auto p = PathBuilder::from_rect(rect);
        if (!p.is_empty()) stroke_path(p, paint, stroke);
    }
    virtual void fill_path(const Path& path, const Paint& paint, FillRule fill_rule = FillRule::Winding) = 0;
    virtual void stroke_path(const Path& path, const Paint& paint, const Stroke& stroke) = 0;

    virtual void clip_path(const Path& path, FillRule fill_rule = FillRule::Winding) {
        (void)path; (void)fill_rule;
    }
    virtual void clip_rect(const Rect& rect) {
        if (auto p = PathBuilder::from_rect(rect); !p.is_empty()) {
            clip_path(p, FillRule::Winding);
        }
    }

    virtual void draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint = PixmapPaint()) {
        (void)x; (void)y; (void)src; (void)paint;
    }

    virtual void draw_pixmap_rect(
        const PixmapRef& src,
        const Rect& src_rect,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        (void)src; (void)src_rect; (void)dst_rect; (void)paint;
    }

    void draw_pixmap_rect(
        const PixmapRef& src,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        if (auto sr = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(src.width()), static_cast<float>(src.height()))) {
            draw_pixmap_rect(src, *sr, dst_rect, paint);
        }
    }

    virtual void draw_pixmap_3d(
        const PixmapRef& src,
        const Transform4x4& transform3d,
        float opacity = 1.0f,
        BlendMode blend_mode = BlendMode::SourceOver
    ) {
        (void)src; (void)transform3d; (void)opacity; (void)blend_mode;
    }

    virtual void fill_round_rect(const Rect& rect, float rx, float ry, const Paint& paint) {
        if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
            fill_path(*p, paint);
        }
    }
    virtual void stroke_round_rect(const Rect& rect, float rx, float ry, const Paint& paint, const Stroke& stroke) {
        if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
            stroke_path(*p, paint, stroke);
        }
    }

    virtual void fill_rrect(const RRect& rrect, const Paint& paint) {
        fill_path(rrect.to_path(), paint);
    }
    virtual void stroke_rrect(const RRect& rrect, const Paint& paint, const Stroke& stroke) {
        stroke_path(rrect.to_path(), paint, stroke);
    }
    virtual void clip_rrect(const RRect& rrect) {
        clip_path(rrect.to_path(), FillRule::Winding);
    }

    virtual void fill_oval(const Rect& oval, const Paint& paint) {
        if (auto p = PathBuilder::from_oval(oval)) {
            fill_path(*p, paint);
        }
    }
    virtual void stroke_oval(const Rect& oval, const Paint& paint, const Stroke& stroke) {
        if (auto p = PathBuilder::from_oval(oval)) {
            stroke_path(*p, paint, stroke);
        }
    }
    virtual void clip_oval(const Rect& oval) {
        if (auto p = PathBuilder::from_oval(oval)) {
            clip_path(*p, FillRule::Winding);
        }
    }

    virtual void fill_arc(const Rect& oval, float start_deg, float sweep_deg, bool use_center, const Paint& paint) {
        PathBuilder pb;
        pb.push_arc(oval, start_deg, sweep_deg, use_center, true);
        if (auto p = pb.finish(); p && !p->is_empty()) {
            fill_path(*p, paint);
        }
    }
    virtual void stroke_arc(
        const Rect& oval, float start_deg, float sweep_deg, bool use_center, const Paint& paint, const Stroke& stroke
    ) {
        PathBuilder pb;
        pb.push_arc(oval, start_deg, sweep_deg, use_center, true);
        if (auto p = pb.finish(); p && !p->is_empty()) {
            stroke_path(*p, paint, stroke);
        }
    }

    virtual void fill_drrect(const RRect& outer, const RRect& inner, const Paint& paint) {
        const auto po = outer.to_path();
        const auto pi = inner.to_path();
        if (auto diff = po.difference(pi)) {
            if (!diff->is_empty()) {
                fill_path(*diff, paint);
            }
        } else {
            fill_path(po, paint);
        }
    }
    virtual void stroke_drrect(const RRect& outer, const RRect& inner, const Paint& paint, const Stroke& stroke) {
        stroke_rrect(outer, paint, stroke);
        stroke_rrect(inner, paint, stroke);
    }

    virtual void draw_points(
        PointMode mode,
        std::span<const Point> points,
        const Paint& paint,
        const Stroke& stroke = Stroke{}
    ) {
        if (points.empty()) return;
        PathBuilder pb;
        switch (mode) {
            case PointMode::Points: {
                const float w = std::max(1.0f, stroke.width);
                const float r = w * 0.5f;
                const bool round = (stroke.line_cap == LineCap::Round);
                for (const auto& pt : points) {
                    if (auto rect = Rect::from_xywh(pt.x - r, pt.y - r, w, w)) {
                        if (round) pb.push_oval(*rect);
                        else pb.push_rect(*rect);
                    }
                }
                if (auto p = pb.finish(); p && !p->is_empty()) {
                    fill_path(*p, paint);
                }
                break;
            }
            case PointMode::Lines: {
                for (size_t i = 0; i + 1 < points.size(); i += 2) {
                    pb.move_to(points[i]);
                    pb.line_to(points[i + 1]);
                }
                if (auto p = pb.finish(); p && !p->is_empty()) {
                    stroke_path(*p, paint, stroke);
                }
                break;
            }
            case PointMode::Polygon: {
                if (points.size() < 2) return;
                pb.move_to(points[0]);
                for (size_t i = 1; i < points.size(); ++i) {
                    pb.line_to(points[i]);
                }
                if (auto p = pb.finish(); p && !p->is_empty()) {
                    stroke_path(*p, paint, stroke);
                }
                break;
            }
        }
    }

    // --- Paint::style Dispatchers ---
    void draw_path(
        const Path& path,
        const Paint& paint,
        const Stroke& stroke = Stroke{},
        FillRule fill_rule = FillRule::Winding
    ) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_path(path, paint, fill_rule);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_path(path, paint, stroke);
        }
    }

    void draw_rect(const Rect& rect, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_rect(rect, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_rect(rect, paint, stroke);
        }
    }

    void draw_round_rect(const Rect& rect, float rx, float ry, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_round_rect(rect, rx, ry, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_round_rect(rect, rx, ry, paint, stroke);
        }
    }

    void draw_oval(const Rect& oval, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_oval(oval, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_oval(oval, paint, stroke);
        }
    }

    void draw_arc(
        const Rect& oval,
        float start_deg,
        float sweep_deg,
        bool use_center,
        const Paint& paint,
        const Stroke& stroke = Stroke{}
    ) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_arc(oval, start_deg, sweep_deg, use_center, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_arc(oval, start_deg, sweep_deg, use_center, paint, stroke);
        }
    }

    void draw_rrect(const RRect& rrect, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_rrect(rrect, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_rrect(rrect, paint, stroke);
        }
    }

    void draw_drrect(const RRect& outer, const RRect& inner, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_drrect(outer, inner, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_drrect(outer, inner, paint, stroke);
        }
    }

    // --- Shadows & Effects ---
    virtual void draw_path_with_shadow(
        const Path& path,
        const Paint& paint,
        const effects::DropShadow& shadow,
        FillRule fill_rule = FillRule::Winding
    ) {
        (void)shadow;
        fill_path(path, paint, fill_rule);
    }

    virtual void draw_rect_with_shadow(
        const Rect& rect,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        fill_rect(rect, paint);
    }

    virtual void draw_round_rect_with_shadow(
        const Rect& rect,
        float rx,
        float ry,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        fill_round_rect(rect, rx, ry, paint);
    }

    virtual void draw_rrect_with_shadow(
        const RRect& rrect,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        fill_rrect(rrect, paint);
    }

    virtual void draw_circle_with_shadow(
        float cx,
        float cy,
        float radius,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        if (auto p = PathBuilder::from_circle(cx, cy, radius)) {
            fill_path(*p, paint);
        }
    }
};

} // namespace nisaba
