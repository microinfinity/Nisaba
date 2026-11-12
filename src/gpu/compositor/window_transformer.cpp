#include "nisaba/compositor/window_transformer.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::compositor {

// ==========================================
// SpringWobbleTransformer
// ==========================================

SpringWobbleTransformer::SpringWobbleTransformer(const SpringMeshConfig& cfg)
    : m_mesh(cfg) {}

RectI SpringWobbleTransformer::transformedExtents(const RectI& currentBox) const noexcept {
    if (m_mesh.isSettled()) return currentBox;
    const RectI def = m_mesh.deformedBounds();
    const int x1 = std::min(currentBox.x, def.x);
    const int y1 = std::min(currentBox.y, def.y);
    const int x2 = std::max(currentBox.x + currentBox.width, def.x + def.width);
    const int y2 = std::max(currentBox.y + currentBox.height, def.y + def.height);
    return RectI{x1, y1, x2 - x1, y2 - y1};
}

void SpringWobbleTransformer::apply(WindowTransformState& state) {
    if (!isActive()) return;
    if (m_mesh.targetBox() != state.logicalBox) {
        m_mesh.setTargetBox(state.logicalBox);
    }
    state.customMesh = &m_mesh.buildTriangleVertices();
}

// ==========================================
// VelocityMotionBlurTransformer
// ==========================================

VelocityMotionBlurTransformer::VelocityMotionBlurTransformer(float shutterStrength, int samples) noexcept
    : m_shutterStrength(std::clamp(shutterStrength, 0.0f, 2.0f)),
      m_samples(std::clamp(samples, 2, 8)) {}

void VelocityMotionBlurTransformer::updateWindowBox(const RectI& currentBox) noexcept {
    if (!m_hasHistory) {
        m_prevBox = currentBox;
        m_currBox = currentBox;
        m_hasHistory = true;
        m_active = false;
        return;
    }
    m_prevBox = m_currBox;
    m_currBox = currentBox;
    const int dx = std::abs(m_currBox.x - m_prevBox.x);
    const int dy = std::abs(m_currBox.y - m_prevBox.y);
    m_active = (dx + dy) >= 2;
}

void VelocityMotionBlurTransformer::step(float /*dtSeconds*/) noexcept {
    if (!m_active) return;
    // Decay previous box toward current box when motion stops
    m_prevBox.x = (m_prevBox.x + m_currBox.x) / 2;
    m_prevBox.y = (m_prevBox.y + m_currBox.y) / 2;
    if (std::abs(m_currBox.x - m_prevBox.x) + std::abs(m_currBox.y - m_prevBox.y) < 2) {
        m_prevBox = m_currBox;
        m_active = false;
    }
}

RectI VelocityMotionBlurTransformer::transformedExtents(const RectI& currentBox) const noexcept {
    if (!m_active) return currentBox;
    const int x1 = std::min(currentBox.x, m_prevBox.x);
    const int y1 = std::min(currentBox.y, m_prevBox.y);
    const int x2 = std::max(currentBox.x + currentBox.width, m_prevBox.x + m_prevBox.width);
    const int y2 = std::max(currentBox.y + currentBox.height, m_prevBox.y + m_prevBox.height);
    return RectI{x1, y1, x2 - x1, y2 - y1};
}

void VelocityMotionBlurTransformer::apply(WindowTransformState& state) {
    if (!m_active || state.logicalBox.isEmpty()) return;
    const float w = static_cast<float>(std::max(state.logicalBox.width, 1));
    const float h = static_cast<float>(std::max(state.logicalBox.height, 1));
    const float vx = (static_cast<float>(m_currBox.x - m_prevBox.x) / w) * m_shutterStrength;
    const float vy = (static_cast<float>(m_currBox.y - m_prevBox.y) / h) * m_shutterStrength;

    if (std::abs(vx) > 0.002f || std::abs(vy) > 0.002f) {
        state.motionBlur.active = true;
        state.motionBlur.velocityUvX = std::clamp(vx, -0.25f, 0.25f);
        state.motionBlur.velocityUvY = std::clamp(vy, -0.25f, 0.25f);
        state.motionBlur.samples = m_samples;
    }
}

// ==========================================
// TransformerChain
// ==========================================

void TransformerChain::add(std::shared_ptr<IWindowTransformer> transformer) {
    if (!transformer) return;
    m_transformers.push_back(std::move(transformer));
    std::sort(m_transformers.begin(), m_transformers.end(), [](const auto& a, const auto& b) {
        return a->priority() < b->priority();
    });
}

bool TransformerChain::hasActiveTransformer() const noexcept {
    for (const auto& t : m_transformers) {
        if (t->isActive()) return true;
    }
    return false;
}

bool TransformerChain::blocksDirectScanout() const noexcept {
    for (const auto& t : m_transformers) {
        if (t->blocksDirectScanout()) return true;
    }
    return false;
}

RectI TransformerChain::computeDamageExtents(const RectI& baseBox) const noexcept {
    RectI ext = baseBox;
    for (const auto& t : m_transformers) {
        if (t->isActive()) {
            const RectI te = t->transformedExtents(ext);
            const int x1 = std::min(ext.x, te.x);
            const int y1 = std::min(ext.y, te.y);
            const int x2 = std::max(ext.x + ext.width, te.x + te.width);
            const int y2 = std::max(ext.y + ext.height, te.y + te.height);
            ext = RectI{x1, y1, x2 - x1, y2 - y1};
        }
    }
    return ext;
}

void TransformerChain::stepAll(float dtSeconds) noexcept {
    for (auto& t : m_transformers) {
        t->step(dtSeconds);
    }
}

void TransformerChain::applyAll(WindowTransformState& state) const {
    for (const auto& t : m_transformers) {
        if (t->isActive()) {
            t->apply(state);
        }
    }
}

} // namespace nisaba::compositor

