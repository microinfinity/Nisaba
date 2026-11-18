#include "nisaba/nisaba.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace nisaba;

void test_linear_gradient() {
    std::cout << "[TEST] Linear Gradient Shader... ";

    auto pixmap = Pixmap::create(100, 100);
    assert(pixmap.has_value());
    pixmap->fill(Color::WHITE);

    std::vector<GradientStop> stops = {
        GradientStop::create(0.0f, Color::from_rgba8(255, 0, 0, 255)),   // Red
        GradientStop::create(0.5f, Color::from_rgba8(0, 255, 0, 255)),   // Green
        GradientStop::create(1.0f, Color::from_rgba8(0, 0, 255, 255))    // Blue
    };

    auto grad = LinearGradient::create(
        Point::from_xy(10.0f, 10.0f),
        Point::from_xy(90.0f, 10.0f),
        stops,
        SpreadMode::Pad
    );
    assert(grad.has_value());

    Paint paint;
    paint.shader = Shader(*grad);
    paint.anti_alias = true;

    Canvas canvas(*pixmap);
    canvas.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 80.0f, 80.0f), paint);

    // Verify left pixel is predominantly red
    auto p_left = pixmap->pixel(11, 50);
    assert(p_left && p_left->red() > 200 && p_left->blue() < 50);

    // Verify right pixel is predominantly blue
    auto p_right = pixmap->pixel(89, 50);
    assert(p_right && p_right->blue() > 200 && p_right->red() < 50);

    std::cout << "PASSED" << std::endl;
}

void test_radial_gradient() {
    std::cout << "[TEST] Radial Gradient Shader... ";

    auto pixmap = Pixmap::create(100, 100);
    assert(pixmap.has_value());
    pixmap->fill(Color::WHITE);

    std::vector<GradientStop> stops = {
        GradientStop::create(0.0f, Color::from_rgba8(255, 255, 0, 255)), // Yellow center
        GradientStop::create(1.0f, Color::from_rgba8(255, 0, 0, 255))    // Red edge
    };

    auto grad = RadialGradient::create(
        Point::from_xy(50.0f, 50.0f),
        40.0f,
        stops,
        SpreadMode::Pad
    );
    assert(grad.has_value());

    Paint paint;
    paint.shader = Shader(*grad);
    Canvas canvas(*pixmap);
    canvas.fill_circle(50.0f, 50.0f, 40.0f, paint);

    // Center pixel should be yellow (red ~255, green ~255)
    auto p_center = pixmap->pixel(50, 50);
    assert(p_center && p_center->red() > 240 && p_center->green() > 240);

    // Outside circle should remain white
    auto p_corner = pixmap->pixel(5, 5);
    assert(p_corner && p_corner->red() == 255 && p_corner->green() == 255 && p_corner->blue() == 255);

    std::cout << "PASSED" << std::endl;
}

void test_canvas_transform_and_stroke() {
    std::cout << "[TEST] Canvas Transform Stack & Stroking... ";

    auto pixmap = Pixmap::create(200, 200);
    assert(pixmap.has_value());
    pixmap->fill(Color::from_rgba8(30, 30, 35, 255)); // Dark background

    Canvas canvas(*pixmap);

    // Draw rotated squares
    canvas.save();
    canvas.translate(100.0f, 100.0f);

    Paint fill_paint;
    fill_paint.set_color_rgba8(0, 180, 255, 180); // Cyan semi-transparent
    fill_paint.blend_mode = BlendMode::SourceOver;

    Stroke stroke(3.0f);
    stroke.line_cap = LineCap::Round;
    stroke.line_join = LineJoin::Round;

    Paint stroke_paint;
    stroke_paint.set_color_rgba8(255, 200, 50, 255); // Golden border

    for (int i = 0; i < 4; ++i) {
        canvas.rotate(45.0f);
        canvas.fill_rect(*Rect::from_xywh(-40.0f, -40.0f, 80.0f, 80.0f), fill_paint);
        canvas.stroke_rect(*Rect::from_xywh(-40.0f, -40.0f, 80.0f, 80.0f), stroke_paint, stroke);
    }
    canvas.restore();

    // Center pixel should not be background
    auto p_center = pixmap->pixel(100, 100);
    assert(p_center && (p_center->red() != 30 || p_center->green() != 30));

    bool saved = pixmap->save_bmp("test_canvas_art.bmp");
    assert(saved);

    std::cout << "PASSED (Saved test_canvas_art.bmp)" << std::endl;
}

void test_mask_and_pattern() {
    std::cout << "[TEST] Mask & Pattern Blending... ";

    // Create a 50x50 pattern pixmap (checkerboard)
    auto src_pix = Pixmap::create(50, 50);
    assert(src_pix.has_value());
    for (uint32_t y = 0; y < 50; ++y) {
        for (uint32_t x = 0; x < 50; ++x) {
            bool is_black = ((x / 10) + (y / 10)) % 2 == 0;
            src_pix->set_pixel(x, y, is_black ? PremultipliedColorU8::from_rgba_unchecked(0, 0, 0, 255)
                                              : PremultipliedColorU8::from_rgba_unchecked(255, 255, 255, 255));
        }
    }

    auto dst_pix = Pixmap::create(100, 100);
    assert(dst_pix.has_value());
    dst_pix->fill(Color::WHITE);

    Canvas canvas(*dst_pix);
    PixmapPaint pp;
    pp.quality = FilterQuality::Bilinear;
    pp.opacity = 0.8f;

    canvas.draw_pixmap(25, 25, src_pix->as_ref(), pp);

    // Now test mask creation and application
    auto mask = Mask::create(100, 100);
    assert(mask.has_value());
    // Draw a circular mask
    PathBuilder pb;
    pb.push_circle(50.0f, 50.0f, 30.0f);
    mask->fill_path(*pb.finish(), FillRule::Winding, true);

    canvas.apply_mask(*mask);

    // Outside the mask radius (e.g. at 5, 5), pixel should be completely transparent
    auto corner = dst_pix->pixel(5, 5);
    assert(corner && corner->alpha() == 0);

    bool saved = dst_pix->save_bmp("test_mask_pattern.bmp");
    assert(saved);

    std::cout << "PASSED (Saved test_mask_pattern.bmp)" << std::endl;
}

void test_save_layer() {
    std::cout << "Testing save_layer / save_layer_bounds / save_layer_backdrop... ";

    // 1. Opacity isolation + nested layer + bounded Clear confinement + ColorFilter
    auto pm = Pixmap::allocate(160, 160);
    assert(pm.has_value());
    Canvas canvas(*pm);
    canvas.clear(Color::from_rgba8(20, 40, 80, 255));

    // Overlapping rects inside 0.5-alpha layer:
    // If NOT isolated: dst gets (R over bg) then (G over that) -> green mixes with bg + red.
    // With isolated layer: green opaque rect covers red inside layer -> layer pixel at overlap is pure green (0,255,0,255),
    // then layer composites at 0.5 over bg (20, 40, 80) -> ~(10, 148, 40)!
    canvas.save_layer_alpha(0.5f);
    canvas.fill_rect(*Rect::from_xywh(10, 10, 80, 80), Paint(Color::from_rgba8(255, 0, 0, 255)));
    canvas.fill_rect(*Rect::from_xywh(30, 30, 80, 80), Paint(Color::from_rgba8(0, 255, 0, 255)));
    canvas.restore();

    auto overlap = pm->pixel(40, 40);
    assert(overlap.has_value());
    assert(overlap->red() < 20);
    assert(overlap->green() > 135 && overlap->green() < 160);
    assert(overlap->blue() > 30 && overlap->blue() < 50);

    // 2. Bounded Clear + ColorMatrix on layer
    {
        Paint layer_p(Color::from_rgba8(255, 255, 255, 255));
        layer_p.set_color_filter(effects::ColorMatrix::invert());
        canvas.save_layer_bounds(*Rect::from_xywh(100, 100, 40, 40), layer_p);
        canvas.fill_rect(*Rect::from_xywh(90, 90, 60, 60), Paint(Color::from_rgba8(255, 255, 0, 255)));
        canvas.restore();
    }
    auto in_b = pm->pixel(110, 110);
    auto out_b = pm->pixel(145, 145);
    assert(in_b.has_value() && out_b.has_value());
    // Invert yellow (255, 255, 0) -> blue (0, 0, 255)
    assert(in_b->red() < 5 && in_b->green() < 5 && in_b->blue() > 250);
    // Outside bounds [100..140] at (145,145) (still inside [90..150] draw rect): untouched background (20, 40, 80)
    assert(out_b->red() == 20 && out_b->green() == 40 && out_b->blue() == 80);

    // 3. Backdrop blur
    canvas.save_layer_backdrop(*Rect::from_xywh(25, 110, 60, 40), Paint(Color::WHITE), 4.0f);
    canvas.fill_rect(*Rect::from_xywh(35, 120, 10, 10), Paint(Color::from_rgba8(255, 0, 255, 255)));
    canvas.restore();
    auto bd_edge = pm->pixel(35, 111);
    assert(bd_edge.has_value());
    assert(bd_edge->green() > 60 && bd_edge->green() < 120); // Blurred green/bg bleed across y=110 edge into backdrop

    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running Nisaba Canvas, Shaders & Pipeline Tests ===" << std::endl;

    test_linear_gradient();
    test_radial_gradient();
    test_canvas_transform_and_stroke();
    test_mask_and_pattern();
    test_save_layer();

    std::cout << "All Canvas & Pipeline Tests Passed Successfully!" << std::endl;
    return 0;
}
