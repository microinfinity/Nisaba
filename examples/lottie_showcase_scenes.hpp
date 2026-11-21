#pragma once

/// @file lottie_showcase_scenes.hpp
/// @brief Shared interactive Lottie showcase scenes demonstrating Phases 1-4 (InvalidationController,
///        ResourceProvider/Multi-frame/kDeferImageLoading, PropertyObserver O(1) handles,
///        ExternalLayer/PrecompInterceptor, Color/Tone & Distortion/Wipe/Noise FX,
///        ExpressionManager, Advanced TextAnimator + Text-on-Path, and AudioLayer/AudioTrack).

#include "nisaba/lottie/lottie.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/path/path_builder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nisaba::showcase {

using namespace nisaba::lottie;

// ── Shared Multi-Frame Procedural ImageAsset (Phase 1.2) ──────────────────

inline std::shared_ptr<DecodedImageAsset> make_animated_plasma_sprite() {
    std::vector<DecodedImageAsset::TimedFrame> frames;
    constexpr uint32_t W = 48, H = 48;
    constexpr int NUM_FRAMES = 8;
    frames.reserve(NUM_FRAMES);

    for (int f = 0; f < NUM_FRAMES; ++f) {
        auto pm = Pixmap::create(W, H);
        if (!pm) continue;
        const float phase = static_cast<float>(f) * (6.2831853f / static_cast<float>(NUM_FRAMES));
        auto mut = pm->as_mut();
        for (uint32_t y = 0; y < H; ++y) {
            auto* row = mut.row(y);
            const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(H) * 2.0f - 1.0f;
            for (uint32_t x = 0; x < W; ++x) {
                const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(W) * 2.0f - 1.0f;
                const float r = std::hypot(nx, ny);
                if (r > 0.96f) {
                    row[x] = PremultipliedColorU8::TRANSPARENT;
                    continue;
                }
                const float ang = std::atan2(ny, nx);
                const float wave = 0.5f + 0.5f * std::sin(r * 10.0f - phase + std::cos(ang * 3.0f + phase));
                const float ring = 0.5f + 0.5f * std::cos(ang * 2.0f - phase);
                const uint8_t cr = static_cast<uint8_t>(40.0f + 215.0f * wave);
                const uint8_t cg = static_cast<uint8_t>(120.0f + 135.0f * ring);
                const uint8_t cb = static_cast<uint8_t>(255.0f - 120.0f * wave);
                const float edge = std::clamp((0.96f - r) * 14.0f, 0.0f, 1.0f);
                const uint8_t ca = static_cast<uint8_t>(edge * 255.0f);
                row[x] = PremultipliedColorU8::from_rgba_unchecked(
                    static_cast<uint8_t>((cr * ca) / 255),
                    static_cast<uint8_t>((cg * ca) / 255),
                    static_cast<uint8_t>((cb * ca) / 255),
                    ca
                );
            }
        }
        frames.push_back({std::move(*pm), 0.125f});
    }
    return DecodedImageAsset::from_frames(std::move(frames));
}

// ── Shared Live Embedded Radar Widget (Phase 2.1 ExternalLayer) ───────────

inline std::shared_ptr<PrecompInterceptor> make_live_radar_interceptor() {
    return std::make_shared<CallbackPrecompInterceptor>(
        [](std::string_view id, std::string_view, Size sz) -> std::shared_ptr<ExternalLayer> {
            if (id != "live_radar") return nullptr;
            const float w = sz.width();
            const float h = sz.height();
            return std::make_shared<CallbackExternalLayer>([w, h](ICanvas& c, float t_sec) {
                const float cx = w * 0.5f;
                const float cy = h * 0.5f;
                const float R = std::min(w, h) * 0.44f;

                // Deep telemetry panel background
                if (auto bg_r = Rect::from_xywh(0.0f, 0.0f, w, h)) {
                    c.fill_round_rect(*bg_r, 14.0f, 14.0f, Paint(Color::from_rgba8(8, 22, 34, 235)));
                    c.stroke_round_rect(*bg_r, 14.0f, 14.0f, Paint(Color::from_rgba8(0, 235, 195, 160)), Stroke(1.8f));
                }

                // Concentric radar rings & crosshair
                Stroke grid_st(1.0f);
                Paint ring_p(Color::from_rgba8(0, 220, 180, 70));
                for (int k = 1; k <= 3; ++k) {
                    const float rk = R * (static_cast<float>(k) / 3.0f);
                    if (auto ov = Rect::from_xywh(cx - rk, cy - rk, rk * 2.0f, rk * 2.0f)) {
                        c.stroke_oval(*ov, ring_p, grid_st);
                    }
                }
                auto stroke_seg = [&](float x0, float y0, float x1, float y1, const Paint& pt, const Stroke& st) {
                    PathBuilder lp;
                    lp.move_to(x0, y0);
                    lp.line_to(x1, y1);
                    if (auto seg = lp.finish()) {
                        c.stroke_path(*seg, pt, st);
                    }
                };
                stroke_seg(cx - R, cy, cx + R, cy, ring_p, grid_st);
                stroke_seg(cx, cy - R, cx, cy + R, ring_p, grid_st);

                // Sweeping radar wedge + bright leading beam
                const float sweep = t_sec * 3.1415926f;
                for (int s = 0; s < 5; ++s) {
                    const float a0 = sweep - static_cast<float>(s) * 0.14f;
                    const float a1 = a0 - 0.15f;
                    PathBuilder pb;
                    pb.move_to(cx, cy);
                    pb.line_to(cx + std::cos(a0) * R, cy + std::sin(a0) * R);
                    pb.line_to(cx + std::cos(a1) * R, cy + std::sin(a1) * R);
                    pb.close();
                    if (auto wedge = pb.finish()) {
                        const uint8_t alpha = static_cast<uint8_t>(std::max(12, 95 - s * 18));
                        c.fill_path(*wedge, Paint(Color::from_rgba8(0, 255, 190, alpha)), nisaba::FillRule::Winding);
                    }
                }
                stroke_seg(cx, cy, cx + std::cos(sweep) * R, cy + std::sin(sweep) * R,
                           Paint(Color::from_rgba8(140, 255, 230, 255)), Stroke(2.2f));

                // Pulsing target blips
                const float blip_angles[3] = {0.7f, 2.5f, 4.4f};
                const float blip_radii[3] = {0.42f, 0.72f, 0.58f};
                for (int i = 0; i < 3; ++i) {
                    const float bx = cx + std::cos(blip_angles[i]) * (R * blip_radii[i]);
                    const float by = cy + std::sin(blip_angles[i]) * (R * blip_radii[i]);
                    const float pulse = 0.5f + 0.5f * std::sin(t_sec * 6.0f + static_cast<float>(i) * 2.1f);
                    const float br = 3.0f + pulse * 3.0f;
                    if (auto bv = Rect::from_xywh(bx - br, by - br, br * 2.0f, br * 2.0f)) {
                        c.fill_oval(*bv, Paint(Color::from_rgba8(255, 220, 60, static_cast<uint8_t>(140 + 115 * pulse))));
                    }
                }

                // Live bottom oscilloscope wave inside the widget
                PathBuilder wb;
                const float wy_base = h - 16.0f;
                for (int i = 0; i <= 28; ++i) {
                    const float u = static_cast<float>(i) / 28.0f;
                    const float wx = 10.0f + u * (w - 20.0f);
                    const float wy = wy_base + std::sin(u * 14.0f - t_sec * 8.0f) * 6.5f * std::sin(u * 3.14159f);
                    if (i == 0) wb.move_to(wx, wy);
                    else wb.line_to(wx, wy);
                }
                if (auto wave_p = wb.finish()) {
                    Stroke wst(1.8f);
                    wst.line_cap = nisaba::LineCap::Round;
                    c.stroke_path(*wave_p, Paint(Color::from_rgba8(56, 220, 255, 230)), wst);
                }
            });
        }
    );
}

// ── Scene JSONs ───────────────────────────────────────────────────────────

inline constexpr std::string_view SCENE_PHASE1_DAMAGE_HANDLES = R"JSON({
    "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160, "nm": "Phase1Showcase",
    "assets": [
        {"id": "plasma_sprite", "w": 48, "h": 48, "u": "", "p": "plasma.gif"}
    ],
    "layers": [
        {
            "ind": 1, "ty": 4, "nm": "OrbitDot", "ip": 0, "op": 120, "st": 0,
            "ks": {
                "p": {"a": 1, "k": [
                    {"t": 0, "s": [75, 118, 0], "e": [195, 48, 0], "o": {"x": 0.42, "y": 0}, "i": {"x": 0.58, "y": 1}},
                    {"t": 60, "s": [195, 48, 0], "e": [75, 118, 0], "o": {"x": 0.42, "y": 0}, "i": {"x": 0.58, "y": 1}},
                    {"t": 120, "s": [75, 118, 0]}
                ]},
                "a": {"a": 0, "k": [0, 0, 0]},
                "s": {"a": 0, "k": [100, 100, 100]},
                "r": {"a": 1, "k": [{"t": 0, "s": [0], "e": [360]}, {"t": 120, "s": [360]}]},
                "o": {"a": 0, "k": 100}
            },
            "shapes": [
                {"ty": "gr", "nm": "Grp", "it": [
                    {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 11}},
                    {"ty": "fl", "nm": "Fill", "c": {"a": 0, "k": [0.0, 0.88, 1.0, 1.0]}, "o": {"a": 0, "k": 100}},
                    {"ty": "st", "nm": "Rim", "c": {"a": 0, "k": [1, 1, 1, 0.85]}, "o": {"a": 0, "k": 100}, "w": {"a": 0, "k": 3}},
                    {"ty": "tr"}
                ]}
            ]
        },
        {
            "ind": 2, "ty": 2, "nm": "PlasmaSprite", "refId": "plasma_sprite", "ip": 0, "op": 120, "st": 0,
            "ks": {
                "p": {"a": 0, "k": [18, 18, 0]},
                "a": {"a": 0, "k": [0, 0, 0]},
                "s": {"a": 0, "k": [100, 100, 100]},
                "o": {"a": 0, "k": 100}
            }
        },
        {
            "ind": 3, "ty": 4, "nm": "StaticGuide", "ip": 0, "op": 120, "st": 0,
            "ks": {"p": {"a": 0, "k": [135, 83, 0]}, "a": {"a": 0, "k": [0, 0, 0]}, "s": {"a": 0, "k": [100, 100, 100]}, "o": {"a": 0, "k": 100}},
            "shapes": [
                {"ty": "gr", "nm": "Track", "it": [
                    {"ty": "sh", "ks": {"a": 0, "k": {"c": false, "v": [[-60, 35], [60, -35]], "i": [[0, 0], [0, 0]], "o": [[0, 0], [0, 0]]}}},
                    {"ty": "st", "c": {"a": 0, "k": [0.25, 0.35, 0.55, 0.5]}, "o": {"a": 0, "k": 100}, "w": {"a": 0, "k": 2}},
                    {"ty": "tr"}
                ]}
            ]
        }
    ]
})JSON";

inline constexpr std::string_view SCENE_PHASE2_EXTERNAL_PRECOMP = R"JSON({
    "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160, "nm": "Phase2ExternalPrecomp",
    "layers": [
        {
            "ind": 1, "ty": 4, "nm": "OuterFrame", "ip": 0, "op": 120, "st": 0,
            "ks": {
                "p": {"a": 0, "k": [120, 80, 0]}, "a": {"a": 0, "k": [0, 0, 0]},
                "s": {"a": 0, "k": [100, 100, 100]},
                "r": {"a": 1, "k": [{"t": 0, "s": [0], "e": [-360]}, {"t": 120, "s": [-360]}]},
                "o": {"a": 0, "k": 90}
            },
            "shapes": [
                {"ty": "gr", "nm": "Ring", "it": [
                    {"ty": "el", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [146, 146]}},
                    {"ty": "tm", "s": {"a": 0, "k": 0}, "e": {"a": 0, "k": 72}, "o": {"a": 0, "k": 0}},
                    {"ty": "st", "c": {"a": 0, "k": [0.0, 0.92, 0.78, 0.85]}, "o": {"a": 0, "k": 100}, "w": {"a": 0, "k": 4}, "lc": 2},
                    {"ty": "tr"}
                ]}
            ]
        },
        {
            "ind": 2, "ty": 0, "nm": "LiveRadarWidget", "refId": "live_radar", "w": 168, "h": 118,
            "ip": 0, "op": 120, "st": 0,
            "ks": {
                "p": {"a": 0, "k": [120, 80, 0]},
                "a": {"a": 0, "k": [84, 59, 0]},
                "s": {"a": 1, "k": [
                    {"t": 0, "s": [92, 92, 100], "e": [103, 103, 100]},
                    {"t": 60, "s": [103, 103, 100], "e": [92, 92, 100]},
                    {"t": 120, "s": [92, 92, 100]}
                ]},
                "r": {"a": 1, "k": [
                    {"t": 0, "s": [-4], "e": [4]},
                    {"t": 60, "s": [4], "e": [-4]},
                    {"t": 120, "s": [-4]}
                ]},
                "o": {"a": 0, "k": 96}
            }
        }
    ]
})JSON";

// Color & Tone FX variants (Phase 2.2)
inline constexpr std::array<std::string_view, 4> COLOR_FX_NAMES = {
    "GradientRamp + HueSaturation",
    "FourColorGradient Aurora",
    "CC Toner + EasyLevels",
    "Black&White + Invert Pulse"
};

inline std::string make_color_fx_scene(int variant) {
    switch (variant % 4) {
        case 0:
            return R"JSON({
                "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160,
                "layers": [
                    {
                        "ind": 1, "ty": 4, "nm": "Emblem", "ip": 0, "op": 120, "st": 0,
                        "ks": {"p": {"a": 0, "k": [120, 80]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]},
                               "r": {"a": 1, "k": [{"t": 0, "s": [0], "e": [360]}, {"t": 120, "s": [360]}]}, "o": {"a": 0, "k": 100}},
                        "shapes": [
                            {"ty": "sr", "sy": 1, "pt": {"a": 0, "k": 6}, "p": {"a": 0, "k": [0, 0]}, "r": {"a": 0, "k": 0}, "ir": {"a": 0, "k": 28}, "or": {"a": 0, "k": 56}},
                            {"ty": "fl", "c": {"a": 0, "k": [1.0, 0.35, 0.15, 1]}, "o": {"a": 0, "k": 100}},
                            {"ty": "st", "c": {"a": 0, "k": [1, 0.95, 0.5, 1]}, "o": {"a": 0, "k": 100}, "w": {"a": 0, "k": 4}}
                        ],
                        "ef": [{"ty": 5, "mn": "ADBE HUE SATURATION", "en": 1, "ef": [
                            {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [0], "e": [360]}, {"t": 120, "s": [360]}]}},
                            {"ty": 0, "v": {"a": 0, "k": 20}},
                            {"ty": 0, "v": {"a": 0, "k": 5}}
                        ]}]
                    },
                    {
                        "ind": 2, "ty": 1, "nm": "RampBG", "sw": 240, "sh": 160, "sc": "#ffffff", "ip": 0, "op": 120, "st": 0,
                        "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                        "ef": [{"ty": 5, "mn": "ADBE Ramp", "en": 1, "ef": [
                            {"ty": 3, "v": {"a": 0, "k": [0, 0]}}, {"ty": 2, "v": {"a": 0, "k": [0.10, 0.14, 0.38, 1]}},
                            {"ty": 3, "v": {"a": 0, "k": [240, 160]}}, {"ty": 2, "v": {"a": 0, "k": [0.68, 0.12, 0.42, 1]}},
                            {"ty": 7, "v": {"a": 0, "k": 1}}, {"ty": 0, "v": {"a": 0, "k": 0}}, {"ty": 0, "v": {"a": 0, "k": 0}}
                        ]}]
                    }
                ]
            })JSON";
        case 1:
            return R"JSON({
                "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160,
                "layers": [
                    {
                        "ind": 1, "ty": 1, "nm": "Aurora", "sw": 240, "sh": 160, "sc": "#000000", "ip": 0, "op": 120, "st": 0,
                        "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                        "ef": [{"ty": 5, "mn": "ADBE 4ColorGradient", "en": 1, "ef": [
                            {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [20, 20], "e": [140, 20]}, {"t": 60, "s": [140, 20], "e": [20, 20]}, {"t": 120, "s": [20, 20]}]}},
                            {"ty": 2, "v": {"a": 0, "k": [0.05, 0.85, 0.95, 1]}},
                            {"ty": 3, "v": {"a": 0, "k": [220, 20]}}, {"ty": 2, "v": {"a": 0, "k": [0.95, 0.20, 0.55, 1]}},
                            {"ty": 3, "v": {"a": 0, "k": [20, 140]}}, {"ty": 2, "v": {"a": 0, "k": [0.35, 0.15, 0.92, 1]}},
                            {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [220, 140], "e": [110, 130]}, {"t": 60, "s": [110, 130], "e": [220, 140]}, {"t": 120, "s": [220, 140]}]}},
                            {"ty": 2, "v": {"a": 0, "k": [1.0, 0.78, 0.18, 1]}},
                            {"ty": 0, "v": {"a": 0, "k": 25}}, {"ty": 0, "v": {"a": 0, "k": 0}}, {"ty": 0, "v": {"a": 0, "k": 100}}
                        ]}]
                    }
                ]
            })JSON";
        case 2:
            return R"JSON({
                "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160,
                "layers": [
                    {
                        "ind": 1, "ty": 4, "nm": "Rings", "ip": 0, "op": 120, "st": 0,
                        "ks": {"p": {"a": 0, "k": [120, 80]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                        "shapes": [
                            {"ty": "gr", "it": [
                                {"ty": "el", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [130, 130]}},
                                {"ty": "gf", "t": 2, "s": {"a": 0, "k": [-20, -20]}, "e": {"a": 0, "k": [65, 65]},
                                 "g": {"p": 3, "k": {"a": 0, "k": [0, 1, 1, 1, 0.5, 0.5, 0.5, 0.5, 1, 0.05, 0.05, 0.05]}}},
                                {"ty": "tr"}
                            ]}
                        ],
                        "ef": [
                            {"ty": 5, "mn": "ADBE Easy Levels2", "en": 1, "ef": [
                                {"ty": 7, "v": {"a": 0, "k": 1}}, {"ty": 0, "v": {"a": 0, "k": 0.08}}, {"ty": 0, "v": {"a": 0, "k": 0.92}},
                                {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [0.7], "e": [1.8]}, {"t": 60, "s": [1.8], "e": [0.7]}, {"t": 120, "s": [0.7]}]}},
                                {"ty": 0, "v": {"a": 0, "k": 0}}, {"ty": 0, "v": {"a": 0, "k": 1}}
                            ]},
                            {"ty": 5, "mn": "CC Toner", "en": 1, "ef": [
                                {"ty": 7, "v": {"a": 0, "k": 2}},
                                {"ty": 2, "v": {"a": 0, "k": [1.0, 0.96, 0.72, 1]}},
                                {"ty": 2, "v": {"a": 0, "k": [0.8, 0.8, 0.8, 1]}},
                                {"ty": 2, "v": {"a": 0, "k": [0.95, 0.32, 0.15, 1]}},
                                {"ty": 2, "v": {"a": 0, "k": [0.2, 0.2, 0.2, 1]}},
                                {"ty": 2, "v": {"a": 0, "k": [0.06, 0.08, 0.24, 1]}},
                                {"ty": 0, "v": {"a": 0, "k": 0}}
                            ]}
                        ]
                    }
                ]
            })JSON";
        default:
            return R"JSON({
                "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160,
                "layers": [
                    {
                        "ind": 1, "ty": 4, "nm": "ColorBars", "ip": 0, "op": 120, "st": 0,
                        "ks": {"p": {"a": 0, "k": [120, 80]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                        "shapes": [
                            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [-65, 0]}, "s": {"a": 0, "k": [52, 120]}, "r": {"a": 0, "k": 8}}, {"ty": "fl", "c": {"a": 0, "k": [1, 0.2, 0.3, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
                            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [52, 120]}, "r": {"a": 0, "k": 8}}, {"ty": "fl", "c": {"a": 0, "k": [0.2, 0.95, 0.5, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
                            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [65, 0]}, "s": {"a": 0, "k": [52, 120]}, "r": {"a": 0, "k": 8}}, {"ty": "fl", "c": {"a": 0, "k": [0.2, 0.5, 1.0, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]}
                        ],
                        "ef": [
                            {"ty": 5, "mn": "ADBE Black&White", "en": 1, "ef": [
                                {"ty": 0, "v": {"a": 0, "k": 80}}, {"ty": 0, "v": {"a": 0, "k": 60}},
                                {"ty": 0, "v": {"a": 0, "k": 40}}, {"ty": 0, "v": {"a": 0, "k": 60}},
                                {"ty": 0, "v": {"a": 0, "k": 25}}, {"ty": 0, "v": {"a": 0, "k": 80}}
                            ]},
                            {"ty": 5, "mn": "ADBE Invert", "en": 1, "ef": [
                                {"ty": 7, "v": {"a": 0, "k": 1}},
                                {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [0], "e": [100]}, {"t": 60, "s": [100], "e": [0]}, {"t": 120, "s": [0]}]}}
                            ]}
                        ]
                    }
                ]
            })JSON";
    }
}

// Transition & Tiling FX variants (Phase 3.1)
inline constexpr std::array<std::string_view, 4> WIPE_FX_NAMES = {
    "LinearWipe (Feathered)",
    "VenetianBlinds (Slats)",
    "MotionTile (Mirror Grid)",
    "TransformFX (Skew+Spin)"
};

inline std::string make_wipe_fx_scene(int variant) {
    const std::string base_card = R"JSON(
        "ind": 1, "ty": 4, "nm": "Card", "ip": 0, "op": 120, "st": 0,
        "ks": {"p": {"a": 0, "k": [120, 80]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
        "shapes": [
            {"ty": "gr", "it": [
                {"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [176, 114]}, "r": {"a": 0, "k": 18}},
                {"ty": "gf", "t": 1, "s": {"a": 0, "k": [-88, -57]}, "e": {"a": 0, "k": [88, 57]},
                 "g": {"p": 2, "k": {"a": 0, "k": [0, 0.08, 0.72, 0.98, 1, 0.56, 0.22, 0.96]}}},
                {"ty": "tr"}
            ]},
            {"ty": "gr", "it": [
                {"ty": "sr", "sy": 1, "pt": {"a": 0, "k": 5}, "p": {"a": 0, "k": [0, 0]}, "r": {"a": 0, "k": 0}, "ir": {"a": 0, "k": 18}, "or": {"a": 0, "k": 38}},
                {"ty": "fl", "c": {"a": 0, "k": [1.0, 0.88, 0.25, 1.0]}, "o": {"a": 0, "k": 100}},
                {"ty": "tr"}
            ]}
        ],
    )JSON";

    std::string ef;
    switch (variant % 4) {
        case 0:
            ef = R"JSON("ef": [{"ty": 5, "mn": "ADBE Linear Wipe", "en": 1, "ef": [
                {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [0], "e": [100]}, {"t": 60, "s": [100], "e": [0]}, {"t": 120, "s": [0]}]}},
                {"ty": 0, "v": {"a": 0, "k": 65}},
                {"ty": 0, "v": {"a": 0, "k": 24}}
            ]}])JSON";
            break;
        case 1:
            ef = R"JSON("ef": [{"ty": 5, "mn": "ADBE Venetian Blinds", "en": 1, "ef": [
                {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [5], "e": [88]}, {"t": 60, "s": [88], "e": [5]}, {"t": 120, "s": [5]}]}},
                {"ty": 0, "v": {"a": 0, "k": 22}},
                {"ty": 0, "v": {"a": 0, "k": 26}},
                {"ty": 0, "v": {"a": 0, "k": 4}}
            ]}])JSON";
            break;
        case 2:
            ef = R"JSON("ef": [{"ty": 5, "mn": "ADBE Tile", "en": 1, "ef": [
                {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [60, 40], "e": [180, 120]}, {"t": 120, "s": [180, 120]}]}},
                {"ty": 0, "v": {"a": 0, "k": 42}},
                {"ty": 0, "v": {"a": 0, "k": 42}},
                {"ty": 0, "v": {"a": 0, "k": 100}},
                {"ty": 0, "v": {"a": 0, "k": 100}},
                {"ty": 7, "v": {"a": 0, "k": 1}}
            ]}])JSON";
            break;
        default:
            ef = R"JSON("ef": [{"ty": 5, "mn": "ADBE Geometry2", "en": 1, "ef": [
                {"ty": 3, "v": {"a": 0, "k": [120, 80]}},
                {"ty": 3, "v": {"a": 0, "k": [120, 80]}},
                {"ty": 7, "v": {"a": 0, "k": 1}},
                {"ty": 0, "v": {"a": 0, "k": 82}},
                {"ty": 0, "v": {"a": 0, "k": 82}},
                {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [-22], "e": [22]}, {"t": 60, "s": [22], "e": [-22]}, {"t": 120, "s": [-22]}]}},
                {"ty": 0, "v": {"a": 0, "k": 0}},
                {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [-15], "e": [15]}, {"t": 60, "s": [15], "e": [-15]}, {"t": 120, "s": [-15]}]}},
                {"ty": 0, "v": {"a": 0, "k": 95}}
            ]}])JSON";
            break;
    }

    return std::string(R"({"v":"5.9.0","fr":60,"ip":0,"op":120,"w":240,"h":160,"layers":[{)") + base_card + ef + "}]}";
}

// Distortion, Blur & Procedural FX variants (Phase 3.2)
inline constexpr std::array<std::string_view, 5> DISTORT_FX_NAMES = {
    "Bulge Lens Warp",
    "Sphere Refraction",
    "CornerPin 3D Homography",
    "DirectionalBlur Motion",
    "FractalNoise Clouds"
};

inline std::string make_distort_fx_scene(int variant) {
    const std::string grid_layer = R"JSON(
        "ind": 1, "ty": 4, "nm": "GridTarget", "ip": 0, "op": 120, "st": 0,
        "ks": {"p": {"a": 0, "k": [120, 80]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
        "shapes": [
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [196, 128]}, "r": {"a": 0, "k": 12}}, {"ty": "fl", "c": {"a": 0, "k": [0.10, 0.16, 0.30, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [-52, -26]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 6}}, {"ty": "fl", "c": {"a": 0, "k": [0.98, 0.32, 0.45, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [0, -26]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 6}}, {"ty": "fl", "c": {"a": 0, "k": [1.0, 0.78, 0.20, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [52, -26]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 6}}, {"ty": "fl", "c": {"a": 0, "k": [0.18, 0.90, 0.62, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [-52, 26]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 6}}, {"ty": "fl", "c": {"a": 0, "k": [0.20, 0.72, 1.0, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [0, 26]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 6}}, {"ty": "fl", "c": {"a": 0, "k": [0.72, 0.35, 1.0, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]},
            {"ty": "gr", "it": [{"ty": "rc", "p": {"a": 0, "k": [52, 26]}, "s": {"a": 0, "k": [42, 42]}, "r": {"a": 0, "k": 6}}, {"ty": "fl", "c": {"a": 0, "k": [1.0, 0.50, 0.18, 1]}, "o": {"a": 0, "k": 100}}, {"ty": "tr"}]}
        ],
    )JSON";

    switch (variant % 5) {
        case 0:
            return std::string(R"({"v":"5.9.0","fr":60,"ip":0,"op":120,"w":240,"h":160,"layers":[{)") + grid_layer +
                R"JSON("ef": [{"ty": 5, "mn": "ADBE Bulge", "en": 1, "ef": [
                    {"ty": 0, "v": {"a": 0, "k": 64}},
                    {"ty": 0, "v": {"a": 0, "k": 64}},
                    {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [75, 80], "e": [165, 80]}, {"t": 60, "s": [165, 80], "e": [75, 80]}, {"t": 120, "s": [75, 80]}]}},
                    {"ty": 0, "v": {"a": 0, "k": 2.2}}
                ]}]}]})JSON";
        case 1:
            return std::string(R"({"v":"5.9.0","fr":60,"ip":0,"op":120,"w":240,"h":160,"layers":[{)") + grid_layer +
                R"JSON("ef": [{"ty": 5, "mn": "ADBE Spherize", "en": 1, "ef": [
                    {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [42], "e": [74]}, {"t": 60, "s": [74], "e": [42]}, {"t": 120, "s": [42]}]}},
                    {"ty": 3, "v": {"a": 0, "k": [120, 80]}}
                ]}]}]})JSON";
        case 2:
            return std::string(R"({"v":"5.9.0","fr":60,"ip":0,"op":120,"w":240,"h":160,"layers":[{)") + grid_layer +
                R"JSON("ef": [{"ty": 5, "mn": "ADBE Corner Pin", "en": 1, "ef": [
                    {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [48, 22], "e": [16, 12]}, {"t": 60, "s": [16, 12], "e": [48, 22]}, {"t": 120, "s": [48, 22]}]}},
                    {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [192, 22], "e": [224, 34]}, {"t": 60, "s": [224, 34], "e": [192, 22]}, {"t": 120, "s": [192, 22]}]}},
                    {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [14, 144], "e": [44, 140]}, {"t": 60, "s": [44, 140], "e": [14, 144]}, {"t": 120, "s": [14, 144]}]}},
                    {"ty": 3, "v": {"a": 1, "k": [{"t": 0, "s": [226, 144], "e": [202, 126]}, {"t": 60, "s": [202, 126], "e": [226, 144]}, {"t": 120, "s": [226, 144]}]}}
                ]}]}]})JSON";
        case 3:
            return std::string(R"({"v":"5.9.0","fr":60,"ip":0,"op":120,"w":240,"h":160,"layers":[{)") + grid_layer +
                R"JSON("ef": [{"ty": 5, "mn": "ADBE Motion Blur", "en": 1, "ef": [
                    {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [0], "e": [180]}, {"t": 120, "s": [180]}]}},
                    {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [4], "e": [32]}, {"t": 60, "s": [32], "e": [4]}, {"t": 120, "s": [4]}]}}
                ]}]}]})JSON";
        default:
            return R"JSON({
                "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160,
                "layers": [{
                    "ind": 1, "ty": 1, "nm": "Noise", "sw": 240, "sh": 160, "sc": "#ffffff", "ip": 0, "op": 120, "st": 0,
                    "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
                    "ef": [
                        {"ty": 5, "mn": "ADBE Fractal Noise", "en": 1, "ef": [
                            {"ty": 7, "v": {"a": 0, "k": 1}}, {"ty": 7, "v": {"a": 0, "k": 2}}, {"ty": 7, "v": {"a": 0, "k": 0}},
                            {"ty": 0, "v": {"a": 0, "k": 165}}, {"ty": 0, "v": {"a": 0, "k": 0}}, {"ty": 7, "v": {"a": 0, "k": 1}},
                            {"ty": 0, "v": {"a": 0, "k": 28}}, {"ty": 0, "v": {"a": 0, "k": 4}}, {"ty": 0, "v": {"a": 0, "k": 55}},
                            {"ty": 0, "v": {"a": 0, "k": 50}},
                            {"ty": 0, "v": {"a": 1, "k": [{"t": 0, "s": [0], "e": [360]}, {"t": 120, "s": [360]}]}},
                            {"ty": 0, "v": {"a": 0, "k": 100}}
                        ]},
                        {"ty": 5, "mn": "CC Toner", "en": 1, "ef": [
                            {"ty": 7, "v": {"a": 0, "k": 2}},
                            {"ty": 2, "v": {"a": 0, "k": [0.75, 0.98, 1.0, 1]}},
                            {"ty": 2, "v": {"a": 0, "k": [0.8, 0.8, 0.8, 1]}},
                            {"ty": 2, "v": {"a": 0, "k": [0.18, 0.48, 0.95, 1]}},
                            {"ty": 2, "v": {"a": 0, "k": [0.2, 0.2, 0.2, 1]}},
                            {"ty": 2, "v": {"a": 0, "k": [0.04, 0.06, 0.16, 1]}},
                            {"ty": 0, "v": {"a": 0, "k": 0}}
                        ]}
                    ]
                }]
            })JSON";
    }
}

// Phase 4: Pluggable ExpressionManager + TextAnimator + Text-on-Path + AudioLayer
inline constexpr std::string_view SCENE_PHASE4_EXPR_TEXT_AUDIO = R"JSON({
    "v": "5.9.0", "fr": 60, "ip": 0, "op": 120, "w": 240, "h": 160, "nm": "Phase4Showcase",
    "assets": [
        {"id": "synth_pulse", "u": "", "p": "synth_pulse.wav"}
    ],
    "layers": [
        {
            "ind": 1, "ty": 4, "nm": "ExprStar", "ip": 0, "op": 120, "st": 0,
            "ks": {
                "p": {"a": 0, "k": [120, 80], "x": "live_orbit"},
                "a": {"a": 0, "k": [0, 0]},
                "s": {"a": 0, "k": [100, 100]},
                "r": {"a": 0, "k": 0, "x": "live_spin"},
                "o": {"a": 0, "k": 100}
            },
            "shapes": [
                {"ty": "sr", "sy": 1, "pt": {"a": 0, "k": 5}, "p": {"a": 0, "k": [0, 0]}, "r": {"a": 0, "k": 0}, "ir": {"a": 0, "k": 6}, "or": {"a": 0, "k": 13}},
                {"ty": "fl", "c": {"a": 0, "k": [0.1, 0.95, 1.0, 1.0], "x": "live_col"}, "o": {"a": 0, "k": 100}}
            ]
        },
        {
            "ind": 2, "ty": 5, "nm": "CurvedBanner", "ip": 0, "op": 120, "st": 0,
            "ks": {"p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]}, "o": {"a": 0, "k": 100}},
            "masksProperties": [{
                "mode": "n",
                "pt": {
                    "a": 0,
                    "k": {
                        "c": false,
                        "v": [[18, 124], [222, 124]],
                        "i": [[0, 0], [0, -96]],
                        "o": [[0, -96], [0, 0]]
                    }
                },
                "o": {"a": 0, "k": 100}
            }],
            "t": {
                "d": {
                    "x": "live_banner",
                    "k": [{"t": 0, "s": {"t": "NISABA ENGINE", "s": 21, "fc": [1.0, 0.88, 0.28], "f": "Sans", "j": 1, "lh": 26}}]
                },
                "p": {"m": 0, "f": {"a": 0, "k": 6}, "l": {"a": 0, "k": 6}, "p": 1, "r": 0, "a": 1},
                "a": [{
                    "nm": "WaveColor",
                    "s": {
                        "s": {"a": 0, "k": 0}, "e": {"a": 0, "k": 45},
                        "o": {"a": 1, "k": [{"t": 0, "s": [-30], "e": [100]}, {"t": 120, "s": [100]}]},
                        "r": 1, "b": 1, "sh": 5, "sm": {"a": 0, "k": 100}
                    },
                    "a": {
                        "p": {"a": 0, "k": [0, -10]},
                        "s": {"a": 0, "k": [122, 122]},
                        "fc": {"a": 0, "k": [0.15, 0.96, 0.82, 1.0]}
                    }
                }]
            }
        },
        {
            "ind": 3, "ty": 6, "nm": "SynthAudio", "refId": "synth_pulse", "ip": 0, "op": 120, "st": 0, "sr": 1,
            "au": {
                "lv": {
                    "a": 1,
                    "k": [
                        {"t": 0, "s": [-14, -18], "e": [-1, -4]},
                        {"t": 30, "s": [-1, -4], "e": [-12, -2]},
                        {"t": 60, "s": [-12, -2], "e": [-2, -1]},
                        {"t": 90, "s": [-2, -1], "e": [-14, -18]},
                        {"t": 120, "s": [-14, -18]}
                    ]
                }
            }
        }
    ]
})JSON";

// ── Complete Interactive 6-Card Showcase Deck ─────────────────────────────

struct AudioMeterState {
    float left_db = -96.0f;
    float right_db = -96.0f;
    float gain = 0.0f;
    float local_time = 0.0f;
    bool active = false;
};

struct Phase1HandlesObserver final : public PropertyObserver {
    std::unique_ptr<ColorHandle> dot_fill;
    std::unique_ptr<TransformHandle> dot_transform;

    void on_color_property(std::string_view keypath, const std::function<std::unique_ptr<ColorHandle>()>& factory) override {
        if (keypath == "OrbitDot.Grp.Fill") dot_fill = factory();
    }
    void on_transform_property(std::string_view keypath, const std::function<std::unique_ptr<TransformHandle>()>& factory) override {
        if (keypath == "OrbitDot") dot_transform = factory();
    }
};

struct ShowcaseDeck {
    // Card 1: Phase 1 (InvalidationController + PropertyObserver + ResourceProvider/Multi-frame/kDeferImageLoading)
    Player p1;
    std::shared_ptr<Phase1HandlesObserver> p1_observer;
    Stats p1_stats{};
    InvalidationController p1_ic;
    std::optional<Rect> p1_last_dirty;
    int p1_palette_idx = 0;

    // Card 2: Phase 2.1 (ExternalLayer + PrecompInterceptor)
    Player p2;

    // Card 3: Phase 2.2 (Color & Tone FX)
    Player p3;
    int color_fx_variant = 0;

    // Card 4: Phase 3.1 (Transitions & Tiling)
    Player p4;
    int wipe_fx_variant = 0;

    // Card 5: Phase 3.2 (Distortion, Blur & Procedural)
    Player p5;
    int distort_fx_variant = 0;

    // Card 6: Phase 4 (ExpressionManager + TextAnimator + Text-on-Path + AudioLayer)
    Player p6;
    AudioMeterState audio_meter;

    // Diagnostics & interactive state
    size_t logger_warnings = 0;
    bool show_damage_overlay = true;
    float speed = 1.0f;
    bool paused = false;

    bool init(const std::shared_ptr<text::FontSystem>& shared_fonts) {
        auto logger = std::make_shared<CallbackLogger>([this](Logger::Level, std::string_view, std::string_view) {
            ++logger_warnings;
        });

        // Card 1 setup
        auto mem_rp = std::make_shared<MemoryResourceProvider>();
        mem_rp->add_image_asset("plasma_sprite", make_animated_plasma_sprite());
        mem_rp->add_audio_asset("synth_pulse", std::make_shared<MemoryAudioAsset>(std::vector<uint8_t>{'R', 'I', 'F', 'F', 1, 2, 3, 4}));

        p1_observer = std::make_shared<Phase1HandlesObserver>();
        auto a1 = AnimationBuilder()
            .set_flags(kDeferImageLoading)
            .set_resource_provider(mem_rp)
            .set_property_observer(p1_observer)
            .set_logger(logger)
            .set_stats(&p1_stats)
            .make(SCENE_PHASE1_DAMAGE_HANDLES);
        if (!a1) return false;
        p1.set_animation(a1);

        // Card 2 setup
        auto a2 = AnimationBuilder()
            .set_precomp_interceptor(make_live_radar_interceptor())
            .set_logger(logger)
            .make(SCENE_PHASE2_EXTERNAL_PRECOMP);
        if (!a2) return false;
        p2.set_animation(a2);

        // Card 3, 4, 5 setup
        if (!reload_color_fx(0)) return false;
        if (!reload_wipe_fx(0)) return false;
        if (!reload_distort_fx(0)) return false;

        // Card 6 setup (ExpressionManager + Text-on-Path + AudioTrack)
        auto expr_mgr = std::make_shared<CallbackExpressionManager>();
        expr_mgr->array_factory = [](std::string_view expr) -> std::shared_ptr<ExpressionEvaluator<std::vector<float>>> {
            if (expr == "live_orbit") {
                return std::make_shared<CallbackExpressionEvaluator<std::vector<float>>>([](float t) {
                    const float a = t * 3.1415926f;
                    return std::vector<float>{120.0f + std::cos(a) * 86.0f, 108.0f + std::sin(a * 2.0f) * 24.0f};
                });
            }
            if (expr == "live_col") {
                return std::make_shared<CallbackExpressionEvaluator<std::vector<float>>>([](float t) {
                    const float s = 0.5f + 0.5f * std::sin(t * 4.0f);
                    return std::vector<float>{0.15f + 0.85f * s, 0.92f - 0.55f * s, 1.0f, 1.0f};
                });
            }
            return nullptr;
        };
        expr_mgr->number_factory = [](std::string_view expr) -> std::shared_ptr<ExpressionEvaluator<float>> {
            if (expr == "live_spin") {
                return std::make_shared<CallbackExpressionEvaluator<float>>([](float t) { return t * 180.0f; });
            }
            return nullptr;
        };
        expr_mgr->string_factory = [](std::string_view expr) -> std::shared_ptr<ExpressionEvaluator<std::string>> {
            if (expr == "live_banner") {
                return std::make_shared<CallbackExpressionEvaluator<std::string>>([](float t) {
                    return (std::fmod(t, 4.0f) < 2.0f) ? std::string("NISABA LOTTIE") : std::string("100% SOVEREIGN");
                });
            }
            return nullptr;
        };

        auto a6 = AnimationBuilder()
            .set_resource_provider(mem_rp)
            .set_expression_manager(expr_mgr)
            .set_logger(logger)
            .make(SCENE_PHASE4_EXPR_TEXT_AUDIO);
        if (!a6) return false;
        if (shared_fonts) {
            a6->set_font_system(shared_fonts);
        }
        p6.set_animation(a6);
        p6.set_audio_track(std::make_shared<CallbackAudioTrack>(
            [this](std::string_view, float local_t, float l_db, float r_db, float gain, bool, const std::shared_ptr<AudioAsset>&) {
                audio_meter.local_time = local_t;
                audio_meter.left_db = l_db;
                audio_meter.right_db = r_db;
                audio_meter.gain = gain;
                audio_meter.active = true;
            },
            [this](std::string_view) {
                audio_meter.active = false;
                audio_meter.gain = 0.0f;
            }
        ));
        p6.seek_frame(0.0f);
        return true;
    }

    bool reload_color_fx(int variant) {
        color_fx_variant = ((variant % 4) + 4) % 4;
        auto a = Animation::load_from_data(make_color_fx_scene(color_fx_variant));
        if (!a) return false;
        p3.set_animation(a);
        p3.set_speed(speed);
        if (paused) p3.pause(); else p3.play();
        return true;
    }

    bool reload_wipe_fx(int variant) {
        wipe_fx_variant = ((variant % 4) + 4) % 4;
        auto a = Animation::load_from_data(make_wipe_fx_scene(wipe_fx_variant));
        if (!a) return false;
        p4.set_animation(a);
        p4.set_speed(speed);
        if (paused) p4.pause(); else p4.play();
        return true;
    }

    bool reload_distort_fx(int variant) {
        distort_fx_variant = ((variant % 5) + 5) % 5;
        auto a = Animation::load_from_data(make_distort_fx_scene(distort_fx_variant));
        if (!a) return false;
        p5.set_animation(a);
        p5.set_speed(speed);
        if (paused) p5.pause(); else p5.play();
        return true;
    }

    void cycle_p1_handle_mutation() {
        static const Color kPalette[4] = {
            Color::from_rgba8(0, 224, 255, 255),
            Color::from_rgba8(255, 195, 35, 255),
            Color::from_rgba8(255, 55, 130, 255),
            Color::from_rgba8(40, 245, 145, 255)
        };
        static const float kScales[4] = {100.0f, 124.0f, 88.0f, 114.0f};
        p1_palette_idx = (p1_palette_idx + 1) % 4;
        if (p1_observer && p1_observer->dot_fill && p1_observer->dot_fill->is_valid()) {
            p1_observer->dot_fill->set(kPalette[p1_palette_idx]);
        }
        if (p1_observer && p1_observer->dot_transform && p1_observer->dot_transform->is_valid()) {
            const float sc = kScales[p1_palette_idx];
            p1_observer->dot_transform->set_scale(Point::from_xy(sc, sc));
        }
    }

    void toggle_pause() {
        paused = !paused;
        Player* all[6] = {&p1, &p2, &p3, &p4, &p5, &p6};
        for (Player* p : all) {
            if (paused) p->pause(); else p->play();
        }
    }

    void restart_all() {
        Player* all[6] = {&p1, &p2, &p3, &p4, &p5, &p6};
        for (Player* p : all) {
            p->seek_frame(0.0f);
            if (!paused) p->play();
        }
    }

    void cycle_speed() {
        if (speed == 1.0f) speed = 2.0f;
        else if (speed == 2.0f) speed = 0.5f;
        else if (speed == 0.5f) speed = 0.25f;
        else speed = 1.0f;
        Player* all[6] = {&p1, &p2, &p3, &p4, &p5, &p6};
        for (Player* p : all) p->set_speed(speed);
    }

    void advance_all(float dt) {
        if (paused) return;
        p1_ic.reset();
        p1.advance(dt, &p1_ic);
        if (auto b = p1_ic.bounds()) {
            p1_last_dirty = *b;
        }
        p2.advance(dt);
        p3.advance(dt);
        p4.advance(dt);
        p5.advance(dt);
        p6.advance(dt);
    }
};

} // namespace nisaba::showcase
