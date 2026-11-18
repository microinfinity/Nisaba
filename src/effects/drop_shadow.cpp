#include "nisaba/effects/drop_shadow.hpp"
#include "nisaba/effects/blur.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/pipeline/simd.hpp"
#include <cmath>
#include <algorithm>

namespace nisaba::effects {

void draw_shadow_mask(
    PixmapMut& dst,
    int32_t x,
    int32_t y,
    const MaskRef& mask,
    Color color,
    const Mask* clip_mask
) {
    if (color.alpha_norm() <= 0.0f) return;

    PremultipliedColorU8 sc = color.premultiply().to_color_u8();
    int32_t mw = static_cast<int32_t>(mask.width());
    int32_t mh = static_cast<int32_t>(mask.height());
    int32_t dw = static_cast<int32_t>(dst.width());
    int32_t dh = static_cast<int32_t>(dst.height());

    int32_t y0 = std::max(0, y);
    int32_t y1 = std::min(dh, y + mh);
    int32_t x0 = std::max(0, x);
    int32_t x1 = std::min(dw, x + mw);
    if (x1 <= x0 || y1 <= y0) return;

    size_t span_len = static_cast<size_t>(x1 - x0);
    int32_t mx_offset = x0 - x;

    if (clip_mask == nullptr) {
        for (int32_t dy = y0; dy < y1; ++dy) {
            int32_t my = dy - y;
            const uint8_t* mask_row = mask.data + my * mask.real_width + mx_offset;
            auto* dst_row = dst.row(static_cast<size_t>(dy)) + x0;
            simd::blend_solid_mask_span(dst_row, sc, mask_row, span_len);
        }
    } else {
        for (int32_t dy = y0; dy < y1; ++dy) {
            int32_t my = dy - y;
            const uint8_t* mask_row = mask.data + my * mask.real_width + mx_offset;
            auto* dst_row = dst.row(static_cast<size_t>(dy)) + x0;
            for (size_t i = 0; i < span_len; ++i) {
                uint8_t alpha = mask_row[i];
                if (alpha == 0) continue;
                int32_t dx = x0 + static_cast<int32_t>(i);
                uint8_t clip_cov = clip_mask->get(static_cast<uint32_t>(dx), static_cast<uint32_t>(dy));
                if (clip_cov == 0) continue;
                uint8_t cov_u8 = static_cast<uint8_t>((static_cast<uint32_t>(alpha) * clip_cov + 128u) / 255u);
                simd::blend_solid_source_over_coverage(dst_row + i, sc, cov_u8, 1);
            }
        }
    }
}

void draw_path_shadow(
    PixmapMut& dst,
    const Path& path,
    const DropShadow& shadow,
    FillRule fill_rule,
    Transform transform,
    const Mask* clip_mask
) {
    if (path.is_empty() || shadow.color.alpha_norm() <= 0.0f) return;

    const Path* p = &path;
    std::optional<Path> transformed;
    if (!transform.is_identity()) {
        transformed = path.transform(transform);
        if (!transformed) return;
        p = &*transformed;
    }

    auto bounds = p->bounds();
    if (bounds.width() <= 0.0f || bounds.height() <= 0.0f) return;

    int32_t pad = static_cast<int32_t>(std::ceil(shadow.sigma * 3.0f)) + 2;
    int32_t bx = static_cast<int32_t>(std::floor(bounds.x()));
    int32_t by = static_cast<int32_t>(std::floor(bounds.y()));
    int32_t bw = static_cast<int32_t>(std::ceil(bounds.right())) - bx;
    int32_t bh = static_cast<int32_t>(std::ceil(bounds.bottom())) - by;

    uint32_t mask_w = static_cast<uint32_t>(bw + 2 * pad);
    uint32_t mask_h = static_cast<uint32_t>(bh + 2 * pad);

    auto m = Mask::allocate(mask_w, mask_h);
    if (!m) return;

    Transform local_ts = Transform::from_translate(static_cast<float>(pad - bx), static_cast<float>(pad - by));
    m->fill_path(*p, fill_rule, true, local_ts);

    gaussian_blur_mask(*m, shadow.sigma);

    int32_t dst_x = bx - pad + static_cast<int32_t>(std::round(shadow.dx));
    int32_t dst_y = by - pad + static_cast<int32_t>(std::round(shadow.dy));

    draw_shadow_mask(dst, dst_x, dst_y, m->as_submask(), shadow.color, clip_mask);
}

void draw_rect_shadow(
    PixmapMut& dst,
    const Rect& rect,
    const DropShadow& shadow,
    Transform transform,
    const Mask* clip_mask
) {
    auto p = PathBuilder::from_rect(rect);
    draw_path_shadow(dst, p, shadow, FillRule::Winding, transform, clip_mask);
}

void draw_round_rect_shadow(
    PixmapMut& dst,
    const Rect& rect,
    float rx,
    float ry,
    const DropShadow& shadow,
    Transform transform,
    const Mask* clip_mask
) {
    auto p = PathBuilder::from_rounded_rect(rect, rx, ry);
    if (p) {
        draw_path_shadow(dst, *p, shadow, FillRule::Winding, transform, clip_mask);
    }
}

void draw_rrect_shadow(
    PixmapMut& dst,
    const RRect& rrect,
    const DropShadow& shadow,
    Transform transform,
    const Mask* clip_mask
) {
    if (rrect.is_empty()) return;
    auto p = PathBuilder::from_rrect(rrect.constrained());
    if (p) {
        draw_path_shadow(dst, *p, shadow, FillRule::Winding, transform, clip_mask);
    }
}

void draw_circle_shadow(
    PixmapMut& dst,
    float cx,
    float cy,
    float radius,
    const DropShadow& shadow,
    Transform transform,
    const Mask* clip_mask
) {
    if (radius <= 0.0f) return;
    auto p = PathBuilder::from_circle(cx, cy, radius);
    if (p) {
        draw_path_shadow(dst, *p, shadow, FillRule::Winding, transform, clip_mask);
    }
}

} // namespace nisaba::effects
