#include <iostream>
#include <cassert>
#include <cmath>
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/color/blend.hpp"
#include "nisaba/pipeline/simd.hpp"
#include <array>
#include <vector>
#include "nisaba/raster/fixed_point.hpp"
#include "nisaba/math/screen_int_rect.hpp"

using namespace nisaba;

void test_color_premultiply_demultiply() {
    // Exact tests from nisaba
    auto c1 = ColorU8::from_rgba(10, 20, 30, 40).premultiply();
    assert(c1 == PremultipliedColorU8::from_rgba_unchecked(2, 3, 5, 40));

    auto c2 = ColorU8::from_rgba(10, 20, 30, 255).premultiply();
    assert(c2 == PremultipliedColorU8::from_rgba_unchecked(10, 20, 30, 255));

    auto d1 = PremultipliedColorU8::from_rgba_unchecked(2, 3, 5, 40).demultiply();
    assert(d1 == ColorU8::from_rgba(13, 19, 32, 40));

    auto d2 = PremultipliedColorU8::from_rgba_unchecked(10, 20, 30, 255).demultiply();
    assert(d2 == ColorU8::from_rgba(10, 20, 30, 255));

    auto d3 = PremultipliedColorU8::from_rgba_unchecked(153, 99, 54, 180).demultiply();
    assert(d3 == ColorU8::from_rgba(217, 140, 77, 180));

    // Float Color
    Color fc = Color::from_rgba8(255, 128, 0, 128);
    auto pfc = fc.premultiply();
    assert(std::abs(pfc.red() - (1.0f * (128.0f / 255.0f))) < 0.01f);

    auto dfc = pfc.demultiply();
    assert(std::abs(dfc.red() - 1.0f) < 0.01f);
    assert(std::abs(dfc.alpha() - (128.0f / 255.0f)) < 0.01f);

    std::cout << "[PASS] Color premultiply and demultiply tests passed\n";
}

void test_colorspace() {
    auto n05 = *NormalizedF32::create(0.5f);
    auto expanded_g2 = ColorSpaceHelper::expand_channel(ColorSpace::Gamma2, n05);
    assert(std::abs(expanded_g2.get() - 0.25f) < 0.001f);

    auto compressed_g2 = ColorSpaceHelper::compress_channel(ColorSpace::Gamma2, expanded_g2);
    assert(std::abs(compressed_g2.get() - 0.5f) < 0.001f);

    std::cout << "[PASS] ColorSpace conversion tests passed\n";
}

void test_blend_mode() {
    assert(should_pre_scale_coverage(BlendMode::SourceOver) == true);
    assert(should_pre_scale_coverage(BlendMode::Plus) == true);
    assert(should_pre_scale_coverage(BlendMode::SourceIn) == false);
    assert(should_pre_scale_coverage(BlendMode::Multiply) == false);

    constexpr std::array<BlendMode, 29> kAllModes = {
        BlendMode::Clear, BlendMode::Source, BlendMode::Destination,
        BlendMode::SourceOver, BlendMode::DestinationOver, BlendMode::SourceIn,
        BlendMode::DestinationIn, BlendMode::SourceOut, BlendMode::DestinationOut,
        BlendMode::SourceAtop, BlendMode::DestinationAtop, BlendMode::Xor,
        BlendMode::Plus, BlendMode::Modulate, BlendMode::Screen,
        BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten,
        BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::HardLight,
        BlendMode::SoftLight, BlendMode::Difference, BlendMode::Exclusion,
        BlendMode::Multiply, BlendMode::Hue, BlendMode::Saturation,
        BlendMode::Color, BlendMode::Luminosity
    };

    // Verify blend_pixels vs simd::blend_pixel_by_mode vs simd::blend_solid_span_by_mode across all 29 modes
    const std::array<PremultipliedColorU8, 4> kDstSamples = {
        ColorU8::from_rgba(64, 180, 210, 170).premultiply(),
        ColorU8::from_rgba(200, 120, 40, 255).premultiply(),
        ColorU8::from_rgba(0, 0, 0, 0).premultiply(),
        ColorU8::from_rgba(255, 255, 255, 128).premultiply()
    };
    const std::array<PremultipliedColorU8, 4> kSrcSamples = {
        ColorU8::from_rgba(220, 90, 45, 190).premultiply(),
        ColorU8::from_rgba(50, 210, 140, 255).premultiply(),
        ColorU8::from_rgba(180, 180, 180, 100).premultiply(),
        ColorU8::from_rgba(0, 0, 0, 0).premultiply()
    };

    for (BlendMode mode : kAllModes) {
        for (const auto& dst : kDstSamples) {
            for (const auto& src : kSrcSamples) {
                PremultipliedColorU8 ref = blend_pixels(src, dst, mode);
                PremultipliedColorU8 pix = simd::blend_pixel_by_mode(dst, src, mode);
                std::vector<PremultipliedColorU8> span(16, dst);
                simd::blend_solid_span_by_mode(span.data(), src, mode, span.size());

                auto diff = [](uint8_t a, uint8_t b) { return std::abs(int(a) - int(b)); };
                assert(diff(ref.red(), pix.red()) <= 2);
                assert(diff(ref.green(), pix.green()) <= 2);
                assert(diff(ref.blue(), pix.blue()) <= 2);
                assert(diff(ref.alpha(), pix.alpha()) <= 2);

                for (size_t k = 0; k < span.size(); ++k) {
                    assert(span[k].red() == pix.red());
                    assert(span[k].green() == pix.green());
                    assert(span[k].blue() == pix.blue());
                    assert(span[k].alpha() == pix.alpha());
                }
            }
        }
    }

    // Specific regression checks: Modulate != SourceIn when src != white, ColorDodge/ColorBurn with da < 255
    {
        auto dst = ColorU8::from_rgba(200, 100, 50, 255).premultiply();
        auto src = ColorU8::from_rgba(128, 255, 0, 255).premultiply();
        auto mod = blend_pixels(src, dst, BlendMode::Modulate);
        auto sin = blend_pixels(src, dst, BlendMode::SourceIn);
        assert(mod.red() != sin.red() || mod.green() != sin.green());
        assert(mod.blue() == 0);
        assert(simd::blend_pixel_by_mode(dst, src, BlendMode::Modulate).blue() == 0);
    }

    std::cout << "[PASS] BlendMode tests passed\n";
}

void test_fixed_point() {
    // fdot6
    assert(fdot6::from_i32(1) == 64);
    assert(fdot6::from_f32(1.5f) == 96);
    assert(fdot6::floor(96) == 1);
    assert(fdot6::ceil(96) == 2);
    assert(fdot6::round(96) == 2);
    assert(fdot6::to_fdot16(64) == 65536);
    assert(fdot6::small_scale(100, 32) == 50);

    // fdot8
    assert(fdot8::from_fdot16(65536) == 256);

    // fdot16
    assert(fdot16::from_f32(1.0f) == 65536);
    assert(fdot16::floor_to_i32(65536) == 1);
    assert(fdot16::round_to_i32(65536 + 32768) == 2);
    assert(fdot16::mul(65536, 65536 * 2) == 65536 * 2);
    assert(fdot16::div(64, 32) == 65536 * 2);

    std::cout << "[PASS] Fixed-point tests passed\n";
}

void test_screen_int_rect() {
    assert(!ScreenIntRect::from_xywh(0, 0, 0, 0).has_value());
    assert(!ScreenIntRect::from_xywh(0, 0, 1, 0).has_value());
    assert(!ScreenIntRect::from_xywh(0, 0, 0, 1).has_value());

    auto r = ScreenIntRect::from_xywh(1, 2, 3, 4);
    assert(r.has_value());
    assert(r->x() == 1);
    assert(r->y() == 2);
    assert(r->width() == 3);
    assert(r->height() == 4);
    assert(r->right() == 4);
    assert(r->bottom() == 6);

    auto r_sub = ScreenIntRect::from_xywh(2, 3, 1, 1);
    assert(r->contains(*r_sub));

    std::cout << "[PASS] ScreenIntRect tests passed\n";
}

int main() {
    std::cout << "Running Nisaba Color, Fixed-Point & ScreenIntRect tests...\n";
    test_color_premultiply_demultiply();
    test_colorspace();
    test_blend_mode();
    test_fixed_point();
    test_screen_int_rect();
    std::cout << "All Color & Fixed-Point tests passed successfully!\n";
    return 0;
}
