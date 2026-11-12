#pragma once

#include "nisaba/compositor/blur_provider.hpp"
#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/dual_kawase.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/matrix3.hpp"
#include "nisaba/compositor/pass_element.hpp"
#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/shader.hpp"
#include "nisaba/compositor/texture.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace nisaba::compositor {

/**
 * @brief Telemetry and culling statistics returned by deferred RenderPass execution.
 */
struct PassExecutionStats {
    size_t totalElements{0};
    size_t executedElements{0};
    size_t culledElements{0};
    Region remainingUnoccludedDamage{};
    Region liveBlurRegion{};
};

/**
 * @brief Sovereign RenderPass interface accumulating 2D compositing drawing commands
 * and submitting them efficiently to GPU, completely decoupled from any graphics API.
 */
class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    /**
     * @brief Clears the entire target buffer with specified color.
     */
    virtual void clear(const Color& color) const noexcept = 0;

    /**
     * @brief Renders a solid color rectangle with optional region clipping.
     */
    virtual void renderRect(
        const RectI& box,
        const Color& color,
        const Region* clip = nullptr) const = 0;

    /**
     * @brief Renders a textured quad with source sub-box, alpha, and surface transform.
     */
    virtual void renderTexture(
        const ITexture& texture,
        const RectI& destBox,
        const RectF& srcBox = RectF{0.0, 0.0, 1.0, 1.0},
        float alpha = 1.0f,
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL,
        const Region* clip = nullptr) const = 0;

    /**
     * @brief Submits the render pass commands to the graphics device.
     */
    virtual bool submit() = 0;

    [[nodiscard]] virtual RenderBackendType backendType() const noexcept = 0;

    /**
     * @brief Native API handle escape hatch for custom shader nodes and extensions.
     */
    [[nodiscard]] virtual void* nativeHandle() noexcept = 0;
};

/**
 * @brief Sovereign OpenGL ES implementation of IRenderPass supporting both immediate
 * drawing and deferred front-to-back occlusion-culled pass graph execution.
 */
class RenderPass : public IRenderPass {
public:
    static std::unique_ptr<RenderPass> begin(
        std::shared_ptr<RenderTarget> target,
        std::shared_ptr<ShaderManager> shaders,
        wl_output_transform outputTransform = WL_OUTPUT_TRANSFORM_FLIPPED_180);

    RenderPass(
        std::shared_ptr<RenderTarget> target,
        std::shared_ptr<ShaderManager> shaders,
        wl_output_transform outputTransform);

    ~RenderPass() override;

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    // ─────────────────────────────────────────────────────────────────────────
    // Deferred Pass Graph & Front-to-Back Occlusion Culling
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Enqueues a pass element in back-to-front draw order for deferred optimization.
     */
    void enqueue(std::unique_ptr<IPassElement> element);

    /**
     * @brief Clears all enqueued pass elements without executing them.
     */
    void clearQueue() noexcept;

    [[nodiscard]] size_t queuedElementCount() const noexcept { return m_queue.size(); }

    /**
     * @brief Performs front-to-back occlusion culling against initialDamage, plans backdrop
     * scopes, and executes only non-occluded elements back-to-front.
     */
    PassExecutionStats optimizeAndExecute(const Region& initialDamage);

    // ─────────────────────────────────────────────────────────────────────────
    // Immediate & Element Execution Primitives
    // ─────────────────────────────────────────────────────────────────────────

    void clear(const Color& color) const noexcept override;

    void renderRect(
        const RectI& box,
        const Color& color,
        const Region* clip = nullptr) const override;

    void renderRoundedRect(
        const RectI& box,
        const Color& color,
        float cornerRadius,
        float roundingPower = 2.0f,
        const Region* clip = nullptr) const;

    void renderTexture(
        const ITexture& texture,
        const RectI& destBox,
        const RectF& srcBox = RectF{0.0, 0.0, 1.0, 1.0},
        float alpha = 1.0f,
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL,
        const Region* clip = nullptr) const override;

    void renderTexture(
        const Texture& texture,
        const RectI& destBox,
        const RectF& srcBox = RectF{0.0, 0.0, 1.0, 1.0},
        float alpha = 1.0f,
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL,
        const Region* clip = nullptr) const;

    /**
     * @brief Renders a client surface using the full SurfaceVariantProgram pipeline
     * (squircle corners, backdrop blur, alpha matte, CM, 3D LUT, motion blur, or mesh wobble).
     */
    void renderSurface(
        const SurfaceRenderParams& params,
        const Region* clip = nullptr) const;

    /**
     * @brief Renders an analytical squircle border with Oklab rotational gradients.
     */
    void renderBorder(
        const RectI& box,
        const GradientSpec& grad1,
        const GradientSpec* grad2,
        float lerpFactor,
        float innerRadius,
        float outerRadius,
        float roundingPower = 2.0f,
        float thickness = 2.0f,
        float alpha = 1.0f,
        const Region* clip = nullptr) const;

    /**
     * @brief Renders an analytical SDF rounded drop shadow with Oklab gradient tinting.
     */
    void renderShadow(
        const RectI& fullBox,
        float cornerRadius,
        float roundingPower,
        float rangeSigma,
        float falloffPower,
        const GradientSpec& grad1,
        const GradientSpec* grad2 = nullptr,
        float lerpFactor = 0.0f,
        float alpha = 1.0f,
        const Region* clip = nullptr) const;

    /**
     * @brief Renders an analytical SDF inner perimeter glow.
     */
    void renderInnerGlow(
        const RectI& box,
        float cornerRadius,
        float roundingPower,
        float range,
        float falloffPower,
        const GradientSpec& grad,
        float alpha = 1.0f,
        const Region* clip = nullptr) const;

    /**
     * @brief Renders a deformable 2D triangle mesh (used by SpringWobbleTransformer).
     */
    void renderMesh(
        const Texture& texture,
        const std::vector<MeshVertex>& vertices,
        float alpha = 1.0f,
        float cornerRadius = 0.0f,
        float roundingPower = 2.0f,
        const SizeI& refSize = {}) const;

    void renderBlur(
        DualKawaseBlur& blurEngine,
        const Texture& source,
        const RectI& destBox,
        const RectI& srcBox = RectI{0, 0, 0, 0},
        const DualKawaseConfig& config = {},
        const Texture* maskTexture = nullptr);

    void renderBlurMaterial(
        IBlurProvider& provider,
        DualKawaseBlur& kawase,
        const Texture& source,
        const RectI& destBox,
        const RectI& srcBox = RectI{0, 0, 0, 0},
        const BlurContext& ctx = {});

    bool submit() override;

    [[nodiscard]] RenderBackendType backendType() const noexcept override {
        return RenderBackendType::OpenGL;
    }

    [[nodiscard]] void* nativeHandle() noexcept override {
        return m_target.get();
    }

    [[nodiscard]] const std::shared_ptr<RenderTarget>& target() const noexcept { return m_target; }
    [[nodiscard]] const std::shared_ptr<ShaderManager>& shaders() const noexcept { return m_shaders; }
    [[nodiscard]] const Matrix3& projection() const noexcept { return m_projection; }

private:
    struct QueuedEntry {
        std::unique_ptr<IPassElement> element;
        Region visibleDamage{};
        bool discarded{false};
    };

    void renderQuads(
        const RectI& box,
        const Region* clip,
        GLint posAttrib) const;

    std::shared_ptr<RenderTarget> m_target;
    std::shared_ptr<ShaderManager> m_shaders;
    Matrix3 m_projection;
    std::vector<QueuedEntry> m_queue;
    mutable std::vector<float> m_quadScratch;
    bool m_submitted{false};
};

} // namespace nisaba::compositor
