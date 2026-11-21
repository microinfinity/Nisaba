#include "nisaba/effects/mask_filter.hpp"
#include "nisaba/effects/blur.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nisaba::effects {

void apply_mask_blur_cpu(
    PixmapMut& layer_roi,
    BlurStyle style,
    float sigma,
    Color tint_color,
    const Transform& layer_transform,
    bool respect_ctm
) {
    if (sigma <= 0.001f) return;
    const uint32_t w = layer_roi.width();
    const uint32_t h = layer_roi.height();
    if (w == 0 || h == 0) return;

    float eff_sx = sigma;
    float eff_sy = sigma;
    if (respect_ctm) {
        eff_sx *= std::hypot(layer_transform.sx, layer_transform.ky);
        eff_sy *= std::hypot(layer_transform.kx, layer_transform.sy);
    }

    // Extract original coverage alpha into a dedicated RGBA8888 buffer for anisotropic blur
    auto blur_pm_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
    if (!blur_pm_opt) return;
    auto blur_mut = blur_pm_opt->as_mut();

    std::vector<uint8_t> orig_alpha(static_cast<size_t>(w) * h, 0);
    if (layer_roi.format() == PixelFormat::RGBA8888 || layer_roi.format() == PixelFormat::BGRA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            const auto* src_row = layer_roi.row(y);
            auto* b_row = blur_mut.row(y);
            uint8_t* a_row = orig_alpha.data() + static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t a = src_row[x].a;
                a_row[x] = a;
                b_row[x] = PremultipliedColorU8::from_rgba_unchecked(a, a, a, a);
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            auto* b_row = blur_mut.row(y);
            uint8_t* a_row = orig_alpha.data() + static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t a = layer_roi.pixel(x, y).value_or(PremultipliedColorU8::TRANSPARENT).a;
                a_row[x] = a;
                b_row[x] = PremultipliedColorU8::from_rgba_unchecked(a, a, a, a);
            }
        }
    }

    if (eff_sx > 0.001f || eff_sy > 0.001f) {
        gaussian_blur_pixmap_xy(blur_mut, eff_sx, eff_sy);
    }

    const PremultipliedColor pm_tint = tint_color.premultiply();
    const bool is_bgra = (layer_roi.format() == PixelFormat::BGRA8888);
    const bool is_rgba_or_bgra = (layer_roi.format() == PixelFormat::RGBA8888 || is_bgra);
    constexpr float inv255 = 1.0f / 255.0f;

    for (uint32_t y = 0; y < h; ++y) {
        const auto* b_row = blur_mut.row(y);
        const uint8_t* a_row = orig_alpha.data() + static_cast<size_t>(y) * w;
        auto* dst_row = is_rgba_or_bgra ? layer_roi.row(y) : nullptr;
        for (uint32_t x = 0; x < w; ++x) {
            const float mo = static_cast<float>(a_row[x]) * inv255;
            const float mb = static_cast<float>(b_row[x].a) * inv255;
            float cov = 0.0f;
            switch (style) {
                case BlurStyle::Normal: cov = mb; break;
                case BlurStyle::Solid:  cov = std::max(mo, mb); break;
                case BlurStyle::Outer:  cov = mb * (1.0f - mo); break;
                case BlurStyle::Inner:  cov = mb * mo; break;
            }
            cov = std::clamp(cov, 0.0f, 1.0f);
            const uint8_t out_a = static_cast<uint8_t>(std::clamp(pm_tint.alpha() * cov * 255.0f + 0.5f, 0.0f, 255.0f));
            const uint8_t out_r = std::min(out_a, static_cast<uint8_t>(std::clamp(pm_tint.red() * cov * 255.0f + 0.5f, 0.0f, 255.0f)));
            const uint8_t out_g = std::min(out_a, static_cast<uint8_t>(std::clamp(pm_tint.green() * cov * 255.0f + 0.5f, 0.0f, 255.0f)));
            const uint8_t out_b = std::min(out_a, static_cast<uint8_t>(std::clamp(pm_tint.blue() * cov * 255.0f + 0.5f, 0.0f, 255.0f)));

            if (is_rgba_or_bgra) {
                dst_row[x] = is_bgra
                    ? PremultipliedColorU8::from_rgba_unchecked(out_b, out_g, out_r, out_a)
                    : PremultipliedColorU8::from_rgba_unchecked(out_r, out_g, out_b, out_a);
            } else {
                layer_roi.set_pixel(x, y, PremultipliedColorU8::from_rgba_unchecked(out_r, out_g, out_b, out_a));
            }
        }
    }
}

} // namespace nisaba::effects

