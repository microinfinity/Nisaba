#include "nisaba/gpu/gpu_texture.hpp"

namespace nisaba::gpu {

GpuTexture::GpuTexture(
    std::shared_ptr<GpuDevice> device,
    uint32_t texture_id,
    uint32_t width,
    uint32_t height,
    GpuTextureFormat format,
    bool owns_texture
) : device_(std::move(device)),
    texture_id_(texture_id),
    width_(width),
    height_(height),
    format_(format),
    owns_texture_(owns_texture) {}

GpuTexture::~GpuTexture() {
    if (device_ && texture_id_ != 0 && owns_texture_) {
        device_->delete_texture(texture_id_);
    }
}

std::shared_ptr<GpuTexture> GpuTexture::from_native_handle(
    std::shared_ptr<GpuDevice> device,
    uint32_t texture_id,
    uint32_t width,
    uint32_t height,
    GpuTextureFormat format,
    bool owns_texture
) {
    if (texture_id == 0 || width == 0 || height == 0) return nullptr;
    return std::shared_ptr<GpuTexture>(new GpuTexture(
        std::move(device), texture_id, width, height, format, owns_texture
    ));
}

std::shared_ptr<GpuTexture> GpuTexture::create(
    std::shared_ptr<GpuDevice> device,
    uint32_t width,
    uint32_t height,
    GpuTextureFormat format,
    GpuTextureFilter filter,
    GpuTextureWrap wrap
) {
    if (!device || width == 0 || height == 0) return nullptr;

    uint32_t tex_id = device->create_texture();
    if (tex_id == 0) return nullptr;

    device->bind_texture(0, tex_id);
    device->upload_texture_image_2d(width, height, nullptr, format, filter, wrap);

    return std::shared_ptr<GpuTexture>(new GpuTexture(device, tex_id, width, height, format));
}

std::shared_ptr<GpuTexture> GpuTexture::create_from_pixmap(
    std::shared_ptr<GpuDevice> device,
    const PixmapRef& pixmap,
    GpuTextureFilter filter,
    GpuTextureWrap wrap
) {
    if (!device || pixmap.width() == 0 || pixmap.height() == 0) return nullptr;

    uint32_t tex_id = device->create_texture();
    if (tex_id == 0) return nullptr;

    device->bind_texture(0, tex_id);
    size_t tight_stride = static_cast<size_t>(pixmap.width()) * 4;
    if (pixmap.format() == PixelFormat::RGBA8888 && pixmap.stride_bytes() == tight_stride) {
        device->upload_texture_image_2d(
            pixmap.width(), pixmap.height(),
            pixmap.data(),
            GpuTextureFormat::RGBA8,
            filter,
            wrap
        );
    } else {
        Pixmap owned_rgba = pixmap.to_rgba8888();
        device->upload_texture_image_2d(
            pixmap.width(), pixmap.height(),
            owned_rgba.data(),
            GpuTextureFormat::RGBA8,
            filter,
            wrap
        );
    }

    return std::shared_ptr<GpuTexture>(new GpuTexture(device, tex_id, pixmap.width(), pixmap.height(), GpuTextureFormat::RGBA8));
}

void GpuTexture::bind(uint32_t unit) {
    if (device_ && texture_id_ != 0) {
        device_->bind_texture(unit, texture_id_);
    }
}

void GpuTexture::update(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* pixels) {
    if (device_ && texture_id_ != 0 && pixels) {
        device_->bind_texture(0, texture_id_);
        device_->update_texture_sub_image_2d(x, y, width, height, pixels);
    }
}

} // namespace nisaba::gpu
