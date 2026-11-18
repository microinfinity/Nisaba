#include "nisaba/raster/perspective_raster.hpp"
#include "nisaba/pipeline/simd.hpp"
#include <algorithm>
#include <cmath>
#include <array>
#include <vector>

namespace nisaba::raster {

namespace {

struct Mat3x3 {
    std::array<float, 9> m; // row-major: [0..2], [3..5], [6..8]

    std::optional<Mat3x3> invert() const noexcept {
        float a = m[0], b = m[1], c = m[2];
        float d = m[3], e = m[4], f = m[5];
        float g = m[6], h = m[7], i = m[8];

        float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (std::abs(det) < 1e-12f) {
            return std::nullopt;
        }

        float inv_det = 1.0f / det;
        return Mat3x3{{
            (e * i - f * h) * inv_det, (c * h - b * i) * inv_det, (b * f - c * e) * inv_det,
            (f * g - d * i) * inv_det, (a * i - c * g) * inv_det, (c * d - a * f) * inv_det,
            (d * h - e * g) * inv_det, (b * g - a * h) * inv_det, (a * e - b * d) * inv_det
        }};
    }
};

inline PremultipliedColorU8 sample_bilinear(const PixmapRef& src, float u, float v) noexcept {
    int32_t w = static_cast<int32_t>(src.width());
    int32_t h = static_cast<int32_t>(src.height());
    if (w <= 0 || h <= 0) return PremultipliedColorU8::TRANSPARENT;

    float fu = u - 0.5f;
    float fv = v - 0.5f;

    int32_t x0 = static_cast<int32_t>(std::floor(fu));
    int32_t y0 = static_cast<int32_t>(std::floor(fv));
    int32_t x1 = x0 + 1;
    int32_t y1 = y0 + 1;

    float fx = fu - static_cast<float>(x0);
    float fy = fv - static_cast<float>(y0);

    x0 = std::clamp(x0, 0, w - 1);
    x1 = std::clamp(x1, 0, w - 1);
    y0 = std::clamp(y0, 0, h - 1);
    y1 = std::clamp(y1, 0, h - 1);

    auto p00 = src.row(y0)[x0];
    auto p10 = src.row(y0)[x1];
    auto p01 = src.row(y1)[x0];
    auto p11 = src.row(y1)[x1];

    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx * fy;

    float r = p00.red() * w00 + p10.red() * w10 + p01.red() * w01 + p11.red() * w11;
    float g = p00.green() * w00 + p10.green() * w10 + p01.green() * w01 + p11.green() * w11;
    float b = p00.blue() * w00 + p10.blue() * w10 + p01.blue() * w01 + p11.blue() * w11;
    float a = p00.alpha() * w00 + p10.alpha() * w10 + p01.alpha() * w01 + p11.alpha() * w11;

    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::clamp(r + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(g + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(b + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(a + 0.5f, 0.0f, 255.0f))
    );
}

inline PremultipliedColorU8 blend_pixel(PremultipliedColorU8 dst, PremultipliedColorU8 src, BlendMode mode) noexcept {
    return simd::blend_pixel_by_mode(dst, src, mode);
}

void rasterize_projective_quad(
    PixmapMut& dst,
    const PixmapRef& src,
    Point p0, Point p1, Point p2, Point p3,
    const Mat3x3& inv_h,
    float opacity,
    BlendMode blend_mode
) {
    if (opacity <= 0.0f || src.is_empty() || dst.width() == 0 || dst.height() == 0) {
        return;
    }

    float min_y = std::min({p0.y, p1.y, p2.y, p3.y});
    float max_y = std::max({p0.y, p1.y, p2.y, p3.y});
    float min_x = std::min({p0.x, p1.x, p2.x, p3.x});
    float max_x = std::max({p0.x, p1.x, p2.x, p3.x});

    float dst_w = static_cast<float>(dst.width());
    float dst_h = static_cast<float>(dst.height());

    if (max_x < 0.0f || min_x >= dst_w || max_y < 0.0f || min_y >= dst_h) {
        return;
    }

    int32_t y_start = std::max(0, static_cast<int32_t>(std::floor(min_y)));
    int32_t y_end = std::min(static_cast<int32_t>(dst.height()) - 1, static_cast<int32_t>(std::ceil(max_y)));

    const std::array<std::pair<Point, Point>, 4> edges = {{
        {p0, p1},
        {p1, p2},
        {p2, p3},
        {p3, p0}
    }};

    for (int32_t y = y_start; y <= y_end; ++y) {
        float y_center = static_cast<float>(y) + 0.5f;

        float x_hits[4];
        int num_hits = 0;

        for (const auto& [a, b] : edges) {
            if ((a.y <= y_center && y_center < b.y) || (b.y <= y_center && y_center < a.y)) {
                float dy = b.y - a.y;
                if (std::abs(dy) > 1e-6f) {
                    float t = (y_center - a.y) / dy;
                    x_hits[num_hits++] = a.x + t * (b.x - a.x);
                }
            }
        }

        if (num_hits < 2) continue;

        float row_min_x = x_hits[0];
        float row_max_x = x_hits[0];
        for (int i = 1; i < num_hits; ++i) {
            row_min_x = std::min(row_min_x, x_hits[i]);
            row_max_x = std::max(row_max_x, x_hits[i]);
        }

        float clamped_min_x = std::clamp(row_min_x, 0.0f, dst_w);
        float clamped_max_x = std::clamp(row_max_x, 0.0f, dst_w);

        if (clamped_min_x >= clamped_max_x) continue;

        int32_t ix_start = std::max(0, static_cast<int32_t>(std::floor(clamped_min_x)));
        int32_t ix_end = std::min(static_cast<int32_t>(dst.width()) - 1, static_cast<int32_t>(std::ceil(clamped_max_x) - 1));

        PremultipliedColorU8* dst_row = dst.row(static_cast<size_t>(y));

        float sx0 = static_cast<float>(ix_start) + 0.5f;
        float u_accum = inv_h.m[0] * sx0 + inv_h.m[1] * y_center + inv_h.m[2];
        float v_accum = inv_h.m[3] * sx0 + inv_h.m[4] * y_center + inv_h.m[5];
        float q_accum = inv_h.m[6] * sx0 + inv_h.m[7] * y_center + inv_h.m[8];

        float du = inv_h.m[0];
        float dv = inv_h.m[3];
        float dq = inv_h.m[6];

        for (int32_t ix = ix_start; ix <= ix_end; ++ix) {
            if (std::abs(q_accum) > 1e-7f) {
                float inv_q = 1.0f / q_accum;
                float u = u_accum * inv_q;
                float v = v_accum * inv_q;

                // Subpixel antialiasing edge coverage
                float px_left = static_cast<float>(ix);
                float px_right = px_left + 1.0f;
                float cov_x = std::clamp(std::min(px_right, row_max_x) - std::max(px_left, row_min_x), 0.0f, 1.0f);

                if (cov_x > 0.0f) {
                    PremultipliedColorU8 sampled = sample_bilinear(src, u, v);
                    float final_alpha = opacity * cov_x;

                    if (final_alpha < 0.999f) {
                        sampled = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>(sampled.red()   * final_alpha + 0.5f),
                            static_cast<uint8_t>(sampled.green() * final_alpha + 0.5f),
                            static_cast<uint8_t>(sampled.blue()  * final_alpha + 0.5f),
                            static_cast<uint8_t>(sampled.alpha() * final_alpha + 0.5f)
                        );
                    }

                    dst_row[ix] = blend_pixel(dst_row[ix], sampled, blend_mode);
                }
            }

            u_accum += du;
            v_accum += dv;
            q_accum += dq;
        }
    }
}

} // namespace

void draw_pixmap_perspective_quad(
    PixmapMut dst,
    const PixmapRef& src,
    Point p0, Point p1, Point p2, Point p3,
    float opacity,
    BlendMode blend_mode
) {
    if (src.is_empty()) return;

    auto src_rect = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(src.width()), static_cast<float>(src.height()));
    if (!src_rect) return;

    auto h_opt = Transform4x4::from_rect_to_quad(*src_rect, p0, p1, p2, p3);
    if (!h_opt) return;

    Mat3x3 h{{
        h_opt->m[0],  h_opt->m[1],  h_opt->m[3],
        h_opt->m[4],  h_opt->m[5],  h_opt->m[7],
        h_opt->m[12], h_opt->m[13], h_opt->m[15]
    }};

    auto inv_h_opt = h.invert();
    if (!inv_h_opt) return;

    rasterize_projective_quad(dst, src, p0, p1, p2, p3, *inv_h_opt, opacity, blend_mode);
}

void draw_pixmap_3d(
    PixmapMut dst,
    const PixmapRef& src,
    const Rect& src_rect,
    const Transform4x4& transform3d,
    float opacity,
    BlendMode blend_mode
) {
    if (src.is_empty()) return;

    auto p0_opt = transform3d.map_point_checked(Point(src_rect.left(), src_rect.top()));
    auto p1_opt = transform3d.map_point_checked(Point(src_rect.right(), src_rect.top()));
    auto p2_opt = transform3d.map_point_checked(Point(src_rect.right(), src_rect.bottom()));
    auto p3_opt = transform3d.map_point_checked(Point(src_rect.left(), src_rect.bottom()));

    if (!p0_opt || !p1_opt || !p2_opt || !p3_opt) {
        return; // Clipped by camera near plane (behind camera)
    }

    Mat3x3 h{{
        transform3d.m[0],  transform3d.m[1],  transform3d.m[3],
        transform3d.m[4],  transform3d.m[5],  transform3d.m[7],
        transform3d.m[12], transform3d.m[13], transform3d.m[15]
    }};

    auto inv_h_opt = h.invert();
    if (!inv_h_opt) return;

    rasterize_projective_quad(dst, src, *p0_opt, *p1_opt, *p2_opt, *p3_opt, *inv_h_opt, opacity, blend_mode);
}

} // namespace nisaba::raster
