#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <span>
#include <limits>
#include "nisaba/types.hpp"
#include "nisaba/math/size.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/pipeline/simd.hpp"

namespace nisaba {

/// Pixel format representing pixel layout in memory.
enum class PixelFormat : uint8_t {
    RGBA8888 = 0, ///< 32-bit RGBA premultiplied (Default)
    BGRA8888 = 1, ///< 32-bit BGRA premultiplied (Native Windows DIB / Wayland / DRM)
    RGB565   = 2, ///< 16-bit RGB 5-6-5 (Embedded MCU / low-RAM displays)
    Alpha8   = 3  ///< 8-bit Alpha/Grayscale mask (Monochrome / text masks)
};

inline constexpr size_t bytes_per_pixel(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::RGBA8888:
        case PixelFormat::BGRA8888: return 4;
        case PixelFormat::RGB565:   return 2;
        case PixelFormat::Alpha8:   return 1;
    }
    return 4;
}

inline constexpr size_t BYTES_PER_PIXEL = 4;

inline std::optional<size_t> min_row_bytes(IntSize size, PixelFormat format = PixelFormat::RGBA8888) noexcept {
    size_t bpp = bytes_per_pixel(format);
    if (size.width() > static_cast<uint32_t>(std::numeric_limits<int32_t>::max() / 4)) {
        return std::nullopt;
    }
    size_t w = static_cast<size_t>(size.width()) * bpp;
    return w;
}

inline std::optional<size_t> compute_data_len(IntSize size, size_t row_bytes, PixelFormat format = PixelFormat::RGBA8888) noexcept {
    if (size.height() == 0) return std::nullopt;
    size_t h_sub1 = static_cast<size_t>(size.height() - 1);
    if (h_sub1 > 0 && row_bytes > std::numeric_limits<size_t>::max() / h_sub1) {
        return std::nullopt;
    }
    size_t h_bytes = h_sub1 * row_bytes;
    size_t w_bytes = static_cast<size_t>(size.width()) * bytes_per_pixel(format);
    if (h_bytes > std::numeric_limits<size_t>::max() - w_bytes) {
        return std::nullopt;
    }
    return h_bytes + w_bytes;
}

inline std::optional<size_t> data_len_for_size(IntSize size, PixelFormat format = PixelFormat::RGBA8888) noexcept {
    auto row_bytes = min_row_bytes(size, format);
    if (!row_bytes) return std::nullopt;
    return compute_data_len(size, *row_bytes, format);
}

class Pixmap;
class PixmapRef;
class PixmapMut;

/// A sub-region of a PixmapMut.
/// Contains `stride_bytes` for arbitrary row pitch (e.g. DRM/KMS dumb buffers, Windows DIBs).
struct SubPixmapMut {
    uint8_t* data{nullptr};
    IntSize size;
    size_t real_width{0};
    size_t stride_bytes{0};
    PixelFormat format{PixelFormat::RGBA8888};

    [[nodiscard]] size_t stride() const noexcept {
        return stride_bytes != 0 ? stride_bytes : real_width * bytes_per_pixel(format);
    }

    [[nodiscard]] uint8_t* row_raw(size_t y) noexcept {
        return data + y * stride();
    }

    [[nodiscard]] const uint8_t* row_raw(size_t y) const noexcept {
        return data + y * stride();
    }

    [[nodiscard]] PremultipliedColorU8* row(size_t y) noexcept {
        return reinterpret_cast<PremultipliedColorU8*>(row_raw(y));
    }

    [[nodiscard]] const PremultipliedColorU8* row(size_t y) const noexcept {
        return reinterpret_cast<const PremultipliedColorU8*>(row_raw(y));
    }

    [[nodiscard]] uint16_t* row_u16(size_t y) noexcept {
        return reinterpret_cast<uint16_t*>(row_raw(y));
    }

    [[nodiscard]] const uint16_t* row_u16(size_t y) const noexcept {
        return reinterpret_cast<const uint16_t*>(row_raw(y));
    }

    [[nodiscard]] uint8_t* row_u8(size_t y) noexcept {
        return row_raw(y);
    }

    [[nodiscard]] const uint8_t* row_u8(size_t y) const noexcept {
        return row_raw(y);
    }

    [[nodiscard]] PremultipliedColorU8* pixels_mut() noexcept {
        return reinterpret_cast<PremultipliedColorU8*>(data);
    }

    [[nodiscard]] PremultipliedColorU8 pixel(size_t x, size_t y) const noexcept {
        switch (format) {
            case PixelFormat::RGBA8888:
                return row(y)[x];
            case PixelFormat::BGRA8888: {
                const auto* p = row_raw(y) + x * 4;
                return PremultipliedColorU8::from_rgba_unchecked(p[2], p[1], p[0], p[3]);
            }
            case PixelFormat::RGB565:
                return PremultipliedColorU8::from_rgb565(row_u16(y)[x]);
            case PixelFormat::Alpha8: {
                uint8_t a = row_u8(y)[x];
                return PremultipliedColorU8::from_rgba_unchecked(a, a, a, a);
            }
        }
        return PremultipliedColorU8::TRANSPARENT;
    }

    void set_pixel(size_t x, size_t y, PremultipliedColorU8 c) noexcept {
        switch (format) {
            case PixelFormat::RGBA8888:
                row(y)[x] = c;
                break;
            case PixelFormat::BGRA8888: {
                auto* p = row_raw(y) + x * 4;
                p[0] = c.blue();
                p[1] = c.green();
                p[2] = c.red();
                p[3] = c.alpha();
                break;
            }
            case PixelFormat::RGB565:
                row_u16(y)[x] = c.to_rgb565();
                break;
            case PixelFormat::Alpha8:
                row_u8(y)[x] = c.alpha();
                break;
        }
    }
};

/// A container that references pixels. Non-owning read-only view.
class PixmapRef {
public:
    constexpr PixmapRef() noexcept = default;
    constexpr PixmapRef(
        const uint8_t* data,
        size_t data_len,
        IntSize size,
        PixelFormat format = PixelFormat::RGBA8888,
        bool is_opaque = false,
        size_t stride_bytes = 0
    ) noexcept
        : data_(data),
          data_len_(data_len),
          size_(size),
          stride_bytes_(stride_bytes != 0 ? stride_bytes : static_cast<size_t>(size.width()) * nisaba::bytes_per_pixel(format)),
          format_(format),
          is_opaque_(is_opaque) {}

    [[nodiscard]] constexpr bool is_opaque() const noexcept {
        return format_ == PixelFormat::RGB565 || is_opaque_;
    }

    static std::optional<PixmapRef> from_bytes(
        const uint8_t* data,
        size_t len,
        uint32_t width,
        uint32_t height,
        PixelFormat format = PixelFormat::RGBA8888
    ) noexcept;

    static std::optional<PixmapRef> from_raw_parts(
        const uint8_t* data,
        uint32_t width,
        uint32_t height,
        size_t stride_bytes = 0,
        PixelFormat format = PixelFormat::RGBA8888,
        bool is_opaque = false
    ) noexcept;

    Pixmap to_owned() const;
    Pixmap to_rgba8888() const;

    constexpr uint32_t width() const noexcept { return size_.width(); }
    constexpr uint32_t height() const noexcept { return size_.height(); }
    constexpr IntSize size() const noexcept { return size_; }
    constexpr PixelFormat format() const noexcept { return format_; }
    constexpr size_t bytes_per_pixel() const noexcept { return nisaba::bytes_per_pixel(format_); }
    constexpr size_t stride_bytes() const noexcept {
        return stride_bytes_ != 0 ? stride_bytes_ : static_cast<size_t>(size_.width()) * bytes_per_pixel();
    }

    ScreenIntRect rect() const noexcept {
        return to_screen_int_rect(size_, 0, 0);
    }

    constexpr const uint8_t* data() const noexcept { return data_; }
    constexpr size_t data_len() const noexcept { return data_len_; }

    std::optional<PremultipliedColorU8> pixel(uint32_t x, uint32_t y) const noexcept {
        if (x >= width() || y >= height()) return std::nullopt;
        switch (format_) {
            case PixelFormat::RGBA8888:
                return row(y)[x];
            case PixelFormat::BGRA8888: {
                const auto* p = row_raw(y) + x * 4;
                return PremultipliedColorU8::from_rgba_unchecked(p[2], p[1], p[0], p[3]);
            }
            case PixelFormat::RGB565:
                return PremultipliedColorU8::from_rgb565(row_u16(y)[x]);
            case PixelFormat::Alpha8: {
                uint8_t a = row_u8(y)[x];
                return PremultipliedColorU8::from_rgba_unchecked(a, a, a, a);
            }
        }
        return std::nullopt;
    }

    const PremultipliedColorU8* pixels() const noexcept {
        return reinterpret_cast<const PremultipliedColorU8*>(data_);
    }

    const PremultipliedColorU8* row(size_t y) const noexcept {
        return reinterpret_cast<const PremultipliedColorU8*>(data_ + y * stride_bytes());
    }

    const uint16_t* row_u16(size_t y) const noexcept {
        return reinterpret_cast<const uint16_t*>(data_ + y * stride_bytes());
    }

    const uint8_t* row_u8(size_t y) const noexcept {
        return data_ + y * stride_bytes();
    }

    const uint8_t* row_raw(size_t y) const noexcept {
        return data_ + y * stride_bytes();
    }

    constexpr bool is_empty() const noexcept {
        return width() == 0 || height() == 0;
    }

    std::optional<Pixmap> clone_rect(IntRect rect) const;

    bool save_ppm(const std::string& path) const;
    bool save_bmp(const std::string& path) const;
    bool save_png(const std::string& path, int compression_level = 6) const;
    bool save_jpeg(const std::string& path, int quality = 90) const;
    bool save_qoi(const std::string& path, bool linear_colorspace = false) const;
    bool save_webp(const std::string& path, bool lossless = true, int quality = 80) const;
    bool save_image(const std::string& path) const;

private:
    const uint8_t* data_{nullptr};
    size_t data_len_{0};
    IntSize size_;
    size_t stride_bytes_{0};
    PixelFormat format_{PixelFormat::RGBA8888};
    bool is_opaque_{false};
};

/// A container that references mutable pixels.
/// Non-owning mutable view supporting arbitrary row strides (DRM/KMS, X11/Wayland, Windows DIB).
class PixmapMut {
public:
    constexpr PixmapMut() noexcept = default;
    constexpr PixmapMut(
        uint8_t* data,
        size_t data_len,
        IntSize size,
        size_t stride_bytes = 0,
        PixelFormat format = PixelFormat::RGBA8888
    ) noexcept
        : data_(data), data_len_(data_len), size_(size),
          stride_bytes_(stride_bytes != 0 ? stride_bytes : static_cast<size_t>(size.width()) * nisaba::bytes_per_pixel(format)),
          format_(format) {}

    static std::optional<PixmapMut> from_bytes(
        uint8_t* data,
        size_t len,
        uint32_t width,
        uint32_t height,
        PixelFormat format = PixelFormat::RGBA8888
    ) noexcept;

    static std::optional<PixmapMut> from_raw_parts(
        uint8_t* data,
        uint32_t width,
        uint32_t height,
        size_t stride_bytes = 0,
        PixelFormat format = PixelFormat::RGBA8888
    ) noexcept;

    Pixmap to_owned() const;
    Pixmap to_rgba8888() const;

    bool is_opaque() const noexcept {
        if (format_ == PixelFormat::RGB565) return true;
        if (format_ == PixelFormat::Alpha8) return false;
        if (is_opaque_cache_.has_value()) return *is_opaque_cache_;
        size_t min_stride = static_cast<size_t>(width()) * bytes_per_pixel();
        bool op = true;
        if (stride_bytes_ == min_stride) {
            op = simd::is_buffer_opaque(pixels(), static_cast<size_t>(width()) * static_cast<size_t>(height()));
        } else {
            for (uint32_t y = 0; y < height(); ++y) {
                if (!simd::is_buffer_opaque(row(y), width())) {
                    op = false;
                    break;
                }
            }
        }
        is_opaque_cache_ = op;
        return op;
    }

    PixmapRef as_ref() const noexcept {
        return PixmapRef(data_, data_len_, size_, format_, is_opaque(), stride_bytes_);
    }

    constexpr uint32_t width() const noexcept { return size_.width(); }
    constexpr uint32_t height() const noexcept { return size_.height(); }
    constexpr IntSize size() const noexcept { return size_; }
    constexpr size_t stride_bytes() const noexcept { return stride_bytes_; }
    constexpr PixelFormat format() const noexcept { return format_; }
    constexpr size_t bytes_per_pixel() const noexcept { return nisaba::bytes_per_pixel(format_); }

    void fill(Color color) noexcept;
    void swap_rb() noexcept;

    constexpr uint8_t* data_mut() noexcept { return data_; }
    constexpr const uint8_t* data() const noexcept { return data_; }
    constexpr size_t data_len() const noexcept { return data_len_; }

    uint8_t* row_raw(size_t y) noexcept {
        return data_ + y * stride_bytes_;
    }

    const uint8_t* row_raw(size_t y) const noexcept {
        return data_ + y * stride_bytes_;
    }

    PremultipliedColorU8* row(size_t y) noexcept {
        return reinterpret_cast<PremultipliedColorU8*>(row_raw(y));
    }

    const PremultipliedColorU8* row(size_t y) const noexcept {
        return reinterpret_cast<const PremultipliedColorU8*>(row_raw(y));
    }

    uint16_t* row_u16(size_t y) noexcept {
        return reinterpret_cast<uint16_t*>(row_raw(y));
    }

    const uint16_t* row_u16(size_t y) const noexcept {
        return reinterpret_cast<const uint16_t*>(row_raw(y));
    }

    uint8_t* row_u8(size_t y) noexcept {
        return row_raw(y);
    }

    const uint8_t* row_u8(size_t y) const noexcept {
        return row_raw(y);
    }

    PremultipliedColorU8* pixels_mut() noexcept {
        return reinterpret_cast<PremultipliedColorU8*>(data_);
    }

    const PremultipliedColorU8* pixels() const noexcept {
        return reinterpret_cast<const PremultipliedColorU8*>(data_);
    }

    std::optional<PremultipliedColorU8> pixel(uint32_t x, uint32_t y) const noexcept {
        if (x >= width() || y >= height()) return std::nullopt;
        switch (format_) {
            case PixelFormat::RGBA8888:
                return row(y)[x];
            case PixelFormat::BGRA8888: {
                const auto* p = row_raw(y) + x * 4;
                return PremultipliedColorU8::from_rgba_unchecked(p[2], p[1], p[0], p[3]);
            }
            case PixelFormat::RGB565:
                return PremultipliedColorU8::from_rgb565(row_u16(y)[x]);
            case PixelFormat::Alpha8: {
                uint8_t a = row_u8(y)[x];
                return PremultipliedColorU8::from_rgba_unchecked(a, a, a, a);
            }
        }
        return std::nullopt;
    }

    bool set_pixel(uint32_t x, uint32_t y, PremultipliedColorU8 color) noexcept {
        if (x >= width() || y >= height()) return false;
        switch (format_) {
            case PixelFormat::RGBA8888:
                row(y)[x] = color;
                break;
            case PixelFormat::BGRA8888: {
                auto* p = row_raw(y) + x * 4;
                p[0] = color.blue();
                p[1] = color.green();
                p[2] = color.red();
                p[3] = color.alpha();
                break;
            }
            case PixelFormat::RGB565:
                row_u16(y)[x] = color.to_rgb565();
                break;
            case PixelFormat::Alpha8:
                row_u8(y)[x] = color.alpha();
                break;
        }
        return true;
    }

    SubPixmapMut as_subpixmap() noexcept {
        return SubPixmapMut{
            .data = data_,
            .size = size_,
            .real_width = static_cast<size_t>(width()),
            .stride_bytes = stride_bytes_,
            .format = format_
        };
    }

    std::optional<SubPixmapMut> subpixmap(IntRect rect) noexcept;

private:
    uint8_t* data_{nullptr};
    size_t data_len_{0};
    IntSize size_;
    size_t stride_bytes_{0};
    PixelFormat format_{PixelFormat::RGBA8888};
    mutable std::optional<bool> is_opaque_cache_{};
};

/// An owning container of pixels.
class Pixmap {
public:
    Pixmap() = default;

    static std::optional<Pixmap> allocate(uint32_t width, uint32_t height, PixelFormat format = PixelFormat::RGBA8888);
    static std::optional<Pixmap> create(uint32_t width, uint32_t height, PixelFormat format = PixelFormat::RGBA8888) {
        return allocate(width, height, format);
    }
    static std::optional<Pixmap> from_vec(std::vector<uint8_t> data, IntSize size, PixelFormat format = PixelFormat::RGBA8888);

    /// Load image from file (PNG, JPEG, BMP supported).
    static std::optional<Pixmap> load_file(const std::string& path);

    /// Load image from memory bytes (PNG, JPEG, BMP supported).
    static std::optional<Pixmap> load_from_memory(std::span<const uint8_t> data);

    /// Load PNG image from file.
    static std::optional<Pixmap> load_png(const std::string& path);

    /// Load JPEG image from file.
    static std::optional<Pixmap> load_jpeg(const std::string& path);

    /// Load QOI image from file.
    static std::optional<Pixmap> load_qoi(const std::string& path);

    /// Load WebP image from file.
    static std::optional<Pixmap> load_webp(const std::string& path);

    PixmapRef as_ref() const noexcept {
        return PixmapRef(data_.data(), data_.size(), size_, format_, is_opaque());
    }

    [[nodiscard]] bool is_opaque() const noexcept;
    void set_opaque(bool opaque) noexcept { is_opaque_cache_ = opaque; }

    PixmapMut as_mut() noexcept {
        return PixmapMut(data_.data(), data_.size(), size_, 0, format_);
    }

    uint32_t width() const noexcept { return size_.width(); }
    uint32_t height() const noexcept { return size_.height(); }
    IntSize size() const noexcept { return size_; }
    PixelFormat format() const noexcept { return format_; }
    size_t bytes_per_pixel() const noexcept { return nisaba::bytes_per_pixel(format_); }
    size_t stride_bytes() const noexcept { return static_cast<size_t>(width()) * bytes_per_pixel(); }

    void fill(Color color) noexcept {
        as_mut().fill(color);
    }

    const uint8_t* data() const noexcept { return data_.data(); }
    uint8_t* data_mut() noexcept { return data_.data(); }
    size_t data_len() const noexcept { return data_.size(); }

    const PremultipliedColorU8* pixels() const noexcept {
        return reinterpret_cast<const PremultipliedColorU8*>(data_.data());
    }

    PremultipliedColorU8* pixels_mut() noexcept {
        return reinterpret_cast<PremultipliedColorU8*>(data_.data());
    }

    std::optional<PremultipliedColorU8> pixel(uint32_t x, uint32_t y) const noexcept {
        return as_ref().pixel(x, y);
    }

    bool set_pixel(uint32_t x, uint32_t y, PremultipliedColorU8 color) noexcept {
        return as_mut().set_pixel(x, y, color);
    }

    std::vector<uint8_t> take() && noexcept {
        return std::move(data_);
    }

    std::vector<uint8_t> take_demultiplied() &&;

    std::optional<Pixmap> clone_rect(IntRect rect) const {
        return as_ref().clone_rect(rect);
    }

    Pixmap to_rgba8888() const {
        return as_ref().to_rgba8888();
    }

    bool save_ppm(const std::string& path) const {
        return as_ref().save_ppm(path);
    }

    bool save_bmp(const std::string& path) const {
        return as_ref().save_bmp(path);
    }

    bool save_png(const std::string& path, int compression_level = 6) const {
        return as_ref().save_png(path, compression_level);
    }

    bool save_jpeg(const std::string& path, int quality = 90) const {
        return as_ref().save_jpeg(path, quality);
    }

    bool save_qoi(const std::string& path, bool linear_colorspace = false) const {
        return as_ref().save_qoi(path, linear_colorspace);
    }

    bool save_webp(const std::string& path, bool lossless = true, int quality = 80) const {
        return as_ref().save_webp(path, lossless, quality);
    }

    bool save_image(const std::string& path) const {
        return as_ref().save_image(path);
    }

private:
    Pixmap(std::vector<uint8_t> data, IntSize size, PixelFormat format = PixelFormat::RGBA8888)
        : data_(std::move(data)), size_(size), format_(format) {}

    std::vector<uint8_t> data_;
    IntSize size_;
    PixelFormat format_{PixelFormat::RGBA8888};
    mutable std::optional<bool> is_opaque_cache_{};
    friend class PixmapRef;
    friend class PixmapMut;
};

} // namespace nisaba
