#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/gpu/context.hpp"
#include "nisaba/gpu/vulkan_surface.hpp"
#include "gl_headers.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>

namespace nisaba::gpu {

std::unique_ptr<Context> GpuSurface::create_context(int flags) {
    if (device_) {
        return device_->create_context(flags);
    }
    return nullptr;
}

GpuSurface::GpuSurface(
    std::shared_ptr<GpuDevice> device,
    uint32_t fbo_id,
    uint32_t resolve_fbo,
    uint32_t msaa_rb_id,
    uint32_t samples,
    uint32_t width,
    uint32_t height,
    std::shared_ptr<GpuTexture> texture
) : device_(std::move(device)),
    fbo_id_(fbo_id),
    resolve_fbo_(resolve_fbo),
    msaa_rb_id_(msaa_rb_id),
    samples_(samples),
    width_(width),
    height_(height),
    texture_(std::move(texture)),
    owns_fbo_(texture_ != nullptr),
    needs_resolve_(false) {}

GpuSurface::~GpuSurface() {
    if (device_ && owns_fbo_) {
        if (msaa_ds_rb_ != 0) {
            device_->delete_renderbuffer(msaa_ds_rb_);
            msaa_ds_rb_ = 0;
        }
        if (msaa_rb_id_ != 0) {
            device_->delete_renderbuffer(msaa_rb_id_);
            msaa_rb_id_ = 0;
        }
        if (resolve_ds_rb_ != 0) {
            device_->delete_renderbuffer(resolve_ds_rb_);
            resolve_ds_rb_ = 0;
        }
        if (fbo_id_ != 0 && fbo_id_ != resolve_fbo_) {
            device_->delete_framebuffer(fbo_id_);
            fbo_id_ = 0;
        }
        if (resolve_fbo_ != 0) {
            device_->delete_framebuffer(resolve_fbo_);
            resolve_fbo_ = 0;
        }
    }
}

std::shared_ptr<GpuSurface> GpuSurface::create(
    std::shared_ptr<GpuDevice> device,
    uint32_t width,
    uint32_t height,
    uint32_t samples
) {
    if (!device || width == 0 || height == 0) return nullptr;

    if (device->backend_type() == GpuBackendType::Vulkan) {
        return VulkanSurface::create(std::static_pointer_cast<VulkanDevice>(device), width, height, samples);
    }

    device->make_current();

    auto tex = GpuTexture::create(device, width, height, GpuTextureFormat::RGBA8);
    if (!tex) return nullptr;

    uint32_t resolve_fbo = device->create_framebuffer();
    if (resolve_fbo == 0) return nullptr;

    device->bind_framebuffer(resolve_fbo);
    device->attach_texture_to_framebuffer(tex->id());

    // Attach depth/stencil buffer to resolve FBO for stencil-and-cover
    uint32_t resolve_ds_rb = device->create_renderbuffer();
    device->bind_renderbuffer(resolve_ds_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    device->attach_renderbuffer_to_framebuffer(GL_DEPTH_STENCIL_ATTACHMENT, resolve_ds_rb);

    if (!device->check_framebuffer_complete()) {
        if (resolve_ds_rb != 0) device->delete_renderbuffer(resolve_ds_rb);
        device->delete_framebuffer(resolve_fbo);
        return nullptr;
    }

    uint32_t active_fbo = resolve_fbo;
    uint32_t msaa_rb = 0;
    uint32_t msaa_ds_rb = 0;
    uint32_t actual_samples = 1;

    if (samples > 1 && device->has_msaa_support()) {
        uint32_t msaa_fbo = device->create_framebuffer();
        msaa_rb = device->create_renderbuffer();
        msaa_ds_rb = device->create_renderbuffer();

        if (msaa_fbo != 0 && msaa_rb != 0 && msaa_ds_rb != 0) {
            device->bind_renderbuffer(msaa_rb);
            device->renderbuffer_storage_multisample(samples, GL_RGBA8, width, height);

            device->bind_renderbuffer(msaa_ds_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);

            device->bind_framebuffer(msaa_fbo);
            device->attach_renderbuffer_to_framebuffer(GL_COLOR_ATTACHMENT0, msaa_rb);
            device->attach_renderbuffer_to_framebuffer(GL_DEPTH_STENCIL_ATTACHMENT, msaa_ds_rb);

            if (device->check_framebuffer_complete()) {
                active_fbo = msaa_fbo;
                actual_samples = samples;
            } else {
                device->delete_framebuffer(msaa_fbo);
                device->delete_renderbuffer(msaa_rb);
                device->delete_renderbuffer(msaa_ds_rb);
                msaa_rb = 0;
                msaa_ds_rb = 0;
            }
        } else {
            if (msaa_fbo != 0) device->delete_framebuffer(msaa_fbo);
            if (msaa_rb != 0) { device->delete_renderbuffer(msaa_rb); msaa_rb = 0; }
            if (msaa_ds_rb != 0) { device->delete_renderbuffer(msaa_ds_rb); msaa_ds_rb = 0; }
        }
    }

    device->bind_framebuffer(active_fbo);
    device->set_viewport(0, 0, width, height);

    auto surface = std::shared_ptr<GpuSurface>(new GpuSurface(
        device, active_fbo, resolve_fbo, msaa_rb, actual_samples, width, height, tex
    ));
    surface->resolve_ds_rb_ = resolve_ds_rb;
    surface->msaa_ds_rb_ = msaa_ds_rb;
    return surface;
}

std::shared_ptr<GpuSurface> GpuSurface::from_screen(
    std::shared_ptr<GpuDevice> device,
    uint32_t width,
    uint32_t height,
    uint32_t screen_fbo_id
) {
    if (!device) return nullptr;
    device->make_current();
    return std::shared_ptr<GpuSurface>(new GpuSurface(
        device, screen_fbo_id, screen_fbo_id, 0, 1, width, height, nullptr
    ));
}

bool GpuSurface::resize_screen(uint32_t width, uint32_t height) noexcept {
    if (owns_fbo_ || width == 0 || height == 0) return false;
    width_ = width;
    height_ = height;
    return true;
}

void GpuSurface::bind() {
    if (device_) {
        device_->make_current();
        device_->bind_framebuffer(fbo_id_);
        device_->set_viewport(0, 0, width_, height_);
        if (samples_ > 1) {
            needs_resolve_ = true;
        }
    }
}

void GpuSurface::resolve() {
    if (!device_ || samples_ <= 1 || !needs_resolve_) return;
    device_->make_current();
    device_->blit_framebuffer(fbo_id_, resolve_fbo_, width_, height_);
    device_->bind_framebuffer(fbo_id_);
    needs_resolve_ = false;
}

void GpuSurface::resolve_rect_internal(const ScreenIntRect& damage_rect) {
    int32_t w = static_cast<int32_t>(width_);
    int32_t h = static_cast<int32_t>(height_);
    int32_t x0 = std::clamp(static_cast<int32_t>(damage_rect.x()), 0, w);
    int32_t y_top = std::clamp(static_cast<int32_t>(damage_rect.y()), 0, h);
    int32_t x1 = std::clamp(static_cast<int32_t>(damage_rect.x() + damage_rect.width()), 0, w);
    int32_t y_bot = std::clamp(static_cast<int32_t>(damage_rect.y() + damage_rect.height()), 0, h);
    if (x1 <= x0 || y_bot <= y_top) return;

    // Convert top-down screen Y to OpenGL bottom-left Y
    int32_t gl_y0 = h - y_bot;
    int32_t gl_y1 = h - y_top;
    device_->blit_framebuffer(fbo_id_, resolve_fbo_, x0, gl_y0, x1, gl_y1, x0, gl_y0, x1, gl_y1);
}

void GpuSurface::resolve(const ScreenIntRect& damage_rect) {
    if (!device_ || samples_ <= 1 || !needs_resolve_) return;
    device_->make_current();
    resolve_rect_internal(damage_rect);
    device_->bind_framebuffer(fbo_id_);
    needs_resolve_ = false;
}

void GpuSurface::resolve(const std::vector<ScreenIntRect>& damage_rects) {
    if (!device_ || samples_ <= 1 || !needs_resolve_) return;
    device_->make_current();
    for (const auto& r : damage_rects) {
        resolve_rect_internal(r);
    }
    device_->bind_framebuffer(fbo_id_);
    needs_resolve_ = false;
}

void GpuSurface::clear(nisaba::Color color) {
    if (device_) {
        device_->clear(color);
    }
}

void GpuSurface::discard() {
    if (!device_) return;
    device_->make_current();
    device_->bind_framebuffer(fbo_id_);
    needs_resolve_ = false;

#if defined(NISABA_TARGET_GLES)
    if (fbo_id_ == 0) {
        const GLenum attachments[3] = {GL_COLOR, GL_DEPTH, GL_STENCIL};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 3, attachments);
    } else {
        const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
        if (resolve_fbo_ != 0 && resolve_fbo_ != fbo_id_) {
            device_->bind_framebuffer(resolve_fbo_);
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
            device_->bind_framebuffer(fbo_id_);
        }
    }
#elif defined(GL_VERSION_4_3) || defined(GL_ARB_invalidate_subdata)
    if (glInvalidateFramebuffer != nullptr) {
        if (fbo_id_ == 0) {
            const GLenum attachments[3] = {GL_COLOR, GL_DEPTH, GL_STENCIL};
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 3, attachments);
        } else {
            const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT};
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
            if (resolve_fbo_ != 0 && resolve_fbo_ != fbo_id_) {
                device_->bind_framebuffer(resolve_fbo_);
                glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
                device_->bind_framebuffer(fbo_id_);
            }
        }
    }
#endif
}

std::shared_ptr<GpuTexture> GpuSurface::texture() {
    resolve();
    return texture_;
}

void GpuSurface::read_pixels(void* dst_rgba) {
    if (!device_ || !dst_rgba || width_ == 0 || height_ == 0) return;

    resolve();

    uint32_t read_fbo = (samples_ > 1) ? resolve_fbo_ : fbo_id_;
    device_->make_current();
    device_->bind_framebuffer(read_fbo);

    size_t row_bytes = width_ * 4;
    std::vector<uint8_t> bottom_up_buf(row_bytes * height_);

    device_->read_pixels(0, 0, width_, height_, bottom_up_buf.data());

    // Vertically flip to top-down 2D coordinate system
    uint8_t* dst_bytes = reinterpret_cast<uint8_t*>(dst_rgba);
    for (uint32_t y = 0; y < height_; ++y) {
        const uint8_t* src_row = bottom_up_buf.data() + (height_ - 1 - y) * row_bytes;
        uint8_t* dst_row = dst_bytes + y * row_bytes;
        std::memcpy(dst_row, src_row, row_bytes);
    }
}

void GpuSurface::read_pixels(PixmapMut& dst) {
    uint32_t w = std::min(width_, dst.width());
    uint32_t h = std::min(height_, dst.height());
    if (w == 0 || h == 0) return;

    if (w == dst.width() && h == dst.height() && dst.stride_bytes() == w * 4) {
        read_pixels(dst.data_mut());
        return;
    }

    std::vector<uint8_t> full_buf(width_ * height_ * 4);
    read_pixels(full_buf.data());

    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src_row = full_buf.data() + y * width_ * 4;
        uint8_t* dst_row = reinterpret_cast<uint8_t*>(dst.row(y));
        std::memcpy(dst_row, src_row, w * 4);
    }
}

std::optional<Pixmap> GpuSurface::to_pixmap() {
    auto pm = Pixmap::allocate(width_, height_);
    if (!pm.has_value()) return std::nullopt;

    auto mut_ref = pm->as_mut();
    read_pixels(mut_ref);
    return pm;
}

} // namespace nisaba::gpu
