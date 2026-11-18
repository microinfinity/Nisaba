#pragma once

#include <cstdint>
#include <optional>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/effects/color_matrix.hpp"

namespace nisaba::effects {

/// Gamma transfer function applied by a ColorEffect.
enum class ColorTransfer : uint8_t {
    None = 0,
    SrgbToLinear = 1,
    LinearToSrgb = 2,
};

/// Value-typed Paint-level color filter supporting 4x5 matrices, gamma transfers,
/// and constant-color BlendMode compositing with full CPU/GPU parity.
class ColorEffect {
public:
    bool has_matrix{false};
    ColorMatrix matrix{};
    ColorTransfer transfer{ColorTransfer::None};
    bool has_blend{false};
    Color blend_color{Color::TRANSPARENT};
    BlendMode blend_mode{BlendMode::SourceOver};

    constexpr ColorEffect() noexcept = default;

    [[nodiscard]] static constexpr ColorEffect from_matrix(const ColorMatrix& mat) noexcept {
        ColorEffect e;
        e.has_matrix = true;
        e.matrix = mat;
        return e;
    }

    [[nodiscard]] static constexpr ColorEffect mode(Color color, BlendMode mode) noexcept {
        ColorEffect e;
        e.has_blend = true;
        e.blend_color = color;
        e.blend_mode = mode;
        return e;
    }

    [[nodiscard]] static ColorEffect tint(Color color, float intensity = 1.0f) noexcept;

    [[nodiscard]] static ColorEffect grayscale() noexcept {
        return from_matrix(ColorMatrix::grayscale());
    }

    [[nodiscard]] static ColorEffect sepia(float intensity = 1.0f) noexcept {
        return from_matrix(ColorMatrix::sepia(intensity));
    }

    [[nodiscard]] static ColorEffect invert() noexcept {
        return from_matrix(ColorMatrix::invert());
    }

    [[nodiscard]] static constexpr ColorEffect srgb_to_linear() noexcept {
        ColorEffect e;
        e.transfer = ColorTransfer::SrgbToLinear;
        return e;
    }

    [[nodiscard]] static constexpr ColorEffect linear_to_srgb() noexcept {
        ColorEffect e;
        e.transfer = ColorTransfer::LinearToSrgb;
        return e;
    }

    /// Composes `outer(inner(c))` when stages fit the Matrix -> Transfer -> Blend pipeline.
    [[nodiscard]] static std::optional<ColorEffect> compose(const ColorEffect& outer, const ColorEffect& inner) noexcept;

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return has_matrix || transfer != ColorTransfer::None || has_blend;
    }

    /// Transforms an unpremultiplied Color in [0.0, 1.0].
    [[nodiscard]] Color transform(Color c) const noexcept;

    /// Transforms a PremultipliedColor in [0.0, 1.0].
    [[nodiscard]] PremultipliedColor transform_premul(PremultipliedColor c) const noexcept;

    /// Transforms an 8-bit PremultipliedColorU8 RGBA pixel.
    [[nodiscard]] PremultipliedColorU8 transform_pixel(PremultipliedColorU8 p) const noexcept;

    /// Transforms all pixels of a PixmapMut in-place (RGBA8888, BGRA8888, etc.).
    void apply(PixmapMut& pixmap) const noexcept;
    void apply(Pixmap& pixmap) const noexcept;
};

/// Fast in-place pixel-level color transformations for Pixmaps.
class ColorFilter {
public:
    /// Inverts RGB channels while preserving alpha.
    static void invert(PixmapMut& pixmap) noexcept;

    /// Converts to perceptual luminance grayscale while preserving alpha.
    static void grayscale(PixmapMut& pixmap) noexcept;

    /// Adjusts brightness in-place.
    /// factor in [-1.0, 1.0]: negative darkens towards black, positive lightens towards white.
    static void adjust_brightness(PixmapMut& pixmap, float factor) noexcept;

    /// Adjusts contrast in-place.
    /// factor > 1.0 increases contrast, factor < 1.0 decreases contrast.
    static void adjust_contrast(PixmapMut& pixmap, float factor) noexcept;

    /// Applies a warm nostalgic sepia tone.
    static void sepia(PixmapMut& pixmap) noexcept;

    /// Tints the pixmap with a solid color according to intensity [0.0, 1.0].
    static void tint(PixmapMut& pixmap, Color tint_color, float intensity) noexcept;

    /// Applies a 4x5 ColorMatrix filter in-place to the pixmap.
    static void apply_matrix(PixmapMut& pixmap, const class ColorMatrix& matrix) noexcept;
};

} // namespace nisaba::effects
