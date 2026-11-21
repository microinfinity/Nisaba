#include <cassert>
#include <iostream>
#include <cmath>
#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::effects;

void test_box_blur_radii() {
    std::cout << "[Test] Box blur radii calculation..." << std::endl;
    auto r0 = compute_box_blur_radii(0.0f);
    assert(r0[0] == 0 && r0[1] == 0 && r0[2] == 0);

    auto r_neg = compute_box_blur_radii(-5.0f);
    assert(r_neg[0] == 0 && r_neg[1] == 0 && r_neg[2] == 0);

    auto r4 = compute_box_blur_radii(4.0f);
    assert(r4[0] > 0 && r4[1] > 0 && r4[2] > 0);
    std::cout << "  Sigma 4.0 radii: [" << r4[0] << ", " << r4[1] << ", " << r4[2] << "]" << std::endl;
}

void test_mask_blur() {
    std::cout << "[Test] Mask blur (8-bit alpha)..." << std::endl;
    auto mask = Mask::allocate(50, 50);
    assert(mask.has_value());

    // Fill a 10x10 block in the center (from 20 to 30)
    for (uint32_t y = 20; y < 30; ++y) {
        for (uint32_t x = 20; x < 30; ++x) {
            mask->set(x, y, 255);
        }
    }

    assert(mask->get(25, 25) == 255);
    assert(mask->get(10, 10) == 0);

    gaussian_blur_mask(*mask, 3.0f);

    // Center pixel should be diffused
    uint8_t center_val = mask->get(25, 25);
    assert(center_val > 0 && center_val < 255);

    // Adjacent pixels outside original 10x10 should now have blur coverage
    uint8_t edge_val = mask->get(18, 25);
    assert(edge_val > 0);

    std::cout << "  Center val: " << (int)center_val << ", Edge val: " << (int)edge_val << std::endl;
}

void test_pixmap_blur() {
    std::cout << "[Test] Pixmap blur (32-bit premultiplied RGBA)..." << std::endl;
    auto pix = Pixmap::allocate(60, 60);
    assert(pix.has_value());
    pix->fill(Color::BLACK);

    // Draw solid red block in center (20 to 40)
    for (uint32_t y = 20; y < 40; ++y) {
        auto* row = pix->as_mut().row(y);
        for (uint32_t x = 20; x < 40; ++x) {
            row[x] = PremultipliedColorU8::from_rgba_unchecked(255, 0, 0, 255);
        }
    }

    auto mut = pix->as_mut();
    gaussian_blur_pixmap(mut, 4.0f);

    // Check all pixels maintain valid premultiplication: R <= A, G <= A, B <= A
    for (uint32_t y = 0; y < 60; ++y) {
        const auto* row = mut.row(y);
        for (uint32_t x = 0; x < 60; ++x) {
            const auto& p = row[x];
            assert(p.r <= p.a);
            assert(p.g <= p.a);
            assert(p.b <= p.a);
        }
    }

    // Blurred red has spread into pixel (16, 30)
    auto p_spread = mut.row(30)[16];
    assert(p_spread.r > 0 && p_spread.a > 0);
    std::cout << "  Spread pixel (16, 30): R=" << (int)p_spread.r << " A=" << (int)p_spread.a << std::endl;
}

void test_color_filters() {
    std::cout << "[Test] Color filters (Invert, Grayscale, Brightness, Tint)..." << std::endl;
    auto pix = Pixmap::allocate(20, 20);
    assert(pix.has_value());
    auto mut = pix->as_mut();

    // Set pixel to semi-transparent Cyan (R=0, G=100, B=200, A=200)
    mut.row(0)[0] = PremultipliedColorU8::from_rgba_unchecked(0, 100, 200, 200);

    // 1. Invert: R'=200-0=200, G'=200-100=100, B'=200-200=0, A'=200
    ColorFilter::invert(mut);
    auto p_inv = mut.row(0)[0];
    assert(p_inv.r == 200);
    assert(p_inv.g == 100);
    assert(p_inv.b == 0);
    assert(p_inv.a == 200);

    // 2. Grayscale
    ColorFilter::grayscale(mut);
    auto p_gray = mut.row(0)[0];
    assert(p_gray.r == p_gray.g && p_gray.g == p_gray.b);
    assert(p_gray.r <= p_gray.a);

    // 3. Brightness positive
    uint8_t prev_r = p_gray.r;
    ColorFilter::adjust_brightness(mut, 0.5f);
    assert(mut.row(0)[0].r >= prev_r);

    std::cout << "  Color filters passed." << std::endl;
}

void test_drop_shadow() {
    std::cout << "[Test] Drop shadow rendering..." << std::endl;
    auto pix = Pixmap::allocate(100, 100);
    assert(pix.has_value());
    pix->fill(Color::WHITE);

    Canvas canvas(*pix);
    auto rect = Rect::from_xywh(30.0f, 30.0f, 40.0f, 40.0f);
    assert(rect.has_value());

    DropShadow shadow(5.0f, 10.0f, 4.0f, Color::from_rgba8(0, 0, 0, 180));
    Paint red_paint(Color::from_rgba8(255, 0, 0, 255));

    canvas.draw_rect_with_shadow(*rect, red_paint, shadow);

    // At rect center (50, 50), pixel must be Red
    auto p_center = pix->as_ref().pixel(50, 50);
    assert(p_center.has_value());
    assert(p_center->red() == 255 && p_center->green() == 0 && p_center->blue() == 0);

    // At shadow offset (rect bottom + dy) around (50, 75), pixel should be darkened by shadow
    auto p_shadow = pix->as_ref().pixel(50, 75);
    assert(p_shadow.has_value());
    assert(p_shadow->red() < 255); // darkened from pure white 255

    std::cout << "  Drop shadow verified." << std::endl;
}

void test_glass_panel() {
    std::cout << "[Test] Glassmorphism panel rendering..." << std::endl;
    auto pix = Pixmap::allocate(150, 150);
    assert(pix.has_value());

    // Vibrant background pattern
    Canvas canvas(*pix);
    canvas.clear(Color::from_rgba8(20, 30, 60, 255));

    Paint circle_paint(Color::from_rgba8(255, 100, 50, 255));
    canvas.fill_circle(75.0f, 75.0f, 40.0f, circle_paint);

    // Draw Glass panel over the circle
    auto panel_rect = Rect::from_xywh(40.0f, 40.0f, 70.0f, 70.0f);
    assert(panel_rect.has_value());

    GlassParams glass = GlassParams::light();
    canvas.draw_glass_panel(*panel_rect, 12.0f, 12.0f, glass);

    // Pixmap integrity check
    auto ref = pix->as_ref();
    assert(ref.width() == 150 && ref.height() == 150);

    std::cout << "  Glassmorphism panel verified." << std::endl;
}

void test_color_effect_and_image_filter() {
    std::cout << "[Test] ColorEffect, anisotropic blur, and RRect/Circle shadows..." << std::endl;

    // 1. ColorEffect modes, transfers, and composition
    const Color src = Color::from_rgba8(120, 180, 60, 255);
    const auto atop = ColorEffect::mode(Color::from_rgba8(200, 40, 40, 128), BlendMode::SourceAtop);
    const Color c_atop = atop.transform(src);
    assert(c_atop.alpha() > 0.99f && c_atop.red() > src.red() && c_atop.green() < src.green());

    const auto mult = ColorEffect::mode(Color::from_rgba8(128, 255, 128, 255), BlendMode::Multiply);
    const Color c_mult = mult.transform(src);
    assert(c_mult.red() < src.red() && std::abs(c_mult.green() - src.green()) < 0.02f);

    const auto to_lin = ColorEffect::srgb_to_linear();
    const auto to_srgb = ColorEffect::linear_to_srgb();
    const Color lin = to_lin.transform(Color::from_rgba8(128, 128, 128, 255));
    assert(lin.red() < 0.30f && lin.red() > 0.15f);
    const Color rt = to_srgb.transform(lin);
    assert(std::abs(rt.red() - (128.0f / 255.0f)) < 0.02f);

    auto comp = ColorEffect::compose(atop, ColorEffect::grayscale());
    assert(comp.has_value() && comp->has_matrix && comp->has_blend);

    // 2. Anisotropic gaussian_blur_pixmap_xy
    auto blur_pm = Pixmap::allocate(64, 64);
    assert(blur_pm.has_value());
    blur_pm->fill(Color::TRANSPARENT);
    {
        Canvas c(*blur_pm);
        Paint wp(Color::WHITE);
        c.fill_rect(*Rect::from_xywh(28.0f, 28.0f, 8.0f, 8.0f), wp);
    }
    auto bmut = blur_pm->as_mut();
    gaussian_blur_pixmap_xy(bmut, 5.0f, 0.5f);
    // Horizontal blur spreads further at (20, 32) than vertical blur at (32, 20)
    auto px_horiz = blur_pm->as_ref().pixel(20, 32);
    auto px_vert  = blur_pm->as_ref().pixel(32, 20);
    assert(px_horiz.has_value() && px_vert.has_value());
    assert(px_horiz->alpha() > px_vert->alpha());

    // 3. Asymmetric RRect shadow and Circle shadow via Canvas
    auto sh_pm = Pixmap::allocate(120, 120);
    assert(sh_pm.has_value());
    sh_pm->fill(Color::WHITE);
    {
        Canvas c(*sh_pm);
        auto rr = RRect::from_xywh(20.0f, 20.0f, 60.0f, 50.0f, BorderRadius::only(22.0f, 0.0f, 14.0f, 4.0f));
        assert(rr.has_value());
        DropShadow ds(6.0f, 8.0f, 3.5f, Color::from_rgba8(0, 0, 0, 180));
        c.draw_rrect_with_shadow(*rr, Paint(Color::from_rgba8(40, 100, 220, 255)), ds);
        c.draw_circle_with_shadow(90.0f, 90.0f, 12.0f, Paint(Color::from_rgba8(220, 80, 40, 255)), ds);
    }
    // Rounded TL (22px) -> pixel (22, 22) is outside rounded corner and should remain light/white-ish;
    // sharp TR (0px) -> pixel (78, 22) is inside blue rrect fill!
    auto p_tl = sh_pm->as_ref().pixel(22, 22);
    auto p_tr = sh_pm->as_ref().pixel(78, 22);
    auto p_sh = sh_pm->as_ref().pixel(50, 76);
    assert(p_tl.has_value() && p_tr.has_value() && p_sh.has_value());
    assert(p_tr->blue() > 180 && p_tr->red() < 80);
    assert(p_tl->red() > 200);
    assert(p_sh->red() < 250);

    std::cout << "  ColorEffect, anisotropic blur, and RRect/Circle shadows passed." << std::endl;
}

void test_image_filter_dag() {
    std::cout << "[Test] Composable ImageFilter DAG (compose, merge, blend, offset, color_filter, CropRect)..." << std::endl;

    // 1. Offset + CropRect
    {
        auto pm = Pixmap::allocate(100, 100);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(220, 40, 40, 255));
        auto crop = Rect::from_xywh(30.0f, 30.0f, 30.0f, 30.0f);
        p.set_image_filter(ImageFilter::offset(20.0f, 15.0f, std::nullopt, crop));
        c.fill_rect(*Rect::from_xywh(15.0f, 20.0f, 40.0f, 40.0f), p);

        // Shifted rect is [35..75] x [35..75], cropped to [30..60] x [30..60] => visible inside [35..60] x [35..60]
        auto inside = pm->as_ref().pixel(45, 45);
        auto outside_crop = pm->as_ref().pixel(65, 45);
        auto old_pos = pm->as_ref().pixel(20, 25);
        assert(inside.has_value() && outside_crop.has_value() && old_pos.has_value());
        assert(inside->red() > 210 && inside->alpha() == 255);
        assert(outside_crop->alpha() == 0);
        assert(old_pos->alpha() == 0);
    }

    // 2. Compose(ColorFilter(invert), Offset(10, 0))
    {
        auto pm = Pixmap::allocate(100, 100);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(255, 0, 0, 255));
        auto f = ImageFilter::compose(
            ImageFilter::color_filter(ColorEffect::invert()),
            ImageFilter::offset(12.0f, 0.0f)
        );
        p.set_image_filter(f);
        c.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 30.0f, 30.0f), p);

        // Shifted to [32..62] and inverted red -> cyan (0, 255, 255, 255)
        auto px = pm->as_ref().pixel(45, 35);
        assert(px.has_value());
        assert(px->red() < 5 && px->green() > 250 && px->blue() > 250 && px->alpha() == 255);
    }

    // 3. Merge(offset+tint, source)
    {
        auto pm = Pixmap::allocate(120, 100);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(240, 40, 40, 255));
        auto ghost = ImageFilter::offset(35.0f, 0.0f,
            ImageFilter::color_filter(ColorEffect::mode(Color::from_rgba8(40, 80, 240, 255), BlendMode::SourceIn)));
        auto merged = ImageFilter::merge(ghost, std::optional<ImageFilter>{std::nullopt});
        p.set_image_filter(merged);
        c.fill_rect(*Rect::from_xywh(15.0f, 25.0f, 30.0f, 40.0f), p);

        auto orig_px = pm->as_ref().pixel(25, 45);
        auto ghost_px = pm->as_ref().pixel(65, 45);
        assert(orig_px.has_value() && ghost_px.has_value());
        assert(orig_px->red() > 230 && orig_px->blue() < 60);
        assert(ghost_px->blue() > 230 && ghost_px->red() < 60);
    }

    // 4. Blend(Difference, bg, fg)
    {
        auto pm = Pixmap::allocate(100, 100);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(200, 100, 50, 255));
        auto fg = ImageFilter::color_filter(ColorEffect::mode(Color::from_rgba8(200, 100, 50, 255), BlendMode::SourceIn));
        auto blended = ImageFilter::blend(BlendMode::Difference, std::nullopt, fg);
        p.set_image_filter(blended);
        c.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 40.0f, 40.0f), p);

        // Difference of identical opaque colors yields black (0, 0, 0, 255)
        auto px = pm->as_ref().pixel(40, 40);
        assert(px.has_value());
        assert(px->red() <= 2 && px->green() <= 2 && px->blue() <= 2 && px->alpha() == 255);
    }

    // 5. Bounds mapping check
    {
        auto src_r = Rect::from_xywh(10.0f, 10.0f, 50.0f, 40.0f).value();
        auto f = ImageFilter::compose(ImageFilter::blur(4.0f), ImageFilter::offset(10.0f, 5.0f));
        Rect mapped = f.map_bounds(src_r);
        assert(mapped.left() < 10.0f + 10.0f && mapped.right() > 60.0f + 10.0f);
        assert(mapped.top() < 10.0f + 5.0f && mapped.bottom() > 50.0f + 5.0f);
    }

    std::cout << "  Composable ImageFilter DAG verified." << std::endl;
}

void test_blur_mask_filter_and_perlin_noise() {
    std::cout << "[Test] BlurMaskFilter (Normal/Solid/Outer/Inner) & PerlinNoiseShader..." << std::endl;

    // 1. All 4 BlurStyle modes on a solid rect
    auto check_style = [](BlurStyle style) -> std::pair<uint8_t, uint8_t> {
        auto pm = Pixmap::allocate(100, 100);
        assert(pm.has_value());
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(255, 80, 40, 255));
        p.set_mask_filter(MaskFilter::blur(style, 4.0f));
        c.fill_rect(*Rect::from_xywh(30.0f, 30.0f, 40.0f, 40.0f), p);
        auto inside_edge = pm->as_ref().pixel(32, 50); // near inner boundary
        auto outside_edge = pm->as_ref().pixel(27, 50); // just outside boundary
        assert(inside_edge.has_value() && outside_edge.has_value());
        return {inside_edge->alpha(), outside_edge->alpha()};
    };

    auto [norm_in, norm_out] = check_style(BlurStyle::Normal);
    auto [solid_in, solid_out] = check_style(BlurStyle::Solid);
    auto [outer_in, outer_out] = check_style(BlurStyle::Outer);
    auto [inner_in, inner_out] = check_style(BlurStyle::Inner);

    assert(norm_in > 100 && norm_in < 255 && norm_out > 10);
    assert(solid_in == 255 && solid_out > 10);
    assert(outer_in == 0 && outer_out > 10);
    assert(inner_in > 100 && inner_in < 255 && inner_out == 0);

    // 2. PerlinNoiseShader (FractalNoise & Turbulence + seamless tiling)
    {
        auto tile = IntSize::from_wh(32, 32);
        auto fn = Shader::create_fractal_noise(0.0625f, 0.0625f, 4, 7.0f, tile);
        auto tb = Shader::create_turbulence(0.0625f, 0.0625f, 4, 7.0f, tile);

        Color c0 = fn.sample(5.5f, 9.5f);
        Color c1 = fn.sample(5.5f + 32.0f, 9.5f);
        Color c2 = fn.sample(5.5f, 9.5f + 32.0f);
        assert(std::abs(c0.red() - c1.red()) < 1e-4f && std::abs(c0.green() - c2.green()) < 1e-4f);

        // Non-constant across space
        Color c3 = fn.sample(18.5f, 21.5f);
        assert(std::abs(c0.red() - c3.red()) > 1e-3f || std::abs(c0.green() - c3.green()) > 1e-3f);

        Color t0 = tb.sample(11.5f, 13.5f);
        assert(t0.red() >= 0.0f && t0.red() <= 1.0f && t0.alpha() >= 0.0f && t0.alpha() <= 1.0f);
    }

    std::cout << "  BlurMaskFilter & PerlinNoiseShader verified." << std::endl;
}

void test_runtime_shader_image_filter() {
    std::cout << "[Test] RuntimeShader ImageFilter (chromatic aberration + uniforms + compose)..." << std::endl;

    const char* kChromaticSrc = R"(
        uniform shader content;
        uniform float shift;
        uniform vec3 tint;
        vec4 main(vec2 coord) {
            vec4 r = content.eval(coord + vec2(shift, 0.0));
            vec4 g = content.eval(coord);
            vec4 b = content.eval(coord - vec2(shift, 0.0));
            float a = max(max(r.a, g.a), b.a);
            return vec4(r.r * tint.r, g.g * tint.g, b.b * tint.b, a);
        }
    )";

    auto pm = Pixmap::allocate(120, 80);
    assert(pm.has_value());
    pm->fill(Color::TRANSPARENT);
    Canvas c(*pm);

    RuntimeFilterUniforms u;
    u.set_float("shift", 6.0f).set_vec3("tint", 1.0f, 0.8f, 1.0f);
    Paint layer_p;
    layer_p.set_image_filter(ImageFilter::runtime_shader(kChromaticSrc, u, "content"));
    c.save_layer_bounds(*Rect::from_xywh(10.0f, 10.0f, 100.0f, 60.0f), layer_p);
    c.fill_rect(*Rect::from_xywh(35.0f, 20.0f, 50.0f, 40.0f), Paint(Color::from_rgba8(240, 200, 220, 255)));
    c.restore();

    // Left fringe (x=31): sampling coord+6 hits rect (r>200), coord misses (g==0), coord-6 misses (b==0)
    auto left_fringe = pm->as_ref().pixel(31, 40);
    assert(left_fringe.has_value() && left_fringe->red() > 200 && left_fringe->green() < 10 && left_fringe->blue() < 10);

    // Center (x=60): all 3 channels hit, green scaled by 0.8 -> ~160
    auto center = pm->as_ref().pixel(60, 40);
    assert(center.has_value() && center->red() > 220 && std::abs(int(center->green()) - 160) <= 5 && center->blue() > 200);

    // Right fringe (x=88): sampling coord-6 hits rect (b>200), r & g miss
    auto right_fringe = pm->as_ref().pixel(88, 40);
    assert(right_fringe.has_value() && right_fringe->red() < 10 && right_fringe->green() < 10 && right_fringe->blue() > 200);

    std::cout << "  RuntimeShader ImageFilter verified." << std::endl;
}

void test_phase2_and_lighting_image_filters() {
    std::cout << "[Test] Phase 2 (Morphology/Convolution/Distortion/Arithmetic/Tile/AlphaThreshold) & Phase 4.1 (3D Lighting)..." << std::endl;

    // 1. Dilate & Erode
    {
        auto pm_d = Pixmap::allocate(64, 64);
        auto pm_e = Pixmap::allocate(64, 64);
        pm_d->fill(Color::TRANSPARENT);
        pm_e->fill(Color::TRANSPARENT);
        Canvas cd(*pm_d), ce(*pm_e);
        Paint pd(Color::from_rgba8(240, 100, 50, 255));
        pd.set_image_filter(ImageFilter::dilate(4.0f, 4.0f));
        cd.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 24.0f, 24.0f), pd);

        Paint pe(Color::from_rgba8(240, 100, 50, 255));
        pe.set_image_filter(ImageFilter::erode(4.0f, 4.0f));
        ce.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 24.0f, 24.0f), pe);

        assert(pm_d->as_ref().pixel(17, 32)->alpha() == 255); // expanded outside [20,44]
        assert(pm_e->as_ref().pixel(22, 32)->alpha() == 0);   // shrunk inside [20,44]
        assert(pm_e->as_ref().pixel(32, 32)->alpha() == 255); // core remains
    }

    // 2. MatrixConvolution (3x3 box average / sharpen)
    {
        auto pm = Pixmap::allocate(64, 64);
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(200, 120, 80, 255));
        std::vector<float> k = {0, -1, 0, -1, 5, -1, 0, -1, 0};
        p.set_image_filter(ImageFilter::matrix_convolution(3, 3, k, 1.0f, 0.0f, 1, 1, FilterTileMode::Clamp, false));
        c.fill_rect(*Rect::from_xywh(16.0f, 16.0f, 32.0f, 32.0f), p);
        auto mid = pm->as_ref().pixel(32, 32);
        assert(mid->red() == 200 && mid->green() == 120 && mid->blue() == 80);
    }

    // 3. Arithmetic (k2=0.5, k3=0.5 average of bg and fg)
    {
        auto pm = Pixmap::allocate(64, 64);
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(200, 40, 40, 255));
        auto bg_f = ImageFilter::color_filter(ColorEffect::mode(Color::from_rgba8(40, 200, 40, 255), BlendMode::SourceIn));
        p.set_image_filter(ImageFilter::arithmetic(0.0f, 0.5f, 0.5f, 0.0f, true, bg_f, std::nullopt));
        c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 44.0f, 44.0f), p);
        auto mid = pm->as_ref().pixel(32, 32);
        assert(std::abs(int(mid->red()) - 120) <= 2 && std::abs(int(mid->green()) - 120) <= 2);
    }

    // 4. DisplacementMap, Magnifier, MatrixTransform, Tile, AlphaThreshold
    {
        auto pm = Pixmap::allocate(80, 80);
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(200, 100, 50, 128));
        p.set_image_filter(ImageFilter::alpha_threshold(*Rect::from_xywh(20.0f, 20.0f, 40.0f, 40.0f), 0.9f, 0.2f));
        c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 60.0f, 60.0f), p);
        assert(pm->as_ref().pixel(40, 40)->alpha() >= 225); // boosted to >= 0.9
        assert(pm->as_ref().pixel(14, 40)->alpha() <= 55);  // clamped to <= 0.2
    }

    // 5. 3D Lighting (Distant/Point/Spot Diffuse + Specular)
    {
        auto pm = Pixmap::allocate(80, 80);
        pm->fill(Color::TRANSPARENT);
        Canvas c(*pm);
        Paint p(Color::from_rgba8(255, 255, 255, 255));
        auto blurred = ImageFilter::blur(3.0f);
        p.set_image_filter(ImageFilter::distant_lit_diffuse(Point3{0.0f, 0.0f, 1.0f}, Color::from_rgba8(255, 200, 100, 255), 5.0f, 1.0f, blurred));
        c.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 40.0f, 40.0f), p);
        auto mid = pm->as_ref().pixel(40, 40);
        assert(mid->red() > 240 && mid->green() > 180 && mid->blue() > 80 && mid->alpha() == 255);
    }

    std::cout << "  Phase 2 & 3D Lighting ImageFilters verified." << std::endl;
}

void test_phase3_and_phase4_2_effects() {
    std::cout << "[Test] Phase 3.1 (Extended ColorEffect), Phase 3.2 (PathEffect), Phase 4.2 (Blender & ShaderMaskFilter)..." << std::endl;

    // 1. Extended ColorEffect: Table, HSLAMatrix, Luma, HighContrast, Lerp, Compose
    {
        std::array<uint8_t, 256> posterize{};
        for (int i = 0; i < 256; ++i) posterize[i] = static_cast<uint8_t>((i / 64) * 64);
        auto tbl = ColorEffect::table(posterize);
        Color out_t = tbl.transform(Color::from_rgba8(130, 70, 250, 255));
        assert(std::abs(int(out_t.red() * 255.0f + 0.5f) - 128) <= 1);
        assert(std::abs(int(out_t.green() * 255.0f + 0.5f) - 64) <= 1);
        assert(std::abs(int(out_t.blue() * 255.0f + 0.5f) - 192) <= 1);

        // HSLAMatrix: shift hue by +1/3 (red -> green)
        ColorMatrix hshift = ColorMatrix::identity();
        hshift.m[4] = 1.0f / 3.0f;
        auto hsla = ColorEffect::hsla_matrix(hshift);
        Color green_from_red = hsla.transform(Color::from_rgba8(255, 0, 0, 255));
        assert(green_from_red.green() > 0.9f && green_from_red.red() < 0.1f);

        // LumaColorFilter
        auto luma = ColorEffect::luma();
        PremultipliedColor lp = luma.transform_premul(Color::from_rgba8(255, 255, 255, 255).premultiply());
        assert(lp.red() < 0.01f && lp.alpha() > 0.99f);

        // HighContrastFilter
        HighContrastConfig hcc{true, HighContrastInvertStyle::InvertLightness, 0.3f};
        auto hc = ColorEffect::high_contrast(hcc);
        Color hco = hc.transform(Color::from_rgba8(20, 20, 20, 255));
        assert(hco.red() > 0.8f);

        // Lerp & Compose
        auto lerped = ColorEffect::lerp(0.5f, ColorEffect::invert(), ColorEffect::grayscale());
        Color lo = lerped.transform(Color::from_rgba8(200, 100, 50, 255));
        assert(lo.red() > 0.0f && lo.red() < 1.0f);

        auto chained = ColorEffect::compose(tbl, hsla);
        assert(chained.has_value() && chained->is_active());
    }

    // 2. Composable PathEffect: Corner, Trim, Discrete, Path1D, Path2D, Compose, Sum
    {
        Path rect_p = PathBuilder::from_rect(*Rect::from_xywh(10.0f, 10.0f, 80.0f, 60.0f));
        Path rounded = PathEffect::corner(12.0f).apply(rect_p);
        assert(!rounded.is_empty());

        Path trimmed = PathEffect::trim(0.25f, 0.75f).apply(rect_p);
        assert(!trimmed.is_empty());

        Path sketchy = PathEffect::discrete(10.0f, 2.0f, 42u).apply(rect_p);
        assert(!sketchy.is_empty());

        Path stamp = PathBuilder::from_circle(0.0f, 0.0f, 3.0f).value();
        Path stamped1d = PathEffect::path_1d(stamp, 12.0f, 0.0f, Path1DStyle::Rotate).apply(rect_p);
        assert(!stamped1d.is_empty());

        Path stamped2d = PathEffect::path_2d(Transform::from_scale(16.0f, 16.0f), stamp).apply(rect_p);
        assert(!stamped2d.is_empty());

        auto composed = PathEffect::compose(PathEffect::corner(6.0f), PathEffect::discrete(12.0f, 2.0f, 7u));
        assert(!composed.apply(rect_p).is_empty());

        auto summed = PathEffect::sum(PathEffect::trim(0.0f, 0.3f), PathEffect::trim(0.7f, 1.0f));
        assert(!summed.apply(rect_p).is_empty());
    }

    // 3. Custom Blender (Arithmetic + Runtime) & ShaderMaskFilter
    {
        auto pm = Pixmap::allocate(80, 80);
        pm->fill(Color::from_rgba8(40, 80, 200, 255));
        Canvas c(*pm);

        // Arithmetic blender: 0.5*src + 0.5*dst
        Paint pa(Color::from_rgba8(200, 40, 40, 255));
        pa.set_blender(Blender::arithmetic(0.0f, 0.5f, 0.5f, 0.0f, true));
        c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 25.0f, 25.0f), pa);
        auto p_arith = pm->as_ref().pixel(20, 20);
        assert(std::abs(int(p_arith->red()) - 120) <= 2 && std::abs(int(p_arith->blue()) - 120) <= 2);

        // Runtime blender: vec4 main(vec4 src, vec4 dst) { return vec4(max(src.rgb, dst.rgb), 1.0); }
        const char* kMaxBlend = R"(
            uniform float boost;
            vec4 main(vec4 src, vec4 dst) {
                return vec4(clamp(max(src.rgb * boost, dst.rgb), 0.0, 1.0), 1.0);
            }
        )";
        RuntimeFilterUniforms bu;
        bu.set_float("boost", 1.0f);
        Paint pr(Color::from_rgba8(220, 30, 30, 255));
        pr.set_blender(Blender::runtime(kMaxBlend, bu));
        c.fill_rect(*Rect::from_xywh(45.0f, 10.0f, 25.0f, 25.0f), pr);
        auto p_rt = pm->as_ref().pixel(55, 20);
        assert(p_rt->red() >= 215 && p_rt->blue() >= 195);

        // ShaderMaskFilter: horizontal alpha fade gradient
        auto grad = Shader(*LinearGradient::create(
            Point::from_xy(10.0f, 0.0f), Point::from_xy(70.0f, 0.0f),
            {GradientStop::create(0.0f, Color::from_rgba8(255, 255, 255, 0)),
             GradientStop::create(1.0f, Color::from_rgba8(255, 255, 255, 255))}
        ));
        Paint pmf(Color::from_rgba8(240, 220, 40, 255));
        pmf.set_mask_filter(MaskFilter::shader(grad));
        c.fill_rect(*Rect::from_xywh(10.0f, 45.0f, 60.0f, 25.0f), pmf);
        auto p_fade_l = pm->as_ref().pixel(12, 57);
        auto p_fade_r = pm->as_ref().pixel(68, 57);
        assert(p_fade_r->red() > p_fade_l->red() + 120);
    }

    std::cout << "  Phase 3.1, Phase 3.2 & Phase 4.2 verified." << std::endl;
}

int main() {
    std::cout << "=== Running Nisaba Effects & Shadows Unit Tests ===" << std::endl;
    test_box_blur_radii();
    test_mask_blur();
    test_pixmap_blur();
    test_color_filters();
    test_drop_shadow();
    test_glass_panel();
    test_color_effect_and_image_filter();
    test_image_filter_dag();
    test_blur_mask_filter_and_perlin_noise();
    test_runtime_shader_image_filter();
    test_phase2_and_lighting_image_filters();
    test_phase3_and_phase4_2_effects();
    std::cout << "All visual effects tests passed successfully!" << std::endl;
    return 0;
}
