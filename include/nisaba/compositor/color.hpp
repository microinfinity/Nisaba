#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "nisaba/color/color.hpp"

namespace nisaba::compositor {

struct OklabColor;

/**
 * @brief Sovereign RGBA Color representation for compositor rendering.
 * Floating point components are normalized in range [0.0f, 1.0f].
 */
struct Color {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};

    constexpr Color() noexcept = default;
    constexpr Color(float red, float green, float blue, float alpha = 1.0f) noexcept
        : r(red), g(green), b(blue), a(alpha) {}

    constexpr Color(const nisaba::ColorU8& c) noexcept
        : r(static_cast<float>(c.r) / 255.0f),
          g(static_cast<float>(c.g) / 255.0f),
          b(static_cast<float>(c.b) / 255.0f),
          a(static_cast<float>(c.a) / 255.0f) {}

    constexpr bool operator==(const Color& other) const noexcept = default;

    static constexpr Color fromRgba(float r, float g, float b, float a = 1.0f) noexcept {
        return Color{r, g, b, a};
    }

    static constexpr Color fromRgb255(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
        return Color{
            static_cast<float>(r) / 255.0f,
            static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f,
            static_cast<float>(a) / 255.0f
        };
    }

    [[nodiscard]] constexpr Color premultiplied() const noexcept {
        return Color{r * a, g * a, b * a, a};
    }

    [[nodiscard]] constexpr nisaba::ColorU8 toColorU8() const noexcept {
        return nisaba::ColorU8(
            static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f),
            static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f),
            static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f),
            static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)
        );
    }

    [[nodiscard]] inline OklabColor toOklab() const noexcept;

    static constexpr Color transparent() noexcept { return Color{0.0f, 0.0f, 0.0f, 0.0f}; }
    static constexpr Color black() noexcept { return Color{0.0f, 0.0f, 0.0f, 1.0f}; }
    static constexpr Color white() noexcept { return Color{1.0f, 1.0f, 1.0f, 1.0f}; }
    static constexpr Color red() noexcept { return Color{1.0f, 0.0f, 0.0f, 1.0f}; }
    static constexpr Color green() noexcept { return Color{0.0f, 1.0f, 0.0f, 1.0f}; }
    static constexpr Color blue() noexcept { return Color{0.0f, 0.0f, 1.0f, 1.0f}; }
};

/**
 * @brief Perceptually uniform Oklab color representation (L, a, b, alpha).
 * Used for mud-free multi-stop gradient interpolation in borders and shadows.
 */
struct OklabColor {
    float L{0.0f};
    float a{0.0f};
    float b{0.0f};
    float alpha{1.0f};

    constexpr OklabColor() noexcept = default;
    constexpr OklabColor(float lVal, float aVal, float bVal, float alphaVal = 1.0f) noexcept
        : L(lVal), a(aVal), b(bVal), alpha(alphaVal) {}

    [[nodiscard]] static constexpr OklabColor lerp(const OklabColor& c0, const OklabColor& c1, float t) noexcept {
        return OklabColor{
            c0.L + (c1.L - c0.L) * t,
            c0.a + (c1.a - c0.a) * t,
            c0.b + (c1.b - c0.b) * t,
            c0.alpha + (c1.alpha - c0.alpha) * t
        };
    }

    [[nodiscard]] inline Color toSrgb() const noexcept {
        const float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
        const float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
        const float s_ = L - 0.0894841775f * a - 1.2914855480f * b;

        const float l = l_ * l_ * l_;
        const float m = m_ * m_ * m_;
        const float s = s_ * s_ * s_;

        const float rLin = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
        const float gLin = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
        const float bLin = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

        auto linToSrgb = [](float x) noexcept -> float {
            x = std::clamp(x, 0.0f, 1.0f);
            return (x <= 0.0031308f) ? (12.92f * x) : (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f);
        };

        return Color{linToSrgb(rLin), linToSrgb(gLin), linToSrgb(bLin), std::clamp(alpha, 0.0f, 1.0f)};
    }
};

inline OklabColor Color::toOklab() const noexcept {
    auto srgbToLin = [](float x) noexcept -> float {
        x = std::clamp(x, 0.0f, 1.0f);
        return (x <= 0.04045f) ? (x / 12.92f) : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };

    const float rLin = srgbToLin(r);
    const float gLin = srgbToLin(g);
    const float bLin = srgbToLin(b);

    const float l = 0.4122214708f * rLin + 0.5363325363f * gLin + 0.0514459929f * bLin;
    const float m = 0.2119034982f * rLin + 0.6806995451f * gLin + 0.1073969566f * bLin;
    const float s = 0.0883024619f * rLin + 0.2817188376f * gLin + 0.6299787005f * bLin;

    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);

    return OklabColor{
        0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
        a
    };
}

/**
 * @brief Multi-stop directional/rotational gradient specification with Oklab interpolation.
 */
struct GradientSpec {
    static constexpr size_t kMaxStops = 10;

    std::vector<Color> stops;
    float angleRad{0.0f};

    GradientSpec() = default;
    explicit GradientSpec(const Color& singleColor, float angle = 0.0f)
        : stops{singleColor}, angleRad(angle) {}
    GradientSpec(std::vector<Color> colorStops, float angle = 0.0f)
        : stops(std::move(colorStops)), angleRad(angle) {}

    [[nodiscard]] bool isEmpty() const noexcept { return stops.empty(); }

    [[nodiscard]] Color primaryColor() const noexcept {
        return stops.empty() ? Color::transparent() : stops.front();
    }

    /**
     * @brief Evaluates the gradient at normalized position t in [0.0, 1.0] in Oklab space.
     */
    [[nodiscard]] Color evaluateOklab(float t) const noexcept {
        if (stops.empty()) return Color::transparent();
        if (stops.size() == 1) return stops.front();

        t = std::clamp(t, 0.0f, 1.0f);
        const float scaled = t * static_cast<float>(stops.size() - 1);
        const size_t idx0 = std::min(static_cast<size_t>(scaled), stops.size() - 1);
        const size_t idx1 = std::min(idx0 + 1, stops.size() - 1);
        const float frac = scaled - static_cast<float>(idx0);

        const OklabColor ok0 = stops[idx0].toOklab();
        const OklabColor ok1 = stops[idx1].toOklab();
        return OklabColor::lerp(ok0, ok1, frac).toSrgb();
    }

    /**
     * @brief Packs up to kMaxStops colors into flat [L, a, b, alpha] float array for GLSL uniform upload.
     */
    [[nodiscard]] std::vector<float> packOklabStops() const {
        const size_t count = std::min(stops.size(), kMaxStops);
        std::vector<float> packed;
        packed.reserve(count * 4);
        for (size_t i = 0; i < count; ++i) {
            const OklabColor ok = stops[i].toOklab();
            packed.push_back(ok.L);
            packed.push_back(ok.a);
            packed.push_back(ok.b);
            packed.push_back(ok.alpha);
        }
        return packed;
    }
};

} // namespace nisaba::compositor
