#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/damage_ring.hpp"
#include "nisaba/compositor/dmabuf_texture.hpp"
#include "nisaba/compositor/dual_kawase.hpp"
#include "nisaba/compositor/egl_context.hpp"
#include "nisaba/compositor/matrix3.hpp"
#include "nisaba/compositor/render_pass.hpp"
#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/renderer.hpp"
#include "nisaba/compositor/scene_tree.hpp"
#include "nisaba/compositor/state_guard.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/gpu/runtime_effect.hpp"
#include "nisaba/effects/drop_shadow.hpp"
#include "nisaba/effects/glass.hpp"
#include "nisaba/svg/svg_document.hpp"

#include <memory>
#include <vector>
#include <string_view>

namespace nisaba::compositor {

/**
 * @brief Styling parameters for modern compositor server-side window decorations.
 */
struct WindowDecoStyle {
    float cornerRadius{10.0f};
    float roundingPower{2.0f};
    float titleBarHeight{30.0f};
    float borderWidth{1.0f};
    Color borderColor{0.35f, 0.40f, 0.50f, 0.35f};
    GradientSpec borderGradient{};
    float innerGlowRange{0.0f};
    Color innerGlowColor{1.0f, 1.0f, 1.0f, 0.25f};
    Color titleBarColor{0.14f, 0.15f, 0.18f, 0.95f};
    Color bodyColor{0.10f, 0.11f, 0.13f, 0.92f};
    Color titleTextColor{0.92f, 0.93f, 0.96f, 1.0f};
    float titleFontSize{13.0f};
    effects::DropShadow shadow{0.0f, 10.0f, 18.0f, nisaba::Color::from_rgba8(0, 0, 0, 160)};
    bool showButtons{true};
    Color closeButtonColor{0.95f, 0.32f, 0.30f, 1.0f};
    Color minButtonColor{0.95f, 0.75f, 0.20f, 1.0f};
    Color maxButtonColor{0.25f, 0.80f, 0.35f, 1.0f};

    WindowDecoStyle() noexcept = default;
};

/**
 * @brief High-level sovereign Compositor Canvas for modern Wayland window managers.
 *
 * Combines low-level zero-copy GPU window composition with Nisaba's vector graphics
 * features (dock panels, rounded rectangles, dropshadows, and frosted glass effects).
 */
class CompositorCanvas {
public:
    static std::unique_ptr<CompositorCanvas> create(
        std::shared_ptr<EglContext> egl = nullptr,
        std::shared_ptr<Renderer> renderer = nullptr);

    CompositorCanvas(
        std::shared_ptr<EglContext> egl,
        std::shared_ptr<Renderer> renderer);

    ~CompositorCanvas();

    CompositorCanvas(const CompositorCanvas&) = delete;
    CompositorCanvas& operator=(const CompositorCanvas&) = delete;

    /**
     * @brief Begins a compositor rendering frame targeting an FBO or hardware scanout target.
     */
    bool beginFrame(
        std::shared_ptr<RenderTarget> target,
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL);

    /**
     * @brief Sets the active damage region to restrict rendering commands.
     */
    void setDamage(const Region* damage);

    /**
     * @brief Clears the current frame target with the specified background color.
     */
    void clear(const Color& color);

    /**
     * @brief Renders a Wayland client surface texture (DMA-BUF or SHM).
     */
    void drawWindowSurface(
        const Texture& texture,
        const RectI& destBox,
        const RectF& srcBox = RectF{0.0, 0.0, 1.0, 1.0},
        float alpha = 1.0f,
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL);

    /**
     * @brief Draws a solid or translucent colored rectangle.
     */
    void drawRect(const RectI& box, const Color& color);

    /**
     * @brief Applies a Dual Kawase frosted glass blur to a sub-region or backdrop.
     */
    void drawFrostedGlass(
        const Texture& source,
        const RectI& destBox,
        const RectI& srcBox = RectI{0, 0, 0, 0},
        const DualKawaseConfig& config = {},
        const Texture* maskTexture = nullptr);

    /**
     * @brief Applies the active pluggable BlurMaterialProvider (Acrylic, OpticalGlass, WaterRipple, etc.).
     */
    void drawBlurMaterial(
        const Texture& source,
        const RectI& destBox,
        const BlurContext& ctx = {},
        const RectI& srcBox = RectI{0, 0, 0, 0});

    /**
     * @brief Renders a complete scene graph tree (immediate mode).
     */
    void drawScene(const SceneTree& tree);

    /**
     * @brief Renders a complete scene graph tree using the Deferred Pass Graph with
     * front-to-back Occlusion Culling.
     */
    PassExecutionStats drawSceneCulled(
        const SceneTree& tree,
        const ImageDescription& outputDesc = ImageDescription::srgbDefault());

    [[nodiscard]] RenderPass* currentPass() noexcept { return m_currentPass.get(); }

    // ─────────────────────────────────────────────────────────────────────────
    // Fused High-Performance Vector Graphics APIs (nisaba::gpu integration)
    // ─────────────────────────────────────────────────────────────────────────

    /// Provides direct access to Nisaba's sovereign vector graphics canvas targeting this frame.
    [[nodiscard]] gpu::GpuCanvas* vg();

    /// Renders analytical SDF rounded window frame, drop shadow, title text, and control buttons.
    void drawWindowDecoration(
        const RectI& clientBox,
        const WindowDecoStyle& style,
        std::string_view title = ""
    );

    /// Convenience overload with default style.
    void drawWindowDecoration(
        const RectI& clientBox,
        std::string_view title = ""
    );

    /// Renders a glassmorphic dock/panel with real-time backdrop blur and SVG launcher icons.
    void drawDesktopPanel(
        const RectI& panelBounds,
        const effects::GlassParams& glassParams,
        const std::vector<const svg::SvgDocument*>& launcherIcons = {}
    );

    /// Applies a procedural GLSL runtime effect (e.g. window animations, ripples, warps) to a window quad.
    void applyWindowShader(
        const Texture& windowTexture,
        const RectI& destBox,
        std::shared_ptr<gpu::RuntimeEffect> effect,
        gpu::RuntimeShaderBuilder uniforms
    );

    /// Renders vector typography directly onto the compositor surface using Nisaba's glyph cache.
    void drawText(
        std::string_view text,
        float x,
        float y,
        const Color& color = Color::white(),
        float font_size = 14.0f,
        const char* font_name = nullptr
    );

    /// Renders arbitrary W3C SVG vector graphics onto the compositor surface.
    void drawSvg(
        const svg::SvgDocument& doc,
        const RectI& destBounds
    );

    /**
     * @brief Finalizes and submits all accumulated commands to the GPU.
     */
    bool endFrame();

    [[nodiscard]] const std::shared_ptr<Renderer>& renderer() const noexcept { return m_renderer; }
    [[nodiscard]] const std::shared_ptr<EglContext>& egl() const noexcept { return m_egl; }

private:
    std::shared_ptr<EglContext> m_egl;
    std::shared_ptr<Renderer> m_renderer;
    std::unique_ptr<RenderPass> m_currentPass;
    const Region* m_currentDamage{nullptr};
    std::shared_ptr<RenderTarget> m_currentTarget{nullptr};

    std::shared_ptr<gpu::GpuDevice> m_gpuDevice{nullptr};
    std::shared_ptr<gpu::GpuSurface> m_gpuSurface{nullptr};
    std::unique_ptr<gpu::GpuCanvas> m_gpuCanvas{nullptr};
};

} // namespace nisaba::compositor
