#include "nisaba/compositor/blur_provider.hpp"
#include "nisaba/compositor/log.hpp"
#include "nisaba/compositor/matrix3.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::compositor {

namespace {

constexpr std::string_view kMaterialVert = R"glsl(
attribute vec2 pos;
uniform mat3 u_proj;
uniform vec4 u_uv_rect; // x, y, width, height
varying vec2 v_local;
varying vec2 v_uv;

void main() {
    vec3 p3 = vec3(pos, 1.0);
    gl_Position = vec4(p3 * u_proj, 1.0);
    v_local = pos;
    v_uv = u_uv_rect.xy + pos * u_uv_rect.zw;
}
)glsl";

constexpr std::string_view kAcrylicFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_tint;
uniform vec4 u_params; // x=brightness, y=contrast, z=noise, w=cornerRadius
uniform vec2 u_box_size;
uniform float u_power;

float ignNoise(vec2 coord) {
    return fract(52.9829189 * fract(dot(coord, vec2(0.06711056, 0.00583715)))) - 0.5;
}

float squircleMask(vec2 localPos, vec2 size, float radius, float power) {
    if (radius <= 0.5) return 1.0;
    vec2 p = abs(localPos * size - size * 0.5) - (size * 0.5 - vec2(radius));
    if (p.x <= 0.0 || p.y <= 0.0) return 1.0;
    float pwr = max(power, 1.0);
    float d = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
    return 1.0 - smoothstep(radius - 0.65, radius + 0.65, d);
}

void main() {
    float mask = squircleMask(v_local, u_box_size, u_params.w, u_power);
    if (mask <= 0.001) discard;

    vec3 c = texture2D(u_tex, v_uv).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Boost saturation by 25% and apply luminosity exclusion
    c = mix(vec3(luma), c, 1.25);
    c = (c - 0.5) * u_params.y + 0.5;
    c *= u_params.x;
    // Exclusion layer for authentic Fluent Acrylic look
    c = c + vec3(0.06) - 2.0 * c * vec3(0.06);
    c = mix(c, u_tint.rgb, u_tint.a);
    c += ignNoise(gl_FragCoord.xy) * (u_params.z / 255.0);

    gl_FragColor = vec4(clamp(c, 0.0, 1.0) * mask, mask);
}
)glsl";

constexpr std::string_view kOpticalGlassFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_tint;
uniform vec4 u_glass; // x=refraction, y=dispersion, z=specular, w=cornerRadius
uniform vec2 u_box_size;
uniform float u_power;

void main() {
    vec2 size = max(u_box_size, vec2(1.0));
    vec2 halfSize = size * 0.5;
    float radius = min(u_glass.w, min(halfSize.x, halfSize.y));
    float pwr = max(u_power, 1.0);

    vec2 centered = v_local * size - halfSize;
    vec2 q = abs(centered) - (halfSize - vec2(radius));
    float dist = 0.0;
    if (q.x > 0.0 && q.y > 0.0) {
        dist = pow(pow(q.x, pwr) + pow(q.y, pwr), 1.0 / pwr) - radius;
    } else {
        dist = max(q.x, q.y) - radius;
    }

    float mask = 1.0 - smoothstep(-0.65, 0.65, dist);
    if (mask <= 0.001) discard;

    // Edge bevel normal for optical lens refraction
    float bevelWidth = max(min(size.x, size.y) * 0.18, 16.0);
    float bevelFactor = clamp(1.0 + dist / bevelWidth, 0.0, 1.0);
    vec2 normalDir = normalize(centered + vec2(1e-4));
    vec2 refractOffset = normalDir * (bevelFactor * bevelFactor * u_glass.x);
    vec2 dispOffset = normalDir * (bevelFactor * u_glass.y);

    float r = texture2D(u_tex, clamp(v_uv - refractOffset + dispOffset, 0.0, 1.0)).r;
    float g = texture2D(u_tex, clamp(v_uv - refractOffset, 0.0, 1.0)).g;
    float b = texture2D(u_tex, clamp(v_uv - refractOffset - dispOffset, 0.0, 1.0)).b;
    vec3 col = vec3(r, g, b);

    // Specular rim reflection
    float rim = pow(bevelFactor, 4.0) * u_glass.z * (0.6 + 0.4 * max(-normalDir.y, 0.0));
    col = mix(col, u_tint.rgb, u_tint.a);
    col += vec3(rim);

    gl_FragColor = vec4(clamp(col, 0.0, 1.0) * mask, mask);
}
)glsl";

constexpr std::string_view kWaterRippleFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_tint;
uniform vec4 u_wave; // x=time, y=cursorX, z=cursorY, w=strength
uniform vec2 u_box_size;
uniform float u_radius;
uniform float u_power;

void main() {
    vec2 size = max(u_box_size, vec2(1.0));
    float mask = 1.0;
    if (u_radius > 0.5) {
        vec2 p = abs(v_local * size - size * 0.5) - (size * 0.5 - vec2(u_radius));
        if (p.x > 0.0 && p.y > 0.0) {
            float pwr = max(u_power, 1.0);
            float d = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
            mask = 1.0 - smoothstep(u_radius - 0.65, u_radius + 0.65, d);
        }
    }
    if (mask <= 0.001) discard;

    vec2 delta = v_local - u_wave.yz;
    float dist = length(delta);
    float phase1 = dist * 42.0 - u_wave.x * 7.5;
    float phase2 = dist * 68.0 - u_wave.x * 11.2;
    float attenuation = exp(-dist * 4.5);

    float wave = (sin(phase1) * 0.65 + cos(phase2) * 0.35) * attenuation;
    vec2 dir = delta / max(dist, 1e-4);
    vec2 uvOffset = dir * wave * 0.015 * u_wave.w;

    vec3 col = texture2D(u_tex, clamp(v_uv + uvOffset, 0.0, 1.0)).rgb;
    float caustic = max(0.0, wave) * 0.18 * u_wave.w;
    col = mix(col, u_tint.rgb, u_tint.a) + vec3(caustic);

    gl_FragColor = vec4(clamp(col, 0.0, 1.0) * mask, mask);
}
)glsl";

constexpr std::string_view kFluidFieldFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_tint;
uniform vec4 u_fluid; // x=time, y=velX, z=velY, w=strength
uniform vec2 u_box_size;
uniform float u_radius;
uniform float u_power;

void main() {
    vec2 size = max(u_box_size, vec2(1.0));
    float mask = 1.0;
    if (u_radius > 0.5) {
        vec2 p = abs(v_local * size - size * 0.5) - (size * 0.5 - vec2(u_radius));
        if (p.x > 0.0 && p.y > 0.0) {
            float pwr = max(u_power, 1.0);
            float d = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
            mask = 1.0 - smoothstep(u_radius - 0.65, u_radius + 0.65, d);
        }
    }
    if (mask <= 0.001) discard;

    float t = u_fluid.x;
    vec2 p = v_local * 3.14159265 * 2.0;
    // Divergence-free stream function curl advection
    vec2 swirl = vec2(
        sin(p.y + t * 1.4) + 0.5 * cos(2.0 * p.y - t * 0.9),
        -sin(p.x - t * 1.2) - 0.5 * cos(2.0 * p.x + t * 1.1)
    );
    vec2 advect = (swirl * 0.008 + u_fluid.yz * 0.025) * u_fluid.w;

    vec3 col = texture2D(u_tex, clamp(v_uv + advect, 0.0, 1.0)).rgb;
    col = mix(col, u_tint.rgb, u_tint.a);

    gl_FragColor = vec4(clamp(col, 0.0, 1.0) * mask, mask);
}
)glsl";

constexpr std::string_view kAtmosphericAuroraFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_local;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_tint;
uniform vec4 u_atmos; // x=time, y=strength, z=cornerRadius, w=power
uniform vec2 u_box_size;

void main() {
    vec2 size = max(u_box_size, vec2(1.0));
    float mask = 1.0;
    float radius = u_atmos.z;
    if (radius > 0.5) {
        vec2 p = abs(v_local * size - size * 0.5) - (size * 0.5 - vec2(radius));
        if (p.x > 0.0 && p.y > 0.0) {
            float pwr = max(u_atmos.w, 1.0);
            float d = pow(pow(p.x, pwr) + pow(p.y, pwr), 1.0 / pwr);
            mask = 1.0 - smoothstep(radius - 0.65, radius + 0.65, d);
        }
    }
    if (mask <= 0.001) discard;

    float t = u_atmos.x;
    vec2 shimmer = vec2(
        sin(v_local.y * 18.0 + t * 2.5) * 0.003,
        cos(v_local.x * 14.0 + t * 2.0) * 0.003
    ) * u_atmos.y;

    vec3 base = texture2D(u_tex, clamp(v_uv + shimmer, 0.0, 1.0)).rgb;
    float ribbon1 = 0.5 + 0.5 * sin(v_local.x * 4.5 + v_local.y * 2.2 + t * 0.8);
    float ribbon2 = 0.5 + 0.5 * cos(v_local.x * 3.1 - v_local.y * 3.7 - t * 0.6);
    vec3 auroraCol = mix(vec3(0.10, 0.85, 0.65), vec3(0.45, 0.25, 0.95), ribbon2) * (ribbon1 * 0.14 * u_atmos.y);

    vec3 col = mix(base + auroraCol, u_tint.rgb, u_tint.a);
    gl_FragColor = vec4(clamp(col, 0.0, 1.0) * mask, mask);
}
)glsl";

RectF computeNormalizedUvRect(const Texture& source, const RectI& srcRect, const RectI& destRect) {
    const float texW = static_cast<float>(std::max(source.width(), 1u));
    const float texH = static_cast<float>(std::max(source.height(), 1u));
    const RectI effective = srcRect.isEmpty() ? destRect : srcRect;
    return RectF{
        static_cast<double>(effective.x) / texW,
        static_cast<double>(effective.y) / texH,
        static_cast<double>(effective.width) / texW,
        static_cast<double>(effective.height) / texH
    };
}

} // namespace

int IBlurProvider::damageExpansionRadius(const BlurContext& ctx) const noexcept {
    const int passes = std::clamp(ctx.kawase.passes, 1, 6);
    const float radius = std::max(ctx.kawase.radius, 1.0f) * std::max(ctx.strength, 0.1f);
    return static_cast<int>(std::ceil(radius * static_cast<float>(1 << passes)));
}

void IBlurProvider::expandDamage(
    Region& damage,
    const BlurContext& ctx,
    float multiplier) const noexcept {
    if (damage.isEmpty()) return;
    const int pad = static_cast<int>(std::ceil(static_cast<float>(damageExpansionRadius(ctx)) * std::max(multiplier, 0.0f)));
    if (pad <= 0) return;

    const auto rects = damage.rects();
    Region expanded;
    for (const auto& r : rects) {
        expanded.add(RectI{
            r.x - pad,
            r.y - pad,
            r.width + pad * 2,
            r.height + pad * 2
        });
    }
    damage = std::move(expanded);
}

// ==========================================
// KawaseBlurProvider
// ==========================================

bool KawaseBlurProvider::renderMaterial(
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const BlurContext& ctx) {
    return kawase.renderBlurSubRegion(source, srcRect, target, destRect, ctx.kawase, ctx.maskTexture);
}

// ==========================================
// AcrylicBlurProvider
// ==========================================

AcrylicBlurProvider::AcrylicBlurProvider() {
    m_program = ShaderProgram::create(kMaterialVert, kAcrylicFrag);
}

bool AcrylicBlurProvider::renderMaterial(
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const BlurContext& ctx) {
    auto blurred = kawase.blur(source, ctx.kawase);
    if (!blurred || !m_program) {
        return kawase.renderBlurSubRegion(source, srcRect, target, destRect, ctx.kawase, ctx.maskTexture);
    }

    target.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_program->use();
    const Matrix3 proj = Matrix3::project(target.width(), target.height(), WL_OUTPUT_TRANSFORM_FLIPPED_180);
    const Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    const RectF uv = computeNormalizedUvRect(source, srcRect, destRect);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred->id());

    glUniformMatrix3fv(m_program->getUniformLocation("u_proj"), 1, GL_FALSE, mvp.data());
    glUniform4f(
        m_program->getUniformLocation("u_uv_rect"),
        static_cast<float>(uv.x),
        static_cast<float>(uv.y),
        static_cast<float>(uv.width),
        static_cast<float>(uv.height));
    glUniform1i(m_program->getUniformLocation("u_tex"), 0);
    glUniform4f(
        m_program->getUniformLocation("u_tint"),
        ctx.kawase.tint.r,
        ctx.kawase.tint.g,
        ctx.kawase.tint.b,
        ctx.kawase.tint.a);
    glUniform4f(
        m_program->getUniformLocation("u_params"),
        ctx.kawase.brightness,
        ctx.kawase.contrast,
        std::max(ctx.kawase.noise, 2.5f),
        ctx.cornerRadius);
    glUniform2f(
        m_program->getUniformLocation("u_box_size"),
        static_cast<float>(destRect.width),
        static_cast<float>(destRect.height));
    glUniform1f(m_program->getUniformLocation("u_power"), ctx.roundingPower);

    kawase.drawUnitQuad(m_program->getAttribLocation("pos"));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ==========================================
// OpticalGlassProvider
// ==========================================

OpticalGlassProvider::OpticalGlassProvider() {
    m_program = ShaderProgram::create(kMaterialVert, kOpticalGlassFrag);
}

bool OpticalGlassProvider::renderMaterial(
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const BlurContext& ctx) {
    auto blurred = kawase.blur(source, ctx.kawase);
    if (!blurred || !m_program) {
        return kawase.renderBlurSubRegion(source, srcRect, target, destRect, ctx.kawase, ctx.maskTexture);
    }

    target.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_program->use();
    const Matrix3 proj = Matrix3::project(target.width(), target.height(), WL_OUTPUT_TRANSFORM_FLIPPED_180);
    const Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    const RectF uv = computeNormalizedUvRect(source, srcRect, destRect);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred->id());

    glUniformMatrix3fv(m_program->getUniformLocation("u_proj"), 1, GL_FALSE, mvp.data());
    glUniform4f(
        m_program->getUniformLocation("u_uv_rect"),
        static_cast<float>(uv.x),
        static_cast<float>(uv.y),
        static_cast<float>(uv.width),
        static_cast<float>(uv.height));
    glUniform1i(m_program->getUniformLocation("u_tex"), 0);
    glUniform4f(
        m_program->getUniformLocation("u_tint"),
        ctx.kawase.tint.r,
        ctx.kawase.tint.g,
        ctx.kawase.tint.b,
        ctx.kawase.tint.a);
    glUniform4f(
        m_program->getUniformLocation("u_glass"),
        ctx.refraction,
        ctx.dispersion,
        ctx.specularIntensity,
        ctx.cornerRadius);
    glUniform2f(
        m_program->getUniformLocation("u_box_size"),
        static_cast<float>(destRect.width),
        static_cast<float>(destRect.height));
    glUniform1f(m_program->getUniformLocation("u_power"), ctx.roundingPower);

    kawase.drawUnitQuad(m_program->getAttribLocation("pos"));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ==========================================
// WaterRippleProvider
// ==========================================

WaterRippleProvider::WaterRippleProvider() {
    m_program = ShaderProgram::create(kMaterialVert, kWaterRippleFrag);
}

bool WaterRippleProvider::renderMaterial(
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const BlurContext& ctx) {
    auto blurred = kawase.blur(source, ctx.kawase);
    if (!blurred || !m_program) {
        return kawase.renderBlurSubRegion(source, srcRect, target, destRect, ctx.kawase, ctx.maskTexture);
    }

    target.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_program->use();
    const Matrix3 proj = Matrix3::project(target.width(), target.height(), WL_OUTPUT_TRANSFORM_FLIPPED_180);
    const Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    const RectF uv = computeNormalizedUvRect(source, srcRect, destRect);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred->id());

    glUniformMatrix3fv(m_program->getUniformLocation("u_proj"), 1, GL_FALSE, mvp.data());
    glUniform4f(
        m_program->getUniformLocation("u_uv_rect"),
        static_cast<float>(uv.x),
        static_cast<float>(uv.y),
        static_cast<float>(uv.width),
        static_cast<float>(uv.height));
    glUniform1i(m_program->getUniformLocation("u_tex"), 0);
    glUniform4f(
        m_program->getUniformLocation("u_tint"),
        ctx.kawase.tint.r,
        ctx.kawase.tint.g,
        ctx.kawase.tint.b,
        ctx.kawase.tint.a);
    glUniform4f(
        m_program->getUniformLocation("u_wave"),
        ctx.timeSeconds,
        static_cast<float>(ctx.cursorUv.x),
        static_cast<float>(ctx.cursorUv.y),
        ctx.strength);
    glUniform2f(
        m_program->getUniformLocation("u_box_size"),
        static_cast<float>(destRect.width),
        static_cast<float>(destRect.height));
    glUniform1f(m_program->getUniformLocation("u_radius"), ctx.cornerRadius);
    glUniform1f(m_program->getUniformLocation("u_power"), ctx.roundingPower);

    kawase.drawUnitQuad(m_program->getAttribLocation("pos"));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ==========================================
// FluidFieldProvider
// ==========================================

FluidFieldProvider::FluidFieldProvider() {
    m_program = ShaderProgram::create(kMaterialVert, kFluidFieldFrag);
}

bool FluidFieldProvider::renderMaterial(
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const BlurContext& ctx) {
    auto blurred = kawase.blur(source, ctx.kawase);
    if (!blurred || !m_program) {
        return kawase.renderBlurSubRegion(source, srcRect, target, destRect, ctx.kawase, ctx.maskTexture);
    }

    target.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_program->use();
    const Matrix3 proj = Matrix3::project(target.width(), target.height(), WL_OUTPUT_TRANSFORM_FLIPPED_180);
    const Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    const RectF uv = computeNormalizedUvRect(source, srcRect, destRect);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred->id());

    glUniformMatrix3fv(m_program->getUniformLocation("u_proj"), 1, GL_FALSE, mvp.data());
    glUniform4f(
        m_program->getUniformLocation("u_uv_rect"),
        static_cast<float>(uv.x),
        static_cast<float>(uv.y),
        static_cast<float>(uv.width),
        static_cast<float>(uv.height));
    glUniform1i(m_program->getUniformLocation("u_tex"), 0);
    glUniform4f(
        m_program->getUniformLocation("u_tint"),
        ctx.kawase.tint.r,
        ctx.kawase.tint.g,
        ctx.kawase.tint.b,
        ctx.kawase.tint.a);
    glUniform4f(
        m_program->getUniformLocation("u_fluid"),
        ctx.timeSeconds,
        static_cast<float>(ctx.cursorVelocityUv.x),
        static_cast<float>(ctx.cursorVelocityUv.y),
        ctx.strength);
    glUniform2f(
        m_program->getUniformLocation("u_box_size"),
        static_cast<float>(destRect.width),
        static_cast<float>(destRect.height));
    glUniform1f(m_program->getUniformLocation("u_radius"), ctx.cornerRadius);
    glUniform1f(m_program->getUniformLocation("u_power"), ctx.roundingPower);

    kawase.drawUnitQuad(m_program->getAttribLocation("pos"));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ==========================================
// AtmosphericBlurProvider
// ==========================================

AtmosphericBlurProvider::AtmosphericBlurProvider() {
    m_program = ShaderProgram::create(kMaterialVert, kAtmosphericAuroraFrag);
}

bool AtmosphericBlurProvider::renderMaterial(
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const BlurContext& ctx) {
    auto blurred = kawase.blur(source, ctx.kawase);
    if (!blurred || !m_program) {
        return kawase.renderBlurSubRegion(source, srcRect, target, destRect, ctx.kawase, ctx.maskTexture);
    }

    target.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_program->use();
    const Matrix3 proj = Matrix3::project(target.width(), target.height(), WL_OUTPUT_TRANSFORM_FLIPPED_180);
    const Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    const RectF uv = computeNormalizedUvRect(source, srcRect, destRect);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred->id());

    glUniformMatrix3fv(m_program->getUniformLocation("u_proj"), 1, GL_FALSE, mvp.data());
    glUniform4f(
        m_program->getUniformLocation("u_uv_rect"),
        static_cast<float>(uv.x),
        static_cast<float>(uv.y),
        static_cast<float>(uv.width),
        static_cast<float>(uv.height));
    glUniform1i(m_program->getUniformLocation("u_tex"), 0);
    glUniform4f(
        m_program->getUniformLocation("u_tint"),
        ctx.kawase.tint.r,
        ctx.kawase.tint.g,
        ctx.kawase.tint.b,
        ctx.kawase.tint.a);
    glUniform4f(
        m_program->getUniformLocation("u_atmos"),
        ctx.timeSeconds,
        ctx.strength,
        ctx.cornerRadius,
        ctx.roundingPower);
    glUniform2f(
        m_program->getUniformLocation("u_box_size"),
        static_cast<float>(destRect.width),
        static_cast<float>(destRect.height));

    kawase.drawUnitQuad(m_program->getAttribLocation("pos"));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ==========================================
// BlurProviderFactory
// ==========================================

std::shared_ptr<IBlurProvider> BlurProviderFactory::create(BlurMaterialType type) {
    switch (type) {
        case BlurMaterialType::Kawase:
            return std::make_shared<KawaseBlurProvider>();
        case BlurMaterialType::Acrylic:
            return std::make_shared<AcrylicBlurProvider>();
        case BlurMaterialType::OpticalGlass:
            return std::make_shared<OpticalGlassProvider>();
        case BlurMaterialType::WaterRipple:
            return std::make_shared<WaterRippleProvider>();
        case BlurMaterialType::FluidField:
            return std::make_shared<FluidFieldProvider>();
        case BlurMaterialType::AtmosphericAurora:
            return std::make_shared<AtmosphericBlurProvider>();
    }
    return std::make_shared<KawaseBlurProvider>();
}

} // namespace nisaba::compositor

