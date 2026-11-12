#pragma once

#include "nisaba/compositor/egl_context.hpp"
#include "nisaba/compositor/egl_image.hpp"
#include "nisaba/compositor/gbm_buffer.hpp"
#include "nisaba/compositor/texture.hpp"
#include "nisaba/compositor/texture_interface.hpp"

#include <GLES2/gl2.h>
#include <memory>

namespace nisaba::compositor {

/**
 * @brief Sovereign abstract render target interface decoupled from any specific graphics API.
 */
class IRenderTarget {
public:
    virtual ~IRenderTarget() = default;

    [[nodiscard]] virtual int width() const noexcept = 0;
    [[nodiscard]] virtual int height() const noexcept = 0;
    [[nodiscard]] virtual RenderBackendType backendType() const noexcept = 0;
    virtual void bind() const noexcept = 0;
    [[nodiscard]] virtual void* nativeHandle() const noexcept = 0;
};

/**
 * @brief Sovereign RenderTarget wrapping OpenGL ES Framebuffer Objects (FBO)
 * backed by hardware scanout buffers (GBM/DMA-BUF) or GPU textures.
 */
class RenderTarget : public IRenderTarget {
public:
    static std::shared_ptr<RenderTarget> createDefault(int width, int height);

    static std::shared_ptr<RenderTarget> fromTexture(std::shared_ptr<Texture> texture);

    /**
     * @brief Allocates a self-contained offscreen FBO backed by a GPU Texture (8-bit RGBA or 16-bit FP16).
     */
    static std::shared_ptr<RenderTarget> createOffscreen(int width, int height, bool preferFp16 = false);

    static std::shared_ptr<RenderTarget> fromGbmBuffer(
        std::shared_ptr<EglContext> context,
        std::shared_ptr<GbmBuffer> buffer);

    RenderTarget(
        int width,
        int height,
        GLuint fbo,
        GLuint rbo = 0,
        bool ownsResources = true,
        std::shared_ptr<EglImage> image = nullptr,
        std::shared_ptr<GbmBuffer> buffer = nullptr,
        std::shared_ptr<Texture> texture = nullptr,
        bool isFp16 = false);

    ~RenderTarget() override;

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    [[nodiscard]] int width() const noexcept override { return m_width; }
    [[nodiscard]] int height() const noexcept override { return m_height; }
    [[nodiscard]] RenderBackendType backendType() const noexcept override { return RenderBackendType::OpenGL; }
    [[nodiscard]] void* nativeHandle() const noexcept override {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(m_fbo));
    }
    [[nodiscard]] GLuint fbo() const noexcept { return m_fbo; }
    [[nodiscard]] GLuint rbo() const noexcept { return m_rbo; }
    [[nodiscard]] bool isFp16() const noexcept { return m_isFp16; }

    [[nodiscard]] const std::shared_ptr<GbmBuffer>& buffer() const noexcept { return m_buffer; }
    [[nodiscard]] const std::shared_ptr<EglImage>& image() const noexcept { return m_image; }
    [[nodiscard]] const std::shared_ptr<Texture>& texture() const noexcept { return m_texture; }
    [[nodiscard]] const std::shared_ptr<Texture>& mirrorTexture() const noexcept { return m_mirrorTexture; }

    /**
     * @brief Attaches a secondary texture to COLOR_ATTACHMENT1 for single-pass display mirroring (MRT).
     */
    bool attachMirrorTexture(std::shared_ptr<Texture> mirrorTex);
    void detachMirrorTexture() noexcept;

    void bind() const noexcept override;
    static void unbind() noexcept;

private:
    int m_width{0};
    int m_height{0};
    GLuint m_fbo{0};
    GLuint m_rbo{0};
    bool m_ownsResources{true};
    bool m_isFp16{false};
    std::shared_ptr<EglImage> m_image;
    std::shared_ptr<GbmBuffer> m_buffer;
    std::shared_ptr<Texture> m_texture;
    std::shared_ptr<Texture> m_mirrorTexture;
};

} // namespace nisaba::compositor
