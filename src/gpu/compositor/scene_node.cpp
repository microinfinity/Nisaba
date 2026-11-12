#include "nisaba/compositor/scene_node.hpp"
#include "nisaba/compositor/pass_element.hpp"
#include "nisaba/compositor/scene_tree.hpp"

namespace nisaba::compositor {

PointI SceneNode::absolutePosition() const noexcept {
    PointI pos{m_x, m_y};
    const SceneTree* p = m_parent;
    while (p) {
        pos.x += p->x();
        pos.y += p->y();
        p = p->parent();
    }
    return pos;
}

// ==========================================
// SceneRect Implementation
// ==========================================

std::shared_ptr<SceneRect> SceneRect::create(int width, int height, const Color& color) {
    return std::make_shared<SceneRect>(width, height, color);
}

SceneRect::SceneRect(int width, int height, const Color& color)
    : SceneNode(SceneNodeType::Rect),
      m_width(width),
      m_height(height),
      m_color(color) {}

void SceneRect::render(
    IRenderPass& pass,
    const PointI& parentOffset,
    const Region* damage) const {
    if (!m_enabled || m_width <= 0 || m_height <= 0) return;

    RectI box{parentOffset.x + m_x, parentOffset.y + m_y, m_width, m_height};
    if (m_cornerRadius > 0.5f) {
        if (auto* rp = dynamic_cast<RenderPass*>(&pass)) {
            rp->renderRoundedRect(box, m_color, m_cornerRadius, m_roundingPower, damage);
            return;
        }
    }
    pass.renderRect(box, m_color, damage);
}

void SceneRect::enqueueToPass(
    RenderPass& pass,
    const PointI& parentOffset,
    const ImageDescription& /*outputDesc*/) const {
    if (!m_enabled || m_width <= 0 || m_height <= 0) return;

    RectI box{parentOffset.x + m_x, parentOffset.y + m_y, m_width, m_height};
    pass.enqueue(std::make_unique<RectPassElement>(
        RectPassElement::Data{box, m_color, m_cornerRadius, m_roundingPower}));
}

SceneNode* SceneRect::nodeAt(const PointI& rootPoint) {
    if (!m_enabled || m_width <= 0 || m_height <= 0) return nullptr;

    auto absPos = absolutePosition();
    RectI box{absPos.x, absPos.y, m_width, m_height};
    if (box.contains(rootPoint)) {
        return this;
    }
    return nullptr;
}

// ==========================================
// SceneBuffer Implementation
// ==========================================

std::shared_ptr<SceneBuffer> SceneBuffer::create(
    std::shared_ptr<Texture> texture,
    int width,
    int height) {
    if (texture) {
        if (width <= 0) width = texture->width();
        if (height <= 0) height = texture->height();
    }
    return std::make_shared<SceneBuffer>(std::move(texture), width, height);
}

SceneBuffer::SceneBuffer(std::shared_ptr<Texture> texture, int width, int height)
    : SceneNode(SceneNodeType::Buffer),
      m_texture(std::move(texture)),
      m_width(width),
      m_height(height) {}

void SceneBuffer::setTexture(std::shared_ptr<Texture> texture, int width, int height) {
    m_texture = std::move(texture);
    if (m_texture) {
        if (width <= 0) m_width = m_texture->width();
        else m_width = width;

        if (height <= 0) m_height = m_texture->height();
        else m_height = height;
    } else {
        m_width = width;
        m_height = height;
    }
}

void SceneBuffer::render(
    IRenderPass& pass,
    const PointI& parentOffset,
    const Region* damage) const {
    if (!m_enabled || !m_texture || m_width <= 0 || m_height <= 0) return;

    RectI box{parentOffset.x + m_x, parentOffset.y + m_y, m_width, m_height};
    if (auto* rp = dynamic_cast<RenderPass*>(&pass)) {
        if (m_cornerRadius > 0.5f || !m_imageDesc.isStandardSrgb() || !m_decorations.empty()) {
            m_decorations.enqueueLayer(*rp, DecorationLayer::Bottom, box, m_alpha);
            m_decorations.enqueueLayer(*rp, DecorationLayer::Under, box, m_alpha);

            SurfaceRenderParams params;
            params.texture = m_texture.get();
            params.destBox = box;
            params.srcBox = m_srcBox;
            params.alpha = m_alpha;
            params.transform = m_transform;
            params.cornerRadius = m_cornerRadius;
            params.roundingPower = m_roundingPower;
            params.colorTransform = ColorManagement::buildTransform(m_imageDesc, ImageDescription::srgbDefault());
            rp->renderSurface(params, damage);

            m_decorations.enqueueLayer(*rp, DecorationLayer::Over, box, m_alpha);
            m_decorations.enqueueLayer(*rp, DecorationLayer::Overlay, box, m_alpha);
            return;
        }
    }
    pass.renderTexture(*m_texture, box, m_srcBox, m_alpha, m_transform, damage);
}

void SceneBuffer::enqueueToPass(
    RenderPass& pass,
    const PointI& parentOffset,
    const ImageDescription& outputDesc) const {
    if (!m_enabled || !m_texture || m_width <= 0 || m_height <= 0) return;

    RectI box{parentOffset.x + m_x, parentOffset.y + m_y, m_width, m_height};

    // 1. Enqueue Bottom & Under window decorations (DropShadow, Underlay)
    m_decorations.enqueueLayer(pass, DecorationLayer::Bottom, box, m_alpha);
    m_decorations.enqueueLayer(pass, DecorationLayer::Under, box, m_alpha);

    // 2. Apply Window Transformers (wobble / motion blur / custom transform)
    WindowTransformState tfState;
    tfState.texture = m_texture.get();
    tfState.logicalBox = box;
    tfState.alpha = m_alpha;
    tfState.cornerRadius = m_cornerRadius;
    tfState.roundingPower = m_roundingPower;
    const_cast<TransformerChain&>(m_transformers).applyAll(tfState);

    SurfaceRenderParams params;
    params.texture = m_texture.get();
    params.destBox = tfState.logicalBox;
    params.srcBox = m_srcBox;
    params.alpha = tfState.alpha;
    params.transform = m_transform;
    params.cornerRadius = tfState.cornerRadius;
    params.roundingPower = tfState.roundingPower;
    params.motionBlur = tfState.motionBlur;
    params.customMesh = tfState.customMesh;
    params.colorTransform = ColorManagement::buildTransform(m_imageDesc, outputDesc);

    pass.enqueue(std::make_unique<SurfacePassElement>(std::move(params)));

    // 3. Enqueue Over & Overlay window decorations (Border, InnerGlow, GroupBar)
    m_decorations.enqueueLayer(pass, DecorationLayer::Over, box, m_alpha);
    m_decorations.enqueueLayer(pass, DecorationLayer::Overlay, box, m_alpha);
}

SceneNode* SceneBuffer::nodeAt(const PointI& rootPoint) {
    if (!m_enabled || m_width <= 0 || m_height <= 0) return nullptr;

    auto absPos = absolutePosition();
    RectI box{absPos.x, absPos.y, m_width, m_height};
    if (box.contains(rootPoint)) {
        return this;
    }
    return nullptr;
}

} // namespace nisaba::compositor
