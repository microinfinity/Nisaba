#include "nisaba/mesh/mesh.hpp"
#include <algorithm>
#include <cmath>

namespace nisaba {

namespace {

inline Point eval_cubic_bezier(Point p0, Point p1, Point p2, Point p3, float t) noexcept {
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;

    float x = uuu * p0.x + 3.0f * uu * t * p1.x + 3.0f * u * tt * p2.x + ttt * p3.x;
    float y = uuu * p0.y + 3.0f * uu * t * p1.y + 3.0f * u * tt * p2.y + ttt * p3.y;
    return Point(x, y);
}

} // namespace

Vertices Vertices::create_grid(
    uint32_t cols, uint32_t rows,
    const std::vector<Point>& points,
    const std::vector<Color>* colors,
    const std::vector<Point>* tex_coords
) {
    Vertices v;
    v.mode = VertexMode::Triangles;
    v.positions = points;
    if (colors) v.colors = *colors;
    if (tex_coords) v.tex_coords = *tex_coords;

    if (cols < 2 || rows < 2 || points.size() < cols * rows) {
        return v;
    }

    v.indices.reserve((cols - 1) * (rows - 1) * 6);

    for (uint32_t r = 0; r < rows - 1; ++r) {
        for (uint32_t c = 0; c < cols - 1; ++c) {
            uint32_t tl = r * cols + c;
            uint32_t tr = tl + 1;
            uint32_t bl = (r + 1) * cols + c;
            uint32_t br = bl + 1;

            // First triangle (TL -> TR -> BL)
            v.indices.push_back(tl);
            v.indices.push_back(tr);
            v.indices.push_back(bl);

            // Second triangle (TR -> BR -> BL)
            v.indices.push_back(tr);
            v.indices.push_back(br);
            v.indices.push_back(bl);
        }
    }

    return v;
}

bool Vertices::is_valid() const noexcept {
    if (positions.size() < 3) return false;
    if (!colors.empty() && colors.size() != positions.size()) return false;
    if (!tex_coords.empty() && tex_coords.size() != positions.size()) return false;

    if (!indices.empty()) {
        size_t n = positions.size();
        for (uint32_t idx : indices) {
            if (idx >= n) return false;
        }
    }

    return triangle_count() > 0;
}

size_t Vertices::triangle_count() const noexcept {
    size_t count = indices.empty() ? positions.size() : indices.size();
    if (count < 3) return 0;

    switch (mode) {
        case VertexMode::Triangles:
            return count / 3;
        case VertexMode::TriangleStrip:
        case VertexMode::TriangleFan:
            return count - 2;
    }
    return 0;
}

std::tuple<size_t, size_t, size_t> Vertices::get_triangle_indices(size_t tri_idx) const noexcept {
    auto get_idx = [this](size_t i) -> size_t {
        if (!indices.empty()) {
            return indices[i];
        }
        return i;
    };

    switch (mode) {
        case VertexMode::Triangles:
            return {get_idx(tri_idx * 3), get_idx(tri_idx * 3 + 1), get_idx(tri_idx * 3 + 2)};

        case VertexMode::TriangleStrip:
            if (tri_idx % 2 == 0) {
                return {get_idx(tri_idx), get_idx(tri_idx + 1), get_idx(tri_idx + 2)};
            } else {
                // Swap first two vertices for odd triangles to maintain counter-clockwise / clockwise winding consistency
                return {get_idx(tri_idx + 1), get_idx(tri_idx), get_idx(tri_idx + 2)};
            }

        case VertexMode::TriangleFan:
            return {get_idx(0), get_idx(tri_idx + 1), get_idx(tri_idx + 2)};
    }

    return {0, 0, 0};
}

Rect Vertices::bounds() const noexcept {
    if (positions.empty()) return Rect();

    float min_x = positions[0].x;
    float max_x = positions[0].x;
    float min_y = positions[0].y;
    float max_y = positions[0].y;

    for (size_t i = 1; i < positions.size(); ++i) {
        min_x = std::min(min_x, positions[i].x);
        max_x = std::max(max_x, positions[i].x);
        min_y = std::min(min_y, positions[i].y);
        max_y = std::max(max_y, positions[i].y);
    }

    auto r = Rect::from_xywh(min_x, min_y, max_x - min_x, max_y - min_y);
    return r ? *r : Rect();
}

Point CoonsPatch::evaluate_position(float u, float v) const noexcept {
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    Point c_top = eval_cubic_bezier(top[0], top[1], top[2], top[3], u);
    Point c_bottom = eval_cubic_bezier(bottom[0], bottom[1], bottom[2], bottom[3], u);
    Point c_left = eval_cubic_bezier(left[0], left[1], left[2], left[3], v);
    Point c_right = eval_cubic_bezier(right[0], right[1], right[2], right[3], v);

    Point p00 = top[0];
    Point p10 = top[3];
    Point p01 = bottom[0];
    Point p11 = bottom[3];

    // Sc(u, v) = (1 - v) * C_top(u) + v * C_bottom(u)
    float sc_x = (1.0f - v) * c_top.x + v * c_bottom.x;
    float sc_y = (1.0f - v) * c_top.y + v * c_bottom.y;

    // Sd(u, v) = (1 - u) * C_left(v) + u * C_right(v)
    float sd_x = (1.0f - u) * c_left.x + u * c_right.x;
    float sd_y = (1.0f - u) * c_left.y + u * c_right.y;

    // Scd(u, v) = (1-u)(1-v) P00 + u(1-v) P10 + (1-u)v P01 + uv P11
    float scd_x = (1.0f - u) * (1.0f - v) * p00.x +
                  u * (1.0f - v) * p10.x +
                  (1.0f - u) * v * p01.x +
                  u * v * p11.x;

    float scd_y = (1.0f - u) * (1.0f - v) * p00.y +
                  u * (1.0f - v) * p10.y +
                  (1.0f - u) * v * p01.y +
                  u * v * p11.y;

    return Point(sc_x + sd_x - scd_x, sc_y + sd_y - scd_y);
}

Color CoonsPatch::evaluate_color(float u, float v) const noexcept {
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    float w00 = (1.0f - u) * (1.0f - v);
    float w10 = u * (1.0f - v);
    float w01 = (1.0f - u) * v;
    float w11 = u * v;

    float r = color_top_left.red()     * w00 + color_top_right.red()     * w10 +
              color_bottom_left.red()  * w01 + color_bottom_right.red()  * w11;

    float g = color_top_left.green()   * w00 + color_top_right.green()   * w10 +
              color_bottom_left.green()* w01 + color_bottom_right.green()* w11;

    float b = color_top_left.blue()    * w00 + color_top_right.blue()    * w10 +
              color_bottom_left.blue() * w01 + color_bottom_right.blue() * w11;

    float a = color_top_left.alpha()   * w00 + color_top_right.alpha()   * w10 +
              color_bottom_left.alpha()* w01 + color_bottom_right.alpha()* w11;

    return Color::from_rgba_unchecked(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f),
        std::clamp(a, 0.0f, 1.0f)
    );
}

std::pair<uint32_t, uint32_t> CoonsPatch::compute_adaptive_subdivision(
    const Transform& transform,
    float tolerance
) const noexcept {
    const float tol = std::max(0.05f, tolerance);
    auto curve_steps = [&](const Point pts[4]) noexcept -> uint32_t {
        Point p[4] = {pts[0], pts[1], pts[2], pts[3]};
        if (!transform.is_identity()) {
            transform.map_points(p, 4);
        }
        const float d1x = p[0].x - 2.0f * p[1].x + p[2].x;
        const float d1y = p[0].y - 2.0f * p[1].y + p[2].y;
        const float d2x = p[1].x - 2.0f * p[2].x + p[3].x;
        const float d2y = p[1].y - 2.0f * p[2].y + p[3].y;
        const float dev = std::max(std::hypot(d1x, d1y), std::hypot(d2x, d2y));
        const uint32_t steps = static_cast<uint32_t>(std::ceil(std::sqrt((0.75f * dev) / tol)));
        return std::clamp<uint32_t>(steps, 4u, 32u);
    };
    const uint32_t su = std::max(curve_steps(top), curve_steps(bottom));
    const uint32_t sv = std::max(curve_steps(left), curve_steps(right));
    return {su, sv};
}

Vertices CoonsPatch::to_vertices(uint32_t subdiv_u, uint32_t subdiv_v) const {
    subdiv_u = std::max(1u, subdiv_u);
    subdiv_v = std::max(1u, subdiv_v);

    uint32_t cols = subdiv_u + 1;
    uint32_t rows = subdiv_v + 1;

    std::vector<Point> pts;
    std::vector<Color> cols_vec;
    std::vector<Point> uvs;

    pts.reserve(cols * rows);
    cols_vec.reserve(cols * rows);
    uvs.reserve(cols * rows);

    for (uint32_t r = 0; r < rows; ++r) {
        float v = static_cast<float>(r) / static_cast<float>(subdiv_v);
        for (uint32_t c = 0; c < cols; ++c) {
            float u = static_cast<float>(c) / static_cast<float>(subdiv_u);

            pts.push_back(evaluate_position(u, v));
            cols_vec.push_back(evaluate_color(u, v));
            uvs.push_back(Point(u, v));
        }
    }

    return Vertices::create_grid(cols, rows, pts, &cols_vec, &uvs);
}

Vertices GradientMesh::to_vertices(uint32_t subdiv_per_patch) const {
    Vertices combined;
    combined.mode = VertexMode::Triangles;

    for (const auto& patch : patches) {
        Vertices patch_v = patch.to_vertices(subdiv_per_patch, subdiv_per_patch);
        uint32_t offset = static_cast<uint32_t>(combined.positions.size());

        combined.positions.insert(combined.positions.end(), patch_v.positions.begin(), patch_v.positions.end());
        combined.colors.insert(combined.colors.end(), patch_v.colors.begin(), patch_v.colors.end());
        combined.tex_coords.insert(combined.tex_coords.end(), patch_v.tex_coords.begin(), patch_v.tex_coords.end());

        for (uint32_t idx : patch_v.indices) {
            combined.indices.push_back(offset + idx);
        }
    }

    return combined;
}

Vertices GradientMesh::to_vertices_adaptive(const Transform& transform, float tolerance) const {
    Vertices combined;
    combined.mode = VertexMode::Triangles;

    for (const auto& patch : patches) {
        const auto [su, sv] = patch.compute_adaptive_subdivision(transform, tolerance);
        Vertices patch_v = patch.to_vertices(su, sv);
        const uint32_t offset = static_cast<uint32_t>(combined.positions.size());

        combined.positions.insert(combined.positions.end(), patch_v.positions.begin(), patch_v.positions.end());
        combined.colors.insert(combined.colors.end(), patch_v.colors.begin(), patch_v.colors.end());
        combined.tex_coords.insert(combined.tex_coords.end(), patch_v.tex_coords.begin(), patch_v.tex_coords.end());

        for (uint32_t idx : patch_v.indices) {
            combined.indices.push_back(offset + idx);
        }
    }

    return combined;
}

} // namespace nisaba
