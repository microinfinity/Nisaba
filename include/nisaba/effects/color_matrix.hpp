#pragma once

#include <array>
#include <cmath>
#include <algorithm>
#include "nisaba/color/color.hpp"
#include "nisaba/canvas/pixmap.hpp"

namespace nisaba::effects {

/// A 4x5 color transformation matrix representing an affine transformation in RGBA color space.
///
/// Output color is computed as:
///   R' = m[0]*R + m[1]*G + m[2]*B + m[3]*A + m[4]
///   G' = m[5]*R + m[6]*G + m[7]*B + m[8]*A + m[9]
///   B' = m[10]*R + m[11]*G + m[12]*B + m[13]*A + m[14]
///   A' = m[15]*R + m[16]*G + m[17]*B + m[18]*A + m[19]
class ColorMatrix {
public:
    std::array<float, 20> m{};

    constexpr ColorMatrix() noexcept {
        m[0] = 1.0f;
        m[6] = 1.0f;
        m[12] = 1.0f;
        m[18] = 1.0f;
    }

    constexpr explicit ColorMatrix(const std::array<float, 20>& values) noexcept : m(values) {}

    static constexpr ColorMatrix identity() noexcept {
        return ColorMatrix();
    }

    /// Scaling matrix for RGBA channels.
    static ColorMatrix scale(float r, float g, float b, float a = 1.0f) noexcept;

    /// Translation matrix (offsets in range [-1.0, 1.0] for normalized colors).
    static ColorMatrix translate(float dr, float dg, float db, float da = 0.0f) noexcept;

    /// Saturation matrix using BT.709 luminance coefficients.
    /// s = 1.0 (identity), s = 0.0 (grayscale luminance), s > 1.0 (hypersaturated).
    static ColorMatrix saturation(float s) noexcept;

    /// Luminance grayscale matrix (equivalent to saturation(0.0f)).
    static ColorMatrix grayscale() noexcept { return saturation(0.0f); }

    /// Hue rotation around luminance axis by specified degrees.
    static ColorMatrix hue_rotate(float degrees) noexcept;

    /// Adjusts brightness. factor in [-1.0, 1.0].
    static ColorMatrix brightness(float factor) noexcept;

    /// Adjusts contrast around midpoint 0.5. factor > 0 (1.0 = normal).
    static ColorMatrix contrast(float factor) noexcept;

    /// Warm nostalgic sepia tone with intensity in [0.0, 1.0].
    static ColorMatrix sepia(float intensity = 1.0f) noexcept;

    /// Color inversion (negates RGB channels while keeping alpha).
    static ColorMatrix invert() noexcept;

    /// Multiply two 4x5 matrices (with implicit 5th row [0, 0, 0, 0, 1]).
    [[nodiscard]] ColorMatrix multiply(const ColorMatrix& other) const noexcept;

    ColorMatrix operator*(const ColorMatrix& other) const noexcept {
        return multiply(other);
    }

    /// Transforms an unpremultiplied Color, clamping output channels to [0.0, 1.0].
    [[nodiscard]] Color transform(Color c) const noexcept;

    /// Transforms a PremultipliedColor (unpremultiplies, transforms, repremultiplies).
    [[nodiscard]] PremultipliedColor transform_premul(PremultipliedColor c) const noexcept;

    /// Transforms an 8-bit PremultipliedColorU8 pixel.
    [[nodiscard]] PremultipliedColorU8 transform_pixel(PremultipliedColorU8 p) const noexcept;

    /// Transforms a span of PremultipliedColorU8 pixels.
    void transform_span(const PremultipliedColorU8* src, PremultipliedColorU8* dst, size_t count) const noexcept;

    /// In-place transformation of all pixels in a Pixmap or PixmapMut.
    void apply(PixmapMut& pixmap) const noexcept;
    void apply(Pixmap& pixmap) const noexcept;
};

} // namespace nisaba::effects
