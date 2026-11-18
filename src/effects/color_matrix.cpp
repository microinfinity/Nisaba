#include "nisaba/effects/color_matrix.hpp"
#include <cmath>
#include <algorithm>

namespace nisaba::effects {

ColorMatrix ColorMatrix::scale(float r, float g, float b, float a) noexcept {
    ColorMatrix res;
    res.m[0] = r;
    res.m[6] = g;
    res.m[12] = b;
    res.m[18] = a;
    return res;
}

ColorMatrix ColorMatrix::translate(float dr, float dg, float db, float da) noexcept {
    ColorMatrix res;
    res.m[4] = dr;
    res.m[9] = dg;
    res.m[14] = db;
    res.m[19] = da;
    return res;
}

ColorMatrix ColorMatrix::saturation(float s) noexcept {
    constexpr float lr = 0.2126f;
    constexpr float lg = 0.7152f;
    constexpr float lb = 0.0722f;

    float inv_s = 1.0f - s;

    ColorMatrix res;
    res.m[0] = inv_s * lr + s;
    res.m[1] = inv_s * lg;
    res.m[2] = inv_s * lb;
    res.m[3] = 0.0f;
    res.m[4] = 0.0f;

    res.m[5] = inv_s * lr;
    res.m[6] = inv_s * lg + s;
    res.m[7] = inv_s * lb;
    res.m[8] = 0.0f;
    res.m[9] = 0.0f;

    res.m[10] = inv_s * lr;
    res.m[11] = inv_s * lg;
    res.m[12] = inv_s * lb + s;
    res.m[13] = 0.0f;
    res.m[14] = 0.0f;

    res.m[15] = 0.0f;
    res.m[16] = 0.0f;
    res.m[17] = 0.0f;
    res.m[18] = 1.0f;
    res.m[19] = 0.0f;

    return res;
}

ColorMatrix ColorMatrix::hue_rotate(float degrees) noexcept {
    float rad = degrees * (static_cast<float>(M_PI) / 180.0f);
    float c = std::cos(rad);
    float s = std::sin(rad);

    ColorMatrix res;
    // Row 0 (R)
    res.m[0] = 0.213f + c * 0.787f - s * 0.213f;
    res.m[1] = 0.715f - c * 0.715f - s * 0.715f;
    res.m[2] = 0.072f - c * 0.072f + s * 0.928f;
    res.m[3] = 0.0f;
    res.m[4] = 0.0f;

    // Row 1 (G)
    res.m[5] = 0.213f - c * 0.213f + s * 0.143f;
    res.m[6] = 0.715f + c * 0.285f + s * 0.140f;
    res.m[7] = 0.072f - c * 0.072f - s * 0.283f;
    res.m[8] = 0.0f;
    res.m[9] = 0.0f;

    // Row 2 (B)
    res.m[10] = 0.213f - c * 0.213f - s * 0.787f;
    res.m[11] = 0.715f - c * 0.715f + s * 0.715f;
    res.m[12] = 0.072f + c * 0.928f + s * 0.072f;
    res.m[13] = 0.0f;
    res.m[14] = 0.0f;

    // Row 3 (A)
    res.m[15] = 0.0f;
    res.m[16] = 0.0f;
    res.m[17] = 0.0f;
    res.m[18] = 1.0f;
    res.m[19] = 0.0f;

    return res;
}

ColorMatrix ColorMatrix::brightness(float factor) noexcept {
    return translate(factor, factor, factor, 0.0f);
}

ColorMatrix ColorMatrix::contrast(float factor) noexcept {
    float offset = 0.5f * (1.0f - factor);
    ColorMatrix res;
    res.m[0] = factor;
    res.m[4] = offset;

    res.m[6] = factor;
    res.m[9] = offset;

    res.m[12] = factor;
    res.m[14] = offset;

    res.m[18] = 1.0f;
    return res;
}

ColorMatrix ColorMatrix::sepia(float intensity) noexcept {
    float t = std::clamp(intensity, 0.0f, 1.0f);
    float inv_t = 1.0f - t;

    ColorMatrix res;
    res.m[0] = inv_t + t * 0.393f;
    res.m[1] = t * 0.769f;
    res.m[2] = t * 0.189f;

    res.m[5] = t * 0.349f;
    res.m[6] = inv_t + t * 0.686f;
    res.m[7] = t * 0.168f;

    res.m[10] = t * 0.272f;
    res.m[11] = t * 0.534f;
    res.m[12] = inv_t + t * 0.131f;

    res.m[18] = 1.0f;
    return res;
}

ColorMatrix ColorMatrix::invert() noexcept {
    ColorMatrix res;
    res.m[0] = -1.0f;
    res.m[4] = 1.0f;

    res.m[6] = -1.0f;
    res.m[9] = 1.0f;

    res.m[12] = -1.0f;
    res.m[14] = 1.0f;

    res.m[18] = 1.0f;
    return res;
}

ColorMatrix ColorMatrix::multiply(const ColorMatrix& other) const noexcept {
    ColorMatrix res;
    for (int r = 0; r < 4; ++r) {
        int r5 = r * 5;
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += m[r5 + k] * other.m[k * 5 + c];
            }
            res.m[r5 + c] = sum;
        }
        // 5th column: includes translation from other plus own translation
        float trans = m[r5 + 4];
        for (int k = 0; k < 4; ++k) {
            trans += m[r5 + k] * other.m[k * 5 + 4];
        }
        res.m[r5 + 4] = trans;
    }
    return res;
}

Color ColorMatrix::transform(Color c) const noexcept {
    float r = c.red();
    float g = c.green();
    float b = c.blue();
    float a = c.alpha();

    float nr = m[0] * r + m[1] * g + m[2] * b + m[3] * a + m[4];
    float ng = m[5] * r + m[6] * g + m[7] * b + m[8] * a + m[9];
    float nb = m[10] * r + m[11] * g + m[12] * b + m[13] * a + m[14];
    float na = m[15] * r + m[16] * g + m[17] * b + m[18] * a + m[19];

    return Color::from_rgba_unchecked(
        std::clamp(nr, 0.0f, 1.0f),
        std::clamp(ng, 0.0f, 1.0f),
        std::clamp(nb, 0.0f, 1.0f),
        std::clamp(na, 0.0f, 1.0f)
    );
}

PremultipliedColor ColorMatrix::transform_premul(PremultipliedColor c) const noexcept {
    Color demul = c.demultiply();
    Color trans = transform(demul);
    return trans.premultiply();
}

PremultipliedColorU8 ColorMatrix::transform_pixel(PremultipliedColorU8 p) const noexcept {
    uint8_t a = p.alpha();
    if (a == 0) {
        if (m[4] == 0.0f && m[9] == 0.0f && m[14] == 0.0f && m[19] == 0.0f) {
            return PremultipliedColorU8::TRANSPARENT;
        }
    }

    constexpr float inv_255 = 1.0f / 255.0f;
    float fa = static_cast<float>(a) * inv_255;
    float fr = (fa > 1e-5f) ? (static_cast<float>(p.red()) * inv_255 / fa) : 0.0f;
    float fg = (fa > 1e-5f) ? (static_cast<float>(p.green()) * inv_255 / fa) : 0.0f;
    float fb = (fa > 1e-5f) ? (static_cast<float>(p.blue()) * inv_255 / fa) : 0.0f;

    float nr = m[0] * fr + m[1] * fg + m[2] * fb + m[3] * fa + m[4];
    float ng = m[5] * fr + m[6] * fg + m[7] * fb + m[8] * fa + m[9];
    float nb = m[10] * fr + m[11] * fg + m[12] * fb + m[13] * fa + m[14];
    float na = m[15] * fr + m[16] * fg + m[17] * fb + m[18] * fa + m[19];

    nr = std::clamp(nr, 0.0f, 1.0f);
    ng = std::clamp(ng, 0.0f, 1.0f);
    nb = std::clamp(nb, 0.0f, 1.0f);
    na = std::clamp(na, 0.0f, 1.0f);

    // Premultiply back
    uint8_t out_a = static_cast<uint8_t>(na * 255.0f + 0.5f);
    uint8_t out_r = static_cast<uint8_t>(nr * na * 255.0f + 0.5f);
    uint8_t out_g = static_cast<uint8_t>(ng * na * 255.0f + 0.5f);
    uint8_t out_b = static_cast<uint8_t>(nb * na * 255.0f + 0.5f);

    return PremultipliedColorU8::from_rgba_unchecked(out_r, out_g, out_b, out_a);
}

void ColorMatrix::transform_span(const PremultipliedColorU8* src, PremultipliedColorU8* dst, size_t count) const noexcept {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = transform_pixel(src[i]);
    }
}

void ColorMatrix::apply(PixmapMut& pixmap) const noexcept {
    uint32_t w = pixmap.width();
    uint32_t h = pixmap.height();
    if (pixmap.format() == PixelFormat::RGBA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            PremultipliedColorU8* row = pixmap.row(y);
            transform_span(row, row, w);
        }
    } else if (pixmap.format() == PixelFormat::BGRA8888) {
        for (uint32_t y = 0; y < h; ++y) {
            PremultipliedColorU8* row = pixmap.row(y);
            for (uint32_t x = 0; x < w; ++x) {
                const auto bgra = row[x];
                auto rgba = PremultipliedColorU8::from_rgba_unchecked(bgra.b, bgra.g, bgra.r, bgra.a);
                auto res = transform_pixel(rgba);
                row[x] = PremultipliedColorU8::from_rgba_unchecked(res.b, res.g, res.r, res.a);
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                if (auto px = pixmap.pixel(x, y)) {
                    pixmap.set_pixel(x, y, transform_pixel(*px));
                }
            }
        }
    }
}

void ColorMatrix::apply(Pixmap& pixmap) const noexcept {
    auto mut = pixmap.as_mut();
    apply(mut);
}

} // namespace nisaba::effects

