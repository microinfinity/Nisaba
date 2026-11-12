#include "nisaba/compositor/window_decoration.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::compositor {

// ==========================================
// DropShadowDecoration
// ==========================================

DecorationExtents DropShadowDecoration::extents() const noexcept {
    const int r = static_cast<int>(std::ceil(std::max(m_cfg.rangeSigma, 0.0f)));
    return DecorationExtents{
        std::max(0, r - m_cfg.offset.y),
        std::max(0, r + m_cfg.offset.y),
        std::max(0, r - m_cfg.offset.x),
        std::max(0, r + m_cfg.offset.x)
    };
}

RectI DropShadowDecoration::damageBox(const RectI& clientBox) const noexcept {
    const int r = static_cast<int>(std::ceil(std::max(m_cfg.rangeSigma, 0.0f)));
    return RectI{
        clientBox.x + m_cfg.offset.x - r,
        clientBox.y + m_cfg.offset.y - r,
        clientBox.width + r * 2,
        clientBox.height + r * 2
    };
}

void DropShadowDecoration::enqueue(
    RenderPass& pass,
    const RectI& clientBox,
    float windowAlpha) const {
    if (clientBox.isEmpty() || m_cfg.rangeSigma <= 0.0f || windowAlpha <= 0.001f) return;

    ShadowPassElement::Data data;
    data.fullBox = damageBox(clientBox);
    data.cornerRadius = m_cfg.cornerRadius;
    data.roundingPower = m_cfg.roundingPower;
    data.rangeSigma = m_cfg.rangeSigma;
    data.falloffPower = m_cfg.falloffPower;
    data.grad1 = m_cfg.activeGradient;
    data.grad2 = m_cfg.inactiveGradient;
    data.hasGrad2 = (m_cfg.transitionLerp > 0.001f && !m_cfg.inactiveGradient.isEmpty());
    data.lerpFactor = m_cfg.transitionLerp;
    data.alpha = windowAlpha;

    pass.enqueue(std::make_unique<ShadowPassElement>( std::move(data) ));
}

// ==========================================
// BorderDecoration
// ==========================================

DecorationExtents BorderDecoration::extents() const noexcept {
    const int b = static_cast<int>(std::ceil(std::max(m_cfg.borderSize, 0.0f)));
    return DecorationExtents{b, b, b, b};
}

void BorderDecoration::enqueue(
    RenderPass& pass,
    const RectI& clientBox,
    float windowAlpha) const {
    if (clientBox.isEmpty() || m_cfg.borderSize <= 0.0f || windowAlpha <= 0.001f) return;

    const float b = m_cfg.borderSize;
    constexpr float kSqrt2Minus1 = 0.41421356237f;
    const float correction = b * kSqrt2Minus1 * std::max(2.0f - m_cfg.roundingPower, 0.0f);
    const float outerR = std::max(0.0f, (m_cfg.cornerRadius + b) - correction);

    BorderPassElement::Data data;
    data.box = extents().expandBox(clientBox);
    data.grad1 = m_cfg.activeGradient;
    data.grad2 = m_cfg.inactiveGradient;
    data.hasGrad2 = (m_cfg.transitionLerp > 0.001f && !m_cfg.inactiveGradient.isEmpty());
    data.lerpFactor = m_cfg.transitionLerp;
    data.innerRadius = m_cfg.cornerRadius;
    data.outerRadius = outerR;
    data.roundingPower = m_cfg.roundingPower;
    data.thickness = b;
    data.alpha = windowAlpha;

    pass.enqueue(std::make_unique<BorderPassElement>(std::move(data)));
}

// ==========================================
// InnerGlowDecoration
// ==========================================

void InnerGlowDecoration::enqueue(
    RenderPass& pass,
    const RectI& clientBox,
    float windowAlpha) const {
    if (clientBox.isEmpty() || m_cfg.range <= 0.0f || windowAlpha <= 0.001f) return;

    InnerGlowPassElement::Data data;
    data.box = clientBox;
    data.cornerRadius = m_cfg.cornerRadius;
    data.roundingPower = m_cfg.roundingPower;
    data.range = m_cfg.range;
    data.falloffPower = m_cfg.falloffPower;
    data.grad = m_cfg.gradient;
    data.alpha = windowAlpha;

    pass.enqueue(std::make_unique<InnerGlowPassElement>(std::move(data)));
}

// ==========================================
// GroupBarDecoration
// ==========================================

DecorationExtents GroupBarDecoration::extents() const noexcept {
    if (m_cfg.tabTitles.empty()) return {0, 0, 0, 0};
    return DecorationExtents{m_cfg.barHeight + m_cfg.gapToWindow, 0, 0, 0};
}

void GroupBarDecoration::enqueue(
    RenderPass& pass,
    const RectI& clientBox,
    float windowAlpha) const {
    if (clientBox.isEmpty() || m_cfg.tabTitles.empty() || windowAlpha <= 0.001f) return;

    const int barY = clientBox.y - m_cfg.barHeight - m_cfg.gapToWindow;
    const RectI barBox{clientBox.x, barY, clientBox.width, m_cfg.barHeight};

    Color bg = m_cfg.backgroundColor;
    bg.a *= windowAlpha;
    pass.enqueue(std::make_unique<RectPassElement>(RectPassElement::Data{
        barBox,
        bg,
        m_cfg.cornerRadius,
        2.0f
    }));

    const size_t tabCount = m_cfg.tabTitles.size();
    const int spacing = 4;
    const int totalSpacing = static_cast<int>(tabCount > 1 ? (tabCount - 1) * spacing : 0);
    const int tabW = std::max(1, (clientBox.width - totalSpacing) / static_cast<int>(tabCount));

    for (size_t i = 0; i < tabCount; ++i) {
        const int tabX = clientBox.x + static_cast<int>(i) * (tabW + spacing);
        const RectI indicatorBox{
            tabX,
            barY + m_cfg.barHeight - m_cfg.indicatorHeight,
            tabW,
            m_cfg.indicatorHeight
        };
        Color tabCol = (i == m_cfg.activeTabIndex) ? m_cfg.activeTabColor : m_cfg.inactiveTabColor;
        tabCol.a *= windowAlpha;
        pass.enqueue(std::make_unique<RectPassElement>(RectPassElement::Data{
            indicatorBox,
            tabCol,
            static_cast<float>(m_cfg.indicatorHeight) * 0.5f,
            2.0f
        }));
    }
}

// ==========================================
// WindowDecorationStack
// ==========================================

void WindowDecorationStack::add(std::shared_ptr<IWindowDecoration> deco) {
    if (!deco) return;
    m_decorations.push_back(std::move(deco));
}

RectI WindowDecorationStack::computeTotalDamageBox(const RectI& clientBox) const noexcept {
    RectI total = clientBox;
    for (const auto& d : m_decorations) {
        if (!d) continue;
        const RectI db = d->damageBox(clientBox);
        if (db.isEmpty()) continue;
        const int x1 = std::min(total.x, db.x);
        const int y1 = std::min(total.y, db.y);
        const int x2 = std::max(total.x + total.width, db.x + db.width);
        const int y2 = std::max(total.y + total.height, db.y + db.height);
        total = RectI{x1, y1, x2 - x1, y2 - y1};
    }
    return total;
}

void WindowDecorationStack::enqueueLayer(
    RenderPass& pass,
    DecorationLayer targetLayer,
    const RectI& clientBox,
    float windowAlpha) const {
    for (const auto& d : m_decorations) {
        if (d && d->layer() == targetLayer) {
            d->enqueue(pass, clientBox, windowAlpha);
        }
    }
}

} // namespace nisaba::compositor

