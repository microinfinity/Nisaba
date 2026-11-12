#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/drm_formats.hpp"
#include "nisaba/compositor/log.hpp"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstring>

namespace nisaba::compositor {

namespace {

#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A
#endif

#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif

#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

#ifndef GL_COLOR_ATTACHMENT1_EXT
#define GL_COLOR_ATTACHMENT1_EXT 0x8CE1
#endif

bool glExtensionSupported(const char* ext) noexcept {
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!exts || !ext) return false;
    return std::strstr(exts, ext) != nullptr;
}

} // namespace

std::shared_ptr<RenderTarget> RenderTarget::createDefault(int width, int height) {
    return std::make_shared<RenderTarget>(width, height, 0, 0, false);
}

std::shared_ptr<RenderTarget> RenderTarget::fromTexture(std::shared_ptr<Texture> texture) {
    if (!texture || !texture->id()) {
        logError("Cannot create RenderTarget from null or invalid texture");
        return nullptr;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (!fbo) {
        logError("Failed to generate OpenGL framebuffer");
        return nullptr;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->id(), 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        logError("Texture RenderTarget FBO incomplete: status 0x{:x}", status);
        glDeleteFramebuffers(1, &fbo);
        return nullptr;
    }

    bool fp16 = drmFormatIsFp16(texture->format());
    return std::make_shared<RenderTarget>(
        static_cast<int>(texture->width()),
        static_cast<int>(texture->height()),
        fbo,
        0,
        true,
        nullptr,
        nullptr,
        std::move(texture),
        fp16);
}

std::shared_ptr<RenderTarget> RenderTarget::createOffscreen(int width, int height, bool preferFp16) {
    if (width <= 0 || height <= 0) return nullptr;

    GLuint texId = 0;
    glGenTextures(1, &texId);
    if (!texId) return nullptr;

    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    bool usedFp16 = false;
    uint32_t drmFmt = DRM_FORMAT_ABGR8888;

    if (preferFp16 && (glExtensionSupported("GL_EXT_color_buffer_half_float") ||
                       glExtensionSupported("GL_EXT_color_buffer_float"))) {
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_EXT, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        if (glGetError() != GL_NO_ERROR) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_HALF_FLOAT_OES, nullptr);
        }
        if (glGetError() == GL_NO_ERROR) {
            usedFp16 = true;
            drmFmt = DRM_FORMAT_ABGR16161616F;
        }
    }

    if (!usedFp16) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    auto tex = std::make_shared<Texture>(
        texId,
        GL_TEXTURE_2D,
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        drmFmt,
        true);

    auto rt = fromTexture(std::move(tex));
    if (!rt && preferFp16 && usedFp16) {
        // Fallback to 8-bit if driver rejected FP16 attachment
        return createOffscreen(width, height, false);
    }
    return rt;
}

std::shared_ptr<RenderTarget> RenderTarget::fromGbmBuffer(
    std::shared_ptr<EglContext> context,
    std::shared_ptr<GbmBuffer> buffer) {
    if (!context || !buffer) {
        logError("Cannot create RenderTarget with null EglContext or GbmBuffer");
        return nullptr;
    }

    auto dmaAttrs = buffer->exportDmaBuf();
    if (!dmaAttrs) {
        logError("Failed to export DMA-BUF attributes from GbmBuffer");
        return nullptr;
    }

    auto image = EglImage::createFromDmaBuf(context, *dmaAttrs);
    if (!image) {
        logError("Failed to import DMA-BUF into EGLImage for RenderTarget");
        return nullptr;
    }

    typedef void (*PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)(GLenum target, GLeglImageOES image);
    auto glEGLImageTargetRenderbufferStorageOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
            context->getProcAddress("glEGLImageTargetRenderbufferStorageOES"));

    if (!glEGLImageTargetRenderbufferStorageOES) {
        logError("Extension glEGLImageTargetRenderbufferStorageOES is unavailable");
        return nullptr;
    }

    GLuint rbo = 0;
    glGenRenderbuffers(1, &rbo);
    if (!rbo) {
        logError("Failed to generate renderbuffer for hardware scanout target");
        return nullptr;
    }

    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image->raw());
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (!fbo) {
        logError("Failed to generate framebuffer for hardware scanout target");
        glDeleteRenderbuffers(1, &rbo);
        return nullptr;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        logError("Hardware RenderTarget FBO incomplete: status 0x{:x}", status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &rbo);
        return nullptr;
    }

    return std::make_shared<RenderTarget>(
        buffer->width(),
        buffer->height(),
        fbo,
        rbo,
        true,
        image,
        buffer
    );
}

RenderTarget::RenderTarget(
    int width,
    int height,
    GLuint fbo,
    GLuint rbo,
    bool ownsResources,
    std::shared_ptr<EglImage> image,
    std::shared_ptr<GbmBuffer> buffer,
    std::shared_ptr<Texture> texture,
    bool isFp16)
    : m_width(width),
      m_height(height),
      m_fbo(fbo),
      m_rbo(rbo),
      m_ownsResources(ownsResources),
      m_isFp16(isFp16),
      m_image(std::move(image)),
      m_buffer(std::move(buffer)),
      m_texture(std::move(texture)) {}

RenderTarget::~RenderTarget() {
    if (m_ownsResources) {
        if (m_fbo) {
            glDeleteFramebuffers(1, &m_fbo);
            m_fbo = 0;
        }
        if (m_rbo) {
            glDeleteRenderbuffers(1, &m_rbo);
            m_rbo = 0;
        }
    }
}

bool RenderTarget::attachMirrorTexture(std::shared_ptr<Texture> mirrorTex) {
    if (!m_fbo || !mirrorTex || !mirrorTex->id()) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT1_EXT,
        GL_TEXTURE_2D,
        mirrorTex->id(),
        0);

    typedef void (*PFNGLDRAWBUFFERSEXTPROC)(GLsizei n, const GLenum* bufs);
    auto drawBuffers = reinterpret_cast<PFNGLDRAWBUFFERSEXTPROC>(
        eglGetProcAddress("glDrawBuffersEXT"));
    if (!drawBuffers) {
        drawBuffers = reinterpret_cast<PFNGLDRAWBUFFERSEXTPROC>(
            eglGetProcAddress("glDrawBuffers"));
    }

    if (drawBuffers) {
        const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1_EXT};
        drawBuffers(2, bufs);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        detachMirrorTexture();
        return false;
    }

    m_mirrorTexture = std::move(mirrorTex);
    return true;
}

void RenderTarget::detachMirrorTexture() noexcept {
    if (m_fbo && m_mirrorTexture) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1_EXT, GL_TEXTURE_2D, 0, 0);

        typedef void (*PFNGLDRAWBUFFERSEXTPROC)(GLsizei n, const GLenum* bufs);
        auto drawBuffers = reinterpret_cast<PFNGLDRAWBUFFERSEXTPROC>(
            eglGetProcAddress("glDrawBuffersEXT"));
        if (!drawBuffers) {
            drawBuffers = reinterpret_cast<PFNGLDRAWBUFFERSEXTPROC>(
                eglGetProcAddress("glDrawBuffers"));
        }
        if (drawBuffers) {
            const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
            drawBuffers(1, bufs);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    m_mirrorTexture.reset();
}

void RenderTarget::bind() const noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);
}

void RenderTarget::unbind() noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace nisaba::compositor
