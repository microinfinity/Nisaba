#include "nisaba/effects/image_filter.hpp"
#include "nisaba/effects/blur.hpp"
#include "nisaba/color/blend.hpp"
#include "nisaba/pipeline/simd.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nisaba::effects {

namespace {

std::shared_ptr<const ImageFilterNode> unwrap_input(const std::optional<ImageFilter>& input) noexcept {
    if (input.has_value() && input->node()) {
        return input->node();
    }
    return nullptr;
}

bool node_is_active(const ImageFilterNode* node) noexcept {
    if (!node) return false;
    switch (node->kind) {
        case ImageFilterKind::None:
            return false;
        case ImageFilterKind::Blur:
            if (node->sigma_x > 0.001f || node->sigma_y > 0.001f || node->crop_rect.has_value()) {
                return true;
            }
            return !node->inputs.empty() && node_is_active(node->inputs[0].get());
        case ImageFilterKind::DropShadow:
            return node->color.alpha() > 0.0f || node->shadow_only || node->crop_rect.has_value() ||
                   (!node->inputs.empty() && node_is_active(node->inputs[0].get()));
        case ImageFilterKind::Compose:
            return (node->inputs.size() >= 1 && node_is_active(node->inputs[0].get())) ||
                   (node->inputs.size() >= 2 && node_is_active(node->inputs[1].get())) ||
                   node->crop_rect.has_value();
        case ImageFilterKind::Merge:
            return !node->inputs.empty();
        case ImageFilterKind::Blend:
            return true;
        case ImageFilterKind::Offset:
            if (std::abs(node->dx) > 1e-4f || std::abs(node->dy) > 1e-4f || node->crop_rect.has_value()) {
                return true;
            }
            return !node->inputs.empty() && node_is_active(node->inputs[0].get());
        case ImageFilterKind::ColorFilter:
            return node->color_effect.is_active() || node->crop_rect.has_value() ||
                   (!node->inputs.empty() && node_is_active(node->inputs[0].get()));
        case ImageFilterKind::RuntimeShader:
            return node->runtime_spec != nullptr;
        case ImageFilterKind::Dilate:
        case ImageFilterKind::Erode:
            return node->sigma_x > 0.0f || node->sigma_y > 0.0f || node->crop_rect.has_value() ||
                   (!node->inputs.empty() && node_is_active(node->inputs[0].get()));
        case ImageFilterKind::MatrixConvolution:
            return (node->kernel_w > 0 && node->kernel_h > 0 &&
                    static_cast<int32_t>(node->kernel.size()) >= node->kernel_w * node->kernel_h) ||
                   node->crop_rect.has_value();
        case ImageFilterKind::DisplacementMap:
        case ImageFilterKind::Magnifier:
        case ImageFilterKind::Arithmetic:
        case ImageFilterKind::MatrixTransform:
        case ImageFilterKind::Tile:
        case ImageFilterKind::AlphaThreshold:
        case ImageFilterKind::Lighting:
            return true;
    }
    return false;
}

float node_max_sigma(const ImageFilterNode* node) noexcept {
    if (!node) return 0.0f;
    float s = std::max(node->sigma_x, node->sigma_y);
    for (const auto& child : node->inputs) {
        s = std::max(s, node_max_sigma(child.get()));
    }
    return s;
}

Rect union_rects(const Rect& a, const Rect& b) noexcept {
    const float l = std::min(a.left(), b.left());
    const float t = std::min(a.top(), b.top());
    const float r = std::max(a.right(), b.right());
    const float bt = std::max(a.bottom(), b.bottom());
    auto u = Rect::from_ltrb(l, t, r, bt);
    return u.value_or(a);
}

Rect expand_rect(const Rect& r, float px, float py) noexcept {
    auto res = Rect::from_ltrb(r.left() - px, r.top() - py, r.right() + px, r.bottom() + py);
    return res.value_or(r);
}

Rect shift_rect(const Rect& r, float dx, float dy) noexcept {
    auto res = Rect::from_ltrb(r.left() + dx, r.top() + dy, r.right() + dx, r.bottom() + dy);
    return res.value_or(r);
}

Rect map_node_bounds(const ImageFilterNode* node, const Rect& src_bounds, bool include_source_union) noexcept {
    if (!node) return src_bounds;

    auto eval_child = [&](size_t idx) -> Rect {
        if (idx < node->inputs.size() && node->inputs[idx]) {
            return map_node_bounds(node->inputs[idx].get(), src_bounds, include_source_union);
        }
        return src_bounds;
    };

    Rect out = src_bounds;
    switch (node->kind) {
        case ImageFilterKind::None:
            out = src_bounds;
            break;
        case ImageFilterKind::Blur: {
            Rect in_b = eval_child(0);
            const float px = (node->sigma_x > 0.001f) ? (std::ceil(node->sigma_x * 3.0f) + 2.0f) : 0.0f;
            const float py = (node->sigma_y > 0.001f) ? (std::ceil(node->sigma_y * 3.0f) + 2.0f) : 0.0f;
            out = expand_rect(in_b, px, py);
            break;
        }
        case ImageFilterKind::DropShadow: {
            Rect in_b = eval_child(0);
            const float px = (node->sigma_x > 0.001f) ? (std::ceil(node->sigma_x * 3.0f) + 2.0f) : 0.0f;
            const float py = (node->sigma_y > 0.001f) ? (std::ceil(node->sigma_y * 3.0f) + 2.0f) : 0.0f;
            Rect sh_b = expand_rect(shift_rect(in_b, node->dx, node->dy), px, py);
            out = (node->shadow_only && !include_source_union) ? sh_b : union_rects(in_b, sh_b);
            break;
        }
        case ImageFilterKind::Offset: {
            Rect in_b = eval_child(0);
            Rect shifted = shift_rect(in_b, node->dx, node->dy);
            out = include_source_union ? union_rects(in_b, shifted) : shifted;
            break;
        }
        case ImageFilterKind::ColorFilter: {
            out = eval_child(0);
            break;
        }
        case ImageFilterKind::Compose: {
            Rect inner_b = (node->inputs.size() >= 2 && node->inputs[1])
                ? map_node_bounds(node->inputs[1].get(), src_bounds, include_source_union)
                : src_bounds;
            Rect outer_b = (node->inputs.size() >= 1 && node->inputs[0])
                ? map_node_bounds(node->inputs[0].get(), inner_b, include_source_union)
                : inner_b;
            out = include_source_union ? union_rects(inner_b, outer_b) : outer_b;
            break;
        }
        case ImageFilterKind::Merge: {
            if (node->inputs.empty()) {
                out = src_bounds;
            } else {
                out = eval_child(0);
                for (size_t i = 1; i < node->inputs.size(); ++i) {
                    out = union_rects(out, eval_child(i));
                }
            }
            break;
        }
        case ImageFilterKind::Blend: {
            Rect bg_b = eval_child(0);
            Rect fg_b = eval_child(1);
            out = union_rects(bg_b, fg_b);
            break;
        }
        case ImageFilterKind::RuntimeShader: {
            out = eval_child(0);
            break;
        }
        case ImageFilterKind::Dilate:
        case ImageFilterKind::Erode: {
            Rect in_b = eval_child(0);
            const float rx = std::max(0.0f, std::ceil(node->sigma_x));
            const float ry = std::max(0.0f, std::ceil(node->sigma_y));
            out = expand_rect(in_b, rx, ry);
            break;
        }
        case ImageFilterKind::MatrixConvolution: {
            Rect in_b = eval_child(0);
            const float px = static_cast<float>(std::max(0, node->kernel_w));
            const float py = static_cast<float>(std::max(0, node->kernel_h));
            out = expand_rect(in_b, px, py);
            break;
        }
        case ImageFilterKind::DisplacementMap: {
            Rect disp_b = eval_child(0);
            Rect col_b = eval_child(1);
            const float pad = std::abs(node->disp_scale) * 0.5f + 2.0f;
            out = expand_rect(union_rects(disp_b, col_b), pad, pad);
            break;
        }
        case ImageFilterKind::Magnifier: {
            Rect in_b = eval_child(0);
            out = include_source_union ? union_rects(in_b, node->src_rect) : node->src_rect;
            break;
        }
        case ImageFilterKind::Arithmetic: {
            Rect bg_b = eval_child(0);
            Rect fg_b = eval_child(1);
            out = union_rects(bg_b, fg_b);
            break;
        }
        case ImageFilterKind::MatrixTransform: {
            Rect in_b = eval_child(0);
            Rect mapped = in_b.transform(node->filter_transform).value_or(in_b);
            out = include_source_union ? union_rects(in_b, mapped) : mapped;
            break;
        }
        case ImageFilterKind::Tile: {
            Rect in_b = eval_child(0);
            out = include_source_union ? union_rects(union_rects(in_b, node->src_rect), node->dst_rect) : node->dst_rect;
            break;
        }
        case ImageFilterKind::AlphaThreshold: {
            Rect in_b = eval_child(0);
            out = union_rects(in_b, node->src_rect);
            break;
        }
        case ImageFilterKind::Lighting: {
            out = expand_rect(eval_child(0), 2.0f, 2.0f);
            break;
        }
    }

    if (node->crop_rect.has_value()) {
        if (include_source_union) {
            out = union_rects(out, *node->crop_rect);
        } else if (auto inter = out.intersect(*node->crop_rect)) {
            out = *inter;
        } else {
            out = *node->crop_rect;
        }
    }
    return include_source_union ? union_rects(src_bounds, out) : out;
}

Pixmap clone_rgba_pixmap(const Pixmap& src) {
    auto copy = Pixmap::allocate(src.width(), src.height(), PixelFormat::RGBA8888);
    if (!copy) return Pixmap{};
    auto dst_mut = copy->as_mut();
    const auto src_ref = src.as_ref();
    for (uint32_t y = 0; y < src.height(); ++y) {
        std::copy_n(src_ref.row(y), src.width(), dst_mut.row(y));
    }
    return std::move(*copy);
}

void apply_crop_rgba(
    PixmapMut& pm,
    const std::optional<Rect>& crop_rect,
    int32_t roi_ox,
    int32_t roi_oy,
    const Transform& ts
) noexcept {
    if (!crop_rect.has_value()) return;
    const int32_t w = static_cast<int32_t>(pm.width());
    const int32_t h = static_cast<int32_t>(pm.height());
    if (w <= 0 || h <= 0) return;

    auto dev_crop = crop_rect->transform(ts);
    if (!dev_crop.has_value()) {
        for (int32_t y = 0; y < h; ++y) {
            std::fill_n(pm.row(static_cast<size_t>(y)), w, PremultipliedColorU8::TRANSPARENT);
        }
        return;
    }

    const int32_t cx0 = std::clamp(static_cast<int32_t>(std::floor(dev_crop->left())) - roi_ox, 0, w);
    const int32_t cy0 = std::clamp(static_cast<int32_t>(std::floor(dev_crop->top())) - roi_oy, 0, h);
    const int32_t cx1 = std::clamp(static_cast<int32_t>(std::ceil(dev_crop->right())) - roi_ox, 0, w);
    const int32_t cy1 = std::clamp(static_cast<int32_t>(std::ceil(dev_crop->bottom())) - roi_oy, 0, h);

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pm.row(static_cast<size_t>(y));
        if (y < cy0 || y >= cy1 || cx1 <= cx0) {
            std::fill_n(row, w, PremultipliedColorU8::TRANSPARENT);
        } else {
            if (cx0 > 0) {
                std::fill_n(row, cx0, PremultipliedColorU8::TRANSPARENT);
            }
            if (cx1 < w) {
                std::fill_n(row + cx1, w - cx1, PremultipliedColorU8::TRANSPARENT);
            }
        }
    }
}

PremultipliedColorU8 sample_bilinear_zero(const PixmapRef& src, float sx, float sy) noexcept {
    const int32_t w = static_cast<int32_t>(src.width());
    const int32_t h = static_cast<int32_t>(src.height());
    const int32_t x0 = static_cast<int32_t>(std::floor(sx));
    const int32_t y0 = static_cast<int32_t>(std::floor(sy));
    const float fx = sx - static_cast<float>(x0);
    const float fy = sy - static_cast<float>(y0);

    auto fetch = [&](int32_t x, int32_t y) noexcept -> PremultipliedColorU8 {
        if (x < 0 || x >= w || y < 0 || y >= h) return PremultipliedColorU8::TRANSPARENT;
        return src.row(static_cast<size_t>(y))[x];
    };

    const auto p00 = fetch(x0, y0);
    const auto p10 = fetch(x0 + 1, y0);
    const auto p01 = fetch(x0, y0 + 1);
    const auto p11 = fetch(x0 + 1, y0 + 1);

    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;

    auto mix_ch = [&](uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11) noexcept -> uint8_t {
        float v = c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11;
        return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    };

    const uint8_t a = mix_ch(p00.a, p10.a, p01.a, p11.a);
    const uint8_t r = std::min(a, mix_ch(p00.r, p10.r, p01.r, p11.r));
    const uint8_t g = std::min(a, mix_ch(p00.g, p10.g, p01.g, p11.g));
    const uint8_t b = std::min(a, mix_ch(p00.b, p10.b, p01.b, p11.b));
    return PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
}

Pixmap offset_rgba_pixmap(const Pixmap& src, float dx, float dy) {
    const int32_t w = static_cast<int32_t>(src.width());
    const int32_t h = static_cast<int32_t>(src.height());
    auto out_opt = Pixmap::allocate(static_cast<uint32_t>(w), static_cast<uint32_t>(h), PixelFormat::RGBA8888);
    if (!out_opt) return Pixmap{};
    auto out_mut = out_opt->as_mut();
    const auto src_ref = src.as_ref();

    const float rdx = std::round(dx);
    const float rdy = std::round(dy);
    if (std::abs(dx - rdx) < 1e-3f && std::abs(dy - rdy) < 1e-3f) {
        const int32_t idx = static_cast<int32_t>(rdx);
        const int32_t idy = static_cast<int32_t>(rdy);
        for (int32_t y = 0; y < h; ++y) {
            const int32_t sy = y - idy;
            if (sy < 0 || sy >= h) continue;
            const auto* src_row = src_ref.row(static_cast<size_t>(sy));
            auto* dst_row = out_mut.row(static_cast<size_t>(y));
            const int32_t dst_x0 = std::max(0, idx);
            const int32_t dst_x1 = std::min(w, w + idx);
            if (dst_x1 > dst_x0) {
                std::copy_n(src_row + (dst_x0 - idx), dst_x1 - dst_x0, dst_row + dst_x0);
            }
        }
    } else {
        for (int32_t y = 0; y < h; ++y) {
            auto* dst_row = out_mut.row(static_cast<size_t>(y));
            const float sy = static_cast<float>(y) - dy;
            for (int32_t x = 0; x < w; ++x) {
                dst_row[x] = sample_bilinear_zero(src_ref, static_cast<float>(x) - dx, sy);
            }
        }
    }
    return std::move(*out_opt);
}

Pixmap eval_node_cpu(
    const ImageFilterNode* node,
    const Pixmap& current_src,
    int32_t roi_ox,
    int32_t roi_oy,
    const Transform& ts
) {
    if (!node || node->kind == ImageFilterKind::None) {
        return clone_rgba_pixmap(current_src);
    }

    const uint32_t w = current_src.width();
    const uint32_t h = current_src.height();
    const float scale_x = std::hypot(ts.sx, ts.ky);
    const float scale_y = std::hypot(ts.kx, ts.sy);

    auto eval_child = [&](size_t idx) -> Pixmap {
        if (idx < node->inputs.size() && node->inputs[idx]) {
            return eval_node_cpu(node->inputs[idx].get(), current_src, roi_ox, roi_oy, ts);
        }
        return clone_rgba_pixmap(current_src);
    };

    Pixmap result;

    switch (node->kind) {
        case ImageFilterKind::None:
            result = clone_rgba_pixmap(current_src);
            break;

        case ImageFilterKind::Blur: {
            result = eval_child(0);
            auto mut = result.as_mut();
            const float eff_sx = node->sigma_x * scale_x;
            const float eff_sy = node->sigma_y * scale_y;
            if (eff_sx > 0.001f || eff_sy > 0.001f) {
                gaussian_blur_pixmap_xy(mut, eff_sx, eff_sy);
            }
            break;
        }

        case ImageFilterKind::DropShadow: {
            Pixmap fg = eval_child(0);
            const auto fg_ref = fg.as_ref();
            auto shadow_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!shadow_opt) return fg;
            auto sh_mut = shadow_opt->as_mut();

            const PremultipliedColorU8 sc = node->color.premultiply().to_color_u8();
            for (uint32_t y = 0; y < h; ++y) {
                const auto* src_row = fg_ref.row(y);
                auto* dst_row = sh_mut.row(y);
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t a = src_row[x].a;
                    if (a == 0) continue;
                    dst_row[x] = PremultipliedColorU8::from_rgba_unchecked(
                        static_cast<uint8_t>((sc.r * a + 128u) / 255u),
                        static_cast<uint8_t>((sc.g * a + 128u) / 255u),
                        static_cast<uint8_t>((sc.b * a + 128u) / 255u),
                        static_cast<uint8_t>((sc.a * a + 128u) / 255u)
                    );
                }
            }

            const float eff_sx = node->sigma_x * scale_x;
            const float eff_sy = node->sigma_y * scale_y;
            if (eff_sx > 0.001f || eff_sy > 0.001f) {
                gaussian_blur_pixmap_xy(sh_mut, eff_sx, eff_sy);
            }

            const float eff_dx = node->dx * ts.sx + node->dy * ts.kx;
            const float eff_dy = node->dx * ts.ky + node->dy * ts.sy;
            result = offset_rgba_pixmap(*shadow_opt, eff_dx, eff_dy);

            if (!node->shadow_only) {
                auto res_mut = result.as_mut();
                for (uint32_t y = 0; y < h; ++y) {
                    simd::blend_source_over_span(res_mut.row(y), fg_ref.row(y), w);
                }
            }
            break;
        }

        case ImageFilterKind::Offset: {
            Pixmap in_pm = eval_child(0);
            const float eff_dx = node->dx * ts.sx + node->dy * ts.kx;
            const float eff_dy = node->dx * ts.ky + node->dy * ts.sy;
            result = offset_rgba_pixmap(in_pm, eff_dx, eff_dy);
            break;
        }

        case ImageFilterKind::ColorFilter: {
            result = eval_child(0);
            if (node->color_effect.is_active()) {
                auto mut = result.as_mut();
                node->color_effect.apply(mut);
            }
            break;
        }

        case ImageFilterKind::Compose: {
            Pixmap inner_pm = (node->inputs.size() >= 2 && node->inputs[1])
                ? eval_node_cpu(node->inputs[1].get(), current_src, roi_ox, roi_oy, ts)
                : clone_rgba_pixmap(current_src);
            result = (node->inputs.size() >= 1 && node->inputs[0])
                ? eval_node_cpu(node->inputs[0].get(), inner_pm, roi_ox, roi_oy, ts)
                : std::move(inner_pm);
            break;
        }

        case ImageFilterKind::Merge: {
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) return Pixmap{};
            auto out_mut = out_opt->as_mut();
            for (size_t i = 0; i < node->inputs.size(); ++i) {
                Pixmap child_pm = eval_child(i);
                const auto child_ref = child_pm.as_ref();
                for (uint32_t y = 0; y < h; ++y) {
                    simd::blend_source_over_span(out_mut.row(y), child_ref.row(y), w);
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::Blend: {
            Pixmap bg = eval_child(0);
            Pixmap fg = eval_child(1);
            const auto bg_ref = bg.as_ref();
            const auto fg_ref = fg.as_ref();
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) return Pixmap{};
            auto out_mut = out_opt->as_mut();
            for (uint32_t y = 0; y < h; ++y) {
                const auto* bg_row = bg_ref.row(y);
                const auto* fg_row = fg_ref.row(y);
                auto* dst_row = out_mut.row(y);
                if (node->blend_mode == BlendMode::SourceOver) {
                    std::copy_n(bg_row, w, dst_row);
                    simd::blend_source_over_span(dst_row, fg_row, w);
                } else {
                    for (uint32_t x = 0; x < w; ++x) {
                        dst_row[x] = blend_pixels(fg_row[x], bg_row[x], node->blend_mode);
                    }
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::RuntimeShader: {
            Pixmap in_pm = eval_child(0);
            if (node->runtime_spec) {
                result = eval_runtime_filter_cpu(*node->runtime_spec, in_pm, roi_ox, roi_oy, ts, node->crop_rect);
                return result;
            }
            result = std::move(in_pm);
            break;
        }

        case ImageFilterKind::Dilate:
        case ImageFilterKind::Erode: {
            Pixmap in_pm = eval_child(0);
            const bool is_dilate = (node->kind == ImageFilterKind::Dilate);
            const int32_t rx = std::clamp(static_cast<int32_t>(std::round(node->sigma_x * scale_x)), 0, 64);
            const int32_t ry = std::clamp(static_cast<int32_t>(std::round(node->sigma_y * scale_y)), 0, 64);
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            auto tmp_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!tmp_opt || !out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto tmp_mut = tmp_opt->as_mut();
            auto out_mut = out_opt->as_mut();

            // Horizontal pass
            for (int32_t y = 0; y < ih; ++y) {
                const auto* srow = in_ref.row(static_cast<size_t>(y));
                auto* drow = tmp_mut.row(static_cast<size_t>(y));
                for (int32_t x = 0; x < iw; ++x) {
                    const int32_t x0 = std::max(0, x - rx);
                    const int32_t x1 = std::min(iw - 1, x + rx);
                    uint8_t br = srow[x0].r, bg = srow[x0].g, bb = srow[x0].b, ba = srow[x0].a;
                    for (int32_t k = x0 + 1; k <= x1; ++k) {
                        const auto p = srow[k];
                        if (is_dilate) {
                            br = std::max(br, p.r); bg = std::max(bg, p.g);
                            bb = std::max(bb, p.b); ba = std::max(ba, p.a);
                        } else {
                            br = std::min(br, p.r); bg = std::min(bg, p.g);
                            bb = std::min(bb, p.b); ba = std::min(ba, p.a);
                        }
                    }
                    drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                        std::min(br, ba), std::min(bg, ba), std::min(bb, ba), ba);
                }
            }
            // Vertical pass
            const auto tmp_ref = tmp_opt->as_ref();
            for (int32_t y = 0; y < ih; ++y) {
                const int32_t y0 = std::max(0, y - ry);
                const int32_t y1 = std::min(ih - 1, y + ry);
                auto* drow = out_mut.row(static_cast<size_t>(y));
                for (int32_t x = 0; x < iw; ++x) {
                    const auto p0 = tmp_ref.row(static_cast<size_t>(y0))[x];
                    uint8_t br = p0.r, bg = p0.g, bb = p0.b, ba = p0.a;
                    for (int32_t k = y0 + 1; k <= y1; ++k) {
                        const auto p = tmp_ref.row(static_cast<size_t>(k))[x];
                        if (is_dilate) {
                            br = std::max(br, p.r); bg = std::max(bg, p.g);
                            bb = std::max(bb, p.b); ba = std::max(ba, p.a);
                        } else {
                            br = std::min(br, p.r); bg = std::min(bg, p.g);
                            bb = std::min(bb, p.b); ba = std::min(ba, p.a);
                        }
                    }
                    drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                        std::min(br, ba), std::min(bg, ba), std::min(bb, ba), ba);
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::MatrixConvolution: {
            Pixmap in_pm = eval_child(0);
            const int32_t kw = node->kernel_w;
            const int32_t kh = node->kernel_h;
            if (kw <= 0 || kh <= 0 || static_cast<int32_t>(node->kernel.size()) < kw * kh) {
                result = std::move(in_pm);
                break;
            }
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            constexpr float inv255 = 1.0f / 255.0f;

            auto fetch_tiled = [&](int32_t sx, int32_t sy) noexcept -> PremultipliedColorU8 {
                if (node->tile_mode == FilterTileMode::ClampToBlack) {
                    if (sx < 0 || sx >= iw || sy < 0 || sy >= ih) return PremultipliedColorU8::TRANSPARENT;
                    return in_ref.row(static_cast<size_t>(sy))[sx];
                }
                if (node->tile_mode == FilterTileMode::Repeat) {
                    sx = ((sx % iw) + iw) % iw;
                    sy = ((sy % ih) + ih) % ih;
                    return in_ref.row(static_cast<size_t>(sy))[sx];
                }
                sx = std::clamp(sx, 0, iw - 1);
                sy = std::clamp(sy, 0, ih - 1);
                return in_ref.row(static_cast<size_t>(sy))[sx];
            };

            for (int32_t y = 0; y < ih; ++y) {
                auto* drow = out_mut.row(static_cast<size_t>(y));
                for (int32_t x = 0; x < iw; ++x) {
                    float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;
                    for (int32_t ky = 0; ky < kh; ++ky) {
                        for (int32_t kx = 0; kx < kw; ++kx) {
                            const float kv = node->kernel[static_cast<size_t>(ky * kw + kx)];
                            if (kv == 0.0f) continue;
                            const auto p = fetch_tiled(x + kx - node->kernel_offset_x, y + ky - node->kernel_offset_y);
                            sr += p.r * inv255 * kv;
                            sg += p.g * inv255 * kv;
                            sb += p.b * inv255 * kv;
                            sa += p.a * inv255 * kv;
                        }
                    }
                    const auto center = in_ref.row(static_cast<size_t>(y))[x];
                    float a = node->convolve_alpha ? (sa * node->gain + node->bias) : (center.a * inv255);
                    a = std::clamp(a, 0.0f, 1.0f);
                    float r = std::clamp(sr * node->gain + node->bias, 0.0f, a);
                    float g = std::clamp(sg * node->gain + node->bias, 0.0f, a);
                    float b = std::clamp(sb * node->gain + node->bias, 0.0f, a);
                    drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                        static_cast<uint8_t>(r * 255.0f + 0.5f),
                        static_cast<uint8_t>(g * 255.0f + 0.5f),
                        static_cast<uint8_t>(b * 255.0f + 0.5f),
                        static_cast<uint8_t>(a * 255.0f + 0.5f));
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::DisplacementMap: {
            Pixmap disp_pm = eval_child(0);
            Pixmap col_pm = eval_child(1);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(col_pm); break; }
            const auto disp_ref = disp_pm.as_ref();
            const auto col_ref = col_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            const float eff_sx = node->disp_scale * scale_x;
            const float eff_sy = node->disp_scale * scale_y;
            constexpr float inv255 = 1.0f / 255.0f;

            auto read_ch = [](const PremultipliedColorU8& p, ColorChannel ch) noexcept -> float {
                const float a = p.a * inv255;
                if (ch == ColorChannel::A) return a;
                if (a <= 1e-4f) return 0.0f;
                const float invA = 1.0f / a;
                switch (ch) {
                    case ColorChannel::R: return std::clamp(p.r * inv255 * invA, 0.0f, 1.0f);
                    case ColorChannel::G: return std::clamp(p.g * inv255 * invA, 0.0f, 1.0f);
                    case ColorChannel::B: return std::clamp(p.b * inv255 * invA, 0.0f, 1.0f);
                    default: return a;
                }
            };

            auto sample_col_dev = [&](float dev_x, float dev_y) noexcept -> PremultipliedColorU8 {
                if (dev_x < static_cast<float>(roi_ox) || dev_x > static_cast<float>(roi_ox + iw) ||
                    dev_y < static_cast<float>(roi_oy) || dev_y > static_cast<float>(roi_oy + ih)) {
                    return PremultipliedColorU8::TRANSPARENT;
                }
                return sample_bilinear_zero(col_ref,
                                            std::clamp(dev_x - static_cast<float>(roi_ox) - 0.5f, 0.0f, static_cast<float>(iw - 1)),
                                            std::clamp(dev_y - static_cast<float>(roi_oy) - 0.5f, 0.0f, static_cast<float>(ih - 1)));
            };

            for (int32_t y = 0; y < ih; ++y) {
                const auto* dsrc = disp_ref.row(static_cast<size_t>(y));
                auto* drow = out_mut.row(static_cast<size_t>(y));
                const float dev_y = static_cast<float>(roi_oy + y) + 0.5f;
                for (int32_t x = 0; x < iw; ++x) {
                    const float dev_x = static_cast<float>(roi_ox + x) + 0.5f;
                    const float dx = (read_ch(dsrc[x], node->x_channel) - 0.5f) * eff_sx;
                    const float dy = (read_ch(dsrc[x], node->y_channel) - 0.5f) * eff_sy;
                    drow[x] = sample_col_dev(dev_x + dx, dev_y + dy);
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::Magnifier: {
            Pixmap in_pm = eval_child(0);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const auto dev_rect = node->src_rect.transform(ts);
            if (!dev_rect || dev_rect->width() <= 1e-3f || dev_rect->height() <= 1e-3f) {
                result = std::move(*out_opt);
                break;
            }
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            const float rx0 = dev_rect->left(), ry0 = dev_rect->top();
            const float rw = dev_rect->width(), rh = dev_rect->height();
            const float ins_x = std::min(std::max(0.0f, node->magnifier_inset * scale_x), rw * 0.45f);
            const float ins_y = std::min(std::max(0.0f, node->magnifier_inset * scale_y), rh * 0.45f);
            const float bx = ins_x / rw;
            const float by = ins_y / rh;

            auto smooth01 = [](float t) noexcept -> float {
                t = std::clamp(t, 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };

            for (int32_t y = 0; y < ih; ++y) {
                auto* drow = out_mut.row(static_cast<size_t>(y));
                const float dev_y = static_cast<float>(roi_oy + y) + 0.5f;
                if (dev_y < ry0 || dev_y > ry0 + rh) continue;
                const float ty = (dev_y - ry0) / rh;
                for (int32_t x = 0; x < iw; ++x) {
                    const float dev_x = static_cast<float>(roi_ox + x) + 0.5f;
                    if (dev_x < rx0 || dev_x > rx0 + rw) continue;
                    const float tx = (dev_x - rx0) / rw;
                    float wt = 1.0f;
                    if (bx > 1e-4f || by > 1e-4f) {
                        const float wx = (bx > 1e-4f) ? (std::min(tx, 1.0f - tx) / bx) : 1.0f;
                        const float wy = (by > 1e-4f) ? (std::min(ty, 1.0f - ty) / by) : 1.0f;
                        wt = smooth01(std::min(wx, wy));
                    }
                    const float zx = (rx0 + ins_x) + tx * (rw - 2.0f * ins_x);
                    const float zy = (ry0 + ins_y) + ty * (rh - 2.0f * ins_y);
                    const float sx = dev_x + (zx - dev_x) * wt;
                    const float sy = dev_y + (zy - dev_y) * wt;
                    if (sx >= static_cast<float>(roi_ox) && sx <= static_cast<float>(roi_ox + iw) &&
                        sy >= static_cast<float>(roi_oy) && sy <= static_cast<float>(roi_oy + ih)) {
                        drow[x] = sample_bilinear_zero(in_ref,
                                                       std::clamp(sx - static_cast<float>(roi_ox) - 0.5f, 0.0f, static_cast<float>(iw - 1)),
                                                       std::clamp(sy - static_cast<float>(roi_oy) - 0.5f, 0.0f, static_cast<float>(ih - 1)));
                    }
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::Arithmetic: {
            Pixmap bg_pm = eval_child(0);
            Pixmap fg_pm = eval_child(1);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(fg_pm); break; }
            const auto bg_ref = bg_pm.as_ref();
            const auto fg_ref = fg_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            constexpr float inv255 = 1.0f / 255.0f;
            for (uint32_t y = 0; y < h; ++y) {
                const auto* brow = bg_ref.row(y);
                const auto* frow = fg_ref.row(y);
                auto* drow = out_mut.row(y);
                for (uint32_t x = 0; x < w; ++x) {
                    const float fr = frow[x].r * inv255, fg_c = frow[x].g * inv255, fb = frow[x].b * inv255, fa = frow[x].a * inv255;
                    const float br = brow[x].r * inv255, bg_c = brow[x].g * inv255, bb = brow[x].b * inv255, ba = brow[x].a * inv255;
                    float a = std::clamp(node->k1 * fa * ba + node->k2 * fa + node->k3 * ba + node->k4, 0.0f, 1.0f);
                    float r = std::clamp(node->k1 * fr * br + node->k2 * fr + node->k3 * br + node->k4, 0.0f, 1.0f);
                    float g = std::clamp(node->k1 * fg_c * bg_c + node->k2 * fg_c + node->k3 * bg_c + node->k4, 0.0f, 1.0f);
                    float b = std::clamp(node->k1 * fb * bb + node->k2 * fb + node->k3 * bb + node->k4, 0.0f, 1.0f);
                    if (node->enforce_premul) {
                        r = std::min(r, a); g = std::min(g, a); b = std::min(b, a);
                    }
                    drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                        static_cast<uint8_t>(r * 255.0f + 0.5f),
                        static_cast<uint8_t>(g * 255.0f + 0.5f),
                        static_cast<uint8_t>(b * 255.0f + 0.5f),
                        static_cast<uint8_t>(a * 255.0f + 0.5f));
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::MatrixTransform: {
            Pixmap in_pm = eval_child(0);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const auto inv_filter = node->filter_transform.invert();
            const auto inv_ts = ts.invert();
            if (!inv_filter || !inv_ts) { result = std::move(*out_opt); break; }
            const Transform dev_inv = ts.pre_concat(*inv_filter).pre_concat(*inv_ts);
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            for (int32_t y = 0; y < ih; ++y) {
                auto* drow = out_mut.row(static_cast<size_t>(y));
                const float dy = static_cast<float>(roi_oy + y) + 0.5f;
                for (int32_t x = 0; x < iw; ++x) {
                    Point p = Point::from_xy(static_cast<float>(roi_ox + x) + 0.5f, dy);
                    dev_inv.map_point(p);
                    if (p.x < static_cast<float>(roi_ox) || p.x > static_cast<float>(roi_ox + iw) ||
                        p.y < static_cast<float>(roi_oy) || p.y > static_cast<float>(roi_oy + ih)) {
                        continue;
                    }
                    const float sx = p.x - static_cast<float>(roi_ox) - 0.5f;
                    const float sy = p.y - static_cast<float>(roi_oy) - 0.5f;
                    if (node->filter_quality == FilterQuality::Nearest) {
                        const int32_t ix = std::clamp(static_cast<int32_t>(std::floor(p.x - static_cast<float>(roi_ox))), 0, iw - 1);
                        const int32_t iy = std::clamp(static_cast<int32_t>(std::floor(p.y - static_cast<float>(roi_oy))), 0, ih - 1);
                        drow[x] = in_ref.row(static_cast<size_t>(iy))[ix];
                    } else {
                        drow[x] = sample_bilinear_zero(in_ref,
                                                       std::clamp(sx, 0.0f, static_cast<float>(iw - 1)),
                                                       std::clamp(sy, 0.0f, static_cast<float>(ih - 1)));
                    }
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::Tile: {
            Pixmap in_pm = eval_child(0);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const auto dev_src = node->src_rect.transform(ts);
            const auto dev_dst = node->dst_rect.transform(ts);
            if (!dev_src || !dev_dst || dev_src->width() <= 1e-3f || dev_src->height() <= 1e-3f) {
                result = std::move(*out_opt);
                break;
            }
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            const float sl = dev_src->left(), st = dev_src->top(), sw = dev_src->width(), sh = dev_src->height();
            const float dl = dev_dst->left(), dt = dev_dst->top(), dr = dev_dst->right(), db = dev_dst->bottom();
            auto pos_mod = [](float v, float m) noexcept -> float {
                float r = std::fmod(v, m);
                return (r < 0.0f) ? (r + m) : r;
            };
            for (int32_t y = 0; y < ih; ++y) {
                auto* drow = out_mut.row(static_cast<size_t>(y));
                const float dy = static_cast<float>(roi_oy + y) + 0.5f;
                if (dy < dt || dy > db) continue;
                const float sy = st + pos_mod(dy - st, sh);
                if (sy < static_cast<float>(roi_oy) || sy > static_cast<float>(roi_oy + ih)) continue;
                for (int32_t x = 0; x < iw; ++x) {
                    const float dx = static_cast<float>(roi_ox + x) + 0.5f;
                    if (dx < dl || dx > dr) continue;
                    const float sx = sl + pos_mod(dx - sl, sw);
                    if (sx < static_cast<float>(roi_ox) || sx > static_cast<float>(roi_ox + iw)) continue;
                    drow[x] = sample_bilinear_zero(in_ref,
                                                   std::clamp(sx - static_cast<float>(roi_ox) - 0.5f, 0.0f, static_cast<float>(iw - 1)),
                                                   std::clamp(sy - static_cast<float>(roi_oy) - 0.5f, 0.0f, static_cast<float>(ih - 1)));
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::AlphaThreshold: {
            Pixmap in_pm = eval_child(0);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const auto dev_reg = node->src_rect.transform(ts);
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            constexpr float inv255 = 1.0f / 255.0f;
            for (int32_t y = 0; y < ih; ++y) {
                const auto* srow = in_ref.row(static_cast<size_t>(y));
                auto* drow = out_mut.row(static_cast<size_t>(y));
                const float dy = static_cast<float>(roi_oy + y) + 0.5f;
                for (int32_t x = 0; x < iw; ++x) {
                    const float dx = static_cast<float>(roi_ox + x) + 0.5f;
                    const bool inside = dev_reg.has_value() &&
                        dx >= dev_reg->left() && dx <= dev_reg->right() &&
                        dy >= dev_reg->top() && dy <= dev_reg->bottom();
                    const auto p = srow[x];
                    const float a = p.a * inv255;
                    const float new_a = std::clamp(inside ? std::max(a, node->inner_min) : std::min(a, node->outer_max), 0.0f, 1.0f);
                    float r = 0.0f, g = 0.0f, b = 0.0f;
                    if (a > 1e-4f) {
                        const float ratio = new_a / a;
                        r = std::clamp(p.r * inv255 * ratio, 0.0f, new_a);
                        g = std::clamp(p.g * inv255 * ratio, 0.0f, new_a);
                        b = std::clamp(p.b * inv255 * ratio, 0.0f, new_a);
                    }
                    drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                        static_cast<uint8_t>(r * 255.0f + 0.5f),
                        static_cast<uint8_t>(g * 255.0f + 0.5f),
                        static_cast<uint8_t>(b * 255.0f + 0.5f),
                        static_cast<uint8_t>(new_a * 255.0f + 0.5f));
                }
            }
            result = std::move(*out_opt);
            break;
        }

        case ImageFilterKind::Lighting: {
            Pixmap in_pm = eval_child(0);
            auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
            if (!out_opt) { result = std::move(in_pm); break; }
            const auto in_ref = in_pm.as_ref();
            auto out_mut = out_opt->as_mut();
            const int32_t iw = static_cast<int32_t>(w);
            const int32_t ih = static_cast<int32_t>(h);
            const auto inv_ts = ts.invert().value_or(Transform());
            constexpr float inv255 = 1.0f / 255.0f;
            const float lcr = node->color.red();
            const float lcg = node->color.green();
            const float lcb = node->color.blue();
            const bool is_spec = (node->lighting_kind == LightingKind::DistantSpecular ||
                                  node->lighting_kind == LightingKind::PointSpecular ||
                                  node->lighting_kind == LightingKind::SpotSpecular);
            const float cos_cutoff = std::cos(node->cutoff_angle * 3.14159265358979323846f / 180.0f);

            // Precompute normalized spot direction
            float sx_dir = node->light_target.x - node->light_pos.x;
            float sy_dir = node->light_target.y - node->light_pos.y;
            float sz_dir = node->light_target.z - node->light_pos.z;
            const float slen = std::sqrt(sx_dir * sx_dir + sy_dir * sy_dir + sz_dir * sz_dir);
            if (slen > 1e-6f) { sx_dir /= slen; sy_dir /= slen; sz_dir /= slen; }

            auto alpha_at = [&](int32_t x, int32_t y) noexcept -> float {
                x = std::clamp(x, 0, iw - 1);
                y = std::clamp(y, 0, ih - 1);
                return in_ref.row(static_cast<size_t>(y))[x].a * inv255;
            };

            for (int32_t y = 0; y < ih; ++y) {
                auto* drow = out_mut.row(static_cast<size_t>(y));
                const float dev_y = static_cast<float>(roi_oy + y) + 0.5f;
                for (int32_t x = 0; x < iw; ++x) {
                    const float a00 = alpha_at(x - 1, y - 1), a10 = alpha_at(x, y - 1), a20 = alpha_at(x + 1, y - 1);
                    const float a01 = alpha_at(x - 1, y),     a11 = alpha_at(x, y),     a21 = alpha_at(x + 1, y);
                    const float a02 = alpha_at(x - 1, y + 1), a12 = alpha_at(x, y + 1), a22 = alpha_at(x + 1, y + 1);
                    const float ddx = (-a00 + a20 - 2.0f * a01 + 2.0f * a21 - a02 + a22) * (node->surface_scale * 0.25f);
                    const float ddy = (-a00 - 2.0f * a10 - a20 + a02 + 2.0f * a12 + a22) * (node->surface_scale * 0.25f);
                    float nx = -ddx, ny = -ddy, nz = 1.0f;
                    const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                    nx /= nlen; ny /= nlen; nz /= nlen;

                    Point lp = Point::from_xy(static_cast<float>(roi_ox + x) + 0.5f, dev_y);
                    inv_ts.map_point(lp);
                    float lx = 0.0f, ly = 0.0f, lz = 1.0f;
                    if (node->lighting_kind == LightingKind::DistantDiffuse ||
                        node->lighting_kind == LightingKind::DistantSpecular) {
                        lx = node->light_pos.x; ly = node->light_pos.y; lz = node->light_pos.z;
                    } else {
                        lx = node->light_pos.x - lp.x;
                        ly = node->light_pos.y - lp.y;
                        lz = node->light_pos.z - node->surface_scale * a11;
                    }
                    const float llen = std::sqrt(lx * lx + ly * ly + lz * lz);
                    if (llen > 1e-6f) { lx /= llen; ly /= llen; lz /= llen; }

                    float atten = 1.0f;
                    if (node->lighting_kind == LightingKind::SpotDiffuse ||
                        node->lighting_kind == LightingKind::SpotSpecular) {
                        const float cos_a = (-lx) * sx_dir + (-ly) * sy_dir + (-lz) * sz_dir;
                        if (cos_a <= 0.0f || (node->cutoff_angle < 180.0f && cos_a < cos_cutoff)) {
                            atten = 0.0f;
                        } else {
                            atten = std::pow(cos_a, std::max(node->specular_exponent, 0.0f));
                        }
                    }

                    float inten = 0.0f;
                    if (!is_spec) {
                        const float ndl = std::max(nx * lx + ny * ly + nz * lz, 0.0f);
                        inten = node->light_kd_ks * ndl * atten;
                    } else {
                        float hx = lx, hy = ly, hz = lz + 1.0f;
                        const float hlen = std::sqrt(hx * hx + hy * hy + hz * hz);
                        if (hlen > 1e-6f) { hx /= hlen; hy /= hlen; hz /= hlen; }
                        const float ndh = std::max(nx * hx + ny * hy + nz * hz, 0.0f);
                        inten = node->light_kd_ks * std::pow(ndh, std::max(node->shininess, 1e-4f)) * atten;
                    }
                    const float r = std::clamp(inten * lcr, 0.0f, 1.0f);
                    const float g = std::clamp(inten * lcg, 0.0f, 1.0f);
                    const float b = std::clamp(inten * lcb, 0.0f, 1.0f);
                    const float a = is_spec ? std::max({r, g, b}) : 1.0f;
                    drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                        static_cast<uint8_t>(r * 255.0f + 0.5f),
                        static_cast<uint8_t>(g * 255.0f + 0.5f),
                        static_cast<uint8_t>(b * 255.0f + 0.5f),
                        static_cast<uint8_t>(a * 255.0f + 0.5f));
                }
            }
            result = std::move(*out_opt);
            break;
        }
    }

    if (node->crop_rect.has_value() && result.width() == w && result.height() == h) {
        auto res_mut = result.as_mut();
        apply_crop_rgba(res_mut, node->crop_rect, roi_ox, roi_oy, ts);
    }

    return result;
}

} // namespace

ImageFilter ImageFilter::blur(
    float sx,
    float sy,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Blur;
    n->sigma_x = sx > 0.0f ? sx : 0.0f;
    n->sigma_y = sy > 0.0f ? sy : 0.0f;
    n->crop_rect = crop_rect;
    if (input.has_value()) {
        n->inputs.push_back(unwrap_input(input));
    }
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::drop_shadow(
    float dx_val,
    float dy_val,
    float sx,
    float sy,
    Color shadow_color,
    bool only_shadow,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::DropShadow;
    n->dx = dx_val;
    n->dy = dy_val;
    n->sigma_x = sx > 0.0f ? sx : 0.0f;
    n->sigma_y = sy > 0.0f ? sy : 0.0f;
    n->color = shadow_color;
    n->shadow_only = only_shadow;
    n->crop_rect = crop_rect;
    if (input.has_value()) {
        n->inputs.push_back(unwrap_input(input));
    }
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::compose(
    const ImageFilter& outer,
    const ImageFilter& inner,
    std::optional<Rect> crop_rect
) {
    if (!outer.node() && !crop_rect.has_value()) return inner;
    if (!inner.node() && !crop_rect.has_value()) return outer;
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Compose;
    n->crop_rect = crop_rect;
    n->inputs.push_back(outer.node());
    n->inputs.push_back(inner.node());
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::merge(
    std::vector<std::optional<ImageFilter>> filters,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Merge;
    n->crop_rect = crop_rect;
    n->inputs.reserve(filters.size());
    for (const auto& f : filters) {
        n->inputs.push_back(unwrap_input(f));
    }
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::merge(
    std::initializer_list<std::optional<ImageFilter>> filters,
    std::optional<Rect> crop_rect
) {
    return merge(std::vector<std::optional<ImageFilter>>(filters.begin(), filters.end()), crop_rect);
}

ImageFilter ImageFilter::blend(
    BlendMode mode,
    std::optional<ImageFilter> background,
    std::optional<ImageFilter> foreground,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Blend;
    n->blend_mode = mode;
    n->crop_rect = crop_rect;
    n->inputs.push_back(unwrap_input(background));
    n->inputs.push_back(unwrap_input(foreground));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::offset(
    float dx_val,
    float dy_val,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Offset;
    n->dx = dx_val;
    n->dy = dy_val;
    n->crop_rect = crop_rect;
    if (input.has_value()) {
        n->inputs.push_back(unwrap_input(input));
    }
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::color_filter(
    const ColorEffect& effect,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::ColorFilter;
    n->color_effect = effect;
    n->crop_rect = crop_rect;
    if (input.has_value()) {
        n->inputs.push_back(unwrap_input(input));
    }
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::runtime_shader(
    std::shared_ptr<const RuntimeFilterSpec> spec,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::RuntimeShader;
    n->runtime_spec = std::move(spec);
    n->crop_rect = crop_rect;
    if (input.has_value()) {
        n->inputs.push_back(unwrap_input(input));
    }
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::runtime_shader(
    std::string_view source,
    const RuntimeFilterUniforms& uniforms,
    std::string_view input_child_name,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto spec = parse_runtime_filter_source(source, &uniforms, input_child_name);
    return runtime_shader(std::move(spec), std::move(input), crop_rect);
}

ImageFilter ImageFilter::dilate(float rx, float ry, std::optional<ImageFilter> input, std::optional<Rect> crop_rect) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Dilate;
    n->sigma_x = std::max(0.0f, rx);
    n->sigma_y = std::max(0.0f, ry);
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::erode(float rx, float ry, std::optional<ImageFilter> input, std::optional<Rect> crop_rect) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Erode;
    n->sigma_x = std::max(0.0f, rx);
    n->sigma_y = std::max(0.0f, ry);
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::matrix_convolution(
    int32_t kernel_w,
    int32_t kernel_h,
    std::vector<float> kernel,
    float gain,
    float bias,
    int32_t target_x,
    int32_t target_y,
    FilterTileMode tile_mode,
    bool convolve_alpha,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::MatrixConvolution;
    n->kernel_w = kernel_w;
    n->kernel_h = kernel_h;
    n->kernel = std::move(kernel);
    n->gain = gain;
    n->bias = bias;
    n->kernel_offset_x = target_x;
    n->kernel_offset_y = target_y;
    n->tile_mode = tile_mode;
    n->convolve_alpha = convolve_alpha;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::displacement_map(
    ColorChannel x_channel,
    ColorChannel y_channel,
    float scale,
    std::optional<ImageFilter> displacement,
    std::optional<ImageFilter> color,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::DisplacementMap;
    n->x_channel = x_channel;
    n->y_channel = y_channel;
    n->disp_scale = scale;
    n->crop_rect = crop_rect;
    n->inputs.push_back(unwrap_input(displacement));
    n->inputs.push_back(unwrap_input(color));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::magnifier(
    const Rect& src_rect,
    float inset,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Magnifier;
    n->src_rect = src_rect;
    n->magnifier_inset = std::max(0.0f, inset);
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::arithmetic(
    float k1,
    float k2,
    float k3,
    float k4,
    bool enforce_premul,
    std::optional<ImageFilter> background,
    std::optional<ImageFilter> foreground,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Arithmetic;
    n->k1 = k1;
    n->k2 = k2;
    n->k3 = k3;
    n->k4 = k4;
    n->enforce_premul = enforce_premul;
    n->crop_rect = crop_rect;
    n->inputs.push_back(unwrap_input(background));
    n->inputs.push_back(unwrap_input(foreground));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::matrix_transform(
    const Transform& transform,
    FilterQuality quality,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::MatrixTransform;
    n->filter_transform = transform;
    n->filter_quality = quality;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::tile(
    const Rect& src,
    const Rect& dst,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Tile;
    n->src_rect = src;
    n->dst_rect = dst;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::alpha_threshold(
    const Rect& region,
    float inner_min,
    float outer_max,
    std::optional<ImageFilter> input,
    std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::AlphaThreshold;
    n->src_rect = region;
    n->inner_min = std::clamp(inner_min, 0.0f, 1.0f);
    n->outer_max = std::clamp(outer_max, 0.0f, 1.0f);
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::distant_lit_diffuse(
    Point3 direction, Color light_color, float surface_scale, float kd,
    std::optional<ImageFilter> input, std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Lighting;
    n->lighting_kind = LightingKind::DistantDiffuse;
    n->light_pos = direction;
    n->color = light_color;
    n->surface_scale = surface_scale;
    n->light_kd_ks = kd;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::distant_lit_specular(
    Point3 direction, Color light_color, float surface_scale, float ks, float shininess,
    std::optional<ImageFilter> input, std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Lighting;
    n->lighting_kind = LightingKind::DistantSpecular;
    n->light_pos = direction;
    n->color = light_color;
    n->surface_scale = surface_scale;
    n->light_kd_ks = ks;
    n->shininess = shininess;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::point_lit_diffuse(
    Point3 location, Color light_color, float surface_scale, float kd,
    std::optional<ImageFilter> input, std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Lighting;
    n->lighting_kind = LightingKind::PointDiffuse;
    n->light_pos = location;
    n->color = light_color;
    n->surface_scale = surface_scale;
    n->light_kd_ks = kd;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::point_lit_specular(
    Point3 location, Color light_color, float surface_scale, float ks, float shininess,
    std::optional<ImageFilter> input, std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Lighting;
    n->lighting_kind = LightingKind::PointSpecular;
    n->light_pos = location;
    n->color = light_color;
    n->surface_scale = surface_scale;
    n->light_kd_ks = ks;
    n->shininess = shininess;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::spot_lit_diffuse(
    Point3 location, Point3 target, float specular_exponent, float cutoff_angle,
    Color light_color, float surface_scale, float kd,
    std::optional<ImageFilter> input, std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Lighting;
    n->lighting_kind = LightingKind::SpotDiffuse;
    n->light_pos = location;
    n->light_target = target;
    n->specular_exponent = specular_exponent;
    n->cutoff_angle = cutoff_angle;
    n->color = light_color;
    n->surface_scale = surface_scale;
    n->light_kd_ks = kd;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::spot_lit_specular(
    Point3 location, Point3 target, float specular_exponent, float cutoff_angle,
    Color light_color, float surface_scale, float ks, float shininess,
    std::optional<ImageFilter> input, std::optional<Rect> crop_rect
) {
    auto n = std::make_shared<ImageFilterNode>();
    n->kind = ImageFilterKind::Lighting;
    n->lighting_kind = LightingKind::SpotSpecular;
    n->light_pos = location;
    n->light_target = target;
    n->specular_exponent = specular_exponent;
    n->cutoff_angle = cutoff_angle;
    n->color = light_color;
    n->surface_scale = surface_scale;
    n->light_kd_ks = ks;
    n->shininess = shininess;
    n->crop_rect = crop_rect;
    if (input.has_value()) n->inputs.push_back(unwrap_input(input));
    return ImageFilter(std::move(n));
}

ImageFilter ImageFilter::with_crop(const Rect& crop) const {
    auto n = node_ ? std::make_shared<ImageFilterNode>(*node_) : std::make_shared<ImageFilterNode>();
    n->crop_rect = crop;
    return ImageFilter(std::move(n));
}

bool ImageFilter::is_active() const noexcept {
    return node_is_active(node_.get());
}

bool ImageFilter::is_simple_leaf() const noexcept {
    if (!node_) return false;
    if (node_->crop_rect.has_value()) return false;
    if (!node_->inputs.empty() && node_->inputs[0] != nullptr) return false;
    return node_->kind == ImageFilterKind::Blur || node_->kind == ImageFilterKind::DropShadow;
}

float ImageFilter::max_sigma() const noexcept {
    return node_max_sigma(node_.get());
}

Rect ImageFilter::map_bounds(const Rect& local_bounds) const noexcept {
    return map_node_bounds(node_.get(), local_bounds, false);
}

Rect ImageFilter::map_layer_bounds(const Rect& local_bounds) const noexcept {
    return map_node_bounds(node_.get(), local_bounds, true);
}

void apply_image_filter_cpu(
    PixmapMut& layer_roi,
    const ImageFilter& filter,
    int32_t roi_origin_x,
    int32_t roi_origin_y,
    const Transform& layer_transform
) {
    if (!filter.is_active()) return;
    const uint32_t w = layer_roi.width();
    const uint32_t h = layer_roi.height();
    if (w == 0 || h == 0) return;

    auto src_rgba_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
    if (!src_rgba_opt) return;
    auto src_mut = src_rgba_opt->as_mut();

    if (layer_roi.format() == PixelFormat::RGBA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            std::copy_n(layer_roi.row(y), w, src_mut.row(y));
        }
    } else if (layer_roi.format() == PixelFormat::BGRA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            const auto* s = layer_roi.row(y);
            auto* d = src_mut.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                d[x] = PremultipliedColorU8::from_rgba_unchecked(s[x].b, s[x].g, s[x].r, s[x].a);
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            auto* d = src_mut.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                d[x] = layer_roi.pixel(x, y).value_or(PremultipliedColorU8::TRANSPARENT);
            }
        }
    }

    Pixmap out_rgba = eval_node_cpu(filter.node().get(), *src_rgba_opt, roi_origin_x, roi_origin_y, layer_transform);
    if (out_rgba.width() != w || out_rgba.height() != h) return;
    const auto out_ref = out_rgba.as_ref();

    if (layer_roi.format() == PixelFormat::RGBA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            std::copy_n(out_ref.row(y), w, layer_roi.row(y));
        }
    } else if (layer_roi.format() == PixelFormat::BGRA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            const auto* s = out_ref.row(y);
            auto* d = layer_roi.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                d[x] = PremultipliedColorU8::from_rgba_unchecked(s[x].b, s[x].g, s[x].r, s[x].a);
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            const auto* s = out_ref.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                layer_roi.set_pixel(x, y, s[x]);
            }
        }
    }
}

} // namespace nisaba::effects
