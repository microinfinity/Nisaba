#include "nisaba/compositor/render_pass.hpp"
#include "nisaba/compositor/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace nisaba::compositor {

namespace {

/**
 * @brief Maps a target-space integer rectangle to an OpenGL scissor box with exact
 * single-valued edge rounding so adjacent Region slices never overlap or gap by 1px.
 */
RectI mapTargetRectToGlScissor(
    const RectI& r,
    int fbW,
    int fbH,
    wl_output_transform transform) noexcept {
    if (r.isEmpty() || fbW <= 0 || fbH <= 0) {
        return RectI{0, 0, 0, 0};
    }

    const int x0 = std::clamp(r.x, 0, fbW);
    const int y0 = std::clamp(r.y, 0, fbH);
    const int x1 = std::clamp(r.x + r.width, 0, fbW);
    const int y1 = std::clamp(r.y + r.height, 0, fbH);
    if (x1 <= x0 || y1 <= y0) {
        return RectI{0, 0, 0, 0};
    }

    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
            sx0 = x0; sx1 = x1;
            sy0 = fbH - y1; sy1 = fbH - y0;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            sx0 = x0; sx1 = x1;
            sy0 = y0; sy1 = y1;
            break;
        case WL_OUTPUT_TRANSFORM_180:
            sx0 = fbW - x1; sx1 = fbW - x0;
            sy0 = y0; sy1 = y1;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            sx0 = fbW - x1; sx1 = fbW - x0;
            sy0 = fbH - y1; sy1 = fbH - y0;
            break;
        case WL_OUTPUT_TRANSFORM_90:
            sx0 = y0; sx1 = y1;
            sy0 = x0; sy1 = x1;
            break;
        case WL_OUTPUT_TRANSFORM_270:
            sx0 = fbW - y1; sx1 = fbW - y0;
            sy0 = fbH - x1; sy1 = fbH - x0;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            sx0 = y0; sx1 = y1;
            sy0 = fbH - x1; sy1 = fbH - x0;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            sx0 = fbW - y1; sx1 = fbW - y0;
            sy0 = x0; sy1 = x1;
            break;
        default: {
            const Matrix3 proj = Matrix3::project(fbW, fbH, transform);
            const double nx0 = static_cast<double>(x0) * proj[0] + static_cast<double>(y0) * proj[1] + proj[2];
            const double ny0 = static_cast<double>(x0) * proj[3] + static_cast<double>(y0) * proj[4] + proj[5];
            const double nx1 = static_cast<double>(x1) * proj[0] + static_cast<double>(y1) * proj[1] + proj[2];
            const double ny1 = static_cast<double>(x1) * proj[3] + static_cast<double>(y1) * proj[4] + proj[5];
            sx0 = static_cast<int>(std::lround((std::min(nx0, nx1) * 0.5 + 0.5) * static_cast<double>(fbW)));
            sx1 = static_cast<int>(std::lround((std::max(nx0, nx1) * 0.5 + 0.5) * static_cast<double>(fbW)));
            sy0 = static_cast<int>(std::lround((std::min(ny0, ny1) * 0.5 + 0.5) * static_cast<double>(fbH)));
            sy1 = static_cast<int>(std::lround((std::max(ny0, ny1) * 0.5 + 0.5) * static_cast<double>(fbH)));
            break;
        }
    }

    sx0 = std::clamp(sx0, 0, fbW);
    sx1 = std::clamp(sx1, 0, fbW);
    sy0 = std::clamp(sy0, 0, fbH);
    sy1 = std::clamp(sy1, 0, fbH);
    if (sx1 <= sx0 || sy1 <= sy0) {
        return RectI{0, 0, 0, 0};
    }
    return RectI{sx0, sy0, sx1 - sx0, sy1 - sy0};
}

Region expandRegionClamped(const Region& reg, int pad, const RectI& bounds) {
    if (reg.isEmpty()) return Region{};
    if (pad <= 0) {
        Region out = reg;
        out.intersect(bounds);
        return out;
    }
    Region expanded;
    for (const auto& r : reg.rects()) {
        if (r.isEmpty()) continue;
        const RectI padded = RectI{
            r.x - pad,
            r.y - pad,
            r.width + pad * 2,
            r.height + pad * 2
        }.intersection(bounds);
        if (!padded.isEmpty()) {
            expanded.add(padded);
        }
    }
    return expanded;
}

} // namespace

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
      m_shaders(std::move(shaders)),
      m_outputTransform(outputTransform) {
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

    // 1. Front-to-Back (reverse) Occlusion Culling pass
    Region remainingDamage = initialDamage;
    for (size_t i = m_queue.size(); i-- > 0;) {
        auto& entry = m_queue[i];
        if (!entry.element) {
            entry.discarded = true;
            continue;
        }

        const bool undiscardable = entry.element->undiscardable();
        if (remainingDamage.isEmpty() && !undiscardable) {
            entry.discarded = true;
            stats.culledElements++;
            continue;
        }

        const auto bbOpt = entry.element->boundingBox();
        if (bbOpt.has_value()) {
            if (bbOpt->isEmpty() || (!undiscardable && !remainingDamage.intersects(*bbOpt))) {
                entry.discarded = true;
                stats.culledElements++;
                continue;
            }
            entry.visibleDamage = remainingDamage;
            entry.visibleDamage.intersect(*bbOpt);
            if (entry.visibleDamage.isEmpty() && !undiscardable) {
                entry.discarded = true;
                stats.culledElements++;
                continue;
            }
        } else {
            entry.visibleDamage = remainingDamage;
        }

        if (entry.element->needsLiveBlur() && !entry.visibleDamage.isEmpty()) {
            const RectI targetBounds = m_target
                ? RectI{0, 0, m_target->width(), m_target->height()}
                : entry.visibleDamage.extents();
            const int pad = std::max(0, entry.element->liveBlurPadding());
            const Region blurSampleRegion = expandRegionClamped(entry.visibleDamage, pad, targetBounds);
            stats.liveBlurRegion.unite(blurSampleRegion);
            // Elements behind this live-blur element must render the full kernel halo,
            // even if an opaque element above subtracted part of that area earlier.
            remainingDamage.unite(blurSampleRegion);
        }

        Region opaque = entry.element->opaqueRegion();
        if (!opaque.isEmpty() && remainingDamage.intersects(opaque.extents())) {
            if (!stats.liveBlurRegion.isEmpty() && opaque.intersects(stats.liveBlurRegion)) {
                opaque.subtract(stats.liveBlurRegion);
            }
            if (!opaque.isEmpty()) {
                remainingDamage.subtract(opaque);
            }
        }
    }

    stats.remainingUnoccludedDamage = remainingDamage;

    // 2. Back-to-Front execution of non-occluded elements
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
    const GLsizeiptr byteSize = static_cast<GLsizeiptr>(vertices.size() * sizeof(MeshVertex));
    const GLuint vbo = m_shaders ? m_shaders->bindMeshStreamingVbo(vertices.data(), byteSize) : 0;
    const void* posPtr = vbo ? reinterpret_cast<const void*>(offsetof(MeshVertex, x))
                             : static_cast<const void*>(&vertices[0].x);
    const void* uvPtr  = vbo ? reinterpret_cast<const void*>(offsetof(MeshVertex, u))
                             : static_cast<const void*>(&vertices[0].u);

    if (posLoc >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(posLoc));
        glVertexAttribPointer(
            static_cast<GLuint>(posLoc),
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(MeshVertex),
            posPtr);
    }
    if (uvLoc >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(uvLoc));
        glVertexAttribPointer(
            static_cast<GLuint>(uvLoc),
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(MeshVertex),
            uvPtr);
    }

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    if (uvLoc >= 0) glDisableVertexAttribArray(static_cast<GLuint>(uvLoc));
    if (posLoc >= 0) glDisableVertexAttribArray(static_cast<GLuint>(posLoc));
    if (vbo) glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(texture.target(), 0);
}

void RenderPass::renderBlur(
    DualKawaseBlur& blurEngine,
    const Texture& source,
    const RectI& destBox,
    const RectI& srcBox,
    const DualKawaseConfig& config,
    const Texture* maskTexture,
    const Region* clip) {
    if (destBox.isEmpty() || !source.id() || !m_target) return;
    if (!clip) {
        blurEngine.renderBlurSubRegion(
            source, srcBox, *m_target, destBox, config, maskTexture, m_outputTransform);
        return;
    }
    if (clip->isEmpty() || !clip->intersects(destBox)) return;

    const int fbW = m_target->width();
    const int fbH = m_target->height();
    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint prevBox[4] = {0, 0, fbW, fbH};
    if (prevScissor) {
        glGetIntegerv(GL_SCISSOR_BOX, prevBox);
    }

    auto blurred = blurEngine.blur(source, config);
    if (!blurred) {
        return;
    }

    const RectI effectiveSrc = srcBox.isEmpty() ? destBox : srcBox;
    const RectF normSrc{
        static_cast<double>(effectiveSrc.x) / static_cast<double>(std::max(1u, source.width())),
        static_cast<double>(effectiveSrc.y) / static_cast<double>(std::max(1u, source.height())),
        static_cast<double>(effectiveSrc.width) / static_cast<double>(std::max(1u, source.width())),
        static_cast<double>(effectiveSrc.height) / static_cast<double>(std::max(1u, source.height()))
    };

    glEnable(GL_SCISSOR_TEST);
    for (const auto& c : clip->rects()) {
        const RectI sub = destBox.intersection(c);
        if (sub.isEmpty()) continue;
        RectI sc = mapTargetRectToGlScissor(sub, fbW, fbH, m_outputTransform);
        if (prevScissor) {
            sc = sc.intersection(RectI{prevBox[0], prevBox[1], prevBox[2], prevBox[3]});
        }
        if (sc.isEmpty()) continue;
        glScissor(sc.x, sc.y, sc.width, sc.height);
        blurEngine.renderBlurredBackdrop(
            *blurred, destBox, normSrc, *m_target, config, maskTexture, m_outputTransform);
    }

    if (prevScissor) {
        glScissor(prevBox[0], prevBox[1], prevBox[2], prevBox[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void RenderPass::renderBlurMaterial(
    IBlurProvider& provider,
    DualKawaseBlur& kawase,
    const Texture& source,
    const RectI& destBox,
    const RectI& srcBox,
    const BlurContext& ctx,
    const Region* clip) {
    if (destBox.isEmpty() || !source.id() || !m_target) return;
    BlurContext effectiveCtx = ctx;
    effectiveCtx.transform = m_outputTransform;

    if (!clip) {
        provider.renderMaterial(kawase, source, srcBox, *m_target, destBox, effectiveCtx);
        return;
    }
    if (clip->isEmpty() || !clip->intersects(destBox)) return;

    const auto& rects = clip->rects();
    if (rects.size() == 1 && destBox.intersection(rects[0]) == destBox) {
        provider.renderMaterial(kawase, source, srcBox, *m_target, destBox, effectiveCtx);
        return;
    }

    const int fbW = m_target->width();
    const int fbH = m_target->height();
    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint prevBox[4] = {0, 0, fbW, fbH};
    if (prevScissor) {
        glGetIntegerv(GL_SCISSOR_BOX, prevBox);
    }

    glEnable(GL_SCISSOR_TEST);
    for (const auto& c : rects) {
        const RectI sub = destBox.intersection(c);
        if (sub.isEmpty()) continue;
        RectI sc = mapTargetRectToGlScissor(sub, fbW, fbH, m_outputTransform);
        if (prevScissor) {
            sc = sc.intersection(RectI{prevBox[0], prevBox[1], prevBox[2], prevBox[3]});
        }
        if (sc.isEmpty()) continue;
        glScissor(sc.x, sc.y, sc.width, sc.height);
        provider.renderMaterial(kawase, source, srcBox, *m_target, destBox, effectiveCtx);
    }

    if (prevScissor) {
        glScissor(prevBox[0], prevBox[1], prevBox[2], prevBox[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void RenderPass::renderQuads(
    const RectI& box,
    const Region* clip,
    GLint posAttrib) const {
    if (posAttrib < 0 || box.isEmpty()) return;

    static constexpr float kFallbackUnitQuad[12] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
    };

    auto drawUnit = [this, posAttrib]() {
        const GLuint vbo = m_shaders ? m_shaders->bindUnitQuadVbo() : 0;
        glEnableVertexAttribArray(static_cast<GLuint>(posAttrib));
        glVertexAttribPointer(
            static_cast<GLuint>(posAttrib),
            2,
            GL_FLOAT,
            GL_FALSE,
            0,
            vbo ? nullptr : static_cast<const void*>(kFallbackUnitQuad));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(static_cast<GLuint>(posAttrib));
        if (vbo) glBindBuffer(GL_ARRAY_BUFFER, 0);
    };

    if (!clip) {
        drawUnit();
        return;
    }

    if (clip->isEmpty() || !clip->intersects(box) || !m_target) {
        return;
    }

    const int fbW = m_target->width();
    const int fbH = m_target->height();
    if (fbW <= 0 || fbH <= 0) {
        return;
    }

    const auto& clipRects = clip->rects();
    if (clipRects.size() == 1 && box.intersection(clipRects[0]) == box) {
        drawUnit();
        return;
    }

    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint prevBox[4] = {0, 0, fbW, fbH};
    if (prevScissor) {
        glGetIntegerv(GL_SCISSOR_BOX, prevBox);
    }

    const GLuint vbo = m_shaders ? m_shaders->bindUnitQuadVbo() : 0;
    glEnableVertexAttribArray(static_cast<GLuint>(posAttrib));
    glVertexAttribPointer(
        static_cast<GLuint>(posAttrib),
        2,
        GL_FLOAT,
        GL_FALSE,
        0,
        vbo ? nullptr : static_cast<const void*>(kFallbackUnitQuad));

    glEnable(GL_SCISSOR_TEST);
    for (const auto& c : clipRects) {
        if (!c.intersects(box)) continue;
        const RectI r = box.intersection(c);
        RectI sc = mapTargetRectToGlScissor(r, fbW, fbH, m_outputTransform);
        if (prevScissor) {
            sc = sc.intersection(RectI{prevBox[0], prevBox[1], prevBox[2], prevBox[3]});
        }
        if (sc.isEmpty()) continue;
        glScissor(sc.x, sc.y, sc.width, sc.height);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    if (prevScissor) {
        glScissor(prevBox[0], prevBox[1], prevBox[2], prevBox[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    glDisableVertexAttribArray(static_cast<GLuint>(posAttrib));
    if (vbo) glBindBuffer(GL_ARRAY_BUFFER, 0);
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
