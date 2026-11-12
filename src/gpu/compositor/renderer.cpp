#include "nisaba/compositor/renderer.hpp"
#include "nisaba/compositor/dmabuf_texture.hpp"
#include "nisaba/compositor/gbm_device.hpp"
#include "nisaba/compositor/log.hpp"

#include <EGL/eglext.h>
#include <vector>

namespace nisaba::compositor {

struct Renderer::Impl {
    DrmFormatSet formats;
    bool inFrame{false};

    explicit Impl(const std::shared_ptr<EglContext>& egl) {
        // Always register baseline 8-bit, 10-bit, and FP16 linear formats
        const uint32_t baseFormats[] = {
            DRM_FORMAT_XRGB8888,
            DRM_FORMAT_ARGB8888,
            DRM_FORMAT_XBGR8888,
            DRM_FORMAT_ABGR8888,
            DRM_FORMAT_XRGB2101010,
            DRM_FORMAT_ARGB2101010,
            DRM_FORMAT_XBGR2101010,
            DRM_FORMAT_ABGR2101010,
            DRM_FORMAT_XBGR16161616F,
            DRM_FORMAT_ABGR16161616F
        };
        for (uint32_t fmt : baseFormats) {
            formats.add(fmt, DRM_FORMAT_MOD_LINEAR);
            formats.add(fmt, DRM_FORMAT_MOD_INVALID);
        }

        // Dynamically query EGL_EXT_image_dma_buf_import_modifiers if available
        if (egl && egl->display() != EGL_NO_DISPLAY &&
            egl->hasExtension("EGL_EXT_image_dma_buf_import_modifiers")) {
            auto queryFormats = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(
                eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
            auto queryModifiers = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
                eglGetProcAddress("eglQueryDmaBufModifiersEXT"));

            if (queryFormats) {
                EGLint numFormats = 0;
                if (queryFormats(egl->display(), 0, nullptr, &numFormats) && numFormats > 0) {
                    std::vector<EGLint> eglFormats(static_cast<size_t>(numFormats));
                    if (queryFormats(egl->display(), numFormats, eglFormats.data(), &numFormats)) {
                        for (EGLint i = 0; i < numFormats; ++i) {
                            uint32_t drmFmt = static_cast<uint32_t>(eglFormats[static_cast<size_t>(i)]);
                            formats.add(drmFmt, DRM_FORMAT_MOD_LINEAR);
                            if (queryModifiers) {
                                EGLint numMods = 0;
                                if (queryModifiers(egl->display(), eglFormats[static_cast<size_t>(i)], 0, nullptr, nullptr, &numMods) &&
                                    numMods > 0) {
                                    std::vector<EGLuint64KHR> mods(static_cast<size_t>(numMods));
                                    std::vector<EGLBoolean> externalOnly(static_cast<size_t>(numMods), EGL_FALSE);
                                    if (queryModifiers(egl->display(), eglFormats[static_cast<size_t>(i)], numMods, mods.data(), externalOnly.data(), &numMods)) {
                                        for (EGLint m = 0; m < numMods; ++m) {
                                            if (!externalOnly[static_cast<size_t>(m)]) {
                                                formats.add(drmFmt, static_cast<uint64_t>(mods[static_cast<size_t>(m)]));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
};

std::shared_ptr<Renderer> Renderer::create(std::shared_ptr<EglContext> egl) {
    if (!egl) {
        egl = EglContext::createAuto();
    }
    if (!egl) {
        logError("Failed to initialize hardware EGL context for Renderer");
        return nullptr;
    }

    if (!egl->makeCurrent()) {
        logError("Failed to make EGL context current for Renderer initialization");
        return nullptr;
    }

    auto shaders = ShaderManager::create(true);
    if (!shaders) {
        logError("Failed to compile ShaderManager for Renderer");
        return nullptr;
    }

    auto blur = DualKawaseBlur::create(egl);
    if (!blur) {
        logWarn("DualKawaseBlur could not be initialized for Renderer (will proceed without blur)");
    }

    return std::make_shared<Renderer>(std::move(egl), std::move(shaders), std::move(blur));
}

Renderer::Renderer(
    std::shared_ptr<EglContext> egl,
    std::shared_ptr<ShaderManager> shaders,
    std::shared_ptr<DualKawaseBlur> blur)
    : m_impl(std::make_unique<Impl>(egl)),
      m_egl(std::move(egl)),
      m_shaders(std::move(shaders)),
      m_blur(std::move(blur)) {
    if (m_egl) {
        m_blurProvider = BlurProviderFactory::create(BlurMaterialType::Kawase);
    }
}

Renderer::~Renderer() = default;

std::string_view Renderer::name() const noexcept {
    return "Nisaba-Sovereign-GPU-Renderer";
}

const DrmFormatSet& Renderer::supportedFormats() const noexcept {
    return m_impl->formats;
}

std::shared_ptr<IBuffer> Renderer::importBuffer(const DmaBufAttributes& attrs) {
    if (!m_egl) return nullptr;

    auto img = EglImage::createFromDmaBuf(m_egl, attrs);
    if (!img) {
        logError("Renderer failed to import DMA-BUF buffer");
        return nullptr;
    }

    return DmaBufTexture::create(
        std::move(img),
        static_cast<int>(attrs.width),
        static_cast<int>(attrs.height),
        attrs.format,
        drmFormatHasAlpha(attrs.format),
        GL_TEXTURE_2D,
        attrs.modifier);
}

bool Renderer::setBlurMaterial(BlurMaterialType type) {
    if (!m_egl) return false;
    auto provider = BlurProviderFactory::create(type);
    if (!provider) return false;
    m_blurProvider = std::move(provider);
    return true;
}

bool Renderer::explicitSyncSupported() const noexcept {
    return m_egl && SyncFence::isSupported(*m_egl);
}

std::unique_ptr<SyncFence> Renderer::exportFrameFence() const {
    if (!m_egl) return nullptr;
    return SyncFence::exportFromCurrentContext(*m_egl);
}

bool Renderer::beginFrame(IBuffer& /*target*/, const RectI& /*damage*/) {
    m_impl->inFrame = true;
    return true;
}

bool Renderer::endFrame() {
    if (!m_impl->inFrame) return false;
    m_impl->inFrame = false;
    return true;
}

std::unique_ptr<RenderPass> Renderer::beginPass(
    std::shared_ptr<RenderTarget> target,
    wl_output_transform outputTransform) {
    if (!m_egl || !m_shaders || !target) return nullptr;
    if (eglGetCurrentContext() != m_egl->context()) {
        m_egl->makeCurrent();
    }
    return RenderPass::begin(std::move(target), m_shaders, outputTransform);
}

std::unique_ptr<IRenderPass> Renderer::beginPass(
    std::shared_ptr<IRenderTarget> target,
    wl_output_transform outputTransform) {
    auto glTarget = std::dynamic_pointer_cast<RenderTarget>(target);
    if (!glTarget) return nullptr;
    return beginPass(std::move(glTarget), outputTransform);
}

// ==========================================
// Allocator Implementation
// ==========================================

std::shared_ptr<Allocator> Allocator::create(std::shared_ptr<GbmAllocator> gbmAlloc) {
    if (!gbmAlloc) {
        auto dev = GbmDevice::createAuto();
        if (!dev) {
            logError("Allocator failed to open DRM device for GBM");
            return nullptr;
        }
        gbmAlloc = GbmAllocator::create(dev);
        if (!gbmAlloc) {
            logError("Allocator failed to create GbmAllocator");
            return nullptr;
        }
    }
    return std::make_shared<Allocator>(std::move(gbmAlloc));
}

Allocator::Allocator(std::shared_ptr<GbmAllocator> gbmAlloc)
    : m_gbmAlloc(std::move(gbmAlloc)) {}

Allocator::~Allocator() = default;

std::string_view Allocator::name() const noexcept {
    return "Nisaba-Sovereign-GBM-Allocator";
}

std::shared_ptr<IBuffer> Allocator::allocate(
    uint32_t width,
    uint32_t height,
    uint32_t format,
    uint64_t modifier) {
    if (!m_gbmAlloc) return nullptr;
    return m_gbmAlloc->allocateBuffer(width, height, format, modifier);
}

} // namespace nisaba::compositor
