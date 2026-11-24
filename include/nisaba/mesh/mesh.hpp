#pragma once

#include <vector>
#include <optional>
#include <tuple>
#include <utility>
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/color/color.hpp"

namespace nisaba {

/// Vertex layout modes for 2D mesh assembly.
enum class VertexMode {
    /// Every 3 consecutive vertices (or indices) define an independent triangle.
    Triangles,
    /// Vertices form a connected strip of triangles sharing edges.
    TriangleStrip,
    /// Vertices radiate outwards from a central origin vertex.
    TriangleFan
};

/// A 2D geometric mesh container holding vertices, per-vertex colors,
/// texture UV coordinates, and optional index buffers.
struct Vertices {
    VertexMode mode{VertexMode::Triangles};
    std::vector<Point> positions{};
    std::vector<Color> colors{};       // Optional per-vertex colors for Gouraud shading
    std::vector<Point> tex_coords{};   // Optional UV texture coordinates
    std::vector<uint32_t> indices{};   // Optional index buffer

    constexpr Vertices() noexcept = default;

    static Vertices create_triangles(
        std::vector<Point> positions,
        std::vector<Color> colors = {},
        std::vector<Point> tex_coords = {},
        std::vector<uint32_t> indices = {}
    ) {
        Vertices v;
        v.mode = VertexMode::Triangles;
        v.positions = std::move(positions);
        v.colors = std::move(colors);
        v.tex_coords = std::move(tex_coords);
        v.indices = std::move(indices);
        return v;
    }

    static Vertices create_triangle_strip(
        std::vector<Point> positions,
        std::vector<Color> colors = {},
        std::vector<Point> tex_coords = {},
        std::vector<uint32_t> indices = {}
    ) {
        Vertices v;
        v.mode = VertexMode::TriangleStrip;
        v.positions = std::move(positions);
        v.colors = std::move(colors);
        v.tex_coords = std::move(tex_coords);
        v.indices = std::move(indices);
        return v;
    }

    static Vertices create_triangle_fan(
        std::vector<Point> positions,
        std::vector<Color> colors = {},
        std::vector<Point> tex_coords = {},
        std::vector<uint32_t> indices = {}
    ) {
        Vertices v;
        v.mode = VertexMode::TriangleFan;
        v.positions = std::move(positions);
        v.colors = std::move(colors);
        v.tex_coords = std::move(tex_coords);
        v.indices = std::move(indices);
        return v;
    }

    /// Helper that constructs a structured 2D deformation grid of (cols x rows) vertices,
    /// tessellated into indexed triangles.
    static Vertices create_grid(
        uint32_t cols, uint32_t rows,
        const std::vector<Point>& points,
        const std::vector<Color>* colors = nullptr,
        const std::vector<Point>* tex_coords = nullptr
    );

    [[nodiscard]] bool is_valid() const noexcept;

    [[nodiscard]] size_t triangle_count() const noexcept;

    [[nodiscard]] std::tuple<size_t, size_t, size_t> get_triangle_indices(size_t tri_idx) const noexcept;

    [[nodiscard]] Rect bounds() const noexcept;
};

/// A bicubic Coons Patch for photorealistic 2D Freeform Gradient Meshes.
/// Defined by 4 cubic Bézier boundary curves and 4 corner colors.
struct CoonsPatch {
    // 12 boundary control points:
    // Top boundary:    top[0] (TL), top[1], top[2], top[3] (TR)
    // Right boundary:  right[0] (TR), right[1], right[2], right[3] (BR)
    // Bottom boundary: bottom[0] (BL), bottom[1], bottom[2], bottom[3] (BR)
    // Left boundary:   left[0] (TL), left[1], left[2], left[3] (BL)
    Point top[4];
    Point right[4];
    Point bottom[4];
    Point left[4];

    // Corner colors:
    Color color_top_left{Color::TRANSPARENT};
    Color color_top_right{Color::TRANSPARENT};
    Color color_bottom_right{Color::TRANSPARENT};
    Color color_bottom_left{Color::TRANSPARENT};

    /// Evaluates the Coons surface position at parametric coordinates (u, v) in [0, 1]^2.
    [[nodiscard]] Point evaluate_position(float u, float v) const noexcept;

    /// Evaluates the interpolated color at (u, v) in [0, 1]^2.
    [[nodiscard]] Color evaluate_color(float u, float v) const noexcept;

    /// Computes adaptive (u, v) tessellation subdivisions based on cubic boundary curvature in device space.
    [[nodiscard]] std::pair<uint32_t, uint32_t> compute_adaptive_subdivision(
        const Transform& transform = Transform(),
        float tolerance = 0.5f
    ) const noexcept;

    /// Tessellates this patch into a fine 2D triangle mesh (Vertices) with Gouraud vertex colors.
    [[nodiscard]] Vertices to_vertices(uint32_t subdiv_u = 8, uint32_t subdiv_v = 8) const;
};

/// A collection of interconnected Coons patches forming a complex vector gradient mesh.
struct GradientMesh {
    std::vector<CoonsPatch> patches{};

    void add_patch(const CoonsPatch& patch) {
        patches.push_back(patch);
    }

    /// Tessellates all patches in this mesh into an indexed Vertices object.
    [[nodiscard]] Vertices to_vertices(uint32_t subdiv_per_patch = 8) const;

    /// Tessellates all patches adaptively according to their curvature under `transform`.
    [[nodiscard]] Vertices to_vertices_adaptive(
        const Transform& transform = Transform(),
        float tolerance = 0.5f
    ) const;
};

} // namespace nisaba
