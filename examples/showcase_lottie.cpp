/// @file showcase_lottie.cpp
/// @brief Interactive CPU Software-Rasterized 3x2 Bento Showcase of Nisaba Lottie Engine (Phases 1-4).

#include "nisaba/nisaba.hpp"
#include "lottie_showcase_scenes.hpp"

#ifdef NISABA_HAS_BACKEND_OS
#  include "nisaba/backend_os/platform.hpp"
#  include "nisaba/backend_os/window.hpp"
#  include "nisaba/gpu/context.hpp"
#  include "nisaba/gpu/gl3_renderer.hpp"
#  ifdef NISABA_GLEW
#    include <GL/glew.h>
#  else
#    include <GL/gl.h>
#  endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace nisaba;
using namespace nisaba::lottie;
using namespace nisaba::effects;
using namespace nisaba::text;
using namespace nisaba::vspr;
using namespace nisaba::showcase;

namespace {

constexpr uint32_t WIN_WIDTH = 1340;
constexpr uint32_t WIN_HEIGHT = 780;
constexpr float MARGIN_X = 36.0f;
constexpr float GAP_X = 16.0f;
constexpr float CARD_W = 412.0f;
constexpr float CARD_H = 290.0f;
constexpr float ROW0_Y = 102.0f;
constexpr float ROW1_Y = 408.0f;

std::string resolve_font(std::string_view filename) {
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

int main(int argc, char** argv) {
    bool run_headless = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--headless") {
            run_headless = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: showcase_lottie [--headless]\n"
                      << "Controls: [SPACE] Pause | [R] Restart | [S] Speed | [D] DirtyBox | [C] Handle | [E] ColorFX | [W] WipeFX | [F] DistortFX | [ESC] Exit\n";
            return 0;
        }
    }

    std::cout << "====================================================================\n"
              << "   NISABA LOTTIE FULL-CAPABILITY CPU SHOWCASE (PHASES 1 - 4)\n"
              << "====================================================================\n";

    auto fb = Pixmap::create(WIN_WIDTH, WIN_HEIGHT);
    if (!fb) {
        std::cerr << "[-] Failed to allocate framebuffer.\n";
        return 1;
    }
    Canvas canvas(*fb);

    auto font_system = std::make_shared<FontSystem>();
    font_system->load_font_file(resolve_font("Inter-Regular.ttf"));
    font_system->load_font_file(resolve_font("FiraMono-Medium.ttf"));
    GlyphCache glyph_cache;

    auto draw_text = [&](Canvas& c, float x, float y, std::string_view txt, float sz, float lh, TextColor col) {
        Buffer buf(Metrics(sz, lh));
        Attrs attrs;
        attrs.set_color(col);
        buf.set_text(txt, attrs);
        buf.draw(c, glyph_cache, *font_system, Color::WHITE, x, y);
    };

    // Pre-render cosmic wallpaper + frosted BackdropBlurCache
    auto wallpaper = Pixmap::create(WIN_WIDTH, WIN_HEIGHT);
    {
        Canvas wc(*wallpaper);
        if (auto bg = LinearGradient::create(
                Point::from_xy(0.0f, 0.0f), Point::from_xy(WIN_WIDTH, WIN_HEIGHT),
                {GradientStop::create(0.0f, Color::from_rgba8(8, 12, 24, 255)),
                 GradientStop::create(0.5f, Color::from_rgba8(15, 20, 38, 255)),
                 GradientStop::create(1.0f, Color::from_rgba8(7, 10, 18, 255))})) {
            wc.fill_rect(*Rect::from_xywh(0.0f, 0.0f, WIN_WIDTH, WIN_HEIGHT), Paint(Shader(*bg)));
        }
        if (auto n1 = RadialGradient::create(Point::from_xy(260.0f, 140.0f), 360.0f, {
                GradientStop::create(0.0f, Color::from_rgba8(0, 220, 255, 34)),
                GradientStop::create(1.0f, Color::from_rgba8(0, 220, 255, 0))})) {
            wc.fill_rect(*Rect::from_xywh(0.0f, 0.0f, 650.0f, 520.0f), Paint(Shader(*n1)));
        }
        if (auto n2 = RadialGradient::create(Point::from_xy(1080.0f, 520.0f), 380.0f, {
                GradientStop::create(0.0f, Color::from_rgba8(180, 60, 255, 32)),
                GradientStop::create(1.0f, Color::from_rgba8(180, 60, 255, 0))})) {
            wc.fill_rect(*Rect::from_xywh(680.0f, 160.0f, 660.0f, 620.0f), Paint(Shader(*n2)));
        }
    }
    BackdropBlurCache blur_cache(*wallpaper, 12.0f, true);

    ShowcaseDeck deck;
    if (!deck.init(font_system)) {
        std::cerr << "[-] Failed to initialize showcase deck.\n";
        return 1;
    }

    // Pre-bake static UI chrome (wallpaper + 8 glass panels + card headers + footer) and only re-bake on mode switch!
    auto baked_chrome = Pixmap::create(WIN_WIDTH, WIN_HEIGHT);
    bool chrome_dirty = true;

    auto bake_static_chrome = [&]() {
        std::memcpy(baked_chrome->pixels_mut(), wallpaper->pixels(), wallpaper->data_len());
        Canvas cc(*baked_chrome);

        if (auto hr = Rect::from_xywh(MARGIN_X, 18.0f, static_cast<float>(WIN_WIDTH) - MARGIN_X * 2.0f, 68.0f)) {
            GlassParams hg;
            hg.blur_sigma = 12.0f;
            hg.tint_color = Color::from_rgba8(14, 20, 36, 210);
            hg.border_color = Color::from_rgba8(0, 235, 255, 95);
            hg.border_width = 1.2f;
            blur_cache.draw_glass_aperture(cc, *hr, 14.0f, 14.0f, hg);
        }
        draw_text(cc, 54.0f, 28.0f, "NISABA LOTTIE ENGINE — CPU REFERENCE LAB (PHASES 1-4)", 18.5f, 22.0f, TextColor::rgb(245, 250, 255));
        draw_text(cc, 54.0f, 54.0f, "1:1 Software Parity • Damage Tracking • External Precomp • 20+ Effects • Expressions • Text-on-Path • Audio", 11.0f, 14.0f, TextColor::rgb(145, 175, 215));

        auto bake_card_chrome = [&](int row, int col, std::string_view tag, std::string_view title,
                                    std::string_view subtitle, Color accent) {
            const float cx = MARGIN_X + static_cast<float>(col) * (CARD_W + GAP_X);
            const float cy = (row == 0) ? ROW0_Y : ROW1_Y;
            const auto card_r = *Rect::from_xywh(cx, cy, CARD_W, CARD_H);

            GlassParams cg;
            cg.blur_sigma = 12.0f;
            cg.tint_color = Color::from_rgba8(14, 19, 34, 215);
            cg.border_color = Color::from_rgba_unchecked(accent.red(), accent.green(), accent.blue(), 0.42f);
            cg.border_width = 1.2f;
            blur_cache.draw_glass_aperture(cc, card_r, 14.0f, 14.0f, cg);

            const TextColor acc_tc = TextColor::rgb(
                static_cast<uint8_t>(accent.red() * 255.0f),
                static_cast<uint8_t>(accent.green() * 255.0f),
                static_cast<uint8_t>(accent.blue() * 255.0f));
            draw_text(cc, cx + 16.0f, cy + 10.0f, tag, 10.0f, 13.0f, acc_tc);
            draw_text(cc, cx + 16.0f, cy + 26.0f, title, 14.0f, 18.0f, TextColor::rgb(242, 247, 255));
            draw_text(cc, cx + 16.0f, cy + 46.0f, subtitle, 10.5f, 14.0f, TextColor::rgb(145, 168, 200));

            const auto stage_r = *Rect::from_xywh(cx + 16.0f, cy + 68.0f, CARD_W - 32.0f, 186.0f);
            cc.fill_round_rect(stage_r, 10.0f, 10.0f, Paint(Color::from_rgba8(8, 11, 20, 240)));
            cc.stroke_round_rect(stage_r, 10.0f, 10.0f, Paint(Color::from_rgba8(42, 58, 86, 140)), Stroke(1.2f));
        };

        bake_card_chrome(0, 0, "01 // PHASE 1: DAMAGE + HANDLES + GIF", "INVALIDATION & O(1) HANDLES",
                         deck.show_damage_overlay ? "Click/[C] O(1) Handle • [D] DirtyBox=ON" : "Click/[C] O(1) Handle • [D] DirtyBox=OFF",
                         Color::from_rgba8(0, 232, 255, 255));
        bake_card_chrome(0, 1, "02 // PHASE 2.1: PRECOMP INTERCEPTOR", "LIVE EMBEDDED EXTERNAL WIDGET",
                         "CallbackExternalLayer radar+wave inside Lottie hierarchy", Color::from_rgba8(0, 245, 165, 255));
        bake_card_chrome(0, 2, "03 // PHASE 2.2: COLOR & TONE FX", "NATIVE COLOR PIPELINE",
                         std::string("Active [E]: ") + std::string(COLOR_FX_NAMES[deck.color_fx_variant]),
                         Color::from_rgba8(255, 185, 45, 255));
        bake_card_chrome(1, 0, "04 // PHASE 3.1: WIPE & TILING FX", "TRANSITIONS & MOTION TILE",
                         std::string("Active [W]: ") + std::string(WIPE_FX_NAMES[deck.wipe_fx_variant]),
                         Color::from_rgba8(140, 130, 255, 255));
        bake_card_chrome(1, 1, "05 // PHASE 3.2: DISTORT & NOISE", "WARP, BLUR & FRACTAL NOISE",
                         std::string("Active [F]: ") + std::string(DISTORT_FX_NAMES[deck.distort_fx_variant]),
                         Color::from_rgba8(255, 75, 145, 255));
        bake_card_chrome(1, 2, "06 // PHASE 4: EXPR + TEXT + AUDIO", "EXPRESSIONS, PATH TEXT & VU",
                         "ExprMgr + Bezier Text-on-Path + Live AudioTrack VU", Color::from_rgba8(55, 215, 255, 255));

        if (auto bot = Rect::from_xywh(MARGIN_X, 712.0f, static_cast<float>(WIN_WIDTH) - MARGIN_X * 2.0f, 50.0f)) {
            GlassParams bg;
            bg.blur_sigma = 12.0f;
            bg.tint_color = Color::from_rgba8(12, 17, 30, 215);
            bg.border_color = Color::from_rgba8(70, 95, 140, 80);
            bg.border_width = 1.0f;
            blur_cache.draw_glass_aperture(cc, *bot, 11.0f, 11.0f, bg);
        }
        draw_text(cc, 54.0f, 728.0f,
                  "[SPACE] Pause | [R] Restart | [S] Speed | [D] Dirty-Rect | [C/1] Handle | [E/3] Color FX | [W/4] Wipe FX | [F/5] Distort FX | [CLICK] Card | [ESC] Exit",
                  11.0f, 15.0f, TextColor::rgb(188, 210, 240));
        chrome_dirty = false;
    };

#ifdef NISABA_HAS_BACKEND_OS
    std::unique_ptr<backend_os::Platform> platform;
    std::unique_ptr<backend_os::Window> window;
    std::unique_ptr<nisaba::gpu::Context> gpu_ctx;
    int gpu_tex_id = 0;
    bool g_running = true;

    if (!run_headless) {
        if (auto p_res = backend_os::Platform::create(); p_res.isOk()) {
            platform = std::move(p_res).value();
            backend_os::WindowConfig cfg;
            cfg.title = "NISABA LOTTIE CPU SHOWCASE — Phases 1-4 Sovereign Reference";
            cfg.width = WIN_WIDTH;
            cfg.height = WIN_HEIGHT;
            cfg.vsync = false;
            if (auto w_res = backend_os::Window::create(*platform, cfg); w_res.isOk()) {
                window = std::move(w_res).value();
#ifdef NISABA_GLEW
                glewInit();
#endif
                gpu_ctx = nisaba::gpu::createContextGL3(nisaba::gpu::CreateFlags::Antialias);
                if (gpu_ctx) {
                    gpu_tex_id = gpu_ctx->createImageRGBA(WIN_WIDTH, WIN_HEIGHT, 0, fb->data());
                    window->onClose().connect([&]() { g_running = false; });
                    platform->onKeyDown().connect([&](int key, int) {
                        if (key == 27 || key == 'q' || key == 'Q') g_running = false;
                        else if (key == 32) deck.toggle_pause();
                        else if (key == 'r' || key == 'R') deck.restart_all();
                        else if (key == 's' || key == 'S') deck.cycle_speed();
                        else if (key == 'd' || key == 'D') { deck.show_damage_overlay = !deck.show_damage_overlay; chrome_dirty = true; }
                        else if (key == 'c' || key == 'C' || key == '1') deck.cycle_p1_handle_mutation();
                        else if (key == '2') { deck.p2.seek_frame(0.0f); if (!deck.paused) deck.p2.play(); }
                        else if (key == 'e' || key == 'E' || key == '3') { deck.reload_color_fx(deck.color_fx_variant + 1); chrome_dirty = true; }
                        else if (key == 'w' || key == 'W' || key == '4') { deck.reload_wipe_fx(deck.wipe_fx_variant + 1); chrome_dirty = true; }
                        else if (key == 'f' || key == 'F' || key == '5') { deck.reload_distort_fx(deck.distort_fx_variant + 1); chrome_dirty = true; }
                        else if (key == '6') { deck.p6.seek_frame(0.0f); if (!deck.paused) deck.p6.play(); }
                    });
                    platform->onMouseDown().connect([&](float mx, float my, int) {
                        int row = (my >= ROW0_Y && my <= ROW0_Y + CARD_H) ? 0
                                : ((my >= ROW1_Y && my <= ROW1_Y + CARD_H) ? 1 : -1);
                        if (row < 0) return;
                        int col = -1;
                        for (int c = 0; c < 3; ++c) {
                            const float x0 = MARGIN_X + static_cast<float>(c) * (CARD_W + GAP_X);
                            if (mx >= x0 && mx <= x0 + CARD_W) { col = c; break; }
                        }
                        if (col < 0) return;
                        const int idx = row * 3 + col;
                        if (idx == 0) deck.cycle_p1_handle_mutation();
                        else if (idx == 1) { deck.p2.seek_frame(0.0f); if (!deck.paused) deck.p2.play(); }
                        else if (idx == 2) { deck.reload_color_fx(deck.color_fx_variant + 1); chrome_dirty = true; }
                        else if (idx == 3) { deck.reload_wipe_fx(deck.wipe_fx_variant + 1); chrome_dirty = true; }
                        else if (idx == 4) { deck.reload_distort_fx(deck.distort_fx_variant + 1); chrome_dirty = true; }
                        else if (idx == 5) { deck.p6.seek_frame(0.0f); if (!deck.paused) deck.p6.play(); }
                    });
                }
            }
        }
    }
#endif

    double last_time = 0.0;
    auto start_tp = std::chrono::high_resolution_clock::now();
    uint32_t fps_frames = 0;
    double fps_accum = 0.0;
    double measured_fps = 60.0;
    double measured_ms = 5.0;

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

        if (chrome_dirty) bake_static_chrome();
        std::memcpy(fb->pixels_mut(), baked_chrome->pixels(), baked_chrome->data_len());
        deck.advance_all(static_cast<float>(dt));

        // Dynamic top-right FPS readout
        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << measured_fps << " FPS | "
               << std::setprecision(2) << measured_ms << " ms | " << deck.speed << "x";
            draw_text(canvas, 1020.0f, 44.0f, ss.str(), 11.5f, 15.0f, TextColor::rgb(80, 245, 255));
        }

        auto draw_card_stage = [&](int row, int col, Player& player, Color accent,
                                   std::function<void(const Rect&, const Rect&)> post_overlay = nullptr) {
            const float cx = MARGIN_X + static_cast<float>(col) * (CARD_W + GAP_X);
            const float cy = (row == 0) ? ROW0_Y : ROW1_Y;
            const auto stage_r = *Rect::from_xywh(cx + 16.0f, cy + 68.0f, CARD_W - 32.0f, 186.0f);
            const auto lottie_r = *Rect::from_xywh(stage_r.left() + 6.0f, stage_r.top() + 6.0f,
                                                   stage_r.width() - 12.0f, stage_r.height() - 12.0f);
            canvas.save();
            canvas.clip_rect(stage_r);
            player.render(canvas, lottie_r, true);
            if (post_overlay) post_overlay(stage_r, lottie_r);
            canvas.restore();

            const float prog = player.progress();
            const float bar_y = cy + 262.0f;
            const float bar_w = CARD_W - 32.0f;
            canvas.fill_round_rect(*Rect::from_xywh(cx + 16.0f, bar_y, bar_w, 4.5f), 2.2f, 2.2f,
                                   Paint(Color::from_rgba8(22, 30, 50, 255)));
            if (prog > 0.002f) {
                canvas.fill_round_rect(*Rect::from_xywh(cx + 16.0f, bar_y, bar_w * prog, 4.5f), 2.2f, 2.2f,
                                       Paint(accent));
            }
        };

        draw_card_stage(0, 0, deck.p1, Color::from_rgba8(0, 232, 255, 255),
                        [&](const Rect&, const Rect& lr) {
                            if (!deck.show_damage_overlay || !deck.p1_last_dirty.has_value()) return;
                            const float sc = std::min(lr.width() / 240.0f, lr.height() / 160.0f);
                            const float ox = lr.left() + (lr.width() - 240.0f * sc) * 0.5f;
                            const float oy = lr.top() + (lr.height() - 160.0f * sc) * 0.5f;
                            const Rect& d = *deck.p1_last_dirty;
                            if (auto dr = Rect::from_ltrb(ox + d.left() * sc, oy + d.top() * sc,
                                                          ox + d.right() * sc, oy + d.bottom() * sc)) {
                                Stroke dst(1.8f);
                                dst.dash = StrokeDash::create({6.0f, 4.0f}, 0.0f);
                                canvas.stroke_rect(*dr, Paint(Color::from_rgba8(0, 255, 190, 240)), dst);
                            }
                        });

        draw_card_stage(0, 1, deck.p2, Color::from_rgba8(0, 245, 165, 255));
        draw_card_stage(0, 2, deck.p3, Color::from_rgba8(255, 185, 45, 255));
        draw_card_stage(1, 0, deck.p4, Color::from_rgba8(140, 130, 255, 255));
        draw_card_stage(1, 1, deck.p5, Color::from_rgba8(255, 75, 145, 255));
        draw_card_stage(1, 2, deck.p6, Color::from_rgba8(55, 215, 255, 255),
                        [&](const Rect& sr, const Rect&) {
                            const float l_norm = std::clamp((deck.audio_meter.left_db + 24.0f) / 24.0f, 0.0f, 1.0f);
                            const float r_norm = std::clamp((deck.audio_meter.right_db + 24.0f) / 24.0f, 0.0f, 1.0f);
                            const float vx = sr.left() + 12.0f;
                            const float vw = sr.width() - 24.0f;
                            const float vy = sr.bottom() - 22.0f;
                            canvas.fill_round_rect(*Rect::from_xywh(vx, vy, vw, 6.0f), 3.0f, 3.0f, Paint(Color::from_rgba8(24, 34, 54, 220)));
                            if (l_norm > 0.01f) canvas.fill_round_rect(*Rect::from_xywh(vx, vy, vw * l_norm, 6.0f), 3.0f, 3.0f, Paint(Color::from_rgba8(40, 240, 165, 240)));
                            canvas.fill_round_rect(*Rect::from_xywh(vx, vy + 9.0f, vw, 6.0f), 3.0f, 3.0f, Paint(Color::from_rgba8(24, 34, 54, 220)));
                            if (r_norm > 0.01f) canvas.fill_round_rect(*Rect::from_xywh(vx, vy + 9.0f, vw * r_norm, 6.0f), 3.0f, 3.0f, Paint(Color::from_rgba8(60, 195, 255, 240)));
                        });

        auto t1 = std::chrono::high_resolution_clock::now();
        measured_ms = measured_ms * 0.9 + std::chrono::duration<double, std::milli>(t1 - t0).count() * 0.1;
        ++fps_frames;
        fps_accum += dt;
        if (fps_accum >= 0.5) {
            measured_fps = static_cast<double>(fps_frames) / fps_accum;
            fps_frames = 0;
            fps_accum = 0.0;
        }

#ifdef NISABA_HAS_BACKEND_OS
        if (gpu_ctx && window) {
            gpu_ctx->updateImage(gpu_tex_id, fb->data());
            gpu_ctx->beginFrame(WIN_WIDTH, WIN_HEIGHT, 1.0f);
            auto p = gpu_ctx->imagePattern(0, 0, WIN_WIDTH, WIN_HEIGHT, 0, gpu_tex_id, 1.0f);
            gpu_ctx->beginPath();
            gpu_ctx->rect(0, 0, WIN_WIDTH, WIN_HEIGHT);
            gpu_ctx->fillPaint(p);
            gpu_ctx->fill();
            gpu_ctx->endFrame();
            window->swapBuffers();
        }
#endif
    }

#ifdef NISABA_HAS_BACKEND_OS
    if (gpu_ctx && gpu_tex_id != 0) gpu_ctx->deleteImage(gpu_tex_id);
#endif

    const std::string out_png = "nisaba_lottie_showcase.png";
    if (fb->save_png(out_png)) {
        std::cout << "[+] Saved CPU showcase snapshot to: " << out_png << "\n";
    }
    return 0;
}
