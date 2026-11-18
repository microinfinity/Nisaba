#include "nisaba/effects/color_filter.hpp"
#include "nisaba/effects/color_matrix.hpp"
#include "nisaba/color/blend.hpp"
#include "nisaba/color/color_space_lut.hpp"
#include <algorithm>
#include <cmath>

namespace nisaba::effects {

namespace {

inline ColorTransfer combine_transfers(ColorTransfer inner, ColorTransfer outer) noexcept {
    if (inner == ColorTransfer::None) return outer;
    if (outer == ColorTransfer::None) return inner;
    if ((inner == ColorTransfer::SrgbToLinear && outer == ColorTransfer::LinearToSrgb) ||
        (inner == ColorTransfer::LinearToSrgb && outer == ColorTransfer::SrgbToLinear)) {
        return ColorTransfer::None;
    }
    return outer;
}

} // namespace

ColorEffect ColorEffect::tint(Color color, float intensity) noexcept {
    float t = std::clamp(intensity, 0.0f, 1.0f);
    Color adjusted = Color::from_rgba_unchecked(
        color.red(),
        color.green(),
        color.blue(),
        color.alpha() * t
    );
    return mode(adjusted, BlendMode::SourceAtop);
}

std::optional<ColorEffect> ColorEffect::compose(const ColorEffect& outer, const ColorEffect& inner) noexcept {
    if (!inner.is_active()) return outer;
    if (!outer.is_active()) return inner;

    // Matrix -> Transfer -> Blend cannot losslessly represent Matrix/Transfer applied *after* Blend
    // unless outer is pure Blend and inner has no Blend (or pure Transfer after Matrix).
    if (inner.has_blend && (outer.has_matrix || outer.transfer != ColorTransfer::None || outer.has_blend)) {
        return std::nullopt;
    }
    if (inner.transfer != ColorTransfer::None && outer.has_matrix) {
        return std::nullopt;
    }

    ColorEffect out;
    out.has_matrix = inner.has_matrix || outer.has_matrix;
    if (inner.has_matrix && outer.has_matrix) {
        out.matrix = outer.matrix.multiply(inner.matrix);
    } else if (inner.has_matrix) {
        out.matrix = inner.matrix;
    } else if (outer.has_matrix) {
        out.matrix = outer.matrix;
    }

    out.transfer = combine_transfers(inner.transfer, outer.transfer);
    out.has_blend = outer.has_blend;
    out.blend_color = outer.blend_color;
    out.blend_mode = outer.blend_mode;
    return out;
}

PremultipliedColor ColorEffect::transform_premul(PremultipliedColor c) const noexcept {
    PremultipliedColor pm = c;
    if (has_matrix) {
        pm = matrix.transform_premul(pm);
    }
    if (transfer != ColorTransfer::None) {
        Color straight = pm.demultiply();
        straight = (transfer == ColorTransfer::SrgbToLinear)
            ? ColorSpaceLut::srgb_to_linear(straight)
            : ColorSpaceLut::linear_to_srgb(straight);
        pm = straight.premultiply();
    }
    if (has_blend) {
        pm = blend_colors(blend_color.premultiply(), pm, blend_mode);
    }
    return pm;
}

Color ColorEffect::transform(Color c) const noexcept {
    if (!is_active()) return c;
    return transform_premul(c.premultiply()).demultiply();
}

PremultipliedColorU8 ColorEffect::transform_pixel(PremultipliedColorU8 p) const noexcept {
    if (!is_active()) return p;
    if (transfer == ColorTransfer::None && !has_blend && has_matrix) {
        return matrix.transform_pixel(p);
    }
    PremultipliedColor pm(
        NormalizedF32::from_u8(p.red()),
        NormalizedF32::from_u8(p.green()),
        NormalizedF32::from_u8(p.blue()),
        NormalizedF32::from_u8(p.alpha())
    );
    return transform_premul(pm).to_color_u8();
}

void ColorEffect::apply(PixmapMut& pixmap) const noexcept {
    if (!is_active()) return;
    if (has_matrix && transfer == ColorTransfer::None && !has_blend) {
        matrix.apply(pixmap);
        return;
    }

    const uint32_t w = pixmap.width();
    const uint32_t h = pixmap.height();
    if (pixmap.format() == PixelFormat::RGBA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            PremultipliedColorU8* row = pixmap.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                row[x] = transform_pixel(row[x]);
            }
        }
    } else if (pixmap.format() == PixelFormat::BGRA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            PremultipliedColorU8* row = pixmap.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                const auto bgra = row[x];
                auto rgba = PremultipliedColorU8::from_rgba_unchecked(bgra.b, bgra.g, bgra.r, bgra.a);
                auto res = transform_pixel(rgba);
                row[x] = PremultipliedColorU8::from_rgba_unchecked(res.b, res.g, res.r, res.a);
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                if (auto px = pixmap.pixel(x, y)) {
                    pixmap.set_pixel(x, y, transform_pixel(*px));
                }
            }
        }
    }
}

void ColorEffect::apply(Pixmap& pixmap) const noexcept {
    auto mut = pixmap.as_mut();
    apply(mut);
}

void ColorFilter::invert(PixmapMut& pixmap) noexcept {
    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pixmap.row(static_cast<size_t>(y));
        for (int32_t x = 0; x < w; ++x) {
            auto& p = row[x];
            p.r = static_cast<uint8_t>(p.a - p.r);
            p.g = static_cast<uint8_t>(p.a - p.g);
            p.b = static_cast<uint8_t>(p.a - p.b);
        }
    }
}

void ColorFilter::grayscale(PixmapMut& pixmap) noexcept {
    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pixmap.row(static_cast<size_t>(y));
        for (int32_t x = 0; x < w; ++x) {
            auto& p = row[x];
            uint32_t lum = (77u * p.r + 150u * p.g + 29u * p.b + 128u) >> 8;
            uint8_t l = static_cast<uint8_t>(std::min(lum, static_cast<uint32_t>(p.a)));
            p.r = l;
            p.g = l;
            p.b = l;
        }
    }
}

void ColorFilter::adjust_brightness(PixmapMut& pixmap, float factor) noexcept {
    if (std::abs(factor) < 0.0001f) return;
    factor = std::clamp(factor, -1.0f, 1.0f);

    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pixmap.row(static_cast<size_t>(y));
        for (int32_t x = 0; x < w; ++x) {
            auto& p = row[x];
            if (p.a == 0) continue;

            if (factor > 0.0f) {
                p.r = static_cast<uint8_t>(p.r + (p.a - p.r) * factor + 0.5f);
                p.g = static_cast<uint8_t>(p.g + (p.a - p.g) * factor + 0.5f);
                p.b = static_cast<uint8_t>(p.b + (p.a - p.b) * factor + 0.5f);
            } else {
                float scale = 1.0f + factor; // in [0, 1]
                p.r = static_cast<uint8_t>(p.r * scale + 0.5f);
                p.g = static_cast<uint8_t>(p.g * scale + 0.5f);
                p.b = static_cast<uint8_t>(p.b * scale + 0.5f);
            }
        }
    }
}

void ColorFilter::adjust_contrast(PixmapMut& pixmap, float factor) noexcept {
    if (std::abs(factor - 1.0f) < 0.0001f || factor < 0.0f) return;

    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pixmap.row(static_cast<size_t>(y));
        for (int32_t x = 0; x < w; ++x) {
            auto& p = row[x];
            if (p.a == 0) continue;

            float mid = static_cast<float>(p.a) * 0.5f;

            auto apply = [mid, factor](uint8_t val, uint8_t alpha) -> uint8_t {
                float v = mid + (static_cast<float>(val) - mid) * factor;
                return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, static_cast<float>(alpha)));
            };

            p.r = apply(p.r, p.a);
            p.g = apply(p.g, p.a);
            p.b = apply(p.b, p.a);
        }
    }
}

void ColorFilter::sepia(PixmapMut& pixmap) noexcept {
    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pixmap.row(static_cast<size_t>(y));
        for (int32_t x = 0; x < w; ++x) {
            auto& p = row[x];
            if (p.a == 0) continue;

            float r = static_cast<float>(p.r);
            float g = static_cast<float>(p.g);
            float b = static_cast<float>(p.b);
            float a = static_cast<float>(p.a);

            float nr = 0.393f * r + 0.769f * g + 0.189f * b;
            float ng = 0.349f * r + 0.686f * g + 0.168f * b;
            float nb = 0.272f * r + 0.534f * g + 0.131f * b;

            p.r = static_cast<uint8_t>(std::min(nr, a));
            p.g = static_cast<uint8_t>(std::min(ng, a));
            p.b = static_cast<uint8_t>(std::min(nb, a));
        }
    }
}

void ColorFilter::tint(PixmapMut& pixmap, Color tint_color, float intensity) noexcept {
    if (intensity <= 0.0f) return;
    intensity = std::clamp(intensity, 0.0f, 1.0f);

    auto tc = tint_color.to_color_u8();
    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());

    for (int32_t y = 0; y < h; ++y) {
        auto* row = pixmap.row(static_cast<size_t>(y));
        for (int32_t x = 0; x < w; ++x) {
            auto& p = row[x];
            if (p.a == 0) continue;

            float af = static_cast<float>(p.a) / 255.0f;
            float target_r = static_cast<float>(tc.red()) * af;
            float target_g = static_cast<float>(tc.green()) * af;
            float target_b = static_cast<float>(tc.blue()) * af;

            p.r = static_cast<uint8_t>(p.r + (target_r - p.r) * intensity + 0.5f);
            p.g = static_cast<uint8_t>(p.g + (target_g - p.g) * intensity + 0.5f);
            p.b = static_cast<uint8_t>(p.b + (target_b - p.b) * intensity + 0.5f);
        }
    }
}

void ColorFilter::apply_matrix(PixmapMut& pixmap, const ColorMatrix& matrix) noexcept {
    matrix.apply(pixmap);
}

} // namespace nisaba::effects

