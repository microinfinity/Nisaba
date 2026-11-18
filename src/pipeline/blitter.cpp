#include "nisaba/pipeline/blitter.hpp"
#include "nisaba/pipeline/simd.hpp"
#include "nisaba/color/blend.hpp"
#include "nisaba/color/color_space_lut.hpp"
#include <algorithm>
#include <cmath>

namespace nisaba {

namespace {

inline PremultipliedColorU8 pack_pixel(float r, float g, float b, float a) noexcept {
    float fa = std::clamp(a, 0.0f, 1.0f);
    float fr = std::clamp(r, 0.0f, fa);
    float fg = std::clamp(g, 0.0f, fa);
    float fb = std::clamp(b, 0.0f, fa);

    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(fr * 255.0f + 0.5f),
        static_cast<uint8_t>(fg * 255.0f + 0.5f),
        static_cast<uint8_t>(fb * 255.0f + 0.5f),
        static_cast<uint8_t>(fa * 255.0f + 0.5f)
    );
}

} // namespace

std::optional<PipelineBlitter> PipelineBlitter::create(
    const Paint& paint,
    std::optional<SubMaskRef> mask,
    SubPixmapMut& pixmap
) {
    if (mask.has_value()) {
        if (mask->size.width() != pixmap.size.width() ||
            mask->size.height() != pixmap.size.height()) {
            return std::nullopt;
        }
    }

    if (paint.blend_mode == BlendMode::Destination) {
        return std::nullopt;
    }
    if (paint.blend_mode == BlendMode::DestinationIn &&
        paint.shader.is_opaque() && paint.is_solid_color()) {
        return std::nullopt;
    }

    BlendMode blend_mode = paint.blend_mode;
    if (paint.shader.is_opaque() && !paint.has_color_filter() && blend_mode == BlendMode::SourceOver && !mask.has_value()) {
        blend_mode = BlendMode::Source;
    }

    std::optional<PremultipliedColorU8> memset_color{};
    if (paint.is_solid_color() && blend_mode == BlendMode::Source && !mask.has_value()) {
        Color base_c = paint.shader.solid_color();
        if (auto cf = paint.effective_color_effect()) {
            base_c = cf->transform(base_c);
        }
        memset_color = base_c.premultiply().to_color_u8();
    }
    if (blend_mode == BlendMode::Clear && !paint.anti_alias && !mask.has_value()) {
        blend_mode = BlendMode::Source;
        memset_color = PremultipliedColorU8::TRANSPARENT;
    }

    return PipelineBlitter(paint, mask, pixmap, blend_mode, memset_color);
}

PipelineBlitter::PipelineBlitter(
    const Paint& paint,
    std::optional<SubMaskRef> mask,
    SubPixmapMut& pixmap,
    BlendMode blend_mode,
    std::optional<PremultipliedColorU8> memset_color
) noexcept
    : paint_(paint),
      mask_(mask),
      pixmap_(pixmap),
      blend_mode_(blend_mode),
      memset_color_(memset_color),
      is_solid_(paint.is_solid_color()),
      pre_scale_coverage_(should_pre_scale_coverage(blend_mode))
{
    if (is_solid_) {
        Color base_c = paint_.shader.solid_color();
        if (auto cf = paint_.effective_color_effect()) {
            base_c = cf->transform(base_c);
        }
        solid_color_pm_ = base_c.premultiply().to_color_u8();
        if (pixmap_.format == PixelFormat::BGRA8888) {
            solid_color_pm_ = PremultipliedColorU8::from_rgba_unchecked(
                solid_color_pm_.blue(), solid_color_pm_.green(), solid_color_pm_.red(), solid_color_pm_.alpha()
            );
        }
        uint32_t sc_r = solid_color_pm_.red();
        uint32_t sc_g = solid_color_pm_.green();
        uint32_t sc_b = solid_color_pm_.blue();
        uint32_t sc_a = solid_color_pm_.alpha();
        for (uint32_t c = 0; c < 256; ++c) {
            uint32_t cv = c + 1;
            uint32_t mod_r = (sc_r * cv) >> 8;
            uint32_t mod_g = (sc_g * cv) >> 8;
            uint32_t mod_b = (sc_b * cv) >> 8;
            uint32_t mod_a = (sc_a * cv) >> 8;
            lut_src_[c] = mod_r | (mod_g << 8) | (mod_b << 16) | (mod_a << 24);
            lut_inv_a_[c] = 256 - mod_a;
        }
    }
    if (memset_color_.has_value() && pixmap_.format == PixelFormat::BGRA8888) {
        memset_color_ = PremultipliedColorU8::from_rgba_unchecked(
            memset_color_->blue(), memset_color_->green(), memset_color_->red(), memset_color_->alpha()
        );
    }
}

void PipelineBlitter::blend_pixel(uint32_t x, uint32_t y, float coverage) noexcept {
    blend_pixel_impl(x, y, coverage, true);
}

void PipelineBlitter::blend_pixel_impl(uint32_t x, uint32_t y, float coverage, bool apply_mask) noexcept {
    if (x >= pixmap_.size.width() || y >= pixmap_.size.height()) return;

    if (apply_mask && mask_.has_value()) {
        float ma = static_cast<float>(mask_->get(x, y)) / 255.0f;
        coverage *= ma;
    }

    if (coverage <= 0.0f) return;

    bool linear = paint_.is_linear_blending();

    if (blend_mode_ == BlendMode::SourceOver && is_solid_) {
        uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
        if (cov_u8 == 0) return;

        if (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888) {
            if (linear) {
                simd::blend_solid_source_over_linear_span(pixmap_.row(y) + x, solid_color_pm_, cov_u8, 1);
                return;
            }
            const PremultipliedColorU8& sc = solid_color_pm_;
            uint32_t cov = static_cast<uint32_t>(cov_u8) + 1;
            uint8_t sr = static_cast<uint8_t>((static_cast<uint32_t>(sc.red()) * cov) >> 8);
            uint8_t sg = static_cast<uint8_t>((static_cast<uint32_t>(sc.green()) * cov) >> 8);
            uint8_t sb = static_cast<uint8_t>((static_cast<uint32_t>(sc.blue()) * cov) >> 8);
            uint8_t sa = static_cast<uint8_t>((static_cast<uint32_t>(sc.alpha()) * cov) >> 8);

            uint32_t inv_a = 256 - static_cast<uint32_t>(sa);
            PremultipliedColorU8 dp = pixmap_.pixel(x, y);
            uint8_t dr = static_cast<uint8_t>(sr + ((static_cast<uint32_t>(dp.red()) * inv_a) >> 8));
            uint8_t dg = static_cast<uint8_t>(sg + ((static_cast<uint32_t>(dp.green()) * inv_a) >> 8));
            uint8_t db = static_cast<uint8_t>(sb + ((static_cast<uint32_t>(dp.blue()) * inv_a) >> 8));
            uint8_t da = static_cast<uint8_t>(sa + ((static_cast<uint32_t>(dp.alpha()) * inv_a) >> 8));
            pixmap_.set_pixel(x, y, PremultipliedColorU8::from_rgba_unchecked(dr, dg, db, da));
            return;
        } else if (pixmap_.format == PixelFormat::RGB565) {
            if (linear) {
                simd::blend_solid_source_over_linear_span_rgb565(
                    pixmap_.row_u16(y) + x,
                    solid_color_pm_.red(), solid_color_pm_.green(), solid_color_pm_.blue(), solid_color_pm_.alpha(),
                    cov_u8, 1
                );
                return;
            }
            simd::blend_solid_source_over_coverage_rgb565(
                pixmap_.row_u16(y) + x,
                solid_color_pm_.red(), solid_color_pm_.green(), solid_color_pm_.blue(), solid_color_pm_.alpha(),
                cov_u8, 1
            );
            return;
        } else if (pixmap_.format == PixelFormat::Alpha8) {
            simd::blend_solid_source_over_coverage_alpha8(
                pixmap_.row_u8(y) + x,
                solid_color_pm_.alpha(),
                cov_u8, 1
            );
            return;
        }
    }

    if (is_solid_ && !linear && !paint_.has_color_filter() &&
        (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888)) {
        uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
        if (cov_u8 == 0) return;
        PremultipliedColorU8 dp = pixmap_.pixel(x, y);
        if (coverage >= 1.0f) {
            pixmap_.set_pixel(x, y, simd::blend_pixel_by_mode(dp, solid_color_pm_, blend_mode_));
        } else if (pre_scale_coverage_) {
            uint32_t cv = static_cast<uint32_t>(cov_u8) + 1;
            PremultipliedColorU8 sc_mod = PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>((solid_color_pm_.red() * cv) >> 8),
                static_cast<uint8_t>((solid_color_pm_.green() * cv) >> 8),
                static_cast<uint8_t>((solid_color_pm_.blue() * cv) >> 8),
                static_cast<uint8_t>((solid_color_pm_.alpha() * cv) >> 8)
            );
            pixmap_.set_pixel(x, y, simd::blend_pixel_by_mode(dp, sc_mod, blend_mode_));
        } else {
            PremultipliedColorU8 target = simd::blend_pixel_by_mode(dp, solid_color_pm_, blend_mode_);
            pixmap_.set_pixel(x, y, simd::lerp_pixel(dp, target, cov_u8));
        }
        return;
    }

    if ((blend_mode_ == BlendMode::SourceOver || blend_mode_ == BlendMode::Source) && !paint_.has_color_filter() && !linear &&
        (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888)) {
        PremultipliedColorU8 s_pm;
        paint_.shader.shade_span(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, 1, &s_pm);
        if (pixmap_.format == PixelFormat::BGRA8888) {
            s_pm = PremultipliedColorU8::from_rgba_unchecked(s_pm.blue(), s_pm.green(), s_pm.red(), s_pm.alpha());
        }
        uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
        if (blend_mode_ == BlendMode::Source && s_pm.alpha() == 255) {
            PremultipliedColorU8* p_dst = pixmap_.row(y) + x;
            *p_dst = simd::lerp_pixel(*p_dst, s_pm, cov_u8);
        } else {
            simd::blend_source_over_span_coverage(pixmap_.row(y) + x, &s_pm, cov_u8, 1);
        }
        return;
    }

    Color sc = paint_.shader.sample(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
    if (auto cf = paint_.effective_color_effect()) {
        sc = cf->transform(sc);
    }
    PremultipliedColorU8 dp = pixmap_.pixel(x, y);

    if (linear) {
        float da = static_cast<float>(dp.alpha()) / 255.0f;
        float dr = da > 0.0f ? (static_cast<float>(dp.red()) / 255.0f) / da : 0.0f;
        float dg = da > 0.0f ? (static_cast<float>(dp.green()) / 255.0f) / da : 0.0f;
        float db = da > 0.0f ? (static_cast<float>(dp.blue()) / 255.0f) / da : 0.0f;

        float dr_lin = ColorSpaceLut::to_linear_f32(dr);
        float dg_lin = ColorSpaceLut::to_linear_f32(dg);
        float db_lin = ColorSpaceLut::to_linear_f32(db);

        float sr = sc.red();
        float sg = sc.green();
        float sb = sc.blue();
        float sa = sc.alpha();

        float sr_lin = ColorSpaceLut::to_linear_f32(sr);
        float sg_lin = ColorSpaceLut::to_linear_f32(sg);
        float sb_lin = ColorSpaceLut::to_linear_f32(sb);

        float eff_sa = sa;
        if (pre_scale_coverage_) {
            eff_sa *= coverage;
        }

        float sr_lin_pm = sr_lin * eff_sa;
        float sg_lin_pm = sg_lin * eff_sa;
        float sb_lin_pm = sb_lin * eff_sa;

        float dr_lin_pm = dr_lin * da;
        float dg_lin_pm = dg_lin * da;
        float db_lin_pm = db_lin * da;

        BlendResult br = blend_components(sr_lin_pm, sg_lin_pm, sb_lin_pm, eff_sa,
                                          dr_lin_pm, dg_lin_pm, db_lin_pm, da, blend_mode_);

        if (!pre_scale_coverage_ && coverage < 1.0f) {
            br.r = dr_lin_pm + (br.r - dr_lin_pm) * coverage;
            br.g = dg_lin_pm + (br.g - dg_lin_pm) * coverage;
            br.b = db_lin_pm + (br.b - db_lin_pm) * coverage;
            br.a = da + (br.a - da) * coverage;
        }

        float out_a = std::clamp(br.a, 0.0f, 1.0f);
        if (out_a <= 0.0f) {
            pixmap_.set_pixel(x, y, PremultipliedColorU8::from_rgba_unchecked(0, 0, 0, 0));
        } else {
            float out_r_lin = std::clamp(br.r / out_a, 0.0f, 1.0f);
            float out_g_lin = std::clamp(br.g / out_a, 0.0f, 1.0f);
            float out_b_lin = std::clamp(br.b / out_a, 0.0f, 1.0f);

            float out_r_srgb = ColorSpaceLut::to_srgb_f32(out_r_lin);
            float out_g_srgb = ColorSpaceLut::to_srgb_f32(out_g_lin);
            float out_b_srgb = ColorSpaceLut::to_srgb_f32(out_b_lin);

            pixmap_.set_pixel(x, y, pack_pixel(out_r_srgb * out_a, out_g_srgb * out_a, out_b_srgb * out_a, out_a));
        }
        return;
    }

    float sr = sc.red() * sc.alpha();
    float sg = sc.green() * sc.alpha();
    float sb = sc.blue() * sc.alpha();
    float sa = sc.alpha();

    if (pre_scale_coverage_) {
        sr *= coverage;
        sg *= coverage;
        sb *= coverage;
        sa *= coverage;
    }

    float dr = static_cast<float>(dp.red()) / 255.0f;
    float dg = static_cast<float>(dp.green()) / 255.0f;
    float db = static_cast<float>(dp.blue()) / 255.0f;
    float da = static_cast<float>(dp.alpha()) / 255.0f;

    BlendResult br = blend_components(sr, sg, sb, sa, dr, dg, db, da, blend_mode_);

    if (!pre_scale_coverage_ && coverage < 1.0f) {
        br.r = dr + (br.r - dr) * coverage;
        br.g = dg + (br.g - dg) * coverage;
        br.b = db + (br.b - db) * coverage;
        br.a = da + (br.a - da) * coverage;
    }

    pixmap_.set_pixel(x, y, pack_pixel(br.r, br.g, br.b, br.a));
}

void PipelineBlitter::blend_span(uint32_t x, uint32_t y, uint32_t len, float coverage) noexcept {
    if (y >= pixmap_.size.height() || x >= pixmap_.size.width() || len == 0) return;
    uint32_t max_w = pixmap_.size.width() - x;
    uint32_t count = std::min(len, max_w);

    if (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888) {
        if (memset_color_.has_value() && coverage >= 1.0f && !mask_.has_value()) {
            simd::fill_solid_span(pixmap_.row(y) + x, *memset_color_, count);
            return;
        }

        if (is_solid_) {
            if (mask_.has_value()) {
                if (blend_mode_ == BlendMode::SourceOver) {
                    const uint8_t* m_ptr = mask_->data + y * mask_->real_width + x;
                    if (coverage >= 1.0f) {
                        simd::blend_solid_mask_span(pixmap_.row(y) + x, solid_color_pm_, m_ptr, count);
                    } else {
                        uint32_t cv = static_cast<uint32_t>(coverage * 255.0f + 0.5f) + 1;
                        PremultipliedColorU8 sc_mod = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>((solid_color_pm_.red() * cv) >> 8),
                            static_cast<uint8_t>((solid_color_pm_.green() * cv) >> 8),
                            static_cast<uint8_t>((solid_color_pm_.blue() * cv) >> 8),
                            static_cast<uint8_t>((solid_color_pm_.alpha() * cv) >> 8)
                        );
                        simd::blend_solid_mask_span(pixmap_.row(y) + x, sc_mod, m_ptr, count);
                    }
                    return;
                }
            } else if (!paint_.is_linear_blending() && !paint_.has_color_filter()) {
                const PremultipliedColorU8& sc = solid_color_pm_;
                if (coverage >= 1.0f) {
                    simd::blend_solid_span_by_mode(pixmap_.row(y) + x, sc, blend_mode_, count);
                    return;
                }
                uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
                if (cov_u8 == 0) return;

                if (pre_scale_coverage_) {
                    if (blend_mode_ == BlendMode::SourceOver) {
                        simd::blend_solid_source_over_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else if (blend_mode_ == BlendMode::DestinationOver) {
                        simd::blend_solid_dest_over_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else if (blend_mode_ == BlendMode::SourceIn) {
                        simd::blend_solid_source_in_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else if (blend_mode_ == BlendMode::SourceOut) {
                        simd::blend_solid_source_out_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else if (blend_mode_ == BlendMode::DestinationOut) {
                        simd::blend_solid_dest_out_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else if (blend_mode_ == BlendMode::Xor) {
                        simd::blend_solid_xor_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else {
                        uint32_t cv = static_cast<uint32_t>(cov_u8) + 1;
                        PremultipliedColorU8 sc_mod = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>((sc.red() * cv) >> 8),
                            static_cast<uint8_t>((sc.green() * cv) >> 8),
                            static_cast<uint8_t>((sc.blue() * cv) >> 8),
                            static_cast<uint8_t>((sc.alpha() * cv) >> 8)
                        );
                        simd::blend_solid_span_by_mode(pixmap_.row(y) + x, sc_mod, blend_mode_, count);
                    }
                    return;
                } else {
                    if (blend_mode_ == BlendMode::Multiply) {
                        simd::blend_solid_multiply_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else if (blend_mode_ == BlendMode::Screen) {
                        simd::blend_solid_screen_coverage(pixmap_.row(y) + x, sc, cov_u8, count);
                    } else {
                        PremultipliedColorU8* dst = pixmap_.row(y) + x;
                        for (size_t i = 0; i < count; ++i) {
                            PremultipliedColorU8 target = simd::blend_pixel_by_mode(dst[i], sc, blend_mode_);
                            dst[i] = simd::lerp_pixel(dst[i], target, cov_u8);
                        }
                    }
                    return;
                }
            } else if (blend_mode_ == BlendMode::SourceOver && paint_.is_linear_blending()) {
                if (coverage >= 1.0f && solid_color_pm_.alpha() == 255) {
                    simd::fill_solid_span(pixmap_.row(y) + x, solid_color_pm_, count);
                } else {
                    uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
                    simd::blend_solid_source_over_linear_span(pixmap_.row(y) + x, solid_color_pm_, cov_u8, count);
                }
                return;
            }
        }

        if ((blend_mode_ == BlendMode::SourceOver || blend_mode_ == BlendMode::Source) &&
            !paint_.has_color_filter() &&
            (paint_.shader.type() == Shader::Type::LinearGradient ||
             paint_.shader.type() == Shader::Type::RadialGradient ||
             paint_.shader.type() == Shader::Type::TwoPointConicalGradient ||
             paint_.shader.type() == Shader::Type::Pattern)) {
            if (!mask_.has_value()) {
                if ((blend_mode_ == BlendMode::Source || paint_.shader.is_opaque()) && coverage >= 1.0f) {
                    paint_.shader.shade_span(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, count, pixmap_.row(y) + x);
                    if (pixmap_.format == PixelFormat::BGRA8888) {
                        auto* row_p = reinterpret_cast<uint8_t*>(pixmap_.row(y) + x);
                        for (uint32_t k = 0; k < count; ++k) {
                            std::swap(row_p[k * 4 + 0], row_p[k * 4 + 2]);
                        }
                    }
                    return;
                }

                alignas(16) PremultipliedColorU8 span_buf[512];
                uint32_t processed = 0;
                uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
                bool has_uni_a = paint_.shader.has_uniform_alpha();
                uint8_t uni_a = paint_.shader.uniform_alpha();

                while (processed < count) {
                    uint32_t cur_chunk = std::min(count - processed, 512u);
                    paint_.shader.shade_span(static_cast<float>(x + processed) + 0.5f, static_cast<float>(y) + 0.5f, cur_chunk, span_buf);
                    if (pixmap_.format == PixelFormat::BGRA8888) {
                        auto* buf_p = reinterpret_cast<uint8_t*>(span_buf);
                        for (uint32_t k = 0; k < cur_chunk; ++k) {
                            std::swap(buf_p[k * 4 + 0], buf_p[k * 4 + 2]);
                        }
                    }
                    PremultipliedColorU8* dst = pixmap_.row(y) + (x + processed);
                    if (blend_mode_ == BlendMode::SourceOver && coverage >= 1.0f) {
                        if (has_uni_a) {
                            simd::blend_source_over_span_uniform_alpha(dst, span_buf, uni_a, cur_chunk);
                        } else {
                            simd::blend_source_over_span(dst, span_buf, cur_chunk);
                        }
                    } else if (blend_mode_ == BlendMode::SourceOver) {
                        simd::blend_source_over_span_coverage(dst, span_buf, cov_u8, cur_chunk);
                    } else {
                        for (uint32_t i = 0; i < cur_chunk; ++i) {
                            blend_pixel(x + processed + i, y, coverage);
                        }
                    }
                    processed += cur_chunk;
                }
                return;
            } else if (blend_mode_ == BlendMode::SourceOver) {
                // Shaded span with mask (e.g. vector clip!)
                const uint8_t* m_ptr = mask_->data + y * mask_->real_width + x;
                alignas(16) PremultipliedColorU8 span_buf[512];
                uint32_t processed = 0;
                uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);

                while (processed < count) {
                    uint32_t cur_chunk = std::min(count - processed, 512u);
                    const uint8_t* cur_m = m_ptr + processed;

                    // Fast-skip zero mask chunks
                    size_t z = 0;
                    while (z + 8 <= cur_chunk) {
                        uint64_t v;
                        std::memcpy(&v, cur_m + z, 8);
                        if (v != 0) break;
                        z += 8;
                    }
                    while (z < cur_chunk && cur_m[z] == 0) ++z;
                    if (z == cur_chunk) {
                        processed += cur_chunk;
                        continue;
                    }

                    paint_.shader.shade_span(static_cast<float>(x + processed) + 0.5f, static_cast<float>(y) + 0.5f, cur_chunk, span_buf);
                    if (pixmap_.format == PixelFormat::BGRA8888) {
                        auto* buf_p = reinterpret_cast<uint8_t*>(span_buf);
                        for (uint32_t k = 0; k < cur_chunk; ++k) {
                            std::swap(buf_p[k * 4 + 0], buf_p[k * 4 + 2]);
                        }
                    }
                    PremultipliedColorU8* dst = pixmap_.row(y) + (x + processed);
                    if (coverage >= 1.0f) {
                        simd::blend_source_over_mask_span(dst, span_buf, cur_m, cur_chunk);
                    } else {
                        alignas(16) uint8_t eff_mask[512];
                        for (uint32_t k = 0; k < cur_chunk; ++k) {
                            eff_mask[k] = static_cast<uint8_t>((static_cast<uint16_t>(cur_m[k]) * cov_u8 + 127) / 255);
                        }
                        simd::blend_source_over_mask_span(dst, span_buf, eff_mask, cur_chunk);
                    }
                    processed += cur_chunk;
                }
                return;
            }
        }
    } else if (pixmap_.format == PixelFormat::RGB565) {
        if (memset_color_.has_value() && coverage >= 1.0f && !mask_.has_value()) {
            simd::fill_solid_span_rgb565(pixmap_.row_u16(y) + x, solid_color_pm_.to_rgb565(), count);
            return;
        }
        if (blend_mode_ == BlendMode::SourceOver && is_solid_ && !mask_.has_value()) {
            if (paint_.is_linear_blending()) {
                if (coverage >= 1.0f && solid_color_pm_.alpha() == 255) {
                    simd::fill_solid_span_rgb565(pixmap_.row_u16(y) + x, solid_color_pm_.to_rgb565(), count);
                } else {
                    uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
                    simd::blend_solid_source_over_linear_span_rgb565(
                        pixmap_.row_u16(y) + x,
                        solid_color_pm_.red(), solid_color_pm_.green(), solid_color_pm_.blue(), solid_color_pm_.alpha(),
                        cov_u8, count
                    );
                }
                return;
            }
            if (coverage >= 1.0f) {
                simd::blend_solid_source_over_rgb565(
                    pixmap_.row_u16(y) + x,
                    solid_color_pm_.red(), solid_color_pm_.green(), solid_color_pm_.blue(), solid_color_pm_.alpha(),
                    count
                );
            } else {
                uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
                simd::blend_solid_source_over_coverage_rgb565(
                    pixmap_.row_u16(y) + x,
                    solid_color_pm_.red(), solid_color_pm_.green(), solid_color_pm_.blue(), solid_color_pm_.alpha(),
                    cov_u8, count
                );
            }
            return;
        }
    } else if (pixmap_.format == PixelFormat::Alpha8) {
        if (memset_color_.has_value() && coverage >= 1.0f && !mask_.has_value()) {
            simd::fill_solid_span_alpha8(pixmap_.row_u8(y) + x, solid_color_pm_.alpha(), count);
            return;
        }
        if (blend_mode_ == BlendMode::SourceOver && is_solid_ && !mask_.has_value()) {
            if (coverage >= 1.0f) {
                simd::blend_solid_source_over_alpha8(pixmap_.row_u8(y) + x, solid_color_pm_.alpha(), count);
            } else {
                uint8_t cov_u8 = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
                simd::blend_solid_source_over_coverage_alpha8(pixmap_.row_u8(y) + x, solid_color_pm_.alpha(), cov_u8, count);
            }
            return;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        blend_pixel(x + i, y, coverage);
    }
}

void PipelineBlitter::blit_h(uint32_t x, uint32_t y, LengthU32 width) {
    blend_span(x, y, width.get(), 1.0f);
}

void PipelineBlitter::blit_anti_h(
    uint32_t x,
    uint32_t y,
    std::span<AlphaU8> antialias,
    std::span<AlphaRun> runs
) {
    if (y >= pixmap_.size.height()) return;

    if (blend_mode_ == BlendMode::SourceOver && is_solid_) {
        const PremultipliedColorU8& sc = solid_color_pm_;
        uint32_t max_w = pixmap_.size.width();
        size_t aa_offset = 0;
        size_t run_offset = 0;
        AlphaRun run_opt = runs[0];

        if (mask_.has_value()) {
            if (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888) {
                const uint8_t* m_ptr = mask_->data + y * mask_->real_width;
                while (run_opt.has_value() && x < max_w) {
                    uint16_t run_len = *run_opt;
                    uint32_t count = std::min(static_cast<uint32_t>(run_len), max_w - x);
                    uint8_t aa = antialias[aa_offset];
                    if (aa == 255) {
                        simd::blend_solid_mask_span(pixmap_.row(y) + x, sc, m_ptr + x, count);
                    } else if (aa > 0) {
                        uint32_t cv = static_cast<uint32_t>(aa) + 1;
                        PremultipliedColorU8 sc_mod = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>((sc.red() * cv) >> 8),
                            static_cast<uint8_t>((sc.green() * cv) >> 8),
                            static_cast<uint8_t>((sc.blue() * cv) >> 8),
                            static_cast<uint8_t>((sc.alpha() * cv) >> 8)
                        );
                        simd::blend_solid_mask_span(pixmap_.row(y) + x, sc_mod, m_ptr + x, count);
                    }
                    x += run_len;
                    run_offset += run_len;
                    aa_offset += run_len;
                    run_opt = (run_offset < runs.size()) ? runs[run_offset] : std::nullopt;
                }
                return;
            }
        } else if (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888) {
            while (run_opt.has_value() && x < max_w) {
                uint16_t run_len = *run_opt;
                uint32_t count = std::min(static_cast<uint32_t>(run_len), max_w - x);
                uint8_t aa = antialias[aa_offset];

                if (paint_.is_linear_blending()) {
                    if (aa == 255 && sc.alpha() == 255) {
                        simd::fill_solid_span(pixmap_.row(y) + x, sc, count);
                    } else if (aa > 0) {
                        simd::blend_solid_source_over_linear_span(pixmap_.row(y) + x, sc, aa, count);
                    }
                } else {
                    if (aa == 255) {
                        simd::blend_solid_source_over(pixmap_.row(y) + x, sc, count);
                    } else if (aa > 0) {
                        simd::blend_solid_source_over_coverage(pixmap_.row(y) + x, sc, aa, count);
                    }
                }

                x += run_len;
                run_offset += run_len;
                aa_offset += run_len;
                run_opt = (run_offset < runs.size()) ? runs[run_offset] : std::nullopt;
            }
            return;
        } else if (pixmap_.format == PixelFormat::RGB565) {
            while (run_opt.has_value() && x < max_w) {
                uint16_t run_len = *run_opt;
                uint32_t count = std::min(static_cast<uint32_t>(run_len), max_w - x);
                uint8_t aa = antialias[aa_offset];

                if (paint_.is_linear_blending()) {
                    if (aa == 255 && sc.alpha() == 255) {
                        simd::fill_solid_span_rgb565(pixmap_.row_u16(y) + x, sc.to_rgb565(), count);
                    } else if (aa > 0) {
                        simd::blend_solid_source_over_linear_span_rgb565(
                            pixmap_.row_u16(y) + x,
                            sc.red(), sc.green(), sc.blue(), sc.alpha(),
                            aa, count
                        );
                    }
                } else {
                    if (aa == 255) {
                        simd::blend_solid_source_over_rgb565(pixmap_.row_u16(y) + x, sc.red(), sc.green(), sc.blue(), sc.alpha(), count);
                    } else if (aa > 0) {
                        simd::blend_solid_source_over_coverage_rgb565(pixmap_.row_u16(y) + x, sc.red(), sc.green(), sc.blue(), sc.alpha(), aa, count);
                    }
                }

                x += run_len;
                run_offset += run_len;
                aa_offset += run_len;
                run_opt = (run_offset < runs.size()) ? runs[run_offset] : std::nullopt;
            }
            return;
        } else if (pixmap_.format == PixelFormat::Alpha8) {
            while (run_opt.has_value() && x < max_w) {
                uint16_t run_len = *run_opt;
                uint32_t count = std::min(static_cast<uint32_t>(run_len), max_w - x);
                uint8_t aa = antialias[aa_offset];

                if (aa == 255) {
                    simd::blend_solid_source_over_alpha8(pixmap_.row_u8(y) + x, sc.alpha(), count);
                } else if (aa > 0) {
                    simd::blend_solid_source_over_coverage_alpha8(pixmap_.row_u8(y) + x, sc.alpha(), aa, count);
                }

                x += run_len;
                run_offset += run_len;
                aa_offset += run_len;
                run_opt = (run_offset < runs.size()) ? runs[run_offset] : std::nullopt;
            }
            return;
        }
    }

    size_t aa_offset = 0;
    size_t run_offset = 0;
    AlphaRun run_opt = runs[0];

    while (run_opt.has_value()) {
        uint16_t run_len = *run_opt;
        LengthU32 width = LengthU32::create_unchecked(run_len);
        uint8_t aa = antialias[aa_offset];

        if (aa == 0) {
            // transparent
        } else if (aa == 255) {
            blit_h(x, y, width);
        } else {
            float cov = static_cast<float>(aa) / 255.0f;
            blend_span(x, y, width.get(), cov);
        }

        x += width.get();
        run_offset += run_len;
        aa_offset += run_len;
        run_opt = (run_offset < runs.size()) ? runs[run_offset] : std::nullopt;
    }
}

void PipelineBlitter::blit_span_coverage(
    uint32_t x,
    uint32_t y,
    const uint8_t* coverage,
    uint32_t count
) {
    if (y >= pixmap_.size.height() || x >= pixmap_.size.width() || count == 0) return;
    count = std::min(count, pixmap_.size.width() - x);

    alignas(32) uint8_t local_cov[2048];
    const uint8_t* active_cov = coverage;
    bool has_mask = mask_.has_value();
    if (has_mask) {
        const uint8_t* m_ptr = mask_->data + y * mask_->real_width + x;
        if (count <= 2048) {
            simd::multiply_mask_spans_2(local_cov, coverage, m_ptr, count);
            active_cov = local_cov;
        } else {
            static thread_local std::vector<uint8_t> tl_cov;
            if (tl_cov.size() < count) tl_cov.resize(count);
            simd::multiply_mask_spans_2(tl_cov.data(), coverage, m_ptr, count);
            active_cov = tl_cov.data();
        }
    }

    if (blend_mode_ == BlendMode::SourceOver && is_solid_) {
        const PremultipliedColorU8& sc = solid_color_pm_;

        if (pixmap_.format == PixelFormat::RGB565) {
            uint16_t* dst = pixmap_.row_u16(y) + x;
            if (paint_.is_linear_blending()) {
                for (uint32_t i = 0; i < count; ++i) {
                    uint8_t c = active_cov[i];
                    if (c == 255 && sc.alpha() == 255) {
                        dst[i] = sc.to_rgb565();
                    } else if (c > 0) {
                        simd::blend_solid_source_over_linear_span_rgb565(dst + i, sc.red(), sc.green(), sc.blue(), sc.alpha(), c, 1);
                    }
                }
                return;
            }
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t c = active_cov[i];
                if (c == 255) {
                    simd::blend_solid_source_over_rgb565(dst + i, sc.red(), sc.green(), sc.blue(), sc.alpha(), 1);
                } else if (c > 0) {
                    simd::blend_solid_source_over_coverage_rgb565(dst + i, sc.red(), sc.green(), sc.blue(), sc.alpha(), c, 1);
                }
            }
            return;
        } else if (pixmap_.format == PixelFormat::Alpha8) {
            uint8_t* dst = pixmap_.row_u8(y) + x;
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t c = active_cov[i];
                if (c == 255) {
                    simd::blend_solid_source_over_alpha8(dst + i, sc.alpha(), 1);
                } else if (c > 0) {
                    simd::blend_solid_source_over_coverage_alpha8(dst + i, sc.alpha(), c, 1);
                }
            }
            return;
        }

        PremultipliedColorU8* dst = pixmap_.row(y) + x;

        if (paint_.is_linear_blending()) {
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t c = active_cov[i];
                if (c == 255 && sc.alpha() == 255) {
                    dst[i] = sc;
                } else if (c > 0) {
                    simd::blend_solid_source_over_linear_span(dst + i, sc, c, 1);
                }
            }
            return;
        }

        size_t i = 0;
        while (i < count) {
            uint8_t c = active_cov[i];
            if (c == 0) {
                // Skip zero run
#if defined(NISABA_HAS_AVX2)
                while (i + 32 <= count) {
                    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(active_cov + i));
                    if (!_mm256_testz_si256(v, v)) break;
                    i += 32;
                }
#endif
                while (i + 8 <= count) {
                    uint64_t c64;
                    std::memcpy(&c64, active_cov + i, sizeof(uint64_t));
                    if (c64 != 0) {
                        i += static_cast<size_t>(std::countr_zero(c64) >> 3);
                        break;
                    }
                    i += 8;
                }
                while (i < count && active_cov[i] == 0) ++i;
                continue;
            }

            if (c == 255) {
                // Fast path: solid 255 run -> direct large SIMD span blend!
                size_t start_full = i;
#if defined(NISABA_HAS_AVX2)
                __m256i ones = _mm256_set1_epi8(static_cast<char>(0xFF));
                while (i + 32 <= count) {
                    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(active_cov + i));
                    __m256i cmp = _mm256_cmpeq_epi8(v, ones);
                    if (_mm256_movemask_epi8(cmp) != -1) break;
                    i += 32;
                }
#endif
                while (i + 8 <= count) {
                    uint64_t c64;
                    std::memcpy(&c64, active_cov + i, sizeof(uint64_t));
                    if (c64 != 0xFFFFFFFFFFFFFFFFULL) break;
                    i += 8;
                }
                while (i < count && active_cov[i] == 255) ++i;
                simd::blend_solid_source_over(dst + start_full, sc, i - start_full);
                continue;
            }

            // Antialiased edge run (coverage between 1 and 254)
            size_t edge_start = i;
            while (i < count && active_cov[i] > 0 && active_cov[i] < 255) ++i;
            size_t edge_len = i - edge_start;

            size_t k = 0;
#if defined(NISABA_HAS_AVX2)
            {
                __m256i zero256 = _mm256_setzero_si256();
                while (k + 8 <= edge_len) {
                    uint8_t c0 = active_cov[edge_start + k];
                    uint8_t c1 = active_cov[edge_start + k + 1];
                    uint8_t c2 = active_cov[edge_start + k + 2];
                    uint8_t c3 = active_cov[edge_start + k + 3];
                    uint8_t c4 = active_cov[edge_start + k + 4];
                    uint8_t c5 = active_cov[edge_start + k + 5];
                    uint8_t c6 = active_cov[edge_start + k + 6];
                    uint8_t c7 = active_cov[edge_start + k + 7];

                    __m256i v_src = _mm256_setr_epi32(
                        static_cast<int>(lut_src_[c0]),
                        static_cast<int>(lut_src_[c1]),
                        static_cast<int>(lut_src_[c2]),
                        static_cast<int>(lut_src_[c3]),
                        static_cast<int>(lut_src_[c4]),
                        static_cast<int>(lut_src_[c5]),
                        static_cast<int>(lut_src_[c6]),
                        static_cast<int>(lut_src_[c7])
                    );

                    __m256i v_ia_lo = _mm256_setr_epi16(
                        static_cast<int16_t>(lut_inv_a_[c0]), static_cast<int16_t>(lut_inv_a_[c0]),
                        static_cast<int16_t>(lut_inv_a_[c0]), static_cast<int16_t>(lut_inv_a_[c0]),
                        static_cast<int16_t>(lut_inv_a_[c1]), static_cast<int16_t>(lut_inv_a_[c1]),
                        static_cast<int16_t>(lut_inv_a_[c1]), static_cast<int16_t>(lut_inv_a_[c1]),
                        static_cast<int16_t>(lut_inv_a_[c2]), static_cast<int16_t>(lut_inv_a_[c2]),
                        static_cast<int16_t>(lut_inv_a_[c2]), static_cast<int16_t>(lut_inv_a_[c2]),
                        static_cast<int16_t>(lut_inv_a_[c3]), static_cast<int16_t>(lut_inv_a_[c3]),
                        static_cast<int16_t>(lut_inv_a_[c3]), static_cast<int16_t>(lut_inv_a_[c3])
                    );
                    __m256i v_ia_hi = _mm256_setr_epi16(
                        static_cast<int16_t>(lut_inv_a_[c4]), static_cast<int16_t>(lut_inv_a_[c4]),
                        static_cast<int16_t>(lut_inv_a_[c4]), static_cast<int16_t>(lut_inv_a_[c4]),
                        static_cast<int16_t>(lut_inv_a_[c5]), static_cast<int16_t>(lut_inv_a_[c5]),
                        static_cast<int16_t>(lut_inv_a_[c5]), static_cast<int16_t>(lut_inv_a_[c5]),
                        static_cast<int16_t>(lut_inv_a_[c6]), static_cast<int16_t>(lut_inv_a_[c6]),
                        static_cast<int16_t>(lut_inv_a_[c6]), static_cast<int16_t>(lut_inv_a_[c6]),
                        static_cast<int16_t>(lut_inv_a_[c7]), static_cast<int16_t>(lut_inv_a_[c7]),
                        static_cast<int16_t>(lut_inv_a_[c7]), static_cast<int16_t>(lut_inv_a_[c7])
                    );

                    __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + edge_start + k));
                    __m256i d_lo = _mm256_unpacklo_epi8(d, zero256);
                    __m256i d_hi = _mm256_unpackhi_epi8(d, zero256);

                    __m256i d_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d_lo, v_ia_lo), 8);
                    __m256i d_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d_hi, v_ia_hi), 8);

                    __m256i d_res = _mm256_packus_epi16(d_mul_lo, d_mul_hi);
                    __m256i out = _mm256_adds_epu8(v_src, d_res);
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + edge_start + k), out);

                    k += 8;
                }
            }
#endif
#if defined(NISABA_HAS_SSE2) || defined(NISABA_HAS_AVX2)
            __m128i zero128 = _mm_setzero_si128();
            while (k + 4 <= edge_len) {
                uint8_t c0 = active_cov[edge_start + k];
                uint8_t c1 = active_cov[edge_start + k + 1];
                uint8_t c2 = active_cov[edge_start + k + 2];
                uint8_t c3 = active_cov[edge_start + k + 3];

                uint32_t s0 = lut_src_[c0];
                uint32_t s1 = lut_src_[c1];
                uint32_t s2 = lut_src_[c2];
                uint32_t s3 = lut_src_[c3];

                int16_t ia0 = static_cast<int16_t>(lut_inv_a_[c0]);
                int16_t ia1 = static_cast<int16_t>(lut_inv_a_[c1]);
                int16_t ia2 = static_cast<int16_t>(lut_inv_a_[c2]);
                int16_t ia3 = static_cast<int16_t>(lut_inv_a_[c3]);

                __m128i v_src = _mm_setr_epi32(
                    static_cast<int>(s0),
                    static_cast<int>(s1),
                    static_cast<int>(s2),
                    static_cast<int>(s3)
                );

                __m128i v_ia_lo = _mm_setr_epi16(ia0, ia0, ia0, ia0, ia1, ia1, ia1, ia1);
                __m128i v_ia_hi = _mm_setr_epi16(ia2, ia2, ia2, ia2, ia3, ia3, ia3, ia3);

                __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + edge_start + k));
                __m128i d_lo = _mm_unpacklo_epi8(d, zero128);
                __m128i d_hi = _mm_unpackhi_epi8(d, zero128);

                __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, v_ia_lo), 8);
                __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, v_ia_hi), 8);

                __m128i d_res = _mm_packus_epi16(d_mul_lo, d_mul_hi);
                __m128i out = _mm_adds_epu8(v_src, d_res);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + edge_start + k), out);

                k += 4;
            }
#endif
            for (; k < edge_len; ++k) {
                uint8_t c_val = active_cov[edge_start + k];
                uint32_t src_packed = lut_src_[c_val];
                uint32_t inv_a = lut_inv_a_[c_val];
                uint32_t d = *reinterpret_cast<const uint32_t*>(dst + edge_start + k);
                uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
                uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
                *reinterpret_cast<uint32_t*>(dst + edge_start + k) = src_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
            }
        }
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t c = active_cov[i];
        if (c > 0) {
            blend_pixel_impl(x + i, y, static_cast<float>(c) / 255.0f, !has_mask);
        }
    }
}

void PipelineBlitter::blit_v(uint32_t x, uint32_t y, LengthU32 height, AlphaU8 alpha) {
    if (x >= pixmap_.size.width() || y >= pixmap_.size.height() || alpha == 0) return;
    uint32_t h = std::min(height.get(), pixmap_.size.height() - y);

    if (blend_mode_ == BlendMode::SourceOver && is_solid_ && !mask_.has_value()) {
        const PremultipliedColorU8& sc = solid_color_pm_;
        if (pixmap_.format == PixelFormat::RGBA8888 || pixmap_.format == PixelFormat::BGRA8888) {
            uint32_t cov = static_cast<uint32_t>(alpha) + 1;
            uint32_t sr = (static_cast<uint32_t>(sc.red()) * cov) >> 8;
            uint32_t sg = (static_cast<uint32_t>(sc.green()) * cov) >> 8;
            uint32_t sb = (static_cast<uint32_t>(sc.blue()) * cov) >> 8;
            uint32_t sa = (static_cast<uint32_t>(sc.alpha()) * cov) >> 8;
            uint32_t inv_a = 256 - sa;

            uint32_t src_packed = sr | (sg << 8) | (sb << 16) | (sa << 24);
            uint8_t* row_ptr = reinterpret_cast<uint8_t*>(pixmap_.row(y) + x);
            size_t stride = pixmap_.stride();

            for (uint32_t i = 0; i < h; ++i) {
                uint32_t* d_ptr = reinterpret_cast<uint32_t*>(row_ptr);
                uint32_t d = *d_ptr;
                uint32_t rb = ((d & 0x00FF00FF) * inv_a) >> 8;
                uint32_t ga = (((d >> 8) & 0x00FF00FF) * inv_a) >> 8;
                *d_ptr = src_packed + ((rb & 0x00FF00FF) | ((ga & 0x00FF00FF) << 8));
                row_ptr += stride;
            }
            return;
        } else if (pixmap_.format == PixelFormat::RGB565) {
            for (uint32_t i = 0; i < h; ++i) {
                simd::blend_solid_source_over_coverage_rgb565(
                    pixmap_.row_u16(y + i) + x,
                    sc.red(), sc.green(), sc.blue(), sc.alpha(), alpha, 1
                );
            }
            return;
        } else if (pixmap_.format == PixelFormat::Alpha8) {
            for (uint32_t i = 0; i < h; ++i) {
                simd::blend_solid_source_over_coverage_alpha8(
                    pixmap_.row_u8(y + i) + x,
                    sc.alpha(), alpha, 1
                );
            }
            return;
        }
    }

    float cov = static_cast<float>(alpha) / 255.0f;
    for (uint32_t i = 0; i < h; ++i) {
        blend_pixel(x, y + i, cov);
    }
}

void PipelineBlitter::blit_anti_h2(uint32_t x, uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) {
    blend_pixel(x, y, static_cast<float>(alpha0) / 255.0f);
    blend_pixel(x + 1, y, static_cast<float>(alpha1) / 255.0f);
}

void PipelineBlitter::blit_anti_v2(uint32_t x, uint32_t y, AlphaU8 alpha0, AlphaU8 alpha1) {
    blend_pixel(x, y, static_cast<float>(alpha0) / 255.0f);
    blend_pixel(x, y + 1, static_cast<float>(alpha1) / 255.0f);
}

void PipelineBlitter::blit_rect(const ScreenIntRect& rect) {
    if (pixmap_.format == PixelFormat::RGB565 || pixmap_.format == PixelFormat::Alpha8) {
        uint32_t y_end = rect.bottom();
        for (uint32_t y = rect.y(); y < y_end; ++y) {
            blend_span(rect.x(), y, rect.width(), 1.0f);
        }
        return;
    }

    if (memset_color_.has_value() && !mask_.has_value()) {
        uint32_t y_end = rect.bottom();
        for (uint32_t y = rect.y(); y < y_end; ++y) {
            simd::fill_solid_span(pixmap_.row(y) + rect.x(), *memset_color_, rect.width());
        }
        return;
    }

    if ((blend_mode_ == BlendMode::SourceOver || blend_mode_ == BlendMode::Source) &&
        !paint_.has_color_filter() &&
        paint_.shader.type() == Shader::Type::LinearGradient && !mask_.has_value()) {
        uint32_t w = rect.width();
        uint32_t x0 = rect.x();
        uint32_t y0 = rect.y();
        uint32_t y_end = rect.bottom();

        if (paint_.shader.is_horizontal_gradient()) {
            std::vector<PremultipliedColorU8> span_heap;
            alignas(16) PremultipliedColorU8 span_stack[1024];
            PremultipliedColorU8* span_buf = span_stack;
            if (w > 1024) {
                span_heap.resize(w);
                span_buf = span_heap.data();
            }

            // Shade row once!
            paint_.shader.shade_span(static_cast<float>(x0) + 0.5f, static_cast<float>(y0) + 0.5f, w, span_buf);
            if (pixmap_.format == PixelFormat::BGRA8888) {
                auto* p = reinterpret_cast<uint8_t*>(span_buf);
                for (uint32_t k = 0; k < w; ++k) {
                    std::swap(p[k * 4 + 0], p[k * 4 + 2]);
                }
            }

            bool is_opaque = (blend_mode_ == BlendMode::Source || paint_.shader.is_opaque());
            bool has_uni_a = paint_.shader.has_uniform_alpha();
            uint8_t uni_a = paint_.shader.uniform_alpha();

            for (uint32_t y = y0; y < y_end; ++y) {
                PremultipliedColorU8* dst = pixmap_.row(y) + x0;
                if (is_opaque) {
                    std::memcpy(dst, span_buf, w * sizeof(PremultipliedColorU8));
                } else if (has_uni_a) {
                    simd::blend_source_over_span_uniform_alpha(dst, span_buf, uni_a, w);
                } else {
                    simd::blend_source_over_span(dst, span_buf, w);
                }
            }
            return;
        } else if (blend_mode_ == BlendMode::Source || paint_.shader.is_opaque()) {
            // Direct row shade with 3-phase clamp-free DDA!
            for (uint32_t y = y0; y < y_end; ++y) {
                paint_.shader.shade_span(static_cast<float>(x0) + 0.5f, static_cast<float>(y) + 0.5f, w, pixmap_.row(y) + x0);
                if (pixmap_.format == PixelFormat::BGRA8888) {
                    auto* p = reinterpret_cast<uint8_t*>(pixmap_.row(y) + x0);
                    for (uint32_t k = 0; k < w; ++k) {
                        std::swap(p[k * 4 + 0], p[k * 4 + 2]);
                    }
                }
            }
            return;
        }
    }

    if ((blend_mode_ == BlendMode::SourceOver || blend_mode_ == BlendMode::Source) &&
        !paint_.has_color_filter() &&
        paint_.shader.type() == Shader::Type::RadialGradient && !mask_.has_value() && pixmap_.format == PixelFormat::RGBA8888) {
        const auto& rad = paint_.shader.radial_gradient();
        const auto& inv_opt = rad.inv_combined();
        if (rad.spread_mode() == SpreadMode::Pad &&
            rad.pad_color().alpha() == 0 && inv_opt.has_value()) {
            const Transform& inv = *inv_opt;
            if (std::abs(inv.kx) < 1e-5f && std::abs(inv.ky) < 1e-5f &&
                std::abs(inv.sx - inv.sy) < 1e-5f && inv.sx > 0.0f) {
                float center_x = -inv.tx / inv.sx;
                float center_y = -inv.ty / inv.sy;
                float radius = 1.0f / inv.sx;
                float inv_r = inv.sx;
                const auto& ramp_sq = rad.ramp_sq();

                uint32_t x0 = rect.x();
                uint32_t y0 = rect.y();
                uint32_t w = rect.width();
                uint32_t y_end = rect.bottom();

                for (uint32_t y = y0; y < y_end; ++y) {
                    float cur_y = static_cast<float>(y) + 0.5f;
                    float py0 = (cur_y - center_y) * inv_r;
                    float rem = 1.0f - py0 * py0;
                    if (rem <= 0.0f) continue;

                    float px_max = std::sqrt(rem);
                    int32_t x_start_circ = static_cast<int32_t>(std::floor(center_x - px_max * radius));
                    int32_t x_end_circ   = static_cast<int32_t>(std::ceil(center_x + px_max * radius));

                    int32_t span_x0 = std::max(static_cast<int32_t>(x0), x_start_circ);
                    int32_t span_x1 = std::min(static_cast<int32_t>(x0 + w), x_end_circ);
                    int32_t in_count = span_x1 - span_x0;
                    if (in_count <= 0) continue;

                    float px0 = (static_cast<float>(span_x0) + 0.5f - center_x) * inv_r;
                    float dpx = inv_r;

                    double A = dpx * dpx;
                    double B = 2.0 * px0 * dpx;
                    double C = px0 * px0 + py0 * py0;

                    constexpr double S = static_cast<double>(1023.0);
                    constexpr int64_t FP_ONE = 1 << 16;
                    int64_t v_fp = static_cast<int64_t>(C * S * FP_ONE);
                    int64_t dv1_fp = static_cast<int64_t>((A + B) * S * FP_ONE);
                    int64_t dv2_fp = static_cast<int64_t>(2.0 * A * S * FP_ONE);

                    const uint32_t* lut32 = reinterpret_cast<const uint32_t*>(ramp_sq.data());
                    PremultipliedColorU8* dst_ptr = pixmap_.row(y) + span_x0;

#if defined(NISABA_HAS_AVX2)
                    __m256i zero = _mm256_setzero_si256();
                    __m256i v_256 = _mm256_set1_epi16(256);

                    int32_t i = 0;
                    int32_t count8 = in_count & ~7;
                    for (; i < count8; i += 8) {
                        uint32_t idx0 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx1 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx2 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx3 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx4 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx5 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx6 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t idx7 = std::min(static_cast<uint32_t>(v_fp >> 16), 1023u);
                        v_fp += dv1_fp; dv1_fp += dv2_fp;

                        __m256i idx_v = _mm256_setr_epi32(idx0, idx1, idx2, idx3, idx4, idx5, idx6, idx7);
                        __m256i s = _mm256_i32gather_epi32(reinterpret_cast<const int*>(lut32), idx_v, 4);

                        if (blend_mode_ == BlendMode::Source) {
                            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_ptr + i), s);
                        } else {
                            __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst_ptr + i));

                            __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
                            __m256i s_hi = _mm256_unpackhi_epi8(s, zero);

                            __m256i sa_lo = _mm256_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
                            sa_lo = _mm256_shufflehi_epi16(sa_lo, _MM_SHUFFLE(3, 3, 3, 3));
                            __m256i inv_a_lo = _mm256_sub_epi16(v_256, sa_lo);

                            __m256i sa_hi = _mm256_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
                            sa_hi = _mm256_shufflehi_epi16(sa_hi, _MM_SHUFFLE(3, 3, 3, 3));
                            __m256i inv_a_hi = _mm256_sub_epi16(v_256, sa_hi);

                            __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
                            __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

                            __m256i d_mul_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d_lo, inv_a_lo), 8);
                            __m256i d_mul_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d_hi, inv_a_hi), 8);

                            __m256i out_lo = _mm256_add_epi16(s_lo, d_mul_lo);
                            __m256i out_hi = _mm256_add_epi16(s_hi, d_mul_hi);

                            __m256i res = _mm256_packus_epi16(out_lo, out_hi);
                            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_ptr + i), res);
                        }
                    }
#elif defined(NISABA_HAS_SSE2)
                    __m128i zero = _mm_setzero_si128();
                    __m128i v_256 = _mm_set1_epi16(256);

                    int32_t i = 0;
                    int32_t count4 = in_count & ~3;
                    for (; i < count4; i += 4) {
                        uint32_t s0 = lut32[std::min(static_cast<uint32_t>(v_fp >> 16), 1023u)];
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t s1 = lut32[std::min(static_cast<uint32_t>(v_fp >> 16), 1023u)];
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t s2 = lut32[std::min(static_cast<uint32_t>(v_fp >> 16), 1023u)];
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        uint32_t s3 = lut32[std::min(static_cast<uint32_t>(v_fp >> 16), 1023u)];
                        v_fp += dv1_fp; dv1_fp += dv2_fp;

                        __m128i s = _mm_setr_epi32(s0, s1, s2, s3);
                        if (blend_mode_ == BlendMode::Source) {
                            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr + i), s);
                        } else {
                            __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst_ptr + i));

                            __m128i s_lo = _mm_unpacklo_epi8(s, zero);
                            __m128i s_hi = _mm_unpackhi_epi8(s, zero);

                            __m128i sa_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
                            sa_lo = _mm_shufflehi_epi16(sa_lo, _MM_SHUFFLE(3, 3, 3, 3));
                            __m128i inv_a_lo = _mm_sub_epi16(v_256, sa_lo);

                            __m128i sa_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
                            sa_hi = _mm_shufflehi_epi16(sa_hi, _MM_SHUFFLE(3, 3, 3, 3));
                            __m128i inv_a_hi = _mm_sub_epi16(v_256, sa_hi);

                            __m128i d_lo = _mm_unpacklo_epi8(d, zero);
                            __m128i d_hi = _mm_unpackhi_epi8(d, zero);

                            __m128i d_mul_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, inv_a_lo), 8);
                            __m128i d_mul_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, inv_a_hi), 8);

                            __m128i out_lo = _mm_add_epi16(s_lo, d_mul_lo);
                            __m128i out_hi = _mm_add_epi16(s_hi, d_mul_hi);

                            __m128i res = _mm_packus_epi16(out_lo, out_hi);
                            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr + i), res);
                        }
                    }
#else
                    int32_t i = 0;
#endif

                    for (; i < in_count; ++i) {
                        PremultipliedColorU8 s = ramp_sq[std::min(static_cast<uint32_t>(v_fp >> 16), 1023u)];
                        v_fp += dv1_fp; dv1_fp += dv2_fp;
                        if (blend_mode_ == BlendMode::Source) {
                            dst_ptr[i] = s;
                        } else {
                            if (s.alpha() == 0) continue;
                            uint32_t inv_a = 256 - static_cast<uint32_t>(s.alpha());
                            PremultipliedColorU8 d = dst_ptr[i];
                            uint8_t r = static_cast<uint8_t>(s.red() + ((static_cast<uint32_t>(d.red()) * inv_a) >> 8));
                            uint8_t g = static_cast<uint8_t>(s.green() + ((static_cast<uint32_t>(d.green()) * inv_a) >> 8));
                            uint8_t b = static_cast<uint8_t>(s.blue() + ((static_cast<uint32_t>(d.blue()) * inv_a) >> 8));
                            uint8_t a = static_cast<uint8_t>(s.alpha() + ((static_cast<uint32_t>(d.alpha()) * inv_a) >> 8));
                            dst_ptr[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
                        }
                    }
                }
                return;
            }
        }
    }

    uint32_t y_end = rect.bottom();
    for (uint32_t y = rect.y(); y < y_end; ++y) {
        blend_span(rect.x(), y, rect.width(), 1.0f);
    }
}

void PipelineBlitter::blit_mask(const MaskInfo& mask_info, const ScreenIntRect& clip) {
    for (uint32_t y = clip.top(); y < clip.bottom(); ++y) {
        for (uint32_t x = clip.left(); x < clip.right(); ++x) {
            uint32_t mx = x - mask_info.bounds.left();
            uint32_t my = y - mask_info.bounds.top();
            uint8_t ma = mask_info.image[my * mask_info.row_bytes + mx];
            if (ma > 0) {
                blend_pixel(x, y, static_cast<float>(ma) / 255.0f);
            }
        }
    }
}

} // namespace nisaba
