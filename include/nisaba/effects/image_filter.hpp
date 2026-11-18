#pragma once

#include <algorithm>
#include <cstdint>
#include "nisaba/color/color.hpp"

namespace nisaba::effects {

/// Type of raster ImageFilter applied to a draw operation or layer.
enum class ImageFilterKind : uint8_t {
    None = 0,
    Blur = 1,
    DropShadow = 2,
};

/// Value-typed Paint-level image filter supporting anisotropic Gaussian blur
/// and blurred drop shadows with full CPU/GPU parity.
class ImageFilter {
public:
    ImageFilterKind kind{ImageFilterKind::None};
    float sigma_x{0.0f};
    float sigma_y{0.0f};
    float dx{0.0f};
    float dy{0.0f};
    Color color{Color::TRANSPARENT};
    bool shadow_only{false};

    constexpr ImageFilter() noexcept = default;

    /// Creates a separable 2D Gaussian blur filter.
    [[nodiscard]] static constexpr ImageFilter blur(float sx, float sy) noexcept {
        ImageFilter f;
        f.kind = ImageFilterKind::Blur;
        f.sigma_x = sx > 0.0f ? sx : 0.0f;
        f.sigma_y = sy > 0.0f ? sy : 0.0f;
        return f;
    }

    /// Creates an isotropic Gaussian blur filter.
    [[nodiscard]] static constexpr ImageFilter blur(float sigma) noexcept {
        return blur(sigma, sigma);
    }

    /// Creates a drop-shadow filter with independent horizontal/vertical sigmas.
    [[nodiscard]] static constexpr ImageFilter drop_shadow(
        float dx_val,
        float dy_val,
        float sx,
        float sy,
        Color shadow_color,
        bool only_shadow = false
    ) noexcept {
        ImageFilter f;
        f.kind = ImageFilterKind::DropShadow;
        f.dx = dx_val;
        f.dy = dy_val;
        f.sigma_x = sx > 0.0f ? sx : 0.0f;
        f.sigma_y = sy > 0.0f ? sy : 0.0f;
        f.color = shadow_color;
        f.shadow_only = only_shadow;
        return f;
    }

    /// Creates a drop-shadow filter with uniform blur sigma.
    [[nodiscard]] static constexpr ImageFilter drop_shadow(
        float dx_val,
        float dy_val,
        float sigma,
        Color shadow_color,
        bool only_shadow = false
    ) noexcept {
        return drop_shadow(dx_val, dy_val, sigma, sigma, shadow_color, only_shadow);
    }

    /// Returns true if this filter has a visible raster effect.
    [[nodiscard]] constexpr bool is_active() const noexcept {
        if (kind == ImageFilterKind::Blur) {
            return sigma_x > 0.001f || sigma_y > 0.001f;
        }
        if (kind == ImageFilterKind::DropShadow) {
            return color.alpha() > 0.0f || shadow_only;
        }
        return false;
    }

    /// Maximum pixel padding required around draw bounds for filter kernels/offsets.
    [[nodiscard]] constexpr float max_sigma() const noexcept {
        return std::max(sigma_x, sigma_y);
    }
};

} // namespace nisaba::effects
