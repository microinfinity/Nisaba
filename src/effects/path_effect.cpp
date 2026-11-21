#include "nisaba/effects/path_effect.hpp"
#include "nisaba/path/dash.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/path/path_measure.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nisaba::effects {

namespace {

void append_path(PathBuilder& pb, const Path& p) {
    auto it = p.segments();
    while (auto seg_opt = it.next()) {
        const auto& seg = *seg_opt;
        switch (seg.type) {
            case PathSegment::Type::MoveTo:
                pb.move_to(seg.p0.x, seg.p0.y);
                break;
            case PathSegment::Type::LineTo:
                pb.line_to(seg.p0.x, seg.p0.y);
                break;
            case PathSegment::Type::QuadTo:
                pb.quad_to(seg.p0.x, seg.p0.y, seg.p1.x, seg.p1.y);
                break;
            case PathSegment::Type::CubicTo:
                pb.cubic_to(seg.p0.x, seg.p0.y, seg.p1.x, seg.p1.y, seg.p2.x, seg.p2.y);
                break;
            case PathSegment::Type::Close:
                pb.close();
                break;
        }
    }
}

void append_transformed_path(PathBuilder& pb, const Path& p, const Transform& ts) {
    auto map = [&](Point pt) noexcept -> Point {
        ts.map_point(pt);
        return pt;
    };
    auto it = p.segments();
    while (auto seg_opt = it.next()) {
        const auto& seg = *seg_opt;
        switch (seg.type) {
            case PathSegment::Type::MoveTo: {
                Point q = map(seg.p0);
                pb.move_to(q.x, q.y);
                break;
            }
            case PathSegment::Type::LineTo: {
                Point q = map(seg.p0);
                pb.line_to(q.x, q.y);
                break;
            }
            case PathSegment::Type::QuadTo: {
                Point q0 = map(seg.p0), q1 = map(seg.p1);
                pb.quad_to(q0.x, q0.y, q1.x, q1.y);
                break;
            }
            case PathSegment::Type::CubicTo: {
                Point q0 = map(seg.p0), q1 = map(seg.p1), q2 = map(seg.p2);
                pb.cubic_to(q0.x, q0.y, q1.x, q1.y, q2.x, q2.y);
                break;
            }
            case PathSegment::Type::Close:
                pb.close();
                break;
        }
    }
}

bool compute_corner_step(Point prev, Point curr, float radius, Point& out_pt) noexcept {
    const float dx = prev.x - curr.x;
    const float dy = prev.y - curr.y;
    const float dist = std::hypot(dx, dy);
    if (dist <= 1e-5f) {
        out_pt = curr;
        return false;
    }
    const float step = std::min(radius, dist * 0.5f);
    const float t = step / dist;
    out_pt = Point::from_xy(curr.x + dx * t, curr.y + dy * t);
    return true;
}

Path apply_corner(const Path& src, float radius) {
    if (radius <= 0.0f || src.is_empty()) return src;

    PathBuilder pb;
    std::vector<Point> contour;
    bool closed = false;

    auto flush_contour = [&]() {
        if (contour.empty()) return;
        // Remove duplicate closing point if explicitly equal to start
        while (contour.size() > 1 &&
               std::hypot(contour.back().x - contour.front().x, contour.back().y - contour.front().y) < 1e-4f) {
            contour.pop_back();
            closed = true;
        }
        const size_t n = contour.size();
        if (n < 2) {
            pb.move_to(contour[0].x, contour[0].y);
            if (closed) pb.close();
            contour.clear();
            closed = false;
            return;
        }
        if (n == 2) {
            pb.move_to(contour[0].x, contour[0].y);
            pb.line_to(contour[1].x, contour[1].y);
            if (closed) pb.close();
            contour.clear();
            closed = false;
            return;
        }

        if (closed) {
            Point first_out{};
            compute_corner_step(contour[1], contour[0], radius, first_out);
            pb.move_to(first_out.x, first_out.y);
            for (size_t i = 1; i < n; ++i) {
                const Point prev = contour[i - 1];
                const Point curr = contour[i];
                const Point next = contour[(i + 1) % n];
                Point in_pt{}, out_pt{};
                compute_corner_step(prev, curr, radius, in_pt);
                compute_corner_step(next, curr, radius, out_pt);
                pb.line_to(in_pt.x, in_pt.y);
                pb.quad_to(curr.x, curr.y, out_pt.x, out_pt.y);
            }
            Point last_in{};
            compute_corner_step(contour[n - 1], contour[0], radius, last_in);
            pb.line_to(last_in.x, last_in.y);
            pb.quad_to(contour[0].x, contour[0].y, first_out.x, first_out.y);
            pb.close();
        } else {
            pb.move_to(contour[0].x, contour[0].y);
            for (size_t i = 1; i + 1 < n; ++i) {
                const Point prev = contour[i - 1];
                const Point curr = contour[i];
                const Point next = contour[i + 1];
                Point in_pt{}, out_pt{};
                compute_corner_step(prev, curr, radius, in_pt);
                compute_corner_step(next, curr, radius, out_pt);
                pb.line_to(in_pt.x, in_pt.y);
                pb.quad_to(curr.x, curr.y, out_pt.x, out_pt.y);
            }
            pb.line_to(contour[n - 1].x, contour[n - 1].y);
        }
        contour.clear();
        closed = false;
    };

    auto src_it = src.segments();
    while (auto seg_opt = src_it.next()) {
        const auto& seg = *seg_opt;
        switch (seg.type) {
            case PathSegment::Type::MoveTo:
                flush_contour();
                contour.push_back(seg.p0);
                break;
            case PathSegment::Type::LineTo:
                contour.push_back(seg.p0);
                break;
            case PathSegment::Type::QuadTo:
                // Preserve curve endpoints and pass through
                flush_contour();
                pb.quad_to(seg.p0.x, seg.p0.y, seg.p1.x, seg.p1.y);
                contour.push_back(seg.p1);
                break;
            case PathSegment::Type::CubicTo:
                flush_contour();
                pb.cubic_to(seg.p0.x, seg.p0.y, seg.p1.x, seg.p1.y, seg.p2.x, seg.p2.y);
                contour.push_back(seg.p2);
                break;
            case PathSegment::Type::Close:
                closed = true;
                flush_contour();
                break;
        }
    }
    flush_contour();
    return pb.finish().value_or(Path{});
}

Path apply_trim(const Path& src, float start, float stop, bool inverted) {
    if (src.is_empty()) return src;
    float s0 = std::clamp(start, 0.0f, 1.0f);
    float s1 = std::clamp(stop, 0.0f, 1.0f);
    if (s0 > s1) std::swap(s0, s1);
    if (!inverted && s0 <= 1e-6f && s1 >= 1.0f - 1e-6f) return src;

    Path out;
    PathMeasure meas(src, false);
    if (meas.contour_count() == 0) return out;
    do {
        const float len = meas.length();
        if (len <= 1e-6f) continue;
        if (!inverted) {
            if (s1 > s0 + 1e-6f) {
                meas.get_segment(s0 * len, s1 * len, &out, true);
            }
        } else {
            if (s0 > 1e-6f) {
                meas.get_segment(0.0f, s0 * len, &out, true);
            }
            if (s1 < 1.0f - 1e-6f) {
                meas.get_segment(s1 * len, len, &out, true);
            }
        }
    } while (meas.next_contour());
    return out;
}

uint32_t hash_u32(uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rand_signed(uint32_t seed, uint32_t idx) noexcept {
    const uint32_t h = hash_u32(seed ^ (idx * 2654435761u));
    const float u = static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
    return u * 2.0f - 1.0f;
}

Path apply_discrete(const Path& src, float seg_len, float dev, uint32_t seed) {
    if (seg_len <= 1e-4f || src.is_empty()) return src;
    PathBuilder pb;
    PathMeasure meas(src, false);
    if (meas.contour_count() == 0) return Path{};

    uint32_t sample_idx = 0;
    do {
        const float len = meas.length();
        if (len <= 1e-5f) continue;
        const bool is_closed = meas.is_closed();
        const int steps = std::max(1, static_cast<int>(std::ceil(len / seg_len)));
        const float step_d = len / static_cast<float>(steps);

        Point first_pt{};
        for (int i = 0; i <= steps; ++i) {
            if (is_closed && i == steps) break;
            const float d = std::min(len, static_cast<float>(i) * step_d);
            Point pos{}, tan{};
            if (!meas.get_pos_tan(d, &pos, &tan)) continue;
            const float nx = -tan.y;
            const float ny = tan.x;
            const float offset = rand_signed(seed, ++sample_idx) * dev;
            const Point p = Point::from_xy(pos.x + nx * offset, pos.y + ny * offset);
            if (i == 0) {
                first_pt = p;
                pb.move_to(p.x, p.y);
            } else {
                pb.line_to(p.x, p.y);
            }
        }
        if (is_closed) {
            pb.line_to(first_pt.x, first_pt.y);
            pb.close();
        }
    } while (meas.next_contour());
    return pb.finish().value_or(Path{});
}

Path apply_path_1d(const Path& src, const Path& stamp, float advance, float phase, Path1DStyle style) {
    if (advance <= 1e-4f || src.is_empty() || stamp.is_empty()) return Path{};
    PathBuilder pb;
    PathMeasure meas(src, false);
    if (meas.contour_count() == 0) return Path{};

    auto morph_point = [&](float base_d, float contour_len, Point sp) noexcept -> Point {
        const float d = std::clamp(base_d + sp.x, 0.0f, contour_len);
        Point pos{}, tan{};
        if (!meas.get_pos_tan(d, &pos, &tan)) return pos;
        return Point::from_xy(pos.x - tan.y * sp.y, pos.y + tan.x * sp.y);
    };

    float start_d = std::fmod(phase, advance);
    if (start_d < 0.0f) start_d += advance;

    do {
        const float len = meas.length();
        if (len <= 1e-5f) continue;
        for (float d = start_d; d <= len + 1e-4f; d += advance) {
            const float cd = std::min(d, len);
            Point pos{}, tan{};
            if (!meas.get_pos_tan(cd, &pos, &tan)) continue;
            if (style == Path1DStyle::Translate) {
                append_transformed_path(pb, stamp, Transform::from_translate(pos.x, pos.y));
            } else if (style == Path1DStyle::Rotate) {
                Transform tr(tan.x, tan.y, -tan.y, tan.x, pos.x, pos.y);
                append_transformed_path(pb, stamp, tr);
            } else {
                // Morph: deform every control/anchor point along contour arc-length + normal
                auto st_it = stamp.segments();
                while (auto seg_opt = st_it.next()) {
                    const auto& seg = *seg_opt;
                    switch (seg.type) {
                        case PathSegment::Type::MoveTo: {
                            Point q = morph_point(cd, len, seg.p0);
                            pb.move_to(q.x, q.y);
                            break;
                        }
                        case PathSegment::Type::LineTo: {
                            Point q = morph_point(cd, len, seg.p0);
                            pb.line_to(q.x, q.y);
                            break;
                        }
                        case PathSegment::Type::QuadTo: {
                            Point q0 = morph_point(cd, len, seg.p0);
                            Point q1 = morph_point(cd, len, seg.p1);
                            pb.quad_to(q0.x, q0.y, q1.x, q1.y);
                            break;
                        }
                        case PathSegment::Type::CubicTo: {
                            Point q0 = morph_point(cd, len, seg.p0);
                            Point q1 = morph_point(cd, len, seg.p1);
                            Point q2 = morph_point(cd, len, seg.p2);
                            pb.cubic_to(q0.x, q0.y, q1.x, q1.y, q2.x, q2.y);
                            break;
                        }
                        case PathSegment::Type::Close:
                            pb.close();
                            break;
                    }
                }
            }
        }
    } while (meas.next_contour());
    return pb.finish().value_or(Path{});
}

Path apply_path_2d(const Path& src, const Transform& mat, const Path& stamp) {
    if (src.is_empty() || stamp.is_empty()) return Path{};
    auto inv_opt = mat.invert();
    if (!inv_opt) return Path{};
    const Rect src_bounds = src.bounds();
    auto inv_bounds = src_bounds.transform(*inv_opt);
    if (!inv_bounds) return Path{};

    const int min_x = static_cast<int>(std::floor(inv_bounds->left())) - 1;
    const int max_x = static_cast<int>(std::ceil(inv_bounds->right())) + 1;
    const int min_y = static_cast<int>(std::floor(inv_bounds->top())) - 1;
    const int max_y = static_cast<int>(std::ceil(inv_bounds->bottom())) + 1;

    // Guard against pathological degenerate matrices
    const int max_cells = 4096;
    if (static_cast<int64_t>(max_x - min_x + 1) * static_cast<int64_t>(max_y - min_y + 1) > max_cells) {
        return Path{};
    }

    PathBuilder pb;
    for (int iy = min_y; iy <= max_y; ++iy) {
        for (int ix = min_x; ix <= max_x; ++ix) {
            const Transform cell_tr = mat.pre_translate(static_cast<float>(ix), static_cast<float>(iy));
            append_transformed_path(pb, stamp, cell_tr);
        }
    }
    return pb.finish().value_or(Path{});
}

Path eval_node(const PathEffectNode* node, const Path& src) {
    if (!node || node->kind == PathEffectKind::None) return src;
    switch (node->kind) {
        case PathEffectKind::None:
            return src;
        case PathEffectKind::Corner:
            return apply_corner(src, node->radius);
        case PathEffectKind::Trim:
            return apply_trim(src, node->start, node->stop, node->inverted);
        case PathEffectKind::Discrete:
            return apply_discrete(src, node->seg_length, node->deviation, node->seed);
        case PathEffectKind::Dash: {
            auto dash = StrokeDash::create(node->intervals, node->phase);
            if (!dash) return src;
            return dash_path(src, *dash, 1.0f).value_or(src);
        }
        case PathEffectKind::Path1D:
            return apply_path_1d(src, node->stamp_path, node->advance, node->phase, node->style_1d);
        case PathEffectKind::Path2D:
            return apply_path_2d(src, node->matrix_2d, node->stamp_path);
        case PathEffectKind::Compose: {
            Path inner_p = node->second ? eval_node(node->second.get(), src) : src;
            return node->first ? eval_node(node->first.get(), inner_p) : inner_p;
        }
        case PathEffectKind::Sum: {
            PathBuilder pb;
            if (node->first) append_path(pb, eval_node(node->first.get(), src));
            if (node->second) append_path(pb, eval_node(node->second.get(), src));
            return pb.finish().value_or(Path{});
        }
    }
    return src;
}

bool node_produces_fill(const PathEffectNode* node) noexcept {
    if (!node) return false;
    if (node->kind == PathEffectKind::Path1D || node->kind == PathEffectKind::Path2D) return true;
    if (node->kind == PathEffectKind::Compose) {
        return node_produces_fill(node->first.get()) || node_produces_fill(node->second.get());
    }
    if (node->kind == PathEffectKind::Sum) {
        return node_produces_fill(node->first.get()) && node_produces_fill(node->second.get());
    }
    return false;
}

bool node_needs_clip(const PathEffectNode* node) noexcept {
    if (!node) return false;
    if (node->kind == PathEffectKind::Path2D) return true;
    if (node->kind == PathEffectKind::Compose || node->kind == PathEffectKind::Sum) {
        return node_needs_clip(node->first.get()) || node_needs_clip(node->second.get());
    }
    return false;
}

} // namespace

PathEffect PathEffect::corner(float radius) {
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Corner;
    n->radius = std::max(0.0f, radius);
    return PathEffect(std::move(n));
}

PathEffect PathEffect::trim(float start, float stop, bool inverted) {
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Trim;
    n->start = start;
    n->stop = stop;
    n->inverted = inverted;
    return PathEffect(std::move(n));
}

PathEffect PathEffect::discrete(float seg_length, float deviation, uint32_t seed) {
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Discrete;
    n->seg_length = std::max(0.0f, seg_length);
    n->deviation = deviation;
    n->seed = seed;
    return PathEffect(std::move(n));
}

PathEffect PathEffect::dash(std::vector<float> intervals, float phase) {
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Dash;
    n->intervals = std::move(intervals);
    n->phase = phase;
    return PathEffect(std::move(n));
}

PathEffect PathEffect::path_1d(const Path& stamp_path, float advance, float phase, Path1DStyle style) {
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Path1D;
    n->stamp_path = stamp_path;
    n->advance = advance;
    n->phase = phase;
    n->style_1d = style;
    return PathEffect(std::move(n));
}

PathEffect PathEffect::path_2d(const Transform& matrix, const Path& stamp_path) {
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Path2D;
    n->matrix_2d = matrix;
    n->stamp_path = stamp_path;
    return PathEffect(std::move(n));
}

PathEffect PathEffect::compose(const PathEffect& outer, const PathEffect& inner) {
    if (!outer.is_active()) return inner;
    if (!inner.is_active()) return outer;
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Compose;
    n->first = outer.node_;
    n->second = inner.node_;
    return PathEffect(std::move(n));
}

PathEffect PathEffect::sum(const PathEffect& first, const PathEffect& second) {
    if (!first.is_active()) return second;
    if (!second.is_active()) return first;
    auto n = std::make_shared<PathEffectNode>();
    n->kind = PathEffectKind::Sum;
    n->first = first.node_;
    n->second = second.node_;
    return PathEffect(std::move(n));
}

bool PathEffect::produces_fill_geometry() const noexcept {
    return node_produces_fill(node_.get());
}

bool PathEffect::needs_source_clip() const noexcept {
    return node_needs_clip(node_.get());
}

Path PathEffect::apply(const Path& src) const {
    return eval_node(node_.get(), src);
}

} // namespace nisaba::effects
