#include "nisaba/raster/mesh_raster.hpp"
#include "nisaba/pipeline/simd.hpp"
#include <algorithm>
#include <cmath>
#include <array>

namespace nisaba::raster {

namespace {

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

void rasterize_triangle_internal(
    PixmapMut dst,
    Point p0, Point p1, Point p2,
    const Color* c0, const Color* c1, const Color* c2,
    const Point* uv0, const Point* uv1, const Point* uv2,
    const PixmapRef* texture,
    float opacity,
    BlendMode blend_mode
) {
    if (opacity <= 0.0f || dst.width() == 0 || dst.height() == 0) return;

    float dst_w = static_cast<float>(dst.width());
    float dst_h = static_cast<float>(dst.height());

    float min_x = std::min({p0.x, p1.x, p2.x});
    float max_x = std::max({p0.x, p1.x, p2.x});
    float min_y = std::min({p0.y, p1.y, p2.y});
    float max_y = std::max({p0.y, p1.y, p2.y});

    if (max_x < 0.0f || min_x >= dst_w || max_y < 0.0f || min_y >= dst_h) {
        return;
    }

    // Signed double area of triangle (p0, p1, p2)
    float det = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::abs(det) < 1e-6f) return; // Degenerate collinear triangle

    float inv_det = 1.0f / det;

    // Partial derivatives of barycentric coordinates:
    // lambda1 = ((x - x0)*(y2 - y0) - (x2 - x0)*(y - y0)) * inv_det
    // lambda2 = ((x1 - x0)*(y - y0) - (x - x0)*(y1 - y0)) * inv_det
    // lambda0 = 1 - lambda1 - lambda2
    float dL1_dx = (p2.y - p0.y) * inv_det;
    float dL2_dx = -(p1.y - p0.y) * inv_det;
    float dL0_dx = -dL1_dx - dL2_dx;

    // Vertex color channels (premultiplied 0..255)
    bool has_color = (c0 != nullptr && c1 != nullptr && c2 != nullptr);
    float r0 = 255.0f, g0 = 255.0f, b0 = 255.0f, a0 = 255.0f;
    float r1 = 255.0f, g1 = 255.0f, b1 = 255.0f, a1 = 255.0f;
    float r2 = 255.0f, g2 = 255.0f, b2 = 255.0f, a2 = 255.0f;

    if (has_color) {
        r0 = c0->red() * c0->alpha() * 255.0f;
        g0 = c0->green() * c0->alpha() * 255.0f;
        b0 = c0->blue() * c0->alpha() * 255.0f;
        a0 = c0->alpha() * 255.0f;

        r1 = c1->red() * c1->alpha() * 255.0f;
        g1 = c1->green() * c1->alpha() * 255.0f;
        b1 = c1->blue() * c1->alpha() * 255.0f;
        a1 = c1->alpha() * 255.0f;

        r2 = c2->red() * c2->alpha() * 255.0f;
        g2 = c2->green() * c2->alpha() * 255.0f;
        b2 = c2->blue() * c2->alpha() * 255.0f;
        a2 = c2->alpha() * 255.0f;
    }

    float dr_dx = has_color ? (dL0_dx * r0 + dL1_dx * r1 + dL2_dx * r2) : 0.0f;
    float dg_dx = has_color ? (dL0_dx * g0 + dL1_dx * g1 + dL2_dx * g2) : 0.0f;
    float db_dx = has_color ? (dL0_dx * b0 + dL1_dx * b1 + dL2_dx * b2) : 0.0f;
    float da_dx = has_color ? (dL0_dx * a0 + dL1_dx * a1 + dL2_dx * a2) : 0.0f;

    // UV texture coordinates
    bool has_uv = (uv0 != nullptr && uv1 != nullptr && uv2 != nullptr && texture != nullptr && !texture->is_empty());
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f, u2 = 0.0f, v2 = 0.0f;
    float du_dx = 0.0f, dv_dx = 0.0f;

    if (has_uv) {
        u0 = uv0->x; v0 = uv0->y;
        u1 = uv1->x; v1 = uv1->y;
        u2 = uv2->x; v2 = uv2->y;

        du_dx = dL0_dx * u0 + dL1_dx * u1 + dL2_dx * u2;
        dv_dx = dL0_dx * v0 + dL1_dx * v1 + dL2_dx * v2;
    }

    int32_t y_start = std::max(0, static_cast<int32_t>(std::floor(min_y)));
    int32_t y_end = std::min(static_cast<int32_t>(dst.height()) - 1, static_cast<int32_t>(std::ceil(max_y)));

    const std::array<std::pair<Point, Point>, 3> edges = {{
        {p0, p1},
        {p1, p2},
        {p2, p0}
    }};

    for (int32_t y = y_start; y <= y_end; ++y) {
        float y_center = static_cast<float>(y) + 0.5f;

        float x_hits[3];
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

        // Barycentric coords at starting pixel (sx0, y_center)
        float dx0 = sx0 - p0.x;
        float dy0 = y_center - p0.y;

        float l1_start = (dx0 * (p2.y - p0.y) - (p2.x - p0.x) * dy0) * inv_det;
        float l2_start = ((p1.x - p0.x) * dy0 - dx0 * (p1.y - p0.y)) * inv_det;
        float l0_start = 1.0f - l1_start - l2_start;

        float cur_r = has_color ? (l0_start * r0 + l1_start * r1 + l2_start * r2) : 255.0f;
        float cur_g = has_color ? (l0_start * g0 + l1_start * g1 + l2_start * g2) : 255.0f;
        float cur_b = has_color ? (l0_start * b0 + l1_start * b1 + l2_start * b2) : 255.0f;
        float cur_a = has_color ? (l0_start * a0 + l1_start * a1 + l2_start * a2) : 255.0f;

        float cur_u = has_uv ? (l0_start * u0 + l1_start * u1 + l2_start * u2) : 0.0f;
        float cur_v = has_uv ? (l0_start * v0 + l1_start * v1 + l2_start * v2) : 0.0f;

        for (int32_t ix = ix_start; ix <= ix_end; ++ix) {
            float px_left = static_cast<float>(ix);
            float px_right = px_left + 1.0f;
            float cov_x = std::clamp(std::min(px_right, row_max_x) - std::max(px_left, row_min_x), 0.0f, 1.0f);

            if (cov_x > 0.0f) {
                float final_alpha = opacity * cov_x;

                PremultipliedColorU8 final_color;

                if (has_uv) {
                    PremultipliedColorU8 tex_sample = sample_bilinear(*texture, cur_u, cur_v);

                    if (has_color) {
                        // Modulate texture color by vertex color
                        float vr = std::clamp(cur_r / 255.0f, 0.0f, 1.0f);
                        float vg = std::clamp(cur_g / 255.0f, 0.0f, 1.0f);
                        float vb = std::clamp(cur_b / 255.0f, 0.0f, 1.0f);
                        float va = std::clamp(cur_a / 255.0f, 0.0f, 1.0f);

                        float fr = tex_sample.red()   * vr * final_alpha;
                        float fg = tex_sample.green() * vg * final_alpha;
                        float fb = tex_sample.blue()  * vb * final_alpha;
                        float fa = tex_sample.alpha() * va * final_alpha;

                        final_color = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>(std::clamp(fr + 0.5f, 0.0f, 255.0f)),
                            static_cast<uint8_t>(std::clamp(fg + 0.5f, 0.0f, 255.0f)),
                            static_cast<uint8_t>(std::clamp(fb + 0.5f, 0.0f, 255.0f)),
                            static_cast<uint8_t>(std::clamp(fa + 0.5f, 0.0f, 255.0f))
                        );
                    } else {
                        float fa = final_alpha;
                        final_color = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>(tex_sample.red()   * fa + 0.5f),
                            static_cast<uint8_t>(tex_sample.green() * fa + 0.5f),
                            static_cast<uint8_t>(tex_sample.blue()  * fa + 0.5f),
                            static_cast<uint8_t>(tex_sample.alpha() * fa + 0.5f)
                        );
                    }
                } else if (has_color) {
                    final_color = PremultipliedColorU8::from_rgba_unchecked(
                        static_cast<uint8_t>(std::clamp(cur_r * final_alpha + 0.5f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(cur_g * final_alpha + 0.5f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(cur_b * final_alpha + 0.5f, 0.0f, 255.0f)),
                        static_cast<uint8_t>(std::clamp(cur_a * final_alpha + 0.5f, 0.0f, 255.0f))
                    );
                }

                dst_row[ix] = simd::blend_pixel_by_mode(dst_row[ix], final_color, blend_mode);
            }

            cur_r += dr_dx;
            cur_g += dg_dx;
            cur_b += db_dx;
            cur_a += da_dx;

            cur_u += du_dx;
            cur_v += dv_dx;
        }
    }
}

} // namespace

void draw_colored_triangle(
    PixmapMut dst,
    Point p0, Point p1, Point p2,
    Color c0, Color c1, Color c2,
    float opacity,
    BlendMode blend_mode
) {
    rasterize_triangle_internal(dst, p0, p1, p2, &c0, &c1, &c2, nullptr, nullptr, nullptr, nullptr, opacity, blend_mode);
}

void draw_textured_triangle(
    PixmapMut dst,
    const PixmapRef& texture,
    Point p0, Point p1, Point p2,
    Point t0, Point t1, Point t2,
    const Color* c0, const Color* c1, const Color* c2,
    float opacity,
    BlendMode blend_mode
) {
    rasterize_triangle_internal(dst, p0, p1, p2, c0, c1, c2, &t0, &t1, &t2, &texture, opacity, blend_mode);
}

void draw_vertices(
    PixmapMut dst,
    const Vertices& vertices,
    const PixmapRef* texture,
    float opacity,
    BlendMode blend_mode,
    Transform transform
) {
    if (!vertices.is_valid() || opacity <= 0.0f) return;

    size_t num_tris = vertices.triangle_count();
    bool has_colors = !vertices.colors.empty();
    bool has_uvs = !vertices.tex_coords.empty() && (texture != nullptr) && !texture->is_empty();

    for (size_t t = 0; t < num_tris; ++t) {
        auto [i0, i1, i2] = vertices.get_triangle_indices(t);

        Point p0 = vertices.positions[i0];
        Point p1 = vertices.positions[i1];
        Point p2 = vertices.positions[i2];

        if (!transform.is_identity()) {
            transform.map_point(p0);
            transform.map_point(p1);
            transform.map_point(p2);
        }

        const Color* c0 = has_colors ? &vertices.colors[i0] : nullptr;
        const Color* c1 = has_colors ? &vertices.colors[i1] : nullptr;
        const Color* c2 = has_colors ? &vertices.colors[i2] : nullptr;

        const Point* uv0 = has_uvs ? &vertices.tex_coords[i0] : nullptr;
        const Point* uv1 = has_uvs ? &vertices.tex_coords[i1] : nullptr;
        const Point* uv2 = has_uvs ? &vertices.tex_coords[i2] : nullptr;

        rasterize_triangle_internal(
            dst,
            p0, p1, p2,
            c0, c1, c2,
            uv0, uv1, uv2,
            texture,
            opacity,
            blend_mode
        );
    }
}

} // namespace nisaba::raster
