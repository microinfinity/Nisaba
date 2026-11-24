#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <algorithm>
#include <cmath>
#include <span>
#include "nisaba/types.hpp"
#include "nisaba/math/size.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/raster/blitter.hpp"
#include "nisaba/pipeline/simd.hpp"

namespace nisaba {

class Shader;

enum class MaskType {
    Alpha,
    Luminance,
};

struct SubMaskRef {
    const uint8_t* data{nullptr};
    IntSize size{};
    uint32_t real_width{0};

    [[nodiscard]] constexpr uint32_t width() const noexcept { return size.width(); }
    [[nodiscard]] constexpr uint32_t height() const noexcept { return size.height(); }

    [[nodiscard]] constexpr uint8_t get(uint32_t x, uint32_t y) const noexcept {
        return data[y * real_width + x];
    }

    static std::optional<SubMaskRef> from_bytes(const uint8_t* data, size_t len, uint32_t width, uint32_t height) noexcept {
        if (!data || len < static_cast<size_t>(width) * height) return std::nullopt;
        auto sz = IntSize::from_wh(width, height);
        if (!sz) return std::nullopt;
        return SubMaskRef{ .data = data, .size = *sz, .real_width = width };
    }
};

using MaskRef = SubMaskRef;

struct SubMaskMut {
    uint8_t* data{nullptr};
    IntSize size{};
    uint32_t real_width{0};

    [[nodiscard]] constexpr uint32_t width() const noexcept { return size.width(); }
    [[nodiscard]] constexpr uint32_t height() const noexcept { return size.height(); }

    [[nodiscard]] constexpr uint8_t get(uint32_t x, uint32_t y) const noexcept {
        return data[y * real_width + x];
    }

    constexpr void set(uint32_t x, uint32_t y, uint8_t val) noexcept {
        data[y * real_width + x] = val;
    }
};

class Mask {
public:
    Mask() noexcept = default;

    static std::optional<Mask> create(uint32_t width, uint32_t height) {
        auto sz = IntSize::from_wh(width, height);
        if (!sz) return std::nullopt;

        Mask m;
        m.size_ = *sz;
        m.data_.assign(static_cast<size_t>(width) * height, 0);
        return m;
    }

    static std::optional<Mask> allocate(uint32_t width, uint32_t height) {
        return create(width, height);
    }

    static Mask from_pixmap(const PixmapRef& pixmap, MaskType type);

    static std::optional<Mask> from_vec(std::vector<uint8_t> data, IntSize size) {
        if (data.size() != static_cast<size_t>(size.width()) * size.height()) {
            return std::nullopt;
        }
        Mask m;
        m.data_ = std::move(data);
        m.size_ = size;
        return m;
    }

    [[nodiscard]] uint32_t width() const noexcept { return size_.width(); }
    [[nodiscard]] uint32_t height() const noexcept { return size_.height(); }
    [[nodiscard]] IntSize size() const noexcept { return size_; }

    [[nodiscard]] uint8_t get(uint32_t x, uint32_t y) const noexcept {
        if (x < width() && y < height()) {
            return data_[static_cast<size_t>(y) * width() + x];
        }
        return 0;
    }

    void set(uint32_t x, uint32_t y, uint8_t val) noexcept {
        if (x < width() && y < height()) {
            data_[static_cast<size_t>(y) * width() + x] = val;
        }
    }

    [[nodiscard]] const uint8_t* data() const noexcept { return data_.data(); }
    [[nodiscard]] uint8_t* data_mut() noexcept { return data_.data(); }
    [[nodiscard]] std::span<const uint8_t> span() const noexcept { return data_; }
    [[nodiscard]] std::span<uint8_t> span_mut() noexcept { return data_; }

    [[nodiscard]] std::vector<uint8_t> take_data() noexcept { return std::move(data_); }

    [[nodiscard]] SubMaskRef as_submask() const noexcept {
        return SubMaskRef{
            .data = data_.data(),
            .size = size_,
            .real_width = size_.width()
        };
    }

    [[nodiscard]] SubMaskMut as_submask_mut() noexcept {
        return SubMaskMut{
            .data = data_.data(),
            .size = size_,
            .real_width = size_.width()
        };
    }

    [[nodiscard]] std::optional<SubMaskRef> submask(const IntRect& rect) const noexcept {
        auto int_rect = IntRect::from_xywh(0, 0, size_.width(), size_.height());
        if (!int_rect) return std::nullopt;
        auto inter = int_rect->intersect(rect);
        if (!inter) return std::nullopt;

        size_t row_bytes = size_.width();
        size_t offset = static_cast<size_t>(inter->top()) * row_bytes + inter->left();

        return SubMaskRef{
            .data = data_.data() + offset,
            .size = inter->size(),
            .real_width = size_.width()
        };
    }

    [[nodiscard]] std::optional<SubMaskMut> submask_mut(const IntRect& rect) noexcept {
        auto int_rect = IntRect::from_xywh(0, 0, size_.width(), size_.height());
        if (!int_rect) return std::nullopt;
        auto inter = int_rect->intersect(rect);
        if (!inter) return std::nullopt;

        size_t row_bytes = size_.width();
        size_t offset = static_cast<size_t>(inter->top()) * row_bytes + inter->left();

        return SubMaskMut{
            .data = data_.data() + offset,
            .size = inter->size(),
            .real_width = size_.width()
        };
    }

    void fill_path(
        const Path& path,
        FillRule fill_rule = FillRule::Winding,
        bool anti_alias = true,
        Transform transform = Transform()
    );

    void intersect_path(
        const Path& path,
        FillRule fill_rule = FillRule::Winding,
        bool anti_alias = true,
        Transform transform = Transform(),
        std::optional<ScreenIntRect> conservative_clip = std::nullopt
    );

    void difference_path(
        const Path& path,
        FillRule fill_rule = FillRule::Winding,
        bool anti_alias = true,
        Transform transform = Transform(),
        std::optional<ScreenIntRect> conservative_clip = std::nullopt
    );

    void apply_shader(
        const Shader& shader,
        Transform transform = Transform(),
        bool is_difference = false,
        std::optional<ScreenIntRect> conservative_clip = std::nullopt
    );

    void fill_rect_value(const ScreenIntRect& rect, uint8_t value) noexcept {
        if (width() == 0 || height() == 0) return;
        const uint32_t x0 = std::min(rect.x(), width());
        const uint32_t y0 = std::min(rect.y(), height());
        const uint32_t x1 = std::min(rect.right(), width());
        const uint32_t y1 = std::min(rect.bottom(), height());
        if (x1 <= x0 || y1 <= y0) return;
        const size_t span_len = static_cast<size_t>(x1 - x0);
        for (uint32_t y = y0; y < y1; ++y) {
            std::memset(data_.data() + static_cast<size_t>(y) * width() + x0, value, span_len);
        }
    }

    void intersect_mask(const Mask& other) noexcept {
        size_t count = std::min(data_.size(), other.data_.size());
        simd::multiply_mask_spans(data_.data(), other.data_.data(), count);
    }

    void difference_mask(const Mask& other) noexcept {
        const size_t count = std::min(data_.size(), other.data_.size());
        for (size_t i = 0; i < count; ++i) {
            const uint32_t inv_s = 255u - static_cast<uint32_t>(other.data_[i]);
            data_[i] = static_cast<uint8_t>((static_cast<uint32_t>(data_[i]) * inv_s + 127u) / 255u);
        }
    }

    void invert() noexcept {
        for (auto& a : data_) {
            a = static_cast<uint8_t>(255 - a);
        }
    }

    void fill_circle(float cx, float cy, float radius) noexcept {
        if (radius <= 0.0f) return;
        float r_sq = radius * radius;
        int32_t y_start = std::max(0, static_cast<int32_t>(std::floor(cy - radius)));
        int32_t y_end = std::min(static_cast<int32_t>(height()), static_cast<int32_t>(std::ceil(cy + radius)));
        int32_t w_limit = static_cast<int32_t>(width());

        for (int32_t y = y_start; y < y_end; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            float dy = py - cy;
            float dy_sq = dy * dy;
            if (dy_sq >= r_sq) continue;

            float dx = std::sqrt(r_sq - dy_sq);
            float x0 = cx - dx;
            float x1 = cx + dx;

            int32_t ix0 = static_cast<int32_t>(std::floor(x0));
            int32_t ix1 = static_cast<int32_t>(std::floor(x1));
            uint8_t* row = data_.data() + y * width();

            if (ix0 == ix1) {
                if (ix0 >= 0 && ix0 < w_limit) {
                    float cov = x1 - x0;
                    row[ix0] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                }
                continue;
            }

            if (ix0 >= 0 && ix0 < w_limit) {
                float cov = (static_cast<float>(ix0 + 1) - x0);
                if (cov > 0.005f) {
                    row[ix0] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                }
            }

            int32_t span_start = std::max(0, ix0 + 1);
            int32_t span_end = std::min(w_limit, ix1);
            if (span_end > span_start) {
                std::memset(row + span_start, 255, span_end - span_start);
            }

            if (ix1 >= 0 && ix1 < w_limit) {
                float cov = (x1 - static_cast<float>(ix1));
                if (cov > 0.005f) {
                    row[ix1] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
                }
            }
        }
    }

    void clear() noexcept {
        if (!data_.empty()) {
            std::memset(data_.data(), 0, data_.size());
        }
    }

private:
    std::vector<uint8_t> data_{};
    IntSize size_{};
};

/// A blitter for rendering 8-bit coverage/alpha directly into a SubMaskMut.
class MaskBlitter final : public Blitter {
public:
    explicit MaskBlitter(SubMaskMut mask) noexcept : mask_(mask) {}

    void blit_h(uint32_t x, uint32_t y, LengthU32 width) override {
        if (y >= mask_.size.height()) return;
        uint32_t max_w = mask_.size.width() > x ? mask_.size.width() - x : 0;
        uint32_t w = std::min(width.get(), max_w);
        uint8_t* row = mask_.data + y * mask_.real_width + x;
        std::memset(row, 255, w);
    }

    void blit_anti_h(
        uint32_t x,
        uint32_t y,
        std::span<AlphaU8> antialias,
        std::span<AlphaRun> runs
    ) override {
        if (y >= mask_.size.height()) return;

        size_t aa_offset = 0;
        size_t run_offset = 0;
        AlphaRun run_opt = runs[0];

        while (run_opt.has_value()) {
            uint16_t run_len = *run_opt;
            LengthU32 width = LengthU32::create_unchecked(run_len);
            uint8_t aa = antialias[aa_offset];

            if (aa == 0) {
                // transparent, skip
            } else if (aa == 255) {
                blit_h(x, y, width);
            } else {
                uint32_t max_w = mask_.size.width() > x ? mask_.size.width() - x : 0;
                uint32_t w = std::min(width.get(), max_w);
                uint8_t* row = mask_.data + y * mask_.real_width + x;
                for (uint32_t i = 0; i < w; ++i) {
                    uint32_t cur = row[i];
                    uint32_t n = cur + ((255 - cur) * aa + 127) / 255;
                    row[i] = static_cast<uint8_t>(std::min(n, 255u));
                }
            }

            x += width.get();
            run_offset += run_len;
            aa_offset += run_len;
            run_opt = (run_offset < runs.size()) ? runs[run_offset] : std::nullopt;
        }
    }

    void blit_span_coverage(
        uint32_t x,
        uint32_t y,
        const uint8_t* coverage,
        uint32_t count
    ) override {
        if (y >= mask_.size.height() || x >= mask_.size.width() || count == 0) return;
        uint32_t w = std::min(count, mask_.size.width() - x);
        uint8_t* row = mask_.data + y * mask_.real_width + x;

        size_t i = 0;
        while (i < w) {
            uint8_t cov = coverage[i];
            if (cov == 0) {
#if defined(NISABA_HAS_AVX2)
                while (i + 32 <= w) {
                    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(coverage + i));
                    if (!_mm256_testz_si256(v, v)) break;
                    i += 32;
                }
#endif
                while (i + 8 <= w) {
                    uint64_t c64;
                    std::memcpy(&c64, coverage + i, sizeof(uint64_t));
                    if (c64 != 0) break;
                    i += 8;
                }
                while (i < w && coverage[i] == 0) ++i;
                continue;
            }
            if (cov == 255) {
                size_t start = i;
#if defined(NISABA_HAS_AVX2)
                __m256i ones = _mm256_set1_epi8(static_cast<char>(0xFF));
                while (i + 32 <= w) {
                    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(coverage + i));
                    __m256i cmp = _mm256_cmpeq_epi8(v, ones);
                    if (_mm256_movemask_epi8(cmp) != -1) break;
                    i += 32;
                }
#endif
                while (i + 8 <= w) {
                    uint64_t c64;
                    std::memcpy(&c64, coverage + i, sizeof(uint64_t));
                    if (c64 != 0xFFFFFFFFFFFFFFFFULL) break;
                    i += 8;
                }
                while (i < w && coverage[i] == 255) ++i;
                std::memset(row + start, 255, i - start);
                continue;
            }
            uint32_t cur = row[i];
            if (cur == 0) {
                row[i] = cov;
            } else {
                uint32_t n = cur + ((255 - cur) * cov + 127) / 255;
                row[i] = static_cast<uint8_t>(std::min(n, 255u));
            }
            ++i;
        }
    }

    void blit_v(uint32_t x, uint32_t y, LengthU32 height, AlphaU8 alpha) override {
        if (x >= mask_.size.width()) return;
        uint32_t max_h = mask_.size.height() > y ? mask_.size.height() - y : 0;
        uint32_t h = std::min(height.get(), max_h);
        uint8_t a = alpha;
        for (uint32_t i = 0; i < h; ++i) {
            uint8_t* ptr = mask_.data + (y + i) * mask_.real_width + x;
            uint32_t cur = *ptr;
            uint32_t n = cur + ((255 - cur) * a + 127) / 255;
            *ptr = static_cast<uint8_t>(std::min(n, 255u));
        }
    }

    void blit_anti_h2(uint32_t x, uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override {
        if (y >= mask_.size.height()) return;
        if (x < mask_.size.width()) {
            uint8_t* ptr = mask_.data + y * mask_.real_width + x;
            uint32_t cur = *ptr;
            *ptr = static_cast<uint8_t>(std::min(cur + ((255 - cur) * alpha0 + 127) / 255, 255u));
        }
        if (x + 1 < mask_.size.width()) {
            uint8_t* ptr = mask_.data + y * mask_.real_width + x + 1;
            uint32_t cur = *ptr;
            *ptr = static_cast<uint8_t>(std::min(cur + ((255 - cur) * alpha1 + 127) / 255, 255u));
        }
    }

    void blit_anti_v2(uint32_t x, uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) override {
        if (x >= mask_.size.width()) return;
        if (y < mask_.size.height()) {
            uint8_t* ptr = mask_.data + y * mask_.real_width + x;
            uint32_t cur = *ptr;
            *ptr = static_cast<uint8_t>(std::min(cur + ((255 - cur) * alpha0 + 127) / 255, 255u));
        }
        if (y + 1 < mask_.size.height()) {
            uint8_t* ptr = mask_.data + (y + 1) * mask_.real_width + x;
            uint32_t cur = *ptr;
            *ptr = static_cast<uint8_t>(std::min(cur + ((255 - cur) * alpha1 + 127) / 255, 255u));
        }
    }

    void blit_rect(const ScreenIntRect& rect) override {
        uint32_t y_end = rect.bottom();
        for (uint32_t y = rect.y(); y < y_end; ++y) {
            blit_h(rect.x(), y, rect.width_safe());
        }
    }

    void blit_mask(const MaskInfo& mask_info, const ScreenIntRect& clip) override {
        for (uint32_t y = clip.top(); y < clip.bottom(); ++y) {
            for (uint32_t x = clip.left(); x < clip.right(); ++x) {
                uint32_t mask_x = x - mask_info.bounds.left();
                uint32_t mask_y = y - mask_info.bounds.top();
                uint8_t ma = mask_info.image[mask_y * mask_info.row_bytes + mask_x];
                if (ma > 0 && x < mask_.size.width() && y < mask_.size.height()) {
                    uint8_t* ptr = mask_.data + y * mask_.real_width + x;
                    uint32_t cur = *ptr;
                    *ptr = static_cast<uint8_t>(std::min(cur + ((255 - cur) * ma + 127) / 255, 255u));
                }
            }
        }
    }

private:
    SubMaskMut mask_;
};

} // namespace nisaba
