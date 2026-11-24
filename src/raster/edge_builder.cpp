#include "nisaba/raster/edge_builder.hpp"
#include "nisaba/raster/edge_clipper.hpp"
#include "nisaba/path/path_geometry.hpp"
#include <algorithm>

namespace nisaba {

std::optional<PathEdge> PathEdgeIter::close_line() noexcept {
    needs_close_line_ = false;
    return PathEdge::line(path_.points()[points_index_ - 1], move_to_);
}

std::optional<PathEdge> PathEdgeIter::next() noexcept {
    while (verb_index_ < path_.verbs().size()) {
        PathVerb verb = path_.verbs()[verb_index_++];
        switch (verb) {
            case PathVerb::Move: {
                if (needs_close_line_) {
                    auto res = close_line();
                    move_to_ = path_.points()[points_index_++];
                    return res;
                }
                move_to_ = path_.points()[points_index_++];
                break;
            }
            case PathVerb::Line: {
                needs_close_line_ = true;
                Point p0 = path_.points()[points_index_ - 1];
                Point p1 = path_.points()[points_index_];
                points_index_ += 1;
                return PathEdge::line(p0, p1);
            }
            case PathVerb::Quad: {
                needs_close_line_ = true;
                Point p0 = path_.points()[points_index_ - 1];
                Point p1 = path_.points()[points_index_];
                Point p2 = path_.points()[points_index_ + 1];
                points_index_ += 2;
                return PathEdge::quad(p0, p1, p2);
            }
            case PathVerb::Cubic: {
                needs_close_line_ = true;
                Point p0 = path_.points()[points_index_ - 1];
                Point p1 = path_.points()[points_index_];
                Point p2 = path_.points()[points_index_ + 1];
                Point p3 = path_.points()[points_index_ + 2];
                points_index_ += 3;
                return PathEdge::cubic(p0, p1, p2, p3);
            }
            case PathVerb::Close: {
                if (needs_close_line_) {
                    return close_line();
                }
                break;
            }
        }
    }

    if (needs_close_line_) {
        return close_line();
    }
    return std::nullopt;
}

namespace {

enum class Combine { No, Partial, Total };

inline Combine combine_vertical(const LineEdge& edge, LineEdge& last) noexcept {
    if (last.dx != 0 || edge.x != last.x) {
        return Combine::No;
    }

    if (edge.winding == last.winding) {
        if (edge.last_y + 1 == last.first_y) {
            last.first_y = edge.first_y;
            return Combine::Partial;
        } else if (edge.first_y == last.last_y + 1) {
            last.last_y = edge.last_y;
            return Combine::Partial;
        } else {
            return Combine::No;
        }
    }

    if (edge.first_y == last.first_y) {
        if (edge.last_y == last.last_y) {
            return Combine::Total;
        } else if (edge.last_y < last.last_y) {
            last.first_y = edge.last_y + 1;
            return Combine::Partial;
        } else {
            last.first_y = last.last_y + 1;
            last.last_y = edge.last_y;
            last.winding = edge.winding;
            return Combine::Partial;
        }
    }

    if (edge.last_y == last.last_y) {
        if (edge.first_y > last.first_y) {
            last.last_y = edge.first_y - 1;
        } else {
            last.last_y = last.first_y - 1;
            last.first_y = edge.first_y;
            last.winding = edge.winding;
        }
        return Combine::Partial;
    }

    return Combine::No;
}

} // namespace

void BasicEdgeBuilder::push_line(Point p0, Point p1) {
    auto edge = LineEdge::create(p0, p1, clip_shift_);
    if (!edge) return;

    Combine combine = Combine::No;
    if (edge->is_vertical() && !edges_.empty()) {
        if (edges_.back().type() == Edge::Type::Line) {
            combine = combine_vertical(*edge, edges_.back().as_line_mut());
        }
    }

    switch (combine) {
        case Combine::Total:
            edges_.pop_back();
            break;
        case Combine::Partial:
            break;
        case Combine::No:
            edges_.push_back(Edge(*edge));
            break;
    }
}

void BasicEdgeBuilder::push_quad(const Point* points) {
    float ux = points[0].x - 2.0f * points[1].x + points[2].x;
    float uy = points[0].y - 2.0f * points[1].y + points[2].y;
    float max_d = std::max(std::abs(ux), std::abs(uy));
    if (max_d < 0.125f) {
        push_line(points[0], points[2]);
        return;
    }
    int32_t n = static_cast<int32_t>(std::ceil(std::sqrt(2.0f * max_d)));
    n = std::clamp(n, 2, 64);

    path_geometry::QuadCoeff coeff = path_geometry::QuadCoeff::from_points(points);
    float dt = 1.0f / static_cast<float>(n);
    Point prev = points[0];
    for (int32_t i = 1; i < n; ++i) {
        float t = static_cast<float>(i) * dt;
        f32x2 pt = coeff.eval(f32x2::splat(t));
        Point cur = Point::from_xy(pt.x(), pt.y());
        push_line(prev, cur);
        prev = cur;
    }
    push_line(prev, points[2]);
}

void BasicEdgeBuilder::push_cubic(const Point* points) {
    float u1x = points[0].x - 2.0f * points[1].x + points[2].x;
    float u1y = points[0].y - 2.0f * points[1].y + points[2].y;
    float u2x = points[1].x - 2.0f * points[2].x + points[3].x;
    float u2y = points[1].y - 2.0f * points[2].y + points[3].y;
    float max_d = std::max({std::abs(u1x), std::abs(u1y), std::abs(u2x), std::abs(u2y)});
    if (max_d < 0.125f) {
        push_line(points[0], points[3]);
        return;
    }
    int32_t n = static_cast<int32_t>(std::ceil(std::sqrt(6.0f * max_d)));
    n = std::clamp(n, 2, 64);

    path_geometry::CubicCoeff coeff = path_geometry::CubicCoeff::from_points(points);
    float dt = 1.0f / static_cast<float>(n);
    Point prev = points[0];
    for (int32_t i = 1; i < n; ++i) {
        float t = static_cast<float>(i) * dt;
        f32x2 pt = coeff.eval(f32x2::splat(t));
        Point cur = Point::from_xy(pt.x(), pt.y());
        push_line(prev, cur);
        prev = cur;
    }
    push_line(prev, points[3]);
}

bool BasicEdgeBuilder::build(
    const Path& path,
    const ShiftedIntRect* clip,
    bool can_cull_to_the_right
) {
    size_t estimated = path.verbs().size() * 3 + 16;
    if (edges_.capacity() < estimated) {
        edges_.reserve(estimated);
    }

    if (clip != nullptr) {
        Rect clip_rect = clip->recover().to_rect();
        EdgeClipperIter iter(path, clip_rect, can_cull_to_the_right);
        while (auto edge = iter.next()) {
            switch (edge->type) {
                case PathEdge::Type::LineTo:
                    if (!edge->points[0].is_finite() || !edge->points[1].is_finite()) return false;
                    push_line(edge->points[0], edge->points[1]);
                    break;
                case PathEdge::Type::QuadTo:
                    if (!edge->points[0].is_finite() || !edge->points[1].is_finite() || !edge->points[2].is_finite()) return false;
                    push_quad(edge->points.data());
                    break;
                case PathEdge::Type::CubicTo:
                    if (!edge->points[0].is_finite() || !edge->points[1].is_finite() || !edge->points[2].is_finite() || !edge->points[3].is_finite()) return false;
                    push_cubic(edge->points.data());
                    break;
            }
        }
    } else {
        PathEdgeIter iter(path);
        while (auto edge = iter.next()) {
            switch (edge->type) {
                case PathEdge::Type::LineTo:
                    push_line(edge->points[0], edge->points[1]);
                    break;
                case PathEdge::Type::QuadTo: {
                    std::array<Point, 5> mono_x{};
                    size_t n = path_geometry::chop_quad_at_y_extrema(edge->points.data(), mono_x.data());
                    for (size_t i = 0; i <= n; ++i) {
                        push_quad(&mono_x[i * 2]);
                    }
                    break;
                }
                case PathEdge::Type::CubicTo: {
                    std::array<Point, 10> mono_y{};
                    size_t n = path_geometry::chop_cubic_at_y_extrema(edge->points.data(), mono_y.data());
                    for (size_t i = 0; i <= n; ++i) {
                        push_cubic(&mono_y[i * 3]);
                    }
                    break;
                }
            }
        }
    }

    return true;
}

std::optional<std::vector<Edge>*> BasicEdgeBuilder::build_edges_inplace(
    const Path& path,
    const ShiftedIntRect* clip,
    int32_t clip_shift
) {
    bool can_cull_to_the_right = false;

    // Reuse a thread-local builder to avoid heap allocation on every fill_path
    static thread_local BasicEdgeBuilder tl_builder(clip_shift);
    tl_builder.clip_shift_ = clip_shift;
    tl_builder.edges_.clear();

    // Head sentinel pre-allocated at index 0
    LineEdge head{};
    head.prev = 0;
    head.next = 1;
    head.x = std::numeric_limits<int32_t>::min();
    head.first_y = std::numeric_limits<int32_t>::min();
    tl_builder.edges_.push_back(Edge(head));

    if (!tl_builder.build(path, clip, can_cull_to_the_right)) {
        return std::nullopt;
    }

    if (tl_builder.edges().size() < 3) {
        return std::nullopt;
    }

    return &tl_builder.edges_;
}

std::optional<std::vector<Edge>> BasicEdgeBuilder::build_edges(
    const Path& path,
    const ShiftedIntRect* clip,
    int32_t clip_shift
) {
    bool can_cull_to_the_right = false;

    BasicEdgeBuilder builder(clip_shift);
    if (!builder.build(path, clip, can_cull_to_the_right)) {
        return std::nullopt;
    }

    if (builder.edges().size() < 2) {
        return std::nullopt;
    }

    return std::move(builder.edges());
}

} // namespace nisaba
