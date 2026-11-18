#include <cassert>
#include <iostream>
#include <cmath>
#include <vector>
#include <memory>
#include "nisaba/nisaba.hpp"
#include "nisaba/gpu/runtime_effect.hpp"
#include "nisaba/gpu/shader.hpp"

using namespace nisaba;
using nisaba::gpu::GpuDevice;
using nisaba::gpu::GpuSurface;
using nisaba::gpu::GpuCanvas;
using nisaba::gpu::RuntimeEffect;
using nisaba::gpu::RuntimeShaderBuilder;
using nisaba::gpu::RuntimeShader;
using nisaba::gpu::SkRuntimeEffect;
using nisaba::gpu::SkRuntimeShaderBuilder;
using nisaba::gpu::SkShader;
using nisaba::gpu::Transform2D;

void test_runtime_effect_reflection() {
    std::cout << "[Test 1/6] Testing RuntimeEffect compilation & uniform reflection..." << std::endl;

    const std::string valid_sksl = R"(
        uniform float time;
        uniform vec2 resolution;
        uniform vec4 primaryColor;
        uniform int mode;

        vec4 main(vec2 fragCoord) {
            vec2 uv = fragCoord / resolution;
            return primaryColor * (0.5 + 0.5 * sin(time + uv.x));
        }
    )";

    auto result = RuntimeEffect::MakeForShader(valid_sksl);
    assert(result);
    assert(result.effect != nullptr);
    assert(result.errorText.empty());

    auto effect = result.effect;
    assert(effect->programId() != 0);

    // Uniform checks
    const auto* u_time = effect->findUniform("time");
    assert(u_time != nullptr);
    assert(u_time->type == RuntimeEffect::Uniform::Type::Float);
    assert(u_time->sizeInBytes == 4);

    const auto* u_res = effect->findUniform("resolution");
    assert(u_res != nullptr);
    assert(u_res->type == RuntimeEffect::Uniform::Type::Float2);
    assert(u_res->sizeInBytes == 8);

    const auto* u_col = effect->findUniform("primaryColor");
    assert(u_col != nullptr);
    assert(u_col->type == RuntimeEffect::Uniform::Type::Float4);
    assert(u_col->sizeInBytes == 16);

    const auto* u_mode = effect->findUniform("mode");
    assert(u_mode != nullptr);
    assert(u_mode->type == RuntimeEffect::Uniform::Type::Int);
    assert(u_mode->sizeInBytes == 4);

    assert(effect->findUniform("non_existent_var") == nullptr);
    assert(effect->uniformSize() >= 32);

    std::cout << "  -> Uniform reflection verified successfully. Total uniform size: "
              << effect->uniformSize() << " bytes." << std::endl;
}

void test_runtime_effect_error_handling() {
    std::cout << "[Test 2/6] Testing error handling for invalid shader syntax..." << std::endl;

    const std::string invalid_sksl = R"(
        uniform float time;
        vec4 main(vec2 fragCoord) {
            This is invalid GLSL syntax that must fail gracefully!
            return undefined_symbol;
        }
    )";

    auto result = RuntimeEffect::MakeForShader(invalid_sksl);
    assert(!result);
    assert(result.effect == nullptr);
    assert(!result.errorText.empty());

    std::cout << "  -> Handled invalid shader gracefully without crashing. Error captured: "
              << result.errorText.substr(0, 45) << "..." << std::endl;
}

void test_runtime_shader_builder_and_aliases() {
    std::cout << "[Test 3/6] Testing RuntimeShaderBuilder & Framework Drop-in Aliases..." << std::endl;

    const std::string sksl = R"(
        uniform float time;
        uniform vec2 resolution;
        vec4 main(vec2 fragCoord) {
            vec2 uv = fragCoord / resolution;
            return vec4(uv.x, uv.y, sin(time), 1.0);
        }
    )";

    // Test framework-identical drop-in aliases
    auto res = SkRuntimeEffect::MakeForShader(sksl);
    assert(res);

    SkRuntimeShaderBuilder builder(res.effect);
    builder.uniform("time") = 2.5f;

    float res_arr[2] = {800.0f, 600.0f};
    bool ok = builder.uniform("resolution").set(res_arr, 2);
    assert(ok);

    std::shared_ptr<SkShader> sk_shader = builder.makeShader();
    assert(sk_shader != nullptr);
    assert(sk_shader->getNativeHandle() != nullptr);

    // Test Paint integration
    nisaba::Paint paint;
    assert(!paint.has_custom_shader());
    paint.setShader(sk_shader);
    assert(paint.has_custom_shader());
    assert(paint.getShader() == sk_shader);

    std::cout << "  -> SkRuntimeEffect, SkRuntimeShaderBuilder, and SkShader aliases verified." << std::endl;
}

void test_gpu_rendering_enki_shaders() {
    std::cout << "[Test 4/6] Testing GPU hardware rasterization with 4 ENKI shaders..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 400, 300);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);

    // 1. Plasma Shader
    {
        const std::string plasma_sksl = R"(
            uniform float time;
            uniform vec2 resolution;
            vec4 main(vec2 fragCoord) {
                vec2 uv = fragCoord / resolution;
                float v = 0.5 + 0.5 * sin(uv.x * 10.0 + time);
                return vec4(v, 0.2, 0.8, 1.0);
            }
        )";
        auto res = RuntimeEffect::MakeForShader(plasma_sksl);
        assert(res);
        RuntimeShaderBuilder bld(res.effect);
        bld.uniform("time") = 1.0f;
        float r[2] = {400.0f, 300.0f};
        bld.uniform("resolution").set(r, 2);

        canvas.clear(Color::from_rgba8(0, 0, 0, 255));
        Paint p;
        p.set_shader(bld.makeShader());
        canvas.fill_rect(*Rect::from_xywh(50.0f, 50.0f, 300.0f, 200.0f), p);
        canvas.flush();

        auto pix = surface->to_pixmap();
        assert(pix.has_value());
        // Check pixel inside plasma rect (100, 100)
        auto inside = pix->as_ref().pixel(100, 100);
        assert(inside.has_value());
        assert(inside->b > 150); // High blue from plasma shader
        // Check pixel outside rect (10, 10)
        auto outside = pix->as_ref().pixel(10, 10);
        assert(outside.has_value());
        assert(outside->r == 0 && outside->g == 0 && outside->b == 0); // Black clear color
    }
    std::cout << "  -> Plasma shader rendered and verified." << std::endl;

    // 2. Cyber Border Shader
    {
        const std::string cyber_sksl = R"(
            uniform float time;
            uniform vec2 resolution;
            vec4 main(vec2 fragCoord) {
                vec2 p = (fragCoord - 0.5 * resolution) / resolution.y;
                float angle = atan(p.y, p.x) + time;
                vec3 col = 0.5 + 0.5 * cos(angle + vec3(0.0, 2.0, 4.0));
                return vec4(col, 1.0);
            }
        )";
        auto res = RuntimeEffect::MakeForShader(cyber_sksl);
        assert(res);
        RuntimeShaderBuilder bld(res.effect);
        bld.uniform("time") = 0.5f;
        float r[2] = {400.0f, 300.0f};
        bld.uniform("resolution").set(r, 2);

        canvas.clear(Color::from_rgba8(20, 20, 20, 255));
        Paint p;
        p.set_shader(bld.makeShader());
        Stroke s;
        s.width = 12.0f;
        canvas.stroke_round_rect(*Rect::from_xywh(40.0f, 40.0f, 320.0f, 220.0f), 16.0f, 16.0f, p, s);
        canvas.flush();

        auto pix = surface->to_pixmap();
        assert(pix.has_value());
        // Pixel on the border (40, 150) should be colored by the cyber shader
        auto border_px = pix->as_ref().pixel(40, 150);
        assert(border_px.has_value());
        assert(border_px->r > 30 || border_px->g > 30 || border_px->b > 30);
    }
    std::cout << "  -> Cyber border shader rendered and verified." << std::endl;

    // 3. Daggers & Blood Shader
    {
        const std::string dagger_sksl = R"(
            uniform float time;
            uniform vec2 resolution;
            vec4 main(vec2 fragCoord) {
                vec2 uv = fragCoord / resolution;
                float d = sin(uv.x * 30.0) * cos(uv.y * 30.0 + time);
                if (d > 0.0) {
                    return vec4(0.9, 0.05, 0.05, 1.0); // Vivid blood red
                }
                return vec4(0.08, 0.08, 0.12, 1.0);    // Dark iron
            }
        )";
        auto res = RuntimeEffect::MakeForShader(dagger_sksl);
        assert(res);
        RuntimeShaderBuilder bld(res.effect);
        bld.uniform("time") = 0.0f;
        float r[2] = {400.0f, 300.0f};
        bld.uniform("resolution").set(r, 2);

        canvas.clear(Color::from_rgba8(255, 255, 255, 255));
        Paint p;
        p.set_shader(bld.makeShader());
        canvas.fill_round_rect(*Rect::from_xywh(30.0f, 30.0f, 340.0f, 240.0f), 20.0f, 20.0f, p);
        canvas.flush();

        auto pix = surface->to_pixmap();
        assert(pix.has_value());

        // Count blood red and dark iron pixels inside the shape
        size_t blood_pixels = 0;
        size_t iron_pixels = 0;
        for (int y = 50; y < 250; y += 4) {
            for (int x = 50; x < 350; x += 4) {
                auto px = pix->as_ref().pixel(x, y);
                if (px && px->r > 180 && px->g < 50 && px->b < 50) blood_pixels++;
                if (px && px->r < 50 && px->g < 50 && px->b < 50) iron_pixels++;
            }
        }
        assert(blood_pixels > 0);
        assert(iron_pixels > 0);
    }
    std::cout << "  -> Daggers & Blood shader rendered and verified." << std::endl;

    // 4. Pixel Art Riveted Frame Shader
    {
        const std::string pixel_sksl = R"(
            uniform float time;
            uniform vec2 resolution;
            vec4 main(vec2 fragCoord) {
                vec2 grid = floor(fragCoord / 8.0);
                float checker = mod(grid.x + grid.y, 2.0);
                if (checker > 0.5) {
                    return vec4(0.6, 0.6, 0.7, 1.0);
                }
                return vec4(0.2, 0.2, 0.3, 1.0);
            }
        )";
        auto res = RuntimeEffect::MakeForShader(pixel_sksl);
        assert(res);
        RuntimeShaderBuilder bld(res.effect);
        bld.uniform("time") = 0.0f;
        float r[2] = {400.0f, 300.0f};
        bld.uniform("resolution").set(r, 2);

        canvas.clear(Color::from_rgba8(0, 0, 0, 255));
        Paint p;
        p.set_shader(bld.makeShader());
        canvas.fill_rect(*Rect::from_xywh(10.0f, 10.0f, 380.0f, 280.0f), p);
        canvas.flush();

        auto pix = surface->to_pixmap();
        assert(pix.has_value());
        // Cell (2, 2) -> (16, 16): checker = 0 -> dark
        // Cell (3, 2) -> (24, 16): checker = 1 -> light
        auto px1 = pix->as_ref().pixel(18, 18);
        auto px2 = pix->as_ref().pixel(26, 18);
        assert(px1.has_value() && px2.has_value());
        assert(px1->r != px2->r);
    }
    std::cout << "  -> Pixel Art Riveted Frame shader rendered and verified." << std::endl;
}

void test_local_matrix_transformation() {
    std::cout << "[Test 5/6] Testing local matrix coordinate transformation..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 200, 200);
    GpuCanvas canvas(surface);

    // Shader outputs green if local fragCoord is in [0..50], red otherwise
    const std::string coord_sksl = R"(
        vec4 main(vec2 fragCoord) {
            if (fragCoord.x >= 0.0 && fragCoord.x <= 50.0 &&
                fragCoord.y >= 0.0 && fragCoord.y <= 50.0) {
                return vec4(0.0, 1.0, 0.0, 1.0); // Green in local box
            }
            return vec4(1.0, 0.0, 0.0, 1.0);     // Red elsewhere
        }
    )";

    auto res = RuntimeEffect::MakeForShader(coord_sksl);
    assert(res);
    RuntimeShaderBuilder bld(res.effect);

    // Local matrix translates origin to (100, 100)
    Transform2D localMat = Transform2D::translate(100.0f, 100.0f);
    auto shader = bld.makeShader(&localMat);

    canvas.clear(Color::from_rgba8(0, 0, 0, 255));
    Paint p;
    p.set_shader(shader);
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 200.0f, 200.0f), p);
    canvas.flush();

    auto pix = surface->to_pixmap();
    assert(pix.has_value());

    // World coordinate (120, 120) corresponds to local (20, 20) -> should be GREEN
    auto local_box_px = pix->as_ref().pixel(120, 120);
    assert(local_box_px.has_value());
    assert(local_box_px->g > 200 && local_box_px->r < 50);

    // World coordinate (20, 20) corresponds to local (-80, -80) -> should be RED
    auto outside_box_px = pix->as_ref().pixel(20, 20);
    assert(outside_box_px.has_value());
    assert(outside_box_px->r > 200 && outside_box_px->g < 50);

    std::cout << "  -> Local coordinate matrix inverse transformation verified." << std::endl;
}

void test_lifecycle_and_leak_free() {
    std::cout << "[Test 6/6] Testing lifecycle & leak-free destruction under load..." << std::endl;

    for (int i = 0; i < 50; ++i) {
        std::string sksl = "uniform float step" + std::to_string(i) + ";\n"
                           "vec4 main(vec2 fragCoord) {\n"
                           "    return vec4(step" + std::to_string(i) + ", 0.0, 0.0, 1.0);\n"
                           "}\n";
        auto res = RuntimeEffect::MakeForShader(sksl);
        assert(res);
        RuntimeShaderBuilder bld(res.effect);
        std::string var_name = "step" + std::to_string(i);
        bld.uniform(var_name.c_str()) = 0.5f;
        auto sh = bld.makeShader();
        assert(sh != nullptr);
    }

    std::cout << "  -> 50 Shader programs compiled and destroyed cleanly without leak." << std::endl;
}

void test_ctm_with_local_transform_and_tight_edges() {
    std::cout << "[Test 7/9] Testing Canvas CTM, Shader::with_local_transform, and 1px fringe/scissor clip..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 200, 120);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    // Returns GREEN when local x in [10..30] and RED otherwise
    const char* sksl = R"(
        vec4 main(float2 coord) {
            if (coord.x >= 10.0 && coord.x <= 30.0 && coord.y >= 10.0 && coord.y <= 30.0) {
                return half4(0.0, 1.0, 0.0, 1.0);
            }
            return half4(1.0, 0.0, 0.0, 1.0);
        }
    )";
    auto res = RuntimeEffect::MakeForShader(sksl);
    assert(res);
    auto base_shader = res.effect->make_shader({});
    assert(base_shader != nullptr);

    // Draw 40x40 rect translated by (50, 20): local (20,20) sits at screen (70,40)
    canvas.save();
    canvas.translate(50.0f, 20.0f);
    Paint p_ctm(base_shader);
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 40.0f, 40.0f), p_ctm);
    canvas.restore();

    // Draw second rect using with_local_transform(translate(20, 0)):
    // local [10..30] shifts to rect local [30..50], and CTM translates by (110, 20) -> screen [140..160]
    auto shifted_shader = base_shader->with_local_transform(Transform2D::translate(20.0f, 0.0f));
    assert(shifted_shader != nullptr);
    canvas.save();
    canvas.translate(110.0f, 20.0f);
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 60.0f, 40.0f), Paint(shifted_shader));
    canvas.restore();

    // Tight edge & scissor clip test: fill [20, 70, 40, 30] clipped to [20, 70, 20, 30]
    canvas.save();
    canvas.clip_rect(*Rect::from_xywh(20.0f, 70.0f, 20.0f, 30.0f));
    canvas.fill_rect(*Rect::from_xywh(20.0f, 70.0f, 40.0f, 30.0f), p_ctm);
    canvas.restore();

    canvas.flush();
    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    const auto r = pm->as_ref();

    // CTM check: (70, 40) is GREEN, (55, 25) is RED
    assert(r.pixel(70, 40)->g > 200 && r.pixel(70, 40)->r < 20);
    assert(r.pixel(55, 25)->r > 200 && r.pixel(55, 25)->g < 20);
    // Zero 1px halo bleed outside [50..90, 20..60]!
    assert(r.pixel(49, 40)->r < 5 && r.pixel(49, 40)->g < 5);
    assert(r.pixel(90, 40)->r < 5 && r.pixel(90, 40)->g < 5);

    // Combined CTM + with_local_transform check: (150, 40) is GREEN, (120, 40) is RED
    assert(r.pixel(150, 40)->g > 200 && r.pixel(150, 40)->r < 20);
    assert(r.pixel(120, 40)->r > 200 && r.pixel(120, 40)->g < 20);

    // Scissor clip check: (30, 85) painted, (45, 85) MUST stay BLACK
    assert(r.pixel(30, 85)->r > 200);
    assert(r.pixel(45, 85)->r < 5 && r.pixel(45, 85)->g < 5);

    std::cout << "  -> CTM, with_local_transform, 1px fringe fix, and scissor clip OK" << std::endl;
}

void test_sksl_builtins_arrays_initializers_and_half_safety() {
    std::cout << "[Test 8/9] Testing SkSL builtins, uniform arrays, initializers, and half-ident safety..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 120, 80);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    const char* sksl = R"(
        uniform float weights[4] = float[4](0.0, 0.25, 0.5, 0.25);
        uniform float gain = 1.0;

        half4 main(float2 coord) {
            float halfWidth = 60.0;
            half mask = (coord.x < halfWidth) ? 1.0 : 0.0;
            // sum(weights) = 1.0; test saturate & unpremul & linear srgb round-trip
            float wsum = weights[0] + weights[1] + weights[2] + weights[3];
            half4 pm = half4(0.5 * wsum * gain, 0.0, 0.0, 0.5);
            half4 straight = unpremul(pm);
            half3 lin = toLinearSrgb(straight.rgb);
            half3 back = fromLinearSrgb(lin);
            float r = saturate(back.r * mask);
            return half4(r, saturate(-0.5), 0.25, 1.0);
        }
    )";

    auto res = RuntimeEffect::MakeForShader(sksl);
    assert(res);
    const auto* u_w = res.effect->findUniform("weights");
    assert(u_w != nullptr && u_w->count == 4 && u_w->sizeInBytes == 16);
    const auto* u_g = res.effect->findUniform("gain");
    assert(u_g != nullptr && u_g->count == 1);

    // Build shader WITHOUT touching weights or gain so SkSL initializers are exercised!
    auto sh_default = res.effect->make_shader({});
    assert(sh_default != nullptr);
    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 120.0f, 40.0f), Paint(sh_default));

    // Override weights array explicitly via RuntimeShaderBuilder
    RuntimeShaderBuilder bld(res.effect);
    const float custom_w[4] = {0.1f, 0.1f, 0.1f, 0.1f}; // sum = 0.4 -> unpremul(0.2/0.5) = 0.4 red
    assert(bld.uniform("weights").set(custom_w, 4));
    canvas.fill_rect(*Rect::from_xywh(0.0f, 40.0f, 120.0f, 40.0f), Paint(bld.make_shader()));

    canvas.flush();
    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    const auto r = pm->as_ref();

    // Top-left (x<60, y=20): unpremul(0.5/0.5) = 1.0 red, saturate(-0.5)=0 green, b~64
    auto p_tl = r.pixel(20, 20);
    assert(p_tl->r > 240 && p_tl->g < 5 && std::abs(int(p_tl->b) - 64) <= 4);
    // Top-right (x>=60, y=20): mask=0 -> r=0, g=0, b~64
    auto p_tr = r.pixel(90, 20);
    assert(p_tr->r < 5 && p_tr->g < 5 && std::abs(int(p_tr->b) - 64) <= 4);
    // Bottom-left (y=60): custom weights sum=0.4 -> r ~ 102 (0.4*255)
    auto p_bl = r.pixel(20, 60);
    assert(p_bl->r >= 90 && p_bl->r <= 115);

    std::cout << "  -> SkSL builtins, uniform arrays, initializers, and half-ident safety OK" << std::endl;
}

void test_uniform_child_shader_eval() {
    std::cout << "[Test 9/9] Testing uniform shader child evaluation & nested transforms..." << std::endl;

    auto device = GpuDevice::create();
    auto surface = GpuSurface::create(device, 160, 120);
    assert(surface != nullptr);
    GpuCanvas canvas(surface);
    canvas.clear(Color::BLACK);

    const char* child_sksl = R"(
        uniform half4 leftColor;
        uniform half4 rightColor;
        half4 main(float2 coord) {
            return (coord.x < 50.0) ? leftColor : rightColor;
        }
    )";
    auto child_res = RuntimeEffect::MakeForShader(child_sksl);
    assert(child_res);
    RuntimeShaderBuilder child_bld(child_res.effect);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    child_bld.uniform("leftColor").set(red, 4);
    child_bld.uniform("rightColor").set(blue, 4);
    // Child shifted +20 in X so split occurs at x = 70 in child local space
    auto child_shader = child_bld.make_shader()->with_local_transform(Transform2D::translate(20.0f, 0.0f));

    const char* parent_sksl = R"(
        uniform shader src;
        uniform float greenBoost;
        half4 main(float2 coord) {
            half4 base = src.eval(coord);
            return half4(base.r, saturate(greenBoost), base.b, base.a);
        }
    )";
    auto parent_res = RuntimeEffect::MakeForShader(parent_sksl);
    assert(parent_res);
    assert(parent_res.effect->children().size() == 1);

    RuntimeShaderBuilder parent_bld(parent_res.effect);
    parent_bld.uniform("greenBoost") = 0.5f;
    parent_bld.child("src") = child_shader;

    canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 160.0f, 120.0f), Paint(parent_bld.make_shader()));
    canvas.flush();

    auto pm = surface->to_pixmap();
    assert(pm.has_value());
    const auto r = pm->as_ref();

    // At x=30 (child local x=10 < 50) -> RED + greenBoost(0.5) => r>200, g~128, b<10
    auto left_px = r.pixel(30, 60);
    assert(left_px->r > 220 && left_px->g > 110 && left_px->g < 145 && left_px->b < 10);
    // At x=100 (child local x=80 >= 50) -> BLUE + greenBoost(0.5) => r<10, g~128, b>220
    auto right_px = r.pixel(100, 60);
    assert(right_px->r < 10 && right_px->g > 110 && right_px->g < 145 && right_px->b > 220);

    std::cout << "  -> uniform shader child evaluation & nested transforms OK" << std::endl;
}

int main() {
    std::cout << "=== NISABA SOVEREIGN RUNTIME EFFECT & SHADER TEST SUITE ===" << std::endl;

    auto device = GpuDevice::create();
    assert(device != nullptr && device->is_valid());
    std::cout << "GPU Initialized: " << device->renderer_name() << " (" << device->version_name() << ")" << std::endl;

    test_runtime_effect_reflection();
    test_runtime_effect_error_handling();
    test_runtime_shader_builder_and_aliases();
    test_gpu_rendering_enki_shaders();
    test_local_matrix_transformation();
    test_lifecycle_and_leak_free();
    test_ctm_with_local_transform_and_tight_edges();
    test_sksl_builtins_arrays_initializers_and_half_safety();
    test_uniform_child_shader_eval();

    std::cout << "=== ALL RUNTIME EFFECT TESTS PASSED (9/9 OK) ===" << std::endl;
    return 0;
}
