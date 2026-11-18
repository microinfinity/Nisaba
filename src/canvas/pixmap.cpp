#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/pipeline/simd.hpp"
#include "nisaba/image/image_io.hpp"
#include <fstream>
#include <cstring>

namespace nisaba {

std::optional<PixmapRef> PixmapRef::from_bytes(
    const uint8_t* data,
    size_t len,
    uint32_t width,
    uint32_t height,
    PixelFormat format
) noexcept {
    if (!data) return std::nullopt;
    auto size = IntSize::from_wh(width, height);
    if (!size) return std::nullopt;
    auto req_len = data_len_for_size(*size, format);
    if (!req_len || len < *req_len) return std::nullopt;

    return PixmapRef(data, len, *size, format);
}

std::optional<PixmapRef> PixmapRef::from_raw_parts(
    const uint8_t* data,
    uint32_t width,
    uint32_t height,
    size_t stride_bytes,
    PixelFormat format,
    bool is_opaque
) noexcept {
    if (!data) return std::nullopt;
    auto size = IntSize::from_wh(width, height);
    if (!size) return std::nullopt;
    size_t min_stride = static_cast<size_t>(width) * nisaba::bytes_per_pixel(format);
    size_t actual_stride = (stride_bytes != 0) ? stride_bytes : min_stride;
    if (actual_stride < min_stride) return std::nullopt;

    auto req_len = compute_data_len(*size, actual_stride, format);
    if (!req_len) return std::nullopt;

    return PixmapRef(data, *req_len, *size, format, is_opaque, actual_stride);
}

Pixmap PixmapRef::to_owned() const {
    size_t tight_stride = static_cast<size_t>(width()) * bytes_per_pixel();
    size_t len = data_len_for_size(size_, format_).value_or(0);
    if (len == 0 || !data_) return Pixmap();
    std::vector<uint8_t> vec(len);
    if (stride_bytes() == tight_stride) {
        std::memcpy(vec.data(), data_, len);
    } else {
        for (uint32_t y = 0; y < height(); ++y) {
            std::memcpy(vec.data() + static_cast<size_t>(y) * tight_stride, row_raw(y), tight_stride);
        }
    }
    return Pixmap(std::move(vec), size_, format_);
}

Pixmap PixmapRef::to_rgba8888() const {
    if (format_ == PixelFormat::RGBA8888) {
        return to_owned();
    }
    auto target_opt = Pixmap::allocate(width(), height(), PixelFormat::RGBA8888);
    if (!target_opt) return Pixmap();
    auto target = std::move(*target_opt);
    auto* dst = target.pixels_mut();

    switch (format_) {
        case PixelFormat::RGBA8888:
            break;
        case PixelFormat::BGRA8888: {
            for (size_t y = 0; y < height(); ++y) {
                const auto* src_row = data_ + y * stride_bytes();
                auto* dst_row = dst + y * width();
                for (size_t x = 0; x < width(); ++x) {
                    dst_row[x] = PremultipliedColorU8::from_rgba_unchecked(
                        src_row[x * 4 + 2],
                        src_row[x * 4 + 1],
                        src_row[x * 4 + 0],
                        src_row[x * 4 + 3]
                    );
                }
            }
            break;
        }
        case PixelFormat::RGB565: {
            for (size_t y = 0; y < height(); ++y) {
                const auto* src_row = row_u16(y);
                auto* dst_row = dst + y * width();
                for (size_t x = 0; x < width(); ++x) {
                    dst_row[x] = PremultipliedColorU8::from_rgb565(src_row[x]);
                }
            }
            break;
        }
        case PixelFormat::Alpha8: {
            for (size_t y = 0; y < height(); ++y) {
                const auto* src_row = row_u8(y);
                auto* dst_row = dst + y * width();
                for (size_t x = 0; x < width(); ++x) {
                    uint8_t a = src_row[x];
                    dst_row[x] = PremultipliedColorU8::from_rgba_unchecked(a, a, a, a);
                }
            }
            break;
        }
    }
    return target;
}

std::optional<Pixmap> PixmapRef::clone_rect(IntRect rect) const {
    auto isect = this->rect().to_int_rect().intersect(rect);
    if (!isect) return std::nullopt;

    auto new_pm = Pixmap::allocate(isect->width(), isect->height(), format_);
    if (!new_pm) return std::nullopt;

    uint32_t w = isect->width();
    uint32_t h = isect->height();
    int32_t rx = isect->x();
    int32_t ry = isect->y();
    size_t bpp = bytes_per_pixel();

    for (uint32_t y = 0; y < h; ++y) {
        size_t src_y = static_cast<size_t>(static_cast<int32_t>(y) + ry);
        const uint8_t* src_row = data_ + src_y * stride_bytes() + static_cast<size_t>(rx) * bpp;
        uint8_t* dst_row = new_pm->data_mut() + static_cast<size_t>(y) * (static_cast<size_t>(w) * bpp);
        std::memcpy(dst_row, src_row, static_cast<size_t>(w) * bpp);
    }

    return new_pm;
}

bool PixmapRef::save_ppm(const std::string& path) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_ppm(path);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    out << "P6\n" << width() << " " << height() << "\n255\n";

    size_t count = static_cast<size_t>(width()) * static_cast<size_t>(height());

    std::vector<uint8_t> rgb_buf(count * 3);
    size_t idx = 0;
    for (uint32_t y = 0; y < height(); ++y) {
        const auto* px = row(y);
        for (uint32_t x = 0; x < width(); ++x) {
            ColorU8 c = px[x].demultiply();
            rgb_buf[idx * 3 + 0] = c.red();
            rgb_buf[idx * 3 + 1] = c.green();
            rgb_buf[idx * 3 + 2] = c.blue();
            ++idx;
        }
    }

    out.write(reinterpret_cast<const char*>(rgb_buf.data()), rgb_buf.size());
    return out.good();
}

bool PixmapRef::save_bmp(const std::string& path) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_bmp(path);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    uint32_t w = width();
    uint32_t h = height();
    uint32_t image_size = w * h * 4;
    uint32_t file_size = 54 + image_size;

    // 14-byte BITMAPFILEHEADER
    uint8_t file_hdr[14] = {
        'B', 'M',
        static_cast<uint8_t>(file_size & 0xFF),
        static_cast<uint8_t>((file_size >> 8) & 0xFF),
        static_cast<uint8_t>((file_size >> 16) & 0xFF),
        static_cast<uint8_t>((file_size >> 24) & 0xFF),
        0, 0, 0, 0, // reserved
        54, 0, 0, 0  // offset to pixels
    };

    // 40-byte BITMAPINFOHEADER
    int32_t neg_h = -static_cast<int32_t>(h); // top-down
    uint8_t info_hdr[40] = {
        40, 0, 0, 0, // size of info header
        static_cast<uint8_t>(w & 0xFF),
        static_cast<uint8_t>((w >> 8) & 0xFF),
        static_cast<uint8_t>((w >> 16) & 0xFF),
        static_cast<uint8_t>((w >> 24) & 0xFF),
        static_cast<uint8_t>(neg_h & 0xFF),
        static_cast<uint8_t>((neg_h >> 8) & 0xFF),
        static_cast<uint8_t>((neg_h >> 16) & 0xFF),
        static_cast<uint8_t>((neg_h >> 24) & 0xFF),
        1, 0,       // 1 plane
        32, 0,      // 32 bits per pixel
        0, 0, 0, 0, // BI_RGB (uncompressed)
        static_cast<uint8_t>(image_size & 0xFF),
        static_cast<uint8_t>((image_size >> 8) & 0xFF),
        static_cast<uint8_t>((image_size >> 16) & 0xFF),
        static_cast<uint8_t>((image_size >> 24) & 0xFF),
        0, 0, 0, 0, // horizontal res
        0, 0, 0, 0, // vertical res
        0, 0, 0, 0, // colors used
        0, 0, 0, 0  // important colors
    };

    out.write(reinterpret_cast<const char*>(file_hdr), sizeof(file_hdr));
    out.write(reinterpret_cast<const char*>(info_hdr), sizeof(info_hdr));

    std::vector<uint8_t> bgra_buf(image_size);
    size_t idx = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const auto* px = row(y);
        for (uint32_t x = 0; x < w; ++x) {
            ColorU8 c = px[x].demultiply();
            bgra_buf[idx * 4 + 0] = c.blue();
            bgra_buf[idx * 4 + 1] = c.green();
            bgra_buf[idx * 4 + 2] = c.red();
            bgra_buf[idx * 4 + 3] = c.alpha();
            ++idx;
        }
    }

    out.write(reinterpret_cast<const char*>(bgra_buf.data()), bgra_buf.size());
    return out.good();
}

bool PixmapRef::save_png(const std::string& path, int compression_level) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_png(path, compression_level);
    }
    return image::save_png_file(*this, path, compression_level);
}

bool PixmapRef::save_jpeg(const std::string& path, int quality) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_jpeg(path, quality);
    }
    return image::save_jpeg_file(*this, path, quality);
}

bool PixmapRef::save_qoi(const std::string& path, bool linear_colorspace) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_qoi(path, linear_colorspace);
    }
    return image::save_qoi_file(*this, path, linear_colorspace);
}

bool PixmapRef::save_webp(const std::string& path, bool lossless, int quality) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_webp(path, lossless, quality);
    }
    return image::save_webp_file(*this, path, lossless, quality);
}

bool PixmapRef::save_image(const std::string& path) const {
    if (format_ != PixelFormat::RGBA8888) {
        return to_rgba8888().save_image(path);
    }
    return image::save_image_file(*this, path);
}

std::optional<PixmapMut> PixmapMut::from_bytes(
    uint8_t* data,
    size_t len,
    uint32_t width,
    uint32_t height,
    PixelFormat format
) noexcept {
    if (!data) return std::nullopt;
    auto size = IntSize::from_wh(width, height);
    if (!size) return std::nullopt;
    auto req_len = data_len_for_size(*size, format);
    if (!req_len || len < *req_len) return std::nullopt;

    return PixmapMut(data, len, *size, static_cast<size_t>(width) * nisaba::bytes_per_pixel(format), format);
}

std::optional<PixmapMut> PixmapMut::from_raw_parts(
    uint8_t* data,
    uint32_t width,
    uint32_t height,
    size_t stride_bytes,
    PixelFormat format
) noexcept {
    if (!data) return std::nullopt;
    auto size = IntSize::from_wh(width, height);
    if (!size) return std::nullopt;
    size_t min_stride = static_cast<size_t>(width) * nisaba::bytes_per_pixel(format);
    size_t actual_stride = (stride_bytes != 0) ? stride_bytes : min_stride;
    if (actual_stride < min_stride) return std::nullopt;

    size_t total_len = actual_stride * static_cast<size_t>(height);
    return PixmapMut(data, total_len, *size, actual_stride, format);
}

Pixmap PixmapMut::to_owned() const {
    return as_ref().to_owned();
}

Pixmap PixmapMut::to_rgba8888() const {
    return as_ref().to_rgba8888();
}

void PixmapMut::fill(Color color) noexcept {
    PremultipliedColorU8 c = color.premultiply().to_color_u8();
    switch (format_) {
        case PixelFormat::RGBA8888: {
            if (stride_bytes_ == width() * 4) {
                simd::fill_solid_span(row(0), c, static_cast<size_t>(width()) * height());
            } else {
                for (size_t y = 0; y < height(); ++y) {
                    simd::fill_solid_span(row(y), c, width());
                }
            }
            break;
        }
        case PixelFormat::BGRA8888: {
            PremultipliedColorU8 bgra = PremultipliedColorU8::from_rgba_unchecked(
                c.blue(), c.green(), c.red(), c.alpha()
            );
            if (stride_bytes_ == width() * 4) {
                simd::fill_solid_span(row(0), bgra, static_cast<size_t>(width()) * height());
            } else {
                for (size_t y = 0; y < height(); ++y) {
                    simd::fill_solid_span(row(y), bgra, width());
                }
            }
            break;
        }
        case PixelFormat::RGB565: {
            uint16_t c565 = c.to_rgb565();
            if (stride_bytes_ == width() * 2) {
                simd::fill_solid_span_rgb565(row_u16(0), c565, static_cast<size_t>(width()) * height());
            } else {
                for (size_t y = 0; y < height(); ++y) {
                    simd::fill_solid_span_rgb565(row_u16(y), c565, width());
                }
            }
            break;
        }
        case PixelFormat::Alpha8: {
            uint8_t a = c.alpha();
            if (stride_bytes_ == width()) {
                simd::fill_solid_span_alpha8(row_u8(0), a, static_cast<size_t>(width()) * height());
            } else {
                for (size_t y = 0; y < height(); ++y) {
                    simd::fill_solid_span_alpha8(row_u8(y), a, width());
                }
            }
            break;
        }
    }
}

void PixmapMut::swap_rb() noexcept {
    if (format_ == PixelFormat::RGBA8888 || format_ == PixelFormat::BGRA8888) {
        for (size_t y = 0; y < height(); ++y) {
            auto* line = row_raw(y);
            for (size_t x = 0; x < width(); ++x) {
                std::swap(line[x * 4 + 0], line[x * 4 + 2]);
            }
        }
    }
}

std::optional<SubPixmapMut> PixmapMut::subpixmap(IntRect rect) noexcept {
    auto isect = to_screen_int_rect(size_, 0, 0).to_int_rect().intersect(rect);
    if (!isect) return std::nullopt;

    size_t offset = static_cast<size_t>(isect->top()) * stride_bytes_ +
                    static_cast<size_t>(isect->left()) * bytes_per_pixel();

    return SubPixmapMut{
        .data = data_ + offset,
        .size = isect->size(),
        .real_width = static_cast<size_t>(width()),
        .stride_bytes = stride_bytes_,
        .format = format_
    };
}

std::optional<Pixmap> Pixmap::allocate(uint32_t width, uint32_t height, PixelFormat format) {
    auto size = IntSize::from_wh(width, height);
    if (!size) return std::nullopt;

    auto len = data_len_for_size(*size, format);
    if (!len) return std::nullopt;

    std::vector<uint8_t> data(*len, 0);
    return Pixmap(std::move(data), *size, format);
}

std::optional<Pixmap> Pixmap::from_vec(std::vector<uint8_t> data, IntSize size, PixelFormat format) {
    auto len = data_len_for_size(size, format);
    if (!len || data.size() != *len) return std::nullopt;

    return Pixmap(std::move(data), size, format);
}

std::vector<uint8_t> Pixmap::take_demultiplied() && {
    if (format_ == PixelFormat::RGBA8888 || format_ == PixelFormat::BGRA8888) {
        auto* px = pixels_mut();
        size_t count = static_cast<size_t>(width()) * static_cast<size_t>(height());
        for (size_t i = 0; i < count; ++i) {
            ColorU8 c = px[i].demultiply();
            px[i] = PremultipliedColorU8::from_rgba_unchecked(c.red(), c.green(), c.blue(), c.alpha());
        }
    }
    return std::move(data_);
}

std::optional<Pixmap> Pixmap::load_file(const std::string& path) {
    auto res = image::load_image_file(path);
    if (!res) return std::nullopt;
    return std::move(*res);
}

std::optional<Pixmap> Pixmap::load_from_memory(std::span<const uint8_t> data) {
    auto res = image::load_image_from_memory(data);
    if (!res) return std::nullopt;
    return std::move(*res);
}

std::optional<Pixmap> Pixmap::load_png(const std::string& path) {
    auto res = image::decode_png_file(path);
    if (!res) return std::nullopt;
    return std::move(*res);
}

std::optional<Pixmap> Pixmap::load_jpeg(const std::string& path) {
    auto res = image::decode_jpeg_file(path);
    if (!res) return std::nullopt;
    return std::move(*res);
}

std::optional<Pixmap> Pixmap::load_qoi(const std::string& path) {
    auto res = image::decode_qoi_file(path);
    if (!res) return std::nullopt;
    return std::move(*res);
}

std::optional<Pixmap> Pixmap::load_webp(const std::string& path) {
    auto res = image::decode_webp_file(path);
    if (!res) return std::nullopt;
    return std::move(*res);
}

bool Pixmap::is_opaque() const noexcept {
    if (format_ == PixelFormat::RGB565) return true;
    if (format_ == PixelFormat::Alpha8) return false;
    if (is_opaque_cache_.has_value()) return *is_opaque_cache_;
    bool op = simd::is_buffer_opaque(pixels(), static_cast<size_t>(width()) * static_cast<size_t>(height()));
    is_opaque_cache_ = op;
    return op;
}

} // namespace nisaba
