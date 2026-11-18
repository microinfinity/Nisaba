#include "nisaba/canvas/painter.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/pipeline/blitter.hpp"
#include "nisaba/raster/scan.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/pipeline/simd.hpp"

namespace nisaba {

namespace painter {

inline void blend_pixel_fast_solid(
    PremultipliedColorU8* pixel_ptr,
    uint32_t sc_r, uint32_t sc_g, uint32_t sc_b, uint32_t sc_a,
    float coverage
) noexcept {
    uint32_t cov = static_cast<uint32_t>(coverage * 255.0f + 0.5f) + 1;
    uint32_t sa = (sc_a * cov) >> 8;
    uint32_t sr = (sc_r * cov) >> 8;
    uint32_t sg = (sc_g * cov) >> 8;
    uint32_t sb = (sc_b * cov) >> 8;
    uint32_t inv_a = 256 - sa;
    uint32_t src_packed = sr | (sg << 8) | (sb << 16) | (sa << 24);

    uint32_t* d_ptr = reinterpret_cast<uint32_t*>(pixel_ptr);
    uint32_t d = *d_ptr;
    uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
    uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
    *d_ptr = src_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
}

static void render_axis_aligned_rect_fast(
    PixmapMut& dst,
    float x0, float y0, float x1, float y1,
    const Paint& paint,
    const Mask* mask,
    std::optional<ScreenIntRect> clip = std::nullopt
) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    float dst_w = static_cast<float>(dst.width());
    float dst_h = static_cast<float>(dst.height());

    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = dst_w;
    float max_y = dst_h;

    if (clip.has_value()) {
        min_x = std::max(min_x, static_cast<float>(clip->x()));
        min_y = std::max(min_y, static_cast<float>(clip->y()));
        max_x = std::min(max_x, static_cast<float>(clip->right()));
        max_y = std::min(max_y, static_cast<float>(clip->bottom()));
    }

    x0 = std::max(x0, min_x);
    y0 = std::max(y0, min_y);
    x1 = std::min(x1, max_x);
    y1 = std::min(y1, max_y);

    if (x1 <= x0 || y1 <= y0) return;

    int32_t ix0 = static_cast<int32_t>(std::floor(x0));
    int32_t ix1 = static_cast<int32_t>(std::ceil(x1));
    int32_t iy0 = static_cast<int32_t>(std::floor(y0));
    int32_t iy1 = static_cast<int32_t>(std::ceil(y1));

    int32_t clamp_min_x = clip ? static_cast<int32_t>(clip->x()) : 0;
    int32_t clamp_max_x = clip ? static_cast<int32_t>(clip->right()) : static_cast<int32_t>(dst.width());
    int32_t clamp_min_y = clip ? static_cast<int32_t>(clip->y()) : 0;
    int32_t clamp_max_y = clip ? static_cast<int32_t>(clip->bottom()) : static_cast<int32_t>(dst.height());

    ix0 = std::clamp(ix0, clamp_min_x, clamp_max_x);
    ix1 = std::clamp(ix1, clamp_min_x, clamp_max_x);
    iy0 = std::clamp(iy0, clamp_min_y, clamp_max_y);
    iy1 = std::clamp(iy1, clamp_min_y, clamp_max_y);

    if (ix1 <= ix0 || iy1 <= iy0) return;

    bool is_integer_aligned = !paint.anti_alias ||
        (std::abs(x0 - static_cast<float>(ix0)) < 1e-4f &&
         std::abs(x1 - static_cast<float>(ix1)) < 1e-4f &&
         std::abs(y0 - static_cast<float>(iy0)) < 1e-4f &&
         std::abs(y1 - static_cast<float>(iy1)) < 1e-4f);

    // Ultra-fast path: direct SIMD solid row blit without constructing PipelineBlitter
    if (is_integer_aligned && paint.is_solid_color() && !paint.has_color_filter() &&
        !paint.is_linear_blending()) {
        if (paint.blend_mode == BlendMode::SourceOver || paint.blend_mode == BlendMode::Source) {
            PremultipliedColorU8 sc = paint.shader.solid_color().premultiply().to_color_u8();
            if (dst.format() == PixelFormat::RGBA8888 || dst.format() == PixelFormat::BGRA8888) {
                if (dst.format() == PixelFormat::BGRA8888) {
                    sc = PremultipliedColorU8::from_rgba_unchecked(sc.blue(), sc.green(), sc.red(), sc.alpha());
                }
                uint32_t w = static_cast<uint32_t>(ix1 - ix0);
                uint32_t h = static_cast<uint32_t>(iy1 - iy0);
                if (mask != nullptr) {
                    for (int32_t y = iy0; y < iy1; ++y) {
                        const uint8_t* m_ptr = mask->data() + y * mask->width() + ix0;
                        if (w == 2) {
                            uint16_t m16;
                            std::memcpy(&m16, m_ptr, sizeof(uint16_t));
                            if (m16 == 0) continue;
                        } else if (w == 1) {
                            if (*m_ptr == 0) continue;
                        } else if (w <= 8) {
                            uint64_t m64 = 0;
                            std::memcpy(&m64, m_ptr, w);
                            if (m64 == 0) continue;
                        }
                        simd::blend_solid_mask_span(dst.row(y) + ix0, sc, m_ptr, w);
                    }
                    return;
                }
                if (paint.blend_mode == BlendMode::Source || sc.alpha() == 255) {
                    for (int32_t y = iy0; y < iy1; ++y) {
                        simd::fill_solid_span(dst.row(y) + ix0, sc, w);
                    }
                } else if (sc.alpha() > 0) {
                    simd::blend_solid_rect_source_over(dst.row(iy0) + ix0, dst.width(), sc, w, h);
                }
                return;
            }
        }
    }

    std::optional<SubMaskRef> submask{};
    if (mask != nullptr) {
        submask = mask->as_submask();
    }
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    if (is_integer_aligned) {
        auto ir = ScreenIntRect::from_xywh(
            static_cast<uint32_t>(ix0), static_cast<uint32_t>(iy0),
            static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0)
        );
        if (ir) blitter->blit_rect(*ir);
        return;
    }

    // Vertical line fast path (tall thin rectangle)
    if (ix1 - ix0 <= 8 && iy1 - iy0 > (ix1 - ix0) && (iy1 - iy0) > 2) {
        int32_t y_start_int = iy0;
        float y_cov_top = 1.0f;
        bool has_top_partial = false;
        if (y0 > static_cast<float>(iy0) + 1e-4f) {
            y_cov_top = static_cast<float>(iy0 + 1) - y0;
            has_top_partial = true;
            y_start_int = iy0 + 1;
        }

        int32_t y_end_int = iy1;
        float y_cov_bot = 1.0f;
        bool has_bot_partial = false;
        if (y1 < static_cast<float>(iy1) - 1e-4f) {
            y_cov_bot = y1 - static_cast<float>(iy1 - 1);
            has_bot_partial = true;
            y_end_int = iy1 - 1;
        }

        for (int32_t x = ix0; x < ix1; ++x) {
            float fx = static_cast<float>(x);
            float x_cov = std::min(x1, fx + 1.0f) - std::max(x0, fx);
            if (x_cov <= 0.0f) continue;

            if (has_top_partial) {
                blitter->blend_pixel(x, iy0, x_cov * y_cov_top);
            }
            if (y_end_int > y_start_int) {
                uint32_t h = static_cast<uint32_t>(y_end_int - y_start_int);
                uint8_t a = static_cast<uint8_t>(x_cov * 255.0f + 0.5f);
                blitter->blit_v(static_cast<uint32_t>(x), static_cast<uint32_t>(y_start_int), LengthU32::create_unchecked(h), a);
            }
            if (has_bot_partial) {
                blitter->blend_pixel(x, iy1 - 1, x_cov * y_cov_bot);
            }
        }
        return;
    }

    // Analytical Anti-Aliased Rectangle Rasterizer (O(1) per scanline span)
    for (int32_t y = iy0; y < iy1; ++y) {
        float y_top = static_cast<float>(y);
        float y_bot = y_top + 1.0f;
        float y_cov = std::min(y1, y_bot) - std::max(y0, y_top);
        if (y_cov <= 0.0f) continue;

        if (ix1 - ix0 == 1) {
            float x_cov = x1 - x0;
            blitter->blend_pixel(ix0, y, x_cov * y_cov);
        } else {
            // First pixel in span
            float x_cov0 = static_cast<float>(ix0 + 1) - x0;
            blitter->blend_pixel(ix0, y, x_cov0 * y_cov);

            // Middle 100% full-coverage span
            if (ix1 - ix0 > 2) {
                blitter->blend_span(ix0 + 1, y, (ix1 - 1) - (ix0 + 1), y_cov);
            }

            // Last pixel in span
            float x_cov1 = x1 - static_cast<float>(ix1 - 1);
            blitter->blend_pixel(ix1 - 1, y, x_cov1 * y_cov);
        }
    }
}

void fill_convex_quad(
    PixmapMut& dst,
    Point p0, Point p1, Point p2, Point p3,
    const Paint& paint,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    std::array<Point, 4> pts = {p0, p1, p2, p3};

    // Find top-most vertex (min y)
    int min_idx = 0;
    for (int i = 1; i < 4; ++i) {
        if (pts[i].y < pts[min_idx].y) min_idx = i;
    }

    // Check winding / orientation to identify left and right chains
    // Cross product (p1 - p0) x (p2 - p1)
    float cp = (p1.x - p0.x) * (p2.y - p1.y) - (p1.y - p0.y) * (p2.x - p1.x);
    int step_left = (cp > 0) ? -1 : 1;
    int step_right = -step_left;

    std::optional<SubMaskRef> submask = mask ? std::make_optional(mask->as_submask()) : std::nullopt;
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    int l_idx = min_idx;
    int r_idx = min_idx;
    int l_next = (l_idx + step_left + 4) % 4;
    int r_next = (r_idx + step_right + 4) % 4;

    float cur_y = std::floor(pts[min_idx].y) + 0.5f;

    // Find max y
    float max_y = pts[0].y;
    for (int i = 1; i < 4; ++i) max_y = std::max(max_y, pts[i].y);
    int end_y = static_cast<int>(std::ceil(max_y));

    float xl = pts[l_idx].x;
    float xr = pts[r_idx].x;
    float dxl = 0.0f, dxr = 0.0f;

    auto setup_edge = [](Point p_start, Point p_end, float cur_y, float& x_out, float& dx_out) {
        float dy = p_end.y - p_start.y;
        if (std::abs(dy) < 1e-4f) {
            x_out = p_end.x;
            dx_out = 0.0f;
            return;
        }
        float inv_dy = 1.0f / dy;
        dx_out = (p_end.x - p_start.x) * inv_dy;
        x_out = p_start.x + (cur_y - p_start.y) * dx_out;
    };

    setup_edge(pts[l_idx], pts[l_next], cur_y, xl, dxl);
    setup_edge(pts[r_idx], pts[r_next], cur_y, xr, dxr);

    int iy = static_cast<int>(std::floor(cur_y));
    int clip_top = clip ? clip->top() : 0;
    int clip_bot = clip ? clip->bottom() : dst.height();
    int clip_l   = clip ? clip->left() : 0;
    int clip_r   = clip ? clip->right() : dst.width();

    auto advance_left = [&]() {
        while (l_next != r_idx && pts[l_next].y <= cur_y) {
            l_idx = l_next;
            l_next = (l_idx + step_left + 4) % 4;
            setup_edge(pts[l_idx], pts[l_next], cur_y, xl, dxl);
        }
    };

    auto advance_right = [&]() {
        while (r_next != l_idx && pts[r_next].y <= cur_y) {
            r_idx = r_next;
            r_next = (r_idx + step_right + 4) % 4;
            setup_edge(pts[r_idx], pts[r_next], cur_y, xr, dxr);
        }
    };

    // Skip initial horizontal edges at top
    advance_left();
    advance_right();

    while (iy < end_y) {
        if (cur_y >= pts[l_next].y && l_next != r_idx) {
            advance_left();
        }
        if (cur_y >= pts[r_next].y && r_next != l_idx) {
            advance_right();
        }

        if (iy >= clip_top && iy < clip_bot) {
            float x_start = std::min(xl, xr);
            float x_end   = std::max(xl, xr);

            int ix0 = static_cast<int>(std::floor(x_start));
            int ix1 = static_cast<int>(std::ceil(x_end));

            if (ix1 > ix0) {
                if (!paint.anti_alias) {
                    int full_l = std::max(ix0, clip_l);
                    int full_r = std::min(ix1, clip_r);
                    if (full_r > full_l) {
                        blitter->blend_span(full_l, iy, full_r - full_l, 1.0f);
                    }
                } else if (ix1 - ix0 == 1) {
                    float cov = std::clamp(x_end - x_start, 0.0f, 1.0f);
                    blitter->blend_pixel(ix0, iy, cov);
                } else {
                    float cov_l = 1.0f - (x_start - static_cast<float>(ix0));
                    float cov_r = x_end - static_cast<float>(ix1 - 1);

                    blitter->blend_pixel(ix0, iy, std::clamp(cov_l, 0.0f, 1.0f));
                    if (ix1 - ix0 > 2) {
                        int full_l = std::max(ix0 + 1, clip_l);
                        int full_r = std::min(ix1 - 1, clip_r);
                        if (full_r > full_l) {
                            blitter->blend_span(full_l, iy, full_r - full_l, 1.0f);
                        }
                    }
                    blitter->blend_pixel(ix1 - 1, iy, std::clamp(cov_r, 0.0f, 1.0f));
                }
            }
        }

        cur_y += 1.0f;
        iy += 1;
        xl += dxl;
        xr += dxr;
    }
}

void fill_rect(
    PixmapMut& dst,
    const Rect& rect,
    const Paint& paint,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (std::abs(transform.kx) < 1e-5f && std::abs(transform.ky) < 1e-5f) {
        float x0 = rect.left() * transform.sx + transform.tx;
        float x1 = rect.right() * transform.sx + transform.tx;
        float y0 = rect.top() * transform.sy + transform.ty;
        float y1 = rect.bottom() * transform.sy + transform.ty;

        if (transform.is_identity()) {
            render_axis_aligned_rect_fast(dst, x0, y0, x1, y1, paint, mask, clip);
        } else {
            Paint p = paint;
            p.shader.transform(transform);
            render_axis_aligned_rect_fast(dst, x0, y0, x1, y1, p, mask, clip);
        }
        return;
    }

    Point p0(rect.left(), rect.top());
    Point p1(rect.right(), rect.top());
    Point p2(rect.right(), rect.bottom());
    Point p3(rect.left(), rect.bottom());
    transform.map_point(p0);
    transform.map_point(p1);
    transform.map_point(p2);
    transform.map_point(p3);

    Paint p = paint;
    p.shader.transform(transform);
    fill_convex_quad(dst, p0, p1, p2, p3, p, mask, clip);
}

void stroke_rect_axis_aligned(
    PixmapMut& dst,
    const Rect& rect,
    float stroke_width,
    const Paint& paint,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    float sw = stroke_width;
    float hw = sw * 0.5f;
    float rw = rect.width();
    float rh = rect.height();
    if (rw <= 0.0f || rh <= 0.0f || sw <= 0.0f) return;

    if (rw <= sw || rh <= sw) {
        auto r_out = Rect::from_xywh(rect.left() - hw, rect.top() - hw, rw + sw, rh + sw);
        if (r_out) fill_rect(dst, *r_out, paint, Transform(), mask, clip);
        return;
    }

    std::optional<SubMaskRef> submask{};
    if (mask != nullptr) submask = mask->as_submask();
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    auto r_top = Rect::from_xywh(rect.left() - hw, rect.top() - hw, rw + sw, sw);
    auto r_bot = Rect::from_xywh(rect.left() - hw, rect.bottom() - hw, rw + sw, sw);
    auto r_left = Rect::from_xywh(rect.left() - hw, rect.top() + hw, sw, rh - sw);
    auto r_right = Rect::from_xywh(rect.right() - hw, rect.top() + hw, sw, rh - sw);

    int32_t clip_left = clip ? clip->left() : 0;
    int32_t clip_top = clip ? clip->top() : 0;
    int32_t clip_right = clip ? clip->right() : dst.width();
    int32_t clip_bottom = clip ? clip->bottom() : dst.height();

    auto render_rect_blitter = [&](const Rect& r) {
        float x0 = r.left();
        float x1 = r.right();
        float y0 = r.top();
        float y1 = r.bottom();

        if (x1 <= static_cast<float>(clip_left) || x0 >= static_cast<float>(clip_right) ||
            y1 <= static_cast<float>(clip_top) || y0 >= static_cast<float>(clip_bottom)) {
            return;
        }

        int32_t ix0 = std::clamp(static_cast<int32_t>(std::floor(x0)), clip_left, clip_right);
        int32_t iy0 = std::clamp(static_cast<int32_t>(std::floor(y0)), clip_top, clip_bottom);
        int32_t ix1 = std::clamp(static_cast<int32_t>(std::ceil(x1)), clip_left, clip_right);
        int32_t iy1 = std::clamp(static_cast<int32_t>(std::ceil(y1)), clip_top, clip_bottom);
        if (ix1 <= ix0 || iy1 <= iy0) return;

        // Vertical line fast path (tall thin rectangle)
        if (ix1 - ix0 <= 8 && iy1 - iy0 > (ix1 - ix0) && (iy1 - iy0) > 2) {
            int32_t y_start_int = iy0;
            float y_cov_top = 1.0f;
            bool has_top_partial = false;
            if (y0 > static_cast<float>(iy0) + 1e-4f) {
                y_cov_top = static_cast<float>(iy0 + 1) - y0;
                has_top_partial = true;
                y_start_int = iy0 + 1;
            }

            int32_t y_end_int = iy1;
            float y_cov_bot = 1.0f;
            bool has_bot_partial = false;
            if (y1 < static_cast<float>(iy1) - 1e-4f) {
                y_cov_bot = y1 - static_cast<float>(iy1 - 1);
                has_bot_partial = true;
                y_end_int = iy1 - 1;
            }

            for (int32_t x = ix0; x < ix1; ++x) {
                float fx = static_cast<float>(x);
                float x_cov = std::min(x1, fx + 1.0f) - std::max(x0, fx);
                if (x_cov <= 0.0f) continue;

                if (has_top_partial) {
                    blitter->blend_pixel(x, iy0, x_cov * y_cov_top);
                }
                if (y_end_int > y_start_int) {
                    uint32_t h = static_cast<uint32_t>(y_end_int - y_start_int);
                    uint8_t a = static_cast<uint8_t>(x_cov * 255.0f + 0.5f);
                    blitter->blit_v(static_cast<uint32_t>(x), static_cast<uint32_t>(y_start_int), LengthU32::create_unchecked(h), a);
                }
                if (has_bot_partial) {
                    blitter->blend_pixel(x, iy1 - 1, x_cov * y_cov_bot);
                }
            }
            return;
        }

        // Horizontal line / general rect
        for (int32_t y = iy0; y < iy1; ++y) {
            float y_top = static_cast<float>(y);
            float y_bot = y_top + 1.0f;
            float y_cov = std::min(y1, y_bot) - std::max(y0, y_top);
            if (y_cov <= 0.0f) continue;

            if (ix1 - ix0 == 1) {
                float x_cov = x1 - x0;
                blitter->blend_pixel(ix0, y, x_cov * y_cov);
            } else {
                float x_cov0 = static_cast<float>(ix0 + 1) - x0;
                blitter->blend_pixel(ix0, y, x_cov0 * y_cov);

                if (ix1 - ix0 > 2) {
                    blitter->blend_span(ix0 + 1, y, (ix1 - 1) - (ix0 + 1), y_cov);
                }

                float x_cov1 = x1 - static_cast<float>(ix1 - 1);
                blitter->blend_pixel(ix1 - 1, y, x_cov1 * y_cov);
            }
        }
    };

    if (r_top) render_rect_blitter(*r_top);
    if (r_bot) render_rect_blitter(*r_bot);
    if (r_left) render_rect_blitter(*r_left);
    if (r_right) render_rect_blitter(*r_right);
}

void fill_path(
    PixmapMut& dst,
    const Path& path,
    const Paint& paint,
    FillRule fill_rule,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (path.is_empty()) return;

    if (transform.is_identity()) {
        auto bounds = path.bounds();
        if (bounds.width() <= 0.0f || bounds.height() <= 0.0f) {
            return;
        }

        int32_t bx0 = std::max(0, static_cast<int32_t>(std::floor(bounds.left())));
        int32_t by0 = std::max(0, static_cast<int32_t>(std::floor(bounds.top())));
        int32_t bx1 = std::min(static_cast<int32_t>(dst.width()), static_cast<int32_t>(std::ceil(bounds.right())));
        int32_t by1 = std::min(static_cast<int32_t>(dst.height()), static_cast<int32_t>(std::ceil(bounds.bottom())));
        if (bx1 <= bx0 || by1 <= by0) return;
        auto path_clip = ScreenIntRect::from_xywh(
            static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
            static_cast<uint32_t>(bx1 - bx0), static_cast<uint32_t>(by1 - by0)
        );
        if (!path_clip.has_value()) return;

        ScreenIntRect clip_bounds = *path_clip;
        if (clip.has_value()) {
            auto is = clip_bounds.intersect(*clip);
            if (!is.has_value()) return;
            clip_bounds = *is;
        }

        std::optional<SubMaskRef> submask{};
        if (mask != nullptr) {
            submask = mask->as_submask();
        }

        SubPixmapMut subpix = dst.as_subpixmap();
        auto blitter = PipelineBlitter::create(paint, submask, subpix);
        if (!blitter) return;

        scan::fill_path(path, fill_rule, clip_bounds, paint.anti_alias, *blitter);
    } else {
        // Compute transformed bounds for clipping
        auto b = path.bounds();
        Point corners[4] = {
            Point::from_xy(b.left(), b.top()),
            Point::from_xy(b.right(), b.top()),
            Point::from_xy(b.right(), b.bottom()),
            Point::from_xy(b.left(), b.bottom())
        };
        for (auto& c : corners) {
            transform.map_point(c);
        }
        float tb_left = std::min({corners[0].x, corners[1].x, corners[2].x, corners[3].x});
        float tb_top = std::min({corners[0].y, corners[1].y, corners[2].y, corners[3].y});
        float tb_right = std::max({corners[0].x, corners[1].x, corners[2].x, corners[3].x});
        float tb_bottom = std::max({corners[0].y, corners[1].y, corners[2].y, corners[3].y});

        if (tb_right <= tb_left || tb_bottom <= tb_top) return;

        int32_t bx0 = std::max(0, static_cast<int32_t>(std::floor(tb_left)));
        int32_t by0 = std::max(0, static_cast<int32_t>(std::floor(tb_top)));
        int32_t bx1 = std::min(static_cast<int32_t>(dst.width()), static_cast<int32_t>(std::ceil(tb_right)));
        int32_t by1 = std::min(static_cast<int32_t>(dst.height()), static_cast<int32_t>(std::ceil(tb_bottom)));
        if (bx1 <= bx0 || by1 <= by0) return;

        auto path_clip = ScreenIntRect::from_xywh(
            static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
            static_cast<uint32_t>(bx1 - bx0), static_cast<uint32_t>(by1 - by0)
        );
        if (!path_clip.has_value()) return;

        ScreenIntRect clip_bounds = *path_clip;
        if (clip.has_value()) {
            auto is = clip_bounds.intersect(*clip);
            if (!is.has_value()) return;
            clip_bounds = *is;
        }

        std::optional<SubMaskRef> submask{};
        if (mask != nullptr) {
            submask = mask->as_submask();
        }

        Paint transformed_paint = paint;
        transformed_paint.shader.transform(transform);

        SubPixmapMut subpix = dst.as_subpixmap();
        auto blitter = PipelineBlitter::create(transformed_paint, submask, subpix);
        auto transformed = path.transform(transform);
        if (!transformed) return;
        scan::fill_path(*transformed, fill_rule, clip_bounds, paint.anti_alias, *blitter);
    }
}

static bool try_stroke_axis_aligned_lines(
    PixmapMut& dst,
    const Path& path,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (stroke.dash.has_value()) return false;
    if (std::abs(transform.kx) > 1e-5f || std::abs(transform.ky) > 1e-5f) return false;

    float sx = std::abs(transform.sx);
    float sy = std::abs(transform.sy);
    float w_h = stroke.width * sy;
    float w_v = stroke.width * sx;

    if (stroke.line_cap == LineCap::Round && (w_h > 1.5f || w_v > 1.5f)) {
        return false;
    }

    const auto& verbs = path.verbs();
    for (auto v : verbs) {
        if (v == PathVerb::Quad || v == PathVerb::Cubic || v == PathVerb::Close) return false;
    }

    auto iter = path.segments();
    Point cur{0.0f, 0.0f};

    struct LineRect {
        float x0, y0, x1, y1;
    };
    constexpr size_t STACK_CAP = 64;
    std::array<LineRect, STACK_CAP> stack_rects;
    std::vector<LineRect> heap_rects;
    size_t rect_count = 0;

    auto add_rect = [&](float x0, float y0, float x1, float y1) {
        if (rect_count < STACK_CAP) {
            stack_rects[rect_count++] = {x0, y0, x1, y1};
        } else {
            heap_rects.push_back({x0, y0, x1, y1});
        }
    };

    struct DiagLine { Point p0, p1; };
    constexpr size_t STACK_DIAG_CAP = 16;
    std::array<DiagLine, STACK_DIAG_CAP> stack_diags;
    std::vector<DiagLine> heap_diags;
    size_t diag_count = 0;

    auto add_diag = [&](Point p0, Point p1) {
        if (diag_count < STACK_DIAG_CAP) {
            stack_diags[diag_count++] = {p0, p1};
        } else {
            heap_diags.push_back({p0, p1});
        }
    };

    while (auto seg = iter.next()) {
        switch (seg->type) {
            case PathSegment::Type::MoveTo:
                cur = seg->p0;
                break;
            case PathSegment::Type::LineTo: {
                Point p0 = cur;
                Point p1 = seg->p0;
                transform.map_point(p0);
                transform.map_point(p1);
                cur = seg->p0;

                bool is_h = std::abs(p0.y - p1.y) < 1e-4f;
                bool is_v = std::abs(p0.x - p1.x) < 1e-4f;

                if (is_h) {
                    float x_min = std::min(p0.x, p1.x);
                    float x_max = std::max(p0.x, p1.x);
                    float y = p0.y;
                    if (stroke.line_cap == LineCap::Square) {
                        x_min -= w_h * 0.5f;
                        x_max += w_h * 0.5f;
                    }
                    float y_min = y - w_h * 0.5f;
                    float y_max = y + w_h * 0.5f;
                    add_rect(x_min, y_min, x_max, y_max);
                } else if (is_v) {
                    float y_min = std::min(p0.y, p1.y);
                    float y_max = std::max(p0.y, p1.y);
                    float x = p0.x;
                    if (stroke.line_cap == LineCap::Square) {
                        y_min -= w_v * 0.5f;
                        y_max += w_v * 0.5f;
                    }
                    float x_min = x - w_v * 0.5f;
                    float x_max = x + w_v * 0.5f;
                    add_rect(x_min, y_min, x_max, y_max);
                } else {
                    add_diag(p0, p1);
                }
                break;
            }
            default:
                break;
        }
    }

    if (rect_count == 0 && heap_rects.empty() && diag_count == 0 && heap_diags.empty()) return true;

    Paint p = paint;
    if (!transform.is_identity()) {
        p.shader.transform(transform);
    }

    if (heap_rects.empty()) {
        for (size_t i = 0; i < rect_count; ++i) {
            render_axis_aligned_rect_fast(dst, stack_rects[i].x0, stack_rects[i].y0, stack_rects[i].x1, stack_rects[i].y1, p, mask, clip);
        }
    } else {
        for (size_t i = 0; i < STACK_CAP; ++i) {
            render_axis_aligned_rect_fast(dst, stack_rects[i].x0, stack_rects[i].y0, stack_rects[i].x1, stack_rects[i].y1, p, mask, clip);
        }
        for (const auto& r : heap_rects) {
            render_axis_aligned_rect_fast(dst, r.x0, r.y0, r.x1, r.y1, p, mask, clip);
        }
    }

    for (size_t i = 0; i < diag_count; ++i) {
        stroke_line(dst, stack_diags[i].p0, stack_diags[i].p1, p, stroke, Transform(), mask, clip);
    }
    for (const auto& d : heap_diags) {
        stroke_line(dst, d.p0, d.p1, p, stroke, Transform(), mask, clip);
    }

    return true;
}

void stroke_path(
    PixmapMut& dst,
    const Path& path,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (stroke.width < 0.0f || path.is_empty()) return;

    if (try_stroke_axis_aligned_lines(dst, path, paint, stroke, transform, mask, clip)) {
        return;
    }

    float res_scale = PathStroker::compute_resolution_scale(transform);

    static thread_local PathStroker tl_stroker;
    auto stroked = tl_stroker.stroke_fast(path, stroke, res_scale);
    if (!stroked) return;

    fill_path(dst, *stroked, paint, FillRule::Winding, transform, mask, clip);
}

void stroke_line(
    PixmapMut& dst,
    Point p0, Point p1,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (stroke.width <= 0.0f) return;

    if (!transform.is_identity()) {
        transform.map_point(p0);
        transform.map_point(p1);
    }

    float hw = stroke.width * 0.5f;

    // Fast path: axis-aligned lines
    if (std::abs(p0.y - p1.y) < 1e-4f) {
        float x_min = std::min(p0.x, p1.x);
        float x_max = std::max(p0.x, p1.x);
        if (stroke.line_cap == LineCap::Square) {
            x_min -= hw;
            x_max += hw;
        }
        render_axis_aligned_rect_fast(dst, x_min, p0.y - hw, x_max, p0.y + hw, paint, mask, clip);
        return;
    }
    if (std::abs(p0.x - p1.x) < 1e-4f) {
        float y_min = std::min(p0.y, p1.y);
        float y_max = std::max(p0.y, p1.y);
        if (stroke.line_cap == LineCap::Square) {
            y_min -= hw;
            y_max += hw;
        }
        render_axis_aligned_rect_fast(dst, p0.x - hw, y_min, p0.x + hw, y_max, paint, mask, clip);
        return;
    }

    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float len = std::hypot(dx, dy);
    if (len < 1e-4f) return;

    float inv_len = 1.0f / len;
    float ux = dx * inv_len;
    float uy = dy * inv_len;

    int32_t dst_w = static_cast<int32_t>(dst.width());
    int32_t dst_h = static_cast<int32_t>(dst.height());
    int32_t clamp_min_x = clip ? static_cast<int32_t>(clip->x()) : 0;
    int32_t clamp_max_x = clip ? static_cast<int32_t>(clip->right()) : dst_w;
    int32_t clamp_min_y = clip ? static_cast<int32_t>(clip->y()) : 0;
    int32_t clamp_max_y = clip ? static_cast<int32_t>(clip->bottom()) : dst_h;

    bool is_solid = paint.is_solid_color() && !paint.is_linear_blending() && !paint.has_color_filter() && (mask == nullptr) && (paint.blend_mode == BlendMode::SourceOver);

    // For solid-color lines, skip blitter creation entirely and compute RGBA once
    PremultipliedColorU8 sc_val{};
    uint32_t sc_r = 0, sc_g = 0, sc_b = 0, sc_a = 0;
    uint32_t sc_packed = 0; // pre-packed RGBA for full-coverage pixels
    if (is_solid) {
        sc_val = paint.shader.solid_color().premultiply().to_color_u8();
        sc_r = sc_val.red();
        sc_g = sc_val.green();
        sc_b = sc_val.blue();
        sc_a = sc_val.alpha();
        sc_packed = sc_r | (sc_g << 8) | (sc_b << 16) | (sc_a << 24);
    }

    // For non-solid paint, create blitter once via move-construction
    std::optional<SubMaskRef> submask{};
    SubPixmapMut subpix = dst.as_subpixmap();
    if (!is_solid && mask != nullptr) submask = mask->as_submask();
    auto blitter = is_solid
        ? std::optional<PipelineBlitter>{}
        : PipelineBlitter::create(paint, submask, subpix);
    if (!is_solid && !blitter) return;

    if (is_solid && stroke.width == 1.0f && stroke.line_cap == LineCap::Butt && !stroke.dash.has_value()) {
        auto blend_cov_u8 = [&](uint32_t* d_ptr, uint32_t cov_u8) {
            if (cov_u8 <= 1) return;
            uint32_t sa = (sc_a * (cov_u8 + 1)) >> 8;
            uint32_t sr = (((sc_packed) & 0xFF) * (cov_u8 + 1)) >> 8;
            uint32_t sg = (((sc_packed >> 8) & 0xFF) * (cov_u8 + 1)) >> 8;
            uint32_t sb = (((sc_packed >> 16) & 0xFF) * (cov_u8 + 1)) >> 8;
            uint32_t src = sr | (sg << 8) | (sb << 16) | (sa << 24);
            uint32_t inv_a = 256 - sa;

            uint32_t d = *d_ptr;
            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
            *d_ptr = src + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
        };

        uint32_t* base_ptr = reinterpret_cast<uint32_t*>(dst.pixels_mut());
        size_t stride = dst.width();

        if (std::abs(dx) >= std::abs(dy)) {
            if (p0.x > p1.x) {
                std::swap(p0, p1);
                dx = -dx; dy = -dy;
            }
            float slope_yx = dy / dx;

            int32_t ix_start = std::max(clamp_min_x, static_cast<int32_t>(std::floor(p0.x)));
            int32_t ix_end   = std::min(clamp_max_x, static_cast<int32_t>(std::ceil(p1.x)));

            float vx0 = (static_cast<float>(ix_start) + 0.5f) - p0.x;
            float mid_y = p0.y + vx0 * slope_yx;

            int64_t y_fp = static_cast<int64_t>((mid_y - 0.5f) * 65536.0f);
            int64_t slope_fp = static_cast<int64_t>(slope_yx * 65536.0f);

            for (int32_t x = ix_start; x < ix_end; ++x, y_fp += slope_fp) {
                int32_t iy = static_cast<int32_t>(y_fp >> 16);
                uint32_t frac = static_cast<uint32_t>((y_fp & 0xFFFF) >> 8);

                if (iy >= clamp_min_y && iy < clamp_max_y) {
                    blend_cov_u8(&base_ptr[iy * stride + x], 255 - frac);
                }
                if (iy + 1 >= clamp_min_y && iy + 1 < clamp_max_y) {
                    blend_cov_u8(&base_ptr[(iy + 1) * stride + x], frac);
                }
            }
        } else {
            if (p0.y > p1.y) {
                std::swap(p0, p1);
                dx = -dx; dy = -dy;
            }
            float slope_xy = dx / dy;

            int32_t iy_start = std::max(clamp_min_y, static_cast<int32_t>(std::floor(p0.y)));
            int32_t iy_end   = std::min(clamp_max_y, static_cast<int32_t>(std::ceil(p1.y)));

            float vy0 = (static_cast<float>(iy_start) + 0.5f) - p0.y;
            float mid_x = p0.x + vy0 * slope_xy;

            int64_t x_fp = static_cast<int64_t>((mid_x - 0.5f) * 65536.0f);
            int64_t slope_fp = static_cast<int64_t>(slope_xy * 65536.0f);

            for (int32_t y = iy_start; y < iy_end; ++y, x_fp += slope_fp) {
                int32_t ix = static_cast<int32_t>(x_fp >> 16);
                uint32_t frac = static_cast<uint32_t>((x_fp & 0xFFFF) >> 8);

                uint32_t* row = &base_ptr[y * stride];
                if (ix >= clamp_min_x && ix < clamp_max_x) {
                    blend_cov_u8(&row[ix], 255 - frac);
                }
                if (ix + 1 >= clamp_min_x && ix + 1 < clamp_max_x) {
                    blend_cov_u8(&row[ix + 1], frac);
                }
            }
        }
        return;
    }

    float hw_plus_half = hw + 0.5f;
    float hw_inner     = hw_plus_half - 1.0f; // full edge coverage zone
    float cap_start    = 0.5f;
    float cap_end      = len - 0.5f;

    if (std::abs(dx) >= std::abs(dy)) {
        // ─────────────────────────────────────────────────────────────────────
        // X-MAJOR: Re-expressed as SCANLINES for cache locality.
        // Instead of: for each x → write scattered y-pixels (cache MISS per write)
        // We do:      for each y → write contiguous x-pixels (cache HOT per write)
        //
        // For an X-major line, for each y-row in [iy_start, iy_end):
        //   center_x(y) = p0.x + (y - p0.y) * (dx/dy)  ... but dx/dy could be huge
        //   Better: use the inverse parametrization.
        //   At row y with vy = (y+0.5) - p0.y:
        //     proj = vy*uy  (varies slowly, uy = dy/len << 1 for X-major)
        //     perp(x) = |vx*ux + vy*uy_perp|  where perp coord = -vy*uy·hat + vx·ux·hat
        //   Actually: perp at pixel (x,y) = |(-ux)*vy + uy*vx|  ... wait, let me use
        //   the correct formula directly.
        //
        // For pixel at (px, py) = (x+0.5, y+0.5):
        //   vx = px - p0.x, vy = py - p0.y
        //   proj = vx*ux + vy*uy
        //   perp = |-uy*vx + ux*vy|  = |vx*(-uy) + vy*ux|
        // cap_cov: proj in [0, len]
        //
        // Scanline loop: for y in [iy_start, iy_end):
        //   vy = (y+0.5) - p0.y
        //   For given vy, proj(x) = vx*ux + vy*uy, perp(x) = |vx*(-uy) + vy*ux|
        //   proj ranges from -0.5 to len+0.5 → vx in [(−0.5−vy*uy)/ux, (len+0.5−vy*uy)/ux]
        //   |perp| <= hw_plus_half → |vx*(-uy) + vy*ux| <= hw_plus_half
        //   Since -uy is large for X-major: vx in [(vy*ux - hw_plus_half)/(-uy), ...]
        //   → center_vx at perp=0 = vy*ux / (-uy) ... nope.
        //   Actually for X-major, the dominant sweep is along x.
        //   The x-span at scanline y: center_x = p0.x + vy*(dx/dy)  but |dx/dy| >> 1
        //   This gives a WIDE span, so we compute ix_start/ix_end from the proj bounds.
        //
        // Correct scanline bounds for X-major:
        //   proj(x, y) = vx*ux + vy*uy = [(x+0.5-p0.x)*ux + vy*uy]
        //   proj in (-0.5, len+0.5) → x in [((-0.5 - vy*uy)/ux + p0.x - 0.5),
        //                                    ((len+0.5 - vy*uy)/ux + p0.x - 0.5)]
        //   perp(x) = |(x+0.5-p0.x)*(-uy) + vy*ux| <= hw_plus_half
        //   Since |uy| < |ux| (X-major), this gives a wide x-span per scanline.
        // ─────────────────────────────────────────────────────────────────────

        // Scanline bounds in y: the line spans from y=min(p0.y,p1.y) to max
        // but we also pad by hw (half-width in screen space = hw/|ux| in x, hw_y in y)
        float hw_y_screen = hw_plus_half / std::abs(ux); // screen-space y half-width
        int32_t iy_start_xmaj = std::max(clamp_min_y,
            static_cast<int32_t>(std::floor(std::min(p0.y, p1.y) - hw_y_screen)));
        int32_t iy_end_xmaj   = std::min(clamp_max_y,
            static_cast<int32_t>(std::ceil(std::max(p0.y, p1.y) + hw_y_screen)));

        for (int32_t y = iy_start_xmaj; y < iy_end_xmaj; ++y) {
            float vy = (static_cast<float>(y) + 0.5f) - p0.y;

            // proj base for this scanline (no x term yet):  proj(x) = vx*ux + base_proj_y
            float base_proj_y  = vy * uy;
            // perp base for this scanline: perp(x) = vx*(-uy) + base_perp_y
            float base_perp_y  = vy * ux;

            // x-range from proj constraint: proj in (-0.5, len+0.5)
            // vx*ux ∈ (-0.5 - base_proj_y, len + 0.5 - base_proj_y)
            float vx_proj_lo, vx_proj_hi;
            if (ux > 0.0f) {
                vx_proj_lo = (-0.5f - base_proj_y) / ux;
                vx_proj_hi = (len + 0.5f - base_proj_y) / ux;
            } else {
                vx_proj_lo = (len + 0.5f - base_proj_y) / ux;
                vx_proj_hi = (-0.5f - base_proj_y) / ux;
            }

            // x-range from perp constraint: |vx*(-uy) + base_perp_y| <= hw_plus_half
            // vx*(-uy) ∈ (-hw_plus_half - base_perp_y, hw_plus_half - base_perp_y)
            float vx_perp_lo, vx_perp_hi;
            if (-uy > 0.0f) {
                vx_perp_lo = (-hw_plus_half - base_perp_y) / (-uy);
                vx_perp_hi = ( hw_plus_half - base_perp_y) / (-uy);
            } else {
                vx_perp_lo = ( hw_plus_half - base_perp_y) / (-uy);
                vx_perp_hi = (-hw_plus_half - base_perp_y) / (-uy);
            }

            // Intersect the two x-ranges, then convert vx → pixel x
            float vx_lo = std::max(vx_proj_lo, vx_perp_lo);
            float vx_hi = std::min(vx_proj_hi, vx_perp_hi);
            if (vx_lo >= vx_hi) continue;

            // pixel x = p0.x + vx - 0.5 → floor/ceil
            int32_t ix0 = std::max(clamp_min_x, static_cast<int32_t>(std::floor(p0.x + vx_lo - 0.5f)));
            int32_t ix1 = std::min(clamp_max_x, static_cast<int32_t>(std::ceil(p0.x + vx_hi + 0.5f)));
            if (ix0 >= ix1) continue;

            PremultipliedColorU8* row_ptr = dst.row(y);
            uint32_t* row_u32 = reinterpret_cast<uint32_t*>(row_ptr);
            float vx = (static_cast<float>(ix0) + 0.5f) - p0.x;

            if (is_solid) {
                for (int32_t x = ix0; x < ix1; ++x, vx += 1.0f) {
                    float proj = vx * ux + base_proj_y;
                    if (proj < -0.5f || proj > len + 0.5f) continue;
                    float perp = std::abs(vx * (-uy) + base_perp_y);
                    if (perp > hw_plus_half) continue;

                    float edge_cov = std::min(hw_plus_half - perp, 1.0f);
                    float cap_cov  = 1.0f;
                    if (proj < cap_start)    cap_cov = proj + 0.5f;
                    else if (proj > cap_end) cap_cov = len + 0.5f - proj;
                    float cov = edge_cov * cap_cov;
                    if (cov <= 0.005f) continue;

                    uint32_t* dp = &row_u32[x];
                    if (edge_cov >= 1.0f && cap_cov >= 1.0f) {
                        if (sc_a == 255) {
                            *dp = sc_packed;
                        } else {
                            uint32_t inv_a = 256 - sc_a;
                            uint32_t d  = *dp;
                            uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
                            uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
                            *dp = sc_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
                        }
                    } else {
                        blend_pixel_fast_solid(row_ptr + x, sc_r, sc_g, sc_b, sc_a, cov);
                    }
                }
            } else {
                for (int32_t x = ix0; x < ix1; ++x, vx += 1.0f) {
                    float proj = vx * ux + base_proj_y;
                    if (proj < -0.5f || proj > len + 0.5f) continue;
                    float perp = std::abs(vx * (-uy) + base_perp_y);
                    if (perp > hw_plus_half) continue;
                    float edge_cov = std::min(hw_plus_half - perp, 1.0f);
                    float cap_cov  = 1.0f;
                    if (proj < cap_start)    cap_cov = proj + 0.5f;
                    else if (proj > cap_end) cap_cov = len + 0.5f - proj;
                    float cov = edge_cov * cap_cov;
                    if (cov > 0.005f) blitter->blend_pixel(x, y, cov);
                }
            }
        }

    } else {
        // ─────────────────────────────────────────────────────────────
        // Y-MAJOR PATH: outer loop = rows, inner loop = 2-3 columns
        // KEY OPT: base_proj ≈ row's projection along line direction
        //          cap_cov is constant for all pixels in this row
        // ─────────────────────────────────────────────────────────────
        float slope_xy = dx / dy;
        float hw_x     = hw_plus_half / std::abs(uy);
        int32_t iy_start = std::max(clamp_min_y, static_cast<int32_t>(std::floor(std::min(p0.y, p1.y) - hw - 0.5f)));
        int32_t iy_end   = std::min(clamp_max_y, static_cast<int32_t>(std::ceil(std::max(p0.y, p1.y) + hw + 0.5f)));

        float vy0        = (static_cast<float>(iy_start) + 0.5f) - p0.y;
        float mid_x      = p0.x + vy0 * slope_xy;
        float base_proj  = vy0 * uy;
        float base_cross = vy0 * ux;

        for (int32_t y = iy_start; y < iy_end;
             ++y, mid_x += slope_xy, base_proj += uy, base_cross += ux)
        {
            if (base_proj < -0.5f || base_proj > len + 0.5f) continue;

            float row_cap_cov = 1.0f;
            if (base_proj < cap_start) {
                row_cap_cov = base_proj + 0.5f;
                if (row_cap_cov <= 0.005f) continue;
            } else if (base_proj > cap_end) {
                row_cap_cov = len + 0.5f - base_proj;
                if (row_cap_cov <= 0.005f) continue;
            }

            int32_t ix0 = std::max(clamp_min_x, static_cast<int32_t>(std::floor(mid_x - hw_x)));
            int32_t ix1 = std::min(clamp_max_x, static_cast<int32_t>(std::ceil(mid_x + hw_x)));
            if (ix0 >= ix1) continue;

            PremultipliedColorU8* row_ptr = dst.row(y);
            uint32_t* row_u32 = reinterpret_cast<uint32_t*>(row_ptr);
            float vx = (static_cast<float>(ix0) + 0.5f) - p0.x;

            if (is_solid) {
                for (int32_t x = ix0; x < ix1; ++x, vx += 1.0f) {
                    float perp = std::abs(base_cross - vx * uy);
                    if (perp > hw_plus_half) continue;

                    uint32_t* dp = &row_u32[x];
                    if (perp <= hw_inner) {
                        if (row_cap_cov >= 1.0f) {
                            if (sc_a == 255) {
                                *dp = sc_packed;
                            } else {
                                uint32_t inv_a = 256 - sc_a;
                                uint32_t d  = *dp;
                                uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
                                uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
                                *dp = sc_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
                            }
                        } else {
                            blend_pixel_fast_solid(row_ptr + x, sc_r, sc_g, sc_b, sc_a, row_cap_cov);
                        }
                    } else {
                        float cov = (hw_plus_half - perp) * row_cap_cov;
                        if (cov > 0.005f)
                            blend_pixel_fast_solid(row_ptr + x, sc_r, sc_g, sc_b, sc_a, cov);
                    }
                }
            } else {
                for (int32_t x = ix0; x < ix1; ++x, vx += 1.0f) {
                    float proj = base_proj + vx * ux;
                    if (proj < -0.5f || proj > len + 0.5f) continue;
                    float perp = std::abs(-vx * uy + base_cross);
                    if (perp > hw_plus_half) continue;
                    float edge_cov = std::min(hw_plus_half - perp, 1.0f);
                    float cap_cov  = 1.0f;
                    if (proj < cap_start)    cap_cov = proj + 0.5f;
                    else if (proj > cap_end) cap_cov = len + 0.5f - proj;
                    float cov = edge_cov * cap_cov;
                    if (cov > 0.005f) blitter->blend_pixel(x, y, cov);
                }
            }
        }
    }
}

void fill_circle(
    PixmapMut& dst,
    float cx, float cy, float radius,
    const Paint& paint,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (radius <= 0.0f) return;

    if (std::abs(transform.kx) < 1e-5f && std::abs(transform.ky) < 1e-5f &&
        std::abs(transform.sx - transform.sy) < 1e-4f) {
        float s = std::abs(transform.sx);
        cx = cx * transform.sx + transform.tx;
        cy = cy * transform.sy + transform.ty;
        radius *= s;
    } else if (!transform.is_identity()) {
        auto p = PathBuilder::from_circle(cx, cy, radius);
        if (p) fill_path(dst, *p, paint, FillRule::Winding, transform, mask, clip);
        return;
    }

    float dst_w = static_cast<float>(dst.width());
    float dst_h = static_cast<float>(dst.height());
    float clamp_min_x = clip ? static_cast<float>(clip->x()) : 0.0f;
    float clamp_max_x = clip ? static_cast<float>(clip->right()) : dst_w;
    float clamp_min_y = clip ? static_cast<float>(clip->y()) : 0.0f;
    float clamp_max_y = clip ? static_cast<float>(clip->bottom()) : dst_h;

    if (cx + radius < clamp_min_x || cx - radius >= clamp_max_x ||
        cy + radius < clamp_min_y || cy - radius >= clamp_max_y) {
        return;
    }

    std::optional<SubMaskRef> submask{};
    if (mask != nullptr) submask = mask->as_submask();
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    bool is_fast_mode = paint.is_solid_color() && !paint.is_linear_blending() && !paint.has_color_filter() && (mask == nullptr);
    bool is_source_over = is_fast_mode && (paint.blend_mode == BlendMode::SourceOver);
    PremultipliedColorU8 sc_val = is_fast_mode ? paint.shader.solid_color().premultiply().to_color_u8() : PremultipliedColorU8();
    uint32_t sc_r = sc_val.red();
    uint32_t sc_g = sc_val.green();
    uint32_t sc_b = sc_val.blue();
    uint32_t sc_a = sc_val.alpha();

    bool is_fast_pattern = false;
    const Pattern* pat_ptr = nullptr;
    int32_t pat_tx = 0;
    int32_t pat_ty = 0;
    if (paint.shader.type() == Shader::Type::Pattern &&
        !paint.is_linear_blending() && !paint.has_color_filter() && (mask == nullptr) &&
        (paint.blend_mode == BlendMode::SourceOver || paint.blend_mode == BlendMode::Source) &&
        paint.shader.is_opaque()) {
        const auto& pat = paint.shader.pattern();
        if (pat.inv_transform().has_value() && pat.pixmap().format() == dst.format()) {
            const auto& inv = *pat.inv_transform();
            if (std::abs(inv.sx - 1.0f) < 1e-5f && std::abs(inv.sy - 1.0f) < 1e-5f &&
                std::abs(inv.kx) < 1e-5f && std::abs(inv.ky) < 1e-5f) {
                float round_tx = std::round(inv.tx);
                float round_ty = std::round(inv.ty);
                if (std::abs(inv.tx - round_tx) < 1e-3f && std::abs(inv.ty - round_ty) < 1e-3f) {
                    is_fast_pattern = true;
                    pat_ptr = &pat;
                    pat_tx = static_cast<int32_t>(round_tx);
                    pat_ty = static_cast<int32_t>(round_ty);
                }
            }
        }
    }

    auto blend_circle_edge = [&](PremultipliedColorU8* p_dst, float cov) {
        if (is_source_over) {
            blend_pixel_fast_solid(p_dst, sc_r, sc_g, sc_b, sc_a, cov);
            return;
        }
        uint8_t cov_u8 = static_cast<uint8_t>(cov * 255.0f + 0.5f);
        if (cov_u8 == 0) return;
        if (paint.blend_mode == BlendMode::Clear) {
            *p_dst = simd::lerp_pixel(*p_dst, PremultipliedColorU8::TRANSPARENT, cov_u8);
        } else if (should_pre_scale_coverage(paint.blend_mode)) {
            uint32_t cv = static_cast<uint32_t>(cov_u8) + 1;
            PremultipliedColorU8 sc_mod = PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>((sc_r * cv) >> 8),
                static_cast<uint8_t>((sc_g * cv) >> 8),
                static_cast<uint8_t>((sc_b * cv) >> 8),
                static_cast<uint8_t>((sc_a * cv) >> 8)
            );
            *p_dst = simd::blend_pixel_by_mode(*p_dst, sc_mod, paint.blend_mode);
        } else {
            PremultipliedColorU8 target = simd::blend_pixel_by_mode(*p_dst, sc_val, paint.blend_mode);
            *p_dst = simd::lerp_pixel(*p_dst, target, cov_u8);
        }
    };

    int32_t y_start = std::max(static_cast<int32_t>(clamp_min_y), static_cast<int32_t>(std::floor(cy - radius)));
    int32_t y_end = std::min(static_cast<int32_t>(clamp_max_y), static_cast<int32_t>(std::ceil(cy + radius)));
    float r_sq = radius * radius;
    int32_t w_min = static_cast<int32_t>(clamp_min_x);
    int32_t w_limit = static_cast<int32_t>(clamp_max_x);

    for (int32_t y = y_start; y < y_end; ++y) {
        float py = static_cast<float>(y) + 0.5f;
        float dy = py - cy;
        float dy_sq = dy * dy;
        if (dy_sq >= r_sq) continue;

        float dx = std::sqrt(r_sq - dy_sq);
        float x0 = cx - dx;
        float x1 = cx + dx;

        int32_t ix0 = static_cast<int32_t>(std::floor(x0));
        int32_t ix1 = static_cast<int32_t>(std::floor(x1));
        PremultipliedColorU8* dst_row = dst.row(y);

        if (is_fast_pattern) {
            int32_t pw = static_cast<int32_t>(pat_ptr->pixmap().width());
            int32_t ph = static_cast<int32_t>(pat_ptr->pixmap().height());
            const PremultipliedColorU8* pat_pixels = pat_ptr->pixmap().pixels();
            SpreadMode p_mode = pat_ptr->spread_mode();

            int32_t sy = y + pat_ty;
            if (p_mode == SpreadMode::Pad) {
                sy = std::clamp(sy, 0, ph - 1);
            } else if (p_mode == SpreadMode::Repeat) {
                sy = (sy % ph + ph) % ph;
            }
            const PremultipliedColorU8* s_row = pat_pixels + sy * pw;

            auto sample_pat_pixel = [&](int32_t px) noexcept -> PremultipliedColorU8 {
                int32_t sx = px + pat_tx;
                if (p_mode == SpreadMode::Pad) {
                    sx = std::clamp(sx, 0, pw - 1);
                } else if (p_mode == SpreadMode::Repeat) {
                    sx = (sx % pw + pw) % pw;
                }
                return s_row[sx];
            };

            if (ix0 == ix1) {
                if (ix0 >= w_min && ix0 < w_limit) {
                    float cov = x1 - x0;
                    uint8_t cov_u8 = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                    dst_row[ix0] = simd::lerp_pixel(dst_row[ix0], sample_pat_pixel(ix0), cov_u8);
                }
                continue;
            }

            // Left AA pixel
            if (ix0 >= w_min && ix0 < w_limit) {
                float cov = (static_cast<float>(ix0 + 1) - x0);
                if (cov > 0.005f) {
                    uint8_t cov_u8 = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                    dst_row[ix0] = simd::lerp_pixel(dst_row[ix0], sample_pat_pixel(ix0), cov_u8);
                }
            }

            // Middle solid run
            int32_t span_start = std::max(w_min, ix0 + 1);
            int32_t span_end = std::min(w_limit, ix1);
            if (span_end > span_start) {
                size_t span_len = span_end - span_start;
                int32_t sx0 = span_start + pat_tx;
                if (p_mode == SpreadMode::Pad) {
                    if (sx0 >= 0 && sx0 + static_cast<int32_t>(span_len) <= pw) {
                        std::memcpy(dst_row + span_start, s_row + sx0, span_len * sizeof(PremultipliedColorU8));
                    } else {
                        int32_t sx_end = sx0 + static_cast<int32_t>(span_len);
                        int32_t left_len = std::max(0, -sx0);
                        int32_t clamp_left = std::min(left_len, static_cast<int32_t>(span_len));
                        if (clamp_left > 0) {
                            simd::fill_solid_span(dst_row + span_start, s_row[0], static_cast<size_t>(clamp_left));
                        }
                        int32_t mid_start = std::max(0, sx0);
                        int32_t mid_end = std::min(pw, sx_end);
                        if (mid_end > mid_start) {
                            size_t mid_len = static_cast<size_t>(mid_end - mid_start);
                            size_t dst_off = static_cast<size_t>(mid_start - sx0);
                            std::memcpy(dst_row + span_start + dst_off, s_row + mid_start, mid_len * sizeof(PremultipliedColorU8));
                        }
                        int32_t right_start = std::max(pw, sx0);
                        if (sx_end > right_start) {
                            size_t right_len = static_cast<size_t>(sx_end - right_start);
                            size_t dst_off = static_cast<size_t>(right_start - sx0);
                            simd::fill_solid_span(dst_row + span_start + dst_off, s_row[pw - 1], right_len);
                        }
                    }
                } else {
                    for (size_t k = 0; k < span_len; ++k) {
                        dst_row[span_start + k] = sample_pat_pixel(span_start + static_cast<int32_t>(k));
                    }
                }
            }

            // Right AA pixel
            if (ix1 >= w_min && ix1 < w_limit) {
                float cov = (x1 - static_cast<float>(ix1));
                if (cov > 0.005f) {
                    uint8_t cov_u8 = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                    dst_row[ix1] = simd::lerp_pixel(dst_row[ix1], sample_pat_pixel(ix1), cov_u8);
                }
            }
            continue;
        }

        if (ix0 == ix1) {
            if (ix0 >= w_min && ix0 < w_limit) {
                float cov = x1 - x0;
                if (is_fast_mode) {
                    blend_circle_edge(&dst_row[ix0], cov);
                } else {
                    blitter->blend_pixel(ix0, y, cov);
                }
            }
            continue;
        }

        // Left antialiased pixel
        if (ix0 >= w_min && ix0 < w_limit) {
            float cov = (static_cast<float>(ix0 + 1) - x0);
            if (cov > 0.005f) {
                if (is_fast_mode) {
                    blend_circle_edge(&dst_row[ix0], cov);
                } else {
                    blitter->blend_pixel(ix0, y, cov);
                }
            }
        }

        // Middle 100% solid run -> Direct SIMD!
        int32_t span_start = std::max(w_min, ix0 + 1);
        int32_t span_end = std::min(w_limit, ix1);
        if (span_end > span_start) {
            if (is_fast_mode) {
                simd::blend_solid_span_by_mode(dst_row + span_start, sc_val, paint.blend_mode, span_end - span_start);
            } else {
                blitter->blend_span(span_start, y, span_end - span_start, 1.0f);
            }
        }

        // Right antialiased pixel
        if (ix1 >= w_min && ix1 < w_limit) {
            float cov = (x1 - static_cast<float>(ix1));
            if (cov > 0.005f) {
                if (is_fast_mode) {
                    blend_circle_edge(&dst_row[ix1], cov);
                } else {
                    blitter->blend_pixel(ix1, y, cov);
                }
            }
        }
    }
}

void stroke_circle(
    PixmapMut& dst,
    float cx, float cy, float radius,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (radius <= 0.0f || stroke.width <= 0.0f) return;

    if (stroke.dash.has_value()) {
        auto p = PathBuilder::from_circle(cx, cy, radius);
        if (p) stroke_path(dst, *p, paint, stroke, transform, mask, clip);
        return;
    }

    float stroke_width = stroke.width;
    if (std::abs(transform.kx) < 1e-5f && std::abs(transform.ky) < 1e-5f &&
        std::abs(transform.sx - transform.sy) < 1e-4f) {
        float s = std::abs(transform.sx);
        cx = cx * transform.sx + transform.tx;
        cy = cy * transform.sy + transform.ty;
        radius *= s;
        stroke_width *= s;
    } else if (!transform.is_identity()) {
        auto p = PathBuilder::from_circle(cx, cy, radius);
        if (p) stroke_path(dst, *p, paint, stroke, transform, mask, clip);
        return;
    }

    float half_w = stroke_width * 0.5f;
    float r_out = radius + half_w;
    float r_in  = radius - half_w;

    if (r_in <= 0.0f) {
        fill_circle(dst, cx, cy, r_out, paint, Transform(), mask, clip);
        return;
    }

    float dst_w = static_cast<float>(dst.width());
    float dst_h = static_cast<float>(dst.height());
    float clamp_min_x = clip ? static_cast<float>(clip->x()) : 0.0f;
    float clamp_max_x = clip ? static_cast<float>(clip->right()) : dst_w;
    float clamp_min_y = clip ? static_cast<float>(clip->y()) : 0.0f;
    float clamp_max_y = clip ? static_cast<float>(clip->bottom()) : dst_h;

    if (cx + r_out < clamp_min_x || cx - r_out >= clamp_max_x ||
        cy + r_out < clamp_min_y || cy - r_out >= clamp_max_y) {
        return;
    }

    std::optional<SubMaskRef> submask{};
    if (mask != nullptr) submask = mask->as_submask();
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    bool is_solid = paint.is_solid_color() && !paint.is_linear_blending() && !paint.has_color_filter() && (mask == nullptr) && (paint.blend_mode == BlendMode::SourceOver);
    PremultipliedColorU8 sc_val = is_solid ? paint.shader.solid_color().premultiply().to_color_u8() : PremultipliedColorU8();
    uint32_t sc_r = sc_val.red();
    uint32_t sc_g = sc_val.green();
    uint32_t sc_b = sc_val.blue();
    uint32_t sc_a = sc_val.alpha();

    int32_t y_start = std::max(static_cast<int32_t>(clamp_min_y), static_cast<int32_t>(std::floor(cy - r_out)));
    int32_t y_end   = std::min(static_cast<int32_t>(clamp_max_y), static_cast<int32_t>(std::ceil(cy + r_out)));
    float r_out_sq = r_out * r_out;
    float r_in_sq  = r_in * r_in;
    int32_t w_min = static_cast<int32_t>(clamp_min_x);
    int32_t w_limit = static_cast<int32_t>(clamp_max_x);

    auto blend_px = [&](PremultipliedColorU8* row, int32_t x, int32_t y, float cov) {
        if (x < w_min || x >= w_limit || cov <= 0.005f) return;
        cov = std::min(cov, 1.0f);
        if (is_solid) {
            blend_pixel_fast_solid(&row[x], sc_r, sc_g, sc_b, sc_a, cov);
        } else {
            blitter->blend_pixel(x, y, cov);
        }
    };

    auto blend_run = [&](PremultipliedColorU8* row, int32_t x0, int32_t x1, int32_t y) {
        int32_t s0 = std::max(w_min, x0);
        int32_t s1 = std::min(w_limit, x1);
        if (s1 > s0) {
            if (is_solid) {
                simd::blend_solid_source_over(row + s0, sc_val, s1 - s0);
            } else {
                blitter->blend_span(s0, y, s1 - s0, 1.0f);
            }
        }
    };

    for (int32_t y = y_start; y < y_end; ++y) {
        float py = static_cast<float>(y) + 0.5f;
        float dy = py - cy;
        float dy_sq = dy * dy;
        if (dy_sq >= r_out_sq) continue;

        float dx_out = std::sqrt(r_out_sq - dy_sq);
        PremultipliedColorU8* dst_row = dst.row(y);

        if (dy_sq >= r_in_sq) {
            // Cap chord: single interval [cx - dx_out, cx + dx_out]
            float x0 = cx - dx_out;
            float x1 = cx + dx_out;
            int32_t ix0 = static_cast<int32_t>(std::floor(x0));
            int32_t ix1 = static_cast<int32_t>(std::floor(x1));
            if (ix0 == ix1) {
                blend_px(dst_row, ix0, y, x1 - x0);
            } else {
                blend_px(dst_row, ix0, y, static_cast<float>(ix0 + 1) - x0);
                blend_run(dst_row, ix0 + 1, ix1, y);
                blend_px(dst_row, ix1, y, x1 - static_cast<float>(ix1));
            }
        } else {
            // Annular ring with inner hole
            float dx_in = std::sqrt(r_in_sq - dy_sq);
            float x0 = cx - dx_out;
            float x1 = cx - dx_in;
            float x2 = cx + dx_in;
            float x3 = cx + dx_out;

            int32_t ix0 = static_cast<int32_t>(std::floor(x0));
            int32_t ix1 = static_cast<int32_t>(std::floor(x1));
            int32_t ix2 = static_cast<int32_t>(std::floor(x2));
            int32_t ix3 = static_cast<int32_t>(std::floor(x3));

            if (ix1 < ix2) {
                // Two disjoint spans
                // Left span [x0, x1]
                if (ix0 == ix1) {
                    blend_px(dst_row, ix0, y, x1 - x0);
                } else {
                    blend_px(dst_row, ix0, y, static_cast<float>(ix0 + 1) - x0);
                    blend_run(dst_row, ix0 + 1, ix1, y);
                    blend_px(dst_row, ix1, y, x1 - static_cast<float>(ix1));
                }
                // Right span [x2, x3]
                if (ix2 == ix3) {
                    blend_px(dst_row, ix2, y, x3 - x2);
                } else {
                    blend_px(dst_row, ix2, y, static_cast<float>(ix2 + 1) - x2);
                    blend_run(dst_row, ix2 + 1, ix3, y);
                    blend_px(dst_row, ix3, y, x3 - static_cast<float>(ix3));
                }
            } else {
                // ix1 == ix2: Inner hole is contained within a single pixel
                int32_t p_hole = ix1;
                if (ix0 == ix3) {
                    blend_px(dst_row, ix0, y, (x1 - x0) + (x3 - x2));
                } else {
                    if (ix0 == p_hole) {
                        blend_px(dst_row, p_hole, y, (x1 - x0) + (static_cast<float>(p_hole + 1) - x2));
                        blend_run(dst_row, p_hole + 1, ix3, y);
                        blend_px(dst_row, ix3, y, x3 - static_cast<float>(ix3));
                    } else if (ix3 == p_hole) {
                        blend_px(dst_row, ix0, y, static_cast<float>(ix0 + 1) - x0);
                        blend_run(dst_row, ix0 + 1, p_hole, y);
                        blend_px(dst_row, p_hole, y, (x1 - static_cast<float>(p_hole)) + (x3 - x2));
                    } else {
                        blend_px(dst_row, ix0, y, static_cast<float>(ix0 + 1) - x0);
                        blend_run(dst_row, ix0 + 1, p_hole, y);
                        blend_px(dst_row, p_hole, y, 1.0f - (x2 - x1));
                        blend_run(dst_row, p_hole + 1, ix3, y);
                        blend_px(dst_row, ix3, y, x3 - static_cast<float>(ix3));
                    }
                }
            }
        }
    }
}

void fill_round_rect(
    PixmapMut& dst,
    const Rect& rect,
    float rx, float ry,
    const Paint& paint,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (rect.width() <= 0.0f || rect.height() <= 0.0f) return;

    if (!transform.is_identity()) {
        if (std::abs(transform.kx) < 1e-5f && std::abs(transform.ky) < 1e-5f) {
            float x0 = rect.left() * transform.sx + transform.tx;
            float x1 = rect.right() * transform.sx + transform.tx;
            float y0 = rect.top() * transform.sy + transform.ty;
            float y1 = rect.bottom() * transform.sy + transform.ty;
            rx *= std::abs(transform.sx);
            ry *= std::abs(transform.sy);
            auto r = Rect::from_ltrb(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
            if (r) fill_round_rect(dst, *r, rx, ry, paint, Transform(), mask, clip);
            return;
        } else {
            float l = rect.left();
            float r = rect.right();
            float t = rect.top();
            float b = rect.bottom();
            float k = 0.5522847498f;
            float kx = rx * k;
            float ky = ry * k;

            auto map_pt = [&](float x, float y) noexcept {
                Point pt = Point::from_xy(x, y);
                transform.map_point(pt);
                return pt;
            };

            PathBuilder builder;
            builder.reserve(10, 16);
            Point p0 = map_pt(l + rx, t);
            Point p1 = map_pt(r - rx, t);
            Point c1 = map_pt(r - rx + kx, t);
            Point c2 = map_pt(r, t + ry - ky);
            Point p2 = map_pt(r, t + ry);
            Point p3 = map_pt(r, b - ry);
            Point c3 = map_pt(r, b - ry + ky);
            Point c4 = map_pt(r - rx + kx, b);
            Point p4 = map_pt(r - rx, b);
            Point p5 = map_pt(l + rx, b);
            Point c5 = map_pt(l + rx - kx, b);
            Point c6 = map_pt(l, b - ry + ky);
            Point p6 = map_pt(l, b - ry);
            Point p7 = map_pt(l, t + ry);
            Point c7 = map_pt(l, t + ry - ky);
            Point c8 = map_pt(l + rx - kx, t);

            builder.move_to(p0);
            builder.line_to(p1);
            builder.cubic_to(c1, c2, p2);
            builder.line_to(p3);
            builder.cubic_to(c3, c4, p4);
            builder.line_to(p5);
            builder.cubic_to(c5, c6, p6);
            builder.line_to(p7);
            builder.cubic_to(c7, c8, p0);
            builder.close();

            auto transformed_path = builder.finish();
            if (transformed_path) {
                Paint transformed_paint = paint;
                transformed_paint.shader.transform(transform);
                fill_path(dst, *transformed_path, transformed_paint, FillRule::Winding, Transform(), mask, clip);
            }
            return;
        }
    }

    rx = std::min(rx, rect.width() * 0.5f);
    ry = std::min(ry, rect.height() * 0.5f);

    if (rx < 0.5f || ry < 0.5f) {
        fill_rect(dst, rect, paint, transform, mask, clip);
        return;
    }

    float x0 = rect.left();
    float x1 = rect.right();
    float y0 = rect.top();
    float y1 = rect.bottom();

    // 1. Middle solid band (y0 + ry to y1 - ry) - bulk of the card!
    if (y1 - ry > y0 + ry) {
        render_axis_aligned_rect_fast(dst, x0, y0 + ry, x1, y1 - ry, paint, mask, clip);
    }

    std::optional<SubMaskRef> submask{};
    if (mask != nullptr) submask = mask->as_submask();
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    int32_t clamp_min_x = clip ? static_cast<int32_t>(clip->x()) : 0;
    int32_t clamp_max_x = clip ? static_cast<int32_t>(clip->right()) : static_cast<int32_t>(dst.width());
    int32_t clamp_min_y = clip ? static_cast<int32_t>(clip->y()) : 0;
    int32_t clamp_max_y = clip ? static_cast<int32_t>(clip->bottom()) : static_cast<int32_t>(dst.height());

    auto raster_cap = [&](float cy_corner, float y_min, float y_max) {
        int32_t iy_start = std::max(clamp_min_y, static_cast<int32_t>(std::floor(y_min)));
        int32_t iy_end = std::min(clamp_max_y, static_cast<int32_t>(std::ceil(y_max)));
        float cx_left = x0 + rx;
        float cx_right = x1 - rx;

        for (int32_t y = iy_start; y < iy_end; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            float dy = std::abs(py - cy_corner);
            if (dy >= ry) {
                float y_cov = std::clamp(ry + 0.5f - dy, 0.0f, 1.0f);
                if (y_cov > 0.005f && cx_right > cx_left) {
                    int32_t sp_start = std::max(clamp_min_x, static_cast<int32_t>(std::floor(cx_left)));
                    int32_t sp_end = std::min(clamp_max_x, static_cast<int32_t>(std::ceil(cx_right)));
                    if (sp_end > sp_start) {
                        blitter->blend_span(sp_start, y, static_cast<uint32_t>(sp_end - sp_start), y_cov);
                    }
                }
                continue;
            }

            float t = dy / ry;
            float dx = rx * std::sqrt(std::max(0.0f, 1.0f - t * t));
            float left_edge = cx_left - dx;
            float right_edge = cx_right + dx;

            int32_t ix_min = std::max(clamp_min_x, static_cast<int32_t>(std::floor(left_edge - 0.5f)));
            int32_t ix_max = std::min(clamp_max_x, static_cast<int32_t>(std::ceil(right_edge + 0.5f)));
            int32_t ix_in_left = std::clamp(static_cast<int32_t>(std::ceil(left_edge)), ix_min, ix_max);
            int32_t ix_in_right = std::clamp(static_cast<int32_t>(std::floor(right_edge)), ix_min, ix_max);

            for (int32_t x = ix_min; x < ix_in_left; ++x) {
                float px = static_cast<float>(x) + 0.5f;
                float ndx = (px - cx_left) / rx;
                float dist = std::sqrt(ndx * ndx + t * t);
                float cov = std::clamp(1.0f + 0.5f / rx - dist, 0.0f, 1.0f);
                if (cov > 0.005f) blitter->blend_pixel(x, y, cov);
            }

            if (ix_in_right > ix_in_left) {
                blitter->blend_span(ix_in_left, y, ix_in_right - ix_in_left, 1.0f);
            }

            for (int32_t x = std::max(ix_in_right, ix_in_left); x < ix_max; ++x) {
                float px = static_cast<float>(x) + 0.5f;
                float ndx = (px - cx_right) / rx;
                float dist = std::sqrt(ndx * ndx + t * t);
                float cov = std::clamp(1.0f + 0.5f / rx - dist, 0.0f, 1.0f);
                if (cov > 0.005f) blitter->blend_pixel(x, y, cov);
            }
        }
    };

    raster_cap(y0 + ry, y0, y0 + ry);
    raster_cap(y1 - ry, y1 - ry, y1);
}

void stroke_round_rect(
    PixmapMut& dst,
    const Rect& rect,
    float rx, float ry,
    const Paint& paint,
    const Stroke& stroke,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (stroke.width <= 0.0f || rect.width() <= 0.0f || rect.height() <= 0.0f) return;

    if (!transform.is_identity() || stroke.dash.has_value() || std::abs(rx - ry) > 1e-4f) {
        auto p = PathBuilder::from_rounded_rect(rect, rx, ry);
        if (p) stroke_path(dst, *p, paint, stroke, transform, mask, clip);
        return;
    }

    rx = std::min(rx, rect.width() * 0.5f);
    ry = std::min(ry, rect.height() * 0.5f);

    float hw = stroke.width * 0.5f;
    float x0 = rect.left();
    float x1 = rect.right();
    float y0 = rect.top();
    float y1 = rect.bottom();

    // 4 straight bars
    if (x1 - rx > x0 + rx) {
        render_axis_aligned_rect_fast(dst, x0 + rx, y0 - hw, x1 - rx, y0 + hw, paint, mask, clip);
        render_axis_aligned_rect_fast(dst, x0 + rx, y1 - hw, x1 - rx, y1 + hw, paint, mask, clip);
    }
    if (y1 - ry > y0 + ry) {
        render_axis_aligned_rect_fast(dst, x0 - hw, y0 + ry, x0 + hw, y1 - ry, paint, mask, clip);
        render_axis_aligned_rect_fast(dst, x1 - hw, y0 + ry, x1 + hw, y1 - ry, paint, mask, clip);
    }

    // 4 Corner Arcs
    std::optional<SubMaskRef> submask{};
    if (mask != nullptr) submask = mask->as_submask();
    SubPixmapMut subpix = dst.as_subpixmap();
    auto blitter = PipelineBlitter::create(paint, submask, subpix);
    if (!blitter) return;

    int32_t clamp_min_x = clip ? static_cast<int32_t>(clip->x()) : 0;
    int32_t clamp_max_x = clip ? static_cast<int32_t>(clip->right()) : static_cast<int32_t>(dst.width());
    int32_t clamp_min_y = clip ? static_cast<int32_t>(clip->y()) : 0;
    int32_t clamp_max_y = clip ? static_cast<int32_t>(clip->bottom()) : static_cast<int32_t>(dst.height());

    auto raster_corner_arc = [&](float cx, float cy, float x_sign, float y_sign) {
        float r_out = rx + hw;
        float r_in = std::max(0.0f, rx - hw);
        int32_t iy0 = std::max(clamp_min_y, static_cast<int32_t>(std::floor(cy + (y_sign < 0 ? -r_out : 0.0f))));
        int32_t iy1 = std::min(clamp_max_y, static_cast<int32_t>(std::ceil(cy + (y_sign < 0 ? 0.0f : r_out))));

        for (int32_t y = iy0; y < iy1; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            float dy = py - cy;
            if ((y_sign < 0 && dy > 0.0f) || (y_sign > 0 && dy < 0.0f)) continue;

            int32_t ix0 = std::max(clamp_min_x, static_cast<int32_t>(std::floor(cx + (x_sign < 0 ? -r_out : 0.0f))));
            int32_t ix1 = std::min(clamp_max_x, static_cast<int32_t>(std::ceil(cx + (x_sign < 0 ? 0.0f : r_out))));

            for (int32_t x = ix0; x < ix1; ++x) {
                float px = static_cast<float>(x) + 0.5f;
                float dx = px - cx;
                if ((x_sign < 0 && dx > 0.0f) || (x_sign > 0 && dx < 0.0f)) continue;

                float dist = std::sqrt(dx * dx + dy * dy);
                float cov_out = std::clamp(r_out + 0.5f - dist, 0.0f, 1.0f);
                float cov_in = std::clamp(dist - (r_in - 0.5f), 0.0f, 1.0f);
                float cov = cov_out * cov_in;
                if (cov > 0.005f) blitter->blend_pixel(x, y, cov);
            }
        }
    };

    raster_corner_arc(x0 + rx, y0 + ry, -1.0f, -1.0f);
    raster_corner_arc(x1 - rx, y0 + ry,  1.0f, -1.0f);
    raster_corner_arc(x0 + rx, y1 - ry, -1.0f,  1.0f);
    raster_corner_arc(x1 - rx, y1 - ry,  1.0f,  1.0f);
}

void draw_pixmap(
    PixmapMut& dst,
    int32_t x,
    int32_t y,
    const PixmapRef& src,
    const PixmapPaint& paint,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (paint.tint != Color::WHITE) {
        auto sr = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(src.width()), static_cast<float>(src.height()));
        auto dr = Rect::from_xywh(static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(src.width()), static_cast<float>(src.height()));
        if (sr && dr) {
            draw_pixmap_rect(dst, src, *sr, *dr, paint, transform, mask, clip);
        }
        return;
    }

    if (transform.is_identity() && mask == nullptr &&
        src.format() == dst.format() &&
        (dst.format() == PixelFormat::RGBA8888 || dst.format() == PixelFormat::BGRA8888)) {
        int32_t clip_x0 = 0;
        int32_t clip_y0 = 0;
        int32_t clip_x1 = static_cast<int32_t>(dst.width());
        int32_t clip_y1 = static_cast<int32_t>(dst.height());

        if (clip) {
            clip_x0 = std::max(clip_x0, static_cast<int32_t>(clip->left()));
            clip_y0 = std::max(clip_y0, static_cast<int32_t>(clip->top()));
            clip_x1 = std::min(clip_x1, static_cast<int32_t>(clip->right()));
            clip_y1 = std::min(clip_y1, static_cast<int32_t>(clip->bottom()));
        }

        int32_t dst_x0 = std::max(x, clip_x0);
        int32_t dst_y0 = std::max(y, clip_y0);
        int32_t dst_x1 = std::min(x + static_cast<int32_t>(src.width()), clip_x1);
        int32_t dst_y1 = std::min(y + static_cast<int32_t>(src.height()), clip_y1);

        if (dst_x0 < dst_x1 && dst_y0 < dst_y1) {
            int32_t copy_w = dst_x1 - dst_x0;
            int32_t src_x0 = dst_x0 - x;

            bool can_memcpy = (paint.blend_mode == BlendMode::Source ||
                              (paint.blend_mode == BlendMode::SourceOver && src.is_opaque())) &&
                              paint.opacity >= 0.999f;
            if (can_memcpy) {
                for (int32_t dy = dst_y0; dy < dst_y1; ++dy) {
                    int32_t sy = dy - y;
                    const PremultipliedColorU8* src_row = src.row(static_cast<size_t>(sy)) + src_x0;
                    PremultipliedColorU8* dst_row = dst.row(static_cast<size_t>(dy)) + dst_x0;
                    std::memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * sizeof(PremultipliedColorU8));
                }
                return;
            }

            if (paint.blend_mode == BlendMode::SourceOver) {
                if (paint.opacity >= 0.999f) {
                    for (int32_t dy = dst_y0; dy < dst_y1; ++dy) {
                        int32_t sy = dy - y;
                        const PremultipliedColorU8* src_row = src.row(static_cast<size_t>(sy)) + src_x0;
                        PremultipliedColorU8* dst_row = dst.row(static_cast<size_t>(dy)) + dst_x0;
                        simd::blend_source_over_span(dst_row, src_row, static_cast<size_t>(copy_w));
                    }
                    return;
                } else if (paint.opacity > 0.001f) {
                    uint8_t op_u8 = static_cast<uint8_t>(paint.opacity * 255.0f + 0.5f);
                    for (int32_t dy = dst_y0; dy < dst_y1; ++dy) {
                        int32_t sy = dy - y;
                        const PremultipliedColorU8* src_row = src.row(static_cast<size_t>(sy)) + src_x0;
                        PremultipliedColorU8* dst_row = dst.row(static_cast<size_t>(dy)) + dst_x0;
                        simd::blend_source_over_span_coverage(dst_row, src_row, op_u8, static_cast<size_t>(copy_w));
                    }
                    return;
                } else {
                    return; // Fully transparent opacity
                }
            }
        }
    }

    // Direct axis-aligned image scaler fastpath (bypasses Shader and fill_rect overhead)
    if (mask == nullptr && std::abs(transform.kx) < 1e-5f && std::abs(transform.ky) < 1e-5f &&
        transform.sx > 0.0f && transform.sy > 0.0f &&
        (paint.blend_mode == BlendMode::Source || (paint.blend_mode == BlendMode::SourceOver && src.is_opaque())) &&
        paint.opacity >= 0.999f &&
        src.format() == dst.format() &&
        (dst.format() == PixelFormat::RGBA8888 || dst.format() == PixelFormat::BGRA8888)) {

        float dst_fx0 = static_cast<float>(x) * transform.sx + transform.tx;
        float dst_fy0 = static_cast<float>(y) * transform.sy + transform.ty;
        float dst_fw = static_cast<float>(src.width()) * transform.sx;
        float dst_fh = static_cast<float>(src.height()) * transform.sy;
        float dst_fx1 = dst_fx0 + dst_fw;
        float dst_fy1 = dst_fy0 + dst_fh;

        int32_t clip_x0 = 0;
        int32_t clip_y0 = 0;
        int32_t clip_x1 = static_cast<int32_t>(dst.width());
        int32_t clip_y1 = static_cast<int32_t>(dst.height());

        if (clip) {
            clip_x0 = std::max(clip_x0, static_cast<int32_t>(clip->left()));
            clip_y0 = std::max(clip_y0, static_cast<int32_t>(clip->top()));
            clip_x1 = std::min(clip_x1, static_cast<int32_t>(clip->right()));
            clip_y1 = std::min(clip_y1, static_cast<int32_t>(clip->bottom()));
        }

        int32_t out_x0 = std::max(clip_x0, static_cast<int32_t>(std::floor(dst_fx0)));
        int32_t out_y0 = std::max(clip_y0, static_cast<int32_t>(std::floor(dst_fy0)));
        int32_t out_x1 = std::min(clip_x1, static_cast<int32_t>(std::ceil(dst_fx1)));
        int32_t out_y1 = std::min(clip_y1, static_cast<int32_t>(std::ceil(dst_fy1)));

        if (out_x1 > out_x0 && out_y1 > out_y0) {
            float inv_sx = 1.0f / transform.sx;
            float inv_sy = 1.0f / transform.sy;
            int32_t sw = static_cast<int32_t>(src.width());
            int32_t sh = static_cast<int32_t>(src.height());

            if (paint.quality == FilterQuality::Bilinear) {
                struct HWeight {
                    int32_t x0;
                    int32_t x1;
#if defined(NISABA_HAS_SSE2)
                    __m128i v_ifu;
                    __m128i v_inv_ifu;
#else
                    uint32_t ifu;
                    uint32_t inv_ifu;
#endif
                };
#if defined(NISABA_HAS_AVX2)
                struct HWeight4 {
                    int32_t x0[4];
                    int32_t x1[4];
                    __m256i v_ifu;
                    __m256i v_inv_ifu;
                };
#endif
                int32_t w_span = out_x1 - out_x0;
                static thread_local std::vector<HWeight> tl_h_weights;
                if (tl_h_weights.size() < static_cast<size_t>(w_span)) {
                    tl_h_weights.resize(static_cast<size_t>(w_span));
                }
                HWeight* h_weights = tl_h_weights.data();
                for (int32_t dx = out_x0; dx < out_x1; ++dx) {
                    float sx = (static_cast<float>(dx) + 0.5f - dst_fx0) * inv_sx - 0.5f;
                    int32_t isx = static_cast<int32_t>(std::floor(sx));
                    float fu = sx - static_cast<float>(isx);
                    uint32_t ifu = static_cast<uint32_t>(std::clamp(fu * 256.0f, 0.0f, 256.0f));
                    int32_t idx = dx - out_x0;
                    h_weights[idx].x0 = std::clamp(isx, 0, sw - 1);
                    h_weights[idx].x1 = std::clamp(isx + 1, 0, sw - 1);
#if defined(NISABA_HAS_SSE2)
                    h_weights[idx].v_ifu = _mm_set1_epi16(static_cast<int16_t>(ifu));
                    h_weights[idx].v_inv_ifu = _mm_set1_epi16(static_cast<int16_t>(256 - ifu));
#else
                    h_weights[idx].ifu = ifu;
                    h_weights[idx].inv_ifu = 256 - ifu;
#endif
                }
#if defined(NISABA_HAS_AVX2)
                size_t num_chunks4 = static_cast<size_t>(w_span / 4);
                static thread_local std::vector<HWeight4> tl_h_weights4;
                if (tl_h_weights4.size() < num_chunks4) {
                    tl_h_weights4.resize(num_chunks4);
                }
                HWeight4* h_weights4 = tl_h_weights4.data();
                for (size_t c = 0; c < num_chunks4; ++c) {
                    size_t base = c * 4;
                    alignas(32) int16_t u_vals[16];
                    alignas(32) int16_t inv_u_vals[16];
                    for (int k = 0; k < 4; ++k) {
                        h_weights4[c].x0[k] = h_weights[base + k].x0;
                        h_weights4[c].x1[k] = h_weights[base + k].x1;
                        int32_t dx = out_x0 + static_cast<int32_t>(base + k);
                        float sx = (static_cast<float>(dx) + 0.5f - dst_fx0) * inv_sx - 0.5f;
                        int32_t isx = static_cast<int32_t>(std::floor(sx));
                        float fu = sx - static_cast<float>(isx);
                        int16_t ifu = static_cast<int16_t>(std::clamp(fu * 256.0f, 0.0f, 256.0f));
                        int16_t inv_ifu = static_cast<int16_t>(256 - ifu);
                        for (int ch = 0; ch < 4; ++ch) {
                            u_vals[k * 4 + ch] = ifu;
                            inv_u_vals[k * 4 + ch] = inv_ifu;
                        }
                    }
                    h_weights4[c].v_ifu = _mm256_load_si256(reinterpret_cast<const __m256i*>(u_vals));
                    h_weights4[c].v_inv_ifu = _mm256_load_si256(reinterpret_cast<const __m256i*>(inv_u_vals));
                }
#endif

                for (int32_t dy = out_y0; dy < out_y1; ++dy) {
                    float sy = (static_cast<float>(dy) + 0.5f - dst_fy0) * inv_sy - 0.5f;
                    int32_t isy = static_cast<int32_t>(std::floor(sy));
                    float fv = sy - static_cast<float>(isy);
                    uint32_t ifv = static_cast<uint32_t>(std::clamp(fv * 256.0f, 0.0f, 256.0f));
                    uint32_t inv_ifv = 256 - ifv;

                    int32_t y0 = std::clamp(isy, 0, sh - 1);
                    int32_t y1 = std::clamp(isy + 1, 0, sh - 1);
                    const PremultipliedColorU8* src_row0 = src.row(static_cast<size_t>(y0));
                    const PremultipliedColorU8* src_row1 = src.row(static_cast<size_t>(y1));
                    PremultipliedColorU8* dst_row = dst.row(static_cast<size_t>(dy)) + out_x0;

#if defined(NISABA_HAS_AVX2)
                    __m256i v_ifv256 = _mm256_set1_epi16(static_cast<int16_t>(ifv));
                    __m256i v_inv_ifv256 = _mm256_set1_epi16(static_cast<int16_t>(inv_ifv));
                    __m128i zero128 = _mm_setzero_si128();

                    int32_t i = 0;
                    size_t c_idx = 0;
                    for (; i + 4 <= w_span; i += 4, ++c_idx) {
                        const auto& hw4 = h_weights4[c_idx];

                        uint32_t p00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x0[0]);
                        uint32_t q00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x0[1]);
                        uint32_t r00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x0[2]);
                        uint32_t s00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x0[3]);

                        uint32_t p10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x1[0]);
                        uint32_t q10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x1[1]);
                        uint32_t r10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x1[2]);
                        uint32_t s10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw4.x1[3]);

                        uint32_t p01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x0[0]);
                        uint32_t q01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x0[1]);
                        uint32_t r01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x0[2]);
                        uint32_t s01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x0[3]);

                        uint32_t p11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x1[0]);
                        uint32_t q11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x1[1]);
                        uint32_t r11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x1[2]);
                        uint32_t s11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw4.x1[3]);

                        __m128i row0_0 = _mm_setr_epi32(static_cast<int>(p00), static_cast<int>(q00), static_cast<int>(r00), static_cast<int>(s00));
                        __m128i row0_1 = _mm_setr_epi32(static_cast<int>(p10), static_cast<int>(q10), static_cast<int>(r10), static_cast<int>(s10));
                        __m128i row1_0 = _mm_setr_epi32(static_cast<int>(p01), static_cast<int>(q01), static_cast<int>(r01), static_cast<int>(s01));
                        __m128i row1_1 = _mm_setr_epi32(static_cast<int>(p11), static_cast<int>(q11), static_cast<int>(r11), static_cast<int>(s11));

                        __m256i c00 = _mm256_cvtepu8_epi16(row0_0);
                        __m256i c10 = _mm256_cvtepu8_epi16(row0_1);
                        __m256i c01 = _mm256_cvtepu8_epi16(row1_0);
                        __m256i c11 = _mm256_cvtepu8_epi16(row1_1);

                        __m256i top = _mm256_srli_epi16(_mm256_add_epi16(
                            _mm256_mullo_epi16(c00, hw4.v_inv_ifu),
                            _mm256_mullo_epi16(c10, hw4.v_ifu)), 8);
                        __m256i bot = _mm256_srli_epi16(_mm256_add_epi16(
                            _mm256_mullo_epi16(c01, hw4.v_inv_ifu),
                            _mm256_mullo_epi16(c11, hw4.v_ifu)), 8);

                        __m256i res16 = _mm256_srli_epi16(_mm256_add_epi16(
                            _mm256_mullo_epi16(top, v_inv_ifv256),
                            _mm256_mullo_epi16(bot, v_ifv256)), 8);

                        __m128i res_lo = _mm256_castsi256_si128(res16);
                        __m128i res_hi = _mm256_extracti128_si256(res16, 1);
                        __m128i out8 = _mm_packus_epi16(res_lo, res_hi);
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_row + i), out8);
                    }
                    for (; i < w_span; ++i) {
                        const auto& hw = h_weights[i];
                        uint32_t p00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw.x0);
                        uint32_t p10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw.x1);
                        uint32_t p01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw.x0);
                        uint32_t p11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw.x1);

                        __m128i c00_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p00), zero128);
                        __m128i c10_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p10), zero128);
                        __m128i c01_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p01), zero128);
                        __m128i c11_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p11), zero128);

                        __m128i top = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c00_16, hw.v_inv_ifu), _mm_mullo_epi16(c10_16, hw.v_ifu)), 8);
                        __m128i bot = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c01_16, hw.v_inv_ifu), _mm_mullo_epi16(c11_16, hw.v_ifu)), 8);

                        __m128i v_ifv = _mm_set1_epi16(static_cast<int16_t>(ifv));
                        __m128i v_inv_ifv = _mm_set1_epi16(static_cast<int16_t>(inv_ifv));
                        __m128i res16 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top, v_inv_ifv), _mm_mullo_epi16(bot, v_ifv)), 8);
                        __m128i res8 = _mm_packus_epi16(res16, zero128);
                        *reinterpret_cast<uint32_t*>(dst_row + i) = _mm_cvtsi128_si32(res8);
                    }
#elif defined(NISABA_HAS_SSE2)
                    __m128i zero = _mm_setzero_si128();
                    __m128i v_ifv = _mm_set1_epi16(static_cast<int16_t>(ifv));
                    __m128i v_inv_ifv = _mm_set1_epi16(static_cast<int16_t>(inv_ifv));

                    for (int32_t i = 0; i < w_span; ++i) {
                        const auto& hw = h_weights[i];

                        uint32_t p00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw.x0);
                        uint32_t p10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw.x1);
                        uint32_t p01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw.x0);
                        uint32_t p11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw.x1);

                        __m128i c00_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p00), zero);
                        __m128i c10_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p10), zero);
                        __m128i c01_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p01), zero);
                        __m128i c11_16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(p11), zero);

                        __m128i top = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c00_16, hw.v_inv_ifu), _mm_mullo_epi16(c10_16, hw.v_ifu)), 8);
                        __m128i bot = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c01_16, hw.v_inv_ifu), _mm_mullo_epi16(c11_16, hw.v_ifu)), 8);

                        __m128i res16 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top, v_inv_ifv), _mm_mullo_epi16(bot, v_ifv)), 8);
                        __m128i res8 = _mm_packus_epi16(res16, zero);
                        *reinterpret_cast<uint32_t*>(dst_row + i) = _mm_cvtsi128_si32(res8);
                    }
#else
                    for (int32_t i = 0; i < w_span; ++i) {
                        const auto& hw = h_weights[i];
                        uint32_t c00 = *reinterpret_cast<const uint32_t*>(src_row0 + hw.x0);
                        uint32_t c10 = *reinterpret_cast<const uint32_t*>(src_row0 + hw.x1);
                        uint32_t c01 = *reinterpret_cast<const uint32_t*>(src_row1 + hw.x0);
                        uint32_t c11 = *reinterpret_cast<const uint32_t*>(src_row1 + hw.x1);

                        uint32_t top_rb = (((c00 & 0x00FF00FF) * hw.inv_ifu + (c10 & 0x00FF00FF) * hw.ifu) >> 8) & 0x00FF00FF;
                        uint32_t top_ga = ((((c00 >> 8) & 0x00FF00FF) * hw.inv_ifu + ((c10 >> 8) & 0x00FF00FF) * hw.ifu) >> 8) & 0x00FF00FF;
                        uint32_t bot_rb = (((c01 & 0x00FF00FF) * hw.inv_ifu + (c11 & 0x00FF00FF) * hw.ifu) >> 8) & 0x00FF00FF;
                        uint32_t bot_ga = ((((c01 >> 8) & 0x00FF00FF) * hw.inv_ifu + ((c11 >> 8) & 0x00FF00FF) * hw.ifu) >> 8) & 0x00FF00FF;

                        uint32_t out_rb = ((top_rb * inv_ifv + bot_rb * ifv) >> 8) & 0x00FF00FF;
                        uint32_t out_ga = (top_ga * inv_ifv + bot_ga * ifv) & 0xFF00FF00;
                        *reinterpret_cast<uint32_t*>(dst_row + i) = out_rb | out_ga;
                    }
#endif
                }
                return;
            } else {
                for (int32_t dy = out_y0; dy < out_y1; ++dy) {
                    int32_t sy = std::clamp(static_cast<int32_t>((static_cast<float>(dy) + 0.5f - dst_fy0) * inv_sy), 0, sh - 1);
                    const PremultipliedColorU8* src_row = src.row(static_cast<size_t>(sy));
                    PremultipliedColorU8* dst_row = dst.row(static_cast<size_t>(dy));

                    for (int32_t dx = out_x0; dx < out_x1; ++dx) {
                        int32_t sx = std::clamp(static_cast<int32_t>((static_cast<float>(dx) + 0.5f - dst_fx0) * inv_sx), 0, sw - 1);
                        dst_row[dx] = src_row[sx];
                    }
                }
                return;
            }
        }
    }

    auto rect = Rect::from_xywh(
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(src.width()),
        static_cast<float>(src.height())
    );
    if (!rect) return;

    Transform pattern_ts = Transform::from_translate(static_cast<float>(x), static_cast<float>(y)).post_concat(transform);
    Pattern pattern(src, SpreadMode::Pad, paint.quality, paint.opacity, pattern_ts);

    Paint p;
    p.shader = Shader(pattern);
    p.blend_mode = paint.blend_mode;
    p.anti_alias = (paint.quality != FilterQuality::Nearest);

    fill_rect(dst, *rect, p, transform, mask, clip);
}

void draw_pixmap_rect(
    PixmapMut& dst,
    const PixmapRef& src,
    const Rect& src_rect,
    const Rect& dst_rect,
    const PixmapPaint& paint,
    Transform transform,
    const Mask* mask,
    std::optional<ScreenIntRect> clip
) {
    if (src.is_empty() || !src.data() || src_rect.is_empty() || dst_rect.is_empty() ||
        dst.width() == 0 || dst.height() == 0) {
        return;
    }

    auto inv_opt = transform.invert();
    if (!inv_opt) return;
    const Transform inv = *inv_opt;

    int32_t clip_x0 = 0;
    int32_t clip_y0 = 0;
    int32_t clip_x1 = static_cast<int32_t>(dst.width());
    int32_t clip_y1 = static_cast<int32_t>(dst.height());
    if (clip) {
        clip_x0 = std::max(clip_x0, static_cast<int32_t>(clip->left()));
        clip_y0 = std::max(clip_y0, static_cast<int32_t>(clip->top()));
        clip_x1 = std::min(clip_x1, static_cast<int32_t>(clip->right()));
        clip_y1 = std::min(clip_y1, static_cast<int32_t>(clip->bottom()));
    }
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;

    Point corners[4] = {
        {dst_rect.left(), dst_rect.top()},
        {dst_rect.right(), dst_rect.top()},
        {dst_rect.right(), dst_rect.bottom()},
        {dst_rect.left(), dst_rect.bottom()}
    };
    transform.map_points(corners, 4);
    float min_fx = corners[0].x, max_fx = corners[0].x;
    float min_fy = corners[0].y, max_fy = corners[0].y;
    for (int i = 1; i < 4; ++i) {
        min_fx = std::min(min_fx, corners[i].x);
        max_fx = std::max(max_fx, corners[i].x);
        min_fy = std::min(min_fy, corners[i].y);
        max_fy = std::max(max_fy, corners[i].y);
    }

    int32_t out_x0 = std::max(clip_x0, static_cast<int32_t>(std::floor(min_fx)));
    int32_t out_y0 = std::max(clip_y0, static_cast<int32_t>(std::floor(min_fy)));
    int32_t out_x1 = std::min(clip_x1, static_cast<int32_t>(std::ceil(max_fx)));
    int32_t out_y1 = std::min(clip_y1, static_cast<int32_t>(std::ceil(max_fy)));
    if (out_x0 >= out_x1 || out_y0 >= out_y1) return;

    int32_t sw = static_cast<int32_t>(src.width());
    int32_t sh = static_cast<int32_t>(src.height());
    int32_t min_sx = std::clamp(static_cast<int32_t>(std::floor(src_rect.left())), 0, sw - 1);
    int32_t max_sx = std::clamp(static_cast<int32_t>(std::ceil(src_rect.right())) - 1, min_sx, sw - 1);
    int32_t min_sy = std::clamp(static_cast<int32_t>(std::floor(src_rect.top())), 0, sh - 1);
    int32_t max_sy = std::clamp(static_cast<int32_t>(std::ceil(src_rect.bottom())) - 1, min_sy, sh - 1);

    float inv_dw = 1.0f / dst_rect.width();
    float inv_dh = 1.0f / dst_rect.height();
    float src_l = src_rect.left();
    float src_t = src_rect.top();
    float src_w = src_rect.width();
    float src_h = src_rect.height();

    float op = std::clamp(paint.opacity, 0.0f, 1.0f);
    float ta = paint.tint.alpha() * op;
    float tr = paint.tint.red() * ta;
    float tg = paint.tint.green() * ta;
    float tb = paint.tint.blue() * ta;
    bool has_tint_or_op = (std::abs(tr - 1.0f) > 1e-4f || std::abs(tg - 1.0f) > 1e-4f ||
                           std::abs(tb - 1.0f) > 1e-4f || std::abs(ta - 1.0f) > 1e-4f);
    bool bilinear = (paint.quality != FilterQuality::Nearest);

    for (int32_t dy = out_y0; dy < out_y1; ++dy) {
        float py = static_cast<float>(dy) + 0.5f;
        for (int32_t dx = out_x0; dx < out_x1; ++dx) {
            uint8_t mask_a = 255;
            if (mask) {
                if (static_cast<uint32_t>(dx) >= mask->width() || static_cast<uint32_t>(dy) >= mask->height()) {
                    continue;
                }
                mask_a = mask->data()[static_cast<size_t>(dy) * mask->width() + static_cast<size_t>(dx)];
                if (mask_a == 0) continue;
            }

            Point lp = {static_cast<float>(dx) + 0.5f, py};
            inv.map_point(lp);
            if (lp.x < dst_rect.left() || lp.x >= dst_rect.right() ||
                lp.y < dst_rect.top() || lp.y >= dst_rect.bottom()) {
                continue;
            }

            float u = (lp.x - dst_rect.left()) * inv_dw;
            float v = (lp.y - dst_rect.top()) * inv_dh;
            float sx_f = src_l + u * src_w;
            float sy_f = src_t + v * src_h;

            PremultipliedColorU8 sampled;
            if (!bilinear) {
                int32_t ix = std::clamp(static_cast<int32_t>(std::floor(sx_f)), min_sx, max_sx);
                int32_t iy = std::clamp(static_cast<int32_t>(std::floor(sy_f)), min_sy, max_sy);
                sampled = *src.pixel(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy));
            } else {
                float fx_s = sx_f - 0.5f;
                float fy_s = sy_f - 0.5f;
                int32_t fx0 = static_cast<int32_t>(std::floor(fx_s));
                int32_t fy0 = static_cast<int32_t>(std::floor(fy_s));
                float wx = fx_s - static_cast<float>(fx0);
                float wy = fy_s - static_cast<float>(fy0);
                int32_t x0 = std::clamp(fx0, min_sx, max_sx);
                int32_t x1 = std::clamp(fx0 + 1, min_sx, max_sx);
                int32_t y0 = std::clamp(fy0, min_sy, max_sy);
                int32_t y1 = std::clamp(fy0 + 1, min_sy, max_sy);

                auto p00 = *src.pixel(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0));
                auto p10 = *src.pixel(static_cast<uint32_t>(x1), static_cast<uint32_t>(y0));
                auto p01 = *src.pixel(static_cast<uint32_t>(x0), static_cast<uint32_t>(y1));
                auto p11 = *src.pixel(static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));

                float w00 = (1.0f - wx) * (1.0f - wy);
                float w10 = wx * (1.0f - wy);
                float w01 = (1.0f - wx) * wy;
                float w11 = wx * wy;

                sampled = PremultipliedColorU8::from_rgba_unchecked(
                    static_cast<uint8_t>(std::clamp(p00.red() * w00 + p10.red() * w10 + p01.red() * w01 + p11.red() * w11 + 0.5f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(p00.green() * w00 + p10.green() * w10 + p01.green() * w01 + p11.green() * w11 + 0.5f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(p00.blue() * w00 + p10.blue() * w10 + p01.blue() * w01 + p11.blue() * w11 + 0.5f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(p00.alpha() * w00 + p10.alpha() * w10 + p01.alpha() * w01 + p11.alpha() * w11 + 0.5f, 0.0f, 255.0f))
                );
            }

            if (has_tint_or_op || mask_a < 255) {
                float mf = (mask_a == 255) ? 1.0f : (static_cast<float>(mask_a) / 255.0f);
                sampled = PremultipliedColorU8::from_rgba_unchecked(
                    static_cast<uint8_t>(std::clamp(static_cast<float>(sampled.red()) * tr * mf + 0.5f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(static_cast<float>(sampled.green()) * tg * mf + 0.5f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(static_cast<float>(sampled.blue()) * tb * mf + 0.5f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(static_cast<float>(sampled.alpha()) * ta * mf + 0.5f, 0.0f, 255.0f))
                );
            }

            auto dst_px = *dst.pixel(static_cast<uint32_t>(dx), static_cast<uint32_t>(dy));
            dst.set_pixel(static_cast<uint32_t>(dx), static_cast<uint32_t>(dy),
                          blend_pixels(sampled, dst_px, paint.blend_mode));
        }
    }
}

void apply_mask(
    PixmapMut& dst,
    const Mask& mask
) {
    if (dst.size() != mask.size()) return;

    auto pixels = dst.pixels_mut();
    const uint8_t* mask_data = mask.data();
    size_t count = static_cast<size_t>(dst.width()) * dst.height();

    for (size_t i = 0; i < count; ++i) {
        uint8_t m = mask_data[i];
        if (m == 255) continue;
        if (m == 0) {
            pixels[i] = PremultipliedColorU8::TRANSPARENT;
        } else {
            auto p = pixels[i];
            pixels[i] = PremultipliedColorU8::from_rgba_unchecked(
                premultiply_u8(p.red(), m),
                premultiply_u8(p.green(), m),
                premultiply_u8(p.blue(), m),
                premultiply_u8(p.alpha(), m)
            );
        }
    }
}

} // namespace painter

} // namespace nisaba
