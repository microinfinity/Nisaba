#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/dual_kawase.hpp"
#include "nisaba/compositor/egl_context.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/shader.hpp"
#include "nisaba/compositor/texture.hpp"

#include <cstdint>
#include <memory>

namespace nisaba::compositor {

/**
 * @brief Supported physical and artistic blur material types.
 */
enum class BlurMaterialType : uint8_t {
    Kawase = 0,            ///< Classic multi-pass Dual Kawase pyramid blur
    Acrylic = 1,           ///< Fluent Acrylic (luminosity exclusion, tint, blue-noise dither)
    OpticalGlass = 2,      ///< Physical glass refraction with RGB chromatic dispersion & specular rim
    WaterRipple = 3,       ///< Interactive 2D shallow water wave equation ripple refraction
    FluidField = 4,        ///< 2D Eulerian advection-diffusion fluid field distortion
    AtmosphericAurora = 5  ///< Animated atmospheric aurora & heat shimmer finish
};

/**
 * @brief Runtime parameters and interaction state passed to a blur material provider.
 */
struct BlurContext {
    DualKawaseConfig kawase{};
    float strength{1.0f};
    float timeSeconds{0.0f};
    PointF cursorUv{0.5, 0.5};
    PointF cursorVelocityUv{0.0, 0.0};
    float refraction{0.022f};
    float dispersion{0.0045f};
    float specularIntensity{0.35f};
    float cornerRadius{12.0f};
    float roundingPower{2.0f};
    bool xray{false}; ///< When true, samples from pre-blurred wallpaper backdrop instead of live scene
    const Texture* maskTexture{nullptr};
};

/**
 * @brief Abstract interface for pluggable compositor blur and refraction materials.
 */
class IBlurProvider {
public:
    virtual ~IBlurProvider() = default;

    [[nodiscard]] virtual BlurMaterialType materialType() const noexcept = 0;
    [[nodiscard]] virtual bool isAnimated() const noexcept = 0;
    [[nodiscard]] virtual bool requiresLiveBlur() const noexcept = 0;

    /**
     * @brief Computes the pixel radius by which damage regions must be expanded
     * to prevent halo artifacts around moving blurred surfaces.
     */
    [[nodiscard]] virtual int damageExpansionRadius(const BlurContext& ctx) const noexcept;

    /**
     * @brief Expands every rectangle in the given damage Region by the effective blur kernel radius.
     */
    virtual void expandDamage(
        Region& damage,
        const BlurContext& ctx,
        float multiplier = 1.0f) const noexcept;

    /**
     * @brief Executes the blur & material finish pipeline from source into target at destRect.
     */
    virtual bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) = 0;
};

/**
 * @brief Standard Dual Kawase blur provider.
 */
class KawaseBlurProvider final : public IBlurProvider {
public:
    KawaseBlurProvider() = default;
    ~KawaseBlurProvider() override = default;

    [[nodiscard]] BlurMaterialType materialType() const noexcept override { return BlurMaterialType::Kawase; }
    [[nodiscard]] bool isAnimated() const noexcept override { return false; }
    [[nodiscard]] bool requiresLiveBlur() const noexcept override { return false; }

    bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) override;
};

/**
 * @brief Fluent Acrylic material provider (luminosity blend + saturation boost + dithering).
 */
class AcrylicBlurProvider final : public IBlurProvider {
public:
    AcrylicBlurProvider();
    ~AcrylicBlurProvider() override = default;

    [[nodiscard]] BlurMaterialType materialType() const noexcept override { return BlurMaterialType::Acrylic; }
    [[nodiscard]] bool isAnimated() const noexcept override { return false; }
    [[nodiscard]] bool requiresLiveBlur() const noexcept override { return false; }

    bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) override;

private:
    std::unique_ptr<ShaderProgram> m_program;
};

/**
 * @brief Physical Optical Glass provider with edge lens normal refraction and chromatic dispersion.
 */
class OpticalGlassProvider final : public IBlurProvider {
public:
    OpticalGlassProvider();
    ~OpticalGlassProvider() override = default;

    [[nodiscard]] BlurMaterialType materialType() const noexcept override { return BlurMaterialType::OpticalGlass; }
    [[nodiscard]] bool isAnimated() const noexcept override { return false; }
    [[nodiscard]] bool requiresLiveBlur() const noexcept override { return true; }

    bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) override;

private:
    std::unique_ptr<ShaderProgram> m_program;
};

/**
 * @brief Interactive Water Ripple blur provider solving the 2D wave equation around cursor/window impulses.
 */
class WaterRippleProvider final : public IBlurProvider {
public:
    WaterRippleProvider();
    ~WaterRippleProvider() override = default;

    [[nodiscard]] BlurMaterialType materialType() const noexcept override { return BlurMaterialType::WaterRipple; }
    [[nodiscard]] bool isAnimated() const noexcept override { return true; }
    [[nodiscard]] bool requiresLiveBlur() const noexcept override { return true; }

    bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) override;

private:
    std::unique_ptr<ShaderProgram> m_program;
};

/**
 * @brief 2D Fluid Field distortion blur provider reacting to window and pointer velocity.
 */
class FluidFieldProvider final : public IBlurProvider {
public:
    FluidFieldProvider();
    ~FluidFieldProvider() override = default;

    [[nodiscard]] BlurMaterialType materialType() const noexcept override { return BlurMaterialType::FluidField; }
    [[nodiscard]] bool isAnimated() const noexcept override { return true; }
    [[nodiscard]] bool requiresLiveBlur() const noexcept override { return true; }

    bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) override;

private:
    std::unique_ptr<ShaderProgram> m_program;
};

/**
 * @brief Animated Atmospheric Aurora & Heat Shimmer blur finish provider.
 */
class AtmosphericBlurProvider final : public IBlurProvider {
public:
    AtmosphericBlurProvider();
    ~AtmosphericBlurProvider() override = default;

    [[nodiscard]] BlurMaterialType materialType() const noexcept override { return BlurMaterialType::AtmosphericAurora; }
    [[nodiscard]] bool isAnimated() const noexcept override { return true; }
    [[nodiscard]] bool requiresLiveBlur() const noexcept override { return false; }

    bool renderMaterial(
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const BlurContext& ctx) override;

private:
    std::unique_ptr<ShaderProgram> m_program;
};

/**
 * @brief Factory for instantiating blur material providers.
 */
class BlurProviderFactory {
public:
    [[nodiscard]] static std::shared_ptr<IBlurProvider> create(BlurMaterialType type);
};

} // namespace nisaba::compositor

