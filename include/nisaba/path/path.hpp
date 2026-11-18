#pragma once

#include <vector>
#include <optional>
#include <cstdint>
#include <algorithm>
#include "nisaba/types.hpp"
#include "nisaba/math/scalar.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/path/path_geometry.hpp"

namespace nisaba {

class PathBuilder;
class PathMeasure;
enum class PathOp : uint8_t;
struct Stroke;
namespace text {
class TtfFont;
}

enum class PathVerb : uint8_t {
    Move,
    Line,
    Quad,
    Cubic,
    Close,
};

/// A path filling rule.
enum class FillRule {
    /// Specifies that "inside" is computed by a non-zero sum of signed edge crossings.
    Winding,
    /// Specifies that "inside" is computed by an odd number of edge crossings.
    EvenOdd,
};

struct PathSegment {
    enum class Type : uint8_t {
        MoveTo,
        LineTo,
        QuadTo,
        CubicTo,
        Close,
    } type;

    Point p0;
    Point p1;
    Point p2;

    static constexpr PathSegment make_move_to(Point p) noexcept {
        return PathSegment{Type::MoveTo, p, Point(), Point()};
    }

    static constexpr PathSegment make_line_to(Point p) noexcept {
        return PathSegment{Type::LineTo, p, Point(), Point()};
    }

    static constexpr PathSegment make_quad_to(Point pt1, Point pt2) noexcept {
        return PathSegment{Type::QuadTo, pt1, pt2, Point()};
    }

    static constexpr PathSegment make_cubic_to(Point pt1, Point pt2, Point pt3) noexcept {
        return PathSegment{Type::CubicTo, pt1, pt2, pt3};
    }

    static constexpr PathSegment make_close() noexcept {
        return PathSegment{Type::Close, Point(), Point(), Point()};
    }

    constexpr bool operator==(const PathSegment& o) const noexcept {
        if (type != o.type) return false;
        switch (type) {
            case Type::MoveTo:
            case Type::LineTo:
                return p0 == o.p0;
            case Type::QuadTo:
                return p0 == o.p0 && p1 == o.p1;
            case Type::CubicTo:
                return p0 == o.p0 && p1 == o.p1 && p2 == o.p2;
            case Type::Close:
                return true;
        }
        return false;
    }
};

class Path;

class PathSegmentsIter {
public:
    PathSegmentsIter(const Path& path) noexcept;

    void set_auto_close(bool flag) noexcept {
        is_auto_close_ = flag;
    }

    std::optional<PathSegment> next() noexcept;

    std::optional<PathVerb> curr_verb() const noexcept;
    std::optional<PathVerb> next_verb() const noexcept;

    Point last_point() const noexcept { return last_point_; }
    Point last_move_to() const noexcept { return last_move_to_; }

private:
    PathSegment auto_close() noexcept;

    const Path& path_;
    size_t verb_index_{0};
    size_t points_index_{0};
    bool is_auto_close_{false};
    Point last_move_to_{0.0f, 0.0f};
    Point last_point_{0.0f, 0.0f};
};

class Path {
public:
    Path() noexcept = default;

    size_t len() const noexcept { return verbs_.size(); }
    bool is_empty() const noexcept { return verbs_.empty(); }
    Rect bounds() const noexcept { return bounds_; }

    /// Returns true if this path represents an axis-aligned rectangle, optionally outputting its bounds.
    bool is_rect(Rect* out_rect = nullptr) const noexcept {
        if (verbs_.size() == 5 &&
            verbs_[0] == PathVerb::Move &&
            verbs_[1] == PathVerb::Line &&
            verbs_[2] == PathVerb::Line &&
            verbs_[3] == PathVerb::Line &&
            verbs_[4] == PathVerb::Close &&
            points_.size() == 4) {
            float x0 = points_[0].x, y0 = points_[0].y;
            float x1 = points_[1].x, y1 = points_[1].y;
            float x2 = points_[2].x, y2 = points_[2].y;
            float x3 = points_[3].x, y3 = points_[3].y;
            bool is_cw  = (x0 == x3 && y0 == y1 && x1 == x2 && y2 == y3 && x0 != x1 && y0 != y2);
            bool is_ccw = (y0 == y3 && x0 == x1 && y1 == y2 && x2 == x3 && x0 != x2 && y0 != y1);
            if (is_cw || is_ccw) {
                if (out_rect) *out_rect = bounds_;
                return true;
            }
        }
        return false;
    }

    /// Returns true if this path represents a circle, optionally outputting its center and radius.
    bool is_circle(float* out_cx = nullptr, float* out_cy = nullptr, float* out_r = nullptr) const noexcept {
        if (verbs_.size() == 6 &&
            verbs_[0] == PathVerb::Move &&
            verbs_[1] == PathVerb::Cubic &&
            verbs_[2] == PathVerb::Cubic &&
            verbs_[3] == PathVerb::Cubic &&
            verbs_[4] == PathVerb::Cubic &&
            verbs_[5] == PathVerb::Close &&
            points_.size() == 13) {
            float w = bounds_.width();
            float h = bounds_.height();
            if (w > 0.0f && std::abs(w - h) < 1e-2f) {
                float r = w * 0.5f;
                float cx = bounds_.left() + r;
                float cy = bounds_.top() + r;
                if (std::abs(points_[0].x - (cx + r)) < 0.1f && std::abs(points_[0].y - cy) < 0.1f) {
                    if (out_cx) *out_cx = cx;
                    if (out_cy) *out_cy = cy;
                    if (out_r) *out_r = r;
                    return true;
                }
            }
        }
        return false;
    }

    const std::vector<PathVerb>& verbs() const noexcept { return verbs_; }
    const std::vector<Point>& points() const noexcept { return points_; }

    PathSegmentsIter segments() const noexcept {
        return PathSegmentsIter(*this);
    }

    std::optional<Path> transform(const Transform& ts) const noexcept {
        if (ts.is_identity()) {
            return *this;
        }

        Path p = *this;
        ts.map_points(p.points_.data(), p.points_.size());
        auto b = Rect::from_points(p.points_.data(), p.points_.size());
        if (!b) {
            return std::nullopt;
        }
        p.bounds_ = *b;
        return p;
    }

    void apply_transform(const Transform& ts) noexcept {
        if (ts.is_identity() || points_.empty()) return;
        ts.map_points(points_.data(), points_.size());
        if (auto b = Rect::from_points(points_.data(), points_.size())) {
            bounds_ = *b;
        }
    }

    void add_path(const Path& other) {
        if (other.is_empty()) return;
        if (is_empty()) {
            *this = other;
            return;
        }
        verbs_.reserve(verbs_.size() + other.verbs_.size());
        points_.reserve(points_.size() + other.points_.size());
        for (auto v : other.verbs_) {
            verbs_.push_back(v);
        }
        for (const auto& pt : other.points_) {
            points_.push_back(pt);
        }
        if (auto b = Rect::from_points(points_.data(), points_.size())) {
            bounds_ = *b;
        }
    }

    std::optional<Rect> compute_tight_bounds() const noexcept;

    std::optional<Path> op(const Path& other, PathOp path_op, FillRule fill_rule_a = FillRule::Winding, FillRule fill_rule_b = FillRule::Winding) const;
    std::optional<Path> unite(const Path& other) const;
    std::optional<Path> difference(const Path& other) const;
    std::optional<Path> intersect(const Path& other) const;
    std::optional<Path> xor_op(const Path& other) const;

    /// Converts this stroked path (with specified width, caps, joins, and dashing)
    /// into a closed, filled outline path representing its outer geometry.
    [[nodiscard]] std::optional<Path> stroke_to_fill(const Stroke& stroke, float resolution_scale = 1.0f) const;

    /// Alias for stroke_to_fill.
    [[nodiscard]] std::optional<Path> stroke(const Stroke& stroke, float resolution_scale = 1.0f) const;

    /// Converts this stroked path to a filled outline and cleans self-intersections.
    [[nodiscard]] std::optional<Path> stroke_to_fill_clean(const Stroke& stroke, float resolution_scale = 1.0f) const;

    PathBuilder clear() &&;

    void reset() noexcept {
        verbs_.clear();
        points_.clear();
        bounds_ = Rect{};
    }

    bool operator==(const Path& o) const noexcept {
        return verbs_ == o.verbs_ && points_ == o.points_ && bounds_ == o.bounds_;
    }

    bool operator!=(const Path& o) const noexcept {
        return !(*this == o);
    }

private:
    std::vector<PathVerb> verbs_;
    std::vector<Point> points_;
    Rect bounds_;

    friend class PathBuilder;
    friend class PathMeasure;
    friend class PathSegmentsIter;
    friend class text::TtfFont;
};

inline PathSegmentsIter::PathSegmentsIter(const Path& path) noexcept
    : path_(path) {}

inline PathSegment PathSegmentsIter::auto_close() noexcept {
    if (is_auto_close_ && last_point_ != last_move_to_) {
        verb_index_--;
        return PathSegment::make_line_to(last_move_to_);
    } else {
        return PathSegment::make_close();
    }
}

inline std::optional<PathVerb> PathSegmentsIter::curr_verb() const noexcept {
    if (verb_index_ > 0 && verb_index_ <= path_.verbs_.size()) {
        return path_.verbs_[verb_index_ - 1];
    }
    return std::nullopt;
}

inline std::optional<PathVerb> PathSegmentsIter::next_verb() const noexcept {
    if (verb_index_ < path_.verbs_.size()) {
        return path_.verbs_[verb_index_];
    }
    return std::nullopt;
}

inline std::optional<PathSegment> PathSegmentsIter::next() noexcept {
    if (verb_index_ < path_.verbs_.size()) {
        PathVerb verb = path_.verbs_[verb_index_++];
        switch (verb) {
            case PathVerb::Move: {
                points_index_++;
                last_move_to_ = path_.points_[points_index_ - 1];
                last_point_ = last_move_to_;
                return PathSegment::make_move_to(last_move_to_);
            }
            case PathVerb::Line: {
                points_index_++;
                last_point_ = path_.points_[points_index_ - 1];
                return PathSegment::make_line_to(last_point_);
            }
            case PathVerb::Quad: {
                points_index_ += 2;
                last_point_ = path_.points_[points_index_ - 1];
                return PathSegment::make_quad_to(path_.points_[points_index_ - 2], last_point_);
            }
            case PathVerb::Cubic: {
                points_index_ += 3;
                last_point_ = path_.points_[points_index_ - 1];
                return PathSegment::make_cubic_to(
                    path_.points_[points_index_ - 3],
                    path_.points_[points_index_ - 2],
                    last_point_
                );
            }
            case PathVerb::Close: {
                PathSegment seg = auto_close();
                last_point_ = last_move_to_;
                return seg;
            }
        }
    }
    return std::nullopt;
}

namespace detail {

inline size_t compute_quad_extremas(Point p0, Point p1, Point p2, Point extremas[5]) noexcept {
    Point src[3] = {p0, p1, p2};
    size_t idx = 0;
    auto t1 = path_geometry::find_quad_extrema(p0.x, p1.x, p2.x);
    if (t1) {
        extremas[idx++] = path_geometry::eval_quad_at(src, t1->to_normalized());
    }
    auto t2 = path_geometry::find_quad_extrema(p0.y, p1.y, p2.y);
    if (t2) {
        extremas[idx++] = path_geometry::eval_quad_at(src, t2->to_normalized());
    }
    extremas[idx++] = p2;
    return idx;
}

inline size_t compute_cubic_extremas(Point p0, Point p1, Point p2, Point p3, Point extremas[5]) noexcept {
    NormalizedF32Exclusive ts0[3];
    NormalizedF32Exclusive ts1[3];
    size_t n0 = path_geometry::find_cubic_extrema(p0.x, p1.x, p2.x, p3.x, ts0);
    size_t n1 = path_geometry::find_cubic_extrema(p0.y, p1.y, p2.y, p3.y, ts1);

    Point src[4] = {p0, p1, p2, p3};
    size_t idx = 0;
    for (size_t i = 0; i < n0; ++i) {
        extremas[idx++] = path_geometry::eval_cubic_pos_at(src, ts0[i].to_normalized());
    }
    for (size_t i = 0; i < n1; ++i) {
        extremas[idx++] = path_geometry::eval_cubic_pos_at(src, ts1[i].to_normalized());
    }
    extremas[idx++] = p3;
    return idx;
}

} // namespace detail

inline std::optional<Rect> Path::compute_tight_bounds() const noexcept {
    if (points_.empty()) return std::nullopt;

    Point extremas[5];
    Point min_pt = points_[0];
    Point max_pt = points_[0];

    PathSegmentsIter iter = segments();
    Point last_point = Point::zero();

    while (auto seg = iter.next()) {
        size_t count = 0;
        switch (seg->type) {
            case PathSegment::Type::MoveTo:
            case PathSegment::Type::LineTo:
                extremas[0] = seg->p0;
                count = 1;
                break;
            case PathSegment::Type::QuadTo:
                count = detail::compute_quad_extremas(last_point, seg->p0, seg->p1, extremas);
                break;
            case PathSegment::Type::CubicTo:
                count = detail::compute_cubic_extremas(last_point, seg->p0, seg->p1, seg->p2, extremas);
                break;
            case PathSegment::Type::Close:
                break;
        }

        last_point = iter.last_point();
        for (size_t i = 0; i < count; ++i) {
            min_pt.x = std::min(min_pt.x, extremas[i].x);
            min_pt.y = std::min(min_pt.y, extremas[i].y);
            max_pt.x = std::max(max_pt.x, extremas[i].x);
            max_pt.y = std::max(max_pt.y, extremas[i].y);
        }
    }

    return Rect::from_ltrb(min_pt.x, min_pt.y, max_pt.x, max_pt.y);
}

} // namespace nisaba
