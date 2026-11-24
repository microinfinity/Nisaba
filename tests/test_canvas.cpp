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

static Pixmap make_nine_slice_pixmap(uint32_t w, uint32_t h, uint32_t border) {
    auto pm = Pixmap::create(w, h);
    assert(pm.has_value());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            bool left = x < border;
            bool right = x >= (w - border);
            bool top = y < border;
            bool bottom = y >= (h - border);
            Color c = Color::from_rgba8(40, 200, 120, 255); // Center: green
            if ((left || right) && (top || bottom)) {
                c = Color::from_rgba8(240, 60, 50, 255);    // Corners: red
            } else if (top || bottom) {
                c = Color::from_rgba8(50, 120, 240, 255);   // Top/Bottom edges: blue
            } else if (left || right) {
                c = Color::from_rgba8(240, 200, 40, 255);   // Left/Right edges: yellow
            }
            pm->set_pixel(x, y, c.premultiply().to_color_u8());
        }
    }
    return std::move(*pm);
}

void test_canvas_phase1_ergonomics_and_lattice() {
    std::cout << "Testing Phase 1: clip queries, save stack, draw_color/draw_paint, 9-slice & lattice... ";

    auto pm = Pixmap::allocate(240, 180);
    assert(pm.has_value());
    Canvas canvas(*pm);
    canvas.clear(Color::from_rgba8(15, 20, 30, 255));

    // --- 1.1 Clip queries & frustum culling ---
    assert(canvas.save_count() == 1);
    auto init_dev = canvas.get_device_clip_bounds();
    assert(init_dev.has_value() && init_dev->x() == 0 && init_dev->y() == 0 &&
           init_dev->width() == 240 && init_dev->height() == 180);
    auto init_loc = canvas.get_local_clip_bounds();
    assert(init_loc.has_value() && std::abs(init_loc->width() - 240.0f) < 1e-3f &&
           std::abs(init_loc->height() - 180.0f) < 1e-3f);

    const int s0 = canvas.save();
    assert(s0 == 1);
    assert(canvas.save_count() == 2);

    canvas.translate(20.0f, 10.0f);
    canvas.scale(2.0f, 2.0f);
    canvas.clip_rect(*Rect::from_xywh(10.0f, 5.0f, 40.0f, 30.0f));
    // Device rect: [20 + 20, 10 + 10, 20 + 100, 10 + 70] = [40, 20, 120, 80] (w=80, h=60)
    auto dev1 = canvas.get_device_clip_bounds();
    assert(dev1.has_value() && dev1->x() == 40 && dev1->y() == 20 &&
           dev1->width() == 80 && dev1->height() == 60);
    auto loc1 = canvas.get_local_clip_bounds();
    assert(loc1.has_value());
    assert(std::abs(loc1->left() - 10.0f) < 1e-3f && std::abs(loc1->top() - 5.0f) < 1e-3f);
    assert(std::abs(loc1->right() - 50.0f) < 1e-3f && std::abs(loc1->bottom() - 35.0f) < 1e-3f);

    assert(!canvas.quick_reject(*Rect::from_xywh(15.0f, 10.0f, 10.0f, 10.0f)));
    assert(canvas.quick_reject(*Rect::from_xywh(50.0f, 5.0f, 10.0f, 10.0f)));   // touches right edge
    assert(canvas.quick_reject(*Rect::from_xywh(-20.0f, -20.0f, 10.0f, 10.0f)));
    Path empty_p;
    assert(canvas.quick_reject(empty_p));
    auto far_circle = PathBuilder::from_circle(120.0f, 120.0f, 5.0f);
    auto near_circle = PathBuilder::from_circle(25.0f, 20.0f, 5.0f);
    assert(far_circle && canvas.quick_reject(*far_circle));
    assert(near_circle && !canvas.quick_reject(*near_circle));

    // Empty intersection clip
    const int s_empty = canvas.save();
    assert(s_empty == 2);
    canvas.clip_rect(*Rect::from_xywh(200.0f, 200.0f, 10.0f, 10.0f));
    assert(!canvas.get_device_clip_bounds().has_value());
    assert(!canvas.get_local_clip_bounds().has_value());
    assert(canvas.quick_reject(*Rect::from_xywh(0.0f, 0.0f, 500.0f, 500.0f)));
    canvas.restore_to_count(s_empty);
    assert(canvas.save_count() == 2);
    assert(canvas.get_device_clip_bounds().has_value());

    // Singular transform check
    canvas.save();
    canvas.scale(0.0f, 1.0f);
    assert(!canvas.get_local_clip_bounds().has_value());
    assert(canvas.quick_reject(*Rect::from_xywh(0.0f, 0.0f, 100.0f, 100.0f)));

    // --- 1.2 Save stack ergonomics & pivot rotate ---
    canvas.restore_to_count(s0);
    assert(canvas.save_count() == 1);
    canvas.restore_to_count(0);  // must clamp to 1 safely
    assert(canvas.save_count() == 1);
    canvas.restore_to_count(50); // no-op
    assert(canvas.save_count() == 1);

    // save_layer_alpha(std::optional<Rect>, float) + restore_to_count composites properly
    const int s_layer_base = canvas.save();
    const int s_layer_ret = canvas.save_layer_alpha(
        std::optional<Rect>(Rect::from_xywh(10.0f, 10.0f, 30.0f, 30.0f)), 0.5f);
    assert(s_layer_ret == 2);
    assert(canvas.save_count() == 3);
    canvas.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 30.0f, 30.0f), Paint(Color::from_rgba8(255, 0, 0, 255)));
    canvas.save();
    canvas.restore_to_count(s_layer_base);
    assert(canvas.save_count() == 1);
    auto lp = pm->pixel(20, 20);
    assert(lp.has_value() && lp->red() > 120 && lp->red() < 150);

    // Pivot rotation: rotate 90 deg around (100, 50) maps (120, 50) -> (100, 70)
    canvas.save();
    canvas.rotate(90.0f, 100.0f, 50.0f);
    canvas.fill_rect(*Rect::from_xywh(115.0f, 45.0f, 10.0f, 10.0f), Paint(Color::from_rgba8(250, 250, 250, 255)));
    canvas.restore();
    auto rot_px = pm->pixel(100, 70);
    auto orig_px = pm->pixel(120, 50);
    assert(rot_px.has_value() && rot_px->red() > 240);
    assert(orig_px.has_value() && orig_px->red() < 30);

    // --- 1.3 Full-clip fill (draw_color & draw_paint) ---
    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(10.0f, 60.0f, 60.0f, 40.0f));
    canvas.draw_color(Color::from_rgba8(200, 100, 40, 255), BlendMode::SourceOver);
    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(20.0f, 70.0f, 20.0f, 20.0f));
    canvas.draw_color(Color::TRANSPARENT, BlendMode::Clear);
    canvas.restore();
    canvas.restore();
    auto dc_in = pm->pixel(55, 80);
    auto dc_clr = pm->pixel(30, 80);
    auto dc_out = pm->pixel(5, 80);
    assert(dc_in && dc_in->red() == 200 && dc_in->green() == 100);
    assert(dc_clr && dc_clr->alpha() == 0);
    assert(dc_out && dc_out->red() == 15 && dc_out->blue() == 30);

    // draw_paint with rotated linear gradient covers entire clip
    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(140.0f, 10.0f, 80.0f, 60.0f));
    canvas.translate(180.0f, 40.0f);
    canvas.rotate(35.0f);
    auto grad = LinearGradient::create(
        Point::from_xy(-40.0f, 0.0f), Point::from_xy(40.0f, 0.0f),
        {GradientStop::create(0.0f, Color::from_rgba8(255, 0, 0, 255)),
         GradientStop::create(1.0f, Color::from_rgba8(0, 0, 255, 255))});
    assert(grad.has_value());
    canvas.draw_paint(Paint(Shader(*grad)));
    canvas.restore();
    auto gp_tl = pm->pixel(142, 12);
    auto gp_br = pm->pixel(218, 68);
    assert(gp_tl && gp_tl->red() > 180 && gp_tl->blue() < 80);
    assert(gp_br && gp_br->blue() > 180 && gp_br->red() < 80);

    // --- 1.4 9-Slice & Lattice image scaling ---
    Pixmap nine_src = make_nine_slice_pixmap(16, 16, 4);
    auto center = *IntRect::from_ltrb(4, 4, 12, 12);
    PixmapPaint pp;
    pp.quality = FilterQuality::Bilinear;
    canvas.draw_image_nine(nine_src.as_ref(), center, *Rect::from_xywh(10.0f, 110.0f, 80.0f, 60.0f), pp);
    // Corners must stay exact 4x4 red, top edge blue, left edge yellow, center green
    auto c_tl = pm->pixel(11, 111);
    auto c_br = pm->pixel(88, 168);
    auto e_top = pm->pixel(50, 111);
    auto e_left = pm->pixel(11, 140);
    auto mid = pm->pixel(50, 140);
    assert(c_tl && c_tl->red() > 230 && c_tl->green() < 70);
    assert(c_br && c_br->red() > 230 && c_br->green() < 70);
    assert(e_top && e_top->blue() > 230 && e_top->red() < 70);
    assert(e_left && e_left->red() > 230 && e_left->green() > 190);
    assert(mid && mid->green() > 190 && mid->red() < 60);

    // 1px center 9-slice stretched must have ZERO border bleed
    Pixmap nine_1px = make_nine_slice_pixmap(9, 9, 4);
    canvas.draw_image_nine(nine_1px.as_ref(), *IntRect::from_ltrb(4, 4, 5, 5),
                           *Rect::from_xywh(100.0f, 110.0f, 50.0f, 30.0f), pp);
    auto mid1 = pm->pixel(105, 125); // right next to left 4px border!
    assert(mid1 && mid1->green() > 190 && mid1->red() < 50);

    // Destination smaller than fixed borders scales corners proportionally
    canvas.draw_image_nine(nine_src.as_ref(), center, *Rect::from_xywh(100.0f, 145.0f, 6.0f, 6.0f), pp);
    auto small_c = pm->pixel(102, 147);
    assert(small_c && small_c->red() > 220);

    // General Lattice with Transparent center + FixedColor corner
    std::array<int32_t, 2> xdivs{4, 12};
    std::array<int32_t, 2> ydivs{4, 12};
    std::array<Lattice::RectType, 9> rtypes{
        Lattice::RectType::FixedColor, Lattice::RectType::Default,     Lattice::RectType::Default,
        Lattice::RectType::Default,    Lattice::RectType::Transparent, Lattice::RectType::Default,
        Lattice::RectType::Default,    Lattice::RectType::Default,     Lattice::RectType::Default
    };
    std::array<Color, 9> rcolors{};
    rcolors[0] = Color::from_rgba8(255, 0, 255, 255);
    Lattice lat;
    lat.x_divs = xdivs;
    lat.y_divs = ydivs;
    lat.rect_types = rtypes;
    lat.colors = rcolors;
    canvas.draw_image_lattice(nine_src.as_ref(), lat, *Rect::from_xywh(160.0f, 110.0f, 60.0f, 50.0f), pp);
    auto lat_fc = pm->pixel(162, 112);
    auto lat_tr = pm->pixel(190, 135);
    assert(lat_fc && lat_fc->red() > 240 && lat_fc->blue() > 240 && lat_fc->green() < 10);
    assert(lat_tr && lat_tr->red() == 15 && lat_tr->green() == 20 && lat_tr->blue() == 30);

    std::cout << "PASSED" << std::endl;
}

static Pixmap make_atlas_pixmap() {
    auto pm = Pixmap::create(32, 32).value();
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            Color c = Color::WHITE;
            if (x < 16 && y < 16)       c = Color::from_rgba8(240, 60, 50, 255);  // TL: red
            else if (x >= 16 && y < 16) c = Color::from_rgba8(50, 220, 90, 255);  // TR: green
            else if (x < 16 && y >= 16) c = Color::from_rgba8(55, 130, 245, 255); // BL: blue
            else                        c = Color::from_rgba8(245, 215, 50, 255); // BR: yellow
            pm.set_pixel(x, y, c.premultiply().to_color_u8());
        }
    }
    return pm;
}

void test_canvas_phase2_clipping_atlas_and_coons() {
    std::cout << "Testing Phase 2: Difference clip, clip_shader, draw_atlas & Coons/GradientMesh... ";

    auto pm = Pixmap::allocate(280, 220);
    assert(pm.has_value());
    Canvas canvas(*pm);
    canvas.clear(Color::from_rgba8(16, 22, 34, 255));

    // --- 2.1 Difference Clipping (spotlight oval cutout + nested difference + restore) ---
    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(10.0f, 10.0f, 80.0f, 80.0f), ClipOp::Intersect);
    canvas.clip_oval(*Rect::from_xywh(30.0f, 30.0f, 40.0f, 40.0f), ClipOp::Difference);
    canvas.save();
    canvas.clip_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(14.0f, 14.0f, 20.0f, 20.0f), 4.0f), ClipOp::Difference);
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 100.0f, 100.0f), Paint(Color::from_rgba8(230, 90, 50, 255)));
    canvas.restore();
    // After restoring inner RRect cutout, (22, 22) is visible again while oval center (50, 50) stays cut out!
    canvas.fill_rect(*Rect::from_xywh(14.0f, 14.0f, 50.0f, 50.0f), Paint(Color::from_rgba8(60, 210, 140, 255)));
    canvas.restore();

    auto p_hole = pm->pixel(50, 50);
    auto p_restored = pm->pixel(22, 22);
    auto p_outer = pm->pixel(80, 80);
    assert(p_hole && p_hole->red() == 16 && p_hole->green() == 22 && p_hole->blue() == 34);
    assert(p_restored && p_restored->green() > 200 && p_restored->red() < 80);
    assert(p_outer && p_outer->red() > 220 && p_outer->green() < 110);

    // --- 2.2 Shader-Based Clipping (clip_shader Intersect & Difference) ---
    canvas.save();
    auto fade_grad = LinearGradient::create(
        Point::from_xy(100.0f, 0.0f), Point::from_xy(180.0f, 0.0f),
        {GradientStop::create(0.0f, Color::from_rgba8(255, 255, 255, 0)),
         GradientStop::create(1.0f, Color::from_rgba8(255, 255, 255, 255))});
    assert(fade_grad.has_value());
    canvas.clip_rect(*Rect::from_xywh(100.0f, 10.0f, 80.0f, 40.0f));
    canvas.clip_shader(Shader(*fade_grad), ClipOp::Intersect);
    canvas.fill_rect(*Rect::from_xywh(100.0f, 10.0f, 80.0f, 40.0f), Paint(Color::from_rgba8(250, 250, 250, 255)));
    canvas.restore();

    auto sf_l = pm->pixel(102, 30);
    auto sf_m = pm->pixel(140, 30);
    auto sf_r = pm->pixel(178, 30);
    assert(sf_l && sf_m && sf_r);
    assert(sf_l->red() < 35 && sf_m->red() > 110 && sf_m->red() < 165 && sf_r->red() > 230);

    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(100.0f, 55.0f, 80.0f, 35.0f));
    canvas.clip_shader(Shader(*fade_grad), ClipOp::Difference);
    canvas.fill_rect(*Rect::from_xywh(100.0f, 55.0f, 80.0f, 35.0f), Paint(Color::from_rgba8(250, 250, 250, 255)));
    canvas.restore();
    auto sfd_l = pm->pixel(102, 72);
    auto sfd_r = pm->pixel(178, 72);
    assert(sfd_l && sfd_l->red() > 230);
    assert(sfd_r && sfd_r->red() < 35);

    // --- 2.3 RSXform & draw_atlas ---
    Pixmap atlas = make_atlas_pixmap();
    std::array<RSXform, 3> xforms{
        RSXform::from_degrees(1.5f, 0.0f, 195.0f, 15.0f),
        RSXform::from_degrees(1.5f, 45.0f, 245.0f, 30.0f, 8.0f, 8.0f),
        RSXform::from_degrees(1.5f, 0.0f, 195.0f, 55.0f)
    };
    std::array<Rect, 3> tex_rects{
        *Rect::from_xywh(0.0f, 0.0f, 16.0f, 16.0f),
        *Rect::from_xywh(16.0f, 0.0f, 16.0f, 16.0f),
        *Rect::from_xywh(16.0f, 16.0f, 16.0f, 16.0f)
    };
    std::array<Color, 3> cols{
        Color::WHITE,
        Color::from_rgba8(255, 255, 255, 255),
        Color::from_rgba8(255, 128, 255, 255)
    };
    canvas.draw_atlas(atlas.as_ref(), xforms, tex_rects, cols, BlendMode::Modulate);
    auto a0 = pm->pixel(207, 27);
    auto a1 = pm->pixel(245, 30);
    auto a2 = pm->pixel(207, 67);
    assert(a0 && a0->red() > 220 && a0->green() < 70);
    assert(a1 && a1->green() > 200 && a1->red() < 70);
    assert(a2 && a2->red() > 220 && a2->green() > 95 && a2->green() < 130);

    // --- 2.4 Adaptive Coons Patch & GradientMesh + Clipped draw_vertices ---
    CoonsPatch patch;
    patch.top[0] = {20, 115};    patch.top[1] = {55, 100};    patch.top[2] = {95, 130};    patch.top[3] = {130, 115};
    patch.right[0] = {130, 115}; patch.right[1] = {145, 145}; patch.right[2] = {115, 175}; patch.right[3] = {130, 200};
    patch.bottom[0] = {20, 200}; patch.bottom[1] = {55, 212}; patch.bottom[2] = {95, 188}; patch.bottom[3] = {130, 200};
    patch.left[0] = {20, 115};   patch.left[1] = {8, 145};    patch.left[2] = {32, 175};   patch.left[3] = {20, 200};
    patch.color_top_left = Color::from_rgba8(255, 50, 50, 255);
    patch.color_top_right = Color::from_rgba8(50, 255, 80, 255);
    patch.color_bottom_right = Color::from_rgba8(50, 100, 255, 255);
    patch.color_bottom_left = Color::from_rgba8(255, 230, 40, 255);

    auto [su, sv] = patch.compute_adaptive_subdivision(Transform::identity());
    assert(su >= 4 && sv >= 4);

    canvas.save();
    canvas.clip_oval(*Rect::from_xywh(60.0f, 145.0f, 30.0f, 30.0f), ClipOp::Difference);
    canvas.draw_coons_patch(patch);
    canvas.restore();

    auto cm_tl = pm->pixel(32, 125);
    auto cm_hole = pm->pixel(75, 160);
    assert(cm_tl && cm_tl->red() > 180);
    assert(cm_hole && cm_hole->red() == 16 && cm_hole->green() == 22 && cm_hole->blue() == 34);

    std::cout << "PASSED" << std::endl;
}

class TestBadgeDrawable final : public Drawable {
public:
    Color badge_color{Color::from_rgba8(245, 180, 40, 255)};

protected:
    Rect on_get_bounds() const override {
        return *Rect::from_xywh(0.0f, 0.0f, 24.0f, 24.0f);
    }
    void on_draw(ICanvas& c) override {
        c.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(0.0f, 0.0f, 24.0f, 24.0f), 6.0f), Paint(badge_color));
    }
};

void test_canvas_phase3_picture_4x4_and_region() {
    std::cout << "Testing Phase 3: Picture/Drawable, 4x4 projective stack & Region algebra... ";

    // --- 3.3 Region set algebra unit checks ---
    Region rA(*IntRect::from_xywh(10, 10, 40, 40));
    Region rB(*IntRect::from_xywh(30, 30, 40, 40));
    Region rU;
    assert(rU.op(rA, rB, RegionOp::Union));
    assert(rU.is_complex());
    assert(rU.contains(15, 15) && rU.contains(40, 40) && rU.contains(65, 65));
    assert(!rU.contains(65, 15) && !rU.contains(15, 65));

    // Coalescing check: two horizontally adjacent equal-height rects merge into 1 rect!
    Region rAdj(*IntRect::from_xywh(0, 0, 20, 30));
    rAdj.union_with(*IntRect::from_xywh(20, 0, 20, 30));
    assert(rAdj.is_rect() && rAdj.bounds() == *IntRect::from_xywh(0, 0, 40, 30));
    // Vertically adjacent equal-width rects also coalesce into 1 rect!
    rAdj.union_with(*IntRect::from_xywh(0, 30, 40, 20));
    assert(rAdj.is_rect() && rAdj.bounds() == *IntRect::from_xywh(0, 0, 40, 50));

    Region rI;
    assert(rI.op(rA, rB, RegionOp::Intersect));
    assert(rI.is_rect() && rI.bounds() == *IntRect::from_xywh(30, 30, 20, 20));

    Region rX;
    assert(rX.op(rA, rB, RegionOp::Xor));
    assert(rX.contains(15, 15) && rX.contains(65, 65) && !rX.contains(40, 40));

    Region rD;
    assert(rD.op(rA, rB, RegionOp::Difference));
    assert(rD.contains(15, 15) && !rD.contains(40, 40) && !rD.contains(65, 65));

    // --- Canvas rendering of 3.1 + 3.2 + 3.3 ---
    auto pm = Pixmap::allocate(300, 220);
    assert(pm.has_value());
    Canvas canvas(*pm);
    canvas.clear(Color::from_rgba8(14, 20, 32, 255));

    // 3.1 PictureRecorder + Picture playback + dynamic Drawable + owned Pixmap lifetime safety
    auto badge = std::make_shared<TestBadgeDrawable>();
    std::shared_ptr<Picture> card_pic;
    {
        auto temp_icon = Pixmap::create(8, 8).value();
        temp_icon.fill(Color::from_rgba8(60, 230, 180, 255));

        PictureRecorder rec;
        ICanvas& rc = rec.begin_recording(*Rect::from_xywh(0.0f, 0.0f, 80.0f, 60.0f));
        assert(rc.canvas_kind() == CanvasKind::Picture);
        rc.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(0.0f, 0.0f, 80.0f, 60.0f), 8.0f),
                      Paint(Color::from_rgba8(40, 90, 210, 255)));
        rc.draw_pixmap(8, 8, temp_icon.as_ref());
        const Transform badge_t = Transform::from_translate(48.0f, 18.0f);
        rc.draw_drawable(badge, &badge_t);
        card_pic = rec.finish_recording();
    } // temp_icon destroyed here — Picture owns a deep copy!
    assert(card_pic && card_pic->approximate_op_count() >= 3);

    const Transform p1_t = Transform::from_translate(10.0f, 10.0f);
    canvas.draw_picture(card_pic, &p1_t);
    // Mutate dynamic Drawable and replay with 0.5 alpha Paint layer!
    badge->badge_color = Color::from_rgba8(240, 60, 120, 255);
    badge->notify_drawing_changed();
    const Transform p2_t = Transform::from_translate(10.0f, 80.0f);
    Paint half_alpha(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, 0.5f));
    canvas.draw_picture(card_pic, &p2_t, &half_alpha);

    auto pic1_bg = pm->pixel(30, 40);
    auto pic1_icon = pm->pixel(22, 22);
    auto pic1_badge = pm->pixel(70, 40);
    auto pic2_badge = pm->pixel(70, 110);
    assert(pic1_bg && pic1_bg->blue() > 190);
    assert(pic1_icon && pic1_icon->green() > 210);
    assert(pic1_badge && pic1_badge->red() > 230 && pic1_badge->green() > 165); // gold on replay 1
    assert(pic2_badge && pic2_badge->red() > 115 && pic2_badge->green() < 60);  // pink @ 0.5 on replay 2

    // 3.2 Full 4x4 projective matrix stack (camera orbit perspective tilt)
    canvas.save();
    Transform4x4 orbit = Transform4x4::from_camera_orbit(160.0f, 65.0f, 12.0f, 32.0f, 0.0f, 350.0f);
    canvas.concat(orbit);
    assert(canvas.current_transform_4x4().is_planar_perspective());
    assert(!canvas.quick_reject(*Rect::from_xywh(120.0f, 25.0f, 80.0f, 80.0f)));
    canvas.fill_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(120.0f, 25.0f, 80.0f, 80.0f), 10.0f),
                      Paint(Color::from_rgba8(230, 85, 65, 255)));
    Stroke st; st.width = 4.0f;
    canvas.stroke_rect(*Rect::from_xywh(120.0f, 25.0f, 80.0f, 80.0f), Paint(Color::WHITE), st);
    canvas.restore();
    assert(!canvas.current_transform_4x4().is_planar_perspective());
    auto p3d_center = pm->pixel(160, 65);
    assert(p3d_center && p3d_center->red() > 210 && p3d_center->green() < 100);

    // 3.3 draw_region & device-space clip_region (unaffected by CTM!)
    Region clip_rgn(*IntRect::from_xywh(215, 15, 70, 70));
    clip_rgn.difference_with(*IntRect::from_xywh(235, 35, 30, 30));
    canvas.save();
    canvas.translate(50.0f, 50.0f); // must NOT shift clip_region!
    canvas.clip_region(clip_rgn, ClipOp::Intersect);
    canvas.draw_color(Color::from_rgba8(55, 215, 145, 255));
    canvas.restore();

    auto rgn_in = pm->pixel(222, 22);
    auto rgn_hole = pm->pixel(250, 50);
    assert(rgn_in && rgn_in->green() > 200);
    assert(rgn_hole && rgn_hole->red() == 14 && rgn_hole->green() == 20 && rgn_hole->blue() == 32);

    Region draw_rgn(*IntRect::from_xywh(120, 130, 70, 50));
    draw_rgn.xor_with(*IntRect::from_xywh(155, 150, 70, 50));
    canvas.draw_region(draw_rgn, Paint(Color::from_rgba8(170, 90, 245, 255)));
    auto dr_a = pm->pixel(135, 145);
    auto dr_xor_hole = pm->pixel(170, 165);
    auto dr_b = pm->pixel(205, 185);
    assert(dr_a && dr_a->blue() > 230);
    assert(dr_xor_hole && dr_xor_hole->red() == 14 && dr_xor_hole->blue() == 32);
    assert(dr_b && dr_b->blue() > 230);

    std::cout << "PASSED" << std::endl;
}

static std::string resolve_canvas_test_font(std::string_view filename) {
    const std::string candidates[3] = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename)
    };
    for (const auto& p : candidates) {
        if (auto font = text::TtfFont::from_file(p); font && font->is_valid()) {
            return p;
        }
    }
    return candidates[0];
}

void test_canvas_phase4_glyphs_textblob_and_annotations() {
    std::cout << "  Testing Phase 4: draw_glyphs (pos & RSXform), TextBlob, annotations & discard... ";

    auto font = text::TtfFont::from_file(resolve_canvas_test_font("NotoSans-Regular.ttf"));
    assert(font && font->is_valid());

    auto pm = Pixmap::allocate(320, 220);
    assert(pm.has_value());
    Canvas canvas(*pm);
    canvas.clear(Color::from_rgba8(12, 16, 26, 255));

    // 1. Positioned draw_glyphs (solid + gradient)
    const std::string_view word = "NISABA";
    std::vector<uint16_t> gids;
    std::vector<Point> positions;
    float pen_x = 20.0f;
    for (char ch : word) {
        uint16_t gid = font->glyph_index(static_cast<char32_t>(ch));
        gids.push_back(gid);
        positions.emplace_back(pen_x, 48.0f);
        pen_x += font->glyph_advance(gid, 28.0f) + 2.0f;
    }
    canvas.draw_glyphs(gids, positions, *font, 28.0f, Paint(Color::from_rgba8(245, 245, 250, 255)));

    // 2. RSXform curved arc draw_glyphs
    const std::string_view arc_str = "CURVED TEXT";
    std::vector<uint16_t> arc_gids;
    std::vector<RSXform> arc_xforms;
    const float cx = 160.0f, cy = 175.0f, radius = 65.0f;
    const float start_ang = -2.35f;
    const float step_ang = 0.16f;
    for (size_t i = 0; i < arc_str.size(); ++i) {
        arc_gids.push_back(font->glyph_index(static_cast<char32_t>(arc_str[i])));
        const float ang = start_ang + static_cast<float>(i) * step_ang;
        const float tangent = ang + 1.5707963f;
        const float px = cx + std::cos(ang) * radius;
        const float py = cy + std::sin(ang) * radius;
        arc_xforms.push_back(RSXform::from_radians(1.0f, tangent, px, py, 0.0f, 0.0f));
    }
    canvas.draw_glyphs(arc_gids, arc_xforms, *font, 18.0f, Paint(Color::from_rgba8(56, 189, 248, 255)));

    // 3. Immutable multi-run TextBlob via TextBlobBuilder & TextBlob::from_text
    auto simple_blob = TextBlob::from_text("Engine 2026", *font, 20.0f);
    assert(simple_blob && !simple_blob->is_empty());
    assert(simple_blob->unique_id() != 0);
    assert(simple_blob->glyph_count() == 11);
    assert(!simple_blob->bounds().is_empty());

    TextBlobBuilder builder;
    std::vector<float> h_xs;
    float hx = 0.0f;
    for (uint16_t g : gids) {
        h_xs.push_back(hx);
        hx += font->glyph_advance(g, 20.0f);
    }
    builder.add_run_pos_h(*font, 20.0f, gids, h_xs, 0.0f);
    builder.add_run_rsxform(*font, 14.0f, arc_gids, arc_xforms);
    auto multi_blob = builder.make();
    assert(multi_blob && multi_blob->runs().size() == 2);
    assert(multi_blob->unique_id() != simple_blob->unique_id());

    std::vector<GradientStop> grad_stops = {
        GradientStop::create(0.0f, Color::from_rgba8(250, 204, 21, 255)),
        GradientStop::create(1.0f, Color::from_rgba8(244, 63, 94, 255))
    };
    auto lg = LinearGradient::create(Point(20.0f, 80.0f), Point(260.0f, 80.0f), grad_stops, SpreadMode::Pad);
    assert(lg.has_value());
    Paint grad_paint;
    grad_paint.shader = Shader(*lg);
    canvas.draw_text_blob(simple_blob, 20.0f, 92.0f, grad_paint);

    // Verify rendered glyph pixels across all 3 regions
    auto count_lit = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        size_t lit = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                auto p = pm->pixel(x, y);
                if (p && (p->red() > 40 || p->green() > 40 || p->blue() > 60)) ++lit;
            }
        }
        return lit;
    };
    assert(count_lit(15, 18, 220, 56) > 120);   // positioned glyphs
    assert(count_lit(15, 68, 240, 102) > 80);   // gradient TextBlob
    assert(count_lit(70, 95, 250, 185) > 80);   // RSXform arc glyphs

    // 4. Picture recording of TextBlob + draw_annotation + discard
    PictureRecorder rec;
    ICanvas& rc = rec.begin_recording(*Rect::from_xywh(0.0f, 0.0f, 320.0f, 220.0f));
    rc.draw_text_blob(simple_blob, 160.0f, 92.0f, Paint(Color::from_rgba8(52, 211, 153, 255)));
    rc.draw_annotation(*Rect::from_xywh(160.0f, 70.0f, 120.0f, 26.0f), "URL", "https://nisaba.dev");
    auto pic = rec.finish_recording();
    assert(pic && pic->approximate_op_count() == 2);
    canvas.draw_picture(pic);

    canvas.draw_annotation(*Rect::from_xywh(0.0f, 0.0f, 10.0f, 10.0f), "Tag", "Test");
    canvas.discard();

    std::cout << "PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running Nisaba Canvas, Shaders & Pipeline Tests ===" << std::endl;

    test_linear_gradient();
    test_radial_gradient();
    test_canvas_transform_and_stroke();
    test_mask_and_pattern();
    test_save_layer();
    test_canvas_phase1_ergonomics_and_lattice();
    test_canvas_phase2_clipping_atlas_and_coons();
    test_canvas_phase3_picture_4x4_and_region();
    test_canvas_phase4_glyphs_textblob_and_annotations();

    std::cout << "All Canvas & Pipeline Tests Passed Successfully!" << std::endl;
    return 0;
}
