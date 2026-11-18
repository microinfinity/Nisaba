#include "nisaba/path/dash.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/path/path_geometry.hpp"

namespace nisaba {

namespace {

inline float adjust_dash_offset(float offset, float len) noexcept {
    if (offset < 0.0f) {
        offset = -offset;
        if (offset > len) {
            offset = std::fmod(offset, len);
        }
        offset = len - offset;
        if (offset == len) {
            offset = 0.0f;
        }
        return offset;
    } else if (offset >= len) {
        return std::fmod(offset, len);
    } else {
        return offset;
    }
}

inline std::pair<float, size_t> find_first_interval(const std::vector<float>& dash_array, float dash_offset) noexcept {
    for (size_t i = 0; i < dash_array.size(); ++i) {
        float gap = dash_array[i];
        if (dash_offset > gap || (dash_offset == gap && gap != 0.0f)) {
            dash_offset -= gap;
        } else {
            return {gap - dash_offset, i};
        }
    }
    return {dash_array[0], 0};
}

constexpr uint32_t MAX_T_VALUE = 0x3FFFFFFF;

enum class SegmentType : uint8_t {
    Line,
    Quad,
    Cubic,
};

struct Segment {
    float distance{0.0f};
    size_t point_index{0};
    uint32_t t_value{0};
    SegmentType kind{SegmentType::Line};

    float scalar_t() const noexcept {
        constexpr float MAX_T_RECIPROCAL = 1.0f / static_cast<float>(MAX_T_VALUE);
        return static_cast<float>(t_value) * MAX_T_RECIPROCAL;
    }
};

inline uint32_t t_span_big_enough(uint32_t t_span) noexcept {
    return t_span >> 10;
}

inline bool quad_too_curvy(Point p0, Point p1, Point p2, float tolerance) noexcept {
    float dx = scalar::half(p1.x) - scalar::half(scalar::half(p0.x + p2.x));
    float dy = scalar::half(p1.y) - scalar::half(scalar::half(p0.y + p2.y));
    float dist = std::max(std::abs(dx), std::abs(dy));
    return dist > tolerance;
}

inline float interp_safe(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

inline bool cheap_dist_exceeds_limit(Point pt, float x, float y, float tolerance) noexcept {
    float dist = std::max(std::abs(x - pt.x), std::abs(y - pt.y));
    return dist > tolerance;
}

inline bool cubic_too_curvy(Point p0, Point p1, Point p2, Point p3, float tolerance) noexcept {
    bool n0 = cheap_dist_exceeds_limit(
        p1,
        interp_safe(p0.x, p3.x, 1.0f / 3.0f),
        interp_safe(p0.y, p3.y, 1.0f / 3.0f),
        tolerance
    );
    bool n1 = cheap_dist_exceeds_limit(
        p2,
        interp_safe(p0.x, p3.x, 2.0f / 3.0f),
        interp_safe(p0.y, p3.y, 2.0f / 3.0f),
        tolerance
    );
    return n0 || n1;
}

inline float interp_scalar(float a, float b, NormalizedF32 t) noexcept {
    return a + (b - a) * t.get();
}

void compute_pos_tan(
    const Point* points,
    SegmentType seg_kind,
    NormalizedF32 t,
    Point* pos,
    Point* tangent
) noexcept {
    switch (seg_kind) {
        case SegmentType::Line: {
            if (pos) {
                pos->x = interp_scalar(points[0].x, points[1].x, t);
                pos->y = interp_scalar(points[0].y, points[1].y, t);
            }
            if (tangent) {
                tangent->set_normalize(points[1].x - points[0].x, points[1].y - points[0].y);
            }
            break;
        }
        case SegmentType::Quad: {
            if (pos) {
                *pos = path_geometry::eval_quad_at(points, t);
            }
            if (tangent) {
                *tangent = path_geometry::eval_quad_tangent_at(points, t);
                tangent->normalize();
            }
            break;
        }
        case SegmentType::Cubic: {
            if (pos) {
                *pos = path_geometry::eval_cubic_pos_at(points, t);
            }
            if (tangent) {
                *tangent = path_geometry::eval_cubic_tangent_at(points, t);
                tangent->normalize();
            }
            break;
        }
    }
}

void segment_to(
    const Point* points,
    SegmentType seg_kind,
    NormalizedF32 start_t,
    NormalizedF32 stop_t,
    PathBuilder& pb
) noexcept {
    if (start_t == stop_t) {
        auto pt = pb.last_point();
        if (pt) {
            pb.line_to(pt->x, pt->y);
        }
        return;
    }

    switch (seg_kind) {
        case SegmentType::Line: {
            if (stop_t == NormalizedF32::ONE) {
                pb.line_to(points[1].x, points[1].y);
            } else {
                pb.line_to(
                    interp_scalar(points[0].x, points[1].x, stop_t),
                    interp_scalar(points[0].y, points[1].y, stop_t)
                );
            }
            break;
        }
        case SegmentType::Quad: {
            Point tmp0[5];
            Point tmp1[5];
            if (start_t == NormalizedF32::ZERO) {
                if (stop_t == NormalizedF32::ONE) {
                    pb.quad_to(points[1].x, points[1].y, points[2].x, points[2].y);
                } else {
                    auto stop_ex = NormalizedF32Exclusive::create_bounded(stop_t.get());
                    path_geometry::chop_quad_at(points, stop_ex, tmp0);
                    pb.quad_to(tmp0[1].x, tmp0[1].y, tmp0[2].x, tmp0[2].y);
                }
            } else {
                auto start_ex = NormalizedF32Exclusive::create_bounded(start_t.get());
                path_geometry::chop_quad_at(points, start_ex, tmp0);
                if (stop_t == NormalizedF32::ONE) {
                    pb.quad_to(tmp0[3].x, tmp0[3].y, tmp0[4].x, tmp0[4].y);
                } else {
                    float new_t = (stop_t.get() - start_t.get()) / (1.0f - start_t.get());
                    auto new_t_ex = NormalizedF32Exclusive::create_bounded(new_t);
                    path_geometry::chop_quad_at(tmp0 + 2, new_t_ex, tmp1);
                    pb.quad_to(tmp1[1].x, tmp1[1].y, tmp1[2].x, tmp1[2].y);
                }
            }
            break;
        }
        case SegmentType::Cubic: {
            Point tmp0[7];
            Point tmp1[7];
            if (start_t == NormalizedF32::ZERO) {
                if (stop_t == NormalizedF32::ONE) {
                    pb.cubic_to(points[1].x, points[1].y, points[2].x, points[2].y, points[3].x, points[3].y);
                } else {
                    auto stop_ex = NormalizedF32Exclusive::create_bounded(stop_t.get());
                    path_geometry::chop_cubic_at2(points, stop_ex, tmp0);
                    pb.cubic_to(tmp0[1].x, tmp0[1].y, tmp0[2].x, tmp0[2].y, tmp0[3].x, tmp0[3].y);
                }
            } else {
                auto start_ex = NormalizedF32Exclusive::create_bounded(start_t.get());
                path_geometry::chop_cubic_at2(points, start_ex, tmp0);
                if (stop_t == NormalizedF32::ONE) {
                    pb.cubic_to(tmp0[4].x, tmp0[4].y, tmp0[5].x, tmp0[5].y, tmp0[6].x, tmp0[6].y);
                } else {
                    float new_t = (stop_t.get() - start_t.get()) / (1.0f - start_t.get());
                    auto new_t_ex = NormalizedF32Exclusive::create_bounded(new_t);
                    path_geometry::chop_cubic_at2(tmp0 + 3, new_t_ex, tmp1);
                    pb.cubic_to(tmp1[1].x, tmp1[1].y, tmp1[2].x, tmp1[2].y, tmp1[3].x, tmp1[3].y);
                }
            }
            break;
        }
    }
}

int32_t find_segment(const std::vector<Segment>& base, float key) noexcept {
    uint32_t lo = 0;
    uint32_t hi = static_cast<uint32_t>(base.size() - 1);

    while (lo < hi) {
        uint32_t mid = (hi + lo) >> 1;
        if (base[mid].distance < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (base[hi].distance < key) {
        hi += 1;
        hi = ~hi;
    } else if (key < base[hi].distance) {
        hi = ~hi;
    }

    return static_cast<int32_t>(hi);
}

struct ContourMeasure {
    std::vector<Segment> segments;
    std::vector<Point> points;
    float length{0.0f};
    bool is_closed{false};

    std::optional<std::pair<size_t, NormalizedF32>> distance_to_segment(float distance) const noexcept {
        int32_t index = find_segment(segments, distance);
        index ^= (index >> 31);
        size_t idx = static_cast<size_t>(index);
        Segment seg = segments[idx];

        float start_t = 0.0f;
        float start_d = 0.0f;
        if (idx > 0) {
            start_d = segments[idx - 1].distance;
            if (segments[idx - 1].point_index == seg.point_index) {
                start_t = segments[idx - 1].scalar_t();
            }
        }

        float t = start_t + (seg.scalar_t() - start_t) * (distance - start_d) / (seg.distance - start_d);
        auto norm_t = NormalizedF32::create(t);
        if (!norm_t) return std::nullopt;
        return std::make_pair(idx, *norm_t);
    }

    float compute_line_seg(Point p0, Point p1, float distance, size_t point_index) {
        float d = p0.distance(p1);
        float prev_d = distance;
        distance += d;
        if (distance > prev_d) {
            segments.push_back(Segment{distance, point_index, MAX_T_VALUE, SegmentType::Line});
        }
        return distance;
    }

    float compute_quad_segs(
        Point p0, Point p1, Point p2,
        float distance, uint32_t min_t, uint32_t max_t,
        size_t point_index, float tolerance
    ) {
        if (t_span_big_enough(max_t - min_t) != 0 && quad_too_curvy(p0, p1, p2, tolerance)) {
            Point tmp[5];
            uint32_t half_t = (min_t + max_t) >> 1;
            Point src[3] = {p0, p1, p2};
            path_geometry::chop_quad_at(src, NormalizedF32Exclusive::HALF, tmp);

            distance = compute_quad_segs(tmp[0], tmp[1], tmp[2], distance, min_t, half_t, point_index, tolerance);
            distance = compute_quad_segs(tmp[2], tmp[3], tmp[4], distance, half_t, max_t, point_index, tolerance);
        } else {
            float d = p0.distance(p2);
            float prev_d = distance;
            distance += d;
            if (distance > prev_d) {
                segments.push_back(Segment{distance, point_index, max_t, SegmentType::Quad});
            }
        }
        return distance;
    }

    float compute_cubic_segs(
        Point p0, Point p1, Point p2, Point p3,
        float distance, uint32_t min_t, uint32_t max_t,
        size_t point_index, float tolerance
    ) {
        if (t_span_big_enough(max_t - min_t) != 0 && cubic_too_curvy(p0, p1, p2, p3, tolerance)) {
            Point tmp[7];
            uint32_t half_t = (min_t + max_t) >> 1;
            Point src[4] = {p0, p1, p2, p3};
            path_geometry::chop_cubic_at2(src, NormalizedF32Exclusive::HALF, tmp);

            distance = compute_cubic_segs(tmp[0], tmp[1], tmp[2], tmp[3], distance, min_t, half_t, point_index, tolerance);
            distance = compute_cubic_segs(tmp[3], tmp[4], tmp[5], tmp[6], distance, half_t, max_t, point_index, tolerance);
        } else {
            float d = p0.distance(p3);
            float prev_d = distance;
            distance += d;
            if (distance > prev_d) {
                segments.push_back(Segment{distance, point_index, max_t, SegmentType::Cubic});
            }
        }
        return distance;
    }

    void push_segment(
        float start_d,
        float stop_d,
        bool start_with_move_to,
        PathBuilder& pb
    ) const {
        if (start_d < 0.0f) start_d = 0.0f;
        if (stop_d > length) stop_d = length;
        if (!(start_d <= stop_d) || segments.empty()) return;

        auto seg_pair = distance_to_segment(start_d);
        if (!seg_pair) return;
        auto [seg_index, start_t] = *seg_pair;
        Segment seg = segments[seg_index];

        auto stop_pair = distance_to_segment(stop_d);
        if (!stop_pair) return;
        auto [stop_seg_index, stop_t] = *stop_pair;
        Segment stop_seg = segments[stop_seg_index];

        if (start_with_move_to) {
            Point p;
            compute_pos_tan(points.data() + seg.point_index, seg.kind, start_t, &p, nullptr);
            pb.move_to(p.x, p.y);
        }

        if (seg.point_index == stop_seg.point_index) {
            segment_to(points.data() + seg.point_index, seg.kind, start_t, stop_t, pb);
        } else {
            size_t new_seg_index = seg_index;
            while (true) {
                segment_to(points.data() + seg.point_index, seg.kind, start_t, NormalizedF32::ONE, pb);

                size_t old_point_index = seg.point_index;
                while (true) {
                    new_seg_index++;
                    if (new_seg_index >= segments.size() || segments[new_seg_index].point_index != old_point_index) {
                        break;
                    }
                }
                if (new_seg_index >= segments.size()) break;
                seg = segments[new_seg_index];

                start_t = NormalizedF32::ZERO;
                if (seg.point_index >= stop_seg.point_index) {
                    break;
                }
            }

            segment_to(points.data() + seg.point_index, seg.kind, NormalizedF32::ZERO, stop_t, pb);
        }
    }
};

class ContourMeasureIter {
public:
    ContourMeasureIter(const Path& path, float res_scale) noexcept
        : iter_(path.segments()), tolerance_(0.5f * scalar::invert(res_scale)) {}

    std::optional<ContourMeasure> next() {
        ContourMeasure contour;
        // Pre-reserve to avoid realloc churn for typical paths
        contour.segments.reserve(128);
        contour.points.reserve(256);
        size_t point_index = 0;
        float distance = 0.0f;
        bool have_seen_close = false;
        Point prev_p = Point::zero();

        while (auto seg = iter_.next()) {
            switch (seg->type) {
                case PathSegment::Type::MoveTo: {
                    contour.points.push_back(seg->p0);
                    prev_p = seg->p0;
                    break;
                }
                case PathSegment::Type::LineTo: {
                    float prev_d = distance;
                    distance = contour.compute_line_seg(prev_p, seg->p0, distance, point_index);
                    if (distance > prev_d) {
                        contour.points.push_back(seg->p0);
                        point_index++;
                    }
                    prev_p = seg->p0;
                    break;
                }
                case PathSegment::Type::QuadTo: {
                    float prev_d = distance;
                    distance = contour.compute_quad_segs(
                        prev_p, seg->p0, seg->p1, distance, 0, MAX_T_VALUE, point_index, tolerance_
                    );
                    if (distance > prev_d) {
                        contour.points.push_back(seg->p0);
                        contour.points.push_back(seg->p1);
                        point_index += 2;
                    }
                    prev_p = seg->p1;
                    break;
                }
                case PathSegment::Type::CubicTo: {
                    float prev_d = distance;
                    distance = contour.compute_cubic_segs(
                        prev_p, seg->p0, seg->p1, seg->p2, distance, 0, MAX_T_VALUE, point_index, tolerance_
                    );
                    if (distance > prev_d) {
                        contour.points.push_back(seg->p0);
                        contour.points.push_back(seg->p1);
                        contour.points.push_back(seg->p2);
                        point_index += 3;
                    }
                    prev_p = seg->p2;
                    break;
                }
                case PathSegment::Type::Close: {
                    have_seen_close = true;
                    break;
                }
            }

            if (iter_.next_verb() == PathVerb::Move) {
                break;
            }
        }

        if (!std::isfinite(distance)) {
            return std::nullopt;
        }

        if (have_seen_close && !contour.points.empty()) {
            float prev_d = distance;
            Point first_pt = contour.points[0];
            distance = contour.compute_line_seg(contour.points[point_index], first_pt, distance, point_index);
            if (distance > prev_d) {
                contour.points.push_back(first_pt);
            }
        }

        contour.length = distance;
        contour.is_closed = have_seen_close;

        if (contour.points.empty()) {
            return std::nullopt;
        }

        return contour;
    }

private:
    PathSegmentsIter iter_;
    float tolerance_{0.5f};
};

} // anonymous namespace

std::optional<StrokeDash> StrokeDash::create(std::vector<float> dash_array, float dash_offset) noexcept {
    auto off = FiniteF32::create(dash_offset);
    if (!off) return std::nullopt;

    if (dash_array.empty()) {
        return std::nullopt;
    }

    for (float n : dash_array) {
        if (n < 0.0f || !std::isfinite(n)) {
            return std::nullopt;
        }
    }

    // Per CSS/SVG/Canvas conventions, odd-length non-empty dash patterns repeat to form even on/off pairs.
    if (dash_array.size() % 2 != 0) {
        size_t orig = dash_array.size();
        dash_array.reserve(orig * 2);
        for (size_t i = 0; i < orig; ++i) {
            dash_array.push_back(dash_array[i]);
        }
    }

    float sum = 0.0f;
    for (float n : dash_array) sum += n;

    auto int_len = NonZeroPositiveF32::create(sum);
    if (!int_len) return std::nullopt;

    float adj_offset = adjust_dash_offset(off->get(), int_len->get());
    auto [f_len, f_idx] = find_first_interval(dash_array, adj_offset);

    return StrokeDash(std::move(dash_array), adj_offset, *int_len, f_len, f_idx);
}

const Path* dash_path_fast(const Path& path, const StrokeDash& dash, float res_scale) {
    auto is_even = [](size_t x) { return x % 2 == 0; };

    PathBuilder pb;
    double dash_count = 0.0;
    constexpr size_t MAX_DASH_COUNT = 1000000;

    ContourMeasureIter measure_iter(path, res_scale);
    while (auto contour = measure_iter.next()) {
        bool skip_first_segment = contour->is_closed;
        bool added_segment = false;
        float length = contour->length;
        size_t index = dash.first_index();

        dash_count += static_cast<double>(length) * static_cast<double>(dash.array().size() >> 1) / static_cast<double>(dash.interval_len().get());
        if (dash_count > static_cast<double>(MAX_DASH_COUNT)) {
            return nullptr;
        }

        double distance = 0.0;
        float d_len = dash.first_len();

        while (distance < static_cast<double>(length)) {
            added_segment = false;
            if (is_even(index) && !skip_first_segment) {
                added_segment = true;
                contour->push_segment(
                    static_cast<float>(distance),
                    static_cast<float>(distance + d_len),
                    true,
                    pb
                );
            }

            distance += static_cast<double>(d_len);
            skip_first_segment = false;

            index++;
            if (index == dash.array().size()) {
                index = 0;
            }
            d_len = dash.array()[index];
        }

        if (contour->is_closed && is_even(dash.first_index()) && dash.first_len() >= 0.0f) {
            contour->push_segment(0.0f, dash.first_len(), !added_segment, pb);
        }
    }

    static thread_local Path tl_dash_path;
    pb.finish_into(tl_dash_path);
    if (tl_dash_path.is_empty()) return nullptr;
    return &tl_dash_path;
}

std::optional<Path> dash_path(const Path& path, const StrokeDash& dash, float res_scale) {
    if (const Path* fast = dash_path_fast(path, dash, res_scale)) {
        return *fast;
    }
    return std::nullopt;
}

} // namespace nisaba
