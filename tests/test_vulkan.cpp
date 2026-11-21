#include <cassert>
#include <iostream>
#include <cmath>
#include <fstream>
#include <vector>
#include <utility>
#include "nisaba/nisaba.hpp"
#include "nisaba/gpu/vulkan_device.hpp"
#include "nisaba/gpu/vulkan_surface.hpp"
#include "nisaba/gpu/vulkan_renderer.hpp"
#include "nisaba/svg/svg_document.hpp"
#include "nisaba/text/buffer.hpp"
#include "nisaba/text/font_system.hpp"

using namespace nisaba;
using nisaba::gpu::VulkanDevice;
using nisaba::gpu::VulkanSurface;
using nisaba::gpu::GpuCanvas;
using nisaba::gpu::GpuBackendType;

namespace {
const Color RED = Color::from_rgba8(255, 0, 0, 255);
const Color GREEN = Color::from_rgba8(0, 255, 0, 255);
const Color BLUE = Color::from_rgba8(0, 0, 255, 255);
const Color YELLOW = Color::from_rgba8(255, 255, 0, 255);
const Color CYAN = Color::from_rgba8(0, 255, 255, 255);
const Color MAGENTA = Color::from_rgba8(255, 0, 255, 255);
const Color BG_COLOR = Color::from_rgba8(25, 30, 45, 255);

std::string resolve_vk_test_font(std::string_view filename) {
    for (const char* prefix : {"fonts/", "../fonts/", "../../fonts/"}) {
        std::string p = std::string(prefix) + std::string(filename);
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return p;
    }
    return std::string("fonts/") + std::string(filename);
}

template <typename DrawFn>
void verify_vulkan_parity(const std::shared_ptr<VulkanDevice>& device,
                          uint32_t w, uint32_t h,
                          Color bg,
                          const char* label,
                          DrawFn&& draw_fn,
                          const std::vector<std::pair<uint32_t, uint32_t>>& pts,
                          int pt_tol = 8,
                          float max_avg_diff = 4.5f,
                          uint32_t vk_samples = 1) {
    auto cpu_pm = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
    assert(cpu_pm.has_value());
    cpu_pm->fill(bg);
    {
        Canvas cpu(cpu_pm->as_mut());
        draw_fn(cpu, false);
    }

    auto surface = VulkanSurface::create(device, w, h, vk_samples);
    assert(surface != nullptr);
    {
        GpuCanvas vk(surface);
        vk.clear(bg);
        draw_fn(vk, true);
        vk.flush();
    }
    auto vk_pm = surface->to_pixmap();
    assert(vk_pm.has_value());

    const auto cpu_r = cpu_pm->as_ref();
    const auto vk_r = vk_pm->as_ref();
    for (auto [px, py] : pts) {
        auto cp = cpu_r.pixel(px, py);
        auto vp = vk_r.pixel(px, py);
        assert(cp.has_value() && vp.has_value());
        int dr = std::abs(int(cp->red()) - int(vp->red()));
        int dg = std::abs(int(cp->green()) - int(vp->green()));
        int db = std::abs(int(cp->blue()) - int(vp->blue()));
        int da = std::abs(int(cp->alpha()) - int(vp->alpha()));
        if (dr > pt_tol || dg > pt_tol || db > pt_tol || da > pt_tol) {
            std::cerr << "  [VK MISMATCH] " << label << " @(" << px << "," << py << ") "
                      << "CPU=(" << int(cp->red()) << "," << int(cp->green()) << "," << int(cp->blue()) << "," << int(cp->alpha()) << ") "
                      << "VK=(" << int(vp->red()) << "," << int(vp->green()) << "," << int(vp->blue()) << "," << int(vp->alpha()) << ")\n";
            assert(false);
        }
    }

    double sum_diff = 0.0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            auto cp = cpu_r.pixel(x, y);
            auto vp = vk_r.pixel(x, y);
            int dr = std::abs(int(cp->red()) - int(vp->red()));
            int dg = std::abs(int(cp->green()) - int(vp->green()));
            int db = std::abs(int(cp->blue()) - int(vp->blue()));
            int da = std::abs(int(cp->alpha()) - int(vp->alpha()));
            sum_diff += (dr + dg + db + da) * 0.25;
        }
    }
    double avg_diff = sum_diff / double(w * h);
    if (avg_diff > max_avg_diff) {
        std::cerr << "  [VK AVG MISMATCH] " << label << " avg_diff=" << avg_diff << " > " << max_avg_diff << "\n";
        assert(false);
    }
}
} // namespace

void test_vulkan_device_init() {
    std::cout << "Testing Vulkan device initialization..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr);
    assert(device->is_valid());
    assert(device->backend_type() == GpuBackendType::Vulkan);

    std::cout << "  -> Vulkan Device Name:    " << device->renderer_name() << std::endl;
    std::cout << "  -> Vulkan API Version:    " << device->version_name() << std::endl;
    std::cout << "  -> Graphics Queue Family: " << device->graphics_queue_family() << std::endl;

    assert(device->instance() != VK_NULL_HANDLE);
    assert(device->device() != VK_NULL_HANDLE);
    assert(device->graphics_queue() != VK_NULL_HANDLE);

    std::cout << "  -> Vulkan device initialization OK" << std::endl;
}

void test_vulkan_surface_and_canvas() {
    std::cout << "Testing Vulkan surface allocation and GpuCanvas API..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr && device->is_valid());

    auto surface = VulkanSurface::create(device, 400, 300, 4); // 4x MSAA
    assert(surface != nullptr);
    assert(surface->width() == 400);
    assert(surface->height() == 300);
    assert(surface->samples() >= 4);
    assert(surface->backend_type() == GpuBackendType::Vulkan);
    assert(surface->render_pass() != VK_NULL_HANDLE);
    assert(surface->framebuffer() != VK_NULL_HANDLE);

    GpuCanvas canvas(surface);
    assert(canvas.width() == 400);
    assert(canvas.height() == 300);

    // 1. Clear with solid background
    canvas.clear(BG_COLOR);

    // 2. Transform stack
    canvas.save();
    canvas.translate(40.0f, 30.0f);
    canvas.scale(1.2f, 1.2f);
    canvas.rotate(15.0f);
    assert(!canvas.transform().is_identity());
    canvas.restore();
    assert(canvas.transform().is_identity());

    // 3. Scissor clipping
    auto clip_r = Rect::from_xywh(10.0f, 10.0f, 380.0f, 280.0f);
    assert(clip_r.has_value());
    canvas.clip_rect(*clip_r);
    assert(canvas.scissor_clip().has_value());
    canvas.reset_clip();

    // 4. Draw basic primitives
    Paint solid_red(RED);
    auto rect1 = Rect::from_xywh(20.0f, 20.0f, 80.0f, 60.0f);
    assert(rect1.has_value());
    canvas.fill_rect(*rect1, solid_red);

    Stroke stroke;
    stroke.width = 3.0f;
    Paint solid_green(GREEN);
    canvas.stroke_rect(*rect1, solid_green, stroke);

    // 5. Rounded rects and circles
    Paint solid_blue(BLUE);
    auto rrect = Rect::from_xywh(120.0f, 20.0f, 100.0f, 60.0f);
    assert(rrect.has_value());
    canvas.fill_round_rect(*rrect, 12.0f, 12.0f, solid_blue);
    canvas.stroke_round_rect(*rrect, 12.0f, 12.0f, solid_red, stroke);

    canvas.fill_circle(280.0f, 60.0f, 30.0f, solid_green);
    canvas.stroke_circle(280.0f, 60.0f, 30.0f, solid_blue, stroke);

    canvas.stroke_line(10.0f, 110.0f, 390.0f, 110.0f, solid_red, stroke);

    // 6. Linear Gradient with analytical SDF
    std::vector<GradientStop> stops = {
        GradientStop::create(0.0f, YELLOW),
        GradientStop::create(1.0f, MAGENTA)
    };
    auto lg = LinearGradient::create(Point(50.0f, 130.0f), Point(250.0f, 200.0f), stops);
    assert(lg.has_value());
    Paint grad_paint;
    grad_paint.shader = Shader(*lg);
    auto grad_rect = Rect::from_xywh(50.0f, 130.0f, 200.0f, 70.0f);
    assert(grad_rect.has_value());
    canvas.fill_rect(*grad_rect, grad_paint);

    // 7. Complex Bézier Path
    PathBuilder pb;
    pb.move_to(270.0f, 140.0f);
    pb.line_to(370.0f, 140.0f);
    pb.quad_to(390.0f, 175.0f, 370.0f, 210.0f);
    pb.cubic_to(340.0f, 220.0f, 300.0f, 220.0f, 270.0f, 180.0f);
    pb.close();
    auto path = pb.finish();
    assert(path.has_value());
    Paint path_paint(CYAN);
    canvas.fill_path(*path, path_paint);

    // 8. 2D Mesh triangles
    std::vector<Point> mesh_pts = { Point(100.0f, 220.0f), Point(200.0f, 220.0f), Point(150.0f, 280.0f) };
    std::vector<Color> mesh_cols = { RED, GREEN, BLUE };
    auto mesh = Vertices::create_triangles(mesh_pts, mesh_cols);
    canvas.draw_vertices(mesh);

    // 9. Flush and Readback
    canvas.flush();

    auto pixmap = surface->to_pixmap();
    assert(pixmap.has_value());
    assert(pixmap->width() == 400);
    assert(pixmap->height() == 300);

    // Verify center of rect1 is red
    auto p_red = pixmap->pixel(60, 50);
    assert(p_red.has_value());
    assert(p_red->alpha() == 255);
    assert(p_red->red() > 200); // Red dominant

    // Verify center of circle is green
    auto p_circle = pixmap->pixel(280, 60);
    assert(p_circle.has_value());
    assert(p_circle->alpha() == 255);
    assert(p_circle->green() > 200); // Green dominant

    std::cout << "  -> Vulkan surface, canvas, primitives, and pixel readback OK" << std::endl;
}

void test_vulkan_cpu_coexistence() {
    std::cout << "Testing CPU Pixmap and Vulkan pipeline coexistence..." << std::endl;

    // 1. Draw a pattern on CPU Canvas
    auto cpu_pixmap = Pixmap::allocate(100, 100);
    assert(cpu_pixmap.has_value());
    Canvas cpu_canvas(*cpu_pixmap);
    cpu_canvas.clear(YELLOW);
    Paint cpu_paint(BLUE);
    cpu_canvas.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 60.0f, 60.0f), cpu_paint);

    // 2. Render on Vulkan surface
    auto device = VulkanDevice::create();
    auto surface = VulkanSurface::create(device, 200, 200);
    GpuCanvas vk_canvas(surface);
    vk_canvas.clear(Color::BLACK);

    // 3. Draw CPU pixmap onto Vulkan Canvas
    vk_canvas.draw_pixmap(50, 50, cpu_pixmap->as_ref());
    vk_canvas.flush();

    // 4. Readback and verify
    auto readback = surface->to_pixmap();
    assert(readback.has_value());
    assert(readback->width() == 200);
    assert(readback->height() == 200);

    // Background outside the drawn image should be black
    auto bg_pix = readback->pixel(10, 10);
    assert(bg_pix.has_value());
    assert(bg_pix->red() == 0 && bg_pix->green() == 0 && bg_pix->blue() == 0);

    // Yellow corner of the drawn CPU image (at x=55, y=55)
    auto yellow_pix = readback->pixel(55, 55);
    assert(yellow_pix.has_value());
    assert(yellow_pix->red() > 200 && yellow_pix->green() > 200);

    // Blue center of the drawn CPU image (at x=90, y=90)
    auto blue_pix = readback->pixel(90, 90);
    assert(blue_pix.has_value());
    assert(blue_pix->blue() > 200);

    std::cout << "  -> CPU and Vulkan coexistence OK" << std::endl;
}

void test_vulkan_gradients_and_color_filters() {
    std::cout << "Testing Vulkan multi-stop gradients (LUT + Sweep + Conical) & ColorEffect parity..." << std::endl;
    auto device = VulkanDevice::create();
    assert(device && device->is_valid());

    constexpr uint32_t W = 240;
    constexpr uint32_t H = 200;
    const Color bg = Color::from_rgba8(225, 230, 240, 255);

    const std::vector<GradientStop> stops4 = {
        GradientStop::create(0.0f, Color::from_rgba8(240, 40, 40, 255)),
        GradientStop::create(0.35f, Color::from_rgba8(240, 220, 40, 220)),
        GradientStop::create(0.7f, Color::from_rgba8(40, 210, 120, 190)),
        GradientStop::create(1.0f, Color::from_rgba8(50, 90, 255, 255)),
    };

    std::vector<GradientStop> stops20;
    for (int i = 0; i < 20; ++i) {
        float t = float(i) / 19.0f;
        uint8_t r = static_cast<uint8_t>(255.0f * (0.5f + 0.5f * std::sin(t * 6.28f)));
        uint8_t g = static_cast<uint8_t>(255.0f * (0.5f + 0.5f * std::cos(t * 9.42f)));
        uint8_t b = static_cast<uint8_t>(255.0f * t);
        stops20.push_back(GradientStop::create(t, Color::from_rgba8(r, g, b, 255)));
    }

    verify_vulkan_parity(device, W, H, bg, "VK Multi-Stop Gradients + LUT", [&](ICanvas& c, bool) {
        auto lg_rep = LinearGradient::create(Point::from_xy(10, 10), Point::from_xy(55, 35), stops4, SpreadMode::Repeat);
        assert(lg_rep.has_value());
        c.fill_rect(*Rect::from_xywh(10, 10, 100, 60), Paint(Shader(*lg_rep)));

        auto lg20 = LinearGradient::create(Point::from_xy(125, 10), Point::from_xy(215, 70), stops20, SpreadMode::Reflect);
        assert(lg20.has_value());
        c.fill_round_rect(*Rect::from_xywh(125, 10, 100, 60), 10.0f, 10.0f, Paint(Shader(*lg20)));

        auto rg_con = TwoPointConicalGradient::create(
            Point::from_xy(48, 128), 8.0f, Point::from_xy(65, 140), 38.0f, stops4, SpreadMode::Pad);
        assert(rg_con.has_value());
        c.fill_rect(*Rect::from_xywh(12, 90, 95, 95), Paint(Shader(*rg_con)));

        auto sg = SweepGradient::create(Point::from_xy(175, 138), 15.0f, 220.0f, stops4, SpreadMode::Reflect);
        assert(sg.has_value());
        c.fill_oval(*Rect::from_xywh(130.0f, 93.0f, 90.0f, 90.0f), Paint(Shader(*sg)));
    }, {{35, 35}, {175, 40}, {60, 130}, {195, 138}}, 10, 4.5f);

    verify_vulkan_parity(device, W, H, bg, "VK ColorFilter Matrix + Gamma + ShaderBlend", [&](ICanvas& c, bool) {
        Paint p_inv(Color::from_rgba8(50, 180, 230, 255));
        p_inv.set_color_filter(effects::ColorMatrix::invert());
        c.fill_rect(*Rect::from_xywh(15, 15, 65, 65), p_inv);

        Paint p_sep(Color::from_rgba8(80, 140, 210, 255));
        p_sep.set_color_filter(effects::ColorEffect::sepia(0.9f));
        c.fill_round_rect(*Rect::from_xywh(95, 15, 65, 65), 8.0f, 8.0f, p_sep);

        Paint p_srgb(Color::from_rgba8(140, 190, 90, 255));
        p_srgb.set_color_filter(effects::ColorEffect::srgb_to_linear());
        c.fill_rect(*Rect::from_xywh(170, 15, 55, 65), p_srgb);

        Paint p_blend(Color::from_rgba8(190, 90, 70, 255));
        p_blend.set_color_filter(effects::ColorEffect::mode(Color::from_rgba8(60, 200, 160, 200), BlendMode::Difference));
        c.fill_rect(*Rect::from_xywh(15, 100, 90, 75), p_blend);
    }, {{45, 45}, {125, 45}, {195, 45}, {55, 135}}, 8, 2.5f);

    std::cout << "  -> Vulkan Gradients & ColorFilter parity OK" << std::endl;
}

void test_vulkan_geometry_clip_blend_and_shadows() {
    std::cout << "Testing Vulkan Stencil Fills, Clipping, Analytical Shadows, Dashes, Blends & BGRA Pixmap3D..." << std::endl;
    auto device = VulkanDevice::create();
    assert(device && device->is_valid());

    constexpr uint32_t W = 260;
    constexpr uint32_t H = 220;
    const Color bg = Color::from_rgba8(24, 30, 42, 255);

    // Build concave star + EvenOdd cutout
    PathBuilder pb_star;
    for (int i = 0; i < 10; ++i) {
        float ang = static_cast<float>(i) * 3.14159265f / 5.0f;
        float r = (i % 2 == 0) ? 45.0f : 18.0f;
        float px = 65.0f + std::cos(ang) * r;
        float py = 60.0f + std::sin(ang) * r;
        if (i == 0) pb_star.move_to(px, py);
        else pb_star.line_to(px, py);
    }
    pb_star.close();
    pb_star.push_circle(65.0f, 60.0f, 10.0f);
    auto star = pb_star.finish();
    assert(star.has_value());

    PathBuilder pb_line;
    pb_line.move_to(15.0f, 120.0f);
    pb_line.line_to(130.0f, 120.0f);
    auto dash_line = pb_line.finish();
    assert(dash_line.has_value());

    // Strided BGRA pixmap (4x4 checkerboard, stride = 24)
    std::vector<uint8_t> raw_bgra(4 * 24, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            size_t off = size_t(y) * 24 + size_t(x) * 4;
            bool qx = (x >= 2), qy = (y >= 2);
            raw_bgra[off + 0] = qx ? 240 : 40;  // B
            raw_bgra[off + 1] = qy ? 230 : 50;  // G
            raw_bgra[off + 2] = (!qx && !qy) ? 245 : 30; // R
            raw_bgra[off + 3] = 255;            // A
        }
    }
    auto bgra_ref = PixmapRef::from_raw_parts(raw_bgra.data(), 4, 4, 24, PixelFormat::BGRA8888);
    assert(bgra_ref.has_value());

    verify_vulkan_parity(device, W, H, bg, "VK Concave Stencil + Clip + Shadow + Dash + Blends + BGRA 3D", [&](ICanvas& c, bool) {
        // 1. EvenOdd concave star with hole via Vulkan stencil fill + AA fringe
        c.fill_path(*star, Paint(Color::from_rgba8(245, 190, 40, 255)), FillRule::EvenOdd);

        // 2. Stencil nested clipping
        c.save();
        c.clip_rrect(RRect::from_rect_and_radius(*Rect::from_xywh(120, 15, 70, 70), 14.0f, 14.0f));
        c.fill_rect(*Rect::from_xywh(110, 5, 90, 90), Paint(Color::from_rgba8(40, 190, 230, 255)));
        c.restore();

        // 3. Analytical drop shadow + RRect
        auto card = RRect::from_rect_and_radius(*Rect::from_xywh(198, 22, 46, 56), 9.0f, 9.0f);
        effects::DropShadow shadow{3.0f, 4.0f, 4.0f, Color::from_rgba8(0, 0, 0, 160)};
        c.draw_rrect_with_shadow(card, Paint(Color::from_rgba8(240, 244, 250, 255)), shadow);

        // 4. Dashed stroke
        Stroke dashed(5.0f);
        dashed.dash = StrokeDash::create({14.0f, 8.0f}, 0.0f);
        c.stroke_path(*dash_line, Paint(GREEN), dashed);

        // 5. Hardware Porter-Duff & Multiply blends
        c.fill_rect(*Rect::from_xywh(15, 140, 45, 45), Paint(Color::from_rgba8(220, 60, 60, 200)));
        {
            Paint p_scr(Color::from_rgba8(60, 180, 240, 200));
            p_scr.blend_mode = BlendMode::Screen;
            c.fill_rect(*Rect::from_xywh(35, 155, 45, 45), p_scr);
        }
        {
            Paint p_mul(Color::from_rgba8(120, 240, 120, 255));
            p_mul.blend_mode = BlendMode::Multiply;
            c.fill_rect(*Rect::from_xywh(90, 145, 40, 40), p_mul);
        }

        // 6. Strided BGRA PixmapRef draw_pixmap_rect + draw_pixmap_3d
        PixmapPaint pp_near;
        pp_near.quality = FilterQuality::Nearest;
        c.draw_pixmap_rect(*bgra_ref, *Rect::from_xywh(0, 0, 4, 4), *Rect::from_xywh(145, 135, 40, 40), pp_near);

        Transform4x4 tr3d = Transform4x4::from_translate(200.0f, 130.0f) *
                            Transform4x4::from_camera_orbit(24.0f, 24.0f, 22.0f, -20.0f, 6.0f, 260.0f) *
                            Transform4x4::from_scale(12.0f, 12.0f);
        c.draw_pixmap_3d(*bgra_ref, tr3d, 0.9f, BlendMode::SourceOver);
    }, {{65, 60}, {95, 60}, {155, 50}, {112, 8}, {221, 50}, {50, 170}, {152, 142}, {175, 165}}, 14, 5.5f, 1);

    // Also run the concave + clipped scene under MSAA 4x on Vulkan
    verify_vulkan_parity(device, W, H, bg, "VK MSAA 4x Stencil Fill + Clip", [&](ICanvas& c, bool) {
        c.fill_path(*star, Paint(Color::from_rgba8(245, 190, 40, 255)), FillRule::EvenOdd);
        c.save();
        c.clip_rect(*Rect::from_xywh(120, 15, 70, 70));
        c.fill_oval(*Rect::from_xywh(105.0f, 0.0f, 100.0f, 100.0f), Paint(CYAN));
        c.restore();
    }, {{65, 60}, {155, 50}, {110, 50}}, 16, 4.5f, 4);

    std::cout << "  -> Vulkan Stencil, Clip, Shadows, Dashes, Blends & BGRA Pixmap3D OK" << std::endl;
}

void test_vulkan_text_and_svg_parity() {
    std::cout << "Testing Vulkan text::Buffer::draw & SvgDocument::render_gpu parity..." << std::endl;
    auto device = VulkanDevice::create();
    assert(device && device->is_valid());

    const std::string font_path = resolve_vk_test_font("NotoSans-Regular.ttf");
    text::FontSystem font_system;
    auto fid = font_system.load_font_file(font_path);
    assert(fid.has_value());
    font_system.set_default_font(*fid);

    text::Buffer buffer(text::Metrics(24.0f, 30.0f));
    buffer.set_size(220.0f, std::nullopt);
    buffer.set_text("Vulkan Text");
    buffer.shape_until_scroll(font_system);
    text::GlyphCache cpu_cache(256);

    const std::string svg_xml = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120" viewBox="0 0 240 120">
  <defs>
    <linearGradient id="vklg" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" stop-color="#ff5030"/>
      <stop offset="100%" stop-color="#30c0ff"/>
    </linearGradient>
  </defs>
  <rect x="10" y="10" width="100" height="80" rx="12" fill="url(#vklg)"/>
  <circle cx="175" cy="50" r="34" fill="#40e080" stroke="#ffffff" stroke-width="4" stroke-dasharray="10 6"/>
</svg>)svg";
    auto doc = svg::SvgDocument::parse(svg_xml);
    assert(doc.has_value());
    Rect svg_bounds = Rect::from_xywh(0.0f, 60.0f, 240.0f, 120.0f).value();

    verify_vulkan_parity(device, 240, 200, Color::from_rgba8(18, 22, 30, 255), "VK Text + SVG",
        [&](ICanvas& c, bool is_gpu) {
            if (is_gpu) {
                auto& g = static_cast<GpuCanvas&>(c);
                g.draw_text_buffer(buffer, font_system, Point::from_xy(16.0f, 14.0f), Color::WHITE);
                doc->render_gpu(g, &svg_bounds);
            } else {
                auto& cp = static_cast<Canvas&>(c);
                buffer.draw(cp, cpu_cache, font_system, Color::WHITE, 16.0f, 14.0f);
                doc->render(cp, &svg_bounds);
            }
        },
        {{10, 10}, {40, 105}, {95, 105}, {175, 110}}, 14, 5.5f);

    std::cout << "  -> Vulkan Text & SVG Parity OK" << std::endl;
}

void test_vulkan_layers_and_image_filter_dag_parity() {
    std::cout << "Testing Vulkan Offscreen Layers & Phase 1.1 Composable ImageFilter DAG parity..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr && device->is_valid());

    // 1. SaveLayer opacity + bounds + Blur & DropShadow ImageFilter
    verify_vulkan_parity(device, 240, 200, Color::from_rgba8(22, 28, 42, 255),
        "VK SaveLayer + Blur & DropShadow",
        [&](ICanvas& c, bool) {
            Paint lp(Color::from_rgba8(255, 255, 255, 180));
            c.save_layer_bounds(*Rect::from_xywh(10.0f, 10.0f, 100.0f, 80.0f), lp);
            c.fill_rect(*Rect::from_xywh(15.0f, 15.0f, 55.0f, 55.0f), Paint(Color::from_rgba8(230, 50, 50, 255)));
            c.fill_rect(*Rect::from_xywh(45.0f, 30.0f, 55.0f, 55.0f), Paint(Color::from_rgba8(40, 120, 240, 255)));
            c.restore();

            Paint bp(Color::from_rgba8(220, 90, 50, 255));
            bp.set_image_filter(effects::ImageFilter::blur(3.0f, 1.5f));
            c.fill_round_rect(*Rect::from_xywh(130.0f, 20.0f, 80.0f, 55.0f), 8.0f, 8.0f, bp);

            Paint dsp(Color::from_rgba8(50, 190, 130, 255));
            dsp.set_image_filter(effects::ImageFilter::drop_shadow(
                6.0f, 6.0f, 3.0f, 3.0f, Color::from_rgba8(0, 0, 0, 200), false));
            c.fill_rect(*Rect::from_xywh(30.0f, 115.0f, 80.0f, 50.0f), dsp);
        },
        {{30, 30}, {55, 45}, {170, 47}, {70, 140}, {114, 168}}, 12, 4.5f);

    // 2. Composable DAG: Compose + Offset + ColorFilter + CropRect + Merge + Blend
    verify_vulkan_parity(device, 260, 200, Color::from_rgba8(22, 26, 36, 255),
        "VK Composable ImageFilter DAG",
        [&](ICanvas& c, bool) {
            Paint p1(Color::from_rgba8(230, 60, 50, 255));
            auto crop = Rect::from_xywh(35.0f, 25.0f, 55.0f, 55.0f);
            p1.set_image_filter(effects::ImageFilter::compose(
                effects::ImageFilter::color_filter(effects::ColorEffect::invert(), std::nullopt, crop),
                effects::ImageFilter::offset(15.0f, 10.0f)
            ));
            c.fill_rect(*Rect::from_xywh(15.0f, 15.0f, 65.0f, 65.0f), p1);

            Paint pm(Color::from_rgba8(240, 80, 50, 255));
            auto sh = effects::ImageFilter::blur(2.5f,
                effects::ImageFilter::offset(14.0f, 10.0f,
                    effects::ImageFilter::color_filter(
                        effects::ColorEffect::mode(Color::from_rgba8(40, 120, 250, 210), BlendMode::SourceIn))));
            pm.set_image_filter(effects::ImageFilter::merge(sh, std::optional<effects::ImageFilter>{std::nullopt}));
            c.fill_rect(*Rect::from_xywh(130.0f, 25.0f, 65.0f, 55.0f), pm);

            Paint pb(Color::from_rgba8(200, 120, 60, 255));
            auto bg_branch = effects::ImageFilter::offset(-10.0f, 0.0f,
                effects::ImageFilter::color_filter(
                    effects::ColorEffect::mode(Color::from_rgba8(80, 180, 240, 255), BlendMode::SourceIn)));
            auto fg_branch = effects::ImageFilter::offset(10.0f, 0.0f);
            pb.set_image_filter(effects::ImageFilter::blend(BlendMode::Screen, bg_branch, fg_branch));
            c.fill_rect(*Rect::from_xywh(80.0f, 120.0f, 65.0f, 55.0f), pb);
        },
        {{55, 50}, {22, 22}, {92, 50}, {160, 50}, {202, 65}, {76, 145}, {112, 145}, {148, 145}}, 12, 4.5f);

    std::cout << "  -> Vulkan Offscreen Layers & Phase 1.1 ImageFilter DAG Parity OK" << std::endl;
}

void test_vulkan_mask_filter_and_perlin_noise_parity() {
    std::cout << "Testing Vulkan Phase 1.3 BlurMaskFilter & Phase 1.4 PerlinNoiseShader parity..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr && device->is_valid());

    verify_vulkan_parity(device, 260, 200, Color::from_rgba8(20, 24, 34, 255),
        "VK BlurMaskFilter 4 Modes",
        [&](ICanvas& c, bool) {
            const effects::BlurStyle styles[4] = {
                effects::BlurStyle::Normal,
                effects::BlurStyle::Solid,
                effects::BlurStyle::Outer,
                effects::BlurStyle::Inner
            };
            const Color colors[4] = {
                Color::from_rgba8(240, 80, 60, 255),
                Color::from_rgba8(60, 200, 120, 255),
                Color::from_rgba8(70, 150, 250, 255),
                Color::from_rgba8(240, 190, 50, 255)
            };
            for (int i = 0; i < 4; ++i) {
                Paint p(colors[i]);
                p.set_mask_filter(effects::MaskFilter::blur(styles[i], 4.0f));
                float x = 15.0f + static_cast<float>(i % 2) * 125.0f;
                float y = 18.0f + static_cast<float>(i / 2) * 95.0f;
                c.fill_round_rect(*Rect::from_xywh(x, y, 90.0f, 65.0f), 10.0f, 10.0f, p);
            }
        },
        {{60, 50}, {185, 50}, {60, 145}, {15, 145}, {185, 145}, {137, 145}}, 12, 3.5f);

    verify_vulkan_parity(device, 260, 200, Color::from_rgba8(20, 24, 34, 255),
        "VK PerlinNoiseShader Fractal & Turbulence",
        [&](ICanvas& c, bool) {
            Paint pf(Shader::create_fractal_noise(0.05f, 0.05f, 4, 3.0f));
            c.fill_rect(*Rect::from_xywh(15.0f, 20.0f, 105.0f, 160.0f), pf);

            Paint pt(Shader::create_turbulence(0.0625f, 0.0625f, 4, 11.0f, IntSize::from_wh(32, 32)));
            c.fill_rect(*Rect::from_xywh(135.0f, 20.0f, 105.0f, 160.0f), pt);
        },
        {{40, 50}, {85, 110}, {165, 65}, {210, 140}}, 4, 1.5f);

    std::cout << "  -> Vulkan BlurMaskFilter & PerlinNoiseShader Parity OK" << std::endl;
}

void test_vulkan_runtime_shader_image_filter_parity() {
    std::cout << "Testing Vulkan Phase 1.2 RuntimeShader ImageFilter parity..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr && device->is_valid());

    const char* kPostFxSrc = R"(
        uniform shader content;
        uniform float shift;
        uniform vec3 gain;
        vec4 main(vec2 pos) {
            vec4 cr = content.eval(pos + vec2(shift, 0.0));
            vec4 cg = content.eval(pos);
            vec4 cb = content.eval(pos - vec2(shift, 0.0));
            float a = max(max(cr.a, cg.a), cb.a);
            return vec4(cr.r * gain.r, cg.g * gain.g, cb.b * gain.b, a);
        }
    )";

    verify_vulkan_parity(device, 220, 160, Color::from_rgba8(18, 22, 32, 255),
        "VK RuntimeShader ImageFilter",
        [&](ICanvas& c, bool) {
            effects::RuntimeFilterUniforms u;
            u.set_float("shift", 7.0f).set_vec3("gain", 1.0f, 0.85f, 0.95f);
            auto crop = Rect::from_xywh(16.0f, 16.0f, 188.0f, 128.0f);
            Paint lp;
            lp.set_image_filter(effects::ImageFilter::runtime_shader(kPostFxSrc, u, "content", std::nullopt, crop));
            c.save_layer_bounds(*Rect::from_xywh(10.0f, 10.0f, 200.0f, 140.0f), lp);
            c.fill_rect(*Rect::from_xywh(45.0f, 35.0f, 130.0f, 90.0f), Paint(Color::from_rgba8(240, 210, 230, 255)));
            c.restore();
        },
        {{41, 80}, {110, 80}, {178, 80}, {10, 10}}, 4, 1.5f);

    std::cout << "  -> Vulkan RuntimeShader ImageFilter Parity OK" << std::endl;
}

void test_vulkan_phase2_and_lighting_parity() {
    std::cout << "Testing Vulkan Phase 2 (Morphology/Conv/Distort/Arith/Xform/Tile/Alpha) & Phase 4.1 (3D Lighting) parity..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr && device->is_valid());

    verify_vulkan_parity(device, 260, 220, Color::from_rgba8(16, 20, 28, 255),
        "VK Dilate/Erode/Conv/Arith",
        [&](ICanvas& c, bool) {
            Paint pd(Color::from_rgba8(240, 80, 50, 255));
            pd.set_image_filter(effects::ImageFilter::dilate(4.0f, 4.0f));
            c.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 45.0f, 45.0f), pd);

            Paint pe(Color::from_rgba8(60, 210, 120, 255));
            pe.set_image_filter(effects::ImageFilter::erode(4.0f, 4.0f));
            c.fill_rect(*Rect::from_xywh(95.0f, 20.0f, 45.0f, 45.0f), pe);

            Paint pc(Color::from_rgba8(80, 150, 240, 255));
            pc.set_image_filter(effects::ImageFilter::matrix_convolution(
                3, 3, {-1, -1, -1, -1, 9, -1, -1, -1, -1}, 1.0f, 0.0f, 1, 1, effects::FilterTileMode::Clamp, false));
            c.fill_rect(*Rect::from_xywh(175.0f, 20.0f, 55.0f, 45.0f), pc);

            Paint pa(Color::from_rgba8(220, 40, 60, 255));
            auto bgf = effects::ImageFilter::color_filter(
                effects::ColorEffect::mode(Color::from_rgba8(40, 200, 220, 255), BlendMode::SourceIn));
            pa.set_image_filter(effects::ImageFilter::arithmetic(0.0f, 0.5f, 0.5f, 0.0f, true, bgf, std::nullopt));
            c.fill_rect(*Rect::from_xywh(20.0f, 110.0f, 80.0f, 65.0f), pa);
        },
        {{18, 42}, {117, 42}, {97, 42}, {200, 42}, {60, 140}}, 8, 2.5f);

    verify_vulkan_parity(device, 260, 220, Color::from_rgba8(16, 20, 28, 255),
        "VK Disp/Mag/Xform/Tile/Alpha",
        [&](ICanvas& c, bool) {
            Paint pdm(Color::from_rgba8(230, 180, 50, 255));
            auto disp_src = effects::ImageFilter::color_filter(
                effects::ColorEffect::mode(Color::from_rgba8(255, 128, 0, 255), BlendMode::SourceIn));
            pdm.set_image_filter(effects::ImageFilter::displacement_map(
                effects::ColorChannel::R, effects::ColorChannel::G, 16.0f, disp_src, std::nullopt));
            c.fill_rect(*Rect::from_xywh(25.0f, 20.0f, 40.0f, 40.0f), pdm);

            Paint pmag;
            pmag.set_image_filter(effects::ImageFilter::magnifier(*Rect::from_xywh(90.0f, 15.0f, 70.0f, 70.0f), 12.0f));
            c.save_layer_bounds(*Rect::from_xywh(85.0f, 10.0f, 80.0f, 80.0f), pmag);
            c.fill_rect(*Rect::from_xywh(105.0f, 30.0f, 40.0f, 40.0f), Paint(Color::from_rgba8(240, 90, 160, 255)));
            c.restore();

            Paint pmx(Color::from_rgba8(90, 220, 210, 255));
            pmx.set_image_filter(effects::ImageFilter::matrix_transform(Transform::from_translate(12.0f, 8.0f), FilterQuality::Bilinear));
            c.fill_rect(*Rect::from_xywh(180.0f, 20.0f, 45.0f, 45.0f), pmx);

            Paint ptl;
            ptl.set_image_filter(effects::ImageFilter::tile(*Rect::from_xywh(20.0f, 115.0f, 20.0f, 20.0f), *Rect::from_xywh(20.0f, 115.0f, 80.0f, 60.0f)));
            c.save_layer_bounds(*Rect::from_xywh(15.0f, 110.0f, 90.0f, 70.0f), ptl);
            c.fill_rect(*Rect::from_xywh(20.0f, 115.0f, 10.0f, 10.0f), Paint(Color::from_rgba8(240, 210, 60, 255)));
            c.restore();

            Paint pat(Color::from_rgba8(180, 100, 240, 128));
            pat.set_image_filter(effects::ImageFilter::alpha_threshold(*Rect::from_xywh(140.0f, 125.0f, 60.0f, 50.0f), 0.9f, 0.2f));
            c.fill_rect(*Rect::from_xywh(120.0f, 115.0f, 100.0f, 70.0f), pat);
        },
        {{40, 40}, {125, 50}, {210, 48}, {45, 140}, {170, 150}, {126, 150}}, 10, 3.0f);

    verify_vulkan_parity(device, 260, 220, Color::from_rgba8(16, 20, 28, 255),
        "VK 3D Surface Lighting",
        [&](ICanvas& c, bool) {
            auto bump = effects::ImageFilter::blur(3.0f);
            Paint p1(Color::WHITE);
            p1.set_image_filter(effects::ImageFilter::distant_lit_diffuse(
                effects::Point3{0.5f, -0.5f, 1.0f}, Color::from_rgba8(255, 220, 180, 255), 6.0f, 1.0f, bump));
            c.fill_oval(*Rect::from_xywh(23.0f, 28.0f, 64.0f, 64.0f), p1);

            Paint p2(Color::WHITE);
            p2.set_image_filter(effects::ImageFilter::point_lit_specular(
                effects::Point3{135.0f, 45.0f, 60.0f}, Color::from_rgba8(140, 210, 255, 255), 8.0f, 1.0f, 12.0f, bump));
            c.fill_oval(*Rect::from_xywh(113.0f, 28.0f, 64.0f, 64.0f), p2);

            Paint p3(Color::WHITE);
            p3.set_image_filter(effects::ImageFilter::spot_lit_diffuse(
                effects::Point3{120.0f, 130.0f, 90.0f}, effects::Point3{130.0f, 155.0f, 0.0f},
                4.0f, 45.0f, Color::from_rgba8(255, 180, 220, 255), 6.0f, 1.0f, bump));
            c.fill_oval(*Rect::from_xywh(95.0f, 120.0f, 70.0f, 70.0f), p3);
        },
        {{55, 60}, {32, 42}, {145, 60}, {130, 155}}, 14, 4.5f);

    std::cout << "  -> Vulkan Phase 2 & Phase 4.1 3D Lighting Parity OK" << std::endl;
}

void test_vulkan_phase3_and_phase4_2_parity() {
    std::cout << "Testing Vulkan Phase 3.1 (Ext ColorEffect), Phase 3.2 (PathEffect) & Phase 4.2 (Blender/ShaderMask) parity..." << std::endl;

    auto device = VulkanDevice::create();
    assert(device != nullptr && device->is_valid());

    verify_vulkan_parity(device, 260, 220, Color::from_rgba8(24, 34, 52, 255),
        "VK Extended ColorEffects",
        [&](ICanvas& c, bool) {
            auto grad = Shader(*LinearGradient::create(
                Point::from_xy(0.0f, 0.0f), Point::from_xy(260.0f, 0.0f),
                {GradientStop::create(0.0f, Color::from_rgba8(240, 60, 40, 255)),
                 GradientStop::create(0.5f, Color::from_rgba8(60, 220, 100, 255)),
                 GradientStop::create(1.0f, Color::from_rgba8(50, 100, 240, 255))}
            ));

            std::array<uint8_t, 256> post{};
            for (int i = 0; i < 256; ++i) post[i] = static_cast<uint8_t>((i / 64) * 64);
            Paint p_tbl(grad);
            p_tbl.set_color_filter(effects::ColorEffect::table(post));
            c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 240.0f, 32.0f), p_tbl);

            effects::ColorMatrix hmat = effects::ColorMatrix::identity();
            hmat.m[4] = 0.33f;
            Paint p_hsla(grad);
            p_hsla.set_color_filter(effects::ColorEffect::hsla_matrix(hmat));
            c.fill_rect(*Rect::from_xywh(10.0f, 52.0f, 240.0f, 32.0f), p_hsla);

            Paint p_luma(grad);
            p_luma.set_color_filter(effects::ColorEffect::luma());
            c.fill_rect(*Rect::from_xywh(10.0f, 94.0f, 240.0f, 32.0f), p_luma);

            Paint p_hc(grad);
            p_hc.set_color_filter(effects::ColorEffect::high_contrast(
                effects::HighContrastConfig{false, effects::HighContrastInvertStyle::InvertLightness, 0.25f}));
            c.fill_rect(*Rect::from_xywh(10.0f, 136.0f, 240.0f, 32.0f), p_hc);

            Paint p_lerp(grad);
            p_lerp.set_color_filter(effects::ColorEffect::lerp(0.5f, effects::ColorEffect::invert(), effects::ColorEffect::grayscale()));
            c.fill_rect(*Rect::from_xywh(10.0f, 178.0f, 240.0f, 32.0f), p_lerp);
        },
        {{60, 25}, {180, 25}, {60, 68}, {130, 110}, {130, 152}, {130, 194}}, 8, 2.5f);

    verify_vulkan_parity(device, 260, 220, Color::from_rgba8(24, 34, 52, 255),
        "VK Composable PathEffects",
        [&](ICanvas& c, bool) {
            Stroke st; st.width = 4.0f;
            Paint p1(Color::from_rgba8(240, 100, 60, 255));
            p1.set_path_effect(effects::PathEffect::corner(14.0f));
            c.fill_rect(*Rect::from_xywh(15.0f, 15.0f, 65.0f, 55.0f), p1);

            Paint p2(Color::from_rgba8(80, 220, 140, 255));
            p2.set_path_effect(effects::PathEffect::trim(0.15f, 0.75f));
            c.stroke_rect(*Rect::from_xywh(100.0f, 15.0f, 65.0f, 55.0f), p2, st);

            Path dot = PathBuilder::from_circle(0.0f, 0.0f, 3.5f).value();
            Paint p4(Color::from_rgba8(240, 200, 60, 255));
            p4.set_path_effect(effects::PathEffect::path_1d(dot, 12.0f, 0.0f, effects::Path1DStyle::Rotate));
            c.stroke_rect(*Rect::from_xywh(20.0f, 105.0f, 80.0f, 80.0f), p4, st);

            Paint p5(Color::from_rgba8(220, 100, 240, 255));
            p5.set_path_effect(effects::PathEffect::path_2d(Transform::from_scale(14.0f, 14.0f), dot));
            c.fill_rect(*Rect::from_xywh(135.0f, 105.0f, 100.0f, 80.0f), p5);
        },
        {{45, 42}, {16, 16}, {130, 15}, {185, 145}}, 12, 3.5f);

    verify_vulkan_parity(device, 260, 220, Color::from_rgba8(24, 34, 52, 255),
        "VK Blenders & ShaderMaskFilter",
        [&](ICanvas& c, bool) {
            Paint pa(Color::from_rgba8(230, 60, 50, 255));
            pa.set_blender(effects::Blender::arithmetic(0.0f, 0.6f, 0.4f, 0.0f, true));
            c.fill_rect(*Rect::from_xywh(15.0f, 20.0f, 70.0f, 75.0f), pa);

            const char* kMaxBlend = R"(
                uniform float gain;
                vec4 main(vec4 src, vec4 dst) {
                    return vec4(clamp(max(src.rgb * gain, dst.rgb), 0.0, 1.0), 1.0);
                }
            )";
            effects::RuntimeFilterUniforms bu;
            bu.set_float("gain", 0.9f);
            Paint pr(Color::from_rgba8(240, 180, 40, 255));
            pr.set_blender(effects::Blender::runtime(kMaxBlend, bu));
            c.fill_rect(*Rect::from_xywh(100.0f, 20.0f, 70.0f, 75.0f), pr);

            auto mask_grad = Shader(*LinearGradient::create(
                Point::from_xy(20.0f, 0.0f), Point::from_xy(220.0f, 0.0f),
                {GradientStop::create(0.0f, Color::from_rgba8(255, 255, 255, 0)),
                 GradientStop::create(1.0f, Color::from_rgba8(255, 255, 255, 255))}
            ));
            Paint psm(Color::from_rgba8(80, 230, 160, 255));
            psm.set_mask_filter(effects::MaskFilter::shader(mask_grad));
            c.fill_rect(*Rect::from_xywh(20.0f, 120.0f, 200.0f, 75.0f), psm);
        },
        {{50, 55}, {135, 55}, {40, 155}, {120, 155}, {200, 155}}, 6, 2.0f);

    std::cout << "  -> Vulkan Phase 3.1, Phase 3.2 & Phase 4.2 Parity OK" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running Nisaba Vulkan GPU Backend Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    test_vulkan_device_init();
    test_vulkan_surface_and_canvas();
    test_vulkan_cpu_coexistence();
    test_vulkan_gradients_and_color_filters();
    test_vulkan_geometry_clip_blend_and_shadows();
    test_vulkan_text_and_svg_parity();
    test_vulkan_layers_and_image_filter_dag_parity();
    test_vulkan_mask_filter_and_perlin_noise_parity();
    test_vulkan_runtime_shader_image_filter_parity();
    test_vulkan_phase2_and_lighting_parity();
    test_vulkan_phase3_and_phase4_2_parity();

    std::cout << "========================================" << std::endl;
    std::cout << "All Nisaba Vulkan GPU tests PASSED (11/11)!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
