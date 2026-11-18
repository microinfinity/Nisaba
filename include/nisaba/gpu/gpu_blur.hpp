#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "nisaba/color/color.hpp"

namespace nisaba::gpu {

/**
 * @brief Configuration parameters for the Dual-Kawase GPU Blur algorithm.
 */
struct DualKawaseConfig {
    int passes{4};                           ///< Number of down/up sample passes (1 to 5, default: 4)
    float radius{3.0f};                      ///< Kernel spread / blur radius multiplier (default: 3.0f)
    float brightness{1.0f};                  ///< Brightness adjustment factor (default: 1.0f)
    float contrast{1.0f};                    ///< Contrast adjustment factor (default: 1.0f)
    nisaba::Color tint{nisaba::Color::TRANSPARENT}; ///< Frosted glass color tint overlay (default: transparent)
};

/**
 * @brief Sovereign GPU Dual-Kawase Backdrop Blur Engine for Nisaba.
 *
 * Implements ARM / Marius Bjørge's SIGGRAPH Dual-Kawase hierarchical blur:
 *  - 5-tap pre-filtered downsampling kernel with hardware bilinear interpolation.
 *  - 8-tap tent-filter upsampling kernel avoiding blockiness and shimmering.
 *  - Pre-allocated ping-pong pyramid FBOs with zero runtime allocations per frame.
 *  - Single-primitive fullscreen triangle rasterization with zero seam overhead.
 *  - Sub-millisecond execution throughput (120+ FPS).
 */
class DualKawaseBlur {
public:
    DualKawaseBlur();
    ~DualKawaseBlur();

    DualKawaseBlur(const DualKawaseBlur&) = delete;
    DualKawaseBlur& operator=(const DualKawaseBlur&) = delete;

    /**
     * @brief Pre-allocates or resizes the FBO pyramid textures for given dimensions.
     */
    bool init(int baseWidth, int baseHeight, int maxPasses = 4);

    /**
     * @brief Blurs the active framebuffer contents and returns the resulting OpenGL texture ID.
     * @param fboId The framebuffer ID to read from (0 for screen or offscreen FBO).
     * @param width Width of the framebuffer in pixels.
     * @param height Height of the framebuffer in pixels.
     * @param config Blur configuration parameters.
     * @return OpenGL texture ID of the blurred result (level 0), or 0 on failure.
     */
    uint32_t blur_framebuffer(
        uint32_t fboId,
        int width,
        int height,
        const DualKawaseConfig& config = {}
    );

    /**
     * @brief Blurs a pre-existing OpenGL texture directly.
     * @param srcTexId The input OpenGL texture ID.
     * @param width Width of the texture.
     * @param height Height of the texture.
     * @param config Blur configuration parameters.
     * @return OpenGL texture ID of the blurred result (level 0), or 0 on failure.
     */
    uint32_t blur_texture(
        uint32_t srcTexId,
        int width,
        int height,
        const DualKawaseConfig& config = {}
    );

    void cleanup();

    [[nodiscard]] int base_width() const noexcept { return base_width_; }
    [[nodiscard]] int base_height() const noexcept { return base_height_; }
    [[nodiscard]] int allocated_passes() const noexcept { return static_cast<int>(pyramid_.size()); }
    [[nodiscard]] uint32_t output_texture() const noexcept;

private:
    struct PyramidLevel {
        int width{0};
        int height{0};
        uint32_t fbo{0};
        uint32_t texture{0};
    };

    struct ShaderProgramSet;

    bool setup_shaders();
    bool setup_vbo();
    uint32_t blur_texture_impl(uint32_t srcTexId, int width, int height, const DualKawaseConfig& config);

    int base_width_{0};
    int base_height_{0};
    uint32_t capture_fbo_{0};
    uint32_t capture_tex_{0};
    uint32_t vao_{0};
    uint32_t vbo_{0};
    std::vector<PyramidLevel> pyramid_;
    std::unique_ptr<ShaderProgramSet> shaders_;
};

} // namespace nisaba::gpu
