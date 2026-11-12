#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/color_management.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/render_pass.hpp"
#include "nisaba/compositor/surface_uv_solver.hpp"
#include "nisaba/compositor/texture.hpp"
#include "nisaba/compositor/window_decoration.hpp"
#include "nisaba/compositor/window_transformer.hpp"

#include <memory>
#include <string>

namespace nisaba::compositor {

class SceneTree;

enum class SceneNodeType {
    Tree,
    Rect,
    Buffer
};

/**
 * @brief Base class for 2D desktop scene graph elements.
 */
class SceneNode : public std::enable_shared_from_this<SceneNode> {
public:
    explicit SceneNode(SceneNodeType type) noexcept
        : m_type(type) {}

    virtual ~SceneNode() = default;

    SceneNode(const SceneNode&) = delete;
    SceneNode& operator=(const SceneNode&) = delete;

    [[nodiscard]] SceneNodeType type() const noexcept { return m_type; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool enabled) noexcept { m_enabled = enabled; }

    [[nodiscard]] int x() const noexcept { return m_x; }
    [[nodiscard]] int y() const noexcept { return m_y; }
    void setPosition(int x, int y) noexcept {
        m_x = x;
        m_y = y;
    }

    [[nodiscard]] SceneTree* parent() const noexcept { return m_parent; }
    void setParent(SceneTree* parent) noexcept { m_parent = parent; }

    /**
     * @brief Computes absolute root coordinate of this node by walking up parent tree.
     */
    [[nodiscard]] PointI absolutePosition() const noexcept;

    /**
     * @brief Recursively renders the node into an active IRenderPass.
     */
    virtual void render(
        IRenderPass& pass,
        const PointI& parentOffset,
        const Region* damage) const = 0;

    /**
     * @brief Enqueues deferred pass elements into a RenderPass for front-to-back occlusion culling.
     */
    virtual void enqueueToPass(
        RenderPass& pass,
        const PointI& parentOffset,
        const ImageDescription& outputDesc = ImageDescription::srgbDefault()) const = 0;

    /**
     * @brief Hit-tests point in root coordinates, returning topmost matching node.
     */
    virtual SceneNode* nodeAt(const PointI& rootPoint) = 0;

protected:
    SceneNodeType m_type;
    int m_x{0};
    int m_y{0};
    bool m_enabled{true};
    SceneTree* m_parent{nullptr};
};

/**
 * @brief Solid color rectangle scene node for borders, backgrounds, and accents.
 */
class SceneRect : public SceneNode {
public:
    static std::shared_ptr<SceneRect> create(int width, int height, const Color& color);

    SceneRect(int width, int height, const Color& color);
    ~SceneRect() override = default;

    void setSize(int width, int height) noexcept {
        m_width = width;
        m_height = height;
    }

    void setColor(const Color& color) noexcept { m_color = color; }
    [[nodiscard]] const Color& color() const noexcept { return m_color; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

    void setRounding(float cornerRadius, float roundingPower = 2.0f) noexcept {
        m_cornerRadius = cornerRadius;
        m_roundingPower = roundingPower;
    }
    [[nodiscard]] float cornerRadius() const noexcept { return m_cornerRadius; }
    [[nodiscard]] float roundingPower() const noexcept { return m_roundingPower; }

    void render(
        IRenderPass& pass,
        const PointI& parentOffset,
        const Region* damage) const override;

    void enqueueToPass(
        RenderPass& pass,
        const PointI& parentOffset,
        const ImageDescription& outputDesc = ImageDescription::srgbDefault()) const override;

    SceneNode* nodeAt(const PointI& rootPoint) override;

private:
    int m_width{0};
    int m_height{0};
    Color m_color{Color::black()};
    float m_cornerRadius{0.0f};
    float m_roundingPower{2.0f};
};

/**
 * @brief Texture buffer scene node for Wayland surfaces, windows, icons, and wallpapers.
 */
class SceneBuffer : public SceneNode {
public:
    static std::shared_ptr<SceneBuffer> create(
        std::shared_ptr<Texture> texture,
        int width = 0,
        int height = 0);

    SceneBuffer(std::shared_ptr<Texture> texture, int width, int height);
    ~SceneBuffer() override = default;

    void setTexture(std::shared_ptr<Texture> texture, int width = 0, int height = 0);
    [[nodiscard]] const std::shared_ptr<Texture>& texture() const noexcept { return m_texture; }

    void setSourceBox(const RectF& src) noexcept { m_srcBox = src; }
    [[nodiscard]] const RectF& sourceBox() const noexcept { return m_srcBox; }

    void setAlpha(float alpha) noexcept { m_alpha = alpha; }
    [[nodiscard]] float alpha() const noexcept { return m_alpha; }

    void setTransform(wl_output_transform t) noexcept { m_transform = t; }
    [[nodiscard]] wl_output_transform transform() const noexcept { return m_transform; }

    void setRounding(float cornerRadius, float roundingPower = 2.0f) noexcept {
        m_cornerRadius = cornerRadius;
        m_roundingPower = roundingPower;
    }
    [[nodiscard]] float cornerRadius() const noexcept { return m_cornerRadius; }
    [[nodiscard]] float roundingPower() const noexcept { return m_roundingPower; }

    void setImageDescription(const ImageDescription& desc) noexcept { m_imageDesc = desc; }
    [[nodiscard]] const ImageDescription& imageDescription() const noexcept { return m_imageDesc; }

    void setTearingHint(bool tearing) noexcept { m_tearingHint = tearing; }
    [[nodiscard]] bool tearingHint() const noexcept { return m_tearingHint; }

    void setOpaqueRegion(const Region& region) { m_opaqueRegion = region; }
    [[nodiscard]] const Region& opaqueRegion() const noexcept { return m_opaqueRegion; }

    [[nodiscard]] TransformerChain& transformers() noexcept { return m_transformers; }
    [[nodiscard]] const TransformerChain& transformers() const noexcept { return m_transformers; }

    [[nodiscard]] WindowDecorationStack& decorations() noexcept { return m_decorations; }
    [[nodiscard]] const WindowDecorationStack& decorations() const noexcept { return m_decorations; }

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

    void setUserData(void* data) noexcept { m_userData = data; }
    [[nodiscard]] void* userData() const noexcept { return m_userData; }

    void render(
        IRenderPass& pass,
        const PointI& parentOffset,
        const Region* damage) const override;

    void enqueueToPass(
        RenderPass& pass,
        const PointI& parentOffset,
        const ImageDescription& outputDesc = ImageDescription::srgbDefault()) const override;

    SceneNode* nodeAt(const PointI& rootPoint) override;

private:
    std::shared_ptr<Texture> m_texture;
    int m_width{0};
    int m_height{0};
    RectF m_srcBox{0.0, 0.0, 1.0, 1.0};
    float m_alpha{1.0f};
    float m_cornerRadius{0.0f};
    float m_roundingPower{2.0f};
    wl_output_transform m_transform{WL_OUTPUT_TRANSFORM_NORMAL};
    ImageDescription m_imageDesc{};
    bool m_tearingHint{false};
    TransformerChain m_transformers{};
    WindowDecorationStack m_decorations{};
    Region m_opaqueRegion{};
    void* m_userData{nullptr};
};

} // namespace nisaba::compositor
