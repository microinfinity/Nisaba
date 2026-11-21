#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::effects;
using namespace nisaba::text;

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string resolve_font(std::string_view filename) {
    const std::vector<std::string> candidates = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename),
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return candidates[0];
}

Stroke make_stroke(float w, LineCap cap = LineCap::Butt, LineJoin join = LineJoin::Miter) noexcept {
    Stroke s(w);
    s.line_cap = cap;
    s.line_join = join;
    return s;
}

Path make_star_path(float cx, float cy, float r_outer, float r_inner, int points = 5) {
    PathBuilder pb;
    for (int i = 0; i < points * 2; ++i) {
        const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / static_cast<float>(points);
        const float r = (i % 2 == 0) ? r_outer : r_inner;
        const float px = cx + std::cos(angle) * r;
        const float py = cy + std::sin(angle) * r;
        if (i == 0) pb.move_to(px, py);
        else pb.line_to(px, py);
    }
    pb.close();
    auto p = pb.finish();
    return p ? *p : Path{};
}

Path make_diamond_stamp(float rx, float ry) {
    PathBuilder pb;
    pb.move_to(rx, 0.0f);
    pb.line_to(0.0f, ry);
    pb.line_to(-rx, 0.0f);
    pb.line_to(0.0f, -ry);
    pb.close();
    auto p = pb.finish();
    return p ? *p : Path{};
}

Path make_wave_path(float x0, float y0, float w, float amp, int segs = 28) {
    PathBuilder pb;
    for (int i = 0; i <= segs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segs);
        const float px = x0 + t * w;
        const float py = y0 + std::sin(t * 2.0f * kPi) * amp;
        if (i == 0) pb.move_to(px, py);
        else pb.line_to(px, py);
    }
    auto p = pb.finish();
    return p ? *p : Path{};
}

Path make_zigzag_path(float x0, float y0, float w, float h, int peaks = 5) {
    PathBuilder pb;
    const float step = w / static_cast<float>(peaks * 2);
    pb.move_to(x0, y0 + h * 0.5f);
    for (int i = 1; i <= peaks * 2; ++i) {
        const float px = x0 + static_cast<float>(i) * step;
        const float py = (i % 2 == 1) ? y0 : (y0 + h);
        pb.line_to(px, py);
    }
    auto p = pb.finish();
    return p ? *p : Path{};
}

double compute_mae(const Pixmap& a, const Pixmap& b) {
    const uint32_t w = std::min(a.width(), b.width());
    const uint32_t h = std::min(a.height(), b.height());
    if (w == 0 || h == 0) return 0.0;
    const auto ar = a.as_ref();
    const auto br = b.as_ref();
    double sum_abs = 0.0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            auto ap = ar.pixel(x, y);
            auto bp = br.pixel(x, y);
            if (ap && bp) {
                sum_abs += std::abs(int(ap->r) - int(bp->r))
                         + std::abs(int(ap->g) - int(bp->g))
                         + std::abs(int(ap->b) - int(bp->b));
            }
        }
    }
    return sum_abs / (static_cast<double>(w) * static_cast<double>(h) * 3.0);
}

template <typename DrawBackend, typename DrawBufferFn>
void render_effects_showcase(
    DrawBackend& backend,
    DrawBufferFn&& draw_buf,
    FontSystem& fs,
    ParagraphCache& pcache,
    uint32_t W,
    uint32_t H
) {
    // -------------------------------------------------------------------------
    // 1. Dark Obsidian Background + Ambient Radial Accents
    // -------------------------------------------------------------------------
    auto bg_grad = LinearGradient::create(
        Point::from_xy(0.0f, 0.0f),
        Point::from_xy(static_cast<float>(W), static_cast<float>(H)),
        {
            GradientStop::create(0.0f, Color::from_rgba8(10, 14, 24, 255)),
            GradientStop::create(0.5f, Color::from_rgba8(14, 19, 34, 255)),
            GradientStop::create(1.0f, Color::from_rgba8(8, 11, 20, 255)),
        }
    );
    Paint bg_paint = bg_grad ? Paint{Shader{*bg_grad}} : Paint(Color::from_rgba8(11, 15, 25, 255));
    backend.fill_rect(*Rect::from_xywh(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H)), bg_paint);

    auto draw_label = [&](float x, float y, std::string_view text, float size, Color color,
                          bool mono = false, float weight = 500.0f) {
        Attrs a;
        if (mono) {
            a.set_family(Family("Mono"));
        } else {
            a.set_font_families({Family("InterVar"), Family("ArabicVar")})
             .add_font_variation("wght", weight);
        }
        a.set_color(TextColor::rgb(
            static_cast<uint8_t>(std::clamp(color.red() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(color.green() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(color.blue() * 255.0f, 0.0f, 255.0f))));
        auto buf = pcache.get_or_layout(fs, text, a, Metrics(size, size * 1.35f));
        draw_buf(*buf, x, y, Color::WHITE);
    };

    auto draw_well = [&](float x, float y, float w, float h, float r = 10.0f) {
        const Rect rect = *Rect::from_xywh(x, y, w, h);
        backend.fill_round_rect(rect, r, r, Paint(Color::from_rgba8(11, 15, 26, 230)));
        backend.stroke_round_rect(rect, r, r, Paint(Color::from_rgba8(44, 58, 88, 145)), Stroke(1.0f));
    };

    auto draw_card = [&](float x, float y, float w, float h, std::string_view title,
                         std::string_view badge, Color accent) {
        const Rect r = *Rect::from_xywh(x, y, w, h);
        auto card_grad = LinearGradient::create(
            Point::from_xy(x, y),
            Point::from_xy(x, y + h),
            {
                GradientStop::create(0.0f, Color::from_rgba8(24, 31, 49, 236)),
                GradientStop::create(1.0f, Color::from_rgba8(16, 21, 35, 242)),
            }
        );
        Paint fill_p = card_grad ? Paint{Shader{*card_grad}} : Paint(Color::from_rgba8(20, 26, 42, 238));
        backend.fill_round_rect(r, 16.0f, 16.0f, fill_p);
        backend.stroke_round_rect(r, 16.0f, 16.0f, Paint(Color::from_rgba8(58, 75, 114, 165)), Stroke(1.25f));

        // Top accent bar
        backend.fill_round_rect(*Rect::from_xywh(x + 18.0f, y + 14.0f, 44.0f, 4.0f), 2.0f, 2.0f, Paint(accent));
        draw_label(x + 18.0f, y + 25.0f, title, 16.0f, Color::from_rgba8(236, 242, 255, 255), false, 700.0f);

        // Right badge pill
        Attrs b_attrs;
        b_attrs.set_family(Family("Mono"))
               .set_color(TextColor::rgb(
                   static_cast<uint8_t>(accent.red() * 255.0f),
                   static_cast<uint8_t>(accent.green() * 255.0f),
                   static_cast<uint8_t>(accent.blue() * 255.0f)));
        auto b_buf = pcache.get_or_layout(fs, badge, b_attrs, Metrics(11.0f, 15.0f));
        const float bw = b_buf->max_line_width() + 14.0f;
        const float bx = x + w - bw - 16.0f;
        backend.fill_round_rect(
            *Rect::from_xywh(bx, y + 21.0f, bw, 22.0f), 11.0f, 11.0f,
            Paint(Color::from_rgba8(
                static_cast<uint8_t>(accent.red() * 255.0f),
                static_cast<uint8_t>(accent.green() * 255.0f),
                static_cast<uint8_t>(accent.blue() * 255.0f), 36)));
        draw_buf(*b_buf, bx + 7.0f, y + 25.0f, Color::WHITE);
    };

    // -------------------------------------------------------------------------
    // 2. Header Banner
    // -------------------------------------------------------------------------
    {
        const Rect hdr = *Rect::from_xywh(36.0f, 22.0f, 1528.0f, 94.0f);
        auto hdr_grad = LinearGradient::create(
            Point::from_xy(36.0f, 22.0f),
            Point::from_xy(1564.0f, 116.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(24, 34, 58, 246)),
                GradientStop::create(0.55f, Color::from_rgba8(20, 27, 47, 246)),
                GradientStop::create(1.0f, Color::from_rgba8(17, 23, 40, 246)),
            }
        );
        backend.fill_round_rect(hdr, 18.0f, 18.0f, hdr_grad ? Paint{Shader{*hdr_grad}} : Paint(Color::from_rgba8(22, 30, 50, 245)));
        backend.stroke_round_rect(hdr, 18.0f, 18.0f, Paint(Color::from_rgba8(72, 96, 148, 175)), Stroke(1.25f));

        // Glowing accent emblem on left
        Paint orb_p(Color::from_rgba8(56, 224, 255, 255));
        orb_p.set_mask_filter(MaskFilter::blur(BlurStyle::Solid, 7.0f));
        backend.fill_circle(76.0f, 69.0f, 16.0f, orb_p);
        backend.fill_circle(76.0f, 69.0f, 7.0f, Paint(Color::from_rgba8(240, 250, 255, 255)));

        draw_label(112.0f, 36.0f, "NISABA EFFECTS ENGINE SHOWCASE", 24.0f,
                   Color::from_rgba8(244, 248, 255, 255), false, 800.0f);
        draw_label(112.0f, 72.0f,
                   "Composable DAG · RuntimeShader · Perlin · Spatial Ops · Extended ColorEffect · PathEffect · 3D Lighting · Custom Blenders",
                   13.5f, Color::from_rgba8(152, 175, 214, 255), false, 500.0f);

        // Right status pills
        const char* pills[3] = {"PHASES 1-4", "CPU / GL / VK", "ZERO-RTTI"};
        const Color pill_colors[3] = {
            Color::from_rgba8(56, 224, 255, 255),
            Color::from_rgba8(168, 128, 255, 255),
            Color::from_rgba8(74, 232, 166, 255),
        };
        float px = 1544.0f;
        for (int i = 2; i >= 0; --i) {
            Attrs pa;
            pa.set_family(Family("Mono"))
              .set_color(TextColor::rgb(
                  static_cast<uint8_t>(pill_colors[i].red() * 255.0f),
                  static_cast<uint8_t>(pill_colors[i].green() * 255.0f),
                  static_cast<uint8_t>(pill_colors[i].blue() * 255.0f)));
            auto pb = pcache.get_or_layout(fs, pills[i], pa, Metrics(11.5f, 16.0f));
            const float pw = pb->max_line_width() + 18.0f;
            px -= pw;
            backend.fill_round_rect(
                *Rect::from_xywh(px, 55.0f, pw, 26.0f), 13.0f, 13.0f,
                Paint(Color::from_rgba8(
                    static_cast<uint8_t>(pill_colors[i].red() * 255.0f),
                    static_cast<uint8_t>(pill_colors[i].green() * 255.0f),
                    static_cast<uint8_t>(pill_colors[i].blue() * 255.0f), 34)));
            backend.stroke_round_rect(
                *Rect::from_xywh(px, 55.0f, pw, 26.0f), 13.0f, 13.0f,
                Paint(Color::from_rgba8(
                    static_cast<uint8_t>(pill_colors[i].red() * 255.0f),
                    static_cast<uint8_t>(pill_colors[i].green() * 255.0f),
                    static_cast<uint8_t>(pill_colors[i].blue() * 255.0f), 120)),
                Stroke(1.0f));
            draw_buf(*pb, px + 9.0f, 60.0f, Color::WHITE);
            px -= 10.0f;
        }
    }

    constexpr float kCardW = 494.0f;
    constexpr float kCardH = 496.0f;
    constexpr float kColX[3] = {36.0f, 553.0f, 1070.0f};
    constexpr float kRowY[2] = {136.0f, 654.0f};

    // =========================================================================
    // CARD 1 (Top-Left): Phase 1.1 & 1.2 — ImageFilter DAG & RuntimeShader
    // =========================================================================
    {
        const float cx = kColX[0], cy = kRowY[0];
        draw_card(cx, cy, kCardW, kCardH, "ImageFilter DAG & RuntimeShader", "PHASE 1.1-1.2",
                  Color::from_rgba8(56, 224, 255, 255));

        // Well A: Compose + Merge + Offset + ColorFilter (Chromatic Ghost Emblem)
        draw_well(cx + 18.0f, cy + 62.0f, 222.0f, 198.0f);
        draw_label(cx + 28.0f, cy + 70.0f, "DAG: Merge + Offset + Blur", 11.5f,
                   Color::from_rgba8(140, 218, 255, 255), true);
        {
            auto cyan_ghost = ImageFilter::compose(
                ImageFilter::blur(5.0f),
                ImageFilter::offset(-10.0f, -6.0f,
                    ImageFilter::color_filter(ColorEffect::tint(Color::from_rgba8(32, 232, 255, 255), 1.0f)))
            );
            auto magenta_ghost = ImageFilter::compose(
                ImageFilter::blur(5.0f),
                ImageFilter::offset(10.0f, 6.0f,
                    ImageFilter::color_filter(ColorEffect::tint(Color::from_rgba8(255, 58, 178, 255), 1.0f)))
            );
            auto dag = ImageFilter::merge({cyan_ghost, magenta_ghost, std::nullopt});

            Paint p(Color::from_rgba8(245, 248, 255, 255));
            p.set_image_filter(dag);
            backend.fill_path(make_star_path(cx + 129.0f, cy + 168.0f, 52.0f, 22.0f, 6), p);
        }

        // Well B: RuntimeShader ImageFilter (RGB Chromatic Aberration + Scanline)
        draw_well(cx + 254.0f, cy + 62.0f, 222.0f, 198.0f);
        draw_label(cx + 264.0f, cy + 70.0f, "RuntimeShader: Chromatic", 11.5f,
                   Color::from_rgba8(140, 218, 255, 255), true);
        {
            const std::string ca_src = R"(
                uniform shader child;
                uniform float shift;
                vec4 main(vec2 coord) {
                    vec4 r = child.eval(coord + vec2(shift, 0.0));
                    vec4 g = child.eval(coord);
                    vec4 b = child.eval(coord - vec2(shift, 0.0));
                    float scan = 0.86 + 0.14 * sin(coord.y * 1.4);
                    float a = max(max(r.a, g.a), b.a);
                    return vec4(r.r * scan, g.g * scan, b.b * scan, a);
                }
            )";
            RuntimeFilterUniforms u;
            u.set_float("shift", 7.0f);
            auto rf = ImageFilter::runtime_shader(ca_src, u, "child");

            Paint p;
            p.set_image_filter(rf);
            backend.save_layer_bounds(*Rect::from_xywh(cx + 266.0f, cy + 92.0f, 198.0f, 158.0f), p);
            backend.stroke_circle(cx + 365.0f, cy + 170.0f, 48.0f,
                                  Paint(Color::from_rgba8(255, 255, 255, 255)), Stroke(9.0f));
            backend.fill_path(make_star_path(cx + 365.0f, cy + 170.0f, 30.0f, 13.0f, 5),
                              Paint(Color::from_rgba8(230, 240, 255, 255)));
            backend.restore();
        }

        // Well C (Bottom Wide): ImageFilter::blend(Screen) + CropRect Window
        draw_well(cx + 18.0f, cy + 274.0f, 458.0f, 204.0f);
        draw_label(cx + 28.0f, cy + 282.0f, "DAG: Blend(Screen, Blur, ColorFilter) + CropRect Window", 11.5f,
                   Color::from_rgba8(140, 218, 255, 255), true);
        {
            const Rect crop = *Rect::from_xywh(cx + 42.0f, cy + 314.0f, 410.0f, 146.0f);
            // Reference dashed crop guide
            Paint guide_p(Color::from_rgba8(82, 128, 180, 140));
            guide_p.set_path_effect(PathEffect::dash({6.0f, 5.0f}, 0.0f));
            backend.stroke_rect(crop, guide_p, Stroke(1.2f));

            auto bg_blur = ImageFilter::blur(10.0f, std::nullopt, crop);
            auto fg_tint = ImageFilter::color_filter(
                ColorEffect::from_matrix(ColorMatrix::saturation(1.6f)), std::nullopt, crop);
            auto blended = ImageFilter::blend(BlendMode::Screen, bg_blur, fg_tint, crop);

            Paint layer_p;
            layer_p.set_image_filter(blended);
            backend.save_layer_bounds(crop, layer_p);

            auto bar_g = LinearGradient::create(
                Point::from_xy(cx + 60.0f, cy + 330.0f),
                Point::from_xy(cx + 430.0f, cy + 440.0f),
                {
                    GradientStop::create(0.0f, Color::from_rgba8(32, 210, 255, 255)),
                    GradientStop::create(0.5f, Color::from_rgba8(145, 92, 255, 255)),
                    GradientStop::create(1.0f, Color::from_rgba8(255, 92, 168, 255)),
                }
            );
            Paint bar_p = bar_g ? Paint{Shader{*bar_g}} : Paint(Color::from_rgba8(32, 210, 255, 255));
            backend.fill_round_rect(*Rect::from_xywh(cx + 68.0f, cy + 342.0f, 150.0f, 92.0f), 18.0f, 18.0f, bar_p);
            backend.fill_circle(cx + 285.0f, cy + 388.0f, 42.0f, Paint(Color::from_rgba8(255, 196, 62, 255)));
            backend.fill_path(make_star_path(cx + 385.0f, cy + 388.0f, 42.0f, 18.0f, 5),
                              Paint(Color::from_rgba8(82, 245, 178, 255)));
            backend.restore();
        }
    }

    // =========================================================================
    // CARD 2 (Top-Mid): Phase 1.3 & 1.4 — BlurMaskFilter & PerlinNoiseShader
    // =========================================================================
    {
        const float cx = kColX[1], cy = kRowY[0];
        draw_card(cx, cy, kCardW, kCardH, "BlurMaskFilter & PerlinNoise", "PHASE 1.3-1.4",
                  Color::from_rgba8(178, 132, 255, 255));

        // Top well: 4 BlurMaskFilter styles (Normal, Solid, Outer, Inner)
        draw_well(cx + 18.0f, cy + 62.0f, 458.0f, 198.0f);
        draw_label(cx + 28.0f, cy + 70.0f, "BlurMaskFilter Styles (sigma = 8.0)", 11.5f,
                   Color::from_rgba8(204, 174, 255, 255), true);

        const BlurStyle styles[4] = {BlurStyle::Normal, BlurStyle::Solid, BlurStyle::Outer, BlurStyle::Inner};
        const char* snames[4] = {"Normal", "Solid", "Outer", "Inner"};
        const Color scolors[4] = {
            Color::from_rgba8(64, 224, 255, 255),
            Color::from_rgba8(186, 118, 255, 255),
            Color::from_rgba8(255, 108, 176, 255),
            Color::from_rgba8(82, 242, 168, 255),
        };
        for (int i = 0; i < 4; ++i) {
            const float ox = cx + 78.0f + static_cast<float>(i) * 112.0f;
            const float oy = cy + 162.0f;
            // Subtle dark backing ring to make Outer/Inner unmistakably clear
            backend.stroke_circle(ox, oy, 32.0f, Paint(Color::from_rgba8(48, 60, 88, 110)), Stroke(1.0f));
            Paint mp(scolors[i]);
            mp.set_mask_filter(MaskFilter::blur(styles[i], 8.0f));
            backend.fill_circle(ox, oy, 30.0f, mp);
            draw_label(ox - 24.0f, cy + 226.0f, snames[i], 11.5f,
                       Color::from_rgba8(194, 208, 234, 255), true);
        }

        // Bottom Left: FractalNoise procedural nebula
        draw_well(cx + 18.0f, cy + 274.0f, 222.0f, 204.0f);
        draw_label(cx + 28.0f, cy + 282.0f, "Perlin: FractalNoise (4 oct)", 11.5f,
                   Color::from_rgba8(204, 174, 255, 255), true);
        {
            const Rect nr = *Rect::from_xywh(cx + 30.0f, cy + 308.0f, 198.0f, 156.0f);
            Shader fn = Shader::create_fractal_noise(0.032f, 0.032f, 4, 7.0f);
            Paint np(fn);
            np.set_color_filter(ColorEffect::tint(Color::from_rgba8(88, 196, 255, 255), 0.72f));
            backend.fill_round_rect(nr, 10.0f, 10.0f, np);
            backend.stroke_round_rect(nr, 10.0f, 10.0f, Paint(Color::from_rgba8(102, 186, 255, 140)), Stroke(1.0f));
        }

        // Bottom Right: Turbulence procedural plasma
        draw_well(cx + 254.0f, cy + 274.0f, 222.0f, 204.0f);
        draw_label(cx + 264.0f, cy + 282.0f, "Perlin: Turbulence (4 oct)", 11.5f,
                   Color::from_rgba8(204, 174, 255, 255), true);
        {
            const Rect nr = *Rect::from_xywh(cx + 266.0f, cy + 308.0f, 198.0f, 156.0f);
            Shader tb = Shader::create_turbulence(0.036f, 0.036f, 4, 19.0f);
            Paint np(tb);
            np.set_color_filter(ColorEffect::tint(Color::from_rgba8(255, 122, 168, 255), 0.72f));
            backend.fill_round_rect(nr, 10.0f, 10.0f, np);
            backend.stroke_round_rect(nr, 10.0f, 10.0f, Paint(Color::from_rgba8(255, 128, 178, 140)), Stroke(1.0f));
        }
    }

    // =========================================================================
    // CARD 3 (Top-Right): Phase 2 — Morphology, Convolution & Distortion
    // =========================================================================
    {
        const float cx = kColX[2], cy = kRowY[0];
        draw_card(cx, cy, kCardW, kCardH, "Spatial, Morphology & Distortion", "PHASE 2.1-2.2",
                  Color::from_rgba8(255, 188, 66, 255));

        // Top-Left: Dilate / Base / Erode
        draw_well(cx + 18.0f, cy + 62.0f, 222.0f, 198.0f);
        draw_label(cx + 28.0f, cy + 70.0f, "Dilate · Base · Erode", 11.5f,
                   Color::from_rgba8(255, 210, 118, 255), true);
        {
            const float sy = cy + 160.0f;
            // Dilated
            Paint dp(Color::from_rgba8(255, 182, 62, 255));
            dp.set_image_filter(ImageFilter::dilate(3.0f, 3.0f));
            backend.stroke_path(make_star_path(cx + 62.0f, sy, 26.0f, 11.0f, 5), dp, Stroke(3.0f));
            // Base
            backend.stroke_path(make_star_path(cx + 129.0f, sy, 26.0f, 11.0f, 5),
                                Paint(Color::from_rgba8(232, 240, 255, 255)), Stroke(3.0f));
            // Eroded (applied to a thicker filled star)
            Paint ep(Color::from_rgba8(72, 228, 255, 255));
            ep.set_image_filter(ImageFilter::erode(2.0f, 2.0f));
            backend.fill_path(make_star_path(cx + 196.0f, sy, 28.0f, 13.0f, 5), ep);

            draw_label(cx + 42.0f, cy + 226.0f, "Dilate", 10.5f, Color::from_rgba8(184, 198, 224, 255), true);
            draw_label(cx + 114.0f, cy + 226.0f, "Base", 10.5f, Color::from_rgba8(184, 198, 224, 255), true);
            draw_label(cx + 178.0f, cy + 226.0f, "Erode", 10.5f, Color::from_rgba8(184, 198, 224, 255), true);
        }

        // Top-Right: 3x3 MatrixConvolution (Emboss relief)
        draw_well(cx + 254.0f, cy + 62.0f, 222.0f, 198.0f);
        draw_label(cx + 264.0f, cy + 70.0f, "3x3 Emboss Convolution", 11.5f,
                   Color::from_rgba8(255, 210, 118, 255), true);
        {
            const Rect r = *Rect::from_xywh(cx + 272.0f, cy + 98.0f, 186.0f, 146.0f);
            std::vector<float> emboss = {
                -2.0f, -1.0f,  0.0f,
                -1.0f,  1.0f,  1.0f,
                 0.0f,  1.0f,  2.0f,
            };
            Paint lp;
            lp.set_image_filter(ImageFilter::matrix_convolution(
                3, 3, emboss, 1.0f, 0.35f, 1, 1, FilterTileMode::Clamp, false));
            backend.save_layer_bounds(r, lp);
            backend.fill_round_rect(*Rect::from_xywh(cx + 286.0f, cy + 112.0f, 158.0f, 118.0f), 14.0f, 14.0f,
                                    Paint(Color::from_rgba8(54, 72, 112, 255)));
            backend.fill_circle(cx + 332.0f, cy + 170.0f, 30.0f, Paint(Color::from_rgba8(255, 168, 62, 255)));
            backend.fill_path(make_star_path(cx + 398.0f, cy + 170.0f, 34.0f, 15.0f, 5),
                              Paint(Color::from_rgba8(82, 224, 255, 255)));
            backend.restore();
        }

        // Bottom-Left: Magnifier Lens
        draw_well(cx + 18.0f, cy + 274.0f, 222.0f, 204.0f);
        draw_label(cx + 28.0f, cy + 282.0f, "Magnifier Lens Filter", 11.5f,
                   Color::from_rgba8(255, 210, 118, 255), true);
        {
            const Rect box = *Rect::from_xywh(cx + 32.0f, cy + 308.0f, 194.0f, 154.0f);
            const Rect lens_src = *Rect::from_xywh(cx + 62.0f, cy + 334.0f, 134.0f, 102.0f);
            Paint lp;
            lp.set_image_filter(ImageFilter::magnifier(lens_src, 14.0f, std::nullopt, box));
            backend.save();
            backend.clip_rrect(RRect::from_rect_and_radius(box, 8.0f, 8.0f));
            backend.save_layer_bounds(box, lp);
            // Draw crisp geometric grid + center target
            for (int gy = 0; gy < 9; ++gy) {
                for (int gx = 0; gx < 11; ++gx) {
                    const bool even = ((gx + gy) % 2) == 0;
                    backend.fill_rect(
                        *Rect::from_xywh(cx + 30.0f + gx * 18.0f, cy + 306.0f + gy * 18.0f, 18.0f, 18.0f),
                        Paint(even ? Color::from_rgba8(30, 44, 76, 255) : Color::from_rgba8(56, 86, 142, 255)));
                }
            }
            backend.fill_circle(cx + 129.0f, cy + 385.0f, 22.0f, Paint(Color::from_rgba8(255, 192, 66, 255)));
            backend.fill_circle(cx + 129.0f, cy + 385.0f, 9.0f, Paint(Color::from_rgba8(22, 32, 56, 255)));
            backend.restore();
            backend.restore();
            backend.stroke_round_rect(box, 8.0f, 8.0f, Paint(Color::from_rgba8(255, 202, 92, 175)), Stroke(1.25f));
        }

        // Bottom-Right: DisplacementMap + AlphaThreshold
        draw_well(cx + 254.0f, cy + 274.0f, 222.0f, 204.0f);
        draw_label(cx + 264.0f, cy + 282.0f, "DisplacementMap Warp", 11.5f,
                   Color::from_rgba8(255, 210, 118, 255), true);
        {
            const Rect box = *Rect::from_xywh(cx + 268.0f, cy + 308.0f, 194.0f, 154.0f);
            // Displacement branch = blurred + hue-shifted copy of source -> organic ripple distortion
            auto disp_branch = ImageFilter::blur(9.0f);
            auto dm = ImageFilter::displacement_map(
                ColorChannel::R, ColorChannel::G, 26.0f, disp_branch, std::nullopt);
            Paint lp;
            lp.set_image_filter(dm);
            backend.save_layer_bounds(box, lp);
            for (int i = 0; i < 7; ++i) {
                const float sx = cx + 278.0f + static_cast<float>(i) * 25.0f;
                const Color c = (i % 2 == 0)
                    ? Color::from_rgba8(56, 218, 255, 255)
                    : Color::from_rgba8(255, 116, 178, 255);
                backend.fill_round_rect(*Rect::from_xywh(sx, cy + 322.0f, 15.0f, 126.0f), 6.0f, 6.0f, Paint(c));
            }
            backend.fill_circle(cx + 365.0f, cy + 385.0f, 34.0f, Paint(Color::from_rgba8(255, 218, 86, 255)));
            backend.restore();
        }
    }

    // =========================================================================
    // CARD 4 (Bottom-Left): Phase 3.1 — Extended ColorEffect
    // =========================================================================
    {
        const float cx = kColX[0], cy = kRowY[1];
        draw_card(cx, cy, kCardW, kCardH, "Extended ColorEffect Pipeline", "PHASE 3.1",
                  Color::from_rgba8(255, 108, 168, 255));

        auto draw_color_swatch = [&](float sx, float sy, float sw, float sh,
                                     std::string_view caption, const std::optional<ColorEffect>& ce,
                                     bool show_gold_backdrop = false) {
            draw_well(sx, sy, sw, sh, 10.0f);
            draw_label(sx + 10.0f, sy + 7.0f, caption, 11.2f, Color::from_rgba8(255, 176, 212, 255), true);

            const Rect inner = *Rect::from_xywh(sx + 10.0f, sy + 28.0f, sw - 20.0f, sh - 38.0f);
            if (show_gold_backdrop) {
                backend.fill_round_rect(inner, 8.0f, 8.0f, Paint(Color::from_rgba8(255, 190, 56, 255)));
            }
            auto rainbow = LinearGradient::create(
                Point::from_xy(inner.left(), inner.top()),
                Point::from_xy(inner.right(), inner.bottom()),
                {
                    GradientStop::create(0.0f, Color::from_rgba8(255, 68, 108, 255)),
                    GradientStop::create(0.33f, Color::from_rgba8(255, 206, 62, 255)),
                    GradientStop::create(0.66f, Color::from_rgba8(52, 224, 168, 255)),
                    GradientStop::create(1.0f, Color::from_rgba8(72, 132, 255, 255)),
                }
            );
            Paint p = rainbow ? Paint{Shader{*rainbow}} : Paint(Color::RED);
            if (ce) p.set_color_filter(*ce);
            backend.fill_round_rect(inner, 8.0f, 8.0f, p);

            // White core circle with same color effect for contrast readability
            Paint cp(Color::from_rgba8(232, 238, 252, 255));
            if (ce) cp.set_color_filter(*ce);
            backend.fill_circle(inner.left() + inner.width() * 0.82f,
                                inner.top() + inner.height() * 0.5f, 13.0f, cp);
        };

        // 1. Reference
        draw_color_swatch(cx + 18.0f, cy + 62.0f, 458.0f, 74.0f, "Reference Swatch (Unfiltered Gradient)", std::nullopt);

        // 2. Table LUT (4-level Posterize)
        std::array<uint8_t, 256> posterize_lut{};
        for (int i = 0; i < 256; ++i) {
            posterize_lut[static_cast<size_t>(i)] = static_cast<uint8_t>((i / 64) * 85);
        }
        draw_color_swatch(cx + 18.0f, cy + 146.0f, 458.0f, 74.0f,
                          "ColorEffect::table (256-entry 4-level Posterize LUT)",
                          ColorEffect::table(posterize_lut));

        // 3. HSLA Matrix (+0.45 Hue Shift, 1.25x Saturation)
        ColorMatrix hsla_m = ColorMatrix::identity();
        hsla_m.m[4] = 0.45f; // Hue +162 deg
        hsla_m.m[6] = 1.25f; // Saturation boost
        draw_color_swatch(cx + 18.0f, cy + 230.0f, 458.0f, 74.0f,
                          "ColorEffect::hsla_matrix (+162 deg Hue Shift & Sat Boost)",
                          ColorEffect::hsla_matrix(hsla_m));

        // 4. HighContrast (Accessibility Lightness Inversion + Contrast)
        HighContrastConfig hc{false, HighContrastInvertStyle::InvertLightness, 0.55f};
        draw_color_swatch(cx + 18.0f, cy + 314.0f, 458.0f, 74.0f,
                          "ColorEffect::high_contrast (InvertLightness, contrast=0.55)",
                          ColorEffect::high_contrast(hc));

        // 5. Split bottom row: Lerp(0.55, sepia, invert) & Luma()
        draw_color_swatch(cx + 18.0f, cy + 398.0f, 222.0f, 80.0f,
                          "ColorEffect::lerp(0.55)",
                          ColorEffect::lerp(0.55f, ColorEffect::sepia(1.0f), ColorEffect::invert()));
        draw_color_swatch(cx + 254.0f, cy + 398.0f, 222.0f, 80.0f,
                          "ColorEffect::luma() Mask",
                          ColorEffect::luma(), true);
    }

    // =========================================================================
    // CARD 5 (Bottom-Mid): Phase 3.2 — Composable PathEffect
    // =========================================================================
    {
        const float cx = kColX[1], cy = kRowY[1];
        draw_card(cx, cy, kCardW, kCardH, "Composable PathEffect", "PHASE 3.2",
                  Color::from_rgba8(68, 236, 168, 255));

        // Row 1: Corner(14) over dashed original polygon
        draw_well(cx + 18.0f, cy + 62.0f, 458.0f, 92.0f);
        draw_label(cx + 28.0f, cy + 69.0f, "PathEffect::corner(14)  [dim = sharp original]", 11.2f,
                   Color::from_rgba8(146, 246, 202, 255), true);
        {
            Path zz = make_zigzag_path(cx + 38.0f, cy + 96.0f, 418.0f, 44.0f, 5);
            Paint ghost_p(Color::from_rgba8(110, 130, 170, 120));
            ghost_p.set_path_effect(PathEffect::dash({4.0f, 4.0f}));
            backend.stroke_path(zz, ghost_p, Stroke(1.25f));

            Paint cp(Color::from_rgba8(68, 236, 168, 255));
            cp.set_path_effect(PathEffect::corner(14.0f));
            backend.stroke_path(zz, cp, make_stroke(4.0f, LineCap::Round, LineJoin::Round));
        }

        // Row 2: Trim(0.10, 0.78) & Discrete(7, 3.2)
        draw_well(cx + 18.0f, cy + 164.0f, 222.0f, 114.0f);
        draw_label(cx + 28.0f, cy + 171.0f, "PathEffect::trim(0.10, 0.78)", 11.0f,
                   Color::from_rgba8(146, 246, 202, 255), true);
        {
            Path track = *PathBuilder::from_circle(cx + 129.0f, cy + 232.0f, 34.0f);
            backend.stroke_path(track, Paint(Color::from_rgba8(44, 58, 86, 180)), Stroke(6.0f));
            Paint tp(Color::from_rgba8(56, 224, 255, 255));
            tp.set_path_effect(PathEffect::trim(0.10f, 0.78f));
            backend.stroke_path(track, tp, make_stroke(6.5f, LineCap::Round));
        }

        draw_well(cx + 254.0f, cy + 164.0f, 222.0f, 114.0f);
        draw_label(cx + 264.0f, cy + 171.0f, "PathEffect::discrete(7, 3.2)", 11.0f,
                   Color::from_rgba8(146, 246, 202, 255), true);
        {
            Path wave = make_wave_path(cx + 272.0f, cy + 232.0f, 186.0f, 22.0f, 24);
            Paint dp(Color::from_rgba8(255, 196, 72, 255));
            dp.set_path_effect(PathEffect::discrete(7.0f, 3.2f, 42));
            backend.stroke_path(wave, dp, make_stroke(2.8f, LineCap::Round, LineJoin::Round));
        }

        // Row 3: Path1D (Rotate) diamond chevrons along sine curve
        draw_well(cx + 18.0f, cy + 288.0f, 458.0f, 88.0f);
        draw_label(cx + 28.0f, cy + 295.0f, "PathEffect::path_1d (Diamond Stamp, Path1DStyle::Rotate)", 11.2f,
                   Color::from_rgba8(146, 246, 202, 255), true);
        {
            Path wave = make_wave_path(cx + 42.0f, cy + 342.0f, 410.0f, 18.0f, 36);
            backend.stroke_path(wave, Paint(Color::from_rgba8(56, 78, 118, 140)), Stroke(1.25f));
            Paint p1d(Color::from_rgba8(186, 134, 255, 255));
            p1d.set_path_effect(PathEffect::path_1d(make_diamond_stamp(7.5f, 4.5f), 20.0f, 0.0f, Path1DStyle::Rotate));
            backend.stroke_path(wave, p1d, Stroke(2.0f));
        }

        // Row 4: Path2D (2D Lattice Gems) & Sum(Dash, Discrete)
        draw_well(cx + 18.0f, cy + 386.0f, 222.0f, 92.0f);
        draw_label(cx + 28.0f, cy + 393.0f, "PathEffect::path_2d Lattice", 11.0f,
                   Color::from_rgba8(146, 246, 202, 255), true);
        {
            Path badge = *PathBuilder::from_rounded_rect(*Rect::from_xywh(cx + 34.0f, cy + 416.0f, 190.0f, 50.0f), 12.0f, 12.0f);
            Path dot = make_diamond_stamp(0.28f, 0.28f);
            Transform lattice = Transform::from_scale(13.0f, 13.0f).pre_rotate(22.0f);
            Paint p2d(Color::from_rgba8(72, 236, 178, 255));
            p2d.set_path_effect(PathEffect::path_2d(lattice, dot));
            backend.fill_path(badge, p2d);
            backend.stroke_path(badge, Paint(Color::from_rgba8(72, 236, 178, 190)), Stroke(1.5f));
        }

        draw_well(cx + 254.0f, cy + 386.0f, 222.0f, 92.0f);
        draw_label(cx + 264.0f, cy + 393.0f, "PathEffect::sum(Dash+Disc)", 11.0f,
                   Color::from_rgba8(146, 246, 202, 255), true);
        {
            Path oval = *PathBuilder::from_oval(*Rect::from_xywh(cx + 278.0f, cy + 418.0f, 174.0f, 46.0f));
            auto dual = PathEffect::sum(
                PathEffect::dash({12.0f, 8.0f}),
                PathEffect::discrete(9.0f, 2.5f, 99)
            );
            Paint sp(Color::from_rgba8(255, 118, 174, 255));
            sp.set_path_effect(dual);
            backend.stroke_path(oval, sp, Stroke(2.2f));
        }
    }

    // =========================================================================
    // CARD 6 (Bottom-Right): Phase 4 — 3D Surface Lighting, Blender & ShaderMask
    // =========================================================================
    {
        const float cx = kColX[2], cy = kRowY[1];
        draw_card(cx, cy, kCardW, kCardH, "3D Lighting, Blender & ShaderMask", "PHASE 4.1-4.2",
                  Color::from_rgba8(98, 168, 255, 255));

        // Top well: 3 Sculpted 3D Surface Lighting Domes (Distant, Point Specular, Spot)
        draw_well(cx + 18.0f, cy + 62.0f, 458.0f, 204.0f);
        draw_label(cx + 28.0f, cy + 70.0f, "3D Bump Surface Lighting (Alpha Heightmap -> Normals)", 11.2f,
                   Color::from_rgba8(162, 206, 255, 255), true);
        {
            const float tile_y = cy + 96.0f;
            const float tile_w = 136.0f;
            const float tile_h = 134.0f;
            const float tile_x[3] = {cx + 28.0f, cx + 179.0f, cx + 330.0f};
            const float ly = tile_y + tile_h * 0.5f;

            // 1. Distant Diffuse (Golden sculptural hemisphere)
            {
                const Rect tile = *Rect::from_xywh(tile_x[0], tile_y, tile_w, tile_h);
                const float lx = tile_x[0] + tile_w * 0.5f;
                auto height = ImageFilter::blur(10.0f);
                auto lit = ImageFilter::distant_lit_diffuse(
                    Point3{-0.55f, -0.55f, 0.62f},
                    Color::from_rgba8(255, 206, 112, 255),
                    8.0f, 1.25f, height, tile);
                Paint p(Color::WHITE);
                p.set_image_filter(lit);
                backend.save();
                backend.clip_rrect(RRect::from_rect_and_radius(tile, 10.0f, 10.0f));
                backend.fill_circle(lx, ly, 34.0f, p);
                backend.restore();
                backend.stroke_round_rect(tile, 10.0f, 10.0f, Paint(Color::from_rgba8(255, 206, 112, 115)), Stroke(1.0f));
                draw_label(tile_x[0] + 16.0f, cy + 240.0f, "Distant Diffuse", 10.5f,
                           Color::from_rgba8(190, 208, 236, 255), true);
            }

            // 2. Point Specular (Chrome highlight dome)
            {
                const Rect tile = *Rect::from_xywh(tile_x[1], tile_y, tile_w, tile_h);
                const float lx = tile_x[1] + tile_w * 0.5f;
                auto height = ImageFilter::blur(10.0f);
                auto lit = ImageFilter::point_lit_specular(
                    Point3{lx - 20.0f, ly - 22.0f, 88.0f},
                    Color::from_rgba8(112, 232, 255, 255),
                    9.0f, 1.4f, 18.0f, height, tile);
                Paint p(Color::WHITE);
                p.set_image_filter(lit);
                backend.save();
                backend.clip_rrect(RRect::from_rect_and_radius(tile, 10.0f, 10.0f));
                backend.fill_circle(lx, ly, 34.0f, p);
                backend.restore();
                backend.stroke_round_rect(tile, 10.0f, 10.0f, Paint(Color::from_rgba8(112, 232, 255, 115)), Stroke(1.0f));
                draw_label(tile_x[1] + 20.0f, cy + 240.0f, "Point Specular", 10.5f,
                           Color::from_rgba8(190, 208, 236, 255), true);
            }

            // 3. Spot Diffuse (Dramatic studio spotlight cone)
            {
                const Rect tile = *Rect::from_xywh(tile_x[2], tile_y, tile_w, tile_h);
                const float lx = tile_x[2] + tile_w * 0.5f;
                auto height = ImageFilter::blur(10.0f);
                auto lit = ImageFilter::spot_lit_diffuse(
                    Point3{lx - 38.0f, ly - 44.0f, 96.0f},
                    Point3{lx + 4.0f, ly + 6.0f, 0.0f},
                    2.5f, 44.0f,
                    Color::from_rgba8(168, 126, 255, 255),
                    8.5f, 1.4f, height, tile);
                Paint p(Color::WHITE);
                p.set_image_filter(lit);
                backend.save();
                backend.clip_rrect(RRect::from_rect_and_radius(tile, 10.0f, 10.0f));
                backend.fill_circle(lx, ly, 34.0f, p);
                backend.restore();
                backend.stroke_round_rect(tile, 10.0f, 10.0f, Paint(Color::from_rgba8(168, 126, 255, 115)), Stroke(1.0f));
                draw_label(tile_x[2] + 24.0f, cy + 240.0f, "Spot Diffuse", 10.5f,
                           Color::from_rgba8(190, 208, 236, 255), true);
            }
        }

        // Bottom Row (3 wells): Arithmetic Blender, Runtime GLSL Blender, ShaderMaskFilter
        // 1. Arithmetic Blender
        draw_well(cx + 18.0f, cy + 278.0f, 144.0f, 200.0f);
        draw_label(cx + 26.0f, cy + 286.0f, "Blender::arith", 10.8f,
                   Color::from_rgba8(162, 206, 255, 255), true);
        {
            const float bx = cx + 74.0f, by = cy + 378.0f;
            backend.fill_circle(bx, by, 34.0f, Paint(Color::from_rgba8(48, 188, 255, 255)));
            Paint bp(Color::from_rgba8(255, 86, 158, 255));
            bp.set_blender(Blender::arithmetic(0.85f, 0.75f, 0.75f, 0.0f));
            backend.fill_circle(bx + 32.0f, by, 34.0f, bp);
            draw_label(cx + 26.0f, cy + 448.0f, "k1*s*d + k2*s + k3*d", 9.6f,
                       Color::from_rgba8(166, 186, 216, 255), true);
        }

        // 2. Runtime GLSL Blender
        draw_well(cx + 175.0f, cy + 278.0f, 144.0f, 200.0f);
        draw_label(cx + 183.0f, cy + 286.0f, "Blender::runtime", 10.8f,
                   Color::from_rgba8(162, 206, 255, 255), true);
        {
            const std::string blend_src = R"(
                uniform float boost;
                vec4 main(vec4 src, vec4 dst) {
                    vec3 overlap = abs(src.rgb - dst.rgb) * boost + (src.rgb * dst.rgb) * 1.4;
                    float a = src.a + dst.a * (1.0 - src.a);
                    return vec4(clamp(overlap, vec3(0.0), vec3(a)), a);
                }
            )";
            RuntimeFilterUniforms u;
            u.set_float("boost", 0.95f);
            const float bx = cx + 231.0f, by = cy + 378.0f;
            backend.fill_circle(bx, by, 34.0f, Paint(Color::from_rgba8(72, 236, 182, 255)));
            Paint bp(Color::from_rgba8(178, 102, 255, 255));
            bp.set_blender(Blender::runtime(blend_src, u));
            backend.fill_circle(bx + 32.0f, by, 34.0f, bp);
            draw_label(cx + 185.0f, cy + 448.0f, "main(src, dst)", 9.8f,
                       Color::from_rgba8(166, 186, 216, 255), true);
        }

        // 3. ShaderMaskFilter
        draw_well(cx + 332.0f, cy + 278.0f, 144.0f, 200.0f);
        draw_label(cx + 340.0f, cy + 286.0f, "MaskFilter::shader", 10.5f,
                   Color::from_rgba8(162, 206, 255, 255), true);
        {
            const float mx = cx + 404.0f, my = cy + 378.0f;
            auto mask_grad = RadialGradient::create(
                Point::from_xy(mx, my), 54.0f,
                {
                    GradientStop::create(0.0f, Color::from_rgba8(255, 255, 255, 255)),
                    GradientStop::create(0.55f, Color::from_rgba8(255, 255, 255, 210)),
                    GradientStop::create(1.0f, Color::from_rgba8(255, 255, 255, 0)),
                }
            );
            auto stripes = LinearGradient::create(
                Point::from_xy(mx - 48.0f, my - 48.0f),
                Point::from_xy(mx + 48.0f, my + 48.0f),
                {
                    GradientStop::create(0.0f, Color::from_rgba8(56, 224, 255, 255)),
                    GradientStop::create(0.35f, Color::from_rgba8(255, 198, 66, 255)),
                    GradientStop::create(0.70f, Color::from_rgba8(255, 92, 168, 255)),
                    GradientStop::create(1.0f, Color::from_rgba8(146, 118, 255, 255)),
                }
            );
            Paint mp = stripes ? Paint{Shader{*stripes}} : Paint(Color::from_rgba8(56, 224, 255, 255));
            if (mask_grad) {
                mp.set_mask_filter(MaskFilter::shader(Shader{*mask_grad}));
            }
            backend.fill_round_rect(*Rect::from_xywh(mx - 50.0f, my - 50.0f, 100.0f, 100.0f), 12.0f, 12.0f, mp);
            draw_label(cx + 344.0f, cy + 448.0f, "Radial Alpha Mask", 9.6f,
                       Color::from_rgba8(166, 186, 216, 255), true);
        }
    }
}

} // namespace

int main() {
    constexpr uint32_t W = 1600;
    constexpr uint32_t H = 1180;

    FontSystem fs;
    fs.set_auto_system_fonts(false);
    auto id_var_en = fs.load_font_file(resolve_font("InterVariable.ttf"), 0, "InterVar");
    auto id_var_ar = fs.load_font_file(resolve_font("NotoSansArabic.ttf"), 0, "ArabicVar");
    auto id_mono = fs.load_font_file(resolve_font("FiraMono-Medium.ttf"), 0, "Mono");
    if (!id_var_en || !id_var_ar || !id_mono) {
        std::cerr << "Failed to load bundled fonts from fonts/ directory." << std::endl;
        return 1;
    }
    fs.set_default_font(*id_var_en);

    ParagraphCache pcache(256);

    // -------------------------------------------------------------------------
    // 1. CPU Render (Canvas)
    // -------------------------------------------------------------------------
    auto cpu_pm = Pixmap::create(W, H);
    if (!cpu_pm) {
        std::cerr << "Failed to allocate CPU Pixmap." << std::endl;
        return 1;
    }
    {
        Canvas cpu_canvas(*cpu_pm);
        GlyphCache gcache(2048);
        render_effects_showcase(
            cpu_canvas,
            [&](Buffer& b, float x, float y, Color c) { b.draw(cpu_canvas, gcache, fs, c, x, y); },
            fs, pcache, W, H
        );
    }
    const std::string cpu_out = "nisaba_effects_showcase_cpu.png";
    cpu_pm->save_png(cpu_out);
    std::cout << "[CPU]    Saved " << cpu_out << " (" << W << "x" << H << ")" << std::endl;

#ifdef NISABA_HAS_GPU
    // -------------------------------------------------------------------------
    // 2. OpenGL GPU Render (GpuCanvas) + Live CPU-vs-OpenGL Parity MAE
    // -------------------------------------------------------------------------
    if (auto gl_dev = gpu::GpuDevice::create(gpu::GpuBackendType::OpenGL)) {
        if (auto surface = gpu::GpuSurface::create(gl_dev, W, H)) {
            gpu::GpuCanvas gl_canvas(surface);
            render_effects_showcase(
                gl_canvas,
                [&](Buffer& b, float x, float y, Color c) { gl_canvas.draw_text_buffer(b, fs, Point::from_xy(x, y), c); },
                fs, pcache, W, H
            );
            gl_canvas.flush();
            if (auto gl_pm = surface->to_pixmap()) {
                const std::string gl_out = "nisaba_effects_showcase_gl.png";
                gl_pm->save_png(gl_out);
                const double mae = compute_mae(*cpu_pm, *gl_pm);
                std::cout << "[OpenGL] Saved " << gl_out << " | Device: " << gl_dev->renderer_name()
                          << " | CPU-vs-GL MAE = " << std::fixed << std::setprecision(3) << mae << " / 255" << std::endl;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 3. Vulkan GPU Render (GpuCanvas) + Live CPU-vs-Vulkan Parity MAE
    // -------------------------------------------------------------------------
    if (auto vk_dev = gpu::GpuDevice::create(gpu::GpuBackendType::Vulkan)) {
        if (auto surface = gpu::GpuSurface::create(vk_dev, W, H)) {
            gpu::GpuCanvas vk_canvas(surface);
            render_effects_showcase(
                vk_canvas,
                [&](Buffer& b, float x, float y, Color c) { vk_canvas.draw_text_buffer(b, fs, Point::from_xy(x, y), c); },
                fs, pcache, W, H
            );
            vk_canvas.flush();
            if (auto vk_pm = surface->to_pixmap()) {
                const std::string vk_out = "nisaba_effects_showcase_vk.png";
                vk_pm->save_png(vk_out);
                const double mae = compute_mae(*cpu_pm, *vk_pm);
                std::cout << "[Vulkan] Saved " << vk_out << " | Device: " << vk_dev->renderer_name()
                          << " | CPU-vs-VK MAE = " << std::fixed << std::setprecision(3) << mae << " / 255" << std::endl;
            }
        }
    }
#endif

    return 0;
}
