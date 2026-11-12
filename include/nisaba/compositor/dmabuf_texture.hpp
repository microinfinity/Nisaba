#pragma once

#include "nisaba/compositor/buffer.hpp"
#include "nisaba/compositor/egl_image.hpp"
#include "nisaba/compositor/texture.hpp"

namespace nisaba::compositor {

/**
 * @brief Zero-copy GPU texture binding an imported hardware DMA-BUF.
 * Implements both Texture (ITexture) and IBuffer for seamless Wayland client buffer import.
 */
class DmaBufTexture : public Texture, public IBuffer {
public:
    static std::shared_ptr<DmaBufTexture> create(
        std::shared_ptr<EglImage> image,
        uint32_t width,
        uint32_t height,
        uint32_t format,
        bool hasAlpha,
        GLenum target = GL_TEXTURE_2D,
        uint64_t modifier = DRM_FORMAT_MOD_LINEAR);

    DmaBufTexture(
        std::shared_ptr<EglImage> image,
        GLuint id,
        GLenum target,
        uint32_t width,
        uint32_t height,
        uint32_t format,
        bool hasAlpha,
        uint64_t modifier = DRM_FORMAT_MOD_LINEAR);

    ~DmaBufTexture() override = default;

    [[nodiscard]] uint32_t width() const noexcept override { return Texture::width(); }
    [[nodiscard]] uint32_t height() const noexcept override { return Texture::height(); }
    [[nodiscard]] uint32_t format() const noexcept override { return Texture::format(); }
    [[nodiscard]] uint64_t modifier() const noexcept override { return m_modifier; }
    [[nodiscard]] std::optional<DmaBufAttributes> exportDmaBuf() const override { return std::nullopt; }

    [[nodiscard]] const std::shared_ptr<EglImage>& image() const noexcept { return m_image; }

private:
    std::shared_ptr<EglImage> m_image;
    uint64_t m_modifier{DRM_FORMAT_MOD_LINEAR};
};

} // namespace nisaba::compositor
