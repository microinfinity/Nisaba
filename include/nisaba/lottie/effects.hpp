#pragma once

/// @file effects.hpp
/// @brief Lottie layer visual effects model and unified CPU/GPU ImageFilter & ColorEffect builder.

#include "nisaba/lottie/lottie_types.hpp"
#include "nisaba/lottie/property.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/effects/color_filter.hpp"
#include "nisaba/effects/image_filter.hpp"
#include "nisaba/math/rect.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nisaba::lottie {

/// Represents a parsed Lottie layer effect (`"ef"`) with animatable parameters.
struct LayerEffect {
    EffectType type = EffectType::Unknown;
    std::string name;
    std::string match_name; // "mn"
    bool enabled = true;

    // ── Blur & DropShadow ────────────────────────────────────────────────
    FloatProperty blurriness{0.0f};            // GaussianBlur (ty=29)
    int blur_dimensions = 1;                   // 1=Both, 2=Horizontal, 3=Vertical
    ColorProperty shadow_color{Color::BLACK};  // DropShadow (ty=25) & Fill (ty=21)
    FloatProperty shadow_opacity{100.0f};      // DropShadow (0..255 or 0..100) / Fill (0..1 or 0..100)
    FloatProperty shadow_direction{135.0f};    // DropShadow angle in degrees
    FloatProperty shadow_distance{5.0f};       // DropShadow distance
    FloatProperty shadow_softness{5.0f};       // DropShadow blur radius
    bool shadow_only = false;                  // DropShadow "Shadow Only"

    // ── Tint, Tritone & CC Toner ─────────────────────────────────────────
    ColorProperty tint_black{Color::BLACK};    // Tint / Tritone / CCToner shadows
    ColorProperty tint_dark{Color::from_rgba8(64, 64, 64, 255)};   // CCToner darktones
    ColorProperty tint_mid{Color::from_rgba8(128, 128, 128, 255)}; // Tritone / CCToner midtones
    ColorProperty tint_bright{Color::from_rgba8(192, 192, 192, 255)}; // CCToner brights
    ColorProperty tint_white{Color::WHITE};    // Tint / Tritone / CCToner highlights
    FloatProperty tint_amount{100.0f};         // Tint amount [0..100] / Tritone & CCToner blend w. original [0..100]
    int toner_mode = 2;                        // CCToner: 1=Duotone, 2=Tritone, 3=Pentone

    // ── Brightness & Contrast ────────────────────────────────────────────
    FloatProperty brightness{0.0f};            // [-150..150]
    FloatProperty contrast{0.0f};              // [-100..100]

    // ── Hue / Saturation ─────────────────────────────────────────────────
    FloatProperty master_hue{0.0f};            // degrees
    FloatProperty master_saturation{0.0f};     // [-100..100]
    FloatProperty master_lightness{0.0f};      // [-100..100]
    bool colorize = false;
    FloatProperty colorize_hue{0.0f};          // degrees
    FloatProperty colorize_saturation{25.0f};  // [0..100]
    FloatProperty colorize_lightness{0.0f};    // [-100..100]

    // ── Black & White ────────────────────────────────────────────────────
    FloatProperty bw_reds{40.0f};
    FloatProperty bw_yellows{60.0f};
    FloatProperty bw_greens{40.0f};
    FloatProperty bw_cyans{60.0f};
    FloatProperty bw_blues{20.0f};
    FloatProperty bw_magentas{80.0f};
    bool bw_tint_enabled = false;
    ColorProperty bw_tint_color{Color::from_rgba8(225, 211, 179, 255)};

    // ── Invert ───────────────────────────────────────────────────────────
    int invert_channel = 1;                    // 1=RGB, 2=R, 3=G, 4=B, 6=Hue, 7=Lightness, 8=Saturation, 16=Alpha
    FloatProperty blend_with_original{0.0f};   // [0..100]%

    // ── Threshold ────────────────────────────────────────────────────────
    FloatProperty threshold_level{128.0f};     // [0..255] or [0..1]

    // ── Shift Channels ───────────────────────────────────────────────────
    int shift_alpha = 1;                       // 1=A, 2=R, 3=G, 4=B, 5=Lum, 9=FullOn, 10=FullOff
    int shift_red = 2;
    int shift_green = 3;
    int shift_blue = 4;

    // ── EasyLevels & ProLevels ───────────────────────────────────────────
    int levels_channel = 1;                    // 1=RGB, 2=R, 3=G, 4=B, 5=A
    FloatProperty levels_in_black{0.0f};
    FloatProperty levels_in_white{1.0f};
    FloatProperty levels_gamma{1.0f};
    FloatProperty levels_out_black{0.0f};
    FloatProperty levels_out_white{1.0f};

    // ── Gradient Ramp (2-color) & 4-Color Gradient ───────────────────────
    PointProperty ramp_start_point{Point::zero()};
    ColorProperty ramp_start_color{Color::BLACK};
    PointProperty ramp_end_point{Point::from_xy(0.0f, 100.0f)};
    ColorProperty ramp_end_color{Color::WHITE};
    int ramp_shape = 1;                        // 1=Linear, 2=Radial
    FloatProperty ramp_scatter{0.0f};

    PointProperty four_pt1{Point::zero()};
    ColorProperty four_col1{Color::RED};
    PointProperty four_pt2{Point::from_xy(100.0f, 0.0f)};
    ColorProperty four_col2{Color::GREEN};
    PointProperty four_pt3{Point::from_xy(0.0f, 100.0f)};
    ColorProperty four_col3{Color::BLUE};
    PointProperty four_pt4{Point::from_xy(100.0f, 100.0f)};
    ColorProperty four_col4{Color::WHITE};
    FloatProperty four_blend{100.0f};
    FloatProperty four_opacity{100.0f};

    // ── Transition & Tiling Effects (LinearWipe, VenetianBlinds, MotionTile, TransformEffect) ──
    FloatProperty wipe_completion{0.0f};       // [0..100]%
    FloatProperty wipe_angle{90.0f};           // degrees
    FloatProperty wipe_feather{0.0f};          // pixels
    FloatProperty blinds_width{20.0f};         // pixels

    PointProperty tile_center{Point::zero()};
    bool has_tile_center = false;
    FloatProperty tile_width{100.0f};          // % of comp width
    FloatProperty tile_height{100.0f};         // % of comp height
    FloatProperty tile_output_width{100.0f};   // % of comp width
    FloatProperty tile_output_height{100.0f};  // % of comp height
    bool tile_mirror_edges = false;
    FloatProperty tile_phase{0.0f};            // degrees
    bool tile_horizontal_phase = false;

    PointProperty fx_anchor{Point::zero()};
    PointProperty fx_position{Point::zero()};
    bool fx_uniform_scale = true;
    FloatProperty fx_scale_height{100.0f};     // %
    FloatProperty fx_scale_width{100.0f};      // %
    FloatProperty fx_skew{0.0f};               // degrees
    FloatProperty fx_skew_axis{0.0f};          // degrees
    FloatProperty fx_rotation{0.0f};           // degrees
    FloatProperty fx_opacity{100.0f};          // [0..100]%

    // ── Distortion, Blur & Procedural Effects (CornerPin, Bulge, Sphere, DirectionalBlur, Sharpen, FractalNoise) ──
    PointProperty pin_upper_left{Point::zero()};
    PointProperty pin_upper_right{Point::from_xy(100.0f, 0.0f)};
    PointProperty pin_lower_left{Point::from_xy(0.0f, 100.0f)};
    PointProperty pin_lower_right{Point::from_xy(100.0f, 100.0f)};

    FloatProperty bulge_radius_x{50.0f};
    FloatProperty bulge_radius_y{50.0f};
    PointProperty bulge_center{Point::zero()};
    bool has_bulge_center = false;
    FloatProperty bulge_height{1.0f};
    FloatProperty bulge_taper{0.0f};

    FloatProperty sphere_radius{50.0f};
    PointProperty sphere_center{Point::zero()};
    bool has_sphere_center = false;

    FloatProperty dir_blur_direction{0.0f};    // degrees
    FloatProperty dir_blur_length{0.0f};       // pixels
    FloatProperty sharpen_amount{0.0f};

    bool fractal_invert = false;
    FloatProperty fractal_contrast{100.0f};    // %
    FloatProperty fractal_brightness{0.0f};    // [-100..100]
    FloatProperty fractal_scale{100.0f};       // pixels / %
    PointProperty fractal_offset{Point::zero()};
    FloatProperty fractal_complexity{4.0f};    // octaves [1..6]
    FloatProperty fractal_sub_influence{50.0f};// %
    FloatProperty fractal_sub_scaling{50.0f};  // %
    FloatProperty fractal_evolution{0.0f};     // degrees
    FloatProperty fractal_opacity{100.0f};     // [0..100]%

    [[nodiscard]] bool is_animated() const noexcept;
    [[nodiscard]] uint64_t visual_fingerprint(float frame) const noexcept;
    void bind_expression_manager(ExpressionManager& mgr);
    static LayerEffect parse(const json::JsonValue& val);
    [[nodiscard]] static bool is_supported_effect_json(const json::JsonValue& val) noexcept;
};

/// Compiles a layer's ordered `LayerEffect` stack into a composable `effects::ImageFilter` DAG
/// that executes with 1:1 parity on both CPU `Canvas` and GPU `GpuCanvas`.
[[nodiscard]] std::optional<effects::ImageFilter> build_layer_effects_filter(
    const std::vector<LayerEffect>& effects,
    float frame,
    float comp_width,
    float comp_height
);

/// Applies a layer's `LayerEffect` stack in-place to an offscreen CPU `Pixmap`
/// using the exact same `ImageFilter` DAG as the direct `ICanvas::save_layer` path.
void apply_layer_effects_pixmap(
    Pixmap& pm,
    const std::vector<LayerEffect>& effects,
    float frame,
    float comp_width,
    float comp_height
);

/// Expands a bounding box to cover blur/shadow halos emitted by active layer effects.
void expand_bounds_for_effects(
    Rect& bounds,
    const std::vector<LayerEffect>& effects,
    float frame
) noexcept;

} // namespace nisaba::lottie
