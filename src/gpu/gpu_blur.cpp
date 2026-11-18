#include "nisaba/gpu/gpu_blur.hpp"
#include "gl_headers.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace nisaba::gpu {

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

static const char* kDualKawaseVertGL = R"glsl(
#version 130
in vec2 pos;
in vec2 uv;
uniform vec2 u_uv_scale;
uniform vec2 u_uv_offset;
out vec2 v_uv;

void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv = uv * u_uv_scale + u_uv_offset;
}
)glsl";

static const char* kDualKawaseVertGLES = R"glsl(
#version 300 es
precision highp float;
precision highp int;
in vec2 pos;
in vec2 uv;
uniform vec2 u_uv_scale;
uniform vec2 u_uv_offset;
out vec2 v_uv;

void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv = uv * u_uv_scale + u_uv_offset;
}
)glsl";

static const char* kDualKawaseDownFragGL = R"glsl(
#version 130
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_halfpixel;
uniform float u_offset;
out vec4 fragColor;

void main() {
    vec2 d = u_halfpixel * u_offset;

    // 5-tap Dual Kawase Downsample pre-filter (4.0 center + 1.0 corners = 8.0 weight)
    vec4 sum = texture(u_tex, v_uv) * 4.0;
    sum += texture(u_tex, v_uv - d);
    sum += texture(u_tex, v_uv + d);
    sum += texture(u_tex, v_uv + vec2(d.x, -d.y));
    sum += texture(u_tex, v_uv + vec2(-d.x, d.y));

    fragColor = sum * 0.125;
}
)glsl";

static const char* kDualKawaseDownFragGLES = R"glsl(
#version 300 es
precision highp float;
precision highp int;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_halfpixel;
uniform float u_offset;
out vec4 fragColor;

void main() {
    vec2 d = u_halfpixel * u_offset;

    // 5-tap Dual Kawase Downsample pre-filter (4.0 center + 1.0 corners = 8.0 weight)
    vec4 sum = texture(u_tex, v_uv) * 4.0;
    sum += texture(u_tex, v_uv - d);
    sum += texture(u_tex, v_uv + d);
    sum += texture(u_tex, v_uv + vec2(d.x, -d.y));
    sum += texture(u_tex, v_uv + vec2(-d.x, d.y));

    fragColor = sum * 0.125;
}
)glsl";

static const char* kDualKawaseUpFragGL = R"glsl(
#version 130
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_halfpixel;
uniform float u_offset;
out vec4 fragColor;

void main() {
    vec2 d = u_halfpixel * u_offset;
    vec2 half_d = d * 0.5;

    // 8-tap Dual Kawase Upsample (Tent reconstruction filter)
    vec4 sum = texture(u_tex, v_uv - half_d);
    sum += texture(u_tex, v_uv + half_d);
    sum += texture(u_tex, v_uv + vec2(half_d.x, -half_d.y));
    sum += texture(u_tex, v_uv + vec2(-half_d.x, half_d.y));

    sum += texture(u_tex, v_uv + vec2(-d.x, 0.0)) * 2.0;
    sum += texture(u_tex, v_uv + vec2(d.x, 0.0)) * 2.0;
    sum += texture(u_tex, v_uv + vec2(0.0, -d.y)) * 2.0;
    sum += texture(u_tex, v_uv + vec2(0.0, d.y)) * 2.0;

    fragColor = sum * (1.0 / 12.0);
}
)glsl";

static const char* kDualKawaseUpFragGLES = R"glsl(
#version 300 es
precision highp float;
precision highp int;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_halfpixel;
uniform float u_offset;
out vec4 fragColor;

void main() {
    vec2 d = u_halfpixel * u_offset;
    vec2 half_d = d * 0.5;

    // 8-tap Dual Kawase Upsample (Tent reconstruction filter)
    vec4 sum = texture(u_tex, v_uv - half_d);
    sum += texture(u_tex, v_uv + half_d);
    sum += texture(u_tex, v_uv + vec2(half_d.x, -half_d.y));
    sum += texture(u_tex, v_uv + vec2(-half_d.x, half_d.y));

    sum += texture(u_tex, v_uv + vec2(-d.x, 0.0)) * 2.0;
    sum += texture(u_tex, v_uv + vec2(d.x, 0.0)) * 2.0;
    sum += texture(u_tex, v_uv + vec2(0.0, -d.y)) * 2.0;
    sum += texture(u_tex, v_uv + vec2(0.0, d.y)) * 2.0;

    fragColor = sum * (1.0 / 12.0);
}
)glsl";

static GLuint compileShader(GLenum type, const char* src) {
    detail::ensure_gl_functions_loaded();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "[-] DualKawase shader compile error: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint linkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[-] DualKawase program link error: " << log << std::endl;
        glDeleteProgram(prog);
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

struct ScopedBlurGLState {
    GLint drawFbo{0};
    GLint readFbo{0};
    GLint viewport[4]{};
    GLint prog{0};
    GLint vao{0};
    GLint activeTex{GL_TEXTURE0};
    GLint tex2D{0};
    GLint sampler{0};
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean blend{GL_FALSE};
    GLboolean depth{GL_FALSE};
    GLboolean scissor{GL_FALSE};
    GLboolean stencil{GL_FALSE};
    GLboolean cull{GL_FALSE};

    ScopedBlurGLState() noexcept {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2D);
#if defined(NISABA_TARGET_GLES)
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
#else
        if (glBindSampler) {
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
        }
#endif
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        cull = glIsEnabled(GL_CULL_FACE);

        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#if defined(NISABA_TARGET_GLES)
        glBindSampler(0, 0);
#else
        if (glBindSampler) {
            glBindSampler(0, 0);
        }
#endif
    }

    ~ScopedBlurGLState() noexcept {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glBindVertexArray(static_cast<GLuint>(vao));
        glUseProgram(static_cast<GLuint>(prog));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex2D));
#if defined(NISABA_TARGET_GLES)
        glBindSampler(0, static_cast<GLuint>(sampler));
#else
        if (glBindSampler) {
            glBindSampler(0, static_cast<GLuint>(sampler));
        }
#endif
        glActiveTexture(static_cast<GLenum>(activeTex));
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        if (blend) glEnable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (stencil) glEnable(GL_STENCIL_TEST);
        if (cull) glEnable(GL_CULL_FACE);
    }
};

} // namespace

struct DualKawaseBlur::ShaderProgramSet {
    GLuint downProg{0};
    GLuint upProg{0};

    // Downsample locations
    GLint downPos{-1};
    GLint downUv{-1};
    GLint downTex{-1};
    GLint downHalfpixel{-1};
    GLint downOffset{-1};
    GLint downUvScale{-1};
    GLint downUvOffset{-1};

    // Upsample locations
    GLint upPos{-1};
    GLint upUv{-1};
    GLint upTex{-1};
    GLint upHalfpixel{-1};
    GLint upOffset{-1};
    GLint upUvScale{-1};
    GLint upUvOffset{-1};

    ~ShaderProgramSet() {
        if (downProg) {
            glDeleteProgram(downProg);
            downProg = 0;
        }
        if (upProg) {
            glDeleteProgram(upProg);
            upProg = 0;
        }
    }
};

DualKawaseBlur::DualKawaseBlur() = default;

DualKawaseBlur::~DualKawaseBlur() {
    cleanup();
}

void DualKawaseBlur::cleanup() {
    for (auto& lvl : pyramid_) {
        if (lvl.fbo) {
            glDeleteFramebuffers(1, &lvl.fbo);
            lvl.fbo = 0;
        }
        if (lvl.texture) {
            glDeleteTextures(1, &lvl.texture);
            lvl.texture = 0;
        }
    }
    pyramid_.clear();

    if (capture_fbo_) {
        glDeleteFramebuffers(1, &capture_fbo_);
        capture_fbo_ = 0;
    }
    if (capture_tex_) {
        glDeleteTextures(1, &capture_tex_);
        capture_tex_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }

    shaders_.reset();
    base_width_ = 0;
    base_height_ = 0;
}

bool DualKawaseBlur::setup_vbo() {
    if (vao_ != 0 && vbo_ != 0) return true;

    if (!vao_) {
        glGenVertexArrays(1, &vao_);
    }
    if (!vbo_) {
        glGenBuffers(1, &vbo_);
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenTriangle), kFullscreenTriangle, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

bool DualKawaseBlur::setup_shaders() {
    if (shaders_) return true;

    const bool is_gles = detail::is_gles_or_webgl();

    const char* vertSrc = is_gles ? kDualKawaseVertGLES : kDualKawaseVertGL;
    const char* downFragSrc = is_gles ? kDualKawaseDownFragGLES : kDualKawaseDownFragGL;
    const char* upFragSrc = is_gles ? kDualKawaseUpFragGLES : kDualKawaseUpFragGL;

    auto set = std::make_unique<ShaderProgramSet>();
    set->downProg = linkProgram(vertSrc, downFragSrc);
    set->upProg = linkProgram(vertSrc, upFragSrc);

    if (!set->downProg || !set->upProg) {
        return false;
    }

    set->downPos = glGetAttribLocation(set->downProg, "pos");
    set->downUv = glGetAttribLocation(set->downProg, "uv");
    set->downTex = glGetUniformLocation(set->downProg, "u_tex");
    set->downHalfpixel = glGetUniformLocation(set->downProg, "u_halfpixel");
    set->downOffset = glGetUniformLocation(set->downProg, "u_offset");
    set->downUvScale = glGetUniformLocation(set->downProg, "u_uv_scale");
    set->downUvOffset = glGetUniformLocation(set->downProg, "u_uv_offset");

    set->upPos = glGetAttribLocation(set->upProg, "pos");
    set->upUv = glGetAttribLocation(set->upProg, "uv");
    set->upTex = glGetUniformLocation(set->upProg, "u_tex");
    set->upHalfpixel = glGetUniformLocation(set->upProg, "u_halfpixel");
    set->upOffset = glGetUniformLocation(set->upProg, "u_offset");
    set->upUvScale = glGetUniformLocation(set->upProg, "u_uv_scale");
    set->upUvOffset = glGetUniformLocation(set->upProg, "u_uv_offset");

    shaders_ = std::move(set);
    return true;
}

bool DualKawaseBlur::init(int baseWidth, int baseHeight, int maxPasses) {
    if (baseWidth <= 0 || baseHeight <= 0) return false;

    maxPasses = std::clamp(maxPasses, 1, 6);

    if (base_width_ == baseWidth && base_height_ == baseHeight && static_cast<int>(pyramid_.size()) == maxPasses) {
        return true;
    }

    if (!setup_shaders() || !setup_vbo()) {
        return false;
    }

    // Clean up existing textures
    for (auto& lvl : pyramid_) {
        if (lvl.fbo) glDeleteFramebuffers(1, &lvl.fbo);
        if (lvl.texture) glDeleteTextures(1, &lvl.texture);
    }
    pyramid_.clear();

    if (capture_fbo_) {
        glDeleteFramebuffers(1, &capture_fbo_);
        capture_fbo_ = 0;
    }
    if (capture_tex_) {
        glDeleteTextures(1, &capture_tex_);
        capture_tex_ = 0;
    }

    ScopedBlurGLState savedState;

    base_width_ = baseWidth;
    base_height_ = baseHeight;

    // Allocate RGBA8 capture texture + single-sample capture FBO (preserves alpha & supports MSAA resolve blit)
    glGenTextures(1, &capture_tex_);
    glBindTexture(GL_TEXTURE_2D, capture_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, baseWidth, baseHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &capture_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture_tex_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[-] DualKawase capture FBO incomplete" << std::endl;
        cleanup();
        return false;
    }

    // Allocate pyramid levels
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, curW, curH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &lvl.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, lvl.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lvl.texture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[-] DualKawase pyramid FBO incomplete: " << status << std::endl;
            cleanup();
            return false;
        }

        pyramid_.push_back(std::move(lvl));
    }

    return true;
}

uint32_t DualKawaseBlur::blur_framebuffer(
    uint32_t fboId,
    int width,
    int height,
    const DualKawaseConfig& config
) {
    if (width <= 0 || height <= 0) return 0;

    int passes = std::clamp(config.passes, 1, 6);
    if (!init(width, height, passes)) return 0;

    ScopedBlurGLState savedState;

    // 1. Capture active framebuffer into RGBA8 single-sample capture_fbo_ / capture_tex_
#if defined(NISABA_TARGET_GLES)
    constexpr bool has_blit = true;
#else
    const bool has_blit = (glBlitFramebuffer != nullptr);
#endif
    if (has_blit && capture_fbo_ != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboId);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, capture_fbo_);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboId);
        glBindTexture(GL_TEXTURE_2D, capture_tex_);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // 2. Execute blur pyramid (already guarded by ScopedBlurGLState above)
    return blur_texture_impl(capture_tex_, width, height, config);
}

uint32_t DualKawaseBlur::blur_texture(
    uint32_t srcTexId,
    int width,
    int height,
    const DualKawaseConfig& config
) {
    if (srcTexId == 0 || width <= 0 || height <= 0 || pyramid_.empty()) return 0;
    ScopedBlurGLState savedState;
    return blur_texture_impl(srcTexId, width, height, config);
}

uint32_t DualKawaseBlur::blur_texture_impl(
    uint32_t srcTexId,
    int width,
    int height,
    const DualKawaseConfig& config
) {
    if (srcTexId == 0 || width <= 0 || height <= 0 || pyramid_.empty()) return 0;

    int passes = std::min(static_cast<int>(pyramid_.size()), std::clamp(config.passes, 1, 6));

    glBindVertexArray(vao_);

    // ==========================================
    // 1. Downsampling passes
    // ==========================================
    glUseProgram(shaders_->downProg);
    glUniform1i(shaders_->downTex, 0);

    for (int p = 0; p < passes; ++p) {
        const auto& dst = pyramid_[p];
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.width, dst.height);

        GLuint srcTex = (p == 0) ? srcTexId : pyramid_[p - 1].texture;
        int srcW = (p == 0) ? width : pyramid_[p - 1].width;
        int srcH = (p == 0) ? height : pyramid_[p - 1].height;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);

        glUniform2f(shaders_->downHalfpixel, 0.5f / static_cast<float>(srcW), 0.5f / static_cast<float>(srcH));
        glUniform1f(shaders_->downOffset, config.radius);
        glUniform2f(shaders_->downUvScale, 1.0f, 1.0f);
        glUniform2f(shaders_->downUvOffset, 0.0f, 0.0f);

        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // ==========================================
    // 2. Upsampling passes
    // ==========================================
    glUseProgram(shaders_->upProg);
    glUniform1i(shaders_->upTex, 0);

    for (int p = passes - 1; p > 0; --p) {
        const auto& src = pyramid_[p];
        const auto& dst = pyramid_[p - 1];

        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.width, dst.height);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.texture);

        glUniform2f(shaders_->upHalfpixel, 0.5f / static_cast<float>(src.width), 0.5f / static_cast<float>(src.height));
        glUniform1f(shaders_->upOffset, config.radius);
        glUniform2f(shaders_->upUvScale, 1.0f, 1.0f);
        glUniform2f(shaders_->upUvOffset, 0.0f, 0.0f);

        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Return blurred level 0 texture (ScopedBlurGLState restores VAO/program/FBO/viewport/texture)
    return pyramid_[0].texture;
}

uint32_t DualKawaseBlur::output_texture() const noexcept {
    return pyramid_.empty() ? 0 : pyramid_[0].texture;
}

} // namespace nisaba::gpu
