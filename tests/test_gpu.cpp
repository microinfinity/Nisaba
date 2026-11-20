#include <GL/glew.h>
#include <GL/gl.h>
#include <cassert>
#include <iostream>
#include <cmath>
#include <fstream>
#include <array>
#include <vector>
#include "nisaba/nisaba.hpp"
#include "nisaba/gpu/gpu_blur.hpp"
#include "nisaba/svg/svg_document.hpp"
#include "nisaba/lottie/animation.hpp"
#include "nisaba/text/ttf_font.hpp"
#include "nisaba/text/buffer.hpp"
#include "nisaba/text/font_system.hpp"

using namespace nisaba;
using nisaba::gpu::GpuVertex;
using nisaba::gpu::GpuUniforms;
using nisaba::gpu::GpuBlendState;
using nisaba::gpu::GpuTessellator;
using nisaba::gpu::GpuDevice;
using nisaba::gpu::GpuBuffer;
using nisaba::gpu::GpuSurface;
using nisaba::gpu::GpuCanvas;

namespace {
const Color RED = Color::from_rgba8(255, 0, 0, 255);
const Color GREEN = Color::from_rgba8(0, 255, 0, 255);
const Color BLUE = Color::from_rgba8(0, 0, 255, 255);
const Color YELLOW = Color::from_rgba8(255, 255, 0, 255);
const Color CYAN = Color::from_rgba8(0, 255, 255, 255);
const Color MAGENTA = Color::from_rgba8(255, 0, 255, 255);

std::string resolve_test_font(std::string_view filename) {
    std::vector<std::string> candidates = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename)
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return candidates[0];
}
} // namespace

void test_gpu_types() {
    std::cout << "Testing GPU types..." << std::endl;

    GpuVertex v1(Point(10.0f, 20.0f), RED, 1.0f);
    assert(v1.pos.x == 10.0f);
    assert(v1.pos.y == 20.0f);
    assert(v1.color == RED);
    assert(v1.coverage == 1.0f);

    GpuVertex v2(30.0f, 40.0f, 0.5f, 0.5f, BLUE, 0.0f);
    assert(v2.pos.x == 30.0f);
    assert(v2.pos.y == 40.0f);
    assert(v2.uv.x == 0.5f);
    assert(v2.uv.y == 0.5f);
    assert(v2.color == BLUE);
    assert(v2.coverage == 0.0f);

    GpuUniforms uniforms;
    assert(uniforms.viewport_width == 1.0f);
    assert(uniforms.opacity == 1.0f);

    GpuBlendState blend(BlendMode::SourceOver);
    assert(blend.blend_mode == BlendMode::SourceOver);
    assert(blend == GpuBlendState(BlendMode::SourceOver));
    assert(blend != GpuBlendState(BlendMode::Clear));

    std::cout << "  -> GPU types OK" << std::endl;
}

void test_gpu_tessellation_rect() {
    std::cout << "Testing GPU tessellation for rectangles..." << std::endl;

    auto r = Rect::from_xywh(10.0f, 20.0f, 100.0f, 50.0f);
    assert(r.has_value());

    // Non-AA rect
    std::vector<GpuVertex> verts_no_aa;
    std::vector<uint32_t> indices_no_aa;
    GpuTessellator::tessellate_rect(verts_no_aa, indices_no_aa, *r, GREEN, Transform(), false);

    assert(verts_no_aa.size() == 4);
    assert(indices_no_aa.size() == 6); // 2 triangles
    for (const auto& v : verts_no_aa) {
        assert(v.coverage == 1.0f);
        assert(v.color == GREEN);
    }

    // AA rect with fringe
    std::vector<GpuVertex> verts_aa;
    std::vector<uint32_t> indices_aa;
    GpuTessellator::tessellate_rect(verts_aa, indices_aa, *r, GREEN, Transform(), true);

    assert(verts_aa.size() == 8);        // 4 inner + 4 outer
    assert(indices_aa.size() == 6 + 24); // 2 triangles inner + 8 triangles fringe = 30 indices

    // First 4 vertices are inner (coverage 1.0)
    for (size_t i = 0; i < 4; ++i) {
        assert(verts_aa[i].coverage == 1.0f);
    }
    // Last 4 vertices are outer (coverage 0.0)
    for (size_t i = 4; i < 8; ++i) {
        assert(verts_aa[i].coverage == 0.0f);
    }

    std::cout << "  -> GPU rect tessellation OK" << std::endl;
}

void test_gpu_tessellation_circle() {
    std::cout << "Testing GPU tessellation for circles..." << std::endl;

    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    GpuTessellator::tessellate_circle(verts, indices, 100.0f, 100.0f, 40.0f, BLUE, Transform(), true);

    assert(!verts.empty());
    assert(!indices.empty());
    assert(verts[0].pos.x == 100.0f && verts[0].pos.y == 100.0f); // Center
    assert(verts[0].coverage == 1.0f);

    // Verify outer vertices have coverage 0.0
    bool has_zero_coverage = false;
    for (const auto& v : verts) {
        if (v.coverage == 0.0f) {
            has_zero_coverage = true;
            break;
        }
    }
    assert(has_zero_coverage);

    std::cout << "  -> GPU circle tessellation OK" << std::endl;
}

void test_gpu_tessellation_round_rect() {
    std::cout << "Testing GPU tessellation for rounded rectangles..." << std::endl;

    auto r = Rect::from_xywh(0.0f, 0.0f, 200.0f, 100.0f);
    assert(r.has_value());

    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    GpuTessellator::tessellate_round_rect(verts, indices, *r, 15.0f, 15.0f, RED, Transform(), true);

    assert(!verts.empty());
    assert(!indices.empty());
    assert(indices.size() % 3 == 0); // Triangles

    std::cout << "  -> GPU rounded rect tessellation OK" << std::endl;
}

void test_gpu_tessellation_line() {
    std::cout << "Testing GPU tessellation for stroked lines..." << std::endl;

    Stroke stroke;
    stroke.width = 10.0f;

    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    GpuTessellator::tessellate_line(verts, indices, Point(10.0f, 10.0f), Point(100.0f, 10.0f), stroke, Color::WHITE, Transform(), true);

    assert(verts.size() == 8);        // 4 inner + 4 outer fringe
    assert(indices.size() == 6 + 12); // 2 inner triangles + 4 fringe triangles = 18

    std::cout << "  -> GPU line tessellation OK" << std::endl;
}

void test_gpu_tessellation_path() {
    std::cout << "Testing GPU tessellation for arbitrary paths..." << std::endl;

    PathBuilder pb;
    pb.move_to(0.0f, 0.0f);
    pb.line_to(100.0f, 0.0f);
    pb.quad_to(150.0f, 50.0f, 100.0f, 100.0f);
    pb.cubic_to(80.0f, 120.0f, 20.0f, 120.0f, 0.0f, 100.0f);
    pb.close();
    auto path = pb.finish();
    assert(path.has_value());

    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    GpuTessellator::tessellate_path(verts, indices, *path, FillRule::Winding, YELLOW, Transform(), true);

    assert(!verts.empty());
    assert(!indices.empty());
    assert(indices.size() % 3 == 0);

    std::cout << "  -> GPU path tessellation OK" << std::endl;
}

void test_gpu_tessellation_mesh() {
    std::cout << "Testing GPU tessellation for 2D meshes..." << std::endl;

    std::vector<Point> pos = { Point(0.0f, 0.0f), Point(100.0f, 0.0f), Point(50.0f, 100.0f) };
    std::vector<Color> cols = { RED, GREEN, BLUE };
    std::vector<Point> uvs = { Point(0.0f, 0.0f), Point(1.0f, 0.0f), Point(0.5f, 1.0f) };
    std::vector<uint32_t> idx = { 0, 1, 2 };

    auto mesh = Vertices::create_triangles(pos, cols, uvs, idx);
    assert(mesh.is_valid());

    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    GpuTessellator::tessellate_mesh(verts, indices, mesh, Transform());

    assert(verts.size() == 3);
    assert(indices.size() == 3);
    assert(verts[0].color == RED);
    assert(verts[1].color == GREEN);
    assert(verts[2].color == BLUE);

    std::cout << "  -> GPU mesh tessellation OK" << std::endl;
}

void test_gpu_device_and_buffer() {
    std::cout << "Testing GPU device and streaming buffer..." << std::endl;

    auto device = GpuDevice::create();
    assert(device != nullptr);
    assert(device->is_valid());

    std::cout << "  Device Renderer: " << device->renderer_name() << std::endl;
    std::cout << "  Device Version:  " << device->version_name() << std::endl;

    auto buffer = GpuBuffer::create(device);
    assert(buffer != nullptr);

    std::vector<GpuVertex> v = {
        GpuVertex(0.0f, 0.0f, RED),
        GpuVertex(10.0f, 0.0f, GREEN),
        GpuVertex(5.0f, 10.0f, BLUE)
    };
    std::vector<uint32_t> i = { 0, 1, 2 };

    buffer->upload_data(v, i);
    assert(buffer->vertex_capacity() >= 3);
    assert(buffer->index_capacity() >= 3);

    std::cout << "  -> GPU device and buffer OK" << std::endl;
}

void test_gpu_surface_and_canvas() {
    std::cout << "Testing GPU surface and high-level GpuCanvas API..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 400, 300);
    assert(surface != nullptr);
    assert(surface->width() == 400);
    assert(surface->height() == 300);

    GpuCanvas canvas(surface);
    assert(canvas.width() == 400);
    assert(canvas.height() == 300);

    // 1. Clear
    canvas.clear(Color::from_rgba8(30, 30, 40, 255));

    // 2. Transform stack
    canvas.save();
    canvas.translate(50.0f, 50.0f);
    canvas.scale(1.5f, 1.5f);
    canvas.rotate(45.0f);
    assert(!canvas.transform().is_identity());
    canvas.restore();
    assert(canvas.transform().is_identity());

    // 3. Clipping
    auto clip_r = Rect::from_xywh(10.0f, 10.0f, 380.0f, 280.0f);
    assert(clip_r.has_value());
    canvas.clip_rect(*clip_r);
    assert(canvas.scissor_clip().has_value());
    canvas.reset_clip();
    assert(canvas.scissor_clip()->width() == 400);

    // 4. Drawing shapes
    Paint solid_paint(Color::from_rgba8(255, 100, 50, 255));
    auto rect = Rect::from_xywh(20.0f, 20.0f, 100.0f, 60.0f);
    assert(rect.has_value());
    canvas.fill_rect(*rect, solid_paint);

    Stroke stroke;
    stroke.width = 4.0f;
    canvas.stroke_rect(*rect, solid_paint, stroke);

    canvas.fill_round_rect(*rect, 10.0f, 10.0f, solid_paint);
    canvas.stroke_round_rect(*rect, 10.0f, 10.0f, solid_paint, stroke);

    canvas.fill_circle(200.0f, 150.0f, 40.0f, solid_paint);
    canvas.stroke_circle(200.0f, 150.0f, 40.0f, solid_paint, stroke);

    canvas.stroke_line(10.0f, 10.0f, 390.0f, 290.0f, solid_paint, stroke);

    // 5. Linear Gradient
    std::vector<GradientStop> stops = {
        GradientStop::create(0.0f, RED),
        GradientStop::create(1.0f, BLUE)
    };
    auto lg = LinearGradient::create(Point(0.0f, 0.0f), Point(400.0f, 300.0f), stops);
    assert(lg.has_value());
    Paint grad_paint;
    grad_paint.shader = Shader(*lg);
    canvas.fill_rect(*Rect::from_xywh(50.0f, 50.0f, 200.0f, 150.0f), grad_paint);

    // 6. Draw 2D Mesh
    std::vector<Point> mesh_pts = { Point(250.0f, 50.0f), Point(350.0f, 50.0f), Point(300.0f, 150.0f) };
    std::vector<Color> mesh_cols = { YELLOW, CYAN, MAGENTA };
    auto mesh = Vertices::create_triangles(mesh_pts, mesh_cols);
    canvas.draw_vertices(mesh);

    // 7. Flush and Readback
    canvas.flush();
    auto pixmap = surface->to_pixmap();
    assert(pixmap.has_value());
    assert(pixmap->width() == 400);
    assert(pixmap->height() == 300);

    std::cout << "  -> GPU surface and canvas OK" << std::endl;
}

void test_gpu_cpu_coexistence() {
    std::cout << "Testing seamless CPU and GPU pipeline coexistence..." << std::endl;

    // 1. CPU Canvas rendering
    auto cpu_pixmap = Pixmap::allocate(200, 200);
    assert(cpu_pixmap.has_value());
    Canvas cpu_canvas(*cpu_pixmap);
    cpu_canvas.clear(Color::WHITE);
    Paint cpu_paint(RED);
    cpu_canvas.fill_rect(*Rect::from_xywh(20.0f, 20.0f, 60.0f, 60.0f), cpu_paint);

    // 2. GPU Canvas rendering
    auto device = GpuDevice::create();
    auto gpu_surface = GpuSurface::create(device, 200, 200);
    assert(gpu_surface != nullptr);
    GpuCanvas gpu_canvas(gpu_surface);
    gpu_canvas.clear(Color::BLACK);
    Paint gpu_paint(BLUE);
    gpu_canvas.fill_rect(*Rect::from_xywh(100.0f, 100.0f, 60.0f, 60.0f), gpu_paint);
    gpu_canvas.flush();

    // 3. Upload CPU Pixmap to GPU Texture and draw on GPU Canvas
    gpu_canvas.draw_pixmap(0, 0, cpu_pixmap->as_ref());
    gpu_canvas.flush();

    // 4. Readback GPU rendering into CPU Pixmap
    auto readback = gpu_surface->to_pixmap();
    assert(readback.has_value());
    assert(readback->width() == 200);
    assert(readback->height() == 200);

    std::cout << "  -> Seamless CPU and GPU coexistence OK" << std::endl;
}

void test_gpu_text_rendering() {
    std::cout << "Testing GPU hardware text rendering..." << std::endl;

    std::string font_path = resolve_test_font("NotoSans-Regular.ttf");
    auto font = text::TtfFont::from_file(font_path);
    assert(font != nullptr);

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 400, 200);
    assert(surface != nullptr);

    GpuCanvas canvas(surface);
    canvas.clear(Color::WHITE);

    // 1. Draw text directly using text::Font & Paint
    canvas.draw_text("Nisaba GPU Hardware Text", 20.0f, 60.0f, *font, Paint(RED), 28.0f);

    // 2. Register font by name on canvas context and draw with font name
    int font_id = canvas.create_font("test_sans", font_path.c_str());
    assert(font_id > 0);
    canvas.draw_text(20.0f, 120.0f, "Rendered With Font Face", Paint(BLUE), 24.0f, "test_sans");

    canvas.flush();

    // 3. Readback and verify actual text glyph pixels were drawn
    auto readback = surface->to_pixmap();
    assert(readback.has_value());
    assert(readback->width() == 400);
    assert(readback->height() == 200);

    size_t non_white_pixels = 0;
    size_t red_tinted = 0;
    size_t blue_tinted = 0;

    for (uint32_t y = 0; y < readback->height(); ++y) {
        for (uint32_t x = 0; x < readback->width(); ++x) {
            auto opt = readback->as_ref().pixel(x, y);
            if (opt && (opt->r < 240 || opt->g < 240 || opt->b < 240)) {
                non_white_pixels++;
                if (opt->r > 128 && opt->g < 100 && opt->b < 100) {
                    red_tinted++;
                }
                if (opt->b > 128 && opt->g < 100 && opt->r < 100) {
                    blue_tinted++;
                }
            }
        }
    }

    assert(non_white_pixels > 100);
    assert(red_tinted > 30);
    assert(blue_tinted > 30);

    // Verify atlas texture was flushed
    assert(canvas.context() != nullptr);
    assert(canvas.context()->atlas().isDirty() == false);

    std::cout << "  -> GPU hardware text rendering OK (" << non_white_pixels << " text pixels rendered)" << std::endl;
}

void test_gpu_text_buffer_rendering() {
    std::cout << "Testing GPU hardware text buffer & paragraph rendering..." << std::endl;

    std::string font_path = resolve_test_font("NotoSans-Regular.ttf");
    text::FontSystem font_system;
    auto fid = font_system.load_font_file(font_path);
    assert(fid.has_value());
    font_system.set_default_font(*fid);

    text::Buffer buffer(text::Metrics(20.0f, 26.0f));
    buffer.set_size(300.0f, std::nullopt);
    buffer.set_text("Nisaba Sovereign Graphics Engine hardware text layout paragraph test for ENKI.");

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 350, 200);
    assert(surface != nullptr);

    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    // Draw full text buffer with default green color
    canvas.draw_text_buffer(buffer, font_system, Point(20.0f, 30.0f), GREEN);
    canvas.flush();

    auto readback = surface->to_pixmap();
    assert(readback.has_value());

    size_t green_pixels = 0;
    for (uint32_t y = 0; y < readback->height(); ++y) {
        for (uint32_t x = 0; x < readback->width(); ++x) {
            auto opt = readback->as_ref().pixel(x, y);
            if (opt && opt->g > 75 && opt->r < 50 && opt->b < 50) {
                green_pixels++;
            }
        }
    }

    assert(green_pixels > 200);

    // Test text buffer with styled colored spans
    text::Attrs red_attrs;
    red_attrs.set_color(text::TextColor::rgb(255, 0, 0));
    text::Attrs blue_attrs;
    blue_attrs.set_color(text::TextColor::rgb(0, 0, 255));

    text::Buffer styled_buffer(text::Metrics(24.0f, 30.0f));
    styled_buffer.set_text("RedWord BlueWord");
    if (!styled_buffer.lines().empty()) {
        styled_buffer.lines_mut()[0].attrs_list_mut().add_span(0, 7, red_attrs);
        styled_buffer.lines_mut()[0].attrs_list_mut().add_span(8, 16, blue_attrs);
    }

    canvas.clear(Color::BLACK);
    canvas.draw_text_buffer(styled_buffer, font_system, Point(20.0f, 40.0f), Color::WHITE);
    canvas.flush();

    auto styled_readback = surface->to_pixmap();
    assert(styled_readback.has_value());

    size_t red_glyph_pixels = 0;
    size_t blue_glyph_pixels = 0;
    for (uint32_t y = 0; y < styled_readback->height(); ++y) {
        for (uint32_t x = 0; x < styled_readback->width(); ++x) {
            auto opt = styled_readback->as_ref().pixel(x, y);
            if (opt && opt->r > 100 && opt->g < 60 && opt->b < 60) red_glyph_pixels++;
            if (opt && opt->b > 100 && opt->g < 60 && opt->r < 60) blue_glyph_pixels++;
        }
    }

    assert(red_glyph_pixels > 30);
    assert(blue_glyph_pixels > 30);

    std::cout << "  -> GPU hardware text buffer rendering OK (Red: " << red_glyph_pixels << ", Blue: " << blue_glyph_pixels << ")" << std::endl;
}

void test_gpu_clip_path() {
    std::cout << "Testing GPU vector path clipping (GpuCanvas::clip_path)..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 200, 200);
    assert(surface != nullptr);

    GpuCanvas canvas(surface);

    // 1. Single Circular Clip (Avatar / ClipOval)
    canvas.clear(Color::WHITE);

    auto circle_clip_opt = PathBuilder::from_circle(100.0f, 100.0f, 40.0f);
    assert(circle_clip_opt.has_value());
    Path circle_clip = *circle_clip_opt;

    canvas.save();
    canvas.clip_path(circle_clip);
    assert(canvas.stencil_clip_depth() == 1);

    // Fill entire screen with RED
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 200.0f, 200.0f), Paint(RED));
    canvas.restore();
    assert(canvas.stencil_clip_depth() == 0);
    canvas.flush();

    auto readback1 = surface->to_pixmap();
    assert(readback1.has_value());

    // Center pixel (100, 100) must be RED
    auto center_px = readback1->as_ref().pixel(100, 100);
    assert(center_px.has_value());
    std::cout << "Center pixel: R=" << (int)center_px->r << " G=" << (int)center_px->g << " B=" << (int)center_px->b << std::endl;
    assert(center_px->r > 200 && center_px->g < 50 && center_px->b < 50);

    // Corner pixel (20, 20) must be WHITE (untouched by RED fill)
    auto corner_px = readback1->as_ref().pixel(20, 20);
    assert(corner_px.has_value());
    assert(corner_px->r > 200 && corner_px->g > 200 && corner_px->b > 200);

    // Point outside the circle (100, 20) must be WHITE
    auto outside_px = readback1->as_ref().pixel(100, 20);
    assert(outside_px.has_value());
    assert(outside_px->r > 200 && outside_px->g > 200 && outside_px->b > 200);

    // Point inside the circle (100, 80) must be RED
    auto inside_px = readback1->as_ref().pixel(100, 80);
    assert(inside_px.has_value());
    assert(inside_px->r > 200 && inside_px->g < 50 && inside_px->b < 50);

    // 2. Nested Clipping: Intersection of Two Paths
    canvas.clear(Color::WHITE);

    auto c1_opt = PathBuilder::from_circle(85.0f, 100.0f, 45.0f);
    auto c2_opt = PathBuilder::from_circle(115.0f, 100.0f, 45.0f);
    assert(c1_opt.has_value() && c2_opt.has_value());
    Path c1 = *c1_opt;
    Path c2 = *c2_opt;

    canvas.save();
    canvas.clip_path(c1);
    assert(canvas.stencil_clip_depth() == 1);

    canvas.save();
    canvas.clip_path(c2);
    assert(canvas.stencil_clip_depth() == 2);

    // Draw GREEN rectangle over the entire canvas (should only draw in c1 ∩ c2)
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 200.0f, 200.0f), Paint(GREEN));
    canvas.restore(); // Restore back to depth 1 (inside c1)
    assert(canvas.stencil_clip_depth() == 1);

    // (55, 100) is inside c1, but outside c2. Draw BLUE small rect here.
    canvas.fill_rect(*Rect::from_xywh(50.0f, 95.0f, 10.0f, 10.0f), Paint(BLUE));
    canvas.restore(); // Restore back to depth 0
    assert(canvas.stencil_clip_depth() == 0);

    canvas.flush();

    auto readback2 = surface->to_pixmap();
    assert(readback2.has_value());

    // Center (100, 100) is in intersection -> GREEN
    auto isect_px = readback2->as_ref().pixel(100, 100);
    assert(isect_px.has_value());
    assert(isect_px->g > 200 && isect_px->r < 50 && isect_px->b < 50);

    // (55, 100) was inside c1 when BLUE was drawn -> BLUE
    auto c1_only_px = readback2->as_ref().pixel(55, 100);
    assert(c1_only_px.has_value());
    assert(c1_only_px->b > 200 && c1_only_px->r < 50 && c1_only_px->g < 50);

    // (150, 100) is outside c1 -> WHITE
    auto outside_both_px = readback2->as_ref().pixel(150, 100);
    assert(outside_both_px.has_value());
    assert(outside_both_px->r > 200 && outside_both_px->g > 200 && outside_both_px->b > 200);

    // 3. Combined Scissor Rect and Vector Path Clipping
    canvas.clear(Color::WHITE);
    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(50.0f, 50.0f, 100.0f, 100.0f));
    canvas.clip_path(circle_clip); // circle at 100,100 radius 40
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 200.0f, 200.0f), Paint(YELLOW));
    canvas.restore();
    canvas.flush();

    auto readback3 = surface->to_pixmap();
    assert(readback3.has_value());

    // (100, 100) is inside both -> YELLOW
    auto comb_center = readback3->as_ref().pixel(100, 100);
    assert(comb_center.has_value());
    assert(comb_center->r > 200 && comb_center->g > 200 && comb_center->b < 50);

    // (20, 100) is outside scissor rect -> WHITE
    auto outside_scissor = readback3->as_ref().pixel(20, 100);
    assert(outside_scissor.has_value());
    assert(outside_scissor->r > 200 && outside_scissor->g > 200 && outside_scissor->b > 200);

    std::cout << "  -> GPU vector path clipping OK (Circle, Nested Intersections, Mixed Scissor+Path)" << std::endl;
}

void test_gpu_backdrop_blur() {
    std::cout << "Testing GPU hardware backdrop blur (Dual-Kawase)..." << std::endl;

    auto device = GpuDevice::create();
    assert(device != nullptr);

    constexpr uint32_t W = 400;
    constexpr uint32_t H = 300;

    auto surface = GpuSurface::create(device, W, H, 1);
    assert(surface != nullptr);

    GpuCanvas canvas(surface);

    // 1. Draw split high-contrast backdrop: Left pure RED, Right pure BLUE
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 200.0f, 300.0f), Paint(RED));
    canvas.fill_rect(*Rect::from_xywh(200.0f, 0.0f, 200.0f, 300.0f), Paint(BLUE));
    canvas.flush();

    // 2. Draw frosted glass panel in the center crossing the seam (x=100 to 300)
    effects::GlassParams params;
    params.blur_sigma = 18.0f;
    params.tint_color = Color::from_rgba8(255, 255, 255, 30);
    params.border_width = 1.5f;
    params.border_color = Color::from_rgba8(255, 255, 255, 100);

    auto card_rect = *Rect::from_xywh(100.0f, 50.0f, 200.0f, 200.0f);
    BorderRadius card_radii = BorderRadius::all(20.0f);

    canvas.draw_glass_panel(card_rect, card_radii, params);
    canvas.flush();

    auto readback = surface->to_pixmap();
    assert(readback.has_value());

    // 3. Verify seam pixel inside blurred card: Red and Blue must have diffused into each other!
    auto seam_px = readback->as_ref().pixel(200, 150);
    assert(seam_px.has_value());
    assert(seam_px->r > 30);
    assert(seam_px->b > 30);

    // 4. Verify outside regions remain 100% sharp and unblurred
    auto outside_red = readback->as_ref().pixel(50, 150);
    assert(outside_red.has_value());
    assert(outside_red->r > 200);
    assert(outside_red->b < 30);

    auto outside_blue = readback->as_ref().pixel(350, 150);
    assert(outside_blue.has_value());
    assert(outside_blue->b > 200);
    assert(outside_blue->r < 30);

    // 5. Test direct draw_backdrop_blur API with asymmetric RRect
    BorderRadius asym_radii(10.0f, 30.0f, 5.0f, 20.0f);
    RRect asym_rrect(*Rect::from_xywh(50.0f, 20.0f, 120.0f, 80.0f), asym_radii);
    canvas.draw_backdrop_blur(asym_rrect, 10.0f);
    canvas.flush();

    // 6. Verify vertical orientation (top stays top, bottom stays bottom across rect & rrect)
    for (int sample_count : {0, 1, 4}) {
        auto v_surf = GpuSurface::create(device, 200, 200, sample_count);
        assert(v_surf != nullptr);
        GpuCanvas v_canvas(v_surf);

        auto check_vertical = [&](const BorderRadius& br) {
            v_canvas.clear(Color::BLACK);
            v_canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 200.0f, 100.0f), Paint(RED));
            v_canvas.fill_rect(*Rect::from_xywh(0.0f, 100.0f, 200.0f, 100.0f), Paint(BLUE));
            v_canvas.draw_backdrop_blur(*Rect::from_xywh(20.0f, 20.0f, 160.0f, 160.0f), br, 8.0f);
            v_canvas.flush();
            auto v_pm = v_surf->to_pixmap();
            assert(v_pm.has_value());
            auto top_px = v_pm->as_ref().pixel(100, 40);
            auto bot_px = v_pm->as_ref().pixel(100, 160);
            assert(top_px.has_value() && bot_px.has_value());
            assert(top_px->r > 200 && top_px->b < 30);
            assert(bot_px->b > 200 && bot_px->r < 30);
        };

        check_vertical(BorderRadius::zero());
        check_vertical(BorderRadius::all(12.0f));
        check_vertical(BorderRadius(40.0f, 12.0f, 40.0f, 12.0f));
    }

    std::cout << "  -> Hardware GPU Dual-Kawase Backdrop Blur OK (Seam blending verified, sharpness preserved)" << std::endl;
}

void test_gpu_stage0_stability() {
    std::cout << "Testing Stage 0 GPU & path/gradient fixes..." << std::endl;

    auto device = GpuDevice::create();
    assert(device != nullptr);

    // 1. Mid-frame flush() inside active clip_path preserves stencil clip depth
    {
        auto surface = GpuSurface::create(device, 200, 200, 1);
        assert(surface != nullptr);
        GpuCanvas canvas(surface);

        canvas.clear(Color::WHITE);
        PathBuilder pb;
        pb.push_circle(100.0f, 100.0f, 40.0f);
        auto circle_clip = pb.finish();
        assert(circle_clip.has_value());

        canvas.save();
        canvas.clip_path(*circle_clip);
        assert(canvas.stencil_clip_depth() == 1);

        canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 100.0f, 200.0f), Paint(RED));
        canvas.flush(); // Mid-frame flush while clipped

        assert(canvas.stencil_clip_depth() == 1);
        canvas.fill_rect(*Rect::from_xywh(100.0f, 0.0f, 100.0f, 200.0f), Paint(BLUE));
        canvas.restore();
        assert(canvas.stencil_clip_depth() == 0);
        canvas.flush();

        auto pm = surface->to_pixmap();
        assert(pm.has_value());
        auto in_left = pm->as_ref().pixel(80, 100);
        auto in_right = pm->as_ref().pixel(120, 100);
        auto out_left = pm->as_ref().pixel(20, 100);
        auto out_right = pm->as_ref().pixel(180, 100);
        assert(in_left && in_left->r > 200 && in_left->g < 30 && in_left->b < 30);
        assert(in_right && in_right->b > 200 && in_right->r < 30 && in_right->g < 30);
        assert(out_left && out_left->r > 200 && out_left->g > 200 && out_left->b > 200);
        assert(out_right && out_right->r > 200 && out_right->g > 200 && out_right->b > 200);
    }

    // 2. Partial damage resolve Y-flip with MSAA=4 & >32 nested saves
    {
        auto surface = GpuSurface::create(device, 200, 200, 4);
        assert(surface != nullptr);
        GpuCanvas canvas(surface);

        canvas.clear(Color::WHITE);
        for (int i = 0; i < 40; ++i) {
            canvas.save();
            canvas.translate(1.0f, 0.0f);
        }
        for (int i = 0; i < 40; ++i) {
            canvas.restore();
        }
        assert(std::abs(canvas.current_transform().tx) < 1e-4f);

        canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 50.0f, 50.0f), Paint(RED));
        auto dmg = ScreenIntRect::from_xywh(0, 0, 50, 50);
        assert(dmg.has_value());
        canvas.flush(*dmg);

        auto pm = surface->to_pixmap();
        assert(pm.has_value());
        auto top_left = pm->as_ref().pixel(25, 25);
        assert(top_left && top_left->r > 200 && top_left->g < 30 && top_left->b < 30);
    }

    // 3. Path::reset() bounds and RadialGradient::create_2point negative t wrapping
    {
        PathBuilder pb;
        pb.push_rect(*Rect::from_xywh(10.0f, 10.0f, 50.0f, 50.0f));
        auto p = pb.finish();
        assert(p.has_value());
        assert(!p->bounds().is_empty());
        p->reset();
        assert(p->is_empty() && p->bounds().is_empty());

        auto g2 = RadialGradient::create_2point(
            Point::from_xy(50.0f, 50.0f), 20.0f,
            Point::from_xy(50.0f, 50.0f), 40.0f,
            {GradientStop::create(0.0f, RED), GradientStop::create(1.0f, BLUE)},
            SpreadMode::Reflect
        );
        assert(g2.has_value());
        Color c = g2->sample(50.0f, 50.0f); // dist=0 => t=-1.0 => Reflect(1.0) => BLUE
        assert(c.alpha() > 0.9f && c.blue() > 0.9f);
    }

    std::cout << "  -> Stage 0 GPU & path/gradient fixes OK" << std::endl;
}

void test_gpu_vertices_and_pixmap_rect() {
    std::cout << "Testing GPU Vertices, Strided BGRA8888 PixmapRef, and draw_pixmap_rect..." << std::endl;
    auto device = GpuDevice::create();
    assert(device != nullptr);

    // Construct a strided 4x4 BGRA8888 raw buffer (stride = 24 bytes: 16B pixels + 8B 0xAA sentinel)
    std::vector<uint8_t> raw_bgra(24 * 4, 0xAA);
    auto set_bgra = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        size_t off = y * 24 + x * 4;
        raw_bgra[off + 0] = b;
        raw_bgra[off + 1] = g;
        raw_bgra[off + 2] = r;
        raw_bgra[off + 3] = a;
    };
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            set_bgra(x, y, 0, 0, 0, 255);
        }
    }
    // Top-left 2x2 texels: (0,0)=Red, (1,0)=Green, (0,1)=Blue, (1,1)=White
    set_bgra(0, 0, 255, 0, 0, 255);
    set_bgra(1, 0, 0, 255, 0, 255);
    set_bgra(0, 1, 0, 0, 255, 255);
    set_bgra(1, 1, 255, 255, 255, 255);

    auto bgra_ref = PixmapRef::from_raw_parts(raw_bgra.data(), 4, 4, 24, PixelFormat::BGRA8888);
    assert(bgra_ref.has_value());
    assert(bgra_ref->stride_bytes() == 24);

    // Check CPU PixmapMut::is_opaque ignores padding sentinel & painter::draw_pixmap_rect works
    {
        auto owned = bgra_ref->to_owned();
        assert(owned.is_opaque());
        auto cpu_pm = Pixmap::allocate(40, 40);
        assert(cpu_pm.has_value());
        Canvas cpu_canvas(cpu_pm->as_mut());
        cpu_canvas.clear(Color::BLACK);
        PixmapPaint pp;
        pp.quality = FilterQuality::Nearest;
        auto srect = Rect::from_xywh(0.0f, 0.0f, 2.0f, 2.0f);
        auto drect = Rect::from_xywh(0.0f, 0.0f, 40.0f, 40.0f);
        cpu_canvas.draw_pixmap_rect(*bgra_ref, *srect, *drect, pp);
        auto p00 = cpu_pm->as_ref().pixel(5, 5);
        auto p10 = cpu_pm->as_ref().pixel(35, 5);
        auto p01 = cpu_pm->as_ref().pixel(5, 35);
        auto p11 = cpu_pm->as_ref().pixel(35, 35);
        assert(p00 && p00->r > 240 && p00->g < 10 && p00->b < 10);
        assert(p10 && p10->g > 240 && p10->r < 10 && p10->b < 10);
        assert(p01 && p01->b > 240 && p01->r < 10 && p01->g < 10);
        assert(p11 && p11->r > 240 && p11->g > 240 && p11->b > 240);
    }

    auto surface = GpuSurface::create(device, 260, 220, 1);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    // 1. draw_pixmap_rect sub-rect [0..2, 0..2] -> [10..90, 10..90] with FilterQuality::Nearest
    {
        PixmapPaint pp;
        pp.quality = FilterQuality::Nearest;
        auto src_r = Rect::from_xywh(0.0f, 0.0f, 2.0f, 2.0f);
        auto dst_r = Rect::from_xywh(10.0f, 10.0f, 80.0f, 80.0f);
        canvas.draw_pixmap_rect(*bgra_ref, *src_r, *dst_r, pp);
    }

    // 2. draw_pixmap_rect with tint Color(120, 200, 50, 255) on White texel (1,1) -> [100..140, 10..50]
    {
        PixmapPaint pp;
        pp.quality = FilterQuality::Nearest;
        pp.tint = Color::from_rgba8(120, 200, 50, 255);
        auto src_r = Rect::from_xywh(1.0f, 1.0f, 1.0f, 1.0f);
        auto dst_r = Rect::from_xywh(100.0f, 10.0f, 40.0f, 40.0f);
        canvas.draw_pixmap_rect(*bgra_ref, *src_r, *dst_r, pp);
    }

    // 3. draw_pixmap_rect with BlendMode::Plus (Green over Red -> Yellow) -> [100..140, 60..100]
    {
        auto r = Rect::from_xywh(100.0f, 60.0f, 40.0f, 40.0f);
        canvas.fill_rect(*r, Paint(RED));
        PixmapPaint pp;
        pp.quality = FilterQuality::Nearest;
        pp.blend_mode = BlendMode::Plus;
        auto src_r = Rect::from_xywh(1.0f, 0.0f, 1.0f, 1.0f); // Green texel
        canvas.draw_pixmap_rect(*bgra_ref, *src_r, *r, pp);
    }

    // 4. draw_gpu_texture_rect from GpuTexture::create_from_pixmap(*bgra_ref) with Nearest -> [160..240, 10..90]
    {
        auto tex = nisaba::gpu::GpuTexture::create_from_pixmap(device, *bgra_ref);
        assert(tex != nullptr);
        PixmapPaint pp;
        pp.quality = FilterQuality::Nearest;
        auto src_r = Rect::from_xywh(0.0f, 0.0f, 2.0f, 2.0f);
        auto dst_r = Rect::from_xywh(160.0f, 10.0f, 80.0f, 80.0f);
        canvas.draw_gpu_texture_rect(tex, *src_r, *dst_r, pp);
    }

    // 5. Gouraud TriangleStrip (both CCW & CW tris!) -> [10..90, 110..190]
    {
        auto verts = Vertices::create_triangle_strip(
            {Point::from_xy(10.0f, 110.0f), Point::from_xy(10.0f, 190.0f),
             Point::from_xy(90.0f, 110.0f), Point::from_xy(90.0f, 190.0f)},
            {RED, BLUE, GREEN, Color::WHITE}
        );
        canvas.draw_vertices(verts, BlendMode::SourceOver, 1.0f);
    }

    // 6. Textured Vertices (pixel tex_coords into strided BGRA8888 PixmapRef) -> [110..190, 110..190]
    {
        auto verts = Vertices::create_triangle_strip(
            {Point::from_xy(110.0f, 110.0f), Point::from_xy(110.0f, 190.0f),
             Point::from_xy(190.0f, 110.0f), Point::from_xy(190.0f, 190.0f)},
            {},
            {Point::from_xy(0.0f, 0.0f), Point::from_xy(0.0f, 2.0f),
             Point::from_xy(2.0f, 0.0f), Point::from_xy(2.0f, 2.0f)}
        );
        canvas.draw_textured_vertices(verts, *bgra_ref, BlendMode::SourceOver, 1.0f);
    }

    canvas.flush();
    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    const auto pm_ref = pm->as_ref();

    // Verify 1: sub-rect Nearest crisp step across texel boundary at x=50 (dst=[10..90])
    auto p_tl = pm_ref.pixel(20, 20);
    auto p_step_l = pm_ref.pixel(48, 20);
    auto p_step_r = pm_ref.pixel(52, 20);
    auto p_bl = pm_ref.pixel(20, 70);
    auto p_br = pm_ref.pixel(70, 70);
    assert(p_tl && p_tl->r > 240 && p_tl->g < 10 && p_tl->b < 10);
    assert(p_step_l && p_step_l->r > 240 && p_step_l->g < 10);
    assert(p_step_r && p_step_r->g > 240 && p_step_r->r < 10);
    assert(p_bl && p_bl->b > 240 && p_bl->r < 10 && p_bl->g < 10);
    assert(p_br && p_br->r > 240 && p_br->g > 240 && p_br->b > 240);

    // Verify 2: tint modulation
    auto p_tint = pm_ref.pixel(120, 30);
    assert(p_tint && std::abs(int(p_tint->r) - 120) <= 3 &&
           std::abs(int(p_tint->g) - 200) <= 3 &&
           std::abs(int(p_tint->b) - 50) <= 3);

    // Verify 3: BlendMode::Plus
    auto p_plus = pm_ref.pixel(120, 80);
    assert(p_plus && p_plus->r > 240 && p_plus->g > 240 && p_plus->b < 10);

    // Verify 4: GpuTexture Nearest sub-rect
    auto p_gt_tl = pm_ref.pixel(180, 30);
    auto p_gt_tr = pm_ref.pixel(220, 30);
    assert(p_gt_tl && p_gt_tl->r > 240 && p_gt_tl->g < 10);
    assert(p_gt_tr && p_gt_tr->g > 240 && p_gt_tr->r < 10);

    // Verify 5: Gouraud TriangleStrip (both CCW tri0 and CW tri1 rendered!)
    auto g_tl = pm_ref.pixel(15, 115);
    auto g_bl = pm_ref.pixel(15, 185);
    auto g_tr = pm_ref.pixel(85, 115);
    auto g_br = pm_ref.pixel(85, 185);
    assert(g_tl && g_tl->r > 180 && g_tl->b < 80);
    assert(g_bl && g_bl->b > 180 && g_bl->r < 80);
    assert(g_tr && g_tr->g > 180 && g_tr->b < 80);
    assert(g_br && g_br->r > 180 && g_br->g > 180 && g_br->b > 180);

    // Verify 6: Textured Vertices UVs in pixel coords
    auto tv_tl = pm_ref.pixel(120, 120);
    auto tv_tr = pm_ref.pixel(165, 120);
    auto tv_bl = pm_ref.pixel(120, 165);
    assert(tv_tl && tv_tl->r > 180 && tv_tl->b < 60);
    assert(tv_tr && tv_tr->g > 180 && tv_tr->r < 60);
    assert(tv_bl && tv_bl->b > 180 && tv_bl->r < 60);

    // Verify 7: Default PixmapPaint (Bilinear) & mipmapped downscaling avoid nearest-neighbor pixelation
    {
        auto q_surf = GpuSurface::create(device, 120, 60, 1);
        assert(q_surf != nullptr);
        GpuCanvas q_canvas(q_surf);
        q_canvas.clear(Color::BLACK);

        // 7a: 2x1 Red|Green stretched 0..80 with default PixmapPaint{} must blend across midpoint x=40
        auto pair_pm = Pixmap::allocate(2, 1, PixelFormat::RGBA8888);
        assert(pair_pm.has_value());
        pair_pm->as_mut().set_pixel(0, 0, PremultipliedColorU8::from_rgba_unchecked(255, 0, 0, 255));
        pair_pm->as_mut().set_pixel(1, 0, PremultipliedColorU8::from_rgba_unchecked(0, 255, 0, 255));
        q_canvas.draw_pixmap_rect(
            pair_pm->as_ref(),
            *Rect::from_xywh(0.0f, 0.0f, 2.0f, 1.0f),
            *Rect::from_xywh(0.0f, 0.0f, 80.0f, 20.0f)
        );
        auto pair_tex = nisaba::gpu::GpuTexture::create_from_pixmap(device, pair_pm->as_ref());
        assert(pair_tex != nullptr);
        q_canvas.draw_gpu_texture_rect(
            pair_tex,
            *Rect::from_xywh(0.0f, 0.0f, 2.0f, 1.0f),
            *Rect::from_xywh(0.0f, 24.0f, 80.0f, 20.0f)
        );

        // 7b: 128x128 1px B/W checkerboard downscaled 16x -> 8x8 must average to mid-gray via mipmaps
        auto cb_pm = Pixmap::allocate(128, 128, PixelFormat::RGBA8888);
        assert(cb_pm.has_value());
        for (uint32_t y = 0; y < 128; ++y) {
            for (uint32_t x = 0; x < 128; ++x) {
                const uint8_t v = ((x ^ y) & 1u) ? 255 : 0;
                cb_pm->as_mut().set_pixel(x, y, PremultipliedColorU8::from_rgba_unchecked(v, v, v, 255));
            }
        }
        q_canvas.draw_pixmap_rect(
            cb_pm->as_ref(),
            *Rect::from_xywh(0.0f, 0.0f, 128.0f, 128.0f),
            *Rect::from_xywh(90.0f, 10.0f, 8.0f, 8.0f)
        );
        q_canvas.flush();

        auto q_out = q_surf->to_pixmap();
        assert(q_out.has_value());
        auto seam_pm = q_out->as_ref().pixel(40, 10);
        auto seam_tex = q_out->as_ref().pixel(40, 34);
        auto down_px = q_out->as_ref().pixel(94, 14);
        assert(seam_pm && seam_pm->r > 80 && seam_pm->g > 80);
        assert(seam_tex && seam_tex->r > 80 && seam_tex->g > 80);
        assert(down_px && down_px->r >= 80 && down_px->r <= 175 &&
               std::abs(int(down_px->r) - int(down_px->g)) <= 5);
    }

    std::cout << "  -> GPU Vertices, Strided BGRA8888 & draw_pixmap_rect OK" << std::endl;
}

void test_gpu_dashed_strokes() {
    std::cout << "Testing GPU Dashed Strokes & Odd-Length Dash Patterns..." << std::endl;
    auto device = GpuDevice::create();
    assert(device != nullptr);
    auto surface = GpuSurface::create(device, 220, 220, 1);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    // 1. Even dash {20, 20} on line (20,30)->(180,30)
    {
        Stroke s(6.0f);
        s.dash = StrokeDash::create({20.0f, 20.0f}, 0.0f);
        assert(s.dash.has_value());
        canvas.stroke_line(20.0f, 30.0f, 180.0f, 30.0f, Paint(RED), s);
    }

    // 2. Odd-length dash {15} on line (20,60)->(140,60) -> auto-duplicates to {15, 15}
    {
        Stroke s(6.0f);
        s.dash = StrokeDash::create({15.0f}, 0.0f);
        assert(s.dash.has_value());
        canvas.stroke_line(20.0f, 60.0f, 140.0f, 60.0f, Paint(GREEN), s);
    }

    // 3. Dashed stroke_rect {16, 16} at (20, 90, 100, 50)
    {
        Stroke s(4.0f);
        s.dash = StrokeDash::create({16.0f, 16.0f}, 0.0f);
        assert(s.dash.has_value());
        canvas.stroke_rect(*Rect::from_xywh(20.0f, 90.0f, 100.0f, 50.0f), Paint(BLUE), s);
    }

    // 4. Dashed stroke_circle {15, 15} at (160, 165) r=25
    {
        Stroke s(4.0f);
        s.dash = StrokeDash::create({15.0f, 15.0f}, 0.0f);
        assert(s.dash.has_value());
        canvas.stroke_circle(160.0f, 165.0f, 25.0f, Paint(YELLOW), s);
    }

    // 5. Elliptical fill_round_rect (rx=30, ry=8) at (20, 165, 80, 40)
    {
        canvas.fill_round_rect(*Rect::from_xywh(20.0f, 165.0f, 80.0f, 40.0f), 30.0f, 8.0f, Paint(CYAN));
    }

    canvas.flush();
    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    const auto r = pm->as_ref();

    // Check line dashes vs gaps
    assert(r.pixel(30, 30)->r > 200 && r.pixel(50, 30)->r < 10);
    assert(r.pixel(70, 30)->r > 200 && r.pixel(90, 30)->r < 10);

    // Check odd-length dash {15}
    assert(r.pixel(27, 60)->g > 200 && r.pixel(42, 60)->g < 10);
    assert(r.pixel(57, 60)->g > 200);

    // Check dashed rect top edge
    assert(r.pixel(28, 90)->b > 200 && r.pixel(44, 90)->b < 10);

    // Check dashed circle has both yellow dash pixels and black gap pixels on radius=25
    size_t yellow_ring = 0, black_ring = 0;
    for (int deg = 0; deg < 360; deg += 5) {
        float rad = deg * 3.14159265f / 180.0f;
        int px = static_cast<int>(std::round(160.0f + 25.0f * std::cos(rad)));
        int py = static_cast<int>(std::round(165.0f + 25.0f * std::sin(rad)));
        auto p = r.pixel(px, py);
        if (p && p->r > 180 && p->g > 180 && p->b < 30) yellow_ring++;
        else if (p && p->r < 20 && p->g < 20 && p->b < 20) black_ring++;
    }
    assert(yellow_ring > 10 && black_ring > 10);

    // Elliptical corner (rx=30, ry=8): at (30, 175) -> y_offset=10 (> ry=8), inside rect, outside rx=8 circle!
    // Must be CYAN (unlike min(30,8)=8 if corners had been clipped to 8x8)! Wait: if rx=30, ry=8:
    // ellipse boundary at x=30 (dx = 20 from rx=30 corner center at x=50) has dy = 8*sqrt(1 - (20/30)^2) = 5.96,
    // so y = 173 - 5.96 = 167.04; at (25, 169) -> inside ry=8? dx=25/30=0.833, y_rel=(173-169)/8=0.5 -> (0.833)^2+(0.5)^2 = 0.944 < 1 (INSIDE rx=30,ry=8) whereas for rx=8,ry=8 x=25 is OUTSIDE corner [20..28] vs circle!
    // More directly: (25, 166) has dx=25 from x=50 -> outside circle rx=30,ry=8 (`(25/30)^2+(7/8)^2 = 1.46 > 1` -> BLACK!), while for rx=8,ry=8 corner at x=25 is ALREADY straight horizontal edge y=165 -> would be CYAN!
    auto ellip_cutout = r.pixel(25, 166);
    assert(ellip_cutout && ellip_cutout->r < 20 && ellip_cutout->g < 20 && ellip_cutout->b < 20);
    auto ellip_inside = r.pixel(60, 185);
    assert(ellip_inside && ellip_inside->g > 200 && ellip_inside->b > 200);

    std::cout << "  -> GPU Dashed Strokes & Odd-Length Patterns OK" << std::endl;
}

void test_gpu_geometry_apis() {
    std::cout << "Testing GPU & CPU P1.8 Geometry APIs (skew, oval, arc, drrect, points, PaintStyle)..." << std::endl;

    // 1. PathBuilder::push_arc (oval, degrees) + Path::compute_tight_bounds + Transform skew
    {
        Transform t = Transform::identity().pre_skew(0.5f, 0.0f);
        Point tp = Point::from_xy(10.0f, 20.0f);
        t.map_point(tp);
        assert(std::abs(tp.x - 20.0f) < 1e-4f && std::abs(tp.y - 20.0f) < 1e-4f);

        PathBuilder pb;
        pb.move_to(0.0f, 0.0f);
        pb.cubic_to(50.0f, 200.0f, 50.0f, 200.0f, 100.0f, 0.0f);
        auto p = pb.finish();
        assert(p.has_value());
        auto tight = p->compute_tight_bounds();
        assert(tight.has_value() && tight->bottom() < 160.0f); // Tight curve peak (~150), well below control point y=200

        PathBuilder arc_pb;
        arc_pb.push_arc(*Rect::from_xywh(20.0f, 20.0f, 100.0f, 60.0f), 0.0f, 90.0f, false, true);
        auto arc_p = arc_pb.finish();
        assert(arc_p.has_value() && !arc_p->is_empty());
        auto arc_b = arc_p->compute_tight_bounds();
        assert(arc_b.has_value());
        assert(arc_b->left() >= 69.5f && arc_b->right() <= 120.5f);
        assert(arc_b->top() >= 49.5f && arc_b->bottom() <= 80.5f);
    }

    // 2. GPU Surface pixel-readback verification for all P1.8 drawing APIs
    auto device = GpuDevice::create();
    assert(device != nullptr);
    auto surface = GpuSurface::create(device, 320, 260, 1);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    // 2a. skew
    canvas.save();
    canvas.skew(0.5f, 0.0f);
    canvas.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 40.0f, 30.0f), Paint(CYAN));
    canvas.restore();

    // 2b. fill_oval, stroke_oval, clip_oval
    canvas.fill_oval(*Rect::from_xywh(120.0f, 10.0f, 80.0f, 40.0f), Paint(RED));
    canvas.stroke_oval(*Rect::from_xywh(120.0f, 60.0f, 80.0f, 40.0f), Paint(GREEN), Stroke(4.0f));
    canvas.save();
    canvas.clip_oval(*Rect::from_xywh(20.0f, 60.0f, 60.0f, 40.0f));
    canvas.fill_rect(*Rect::from_xywh(20.0f, 60.0f, 60.0f, 40.0f), Paint(BLUE));
    canvas.restore();

    // 2c. draw_arc (use_center = true -> pie wedge in bottom-right quadrant 0..90 deg)
    canvas.draw_arc(*Rect::from_xywh(20.0f, 110.0f, 80.0f, 60.0f), 0.0f, 90.0f, true, Paint(YELLOW));

    // 2d. fill_drrect (nested fast stencil path) + snug concentric deflate (difference fallback)
    {
        auto outer = RRect::from_rect_and_radius(*Rect::from_xywh(120.0f, 110.0f, 90.0f, 60.0f), 12.0f);
        auto inner = outer.deflate(18.0f, 14.0f);
        canvas.fill_drrect(outer, inner, Paint(CYAN));

        auto outer_snug = RRect::from_rect_and_radius(*Rect::from_xywh(220.0f, 110.0f, 80.0f, 60.0f), 25.0f);
        auto inner_snug = outer_snug.deflate(6.0f, 6.0f);
        canvas.fill_drrect(outer_snug, inner_snug, Paint(GREEN));
    }

    // 2e. stroke_drrect
    {
        auto outer2 = RRect::from_rect_and_radius(*Rect::from_xywh(220.0f, 180.0f, 80.0f, 50.0f), 6.0f);
        auto inner2 = outer2.deflate(10.0f, 10.0f);
        canvas.stroke_drrect(outer2, inner2, Paint(MAGENTA), Stroke(2.0f));
    }

    // 2f. draw_points (Points, Lines, Polygon)
    {
        std::array<Point, 2> pts{Point::from_xy(30.0f, 200.0f), Point::from_xy(60.0f, 200.0f)};
        Stroke pt_stroke(8.0f);
        pt_stroke.line_cap = LineCap::Round;
        canvas.draw_points(PointMode::Points, pts, Paint(Color::WHITE), pt_stroke);

        std::array<Point, 4> lines{
            Point::from_xy(80.0f, 190.0f), Point::from_xy(110.0f, 190.0f),
            Point::from_xy(80.0f, 210.0f), Point::from_xy(110.0f, 210.0f)
        };
        canvas.draw_points(PointMode::Lines, lines, Paint(GREEN), Stroke(4.0f));

        std::array<Point, 3> poly{
            Point::from_xy(125.0f, 190.0f), Point::from_xy(155.0f, 190.0f), Point::from_xy(155.0f, 215.0f)
        };
        canvas.draw_points(PointMode::Polygon, poly, Paint(BLUE), Stroke(4.0f));
    }

    // 2g. PaintStyle::StrokeAndFill
    {
        Paint sf_paint(RED);
        sf_paint.set_style(PaintStyle::StrokeAndFill);
        canvas.draw_rect(*Rect::from_xywh(175.0f, 185.0f, 30.0f, 30.0f), sf_paint, Stroke(6.0f));
    }

    canvas.flush();
    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    const auto r = pm->as_ref();

    // Skew assertions: row y=35 shifted right by 17.5 -> x in [27.5, 67.5]; row y=12 shifted by 6 -> x in [16, 56]
    assert(r.pixel(60, 35)->g > 200 && r.pixel(60, 35)->b > 200);
    assert(r.pixel(60, 12)->g < 10 && r.pixel(60, 12)->b < 10);

    // Oval assertions
    assert(r.pixel(160, 30)->r > 200 && r.pixel(160, 30)->g < 10);
    assert(r.pixel(122, 12)->r < 10);
    assert(r.pixel(160, 60)->g > 200 && r.pixel(160, 80)->g < 10);
    assert(r.pixel(50, 80)->b > 200 && r.pixel(22, 62)->b < 10);

    // Arc assertions
    assert(r.pixel(75, 155)->r > 200 && r.pixel(75, 155)->g > 200 && r.pixel(75, 155)->b < 10);
    assert(r.pixel(45, 125)->r < 10 && r.pixel(45, 155)->r < 10);

    // DRRect assertions
    assert(r.pixel(128, 140)->g > 200 && r.pixel(128, 140)->b > 200);
    assert(r.pixel(165, 140)->g < 10 && r.pixel(165, 140)->b < 10);
    assert(r.pixel(223, 140)->g > 200 && r.pixel(260, 140)->g < 10);
    assert(r.pixel(260, 180)->r > 200 && r.pixel(260, 190)->r > 200 && r.pixel(260, 185)->r < 10);

    // Points / Lines / Polygon assertions
    assert(r.pixel(30, 200)->r > 200 && r.pixel(60, 200)->r > 200 && r.pixel(45, 200)->r < 10);
    assert(r.pixel(95, 190)->g > 200 && r.pixel(95, 200)->g < 10 && r.pixel(95, 210)->g > 200);
    assert(r.pixel(140, 190)->b > 200 && r.pixel(155, 205)->b > 200);

    // PaintStyle::StrokeAndFill assertions (interior + expanded stroke rim)
    assert(r.pixel(190, 200)->r > 200 && r.pixel(173, 200)->r > 200);

    // 3. CPU Canvas parity checks (draw_oval, draw_drrect, StrokeAndFill, elliptical stroke_round_rect)
    {
        auto cpu_pm = Pixmap::allocate(160, 120, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(Color::BLACK);
        Canvas cpu_canvas(cpu_pm->as_mut());

        Paint sf(RED);
        sf.set_style(PaintStyle::StrokeAndFill);
        cpu_canvas.draw_rect(*Rect::from_xywh(10.0f, 10.0f, 20.0f, 20.0f), sf, Stroke(4.0f));
        assert(cpu_pm->as_ref().pixel(20, 20)->r > 200);
        assert(cpu_pm->as_ref().pixel(9, 20)->r > 200);

        // Elliptical stroke_round_rect (rx=24, ry=6) on [50, 10, 80, 40]
        cpu_canvas.stroke_round_rect(*Rect::from_xywh(50.0f, 10.0f, 80.0f, 40.0f), 24.0f, 6.0f, Paint(GREEN), Stroke(2.0f));
        assert(cpu_pm->as_ref().pixel(50, 10)->g < 20); // Corner cut out
        assert(cpu_pm->as_ref().pixel(80, 10)->g > 150); // Straight horizontal top segment

        auto outer = RRect::from_rect_and_radius(*Rect::from_xywh(10.0f, 55.0f, 60.0f, 50.0f), 8.0f);
        cpu_canvas.fill_drrect(outer, outer.deflate(10.0f, 10.0f), Paint(BLUE));
        assert(cpu_pm->as_ref().pixel(15, 80)->b > 200 && cpu_pm->as_ref().pixel(40, 80)->b < 20);
    }

    std::cout << "  -> GPU & CPU P1.8 Geometry APIs OK" << std::endl;
}

void test_gpu_text_buffer_parity() {
    std::cout << "Testing CPU-vs-GPU P1.6-PartA text baseline, background, & decoration parity..." << std::endl;

    std::string font_path = resolve_test_font("NotoSans-Regular.ttf");
    text::FontSystem font_system;
    auto fid = font_system.load_font_file(font_path);
    assert(fid.has_value());
    font_system.set_default_font(*fid);

    text::Buffer buf(text::Metrics(24.0f, 34.0f));
    buf.set_text("HHH UUU SSS");
    {
        auto& attrs = buf.lines_mut()[0].attrs_list_mut();
        text::Attrs bg_attrs;
        bg_attrs.set_color(text::TextColor::rgb(255, 255, 255))
                .set_background_color(text::TextColor::rgb(0, 0, 220));
        text::Attrs ul_attrs;
        ul_attrs.set_color(text::TextColor::rgb(255, 0, 0)).set_underline(true);
        text::Attrs st_attrs;
        st_attrs.set_color(text::TextColor::rgb(0, 255, 0)).set_strikethrough(true);
        attrs.add_span(0, 3, bg_attrs);
        attrs.add_span(4, 7, ul_attrs);
        attrs.add_span(8, 11, st_attrs);
    }

    constexpr uint32_t W = 360;
    constexpr uint32_t H = 100;
    const float ox = 20.0f;
    const float oy = 20.0f;

    auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
    assert(cpu_pm.has_value());
    cpu_pm->fill(Color::BLACK);
    {
        Canvas cpu_canvas(cpu_pm->as_mut());
        text::GlyphCache cpu_cache(128);
        buf.draw(cpu_canvas, cpu_cache, font_system, Color::WHITE, ox, oy);
    }

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, W, H);
    assert(surface != nullptr);
    GpuCanvas gpu_canvas(surface);
    gpu_canvas.clear(Color::BLACK);
    gpu_canvas.draw_text_buffer(buf, font_system, Point(ox, oy), Color::WHITE);
    gpu_canvas.flush();
    auto gpu_pm = surface->to_pixmap();
    assert(gpu_pm.has_value());

    const auto cpu_r = cpu_pm->as_ref();
    const auto gpu_r = gpu_pm->as_ref();

    // 1. Background highlight parity on word 0 ("HHH"): blue fill inside line_top..line_top+line_height [20..54)
    auto find_blue_bounds = [&](const PixmapRef& pm, uint32_t& min_y, uint32_t& max_y, size_t& count) {
        min_y = H; max_y = 0; count = 0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto p = pm.pixel(x, y);
                if (p && p->b > 180 && p->r < 40 && p->g < 40) {
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                    ++count;
                }
            }
        }
    };
    uint32_t cpu_bg_y0, cpu_bg_y1, gpu_bg_y0, gpu_bg_y1;
    size_t cpu_bg_cnt, gpu_bg_cnt;
    find_blue_bounds(cpu_r, cpu_bg_y0, cpu_bg_y1, cpu_bg_cnt);
    find_blue_bounds(gpu_r, gpu_bg_y0, gpu_bg_y1, gpu_bg_cnt);
    assert(cpu_bg_cnt > 300 && gpu_bg_cnt > 300);
    assert(std::abs(int(cpu_bg_y0) - int(gpu_bg_y0)) <= 1);
    assert(std::abs(int(cpu_bg_y1) - int(gpu_bg_y1)) <= 1);
    assert(gpu_r.pixel(30, 15)->b < 10 && gpu_r.pixel(30, 60)->b < 10);

    // 2. Baseline Y parity + underline parity on red glyphs ("UUU")
    auto find_red_bounds = [&](const PixmapRef& pm, uint32_t& min_y, uint32_t& max_y, uint32_t& best_row, uint32_t& best_row_cnt) {
        min_y = H; max_y = 0; best_row = 0; best_row_cnt = 0;
        for (uint32_t y = 0; y < H; ++y) {
            uint32_t row_cnt = 0;
            for (uint32_t x = 0; x < W; ++x) {
                auto p = pm.pixel(x, y);
                if (p && p->r > 150 && p->g < 40 && p->b < 40) {
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                    ++row_cnt;
                }
            }
            if (row_cnt > best_row_cnt) {
                best_row_cnt = row_cnt;
                best_row = y;
            }
        }
    };
    uint32_t cpu_ry0, cpu_ry1, cpu_ul_y, cpu_ul_w;
    uint32_t gpu_ry0, gpu_ry1, gpu_ul_y, gpu_ul_w;
    find_red_bounds(cpu_r, cpu_ry0, cpu_ry1, cpu_ul_y, cpu_ul_w);
    find_red_bounds(gpu_r, gpu_ry0, gpu_ry1, gpu_ul_y, gpu_ul_w);
    assert(cpu_ry0 < cpu_ry1 && gpu_ry0 < gpu_ry1);
    assert(std::abs(int(cpu_ry0) - int(gpu_ry0)) <= 2);
    assert(std::abs(int(cpu_ry1) - int(gpu_ry1)) <= 2);
    assert(cpu_ul_w > 30 && gpu_ul_w > 30);
    assert(std::abs(int(cpu_ul_y) - int(gpu_ul_y)) <= 1);

    // 3. Strikethrough parity on green glyphs ("SSS"): brightest wide horizontal green bar across word
    auto find_green_bar = [&](const PixmapRef& pm, uint32_t& min_y, uint32_t& max_y, uint32_t& best_row, uint32_t& best_row_cnt) {
        min_y = H; max_y = 0; best_row = 0; best_row_cnt = 0;
        for (uint32_t y = 0; y < H; ++y) {
            uint32_t row_cnt = 0;
            for (uint32_t x = 0; x < W; ++x) {
                auto p = pm.pixel(x, y);
                if (p && p->g > 180 && p->r < 40 && p->b < 40) {
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                    ++row_cnt;
                }
            }
            if (row_cnt > best_row_cnt) {
                best_row_cnt = row_cnt;
                best_row = y;
            }
        }
    };
    uint32_t cpu_gy0, cpu_gy1, cpu_st_y, cpu_st_w;
    uint32_t gpu_gy0, gpu_gy1, gpu_st_y, gpu_st_w;
    find_green_bar(cpu_r, cpu_gy0, cpu_gy1, cpu_st_y, cpu_st_w);
    find_green_bar(gpu_r, gpu_gy0, gpu_gy1, gpu_st_y, gpu_st_w);
    assert(cpu_st_w > 25 && gpu_st_w > 25);
    assert(std::abs(int(cpu_gy0) - int(gpu_gy0)) <= 2);
    assert(std::abs(int(cpu_gy1) - int(gpu_gy1)) <= 2);
    assert(std::abs(int(cpu_st_y) - int(gpu_st_y)) <= 1);
    assert(cpu_st_y < cpu_ul_y && gpu_st_y < gpu_ul_y);

    std::cout << "  -> P1.6-PartA Text baseline, bg, & decorations parity OK" << std::endl;
}

static void test_gpu_blend_modes_parity() {
    std::cout << "[Test 20/20] P0.2 All 29 BlendModes CPU-vs-GPU parity & MSAA..." << std::endl;

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

    constexpr uint32_t W = 64;
    constexpr uint32_t H = 64;
    auto device = GpuDevice::create();
    assert(device != nullptr);

    const Color bg_clear = Color::TRANSPARENT;
    const Color dst_col = Color::from_rgba8(64, 180, 210, 170);
    const Color src_col = Color::from_rgba8(220, 90, 45, 190);
    const Rect r_dst = *Rect::from_xywh(6.0f, 6.0f, 40.0f, 40.0f);
    const Rect r_src = *Rect::from_xywh(20.0f, 20.0f, 38.0f, 38.0f);

    auto surface = GpuSurface::create(device, W, H);
    assert(surface != nullptr);

    for (size_t idx = 0; idx < kAllModes.size(); ++idx) {
        BlendMode mode = kAllModes[idx];

        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(bg_clear);
        {
            Canvas cpu(cpu_pm->as_mut());
            cpu.fill_rect(r_dst, Paint(dst_col));
            Paint src_paint(src_col);
            src_paint.blend_mode = mode;
            cpu.fill_rect(r_src, src_paint);
            cpu.fill_circle(32.0f, 16.0f, 10.5f, src_paint);
        }

        GpuCanvas gpu(surface);
        gpu.clear(bg_clear);
        gpu.fill_rect(r_dst, Paint(dst_col));
        {
            Paint src_paint(src_col);
            src_paint.blend_mode = mode;
            gpu.fill_rect(r_src, src_paint);
            gpu.fill_circle(32.0f, 16.0f, 10.5f, src_paint);
        }
        gpu.flush();
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();

        auto check_pt = [&](uint32_t px, uint32_t py, int tol) {
            auto cp = cpu_r.pixel(px, py);
            auto gp = gpu_r.pixel(px, py);
            assert(cp.has_value() && gp.has_value());
            int dr = std::abs(int(cp->red()) - int(gp->red()));
            int dg = std::abs(int(cp->green()) - int(gp->green()));
            int db = std::abs(int(cp->blue()) - int(gp->blue()));
            int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
            assert(dr <= tol && dg <= tol && db <= tol && da <= tol);
        };

        // Untouched transparent background
        check_pt(2, 2, 1);
        // Backdrop-only region (must remain intact even after Clear/Source AA quad draws!)
        check_pt(12, 12, 2);
        // Overlap interior: src over semi-transparent dst
        check_pt(30, 30, 3);
        // Overlap with transparent bg outside dst
        check_pt(50, 50, 2);
        // Circle center (semi-transparent dst behind) & outside bounding corner of circle quad
        check_pt(32, 16, 3);
        check_pt(21, 6, 3);
    }

    // Verify MSAA surface snapshot resolve with programmable blend modes
    auto msaa_surface = GpuSurface::create(device, W, H, 4);
    assert(msaa_surface != nullptr);
    constexpr std::array<BlendMode, 6> kMsaaModes = {
        BlendMode::Multiply, BlendMode::SoftLight, BlendMode::ColorDodge,
        BlendMode::Hue, BlendMode::Modulate, BlendMode::Source
    };
    for (BlendMode mode : kMsaaModes) {
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(bg_clear);
        {
            Canvas cpu(cpu_pm->as_mut());
            cpu.fill_rect(r_dst, Paint(dst_col));
            Paint p(src_col);
            p.blend_mode = mode;
            cpu.fill_rect(r_src, p);
        }

        GpuCanvas gpu(msaa_surface);
        gpu.clear(bg_clear);
        gpu.fill_rect(r_dst, Paint(dst_col));
        {
            Paint p(src_col);
            p.blend_mode = mode;
            gpu.fill_rect(r_src, p);
        }
        gpu.flush();
        auto gpu_pm = msaa_surface->to_pixmap();
        assert(gpu_pm.has_value());

        auto cp = cpu_pm->as_ref().pixel(30, 30);
        auto gp = gpu_pm->as_ref().pixel(30, 30);
        assert(cp.has_value() && gp.has_value());
        assert(std::abs(int(cp->red()) - int(gp->red())) <= 3);
        assert(std::abs(int(cp->green()) - int(gp->green())) <= 3);
        assert(std::abs(int(cp->blue()) - int(gp->blue())) <= 3);
        assert(std::abs(int(cp->alpha()) - int(gp->alpha())) <= 3);
    }

    std::cout << "  -> P0.2 All 29 BlendModes CPU-vs-GPU parity & MSAA OK" << std::endl;
}

static void test_gpu_save_layer_parity() {
    std::cout << "[TEST] P0.1 save_layer / bounds / backdrop CPU-vs-GPU Parity..." << std::endl;

    auto dev = GpuDevice::create_headless();
    assert(dev && dev->is_valid());

    constexpr uint32_t W = 200;
    constexpr uint32_t H = 200;
    const Color bg = Color::from_rgba8(30, 60, 120, 255);

    auto render_scene = [&](ICanvas& c) {
        c.save();
        // 1. Opacity isolation: opaque red overlapped by opaque green inside 0.5 layer
        c.save_layer_alpha(0.5f);
        c.fill_rect(*Rect::from_xywh(10, 10, 70, 70), Paint(Color::from_rgba8(255, 0, 0, 255)));
        c.fill_rect(*Rect::from_xywh(30, 30, 70, 70), Paint(Color::from_rgba8(0, 255, 0, 255)));
        c.restore();

        // 2. Bounded layer with BlendMode::Multiply + ColorMatrix + clip path
        {
            Paint lp(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, 0.8f));
            lp.blend_mode = BlendMode::Multiply;
            lp.set_color_filter(effects::ColorMatrix::invert());

            PathBuilder clip_pb;
            clip_pb.push_circle(140.0f, 55.0f, 40.0f);
            auto clip_p = clip_pb.finish();
            assert(clip_p.has_value());

            c.save();
            c.clip_path(*clip_p);
            c.save_layer_bounds(*Rect::from_xywh(100, 15, 80, 80), lp);
            c.translate(100.0f, 15.0f);
            // White inverted -> black (0,0,0,1); 0.8 alpha Multiply over bg
            c.fill_rect(*Rect::from_xywh(-10, -10, 100, 100), Paint(Color::from_rgba8(255, 255, 255, 255)));
            c.restore();
            c.restore();
        }

        // 3. Nested layers
        {
            Paint outer_p(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, 0.5f));
            Paint inner_p(Color::WHITE);
            inner_p.blend_mode = BlendMode::Plus;
            c.save_layer(outer_p);
            c.fill_rect(*Rect::from_xywh(110, 115, 70, 70), Paint(Color::from_rgba8(200, 0, 0, 255)));
            c.save_layer(inner_p);
            c.fill_rect(*Rect::from_xywh(125, 130, 40, 40), Paint(Color::from_rgba8(0, 200, 0, 255)));
            c.restore();
            c.restore();
        }

        // 4. Backdrop blur layer on sharp step edge
        c.fill_rect(*Rect::from_xywh(10, 115, 40, 70), Paint(Color::from_rgba8(255, 255, 255, 255)));
        c.fill_rect(*Rect::from_xywh(50, 115, 40, 70), Paint(Color::from_rgba8(0, 0, 0, 255)));
        c.save_layer_backdrop(*Rect::from_xywh(20, 125, 60, 50), Paint(Color::WHITE), 4.0f);
        c.fill_rect(*Rect::from_xywh(45, 145, 10, 10), Paint(Color::from_rgba8(255, 0, 0, 255)));
        c.restore();

        c.restore();
    };

    auto cpu_pm = Pixmap::allocate(W, H);
    assert(cpu_pm.has_value());
    {
        Canvas cpu(*cpu_pm);
        cpu.clear(bg);
        render_scene(cpu);
    }

    for (uint32_t samples : {1u, 4u}) {
        auto surface = GpuSurface::create(dev, W, H, samples);
        assert(surface);
        GpuCanvas gpu(surface);
        gpu.clear(bg);
        render_scene(gpu);
        gpu.flush();

        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        auto check_pt = [&](uint32_t x, uint32_t y, int tol) {
            auto cp = cpu_pm->as_ref().pixel(x, y);
            auto gp = gpu_pm->as_ref().pixel(x, y);
            assert(cp.has_value() && gp.has_value());
            assert(std::abs(int(cp->red()) - int(gp->red())) <= tol);
            assert(std::abs(int(cp->green()) - int(gp->green())) <= tol);
            assert(std::abs(int(cp->blue()) - int(gp->blue())) <= tol);
            assert(std::abs(int(cp->alpha()) - int(gp->alpha())) <= tol);
        };

        // 1. Isolated opacity checks (red-only, overlap green-over-red, green-only)
        check_pt(15, 15, 3);
        check_pt(45, 45, 3);
        check_pt(90, 90, 3);

        // 2. Bounded + Multiply + Invert ColorMatrix + Outer Circular Clip
        check_pt(140, 55, 4);   // Inside both bounds and circle clip
        check_pt(102, 17, 3);   // Inside bounds [100..180] but OUTSIDE circle clip (r=40 at 140,55)
        check_pt(185, 55, 3);   // Outside bounds

        // 3. Nested layers
        check_pt(145, 150, 4);
        check_pt(115, 120, 3);

        // 4. Backdrop blur: check step transition softened & center red painted over blurred backdrop
        check_pt(50, 135, 15);  // Both CPU box-gaussian and GPU separable Gaussian blur step ~127
        check_pt(50, 150, 3);   // Center red drawn on top of backdrop
    }

    std::cout << "  -> P0.1 save_layer / bounds / backdrop CPU-vs-GPU Parity (1x & 4x MSAA) OK" << std::endl;
}

static void test_gpu_gradients_and_pattern_parity() {
    std::cout << "[TEST] P0.3 Full GPU Gradients & Pattern (ImageShader) CPU-vs-GPU Parity..." << std::endl;

    auto dev = GpuDevice::create_headless();
    assert(dev && dev->is_valid());

    constexpr uint32_t W = 240;
    constexpr uint32_t H = 200;
    const Color bg = Color::from_rgba8(20, 28, 44, 255);

    auto run_parity_case = [&](const char* label, auto&& draw_fn,
                               const std::vector<std::pair<uint32_t, uint32_t>>& pts,
                               int pt_tol = 4, float max_avg_diff = 3.0f) {
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(bg);
        {
            Canvas cpu(cpu_pm->as_mut());
            draw_fn(cpu);
        }

        auto surface = GpuSurface::create(dev, W, H, 1);
        assert(surface != nullptr);
        {
            GpuCanvas gpu(surface);
            gpu.clear(bg);
            draw_fn(gpu);
            gpu.flush();
        }
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();

        for (auto [px, py] : pts) {
            auto cp = cpu_r.pixel(px, py);
            auto gp = gpu_r.pixel(px, py);
            assert(cp.has_value() && gp.has_value());
            int dr = std::abs(int(cp->red()) - int(gp->red()));
            int dg = std::abs(int(cp->green()) - int(gp->green()));
            int db = std::abs(int(cp->blue()) - int(gp->blue()));
            int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
            if (dr > pt_tol || dg > pt_tol || db > pt_tol || da > pt_tol) {
                std::cerr << "  [MISMATCH] " << label << " @(" << px << "," << py << ") "
                          << "CPU=(" << int(cp->red()) << "," << int(cp->green()) << "," << int(cp->blue()) << "," << int(cp->alpha()) << ") "
                          << "GPU=(" << int(gp->red()) << "," << int(gp->green()) << "," << int(gp->blue()) << "," << int(gp->alpha()) << ")\n";
                assert(false);
            }
        }

        double sum_diff = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                int dr = std::abs(int(cp->red()) - int(gp->red()));
                int dg = std::abs(int(cp->green()) - int(gp->green()));
                int db = std::abs(int(cp->blue()) - int(gp->blue()));
                int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
                sum_diff += (dr + dg + db + da) * 0.25;
            }
        }
        double avg_diff = sum_diff / double(W * H);
        if (avg_diff > max_avg_diff) {
            std::cerr << "  [AVG MISMATCH] " << label << " avg_diff=" << avg_diff << " > " << max_avg_diff << "\n";
            assert(false);
        }
    };

    const std::vector<GradientStop> stops4 = {
        GradientStop::create(0.0f, Color::from_rgba8(240, 40, 40, 255)),
        GradientStop::create(0.35f, Color::from_rgba8(240, 220, 40, 220)),
        GradientStop::create(0.7f, Color::from_rgba8(40, 210, 120, 180)),
        GradientStop::create(1.0f, Color::from_rgba8(50, 90, 255, 255)),
    };

    // 1. Multi-stop LinearGradient: Pad, Repeat, Reflect + Shader Transform + RoundRect & Stroke
    run_parity_case("LinearGradient Pad/Repeat/Reflect + Transform", [&](ICanvas& c) {
        Transform grad_xf = Transform::from_translate(12.0f, 16.0f).post_concat(Transform::from_rotate(22.0f));
        auto lg_pad = LinearGradient::create(Point::from_xy(20.0f, 20.0f), Point::from_xy(70.0f, 20.0f), stops4, SpreadMode::Pad, grad_xf);
        auto lg_rep = LinearGradient::create(Point::from_xy(100.0f, 20.0f), Point::from_xy(130.0f, 20.0f), stops4, SpreadMode::Repeat);
        auto lg_ref = LinearGradient::create(Point::from_xy(170.0f, 20.0f), Point::from_xy(200.0f, 20.0f), stops4, SpreadMode::Reflect);
        assert(lg_pad && lg_rep && lg_ref);

        c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 70.0f, 70.0f), Paint(Shader(*lg_pad)));
        c.fill_round_rect(*Rect::from_xywh(90.0f, 10.0f, 70.0f, 70.0f), 10.0f, 10.0f, Paint(Shader(*lg_rep)));
        c.fill_rect(*Rect::from_xywh(165.0f, 10.0f, 65.0f, 70.0f), Paint(Shader(*lg_ref)));

        // Stroke with multi-stop Repeat gradient + canvas scale
        c.save();
        c.translate(10.0f, 100.0f);
        auto lg_stroke = LinearGradient::create(Point::from_xy(0.0f, 0.0f), Point::from_xy(35.0f, 25.0f), stops4, SpreadMode::Repeat);
        c.stroke_rect(*Rect::from_xywh(10.0f, 10.0f, 190.0f, 65.0f), Paint(Shader(*lg_stroke)), Stroke(8.0f));
        c.restore();
    }, {{25, 35}, {55, 50}, {108, 45}, {140, 45}, {182, 45}, {212, 45}, {24, 140}, {180, 110}}, 5, 2.5f);

    // 2. >16 stop LUT fallback (20 stops) Linear & RadialGradient
    {
        std::vector<GradientStop> stops20;
        for (int i = 0; i < 20; ++i) {
            float t = float(i) / 19.0f;
            uint8_t r = uint8_t(127.5f + 127.5f * std::sin(t * 6.28318f));
            uint8_t g = uint8_t(127.5f + 127.5f * std::cos(t * 4.5f));
            uint8_t b = uint8_t(255.0f * (1.0f - t));
            uint8_t a = uint8_t(160.0f + 95.0f * t);
            stops20.push_back(GradientStop::create(t, Color::from_rgba8(r, g, b, a)));
        }
        run_parity_case("20-stop LUT Linear & Radial", [&](ICanvas& c) {
            auto lg20 = LinearGradient::create(Point::from_xy(15.0f, 20.0f), Point::from_xy(95.0f, 90.0f), stops20, SpreadMode::Reflect);
            auto rg20 = RadialGradient::create(Point::from_xy(170.0f, 100.0f), 42.0f, stops20, SpreadMode::Repeat);
            assert(lg20 && rg20);
            c.fill_rect(*Rect::from_xywh(10.0f, 15.0f, 105.0f, 160.0f), Paint(Shader(*lg20)));
            c.fill_oval(*Rect::from_xywh(110.0f, 40.0f, 120.0f, 120.0f), Paint(Shader(*rg20)));
        }, {{30, 40}, {60, 80}, {90, 140}, {170, 100}, {195, 100}, {140, 75}}, 6, 3.0f);
    }

    // 3. RadialGradient (1pt & 2pt) & TwoPointConicalGradient (concentric & eccentric)
    run_parity_case("Radial 1pt/2pt & TwoPointConical", [&](ICanvas& c) {
        auto rg_rep = RadialGradient::create(Point::from_xy(55.0f, 50.0f), 22.0f, stops4, SpreadMode::Repeat);
        auto rg_2pt = RadialGradient::create_2point(
            Point::from_xy(155.0f, 45.0f), 10.0f,
            Point::from_xy(185.0f, 55.0f), 40.0f,
            stops4, SpreadMode::Reflect);
        auto cg_con = TwoPointConicalGradient::create(
            Point::from_xy(60.0f, 145.0f), 8.0f,
            Point::from_xy(60.0f, 145.0f), 42.0f,
            stops4, SpreadMode::Pad);
        auto cg_ecc = TwoPointConicalGradient::create(
            Point::from_xy(150.0f, 145.0f), 6.0f,
            Point::from_xy(190.0f, 150.0f), 34.0f,
            stops4, SpreadMode::Repeat);
        assert(rg_rep && rg_2pt && cg_con && cg_ecc);

        c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 95.0f, 80.0f), Paint(Shader(*rg_rep)));
        c.fill_rect(*Rect::from_xywh(120.0f, 10.0f, 110.0f, 80.0f), Paint(Shader(*rg_2pt)));
        c.fill_rect(*Rect::from_xywh(10.0f, 100.0f, 95.0f, 90.0f), Paint(Shader(*cg_con)));
        c.fill_rect(*Rect::from_xywh(120.0f, 100.0f, 110.0f, 90.0f), Paint(Shader(*cg_ecc)));
    }, {{55, 50}, {85, 50}, {165, 50}, {210, 50}, {60, 145}, {80, 145}, {165, 148}, {195, 150}}, 6, 3.0f);

    // 4. SweepGradient (full & partial angle arc, Pad/Repeat/Reflect + Transform + ColorMatrix)
    run_parity_case("SweepGradient + ColorMatrix", [&](ICanvas& c) {
        auto sg_pad = SweepGradient::create(Point::from_xy(55.0f, 55.0f), 30.0f, 240.0f, stops4, SpreadMode::Pad);
        auto sg_rep = SweepGradient::create(Point::from_xy(175.0f, 55.0f), 0.0f, 120.0f, stops4, SpreadMode::Repeat,
                                            Transform::from_rotate_at(35.0f, 175.0f, 55.0f));
        auto sg_ref = SweepGradient::create(Point::from_xy(115.0f, 145.0f), 20.0f, 140.0f, stops4, SpreadMode::Reflect);
        assert(sg_pad && sg_rep && sg_ref);

        c.fill_oval(*Rect::from_xywh(13.0f, 13.0f, 84.0f, 84.0f), Paint(Shader(*sg_pad)));
        c.fill_oval(*Rect::from_xywh(133.0f, 13.0f, 84.0f, 84.0f), Paint(Shader(*sg_rep)));

        Paint p_gray{Shader(*sg_ref)};
        p_gray.set_color_filter(effects::ColorMatrix::saturation(0.0f));
        c.fill_rect(*Rect::from_xywh(40.0f, 105.0f, 150.0f, 80.0f), p_gray);
    }, {{78, 55}, {35, 35}, {195, 65}, {155, 45}, {145, 145}, {85, 145}}, 6, 3.5f);

    // 5. Pattern (ImageShader): strided BGRA8888, Pad/Repeat/Reflect, Nearest/Bilinear, Transform, opacity, Path & Stroke + ColorMatrix::invert
    {
        // 4x4 strided BGRA8888 pattern (stride = 24 bytes)
        std::vector<uint8_t> raw_bgra(24 * 4, 0x55);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                size_t off = y * 24 + x * 4;
                bool cx = (x < 2);
                bool cy = (y < 2);
                uint8_t r = cx ? 240 : 40;
                uint8_t g = cy ? 220 : 50;
                uint8_t b = (cx != cy) ? 240 : 30;
                uint8_t a = ((x + y) % 2 == 0) ? 255 : 180;
                uint8_t rp = static_cast<uint8_t>((uint32_t(r) * a + 127) / 255);
                uint8_t gp = static_cast<uint8_t>((uint32_t(g) * a + 127) / 255);
                uint8_t bp = static_cast<uint8_t>((uint32_t(b) * a + 127) / 255);
                raw_bgra[off + 0] = bp;
                raw_bgra[off + 1] = gp;
                raw_bgra[off + 2] = rp;
                raw_bgra[off + 3] = a;
            }
        }
        auto tile_ref = PixmapRef::from_raw_parts(raw_bgra.data(), 4, 4, 24, PixelFormat::BGRA8888);
        assert(tile_ref.has_value());

        run_parity_case("Pattern ImageShader Spread/Filter/Transform/Opacity", [&](ICanvas& c) {
            Transform px_rep = Transform::from_scale(8.0f, 8.0f);
            Transform px_rot = Transform::from_translate(20.0f, 20.0f)
                                   .post_concat(Transform::from_rotate(20.0f))
                                   .post_concat(Transform::from_scale(6.0f, 6.0f));

            Pattern pat_rep(*tile_ref, SpreadMode::Repeat, FilterQuality::Nearest, 0.85f, px_rep);
            Pattern pat_ref(*tile_ref, SpreadMode::Reflect, FilterQuality::Bilinear, 0.75f, px_rot);
            Pattern pat_pad(*tile_ref, SpreadMode::Pad, FilterQuality::Bilinear, 1.0f,
                            Transform::from_translate(15.0f, 115.0f).post_concat(Transform::from_scale(12.0f, 12.0f)));

            c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 95.0f, 80.0f), Paint(Shader(pat_rep)));

            Paint p_ref_inv{Shader(pat_ref)};
            p_ref_inv.set_color_filter(effects::ColorMatrix::invert());
            c.fill_round_rect(*Rect::from_xywh(120.0f, 10.0f, 105.0f, 80.0f), 12.0f, 12.0f, p_ref_inv);

            // Arbitrary concave triangle path + stroked rect using Pattern
            PathBuilder pb;
            pb.move_to(20.0f, 110.0f);
            pb.line_to(110.0f, 125.0f);
            pb.line_to(65.0f, 185.0f);
            pb.close();
            auto tri = pb.finish();
            assert(tri.has_value());
            c.fill_path(*tri, Paint(Shader(pat_pad)));

            c.stroke_rect(*Rect::from_xywh(130.0f, 115.0f, 90.0f, 65.0f), Paint(Shader(pat_rep)), Stroke(8.0f));
        }, {{20, 20}, {42, 42}, {150, 35}, {180, 55}, {55, 135}, {132, 135}}, 8, 5.0f);
    }

    std::cout << "  -> P0.3 Full GPU Gradients & Pattern (ImageShader) CPU-vs-GPU Parity OK" << std::endl;
}

static void test_gpu_effects_and_shadows_parity() {
    std::cout << "[TEST] P0.4-B ColorEffect, ImageFilter Blur/DropShadow & RRect/Circle Shadows CPU-vs-GPU Parity..." << std::endl;

    auto dev = GpuDevice::create_headless();
    assert(dev && dev->is_valid());

    constexpr uint32_t W = 240;
    constexpr uint32_t H = 200;
    const Color bg = Color::from_rgba8(230, 235, 245, 255);

    auto run_parity_case = [&](const char* label, auto&& draw_fn,
                               const std::vector<std::pair<uint32_t, uint32_t>>& pts,
                               int pt_tol = 6, float max_avg_diff = 3.5f) {
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(bg);
        {
            Canvas cpu(cpu_pm->as_mut());
            draw_fn(cpu);
        }

        auto surface = GpuSurface::create(dev, W, H, 1);
        assert(surface != nullptr);
        {
            GpuCanvas gpu(surface);
            gpu.clear(bg);
            draw_fn(gpu);
            gpu.flush();
        }
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();

        for (auto [px, py] : pts) {
            auto cp = cpu_r.pixel(px, py);
            auto gp = gpu_r.pixel(px, py);
            assert(cp.has_value() && gp.has_value());
            int dr = std::abs(int(cp->red()) - int(gp->red()));
            int dg = std::abs(int(cp->green()) - int(gp->green()));
            int db = std::abs(int(cp->blue()) - int(gp->blue()));
            int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
            if (dr > pt_tol || dg > pt_tol || db > pt_tol || da > pt_tol) {
                std::cerr << "  [MISMATCH] " << label << " @(" << px << "," << py << ") "
                          << "CPU=(" << int(cp->red()) << "," << int(cp->green()) << "," << int(cp->blue()) << "," << int(cp->alpha()) << ") "
                          << "GPU=(" << int(gp->red()) << "," << int(gp->green()) << "," << int(gp->blue()) << "," << int(gp->alpha()) << ")\n";
                assert(false);
            }
        }

        double sum_diff = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                int dr = std::abs(int(cp->red()) - int(gp->red()));
                int dg = std::abs(int(cp->green()) - int(gp->green()));
                int db = std::abs(int(cp->blue()) - int(gp->blue()));
                int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
                sum_diff += (dr + dg + db + da) * 0.25;
            }
        }
        double avg_diff = sum_diff / double(W * H);
        if (avg_diff > max_avg_diff) {
            std::cerr << "  [AVG MISMATCH] " << label << " avg_diff=" << avg_diff << " > " << max_avg_diff << "\n";
            assert(false);
        }
    };

    // 1. ColorEffect variants (SourceAtop, Multiply, Screen, Tint, srgb_to_linear, linear_to_srgb)
    run_parity_case("ColorEffect Mode & Gamma", [&](ICanvas& c) {
        Paint p1(Color::from_rgba8(60, 140, 220, 255));
        p1.set_color_filter(effects::ColorEffect::mode(Color::from_rgba8(220, 40, 40, 140), BlendMode::SourceAtop));
        c.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 65.0f, 75.0f), p1);

        Paint p2(Color::from_rgba8(200, 180, 100, 255));
        p2.set_color_filter(effects::ColorEffect::mode(Color::from_rgba8(120, 200, 240, 255), BlendMode::Multiply));
        c.fill_rect(*Rect::from_xywh(85.0f, 10.0f, 65.0f, 75.0f), p2);

        Paint p3(Color::from_rgba8(80, 120, 180, 255));
        p3.set_color_filter(effects::ColorEffect::mode(Color::from_rgba8(90, 60, 40, 255), BlendMode::Screen));
        c.fill_rect(*Rect::from_xywh(160.0f, 10.0f, 65.0f, 75.0f), p3);

        Paint p4(Color::from_rgba8(140, 180, 90, 255));
        p4.set_color_filter(effects::ColorEffect::srgb_to_linear());
        c.fill_rect(*Rect::from_xywh(10.0f, 105.0f, 65.0f, 75.0f), p4);

        Paint p5(Color::from_rgba8(80, 120, 200, 255));
        p5.set_color_filter(effects::ColorEffect::linear_to_srgb());
        c.fill_rect(*Rect::from_xywh(85.0f, 105.0f, 65.0f, 75.0f), p5);

        Paint p6(Color::from_rgba8(180, 180, 180, 255));
        p6.set_color_filter(effects::ColorEffect::tint(Color::from_rgba8(40, 200, 120, 255), 0.65f));
        c.fill_rect(*Rect::from_xywh(160.0f, 105.0f, 65.0f, 75.0f), p6);
    }, {{40, 40}, {115, 40}, {190, 40}, {40, 140}, {115, 140}, {190, 140}}, 5, 2.0f);

    // 2. Paint::set_image_filter(Blur & DropShadow) + SaveLayer composite opacity + ColorEffect
    run_parity_case("ImageFilter Blur & DropShadow", [&](ICanvas& c) {
        Paint bp(Color::from_rgba8(220, 70, 50, 255));
        bp.set_image_filter(effects::ImageFilter::blur(3.5f, 1.5f));
        c.fill_round_rect(*Rect::from_xywh(20.0f, 20.0f, 80.0f, 60.0f), 10.0f, 10.0f, bp);

        Paint dsp(Color::from_rgba8(40, 150, 230, 255));
        dsp.set_image_filter(effects::ImageFilter::drop_shadow(
            6.0f, 6.0f, 3.0f, 3.0f, Color::from_rgba8(10, 20, 40, 190), false));
        c.fill_rect(*Rect::from_xywh(130.0f, 22.0f, 75.0f, 55.0f), dsp);

        // shadow_only + layer opacity + ColorEffect
        Paint layer_p(Color::from_rgba8(255, 255, 255, 200));
        layer_p.set_image_filter(effects::ImageFilter::drop_shadow(
            5.0f, 4.0f, 2.5f, 2.5f, Color::from_rgba8(20, 120, 60, 220), true));
        auto bnds = Rect::from_xywh(20.0f, 110.0f, 95.0f, 75.0f);
        c.save_layer_bounds(*bnds, layer_p);
        c.fill_rect(*Rect::from_xywh(30.0f, 120.0f, 60.0f, 45.0f), Paint(Color::WHITE));
        c.restore();
    }, {{60, 50}, {165, 50}, {175, 81}, {65, 145}}, 12, 4.5f);

    // 3. Asymmetric 4-corner draw_rrect_with_shadow, draw_circle_with_shadow, draw_path_with_shadow
    run_parity_case("Asymmetric RRect & Circle DropShadow", [&](ICanvas& c) {
        auto rr = RRect::from_xywh(20.0f, 20.0f, 85.0f, 70.0f, BorderRadius::only(24.0f, 0.0f, 16.0f, 6.0f));
        assert(rr.has_value());
        effects::DropShadow ds(6.0f, 7.0f, 4.0f, Color::from_rgba8(15, 20, 35, 180));
        c.draw_rrect_with_shadow(*rr, Paint(Color::from_rgba8(55, 125, 235, 255)), ds);

        c.draw_circle_with_shadow(175.0f, 60.0f, 32.0f, Paint(Color::from_rgba8(235, 90, 65, 255)), ds);

        PathBuilder pb;
        pb.move_to(55.0f, 115.0f);
        pb.line_to(115.0f, 175.0f);
        pb.line_to(25.0f, 175.0f);
        pb.close();
        auto tri = pb.finish();
        assert(tri.has_value());
        c.draw_path_with_shadow(*tri, Paint(Color::from_rgba8(50, 180, 110, 255)), ds);
    }, {{60, 55}, {23, 23}, {102, 23}, {60, 96}, {175, 60}, {182, 98}, {60, 155}}, 12, 4.5f);

    std::cout << "  -> P0.4-B ColorEffect, ImageFilter Blur/DropShadow & RRect/Circle Shadows Parity OK" << std::endl;
}

static void test_gpu_text_decorations_and_shadows_parity() {
    std::cout << "[Test 24/24] P1.6-PartB & P1.7 Text decorations, shadows, Align::Start & font aliases parity..." << std::endl;

    text::FontSystem font_system;
    auto fid_noto = font_system.load_font_file(resolve_test_font("NotoSans-Regular.ttf"), 0, "UiParitySans");
    auto fid_arabic = font_system.load_font_file(resolve_test_font("NotoSansArabic.ttf"));
    assert(fid_noto.has_value() && fid_arabic.has_value());
    font_system.register_family_alias("ParityBody", "UiParitySans");
    font_system.set_default_font(*fid_noto);

    // Part A: Rich decorations (overline, Double/Dotted/Dashed/Wavy, custom colors/thickness) & sharp/blurred shadows
    text::Buffer buf(text::Metrics(22.0f, 34.0f));
    text::Attrs base;
    base.set_family(text::Family("ParityBody"))
        .set_color(text::TextColor::rgb(255, 255, 255));
    buf.set_text("OOO DDD TTT AAA WWW SHW", base);
    {
        auto& alist = buf.lines_mut()[0].attrs_list_mut();
        text::Attrs a_over = base;
        a_over.set_overline(true)
              .set_decoration_color(text::TextColor::rgb(255, 50, 50))
              .set_decoration_thickness(1.5f);
        text::Attrs a_dbl = base;
        a_dbl.set_underline(true)
             .set_decoration_style(text::DecorationStyle::Double)
             .set_decoration_color(text::TextColor::rgb(50, 255, 50));
        text::Attrs a_dot = base;
        a_dot.set_underline(true)
             .set_decoration_style(text::DecorationStyle::Dotted)
             .set_decoration_color(text::TextColor::rgb(50, 240, 255));
        text::Attrs a_dash = base;
        a_dash.set_underline(true)
              .set_decoration_style(text::DecorationStyle::Dashed)
              .set_decoration_color(text::TextColor::rgb(255, 230, 40));
        text::Attrs a_wavy = base;
        a_wavy.set_underline(true)
              .set_decoration_style(text::DecorationStyle::Wavy)
              .set_decoration_color(text::TextColor::rgb(255, 50, 255));
        text::Attrs a_shw = base;
        a_shw.add_shadow(text::TextColor::rgb(0, 220, 0), 6.0f, 5.0f, 0.0f)
             .add_shadow(text::TextColor::rgb(0, 130, 255), -5.0f, 8.0f, 2.5f);

        alist.add_span(0, 3, a_over);
        alist.add_span(4, 7, a_dbl);
        alist.add_span(8, 11, a_dot);
        alist.add_span(12, 15, a_dash);
        alist.add_span(16, 19, a_wavy);
        alist.add_span(20, 23, a_shw);
    }

    // Part B: RTL Align::Start buffer
    text::Buffer rtl_buf(text::Metrics(20.0f, 28.0f));
    rtl_buf.set_size(240.0f, std::nullopt);
    rtl_buf.set_align(text::Align::Start);
    rtl_buf.set_text("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");

    constexpr uint32_t W = 500;
    constexpr uint32_t H = 140;

    auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
    assert(cpu_pm.has_value());
    cpu_pm->fill(Color::BLACK);
    {
        Canvas cpu_canvas(cpu_pm->as_mut());
        text::GlyphCache cpu_cache(256);
        buf.draw(cpu_canvas, cpu_cache, font_system, Color::WHITE, 16.0f, 20.0f);
        rtl_buf.draw(cpu_canvas, cpu_cache, font_system, Color::from_rgba8(255, 180, 50, 255), 16.0f, 86.0f);
    }

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, W, H);
    assert(surface != nullptr);
    GpuCanvas gpu_canvas(surface);
    gpu_canvas.clear(Color::BLACK);
    gpu_canvas.draw_text_buffer(buf, font_system, Point(16.0f, 20.0f), Color::WHITE);
    gpu_canvas.draw_text_buffer(rtl_buf, font_system, Point(16.0f, 86.0f), Color::from_rgba8(255, 180, 50, 255));

    // Also verify createFontAtIndex & createFontMemAtIndex on gpu::Context
    {
        auto* ctx = gpu_canvas.context();
        assert(ctx != nullptr);
        std::string mono_path = resolve_test_font("FiraMono-Medium.ttf");
        int idx_font = ctx->createFontAtIndex("mono_idx_alias", mono_path.c_str(), 0);
        assert(idx_font > 0);
        assert(ctx->findFont("mono_idx_alias") == idx_font);

        std::ifstream f(mono_path, std::ios::binary | std::ios::ate);
        assert(f.is_open());
        std::vector<unsigned char> raw(static_cast<size_t>(f.tellg()));
        f.seekg(0, std::ios::beg);
        f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        int mem_font = ctx->createFontMemAtIndex("mono_mem_alias", raw.data(), int(raw.size()), 0, 0);
        assert(mem_font > 0);
        assert(ctx->findFont("mono_mem_alias") == mem_font);
    }

    gpu_canvas.flush();
    auto gpu_pm = surface->to_pixmap();
    assert(gpu_pm.has_value());

    const auto cpu_r = cpu_pm->as_ref();
    const auto gpu_r = gpu_pm->as_ref();

    struct ColorBounds {
        uint32_t min_x = W, max_x = 0, min_y = H, max_y = 0, count = 0;
        std::array<uint32_t, H> row_counts{};
    };
    auto scan_bounds = [&](const PixmapRef& pm, uint32_t y0, uint32_t y1, auto&& pred) {
        ColorBounds b;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto p = pm.pixel(x, y);
                if (p && pred(*p)) {
                    b.min_x = std::min(b.min_x, x);
                    b.max_x = std::max(b.max_x, x);
                    b.min_y = std::min(b.min_y, y);
                    b.max_y = std::max(b.max_y, y);
                    ++b.count;
                    ++b.row_counts[y];
                }
            }
        }
        return b;
    };

    // 1. Overline (red bar strictly at top of row 1)
    auto is_red = [](const PremultipliedColorU8& p) { return p.r > 180 && p.g < 70 && p.b < 70; };
    auto cpu_red = scan_bounds(cpu_r, 0, 75, is_red);
    auto gpu_red = scan_bounds(gpu_r, 0, 75, is_red);
    assert(cpu_red.count > 20 && gpu_red.count > 20);
    assert(std::abs(int(cpu_red.min_y) - int(gpu_red.min_y)) <= 1);
    assert(cpu_red.max_y < 40 && gpu_red.max_y < 40);

    // 2. Double underline ("DDD" green lines, 2 rows separated or multi-row around baseline)
    auto is_green = [](const PremultipliedColorU8& p) { return p.g > 180 && p.r < 70 && p.b < 70; };
    auto cpu_dbl = scan_bounds(cpu_r, 38, 70, is_green);
    auto gpu_dbl = scan_bounds(gpu_r, 38, 70, is_green);
    assert(cpu_dbl.count > 40 && gpu_dbl.count > 40);
    assert(std::abs(int(cpu_dbl.min_y) - int(gpu_dbl.min_y)) <= 2);
    assert(std::abs(int(cpu_dbl.max_y) - int(gpu_dbl.max_y)) <= 2);
    assert(gpu_dbl.max_y - gpu_dbl.min_y >= 2);

    // 3. Dotted (cyan) and Dashed (yellow) periodic horizontal gaps
    auto is_cyan = [](const PremultipliedColorU8& p) { return p.g > 85 && p.b > 85 && p.r < 50; };
    auto is_yellow = [](const PremultipliedColorU8& p) { return p.r > 85 && p.g > 75 && p.b < 50; };
    auto cpu_dot = scan_bounds(cpu_r, 38, 70, is_cyan);
    auto gpu_dot = scan_bounds(gpu_r, 38, 70, is_cyan);
    auto cpu_dash = scan_bounds(cpu_r, 38, 70, is_yellow);
    auto gpu_dash = scan_bounds(gpu_r, 38, 70, is_yellow);
    assert(cpu_dot.count >= 6 && gpu_dot.count >= 6);
    assert(cpu_dash.count >= 12 && gpu_dash.count >= 12);
    assert(cpu_dot.max_x > cpu_dot.min_x + 15 && gpu_dot.max_x > gpu_dot.min_x + 15);
    assert(std::abs(int(cpu_dot.min_y) - int(gpu_dot.min_y)) <= 2);
    assert(std::abs(int(cpu_dash.min_y) - int(gpu_dash.min_y)) <= 2);

    // 4. Wavy (magenta) spans multiple scanlines below baseline
    auto is_mag = [](const PremultipliedColorU8& p) { return p.r > 180 && p.b > 180 && p.g < 70; };
    auto cpu_wav = scan_bounds(cpu_r, 38, 70, is_mag);
    auto gpu_wav = scan_bounds(gpu_r, 38, 70, is_mag);
    assert(cpu_wav.count > 25 && gpu_wav.count > 25);
    assert(cpu_wav.max_y - cpu_wav.min_y >= 2 && gpu_wav.max_y - gpu_wav.min_y >= 2);
    assert(std::abs(int(cpu_wav.min_y) - int(gpu_wav.min_y)) <= 2);

    // 5. TextShadow ("SHW": sharp green shadow x > 300, blurred blue glow halo x > 280)
    auto is_shw_green = [](const PremultipliedColorU8& p) { return p.g > 140 && p.r < 60 && p.b < 60; };
    auto is_shw_blue = [](const PremultipliedColorU8& p) { return p.b > 15 && p.r < 30 && p.g < 140; };
    ColorBounds cpu_sg, gpu_sg, cpu_sb, gpu_sb;
    for (uint32_t y = 20; y < 80; ++y) {
        for (uint32_t x = 250; x < W; ++x) {
            auto cp = cpu_r.pixel(x, y);
            auto gp = gpu_r.pixel(x, y);
            if (cp && is_shw_green(*cp)) ++cpu_sg.count, cpu_sg.min_y = std::min(cpu_sg.min_y, y), cpu_sg.max_y = std::max(cpu_sg.max_y, y);
            if (gp && is_shw_green(*gp)) ++gpu_sg.count, gpu_sg.min_y = std::min(gpu_sg.min_y, y), gpu_sg.max_y = std::max(gpu_sg.max_y, y);
            if (cp && is_shw_blue(*cp)) ++cpu_sb.count, cpu_sb.min_y = std::min(cpu_sb.min_y, y), cpu_sb.max_y = std::max(cpu_sb.max_y, y);
            if (gp && is_shw_blue(*gp)) ++gpu_sb.count, gpu_sb.min_y = std::min(gpu_sb.min_y, y), gpu_sb.max_y = std::max(gpu_sb.max_y, y);
        }
    }
    assert(cpu_sg.count > 25 && gpu_sg.count > 25);
    assert(cpu_sb.count > 40 && gpu_sb.count > 40);
    assert(std::abs(int(cpu_sg.min_y) - int(gpu_sg.min_y)) <= 2);
    assert(std::abs(int(cpu_sb.min_y) - int(gpu_sb.min_y)) <= 4);

    // 6. RTL Align::Start amber row (y >= 80) right-aligned against width 240
    auto is_amber = [](const PremultipliedColorU8& p) { return p.r > 120 && p.g > 60 && p.b < 60; };
    auto cpu_rtl = scan_bounds(cpu_r, 80, H, is_amber);
    auto gpu_rtl = scan_bounds(gpu_r, 80, H, is_amber);
    assert(cpu_rtl.count > 40 && gpu_rtl.count > 40);
    assert(cpu_rtl.min_x > 120 && gpu_rtl.min_x > 120);
    assert(std::abs(int(cpu_rtl.max_x) - int(gpu_rtl.max_x)) <= 2);
    assert(std::abs(int(cpu_rtl.min_y) - int(gpu_rtl.min_y)) <= 2);

    std::cout << "  -> P1.6-PartB & P1.7 Text decorations, shadows, Align::Start & aliases parity OK" << std::endl;
}

static void test_gpu_shared_state_reset_and_blur_hardening() {
    std::cout << "[Test 25/25] P2.10 Shared GL context resetState() & DualKawaseBlur state preservation..." << std::endl;

    constexpr uint32_t W = 128;
    constexpr uint32_t H = 128;
    auto device = GpuDevice::create();
    assert(device != nullptr);
    auto surface = GpuSurface::create(device, W, H, 4);
    assert(surface != nullptr);

    GpuCanvas canvas(surface);
    canvas.clear(Color::TRANSPARENT);
    canvas.fill_rect(*Rect::from_xywh(16.0f, 16.0f, 40.0f, 40.0f), Paint(Color::from_rgba8(240, 40, 40, 200)));
    canvas.flush();

    // 1. Verify DualKawaseBlur preserves FBO, viewport, scissor, stencil, blend & alpha over MSAA FBO
    {
        GLint beforeDrawFbo = 0;
        GLint beforeVp[4]{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &beforeDrawFbo);
        glGetIntegerv(GL_VIEWPORT, beforeVp);
        glEnable(GL_SCISSOR_TEST);
        glScissor(10, 10, 50, 50);
        glEnable(GL_BLEND);

        nisaba::gpu::DualKawaseBlur blur;
        nisaba::gpu::DualKawaseConfig cfg;
        cfg.passes = 2;
        cfg.radius = 2.0f;
        // Pass MSAA fbo_id directly (tests MSAA resolve + RGBA8 alpha preservation!)
        uint32_t out_tex = blur.blur_framebuffer(surface->fbo_id(), static_cast<int>(W), static_cast<int>(H), cfg);
        assert(out_tex != 0);

        GLint afterDrawFbo = 0;
        GLint afterVp[4]{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &afterDrawFbo);
        glGetIntegerv(GL_VIEWPORT, afterVp);
        assert(afterDrawFbo == beforeDrawFbo);
        assert(afterVp[0] == beforeVp[0] && afterVp[1] == beforeVp[1] && afterVp[2] == beforeVp[2] && afterVp[3] == beforeVp[3]);
        assert(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE);
        assert(glIsEnabled(GL_BLEND) == GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
    }

    // 2. Deliberately corrupt GL state as an external embedder host might do
    glActiveTexture(GL_TEXTURE1);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 2, 2);
    glEnable(GL_STENCIL_TEST);
    glEnable(GL_CULL_FACE);
    glBlendEquation(GL_FUNC_SUBTRACT);
    glBlendFunc(GL_ZERO, GL_ZERO);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 13);

    // Notify Nisaba that host mutated shared GL state, then draw a crisp green rect + strided pixmap
    canvas.reset_state();
    assert(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE); // resetState resets caches & unpack/blendEq without failing draws

    canvas.fill_rect(*Rect::from_xywh(60.0f, 20.0f, 48.0f, 48.0f), Paint(Color::from_rgba8(20, 220, 40, 255)));

    auto stamp = Pixmap::allocate(16, 16, PixelFormat::RGBA8888);
    assert(stamp.has_value());
    stamp->fill(Color::from_rgba8(40, 80, 240, 255));
    canvas.draw_pixmap(60, 76, stamp->as_ref());
    canvas.flush();

    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    auto p_green = pm->as_ref().pixel(80, 40);
    auto p_blue = pm->as_ref().pixel(68, 84);
    assert(p_green.has_value() && p_green->g > 200 && p_green->r < 40 && p_green->b < 60 && p_green->a > 240);
    assert(p_blue.has_value() && p_blue->b > 220 && p_blue->g >= 70 && p_blue->g <= 95 && p_blue->r <= 50 && p_blue->a > 240);

    std::cout << "  -> P2.10 Shared GL context resetState() & DualKawaseBlur state preservation OK" << std::endl;
}

static void test_gpu_p3_svg_lottie_3d_parity() {
    std::cout << "[TEST] P3.11 draw_pixmap_3d, SvgDocument::render_gpu & 3D Lottie CPU-vs-GPU Parity..." << std::endl;

    auto dev = GpuDevice::create_headless();
    assert(dev && dev->is_valid());

    constexpr uint32_t W = 240;
    constexpr uint32_t H = 200;
    const Color bg = Color::from_rgba8(24, 28, 36, 255);

    auto run_parity_case = [&](const char* label, auto&& draw_fn,
                               const std::vector<std::pair<uint32_t, uint32_t>>& pts,
                               int pt_tol = 10, float max_avg_diff = 5.5f) {
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(bg);
        {
            Canvas cpu(cpu_pm->as_mut());
            draw_fn(cpu, false);
        }

        auto surface = GpuSurface::create(dev, W, H, 1);
        assert(surface != nullptr);
        GpuCanvas gpu(surface);
        gpu.clear(bg);
        draw_fn(gpu, true);
        gpu.flush();
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();
        for (auto [px, py] : pts) {
            auto cp = cpu_r.pixel(px, py);
            auto gp = gpu_r.pixel(px, py);
            assert(cp.has_value() && gp.has_value());
            int dr = std::abs(int(cp->red()) - int(gp->red()));
            int dg = std::abs(int(cp->green()) - int(gp->green()));
            int db = std::abs(int(cp->blue()) - int(gp->blue()));
            int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
            if (dr > pt_tol || dg > pt_tol || db > pt_tol || da > pt_tol) {
                std::cerr << "  [MISMATCH] " << label << " @(" << px << "," << py << ") "
                          << "CPU=(" << int(cp->red()) << "," << int(cp->green()) << "," << int(cp->blue()) << "," << int(cp->alpha()) << ") "
                          << "GPU=(" << int(gp->red()) << "," << int(gp->green()) << "," << int(gp->blue()) << "," << int(gp->alpha()) << ")\n";
                assert(false);
            }
        }

        double sum_diff = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                int dr = std::abs(int(cp->red()) - int(gp->red()));
                int dg = std::abs(int(cp->green()) - int(gp->green()));
                int db = std::abs(int(cp->blue()) - int(gp->blue()));
                int da = std::abs(int(cp->alpha()) - int(gp->alpha()));
                sum_diff += (dr + dg + db + da) * 0.25;
            }
        }
        double avg_diff = sum_diff / double(W * H);
        if (avg_diff > max_avg_diff) {
            std::cerr << "  [AVG MISMATCH] " << label << " avg_diff=" << avg_diff << " > " << max_avg_diff << "\n";
            assert(false);
        }
    };

    // 1. draw_pixmap_3d full + src_rect + opacity + BlendMode::Screen
    {
        auto quad_pm = Pixmap::allocate(64, 64, PixelFormat::RGBA8888);
        assert(quad_pm.has_value());
        Canvas qcanvas(quad_pm->as_mut());
        qcanvas.fill_rect(*Rect::from_xywh(0, 0, 32, 32), Paint(Color::from_rgba8(230, 50, 40, 255)));
        qcanvas.fill_rect(*Rect::from_xywh(32, 0, 32, 32), Paint(Color::from_rgba8(40, 220, 60, 255)));
        qcanvas.fill_rect(*Rect::from_xywh(0, 32, 32, 32), Paint(Color::from_rgba8(40, 90, 240, 255)));
        qcanvas.fill_rect(*Rect::from_xywh(32, 32, 32, 32), Paint(Color::from_rgba8(240, 220, 40, 255)));

        Transform4x4 tr1 = Transform4x4::from_translate(35.0f, 35.0f) *
                           Transform4x4::from_camera_orbit(32.0f, 32.0f, 24.0f, -26.0f, 10.0f, 320.0f);
        Transform4x4 tr2 = Transform4x4::from_translate(145.0f, 80.0f) *
                           Transform4x4::from_camera_orbit(16.0f, 16.0f, -20.0f, 22.0f, -8.0f, 280.0f);

        run_parity_case("draw_pixmap_3d", [&](ICanvas& c, bool is_gpu) {
            c.draw_pixmap_3d(quad_pm->as_ref(), tr1, 1.0f, BlendMode::SourceOver);
            c.fill_rect(*Rect::from_xywh(135.0f, 70.0f, 70.0f, 70.0f), Paint(Color::from_rgba8(30, 60, 120, 255)));
            if (is_gpu) {
                static_cast<GpuCanvas&>(c).draw_pixmap_3d(
                    quad_pm->as_ref(), *Rect::from_xywh(16.0f, 16.0f, 32.0f, 32.0f),
                    tr2, 0.85f, BlendMode::Screen);
            } else {
                static_cast<Canvas&>(c).draw_pixmap_3d(
                    quad_pm->as_ref(), *Rect::from_xywh(16.0f, 16.0f, 32.0f, 32.0f),
                    tr2, 0.85f, BlendMode::Screen);
            }
        }, {{67, 67}, {161, 96}, {10, 10}}, 14, 4.0f);
    }

    // 2. SvgDocument::render vs render_gpu: gradients, pattern, objectBoundingBox clipPath,
    //    isolated <g opacity>, stroke-dasharray, mix-blend-mode, nested <text>
    {
        const std::string svg_xml = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="200" viewBox="0 0 240 200">
  <defs>
    <linearGradient id="lg" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" stop-color="#f03040"/>
      <stop offset="100%" stop-color="#3080f0"/>
    </linearGradient>
    <radialGradient id="rg" cx="50%" cy="50%" r="50%">
      <stop offset="0%" stop-color="#fff040"/>
      <stop offset="100%" stop-color="#20b050"/>
    </radialGradient>
    <pattern id="pat" width="16" height="16" patternUnits="userSpaceOnUse">
      <rect x="0" y="0" width="8" height="8" fill="#40d0e0"/>
      <rect x="8" y="8" width="8" height="8" fill="#e060c0"/>
    </pattern>
    <clipPath id="cp" clipPathUnits="objectBoundingBox">
      <circle cx="0.5" cy="0.5" r="0.45"/>
    </clipPath>
  </defs>
  <rect x="12" y="12" width="90" height="70" rx="10" fill="url(#lg)"/>
  <g opacity="0.55">
    <rect x="115" y="15" width="55" height="55" fill="#ff3030"/>
    <rect x="140" y="30" width="55" height="55" fill="#30ff60"/>
  </g>
  <rect x="15" y="100" width="80" height="80" fill="url(#pat)" clip-path="url(#cp)"/>
  <circle cx="145" cy="135" r="35" fill="url(#rg)" stroke="#ffffff" stroke-width="4" stroke-dasharray="10 6"/>
  <rect x="165" y="110" width="55" height="55" fill="#6090ff" style="mix-blend-mode:multiply"/>
</svg>)svg";
        auto doc = svg::SvgDocument::parse(svg_xml);
        assert(doc.has_value());
        Rect bounds = Rect::from_xywh(0.0f, 0.0f, float(W), float(H)).value();

        run_parity_case("SvgDocument::render vs render_gpu", [&](ICanvas& c, bool is_gpu) {
            if (is_gpu) {
                doc->render_gpu(static_cast<GpuCanvas&>(c), &bounds);
            } else {
                doc->render(static_cast<Canvas&>(c), &bounds);
            }
        }, {{30, 45}, {152, 48}, {55, 140}, {145, 135}, {180, 135}}, 14, 5.5f);
    }

    // 3. 3D Lottie animation layer with blend mode ("ddd": 1, "rx": 28, "bm": 2 Screen)
    {
        const std::string lottie_3d_json = R"({
            "v": "5.7.4",
            "fr": 60,
            "ip": 0,
            "op": 60,
            "w": 240,
            "h": 200,
            "layers": [
                {
                    "ddd": 0,
                    "ind": 1,
                    "ty": 4,
                    "nm": "Backdrop",
                    "ks": {
                        "p": {"a": 0, "k": [120, 100]},
                        "a": {"a": 0, "k": [0, 0]},
                        "s": {"a": 0, "k": [100, 100]}
                    },
                    "shapes": [
                        {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [150, 120]}, "r": {"a": 0, "k": 12}},
                        {"ty": "fl", "c": {"a": 0, "k": [0.15, 0.25, 0.65, 1.0]}, "o": {"a": 0, "k": 100}}
                    ]
                },
                {
                    "ddd": 1,
                    "ind": 2,
                    "ty": 4,
                    "nm": "Plane3D",
                    "bm": 2,
                    "ks": {
                        "p": {"a": 0, "k": [120, 100, 0]},
                        "a": {"a": 0, "k": [0, 0, 0]},
                        "s": {"a": 0, "k": [100, 100, 100]},
                        "rx": {"a": 0, "k": 28},
                        "ry": {"a": 0, "k": -22},
                        "rz": {"a": 0, "k": 8},
                        "o": {"a": 0, "k": 85}
                    },
                    "shapes": [
                        {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 80]}, "r": {"a": 0, "k": 8}},
                        {"ty": "fl", "c": {"a": 0, "k": [0.95, 0.35, 0.20, 1.0]}, "o": {"a": 0, "k": 100}}
                    ]
                }
            ]
        })";
        auto anim = lottie::Animation::load_from_data(lottie_3d_json);
        assert(anim != nullptr);

        run_parity_case("Lottie 3D layer + BlendMode", [&](ICanvas& c, bool) {
            anim->render(c, 10.0f);
        }, {{120, 100}, {60, 60}, {10, 10}}, 14, 5.0f);
    }

    std::cout << "  -> P3.11 draw_pixmap_3d, SvgDocument::render_gpu & 3D Lottie Parity OK" << std::endl;
}

static void test_gpu_phase1_ellipsis_and_direction_parity() {
    std::cout << "[Test 27/27] Phase 1 Custom Ellipsis & Explicit TextDirection CPU-vs-GPU Parity..." << std::endl;

    text::FontSystem font_system;
    font_system.set_auto_system_fonts(false);
    auto fid_noto = font_system.load_font_file(resolve_test_font("NotoSans-Regular.ttf"));
    auto fid_arabic = font_system.load_font_file(resolve_test_font("NotoSansArabic.ttf"));
    assert(fid_noto.has_value() && fid_arabic.has_value());
    font_system.set_default_font(*fid_noto);

    auto device = GpuDevice::create();
    constexpr uint32_t W = 380;
    constexpr uint32_t H = 72;

    auto verify_cpu_gpu_text_parity = [&](const char* label, text::Buffer& buf, float ox, float oy,
                                          bool expect_right_half, bool expect_left_half) {
        buf.shape_until_scroll(font_system);
        auto lm = buf.line_metrics();
        assert(!lm.empty());

        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(Color::BLACK);
        {
            Canvas cpu_canvas(cpu_pm->as_mut());
            text::GlyphCache cpu_cache(256);
            buf.draw(cpu_canvas, cpu_cache, font_system, Color::WHITE, ox, oy);
        }

        auto surface = GpuSurface::create(device, W, H);
        assert(surface != nullptr);
        GpuCanvas gpu_canvas(surface);
        gpu_canvas.clear(Color::BLACK);
        gpu_canvas.draw_text_buffer(buf, font_system, Point(ox, oy), Color::WHITE);
        gpu_canvas.flush();
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();

        uint32_t cpu_x0 = W, cpu_x1 = 0, cpu_y0 = H, cpu_y1 = 0;
        uint32_t gpu_x0 = W, gpu_x1 = 0, gpu_y0 = H, gpu_y1 = 0;
        size_t cpu_lit = 0, gpu_lit = 0;
        double sum_abs_diff = 0.0;

        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                assert(cp && gp);
                if (cp->r > 40 || cp->g > 40 || cp->b > 40) {
                    ++cpu_lit;
                    cpu_x0 = std::min(cpu_x0, x);
                    cpu_x1 = std::max(cpu_x1, x);
                    cpu_y0 = std::min(cpu_y0, y);
                    cpu_y1 = std::max(cpu_y1, y);
                }
                if (gp->r > 40 || gp->g > 40 || gp->b > 40) {
                    ++gpu_lit;
                    gpu_x0 = std::min(gpu_x0, x);
                    gpu_x1 = std::max(gpu_x1, x);
                    gpu_y0 = std::min(gpu_y0, y);
                    gpu_y1 = std::max(gpu_y1, y);
                }
                sum_abs_diff += std::abs(int(cp->r) - int(gp->r))
                              + std::abs(int(cp->g) - int(gp->g))
                              + std::abs(int(cp->b) - int(gp->b));
            }
        }

        const double mae = sum_abs_diff / (double(W) * double(H) * 3.0);
        if (cpu_lit < 40 || gpu_lit < 40 ||
            std::abs(int(cpu_x0) - int(gpu_x0)) > 2 ||
            std::abs(int(cpu_x1) - int(gpu_x1)) > 2 ||
            std::abs(int(cpu_y0) - int(gpu_y0)) > 2 ||
            std::abs(int(cpu_y1) - int(gpu_y1)) > 2 ||
            mae > 3.5) {
            std::cerr << "  [FAIL] " << label
                      << " cpu_lit=" << cpu_lit << " gpu_lit=" << gpu_lit
                      << " cpu_box=(" << cpu_x0 << "," << cpu_y0 << ")-(" << cpu_x1 << "," << cpu_y1 << ")"
                      << " gpu_box=(" << gpu_x0 << "," << gpu_y0 << ")-(" << gpu_x1 << "," << gpu_y1 << ")"
                      << " mae=" << mae << std::endl;
            assert(false);
        }
        if (expect_right_half) {
            assert(cpu_x0 > W / 3 && gpu_x0 > W / 3);
        }
        if (expect_left_half) {
            assert(cpu_x0 < W / 4 && gpu_x0 < W / 4);
        }
    };

    // 1. Custom Latin ellipsis ("... more")
    {
        text::Buffer b(text::Metrics(20.0f, 28.0f));
        b.set_size(220.0f, std::nullopt);
        b.set_wrap(text::Wrap::Word);
        b.set_overflow(text::Overflow::Ellipsis);
        b.set_max_lines(1);
        b.set_ellipsis("... more");
        b.set_text("Nisaba sovereign text engine with custom truncation label");
        verify_cpu_gpu_text_parity("Custom Latin Ellipsis", b, 16.0f, 16.0f, false, true);
        assert(b.did_exceed_max_lines());
        assert(b.unresolved_glyphs() == 0);
    }

    // 2. Custom Arabic RTL ellipsis ("… المزيد")
    {
        text::Buffer b(text::Metrics(20.0f, 32.0f));
        b.set_size(240.0f, std::nullopt);
        b.set_wrap(text::Wrap::Word);
        b.set_align(text::Align::Start);
        b.set_overflow(text::Overflow::Ellipsis);
        b.set_max_lines(1);
        b.set_ellipsis("\xE2\x80\xA6 \xD8\xA7\xD9\x84\xD9\x85\xD8\xB2\xD9\x8A\xD8\xAF");
        b.set_text("مرحبا بكم في محرك نيسابا المتكامل للنصوص العربية والطباعة المتقدمة");
        verify_cpu_gpu_text_parity("Custom Arabic RTL Ellipsis", b, 20.0f, 16.0f, false, false);
        assert(b.did_exceed_max_lines());
        assert(b.unresolved_glyphs() == 0);
    }

    // 3. Explicit TextDirection::RTL on mixed BiDi with Align::Start (aligns right)
    {
        text::Buffer b(text::Metrics(20.0f, 30.0f));
        b.set_size(340.0f, std::nullopt);
        b.set_align(text::Align::Start);
        b.set_text_direction(text::TextDirection::RTL);
        b.set_text("مرحبا Hello");
        verify_cpu_gpu_text_parity("Explicit RTL BiDi", b, 20.0f, 16.0f, true, false);
        assert(!b.did_exceed_max_lines());
    }

    // 4. Explicit TextDirection::LTR on mixed BiDi with Align::Start (aligns left)
    {
        text::Buffer b(text::Metrics(20.0f, 30.0f));
        b.set_size(340.0f, std::nullopt);
        b.set_align(text::Align::Start);
        b.set_text_direction(text::TextDirection::LTR);
        b.set_text("مرحبا Hello");
        verify_cpu_gpu_text_parity("Explicit LTR BiDi", b, 20.0f, 16.0f, false, true);
        assert(!b.did_exceed_max_lines());
    }

    std::cout << "  -> Phase 1 Custom Ellipsis & TextDirection CPU-vs-GPU Parity OK" << std::endl;
}

static void test_gpu_phase2_placeholders_rich_paint_and_gaps_parity() {
    std::cout << "[Test 28/28] Phase 2 Placeholders, Rich Paint Spans & DecorationMode::Gaps CPU-vs-GPU Parity..." << std::endl;

    text::FontSystem font_system;
    font_system.set_auto_system_fonts(false);
    auto fid_noto = font_system.load_font_file(resolve_test_font("NotoSans-Regular.ttf"));
    auto fid_arabic = font_system.load_font_file(resolve_test_font("NotoSansArabic.ttf"));
    assert(fid_noto.has_value() && fid_arabic.has_value());
    font_system.set_default_font(*fid_noto);

    auto device = GpuDevice::create();
    constexpr uint32_t W = 400;
    constexpr uint32_t H = 96;

    auto verify_parity = [&](const char* label, auto&& draw_fn, double max_mae) {
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(Color::BLACK);
        {
            Canvas cpu_canvas(cpu_pm->as_mut());
            text::GlyphCache cpu_cache(256);
            draw_fn(&cpu_canvas, &cpu_cache, nullptr);
        }

        auto surface = GpuSurface::create(device, W, H);
        assert(surface != nullptr);
        GpuCanvas gpu_canvas(surface);
        gpu_canvas.clear(Color::BLACK);
        draw_fn(nullptr, nullptr, &gpu_canvas);
        gpu_canvas.flush();
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();

        size_t cpu_lit = 0, gpu_lit = 0;
        double sum_abs = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                assert(cp && gp);
                if (cp->r > 30 || cp->g > 30 || cp->b > 30) ++cpu_lit;
                if (gp->r > 30 || gp->g > 30 || gp->b > 30) ++gpu_lit;
                sum_abs += std::abs(int(cp->r) - int(gp->r))
                         + std::abs(int(cp->g) - int(gp->g))
                         + std::abs(int(cp->b) - int(gp->b));
            }
        }
        const double mae = sum_abs / (double(W) * double(H) * 3.0);
        if (cpu_lit < 80 || gpu_lit < 80 || mae > max_mae) {
            std::cerr << "  [FAIL] " << label
                      << " cpu_lit=" << cpu_lit << " gpu_lit=" << gpu_lit
                      << " mae=" << mae << " (max=" << max_mae << ")" << std::endl;
            assert(false);
        }
    };

    // 1. Gradient foreground + Stroked foreground + Gradient background + BlendMode::Plus
    {
        text::Buffer buf(text::Metrics(30.0f, 44.0f));
        auto fg_grad = LinearGradient::create(
            Point::from_xy(16.0f, 0.0f), Point::from_xy(190.0f, 0.0f),
            {GradientStop::create(0.0f, Color::from_rgba8(255, 40, 40, 255)),
             GradientStop::create(1.0f, Color::from_rgba8(40, 120, 255, 255))}
        );
        auto bg_grad = LinearGradient::create(
            Point::from_xy(190.0f, 0.0f), Point::from_xy(360.0f, 0.0f),
            {GradientStop::create(0.0f, Color::from_rgba8(40, 60, 140, 255)),
             GradientStop::create(1.0f, Color::from_rgba8(30, 140, 120, 255))}
        );
        assert(fg_grad.has_value() && bg_grad.has_value());

        Paint p_grad{Shader{*fg_grad}};
        Paint p_stroke(Color::from_rgba8(255, 230, 40, 255));
        p_stroke.set_style(PaintStyle::Stroke);
        p_stroke.set_stroke_width(1.8f);
        Paint p_bg{Shader{*bg_grad}};

        text::Attrs a1;
        a1.set_foreground_paint(p_grad);
        text::Attrs a2;
        a2.set_foreground_paint(p_stroke).set_background_paint(p_bg);

        buf.add_text("GRAD ", a1);
        buf.add_text("STROKE", a2);
        buf.shape_until_scroll(font_system);

        verify_parity("Gradient & Stroked Span Paint", [&](Canvas* cpu, text::GlyphCache* cache, GpuCanvas* gpu) {
            if (cpu) buf.draw(*cpu, *cache, font_system, Color::WHITE, 16.0f, 20.0f);
            else gpu->draw_text_buffer(buf, font_system, Point(16.0f, 20.0f), Color::WHITE);
        }, 4.5);
    }

    // 2. Smart Underline Gap Skipping (DecorationMode::Gaps) for Latin & Arabic descenders
    {
        text::Buffer buf(text::Metrics(26.0f, 38.0f));
        text::Attrs a;
        a.set_color(text::TextColor::rgb(240, 240, 240))
         .set_underline(true)
         .set_decoration_mode(text::DecorationMode::Gaps)
         .set_decoration_color(text::TextColor::rgb(255, 50, 50))
         .set_decoration_thickness(1.3f);
        buf.set_text("agypq يرجى التوقيع", a);
        buf.shape_until_scroll(font_system);

        verify_parity("DecorationMode::Gaps Latin+Arabic", [&](Canvas* cpu, text::GlyphCache* cache, GpuCanvas* gpu) {
            if (cpu) buf.draw(*cpu, *cache, font_system, Color::WHITE, 16.0f, 24.0f);
            else gpu->draw_text_buffer(buf, font_system, Point(16.0f, 24.0f), Color::WHITE);
        }, 4.0);
    }

    // 3. Inline Placeholders in mixed LTR + Arabic RTL rendered identically on CPU & GPU
    {
        text::Buffer buf(text::Metrics(22.0f, 34.0f));
        text::Attrs a;
        a.set_color(text::TextColor::rgb(245, 245, 255));
        buf.add_text("Tag ", a);
        buf.add_placeholder(24.0f, 24.0f, text::PlaceholderAlignment::Middle);
        buf.add_text(" مرحبا ", a);
        buf.add_placeholder(28.0f, 20.0f, text::PlaceholderAlignment::AboveBaseline);
        buf.add_text(" OK", a);
        buf.shape_until_scroll(font_system);
        auto prects = buf.placeholder_rects(16.0f, 24.0f);
        assert(prects.size() == 2);

        verify_parity("Inline Placeholders CPU vs GPU", [&](Canvas* cpu, text::GlyphCache* cache, GpuCanvas* gpu) {
            Paint badge(Color::from_rgba8(60, 180, 255, 255));
            if (cpu) {
                buf.draw(*cpu, *cache, font_system, Color::WHITE, 16.0f, 24.0f);
                for (const auto& pr : prects) cpu->fill_rect(pr.rect, badge);
            } else {
                gpu->draw_text_buffer(buf, font_system, Point(16.0f, 24.0f), Color::WHITE);
                for (const auto& pr : prects) gpu->fill_rect(pr.rect, badge);
            }
        }, 4.0);
    }

    std::cout << "  -> Phase 2 Placeholders, Rich Paint Spans & DecorationMode::Gaps Parity OK" << std::endl;
}

static void test_gpu_phase3_features_variable_fonts_and_strut_parity() {
    std::cout << "[Test 29/29] Phase 3 OpenType Features, Variable Fonts & StrutStyle CPU-vs-GPU Parity..." << std::endl;

    text::FontSystem font_system;
    font_system.set_auto_system_fonts(false);
    auto fid_var_en = font_system.load_font_file(resolve_test_font("InterVariable.ttf"), 0, "InterVarGpu");
    auto fid_var_ar = font_system.load_font_file(resolve_test_font("NotoSansArabic.ttf"), 0, "ArabicVarGpu");
    assert(fid_var_en.has_value() && fid_var_ar.has_value());
    font_system.set_default_font(*fid_var_en);

    auto device = GpuDevice::create();
    constexpr uint32_t W = 400;
    constexpr uint32_t H = 120;

    auto verify_parity = [&](const char* label, text::Buffer& buf, float ox, float oy, double max_mae) {
        buf.shape_until_scroll(font_system);
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(Color::BLACK);
        {
            Canvas cpu_canvas(cpu_pm->as_mut());
            text::GlyphCache cpu_cache(256);
            buf.draw(cpu_canvas, cpu_cache, font_system, Color::WHITE, ox, oy);
        }

        auto surface = GpuSurface::create(device, W, H);
        assert(surface != nullptr);
        GpuCanvas gpu_canvas(surface);
        gpu_canvas.clear(Color::BLACK);
        gpu_canvas.draw_text_buffer(buf, font_system, Point(ox, oy), Color::WHITE);
        gpu_canvas.flush();
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();
        size_t cpu_lit = 0, gpu_lit = 0;
        double sum_abs = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                assert(cp && gp);
                if (cp->r > 30 || cp->g > 30 || cp->b > 30) ++cpu_lit;
                if (gp->r > 30 || gp->g > 30 || gp->b > 30) ++gpu_lit;
                sum_abs += std::abs(int(cp->r) - int(gp->r))
                         + std::abs(int(cp->g) - int(gp->g))
                         + std::abs(int(cp->b) - int(gp->b));
            }
        }
        const double mae = sum_abs / (double(W) * double(H) * 3.0);
        if (cpu_lit < 80 || gpu_lit < 80 || mae > max_mae) {
            std::cerr << "  [FAIL] " << label
                      << " cpu_lit=" << cpu_lit << " gpu_lit=" << gpu_lit
                      << " mae=" << mae << " (max=" << max_mae << ")" << std::endl;
            assert(false);
        }
    };

    // 1. Variable font Latin Thin (wght=200) vs Black (wght=850) + 'tnum' + 'zero'
    {
        text::Buffer buf(text::Metrics(28.0f, 40.0f));
        text::Attrs a_thin;
        a_thin.set_family(text::Family("InterVarGpu"))
              .add_font_variation("wght", 200.0f)
              .add_font_feature("tnum", 1)
              .add_font_feature("zero", 1)
              .set_color(text::TextColor::rgb(200, 230, 255));
        text::Attrs a_heavy;
        a_heavy.set_family(text::Family("InterVarGpu"))
               .add_font_variation("wght", 850.0f)
               .add_font_feature("tnum", 1)
               .add_font_feature("zero", 1)
               .set_color(text::TextColor::rgb(255, 220, 120));
        buf.add_text("01:20 ", a_thin);
        buf.add_text("09:50", a_heavy);
        verify_parity("Variable Font wght + tnum + zero", buf, 16.0f, 24.0f, 4.0);
    }

    // 2. Variable font Arabic RTL (wght=800, wdth=75) + StrutStyle + HalfLeading + TextHeightBehavior
    {
        text::Buffer buf(text::Metrics(24.0f, 38.0f));
        text::StrutStyle strut;
        strut.enabled = true;
        strut.font_families = {text::Family("InterVarGpu")};
        strut.font_size = 24.0f;
        strut.height_multiplier = 1.4f;
        strut.force_strut_height = true;
        strut.half_leading = true;
        buf.set_strut_style(strut);
        buf.set_text_height_behavior(text::TextHeightBehavior{false, false});

        text::Attrs ar;
        ar.set_font_families({text::Family("InterVarGpu"), text::Family("ArabicVarGpu")})
          .set_locale("ar")
          .add_font_variation("wght", 800.0f)
          .add_font_variation("wdth", 75.0f)
          .set_color(text::TextColor::rgb(240, 248, 255));
        buf.set_text("مرحبا بالعالم 2026\nNisaba Typography", ar);
        verify_parity("Variable Arabic + StrutStyle + TextHeightBehavior", buf, 16.0f, 16.0f, 4.5);
    }

    std::cout << "  -> Phase 3 OpenType Features, Variable Fonts & StrutStyle Parity OK" << std::endl;
}

static void test_gpu_phase4_incremental_updates_cache_and_visitor_parity() {
    std::cout << "[Test 30/30] Phase 4 Incremental Updates, ParagraphCache & Visitor CPU-vs-GPU Parity..." << std::endl;

    text::FontSystem fs;
    fs.set_auto_system_fonts(false);
    auto id_en = fs.load_font_file(resolve_test_font("Inter-Regular.ttf"));
    auto id_ar = fs.load_font_file(resolve_test_font("NotoSansArabic.ttf"));
    assert(id_en.has_value() && id_ar.has_value());
    fs.set_default_font(*id_en);

    auto device = GpuDevice::create();
    constexpr uint32_t W = 440;
    constexpr uint32_t H = 130;

    auto verify_parity = [&](const char* label, text::Buffer& buf, float ox, float oy, double max_mae) {
        auto cpu_pm = Pixmap::allocate(W, H, PixelFormat::RGBA8888);
        assert(cpu_pm.has_value());
        cpu_pm->fill(Color::from_rgba8(14, 16, 20, 255));
        {
            Canvas cpu_canvas(cpu_pm->as_mut());
            text::GlyphCache gcache(256);
            buf.draw(cpu_canvas, gcache, fs, Color::WHITE, ox, oy);
        }

        auto surface = GpuSurface::create(device, W, H);
        assert(surface != nullptr);
        GpuCanvas gpu_canvas(surface);
        gpu_canvas.clear(Color::from_rgba8(14, 16, 20, 255));
        gpu_canvas.draw_text_buffer(buf, fs, Point(ox, oy), Color::WHITE);
        gpu_canvas.flush();
        auto gpu_pm = surface->to_pixmap();
        assert(gpu_pm.has_value());

        const auto cpu_r = cpu_pm->as_ref();
        const auto gpu_r = gpu_pm->as_ref();
        size_t cpu_lit = 0, gpu_lit = 0;
        double sum_abs = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                auto cp = cpu_r.pixel(x, y);
                auto gp = gpu_r.pixel(x, y);
                assert(cp && gp);
                if (cp->r > 35 || cp->g > 35 || cp->b > 35) ++cpu_lit;
                if (gp->r > 35 || gp->g > 35 || gp->b > 35) ++gpu_lit;
                sum_abs += std::abs(int(cp->r) - int(gp->r))
                         + std::abs(int(cp->g) - int(gp->g))
                         + std::abs(int(cp->b) - int(gp->b));
            }
        }
        const double mae = sum_abs / (double(W) * double(H) * 3.0);
        if (cpu_lit < 100 || gpu_lit < 100 || mae > max_mae) {
            std::cerr << "  [FAIL] " << label
                      << " cpu_lit=" << cpu_lit << " gpu_lit=" << gpu_lit
                      << " mae=" << mae << " (max=" << max_mae << ")" << std::endl;
            assert(false);
        }
    };

    // 1. In-place updates (align + gradient fg + bg paint + font size) with ZERO re-shape
    {
        text::Buffer buf(text::Metrics(22.0f, 32.0f));
        buf.set_size(400.0f, std::nullopt);
        buf.set_align(text::Align::Left);
        buf.set_text("Nisaba Live Editor\nتحديث فوري");
        buf.shape_until_scroll(fs);
        assert(buf.shape_count() == 1);

        auto grad = LinearGradient::create(
            Point::from_xy(60.0f, 0.0f),
            Point::from_xy(340.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 90, 90, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(80, 200, 255, 255))
            }
        );
        assert(grad.has_value());
        Paint fg_grad{Shader{*grad}};
        Paint bg_hl(Color::from_rgba8(60, 75, 130, 255));

        buf.update_text_align(text::Align::Center);
        buf.update_background_paint(0, 6, bg_hl);
        buf.update_foreground_paint(7, 11, fg_grad);
        buf.update_font_size(12, 18, 30.0f);
        assert(buf.shape_count() == 1);

        // Verify visit() sees updated font size and RTL Arabic run on line 1
        bool saw_30px = false;
        bool saw_ar_rtl = false;
        buf.visit(fs, [&](const text::GlyphRunInfo& info) {
            if (!info.is_placeholder && std::abs(info.font_size - 30.0f) < 0.01f) saw_30px = true;
            if (info.is_rtl && info.line_number == 1 && !info.glyph_ids.empty()) saw_ar_rtl = true;
        }, 20.0f, 16.0f);
        assert(saw_30px && saw_ar_rtl);

        verify_parity("Incremental Updates (Align+Paint+Size)", buf, 20.0f, 16.0f, 4.5);
        assert(buf.shape_count() == 1);
    }

    // 2. ParagraphCache hit rendering parity (mixed English + Arabic RTL)
    {
        text::ParagraphCache pcache(16);
        text::Attrs attrs;
        attrs.set_color(text::TextColor::rgb(235, 245, 255));
        auto b_miss = pcache.get_or_layout(fs, "Cached Label مرحبا 2026", attrs, text::Metrics(24.0f, 34.0f), 380.0f, text::Wrap::Word, text::Align::Center);
        auto b_hit = pcache.get_or_layout(fs, "Cached Label مرحبا 2026", attrs, text::Metrics(24.0f, 34.0f), 380.0f, text::Wrap::Word, text::Align::Center);
        assert(b_miss == b_hit && pcache.hits() == 1 && pcache.misses() == 1);
        verify_parity("ParagraphCache Hit (Latin+Arabic)", *b_hit, 20.0f, 32.0f, 4.0);
    }

    std::cout << "  -> Phase 4 Incremental Updates, ParagraphCache & Visitor Parity OK" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running Nisaba GPU Rendering Pipeline Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    test_gpu_types();
    test_gpu_tessellation_rect();
    test_gpu_tessellation_circle();
    test_gpu_tessellation_round_rect();
    test_gpu_tessellation_line();
    test_gpu_tessellation_path();
    test_gpu_tessellation_mesh();
    test_gpu_device_and_buffer();
    test_gpu_surface_and_canvas();
    test_gpu_cpu_coexistence();
    test_gpu_text_rendering();
    test_gpu_text_buffer_rendering();
    test_gpu_clip_path();
    test_gpu_backdrop_blur();
    test_gpu_stage0_stability();
    test_gpu_vertices_and_pixmap_rect();
    test_gpu_dashed_strokes();
    test_gpu_geometry_apis();
    test_gpu_text_buffer_parity();
    test_gpu_blend_modes_parity();
    test_gpu_save_layer_parity();
    test_gpu_gradients_and_pattern_parity();
    test_gpu_effects_and_shadows_parity();
    test_gpu_text_decorations_and_shadows_parity();
    test_gpu_shared_state_reset_and_blur_hardening();
    test_gpu_p3_svg_lottie_3d_parity();
    test_gpu_phase1_ellipsis_and_direction_parity();
    test_gpu_phase2_placeholders_rich_paint_and_gaps_parity();
    test_gpu_phase3_features_variable_fonts_and_strut_parity();
    test_gpu_phase4_incremental_updates_cache_and_visitor_parity();

    std::cout << "========================================" << std::endl;
    std::cout << "All Nisaba GPU pipeline tests PASSED (30/30)!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
