#include "nisaba/compositor/dual_kawase.hpp"
#include "nisaba/compositor/log.hpp"
#include "nisaba/compositor/matrix3.hpp"
#include "nisaba/compositor/shader.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nisaba::compositor {

namespace {

struct Vertex {
    float x, y;
    float u, v;
};

// Fullscreen triangle covers [-1, 1] clip space in a single primitive with zero diagonal seam overhead
static constexpr Vertex kFullscreenTriangle[3] = {
    {-1.0f, -1.0f, 0.0f, 0.0f},
    { 3.0f, -1.0f, 2.0f, 0.0f},
    {-1.0f,  3.0f, 0.0f, 2.0f}
};

static constexpr Vertex kUnitQuad[6] = {
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 1.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 1.0f},
    {1.0f, 0.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.0f, 1.0f, 0.0f, 1.0f}
};

// Shaders

constexpr std::string_view kDualKawaseVert = R"glsl(
attribute vec2 pos;
attribute vec2 uv;
uniform vec2 u_uv_scale;
uniform vec2 u_uv_offset;
varying vec2 v_uv;

void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv = uv * u_uv_scale + u_uv_offset;
}
)glsl";

constexpr std::string_view kDualKawaseDownFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_halfpixel;
uniform float u_offset;

void main() {
    vec2 d = u_halfpixel * u_offset;

    // 5-tap Dual Kawase Downsample pre-filter (4.0 center + 1.0 corners = 8.0 weight)
    vec4 sum = texture2D(u_tex, v_uv) * 4.0;
    sum += texture2D(u_tex, v_uv - d);
    sum += texture2D(u_tex, v_uv + d);
    sum += texture2D(u_tex, v_uv + vec2(d.x, -d.y));
    sum += texture2D(u_tex, v_uv + vec2(-d.x, d.y));

    gl_FragColor = sum * 0.125;
}
)glsl";

constexpr std::string_view kDualKawaseUpFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_halfpixel;
uniform float u_offset;

void main() {
    vec2 d = u_halfpixel * u_offset;
    vec2 half_d = d * 0.5;

    // 8-tap Dual Kawase Upsample (Tent reconstruction filter)
    // 4 diagonal taps at half offset (weight 1 each)
    vec4 sum = texture2D(u_tex, v_uv - half_d);
    sum += texture2D(u_tex, v_uv + half_d);
    sum += texture2D(u_tex, v_uv + vec2(half_d.x, -half_d.y));
    sum += texture2D(u_tex, v_uv + vec2(-half_d.x, half_d.y));

    // 4 axial taps at full offset (weight 2 each)
    sum += texture2D(u_tex, v_uv + vec2(-d.x, 0.0)) * 2.0;
    sum += texture2D(u_tex, v_uv + vec2(d.x, 0.0)) * 2.0;
    sum += texture2D(u_tex, v_uv + vec2(0.0, -d.y)) * 2.0;
    sum += texture2D(u_tex, v_uv + vec2(0.0, d.y)) * 2.0;

    gl_FragColor = sum * (1.0 / 12.0);
}
)glsl";

constexpr std::string_view kDualKawaseCompositeVert = R"glsl(
attribute vec2 pos;
attribute vec2 uv;
varying vec2 v_uv;
varying vec2 v_mask_uv;
uniform mat3 u_proj;
uniform vec2 u_uv_scale;
uniform vec2 u_uv_offset;
uniform vec2 u_mask_uv_scale;
uniform vec2 u_mask_uv_offset;

void main() {
    vec3 pos3 = vec3(pos, 1.0);
    gl_Position = vec4(pos3 * u_proj, 1.0);
    v_uv = uv * u_uv_scale + u_uv_offset;
    v_mask_uv = uv * u_mask_uv_scale + u_mask_uv_offset;
}
)glsl";

constexpr std::string_view kDualKawaseCompositeFrag = R"glsl(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_uv;
varying vec2 v_mask_uv;
uniform sampler2D u_tex;
uniform sampler2D u_mask;
uniform int u_has_mask;
uniform vec2 u_halfpixel;
uniform vec2 u_mask_halfpixel;
uniform float u_offset;
uniform vec4 u_tint;
uniform float u_brightness;
uniform float u_contrast;
uniform float u_noise;

float hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

void main() {
    vec2 d = u_halfpixel * u_offset;
    vec2 half_d = d * 0.5;

    // 8-tap upsample tent filter
    vec4 sum = texture2D(u_tex, v_uv - half_d);
    sum += texture2D(u_tex, v_uv + half_d);
    sum += texture2D(u_tex, v_uv + vec2(half_d.x, -half_d.y));
    sum += texture2D(u_tex, v_uv + vec2(-half_d.x, half_d.y));

    sum += texture2D(u_tex, v_uv + vec2(-d.x, 0.0)) * 2.0;
    sum += texture2D(u_tex, v_uv + vec2(d.x, 0.0)) * 2.0;
    sum += texture2D(u_tex, v_uv + vec2(0.0, -d.y)) * 2.0;
    sum += texture2D(u_tex, v_uv + vec2(0.0, d.y)) * 2.0;

    vec4 color = sum * (1.0 / 12.0);
    color.a = 1.0;

    // Contrast adjustment
    color.rgb = (color.rgb - 0.5) * u_contrast + 0.5;

    // Brightness adjustment
    color.rgb *= u_brightness;

    // Tint overlay
    if (u_tint.a > 0.0) {
        color.rgb = mix(color.rgb, u_tint.rgb, u_tint.a);
    }

    // High quality dithering to eliminate 8-bit banding on dark gradients
    if (u_noise > 0.0) {
        float dither = (hash(gl_FragCoord.xy) - 0.5) * (u_noise / 255.0);
        color.rgb += dither;
    }

    // Mask modulation (e.g. rounded corners, window shape, dock pill shape)
    if (u_has_mask == 1) {
        float maskAlpha = texture2D(u_mask, v_mask_uv).a;
        float edge = smoothstep(0.0, 0.02, maskAlpha);
        color *= edge;
    }

    gl_FragColor = color;
}
)glsl";

} // namespace

struct DualKawaseBlur::ShaderProgramSet {
    std::unique_ptr<ShaderProgram> downsampleProg;
    std::unique_ptr<ShaderProgram> upsampleProg;
    std::unique_ptr<ShaderProgram> compositeProg;

    // Downsample uniforms/attribs
    GLint downPos{-1};
    GLint downUv{-1};
    GLint downTex{-1};
    GLint downHalfpixel{-1};
    GLint downOffset{-1};
    GLint downUvScale{-1};
    GLint downUvOffset{-1};

    // Upsample uniforms/attribs
    GLint upPos{-1};
    GLint upUv{-1};
    GLint upTex{-1};
    GLint upHalfpixel{-1};
    GLint upOffset{-1};
    GLint upUvScale{-1};
    GLint upUvOffset{-1};

    // Composite uniforms/attribs
    GLint compPos{-1};
    GLint compUv{-1};
    GLint compProj{-1};
    GLint compTex{-1};
    GLint compHalfpixel{-1};
    GLint compMaskHalfpixel{-1};
    GLint compOffset{-1};
    GLint compTint{-1};
    GLint compBrightness{-1};
    GLint compContrast{-1};
    GLint compNoise{-1};
    GLint compMask{-1};
    GLint compHasMask{-1};
    GLint compUvScale{-1};
    GLint compUvOffset{-1};
    GLint compMaskUvScale{-1};
    GLint compMaskUvOffset{-1};
};

std::shared_ptr<DualKawaseBlur> DualKawaseBlur::create(std::shared_ptr<EglContext> egl) {
    if (egl) {
        egl->makeCurrent();
    }
    auto blur = std::make_shared<DualKawaseBlur>(std::move(egl));
    if (!blur->setupShaders() || !blur->setupVbo()) {
        logError("Failed to initialize DualKawaseBlur engine");
        return nullptr;
    }
    return blur;
}

DualKawaseBlur::DualKawaseBlur(std::shared_ptr<EglContext> egl)
    : m_egl(std::move(egl)) {}

DualKawaseBlur::~DualKawaseBlur() {
    cleanupPyramid();
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
}

bool DualKawaseBlur::setupVbo() {
    if (m_vbo != 0) return true;

    glGenBuffers(1, &m_vbo);
    if (!m_vbo) {
        logError("Failed to generate DualKawase VBO");
        return false;
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Allocate space for both fullscreen triangle (3 vertices) and unit quad (6 vertices)
    size_t triSize = sizeof(kFullscreenTriangle);
    size_t quadSize = sizeof(kUnitQuad);
    glBufferData(GL_ARRAY_BUFFER, triSize + quadSize, nullptr, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, triSize, kFullscreenTriangle);
    glBufferSubData(GL_ARRAY_BUFFER, triSize, quadSize, kUnitQuad);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void DualKawaseBlur::drawUnitQuad(GLint posAttrib) const noexcept {
    if (posAttrib < 0 || m_vbo == 0) return;

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    const size_t quadOffset = sizeof(kFullscreenTriangle);
    glEnableVertexAttribArray(static_cast<GLuint>(posAttrib));
    glVertexAttribPointer(
        static_cast<GLuint>(posAttrib),
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(Vertex),
        reinterpret_cast<const void*>(quadOffset + offsetof(Vertex, x)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(static_cast<GLuint>(posAttrib));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool DualKawaseBlur::setupShaders() {
    if (m_shaders) return true;

    auto set = std::make_unique<ShaderProgramSet>();

    // 1. Downsample shader
    set->downsampleProg = ShaderProgram::create(kDualKawaseVert, kDualKawaseDownFrag);
    if (!set->downsampleProg) {
        logError("Failed to compile DualKawase downsample shader");
        return false;
    }
    set->downPos = set->downsampleProg->getAttribLocation("pos");
    set->downUv = set->downsampleProg->getAttribLocation("uv");
    set->downTex = set->downsampleProg->getUniformLocation("u_tex");
    set->downHalfpixel = set->downsampleProg->getUniformLocation("u_halfpixel");
    set->downOffset = set->downsampleProg->getUniformLocation("u_offset");
    set->downUvScale = set->downsampleProg->getUniformLocation("u_uv_scale");
    set->downUvOffset = set->downsampleProg->getUniformLocation("u_uv_offset");

    // 2. Upsample shader
    set->upsampleProg = ShaderProgram::create(kDualKawaseVert, kDualKawaseUpFrag);
    if (!set->upsampleProg) {
        logError("Failed to compile DualKawase upsample shader");
        return false;
    }
    set->upPos = set->upsampleProg->getAttribLocation("pos");
    set->upUv = set->upsampleProg->getAttribLocation("uv");
    set->upTex = set->upsampleProg->getUniformLocation("u_tex");
    set->upHalfpixel = set->upsampleProg->getUniformLocation("u_halfpixel");
    set->upOffset = set->upsampleProg->getUniformLocation("u_offset");
    set->upUvScale = set->upsampleProg->getUniformLocation("u_uv_scale");
    set->upUvOffset = set->upsampleProg->getUniformLocation("u_uv_offset");

    // 3. Composite shader
    set->compositeProg = ShaderProgram::create(kDualKawaseCompositeVert, kDualKawaseCompositeFrag);
    if (!set->compositeProg) {
        logError("Failed to compile DualKawase composite shader");
        return false;
    }
    set->compPos = set->compositeProg->getAttribLocation("pos");
    set->compUv = set->compositeProg->getAttribLocation("uv");
    set->compProj = set->compositeProg->getUniformLocation("u_proj");
    set->compTex = set->compositeProg->getUniformLocation("u_tex");
    set->compHalfpixel = set->compositeProg->getUniformLocation("u_halfpixel");
    set->compMaskHalfpixel = set->compositeProg->getUniformLocation("u_mask_halfpixel");
    set->compOffset = set->compositeProg->getUniformLocation("u_offset");
    set->compTint = set->compositeProg->getUniformLocation("u_tint");
    set->compBrightness = set->compositeProg->getUniformLocation("u_brightness");
    set->compContrast = set->compositeProg->getUniformLocation("u_contrast");
    set->compNoise = set->compositeProg->getUniformLocation("u_noise");
    set->compMask = set->compositeProg->getUniformLocation("u_mask");
    set->compHasMask = set->compositeProg->getUniformLocation("u_has_mask");
    set->compUvScale = set->compositeProg->getUniformLocation("u_uv_scale");
    set->compUvOffset = set->compositeProg->getUniformLocation("u_uv_offset");
    set->compMaskUvScale = set->compositeProg->getUniformLocation("u_mask_uv_scale");
    set->compMaskUvOffset = set->compositeProg->getUniformLocation("u_mask_uv_offset");

    m_shaders = std::move(set);
    logInfo("DualKawaseBlur GPU shaders initialized successfully! 🌟");
    return true;
}

void DualKawaseBlur::cleanupPyramid() {
    for (auto& lvl : m_pyramid) {
        if (lvl.fbo) {
            glDeleteFramebuffers(1, &lvl.fbo);
            lvl.fbo = 0;
        }
        if (lvl.textureObj) {
            lvl.textureObj.reset();
            lvl.texture = 0;
        } else if (lvl.texture) {
            glDeleteTextures(1, &lvl.texture);
            lvl.texture = 0;
        }
    }
    m_pyramid.clear();
    m_baseWidth = 0;
    m_baseHeight = 0;
}

bool DualKawaseBlur::init(int baseWidth, int baseHeight, int maxPasses) {
    if (baseWidth <= 0 || baseHeight <= 0) {
        logError("Invalid dimensions for DualKawaseBlur::init: {}x{}", baseWidth, baseHeight);
        return false;
    }

    maxPasses = std::clamp(maxPasses, 1, 6);

    // Reuse existing pyramid if matching dimensions and pass count
    if (m_baseWidth == baseWidth && m_baseHeight == baseHeight && static_cast<int>(m_pyramid.size()) == maxPasses) {
        return true;
    }

    cleanupPyramid();
    m_baseWidth = baseWidth;
    m_baseHeight = baseHeight;

    int curW = baseWidth;
    int curH = baseHeight;

    for (int i = 0; i < maxPasses; ++i) {
        curW = std::max(1, curW / 2);
        curH = std::max(1, curH / 2);

        PyramidLevel lvl;
        lvl.width = curW;
        lvl.height = curH;

        glGenTextures(1, &lvl.texture);
        glBindTexture(GL_TEXTURE_2D, lvl.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, curW, curH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &lvl.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, lvl.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lvl.texture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            logError("DualKawase FBO pyramid level {} incomplete: 0x{:x}", i, status);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            cleanupPyramid();
            return false;
        }

        lvl.textureObj = std::make_shared<Texture>(lvl.texture, GL_TEXTURE_2D, curW, curH, 0, true);
        m_pyramid.push_back(std::move(lvl));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    logDebug("DualKawase pyramid initialized: base {}x{}, {} levels (finest: {}x{}, coarsest: {}x{})",
                   baseWidth, baseHeight, m_pyramid.size(),
                   m_pyramid.front().width, m_pyramid.front().height,
                   m_pyramid.back().width, m_pyramid.back().height);
    return true;
}

std::shared_ptr<Texture> DualKawaseBlur::blur(
    const Texture& source,
    const DualKawaseConfig& config) {
    if (!source.id() || source.width() == 0 || source.height() == 0) {
        return nullptr;
    }

    int passes = std::clamp(config.passes, 1, 6);
    if (!init(source.width(), source.height(), passes)) {
        return nullptr;
    }

    // Save previous GL states
    GLint prevFbo = 0;
    GLint prevViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // ==========================================
    // 1. Downsampling passes
    // ==========================================
    m_shaders->downsampleProg->use();
    glUniform1i(m_shaders->downTex, 0);

    glEnableVertexAttribArray(m_shaders->downPos);
    glEnableVertexAttribArray(m_shaders->downUv);
    glVertexAttribPointer(m_shaders->downPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(m_shaders->downUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));

    for (int p = 0; p < passes; ++p) {
        const auto& dst = m_pyramid[p];
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.width, dst.height);

        GLuint srcTex = (p == 0) ? source.id() : m_pyramid[p - 1].texture;
        int srcW = (p == 0) ? source.width() : m_pyramid[p - 1].width;
        int srcH = (p == 0) ? source.height() : m_pyramid[p - 1].height;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);

        glUniform2f(m_shaders->downHalfpixel, 0.5f / static_cast<float>(srcW), 0.5f / static_cast<float>(srcH));
        glUniform1f(m_shaders->downOffset, config.radius);
        glUniform2f(m_shaders->downUvScale, 1.0f, 1.0f);
        glUniform2f(m_shaders->downUvOffset, 0.0f, 0.0f);

        // Draw fullscreen triangle (3 vertices, 0 seam overhead)
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glDisableVertexAttribArray(m_shaders->downPos);
    glDisableVertexAttribArray(m_shaders->downUv);

    // ==========================================
    // 2. Upsampling passes
    // ==========================================
    m_shaders->upsampleProg->use();
    glUniform1i(m_shaders->upTex, 0);

    glEnableVertexAttribArray(m_shaders->upPos);
    glEnableVertexAttribArray(m_shaders->upUv);
    glVertexAttribPointer(m_shaders->upPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(m_shaders->upUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));

    for (int p = passes - 1; p > 0; --p) {
        const auto& src = m_pyramid[p];
        const auto& dst = m_pyramid[p - 1];

        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.width, dst.height);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.texture);

        glUniform2f(m_shaders->upHalfpixel, 0.5f / static_cast<float>(src.width), 0.5f / static_cast<float>(src.height));
        glUniform1f(m_shaders->upOffset, config.radius);
        glUniform2f(m_shaders->upUvScale, 1.0f, 1.0f);
        glUniform2f(m_shaders->upUvOffset, 0.0f, 0.0f);

        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glDisableVertexAttribArray(m_shaders->upPos);
    glDisableVertexAttribArray(m_shaders->upUv);

    // Restore previous GL states
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Return the blurred texture at pyramid level 0 (high-resolution half-size with bilinear filtering)
    return m_pyramid[0].textureObj;
}

bool DualKawaseBlur::renderBlurSubRegion(
    const Texture& source,
    const RectI& srcRect,
    RenderTarget& target,
    const RectI& destRect,
    const DualKawaseConfig& config,
    const Texture* maskTexture) {
    if (!source.id() || source.width() == 0 || source.height() == 0 || destRect.isEmpty()) {
        return false;
    }

    int blurW = (srcRect.width > 0) ? srcRect.width : source.width();
    int blurH = (srcRect.height > 0) ? srcRect.height : source.height();

    int passes = std::clamp(config.passes, 1, 6);
    if (!init(blurW, blurH, passes)) {
        return false;
    }

    // Save previous GL states
    GLint prevFbo = 0;
    GLint prevViewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // ==========================================
    // 1. Downsampling passes
    // ==========================================
    m_shaders->downsampleProg->use();
    glUniform1i(m_shaders->downTex, 0);

    glEnableVertexAttribArray(m_shaders->downPos);
    glEnableVertexAttribArray(m_shaders->downUv);
    glVertexAttribPointer(m_shaders->downPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(m_shaders->downUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));

    // Calculate source sub-region UV scale and offset
    float uvScaleX = 1.0f;
    float uvScaleY = 1.0f;
    float uvOffsetX = 0.0f;
    float uvOffsetY = 0.0f;

    if (srcRect.width > 0 && srcRect.height > 0) {
        uvScaleX = static_cast<float>(srcRect.width) / static_cast<float>(source.width());
        uvScaleY = static_cast<float>(srcRect.height) / static_cast<float>(source.height());
        uvOffsetX = static_cast<float>(srcRect.x) / static_cast<float>(source.width());
        uvOffsetY = static_cast<float>(srcRect.y) / static_cast<float>(source.height());
    }

    for (int p = 0; p < passes; ++p) {
        const auto& dst = m_pyramid[p];
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.width, dst.height);

        GLuint srcTex = (p == 0) ? source.id() : m_pyramid[p - 1].texture;
        int srcW = (p == 0) ? source.width() : m_pyramid[p - 1].width;
        int srcH = (p == 0) ? source.height() : m_pyramid[p - 1].height;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);

        glUniform2f(m_shaders->downHalfpixel, 0.5f / static_cast<float>(srcW), 0.5f / static_cast<float>(srcH));
        glUniform1f(m_shaders->downOffset, config.radius);

        if (p == 0) {
            glUniform2f(m_shaders->downUvScale, uvScaleX, uvScaleY);
            glUniform2f(m_shaders->downUvOffset, uvOffsetX, uvOffsetY);
        } else {
            glUniform2f(m_shaders->downUvScale, 1.0f, 1.0f);
            glUniform2f(m_shaders->downUvOffset, 0.0f, 0.0f);
        }

        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glDisableVertexAttribArray(m_shaders->downPos);
    glDisableVertexAttribArray(m_shaders->downUv);

    // ==========================================
    // 2. Intermediate Upsampling passes
    // ==========================================
    m_shaders->upsampleProg->use();
    glUniform1i(m_shaders->upTex, 0);

    glEnableVertexAttribArray(m_shaders->upPos);
    glEnableVertexAttribArray(m_shaders->upUv);
    glVertexAttribPointer(m_shaders->upPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(m_shaders->upUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));

    for (int p = passes - 1; p > 0; --p) {
        const auto& src = m_pyramid[p];
        const auto& dst = m_pyramid[p - 1];

        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.width, dst.height);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.texture);

        glUniform2f(m_shaders->upHalfpixel, 0.5f / static_cast<float>(src.width), 0.5f / static_cast<float>(src.height));
        glUniform1f(m_shaders->upOffset, config.radius);
        glUniform2f(m_shaders->upUvScale, 1.0f, 1.0f);
        glUniform2f(m_shaders->upUvOffset, 0.0f, 0.0f);

        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glDisableVertexAttribArray(m_shaders->upPos);
    glDisableVertexAttribArray(m_shaders->upUv);

    // ==========================================
    // 3. Final Composite pass onto destination target
    // ==========================================
    target.bind();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_shaders->compositeProg->use();
    glUniform1i(m_shaders->compTex, 0);

    if (maskTexture && maskTexture->id() != 0 && maskTexture->target() == GL_TEXTURE_2D &&
        maskTexture->width() > 0 && maskTexture->height() > 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, maskTexture->id());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(m_shaders->compMask, 1);
        glUniform1i(m_shaders->compHasMask, 1);
        glUniform2f(m_shaders->compMaskHalfpixel, 0.5f / static_cast<float>(maskTexture->width()), 0.5f / static_cast<float>(maskTexture->height()));
    } else {
        glUniform1i(m_shaders->compHasMask, 0);
        glUniform2f(m_shaders->compMaskHalfpixel, 0.0f, 0.0f);
    }

    Matrix3 proj = Matrix3::project(target.width(), target.height(), WL_OUTPUT_TRANSFORM_NORMAL);
    Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    glUniformMatrix3fv(m_shaders->compProj, 1, GL_FALSE, mvp.data());
    glUniform2f(m_shaders->compUvScale, 1.0f, 1.0f);
    glUniform2f(m_shaders->compUvOffset, 0.0f, 0.0f);
    glUniform2f(m_shaders->compMaskUvScale, 1.0f, 1.0f);
    glUniform2f(m_shaders->compMaskUvOffset, 0.0f, 0.0f);

    const auto& finest = m_pyramid[0];
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, finest.texture);

    glUniform2f(m_shaders->compHalfpixel, 0.5f / static_cast<float>(finest.width), 0.5f / static_cast<float>(finest.height));
    glUniform1f(m_shaders->compOffset, config.radius);
    glUniform4f(m_shaders->compTint, config.tint.r, config.tint.g, config.tint.b, config.tint.a);
    glUniform1f(m_shaders->compBrightness, config.brightness);
    glUniform1f(m_shaders->compContrast, config.contrast);
    glUniform1f(m_shaders->compNoise, config.noise);

    // Use unit quad from VBO offset
    size_t quadOffset = sizeof(kFullscreenTriangle);
    glEnableVertexAttribArray(m_shaders->compPos);
    glEnableVertexAttribArray(m_shaders->compUv);
    glVertexAttribPointer(m_shaders->compPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(quadOffset + offsetof(Vertex, x)));
    glVertexAttribPointer(m_shaders->compUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(quadOffset + offsetof(Vertex, u)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(m_shaders->compPos);
    glDisableVertexAttribArray(m_shaders->compUv);

    if (maskTexture && maskTexture->id() != 0 && maskTexture->target() == GL_TEXTURE_2D) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (prevBlend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);

    return true;
}

bool DualKawaseBlur::renderBlurredBackdrop(
    const Texture& blurredTex,
    const RectI& destRect,
    const RectF& srcBox,
    RenderTarget& target,
    const DualKawaseConfig& config,
    const Texture* maskTexture,
    wl_output_transform transform,
    const RectF& maskSrcBox) {
    if (!blurredTex.id() || destRect.isEmpty() || target.width() <= 0 || target.height() <= 0) {
        return false;
    }

    target.bind();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    m_shaders->compositeProg->use();
    glUniform1i(m_shaders->compTex, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurredTex.id());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (maskTexture && maskTexture->id() != 0 && maskTexture->target() == GL_TEXTURE_2D &&
        maskTexture->width() > 0 && maskTexture->height() > 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, maskTexture->id());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(m_shaders->compMask, 1);
        glUniform1i(m_shaders->compHasMask, 1);
        glUniform2f(m_shaders->compMaskHalfpixel, 0.5f / static_cast<float>(maskTexture->width()), 0.5f / static_cast<float>(maskTexture->height()));
    } else {
        glUniform1i(m_shaders->compHasMask, 0);
        glUniform2f(m_shaders->compMaskHalfpixel, 0.0f, 0.0f);
    }

    Matrix3 proj = Matrix3::project(target.width(), target.height(), transform);
    Matrix3 mvp = Matrix3::projectBox(destRect, WL_OUTPUT_TRANSFORM_NORMAL, proj);
    glUniformMatrix3fv(m_shaders->compProj, 1, GL_FALSE, mvp.data());

    glUniform2f(m_shaders->compUvScale, srcBox.width, srcBox.height);
    glUniform2f(m_shaders->compUvOffset, srcBox.x, srcBox.y);
    glUniform2f(m_shaders->compMaskUvScale, maskSrcBox.width, maskSrcBox.height);
    glUniform2f(m_shaders->compMaskUvOffset, maskSrcBox.x, maskSrcBox.y);

    glUniform2f(m_shaders->compHalfpixel, 0.5f / static_cast<float>(blurredTex.width()), 0.5f / static_cast<float>(blurredTex.height()));
    glUniform1f(m_shaders->compOffset, config.radius);
    glUniform4f(m_shaders->compTint, config.tint.r, config.tint.g, config.tint.b, config.tint.a);
    glUniform1f(m_shaders->compBrightness, config.brightness);
    glUniform1f(m_shaders->compContrast, config.contrast);
    glUniform1f(m_shaders->compNoise, config.noise);

    size_t quadOffset = sizeof(kFullscreenTriangle);
    glEnableVertexAttribArray(m_shaders->compPos);
    glEnableVertexAttribArray(m_shaders->compUv);
    glVertexAttribPointer(m_shaders->compPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(quadOffset + offsetof(Vertex, x)));
    glVertexAttribPointer(m_shaders->compUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(quadOffset + offsetof(Vertex, u)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(m_shaders->compPos);
    glDisableVertexAttribArray(m_shaders->compUv);

    if (maskTexture && maskTexture->id() != 0 && maskTexture->target() == GL_TEXTURE_2D) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

} // namespace nisaba::compositor
