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

inline void rgb_to_hsl(float r, float g, float b, float& h, float& s, float& l) noexcept {
    const float max_c = std::max({r, g, b});
    const float min_c = std::min({r, g, b});
    l = 0.5f * (max_c + min_c);
    const float d = max_c - min_c;
    if (d <= 1e-6f) {
        h = 0.0f;
        s = 0.0f;
        return;
    }
    s = (l > 0.5f) ? (d / std::max(2.0f - max_c - min_c, 1e-6f)) : (d / std::max(max_c + min_c, 1e-6f));
    if (max_c == r) {
        h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    } else if (max_c == g) {
        h = (b - r) / d + 2.0f;
    } else {
        h = (r - g) / d + 4.0f;
    }
    h /= 6.0f;
}

inline float hue_to_rgb(float p, float q, float t) noexcept {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

inline void hsl_to_rgb(float h, float s, float l, float& r, float& g, float& b) noexcept {
    h = h - std::floor(h);
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);
    if (s <= 1e-6f) {
        r = g = b = l;
        return;
    }
    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
}

} // namespace

bool ColorEffect::is_extended() const noexcept {
    return ext_node != nullptr && ext_node->kind != ColorEffectExtKind::None;
}

bool ColorEffect::is_active() const noexcept {
    return has_matrix || transfer != ColorTransfer::None || has_blend || is_extended();
}

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

ColorEffect ColorEffect::table(const std::array<uint8_t, 256>& rgb_lut) {
    return table_argb(nullptr, rgb_lut.data(), rgb_lut.data(), rgb_lut.data());
}

ColorEffect ColorEffect::table_argb(
    const uint8_t* a_lut,
    const uint8_t* r_lut,
    const uint8_t* g_lut,
    const uint8_t* b_lut
) {
    auto node = std::make_shared<ColorEffectExtNode>();
    node->kind = ColorEffectExtKind::Table;
    for (int i = 0; i < 256; ++i) {
        const uint8_t id = static_cast<uint8_t>(i);
        node->lut_a[i] = a_lut ? a_lut[i] : id;
        node->lut_r[i] = r_lut ? r_lut[i] : id;
        node->lut_g[i] = g_lut ? g_lut[i] : id;
        node->lut_b[i] = b_lut ? b_lut[i] : id;
    }
    ColorEffect e;
    e.ext_node = std::move(node);
    return e;
}

ColorEffect ColorEffect::hsla_matrix(const ColorMatrix& mat) {
    auto node = std::make_shared<ColorEffectExtNode>();
    node->kind = ColorEffectExtKind::HslaMatrix;
    node->hsla_matrix = mat;
    ColorEffect e;
    e.ext_node = std::move(node);
    return e;
}

ColorEffect ColorEffect::luma() {
    auto node = std::make_shared<ColorEffectExtNode>();
    node->kind = ColorEffectExtKind::Luma;
    ColorEffect e;
    e.ext_node = std::move(node);
    return e;
}

ColorEffect ColorEffect::high_contrast(const HighContrastConfig& cfg) {
    auto node = std::make_shared<ColorEffectExtNode>();
    node->kind = ColorEffectExtKind::HighContrast;
    node->high_contrast = cfg;
    ColorEffect e;
    e.ext_node = std::move(node);
    return e;
}

ColorEffect ColorEffect::lerp(float t, const ColorEffect& cf0, const ColorEffect& cf1) {
    const float ct = std::clamp(t, 0.0f, 1.0f);
    if (ct <= 0.0f) return cf0;
    if (ct >= 1.0f) return cf1;
    auto node = std::make_shared<ColorEffectExtNode>();
    node->kind = ColorEffectExtKind::Lerp;
    node->lerp_t = ct;
    node->child0 = std::make_shared<ColorEffect>(cf0);
    node->child1 = std::make_shared<ColorEffect>(cf1);
    ColorEffect e;
    e.ext_node = std::move(node);
    return e;
}

std::optional<ColorEffect> ColorEffect::compose(const ColorEffect& outer, const ColorEffect& inner) {
    if (!inner.is_active()) return outer;
    if (!outer.is_active()) return inner;

    const bool can_fold = !inner.is_extended() && !outer.is_extended() &&
                          !(inner.has_blend && (outer.has_matrix || outer.transfer != ColorTransfer::None || outer.has_blend)) &&
                          !(inner.transfer != ColorTransfer::None && outer.has_matrix);
    if (can_fold) {
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

    auto node = std::make_shared<ColorEffectExtNode>();
    node->kind = ColorEffectExtKind::Compose;
    node->child0 = std::make_shared<ColorEffect>(outer);
    node->child1 = std::make_shared<ColorEffect>(inner);
    ColorEffect out;
    out.ext_node = std::move(node);
    return out;
}

PremultipliedColor ColorEffect::transform_premul(PremultipliedColor c) const noexcept {
    if (is_extended()) {
        const auto& en = *ext_node;
        switch (en.kind) {
            case ColorEffectExtKind::Table: {
                const Color s = c.demultiply();
                const uint8_t r8 = static_cast<uint8_t>(std::clamp(s.red() * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint8_t g8 = static_cast<uint8_t>(std::clamp(s.green() * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint8_t b8 = static_cast<uint8_t>(std::clamp(s.blue() * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint8_t a8 = static_cast<uint8_t>(std::clamp(s.alpha() * 255.0f + 0.5f, 0.0f, 255.0f));
                constexpr float inv = 1.0f / 255.0f;
                return Color::from_rgba_unchecked(
                    en.lut_r[r8] * inv, en.lut_g[g8] * inv, en.lut_b[b8] * inv, en.lut_a[a8] * inv
                ).premultiply();
            }
            case ColorEffectExtKind::HslaMatrix: {
                const Color s = c.demultiply();
                float h = 0.0f, sat = 0.0f, l = 0.0f;
                rgb_to_hsl(s.red(), s.green(), s.blue(), h, sat, l);
                const auto& m = en.hsla_matrix.m;
                const float a = s.alpha();
                const float nh = m[0] * h + m[1] * sat + m[2] * l + m[3] * a + m[4];
                const float ns = std::clamp(m[5] * h + m[6] * sat + m[7] * l + m[8] * a + m[9], 0.0f, 1.0f);
                const float nl = std::clamp(m[10] * h + m[11] * sat + m[12] * l + m[13] * a + m[14], 0.0f, 1.0f);
                const float na = std::clamp(m[15] * h + m[16] * sat + m[17] * l + m[18] * a + m[19], 0.0f, 1.0f);
                float nr = 0.0f, ng = 0.0f, nb = 0.0f;
                hsl_to_rgb(nh, ns, nl, nr, ng, nb);
                return Color::from_rgba_unchecked(nr, ng, nb, na).premultiply();
            }
            case ColorEffectExtKind::Luma: {
                const float lum = std::clamp(
                    0.2126f * c.red() + 0.7152f * c.green() + 0.0722f * c.blue(),
                    0.0f, 1.0f
                );
                return PremultipliedColor(
                    NormalizedF32::ZERO, NormalizedF32::ZERO, NormalizedF32::ZERO,
                    NormalizedF32::create_clamped(lum)
                );
            }
            case ColorEffectExtKind::HighContrast: {
                const Color s = c.demultiply();
                float r = s.red(), g = s.green(), b = s.blue(), a = s.alpha();
                if (en.high_contrast.grayscale) {
                    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = g = b = l;
                }
                if (en.high_contrast.invert_style == HighContrastInvertStyle::InvertBrightness) {
                    r = 1.0f - r; g = 1.0f - g; b = 1.0f - b;
                } else if (en.high_contrast.invert_style == HighContrastInvertStyle::InvertLightness) {
                    float h = 0.0f, sat = 0.0f, l = 0.0f;
                    rgb_to_hsl(r, g, b, h, sat, l);
                    hsl_to_rgb(h, sat, 1.0f - l, r, g, b);
                }
                if (std::abs(en.high_contrast.contrast) > 1e-5f) {
                    const float cf = std::clamp(en.high_contrast.contrast, -0.99f, 0.99f);
                    const float m = (1.0f + cf) / (1.0f - cf);
                    r = std::clamp(m * (r - 0.5f) + 0.5f, 0.0f, 1.0f);
                    g = std::clamp(m * (g - 0.5f) + 0.5f, 0.0f, 1.0f);
                    b = std::clamp(m * (b - 0.5f) + 0.5f, 0.0f, 1.0f);
                }
                return Color::from_rgba_unchecked(r, g, b, a).premultiply();
            }
            case ColorEffectExtKind::Compose: {
                PremultipliedColor tmp = en.child1 ? en.child1->transform_premul(c) : c;
                return en.child0 ? en.child0->transform_premul(tmp) : tmp;
            }
            case ColorEffectExtKind::Lerp: {
                const PremultipliedColor c0 = en.child0 ? en.child0->transform_premul(c) : c;
                const PremultipliedColor c1 = en.child1 ? en.child1->transform_premul(c) : c;
                const float t = en.lerp_t;
                const float it = 1.0f - t;
                const float a = std::clamp(c0.alpha() * it + c1.alpha() * t, 0.0f, 1.0f);
                const float r = std::clamp(c0.red() * it + c1.red() * t, 0.0f, a);
                const float g = std::clamp(c0.green() * it + c1.green() * t, 0.0f, a);
                const float b = std::clamp(c0.blue() * it + c1.blue() * t, 0.0f, a);
                return PremultipliedColor(
                    NormalizedF32::create_clamped(r), NormalizedF32::create_clamped(g),
                    NormalizedF32::create_clamped(b), NormalizedF32::create_clamped(a)
                );
            }
            default:
                break;
        }
    }

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
    if (!is_extended() && transfer == ColorTransfer::None && !has_blend && has_matrix) {
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
    if (!is_extended() && has_matrix && transfer == ColorTransfer::None && !has_blend) {
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

