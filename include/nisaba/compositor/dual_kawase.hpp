#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/egl_context.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/texture.hpp"

#include <GLES2/gl2.h>
#include <memory>
#include <vector>
#include <wayland-server-protocol.h>

namespace nisaba::compositor {

/**
 * @brief Configuration parameters for the Dual Kawase Blur algorithm.
 */
struct DualKawaseConfig {
    int passes{4};                           ///< Number of down/up sample passes (1 to 6, default: 4)
    float radius{3.0f};                      ///< Kernel spread / blur radius multiplier (default: 3.0f)
    float brightness{1.0f};                  ///< Brightness adjustment factor (default: 1.0f)
    float contrast{1.0f};                    ///< Contrast adjustment factor (default: 1.0f)
    Color tint{0.0f, 0.0f, 0.0f, 0.0f};      ///< Frosted glass color tint overlay (default: transparent)
    float noise{0.0f};                       ///< Dithering amplitude to eliminate 8-bit banding (0.0 to 10.0, default: 0.0f)
};

/**
 * @brief High-performance, bandwidth-optimized Dual Kawase Blur engine for Compositor.
 *
 * Implements the SIGGRAPH 2015 Dual Kawase algorithm (Marius Bjørge, ARM)
 * featuring:
 *  - 5-tap pre-filtered downsampling kernel with hardware bilinear interpolation.
 *  - 8-tap tent-filter upsampling kernel avoiding blockiness and shimmering.
 *  - Pre-allocated ping-pong pyramid FBOs with zero runtime allocations per frame.
 *  - Single-primitive fullscreen triangle rasterization with zero seam overhead.
 *  - Fullscreen or sub-region blur for transparent windows, docks, and panels.
 *  - Integrated post-processing (dithering, tinting, contrast, and brightness).
 */
class DualKawaseBlur {
public:
    static std::shared_ptr<DualKawaseBlur> create(std::shared_ptr<EglContext> egl = nullptr);

    explicit DualKawaseBlur(std::shared_ptr<EglContext> egl);
    ~DualKawaseBlur();

    DualKawaseBlur(const DualKawaseBlur&) = delete;
    DualKawaseBlur& operator=(const DualKawaseBlur&) = delete;

    /**
     * @brief Pre-allocates or updates the FBO pyramid textures for given dimensions.
     */
    bool init(int baseWidth, int baseHeight, int maxPasses = 5);

    /**
     * @brief Blurs an input texture and returns the blurred output texture.
     */
    [[nodiscard]] std::shared_ptr<Texture> blur(
        const Texture& source,
        const DualKawaseConfig& config = {});

    /**
     * @brief Blurs a sub-region of an input texture and outputs to a RenderTarget.
     */
    bool renderBlurSubRegion(
        const Texture& source,
        const RectI& srcRect,
        RenderTarget& target,
        const RectI& destRect,
        const DualKawaseConfig& config = {},
        const Texture* maskTexture = nullptr);

    /**
     * @brief Blurs full input texture directly into target RenderTarget at destRect.
     */
    bool renderBlur(
        const Texture& source,
        RenderTarget& target,
        const RectI& destRect,
        const DualKawaseConfig& config = {},
        const Texture* maskTexture = nullptr) {
        return renderBlurSubRegion(source, RectI{0, 0, 0, 0}, target, destRect, config, maskTexture);
    }

    /**
     * @brief Renders a sub-region from a pre-blurred backdrop texture directly onto a target.
     */
    bool renderBlurredBackdrop(
        const Texture& blurredTex,
        const RectI& destBox,
        const RectF& srcBox,
        RenderTarget& target,
        const DualKawaseConfig& config = {},
        const Texture* maskTexture = nullptr,
        wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL,
        const RectF& maskSrcBox = RectF{0.0f, 0.0f, 1.0f, 1.0f});

    [[nodiscard]] int baseWidth() const noexcept { return m_baseWidth; }
    [[nodiscard]] int baseHeight() const noexcept { return m_baseHeight; }
    [[nodiscard]] int allocatedPasses() const noexcept { return static_cast<int>(m_pyramid.size()); }

    /**
     * @brief Draws the persistent unit quad (0,0)-(1,1) using DualKawaseBlur's static VBO.
     */
    void drawUnitQuad(GLint posAttrib) const noexcept;

private:
    struct PyramidLevel {
        int width{0};
        int height{0};
        GLuint fbo{0};
        GLuint texture{0};
        std::shared_ptr<Texture> textureObj;
    };

    struct ShaderProgramSet;

    void cleanupPyramid();
    bool setupShaders();
    bool setupVbo();

    std::shared_ptr<EglContext> m_egl;
    int m_baseWidth{0};
    int m_baseHeight{0};
    std::vector<PyramidLevel> m_pyramid;

    GLuint m_vbo{0};
    std::unique_ptr<ShaderProgramSet> m_shaders;
};

} // namespace nisaba::compositor
