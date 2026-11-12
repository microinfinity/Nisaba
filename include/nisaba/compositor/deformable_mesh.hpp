#pragma once

#include "nisaba/compositor/geometry.hpp"

#include <cstdint>
#include <vector>

namespace nisaba::compositor {

/**
 * @brief Interleaved 2D position and texture UV vertex for deformable mesh rendering.
 */
struct MeshVertex {
    float x{0.0f};
    float y{0.0f};
    float u{0.0f};
    float v{0.0f};
};

/**
 * @brief Spring-damper physics parameters for deformable window meshes.
 */
struct SpringMeshConfig {
    uint32_t columns{8};
    uint32_t rows{8};
    float stiffness{260.0f};   ///< Hooke spring constant k
    float damping{22.0f};      ///< Viscous damping coefficient c
    float anchorWeight{0.85f}; ///< Influence of grab anchor vs structural neighbors
    float restEpsilon{0.15f};  ///< Velocity/displacement threshold for rest detection
};

/**
 * @brief 2D Mass-Spring-Damper Deformable Mesh for interactive window wobble (jelly windows).
 */
class DeformableMesh {
public:
    explicit DeformableMesh(const SpringMeshConfig& config = {});
    ~DeformableMesh() = default;

    /**
     * @brief Snaps all mesh nodes immediately to the given screen-space rectangle at rest.
     */
    void resetToBox(const RectI& box) noexcept;

    /**
     * @brief Starts an interactive grab at normalized coordinates anchorUv in [0, 1]^2.
     */
    void grab(PointF anchorUv, const RectI& currentBox) noexcept;

    /**
     * @brief Updates the target window rectangle during drag/resize.
     */
    void setTargetBox(const RectI& newBox) noexcept;

    /**
     * @brief Releases the interactive grab, allowing all nodes to spring back to targetBox.
     */
    void release() noexcept;

    /**
     * @brief Advances the coupled mass-spring-damper system by dtSeconds (sub-stepped for stability).
     * @return true if the mesh is still actively deforming, false if settled at rest.
     */
    bool step(float dtSeconds) noexcept;

    [[nodiscard]] bool isGrabbed() const noexcept { return m_grabbed; }
    [[nodiscard]] bool isSettled() const noexcept { return m_settled && !m_grabbed; }
    [[nodiscard]] const RectI& targetBox() const noexcept { return m_targetBox; }

    /**
     * @brief Computes the axis-aligned bounding box enclosing all deformed mesh nodes.
     */
    [[nodiscard]] RectI deformedBounds() const noexcept;

    /**
     * @brief Generates the triangle list (6 vertices per grid cell) in screen-space coordinates.
     */
    [[nodiscard]] const std::vector<MeshVertex>& buildTriangleVertices();

private:
    struct Node {
        float x{0.0f};
        float y{0.0f};
        float vx{0.0f};
        float vy{0.0f};
        float u{0.0f};
        float v{0.0f};
    };

    SpringMeshConfig m_cfg;
    RectI m_targetBox{0, 0, 100, 100};
    PointF m_anchorUv{0.5, 0.0};
    bool m_grabbed{false};
    bool m_settled{true};
    std::vector<Node> m_nodes;
    std::vector<MeshVertex> m_triangleVerts;
};

} // namespace nisaba::compositor

