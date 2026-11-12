#pragma once

#include "nisaba/compositor/blur_provider.hpp"
#include "nisaba/compositor/dual_kawase.hpp"
#include "nisaba/compositor/egl_context.hpp"
#include "nisaba/compositor/gbm_allocator.hpp"
#include "nisaba/compositor/render_pass.hpp"
#include "nisaba/compositor/render_target.hpp"
#include "nisaba/compositor/renderer_interface.hpp"
#include "nisaba/compositor/shader.hpp"
#include "nisaba/compositor/sync_fence.hpp"

#include <memory>
#include <string_view>

namespace nisaba::compositor {

/**
 * @brief Sovereign GPU-accelerated graphics rendering engine for Compositor.
 * Handles hardware EGL context, shaders, render passes, Dual Kawase / Pluggable Blur Materials,
 * Explicit Sync fences, and DMA-BUF import.
 */
class Renderer : public IRenderer {
public:
    static std::shared_ptr<Renderer> create(std::shared_ptr<EglContext> egl = nullptr);

    Renderer(
        std::shared_ptr<EglContext> egl,
        std::shared_ptr<ShaderManager> shaders,
        std::shared_ptr<DualKawaseBlur> blur = nullptr);
    ~Renderer() override;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] const DrmFormatSet& supportedFormats() const noexcept override;
    [[nodiscard]] std::shared_ptr<IBuffer> importBuffer(const DmaBufAttributes& attrs) override;
    bool beginFrame(IBuffer& target, const RectI& damage) override;
    bool endFrame() override;

    [[nodiscard]] const std::shared_ptr<EglContext>& egl() const noexcept { return m_egl; }
    [[nodiscard]] const std::shared_ptr<ShaderManager>& shaders() const noexcept { return m_shaders; }
    [[nodiscard]] const std::shared_ptr<DualKawaseBlur>& blur() const noexcept { return m_blur; }

    /**
     * @brief Selects the active pluggable blur material provider (Kawase, Acrylic, OpticalGlass, etc.).
     */
    bool setBlurMaterial(BlurMaterialType type);
    [[nodiscard]] const std::shared_ptr<IBlurProvider>& blurProvider() const noexcept { return m_blurProvider; }

    /**
     * @brief Checks whether hardware explicit sync (EGL_ANDROID_native_fence_sync) is supported.
     */
    [[nodiscard]] bool explicitSyncSupported() const noexcept;

    /**
     * @brief Exports a GPU completion fence for the current frame.
     */
    [[nodiscard]] std::unique_ptr<SyncFence> exportFrameFence() const;

    [[nodiscard]] std::unique_ptr<RenderPass> beginPass(
        std::shared_ptr<RenderTarget> target,
        wl_output_transform outputTransform = WL_OUTPUT_TRANSFORM_FLIPPED_180);

    [[nodiscard]] std::unique_ptr<IRenderPass> beginPass(
        std::shared_ptr<IRenderTarget> target,
        wl_output_transform outputTransform = WL_OUTPUT_TRANSFORM_FLIPPED_180) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::shared_ptr<EglContext> m_egl;
    std::shared_ptr<ShaderManager> m_shaders;
    std::shared_ptr<DualKawaseBlur> m_blur;
    std::shared_ptr<IBlurProvider> m_blurProvider;
};

/**
 * @brief Sovereign hardware graphics buffer memory allocator backed by Linux GBM.
 */
class Allocator : public IAllocator {
public:
    static std::shared_ptr<Allocator> create(std::shared_ptr<GbmAllocator> gbmAlloc = nullptr);

    explicit Allocator(std::shared_ptr<GbmAllocator> gbmAlloc);
    ~Allocator() override;

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::shared_ptr<IBuffer> allocate(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        uint64_t modifier) override;

    [[nodiscard]] const std::shared_ptr<GbmAllocator>& gbmAllocator() const noexcept { return m_gbmAlloc; }

private:
    std::shared_ptr<GbmAllocator> m_gbmAlloc;
};

} // namespace nisaba::compositor
