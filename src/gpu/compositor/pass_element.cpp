#include "nisaba/compositor/pass_element.hpp"
#include "nisaba/compositor/render_pass.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::compositor {

namespace {

Region computeRoundedBoxOpaqueRegion(const RectI& box, float cornerRadius) noexcept {
    if (box.isEmpty()) return Region{};
    const int r = static_cast<int>(std::ceil(std::max(cornerRadius, 0.0f)));
    if (r <= 0) {
        return Region(box);
    }
    if (r * 2 >= box.width || r * 2 >= box.height) {
        return Region{};
    }

    Region opaque;
    // Center vertical strip (full height, inset horizontally by r)
    opaque.add(RectI{box.x + r, box.y, box.width - 2 * r, box.height});
    // Left and right vertical strips (inset vertically by r)
    opaque.add(RectI{box.x, box.y + r, r, box.height - 2 * r});
    opaque.add(RectI{box.x + box.width - r, box.y + r, r, box.height - 2 * r});
    return opaque;
}

} // namespace

// ==========================================
// ClearPassElement
// ==========================================

void ClearPassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    if (visibleDamage.isEmpty()) return;
    if (pass.target()) {
        const RectI fullTarget{0, 0, pass.target()->width(), pass.target()->height()};
        if (visibleDamage.rects().size() == 1 && visibleDamage.extents() == fullTarget) {
            pass.clear(m_color);
            return;
        }
    }
    pass.renderRect(visibleDamage.extents(), m_color, &visibleDamage);
}

// ==========================================
// RectPassElement
// ==========================================

Region RectPassElement::opaqueRegion() const noexcept {
    if (m_data.color.a < 0.999f || m_data.box.isEmpty()) {
        return Region{};
    }
    return computeRoundedBoxOpaqueRegion(m_data.box, m_data.cornerRadius);
}

void RectPassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    if (m_data.cornerRadius > 0.5f) {
        pass.renderRoundedRect(
            m_data.box,
            m_data.color,
            m_data.cornerRadius,
            m_data.roundingPower,
            &visibleDamage);
    } else {
        pass.renderRect(m_data.box, m_data.color, &visibleDamage);
    }
}

// ==========================================
// SurfacePassElement
// ==========================================

std::optional<RectI> SurfacePassElement::boundingBox() const noexcept {
    if (m_params.customMesh && !m_params.customMesh->empty()) {
        float minX = (*m_params.customMesh)[0].x;
        float minY = (*m_params.customMesh)[0].y;
        float maxX = minX;
        float maxY = minY;
        for (const auto& v : *m_params.customMesh) {
            minX = std::min(minX, v.x);
            minY = std::min(minY, v.y);
            maxX = std::max(maxX, v.x);
            maxY = std::max(maxY, v.y);
        }
        const int x1 = static_cast<int>(std::floor(minX));
        const int y1 = static_cast<int>(std::floor(minY));
        const int x2 = static_cast<int>(std::ceil(maxX));
        const int y2 = static_cast<int>(std::ceil(maxY));
        return RectI{x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
    }
    return m_params.destBox;
}

Region SurfacePassElement::opaqueRegion() const noexcept {
    if (!m_params.texture ||
        m_params.texture->hasAlpha() ||
        m_params.alpha < 0.999f ||
        m_params.alphaMatte != nullptr ||
        m_params.customMesh != nullptr ||
        m_params.motionBlur.active) {
        return Region{};
    }
    return computeRoundedBoxOpaqueRegion(m_params.destBox, m_params.cornerRadius);
}

void SurfacePassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    pass.renderSurface(m_params, &visibleDamage);
}

// ==========================================
// BorderPassElement
// ==========================================

void BorderPassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    pass.renderBorder(
        m_data.box,
        m_data.grad1,
        m_data.hasGrad2 ? &m_data.grad2 : nullptr,
        m_data.lerpFactor,
        m_data.innerRadius,
        m_data.outerRadius,
        m_data.roundingPower,
        m_data.thickness,
        m_data.alpha,
        &visibleDamage);
}

// ==========================================
// ShadowPassElement
// ==========================================

void ShadowPassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    pass.renderShadow(
        m_data.fullBox,
        m_data.cornerRadius,
        m_data.roundingPower,
        m_data.rangeSigma,
        m_data.falloffPower,
        m_data.grad1,
        m_data.hasGrad2 ? &m_data.grad2 : nullptr,
        m_data.lerpFactor,
        m_data.alpha,
        &visibleDamage);
}

// ==========================================
// InnerGlowPassElement
// ==========================================

void InnerGlowPassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    pass.renderInnerGlow(
        m_data.box,
        m_data.cornerRadius,
        m_data.roundingPower,
        m_data.range,
        m_data.falloffPower,
        m_data.grad,
        m_data.alpha,
        &visibleDamage);
}

// ==========================================
// BlurMaterialPassElement
// ==========================================

void BlurMaterialPassElement::draw(RenderPass& pass, const Region& visibleDamage) {
    if (!m_data.kawase || !m_data.source || m_data.destBox.isEmpty()) return;
    if (!visibleDamage.intersects(m_data.destBox)) return;

    if (m_data.provider) {
        pass.renderBlurMaterial(
            *m_data.provider,
            *m_data.kawase,
            *m_data.source,
            m_data.destBox,
            m_data.srcBox,
            m_data.context);
    } else {
        pass.renderBlur(
            *m_data.kawase,
            *m_data.source,
            m_data.destBox,
            m_data.srcBox,
            m_data.context.kawase,
            m_data.context.maskTexture);
    }
}

} // namespace nisaba::compositor

