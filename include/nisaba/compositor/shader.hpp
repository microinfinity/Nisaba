#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/color_management.hpp"
#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/matrix3.hpp"

#include <GLES2/gl2.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nisaba::compositor {

/**
 * @brief Bitmask feature flags for dynamic shader permutation generation.
 */
enum ShaderFeatureFlags : uint32_t {
    kShaderFeatNone            = 0,
    kShaderFeatRoundCorners    = 1u << 0, ///< Analytical Lp-norm squircle corner clipping
    kShaderFeatBackdropBlur    = 1u << 1, ///< Single-pass blurred backdrop blending
    kShaderFeatAlphaMatte      = 1u << 2, ///< External alpha/matte mask texture
    kShaderFeatDiscard         = 1u << 3, ///< Opaque or alpha-threshold fragment discard
    kShaderFeatMotionBlur      = 1u << 4, ///< Directional velocity multi-tap motion blur
    kShaderFeatColorManagement = 1u << 5, ///< EOTF -> 3x3 Gamut Matrix -> Luminance -> OETF
    kShaderFeatTonemap         = 1u << 6, ///< HDR tone-mapping operator (Reinhard / BT.2390 / Filmic)
    kShaderFeatIccLut3D        = 1u << 7, ///< 3D ICC LUT trilinear color lookup
    kShaderFeatMirrorMrt       = 1u << 8  ///< Dual output attachment for mirrored displays
};

/**
 * @brief Sovereign OpenGL ES Shader object.
 */
class Shader {
public:
    static std::unique_ptr<Shader> compile(GLenum type, std::string_view source);

    Shader(GLenum type, GLuint id);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] GLenum type() const noexcept { return m_type; }

private:
    GLenum m_type{0};
    GLuint m_id{0};
};

/**
 * @brief Base class for linked OpenGL ES shader programs.
 */
class ShaderProgram {
public:
    static std::unique_ptr<ShaderProgram> create(std::string_view vertSource, std::string_view fragSource);

    ShaderProgram(GLuint programId);
    virtual ~ShaderProgram();

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    void use() const noexcept;

    [[nodiscard]] GLuint id() const noexcept { return m_program; }
    [[nodiscard]] GLuint release() noexcept {
        GLuint prog = m_program;
        m_program = 0;
        return prog;
    }
    [[nodiscard]] GLint getUniformLocation(const char* name) const noexcept;
    [[nodiscard]] GLint getAttribLocation(const char* name) const noexcept;

protected:
    GLuint m_program{0};
};

/**
 * @brief Solid color box shader program (supports optional squircle rounding).
 */
class SolidQuadProgram : public ShaderProgram {
public:
    static std::unique_ptr<SolidQuadProgram> create();
    SolidQuadProgram(GLuint programId);

    void setProjection(const Matrix3& proj) const noexcept;
    void setColor(const Color& color) const noexcept;
    void setRounding(const RectI& box, float radius = 0.0f, float power = 2.0f) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }

private:
    GLint m_projLoc{-1};
    GLint m_colorLoc{-1};
    GLint m_boxLoc{-1};
    GLint m_radiusLoc{-1};
    GLint m_powerLoc{-1};
    GLint m_posAttrib{-1};
};

/**
 * @brief Texture quad shader program supporting 2D textures or external OES.
 */
class TextureQuadProgram : public ShaderProgram {
public:
    enum class Target : uint8_t {
        Rgba = 0,
        Rgbx = 1,
        ExternalOes = 2,
        ExternalOesRgbx = 3
    };

    static std::unique_ptr<TextureQuadProgram> create(Target target);
    TextureQuadProgram(GLuint programId, Target target);

    void setProjection(const Matrix3& proj) const noexcept;
    void setTextureProjection(const Matrix3& texProj) const noexcept;
    void setAlpha(float alpha) const noexcept;
    void setTextureUnit(int unit) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }
    [[nodiscard]] Target target() const noexcept { return m_target; }

private:
    Target m_target;
    GLint m_projLoc{-1};
    GLint m_texProjLoc{-1};
    GLint m_texLoc{-1};
    GLint m_alphaLoc{-1};
    GLint m_posAttrib{-1};
};

/**
 * @brief Multi-feature surface shader permutation compiled for a specific feature bitmask.
 */
class SurfaceVariantProgram : public ShaderProgram {
public:
    static std::unique_ptr<SurfaceVariantProgram> create(
        TextureQuadProgram::Target target,
        uint32_t features);

    SurfaceVariantProgram(
        GLuint programId,
        TextureQuadProgram::Target target,
        uint32_t features);

    void setProjection(const Matrix3& proj) const noexcept;
    void setTextureProjection(const Matrix3& texProj) const noexcept;
    void setTextureUnit(int unit) const noexcept;
    void setAlpha(float alpha) const noexcept;
    void setRounding(const RectI& box, float radius, float power = 2.0f) const noexcept;
    void setBackdropBlur(int texUnit, const RectF& uvBox, float blurAlpha = 1.0f) const noexcept;
    void setAlphaMatte(int texUnit) const noexcept;
    void setDiscard(bool discardOpaque, float discardAlphaThreshold = 0.0f) const noexcept;
    void setMotionBlur(float velocityUvX, float velocityUvY, int samples = 8) const noexcept;
    void setColorTransform(const ColorTransformParams& cm) const noexcept;
    void setLut3D(int texUnit, float dimN) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }
    [[nodiscard]] uint32_t features() const noexcept { return m_features; }

private:
    TextureQuadProgram::Target m_target;
    uint32_t m_features{0};
    GLint m_posAttrib{-1};
    GLint m_projLoc{-1};
    GLint m_texProjLoc{-1};
    GLint m_texLoc{-1};
    GLint m_alphaLoc{-1};
    // Rounding
    GLint m_boxLoc{-1};
    GLint m_radiusLoc{-1};
    GLint m_powerLoc{-1};
    // Backdrop blur
    GLint m_blurTexLoc{-1};
    GLint m_blurUvBoxLoc{-1};
    GLint m_blurAlphaLoc{-1};
    // Matte
    GLint m_matteTexLoc{-1};
    // Discard
    GLint m_discardOpaqueLoc{-1};
    GLint m_discardThresholdLoc{-1};
    // Motion blur
    GLint m_motionVelLoc{-1};
    GLint m_motionSamplesLoc{-1};
    // Color management
    GLint m_srcTfLoc{-1};
    GLint m_dstTfLoc{-1};
    GLint m_gamutMatLoc{-1};
    GLint m_lumParamsLoc{-1};
    GLint m_tonemapOpLoc{-1};
    // 3D LUT
    GLint m_lutTexLoc{-1};
    GLint m_lutDimLoc{-1};
};

/**
 * @brief Analytical squircle border shader with Oklab multi-stop rotational gradients.
 */
class BorderProgram : public ShaderProgram {
public:
    static std::unique_ptr<BorderProgram> create();
    explicit BorderProgram(GLuint programId);

    void setProjection(const Matrix3& proj) const noexcept;
    void setGeometry(
        const RectI& box,
        float innerRadius,
        float outerRadius,
        float roundingPower,
        float thickness) const noexcept;
    void setGradients(
        const GradientSpec& grad1,
        const GradientSpec* grad2 = nullptr,
        float lerpFactor = 0.0f,
        float alpha = 1.0f) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }

private:
    GLint m_posAttrib{-1};
    GLint m_projLoc{-1};
    GLint m_boxLoc{-1};
    GLint m_radiiLoc{-1}; // vec4(innerRadius, outerRadius, roundingPower, thickness)
    GLint m_grad1Loc{-1};
    GLint m_grad1CountLoc{-1};
    GLint m_grad1AngleLoc{-1};
    GLint m_grad2Loc{-1};
    GLint m_grad2CountLoc{-1};
    GLint m_grad2AngleLoc{-1};
    GLint m_lerpAlphaLoc{-1}; // vec2(lerpFactor, alpha)
};

/**
 * @brief Analytical SDF rounded drop shadow shader with Oklab gradient tinting.
 */
class ShadowProgram : public ShaderProgram {
public:
    static std::unique_ptr<ShadowProgram> create();
    explicit ShadowProgram(GLuint programId);

    void setProjection(const Matrix3& proj) const noexcept;
    void setShadowParams(
        const RectI& fullBox,
        float cornerRadius,
        float roundingPower,
        float rangeSigma,
        float falloffPower,
        const GradientSpec& grad1,
        const GradientSpec* grad2 = nullptr,
        float lerpFactor = 0.0f,
        float alpha = 1.0f) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }

private:
    GLint m_posAttrib{-1};
    GLint m_projLoc{-1};
    GLint m_boxLoc{-1};
    GLint m_paramsLoc{-1}; // vec4(cornerRadius, roundingPower, rangeSigma, falloffPower)
    GLint m_grad1Loc{-1};
    GLint m_grad1CountLoc{-1};
    GLint m_grad1AngleLoc{-1};
    GLint m_grad2Loc{-1};
    GLint m_grad2CountLoc{-1};
    GLint m_grad2AngleLoc{-1};
    GLint m_lerpAlphaLoc{-1};
};

/**
 * @brief Analytical SDF inner perimeter glow shader.
 */
class InnerGlowProgram : public ShaderProgram {
public:
    static std::unique_ptr<InnerGlowProgram> create();
    explicit InnerGlowProgram(GLuint programId);

    void setProjection(const Matrix3& proj) const noexcept;
    void setGlowParams(
        const RectI& box,
        float cornerRadius,
        float roundingPower,
        float range,
        float falloffPower,
        const GradientSpec& grad,
        float alpha = 1.0f) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }

private:
    GLint m_posAttrib{-1};
    GLint m_projLoc{-1};
    GLint m_boxLoc{-1};
    GLint m_paramsLoc{-1}; // vec4(cornerRadius, roundingPower, range, falloffPower)
    GLint m_gradLoc{-1};
    GLint m_gradCountLoc{-1};
    GLint m_gradAngleLoc{-1};
    GLint m_alphaLoc{-1};
};

/**
 * @brief Deformable 2D triangle mesh shader (used for spring-physics window wobble).
 */
class DeformableMeshProgram : public ShaderProgram {
public:
    static std::unique_ptr<DeformableMeshProgram> create();
    explicit DeformableMeshProgram(GLuint programId);

    void setProjection(const Matrix3& proj) const noexcept;
    void setTextureUnit(int unit) const noexcept;
    void setAlpha(float alpha) const noexcept;
    void setRounding(float radius, float power = 2.0f, float width = 1.0f, float height = 1.0f) const noexcept;

    [[nodiscard]] GLint posAttrib() const noexcept { return m_posAttrib; }
    [[nodiscard]] GLint uvAttrib() const noexcept { return m_uvAttrib; }

private:
    GLint m_posAttrib{-1};
    GLint m_uvAttrib{-1};
    GLint m_projLoc{-1};
    GLint m_texLoc{-1};
    GLint m_alphaLoc{-1};
    GLint m_roundLoc{-1}; // vec4(radius, power, width, height)
};

/**
 * @brief Manager that compiles, caches, and provides access to compositor shaders and variants.
 */
class ShaderManager {
public:
    static std::shared_ptr<ShaderManager> create(bool supportExternalOes = true);

    ShaderManager(
        std::unique_ptr<SolidQuadProgram> solid,
        std::unique_ptr<TextureQuadProgram> texRgba,
        std::unique_ptr<TextureQuadProgram> texRgbx,
        std::unique_ptr<TextureQuadProgram> texExternal,
        std::unique_ptr<TextureQuadProgram> texExternalRgbx,
        std::unique_ptr<BorderProgram> border,
        std::unique_ptr<ShadowProgram> shadow,
        std::unique_ptr<InnerGlowProgram> innerGlow,
        std::unique_ptr<DeformableMeshProgram> mesh);

    ~ShaderManager();

    [[nodiscard]] const SolidQuadProgram& solidQuad() const noexcept { return *m_solidQuad; }
    [[nodiscard]] const TextureQuadProgram& textureRgba() const noexcept { return *m_textureRgba; }
    [[nodiscard]] const TextureQuadProgram& textureRgbx() const noexcept { return *m_textureRgbx; }
    [[nodiscard]] const TextureQuadProgram* textureExternal() const noexcept { return m_textureExternal.get(); }
    [[nodiscard]] const TextureQuadProgram* textureExternalRgbx() const noexcept { return m_textureExternalRgbx.get(); }

    [[nodiscard]] const BorderProgram& border() const noexcept { return *m_border; }
    [[nodiscard]] const ShadowProgram& shadow() const noexcept { return *m_shadow; }
    [[nodiscard]] const InnerGlowProgram& innerGlow() const noexcept { return *m_innerGlow; }
    [[nodiscard]] const DeformableMeshProgram& deformableMesh() const noexcept { return *m_mesh; }

    /**
     * @brief Retrieves or JIT-compiles a surface shader permutation for the given target and feature mask.
     */
    [[nodiscard]] const SurfaceVariantProgram* getSurfaceVariant(
        TextureQuadProgram::Target target,
        uint32_t features);

    [[nodiscard]] size_t cachedVariantCount() const noexcept { return m_variants.size(); }

    /**
     * @brief Uploads transient deformable-mesh vertices to a persistent GL_STREAM_DRAW VBO
     *        (with buffer orphaning) and leaves it bound to GL_ARRAY_BUFFER.
     */
    [[nodiscard]] GLuint bindMeshStreamingVbo(const void* data, GLsizeiptr byteSize) const;

    /**
     * @brief Uploads clipped quad vertices to a persistent GL_STREAM_DRAW VBO
     *        and leaves it bound to GL_ARRAY_BUFFER.
     */
    [[nodiscard]] GLuint bindQuadStreamingVbo(const void* data, GLsizeiptr byteSize) const;

    /**
     * @brief Lazily creates and binds a static [0,1]^2 unit quad VBO to GL_ARRAY_BUFFER.
     */
    [[nodiscard]] GLuint bindUnitQuadVbo() const;

private:
    struct StreamingVbo {
        GLuint id{0};
        GLsizeiptr capacity{0};
    };

    GLuint uploadStreamingVbo(
        StreamingVbo& buf,
        const void* data,
        GLsizeiptr byteSize,
        GLsizeiptr minCapacity) const;

    std::unique_ptr<SolidQuadProgram> m_solidQuad;
    std::unique_ptr<TextureQuadProgram> m_textureRgba;
    std::unique_ptr<TextureQuadProgram> m_textureRgbx;
    std::unique_ptr<TextureQuadProgram> m_textureExternal;
    std::unique_ptr<TextureQuadProgram> m_textureExternalRgbx;
    std::unique_ptr<BorderProgram> m_border;
    std::unique_ptr<ShadowProgram> m_shadow;
    std::unique_ptr<InnerGlowProgram> m_innerGlow;
    std::unique_ptr<DeformableMeshProgram> m_mesh;

    std::unordered_map<uint64_t, std::unique_ptr<SurfaceVariantProgram>> m_variants;
    mutable StreamingVbo m_meshStreamingVbo;
    mutable StreamingVbo m_quadStreamingVbo;
    mutable GLuint m_unitQuadVbo{0};
};

} // namespace nisaba::compositor
