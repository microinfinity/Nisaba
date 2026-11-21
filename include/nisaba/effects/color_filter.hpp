#pragma once

#include <array>
#include <cstdint>
#include <memory>
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

/// Inversion mode for HighContrastFilter.
enum class HighContrastInvertStyle : uint8_t {
    NoInvert = 0,
    InvertBrightness = 1,
    InvertLightness = 2,
};

/// Configuration for accessibility HighContrastFilter.
struct HighContrastConfig {
    bool grayscale{false};
    HighContrastInvertStyle invert_style{HighContrastInvertStyle::NoInvert};
    float contrast{0.0f}; ///< In [-1.0, 1.0]
};

/// Extended ColorEffect operation kind (RTTI-free).
enum class ColorEffectExtKind : uint8_t {
    None = 0,
    Table = 1,
    HslaMatrix = 2,
    Luma = 3,
    HighContrast = 4,
    Compose = 5,
    Lerp = 6,
};

class ColorEffect;

/// Immutable node for extended ColorEffect operations (LUTs, HSLA, Luma, HighContrast, Compose, Lerp).
struct ColorEffectExtNode {
    ColorEffectExtKind kind{ColorEffectExtKind::None};
    std::array<uint8_t, 256> lut_a{};
    std::array<uint8_t, 256> lut_r{};
    std::array<uint8_t, 256> lut_g{};
    std::array<uint8_t, 256> lut_b{};
    ColorMatrix hsla_matrix{};
    HighContrastConfig high_contrast{};
    float lerp_t{0.0f};
    std::shared_ptr<const ColorEffect> child0{nullptr};
    std::shared_ptr<const ColorEffect> child1{nullptr};
};

/// Value-typed Paint-level color filter supporting 4x5 matrices, gamma transfers,
/// BlendMode compositing, 256-entry RGBA LUTs, HSLA matrices, Luma, HighContrast,
/// Compose, and Lerp with full CPU/GPU parity.
class ColorEffect {
public:
    bool has_matrix{false};
    ColorMatrix matrix{};
    ColorTransfer transfer{ColorTransfer::None};
    bool has_blend{false};
    Color blend_color{Color::TRANSPARENT};
    BlendMode blend_mode{BlendMode::SourceOver};
    std::shared_ptr<const ColorEffectExtNode> ext_node{nullptr};

    ColorEffect() noexcept = default;

    [[nodiscard]] static ColorEffect from_matrix(const ColorMatrix& mat) noexcept {
        ColorEffect e;
        e.has_matrix = true;
        e.matrix = mat;
        return e;
    }

    [[nodiscard]] static ColorEffect mode(Color color, BlendMode mode) noexcept {
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

    [[nodiscard]] static ColorEffect srgb_to_linear() noexcept {
        ColorEffect e;
        e.transfer = ColorTransfer::SrgbToLinear;
        return e;
    }

    [[nodiscard]] static ColorEffect linear_to_srgb() noexcept {
        ColorEffect e;
        e.transfer = ColorTransfer::LinearToSrgb;
        return e;
    }

    /// 256-entry lookup table applied to R, G, and B channels (alpha unchanged).
    [[nodiscard]] static ColorEffect table(const std::array<uint8_t, 256>& rgb_lut);

    /// 256-entry per-channel lookup tables for A, R, G, B (nullptr means identity).
    [[nodiscard]] static ColorEffect table_argb(
        const uint8_t* a_lut,
        const uint8_t* r_lut,
        const uint8_t* g_lut,
        const uint8_t* b_lut
    );

    /// 4x5 color matrix operating in HSLA color space.
    [[nodiscard]] static ColorEffect hsla_matrix(const ColorMatrix& mat);

    /// Converts RGB luminance into the alpha channel (with RGB zeroed).
    [[nodiscard]] static ColorEffect luma();

    /// Accessibility high-contrast / grayscale / lightness-inversion filter.
    [[nodiscard]] static ColorEffect high_contrast(const HighContrastConfig& cfg);

    /// Smoothly interpolates between `cf0` (t=0) and `cf1` (t=1).
    [[nodiscard]] static ColorEffect lerp(float t, const ColorEffect& cf0, const ColorEffect& cf1);

    /// Composes `outer(inner(c))`, folding into a single fast pipeline when possible
    /// and otherwise constructing an exact composable filter chain.
    [[nodiscard]] static std::optional<ColorEffect> compose(const ColorEffect& outer, const ColorEffect& inner);

    [[nodiscard]] bool is_extended() const noexcept;

    [[nodiscard]] bool is_active() const noexcept;

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
