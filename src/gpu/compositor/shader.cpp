#include "nisaba/compositor/shader.hpp"
#include "nisaba/compositor/log.hpp"

#include <EGL/egl.h>
#include <algorithm>
#include <sstream>
#include <vector>

namespace nisaba::compositor {

namespace {

// Common vertex shader passing both normalized local quad coordinate v_local [0,1]^2 and transformed v_texcoord
constexpr std::string_view kUnitBoxVert = R"glsl(
attribute vec2 pos;
uniform mat3 proj;
varying vec2 v_local;

void main() {
    vec3 pos3 = vec3(pos, 1.0);
    gl_Position = vec4(pos3 * proj, 1.0);
    v_local = pos;
}
)glsl";

constexpr std::string_view kSolidQuadFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
uniform vec4 color;
uniform vec4 u_box;
uniform float u_radius;
uniform float u_power;

float squircleMask(vec2 localPos, vec2 size, float radius, float power) {
    if (radius <= 0.5) return 1.0;
    vec2 p = abs(localPos * size - size * 0.5) - (size * 0.5 - vec2(radius));
    if (p.x <= 0.0 || p.y <= 0.0) return 1.0;
    float pwr = max(power, 1.0);
    float dist = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
    return 1.0 - smoothstep(radius - 0.65, radius + 0.65, dist);
}

void main() {
    float mask = squircleMask(v_local, u_box.zw, u_radius, u_power);
    if (mask <= 0.001) discard;
    gl_FragColor = color * mask;
}
)glsl";

constexpr std::string_view kTextureQuadVert = R"glsl(
attribute vec2 pos;
uniform mat3 proj;
uniform mat3 tex_proj;
varying vec2 v_texcoord;
varying vec2 v_local;

void main() {
    vec3 pos3 = vec3(pos, 1.0);
    gl_Position = vec4(pos3 * proj, 1.0);
    v_texcoord = (pos3 * tex_proj).xy;
    v_local = pos;
}
)glsl";

constexpr std::string_view kTextureQuadRgbaFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
varying vec2 v_local;
uniform sampler2D tex;
uniform float alpha;

void main() {
    gl_FragColor = texture2D(tex, v_texcoord) * alpha;
}
)glsl";

constexpr std::string_view kTextureQuadRgbxFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
varying vec2 v_local;
uniform sampler2D tex;
uniform float alpha;

void main() {
    gl_FragColor = vec4(texture2D(tex, v_texcoord).rgb, 1.0) * alpha;
}
)glsl";

constexpr std::string_view kTextureQuadExternalFrag = R"glsl(
#extension GL_OES_EGL_image_external : require

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
varying vec2 v_local;
uniform samplerExternalOES tex;
uniform float alpha;

void main() {
    gl_FragColor = texture2D(tex, v_texcoord) * alpha;
}
)glsl";

constexpr std::string_view kTextureQuadExternalRgbxFrag = R"glsl(
#extension GL_OES_EGL_image_external : require

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
varying vec2 v_local;
uniform samplerExternalOES tex;
uniform float alpha;

void main() {
    gl_FragColor = vec4(texture2D(tex, v_texcoord).rgb, 1.0) * alpha;
}
)glsl";

// Shared Oklab & Rotational Gradient GLSL helper functions
constexpr std::string_view kOklabGradientGlsl = R"glsl(
vec4 oklabToLinearSrgb(vec4 lab) {
    float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    vec3 rgbLin = vec3(
        +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    );
    rgbLin = clamp(rgbLin, 0.0, 1.0);
    vec3 srgb = pow(rgbLin, vec3(1.0 / 2.2));
    return vec4(srgb * lab.w, lab.w);
}

vec4 sampleOklabStop(vec4 stops[10], int count, float t) {
    if (count <= 1) return stops[0];
    float scaled = clamp(t, 0.0, 1.0) * float(count - 1);
    int idx = int(floor(scaled));
    float frac = scaled - float(idx);
    vec4 c0 = stops[0];
    vec4 c1 = stops[0];
    for (int i = 0; i < 9; ++i) {
        if (i == idx) {
            c0 = stops[i];
            c1 = stops[i + 1];
        }
    }
    if (idx >= count - 1) {
        c0 = stops[count - 1];
        c1 = c0;
    }
    return mix(c0, c1, frac);
}

float computeGradientParam(vec2 localUv, float angle) {
    vec2 dir = vec2(cos(angle), sin(angle));
    vec2 centered = localUv - vec2(0.5);
    return clamp(dot(centered, dir) + 0.5, 0.0, 1.0);
}
)glsl";

std::string buildBorderFragSource() {
    std::ostringstream ss;
    ss << R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
uniform vec4 u_box;
uniform vec4 u_radii; // x=innerRadius, y=outerRadius, z=roundingPower, w=thickness
uniform vec4 u_grad1[10];
uniform int u_grad1_count;
uniform float u_grad1_angle;
uniform vec4 u_grad2[10];
uniform int u_grad2_count;
uniform float u_grad2_angle;
uniform vec2 u_lerp_alpha; // x=lerp, y=alpha
)glsl";
    ss << kOklabGradientGlsl;
    ss << R"glsl(
float squircleSignedDist(vec2 p, vec2 halfSize, float radius, float power) {
    float r = min(radius, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - (halfSize - vec2(r));
    if (q.x > 0.0 && q.y > 0.0) {
        float pwr = max(power, 1.0);
        return pow(pow(q.x, pwr) + pow(q.y, pwr), 1.0 / pwr) - r;
    }
    return max(q.x, q.y) - r;
}

void main() {
    vec2 size = u_box.zw;
    vec2 halfSize = size * 0.5;
    vec2 p = v_local * size - halfSize;

    float innerR = u_radii.x;
    float outerR = u_radii.y;
    float power = u_radii.z;
    float thick = max(u_radii.w, 1.0);

    float dOuter = squircleSignedDist(p, halfSize, outerR, power);
    float dInner = squircleSignedDist(p, max(halfSize - vec2(thick), vec2(1.0)), innerR, power);

    float outerCoverage = 1.0 - smoothstep(-0.65, 0.65, dOuter);
    float innerCoverage = smoothstep(-0.65, 0.65, dInner);
    float ringMask = outerCoverage * innerCoverage;
    if (ringMask <= 0.002) discard;

    float t1 = computeGradientParam(v_local, u_grad1_angle);
    vec4 lab = sampleOklabStop(u_grad1, u_grad1_count, t1);
    if (u_grad2_count > 0 && u_lerp_alpha.x > 0.001) {
        float t2 = computeGradientParam(v_local, u_grad2_angle);
        vec4 lab2 = sampleOklabStop(u_grad2, u_grad2_count, t2);
        lab = mix(lab, lab2, u_lerp_alpha.x);
    }

    vec4 col = oklabToLinearSrgb(lab);
    gl_FragColor = col * (ringMask * u_lerp_alpha.y);
}
)glsl";
    return ss.str();
}

std::string buildShadowFragSource() {
    std::ostringstream ss;
    ss << R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
uniform vec4 u_box;
uniform vec4 u_params; // x=cornerRadius, y=roundingPower, z=rangeSigma, w=falloffPower
uniform vec4 u_grad1[10];
uniform int u_grad1_count;
uniform float u_grad1_angle;
uniform vec4 u_grad2[10];
uniform int u_grad2_count;
uniform float u_grad2_angle;
uniform vec2 u_lerp_alpha;
)glsl";
    ss << kOklabGradientGlsl;
    ss << R"glsl(
void main() {
    vec2 size = u_box.zw;
    float radius = u_params.x;
    float power = max(u_params.y, 1.0);
    float range = max(u_params.z, 1.0);
    float falloffPwr = clamp(u_params.w, 1.0, 4.0);

    vec2 halfInner = max(size * 0.5 - vec2(range), vec2(1.0));
    float r = min(radius, min(halfInner.x, halfInner.y));
    vec2 p = abs(v_local * size - size * 0.5) - (halfInner - vec2(r));

    float dist = 0.0;
    if (p.x > 0.0 && p.y > 0.0) {
        dist = pow(pow(p.x, power) + pow(p.y, power), 1.0 / power) - r;
    } else {
        dist = max(p.x, p.y) - r;
    }

    float normDist = clamp(dist / range, 0.0, 1.0);
    float shadowMask = pow(1.0 - smoothstep(0.0, 1.0, normDist), falloffPwr);
    if (shadowMask <= 0.001) discard;

    float t1 = computeGradientParam(v_local, u_grad1_angle);
    vec4 lab = sampleOklabStop(u_grad1, u_grad1_count, t1);
    if (u_grad2_count > 0 && u_lerp_alpha.x > 0.001) {
        float t2 = computeGradientParam(v_local, u_grad2_angle);
        vec4 lab2 = sampleOklabStop(u_grad2, u_grad2_count, t2);
        lab = mix(lab, lab2, u_lerp_alpha.x);
    }

    vec4 col = oklabToLinearSrgb(lab);
    gl_FragColor = col * (shadowMask * u_lerp_alpha.y);
}
)glsl";
    return ss.str();
}

std::string buildInnerGlowFragSource() {
    std::ostringstream ss;
    ss << R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
uniform vec4 u_box;
uniform vec4 u_params; // x=cornerRadius, y=roundingPower, z=range, w=falloffPower
uniform vec4 u_grad[10];
uniform int u_grad_count;
uniform float u_grad_angle;
uniform float u_alpha;
)glsl";
    ss << kOklabGradientGlsl;
    ss << R"glsl(
void main() {
    vec2 size = u_box.zw;
    vec2 halfSize = size * 0.5;
    float radius = min(u_params.x, min(halfSize.x, halfSize.y));
    float power = max(u_params.y, 1.0);
    float range = max(u_params.z, 1.0);
    float falloffPwr = clamp(u_params.w, 1.0, 4.0);

    vec2 p = abs(v_local * size - halfSize) - (halfSize - vec2(radius));
    float dist = 0.0;
    if (p.x > 0.0 && p.y > 0.0) {
        dist = pow(pow(p.x, power) + pow(p.y, power), 1.0 / power) - radius;
    } else {
        dist = max(p.x, p.y) - radius;
    }

    if (dist > 0.5) discard;
    float outsideClip = 1.0 - smoothstep(-0.5, 0.5, dist);
    float inwardDist = clamp(-dist / range, 0.0, 1.0);
    float glow = pow(1.0 - smoothstep(0.0, 1.0, inwardDist), falloffPwr) * outsideClip;
    if (glow <= 0.001) discard;

    float t = computeGradientParam(v_local, u_grad_angle);
    vec4 lab = sampleOklabStop(u_grad, u_grad_count, t);
    vec4 col = oklabToLinearSrgb(lab);
    gl_FragColor = col * (glow * u_alpha);
}
)glsl";
    return ss.str();
}

constexpr std::string_view kDeformableMeshVert = R"glsl(
attribute vec2 pos;
attribute vec2 uv;
uniform mat3 proj;
varying vec2 v_uv;

void main() {
    vec3 pos3 = vec3(pos, 1.0);
    gl_Position = vec4(pos3 * proj, 1.0);
    v_uv = uv;
}
)glsl";

constexpr std::string_view kDeformableMeshFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_uv;
uniform sampler2D tex;
uniform float alpha;
uniform vec4 u_round; // x=radius, y=power, z=width, w=height

void main() {
    vec4 c = texture2D(tex, v_uv);
    float radius = u_round.x;
    float mask = 1.0;
    if (radius > 0.5) {
        vec2 size = max(u_round.zw, vec2(1.0));
        vec2 p = abs(v_uv * size - size * 0.5) - (size * 0.5 - vec2(radius));
        if (p.x > 0.0 && p.y > 0.0) {
            float pwr = max(u_round.y, 1.0);
            float d = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
            mask = 1.0 - smoothstep(radius - 0.65, radius + 0.65, d);
        }
    }
    if (mask <= 0.001) discard;
    gl_FragColor = c * (alpha * mask);
}
)glsl";

std::string buildSurfaceVariantFragSource(TextureQuadProgram::Target target, uint32_t features) {
    const bool isExt = (target == TextureQuadProgram::Target::ExternalOes ||
                        target == TextureQuadProgram::Target::ExternalOesRgbx);
    const bool isRgbx = (target == TextureQuadProgram::Target::Rgbx ||
                         target == TextureQuadProgram::Target::ExternalOesRgbx);

    std::ostringstream ss;
    if (isExt) {
        ss << "#extension GL_OES_EGL_image_external : require\n";
    }
    if (features & kShaderFeatMirrorMrt) {
        ss << "#extension GL_EXT_draw_buffers : enable\n";
    }

    ss << R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
varying vec2 v_local;
)glsl";

    ss << (isExt ? "uniform samplerExternalOES tex;\n" : "uniform sampler2D tex;\n");
    ss << "uniform float alpha;\n";

    if (features & kShaderFeatRoundCorners) {
        ss << "uniform vec4 u_box;\n"
           << "uniform float u_radius;\n"
           << "uniform float u_power;\n";
    }
    if (features & kShaderFeatBackdropBlur) {
        ss << "uniform sampler2D u_blur_tex;\n"
           << "uniform vec4 u_blur_uv_box;\n"
           << "uniform float u_blur_alpha;\n";
    }
    if (features & kShaderFeatAlphaMatte) {
        ss << "uniform sampler2D u_matte_tex;\n";
    }
    if (features & kShaderFeatDiscard) {
        ss << "uniform int u_discard_opaque;\n"
           << "uniform float u_discard_threshold;\n";
    }
    if (features & kShaderFeatMotionBlur) {
        ss << "uniform vec2 u_motion_vel;\n"
           << "uniform int u_motion_samples;\n";
    }
    if (features & kShaderFeatColorManagement) {
        ss << "uniform int u_src_tf;\n"
           << "uniform int u_dst_tf;\n"
           << "uniform mat3 u_gamut_mat;\n"
           << "uniform vec4 u_lum_params;\n" // x=lumScale, y=sdrSat, z=peakRatio, w=unused
           << "uniform int u_tonemap_op;\n";
        ss << R"glsl(
vec3 evalEotf3(vec3 v, int tf) {
    v = max(v, vec3(0.0));
    if (tf == 1) return v; // Linear
    if (tf == 2) return pow(v, vec3(2.2));
    if (tf == 5) { // PQ ST2084
        vec3 vp = pow(v, vec3(1.0 / 78.84375));
        vec3 num = max(vp - vec3(0.8359375), vec3(0.0));
        vec3 den = max(vec3(18.8515625) - vec3(18.6875) * vp, vec3(1e-6));
        return pow(num / den, vec3(1.0 / 0.1593017578)) * (10000.0 / 203.0);
    }
    // Default sRGB
    return pow((v + vec3(0.055)) / vec3(1.055), vec3(2.4));
}

vec3 evalOetf3(vec3 l, int tf) {
    l = max(l, vec3(0.0));
    if (tf == 1) return l;
    if (tf == 2) return pow(l, vec3(1.0 / 2.2));
    if (tf == 5) {
        vec3 ln = clamp(l * (203.0 / 10000.0), 0.0, 1.0);
        vec3 lm = pow(ln, vec3(0.1593017578));
        return pow((vec3(0.8359375) + vec3(18.8515625) * lm) / (vec3(1.0) + vec3(18.6875) * lm), vec3(78.84375));
    }
    return pow(l, vec3(1.0 / 2.2));
}
)glsl";
    }
    if (features & kShaderFeatIccLut3D) {
        ss << "uniform sampler2D u_lut_tex;\n"
           << "uniform float u_lut_dim;\n";
        ss << R"glsl(
vec3 sampleLut3DStrip(sampler2D lut, vec3 rgb, float dimN) {
    vec3 c = clamp(rgb, 0.0, 1.0) * (dimN - 1.0);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0, dimN - 1.0);
    float bf = c.b - b0;
    vec2 invSize = vec2(1.0 / (dimN * dimN), 1.0 / dimN);
    vec2 uv0 = vec2((b0 * dimN + c.r + 0.5) * invSize.x, (c.g + 0.5) * invSize.y);
    vec2 uv1 = vec2((b1 * dimN + c.r + 0.5) * invSize.x, (c.g + 0.5) * invSize.y);
    return mix(texture2D(lut, uv0).rgb, texture2D(lut, uv1).rgb, bf);
}
)glsl";
    }

    ss << "\nvoid main() {\n";

    // 1. Sample surface texture (with optional directional motion blur)
    if (features & kShaderFeatMotionBlur) {
        ss << R"glsl(
    vec4 pixColor = vec4(0.0);
    float totalW = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= u_motion_samples) break;
        float t = (float(i) / max(float(u_motion_samples - 1), 1.0)) - 0.5;
        pixColor += texture2D(tex, clamp(v_texcoord + u_motion_vel * t, 0.0, 1.0));
        totalW += 1.0;
    }
    pixColor /= max(totalW, 1.0);
)glsl";
    } else {
        ss << "    vec4 pixColor = texture2D(tex, v_texcoord);\n";
    }

    if (isRgbx) {
        ss << "    pixColor.a = 1.0;\n";
    }

    // 2. Discard checks
    if (features & kShaderFeatDiscard) {
        ss << "    if (u_discard_opaque != 0 && pixColor.a * alpha >= 0.999) discard;\n"
           << "    if (pixColor.a <= u_discard_threshold) discard;\n";
    }

    // 3. Color Management & Tonemapping
    if (features & kShaderFeatColorManagement) {
        ss << R"glsl(
    float invA = (pixColor.a > 1e-4) ? (1.0 / pixColor.a) : 0.0;
    vec3 lin = evalEotf3(pixColor.rgb * invA, u_src_tf);
    lin = u_gamut_mat * lin;
    lin *= u_lum_params.x;
    if (u_tonemap_op == 1 || u_tonemap_op == 2) {
        float peak2 = max(u_lum_params.z * u_lum_params.z, 1.0);
        lin = (lin * (vec3(1.0) + lin / peak2)) / (vec3(1.0) + lin);
    } else if (u_tonemap_op == 3) {
        vec3 x = max(vec3(0.0), lin - vec3(0.004));
        lin = (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
    }
    vec3 enc = evalOetf3(lin, u_dst_tf);
    pixColor.rgb = enc * pixColor.a;
)glsl";
    }

    // 4. 3D LUT lookup
    if (features & kShaderFeatIccLut3D) {
        ss << R"glsl(
    if (u_lut_dim >= 2.0) {
        float invA = (pixColor.a > 1e-4) ? (1.0 / pixColor.a) : 0.0;
        vec3 mapped = sampleLut3DStrip(u_lut_tex, pixColor.rgb * invA, u_lut_dim);
        pixColor.rgb = mapped * pixColor.a;
    }
)glsl";
    }

    // 5. Apply surface alpha & optional alpha matte
    ss << "    pixColor *= alpha;\n";
    if (features & kShaderFeatAlphaMatte) {
        ss << "    float matte = clamp(texture2D(u_matte_tex, v_local).r, 0.0, 1.0);\n"
           << "    pixColor *= matte;\n";
    }

    // 6. Composite over pre-blurred backdrop if enabled
    if (features & kShaderFeatBackdropBlur) {
        ss << R"glsl(
    vec2 bgUv = u_blur_uv_box.xy + v_local * u_blur_uv_box.zw;
    vec4 bgCol = texture2D(u_blur_tex, bgUv) * u_blur_alpha;
    pixColor = pixColor + bgCol * (1.0 - pixColor.a);
)glsl";
    }

    // 7. Analytical Squircle Corner Clipping
    if (features & kShaderFeatRoundCorners) {
        ss << R"glsl(
    if (u_radius > 0.5) {
        vec2 size = max(u_box.zw, vec2(1.0));
        vec2 p = abs(v_local * size - size * 0.5) - (size * 0.5 - vec2(u_radius));
        if (p.x > 0.0 && p.y > 0.0) {
            float pwr = max(u_power, 1.0);
            float dist = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
            float cornerMask = 1.0 - smoothstep(u_radius - 0.65, u_radius + 0.65, dist);
            if (cornerMask <= 0.001) discard;
            pixColor *= cornerMask;
        }
    }
)glsl";
    }

    // 8. Output (single target or MRT mirror)
    if (features & kShaderFeatMirrorMrt) {
        ss << R"glsl(
#ifdef GL_EXT_draw_buffers
    gl_FragData[0] = pixColor;
    gl_FragData[1] = pixColor;
#else
    gl_FragColor = pixColor;
#endif
)glsl";
    } else {
        ss << "    gl_FragColor = pixColor;\n";
    }

    ss << "}\n";
    return ss.str();
}

} // namespace

// ==========================================
// Shader Implementation
// ==========================================

std::unique_ptr<Shader> Shader::compile(GLenum type, std::string_view source) {
    GLuint id = glCreateShader(type);
    if (!id) {
        logError("Failed to create OpenGL ES shader of type 0x{:x}", type);
        return nullptr;
    }

    const char* srcPtr = source.data();
    GLint srcLen = static_cast<GLint>(source.size());
    glShaderSource(id, 1, &srcPtr, &srcLen);
    glCompileShader(id);

    GLint status = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> infoLog(static_cast<size_t>(logLen > 1 ? logLen : 256));
        glGetShaderInfoLog(id, static_cast<GLsizei>(infoLog.size()), nullptr, infoLog.data());
        logError("Shader compilation failed (type 0x{:x}): {}", type, infoLog.data());
        glDeleteShader(id);
        return nullptr;
    }

    return std::make_unique<Shader>(type, id);
}

Shader::Shader(GLenum type, GLuint id)
    : m_type(type), m_id(id) {}

Shader::~Shader() {
    if (m_id) {
        glDeleteShader(m_id);
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_type(other.m_type), m_id(other.m_id) {
    other.m_id = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_id) glDeleteShader(m_id);
        m_type = other.m_type;
        m_id = other.m_id;
        other.m_id = 0;
    }
    return *this;
}

// ==========================================
// ShaderProgram Implementation
// ==========================================

std::unique_ptr<ShaderProgram> ShaderProgram::create(std::string_view vertSource, std::string_view fragSource) {
    auto vert = Shader::compile(GL_VERTEX_SHADER, vertSource);
    if (!vert) return nullptr;

    auto frag = Shader::compile(GL_FRAGMENT_SHADER, fragSource);
    if (!frag) return nullptr;

    GLuint prog = glCreateProgram();
    if (!prog) {
        logError("Failed to create OpenGL ES program");
        return nullptr;
    }

    glAttachShader(prog, vert->id());
    glAttachShader(prog, frag->id());
    glLinkProgram(prog);

    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> infoLog(static_cast<size_t>(logLen > 1 ? logLen : 256));
        glGetProgramInfoLog(prog, static_cast<GLsizei>(infoLog.size()), nullptr, infoLog.data());
        logError("Program linking failed: {}", infoLog.data());
        glDeleteProgram(prog);
        return nullptr;
    }

    return std::make_unique<ShaderProgram>(prog);
}

ShaderProgram::ShaderProgram(GLuint programId)
    : m_program(programId) {}

ShaderProgram::~ShaderProgram() {
    if (m_program) {
        glDeleteProgram(m_program);
    }
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : m_program(other.m_program) {
    other.m_program = 0;
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        if (m_program) glDeleteProgram(m_program);
        m_program = other.m_program;
        other.m_program = 0;
    }
    return *this;
}

void ShaderProgram::use() const noexcept {
    glUseProgram(m_program);
}

GLint ShaderProgram::getUniformLocation(const char* name) const noexcept {
    return glGetUniformLocation(m_program, name);
}

GLint ShaderProgram::getAttribLocation(const char* name) const noexcept {
    return glGetAttribLocation(m_program, name);
}

// ==========================================
// SolidQuadProgram Implementation
// ==========================================

std::unique_ptr<SolidQuadProgram> SolidQuadProgram::create() {
    auto base = ShaderProgram::create(kUnitBoxVert, kSolidQuadFrag);
    if (!base) return nullptr;

    GLuint id = base->release();
    return std::make_unique<SolidQuadProgram>(id);
}

SolidQuadProgram::SolidQuadProgram(GLuint programId)
    : ShaderProgram(programId) {
    m_projLoc = getUniformLocation("proj");
    m_colorLoc = getUniformLocation("color");
    m_boxLoc = getUniformLocation("u_box");
    m_radiusLoc = getUniformLocation("u_radius");
    m_powerLoc = getUniformLocation("u_power");
    m_posAttrib = getAttribLocation("pos");
}

void SolidQuadProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) {
        glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
    }
}

void SolidQuadProgram::setColor(const Color& color) const noexcept {
    if (m_colorLoc >= 0) {
        glUniform4f(m_colorLoc, color.r, color.g, color.b, color.a);
    }
    if (m_radiusLoc >= 0) {
        glUniform1f(m_radiusLoc, 0.0f);
    }
}

void SolidQuadProgram::setRounding(const RectI& box, float radius, float power) const noexcept {
    if (m_boxLoc >= 0) {
        glUniform4f(
            m_boxLoc,
            static_cast<float>(box.x),
            static_cast<float>(box.y),
            static_cast<float>(box.width),
            static_cast<float>(box.height));
    }
    if (m_radiusLoc >= 0) glUniform1f(m_radiusLoc, radius);
    if (m_powerLoc >= 0) glUniform1f(m_powerLoc, power);
}

// ==========================================
// TextureQuadProgram Implementation
// ==========================================

std::unique_ptr<TextureQuadProgram> TextureQuadProgram::create(Target target) {
    std::string_view fragSrc;
    switch (target) {
        case Target::Rgba:
            fragSrc = kTextureQuadRgbaFrag;
            break;
        case Target::Rgbx:
            fragSrc = kTextureQuadRgbxFrag;
            break;
        case Target::ExternalOes:
            fragSrc = kTextureQuadExternalFrag;
            break;
        case Target::ExternalOesRgbx:
            fragSrc = kTextureQuadExternalRgbxFrag;
            break;
    }

    auto base = ShaderProgram::create(kTextureQuadVert, fragSrc);
    if (!base) return nullptr;
    return std::make_unique<TextureQuadProgram>(base->release(), target);
}

TextureQuadProgram::TextureQuadProgram(GLuint programId, Target target)
    : ShaderProgram(programId), m_target(target) {
    m_projLoc = getUniformLocation("proj");
    m_texProjLoc = getUniformLocation("tex_proj");
    m_texLoc = getUniformLocation("tex");
    m_alphaLoc = getUniformLocation("alpha");
    m_posAttrib = getAttribLocation("pos");
}

void TextureQuadProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) {
        glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
    }
}

void TextureQuadProgram::setTextureProjection(const Matrix3& texProj) const noexcept {
    if (m_texProjLoc >= 0) {
        glUniformMatrix3fv(m_texProjLoc, 1, GL_FALSE, texProj.data());
    }
}

void TextureQuadProgram::setAlpha(float alpha) const noexcept {
    if (m_alphaLoc >= 0) {
        glUniform1f(m_alphaLoc, alpha);
    }
}

void TextureQuadProgram::setTextureUnit(int unit) const noexcept {
    if (m_texLoc >= 0) {
        glUniform1i(m_texLoc, unit);
    }
}

// ==========================================
// SurfaceVariantProgram Implementation
// ==========================================

std::unique_ptr<SurfaceVariantProgram> SurfaceVariantProgram::create(
    TextureQuadProgram::Target target,
    uint32_t features) {
    const std::string fragSrc = buildSurfaceVariantFragSource(target, features);
    auto base = ShaderProgram::create(kTextureQuadVert, fragSrc);
    if (!base) return nullptr;
    return std::make_unique<SurfaceVariantProgram>(base->release(), target, features);
}

SurfaceVariantProgram::SurfaceVariantProgram(
    GLuint programId,
    TextureQuadProgram::Target target,
    uint32_t features)
    : ShaderProgram(programId), m_target(target), m_features(features) {
    m_posAttrib = getAttribLocation("pos");
    m_projLoc = getUniformLocation("proj");
    m_texProjLoc = getUniformLocation("tex_proj");
    m_texLoc = getUniformLocation("tex");
    m_alphaLoc = getUniformLocation("alpha");

    m_boxLoc = getUniformLocation("u_box");
    m_radiusLoc = getUniformLocation("u_radius");
    m_powerLoc = getUniformLocation("u_power");

    m_blurTexLoc = getUniformLocation("u_blur_tex");
    m_blurUvBoxLoc = getUniformLocation("u_blur_uv_box");
    m_blurAlphaLoc = getUniformLocation("u_blur_alpha");

    m_matteTexLoc = getUniformLocation("u_matte_tex");

    m_discardOpaqueLoc = getUniformLocation("u_discard_opaque");
    m_discardThresholdLoc = getUniformLocation("u_discard_threshold");

    m_motionVelLoc = getUniformLocation("u_motion_vel");
    m_motionSamplesLoc = getUniformLocation("u_motion_samples");

    m_srcTfLoc = getUniformLocation("u_src_tf");
    m_dstTfLoc = getUniformLocation("u_dst_tf");
    m_gamutMatLoc = getUniformLocation("u_gamut_mat");
    m_lumParamsLoc = getUniformLocation("u_lum_params");
    m_tonemapOpLoc = getUniformLocation("u_tonemap_op");

    m_lutTexLoc = getUniformLocation("u_lut_tex");
    m_lutDimLoc = getUniformLocation("u_lut_dim");
}

void SurfaceVariantProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
}

void SurfaceVariantProgram::setTextureProjection(const Matrix3& texProj) const noexcept {
    if (m_texProjLoc >= 0) glUniformMatrix3fv(m_texProjLoc, 1, GL_FALSE, texProj.data());
}

void SurfaceVariantProgram::setTextureUnit(int unit) const noexcept {
    if (m_texLoc >= 0) glUniform1i(m_texLoc, unit);
}

void SurfaceVariantProgram::setAlpha(float alpha) const noexcept {
    if (m_alphaLoc >= 0) glUniform1f(m_alphaLoc, alpha);
}

void SurfaceVariantProgram::setRounding(const RectI& box, float radius, float power) const noexcept {
    if (m_boxLoc >= 0) {
        glUniform4f(
            m_boxLoc,
            static_cast<float>(box.x),
            static_cast<float>(box.y),
            static_cast<float>(box.width),
            static_cast<float>(box.height));
    }
    if (m_radiusLoc >= 0) glUniform1f(m_radiusLoc, radius);
    if (m_powerLoc >= 0) glUniform1f(m_powerLoc, power);
}

void SurfaceVariantProgram::setBackdropBlur(int texUnit, const RectF& uvBox, float blurAlpha) const noexcept {
    if (m_blurTexLoc >= 0) glUniform1i(m_blurTexLoc, texUnit);
    if (m_blurUvBoxLoc >= 0) {
        glUniform4f(
            m_blurUvBoxLoc,
            static_cast<float>(uvBox.x),
            static_cast<float>(uvBox.y),
            static_cast<float>(uvBox.width),
            static_cast<float>(uvBox.height));
    }
    if (m_blurAlphaLoc >= 0) glUniform1f(m_blurAlphaLoc, blurAlpha);
}

void SurfaceVariantProgram::setAlphaMatte(int texUnit) const noexcept {
    if (m_matteTexLoc >= 0) glUniform1i(m_matteTexLoc, texUnit);
}

void SurfaceVariantProgram::setDiscard(bool discardOpaque, float discardAlphaThreshold) const noexcept {
    if (m_discardOpaqueLoc >= 0) glUniform1i(m_discardOpaqueLoc, discardOpaque ? 1 : 0);
    if (m_discardThresholdLoc >= 0) glUniform1f(m_discardThresholdLoc, discardAlphaThreshold);
}

void SurfaceVariantProgram::setMotionBlur(float velocityUvX, float velocityUvY, int samples) const noexcept {
    if (m_motionVelLoc >= 0) glUniform2f(m_motionVelLoc, velocityUvX, velocityUvY);
    if (m_motionSamplesLoc >= 0) glUniform1i(m_motionSamplesLoc, std::clamp(samples, 2, 8));
}

void SurfaceVariantProgram::setColorTransform(const ColorTransformParams& cm) const noexcept {
    if (m_srcTfLoc >= 0) glUniform1i(m_srcTfLoc, static_cast<int>(cm.sourceTf));
    if (m_dstTfLoc >= 0) glUniform1i(m_dstTfLoc, static_cast<int>(cm.targetTf));
    if (m_gamutMatLoc >= 0) glUniformMatrix3fv(m_gamutMatLoc, 1, GL_FALSE, cm.gamutMatrix.data());
    if (m_lumParamsLoc >= 0) {
        const float lumScale = (cm.srcRefLuminance / std::max(cm.dstRefLuminance, 1.0f)) * cm.sdrBrightness;
        const float peakRatio = std::max(cm.dstMaxLuminance / std::max(cm.dstRefLuminance, 1.0f), 1.0f);
        glUniform4f(m_lumParamsLoc, lumScale, cm.sdrSaturation, peakRatio, 0.0f);
    }
    if (m_tonemapOpLoc >= 0) glUniform1i(m_tonemapOpLoc, static_cast<int>(cm.tonemap));
}

void SurfaceVariantProgram::setLut3D(int texUnit, float dimN) const noexcept {
    if (m_lutTexLoc >= 0) glUniform1i(m_lutTexLoc, texUnit);
    if (m_lutDimLoc >= 0) glUniform1f(m_lutDimLoc, dimN);
}

// ==========================================
// BorderProgram Implementation
// ==========================================

std::unique_ptr<BorderProgram> BorderProgram::create() {
    auto base = ShaderProgram::create(kUnitBoxVert, buildBorderFragSource());
    if (!base) return nullptr;
    return std::make_unique<BorderProgram>(base->release());
}

BorderProgram::BorderProgram(GLuint programId)
    : ShaderProgram(programId) {
    m_posAttrib = getAttribLocation("pos");
    m_projLoc = getUniformLocation("proj");
    m_boxLoc = getUniformLocation("u_box");
    m_radiiLoc = getUniformLocation("u_radii");
    m_grad1Loc = getUniformLocation("u_grad1[0]");
    if (m_grad1Loc < 0) m_grad1Loc = getUniformLocation("u_grad1");
    m_grad1CountLoc = getUniformLocation("u_grad1_count");
    m_grad1AngleLoc = getUniformLocation("u_grad1_angle");
    m_grad2Loc = getUniformLocation("u_grad2[0]");
    if (m_grad2Loc < 0) m_grad2Loc = getUniformLocation("u_grad2");
    m_grad2CountLoc = getUniformLocation("u_grad2_count");
    m_grad2AngleLoc = getUniformLocation("u_grad2_angle");
    m_lerpAlphaLoc = getUniformLocation("u_lerp_alpha");
}

void BorderProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
}

void BorderProgram::setGeometry(
    const RectI& box,
    float innerRadius,
    float outerRadius,
    float roundingPower,
    float thickness) const noexcept {
    if (m_boxLoc >= 0) {
        glUniform4f(
            m_boxLoc,
            static_cast<float>(box.x),
            static_cast<float>(box.y),
            static_cast<float>(box.width),
            static_cast<float>(box.height));
    }
    if (m_radiiLoc >= 0) {
        glUniform4f(m_radiiLoc, innerRadius, outerRadius, roundingPower, thickness);
    }
}

void BorderProgram::setGradients(
    const GradientSpec& grad1,
    const GradientSpec* grad2,
    float lerpFactor,
    float alpha) const noexcept {
    const auto p1 = grad1.packOklabStops();
    const int c1 = static_cast<int>(p1.size() / 4);
    if (m_grad1Loc >= 0 && c1 > 0) {
        glUniform4fv(m_grad1Loc, c1, p1.data());
    }
    if (m_grad1CountLoc >= 0) glUniform1i(m_grad1CountLoc, c1);
    if (m_grad1AngleLoc >= 0) glUniform1f(m_grad1AngleLoc, grad1.angleRad);

    int c2 = 0;
    if (grad2 && !grad2->isEmpty()) {
        const auto p2 = grad2->packOklabStops();
        c2 = static_cast<int>(p2.size() / 4);
        if (m_grad2Loc >= 0 && c2 > 0) {
            glUniform4fv(m_grad2Loc, c2, p2.data());
        }
        if (m_grad2AngleLoc >= 0) glUniform1f(m_grad2AngleLoc, grad2->angleRad);
    }
    if (m_grad2CountLoc >= 0) glUniform1i(m_grad2CountLoc, c2);
    if (m_lerpAlphaLoc >= 0) glUniform2f(m_lerpAlphaLoc, std::clamp(lerpFactor, 0.0f, 1.0f), alpha);
}

// ==========================================
// ShadowProgram Implementation
// ==========================================

std::unique_ptr<ShadowProgram> ShadowProgram::create() {
    auto base = ShaderProgram::create(kUnitBoxVert, buildShadowFragSource());
    if (!base) return nullptr;
    return std::make_unique<ShadowProgram>(base->release());
}

ShadowProgram::ShadowProgram(GLuint programId)
    : ShaderProgram(programId) {
    m_posAttrib = getAttribLocation("pos");
    m_projLoc = getUniformLocation("proj");
    m_boxLoc = getUniformLocation("u_box");
    m_paramsLoc = getUniformLocation("u_params");
    m_grad1Loc = getUniformLocation("u_grad1[0]");
    if (m_grad1Loc < 0) m_grad1Loc = getUniformLocation("u_grad1");
    m_grad1CountLoc = getUniformLocation("u_grad1_count");
    m_grad1AngleLoc = getUniformLocation("u_grad1_angle");
    m_grad2Loc = getUniformLocation("u_grad2[0]");
    if (m_grad2Loc < 0) m_grad2Loc = getUniformLocation("u_grad2");
    m_grad2CountLoc = getUniformLocation("u_grad2_count");
    m_grad2AngleLoc = getUniformLocation("u_grad2_angle");
    m_lerpAlphaLoc = getUniformLocation("u_lerp_alpha");
}

void ShadowProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
}

void ShadowProgram::setShadowParams(
    const RectI& fullBox,
    float cornerRadius,
    float roundingPower,
    float rangeSigma,
    float falloffPower,
    const GradientSpec& grad1,
    const GradientSpec* grad2,
    float lerpFactor,
    float alpha) const noexcept {
    if (m_boxLoc >= 0) {
        glUniform4f(
            m_boxLoc,
            static_cast<float>(fullBox.x),
            static_cast<float>(fullBox.y),
            static_cast<float>(fullBox.width),
            static_cast<float>(fullBox.height));
    }
    if (m_paramsLoc >= 0) {
        glUniform4f(m_paramsLoc, cornerRadius, roundingPower, rangeSigma, falloffPower);
    }

    const auto p1 = grad1.packOklabStops();
    const int c1 = static_cast<int>(p1.size() / 4);
    if (m_grad1Loc >= 0 && c1 > 0) glUniform4fv(m_grad1Loc, c1, p1.data());
    if (m_grad1CountLoc >= 0) glUniform1i(m_grad1CountLoc, c1);
    if (m_grad1AngleLoc >= 0) glUniform1f(m_grad1AngleLoc, grad1.angleRad);

    int c2 = 0;
    if (grad2 && !grad2->isEmpty()) {
        const auto p2 = grad2->packOklabStops();
        c2 = static_cast<int>(p2.size() / 4);
        if (m_grad2Loc >= 0 && c2 > 0) glUniform4fv(m_grad2Loc, c2, p2.data());
        if (m_grad2AngleLoc >= 0) glUniform1f(m_grad2AngleLoc, grad2->angleRad);
    }
    if (m_grad2CountLoc >= 0) glUniform1i(m_grad2CountLoc, c2);
    if (m_lerpAlphaLoc >= 0) glUniform2f(m_lerpAlphaLoc, std::clamp(lerpFactor, 0.0f, 1.0f), alpha);
}

// ==========================================
// InnerGlowProgram Implementation
// ==========================================

std::unique_ptr<InnerGlowProgram> InnerGlowProgram::create() {
    auto base = ShaderProgram::create(kUnitBoxVert, buildInnerGlowFragSource());
    if (!base) return nullptr;
    return std::make_unique<InnerGlowProgram>(base->release());
}

InnerGlowProgram::InnerGlowProgram(GLuint programId)
    : ShaderProgram(programId) {
    m_posAttrib = getAttribLocation("pos");
    m_projLoc = getUniformLocation("proj");
    m_boxLoc = getUniformLocation("u_box");
    m_paramsLoc = getUniformLocation("u_params");
    m_gradLoc = getUniformLocation("u_grad[0]");
    if (m_gradLoc < 0) m_gradLoc = getUniformLocation("u_grad");
    m_gradCountLoc = getUniformLocation("u_grad_count");
    m_gradAngleLoc = getUniformLocation("u_grad_angle");
    m_alphaLoc = getUniformLocation("u_alpha");
}

void InnerGlowProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
}

void InnerGlowProgram::setGlowParams(
    const RectI& box,
    float cornerRadius,
    float roundingPower,
    float range,
    float falloffPower,
    const GradientSpec& grad,
    float alpha) const noexcept {
    if (m_boxLoc >= 0) {
        glUniform4f(
            m_boxLoc,
            static_cast<float>(box.x),
            static_cast<float>(box.y),
            static_cast<float>(box.width),
            static_cast<float>(box.height));
    }
    if (m_paramsLoc >= 0) {
        glUniform4f(m_paramsLoc, cornerRadius, roundingPower, range, falloffPower);
    }
    const auto p = grad.packOklabStops();
    const int c = static_cast<int>(p.size() / 4);
    if (m_gradLoc >= 0 && c > 0) glUniform4fv(m_gradLoc, c, p.data());
    if (m_gradCountLoc >= 0) glUniform1i(m_gradCountLoc, c);
    if (m_gradAngleLoc >= 0) glUniform1f(m_gradAngleLoc, grad.angleRad);
    if (m_alphaLoc >= 0) glUniform1f(m_alphaLoc, alpha);
}

// ==========================================
// DeformableMeshProgram Implementation
// ==========================================

std::unique_ptr<DeformableMeshProgram> DeformableMeshProgram::create() {
    auto base = ShaderProgram::create(kDeformableMeshVert, kDeformableMeshFrag);
    if (!base) return nullptr;
    return std::make_unique<DeformableMeshProgram>(base->release());
}

DeformableMeshProgram::DeformableMeshProgram(GLuint programId)
    : ShaderProgram(programId) {
    m_posAttrib = getAttribLocation("pos");
    m_uvAttrib = getAttribLocation("uv");
    m_projLoc = getUniformLocation("proj");
    m_texLoc = getUniformLocation("tex");
    m_alphaLoc = getUniformLocation("alpha");
    m_roundLoc = getUniformLocation("u_round");
}

void DeformableMeshProgram::setProjection(const Matrix3& proj) const noexcept {
    if (m_projLoc >= 0) glUniformMatrix3fv(m_projLoc, 1, GL_FALSE, proj.data());
}

void DeformableMeshProgram::setTextureUnit(int unit) const noexcept {
    if (m_texLoc >= 0) glUniform1i(m_texLoc, unit);
}

void DeformableMeshProgram::setAlpha(float alpha) const noexcept {
    if (m_alphaLoc >= 0) glUniform1f(m_alphaLoc, alpha);
}

void DeformableMeshProgram::setRounding(float radius, float power, float width, float height) const noexcept {
    if (m_roundLoc >= 0) glUniform4f(m_roundLoc, radius, power, width, height);
}

// ==========================================
// ShaderManager Implementation
// ==========================================

std::shared_ptr<ShaderManager> ShaderManager::create(bool supportExternalOes) {
    auto solid = SolidQuadProgram::create();
    if (!solid) {
        logError("Failed to compile SolidQuadProgram");
        return nullptr;
    }

    auto texRgba = TextureQuadProgram::create(TextureQuadProgram::Target::Rgba);
    if (!texRgba) {
        logError("Failed to compile TextureQuadProgram (RGBA)");
        return nullptr;
    }

    auto texRgbx = TextureQuadProgram::create(TextureQuadProgram::Target::Rgbx);
    if (!texRgbx) {
        logError("Failed to compile TextureQuadProgram (RGBX)");
        return nullptr;
    }

    std::unique_ptr<TextureQuadProgram> texExternal = nullptr;
    std::unique_ptr<TextureQuadProgram> texExternalRgbx = nullptr;
    if (supportExternalOes) {
        texExternal = TextureQuadProgram::create(TextureQuadProgram::Target::ExternalOes);
        texExternalRgbx = TextureQuadProgram::create(TextureQuadProgram::Target::ExternalOesRgbx);
    }

    auto border = BorderProgram::create();
    auto shadow = ShadowProgram::create();
    auto innerGlow = InnerGlowProgram::create();
    auto mesh = DeformableMeshProgram::create();

    if (!border || !shadow || !innerGlow || !mesh) {
        logError("Failed to compile analytical decoration or mesh shaders");
        return nullptr;
    }

    return std::make_shared<ShaderManager>(
        std::move(solid),
        std::move(texRgba),
        std::move(texRgbx),
        std::move(texExternal),
        std::move(texExternalRgbx),
        std::move(border),
        std::move(shadow),
        std::move(innerGlow),
        std::move(mesh));
}

ShaderManager::ShaderManager(
    std::unique_ptr<SolidQuadProgram> solid,
    std::unique_ptr<TextureQuadProgram> texRgba,
    std::unique_ptr<TextureQuadProgram> texRgbx,
    std::unique_ptr<TextureQuadProgram> texExternal,
    std::unique_ptr<TextureQuadProgram> texExternalRgbx,
    std::unique_ptr<BorderProgram> border,
    std::unique_ptr<ShadowProgram> shadow,
    std::unique_ptr<InnerGlowProgram> innerGlow,
    std::unique_ptr<DeformableMeshProgram> mesh)
    : m_solidQuad(std::move(solid)),
      m_textureRgba(std::move(texRgba)),
      m_textureRgbx(std::move(texRgbx)),
      m_textureExternal(std::move(texExternal)),
      m_textureExternalRgbx(std::move(texExternalRgbx)),
      m_border(std::move(border)),
      m_shadow(std::move(shadow)),
      m_innerGlow(std::move(innerGlow)),
      m_mesh(std::move(mesh)) {}

const SurfaceVariantProgram* ShaderManager::getSurfaceVariant(
    TextureQuadProgram::Target target,
    uint32_t features) {
    const uint64_t key = (static_cast<uint64_t>(target) << 32) | static_cast<uint64_t>(features);
    auto it = m_variants.find(key);
    if (it != m_variants.end()) {
        return it->second.get();
    }

    auto prog = SurfaceVariantProgram::create(target, features);
    if (!prog) {
        return nullptr;
    }
    const auto* rawPtr = prog.get();
    m_variants.emplace(key, std::move(prog));
    return rawPtr;
}

ShaderManager::~ShaderManager() {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return;
    }
    if (m_meshStreamingVbo.id != 0) {
        glDeleteBuffers(1, &m_meshStreamingVbo.id);
        m_meshStreamingVbo.id = 0;
    }
    if (m_quadStreamingVbo.id != 0) {
        glDeleteBuffers(1, &m_quadStreamingVbo.id);
        m_quadStreamingVbo.id = 0;
    }
    if (m_unitQuadVbo != 0) {
        glDeleteBuffers(1, &m_unitQuadVbo);
        m_unitQuadVbo = 0;
    }
}

GLuint ShaderManager::uploadStreamingVbo(
    StreamingVbo& buf,
    const void* data,
    GLsizeiptr byteSize,
    GLsizeiptr minCapacity) const {
    if (!data || byteSize <= 0) {
        return 0;
    }
    if (buf.id == 0) {
        glGenBuffers(1, &buf.id);
        if (buf.id == 0) {
            return 0;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf.id);
    if (byteSize > buf.capacity) {
        buf.capacity = std::max(minCapacity, std::max(byteSize, buf.capacity * 2));
    }
    // Orphan previous backing store to avoid implicit CPU-GPU synchronization stalls
    glBufferData(GL_ARRAY_BUFFER, buf.capacity, nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, byteSize, data);
    return buf.id;
}

GLuint ShaderManager::bindMeshStreamingVbo(const void* data, GLsizeiptr byteSize) const {
    return uploadStreamingVbo(m_meshStreamingVbo, data, byteSize, 16384);
}

GLuint ShaderManager::bindQuadStreamingVbo(const void* data, GLsizeiptr byteSize) const {
    return uploadStreamingVbo(m_quadStreamingVbo, data, byteSize, 4096);
}

GLuint ShaderManager::bindUnitQuadVbo() const {
    if (m_unitQuadVbo == 0) {
        static constexpr float kUnitQuad[12] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            0.0f, 1.0f,
            1.0f, 0.0f,
            1.0f, 1.0f,
            0.0f, 1.0f,
        };
        glGenBuffers(1, &m_unitQuadVbo);
        if (m_unitQuadVbo == 0) {
            return 0;
        }
        glBindBuffer(GL_ARRAY_BUFFER, m_unitQuadVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
        return m_unitQuadVbo;
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_unitQuadVbo);
    return m_unitQuadVbo;
}

} // namespace nisaba::compositor
