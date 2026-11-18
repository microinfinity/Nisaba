#pragma once

#include <algorithm>
#include <cmath>
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"

namespace nisaba {

namespace detail {

inline float hsl_lum(float r, float g, float b) noexcept {
    return 0.3f * r + 0.59f * g + 0.11f * b;
}

inline void hsl_set_lum(float& r, float& g, float& b, float l) noexcept {
    float d = l - hsl_lum(r, g, b);
    r += d;
    g += d;
    b += d;
    float lum = hsl_lum(r, g, b);
    float mn = std::min({r, g, b});
    float mx = std::max({r, g, b});
    if (mn < 0.0f) {
        float denom = lum - mn;
        if (denom > 1e-6f) {
            r = lum + (r - lum) * lum / denom;
            g = lum + (g - lum) * lum / denom;
            b = lum + (b - lum) * lum / denom;
        } else {
            r = g = b = lum;
        }
    }
    if (mx > 1.0f) {
        float denom = mx - lum;
        if (denom > 1e-6f) {
            r = lum + (r - lum) * (1.0f - lum) / denom;
            g = lum + (g - lum) * (1.0f - lum) / denom;
            b = lum + (b - lum) * (1.0f - lum) / denom;
        } else {
            r = g = b = lum;
        }
    }
}

inline float hsl_sat(float r, float g, float b) noexcept {
    return std::max({r, g, b}) - std::min({r, g, b});
}

inline void hsl_set_sat(float& r, float& g, float& b, float s) noexcept {
    float* c[3] = {&r, &g, &b};
    if (*c[0] > *c[1]) std::swap(c[0], c[1]);
    if (*c[1] > *c[2]) std::swap(c[1], c[2]);
    if (*c[0] > *c[1]) std::swap(c[0], c[1]);
    if (*c[2] > *c[0]) {
        *c[1] = ((*c[1] - *c[0]) * s) / (*c[2] - *c[0]);
        *c[2] = s;
    } else {
        *c[1] = *c[2] = 0.0f;
    }
    *c[0] = 0.0f;
}

} // namespace detail

struct BlendResult {
    float r, g, b, a;
};

inline BlendResult blend_components(
    float sr, float sg, float sb, float sa,
    float dr, float dg, float db, float da,
    BlendMode mode
) noexcept {
    auto inv = [](float v) noexcept { return 1.0f - v; };
    auto mad = [](float a, float b, float c) noexcept { return a * b + c; };

    switch (mode) {
        case BlendMode::Clear:
            return {0.0f, 0.0f, 0.0f, 0.0f};

        case BlendMode::Source:
            return {sr, sg, sb, sa};

        case BlendMode::Destination:
            return {dr, dg, db, da};

        case BlendMode::SourceOver:
            return {
                mad(dr, inv(sa), sr),
                mad(dg, inv(sa), sg),
                mad(db, inv(sa), sb),
                mad(da, inv(sa), sa)
            };

        case BlendMode::DestinationOver:
            return {
                mad(sr, inv(da), dr),
                mad(sg, inv(da), dg),
                mad(sb, inv(da), db),
                mad(sa, inv(da), da)
            };

        case BlendMode::SourceIn:
            return {sr * da, sg * da, sb * da, sa * da};

        case BlendMode::DestinationIn:
            return {dr * sa, dg * sa, db * sa, da * sa};

        case BlendMode::SourceOut:
            return {sr * inv(da), sg * inv(da), sb * inv(da), sa * inv(da)};

        case BlendMode::DestinationOut:
            return {dr * inv(sa), dg * inv(sa), db * inv(sa), da * inv(sa)};

        case BlendMode::SourceAtop:
            return {
                sr * da + dr * inv(sa),
                sg * da + dg * inv(sa),
                sb * da + db * inv(sa),
                da
            };

        case BlendMode::DestinationAtop:
            return {
                dr * sa + sr * inv(da),
                dg * sa + sg * inv(da),
                db * sa + sb * inv(da),
                sa
            };

        case BlendMode::Xor:
            return {
                sr * inv(da) + dr * inv(sa),
                sg * inv(da) + dg * inv(sa),
                sb * inv(da) + db * inv(sa),
                sa * inv(da) + da * inv(sa)
            };

        case BlendMode::Plus:
            return {
                std::min(sr + dr, 1.0f),
                std::min(sg + dg, 1.0f),
                std::min(sb + db, 1.0f),
                std::min(sa + da, 1.0f)
            };

        case BlendMode::Modulate:
            return {sr * dr, sg * dg, sb * db, sa * da};

        case BlendMode::Screen:
            return {
                sr + dr - sr * dr,
                sg + dg - sg * dg,
                sb + db - sb * db,
                sa + da - sa * da
            };

        case BlendMode::Multiply:
            return {
                sr * inv(da) + dr * inv(sa) + sr * dr,
                sg * inv(da) + dg * inv(sa) + sg * dg,
                sb * inv(da) + db * inv(sa) + sb * db,
                sa * inv(da) + da * inv(sa) + sa * da
            };

        case BlendMode::Darken:
            return {
                sr + dr - std::max(sr * da, dr * sa),
                sg + dg - std::max(sg * da, dg * sa),
                sb + db - std::max(sb * da, db * sa),
                mad(da, inv(sa), sa)
            };

        case BlendMode::Lighten:
            return {
                sr + dr - std::min(sr * da, dr * sa),
                sg + dg - std::min(sg * da, dg * sa),
                sb + db - std::min(sb * da, db * sa),
                mad(da, inv(sa), sa)
            };

        case BlendMode::Difference:
            return {
                sr + dr - 2.0f * std::min(sr * da, dr * sa),
                sg + dg - 2.0f * std::min(sg * da, dg * sa),
                sb + db - 2.0f * std::min(sb * da, db * sa),
                mad(da, inv(sa), sa)
            };

        case BlendMode::Exclusion:
            return {
                sr + dr - 2.0f * sr * dr,
                sg + dg - 2.0f * sg * dg,
                sb + db - 2.0f * sb * db,
                mad(da, inv(sa), sa)
            };

        case BlendMode::Overlay: {
            auto ch = [&](float s, float d) noexcept {
                return (2.0f * d <= da)
                    ? (2.0f * s * d)
                    : (sa * da - 2.0f * (da - d) * (sa - s));
            };
            return {
                sr * inv(da) + dr * inv(sa) + ch(sr, dr),
                sg * inv(da) + dg * inv(sa) + ch(sg, dg),
                sb * inv(da) + db * inv(sa) + ch(sb, db),
                mad(da, inv(sa), sa)
            };
        }

        case BlendMode::HardLight: {
            auto ch = [&](float s, float d) noexcept {
                return (2.0f * s <= sa)
                    ? (2.0f * s * d)
                    : (sa * da - 2.0f * (da - d) * (sa - s));
            };
            return {
                sr * inv(da) + dr * inv(sa) + ch(sr, dr),
                sg * inv(da) + dg * inv(sa) + ch(sg, dg),
                sb * inv(da) + db * inv(sa) + ch(sb, db),
                mad(da, inv(sa), sa)
            };
        }

        case BlendMode::ColorDodge: {
            auto ch = [&](float sc, float dc) noexcept -> float {
                if (dc <= 0.0f) return sc * inv(da);
                float m = 0.0f;
                if (sa > 0.0f) {
                    m = (sc >= sa)
                        ? (sa * da)
                        : std::min(sa * da, (dc * sa * sa) / (sa - sc));
                }
                return sc * inv(da) + dc * inv(sa) + m;
            };
            return {ch(sr, dr), ch(sg, dg), ch(sb, db), mad(da, inv(sa), sa)};
        }

        case BlendMode::ColorBurn: {
            auto ch = [&](float sc, float dc) noexcept -> float {
                if (dc >= da) return sa * da + sc * inv(da) + dc * inv(sa);
                if (sc <= 0.0f) return dc * inv(sa);
                float sub = std::min(sa * da, ((da - dc) * sa * sa) / sc);
                float m = sa * da - sub;
                return sc * inv(da) + dc * inv(sa) + m;
            };
            return {ch(sr, dr), ch(sg, dg), ch(sb, db), mad(da, inv(sa), sa)};
        }

        case BlendMode::SoftLight: {
            auto ch = [&](float sc, float dc) noexcept -> float {
                if (da <= 0.0f) return sc;
                if (sa <= 0.0f) return dc;
                float d_norm = std::clamp(dc / da, 0.0f, 1.0f);
                float d_func = (d_norm <= 0.25f)
                    ? (((16.0f * d_norm - 12.0f) * d_norm + 4.0f) * d_norm)
                    : std::sqrt(d_norm);
                float m = (2.0f * sc <= sa)
                    ? (dc * (sa + (2.0f * sc - sa) * (1.0f - d_norm)))
                    : (dc * sa + (2.0f * sc - sa) * (d_func * da - dc));
                return sc * inv(da) + dc * inv(sa) + m;
            };
            return {ch(sr, dr), ch(sg, dg), ch(sb, db), mad(da, inv(sa), sa)};
        }

        case BlendMode::Hue:
        case BlendMode::Saturation:
        case BlendMode::Color:
        case BlendMode::Luminosity: {
            if (sa <= 0.0f) return {dr, dg, db, da};
            if (da <= 0.0f) return {sr, sg, sb, sa};
            float s_r = std::clamp(sr / sa, 0.0f, 1.0f);
            float s_g = std::clamp(sg / sa, 0.0f, 1.0f);
            float s_b = std::clamp(sb / sa, 0.0f, 1.0f);
            float d_r = std::clamp(dr / da, 0.0f, 1.0f);
            float d_g = std::clamp(dg / da, 0.0f, 1.0f);
            float d_b = std::clamp(db / da, 0.0f, 1.0f);
            float c_r = d_r, c_g = d_g, c_b = d_b;
            if (mode == BlendMode::Hue) {
                c_r = s_r; c_g = s_g; c_b = s_b;
                detail::hsl_set_sat(c_r, c_g, c_b, detail::hsl_sat(d_r, d_g, d_b));
                detail::hsl_set_lum(c_r, c_g, c_b, detail::hsl_lum(d_r, d_g, d_b));
            } else if (mode == BlendMode::Saturation) {
                detail::hsl_set_sat(c_r, c_g, c_b, detail::hsl_sat(s_r, s_g, s_b));
                detail::hsl_set_lum(c_r, c_g, c_b, detail::hsl_lum(d_r, d_g, d_b));
            } else if (mode == BlendMode::Color) {
                c_r = s_r; c_g = s_g; c_b = s_b;
                detail::hsl_set_lum(c_r, c_g, c_b, detail::hsl_lum(d_r, d_g, d_b));
            } else {
                detail::hsl_set_lum(c_r, c_g, c_b, detail::hsl_lum(s_r, s_g, s_b));
            }
            return {
                sr * inv(da) + dr * inv(sa) + sa * da * c_r,
                sg * inv(da) + dg * inv(sa) + sa * da * c_g,
                sb * inv(da) + db * inv(sa) + sa * da * c_b,
                mad(da, inv(sa), sa)
            };
        }

        default:
            return {
                mad(dr, inv(sa), sr),
                mad(dg, inv(sa), sg),
                mad(db, inv(sa), sb),
                mad(da, inv(sa), sa)
            };
    }
}

inline PremultipliedColor blend_colors(PremultipliedColor src, PremultipliedColor dst, BlendMode mode) noexcept {
    auto res = blend_components(
        src.red(), src.green(), src.blue(), src.alpha(),
        dst.red(), dst.green(), dst.blue(), dst.alpha(),
        mode
    );
    return PremultipliedColor(
        NormalizedF32::create_clamped(res.r),
        NormalizedF32::create_clamped(res.g),
        NormalizedF32::create_clamped(res.b),
        NormalizedF32::create_clamped(res.a)
    );
}

inline PremultipliedColorU8 blend_pixels(PremultipliedColorU8 src, PremultipliedColorU8 dst, BlendMode mode) noexcept {
    if (mode == BlendMode::SourceOver && src.alpha() == 255) return src;
    if (mode == BlendMode::Source) return src;
    if (mode == BlendMode::Destination) return dst;
    if (mode == BlendMode::Clear) return PremultipliedColorU8::TRANSPARENT;

    constexpr float inv_255 = 1.0f / 255.0f;
    auto res = blend_components(
        static_cast<float>(src.red()) * inv_255,
        static_cast<float>(src.green()) * inv_255,
        static_cast<float>(src.blue()) * inv_255,
        static_cast<float>(src.alpha()) * inv_255,
        static_cast<float>(dst.red()) * inv_255,
        static_cast<float>(dst.green()) * inv_255,
        static_cast<float>(dst.blue()) * inv_255,
        static_cast<float>(dst.alpha()) * inv_255,
        mode
    );

    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(std::clamp(res.r * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(res.g * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(res.b * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(res.a * 255.0f + 0.5f, 0.0f, 255.0f))
    );
}

} // namespace nisaba
