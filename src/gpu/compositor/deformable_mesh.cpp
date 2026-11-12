#include "nisaba/compositor/deformable_mesh.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::compositor {

DeformableMesh::DeformableMesh(const SpringMeshConfig& config)
    : m_cfg(config) {
    m_cfg.columns = std::max(m_cfg.columns, 2u);
    m_cfg.rows = std::max(m_cfg.rows, 2u);

    const uint32_t cols1 = m_cfg.columns + 1;
    const uint32_t rows1 = m_cfg.rows + 1;
    m_nodes.resize(static_cast<size_t>(cols1) * rows1);

    for (uint32_t r = 0; r < rows1; ++r) {
        for (uint32_t c = 0; c < cols1; ++c) {
            auto& node = m_nodes[static_cast<size_t>(r) * cols1 + c];
            node.u = static_cast<float>(c) / static_cast<float>(m_cfg.columns);
            node.v = static_cast<float>(r) / static_cast<float>(m_cfg.rows);
        }
    }
    resetToBox(m_targetBox);
}

void DeformableMesh::resetToBox(const RectI& box) noexcept {
    m_targetBox = box;
    for (auto& n : m_nodes) {
        n.x = static_cast<float>(box.x) + n.u * static_cast<float>(box.width);
        n.y = static_cast<float>(box.y) + n.v * static_cast<float>(box.height);
        n.vx = 0.0f;
        n.vy = 0.0f;
    }
    m_settled = true;
}

void DeformableMesh::grab(PointF anchorUv, const RectI& currentBox) noexcept {
    m_anchorUv.x = std::clamp(anchorUv.x, 0.0, 1.0);
    m_anchorUv.y = std::clamp(anchorUv.y, 0.0, 1.0);
    if (m_settled) {
        resetToBox(currentBox);
    }
    m_targetBox = currentBox;
    m_grabbed = true;
    m_settled = false;
}

void DeformableMesh::setTargetBox(const RectI& newBox) noexcept {
    if (newBox == m_targetBox) return;
    m_targetBox = newBox;
    m_settled = false;
}

void DeformableMesh::release() noexcept {
    m_grabbed = false;
    m_settled = false;
}

bool DeformableMesh::step(float dtSeconds) noexcept {
    if (m_settled && !m_grabbed) return false;

    dtSeconds = std::clamp(dtSeconds, 0.0005f, 0.05f);
    constexpr float kMaxSubStep = 0.0035f;
    const int steps = std::max(1, static_cast<int>(std::ceil(dtSeconds / kMaxSubStep)));
    const float h = dtSeconds / static_cast<float>(steps);

    const uint32_t cols = m_cfg.columns;
    const uint32_t rows = m_cfg.rows;
    const uint32_t cols1 = cols + 1;
    const float cellW = static_cast<float>(m_targetBox.width) / static_cast<float>(cols);
    const float cellH = static_cast<float>(m_targetBox.height) / static_cast<float>(rows);

    float maxEnergy = 0.0f;

    for (int s = 0; s < steps; ++s) {
        maxEnergy = 0.0f;
        for (uint32_t r = 0; r <= rows; ++r) {
            for (uint32_t c = 0; c <= cols; ++c) {
                auto& node = m_nodes[static_cast<size_t>(r) * cols1 + c];

                const float restX = static_cast<float>(m_targetBox.x) + node.u * static_cast<float>(m_targetBox.width);
                const float restY = static_cast<float>(m_targetBox.y) + node.v * static_cast<float>(m_targetBox.height);

                // Distance from grab anchor modulates how tightly the node tracks the cursor directly
                const float du = node.u - static_cast<float>(m_anchorUv.x);
                const float dv = node.v - static_cast<float>(m_anchorUv.y);
                const float distSq = du * du + dv * dv;
                const float anchorStiffness = m_grabbed
                    ? m_cfg.stiffness * (0.22f + 1.8f * std::exp(-distSq * 6.5f))
                    : m_cfg.stiffness * 1.15f;

                float fx = (restX - node.x) * anchorStiffness;
                float fy = (restY - node.y) * anchorStiffness;

                // Structural neighbor springs (left, right, up, down)
                const float kStruct = m_cfg.stiffness * 0.65f;
                if (c > 0) {
                    const auto& left = m_nodes[static_cast<size_t>(r) * cols1 + (c - 1)];
                    fx += ((left.x + cellW) - node.x) * kStruct;
                    fy += (left.y - node.y) * kStruct;
                }
                if (c < cols) {
                    const auto& right = m_nodes[static_cast<size_t>(r) * cols1 + (c + 1)];
                    fx += ((right.x - cellW) - node.x) * kStruct;
                    fy += (right.y - node.y) * kStruct;
                }
                if (r > 0) {
                    const auto& up = m_nodes[static_cast<size_t>(r - 1) * cols1 + c];
                    fx += (up.x - node.x) * kStruct;
                    fy += ((up.y + cellH) - node.y) * kStruct;
                }
                if (r < rows) {
                    const auto& down = m_nodes[static_cast<size_t>(r + 1) * cols1 + c];
                    fx += (down.x - node.x) * kStruct;
                    fy += ((down.y - cellH) - node.y) * kStruct;
                }

                // Viscous damping
                fx -= node.vx * m_cfg.damping;
                fy -= node.vy * m_cfg.damping;

                node.vx += fx * h;
                node.vy += fy * h;
                node.x += node.vx * h;
                node.y += node.vy * h;

                const float disp = std::abs(restX - node.x) + std::abs(restY - node.y);
                const float vel = std::abs(node.vx) + std::abs(node.vy);
                maxEnergy = std::max(maxEnergy, disp + vel * 0.05f);
            }
        }
    }

    if (!m_grabbed && maxEnergy < m_cfg.restEpsilon) {
        resetToBox(m_targetBox);
        return false;
    }

    return true;
}

RectI DeformableMesh::deformedBounds() const noexcept {
    if (m_nodes.empty()) return m_targetBox;

    float minX = m_nodes[0].x;
    float minY = m_nodes[0].y;
    float maxX = m_nodes[0].x;
    float maxY = m_nodes[0].y;

    for (const auto& n : m_nodes) {
        minX = std::min(minX, n.x);
        minY = std::min(minY, n.y);
        maxX = std::max(maxX, n.x);
        maxY = std::max(maxY, n.y);
    }

    const int x1 = static_cast<int>(std::floor(minX)) - 1;
    const int y1 = static_cast<int>(std::floor(minY)) - 1;
    const int x2 = static_cast<int>(std::ceil(maxX)) + 1;
    const int y2 = static_cast<int>(std::ceil(maxY)) + 1;
    return RectI{x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

const std::vector<MeshVertex>& DeformableMesh::buildTriangleVertices() {
    const uint32_t cols = m_cfg.columns;
    const uint32_t rows = m_cfg.rows;
    const uint32_t cols1 = cols + 1;

    m_triangleVerts.clear();
    m_triangleVerts.reserve(static_cast<size_t>(cols) * rows * 6);

    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            const auto& n00 = m_nodes[static_cast<size_t>(r) * cols1 + c];
            const auto& n10 = m_nodes[static_cast<size_t>(r) * cols1 + (c + 1)];
            const auto& n01 = m_nodes[static_cast<size_t>(r + 1) * cols1 + c];
            const auto& n11 = m_nodes[static_cast<size_t>(r + 1) * cols1 + (c + 1)];

            // Triangle 1: n00 -> n10 -> n01
            m_triangleVerts.push_back(MeshVertex{n00.x, n00.y, n00.u, n00.v});
            m_triangleVerts.push_back(MeshVertex{n10.x, n10.y, n10.u, n10.v});
            m_triangleVerts.push_back(MeshVertex{n01.x, n01.y, n01.u, n01.v});

            // Triangle 2: n10 -> n11 -> n01
            m_triangleVerts.push_back(MeshVertex{n10.x, n10.y, n10.u, n10.v});
            m_triangleVerts.push_back(MeshVertex{n11.x, n11.y, n11.u, n11.v});
            m_triangleVerts.push_back(MeshVertex{n01.x, n01.y, n01.u, n01.v});
        }
    }

    return m_triangleVerts;
}

} // namespace nisaba::compositor

