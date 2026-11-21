/// @file showcase_lottie_gpu.cpp
/// @brief 100% Hardware GPU Accelerated Interactive Bento Showcase of Nisaba Lottie Engine (Phases 1-4).

#include "nisaba/nisaba.hpp"
#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "lottie_showcase_scenes.hpp"

#ifdef NISABA_HAS_BACKEND_OS
#  include "nisaba/backend_os/platform.hpp"
#  include "nisaba/backend_os/window.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace nisaba;
using namespace nisaba::lottie;
using namespace nisaba::effects;
using namespace nisaba::showcase;
using nisaba::gpu::GpuBackendType;
using nisaba::gpu::GpuCanvas;
using nisaba::gpu::GpuDevice;
using nisaba::gpu::GpuSurface;

namespace {

constexpr uint32_t WIN_WIDTH = 1340;
constexpr uint32_t WIN_HEIGHT = 780;

// 3x2 Bento Grid geometry
constexpr float MARGIN_X = 36.0f;
constexpr float GAP_X = 16.0f;
constexpr float CARD_W = 412.0f;
constexpr float CARD_H = 290.0f;
constexpr float ROW0_Y = 102.0f;
constexpr float ROW1_Y = 408.0f;

static const char* resolveFont(const char* name) {
    static char resolvedPaths[5][512];
    static int nextIdx = 0;
    char* outBuf = resolvedPaths[nextIdx++ % 5];

    const char* prefixes[] = {
        "fonts/",
        "nisaba/fonts/",
        "../fonts/",
        "../../fonts/",
        "../../../fonts/"
    };
    for (const auto& prefix : prefixes) {
        std::snprintf(outBuf, 512, "%s%s", prefix, name);
        FILE* f = std::fopen(outBuf, "rb");
        if (f) {
            std::fclose(f);
            return outBuf;
        }
    }
    return name;
}

} // namespace

int main(int argc, char** argv) {
    bool run_headless = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--headless") {
            run_headless = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: showcase_lottie_gpu [options]\n\n"
                      << "Options:\n"
                      << "  --headless   Render 60 frames offscreen and save PNG snapshot\n"
                      << "  --help, -h   Display this help message\n\n"
                      << "Interactive Controls:\n"
                      << "  [SPACE]      Play / Pause all 6 scenes\n"
                      << "  [R]          Restart all scenes from frame 0\n"
                      << "  [S]          Cycle speed (0.25x / 0.5x / 1.0x / 2.0x)\n"
                      << "  [D]          Toggle Phase 1 InvalidationController dirty-rect overlay\n"
                      << "  [C] / [1]    Mutate Card 1 color & scale in O(1) via PropertyObserver handles\n"
                      << "  [E] / [3]    Cycle Card 3 Color & Tone FX (Ramp+HueSat / 4-Color / CC Toner / B&W+Invert)\n"
                      << "  [W] / [4]    Cycle Card 4 Transition & Tiling FX (LinearWipe / Blinds / MotionTile / TransformFX)\n"
                      << "  [F] / [5]    Cycle Card 5 Distortion/Blur/Noise FX (Bulge / Sphere / CornerPin / DirBlur / FractalNoise)\n"
                      << "  [CLICK]      Click any card to interact / cycle its live mode\n"
                      << "  [ESC] / [Q]  Exit showcase\n";
            return 0;
        }
    }

    std::cout << "====================================================================\n"
              << "   NISABA LOTTIE FULL-CAPABILITY GPU SHOWCASE (PHASES 1 - 4)\n"
              << "====================================================================\n";

#ifdef NISABA_HAS_BACKEND_OS
    std::unique_ptr<backend_os::Platform> platform;
    std::unique_ptr<backend_os::Window> window;
    bool g_running = true;

    if (!run_headless) {
        auto p_res = backend_os::Platform::create();
        if (p_res.isOk()) {
            platform = std::move(p_res).value();
            backend_os::WindowConfig cfg;
            cfg.title = "NISABA LOTTIE GPU SHOWCASE — Phases 1-4 Live Interactive Lab";
            cfg.width = WIN_WIDTH;
            cfg.height = WIN_HEIGHT;
            cfg.vsync = true;
            auto w_res = backend_os::Window::create(*platform, cfg);
            if (w_res.isOk()) {
                window = std::move(w_res).value();
                std::cout << "[+] Native OS GUI window created (" << WIN_WIDTH << "x" << WIN_HEIGHT << ").\n";
            }
        }
    }
#endif

    auto device = run_headless ? GpuDevice::create_headless() : GpuDevice::create();
    if (!device || !device->is_valid()) {
        std::cerr << "[-] Error: Failed to initialize GPU device.\n";
        return 1;
    }

    std::cout << "[*] GPU Backend : " << (device->backend_type() == GpuBackendType::Vulkan ? "Vulkan" : "OpenGL 4.6 / GLES 3.2") << "\n";
    std::cout << "[*] GPU Renderer: " << device->renderer_name() << "\n";

    std::shared_ptr<GpuSurface> surface = run_headless
        ? GpuSurface::create(device, WIN_WIDTH, WIN_HEIGHT, 4)
        : GpuSurface::from_screen(device, WIN_WIDTH, WIN_HEIGHT, 0);
    if (!surface) {
        std::cerr << "[-] Error: Failed to create GpuSurface.\n";
        return 1;
    }
    GpuCanvas gpu_canvas(surface);

    // Load vector font system for Lottie TextLayer (Phase 4 Text-on-Path)
    auto lottie_fonts = std::make_shared<text::FontSystem>();
    lottie_fonts->load_font_file(resolveFont("Inter-Regular.ttf"));
    lottie_fonts->load_font_file(resolveFont("FiraMono-Medium.ttf"));

    ShowcaseDeck deck;
    if (!deck.init(lottie_fonts)) {
        std::cerr << "[-] Error: Failed initializing Lottie showcase deck.\n";
        return 1;
    }

    std::cout << "[+] Loaded all 6 Phase 1-4 scenes (Phase 1 parse=" << std::fixed << std::setprecision(2)
              << deck.p1_stats.json_parse_ms << "ms, build=" << deck.p1_stats.scene_build_ms
              << "ms, animators=" << deck.p1_stats.animator_count << ").\n";

    // GPU HUD Typography
    if (gpu_canvas.context()) {
        int f_sans = gpu_canvas.context()->createFont("sans", resolveFont("Inter-Regular.ttf"));
        if (f_sans == -1) f_sans = gpu_canvas.context()->createFont("sans", resolveFont("NotoSans-Regular.ttf"));
        int f_mono = gpu_canvas.context()->createFont("mono", resolveFont("FiraMono-Medium.ttf"));
        if (f_mono == -1) (void)gpu_canvas.context()->createFont("mono", resolveFont("Inter-Regular.ttf"));
        gpu_canvas.context()->fontFace("sans");
    }

    auto draw_text = [&](float x, float y, std::string_view txt, float sz, Color col, const char* face = "sans") {
        if (!gpu_canvas.context()) return;
        gpu_canvas.context()->fontFace(face);
        gpu_canvas.context()->fontSize(sz);
        gpu_canvas.context()->fillColor(col);
        gpu_canvas.context()->textAlign(gpu::Align::Left | gpu::Align::Top);
        gpu_canvas.context()->text(x, y, txt.data(), txt.data() + txt.size());
    };

#ifdef NISABA_HAS_BACKEND_OS
    if (platform && window) {
        window->onClose().connect([&]() { g_running = false; });

        platform->onKeyDown().connect([&](int key, int) {
            if (key == 27 || key == 'q' || key == 'Q') g_running = false;
            else if (key == 32) deck.toggle_pause();
            else if (key == 'r' || key == 'R') deck.restart_all();
            else if (key == 's' || key == 'S') deck.cycle_speed();
            else if (key == 'd' || key == 'D') deck.show_damage_overlay = !deck.show_damage_overlay;
            else if (key == 'c' || key == 'C' || key == '1') deck.cycle_p1_handle_mutation();
            else if (key == '2') { deck.p2.seek_frame(0.0f); if (!deck.paused) deck.p2.play(); }
            else if (key == 'e' || key == 'E' || key == '3') deck.reload_color_fx(deck.color_fx_variant + 1);
            else if (key == 'w' || key == 'W' || key == '4') deck.reload_wipe_fx(deck.wipe_fx_variant + 1);
            else if (key == 'f' || key == 'F' || key == '5') deck.reload_distort_fx(deck.distort_fx_variant + 1);
            else if (key == '6') { deck.p6.seek_frame(0.0f); if (!deck.paused) deck.p6.play(); }
        });

        platform->onMouseDown().connect([&](float mx, float my, int) {
            int row = -1;
            if (my >= ROW0_Y && my <= ROW0_Y + CARD_H) row = 0;
            else if (my >= ROW1_Y && my <= ROW1_Y + CARD_H) row = 1;
            if (row < 0) return;

            int col = -1;
            for (int c = 0; c < 3; ++c) {
                const float x0 = MARGIN_X + static_cast<float>(c) * (CARD_W + GAP_X);
                if (mx >= x0 && mx <= x0 + CARD_W) { col = c; break; }
            }
            if (col < 0) return;

            const int card_idx = row * 3 + col;
            if (card_idx == 0) deck.cycle_p1_handle_mutation();
            else if (card_idx == 1) { deck.p2.seek_frame(0.0f); if (!deck.paused) deck.p2.play(); }
            else if (card_idx == 2) deck.reload_color_fx(deck.color_fx_variant + 1);
            else if (card_idx == 3) deck.reload_wipe_fx(deck.wipe_fx_variant + 1);
            else if (card_idx == 4) deck.reload_distort_fx(deck.distort_fx_variant + 1);
            else if (card_idx == 5) { deck.p6.seek_frame(0.0f); if (!deck.paused) deck.p6.play(); }
        });
    }
#endif

    double last_time = 0.0;
    auto start_tp = std::chrono::high_resolution_clock::now();
    uint32_t fps_frames = 0;
    double fps_accum = 0.0;
    double measured_fps = 60.0;
    double measured_ms = 2.0;

    const uint32_t max_frames = run_headless ? 42 : 10000000;

    for (uint32_t f = 0; f < max_frames; ++f) {
        auto t0 = std::chrono::high_resolution_clock::now();
#ifdef NISABA_HAS_BACKEND_OS
        if (platform && window) {
            if (!platform->pollEvents() || !g_running) break;
        }
#endif
        const double now_sec = std::chrono::duration<double>(t0 - start_tp).count();
        double dt = run_headless ? (1.0 / 60.0) : ((last_time > 0.0) ? (now_sec - last_time) : (1.0 / 60.0));
        last_time = now_sec;
        if (dt > 0.05) dt = 0.05;

        deck.advance_all(static_cast<float>(dt));

        // 1. Background cosmic gradient + soft nebulae
        gpu_canvas.clear(Color::from_rgba8(8, 11, 20, 255));
        if (auto full_r = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(WIN_WIDTH), static_cast<float>(WIN_HEIGHT))) {
            if (auto bg = LinearGradient::create(
                    Point::from_xy(0.0f, 0.0f), Point::from_xy(WIN_WIDTH, WIN_HEIGHT),
                    {GradientStop::create(0.0f, Color::from_rgba8(8, 12, 24, 255)),
                     GradientStop::create(0.5f, Color::from_rgba8(15, 20, 38, 255)),
                     GradientStop::create(1.0f, Color::from_rgba8(7, 10, 18, 255))})) {
                gpu_canvas.fill_rect(*full_r, Paint(Shader(*bg)));
            }
        }
        if (auto neb1 = RadialGradient::create(Point::from_xy(260.0f, 140.0f), 360.0f, {
                GradientStop::create(0.0f, Color::from_rgba8(0, 220, 255, 32)),
                GradientStop::create(1.0f, Color::from_rgba8(0, 220, 255, 0))})) {
            gpu_canvas.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 650.0f, 520.0f), Paint(Shader(*neb1)));
        }
        if (auto neb2 = RadialGradient::create(Point::from_xy(1080.0f, 520.0f), 380.0f, {
                GradientStop::create(0.0f, Color::from_rgba8(180, 60, 255, 30)),
                GradientStop::create(1.0f, Color::from_rgba8(180, 60, 255, 0))})) {
            gpu_canvas.fill_rect(*Rect::from_xywh(680.0f, 160.0f, 660.0f, 620.0f), Paint(Shader(*neb2)));
        }

        // 2. Top Header Bar
        if (auto hr = Rect::from_xywh(MARGIN_X, 18.0f, static_cast<float>(WIN_WIDTH) - MARGIN_X * 2.0f, 68.0f)) {
            gpu_canvas.draw_round_rect_shadow(*hr, 14.0f, 14.0f, DropShadow(0.0f, 5.0f, 12.0f, Color::from_rgba8(0, 0, 0, 140)));
            GlassParams hg;
            hg.tint_color = Color::from_rgba8(14, 20, 36, 215);
            hg.border_color = Color::from_rgba8(0, 235, 255, 95);
            hg.border_width = 1.2f;
            gpu_canvas.draw_glass_panel(*hr, 14.0f, 14.0f, hg);
        }
        draw_text(54.0f, 30.0f, "NISABA LOTTIE ENGINE — LIVE GPU FEATURE LAB (PHASES 1-4)", 19.5f, Color::from_rgba8(245, 250, 255, 255), "sans");
        draw_text(54.0f, 56.0f, "Damage Tracking • Multi-frame Assets • O(1) Handles • External Precomp • 20+ Effects • Expressions • Text-on-Path • Audio", 11.0f, Color::from_rgba8(145, 175, 215, 255), "sans");

        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << measured_fps << " FPS | "
               << std::setprecision(2) << measured_ms << " ms | " << deck.speed << "x | "
               << (deck.paused ? "PAUSED" : "LIVE GPU");
            if (auto br = Rect::from_xywh(968.0f, 32.0f, 320.0f, 38.0f)) {
                GlassParams bg;
                bg.tint_color = Color::from_rgba8(0, 225, 255, 28);
                bg.border_color = Color::from_rgba8(0, 235, 255, 155);
                bg.border_width = 1.0f;
                gpu_canvas.draw_glass_panel(*br, 10.0f, 10.0f, bg);
            }
            draw_text(984.0f, 44.0f, ss.str(), 11.5f, Color::from_rgba8(80, 245, 255, 255), "mono");
        }

        // 3. Helper to draw each Bento Card + Live Lottie Stage
        auto draw_card = [&](int row, int col, std::string_view tag, std::string_view title,
                             std::string_view subtitle, Player& player, Color accent,
                             std::function<void(const Rect&, const Rect&)> post_overlay = nullptr) {
            const float cx = MARGIN_X + static_cast<float>(col) * (CARD_W + GAP_X);
            const float cy = (row == 0) ? ROW0_Y : ROW1_Y;
            const auto card_r = *Rect::from_xywh(cx, cy, CARD_W, CARD_H);

            gpu_canvas.draw_round_rect_shadow(card_r, 14.0f, 14.0f, DropShadow(0.0f, 6.0f, 14.0f, Color::from_rgba8(0, 0, 0, 150)));
            GlassParams cg;
            cg.tint_color = Color::from_rgba8(14, 19, 34, 215);
            cg.border_color = Color::from_rgba_unchecked(accent.red(), accent.green(), accent.blue(), 0.42f);
            cg.border_width = 1.2f;
            gpu_canvas.draw_glass_panel(card_r, 14.0f, 14.0f, cg);

            draw_text(cx + 16.0f, cy + 12.0f, tag, 10.5f, accent, "mono");
            draw_text(cx + 16.0f, cy + 28.0f, title, 14.5f, Color::from_rgba8(242, 247, 255, 255), "sans");
            draw_text(cx + 16.0f, cy + 48.0f, subtitle, 10.5f, Color::from_rgba8(145, 168, 200, 255), "sans");

            const auto stage_r = *Rect::from_xywh(cx + 16.0f, cy + 68.0f, CARD_W - 32.0f, 186.0f);
            gpu_canvas.fill_round_rect(stage_r, 10.0f, 10.0f, Paint(Color::from_rgba8(8, 11, 20, 240)));
            gpu_canvas.stroke_round_rect(stage_r, 10.0f, 10.0f, Paint(Color::from_rgba8(42, 58, 86, 140)), Stroke(1.2f));

            const auto lottie_r = *Rect::from_xywh(stage_r.left() + 6.0f, stage_r.top() + 6.0f,
                                                   stage_r.width() - 12.0f, stage_r.height() - 12.0f);
            gpu_canvas.save();
            gpu_canvas.clip_rect(stage_r);
            player.render(gpu_canvas, lottie_r, true);
            if (post_overlay) {
                post_overlay(stage_r, lottie_r);
            }
            gpu_canvas.restore();

            // Progress bar & frame readout
            const float prog = player.progress();
            const float bar_y = cy + 262.0f;
            const float bar_w = CARD_W - 32.0f;
            gpu_canvas.fill_round_rect(*Rect::from_xywh(cx + 16.0f, bar_y, bar_w, 4.5f), 2.2f, 2.2f,
                                       Paint(Color::from_rgba8(22, 30, 50, 255)));
            if (prog > 0.002f) {
                gpu_canvas.fill_round_rect(*Rect::from_xywh(cx + 16.0f, bar_y, bar_w * prog, 4.5f), 2.2f, 2.2f,
                                           Paint(accent));
            }
            std::ostringstream fi;
            fi << "F " << std::fixed << std::setprecision(0) << player.current_frame()
               << "/" << (player.animation() ? player.animation()->out_point() : 120.0f);
            draw_text(cx + CARD_W - 76.0f, cy + 12.0f, fi.str(), 10.0f, Color::from_rgba8(130, 155, 190, 255), "mono");
        };

        // Card 1: Phase 1 — Damage + Handles + Multi-frame Asset
        {
            std::ostringstream sub;
            sub << "Click/[C] O(1) Handle • [D] DirtyBox=" << (deck.show_damage_overlay ? "ON" : "OFF")
                << " • " << std::fixed << std::setprecision(2) << deck.p1_stats.scene_build_ms << "ms";
            draw_card(0, 0, "01 // PHASE 1: DAMAGE + HANDLES + GIF", "INVALIDATION & O(1) HANDLES",
                      sub.str(), deck.p1, Color::from_rgba8(0, 232, 255, 255),
                      [&](const Rect&, const Rect& lr) {
                          if (!deck.show_damage_overlay || !deck.p1_last_dirty.has_value()) return;
                          // Map composition 240x160 dirty rect into aspect-fitted lottie_r
                          const float sx = lr.width() / 240.0f;
                          const float sy = lr.height() / 160.0f;
                          const float sc = std::min(sx, sy);
                          const float ox = lr.left() + (lr.width() - 240.0f * sc) * 0.5f;
                          const float oy = lr.top() + (lr.height() - 160.0f * sc) * 0.5f;
                          const Rect& d = *deck.p1_last_dirty;
                          if (auto dr = Rect::from_ltrb(ox + d.left() * sc, oy + d.top() * sc,
                                                        ox + d.right() * sc, oy + d.bottom() * sc)) {
                              Stroke dst(1.8f);
                              dst.dash = StrokeDash::create({6.0f, 4.0f}, 0.0f);
                              gpu_canvas.stroke_rect(*dr, Paint(Color::from_rgba8(0, 255, 190, 240)), dst);
                          }
                      });
        }

        // Card 2: Phase 2.1 — External Precomp Layer
        draw_card(0, 1, "02 // PHASE 2.1: PRECOMP INTERCEPTOR", "LIVE EMBEDDED EXTERNAL WIDGET",
                  "CallbackExternalLayer radar+wave inside animated Lottie hierarchy",
                  deck.p2, Color::from_rgba8(0, 245, 165, 255));

        // Card 3: Phase 2.2 — Color & Tone Effects
        {
            std::string sub = std::string("Active [E]: ") + std::string(COLOR_FX_NAMES[deck.color_fx_variant]);
            draw_card(0, 2, "03 // PHASE 2.2: COLOR & TONE FX", "NATIVE GPU COLOR PIPELINE",
                      sub, deck.p3, Color::from_rgba8(255, 185, 45, 255));
        }

        // Card 4: Phase 3.1 — Transitions & Tiling
        {
            std::string sub = std::string("Active [W]: ") + std::string(WIPE_FX_NAMES[deck.wipe_fx_variant]);
            draw_card(1, 0, "04 // PHASE 3.1: WIPE & TILING FX", "TRANSITIONS & MOTION TILE",
                      sub, deck.p4, Color::from_rgba8(140, 130, 255, 255));
        }

        // Card 5: Phase 3.2 — Distortion, Blur & Procedural
        {
            std::string sub = std::string("Active [F]: ") + std::string(DISTORT_FX_NAMES[deck.distort_fx_variant]);
            draw_card(1, 1, "05 // PHASE 3.2: DISTORT & NOISE", "WARP, BLUR & FRACTAL NOISE",
                      sub, deck.p5, Color::from_rgba8(255, 75, 145, 255));
        }

        // Card 6: Phase 4 — Expressions + Text-on-Path + AudioLayer Live VU Meter
        {
            std::ostringstream sub;
            sub << "ExprMgr + Bezier Text-on-Path • Audio Gain " << std::fixed << std::setprecision(2) << deck.audio_meter.gain;
            draw_card(1, 2, "06 // PHASE 4: EXPR + TEXT + AUDIO", "EXPRESSIONS, PATH TEXT & VU",
                      sub.str(), deck.p6, Color::from_rgba8(55, 215, 255, 255),
                      [&](const Rect& sr, const Rect&) {
                          // Live stereo VU meter driven by AudioLayer ("ty": 6) & AudioTrack!
                          const float l_norm = std::clamp((deck.audio_meter.left_db + 24.0f) / 24.0f, 0.0f, 1.0f);
                          const float r_norm = std::clamp((deck.audio_meter.right_db + 24.0f) / 24.0f, 0.0f, 1.0f);
                          const float vx = sr.left() + 12.0f;
                          const float vw = sr.width() - 24.0f;
                          const float vy = sr.bottom() - 22.0f;
                          gpu_canvas.fill_round_rect(*Rect::from_xywh(vx, vy, vw, 6.0f), 3.0f, 3.0f,
                                                     Paint(Color::from_rgba8(24, 34, 54, 220)));
                          if (l_norm > 0.01f) {
                              gpu_canvas.fill_round_rect(*Rect::from_xywh(vx, vy, vw * l_norm, 6.0f), 3.0f, 3.0f,
                                                         Paint(Color::from_rgba8(40, 240, 165, 240)));
                          }
                          gpu_canvas.fill_round_rect(*Rect::from_xywh(vx, vy + 9.0f, vw, 6.0f), 3.0f, 3.0f,
                                                     Paint(Color::from_rgba8(24, 34, 54, 220)));
                          if (r_norm > 0.01f) {
                              gpu_canvas.fill_round_rect(*Rect::from_xywh(vx, vy + 9.0f, vw * r_norm, 6.0f), 3.0f, 3.0f,
                                                         Paint(Color::from_rgba8(60, 195, 255, 240)));
                          }
                      });
        }

        // 4. Bottom Interactive Control Bar
        if (auto bot = Rect::from_xywh(MARGIN_X, 712.0f, static_cast<float>(WIN_WIDTH) - MARGIN_X * 2.0f, 50.0f)) {
            GlassParams bg;
            bg.tint_color = Color::from_rgba8(12, 17, 30, 215);
            bg.border_color = Color::from_rgba8(70, 95, 140, 80);
            bg.border_width = 1.0f;
            gpu_canvas.draw_glass_panel(*bot, 11.0f, 11.0f, bg);
        }
        draw_text(54.0f, 723.0f,
                  "[SPACE] Pause  |  [R] Restart  |  [S] Speed  |  [D] Dirty-Rect  |  [C/1] Mutate Handle  |  [E/3] Color FX  |  [W/4] Wipe FX  |  [F/5] Distort FX  |  [CLICK] Card  |  [ESC] Exit",
                  11.0f, Color::from_rgba8(188, 210, 240, 255), "sans");

        gpu_canvas.flush();
#ifdef NISABA_HAS_BACKEND_OS
        if (window) window->swapBuffers();
#endif

        auto t1 = std::chrono::high_resolution_clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        measured_ms = measured_ms * 0.9 + frame_ms * 0.1;
        ++fps_frames;
        fps_accum += dt;
        if (fps_accum >= 0.5) {
            measured_fps = static_cast<double>(fps_frames) / fps_accum;
            fps_frames = 0;
            fps_accum = 0.0;
        }
    }

    const std::string out_png = "nisaba_lottie_gpu_showcase.png";
    if (auto snap = surface->to_pixmap(); snap && snap->save_png(out_png)) {
        std::cout << "[+] Saved GPU showcase snapshot to: " << out_png << "\n";
    }
    return 0;
}
