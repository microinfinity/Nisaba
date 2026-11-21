#pragma once

#include <cstdint>
#include <memory>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/shaders/shader.hpp"

namespace nisaba::effects {

/// Blur mode for a geometric coverage MaskFilter.
enum class BlurStyle : uint8_t {
    Normal = 0, ///< Blur both inside and outside the shape edge
    Solid  = 1, ///< Crisp original interior with blurred outer halo
    Outer  = 2, ///< Blurred outer halo only (interior removed)
    Inner  = 3, ///< Blurred interior only (clipped to original shape)
};

/// Type of MaskFilter applied to a draw operation's coverage mask.
enum class MaskFilterKind : uint8_t {
    None = 0,
    Blur = 1,
    Shader = 2,
};

/// Value-typed Paint-level mask filter supporting Gaussian blur masks
/// (Normal, Solid, Outer, Inner) and Shader alpha masks with full CPU/GPU parity.
class MaskFilter {
public:
    MaskFilterKind kind{MaskFilterKind::None};
    BlurStyle style{BlurStyle::Normal};
    float sigma{0.0f};
    bool respect_ctm{true};
    std::shared_ptr<const Shader> mask_shader{nullptr};

    MaskFilter() noexcept = default;

    /// Creates a Gaussian blur mask filter with the specified BlurStyle and sigma.
    [[nodiscard]] static MaskFilter blur(
        BlurStyle style_val,
        float sigma_val,
        bool respect_ctm_val = true
    ) noexcept {
        MaskFilter mf;
        mf.kind = MaskFilterKind::Blur;
        mf.style = style_val;
        mf.sigma = sigma_val > 0.0f ? sigma_val : 0.0f;
        mf.respect_ctm = respect_ctm_val;
        return mf;
    }

    /// Creates a ShaderMaskFilter that modulates drawn geometry coverage by `s`'s alpha channel.
    [[nodiscard]] static MaskFilter shader(const Shader& s) {
        MaskFilter mf;
        mf.kind = MaskFilterKind::Shader;
        mf.mask_shader = std::make_shared<Shader>(s);
        return mf;
    }

    [[nodiscard]] bool is_blur() const noexcept {
        return kind == MaskFilterKind::Blur && sigma > 0.001f;
    }

    [[nodiscard]] bool is_shader() const noexcept {
        return kind == MaskFilterKind::Shader && mask_shader != nullptr;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return is_blur() || is_shader();
    }
};

/// Evaluates a BlurMaskFilter in-place on a device-space coverage ROI pixmap
/// and tints the resulting coverage with `tint_color`.
void apply_mask_blur_cpu(
    PixmapMut& layer_roi,
    BlurStyle style,
    float sigma,
    Color tint_color,
    const Transform& layer_transform = Transform(),
    bool respect_ctm = true
);

} // namespace nisaba::effects
