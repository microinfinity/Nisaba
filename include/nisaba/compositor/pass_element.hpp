#pragma once

#include "nisaba/compositor/blur_provider.hpp"
#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/color_management.hpp"
#include "nisaba/compositor/deformable_mesh.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/texture.hpp"
#include "nisaba/compositor/window_transformer.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <wayland-server-protocol.h>

namespace nisaba::compositor {

class RenderPass;

/**
 * @brief Rich surface rendering configuration for SurfacePassElement and RenderPass::renderSurface.
 */
struct SurfaceRenderParams {
    const Texture* texture{nullptr};
    RectI destBox{0, 0, 0, 0};
    RectF srcBox{0.0, 0.0, 1.0, 1.0};
    float alpha{1.0f};
    wl_output_transform transform{WL_OUTPUT_TRANSFORM_NORMAL};
    bool preferNearestFilter{false};

    // Squircle corner rounding
    float cornerRadius{0.0f};
    float roundingPower{2.0f};

    // Single-pass blurred backdrop composition
    const Texture* blurredBackdrop{nullptr};
    RectF blurredBackdropUv{0.0, 0.0, 1.0, 1.0};
    float blurAlpha{1.0f};
    bool requiresLiveBlur{false};

    // Optional alpha matte mask
    const Texture* alphaMatte{nullptr};

    // Fragment discard controls
    bool discardOpaque{false};
    float discardAlphaThreshold{0.0f};

    // Sub-frame directional motion blur
    MotionBlurState motionBlur{};

    // Color Management & 3D LUT
    ColorTransformParams colorTransform{};
    const Lut3DTexture* lut3D{nullptr};

    // Deformable mesh override (e.g. wobbly window)
    const std::vector<MeshVertex>* customMesh{nullptr};
};

/**
 * @brief Enumeration of deferred pass element kinds.
 */
enum class PassElementKind : uint8_t {
    Clear = 0,
    Rect = 1,
    Surface = 2,
    Border = 3,
    Shadow = 4,
    InnerGlow = 5,
    BlurMaterial = 6,
    PreBlur = 7,
    BackdropScope = 8,
    Custom = 9
};

/**
 * @brief Abstract element in a deferred compositor RenderPass.
 *
 * Provides bounding geometry, opaque occlusion regions, and blur requirements
 * so the pass planner can perform front-to-back occlusion culling before issuing GPU draws.
 */
class IPassElement {
public:
    virtual ~IPassElement() = default;

    [[nodiscard]] virtual PassElementKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Screen-space bounding box of this element, or std::nullopt if fullscreen/unbounded.
     */
    [[nodiscard]] virtual std::optional<RectI> boundingBox() const noexcept = 0;

    /**
     * @brief 100% opaque screen-space region guaranteed to occlude any elements behind it.
     */
    [[nodiscard]] virtual Region opaqueRegion() const noexcept { return Region{}; }

    [[nodiscard]] virtual bool needsLiveBlur() const noexcept { return false; }
    [[nodiscard]] virtual bool needsPrecomputedBlur() const noexcept { return false; }
    [[nodiscard]] virtual bool undiscardable() const noexcept { return false; }

    /**
     * @brief Executes the GPU draw calls for this element restricted to the visible damage region.
     */
    virtual void draw(RenderPass& pass, const Region& visibleDamage) = 0;
};

/**
 * @brief Pass element that clears the target buffer or damage region.
 */
class ClearPassElement final : public IPassElement {
public:
    explicit ClearPassElement(const Color& color) noexcept : m_color(color) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::Clear; }
    [[nodiscard]] std::string_view name() const noexcept override { return "ClearPassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override { return std::nullopt; }
    [[nodiscard]] bool undiscardable() const noexcept override { return false; }

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    Color m_color;
};

/**
 * @brief Pass element for solid or rounded rectangles.
 */
class RectPassElement final : public IPassElement {
public:
    struct Data {
        RectI box{0, 0, 0, 0};
        Color color{Color::black()};
        float cornerRadius{0.0f};
        float roundingPower{2.0f};
    };

    explicit RectPassElement(const Data& data) noexcept : m_data(data) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::Rect; }
    [[nodiscard]] std::string_view name() const noexcept override { return "RectPassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override { return m_data.box; }
    [[nodiscard]] Region opaqueRegion() const noexcept override;

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    Data m_data;
};

/**
 * @brief Pass element for Wayland client surfaces with squircle rounding, CM, motion blur, or mesh wobble.
 */
class SurfacePassElement final : public IPassElement {
public:
    explicit SurfacePassElement(SurfaceRenderParams params) noexcept
        : m_params(std::move(params)) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::Surface; }
    [[nodiscard]] std::string_view name() const noexcept override { return "SurfacePassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override;
    [[nodiscard]] Region opaqueRegion() const noexcept override;
    [[nodiscard]] bool needsLiveBlur() const noexcept override { return m_params.requiresLiveBlur; }
    [[nodiscard]] bool needsPrecomputedBlur() const noexcept override { return m_params.blurredBackdrop != nullptr; }

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    SurfaceRenderParams m_params;
};

/**
 * @brief Pass element for analytical squircle window borders with Oklab rotational gradients.
 */
class BorderPassElement final : public IPassElement {
public:
    struct Data {
        RectI box{0, 0, 0, 0};
        GradientSpec grad1{};
        GradientSpec grad2{};
        bool hasGrad2{false};
        float lerpFactor{0.0f};
        float innerRadius{10.0f};
        float outerRadius{12.0f};
        float roundingPower{2.0f};
        float thickness{2.0f};
        float alpha{1.0f};
    };

    explicit BorderPassElement(Data data) noexcept : m_data(std::move(data)) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::Border; }
    [[nodiscard]] std::string_view name() const noexcept override { return "BorderPassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override { return m_data.box; }

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    Data m_data;
};

/**
 * @brief Pass element for analytical SDF drop shadows.
 */
class ShadowPassElement final : public IPassElement {
public:
    struct Data {
        RectI fullBox{0, 0, 0, 0};
        float cornerRadius{10.0f};
        float roundingPower{2.0f};
        float rangeSigma{18.0f};
        float falloffPower{2.2f};
        GradientSpec grad1{Color{0.0f, 0.0f, 0.0f, 0.65f}};
        GradientSpec grad2{};
        bool hasGrad2{false};
        float lerpFactor{0.0f};
        float alpha{1.0f};
    };

    explicit ShadowPassElement(Data data) noexcept : m_data(std::move(data)) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::Shadow; }
    [[nodiscard]] std::string_view name() const noexcept override { return "ShadowPassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override { return m_data.fullBox; }

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    Data m_data;
};

/**
 * @brief Pass element for analytical inner perimeter window glow.
 */
class InnerGlowPassElement final : public IPassElement {
public:
    struct Data {
        RectI box{0, 0, 0, 0};
        float cornerRadius{10.0f};
        float roundingPower{2.0f};
        float range{8.0f};
        float falloffPower{2.0f};
        GradientSpec grad{Color{1.0f, 1.0f, 1.0f, 0.25f}};
        float alpha{1.0f};
    };

    explicit InnerGlowPassElement(Data data) noexcept : m_data(std::move(data)) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::InnerGlow; }
    [[nodiscard]] std::string_view name() const noexcept override { return "InnerGlowPassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override { return m_data.box; }

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    Data m_data;
};

/**
 * @brief Pass element for physical blur materials (Kawase, Acrylic, OpticalGlass, WaterRipple, FluidField, Aurora).
 */
class BlurMaterialPassElement final : public IPassElement {
public:
    struct Data {
        std::shared_ptr<IBlurProvider> provider;
        DualKawaseBlur* kawase{nullptr};
        const Texture* source{nullptr};
        RectI destBox{0, 0, 0, 0};
        RectI srcBox{0, 0, 0, 0};
        BlurContext context{};
    };

    explicit BlurMaterialPassElement(Data data) noexcept : m_data(std::move(data)) {}

    [[nodiscard]] PassElementKind kind() const noexcept override { return PassElementKind::BlurMaterial; }
    [[nodiscard]] std::string_view name() const noexcept override { return "BlurMaterialPassElement"; }
    [[nodiscard]] std::optional<RectI> boundingBox() const noexcept override { return m_data.destBox; }
    [[nodiscard]] bool needsLiveBlur() const noexcept override {
        return !m_data.context.xray && (!m_data.provider || m_data.provider->requiresLiveBlur());
    }
    [[nodiscard]] bool needsPrecomputedBlur() const noexcept override {
        return m_data.context.xray;
    }

    void draw(RenderPass& pass, const Region& visibleDamage) override;

private:
    Data m_data;
};

} // namespace nisaba::compositor

