#include "nisaba/effects/blur.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace nisaba::effects {

std::array<int32_t, 3> compute_box_blur_radii(float sigma) noexcept {
    if (sigma <= 0.0f) {
        return {0, 0, 0};
    }

    float w_ideal = std::sqrt((12.0f * sigma * sigma / 3.0f) + 1.0f);
    int32_t wl = static_cast<int32_t>(std::floor(w_ideal));
    if (wl % 2 == 0) wl--;
    int32_t wu = wl + 2;

    float m_ideal = (12.0f * sigma * sigma - 3.0f * wl * wl - 12.0f * wl - 9.0f) / (-4.0f * wl - 4.0f);
    int32_t m = static_cast<int32_t>(std::round(m_ideal));

    std::array<int32_t, 3> radii{};
    for (int i = 0; i < 3; ++i) {
        int32_t w = (i < m) ? wl : wu;
        radii[i] = std::max(0, (w - 1) / 2);
    }
    return radii;
}

namespace {

void box_blur_1d_h_mask(const uint8_t* src, uint8_t* dst, int32_t w, int32_t h, int32_t r) noexcept {
    if (r <= 0 || w <= 0 || h <= 0) {
        if (src != dst) {
            std::copy_n(src, static_cast<size_t>(w) * h, dst);
        }
        return;
    }

    float inv_w = 1.0f / static_cast<float>(2 * r + 1);

    for (int32_t y = 0; y < h; ++y) {
        const uint8_t* in_row = src + y * w;
        uint8_t* out_row = dst + y * w;

        int32_t first_val = in_row[0];
        int32_t last_val = in_row[w - 1];

        int32_t acc = first_val * (r + 1);
        for (int32_t i = 1; i <= r; ++i) {
            acc += in_row[std::min(i, w - 1)];
        }

        for (int32_t x = 0; x < w; ++x) {
            out_row[x] = static_cast<uint8_t>(static_cast<float>(acc) * inv_w + 0.5f);

            int32_t outgoing = (x - r >= 0) ? in_row[x - r] : first_val;
            int32_t incoming = (x + r + 1 < w) ? in_row[x + r + 1] : last_val;

            acc += incoming - outgoing;
        }
    }
}

void box_blur_1d_v_mask(const uint8_t* src, uint8_t* dst, int32_t w, int32_t h, int32_t r) noexcept {
    if (r <= 0 || w <= 0 || h <= 0) {
        if (src != dst) {
            std::copy_n(src, static_cast<size_t>(w) * h, dst);
        }
        return;
    }

    float inv_w = 1.0f / static_cast<float>(2 * r + 1);

    for (int32_t x = 0; x < w; ++x) {
        int32_t first_val = src[x];
        int32_t last_val = src[(h - 1) * w + x];

        int32_t acc = first_val * (r + 1);
        for (int32_t i = 1; i <= r; ++i) {
            acc += src[std::min(i, h - 1) * w + x];
        }

        for (int32_t y = 0; y < h; ++y) {
            dst[y * w + x] = static_cast<uint8_t>(static_cast<float>(acc) * inv_w + 0.5f);

            int32_t outgoing = (y - r >= 0) ? src[(y - r) * w + x] : first_val;
            int32_t incoming = (y + r + 1 < h) ? src[(y + r + 1) * w + x] : last_val;

            acc += incoming - outgoing;
        }
    }
}

void box_blur_1d_h_pixmap(const PremultipliedColorU8* src, PremultipliedColorU8* dst, int32_t w, int32_t h, int32_t r) noexcept {
    if (r <= 0 || w <= 0 || h <= 0) {
        if (src != dst) {
            std::copy_n(src, static_cast<size_t>(w) * h, dst);
        }
        return;
    }

    float inv_w = 1.0f / static_cast<float>(2 * r + 1);

    for (int32_t y = 0; y < h; ++y) {
        const PremultipliedColorU8* in_row = src + y * w;
        PremultipliedColorU8* out_row = dst + y * w;

        PremultipliedColorU8 first_val = in_row[0];
        PremultipliedColorU8 last_val = in_row[w - 1];

        int32_t acc_r = first_val.r * (r + 1);
        int32_t acc_g = first_val.g * (r + 1);
        int32_t acc_b = first_val.b * (r + 1);
        int32_t acc_a = first_val.a * (r + 1);

        for (int32_t i = 1; i <= r; ++i) {
            const auto& p = in_row[std::min(i, w - 1)];
            acc_r += p.r;
            acc_g += p.g;
            acc_b += p.b;
            acc_a += p.a;
        }

        for (int32_t x = 0; x < w; ++x) {
            out_row[x] = PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>(static_cast<float>(acc_r) * inv_w + 0.5f),
                static_cast<uint8_t>(static_cast<float>(acc_g) * inv_w + 0.5f),
                static_cast<uint8_t>(static_cast<float>(acc_b) * inv_w + 0.5f),
                static_cast<uint8_t>(static_cast<float>(acc_a) * inv_w + 0.5f)
            );

            const auto& outgoing = (x - r >= 0) ? in_row[x - r] : first_val;
            const auto& incoming = (x + r + 1 < w) ? in_row[x + r + 1] : last_val;

            acc_r += incoming.r - outgoing.r;
            acc_g += incoming.g - outgoing.g;
            acc_b += incoming.b - outgoing.b;
            acc_a += incoming.a - outgoing.a;
        }
    }
}

void box_blur_1d_v_pixmap(const PremultipliedColorU8* src, PremultipliedColorU8* dst, int32_t w, int32_t h, int32_t r) noexcept {
    if (r <= 0 || w <= 0 || h <= 0) {
        if (src != dst) {
            std::copy_n(src, static_cast<size_t>(w) * h, dst);
        }
        return;
    }

    float inv_w = 1.0f / static_cast<float>(2 * r + 1);

    for (int32_t x = 0; x < w; ++x) {
        PremultipliedColorU8 first_val = src[x];
        PremultipliedColorU8 last_val = src[(h - 1) * w + x];

        int32_t acc_r = first_val.r * (r + 1);
        int32_t acc_g = first_val.g * (r + 1);
        int32_t acc_b = first_val.b * (r + 1);
        int32_t acc_a = first_val.a * (r + 1);

        for (int32_t i = 1; i <= r; ++i) {
            const auto& p = src[std::min(i, h - 1) * w + x];
            acc_r += p.r;
            acc_g += p.g;
            acc_b += p.b;
            acc_a += p.a;
        }

        for (int32_t y = 0; y < h; ++y) {
            dst[y * w + x] = PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>(static_cast<float>(acc_r) * inv_w + 0.5f),
                static_cast<uint8_t>(static_cast<float>(acc_g) * inv_w + 0.5f),
                static_cast<uint8_t>(static_cast<float>(acc_b) * inv_w + 0.5f),
                static_cast<uint8_t>(static_cast<float>(acc_a) * inv_w + 0.5f)
            );

            const auto& outgoing = (y - r >= 0) ? src[(y - r) * w + x] : first_val;
            const auto& incoming = (y + r + 1 < h) ? src[(y + r + 1) * w + x] : last_val;

            acc_r += incoming.r - outgoing.r;
            acc_g += incoming.g - outgoing.g;
            acc_b += incoming.b - outgoing.b;
            acc_a += incoming.a - outgoing.a;
        }
    }
}

} // namespace

void box_blur_mask(Mask& mask, int32_t radius) {
    if (radius <= 0) return;
    int32_t w = static_cast<int32_t>(mask.width());
    int32_t h = static_cast<int32_t>(mask.height());
    if (w <= 0 || h <= 0) return;

    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h);
    uint8_t* ptr = mask.data_mut();

    box_blur_1d_h_mask(ptr, tmp.data(), w, h, radius);
    box_blur_1d_v_mask(tmp.data(), ptr, w, h, radius);
}

void gaussian_blur_mask(Mask& mask, float sigma) {
    if (sigma <= 0.0f) return;
    auto radii = compute_box_blur_radii(sigma);
    int32_t w = static_cast<int32_t>(mask.width());
    int32_t h = static_cast<int32_t>(mask.height());
    if (w <= 0 || h <= 0) return;

    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h);
    uint8_t* ptr = mask.data_mut();

    for (int32_t r : radii) {
        if (r > 0) {
            box_blur_1d_h_mask(ptr, tmp.data(), w, h, r);
            box_blur_1d_v_mask(tmp.data(), ptr, w, h, r);
        }
    }
}

Mask blur_mask(const MaskRef& src, float sigma) {
    auto m = Mask::allocate(src.width(), src.height());
    if (!m) return Mask();

    int32_t w = static_cast<int32_t>(src.width());
    int32_t h = static_cast<int32_t>(src.height());

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            m->set(static_cast<uint32_t>(x), static_cast<uint32_t>(y), src.get(x, y));
        }
    }

    gaussian_blur_mask(*m, sigma);
    return std::move(*m);
}

void box_blur_pixmap(PixmapMut& pixmap, int32_t radius) {
    if (radius <= 0) return;
    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());
    if (w <= 0 || h <= 0) return;

    std::vector<PremultipliedColorU8> flat_src(static_cast<size_t>(w) * h);
    std::vector<PremultipliedColorU8> tmp(static_cast<size_t>(w) * h);

    for (int32_t y = 0; y < h; ++y) {
        const auto* row_in = pixmap.row(static_cast<size_t>(y));
        std::copy_n(row_in, static_cast<size_t>(w), flat_src.data() + y * w);
    }

    box_blur_1d_h_pixmap(flat_src.data(), tmp.data(), w, h, radius);
    box_blur_1d_v_pixmap(tmp.data(), flat_src.data(), w, h, radius);

    for (int32_t y = 0; y < h; ++y) {
        auto* row_out = pixmap.row(static_cast<size_t>(y));
        std::copy_n(flat_src.data() + y * w, static_cast<size_t>(w), row_out);
    }
}

void gaussian_blur_pixmap_xy(PixmapMut& pixmap, float sigma_x, float sigma_y) {
    if (sigma_x <= 0.0f && sigma_y <= 0.0f) return;
    auto radii_x = compute_box_blur_radii(sigma_x);
    auto radii_y = compute_box_blur_radii(sigma_y);
    int32_t w = static_cast<int32_t>(pixmap.width());
    int32_t h = static_cast<int32_t>(pixmap.height());
    if (w <= 0 || h <= 0) return;

    std::vector<PremultipliedColorU8> flat_src(static_cast<size_t>(w) * h);
    std::vector<PremultipliedColorU8> tmp(static_cast<size_t>(w) * h);

    for (int32_t y = 0; y < h; ++y) {
        const auto* row_in = pixmap.row(static_cast<size_t>(y));
        std::copy_n(row_in, static_cast<size_t>(w), flat_src.data() + y * w);
    }

    for (size_t i = 0; i < 3; ++i) {
        int32_t rx = radii_x[i];
        int32_t ry = radii_y[i];
        if (rx > 0 || ry > 0) {
            box_blur_1d_h_pixmap(flat_src.data(), tmp.data(), w, h, rx);
            box_blur_1d_v_pixmap(tmp.data(), flat_src.data(), w, h, ry);
        }
    }

    for (int32_t y = 0; y < h; ++y) {
        auto* row_out = pixmap.row(static_cast<size_t>(y));
        std::copy_n(flat_src.data() + y * w, static_cast<size_t>(w), row_out);
    }
}

void gaussian_blur_pixmap(PixmapMut& pixmap, float sigma) {
    gaussian_blur_pixmap_xy(pixmap, sigma, sigma);
}

Pixmap blur_pixmap(const PixmapRef& src, float sigma) {
    Pixmap copy = src.to_owned();
    auto mut = copy.as_mut();
    gaussian_blur_pixmap(mut, sigma);
    return copy;
}

} // namespace nisaba::effects
