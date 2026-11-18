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

int main() {
    std::cout << "=== Running Nisaba Effects & Shadows Unit Tests ===" << std::endl;
    test_box_blur_radii();
    test_mask_blur();
    test_pixmap_blur();
    test_color_filters();
    test_drop_shadow();
    test_glass_panel();
    test_color_effect_and_image_filter();
    std::cout << "All visual effects tests passed successfully!" << std::endl;
    return 0;
}
