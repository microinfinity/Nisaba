#pragma once

#include "nisaba/compositor/deformable_mesh.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/texture.hpp"

#include <memory>
#include <vector>

namespace nisaba::compositor {

class RenderPass;

/**
 * @brief Sub-frame motion blur parameters populated by VelocityMotionBlurTransformer.
 */
struct MotionBlurState {
    bool active{false};
    float velocityUvX{0.0f};
    float velocityUvY{0.0f};
    int samples{8};
};

/**
 * @brief Window rendering state passed through a TransformerChain.
 */
struct WindowTransformState {
    const Texture* texture{nullptr};
    RectI logicalBox{0, 0, 0, 0};
    float alpha{1.0f};
    float cornerRadius{0.0f};
    float roundingPower{2.0f};
    MotionBlurState motionBlur{};
    const std::vector<MeshVertex>* customMesh{nullptr};
};

/**
 * @brief Abstract window transformer attached to a window or surface.
 *
 * Allows modular pre/post-processing of windows (physics wobble mesh deformation,
 * directional velocity motion blur, warps) with full damage and direct-scanout awareness.
 */
class IWindowTransformer {
public:
    virtual ~IWindowTransformer() = default;

    [[nodiscard]] virtual int priority() const noexcept { return 0; }
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
    [[nodiscard]] virtual bool blocksDirectScanout() const noexcept { return isActive(); }

    /**
     * @brief Computes the expanded damage bounding box required by this transformer.
     */
    [[nodiscard]] virtual RectI transformedExtents(const RectI& currentBox) const noexcept {
        return currentBox;
    }

    /**
     * @brief Advances any internal physics or animation state by dtSeconds.
     */
    virtual void step(float dtSeconds) noexcept { (void)dtSeconds; }

    /**
     * @brief Mutates or augments the WindowTransformState prior to pass submission.
     */
    virtual void apply(WindowTransformState& state) = 0;
};

/**
 * @brief Spring-physics deformable mesh transformer (Wobbly Windows).
 */
class SpringWobbleTransformer final : public IWindowTransformer {
public:
    explicit SpringWobbleTransformer(const SpringMeshConfig& cfg = {});
    ~SpringWobbleTransformer() override = default;

    [[nodiscard]] int priority() const noexcept override { return 100; }
    [[nodiscard]] bool isActive() const noexcept override { return !m_mesh.isSettled(); }

    [[nodiscard]] RectI transformedExtents(const RectI& currentBox) const noexcept override;

    void grab(PointF anchorUv, const RectI& currentBox) noexcept { m_mesh.grab(anchorUv, currentBox); }
    void moveTo(const RectI& newBox) noexcept { m_mesh.setTargetBox(newBox); }
    void release() noexcept { m_mesh.release(); }

    void step(float dtSeconds) noexcept override { m_mesh.step(dtSeconds); }
    void apply(WindowTransformState& state) override;

    [[nodiscard]] DeformableMesh& mesh() noexcept { return m_mesh; }
    [[nodiscard]] const DeformableMesh& mesh() const noexcept { return m_mesh; }

private:
    DeformableMesh m_mesh;
};

/**
 * @brief Sub-frame velocity tracking and directional motion blur transformer.
 */
class VelocityMotionBlurTransformer final : public IWindowTransformer {
public:
    explicit VelocityMotionBlurTransformer(float shutterStrength = 0.65f, int samples = 8) noexcept;
    ~VelocityMotionBlurTransformer() override = default;

    [[nodiscard]] int priority() const noexcept override { return 50; }
    [[nodiscard]] bool isActive() const noexcept override { return m_active; }

    [[nodiscard]] RectI transformedExtents(const RectI& currentBox) const noexcept override;

    void updateWindowBox(const RectI& currentBox) noexcept;
    void step(float dtSeconds) noexcept override;
    void apply(WindowTransformState& state) override;

private:
    RectI m_prevBox{0, 0, 0, 0};
    RectI m_currBox{0, 0, 0, 0};
    float m_shutterStrength{0.65f};
    int m_samples{8};
    bool m_hasHistory{false};
    bool m_active{false};
};

/**
 * @brief Ordered chain of IWindowTransformer instances attached to a window.
 */
class TransformerChain {
public:
    TransformerChain() = default;
    ~TransformerChain() = default;

    void add(std::shared_ptr<IWindowTransformer> transformer);
    void clear() noexcept { m_transformers.clear(); }

    [[nodiscard]] bool empty() const noexcept { return m_transformers.empty(); }
    [[nodiscard]] bool hasActiveTransformer() const noexcept;
    [[nodiscard]] bool blocksDirectScanout() const noexcept;

    [[nodiscard]] RectI computeDamageExtents(const RectI& baseBox) const noexcept;

    void stepAll(float dtSeconds) noexcept;
    void applyAll(WindowTransformState& state) const;

private:
    std::vector<std::shared_ptr<IWindowTransformer>> m_transformers;
};

} // namespace nisaba::compositor

