#include "nisaba/compositor/dmabuf_texture.hpp"
#include "nisaba/compositor/log.hpp"

namespace nisaba::compositor {

std::shared_ptr<DmaBufTexture> DmaBufTexture::create(
    std::shared_ptr<EglImage> image,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    bool hasAlpha,
    GLenum target,
    uint64_t modifier) {
    if (!image || image->raw() == EGL_NO_IMAGE_KHR) {
        logError("DmaBufTexture::create: Invalid EglImage");
        return nullptr;
    }

    GLuint texId = 0;
    glGenTextures(1, &texId);
    if (texId == 0) {
        logError("DmaBufTexture::create: Failed to generate GL texture");
        return nullptr;
    }

    typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
    auto glImageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        image->context()->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (!glImageTarget) {
        logError("DmaBufTexture::create: glEGLImageTargetTexture2DOES not available");
        glDeleteTextures(1, &texId);
        return nullptr;
    }

    while (glGetError() != GL_NO_ERROR);

    glBindTexture(target, texId);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glImageTarget(target, image->raw());
    GLenum err = glGetError();
    if (err != GL_NO_ERROR && target == GL_TEXTURE_2D) {
        logWarn("DmaBufTexture: glImageTarget(GL_TEXTURE_2D) returned 0x{:04x}, retrying with GL_TEXTURE_EXTERNAL_OES", err);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texId);
        texId = 0;
        glGenTextures(1, &texId);
        if (texId == 0) {
            logError("DmaBufTexture: Failed to generate fallback GL texture for external OES");
            return nullptr;
        }
        target = GL_TEXTURE_EXTERNAL_OES;
        glBindTexture(target, texId);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glImageTarget(target, image->raw());
        GLenum err2 = glGetError();
        if (err2 != GL_NO_ERROR) {
            logError("DmaBufTexture: glImageTarget(GL_TEXTURE_EXTERNAL_OES) failed with 0x{:04x}", err2);
        }
        while (glGetError() != GL_NO_ERROR);
    }
    glBindTexture(target, 0);

    return std::make_shared<DmaBufTexture>(
        std::move(image),
        texId,
        target,
        width,
        height,
        format,
        hasAlpha,
        modifier);
}

DmaBufTexture::DmaBufTexture(
    std::shared_ptr<EglImage> image,
    GLuint id,
    GLenum target,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    bool hasAlpha,
    uint64_t modifier)
    : Texture(id, target, width, height, format, hasAlpha),
      m_image(std::move(image)),
      m_modifier(modifier) {}

} // namespace nisaba::compositor
