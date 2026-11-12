#include "nisaba/compositor/render_pass.hpp"
#include "nisaba/compositor/log.hpp"

#include <vector>

namespace nisaba::compositor {

std::unique_ptr<RenderPass> RenderPass::begin(
    std::shared_ptr<RenderTarget> target,
    std::shared_ptr<ShaderManager> shaders,
    wl_output_transform outputTransform) {
    if (!target || !shaders) {
        logError("RenderPass::begin failed: target or shaders is null");
        return nullptr;
    }

    return std::make_unique<RenderPass>(std::move(target), std::move(shaders), outputTransform);
}

RenderPass::RenderPass(
    std::shared_ptr<RenderTarget> target,
    std::shared_ptr<ShaderManager> shaders,
    wl_output_transform outputTransform)
    : m_target(std::move(target)),
      m_shaders(std::move(shaders)) {
    m_target->bind();
    m_projection = Matrix3::project(m_target->width(), m_target->height(), outputTransform);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
}

RenderPass::~RenderPass() {
    if (!m_submitted) {
        submit();
    }
}

void RenderPass::enqueue(std::unique_ptr<IPassElement> element) {
    if (!element) return;
    QueuedEntry entry;
    entry.element = std::move(element);
    m_queue.push_back(std::move(entry));
}

void RenderPass::clearQueue() noexcept {
    m_queue.clear();
}

PassExecutionStats RenderPass::optimizeAndExecute(const Region& initialDamage) {
    PassExecutionStats stats;
    stats.totalElements = m_queue.size();
    if (m_queue.empty()) {
        return stats;
    }

    // 1. Identify any active live-blur regions so underlying backdrops are not prematurely occluded
    for (const auto& entry : m_queue) {
        if (entry.element && entry.element->needsLiveBlur()) {
            if (auto bb = entry.element->boundingBox()) {
                stats.liveBlurRegion.add(*bb);
            }
        }
    }

    // 2. Front-to-Back (reverse) Occlusion Culling pass
    Region remainingDamage = initialDamage;
    for (size_t i = m_queue.size(); i-- > 0;) {
        auto& entry = m_queue[i];
        if (!entry.element) {
            entry.discarded = true;
            continue;
        }

        if (remainingDamage.isEmpty() && !entry.element->undiscardable()) {
            entry.discarded = true;
            stats.culledElements++;
            continue;
        }

        const auto bbOpt = entry.element->boundingBox();
        if (bbOpt.has_value()) {
            if (bbOpt->isEmpty()) {
                entry.discarded = true;
                stats.culledElements++;
                continue;
            }
            Region visible = remainingDamage;
            visible.intersect(Region(*bbOpt));
            if (visible.isEmpty() && !entry.element->undiscardable()) {
                entry.discarded = true;
                stats.culledElements++;
                continue;
            }
            entry.visibleDamage = std::move(visible);
        } else {
            entry.visibleDamage = remainingDamage;
        }

        Region opaque = entry.element->opaqueRegion();
        if (!opaque.isEmpty()) {
            if (!stats.liveBlurRegion.isEmpty()) {
                opaque.subtract(stats.liveBlurRegion);
            }
            remainingDamage.subtract(opaque);
        }
    }

    stats.remainingUnoccludedDamage = remainingDamage;

    // 3. Back-to-Front execution of non-occluded elements
    for (auto& entry : m_queue) {
        if (entry.discarded || !entry.element) continue;
        entry.element->draw(*this, entry.visibleDamage);
        stats.executedElements++;
    }

    m_queue.clear();
    return stats;
}

void RenderPass::clear(const Color& color) const noexcept {
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void RenderPass::renderRect(
    const RectI& box,
    const Color& color,
    const Region* clip) const {
    if (box.isEmpty()) return;

    if (color.a >= 0.999f) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    const auto& shader = m_shaders->solidQuad();
    shader.use();

    Matrix3 mvp = Matrix3::projectBox(box, WL_OUTPUT_TRANSFORM_NORMAL, m_projection);
    shader.setProjection(mvp);
    shader.setColor(color.premultiplied());

    renderQuads(box, clip, shader.posAttrib());
}

void RenderPass::renderRoundedRect(
    const RectI& box,
    const Color& color,
    float cornerRadius,
    float roundingPower,
    const Region* clip) const {
    if (box.isEmpty()) return;
    if (cornerRadius <= 0.5f) {
        renderRect(box, color, clip);
        return;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const auto& shader = m_shaders->solidQuad();
    shader.use();

    Matrix3 mvp = Matrix3::projectBox(box, WL_OUTPUT_TRANSFORM_NORMAL, m_projection);
    shader.setProjection(mvp);
    shader.setColor(color.premultiplied());
    shader.setRounding(box, cornerRadius, roundingPower);

    renderQuads(box, clip, shader.posAttrib());
}

void RenderPass::renderTexture(
    const ITexture& texture,
    const RectI& destBox,
    const RectF& srcBox,
    float alpha,
    wl_output_transform transform,
    const Region* clip) const {
    if (texture.backendType() == RenderBackendType::OpenGL) {
        renderTexture(static_cast<const Texture&>(texture), destBox, srcBox, alpha, transform, clip);
    }
}

void RenderPass::renderTexture(
    const Texture& texture,
    const RectI& destBox,
    const RectF& srcBox,
    float alpha,
    wl_output_transform transform,
    const Region* clip) const {
    if (destBox.isEmpty() || !texture.id()) return;

    const TextureQuadProgram* shader = nullptr;
    if (texture.target() == GL_TEXTURE_EXTERNAL_OES) {
        if (!texture.hasAlpha()) {
            shader = m_shaders->textureExternalRgbx();
        } else {
            shader = m_shaders->textureExternal();
        }
        if (!shader) {
            logError("External OES shader not available for external texture");
            return;
        }
    } else {
        if (!texture.hasAlpha()) {
            shader = &m_shaders->textureRgbx();
        } else {
            shader = &m_shaders->textureRgba();
        }
    }

    if (!texture.hasAlpha() && alpha >= 0.999f) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    shader->use();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(texture.target(), texture.id());
    glTexParameteri(texture.target(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(texture.target(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    shader->setTextureUnit(0);
    shader->setAlpha(alpha);

    Matrix3 mvp = Matrix3::projectBox(destBox, transform, m_projection);
    shader->setProjection(mvp);

    Matrix3 texMvp = Matrix3::projectTexture(srcBox, transform);
    shader->setTextureProjection(texMvp);

    renderQuads(destBox, clip, shader->posAttrib());

    glBindTexture(texture.target(), 0);
}

void RenderPass::renderSurface(
    const SurfaceRenderParams& params,
    const Region* clip) const {
    if (!params.texture || !params.texture->id()) return;

    // Deformable mesh path (e.g. wobbly window)
    if (params.customMesh && !params.customMesh->empty()) {
        renderMesh(
            *params.texture,
            *params.customMesh,
            params.alpha,
            params.cornerRadius,
            params.roundingPower,
            SizeI{params.destBox.width, params.destBox.height});
        return;
    }

    if (params.destBox.isEmpty()) return;

    uint32_t features = kShaderFeatNone;
    if (params.cornerRadius > 0.5f) features |= kShaderFeatRoundCorners;
    if (params.blurredBackdrop && params.blurredBackdrop->id()) features |= kShaderFeatBackdropBlur;
    if (params.alphaMatte && params.alphaMatte->id()) features |= kShaderFeatAlphaMatte;
    if (params.discardOpaque || params.discardAlphaThreshold > 0.0f) features |= kShaderFeatDiscard;
    if (params.motionBlur.active) features |= kShaderFeatMotionBlur;
    if (params.colorTransform.enabled) {
        features |= kShaderFeatColorManagement;
        if (params.colorTransform.tonemap != TonemapOperator::None) {
            features |= kShaderFeatTonemap;
        }
    }
    if (params.lut3D && params.lut3D->texture() && params.lut3D->texture()->id()) {
        features |= kShaderFeatIccLut3D;
    }
    if (m_target && m_target->mirrorTexture()) {
        features |= kShaderFeatMirrorMrt;
    }

    // Fast path if no extra features and linear filter
    if (features == kShaderFeatNone && !params.preferNearestFilter) {
        renderTexture(*params.texture, params.destBox, params.srcBox, params.alpha, params.transform, clip);
        return;
    }

    TextureQuadProgram::Target targetType = TextureQuadProgram::Target::Rgba;
    if (params.texture->target() == GL_TEXTURE_EXTERNAL_OES) {
        targetType = params.texture->hasAlpha()
                         ? TextureQuadProgram::Target::ExternalOes
                         : TextureQuadProgram::Target::ExternalOesRgbx;
    } else {
        targetType = params.texture->hasAlpha()
                         ? TextureQuadProgram::Target::Rgba
                         : TextureQuadProgram::Target::Rgbx;
    }

    const auto* variant = m_shaders->getSurfaceVariant(targetType, features);
    if (!variant) {
        renderTexture(*params.texture, params.destBox, params.srcBox, params.alpha, params.transform, clip);
        return;
    }

    const bool canDisableBlend = !params.texture->hasAlpha() &&
                                 params.alpha >= 0.999f &&
                                 params.cornerRadius <= 0.5f &&
                                 params.alphaMatte == nullptr;
    if (canDisableBlend) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    variant->use();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(params.texture->target(), params.texture->id());
    const GLint filter = params.preferNearestFilter ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(params.texture->target(), GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(params.texture->target(), GL_TEXTURE_MAG_FILTER, filter);
    variant->setTextureUnit(0);
    variant->setAlpha(params.alpha);

    const Matrix3 mvp = Matrix3::projectBox(params.destBox, params.transform, m_projection);
    variant->setProjection(mvp);

    const Matrix3 texMvp = Matrix3::projectTexture(params.srcBox, params.transform);
    variant->setTextureProjection(texMvp);

    if (features & kShaderFeatRoundCorners) {
        variant->setRounding(params.destBox, params.cornerRadius, params.roundingPower);
    }
    if (features & kShaderFeatBackdropBlur) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, params.blurredBackdrop->id());
        variant->setBackdropBlur(1, params.blurredBackdropUv, params.blurAlpha);
    }
    if (features & kShaderFeatAlphaMatte) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, params.alphaMatte->id());
        variant->setAlphaMatte(2);
    }
    if (features & kShaderFeatDiscard) {
        variant->setDiscard(params.discardOpaque, params.discardAlphaThreshold);
    }
    if (features & kShaderFeatMotionBlur) {
        variant->setMotionBlur(
            params.motionBlur.velocityUvX,
            params.motionBlur.velocityUvY,
            params.motionBlur.samples);
    }
    if (features & kShaderFeatColorManagement) {
        variant->setColorTransform(params.colorTransform);
    }
    if (features & kShaderFeatIccLut3D) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, params.lut3D->texture()->id());
        variant->setLut3D(3, static_cast<float>(params.lut3D->dimension()));
    }

    renderQuads(params.destBox, clip, variant->posAttrib());

    if (features & kShaderFeatIccLut3D) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (features & kShaderFeatAlphaMatte) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (features & kShaderFeatBackdropBlur) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    if (params.preferNearestFilter) {
        glTexParameteri(params.texture->target(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(params.texture->target(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(params.texture->target(), 0);
}

void RenderPass::renderBorder(
    const RectI& box,
    const GradientSpec& grad1,
    const GradientSpec* grad2,
    float lerpFactor,
    float innerRadius,
    float outerRadius,
    float roundingPower,
    float thickness,
    float alpha,
    const Region* clip) const {
    if (box.isEmpty() || thickness <= 0.0f || alpha <= 0.001f) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const auto& shader = m_shaders->border();
    shader.use();

    const Matrix3 mvp = Matrix3::projectBox(box, WL_OUTPUT_TRANSFORM_NORMAL, m_projection);
    shader.setProjection(mvp);
    shader.setGeometry(box, innerRadius, outerRadius, roundingPower, thickness);
    shader.setGradients(grad1, grad2, lerpFactor, alpha);

    renderQuads(box, clip, shader.posAttrib());
}

void RenderPass::renderShadow(
    const RectI& fullBox,
    float cornerRadius,
    float roundingPower,
    float rangeSigma,
    float falloffPower,
    const GradientSpec& grad1,
    const GradientSpec* grad2,
    float lerpFactor,
    float alpha,
    const Region* clip) const {
    if (fullBox.isEmpty() || rangeSigma <= 0.0f || alpha <= 0.001f) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const auto& shader = m_shaders->shadow();
    shader.use();

    const Matrix3 mvp = Matrix3::projectBox(fullBox, WL_OUTPUT_TRANSFORM_NORMAL, m_projection);
    shader.setProjection(mvp);
    shader.setShadowParams(
        fullBox,
        cornerRadius,
        roundingPower,
        rangeSigma,
        falloffPower,
        grad1,
        grad2,
        lerpFactor,
        alpha);

    renderQuads(fullBox, clip, shader.posAttrib());
}

void RenderPass::renderInnerGlow(
    const RectI& box,
    float cornerRadius,
    float roundingPower,
    float range,
    float falloffPower,
    const GradientSpec& grad,
    float alpha,
    const Region* clip) const {
    if (box.isEmpty() || range <= 0.0f || alpha <= 0.001f) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const auto& shader = m_shaders->innerGlow();
    shader.use();

    const Matrix3 mvp = Matrix3::projectBox(box, WL_OUTPUT_TRANSFORM_NORMAL, m_projection);
    shader.setProjection(mvp);
    shader.setGlowParams(box, cornerRadius, roundingPower, range, falloffPower, grad, alpha);

    renderQuads(box, clip, shader.posAttrib());
}

void RenderPass::renderMesh(
    const Texture& texture,
    const std::vector<MeshVertex>& vertices,
    float alpha,
    float cornerRadius,
    float roundingPower,
    const SizeI& refSize) const {
    if (vertices.empty() || !texture.id()) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const auto& shader = m_shaders->deformableMesh();
    shader.use();

    shader.setProjection(m_projection);
    shader.setTextureUnit(0);
    shader.setAlpha(alpha);
    const float w = static_cast<float>(refSize.width > 0 ? refSize.width : static_cast<int>(texture.width()));
    const float h = static_cast<float>(refSize.height > 0 ? refSize.height : static_cast<int>(texture.height()));
    shader.setRounding(cornerRadius, roundingPower, w, h);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(texture.target(), texture.id());
    glTexParameteri(texture.target(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(texture.target(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const GLint posLoc = shader.posAttrib();
    const GLint uvLoc = shader.uvAttrib();
    if (posLoc >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(posLoc));
        glVertexAttribPointer(
            static_cast<GLuint>(posLoc),
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(MeshVertex),
            &vertices[0].x);
    }
    if (uvLoc >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(uvLoc));
        glVertexAttribPointer(
            static_cast<GLuint>(uvLoc),
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(MeshVertex),
            &vertices[0].u);
    }

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    if (uvLoc >= 0) glDisableVertexAttribArray(static_cast<GLuint>(uvLoc));
    if (posLoc >= 0) glDisableVertexAttribArray(static_cast<GLuint>(posLoc));
    glBindTexture(texture.target(), 0);
}

void RenderPass::renderBlur(
    DualKawaseBlur& blurEngine,
    const Texture& source,
    const RectI& destBox,
    const RectI& srcBox,
    const DualKawaseConfig& config,
    const Texture* maskTexture) {
    if (destBox.isEmpty() || !source.id() || !m_target) return;
    blurEngine.renderBlurSubRegion(source, srcBox, *m_target, destBox, config, maskTexture);
}

void RenderPass::renderBlurMaterial(
    IBlurProvider& provider,
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& destBox,
    const RectI& srcBox,
    const BlurContext& ctx) {
    if (destBox.isEmpty() || !source.id() || !m_target) return;
    provider.renderMaterial(kawase, source, srcBox, *m_target, destBox, ctx);
}

void RenderPass::renderQuads(
    const RectI& box,
    const Region* clip,
    GLint posAttrib) const {
    if (posAttrib < 0) return;

    if (!clip) {
        static constexpr float kUnitQuad[12] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            0.0f, 1.0f,
            1.0f, 0.0f,
            1.0f, 1.0f,
            0.0f, 1.0f,
        };
        glEnableVertexAttribArray(static_cast<GLuint>(posAttrib));
        glVertexAttribPointer(static_cast<GLuint>(posAttrib), 2, GL_FLOAT, GL_FALSE, 0, kUnitQuad);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(static_cast<GLuint>(posAttrib));
        return;
    }

    Region boxRegion(box);
    boxRegion.intersect(*clip);
    auto rects = boxRegion.rects();
    if (rects.empty()) return;

    std::vector<float> verts;
    verts.reserve(rects.size() * 12);
    const float invW = 1.0f / static_cast<float>(box.width);
    const float invH = 1.0f / static_cast<float>(box.height);

    for (const auto& r : rects) {
        float x1 = static_cast<float>(r.x - box.x) * invW;
        float y1 = static_cast<float>(r.y - box.y) * invH;
        float x2 = static_cast<float>(r.x + r.width - box.x) * invW;
        float y2 = static_cast<float>(r.y + r.height - box.y) * invH;

        // Triangle 1
        verts.push_back(x1); verts.push_back(y1);
        verts.push_back(x2); verts.push_back(y1);
        verts.push_back(x1); verts.push_back(y2);

        // Triangle 2
        verts.push_back(x2); verts.push_back(y1);
        verts.push_back(x2); verts.push_back(y2);
        verts.push_back(x1); verts.push_back(y2);
    }

    glEnableVertexAttribArray(static_cast<GLuint>(posAttrib));
    glVertexAttribPointer(static_cast<GLuint>(posAttrib), 2, GL_FLOAT, GL_FALSE, 0, verts.data());
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(rects.size() * 6));
    glDisableVertexAttribArray(static_cast<GLuint>(posAttrib));
}

bool RenderPass::submit() {
    if (m_submitted) return true;

    if (!m_queue.empty() && m_target) {
        optimizeAndExecute(Region(RectI{0, 0, m_target->width(), m_target->height()}));
    }

    glFlush();
    RenderTarget::unbind();
    m_submitted = true;
    return true;
}

} // namespace nisaba::compositor
