#pragma once

#include <vector>
#include <optional>
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/border_radius.hpp"
#include "nisaba/math/rrect.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/path_geometry.hpp"

namespace nisaba {

class PathBuilder {
public:
    PathBuilder() noexcept : last_move_to_index_(0), move_to_required_(true) {}

    explicit PathBuilder(size_t verbs_capacity, size_t points_capacity) {
        verbs_.reserve(verbs_capacity);
        points_.reserve(points_capacity);
        last_move_to_index_ = 0;
        move_to_required_ = true;
    }

    static Path from_rect(const Rect& rect) noexcept {
        Path p;
        p.bounds_ = rect;
        p.verbs_ = {
            PathVerb::Move,
            PathVerb::Line,
            PathVerb::Line,
            PathVerb::Line,
            PathVerb::Close,
        };
        p.points_ = {
            Point::from_xy(rect.left(), rect.top()),
            Point::from_xy(rect.right(), rect.top()),
            Point::from_xy(rect.right(), rect.bottom()),
            Point::from_xy(rect.left(), rect.bottom()),
        };
        return p;
    }

    static std::optional<Path> from_circle(float cx, float cy, float radius) noexcept {
        PathBuilder b;
        b.push_circle(cx, cy, radius);
        return b.finish();
    }

    static std::optional<Path> from_oval(const Rect& oval) noexcept {
        PathBuilder b;
        b.push_oval(oval);
        return b.finish();
    }

    static std::optional<Path> from_rounded_rect(const Rect& rect, float rx, float ry) noexcept {
        PathBuilder b;
        b.push_rounded_rect(rect, rx, ry);
        return b.finish();
    }

    static std::optional<Path> from_rounded_rect(const Rect& rect, const BorderRadius& radii) noexcept {
        PathBuilder b;
        b.push_rounded_rect(rect, radii);
        return b.finish();
    }

    static std::optional<Path> from_rrect(const RRect& rrect) noexcept {
        PathBuilder b;
        b.push_rrect(rrect);
        return b.finish();
    }

    size_t len() const noexcept { return verbs_.size(); }
    bool is_empty() const noexcept { return verbs_.empty(); }

    void reserve(size_t verbs_capacity, size_t points_capacity) {
        verbs_.reserve(verbs_capacity);
        points_.reserve(points_capacity);
    }

    void move_to(float x, float y) {
        if (!verbs_.empty() && verbs_.back() == PathVerb::Move) {
            points_.back() = Point::from_xy(x, y);
        } else {
            last_move_to_index_ = points_.size();
            move_to_required_ = false;
            verbs_.push_back(PathVerb::Move);
            points_.push_back(Point::from_xy(x, y));
        }
    }
    void move_to(Point p) { move_to(p.x, p.y); }

    void line_to(float x, float y) {
        inject_move_to_if_needed();
        verbs_.push_back(PathVerb::Line);
        points_.push_back(Point::from_xy(x, y));
    }
    void line_to(Point p) { line_to(p.x, p.y); }

    void quad_to(float x1, float y1, float x, float y) {
        inject_move_to_if_needed();
        verbs_.push_back(PathVerb::Quad);
        points_.push_back(Point::from_xy(x1, y1));
        points_.push_back(Point::from_xy(x, y));
    }
    void quad_to(Point p1, Point p2) { quad_to(p1.x, p1.y, p2.x, p2.y); }

    void conic_to(float x1, float y1, float x, float y, float weight) {
        if (!(weight > 0.0f)) {
            line_to(x, y);
        } else if (!std::isfinite(weight)) {
            line_to(x1, y1);
            line_to(x, y);
        } else if (weight == 1.0f) {
            quad_to(x1, y1, x, y);
        } else {
            inject_move_to_if_needed();
            Point last = *last_point();
            auto quadder = path_geometry::AutoConicToQuads::compute(
                last,
                Point::from_xy(x1, y1),
                Point::from_xy(x, y),
                weight
            );
            if (quadder) {
                size_t offset = 1;
                for (size_t i = 0; i < quadder->len; ++i) {
                    Point pt1 = quadder->points[offset + 0];
                    Point pt2 = quadder->points[offset + 1];
                    quad_to(pt1.x, pt1.y, pt2.x, pt2.y);
                    offset += 2;
                }
            }
        }
    }

    void cubic_to(float x1, float y1, float x2, float y2, float x, float y) {
        inject_move_to_if_needed();
        verbs_.push_back(PathVerb::Cubic);
        points_.push_back(Point::from_xy(x1, y1));
        points_.push_back(Point::from_xy(x2, y2));
        points_.push_back(Point::from_xy(x, y));
    }
    void cubic_to(Point p1, Point p2, Point p3) { cubic_to(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y); }

    void close() {
        if (!verbs_.empty() && verbs_.back() != PathVerb::Close) {
            verbs_.push_back(PathVerb::Close);
        }
        move_to_required_ = true;
    }

    inline void push_move_to(float x, float y) noexcept {
        verbs_.push_back(PathVerb::Move);
        points_.push_back(Point::from_xy(x, y));
    }

    inline void push_line_to(float x, float y) noexcept {
        verbs_.push_back(PathVerb::Line);
        points_.push_back(Point::from_xy(x, y));
    }

    inline void push_quad_to(float x1, float y1, float x, float y) noexcept {
        verbs_.push_back(PathVerb::Quad);
        points_.push_back(Point::from_xy(x1, y1));
        points_.push_back(Point::from_xy(x, y));
    }

    inline void push_close() noexcept {
        verbs_.push_back(PathVerb::Close);
    }

    std::optional<Point> last_point() const noexcept {
        if (points_.empty()) return std::nullopt;
        return points_.back();
    }

    void set_last_point(Point pt) {
        if (!points_.empty()) {
            points_.back() = pt;
        } else {
            move_to(pt.x, pt.y);
        }
    }

    void push_rect(const Rect& rect) {
        move_to(rect.left(), rect.top());
        line_to(rect.right(), rect.top());
        line_to(rect.right(), rect.bottom());
        line_to(rect.left(), rect.bottom());
        close();
    }

    void push_oval(const Rect& oval) {
        float cx = scalar::half(oval.left()) + scalar::half(oval.right());
        float cy = scalar::half(oval.top()) + scalar::half(oval.bottom());
        float rx = scalar::half(oval.width());
        float ry = scalar::half(oval.height());

        constexpr float KAPPA = 0.5522847498f;
        float kx = rx * KAPPA;
        float ky = ry * KAPPA;

        float l = oval.left();
        float t = oval.top();
        float r = oval.right();
        float b = oval.bottom();

        move_to(r, cy);
        cubic_to(r, cy + ky, cx + kx, b, cx, b);
        cubic_to(cx - kx, b, l, cy + ky, l, cy);
        cubic_to(l, cy - ky, cx - kx, t, cx, t);
        cubic_to(cx + kx, t, r, cy - ky, r, cy);
        close();
    }

    void push_circle(float x, float y, float r) {
        auto rect = Rect::from_xywh(x - r, y - r, r + r, r + r);
        if (rect) {
            push_oval(*rect);
        }
    }

    void push_arc(float cx, float cy, float r, float start_angle, float sweep_angle) {
        if (r <= 0.0f || std::abs(sweep_angle) < 1e-6f) return;

        int n_splits = static_cast<int>(std::ceil(std::abs(sweep_angle) / (static_cast<float>(M_PI) * 0.5f)));
        n_splits = std::clamp(n_splits, 1, 8);
        float step = sweep_angle / static_cast<float>(n_splits);
        float k = (4.0f / 3.0f) * std::tan(step * 0.25f);

        float cur_a = start_angle;
        float cos_a = std::cos(cur_a);
        float sin_a = std::sin(cur_a);
        float x0 = cx + r * cos_a;
        float y0 = cy + r * sin_a;

        if (verbs_.empty() || move_to_required_) {
            move_to(x0, y0);
        } else {
            line_to(x0, y0);
        }

        for (int i = 0; i < n_splits; ++i) {
            float next_a = cur_a + step;
            float cos_next = std::cos(next_a);
            float sin_next = std::sin(next_a);

            float cp1_x = x0 - k * r * sin_a;
            float cp1_y = y0 + k * r * cos_a;

            float x1 = cx + r * cos_next;
            float y1 = cy + r * sin_next;

            float cp2_x = x1 + k * r * sin_next;
            float cp2_y = y1 - k * r * cos_next;

            cubic_to(cp1_x, cp1_y, cp2_x, cp2_y, x1, y1);

            cur_a = next_a;
            cos_a = cos_next;
            sin_a = sin_next;
            x0 = x1;
            y0 = y1;
        }
    }

    /// Appends an elliptical arc bounded by `oval`, with start and sweep angles in degrees.
    /// When `use_center` is true, creates a closed pie wedge connected to the oval center.
    void push_arc(
        const Rect& oval,
        float start_deg,
        float sweep_deg,
        bool use_center = false,
        bool force_move_to = false
    ) {
        const float rx = oval.width() * 0.5f;
        const float ry = oval.height() * 0.5f;
        if (rx <= 0.0f || ry <= 0.0f || std::abs(sweep_deg) < 1e-6f) return;

        const float cx = oval.left() + rx;
        const float cy = oval.top() + ry;

        constexpr float kDegToRad = static_cast<float>(M_PI) / 180.0f;
        const float clamped_sweep_deg = std::clamp(sweep_deg, -360.0f, 360.0f);
        const float start_rad = start_deg * kDegToRad;
        const float sweep_rad = clamped_sweep_deg * kDegToRad;

        int n_splits = static_cast<int>(std::ceil(std::abs(sweep_rad) / (static_cast<float>(M_PI) * 0.5f)));
        n_splits = std::clamp(n_splits, 1, 8);
        const float step = sweep_rad / static_cast<float>(n_splits);
        const float k = (4.0f / 3.0f) * std::tan(step * 0.25f);

        float cur_a = start_rad;
        float cos_a = std::cos(cur_a);
        float sin_a = std::sin(cur_a);
        float x0 = cx + rx * cos_a;
        float y0 = cy + ry * sin_a;

        if (use_center) {
            move_to(cx, cy);
            line_to(x0, y0);
        } else if (force_move_to || verbs_.empty() || move_to_required_) {
            move_to(x0, y0);
        } else {
            line_to(x0, y0);
        }

        for (int i = 0; i < n_splits; ++i) {
            const float next_a = cur_a + step;
            const float cos_next = std::cos(next_a);
            const float sin_next = std::sin(next_a);

            const float cp1_x = x0 - k * rx * sin_a;
            const float cp1_y = y0 + k * ry * cos_a;

            const float x1 = cx + rx * cos_next;
            const float y1 = cy + ry * sin_next;

            const float cp2_x = x1 + k * rx * sin_next;
            const float cp2_y = y1 - k * ry * cos_next;

            cubic_to(cp1_x, cp1_y, cp2_x, cp2_y, x1, y1);

            cur_a = next_a;
            cos_a = cos_next;
            sin_a = sin_next;
            x0 = x1;
            y0 = y1;
        }

        if (use_center) {
            close();
        }
    }

    void push_rounded_rect(const Rect& rect, float rx, float ry) {
        float max_rx = scalar::half(rect.width());
        float max_ry = scalar::half(rect.height());
        rx = std::clamp(rx, 0.0f, max_rx);
        ry = std::clamp(ry, 0.0f, max_ry);

        if (rx <= 0.0f || ry <= 0.0f) {
            push_rect(rect);
            return;
        }

        constexpr float KAPPA = 0.5522847498f;
        float kx = rx * KAPPA;
        float ky = ry * KAPPA;

        float l = rect.left();
        float t = rect.top();
        float r = rect.right();
        float b = rect.bottom();

        move_to(l + rx, t);
        line_to(r - rx, t);
        cubic_to(r - rx + kx, t, r, t + ry - ky, r, t + ry);
        line_to(r, b - ry);
        cubic_to(r, b - ry + ky, r - rx + kx, b, r - rx, b);
        line_to(l + rx, b);
        cubic_to(l + rx - kx, b, l, b - ry + ky, l, b - ry);
        line_to(l, t + ry);
        cubic_to(l, t + ry - ky, l + rx - kx, t, l + rx, t);
        close();
    }

    void push_rounded_rect(const Rect& rect, const BorderRadius& radii) {
        if (radii.is_zero()) {
            push_rect(rect);
            return;
        }

        BorderRadius cr = radii.constrained_for_size(rect.width(), rect.height());
        if (cr.is_zero()) {
            push_rect(rect);
            return;
        }

        constexpr float KAPPA = 0.5522847498f;

        float l = rect.left();
        float t = rect.top();
        float r = rect.right();
        float b = rect.bottom();

        float rtl = cr.top_left;
        float rtr = cr.top_right;
        float rbr = cr.bottom_right;
        float rbl = cr.bottom_left;

        // Start at top-left edge after top-left corner
        move_to(l + rtl, t);

        // Top edge
        line_to(r - rtr, t);

        // Top-right corner
        if (rtr > 0.0f) {
            float k = rtr * KAPPA;
            cubic_to(r - rtr + k, t, r, t + rtr - k, r, t + rtr);
        }

        // Right edge
        line_to(r, b - rbr);

        // Bottom-right corner
        if (rbr > 0.0f) {
            float k = rbr * KAPPA;
            cubic_to(r, b - rbr + k, r - rbr + k, b, r - rbr, b);
        }

        // Bottom edge
        line_to(l + rbl, b);

        // Bottom-left corner
        if (rbl > 0.0f) {
            float k = rbl * KAPPA;
            cubic_to(l + rbl - k, b, l, b - rbl + k, l, b - rbl);
        }

        // Left edge
        line_to(l, t + rtl);

        // Top-left corner
        if (rtl > 0.0f) {
            float k = rtl * KAPPA;
            cubic_to(l, t + rtl - k, l + rtl - k, t, l + rtl, t);
        }

        close();
    }

    void push_rrect(const RRect& rrect) {
        push_rounded_rect(rrect.rect, rrect.radii);
    }

    const std::vector<Point>& points() const noexcept { return points_; }
    const std::vector<PathVerb>& verbs() const noexcept { return verbs_; }

    void push_path(const Path& other) {
        last_move_to_index_ = points_.size();
        verbs_.insert(verbs_.end(), other.verbs().begin(), other.verbs().end());
        points_.insert(points_.end(), other.points().begin(), other.points().end());
    }

    void push_path_builder(const PathBuilder* other) {
        if (!other || other->is_empty()) return;
        if (last_move_to_index_ != 0) {
            last_move_to_index_ = points_.size() + other->last_move_to_index_;
        }
        verbs_.insert(verbs_.end(), other->verbs_.begin(), other->verbs_.end());
        points_.insert(points_.end(), other->points_.begin(), other->points_.end());
    }

    void reverse_path_to(const PathBuilder* other) {
        if (!other || other->is_empty()) return;
        size_t points_offset = other->points_.size() - 1;
        for (auto it = other->verbs_.rbegin(); it != other->verbs_.rend(); ++it) {
            switch (*it) {
                case PathVerb::Move:
                    return;
                case PathVerb::Line: {
                    Point pt = other->points_[points_offset - 1];
                    points_offset -= 1;
                    line_to(pt.x, pt.y);
                    break;
                }
                case PathVerb::Quad: {
                    Point pt1 = other->points_[points_offset - 1];
                    Point pt2 = other->points_[points_offset - 2];
                    points_offset -= 2;
                    quad_to(pt1.x, pt1.y, pt2.x, pt2.y);
                    break;
                }
                case PathVerb::Cubic: {
                    Point pt1 = other->points_[points_offset - 1];
                    Point pt2 = other->points_[points_offset - 2];
                    Point pt3 = other->points_[points_offset - 3];
                    points_offset -= 3;
                    cubic_to(pt1.x, pt1.y, pt2.x, pt2.y, pt3.x, pt3.y);
                    break;
                }
                case PathVerb::Close:
                    break;
            }
        }
    }

    void clear() noexcept {
        verbs_.clear();
        points_.clear();
        last_move_to_index_ = 0;
        move_to_required_ = true;
    }

    std::optional<Path> finish() {
        if (is_empty() || verbs_.size() == 1) {
            return std::nullopt;
        }

        auto bounds = Rect::from_points(points_.data(), points_.size());
        if (!bounds) {
            return std::nullopt;
        }

        Path p;
        p.bounds_ = *bounds;
        p.verbs_ = std::move(verbs_);
        p.points_ = std::move(points_);
        return p;
    }

    std::optional<Path> finish_with_bounds(const Rect& bounds) {
        if (is_empty() || verbs_.size() == 1) {
            return std::nullopt;
        }

        Path p;
        p.bounds_ = bounds;
        p.verbs_ = std::move(verbs_);
        p.points_ = std::move(points_);
        return p;
    }

    void finish_into(Path& p) {
        if (is_empty() || verbs_.size() == 1) {
            p.reset();
            return;
        }
        auto bounds = Rect::from_points(points_.data(), points_.size());
        if (!bounds) {
            p.reset();
            return;
        }
        p.bounds_ = *bounds;
        // Swap to preserve the capacities of both Path and PathBuilder vectors
        std::swap(p.verbs_, verbs_);
        std::swap(p.points_, points_);
        // Clear the PathBuilder's new vectors (which are the old Path's vectors, retaining their capacity)
        verbs_.clear();
        points_.clear();
    }

private:
    void inject_move_to_if_needed() {
        if (move_to_required_) {
            if (last_move_to_index_ < points_.size()) {
                Point p = points_[last_move_to_index_];
                move_to(p.x, p.y);
            } else {
                move_to(0.0f, 0.0f);
            }
        }
    }

    std::vector<PathVerb> verbs_;
    std::vector<Point> points_;
    size_t last_move_to_index_{0};
    bool move_to_required_{true};
};

inline PathBuilder Path::clear() && {
    verbs_.clear();
    points_.clear();
    PathBuilder b;
    return b;
}

} // namespace nisaba
