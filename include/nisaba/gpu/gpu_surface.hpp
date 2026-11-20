#pragma once

#include <cstdint>
#include <memory>
#include "nisaba/gpu/gpu_types.hpp"
#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_texture.hpp"
#include "nisaba/canvas/pixmap.hpp"

namespace nisaba::gpu {

class Context;

/// Represents a GPU render target (FBO or screen framebuffer) with readback capabilities.
class GpuSurface {
public:
    static std::shared_ptr<GpuSurface> create(
        std::shared_ptr<GpuDevice> device,
        uint32_t width,
        uint32_t height,
        uint32_t samples = 4
    );

    static std::shared_ptr<GpuSurface> from_screen(
        std::shared_ptr<GpuDevice> device,
        uint32_t width,
        uint32_t height,
        uint32_t screen_fbo_id = 0
    );

    /// Wraps an existing compositor RenderTarget FBO into a sovereign non-owning GpuSurface.
    static std::shared_ptr<GpuSurface> from_render_target(
        std::shared_ptr<GpuDevice> device,
        uint32_t width,
        uint32_t height,
        uint32_t fbo_id
    ) {
        return from_screen(std::move(device), width, height, fbo_id);
    }

    virtual ~GpuSurface();

    [[nodiscard]] virtual GpuBackendType backend_type() const noexcept {
        return device_ ? device_->backend_type() : GpuBackendType::OpenGL;
    }
    virtual std::unique_ptr<Context> create_context(int flags = 0);

    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    bool resize_screen(uint32_t width, uint32_t height) noexcept;
    [[nodiscard]] uint32_t fbo_id() const noexcept { return fbo_id_; }
    [[nodiscard]] uint32_t resolve_fbo() const noexcept { return resolve_fbo_; }
    [[nodiscard]] uint32_t samples() const noexcept { return samples_; }
    [[nodiscard]] std::shared_ptr<GpuTexture> texture();
    [[nodiscard]] std::shared_ptr<GpuDevice> device() const noexcept { return device_; }

    virtual void bind();
    virtual void resolve();
    virtual void resolve(const ScreenIntRect& damage_rect);
    virtual void resolve(const std::vector<ScreenIntRect>& damage_rects);
    virtual void clear(nisaba::Color color);

    /// Damage tracking and frame preservation
    virtual void set_preserve_contents(bool preserve) { preserve_contents_ = preserve; }
    [[nodiscard]] virtual bool preserve_contents() const noexcept { return preserve_contents_; }

    /// Reads back pixels from the GPU into CPU memory.
    virtual void read_pixels(void* dst_rgba);
    virtual void read_pixels(PixmapMut& dst);
    [[nodiscard]] virtual std::optional<Pixmap> to_pixmap();

protected:
    GpuSurface(
        std::shared_ptr<GpuDevice> device,
        uint32_t fbo_id,
        uint32_t resolve_fbo,
        uint32_t msaa_rb_id,
        uint32_t samples,
        uint32_t width,
        uint32_t height,
        std::shared_ptr<GpuTexture> texture
    );

    std::shared_ptr<GpuDevice> device_;
    uint32_t fbo_id_{0};
    uint32_t resolve_fbo_{0};
    uint32_t msaa_rb_id_{0};
    uint32_t resolve_ds_rb_{0};
    uint32_t msaa_ds_rb_{0};
    uint32_t samples_{1};
    uint32_t width_{0};
    uint32_t height_{0};
    std::shared_ptr<GpuTexture> texture_{nullptr};
    bool owns_fbo_{true};
    bool needs_resolve_{false};
    bool preserve_contents_{false};

private:
    void resolve_rect_internal(const ScreenIntRect& damage_rect);
};

} // namespace nisaba::gpu
