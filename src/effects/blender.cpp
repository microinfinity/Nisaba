#include "nisaba/effects/blender.hpp"
#include "nisaba/color/blend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nisaba::effects {

namespace {

inline PremultipliedColorU8 eval_arithmetic_pixel(
    PremultipliedColorU8 src,
    PremultipliedColorU8 dst,
    float k1, float k2, float k3, float k4,
    bool enforce_premul
) noexcept {
    constexpr float inv = 1.0f / 255.0f;
    const float sr = src.r * inv, sg = src.g * inv, sb = src.b * inv, sa = src.a * inv;
    const float dr = dst.r * inv, dg = dst.g * inv, db = dst.b * inv, da = dst.a * inv;
    float a = std::clamp(k1 * sa * da + k2 * sa + k3 * da + k4, 0.0f, 1.0f);
    float r = std::clamp(k1 * sr * dr + k2 * sr + k3 * dr + k4, 0.0f, 1.0f);
    float g = std::clamp(k1 * sg * dg + k2 * sg + k3 * dg + k4, 0.0f, 1.0f);
    float b = std::clamp(k1 * sb * db + k2 * sb + k3 * db + k4, 0.0f, 1.0f);
    if (enforce_premul) {
        r = std::min(r, a);
        g = std::min(g, a);
        b = std::min(b, a);
    }
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(r * 255.0f + 0.5f),
        static_cast<uint8_t>(g * 255.0f + 0.5f),
        static_cast<uint8_t>(b * 255.0f + 0.5f),
        static_cast<uint8_t>(a * 255.0f + 0.5f)
    );
}

inline PremultipliedColorU8 scale_premul(PremultipliedColorU8 p, float f) noexcept {
    if (f >= 0.9999f) return p;
    if (f <= 0.0f) return PremultipliedColorU8::TRANSPARENT;
    const uint8_t a = static_cast<uint8_t>(std::clamp(p.a * f + 0.5f, 0.0f, 255.0f));
    return PremultipliedColorU8::from_rgba_unchecked(
        std::min(a, static_cast<uint8_t>(std::clamp(p.r * f + 0.5f, 0.0f, 255.0f))),
        std::min(a, static_cast<uint8_t>(std::clamp(p.g * f + 0.5f, 0.0f, 255.0f))),
        std::min(a, static_cast<uint8_t>(std::clamp(p.b * f + 0.5f, 0.0f, 255.0f))),
        a
    );
}

inline PremultipliedColorU8 lerp_pixel(PremultipliedColorU8 a, PremultipliedColorU8 b, uint8_t cov) noexcept {
    if (cov == 255) return b;
    if (cov == 0) return a;
    const uint32_t ic = 255u - cov;
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>((a.r * ic + b.r * cov + 128u) / 255u),
        static_cast<uint8_t>((a.g * ic + b.g * cov + 128u) / 255u),
        static_cast<uint8_t>((a.b * ic + b.b * cov + 128u) / 255u),
        static_cast<uint8_t>((a.a * ic + b.a * cov + 128u) / 255u)
    );
}

} // namespace

// Declared in runtime_filter.cpp (shared RTTI-free GLSL-subset evaluator)
void eval_runtime_blender_cpu_region(
    const RuntimeFilterSpec& spec,
    PixmapMut& dst,
    const PixmapRef& src,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    float opacity,
    const uint8_t* clip_mask,
    uint32_t clip_stride
);

PremultipliedColorU8 eval_runtime_blender_cpu_pixel(
    const RuntimeFilterSpec& spec,
    PremultipliedColorU8 src,
    PremultipliedColorU8 dst
);

Blender Blender::runtime(std::string_view source, const RuntimeFilterUniforms& uniforms) {
    Blender b;
    b.kind = BlenderKind::Runtime;
    b.runtime_spec = parse_runtime_filter_source(source, &uniforms, {});
    return b;
}

PremultipliedColorU8 Blender::blend_pixel(
    PremultipliedColorU8 src,
    PremultipliedColorU8 dst
) const noexcept {
    if (kind == BlenderKind::Arithmetic) {
        return eval_arithmetic_pixel(src, dst, k1, k2, k3, k4, enforce_premul);
    }
    if (kind == BlenderKind::Runtime && runtime_spec) {
        return eval_runtime_blender_cpu_pixel(*runtime_spec, src, dst);
    }
    return blend_pixels(src, dst, blend_mode);
}

void Blender::composite_cpu(
    PixmapMut& dst,
    const PixmapRef& src,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    float opacity,
    const uint8_t* clip_mask,
    uint32_t clip_stride
) const {
    x0 = std::max<int32_t>(0, x0);
    y0 = std::max<int32_t>(0, y0);
    x1 = std::min<int32_t>(x1, static_cast<int32_t>(std::min(dst.width(), src.width())));
    y1 = std::min<int32_t>(y1, static_cast<int32_t>(std::min(dst.height(), src.height())));
    if (x1 <= x0 || y1 <= y0) return;

    if (kind == BlenderKind::Runtime && runtime_spec) {
        eval_runtime_blender_cpu_region(*runtime_spec, dst, src, x0, y0, x1, y1, opacity, clip_mask, clip_stride);
        return;
    }

    const bool dst_bgra = (dst.format() == PixelFormat::BGRA8888);
    const bool src_bgra = (src.format() == PixelFormat::BGRA8888);

    for (int32_t y = y0; y < y1; ++y) {
        const auto* srow = src.row(static_cast<size_t>(y));
        auto* drow = dst.row(static_cast<size_t>(y));
        const uint8_t* mrow = clip_mask ? (clip_mask + static_cast<size_t>(y) * clip_stride) : nullptr;
        for (int32_t x = x0; x < x1; ++x) {
            const uint8_t cov = mrow ? mrow[x] : 255u;
            if (cov == 0) continue;
            auto sp = srow[x];
            auto dp = drow[x];
            if (src_bgra) sp = PremultipliedColorU8::from_rgba_unchecked(sp.b, sp.g, sp.r, sp.a);
            if (dst_bgra) dp = PremultipliedColorU8::from_rgba_unchecked(dp.b, dp.g, dp.r, dp.a);
            sp = scale_premul(sp, opacity);
            PremultipliedColorU8 out = (kind == BlenderKind::Arithmetic)
                ? eval_arithmetic_pixel(sp, dp, k1, k2, k3, k4, enforce_premul)
                : blend_pixels(sp, dp, blend_mode);
            out = lerp_pixel(dp, out, cov);
            if (dst_bgra) out = PremultipliedColorU8::from_rgba_unchecked(out.b, out.g, out.r, out.a);
            drow[x] = out;
        }
    }
}

} // namespace nisaba::effects

