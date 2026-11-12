#include "nisaba/compositor/scene_tree.hpp"

#include <algorithm>

namespace nisaba::compositor {

std::shared_ptr<SceneTree> SceneTree::create(SceneTree* parent) {
    auto tree = std::make_shared<SceneTree>();
    if (parent) {
        parent->addChild(tree);
    }
    return tree;
}

SceneTree::SceneTree()
    : SceneNode(SceneNodeType::Tree) {}

std::shared_ptr<SceneTree> SceneTree::createChildTree() {
    return SceneTree::create(this);
}

std::shared_ptr<SceneRect> SceneTree::createChildRect(int width, int height, const Color& color) {
    auto rect = SceneRect::create(width, height, color);
    addChild(rect);
    return rect;
}

std::shared_ptr<SceneBuffer> SceneTree::createChildBuffer(
    std::shared_ptr<Texture> texture,
    int width,
    int height) {
    auto buf = SceneBuffer::create(std::move(texture), width, height);
    addChild(buf);
    return buf;
}

void SceneTree::addChild(std::shared_ptr<SceneNode> child) {
    if (!child) return;
    child->setParent(this);
    m_children.push_back(std::move(child));
}

void SceneTree::removeChild(SceneNode* child) {
    if (!child) return;
    auto it = std::find_if(m_children.begin(), m_children.end(), [child](const auto& c) {
        return c.get() == child;
    });

    if (it != m_children.end()) {
        (*it)->setParent(nullptr);
        m_children.erase(it);
    }
}

void SceneTree::raiseToTop(SceneNode* child) {
    if (!child) return;
    auto it = std::find_if(m_children.begin(), m_children.end(), [child](const auto& c) {
        return c.get() == child;
    });

    if (it != m_children.end() && it != m_children.end() - 1) {
        std::rotate(it, it + 1, m_children.end());
    }
}

void SceneTree::lowerToBottom(SceneNode* child) {
    if (!child) return;
    auto it = std::find_if(m_children.begin(), m_children.end(), [child](const auto& c) {
        return c.get() == child;
    });

    if (it != m_children.end() && it != m_children.begin()) {
        std::rotate(m_children.begin(), it, it + 1);
    }
}

void SceneTree::render(
    IRenderPass& pass,
    const PointI& parentOffset,
    const Region* damage) const {
    if (!m_enabled) return;

    const PointI currentOffset{parentOffset.x + m_x, parentOffset.y + m_y};

    for (const auto& child : m_children) {
        if (child && child->isEnabled()) {
            child->render(pass, currentOffset, damage);
        }
    }
}

void SceneTree::enqueueToPass(
    RenderPass& pass,
    const PointI& parentOffset,
    const ImageDescription& outputDesc) const {
    if (!m_enabled) return;

    const PointI currentOffset{parentOffset.x + m_x, parentOffset.y + m_y};

    for (const auto& child : m_children) {
        if (child && child->isEnabled()) {
            child->enqueueToPass(pass, currentOffset, outputDesc);
        }
    }
}

PassExecutionStats SceneTree::renderCulled(
    RenderPass& pass,
    const Region& damage,
    const ImageDescription& outputDesc) const {
    enqueueToPass(pass, PointI{0, 0}, outputDesc);
    return pass.optimizeAndExecute(damage);
}

SceneNode* SceneTree::nodeAt(const PointI& rootPoint) {
    if (!m_enabled) return nullptr;

    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        if (*it && (*it)->isEnabled()) {
            SceneNode* hit = (*it)->nodeAt(rootPoint);
            if (hit) {
                return hit;
            }
        }
    }

    return nullptr;
}

SceneBuffer* SceneTree::findDirectScanoutCandidate(int targetWidth, int targetHeight) const {
    if (!m_enabled || targetWidth <= 0 || targetHeight <= 0) {
        return nullptr;
    }

    struct VisibleLeaf {
        const SceneNode* node;
        int x;
        int y;
    };

    std::vector<VisibleLeaf> leafs;

    auto collectLeafs = [&](auto& self, const SceneTree* tree, int curX, int curY) -> void {
        if (!tree || !tree->isEnabled()) return;
        for (const auto& child : tree->children()) {
            if (!child || !child->isEnabled()) continue;
            int childX = curX + child->x();
            int childY = curY + child->y();
            if (child->type() == SceneNodeType::Tree) {
                self(self, static_cast<const SceneTree*>(child.get()), childX, childY);
            } else {
                leafs.push_back({child.get(), childX, childY});
            }
        }
    };

    collectLeafs(collectLeafs, this, m_x, m_y);

    if (leafs.empty()) {
        return nullptr;
    }

    for (int i = static_cast<int>(leafs.size()) - 1; i >= 0; --i) {
        const auto& item = leafs[i];
        if (item.node->type() == SceneNodeType::Buffer) {
            const auto* buf = static_cast<const SceneBuffer*>(item.node);
            if (item.x == 0 && item.y == 0 &&
                buf->width() == targetWidth && buf->height() == targetHeight &&
                buf->alpha() >= 0.999f &&
                buf->cornerRadius() <= 0.5f &&
                !buf->transformers().blocksDirectScanout() &&
                buf->imageDescription().isStandardSrgb() &&
                buf->transform() == WL_OUTPUT_TRANSFORM_NORMAL &&
                buf->texture() != nullptr) {

                const auto& src = buf->sourceBox();
                if (src.x <= 0.001 && src.y <= 0.001 &&
                    src.width >= 0.999 && src.height >= 0.999) {

                    bool occludedOrOverlaid = false;
                    for (size_t j = static_cast<size_t>(i + 1); j < leafs.size(); ++j) {
                        const auto& above = leafs[j];
                        if (above.node->type() == SceneNodeType::Rect) {
                            const auto* r = static_cast<const SceneRect*>(above.node);
                            if (r->width() > 0 && r->height() > 0 && r->color().a > 0.001f) {
                                occludedOrOverlaid = true;
                                break;
                            }
                        } else if (above.node->type() == SceneNodeType::Buffer) {
                            const auto* b = static_cast<const SceneBuffer*>(above.node);
                            if (b->width() > 0 && b->height() > 0 && b->alpha() > 0.001f) {
                                occludedOrOverlaid = true;
                                break;
                            }
                        } else {
                            occludedOrOverlaid = true;
                            break;
                        }
                    }

                    if (!occludedOrOverlaid) {
                        return const_cast<SceneBuffer*>(buf);
                    }
                }
            }
        }
    }

    return nullptr;
}

} // namespace nisaba::compositor
