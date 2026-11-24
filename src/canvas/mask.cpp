#include "nisaba/canvas/mask.hpp"
#include "nisaba/shaders/shader.hpp"
#include "nisaba/raster/scan.hpp"
#include "nisaba/color/color.hpp"
#include <algorithm>
#include <cstring>

namespace nisaba {

Mask Mask::from_pixmap(const PixmapRef& pixmap, MaskType type) {
    Mask mask = *Mask::create(pixmap.width(), pixmap.height());
    auto pixels = pixmap.pixels();
    auto dst = mask.data_mut();
    size_t count = static_cast<size_t>(pixmap.width()) * pixmap.height();

    if (type == MaskType::Alpha) {
        for (size_t i = 0; i < count; ++i) {
            dst[i] = pixels[i].alpha();
        }
    } else { // Luminance
        for (size_t i = 0; i < count; ++i) {
            auto p = pixels[i];
            float r = static_cast<float>(p.red()) / 255.0f;
            float g = static_cast<float>(p.green()) / 255.0f;
            float b = static_cast<float>(p.blue()) / 255.0f;
            float a = static_cast<float>(p.alpha()) / 255.0f;

            if (p.alpha() != 0) {
                r /= a;
                g /= a;
                b /= a;
            }

            float luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
            float ma = std::clamp(std::ceil((luma * a) * 255.0f), 0.0f, 255.0f);
            dst[i] = static_cast<uint8_t>(ma);
        }
    }

    return mask;
}

void Mask::fill_path(
    const Path& path,
    FillRule fill_rule,
    bool anti_alias,
    Transform transform
) {
    if (path.is_empty()) return;

    Path transformed_path;
    const Path* target_path = &path;

    if (!transform.is_identity()) {
        auto res = path.transform(transform);
        if (!res) return;
        transformed_path = std::move(*res);
        target_path = &transformed_path;
    }

    auto bounds = target_path->bounds();
    if (bounds.width() <= 0.0f || bounds.height() <= 0.0f) {
        return;
    }

    int32_t bx0 = std::clamp(static_cast<int32_t>(std::floor(bounds.left())), 0, static_cast<int32_t>(width()));
    int32_t by0 = std::clamp(static_cast<int32_t>(std::floor(bounds.top())), 0, static_cast<int32_t>(height()));
    int32_t bx1 = std::clamp(static_cast<int32_t>(std::ceil(bounds.right())), 0, static_cast<int32_t>(width()));
    int32_t by1 = std::clamp(static_cast<int32_t>(std::ceil(bounds.bottom())), 0, static_cast<int32_t>(height()));
    if (bx1 <= bx0 || by1 <= by0) return;

    auto clip_opt = ScreenIntRect::from_xywh(
        static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
        static_cast<uint32_t>(bx1 - bx0), static_cast<uint32_t>(by1 - by0)
    );
    if (!clip_opt) return;

    MaskBlitter blitter(as_submask_mut());
    scan::fill_path(*target_path, fill_rule, *clip_opt, anti_alias, blitter);
}

void Mask::intersect_path(
    const Path& path,
    FillRule fill_rule,
    bool anti_alias,
    Transform transform,
    std::optional<ScreenIntRect> conservative_clip
) {
    if (path.is_empty() || width() == 0 || height() == 0) return;

    Rect b = path.bounds();
    Path transformed_path;
    const Path* target_path = &path;
    if (!transform.is_identity()) {
        auto res = path.transform(transform);
        if (!res) return;
        transformed_path = std::move(*res);
        target_path = &transformed_path;
        b = target_path->bounds();
    }

    int32_t w = static_cast<int32_t>(width());
    int32_t h = static_cast<int32_t>(height());

    int32_t bx0 = std::clamp(static_cast<int32_t>(std::floor(b.left())), 0, w);
    int32_t by0 = std::clamp(static_cast<int32_t>(std::floor(b.top())), 0, h);
    int32_t bx1 = std::clamp(static_cast<int32_t>(std::ceil(b.right())), 0, w);
    int32_t by1 = std::clamp(static_cast<int32_t>(std::ceil(b.bottom())), 0, h);

    if (conservative_clip.has_value()) {
        bx0 = std::max(bx0, static_cast<int32_t>(conservative_clip->x()));
        by0 = std::max(by0, static_cast<int32_t>(conservative_clip->y()));
        bx1 = std::min(bx1, static_cast<int32_t>(conservative_clip->right()));
        by1 = std::min(by1, static_cast<int32_t>(conservative_clip->bottom()));
    }

    if (bx1 <= bx0 || by1 <= by0) {
        std::fill(data_.begin(), data_.end(), static_cast<uint8_t>(0));
        return;
    }

    static thread_local std::vector<uint8_t> scratch;
    size_t total = static_cast<size_t>(width()) * height();
    if (scratch.size() < total) {
        scratch.resize(total);
    }

    for (int32_t y = by0; y < by1; ++y) {
        std::memset(scratch.data() + static_cast<size_t>(y) * width() + bx0, 0, static_cast<size_t>(bx1 - bx0));
    }

    auto sz = IntSize::from_wh(width(), height());
    if (!sz) return;
    SubMaskMut submask{ .data = scratch.data(), .size = *sz, .real_width = width() };
    MaskBlitter blitter(submask);

    auto clip_opt = ScreenIntRect::from_xywh(
        static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
        static_cast<uint32_t>(bx1 - bx0), static_cast<uint32_t>(by1 - by0)
    );
    if (!clip_opt) return;

    scan::fill_path(*target_path, fill_rule, *clip_opt, anti_alias, blitter);

    for (int32_t y = by0; y < by1; ++y) {
        uint8_t* d_row = data_.data() + y * width();
        const uint8_t* s_row = scratch.data() + y * width();
        simd::multiply_mask_spans(d_row + bx0, s_row + bx0, static_cast<size_t>(bx1 - bx0));
    }

    if (!conservative_clip.has_value()) {
        if (by0 > 0) {
            std::memset(data_.data(), 0, static_cast<size_t>(by0) * width());
        }
        for (int32_t y = by0; y < by1; ++y) {
            uint8_t* d_row = data_.data() + y * width();
            if (bx0 > 0) std::memset(d_row, 0, static_cast<size_t>(bx0));
            if (bx1 < w) std::memset(d_row + bx1, 0, static_cast<size_t>(w - bx1));
        }
        if (by1 < h) {
            std::memset(data_.data() + static_cast<size_t>(by1) * width(), 0, static_cast<size_t>(h - by1) * width());
        }
    }
}

void Mask::difference_path(
    const Path& path,
    FillRule fill_rule,
    bool anti_alias,
    Transform transform,
    std::optional<ScreenIntRect> conservative_clip
) {
    if (path.is_empty() || width() == 0 || height() == 0) return;

    Rect b = path.bounds();
    Path transformed_path;
    const Path* target_path = &path;
    if (!transform.is_identity()) {
        auto res = path.transform(transform);
        if (!res) return;
        transformed_path = std::move(*res);
        target_path = &transformed_path;
        b = target_path->bounds();
    }

    const int32_t w = static_cast<int32_t>(width());
    const int32_t h = static_cast<int32_t>(height());

    int32_t bx0 = std::clamp(static_cast<int32_t>(std::floor(b.left())), 0, w);
    int32_t by0 = std::clamp(static_cast<int32_t>(std::floor(b.top())), 0, h);
    int32_t bx1 = std::clamp(static_cast<int32_t>(std::ceil(b.right())), 0, w);
    int32_t by1 = std::clamp(static_cast<int32_t>(std::ceil(b.bottom())), 0, h);

    if (conservative_clip.has_value()) {
        bx0 = std::max(bx0, static_cast<int32_t>(conservative_clip->x()));
        by0 = std::max(by0, static_cast<int32_t>(conservative_clip->y()));
        bx1 = std::min(bx1, static_cast<int32_t>(conservative_clip->right()));
        by1 = std::min(by1, static_cast<int32_t>(conservative_clip->bottom()));
    }

    if (bx1 <= bx0 || by1 <= by0) return;

    static thread_local std::vector<uint8_t> scratch;
    const size_t total = static_cast<size_t>(width()) * height();
    if (scratch.size() < total) {
        scratch.resize(total);
    }

    for (int32_t y = by0; y < by1; ++y) {
        std::memset(scratch.data() + static_cast<size_t>(y) * width() + bx0, 0, static_cast<size_t>(bx1 - bx0));
    }

    auto sz = IntSize::from_wh(width(), height());
    if (!sz) return;
    SubMaskMut submask{ .data = scratch.data(), .size = *sz, .real_width = width() };
    MaskBlitter blitter(submask);

    auto clip_opt = ScreenIntRect::from_xywh(
        static_cast<uint32_t>(bx0), static_cast<uint32_t>(by0),
        static_cast<uint32_t>(bx1 - bx0), static_cast<uint32_t>(by1 - by0)
    );
    if (!clip_opt) return;

    scan::fill_path(*target_path, fill_rule, *clip_opt, anti_alias, blitter);

    for (int32_t y = by0; y < by1; ++y) {
        uint8_t* d_row = data_.data() + static_cast<size_t>(y) * width();
        const uint8_t* s_row = scratch.data() + static_cast<size_t>(y) * width();
        for (int32_t x = bx0; x < bx1; ++x) {
            const uint32_t cov = static_cast<uint32_t>(s_row[x]);
            if (cov == 0u) continue;
            if (cov == 255u) {
                d_row[x] = 0;
            } else {
                d_row[x] = static_cast<uint8_t>((static_cast<uint32_t>(d_row[x]) * (255u - cov) + 127u) / 255u);
            }
        }
    }
}

void Mask::apply_shader(
    const Shader& shader,
    Transform transform,
    bool is_difference,
    std::optional<ScreenIntRect> conservative_clip
) {
    if (width() == 0 || height() == 0) return;

    int32_t bx0 = 0;
    int32_t by0 = 0;
    int32_t bx1 = static_cast<int32_t>(width());
    int32_t by1 = static_cast<int32_t>(height());
    if (conservative_clip.has_value()) {
        bx0 = std::max(bx0, static_cast<int32_t>(conservative_clip->x()));
        by0 = std::max(by0, static_cast<int32_t>(conservative_clip->y()));
        bx1 = std::min(bx1, static_cast<int32_t>(conservative_clip->right()));
        by1 = std::min(by1, static_cast<int32_t>(conservative_clip->bottom()));
    }
    if (bx1 <= bx0 || by1 <= by0) return;

    if (shader.is_solid_color()) {
        uint32_t sa = static_cast<uint32_t>(std::clamp(shader.solid_color().alpha() * 255.0f + 0.5f, 0.0f, 255.0f));
        if (is_difference) sa = 255u - sa;
        if (sa == 255u) return;
        for (int32_t y = by0; y < by1; ++y) {
            uint8_t* d_row = data_.data() + static_cast<size_t>(y) * width();
            if (sa == 0u) {
                std::memset(d_row + bx0, 0, static_cast<size_t>(bx1 - bx0));
            } else {
                for (int32_t x = bx0; x < bx1; ++x) {
                    d_row[x] = static_cast<uint8_t>((static_cast<uint32_t>(d_row[x]) * sa + 127u) / 255u);
                }
            }
        }
        return;
    }

    auto inv_opt = transform.invert();
    if (!inv_opt) return;
    const Transform inv = *inv_opt;

    if (inv.is_identity()) {
        static thread_local std::vector<PremultipliedColorU8> span_buf;
        const uint32_t span_w = static_cast<uint32_t>(bx1 - bx0);
        if (span_buf.size() < span_w) span_buf.resize(span_w);

        for (int32_t y = by0; y < by1; ++y) {
            shader.shade_span(static_cast<float>(bx0) + 0.5f, static_cast<float>(y) + 0.5f, span_w, span_buf.data());
            uint8_t* d_row = data_.data() + static_cast<size_t>(y) * width() + bx0;
            for (uint32_t i = 0; i < span_w; ++i) {
                uint32_t sa = static_cast<uint32_t>(span_buf[i].alpha());
                if (is_difference) sa = 255u - sa;
                d_row[i] = static_cast<uint8_t>((static_cast<uint32_t>(d_row[i]) * sa + 127u) / 255u);
            }
        }
        return;
    }

    for (int32_t y = by0; y < by1; ++y) {
        uint8_t* d_row = data_.data() + static_cast<size_t>(y) * width();
        const float py = static_cast<float>(y) + 0.5f;
        for (int32_t x = bx0; x < bx1; ++x) {
            if (d_row[x] == 0) continue;
            Point lp(static_cast<float>(x) + 0.5f, py);
            inv.map_point(lp);
            const PremultipliedColor pc = shader.sample_premul(lp.x, lp.y);
            uint32_t sa = static_cast<uint32_t>(std::clamp(pc.alpha() * 255.0f + 0.5f, 0.0f, 255.0f));
            if (is_difference) sa = 255u - sa;
            d_row[x] = static_cast<uint8_t>((static_cast<uint32_t>(d_row[x]) * sa + 127u) / 255u);
        }
    }
}

} // namespace nisaba
