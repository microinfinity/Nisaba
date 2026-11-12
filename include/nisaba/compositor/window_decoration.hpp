#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/pass_element.hpp"
#include "nisaba/compositor/render_pass.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nisaba::compositor {

/**
 * @brief Z-order layer for modular window decorations relative to the client surface.
 */
enum class DecorationLayer : uint8_t {
    Bottom = 0,  ///< Lowest layer (e.g. Drop Shadow)
    Under = 1,   ///< Under client surface, above Bottom
    Over = 2,    ///< Above client surface, below popups (e.g. Border, Inner Glow, GroupBar)
    Overlay = 3  ///< Topmost layer above popups
};

/**
 * @brief Outward pixel margins reserved or occupied by a window decoration.
 */
struct DecorationExtents {
    int top{0};
    int bottom{0};
    int left{0};
    int right{0};

    [[nodiscard]] constexpr RectI expandBox(const RectI& box) const noexcept {
        return RectI{
            box.x - left,
            box.y - top,
            box.width + left + right,
            box.height + top + bottom
        };
    }
};

/**
 * @brief Abstract modular window decoration interface.
 */
class IWindowDecoration {
public:
    virtual ~IWindowDecoration() = default;

    [[nodiscard]] virtual DecorationLayer layer() const noexcept = 0;
    [[nodiscard]] virtual DecorationExtents extents() const noexcept = 0;
    [[nodiscard]] virtual RectI damageBox(const RectI& clientBox) const noexcept {
        return extents().expandBox(clientBox);
    }

    /**
     * @brief Enqueues pass elements for this decoration into the active RenderPass.
     */
    virtual void enqueue(
        RenderPass& pass,
        const RectI& clientBox,
        float windowAlpha = 1.0f) const = 0;
};

/**
 * @brief Analytical SDF rounded drop shadow decoration.
 */
class DropShadowDecoration final : public IWindowDecoration {
public:
    struct Config {
        float cornerRadius{10.0f};
        float roundingPower{2.0f};
        float rangeSigma{18.0f};
        float falloffPower{2.2f};
        PointI offset{0, 6};
        GradientSpec activeGradient{Color{0.0f, 0.0f, 0.0f, 0.65f}};
        GradientSpec inactiveGradient{Color{0.0f, 0.0f, 0.0f, 0.40f}};
        float transitionLerp{0.0f}; ///< 0.0 = activeGradient, 1.0 = inactiveGradient
    };

    DropShadowDecoration() noexcept = default;
    explicit DropShadowDecoration(Config cfg) noexcept : m_cfg(std::move(cfg)) {}
    ~DropShadowDecoration() override = default;

    [[nodiscard]] DecorationLayer layer() const noexcept override { return DecorationLayer::Bottom; }
    [[nodiscard]] DecorationExtents extents() const noexcept override;
    [[nodiscard]] RectI damageBox(const RectI& clientBox) const noexcept override;

    void enqueue(RenderPass& pass, const RectI& clientBox, float windowAlpha = 1.0f) const override;

    [[nodiscard]] Config& config() noexcept { return m_cfg; }
    [[nodiscard]] const Config& config() const noexcept { return m_cfg; }

private:
    Config m_cfg;
};

/**
 * @brief Analytical squircle border decoration with Oklab rotational gradients and corner correction.
 */
class BorderDecoration final : public IWindowDecoration {
public:
    struct Config {
        float cornerRadius{10.0f};
        float roundingPower{2.0f};
        float borderSize{2.0f};
        GradientSpec activeGradient{
            {Color{0.20f, 0.80f, 1.0f, 0.95f}, Color{0.65f, 0.35f, 1.0f, 0.95f}},
            0.785398f
        };
        GradientSpec inactiveGradient{Color{0.35f, 0.38f, 0.45f, 0.55f}};
        float transitionLerp{0.0f};
    };

    BorderDecoration() noexcept = default;
    explicit BorderDecoration(Config cfg) noexcept : m_cfg(std::move(cfg)) {}
    ~BorderDecoration() override = default;

    [[nodiscard]] DecorationLayer layer() const noexcept override { return DecorationLayer::Over; }
    [[nodiscard]] DecorationExtents extents() const noexcept override;

    void enqueue(RenderPass& pass, const RectI& clientBox, float windowAlpha = 1.0f) const override;

    [[nodiscard]] Config& config() noexcept { return m_cfg; }
    [[nodiscard]] const Config& config() const noexcept { return m_cfg; }

private:
    Config m_cfg;
};

/**
 * @brief Analytical inner perimeter glow decoration.
 */
class InnerGlowDecoration final : public IWindowDecoration {
public:
    struct Config {
        float cornerRadius{10.0f};
        float roundingPower{2.0f};
        float range{8.0f};
        float falloffPower{2.0f};
        GradientSpec gradient{Color{0.85f, 0.92f, 1.0f, 0.28f}};
    };

    InnerGlowDecoration() noexcept = default;
    explicit InnerGlowDecoration(Config cfg) noexcept : m_cfg(std::move(cfg)) {}
    ~InnerGlowDecoration() override = default;

    [[nodiscard]] DecorationLayer layer() const noexcept override { return DecorationLayer::Over; }
    [[nodiscard]] DecorationExtents extents() const noexcept override { return {0, 0, 0, 0}; }

    void enqueue(RenderPass& pass, const RectI& clientBox, float windowAlpha = 1.0f) const override;

    [[nodiscard]] Config& config() noexcept { return m_cfg; }
    [[nodiscard]] const Config& config() const noexcept { return m_cfg; }

private:
    Config m_cfg;
};

/**
 * @brief Tabbed window group bar decoration (Hyprland-style GroupBar).
 */
class GroupBarDecoration final : public IWindowDecoration {
public:
    struct Config {
        int barHeight{24};
        int gapToWindow{3};
        int indicatorHeight{3};
        float cornerRadius{6.0f};
        Color activeTabColor{0.22f, 0.68f, 0.96f, 0.95f};
        Color inactiveTabColor{0.18f, 0.20f, 0.25f, 0.85f};
        Color backgroundColor{0.11f, 0.12f, 0.15f, 0.90f};
        std::vector<std::string> tabTitles;
        size_t activeTabIndex{0};
    };

    GroupBarDecoration() noexcept = default;
    explicit GroupBarDecoration(Config cfg) noexcept : m_cfg(std::move(cfg)) {}
    ~GroupBarDecoration() override = default;

    [[nodiscard]] DecorationLayer layer() const noexcept override { return DecorationLayer::Over; }
    [[nodiscard]] DecorationExtents extents() const noexcept override;

    void enqueue(RenderPass& pass, const RectI& clientBox, float windowAlpha = 1.0f) const override;

    [[nodiscard]] Config& config() noexcept { return m_cfg; }
    [[nodiscard]] const Config& config() const noexcept { return m_cfg; }

private:
    Config m_cfg;
};

/**
 * @brief Container managing all decorations attached to a window across all Z-layers.
 */
class WindowDecorationStack {
public:
    WindowDecorationStack() = default;
    ~WindowDecorationStack() = default;

    void add(std::shared_ptr<IWindowDecoration> deco);
    void clear() noexcept { m_decorations.clear(); }
    [[nodiscard]] bool empty() const noexcept { return m_decorations.empty(); }
    [[nodiscard]] const std::vector<std::shared_ptr<IWindowDecoration>>& decorations() const noexcept {
        return m_decorations;
    }

    [[nodiscard]] RectI computeTotalDamageBox(const RectI& clientBox) const noexcept;

    void enqueueLayer(
        RenderPass& pass,
        DecorationLayer targetLayer,
        const RectI& clientBox,
        float windowAlpha = 1.0f) const;

private:
    std::vector<std::shared_ptr<IWindowDecoration>> m_decorations;
};

} // namespace nisaba::compositor
