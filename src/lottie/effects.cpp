/// @file effects.cpp
/// @brief Lottie layer effects parsing and unified CPU/GPU ImageFilter & ColorEffect builder.

#include "nisaba/lottie/effects.hpp"
#include "nisaba/effects/color_matrix.hpp"
#include "nisaba/effects/runtime_filter.hpp"
#include "nisaba/math/transform4x4.hpp"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace nisaba::lottie {

namespace {

inline uint64_t ehash_mix(uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

inline uint64_t ehash_f32(uint64_t h, float f) noexcept {
    uint32_t bits = 0;
    if (f == 0.0f) f = 0.0f;
    std::memcpy(&bits, &f, sizeof(bits));
    return ehash_mix(h, static_cast<uint64_t>(bits));
}

inline uint64_t ehash_pt(uint64_t h, Point p) noexcept {
    return ehash_f32(ehash_f32(h, p.x), p.y);
}

inline uint64_t ehash_col(uint64_t h, Color c) noexcept {
    h = ehash_f32(h, c.red());
    h = ehash_f32(h, c.green());
    h = ehash_f32(h, c.blue());
    return ehash_f32(h, c.alpha());
}

bool icontains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

EffectType classify_effect_type(const json::JsonValue& val) noexcept {
    const std::string_view mn = val.get_string("mn", "");
    const std::string_view nm = val.get_string("nm", "");
    const int ty = val.get_int("ty", -1);

    // 1. Match by canonical match-name ("mn") first
    if (!mn.empty()) {
        if (mn == "ADBE Gaussian Blur 2" || mn == "ADBE Gaussian Blur") return EffectType::GaussianBlur;
        if (mn == "ADBE Drop Shadow") return EffectType::DropShadow;
        if (mn == "ADBE Fill") return EffectType::Fill;
        if (mn == "ADBE Tint") return EffectType::Tint;
        if (mn == "ADBE Tritone") return EffectType::Tritone;
        if (mn == "ADBE Brightness & Contrast 2" || mn == "ADBE Brightness & Contrast") return EffectType::BrightnessContrast;
        if (mn == "ADBE HUE SATURATION") return EffectType::HueSaturation;
        if (mn == "ADBE Black&White" || mn == "ADBE Black & White") return EffectType::BlackAndWhite;
        if (mn == "ADBE Invert") return EffectType::Invert;
        if (mn == "ADBE Threshold2" || mn == "ADBE Threshold") return EffectType::Threshold;
        if (mn == "ADBE Shift Channels") return EffectType::ShiftChannels;
        if (mn == "CC Toner") return EffectType::CCToner;
        if (mn == "ADBE Easy Levels2" || mn == "ADBE Easy Levels") return EffectType::EasyLevels;
        if (mn == "ADBE Pro Levels2" || mn == "ADBE Pro Levels") return EffectType::ProLevels;
        if (mn == "ADBE Ramp") return EffectType::GradientRamp;
        if (mn == "ADBE 4ColorGradient") return EffectType::FourColorGradient;
        if (mn == "ADBE Linear Wipe") return EffectType::LinearWipe;
        if (mn == "ADBE Venetian Blinds") return EffectType::VenetianBlinds;
        if (mn == "ADBE Tile") return EffectType::MotionTile;
        if (mn == "ADBE Geometry2" || mn == "ADBE Geometry") return EffectType::TransformEffect;
        if (mn == "ADBE Corner Pin") return EffectType::CornerPin;
        if (mn == "ADBE Bulge") return EffectType::Bulge;
        if (mn == "ADBE Spherize" || mn == "CC Sphere") return EffectType::Sphere;
        if (mn == "ADBE Motion Blur") return EffectType::DirectionalBlur;
        if (mn == "ADBE Sharpen") return EffectType::Sharpen;
        if (mn == "ADBE Fractal Noise") return EffectType::FractalNoise;
    }

    // 2. Match by legacy numeric Lottie effect type ("ty")
    switch (ty) {
        case 20: return EffectType::Tint;
        case 21: return EffectType::Fill;
        case 23: return EffectType::Tritone;
        case 24: return EffectType::ProLevels;
        case 25: return EffectType::DropShadow;
        case 29: return EffectType::GaussianBlur;
        default: break;
    }

    // 3. Fallback match by effect display name ("nm") for ty == 5 generic effects
    if (ty == 5 && !nm.empty()) {
        if (icontains(nm, "brightness")) return EffectType::BrightnessContrast;
        if (icontains(nm, "hue") && icontains(nm, "sat")) return EffectType::HueSaturation;
        if (icontains(nm, "black") && icontains(nm, "white")) return EffectType::BlackAndWhite;
        if (icontains(nm, "invert")) return EffectType::Invert;
        if (icontains(nm, "threshold")) return EffectType::Threshold;
        if (icontains(nm, "shift channel")) return EffectType::ShiftChannels;
        if (icontains(nm, "toner")) return EffectType::CCToner;
        if (icontains(nm, "level")) return EffectType::EasyLevels;
        if (icontains(nm, "4-color") || icontains(nm, "4 color") || icontains(nm, "four color")) return EffectType::FourColorGradient;
        if (icontains(nm, "ramp") || icontains(nm, "gradient")) return EffectType::GradientRamp;
        if (icontains(nm, "linear wipe")) return EffectType::LinearWipe;
        if (icontains(nm, "venetian")) return EffectType::VenetianBlinds;
        if (icontains(nm, "motion tile")) return EffectType::MotionTile;
        if (icontains(nm, "transform")) return EffectType::TransformEffect;
        if (icontains(nm, "corner pin")) return EffectType::CornerPin;
        if (icontains(nm, "bulge")) return EffectType::Bulge;
        if (icontains(nm, "spher")) return EffectType::Sphere;
        if (icontains(nm, "directional blur")) return EffectType::DirectionalBlur;
        if (icontains(nm, "sharpen")) return EffectType::Sharpen;
        if (icontains(nm, "fractal noise")) return EffectType::FractalNoise;
    }

    return EffectType::Unknown;
}

const json::JsonValue* find_param_v(
    const std::vector<json::JsonValue>& params,
    size_t fallback_index,
    std::string_view mn_suffix = {}
) noexcept {
    if (!mn_suffix.empty()) {
        for (const auto& p : params) {
            if (!p.is_object()) continue;
            const std::string_view pmn = p.get_string("mn", "");
            if (!pmn.empty() && pmn.ends_with(mn_suffix)) {
                if (const auto* v = p.get("v")) return v;
            }
        }
    }
    if (fallback_index < params.size() && params[fallback_index].is_object()) {
        return params[fallback_index].get("v");
    }
    return nullptr;
}

float normalize_01_or_255(float v) noexcept {
    if (v > 1.0f) return std::clamp(v / 255.0f, 0.0f, 1.0f);
    return std::clamp(v, 0.0f, 1.0f);
}

uint8_t to_u8(float v) noexcept {
    return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
}

Color lerp_color(Color a, Color b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color::from_rgba_unchecked(
        a.red() + (b.red() - a.red()) * t,
        a.green() + (b.green() - a.green()) * t,
        a.blue() + (b.blue() - a.blue()) * t,
        a.alpha() + (b.alpha() - a.alpha()) * t
    );
}

void set_shift_channel_row(std::array<float, 20>& mat, int row, int source_enum) noexcept {
    const int base = row * 5;
    mat[base + 0] = 0.0f;
    mat[base + 1] = 0.0f;
    mat[base + 2] = 0.0f;
    mat[base + 3] = 0.0f;
    mat[base + 4] = 0.0f;
    switch (source_enum) {
        case 1: mat[base + 3] = 1.0f; break; // Alpha
        case 2: mat[base + 0] = 1.0f; break; // Red
        case 3: mat[base + 1] = 1.0f; break; // Green
        case 4: mat[base + 2] = 1.0f; break; // Blue
        case 5: // Luminance (BT.709)
            mat[base + 0] = 0.2126f;
            mat[base + 1] = 0.7152f;
            mat[base + 2] = 0.0722f;
            break;
        case 9: mat[base + 4] = 1.0f; break; // Full On
        case 10: break;                      // Full Off
        default:
            // Identity fallback for row (0=R, 1=G, 2=B, 3=A)
            mat[base + row] = 1.0f;
            break;
    }
}

// ── Shared RuntimeFilter Shader Programs (1:1 CPU & GPU) ──────────────────

constexpr const char* kBlackAndWhiteShader = R"(
    uniform shader content;
    uniform float w_red;
    uniform float w_yellow;
    uniform float w_green;
    uniform float w_cyan;
    uniform float w_blue;
    uniform float w_magenta;
    uniform float use_tint;
    uniform vec4 tint_col;
    vec4 main(vec2 pos) {
        vec4 src = content.eval(pos);
        float a = src.a;
        if (a <= 0.0001) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        float inv_a = 1.0 / a;
        float r = clamp(src.r * inv_a, 0.0, 1.0);
        float g = clamp(src.g * inv_a, 0.0, 1.0);
        float b = clamp(src.b * inv_a, 0.0, 1.0);
        float lo = min(r, min(g, b));
        float hi = max(r, max(g, b));
        float mid = r + g + b - lo - hi;
        float wp = w_red;
        float ws = w_yellow;
        if (r >= g && r >= b) {
            wp = w_red;
            ws = (g >= b) ? w_yellow : w_magenta;
        } else if (g >= r && g >= b) {
            wp = w_green;
            ws = (r >= b) ? w_yellow : w_cyan;
        } else {
            wp = w_blue;
            ws = (r >= g) ? w_magenta : w_cyan;
        }
        float gray = clamp(lo + (hi - mid) * wp + (mid - lo) * ws, 0.0, 1.0);
        vec3 out_rgb = vec3(gray, gray, gray);
        if (use_tint > 0.5) {
            out_rgb = clamp(out_rgb * tint_col.rgb + (1.0 - tint_col.rgb) * gray * 0.25, 0.0, 1.0);
        }
        return vec4(out_rgb * a, a);
    }
)";

constexpr const char* kGradientRampShader = R"(
    uniform shader content;
    uniform vec2 start_pt;
    uniform vec2 end_pt;
    uniform vec4 start_col;
    uniform vec4 end_col;
    uniform float shape_radial;
    uniform float blend_orig;
    vec4 main(vec2 pos) {
        vec4 src = content.eval(pos);
        float a = src.a;
        if (a <= 0.0001) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        vec2 delta = end_pt - start_pt;
        float len2 = dot(delta, delta);
        float t = 0.0;
        if (len2 > 0.0001) {
            if (shape_radial > 0.5) {
                t = clamp(length(pos - start_pt) / sqrt(len2), 0.0, 1.0);
            } else {
                t = clamp(dot(pos - start_pt, delta) / len2, 0.0, 1.0);
            }
        }
        vec3 ramp_rgb = mix(start_col.rgb, end_col.rgb, t);
        vec3 orig_rgb = clamp(src.rgb / a, 0.0, 1.0);
        vec3 out_rgb = mix(ramp_rgb, orig_rgb, clamp(blend_orig, 0.0, 1.0));
        return vec4(out_rgb * a, a);
    }
)";

constexpr const char* kFourColorGradientShader = R"(
    uniform shader content;
    uniform vec2 p1;
    uniform vec2 p2;
    uniform vec2 p3;
    uniform vec2 p4;
    uniform vec4 c1;
    uniform vec4 c2;
    uniform vec4 c3;
    uniform vec4 c4;
    uniform float blend_amt;
    uniform float opacity;
    vec4 main(vec2 pos) {
        vec4 src = content.eval(pos);
        float a = src.a;
        if (a <= 0.0001) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        float eps = 1.0 + max(0.0, blend_amt) * 0.5;
        float d1 = length(pos - p1);
        float d2 = length(pos - p2);
        float d3 = length(pos - p3);
        float d4 = length(pos - p4);
        float w1 = 1.0 / (d1 * d1 + eps * eps);
        float w2 = 1.0 / (d2 * d2 + eps * eps);
        float w3 = 1.0 / (d3 * d3 + eps * eps);
        float w4 = 1.0 / (d4 * d4 + eps * eps);
        float wsum = w1 + w2 + w3 + w4;
        vec3 grad_rgb = (c1.rgb * w1 + c2.rgb * w2 + c3.rgb * w3 + c4.rgb * w4) / max(wsum, 0.00001);
        vec3 orig_rgb = clamp(src.rgb / a, 0.0, 1.0);
        vec3 out_rgb = mix(orig_rgb, grad_rgb, clamp(opacity, 0.0, 1.0));
        return vec4(out_rgb * a, a);
    }
)";

constexpr const char* kLinearWipeShader = R"(
    uniform shader content;
    uniform vec2 wipe_dir;
    uniform float min_proj;
    uniform float max_proj;
    uniform float completion;
    uniform float feather;
    vec4 main(vec2 pos) {
        if (completion <= 0.0001) return content.eval(pos);
        if (completion >= 0.9999) return vec4(0.0, 0.0, 0.0, 0.0);
        float p = dot(pos, wipe_dir);
        float cut = mix(min_proj, max_proj, completion);
        float mask = (feather > 0.001)
            ? smoothstep(cut - feather * 0.5, cut + feather * 0.5, p)
            : step(cut, p);
        return content.eval(pos) * mask;
    }
)";

constexpr const char* kVenetianBlindsShader = R"(
    uniform shader content;
    uniform vec2 axis;
    uniform float width;
    uniform float completion;
    uniform float feather;
    vec4 main(vec2 pos) {
        if (completion <= 0.0001) return content.eval(pos);
        if (completion >= 0.9999) return vec4(0.0, 0.0, 0.0, 0.0);
        float w = max(width, 1.0);
        float coord = dot(pos, axis);
        float cell = mod(coord, w);
        if (cell < 0.0) cell += w;
        float dist = abs(cell / w - 0.5) * 2.0;
        float fw = feather / w;
        float mask = (fw > 0.001)
            ? smoothstep(completion - fw * 0.5, completion + fw * 0.5, dist)
            : step(completion, dist);
        return content.eval(pos) * mask;
    }
)";

constexpr const char* kMotionTileShader = R"(
    uniform shader content;
    uniform vec2 comp_size;
    uniform vec2 tile_center;
    uniform vec2 tile_size;
    uniform vec2 out_half;
    uniform float mirror_edges;
    uniform float phase;
    uniform float horiz_phase;
    vec4 main(vec2 pos) {
        vec2 c_mid = comp_size * 0.5;
        vec2 off = abs(pos - c_mid);
        if (off.x > out_half.x || off.y > out_half.y) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        vec2 ts = max(tile_size, vec2(1.0, 1.0));
        vec2 rel = pos - tile_center + ts * 0.5;
        vec2 idx = floor(rel / ts);
        if (abs(phase) > 0.0001) {
            if (horiz_phase > 0.5) {
                if (mod(abs(idx.y), 2.0) >= 0.5) rel.x += phase * ts.x;
            } else {
                if (mod(abs(idx.x), 2.0) >= 0.5) rel.y += phase * ts.y;
            }
            idx = floor(rel / ts);
        }
        vec2 u = (rel - idx * ts) / ts;
        if (mirror_edges > 0.5) {
            if (mod(abs(idx.x), 2.0) >= 0.5) u.x = 1.0 - u.x;
            if (mod(abs(idx.y), 2.0) >= 0.5) u.y = 1.0 - u.y;
        }
        return content.eval(u * comp_size);
    }
)";

constexpr const char* kCornerPinShader = R"(
    uniform shader content;
    uniform vec3 row0;
    uniform vec3 row1;
    uniform vec3 row2;
    uniform vec2 comp_size;
    vec4 main(vec2 pos) {
        float w = row2.x * pos.x + row2.y * pos.y + row2.z;
        if (w <= 0.0001) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        float sx = (row0.x * pos.x + row0.y * pos.y + row0.z) / w;
        float sy = (row1.x * pos.x + row1.y * pos.y + row1.z) / w;
        if (sx < 0.0 || sy < 0.0 || sx > comp_size.x || sy > comp_size.y) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        return content.eval(vec2(sx, sy));
    }
)";

constexpr const char* kBulgeShader = R"(
    uniform shader content;
    uniform vec2 center;
    uniform vec2 radii;
    uniform float pwr;
    vec4 main(vec2 pos) {
        vec2 rxy = max(radii, vec2(0.001, 0.001));
        vec2 d = (pos - center) / rxy;
        float r = length(d);
        if (r >= 1.0 || r <= 0.00001) {
            return content.eval(pos);
        }
        float r_new = pow(r, pwr);
        vec2 src = center + d * (r_new / r) * rxy;
        return content.eval(src);
    }
)";

constexpr const char* kSphereShader = R"(
    uniform shader content;
    uniform vec2 center;
    uniform float radius;
    vec4 main(vec2 pos) {
        float rad = max(radius, 0.001);
        vec2 d = (pos - center) / rad;
        float r = length(d);
        if (r >= 1.0 || r <= 0.00001) {
            return content.eval(pos);
        }
        float z = sqrt(max(0.0, 1.0 - r * r));
        float r_new = atan(r, z) / 1.5707963;
        vec2 src = center + d * (r_new / r) * rad;
        return content.eval(src);
    }
)";

constexpr const char* kDirectionalBlurShader = R"(
    uniform shader content;
    uniform vec2 step_vec;
    vec4 main(vec2 pos) {
        vec4 sum = content.eval(pos);
        sum += content.eval(pos + step_vec);
        sum += content.eval(pos - step_vec);
        sum += content.eval(pos + step_vec * 2.0);
        sum += content.eval(pos - step_vec * 2.0);
        sum += content.eval(pos + step_vec * 3.0);
        sum += content.eval(pos - step_vec * 3.0);
        sum += content.eval(pos + step_vec * 4.0);
        sum += content.eval(pos - step_vec * 4.0);
        return sum * (1.0 / 9.0);
    }
)";

constexpr const char* kFractalNoiseShader = R"(
    uniform shader content;
    uniform vec2 scale_inv;
    uniform vec2 offset;
    uniform float contrast;
    uniform float brightness;
    uniform float invert_noise;
    uniform float complexity;
    uniform float sub_inf;
    uniform float sub_scale_inv;
    uniform float evo_shift;
    uniform float opacity;
    vec4 main(vec2 pos) {
        vec4 src = content.eval(pos);
        float a = src.a;
        if (a <= 0.0001) {
            return vec4(0.0, 0.0, 0.0, 0.0);
        }
        vec2 p = (pos - offset) * scale_inv;
        float total = 0.0;
        float norm = 0.0;
        float amp = 1.0;
        float freq = 1.0;
        for (int oct = 0; oct < 4; oct += 1) {
            float w = clamp(complexity - float(oct), 0.0, 1.0);
            if (w > 0.001) {
                vec2 q = p * freq + vec2(evo_shift * float(oct + 1), evo_shift * 0.618 * float(oct + 1));
                vec2 i0 = floor(q);
                vec2 f = q - i0;
                vec2 u = f * f * (3.0 - 2.0 * f);
                vec2 p00 = fract(i0 * vec2(0.1031, 0.1030));
                float d00 = dot(p00, p00.yx + vec2(33.33, 33.33));
                float h00 = fract((p00.x + p00.y + d00) * (p00.x + d00));
                vec2 p10 = fract((i0 + vec2(1.0, 0.0)) * vec2(0.1031, 0.1030));
                float d10 = dot(p10, p10.yx + vec2(33.33, 33.33));
                float h10 = fract((p10.x + p10.y + d10) * (p10.x + d10));
                vec2 p01 = fract((i0 + vec2(0.0, 1.0)) * vec2(0.1031, 0.1030));
                float d01 = dot(p01, p01.yx + vec2(33.33, 33.33));
                float h01 = fract((p01.x + p01.y + d01) * (p01.x + d01));
                vec2 p11 = fract((i0 + vec2(1.0, 1.0)) * vec2(0.1031, 0.1030));
                float d11 = dot(p11, p11.yx + vec2(33.33, 33.33));
                float h11 = fract((p11.x + p11.y + d11) * (p11.x + d11));
                float n = mix(mix(h00, h10, u.x), mix(h01, h11, u.x), u.y);
                total += n * amp * w;
                norm += amp * w;
            }
            amp *= sub_inf;
            freq *= sub_scale_inv;
        }
        float val = (norm > 0.0001) ? (total / norm) : 0.5;
        val = (val - 0.5) * contrast + 0.5 + brightness;
        if (invert_noise > 0.5) val = 1.0 - val;
        val = clamp(val, 0.0, 1.0);
        vec3 orig_rgb = clamp(src.rgb / a, 0.0, 1.0);
        vec3 out_rgb = mix(orig_rgb, vec3(val, val, val), clamp(opacity, 0.0, 1.0));
        return vec4(out_rgb * a, a);
    }
)";

// ── Compiled Native C++20 CPU Fast-Path Helpers (Exact 1:1 GPU Shader Parity) ──

inline float smoothstep_f(float e0, float e1, float x) noexcept {
    const float den = e1 - e0;
    const float t = (std::abs(den) > 1e-8f) ? std::clamp((x - e0) / den, 0.0f, 1.0f) : 0.0f;
    return t * t * (3.0f - 2.0f * t);
}

inline float fract_f(float x) noexcept {
    return x - std::floor(x);
}

inline PremultipliedColorU8 pack_premul_f(float r, float g, float b, float a) noexcept {
    const float ac = std::clamp(a, 0.0f, 1.0f);
    const float rc = std::clamp(r, 0.0f, ac);
    const float gc = std::clamp(g, 0.0f, ac);
    const float bc = std::clamp(b, 0.0f, ac);
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(rc * 255.0f + 0.5f),
        static_cast<uint8_t>(gc * 255.0f + 0.5f),
        static_cast<uint8_t>(bc * 255.0f + 0.5f),
        static_cast<uint8_t>(ac * 255.0f + 0.5f)
    );
}

inline void sample_bilinear_dev_f(
    const PixmapRef& src,
    float dev_x,
    float dev_y,
    int32_t roi_ox,
    int32_t roi_oy,
    int32_t w,
    int32_t h,
    float& out_r,
    float& out_g,
    float& out_b,
    float& out_a
) noexcept {
    const float f_ox = static_cast<float>(roi_ox);
    const float f_oy = static_cast<float>(roi_oy);
    if (dev_x < f_ox || dev_x > static_cast<float>(roi_ox + w) ||
        dev_y < f_oy || dev_y > static_cast<float>(roi_oy + h)) {
        out_r = out_g = out_b = out_a = 0.0f;
        return;
    }
    const float rx = dev_x - f_ox - 0.5f;
    const float ry = dev_y - f_oy - 0.5f;
    const int32_t x0 = static_cast<int32_t>(std::floor(rx));
    const int32_t y0 = static_cast<int32_t>(std::floor(ry));
    const float fx = rx - static_cast<float>(x0);
    const float fy = ry - static_cast<float>(y0);

    const int32_t cx0 = std::clamp(x0, 0, w - 1);
    const int32_t cx1 = std::clamp(x0 + 1, 0, w - 1);
    const int32_t cy0 = std::clamp(y0, 0, h - 1);
    const int32_t cy1 = std::clamp(y0 + 1, 0, h - 1);

    const auto* r0 = src.row(static_cast<size_t>(cy0));
    const auto* r1 = src.row(static_cast<size_t>(cy1));
    const auto p00 = r0[cx0];
    const auto p10 = r0[cx1];
    const auto p01 = r1[cx0];
    const auto p11 = r1[cx1];

    const float w00 = (1.0f - fx) * (1.0f - fy) * (1.0f / 255.0f);
    const float w10 = fx * (1.0f - fy) * (1.0f / 255.0f);
    const float w01 = (1.0f - fx) * fy * (1.0f / 255.0f);
    const float w11 = fx * fy * (1.0f / 255.0f);

    out_r = p00.r * w00 + p10.r * w10 + p01.r * w01 + p11.r * w11;
    out_g = p00.g * w00 + p10.g * w10 + p01.g * w01 + p11.g * w11;
    out_b = p00.b * w00 + p10.b * w10 + p01.b * w01 + p11.b * w11;
    out_a = p00.a * w00 + p10.a * w10 + p01.a * w01 + p11.a * w11;
}

inline PremultipliedColorU8 sample_bilinear_local_u8(
    const PixmapRef& src,
    float local_x,
    float local_y,
    int32_t roi_ox,
    int32_t roi_oy,
    int32_t w,
    int32_t h,
    const Transform& ts
) noexcept {
    Point dp = Point::from_xy(local_x, local_y);
    ts.map_point(dp);
    float r, g, b, a;
    sample_bilinear_dev_f(src, dp.x, dp.y, roi_ox, roi_oy, w, h, r, g, b, a);
    return pack_premul_f(r, g, b, a);
}

effects::ImageFilter make_lottie_runtime_filter(
    const char* shader_src,
    const effects::RuntimeFilterUniforms& uniforms,
    effects::CpuRuntimeFilterFn cpu_kernel,
    std::optional<effects::ImageFilter> input
) {
    static std::mutex s_cache_mu;
    static std::unordered_map<const char*, std::shared_ptr<effects::RuntimeFilterSpec>> s_base_specs;

    std::shared_ptr<effects::RuntimeFilterSpec> base;
    {
        std::lock_guard<std::mutex> lock(s_cache_mu);
        auto it = s_base_specs.find(shader_src);
        if (it != s_base_specs.end()) {
            base = it->second;
        } else {
            base = effects::parse_runtime_filter_source(shader_src, nullptr, "content");
            s_base_specs.emplace(shader_src, base);
        }
    }
    if (!base) {
        return effects::ImageFilter::runtime_shader(shader_src, uniforms, "content", std::move(input));
    }
    auto spec = effects::clone_runtime_filter_with_uniforms(*base, uniforms, std::move(cpu_kernel));
    return effects::ImageFilter::runtime_shader(std::move(spec), std::move(input));
}

std::optional<effects::ImageFilter> build_single_effect_filter(
    const LayerEffect& eff,
    float frame,
    float comp_width,
    float comp_height,
    std::optional<effects::ImageFilter> input
) {
    if (!eff.enabled) return input;

    switch (eff.type) {
        case EffectType::GaussianBlur: {
            const float blur_val = eff.blurriness.evaluate(frame);
            if (blur_val > 0.1f) {
                const float sigma = blur_val * 0.35f;
                const float sx = (eff.blur_dimensions != 3) ? sigma : 0.0f;
                const float sy = (eff.blur_dimensions != 2) ? sigma : 0.0f;
                if (sx > 0.001f || sy > 0.001f) {
                    return effects::ImageFilter::blur(sx, sy, std::move(input));
                }
            }
            return input;
        }

        case EffectType::DropShadow: {
            Color sc = eff.shadow_color.evaluate(frame);
            const float sop = eff.shadow_opacity.evaluate(frame);
            const float alpha_scale = (sop > 100.0f) ? (sop / 255.0f) : (sop / 100.0f);
            sc.apply_opacity(std::clamp(alpha_scale, 0.0f, 1.0f));

            const float angle_rad = (eff.shadow_direction.evaluate(frame) - 90.0f) * static_cast<float>(M_PI / 180.0);
            const float dist = eff.shadow_distance.evaluate(frame);
            const float dx = std::cos(angle_rad) * dist;
            const float dy = std::sin(angle_rad) * dist;
            const float sigma = std::max(0.0f, eff.shadow_softness.evaluate(frame) * 0.4f);
            return effects::ImageFilter::drop_shadow(dx, dy, sigma, sc, eff.shadow_only, std::move(input));
        }

        case EffectType::Fill: {
            const Color fc = eff.shadow_color.evaluate(frame);
            float fop = eff.shadow_opacity.evaluate(frame);
            if (fop > 1.0f) fop *= 0.01f;
            fop = std::clamp(fop, 0.0f, 1.0f);

            const std::array<float, 20> mat = {
                0.0f, 0.0f, 0.0f, 0.0f, fc.red(),
                0.0f, 0.0f, 0.0f, 0.0f, fc.green(),
                0.0f, 0.0f, 0.0f, 0.0f, fc.blue(),
                0.0f, 0.0f, 0.0f, fop,  0.0f
            };
            return effects::ImageFilter::color_filter(
                effects::ColorEffect::from_matrix(effects::ColorMatrix(mat)),
                std::move(input)
            );
        }

        case EffectType::Tint: {
            const Color cb = eff.tint_black.evaluate(frame);
            const Color cw = eff.tint_white.evaluate(frame);
            const float amt = std::clamp(eff.tint_amount.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            if (amt <= 1e-4f) return input;

            const float dr = cw.red() - cb.red();
            const float dg = cw.green() - cb.green();
            const float db = cw.blue() - cb.blue();
            const float inv = 1.0f - amt;
            const std::array<float, 20> mat = {
                inv + amt * dr * 0.2126f, amt * dr * 0.7152f,       amt * dr * 0.0722f,       0.0f, amt * cb.red(),
                amt * dg * 0.2126f,       inv + amt * dg * 0.7152f, amt * dg * 0.0722f,       0.0f, amt * cb.green(),
                amt * db * 0.2126f,       amt * db * 0.7152f,       inv + amt * db * 0.0722f, 0.0f, amt * cb.blue(),
                0.0f,                     0.0f,                     0.0f,                     1.0f, 0.0f
            };
            return effects::ImageFilter::color_filter(
                effects::ColorEffect::from_matrix(effects::ColorMatrix(mat)),
                std::move(input)
            );
        }

        case EffectType::Tritone:
        case EffectType::CCToner: {
            const Color c_sh = eff.tint_black.evaluate(frame);
            const Color c_dk = eff.tint_dark.evaluate(frame);
            const Color c_md = eff.tint_mid.evaluate(frame);
            const Color c_br = eff.tint_bright.evaluate(frame);
            const Color c_hi = eff.tint_white.evaluate(frame);

            // In AE Tritone & CC Toner, parameter is "Blend With Original" [0..100]: 0% = full effect, 100% = original
            const float blend_orig = std::clamp(eff.tint_amount.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            const float effect_weight = 1.0f - blend_orig;
            if (effect_weight <= 1e-4f) return input;

            const int mode = (eff.type == EffectType::Tritone) ? 2 : eff.toner_mode;
            std::array<uint8_t, 256> lr{}, lg{}, lb{};
            for (int i = 0; i < 256; ++i) {
                const float t = static_cast<float>(i) / 255.0f;
                Color mapped = c_md;
                if (mode == 1) {
                    // Duotone: shadows -> highlights
                    mapped = lerp_color(c_sh, c_hi, t);
                } else if (mode == 3) {
                    // Pentone: 5 stops at 0.0, 0.25, 0.5, 0.75, 1.0
                    if (t < 0.25f) mapped = lerp_color(c_sh, c_dk, t * 4.0f);
                    else if (t < 0.50f) mapped = lerp_color(c_dk, c_md, (t - 0.25f) * 4.0f);
                    else if (t < 0.75f) mapped = lerp_color(c_md, c_br, (t - 0.50f) * 4.0f);
                    else mapped = lerp_color(c_br, c_hi, (t - 0.75f) * 4.0f);
                } else {
                    // Tritone: shadows -> midtones -> highlights
                    if (t < 0.5f) mapped = lerp_color(c_sh, c_md, t * 2.0f);
                    else mapped = lerp_color(c_md, c_hi, (t - 0.5f) * 2.0f);
                }
                lr[i] = to_u8(mapped.red());
                lg[i] = to_u8(mapped.green());
                lb[i] = to_u8(mapped.blue());
            }

            const auto lut_fx = effects::ColorEffect::table_argb(nullptr, lr.data(), lg.data(), lb.data());
            auto toned = effects::ColorEffect::compose(lut_fx, effects::ColorEffect::grayscale());
            if (!toned.has_value()) return input;

            effects::ColorEffect final_cf = *toned;
            if (blend_orig > 1e-4f) {
                final_cf = effects::ColorEffect::lerp(
                    effect_weight,
                    effects::ColorEffect::from_matrix(effects::ColorMatrix::identity()),
                    *toned
                );
            }
            return effects::ImageFilter::color_filter(final_cf, std::move(input));
        }

        case EffectType::BrightnessContrast: {
            const float b_raw = eff.brightness.evaluate(frame);
            const float c_raw = eff.contrast.evaluate(frame);
            if (std::abs(b_raw) < 1e-4f && std::abs(c_raw) < 1e-4f) return input;

            const float b = std::clamp(b_raw, -150.0f, 150.0f) / 100.0f;
            const float c_norm = std::clamp(c_raw, -100.0f, 100.0f) / 100.0f;
            const float c_slope = (c_norm >= 0.0f) ? (1.0f + c_norm) : std::max(0.0f, 1.0f + c_norm);

            const effects::ColorMatrix mat = effects::ColorMatrix::contrast(c_slope)
                                           * effects::ColorMatrix::brightness(b);
            return effects::ImageFilter::color_filter(effects::ColorEffect::from_matrix(mat), std::move(input));
        }

        case EffectType::HueSaturation: {
            effects::ColorMatrix m{};
            for (float& v : m.m) v = 0.0f;
            m.m[18] = 1.0f; // preserve alpha

            if (eff.colorize) {
                const float ch = eff.colorize_hue.evaluate(frame) / 360.0f;
                const float cs = std::clamp(eff.colorize_saturation.evaluate(frame) * 0.01f, 0.0f, 1.0f);
                const float cl = std::clamp(eff.colorize_lightness.evaluate(frame) * 0.01f, -1.0f, 1.0f);
                m.m[4] = ch;
                m.m[9] = cs;
                m.m[12] = 1.0f;
                m.m[14] = cl;
            } else {
                const float dh = eff.master_hue.evaluate(frame) / 360.0f;
                const float ds = eff.master_saturation.evaluate(frame) * 0.01f;
                const float dl = eff.master_lightness.evaluate(frame) * 0.01f;
                if (std::abs(dh) < 1e-5f && std::abs(ds) < 1e-5f && std::abs(dl) < 1e-5f) {
                    return input;
                }
                m.m[0] = 1.0f;
                m.m[4] = dh;
                m.m[6] = std::max(0.0f, 1.0f + ds);
                m.m[12] = 1.0f;
                m.m[14] = dl;
            }
            return effects::ImageFilter::color_filter(effects::ColorEffect::hsla_matrix(m), std::move(input));
        }

        case EffectType::BlackAndWhite: {
            const float w_red = eff.bw_reds.evaluate(frame) * 0.01f;
            const float w_yellow = eff.bw_yellows.evaluate(frame) * 0.01f;
            const float w_green = eff.bw_greens.evaluate(frame) * 0.01f;
            const float w_cyan = eff.bw_cyans.evaluate(frame) * 0.01f;
            const float w_blue = eff.bw_blues.evaluate(frame) * 0.01f;
            const float w_magenta = eff.bw_magentas.evaluate(frame) * 0.01f;
            const bool use_tint = eff.bw_tint_enabled;
            const Color tc = eff.bw_tint_color.evaluate(frame);

            effects::RuntimeFilterUniforms u;
            u.set_float("w_red", w_red)
             .set_float("w_yellow", w_yellow)
             .set_float("w_green", w_green)
             .set_float("w_cyan", w_cyan)
             .set_float("w_blue", w_blue)
             .set_float("w_magenta", w_magenta)
             .set_float("use_tint", use_tint ? 1.0f : 0.0f)
             .set_vec4("tint_col", tc.red(), tc.green(), tc.blue(), tc.alpha());

            auto cpu_fn = [w_red, w_yellow, w_green, w_cyan, w_blue, w_magenta, use_tint, tc](
                const PixmapRef& src, PixmapMut& dst, int32_t, int32_t,
                const Transform&, const Transform&, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                constexpr float inv255 = 1.0f / 255.0f;
                const float tr = tc.red(), tg = tc.green(), tb = tc.blue();
                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    for (int32_t x = cx0; x < cx1; ++x) {
                        const auto sp = srow[x];
                        if (sp.a == 0) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float a = sp.a * inv255;
                        const float inv_a = 1.0f / a;
                        const float r = std::clamp(sp.r * inv255 * inv_a, 0.0f, 1.0f);
                        const float g = std::clamp(sp.g * inv255 * inv_a, 0.0f, 1.0f);
                        const float b = std::clamp(sp.b * inv255 * inv_a, 0.0f, 1.0f);
                        const float lo = std::min(r, std::min(g, b));
                        const float hi = std::max(r, std::max(g, b));
                        const float mid = r + g + b - lo - hi;
                        float wp, ws;
                        if (r >= g && r >= b) {
                            wp = w_red;
                            ws = (g >= b) ? w_yellow : w_magenta;
                        } else if (g >= r && g >= b) {
                            wp = w_green;
                            ws = (r >= b) ? w_yellow : w_cyan;
                        } else {
                            wp = w_blue;
                            ws = (r >= g) ? w_magenta : w_cyan;
                        }
                        const float gray = std::clamp(lo + (hi - mid) * wp + (mid - lo) * ws, 0.0f, 1.0f);
                        float or_ = gray, og = gray, ob = gray;
                        if (use_tint) {
                            or_ = std::clamp(gray * tr + (1.0f - tr) * gray * 0.25f, 0.0f, 1.0f);
                            og  = std::clamp(gray * tg + (1.0f - tg) * gray * 0.25f, 0.0f, 1.0f);
                            ob  = std::clamp(gray * tb + (1.0f - tb) * gray * 0.25f, 0.0f, 1.0f);
                        }
                        drow[x] = pack_premul_f(or_ * a, og * a, ob * a, a);
                    }
                }
            };
            return make_lottie_runtime_filter(kBlackAndWhiteShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::Invert: {
            const float blend_orig = std::clamp(eff.blend_with_original.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            const float amt = 1.0f - blend_orig;
            if (amt <= 1e-4f) return input;

            // HLS channel inversions (6=Hue, 7=Lightness, 8=Saturation)
            if (eff.invert_channel == 6 || eff.invert_channel == 7 || eff.invert_channel == 8) {
                effects::ColorMatrix m = effects::ColorMatrix::identity();
                if (eff.invert_channel == 6) {
                    m.m[4] = 0.5f; // 180 degree hue shift
                } else if (eff.invert_channel == 7) {
                    m.m[12] = -1.0f;
                    m.m[14] = 1.0f;
                } else {
                    m.m[6] = -1.0f;
                    m.m[9] = 1.0f;
                }
                effects::ColorEffect hls_fx = effects::ColorEffect::hsla_matrix(m);
                if (blend_orig > 1e-4f) {
                    hls_fx = effects::ColorEffect::lerp(
                        amt,
                        effects::ColorEffect::from_matrix(effects::ColorMatrix::identity()),
                        hls_fx
                    );
                }
                return effects::ImageFilter::color_filter(hls_fx, std::move(input));
            }

            effects::ColorMatrix mat = effects::ColorMatrix::identity();
            const float diag = 1.0f - 2.0f * amt;
            auto inv_row = [&](int r) {
                mat.m[r * 5 + r] = diag;
                mat.m[r * 5 + 4] = amt;
            };
            switch (eff.invert_channel) {
                case 2: inv_row(0); break; // Red
                case 3: inv_row(1); break; // Green
                case 4: inv_row(2); break; // Blue
                case 16: inv_row(3); break; // Alpha
                case 1:
                default:
                    inv_row(0);
                    inv_row(1);
                    inv_row(2);
                    break;
            }
            return effects::ImageFilter::color_filter(effects::ColorEffect::from_matrix(mat), std::move(input));
        }

        case EffectType::Threshold: {
            const float lvl = normalize_01_or_255(eff.threshold_level.evaluate(frame));
            const uint8_t thresh_u8 = to_u8(lvl);
            std::array<uint8_t, 256> lut{};
            for (int i = 0; i < 256; ++i) {
                lut[i] = (static_cast<uint8_t>(i) >= thresh_u8) ? 255u : 0u;
            }
            auto composed = effects::ColorEffect::compose(
                effects::ColorEffect::table(lut),
                effects::ColorEffect::grayscale()
            );
            if (!composed.has_value()) return input;
            return effects::ImageFilter::color_filter(*composed, std::move(input));
        }

        case EffectType::ShiftChannels: {
            std::array<float, 20> mat{};
            set_shift_channel_row(mat, 0, eff.shift_red);
            set_shift_channel_row(mat, 1, eff.shift_green);
            set_shift_channel_row(mat, 2, eff.shift_blue);
            set_shift_channel_row(mat, 3, eff.shift_alpha);
            return effects::ImageFilter::color_filter(
                effects::ColorEffect::from_matrix(effects::ColorMatrix(mat)),
                std::move(input)
            );
        }

        case EffectType::EasyLevels:
        case EffectType::ProLevels: {
            const float in_b = normalize_01_or_255(eff.levels_in_black.evaluate(frame));
            const float in_w = normalize_01_or_255(eff.levels_in_white.evaluate(frame));
            const float gamma = std::max(0.01f, eff.levels_gamma.evaluate(frame));
            const float out_b = normalize_01_or_255(eff.levels_out_black.evaluate(frame));
            const float out_w = normalize_01_or_255(eff.levels_out_white.evaluate(frame));

            const float in_span = std::max(in_w - in_b, 1e-5f);
            const float inv_gamma = 1.0f / gamma;

            std::array<uint8_t, 256> curve{};
            for (int i = 0; i < 256; ++i) {
                const float x = static_cast<float>(i) / 255.0f;
                const float t = std::clamp((x - in_b) / in_span, 0.0f, 1.0f);
                const float g = std::pow(t, inv_gamma);
                const float y = out_b + g * (out_w - out_b);
                curve[i] = to_u8(y);
            }

            const uint8_t* la = nullptr;
            const uint8_t* lr = nullptr;
            const uint8_t* lg = nullptr;
            const uint8_t* lb = nullptr;
            switch (eff.levels_channel) {
                case 2: lr = curve.data(); break;
                case 3: lg = curve.data(); break;
                case 4: lb = curve.data(); break;
                case 5: la = curve.data(); break;
                case 1:
                default:
                    lr = lg = lb = curve.data();
                    break;
            }
            return effects::ImageFilter::color_filter(
                effects::ColorEffect::table_argb(la, lr, lg, lb),
                std::move(input)
            );
        }

        case EffectType::GradientRamp: {
            const Point sp = eff.ramp_start_point.evaluate(frame);
            const Point ep = eff.ramp_end_point.evaluate(frame);
            const Color sc = eff.ramp_start_color.evaluate(frame);
            const Color ec = eff.ramp_end_color.evaluate(frame);
            const float blend_orig = std::clamp(eff.blend_with_original.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            const bool radial = (eff.ramp_shape == 2);

            effects::RuntimeFilterUniforms u;
            u.set_vec2("start_pt", sp.x, sp.y)
             .set_vec2("end_pt", ep.x, ep.y)
             .set_vec4("start_col", sc.red(), sc.green(), sc.blue(), sc.alpha())
             .set_vec4("end_col", ec.red(), ec.green(), ec.blue(), ec.alpha())
             .set_float("shape_radial", radial ? 1.0f : 0.0f)
             .set_float("blend_orig", blend_orig);

            auto cpu_fn = [sp, ep, sc, ec, radial, blend_orig](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform&, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                constexpr float inv255 = 1.0f / 255.0f;
                const float dx = ep.x - sp.x, dy = ep.y - sp.y;
                const float len2 = dx * dx + dy * dy;
                const float inv_len = (len2 > 1e-4f) ? (1.0f / std::sqrt(len2)) : 0.0f;
                const float inv_len2 = (len2 > 1e-4f) ? (1.0f / len2) : 0.0f;
                const float sr = sc.red(), sg = sc.green(), sb = sc.blue();
                const float dr = ec.red() - sr, dg = ec.green() - sg, db = ec.blue() - sb;
                const float inv_bo = 1.0f - blend_orig;
                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    const float dev_y = static_cast<float>(roi_oy + y) + 0.5f;
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, dev_y);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        const auto px = srow[x];
                        if (px.a == 0) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float a = px.a * inv255;
                        float t = 0.0f;
                        if (len2 > 1e-4f) {
                            const float rx = lp.x - sp.x, ry = lp.y - sp.y;
                            t = radial ? std::clamp(std::hypot(rx, ry) * inv_len, 0.0f, 1.0f)
                                       : std::clamp((rx * dx + ry * dy) * inv_len2, 0.0f, 1.0f);
                        }
                        const float rr = sr + dr * t;
                        const float rg = sg + dg * t;
                        const float rb = sb + db * t;
                        if (blend_orig <= 1e-4f) {
                            drow[x] = pack_premul_f(rr * a, rg * a, rb * a, a);
                        } else {
                            const float inv_a = 1.0f / a;
                            const float or_ = std::clamp(px.r * inv255 * inv_a, 0.0f, 1.0f);
                            const float og  = std::clamp(px.g * inv255 * inv_a, 0.0f, 1.0f);
                            const float ob  = std::clamp(px.b * inv255 * inv_a, 0.0f, 1.0f);
                            drow[x] = pack_premul_f((rr * inv_bo + or_ * blend_orig) * a,
                                                    (rg * inv_bo + og  * blend_orig) * a,
                                                    (rb * inv_bo + ob  * blend_orig) * a, a);
                        }
                    }
                }
            };
            return make_lottie_runtime_filter(kGradientRampShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::FourColorGradient: {
            const Point p1 = eff.four_pt1.evaluate(frame);
            const Point p2 = eff.four_pt2.evaluate(frame);
            const Point p3 = eff.four_pt3.evaluate(frame);
            const Point p4 = eff.four_pt4.evaluate(frame);
            const Color c1 = eff.four_col1.evaluate(frame);
            const Color c2 = eff.four_col2.evaluate(frame);
            const Color c3 = eff.four_col3.evaluate(frame);
            const Color c4 = eff.four_col4.evaluate(frame);
            const float blend_amt = eff.four_blend.evaluate(frame);
            float op = eff.four_opacity.evaluate(frame);
            if (op > 1.0f) op *= 0.01f;
            op = std::clamp(op, 0.0f, 1.0f);

            effects::RuntimeFilterUniforms u;
            u.set_vec2("p1", p1.x, p1.y)
             .set_vec2("p2", p2.x, p2.y)
             .set_vec2("p3", p3.x, p3.y)
             .set_vec2("p4", p4.x, p4.y)
             .set_vec4("c1", c1.red(), c1.green(), c1.blue(), c1.alpha())
             .set_vec4("c2", c2.red(), c2.green(), c2.blue(), c2.alpha())
             .set_vec4("c3", c3.red(), c3.green(), c3.blue(), c3.alpha())
             .set_vec4("c4", c4.red(), c4.green(), c4.blue(), c4.alpha())
             .set_float("blend_amt", blend_amt)
             .set_float("opacity", op);

            auto cpu_fn = [p1, p2, p3, p4, c1, c2, c3, c4, blend_amt, op](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform&, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                constexpr float inv255 = 1.0f / 255.0f;
                const float eps = 1.0f + std::max(0.0f, blend_amt) * 0.5f;
                const float eps2 = eps * eps;
                const float inv_op = 1.0f - op;
                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    const float dev_y = static_cast<float>(roi_oy + y) + 0.5f;
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, dev_y);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        const auto px = srow[x];
                        if (px.a == 0) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float dx1 = lp.x - p1.x, dy1 = lp.y - p1.y;
                        const float dx2 = lp.x - p2.x, dy2 = lp.y - p2.y;
                        const float dx3 = lp.x - p3.x, dy3 = lp.y - p3.y;
                        const float dx4 = lp.x - p4.x, dy4 = lp.y - p4.y;
                        const float w1 = 1.0f / (dx1 * dx1 + dy1 * dy1 + eps2);
                        const float w2 = 1.0f / (dx2 * dx2 + dy2 * dy2 + eps2);
                        const float w3 = 1.0f / (dx3 * dx3 + dy3 * dy3 + eps2);
                        const float w4 = 1.0f / (dx4 * dx4 + dy4 * dy4 + eps2);
                        const float inv_w = 1.0f / std::max(w1 + w2 + w3 + w4, 1e-5f);
                        const float gr = (c1.red() * w1 + c2.red() * w2 + c3.red() * w3 + c4.red() * w4) * inv_w;
                        const float gg = (c1.green() * w1 + c2.green() * w2 + c3.green() * w3 + c4.green() * w4) * inv_w;
                        const float gb = (c1.blue() * w1 + c2.blue() * w2 + c3.blue() * w3 + c4.blue() * w4) * inv_w;
                        const float a = px.a * inv255;
                        if (op >= 0.9999f) {
                            drow[x] = pack_premul_f(gr * a, gg * a, gb * a, a);
                        } else {
                            const float inv_a = 1.0f / a;
                            const float or_ = std::clamp(px.r * inv255 * inv_a, 0.0f, 1.0f);
                            const float og  = std::clamp(px.g * inv255 * inv_a, 0.0f, 1.0f);
                            const float ob  = std::clamp(px.b * inv255 * inv_a, 0.0f, 1.0f);
                            drow[x] = pack_premul_f((or_ * inv_op + gr * op) * a,
                                                    (og  * inv_op + gg * op) * a,
                                                    (ob  * inv_op + gb * op) * a, a);
                        }
                    }
                }
            };
            return make_lottie_runtime_filter(kFourColorGradientShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::LinearWipe: {
            const float comp = std::clamp(eff.wipe_completion.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            if (comp <= 1e-4f) return input;
            const float ang = eff.wipe_angle.evaluate(frame) * static_cast<float>(M_PI / 180.0);
            const float dx = std::sin(ang);
            const float dy = -std::cos(ang);
            const float corners[4] = {
                0.0f,
                dx * comp_width,
                dy * comp_height,
                dx * comp_width + dy * comp_height
            };
            const float min_p = std::min({corners[0], corners[1], corners[2], corners[3]});
            const float max_p = std::max({corners[0], corners[1], corners[2], corners[3]});
            const float feather = std::max(0.0f, eff.wipe_feather.evaluate(frame));

            effects::RuntimeFilterUniforms u;
            u.set_vec2("wipe_dir", dx, dy)
             .set_float("min_proj", min_p)
             .set_float("max_proj", max_p)
             .set_float("completion", comp)
             .set_float("feather", feather);

            auto cpu_fn = [dx, dy, min_p, max_p, comp, feather](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform&, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                if (comp >= 0.9999f) {
                    for (int32_t y = cy0; y < cy1; ++y) {
                        std::fill_n(dst.row(static_cast<size_t>(y)) + cx0, cx1 - cx0, PremultipliedColorU8::TRANSPARENT);
                    }
                    return;
                }
                const float cut = min_p + (max_p - min_p) * comp;
                const float e0 = cut - feather * 0.5f;
                const float e1 = cut + feather * 0.5f;
                const bool use_feather = (feather > 0.001f);
                const float dproj_dx = inv_ts.sx * dx + inv_ts.ky * dy;

                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    float proj = lp.x * dx + lp.y * dy;
                    for (int32_t x = cx0; x < cx1; ++x, proj += dproj_dx) {
                        const auto sp = srow[x];
                        if (sp.a == 0) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float mask = use_feather ? smoothstep_f(e0, e1, proj) : ((proj < cut) ? 0.0f : 1.0f);
                        if (mask <= 0.0f) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                        } else if (mask >= 1.0f) {
                            drow[x] = sp;
                        } else {
                            drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                                static_cast<uint8_t>(sp.r * mask + 0.5f),
                                static_cast<uint8_t>(sp.g * mask + 0.5f),
                                static_cast<uint8_t>(sp.b * mask + 0.5f),
                                static_cast<uint8_t>(sp.a * mask + 0.5f)
                            );
                        }
                    }
                }
            };
            return make_lottie_runtime_filter(kLinearWipeShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::VenetianBlinds: {
            const float comp = std::clamp(eff.wipe_completion.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            if (comp <= 1e-4f) return input;
            const float ang = eff.wipe_angle.evaluate(frame) * static_cast<float>(M_PI / 180.0);
            const float ax = std::cos(ang), ay = std::sin(ang);
            const float width = std::max(1.0f, eff.blinds_width.evaluate(frame));
            const float feather = std::max(0.0f, eff.wipe_feather.evaluate(frame));

            effects::RuntimeFilterUniforms u;
            u.set_vec2("axis", ax, ay)
             .set_float("width", width)
             .set_float("completion", comp)
             .set_float("feather", feather);

            auto cpu_fn = [ax, ay, width, comp, feather](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform&, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                if (comp >= 0.9999f) {
                    for (int32_t y = cy0; y < cy1; ++y) {
                        std::fill_n(dst.row(static_cast<size_t>(y)) + cx0, cx1 - cx0, PremultipliedColorU8::TRANSPARENT);
                    }
                    return;
                }
                const float inv_w = 1.0f / width;
                const float fw = feather * inv_w;
                const bool use_f = (fw > 0.001f);
                const float e0 = comp - fw * 0.5f;
                const float e1 = comp + fw * 0.5f;
                const float dcoord_dx = inv_ts.sx * ax + inv_ts.ky * ay;

                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    float coord = lp.x * ax + lp.y * ay;
                    for (int32_t x = cx0; x < cx1; ++x, coord += dcoord_dx) {
                        const auto sp = srow[x];
                        if (sp.a == 0) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float cell = coord - width * std::floor(coord * inv_w);
                        const float dist = std::abs(cell * inv_w - 0.5f) * 2.0f;
                        const float mask = use_f ? smoothstep_f(e0, e1, dist) : ((dist < comp) ? 0.0f : 1.0f);
                        if (mask <= 0.0f) drow[x] = PremultipliedColorU8::TRANSPARENT;
                        else if (mask >= 1.0f) drow[x] = sp;
                        else {
                            drow[x] = PremultipliedColorU8::from_rgba_unchecked(
                                static_cast<uint8_t>(sp.r * mask + 0.5f),
                                static_cast<uint8_t>(sp.g * mask + 0.5f),
                                static_cast<uint8_t>(sp.b * mask + 0.5f),
                                static_cast<uint8_t>(sp.a * mask + 0.5f)
                            );
                        }
                    }
                }
            };
            return make_lottie_runtime_filter(kVenetianBlindsShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::MotionTile: {
            const float cw = std::max(1.0f, comp_width);
            const float ch = std::max(1.0f, comp_height);
            const Point tc = eff.has_tile_center
                ? eff.tile_center.evaluate(frame)
                : Point::from_xy(cw * 0.5f, ch * 0.5f);
            const float tw = std::max(1.0f, cw * std::max(0.01f, eff.tile_width.evaluate(frame) * 0.01f));
            const float th = std::max(1.0f, ch * std::max(0.01f, eff.tile_height.evaluate(frame) * 0.01f));
            const float ow = cw * std::max(0.01f, eff.tile_output_width.evaluate(frame) * 0.01f);
            const float oh = ch * std::max(0.01f, eff.tile_output_height.evaluate(frame) * 0.01f);
            const float ph = eff.tile_phase.evaluate(frame) / 360.0f;
            const bool mirror = eff.tile_mirror_edges;
            const bool hphase = eff.tile_horizontal_phase;

            effects::RuntimeFilterUniforms u;
            u.set_vec2("comp_size", cw, ch)
             .set_vec2("tile_center", tc.x, tc.y)
             .set_vec2("tile_size", tw, th)
             .set_vec2("out_half", ow * 0.5f, oh * 0.5f)
             .set_float("mirror_edges", mirror ? 1.0f : 0.0f)
             .set_float("phase", ph)
             .set_float("horiz_phase", hphase ? 1.0f : 0.0f);

            auto cpu_fn = [cw, ch, tc, tw, th, ohx = ow * 0.5f, ohy = oh * 0.5f, mirror, ph, hphase](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform& ts, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                const int32_t w = static_cast<int32_t>(src.width());
                const int32_t h = static_cast<int32_t>(src.height());
                const float cmx = cw * 0.5f, cmy = ch * 0.5f;
                const float inv_tw = 1.0f / tw, inv_th = 1.0f / th;
                const bool use_phase = (std::abs(ph) > 1e-4f);
                for (int32_t y = cy0; y < cy1; ++y) {
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        if (std::abs(lp.x - cmx) > ohx || std::abs(lp.y - cmy) > ohy) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        float rel_x = lp.x - tc.x + tw * 0.5f;
                        float rel_y = lp.y - tc.y + th * 0.5f;
                        float idx_x = std::floor(rel_x * inv_tw);
                        float idx_y = std::floor(rel_y * inv_th);
                        if (use_phase) {
                            if (hphase) {
                                if ((static_cast<int>(std::abs(idx_y)) & 1) != 0) rel_x += ph * tw;
                            } else {
                                if ((static_cast<int>(std::abs(idx_x)) & 1) != 0) rel_y += ph * th;
                            }
                            idx_x = std::floor(rel_x * inv_tw);
                            idx_y = std::floor(rel_y * inv_th);
                        }
                        float ux = (rel_x - idx_x * tw) * inv_tw;
                        float uy = (rel_y - idx_y * th) * inv_th;
                        if (mirror) {
                            if ((static_cast<int>(std::abs(idx_x)) & 1) != 0) ux = 1.0f - ux;
                            if ((static_cast<int>(std::abs(idx_y)) & 1) != 0) uy = 1.0f - uy;
                        }
                        drow[x] = sample_bilinear_local_u8(src, ux * cw, uy * ch, roi_ox, roi_oy, w, h, ts);
                    }
                }
            };
            return make_lottie_runtime_filter(kMotionTileShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::TransformEffect: {
            const Point anc = eff.fx_anchor.evaluate(frame);
            const Point pos = eff.fx_position.evaluate(frame);
            const float sy = eff.fx_scale_height.evaluate(frame) * 0.01f;
            const float sx = eff.fx_uniform_scale ? sy : (eff.fx_scale_width.evaluate(frame) * 0.01f);
            const float rot = eff.fx_rotation.evaluate(frame);
            const float sk = eff.fx_skew.evaluate(frame);
            const float sa = eff.fx_skew_axis.evaluate(frame);
            const float op = std::clamp(eff.fx_opacity.evaluate(frame) * 0.01f, 0.0f, 1.0f);

            Transform tr = Transform::from_translate(pos.x, pos.y);
            if (std::abs(rot) > 1e-4f) tr = tr.pre_rotate(rot);
            if (std::abs(sk) > 1e-4f) {
                if (std::abs(sa) > 1e-4f) tr = tr.pre_rotate(-sa);
                tr = tr.pre_concat(Transform::from_skew(std::tan(-sk * static_cast<float>(M_PI / 180.0)), 0.0f));
                if (std::abs(sa) > 1e-4f) tr = tr.pre_rotate(sa);
            }
            tr = tr.pre_scale(sx, sy);
            tr = tr.pre_translate(-anc.x, -anc.y);

            effects::ImageFilter xform_fx = effects::ImageFilter::matrix_transform(
                tr, FilterQuality::Bilinear, std::move(input)
            );
            if (op < 0.999f) {
                return effects::ImageFilter::color_filter(
                    effects::ColorEffect::from_matrix(effects::ColorMatrix::scale(1.0f, 1.0f, 1.0f, op)),
                    std::move(xform_fx)
                );
            }
            return xform_fx;
        }

        case EffectType::CornerPin: {
            const float cw = std::max(1.0f, comp_width);
            const float ch = std::max(1.0f, comp_height);
            auto src_rect = Rect::from_xywh(0.0f, 0.0f, cw, ch);
            if (!src_rect) return input;

            const Point ul = eff.pin_upper_left.evaluate(frame);
            const Point ur = eff.pin_upper_right.evaluate(frame);
            const Point ll = eff.pin_lower_left.evaluate(frame);
            const Point lr = eff.pin_lower_right.evaluate(frame);

            auto H = Transform4x4::from_rect_to_quad(*src_rect, ul, ur, lr, ll);
            if (!H) return input;
            auto invH = H->invert();
            if (!invH) return input;

            const float r00 = invH->m[0],  r01 = invH->m[1],  r02 = invH->m[3];
            const float r10 = invH->m[4],  r11 = invH->m[5],  r12 = invH->m[7];
            const float r20 = invH->m[12], r21 = invH->m[13], r22 = invH->m[15];

            effects::RuntimeFilterUniforms u;
            u.set_vec3("row0", r00, r01, r02)
             .set_vec3("row1", r10, r11, r12)
             .set_vec3("row2", r20, r21, r22)
             .set_vec2("comp_size", cw, ch);

            auto cpu_fn = [r00, r01, r02, r10, r11, r12, r20, r21, r22, cw, ch](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform& ts, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                const int32_t w = static_cast<int32_t>(src.width());
                const int32_t h = static_cast<int32_t>(src.height());
                for (int32_t y = cy0; y < cy1; ++y) {
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        const float pw = r20 * lp.x + r21 * lp.y + r22;
                        if (pw <= 1e-4f) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float inv_pw = 1.0f / pw;
                        const float sx = (r00 * lp.x + r01 * lp.y + r02) * inv_pw;
                        const float sy = (r10 * lp.x + r11 * lp.y + r12) * inv_pw;
                        if (sx < 0.0f || sy < 0.0f || sx > cw || sy > ch) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        drow[x] = sample_bilinear_local_u8(src, sx, sy, roi_ox, roi_oy, w, h, ts);
                    }
                }
            };
            return make_lottie_runtime_filter(kCornerPinShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::Bulge: {
            const float rx = std::max(1.0f, eff.bulge_radius_x.evaluate(frame));
            const float ry = std::max(1.0f, eff.bulge_radius_y.evaluate(frame));
            const Point center = eff.has_bulge_center
                ? eff.bulge_center.evaluate(frame)
                : Point::from_xy(comp_width * 0.5f, comp_height * 0.5f);
            const float h = std::clamp(eff.bulge_height.evaluate(frame), -4.0f, 4.0f);
            if (std::abs(h) < 1e-4f) return input;
            const float pwr = std::exp(h * 0.75f);

            effects::RuntimeFilterUniforms u;
            u.set_vec2("center", center.x, center.y)
             .set_vec2("radii", rx, ry)
             .set_float("pwr", pwr);

            auto cpu_fn = [center, rx, ry, pwr](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform& ts, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                const int32_t w = static_cast<int32_t>(src.width());
                const int32_t h = static_cast<int32_t>(src.height());
                const float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;
                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        const float dx = (lp.x - center.x) * inv_rx;
                        const float dy = (lp.y - center.y) * inv_ry;
                        const float r2 = dx * dx + dy * dy;
                        if (r2 >= 1.0f || r2 <= 1e-10f) {
                            drow[x] = srow[x];
                            continue;
                        }
                        const float r = std::sqrt(r2);
                        const float scale = std::pow(r, pwr - 1.0f);
                        const float sx = center.x + dx * scale * rx;
                        const float sy = center.y + dy * scale * ry;
                        drow[x] = sample_bilinear_local_u8(src, sx, sy, roi_ox, roi_oy, w, h, ts);
                    }
                }
            };
            return make_lottie_runtime_filter(kBulgeShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::Sphere: {
            const float rad = std::max(0.0f, eff.sphere_radius.evaluate(frame));
            if (rad < 1.0f) return input;
            const Point center = eff.has_sphere_center
                ? eff.sphere_center.evaluate(frame)
                : Point::from_xy(comp_width * 0.5f, comp_height * 0.5f);

            effects::RuntimeFilterUniforms u;
            u.set_vec2("center", center.x, center.y)
             .set_float("radius", rad);

            auto cpu_fn = [center, rad](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform& ts, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                const int32_t w = static_cast<int32_t>(src.width());
                const int32_t h = static_cast<int32_t>(src.height());
                const float inv_r = 1.0f / rad;
                constexpr float inv_half_pi = 1.0f / 1.5707963f;
                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        const float dx = (lp.x - center.x) * inv_r;
                        const float dy = (lp.y - center.y) * inv_r;
                        const float r2 = dx * dx + dy * dy;
                        if (r2 >= 1.0f || r2 <= 1e-10f) {
                            drow[x] = srow[x];
                            continue;
                        }
                        const float r = std::sqrt(r2);
                        const float z = std::sqrt(std::max(0.0f, 1.0f - r2));
                        const float scale = (std::atan2(r, z) * inv_half_pi) / r;
                        const float sx = center.x + dx * scale * rad;
                        const float sy = center.y + dy * scale * rad;
                        drow[x] = sample_bilinear_local_u8(src, sx, sy, roi_ox, roi_oy, w, h, ts);
                    }
                }
            };
            return make_lottie_runtime_filter(kSphereShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::DirectionalBlur: {
            const float len = std::max(0.0f, eff.dir_blur_length.evaluate(frame));
            if (len <= 0.1f) return input;
            const float ang = eff.dir_blur_direction.evaluate(frame) * static_cast<float>(M_PI / 180.0);
            const float step_len = len / 8.0f;
            const float svx = std::sin(ang) * step_len;
            const float svy = -std::cos(ang) * step_len;

            effects::RuntimeFilterUniforms u;
            u.set_vec2("step_vec", svx, svy);

            auto cpu_fn = [svx, svy](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform& ts, const Transform&, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                const int32_t w = static_cast<int32_t>(src.width());
                const int32_t h = static_cast<int32_t>(src.height());
                // Convert local step vector to device-space delta once!
                const float dsx = svx * ts.sx + svy * ts.kx;
                const float dsy = svx * ts.ky + svy * ts.sy;
                constexpr float inv9 = 1.0f / 9.0f;
                constexpr float inv255 = 1.0f / 255.0f;
                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    const float dev_y = static_cast<float>(roi_oy + y) + 0.5f;
                    for (int32_t x = cx0; x < cx1; ++x) {
                        const float dev_x = static_cast<float>(roi_ox + x) + 0.5f;
                        const auto sp = srow[x];
                        float sr = sp.r * inv255, sg = sp.g * inv255, sb = sp.b * inv255, sa = sp.a * inv255;
                        for (int k = 1; k <= 4; ++k) {
                            const float ox = dsx * static_cast<float>(k);
                            const float oy = dsy * static_cast<float>(k);
                            float r1, g1, b1, a1, r2, g2, b2, a2;
                            sample_bilinear_dev_f(src, dev_x + ox, dev_y + oy, roi_ox, roi_oy, w, h, r1, g1, b1, a1);
                            sample_bilinear_dev_f(src, dev_x - ox, dev_y - oy, roi_ox, roi_oy, w, h, r2, g2, b2, a2);
                            sr += r1 + r2; sg += g1 + g2; sb += b1 + b2; sa += a1 + a2;
                        }
                        drow[x] = pack_premul_f(sr * inv9, sg * inv9, sb * inv9, sa * inv9);
                    }
                }
            };
            return make_lottie_runtime_filter(kDirectionalBlurShader, u, std::move(cpu_fn), std::move(input));
        }

        case EffectType::Sharpen: {
            const float amt = std::max(0.0f, eff.sharpen_amount.evaluate(frame));
            if (amt <= 1e-3f) return input;
            const float k = amt * 0.01f;
            std::vector<float> kernel = {
                0.0f, -k,           0.0f,
                -k,   1.0f + 4.0f * k, -k,
                0.0f, -k,           0.0f
            };
            return effects::ImageFilter::matrix_convolution(
                3, 3, std::move(kernel), 1.0f, 0.0f, 1, 1,
                effects::FilterTileMode::Clamp, false, std::move(input)
            );
        }

        case EffectType::FractalNoise: {
            const float scale_px = std::max(2.0f, eff.fractal_scale.evaluate(frame));
            const float inv_sc = 1.0f / scale_px;
            const Point off = eff.fractal_offset.evaluate(frame);
            const float contrast = std::max(0.0f, eff.fractal_contrast.evaluate(frame) * 0.01f);
            const float brightness = eff.fractal_brightness.evaluate(frame) * 0.01f;
            const float complexity = std::clamp(eff.fractal_complexity.evaluate(frame), 1.0f, 4.0f);
            const float sub_inf = std::clamp(eff.fractal_sub_influence.evaluate(frame) * 0.01f, 0.05f, 1.0f);
            const float sub_sc = std::clamp(eff.fractal_sub_scaling.evaluate(frame) * 0.01f, 0.1f, 0.95f);
            const float sub_scale_inv = 1.0f / sub_sc;
            const float evo = eff.fractal_evolution.evaluate(frame) * (1.0f / 360.0f) * 4.0f;
            const float op = std::clamp(eff.fractal_opacity.evaluate(frame) * 0.01f, 0.0f, 1.0f);
            const bool inv_noise = eff.fractal_invert;

            effects::RuntimeFilterUniforms u;
            u.set_vec2("scale_inv", inv_sc, inv_sc)
             .set_vec2("offset", off.x, off.y)
             .set_float("contrast", contrast)
             .set_float("brightness", brightness)
             .set_float("invert_noise", inv_noise ? 1.0f : 0.0f)
             .set_float("complexity", complexity)
             .set_float("sub_inf", sub_inf)
             .set_float("sub_scale_inv", sub_scale_inv)
             .set_float("evo_shift", evo)
             .set_float("opacity", op);

            auto cpu_fn = [inv_sc, off, contrast, brightness, inv_noise, complexity, sub_inf, sub_scale_inv, evo, op](
                const PixmapRef& src, PixmapMut& dst, int32_t roi_ox, int32_t roi_oy,
                const Transform&, const Transform& inv_ts, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1
            ) {
                constexpr float inv255 = 1.0f / 255.0f;
                const float inv_op = 1.0f - op;
                float oct_w[4], oct_freq[4], oct_sx[4], oct_sy[4];
                int active_octs = 0;
                float amp = 1.0f, freq = 1.0f, norm = 0.0f;
                for (int oct = 0; oct < 4; ++oct) {
                    const float w = std::clamp(complexity - static_cast<float>(oct), 0.0f, 1.0f);
                    if (w > 0.001f) {
                        oct_w[active_octs] = amp * w;
                        oct_freq[active_octs] = freq;
                        oct_sx[active_octs] = evo * static_cast<float>(oct + 1);
                        oct_sy[active_octs] = evo * 0.618f * static_cast<float>(oct + 1);
                        norm += amp * w;
                        ++active_octs;
                    }
                    amp *= sub_inf;
                    freq *= sub_scale_inv;
                }
                const float inv_norm = (norm > 1e-4f) ? (1.0f / norm) : 0.0f;
                auto hash2 = [](float ix, float iy) noexcept -> float {
                    const float px = fract_f(ix * 0.1031f);
                    const float py = fract_f(iy * 0.1030f);
                    const float d = px * (py + 33.33f) + py * (px + 33.33f);
                    return fract_f((px + py + d) * (px + d));
                };

                for (int32_t y = cy0; y < cy1; ++y) {
                    const auto* srow = src.row(static_cast<size_t>(y));
                    auto* drow = dst.row(static_cast<size_t>(y));
                    Point lp = Point::from_xy(static_cast<float>(roi_ox + cx0) + 0.5f, static_cast<float>(roi_oy + y) + 0.5f);
                    inv_ts.map_point(lp);
                    for (int32_t x = cx0; x < cx1; ++x, lp.x += inv_ts.sx, lp.y += inv_ts.ky) {
                        const auto sp = srow[x];
                        if (sp.a == 0) {
                            drow[x] = PremultipliedColorU8::TRANSPARENT;
                            continue;
                        }
                        const float px = (lp.x - off.x) * inv_sc;
                        const float py = (lp.y - off.y) * inv_sc;
                        float total = 0.0f;
                        for (int o = 0; o < active_octs; ++o) {
                            const float qx = px * oct_freq[o] + oct_sx[o];
                            const float qy = py * oct_freq[o] + oct_sy[o];
                            const float ix = std::floor(qx);
                            const float iy = std::floor(qy);
                            const float fx = qx - ix;
                            const float fy = qy - iy;
                            const float ux = fx * fx * (3.0f - 2.0f * fx);
                            const float uy = fy * fy * (3.0f - 2.0f * fy);
                            const float h00 = hash2(ix, iy);
                            const float h10 = hash2(ix + 1.0f, iy);
                            const float h01 = hash2(ix, iy + 1.0f);
                            const float h11 = hash2(ix + 1.0f, iy + 1.0f);
                            const float nx0 = h00 + (h10 - h00) * ux;
                            const float nx1 = h01 + (h11 - h01) * ux;
                            total += (nx0 + (nx1 - nx0) * uy) * oct_w[o];
                        }
                        float val = (inv_norm > 0.0f) ? (total * inv_norm) : 0.5f;
                        val = (val - 0.5f) * contrast + 0.5f + brightness;
                        if (inv_noise) val = 1.0f - val;
                        val = std::clamp(val, 0.0f, 1.0f);
                        const float a = sp.a * inv255;
                        if (op >= 0.9999f) {
                            const uint8_t gv = static_cast<uint8_t>(val * a * 255.0f + 0.5f);
                            drow[x] = PremultipliedColorU8::from_rgba_unchecked(gv, gv, gv, sp.a);
                        } else {
                            const float inv_a = 1.0f / a;
                            const float or_ = std::clamp(sp.r * inv255 * inv_a, 0.0f, 1.0f);
                            const float og  = std::clamp(sp.g * inv255 * inv_a, 0.0f, 1.0f);
                            const float ob  = std::clamp(sp.b * inv255 * inv_a, 0.0f, 1.0f);
                            drow[x] = pack_premul_f((or_ * inv_op + val * op) * a,
                                                    (og  * inv_op + val * op) * a,
                                                    (ob  * inv_op + val * op) * a, a);
                        }
                    }
                }
            };
            return make_lottie_runtime_filter(kFractalNoiseShader, u, std::move(cpu_fn), std::move(input));
        }

        default:
            return input;
    }
}

} // anonymous namespace

// ── LayerEffect Methods ───────────────────────────────────────────────────

bool LayerEffect::is_supported_effect_json(const json::JsonValue& val) noexcept {
    if (!val.is_object()) return false;
    return classify_effect_type(val) != EffectType::Unknown;
}

bool LayerEffect::is_animated() const noexcept {
    if (!enabled) return false;
    return blurriness.is_animated() || shadow_color.is_animated() ||
           shadow_opacity.is_animated() || shadow_direction.is_animated() ||
           shadow_distance.is_animated() || shadow_softness.is_animated() ||
           tint_black.is_animated() || tint_dark.is_animated() ||
           tint_mid.is_animated() || tint_bright.is_animated() ||
           tint_white.is_animated() || tint_amount.is_animated() ||
           brightness.is_animated() || contrast.is_animated() ||
           master_hue.is_animated() || master_saturation.is_animated() ||
           master_lightness.is_animated() || colorize_hue.is_animated() ||
           colorize_saturation.is_animated() || colorize_lightness.is_animated() ||
           bw_reds.is_animated() || bw_yellows.is_animated() ||
           bw_greens.is_animated() || bw_cyans.is_animated() ||
           bw_blues.is_animated() || bw_magentas.is_animated() ||
           bw_tint_color.is_animated() || blend_with_original.is_animated() ||
           threshold_level.is_animated() || levels_in_black.is_animated() ||
           levels_in_white.is_animated() || levels_gamma.is_animated() ||
           levels_out_black.is_animated() || levels_out_white.is_animated() ||
           ramp_start_point.is_animated() || ramp_start_color.is_animated() ||
           ramp_end_point.is_animated() || ramp_end_color.is_animated() ||
           ramp_scatter.is_animated() || four_pt1.is_animated() ||
           four_col1.is_animated() || four_pt2.is_animated() ||
           four_col2.is_animated() || four_pt3.is_animated() ||
           four_col3.is_animated() || four_pt4.is_animated() ||
           four_col4.is_animated() || four_blend.is_animated() ||
           four_opacity.is_animated() || wipe_completion.is_animated() ||
           wipe_angle.is_animated() || wipe_feather.is_animated() ||
           blinds_width.is_animated() || tile_center.is_animated() ||
           tile_width.is_animated() || tile_height.is_animated() ||
           tile_output_width.is_animated() || tile_output_height.is_animated() ||
           tile_phase.is_animated() || fx_anchor.is_animated() ||
           fx_position.is_animated() || fx_scale_height.is_animated() ||
           fx_scale_width.is_animated() || fx_skew.is_animated() ||
           fx_skew_axis.is_animated() || fx_rotation.is_animated() ||
           fx_opacity.is_animated() || pin_upper_left.is_animated() ||
           pin_upper_right.is_animated() || pin_lower_left.is_animated() ||
           pin_lower_right.is_animated() || bulge_radius_x.is_animated() ||
           bulge_radius_y.is_animated() || bulge_center.is_animated() ||
           bulge_height.is_animated() || bulge_taper.is_animated() ||
           sphere_radius.is_animated() || sphere_center.is_animated() ||
           dir_blur_direction.is_animated() || dir_blur_length.is_animated() ||
           sharpen_amount.is_animated() || fractal_contrast.is_animated() ||
           fractal_brightness.is_animated() || fractal_scale.is_animated() ||
           fractal_offset.is_animated() || fractal_complexity.is_animated() ||
           fractal_sub_influence.is_animated() || fractal_sub_scaling.is_animated() ||
           fractal_evolution.is_animated() || fractal_opacity.is_animated();
}

void LayerEffect::bind_expression_manager(ExpressionManager& mgr) {
    blurriness.bind_expression_manager(mgr);
    shadow_color.bind_expression_manager(mgr);
    shadow_opacity.bind_expression_manager(mgr);
    shadow_direction.bind_expression_manager(mgr);
    shadow_distance.bind_expression_manager(mgr);
    shadow_softness.bind_expression_manager(mgr);
    tint_black.bind_expression_manager(mgr);
    tint_dark.bind_expression_manager(mgr);
    tint_mid.bind_expression_manager(mgr);
    tint_bright.bind_expression_manager(mgr);
    tint_white.bind_expression_manager(mgr);
    tint_amount.bind_expression_manager(mgr);
    brightness.bind_expression_manager(mgr);
    contrast.bind_expression_manager(mgr);
    master_hue.bind_expression_manager(mgr);
    master_saturation.bind_expression_manager(mgr);
    master_lightness.bind_expression_manager(mgr);
    colorize_hue.bind_expression_manager(mgr);
    colorize_saturation.bind_expression_manager(mgr);
    colorize_lightness.bind_expression_manager(mgr);
    bw_reds.bind_expression_manager(mgr);
    bw_yellows.bind_expression_manager(mgr);
    bw_greens.bind_expression_manager(mgr);
    bw_cyans.bind_expression_manager(mgr);
    bw_blues.bind_expression_manager(mgr);
    bw_magentas.bind_expression_manager(mgr);
    bw_tint_color.bind_expression_manager(mgr);
    blend_with_original.bind_expression_manager(mgr);
    threshold_level.bind_expression_manager(mgr);
    levels_in_black.bind_expression_manager(mgr);
    levels_in_white.bind_expression_manager(mgr);
    levels_gamma.bind_expression_manager(mgr);
    levels_out_black.bind_expression_manager(mgr);
    levels_out_white.bind_expression_manager(mgr);
    ramp_start_point.bind_expression_manager(mgr);
    ramp_start_color.bind_expression_manager(mgr);
    ramp_end_point.bind_expression_manager(mgr);
    ramp_end_color.bind_expression_manager(mgr);
    four_pt1.bind_expression_manager(mgr);
    four_col1.bind_expression_manager(mgr);
    four_pt2.bind_expression_manager(mgr);
    four_col2.bind_expression_manager(mgr);
    four_pt3.bind_expression_manager(mgr);
    four_col3.bind_expression_manager(mgr);
    four_pt4.bind_expression_manager(mgr);
    four_col4.bind_expression_manager(mgr);
    four_blend.bind_expression_manager(mgr);
    four_opacity.bind_expression_manager(mgr);
    wipe_completion.bind_expression_manager(mgr);
    wipe_angle.bind_expression_manager(mgr);
    wipe_feather.bind_expression_manager(mgr);
    blinds_width.bind_expression_manager(mgr);
    tile_center.bind_expression_manager(mgr);
    tile_width.bind_expression_manager(mgr);
    tile_height.bind_expression_manager(mgr);
    tile_output_width.bind_expression_manager(mgr);
    tile_output_height.bind_expression_manager(mgr);
    tile_phase.bind_expression_manager(mgr);
    fx_anchor.bind_expression_manager(mgr);
    fx_position.bind_expression_manager(mgr);
    fx_scale_height.bind_expression_manager(mgr);
    fx_scale_width.bind_expression_manager(mgr);
    fx_skew.bind_expression_manager(mgr);
    fx_skew_axis.bind_expression_manager(mgr);
    fx_rotation.bind_expression_manager(mgr);
    fx_opacity.bind_expression_manager(mgr);
    pin_upper_left.bind_expression_manager(mgr);
    pin_upper_right.bind_expression_manager(mgr);
    pin_lower_left.bind_expression_manager(mgr);
    pin_lower_right.bind_expression_manager(mgr);
    bulge_radius_x.bind_expression_manager(mgr);
    bulge_radius_y.bind_expression_manager(mgr);
    bulge_center.bind_expression_manager(mgr);
    bulge_height.bind_expression_manager(mgr);
    sphere_radius.bind_expression_manager(mgr);
    sphere_center.bind_expression_manager(mgr);
    dir_blur_direction.bind_expression_manager(mgr);
    dir_blur_length.bind_expression_manager(mgr);
    sharpen_amount.bind_expression_manager(mgr);
    fractal_contrast.bind_expression_manager(mgr);
    fractal_brightness.bind_expression_manager(mgr);
    fractal_scale.bind_expression_manager(mgr);
    fractal_offset.bind_expression_manager(mgr);
    fractal_complexity.bind_expression_manager(mgr);
    fractal_sub_influence.bind_expression_manager(mgr);
    fractal_sub_scaling.bind_expression_manager(mgr);
    fractal_evolution.bind_expression_manager(mgr);
    fractal_opacity.bind_expression_manager(mgr);
}

uint64_t LayerEffect::visual_fingerprint(float frame) const noexcept {
    if (!enabled) return 0u;
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    h = ehash_mix(h, static_cast<uint64_t>(type));
    switch (type) {
        case EffectType::GaussianBlur:
            h = ehash_f32(h, blurriness.evaluate(frame));
            h = ehash_mix(h, static_cast<uint64_t>(blur_dimensions));
            break;
        case EffectType::DropShadow:
            h = ehash_col(h, shadow_color.evaluate(frame));
            h = ehash_f32(h, shadow_opacity.evaluate(frame));
            h = ehash_f32(h, shadow_direction.evaluate(frame));
            h = ehash_f32(h, shadow_distance.evaluate(frame));
            h = ehash_f32(h, shadow_softness.evaluate(frame));
            h = ehash_mix(h, shadow_only ? 1u : 0u);
            break;
        case EffectType::Fill:
            h = ehash_col(h, shadow_color.evaluate(frame));
            h = ehash_f32(h, shadow_opacity.evaluate(frame));
            break;
        case EffectType::Tint:
        case EffectType::Tritone:
        case EffectType::CCToner:
            h = ehash_col(h, tint_black.evaluate(frame));
            h = ehash_col(h, tint_dark.evaluate(frame));
            h = ehash_col(h, tint_mid.evaluate(frame));
            h = ehash_col(h, tint_bright.evaluate(frame));
            h = ehash_col(h, tint_white.evaluate(frame));
            h = ehash_f32(h, tint_amount.evaluate(frame));
            h = ehash_mix(h, static_cast<uint64_t>(toner_mode));
            break;
        case EffectType::BrightnessContrast:
            h = ehash_f32(h, brightness.evaluate(frame));
            h = ehash_f32(h, contrast.evaluate(frame));
            break;
        case EffectType::HueSaturation:
            h = ehash_f32(h, master_hue.evaluate(frame));
            h = ehash_f32(h, master_saturation.evaluate(frame));
            h = ehash_f32(h, master_lightness.evaluate(frame));
            h = ehash_mix(h, colorize ? 1u : 0u);
            h = ehash_f32(h, colorize_hue.evaluate(frame));
            h = ehash_f32(h, colorize_saturation.evaluate(frame));
            h = ehash_f32(h, colorize_lightness.evaluate(frame));
            break;
        case EffectType::BlackAndWhite:
            h = ehash_f32(h, bw_reds.evaluate(frame));
            h = ehash_f32(h, bw_yellows.evaluate(frame));
            h = ehash_f32(h, bw_greens.evaluate(frame));
            h = ehash_f32(h, bw_cyans.evaluate(frame));
            h = ehash_f32(h, bw_blues.evaluate(frame));
            h = ehash_f32(h, bw_magentas.evaluate(frame));
            h = ehash_mix(h, bw_tint_enabled ? 1u : 0u);
            h = ehash_col(h, bw_tint_color.evaluate(frame));
            break;
        case EffectType::Invert:
            h = ehash_mix(h, static_cast<uint64_t>(invert_channel));
            h = ehash_f32(h, blend_with_original.evaluate(frame));
            break;
        case EffectType::Threshold:
            h = ehash_f32(h, threshold_level.evaluate(frame));
            break;
        case EffectType::ShiftChannels:
            h = ehash_mix(h, static_cast<uint64_t>(shift_alpha));
            h = ehash_mix(h, static_cast<uint64_t>(shift_red));
            h = ehash_mix(h, static_cast<uint64_t>(shift_green));
            h = ehash_mix(h, static_cast<uint64_t>(shift_blue));
            break;
        case EffectType::EasyLevels:
        case EffectType::ProLevels:
            h = ehash_mix(h, static_cast<uint64_t>(levels_channel));
            h = ehash_f32(h, levels_in_black.evaluate(frame));
            h = ehash_f32(h, levels_in_white.evaluate(frame));
            h = ehash_f32(h, levels_gamma.evaluate(frame));
            h = ehash_f32(h, levels_out_black.evaluate(frame));
            h = ehash_f32(h, levels_out_white.evaluate(frame));
            break;
        case EffectType::GradientRamp:
            h = ehash_pt(h, ramp_start_point.evaluate(frame));
            h = ehash_col(h, ramp_start_color.evaluate(frame));
            h = ehash_pt(h, ramp_end_point.evaluate(frame));
            h = ehash_col(h, ramp_end_color.evaluate(frame));
            h = ehash_mix(h, static_cast<uint64_t>(ramp_shape));
            h = ehash_f32(h, blend_with_original.evaluate(frame));
            break;
        case EffectType::FourColorGradient:
            h = ehash_pt(h, four_pt1.evaluate(frame));
            h = ehash_col(h, four_col1.evaluate(frame));
            h = ehash_pt(h, four_pt2.evaluate(frame));
            h = ehash_col(h, four_col2.evaluate(frame));
            h = ehash_pt(h, four_pt3.evaluate(frame));
            h = ehash_col(h, four_col3.evaluate(frame));
            h = ehash_pt(h, four_pt4.evaluate(frame));
            h = ehash_col(h, four_col4.evaluate(frame));
            h = ehash_f32(h, four_blend.evaluate(frame));
            h = ehash_f32(h, four_opacity.evaluate(frame));
            break;
        case EffectType::LinearWipe:
        case EffectType::VenetianBlinds:
            h = ehash_f32(h, wipe_completion.evaluate(frame));
            h = ehash_f32(h, wipe_angle.evaluate(frame));
            h = ehash_f32(h, wipe_feather.evaluate(frame));
            h = ehash_f32(h, blinds_width.evaluate(frame));
            break;
        case EffectType::MotionTile:
            h = ehash_pt(h, tile_center.evaluate(frame));
            h = ehash_f32(h, tile_width.evaluate(frame));
            h = ehash_f32(h, tile_height.evaluate(frame));
            h = ehash_f32(h, tile_output_width.evaluate(frame));
            h = ehash_f32(h, tile_output_height.evaluate(frame));
            h = ehash_mix(h, tile_mirror_edges ? 1u : 0u);
            h = ehash_f32(h, tile_phase.evaluate(frame));
            break;
        case EffectType::TransformEffect:
            h = ehash_pt(h, fx_anchor.evaluate(frame));
            h = ehash_pt(h, fx_position.evaluate(frame));
            h = ehash_f32(h, fx_scale_height.evaluate(frame));
            h = ehash_f32(h, fx_scale_width.evaluate(frame));
            h = ehash_f32(h, fx_skew.evaluate(frame));
            h = ehash_f32(h, fx_skew_axis.evaluate(frame));
            h = ehash_f32(h, fx_rotation.evaluate(frame));
            h = ehash_f32(h, fx_opacity.evaluate(frame));
            break;
        case EffectType::CornerPin:
            h = ehash_pt(h, pin_upper_left.evaluate(frame));
            h = ehash_pt(h, pin_upper_right.evaluate(frame));
            h = ehash_pt(h, pin_lower_left.evaluate(frame));
            h = ehash_pt(h, pin_lower_right.evaluate(frame));
            break;
        case EffectType::Bulge:
            h = ehash_f32(h, bulge_radius_x.evaluate(frame));
            h = ehash_f32(h, bulge_radius_y.evaluate(frame));
            h = ehash_pt(h, bulge_center.evaluate(frame));
            h = ehash_f32(h, bulge_height.evaluate(frame));
            break;
        case EffectType::Sphere:
            h = ehash_f32(h, sphere_radius.evaluate(frame));
            h = ehash_pt(h, sphere_center.evaluate(frame));
            break;
        case EffectType::DirectionalBlur:
            h = ehash_f32(h, dir_blur_direction.evaluate(frame));
            h = ehash_f32(h, dir_blur_length.evaluate(frame));
            break;
        case EffectType::Sharpen:
            h = ehash_f32(h, sharpen_amount.evaluate(frame));
            break;
        case EffectType::FractalNoise:
            h = ehash_mix(h, fractal_invert ? 1u : 0u);
            h = ehash_f32(h, fractal_contrast.evaluate(frame));
            h = ehash_f32(h, fractal_brightness.evaluate(frame));
            h = ehash_f32(h, fractal_scale.evaluate(frame));
            h = ehash_pt(h, fractal_offset.evaluate(frame));
            h = ehash_f32(h, fractal_complexity.evaluate(frame));
            h = ehash_f32(h, fractal_evolution.evaluate(frame));
            h = ehash_f32(h, fractal_opacity.evaluate(frame));
            break;
        default:
            break;
    }
    return h;
}

LayerEffect LayerEffect::parse(const json::JsonValue& val) {
    LayerEffect eff;
    if (!val.is_object()) return eff;

    eff.name = std::string(val.get_string("nm", ""));
    eff.match_name = std::string(val.get_string("mn", ""));
    eff.enabled = (val.get_int("en", 1) != 0);
    eff.type = classify_effect_type(val);

    const auto* sub_ef = val.get("ef");
    if (!sub_ef || !sub_ef->is_array()) return eff;
    const auto& params = sub_ef->as_array();

    switch (eff.type) {
        case EffectType::GaussianBlur: {
            if (const auto* v = find_param_v(params, 0)) eff.blurriness = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 1)) eff.blur_dimensions = static_cast<int>(parse_float_property(*v, 1.0f).evaluate(0.0f));
            break;
        }
        case EffectType::DropShadow: {
            if (const auto* v = find_param_v(params, 0)) eff.shadow_color = parse_color_property(*v, Color::BLACK);
            if (const auto* v = find_param_v(params, 1)) eff.shadow_opacity = parse_float_property(*v, 128.0f);
            if (const auto* v = find_param_v(params, 2)) eff.shadow_direction = parse_float_property(*v, 135.0f);
            if (const auto* v = find_param_v(params, 3)) eff.shadow_distance = parse_float_property(*v, 5.0f);
            if (const auto* v = find_param_v(params, 4)) eff.shadow_softness = parse_float_property(*v, 5.0f);
            if (const auto* v = find_param_v(params, 5)) eff.shadow_only = (parse_float_property(*v, 0.0f).evaluate(0.0f) >= 0.5f);
            break;
        }
        case EffectType::Fill: {
            for (size_t i = 0; i < params.size(); ++i) {
                if (!params[i].is_object()) continue;
                const auto* v = params[i].get("v");
                if (!v) continue;
                const int pty = params[i].get_int("ty", -1);
                if (pty == 2 || i == 2) eff.shadow_color = parse_color_property(*v, Color::RED);
                else if (i == 6) eff.shadow_opacity = parse_float_property(*v, 1.0f);
            }
            break;
        }
        case EffectType::Tint: {
            if (const auto* v = find_param_v(params, 0)) eff.tint_black = parse_color_property(*v, Color::BLACK);
            if (const auto* v = find_param_v(params, 1)) eff.tint_white = parse_color_property(*v, Color::WHITE);
            if (const auto* v = find_param_v(params, 2)) eff.tint_amount = parse_float_property(*v, 100.0f);
            break;
        }
        case EffectType::Tritone: {
            if (const auto* v = find_param_v(params, 0)) eff.tint_white = parse_color_property(*v, Color::WHITE);
            if (const auto* v = find_param_v(params, 1)) eff.tint_mid = parse_color_property(*v, Color::from_rgba8(128, 128, 128, 255));
            if (const auto* v = find_param_v(params, 2)) eff.tint_black = parse_color_property(*v, Color::BLACK);
            if (const auto* v = find_param_v(params, 3)) eff.tint_amount = parse_float_property(*v, 0.0f);
            else eff.tint_amount = FloatProperty(0.0f);
            break;
        }
        case EffectType::CCToner: {
            eff.tint_amount = FloatProperty(0.0f);
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.toner_mode = static_cast<int>(parse_float_property(*v, 2.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.tint_white = parse_color_property(*v, Color::WHITE);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.tint_bright = parse_color_property(*v, Color::from_rgba8(192, 192, 192, 255));
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.tint_mid = parse_color_property(*v, Color::from_rgba8(128, 128, 128, 255));
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.tint_dark = parse_color_property(*v, Color::from_rgba8(64, 64, 64, 255));
            if (const auto* v = find_param_v(params, 5, "-0006")) eff.tint_black = parse_color_property(*v, Color::BLACK);
            if (const auto* v = find_param_v(params, 6, "-0007")) eff.tint_amount = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::BrightnessContrast: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.brightness = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.contrast = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::HueSaturation: {
            // AE Hue/Saturation params: [0]=Channel Control, [1]=Channel Range, [2]=Master Hue, [3]=Master Sat,
            // [4]=Master Lightness, [5]=Colorize, [6]=Colorize Hue, [7]=Colorize Sat, [8]=Colorize Lightness
            // Also supports compact 3-param form [0]=Hue, [1]=Sat, [2]=Lightness when params.size() <= 4
            if (params.size() <= 4 && !find_param_v(params, 99, "-0004")) {
                if (const auto* v = find_param_v(params, 0)) eff.master_hue = parse_float_property(*v, 0.0f);
                if (const auto* v = find_param_v(params, 1)) eff.master_saturation = parse_float_property(*v, 0.0f);
                if (const auto* v = find_param_v(params, 2)) eff.master_lightness = parse_float_property(*v, 0.0f);
            } else {
                if (const auto* v = find_param_v(params, 2, "-0004")) eff.master_hue = parse_float_property(*v, 0.0f);
                if (const auto* v = find_param_v(params, 3, "-0005")) eff.master_saturation = parse_float_property(*v, 0.0f);
                if (const auto* v = find_param_v(params, 4, "-0006")) eff.master_lightness = parse_float_property(*v, 0.0f);
                if (const auto* v = find_param_v(params, 5, "-0007")) eff.colorize = (parse_float_property(*v, 0.0f).evaluate(0.0f) >= 0.5f);
                if (const auto* v = find_param_v(params, 6, "-0008")) eff.colorize_hue = parse_float_property(*v, 0.0f);
                if (const auto* v = find_param_v(params, 7, "-0009")) eff.colorize_saturation = parse_float_property(*v, 25.0f);
                if (const auto* v = find_param_v(params, 8, "-0010")) eff.colorize_lightness = parse_float_property(*v, 0.0f);
            }
            break;
        }
        case EffectType::BlackAndWhite: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.bw_reds = parse_float_property(*v, 40.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.bw_yellows = parse_float_property(*v, 60.0f);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.bw_greens = parse_float_property(*v, 40.0f);
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.bw_cyans = parse_float_property(*v, 60.0f);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.bw_blues = parse_float_property(*v, 20.0f);
            if (const auto* v = find_param_v(params, 5, "-0006")) eff.bw_magentas = parse_float_property(*v, 80.0f);
            if (const auto* v = find_param_v(params, 6, "-0007")) eff.bw_tint_enabled = (parse_float_property(*v, 0.0f).evaluate(0.0f) >= 0.5f);
            if (const auto* v = find_param_v(params, 7, "-0008")) eff.bw_tint_color = parse_color_property(*v, Color::from_rgba8(225, 211, 179, 255));
            break;
        }
        case EffectType::Invert: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.invert_channel = static_cast<int>(parse_float_property(*v, 1.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.blend_with_original = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::Threshold: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.threshold_level = parse_float_property(*v, 128.0f);
            break;
        }
        case EffectType::ShiftChannels: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.shift_alpha = static_cast<int>(parse_float_property(*v, 1.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.shift_red = static_cast<int>(parse_float_property(*v, 2.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.shift_green = static_cast<int>(parse_float_property(*v, 3.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.shift_blue = static_cast<int>(parse_float_property(*v, 4.0f).evaluate(0.0f));
            break;
        }
        case EffectType::EasyLevels:
        case EffectType::ProLevels: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.levels_channel = static_cast<int>(parse_float_property(*v, 1.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 1, "-0003")) eff.levels_in_black = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 2, "-0004")) eff.levels_in_white = parse_float_property(*v, 1.0f);
            if (const auto* v = find_param_v(params, 3, "-0005")) eff.levels_gamma = parse_float_property(*v, 1.0f);
            if (const auto* v = find_param_v(params, 4, "-0006")) eff.levels_out_black = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 5, "-0007")) eff.levels_out_white = parse_float_property(*v, 1.0f);
            break;
        }
        case EffectType::GradientRamp: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.ramp_start_point = parse_point_property(*v, Point::zero());
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.ramp_start_color = parse_color_property(*v, Color::BLACK);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.ramp_end_point = parse_point_property(*v, Point::from_xy(0.0f, 100.0f));
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.ramp_end_color = parse_color_property(*v, Color::WHITE);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.ramp_shape = static_cast<int>(parse_float_property(*v, 1.0f).evaluate(0.0f));
            if (const auto* v = find_param_v(params, 5, "-0006")) eff.ramp_scatter = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 6, "-0007")) eff.blend_with_original = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::FourColorGradient: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.four_pt1 = parse_point_property(*v, Point::zero());
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.four_col1 = parse_color_property(*v, Color::RED);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.four_pt2 = parse_point_property(*v, Point::from_xy(100.0f, 0.0f));
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.four_col2 = parse_color_property(*v, Color::GREEN);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.four_pt3 = parse_point_property(*v, Point::from_xy(0.0f, 100.0f));
            if (const auto* v = find_param_v(params, 5, "-0006")) eff.four_col3 = parse_color_property(*v, Color::BLUE);
            if (const auto* v = find_param_v(params, 6, "-0007")) eff.four_pt4 = parse_point_property(*v, Point::from_xy(100.0f, 100.0f));
            if (const auto* v = find_param_v(params, 7, "-0008")) eff.four_col4 = parse_color_property(*v, Color::WHITE);
            if (const auto* v = find_param_v(params, 8, "-0009")) eff.four_blend = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 10, "-0011")) eff.four_opacity = parse_float_property(*v, 100.0f);
            break;
        }
        case EffectType::LinearWipe: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.wipe_completion = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.wipe_angle = parse_float_property(*v, 90.0f);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.wipe_feather = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::VenetianBlinds: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.wipe_completion = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.wipe_angle = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.blinds_width = parse_float_property(*v, 20.0f);
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.wipe_feather = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::MotionTile: {
            if (const auto* v = find_param_v(params, 0, "-0001")) {
                eff.tile_center = parse_point_property(*v, Point::zero());
                eff.has_tile_center = true;
            }
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.tile_width = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.tile_height = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.tile_output_width = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.tile_output_height = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 5, "-0006")) eff.tile_mirror_edges = (parse_float_property(*v, 0.0f).evaluate(0.0f) >= 0.5f);
            if (const auto* v = find_param_v(params, 6, "-0007")) eff.tile_phase = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 7, "-0008")) eff.tile_horizontal_phase = (parse_float_property(*v, 0.0f).evaluate(0.0f) >= 0.5f);
            break;
        }
        case EffectType::TransformEffect: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.fx_anchor = parse_point_property(*v, Point::zero());
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.fx_position = parse_point_property(*v, Point::zero());
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.fx_uniform_scale = (parse_float_property(*v, 1.0f).evaluate(0.0f) >= 0.5f);
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.fx_scale_height = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.fx_scale_width = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 5, "-0006")) eff.fx_skew = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 6, "-0007")) eff.fx_skew_axis = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 7, "-0008")) eff.fx_rotation = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 8, "-0009")) eff.fx_opacity = parse_float_property(*v, 100.0f);
            break;
        }
        case EffectType::CornerPin: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.pin_upper_left = parse_point_property(*v, Point::zero());
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.pin_upper_right = parse_point_property(*v, Point::from_xy(100.0f, 0.0f));
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.pin_lower_left = parse_point_property(*v, Point::from_xy(0.0f, 100.0f));
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.pin_lower_right = parse_point_property(*v, Point::from_xy(100.0f, 100.0f));
            break;
        }
        case EffectType::Bulge: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.bulge_radius_x = parse_float_property(*v, 50.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.bulge_radius_y = parse_float_property(*v, 50.0f);
            if (const auto* v = find_param_v(params, 2, "-0003")) {
                eff.bulge_center = parse_point_property(*v, Point::zero());
                eff.has_bulge_center = true;
            }
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.bulge_height = parse_float_property(*v, 1.0f);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.bulge_taper = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::Sphere: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.sphere_radius = parse_float_property(*v, 50.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) {
                eff.sphere_center = parse_point_property(*v, Point::zero());
                eff.has_sphere_center = true;
            }
            break;
        }
        case EffectType::DirectionalBlur: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.dir_blur_direction = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 1, "-0002")) eff.dir_blur_length = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::Sharpen: {
            if (const auto* v = find_param_v(params, 0, "-0001")) eff.sharpen_amount = parse_float_property(*v, 0.0f);
            break;
        }
        case EffectType::FractalNoise: {
            // Support both full AE Fractal Noise indices and compact form
            if (const auto* v = find_param_v(params, 2, "-0003")) eff.fractal_invert = (parse_float_property(*v, 0.0f).evaluate(0.0f) >= 0.5f);
            if (const auto* v = find_param_v(params, 3, "-0004")) eff.fractal_contrast = parse_float_property(*v, 100.0f);
            if (const auto* v = find_param_v(params, 4, "-0005")) eff.fractal_brightness = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 6, "-0009")) eff.fractal_scale = parse_float_property(*v, 64.0f);
            if (const auto* v = find_param_v(params, 7, "-0016")) eff.fractal_complexity = parse_float_property(*v, 4.0f);
            if (const auto* v = find_param_v(params, 8, "-0017")) eff.fractal_sub_influence = parse_float_property(*v, 50.0f);
            if (const auto* v = find_param_v(params, 9, "-0018")) eff.fractal_sub_scaling = parse_float_property(*v, 50.0f);
            if (const auto* v = find_param_v(params, 10, "-0020")) eff.fractal_evolution = parse_float_property(*v, 0.0f);
            if (const auto* v = find_param_v(params, 11, "-0027")) eff.fractal_opacity = parse_float_property(*v, 100.0f);
            break;
        }
        default:
            break;
    }

    return eff;
}

// ── Unified CPU/GPU ImageFilter Stack Builder ─────────────────────────────

std::optional<effects::ImageFilter> build_layer_effects_filter(
    const std::vector<LayerEffect>& effects,
    float frame,
    float comp_width,
    float comp_height
) {
    std::optional<effects::ImageFilter> chain;
    for (const auto& eff : effects) {
        if (!eff.enabled) continue;
        chain = build_single_effect_filter(eff, frame, comp_width, comp_height, std::move(chain));
    }
    if (chain.has_value() && chain->is_active()) {
        return chain;
    }
    return std::nullopt;
}

void apply_layer_effects_pixmap(
    Pixmap& pm,
    const std::vector<LayerEffect>& effects,
    float frame,
    float comp_width,
    float comp_height
) {
    if (effects.empty() || pm.width() == 0 || pm.height() == 0) return;
    auto filter = build_layer_effects_filter(effects, frame, comp_width, comp_height);
    if (!filter.has_value()) return;
    PixmapMut mut = pm.as_mut();
    effects::apply_image_filter_cpu(mut, *filter, 0, 0, Transform::identity());
}

void expand_bounds_for_effects(
    Rect& bounds,
    const std::vector<LayerEffect>& effects,
    float frame
) noexcept {
    float pad_x = 0.0f;
    float pad_y = 0.0f;
    for (const auto& eff : effects) {
        if (!eff.enabled) continue;
        if (eff.type == EffectType::GaussianBlur) {
            const float blur_val = eff.blurriness.evaluate(frame);
            if (blur_val > 0.1f) {
                const float halo = std::ceil(blur_val * 0.35f * 3.0f);
                if (eff.blur_dimensions != 3) pad_x += halo;
                if (eff.blur_dimensions != 2) pad_y += halo;
            }
        } else if (eff.type == EffectType::DropShadow) {
            const float angle_rad = (eff.shadow_direction.evaluate(frame) - 90.0f) * static_cast<float>(M_PI / 180.0);
            const float dist = eff.shadow_distance.evaluate(frame);
            const float dx = std::cos(angle_rad) * dist;
            const float dy = std::sin(angle_rad) * dist;
            const float halo = std::ceil(std::max(0.0f, eff.shadow_softness.evaluate(frame) * 0.4f) * 3.0f);
            if (auto sh_r = Rect::from_ltrb(
                    bounds.left() + dx - halo, bounds.top() + dy - halo,
                    bounds.right() + dx + halo, bounds.bottom() + dy + halo)) {
                if (auto joined = bounds.join(*sh_r)) {
                    bounds = *joined;
                }
            }
        } else if (eff.type == EffectType::DirectionalBlur) {
            const float len = std::max(0.0f, eff.dir_blur_length.evaluate(frame));
            if (len > 0.1f) {
                const float half = std::ceil(len * 0.5f);
                pad_x += half;
                pad_y += half;
            }
        } else if (eff.type == EffectType::CornerPin) {
            Point pts[4] = {
                eff.pin_upper_left.evaluate(frame),
                eff.pin_upper_right.evaluate(frame),
                eff.pin_lower_left.evaluate(frame),
                eff.pin_lower_right.evaluate(frame)
            };
            if (auto qb = Rect::from_points(pts, 4)) {
                if (auto joined = bounds.join(*qb)) {
                    bounds = *joined;
                }
            }
        }
    }
    if (pad_x > 0.0f || pad_y > 0.0f) {
        if (auto expanded = bounds.outset(pad_x, pad_y)) {
            bounds = *expanded;
        }
    }
}

} // namespace nisaba::lottie
