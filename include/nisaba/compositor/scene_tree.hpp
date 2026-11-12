#pragma once

#include "nisaba/compositor/scene_node.hpp"

#include <vector>

namespace nisaba::compositor {

/**
 * @brief Container node in the compositor scene graph managing children hierarchy and z-ordering.
 */
class SceneTree : public SceneNode {
public:
    static std::shared_ptr<SceneTree> create(SceneTree* parent = nullptr);

    SceneTree();
    ~SceneTree() override = default;

    std::shared_ptr<SceneTree> createChildTree();

    std::shared_ptr<SceneRect> createChildRect(int width, int height, const Color& color);

    std::shared_ptr<SceneBuffer> createChildBuffer(
        std::shared_ptr<Texture> texture,
        int width = 0,
        int height = 0);

    void addChild(std::shared_ptr<SceneNode> child);
    void removeChild(SceneNode* child);
    void raiseToTop(SceneNode* child);
    void lowerToBottom(SceneNode* child);

    [[nodiscard]] const std::vector<std::shared_ptr<SceneNode>>& children() const noexcept {
        return m_children;
    }

    void render(
        IRenderPass& pass,
        const PointI& parentOffset,
        const Region* damage) const override;

    void enqueueToPass(
        RenderPass& pass,
        const PointI& parentOffset,
        const ImageDescription& outputDesc = ImageDescription::srgbDefault()) const override;

    /**
     * @brief Enqueues the scene tree into the deferred RenderPass and executes with
     * front-to-back occlusion culling.
     */
    PassExecutionStats renderCulled(
        RenderPass& pass,
        const Region& damage,
        const ImageDescription& outputDesc = ImageDescription::srgbDefault()) const;

    SceneNode* nodeAt(const PointI& rootPoint) override;

    /**
     * @brief Checks if a single fullscreen, opaque buffer node covers the entire target area
     * without any occluding or overlapping nodes above it, making it eligible for Direct Scanout.
     */
    [[nodiscard]] SceneBuffer* findDirectScanoutCandidate(int targetWidth, int targetHeight) const;

private:
    std::vector<std::shared_ptr<SceneNode>> m_children;
};

} // namespace nisaba::compositor
