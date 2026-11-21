/// @file property.cpp
/// @brief Implementation of animatable properties, spatial/temporal Bezier evaluators, expressions, and transforms.

#include "nisaba/lottie/property.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/pipeline/simd.hpp"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <algorithm>
#include <charconv>

namespace nisaba::lottie {

namespace {

inline float lerp(float a, float b, float t) noexcept {
    return a + t * (b - a);
}

inline Point lerp(const Point& a, const Point& b, float t) noexcept {
    return Point::from_xy(lerp(a.x, b.x, t), lerp(a.y, b.y, t));
}

inline Color lerp(const Color& a, const Color& b, float t) noexcept {
    return Color::from_rgba_unchecked(
        lerp(a.red(), b.red(), t),
        lerp(a.green(), b.green(), t),
        lerp(a.blue(), b.blue(), t),
        lerp(a.alpha(), b.alpha(), t)
    );
}

inline BezierData lerp(const BezierData& a, const BezierData& b, float t) {
    return BezierData::interpolate(a, b, t);
}

inline std::vector<float> lerp(const std::vector<float>& a, const std::vector<float>& b, float t) {
    size_t count = std::min(a.size(), b.size());
    std::vector<float> res(count);
    for (size_t i = 0; i < count; ++i) {
        res[i] = lerp(a[i], b[i], t);
    }
    return res;
}

// Smooth deterministic 1D value noise for wiggle(freq, amp)
inline float hash_1d(int x) noexcept {
    uint32_t n = static_cast<uint32_t>(x) * 374761393u;
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return static_cast<float>(n & 0x0FFFFFFFu) / static_cast<float>(0x0FFFFFFFu) * 2.0f - 1.0f;
}

inline float smooth_noise_1d(float t, int seed_offset = 0) noexcept {
    int i0 = static_cast<int>(std::floor(t)) + seed_offset;
    float f = t - std::floor(t);
    float u = f * f * (3.0f - 2.0f * f);
    return lerp(hash_1d(i0), hash_1d(i0 + 1), u);
}

inline float add_wiggle(float val, float time_sec, float freq, float amp) noexcept {
    return val + smooth_noise_1d(time_sec * freq, 17) * amp;
}

inline Point add_wiggle(const Point& val, float time_sec, float freq, float amp) noexcept {
    return Point::from_xy(
        val.x + smooth_noise_1d(time_sec * freq, 17) * amp,
        val.y + smooth_noise_1d(time_sec * freq, 101) * amp
    );
}

inline Color add_wiggle(const Color& val, float, float, float) noexcept {
    return val;
}

inline BezierData add_wiggle(const BezierData& val, float, float, float) {
    return val;
}

inline std::vector<float> add_wiggle(const std::vector<float>& val, float, float, float) {
    return val;
}

inline float add_offset(float base, const float& start_v, const float& end_v, float cycles) noexcept {
    return base + (end_v - start_v) * cycles;
}

inline Point add_offset(Point base, const Point& start_v, const Point& end_v, float cycles) noexcept {
    return Point::from_xy(
        base.x + (end_v.x - start_v.x) * cycles,
        base.y + (end_v.y - start_v.y) * cycles
    );
}

template <typename T>
inline T add_offset(const T& base, const T&, const T&, float) {
    return base;
}

} // anonymous namespace

LottieExpression LottieExpression::parse(std::string_view expr_str) {
    LottieExpression expr;
    if (expr_str.empty()) return expr;

    if (expr_str.find("loopOut") != std::string_view::npos) {
        if (expr_str.find("pingpong") != std::string_view::npos) {
            expr.type = Type::LoopOutPingPong;
        } else if (expr_str.find("offset") != std::string_view::npos) {
            expr.type = Type::LoopOutOffset;
        } else {
            expr.type = Type::LoopOutCycle;
        }
    } else if (expr_str.find("loopIn") != std::string_view::npos) {
        if (expr_str.find("pingpong") != std::string_view::npos) {
            expr.type = Type::LoopInPingPong;
        } else {
            expr.type = Type::LoopInCycle;
        }
    } else if (auto w_pos = expr_str.find("wiggle("); w_pos != std::string_view::npos) {
        expr.type = Type::Wiggle;
        std::string_view args = expr_str.substr(w_pos + 7);
        while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) args.remove_prefix(1);
        float f = 2.0f, a = 10.0f;
        auto [p1, ec1] = std::from_chars(args.data(), args.data() + args.size(), f);
        if (ec1 == std::errc()) {
            expr.freq = f;
            std::string_view rem(p1, static_cast<size_t>((args.data() + args.size()) - p1));
            if (auto comma = rem.find(','); comma != std::string_view::npos) {
                rem.remove_prefix(comma + 1);
                while (!rem.empty() && (rem.front() == ' ' || rem.front() == '\t')) rem.remove_prefix(1);
                auto [p2, ec2] = std::from_chars(rem.data(), rem.data() + rem.size(), a);
                if (ec2 == std::errc()) expr.amp = a;
            }
        }
    } else if (expr_str.find("bounce") != std::string_view::npos || expr_str.find("inert") != std::string_view::npos) {
        expr.type = Type::InertialBounce;
    }

    return expr;
}

float solve_bezier_easing(float x, const EasingHandle& out_h, const EasingHandle& in_h) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    float x1 = std::clamp(out_h.x, 0.0f, 1.0f);
    float y1 = out_h.y;
    float x2 = std::clamp(in_h.x, 0.0f, 1.0f);
    float y2 = in_h.y;

    // Linear fast path
    if (std::abs(x1 - y1) < 1e-4f && std::abs(x2 - y2) < 1e-4f) {
        return x;
    }

    // Newton-Raphson + Binary search hybrid for fast & exact root finding
    float s = x;
    for (int iter = 0; iter < 5; ++iter) {
        float one_minus_s = 1.0f - s;
        float bx = 3.0f * one_minus_s * one_minus_s * s * x1 +
                   3.0f * one_minus_s * s * s * x2 +
                   s * s * s;
        float err = bx - x;
        if (std::abs(err) < 1e-5f) {
            return 3.0f * one_minus_s * one_minus_s * s * y1 +
                   3.0f * one_minus_s * s * s * y2 +
                   s * s * s;
        }
        float dbx = 3.0f * one_minus_s * one_minus_s * x1 +
                    6.0f * one_minus_s * s * (x2 - x1) +
                    3.0f * s * s * (1.0f - x2);
        if (std::abs(dbx) < 1e-5f) break;
        s = std::clamp(s - err / dbx, 0.0f, 1.0f);
    }

    float low = 0.0f;
    float high = 1.0f;
    s = x;
    for (int iter = 0; iter < 10; ++iter) {
        float one_minus_s = 1.0f - s;
        float bx = 3.0f * one_minus_s * one_minus_s * s * x1 +
                   3.0f * one_minus_s * s * s * x2 +
                   s * s * s;
        if (std::abs(bx - x) < 1e-5f) break;
        if (bx < x) low = s;
        else high = s;
        s = 0.5f * (low + high);
    }

    float one_minus_s = 1.0f - s;
    return 3.0f * one_minus_s * one_minus_s * s * y1 +
           3.0f * one_minus_s * s * s * y2 +
           s * s * s;
}

Point eval_spatial_bezier(
    Point p0, Point out_tan, Point in_tan, Point p1, float t, Point* out_derivative
) noexcept {
    Point cp1 = p0 + out_tan;
    Point cp2 = p1 + in_tan;

    auto eval_cubic = [&](float u) noexcept -> Point {
        float om = 1.0f - u;
        float om2 = om * om;
        float om3 = om2 * om;
        float u2 = u * u;
        float u3 = u2 * u;
        return Point::from_xy(
            om3 * p0.x + 3.0f * om2 * u * cp1.x + 3.0f * om * u2 * cp2.x + u3 * p1.x,
            om3 * p0.y + 3.0f * om2 * u * cp1.y + 3.0f * om * u2 * cp2.y + u3 * p1.y
        );
    };

    // 16-segment arc-length LUT to guarantee constant speed along spatial curve
    constexpr int NUM_SAMPLES = 16;
    float arc_lengths[NUM_SAMPLES + 1];
    arc_lengths[0] = 0.0f;
    Point prev_pt = p0;
    for (int i = 1; i <= NUM_SAMPLES; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(NUM_SAMPLES);
        Point cur_pt = eval_cubic(u);
        arc_lengths[i] = arc_lengths[i - 1] + prev_pt.distance(cur_pt);
        prev_pt = cur_pt;
    }

    float total_len = arc_lengths[NUM_SAMPLES];
    float u = std::clamp(t, 0.0f, 1.0f);
    if (total_len > 1e-4f) {
        float target_len = u * total_len;
        int idx = 0;
        while (idx < NUM_SAMPLES && arc_lengths[idx + 1] < target_len) {
            ++idx;
        }
        float seg_len = arc_lengths[idx + 1] - arc_lengths[idx];
        float frac = (seg_len > 1e-5f) ? ((target_len - arc_lengths[idx]) / seg_len) : 0.0f;
        u = (static_cast<float>(idx) + frac) / static_cast<float>(NUM_SAMPLES);
    }

    if (out_derivative) {
        float om = 1.0f - u;
        out_derivative->x = 3.0f * om * om * (cp1.x - p0.x) +
                            6.0f * om * u * (cp2.x - cp1.x) +
                            3.0f * u * u * (p1.x - cp2.x);
        out_derivative->y = 3.0f * om * om * (cp1.y - p0.y) +
                            6.0f * om * u * (cp2.y - cp1.y) +
                            3.0f * u * u * (p1.y - cp2.y);
    }

    return eval_cubic(u);
}

Path BezierData::to_path(bool reverse_direction) const {
    if (vertices.empty()) return Path();

    PathBuilder builder;
    size_t count = vertices.size();

    if (!reverse_direction) {
        builder.move_to(vertices[0].x, vertices[0].y);
        for (size_t i = 0; i + 1 < count; ++i) {
            Point cp1 = vertices[i] + out_tangents[i];
            Point cp2 = vertices[i + 1] + in_tangents[i + 1];
            Point p2 = vertices[i + 1];
            if (out_tangents[i].x == 0.0f && out_tangents[i].y == 0.0f &&
                in_tangents[i + 1].x == 0.0f && in_tangents[i + 1].y == 0.0f) {
                builder.line_to(p2.x, p2.y);
            } else {
                builder.cubic_to(cp1.x, cp1.y, cp2.x, cp2.y, p2.x, p2.y);
            }
        }
        if (closed && count > 1) {
            Point cp1 = vertices[count - 1] + out_tangents[count - 1];
            Point cp2 = vertices[0] + in_tangents[0];
            Point p0 = vertices[0];
            if (out_tangents[count - 1].x == 0.0f && out_tangents[count - 1].y == 0.0f &&
                in_tangents[0].x == 0.0f && in_tangents[0].y == 0.0f) {
                builder.line_to(p0.x, p0.y);
            } else {
                builder.cubic_to(cp1.x, cp1.y, cp2.x, cp2.y, p0.x, p0.y);
            }
            builder.close();
        }
    } else {
        // Reverse contour direction ("d": 3)
        builder.move_to(vertices[count - 1].x, vertices[count - 1].y);
        for (size_t i = count - 1; i > 0; --i) {
            Point cp1 = vertices[i] + in_tangents[i];
            Point cp2 = vertices[i - 1] + out_tangents[i - 1];
            Point p2 = vertices[i - 1];
            if (in_tangents[i].x == 0.0f && in_tangents[i].y == 0.0f &&
                out_tangents[i - 1].x == 0.0f && out_tangents[i - 1].y == 0.0f) {
                builder.line_to(p2.x, p2.y);
            } else {
                builder.cubic_to(cp1.x, cp1.y, cp2.x, cp2.y, p2.x, p2.y);
            }
        }
        if (closed && count > 1) {
            Point cp1 = vertices[0] + in_tangents[0];
            Point cp2 = vertices[count - 1] + out_tangents[count - 1];
            Point p_end = vertices[count - 1];
            if (in_tangents[0].x == 0.0f && in_tangents[0].y == 0.0f &&
                out_tangents[count - 1].x == 0.0f && out_tangents[count - 1].y == 0.0f) {
                builder.line_to(p_end.x, p_end.y);
            } else {
                builder.cubic_to(cp1.x, cp1.y, cp2.x, cp2.y, p_end.x, p_end.y);
            }
            builder.close();
        }
    }

    if (auto p = builder.finish()) {
        return *p;
    }
    return Path();
}

void BezierData::interpolate_into(const BezierData& a, const BezierData& b, float t, BezierData& out) {
    out.closed = a.closed || b.closed;
    size_t count = std::min(a.vertices.size(), b.vertices.size());
    out.vertices.resize(count);
    out.in_tangents.resize(count);
    out.out_tangents.resize(count);

    for (size_t i = 0; i < count; ++i) {
        out.vertices[i] = lerp(a.vertices[i], b.vertices[i], t);
        out.in_tangents[i] = lerp(a.in_tangents[i], b.in_tangents[i], t);
        out.out_tangents[i] = lerp(a.out_tangents[i], b.out_tangents[i], t);
    }
}

BezierData BezierData::interpolate(const BezierData& a, const BezierData& b, float t) {
    BezierData res;
    interpolate_into(a, b, t, res);
    return res;
}

template <typename T>
void Property<T>::bind_expression_manager(ExpressionManager& mgr) {
    if (raw_expression_.empty()) return;
    if constexpr (std::is_same_v<T, float>) {
        if (auto ev = mgr.create_number_expression_evaluator(raw_expression_)) {
            number_eval_ = std::move(ev);
        }
    } else if constexpr (std::is_same_v<T, Point> || std::is_same_v<T, Color> ||
                         std::is_same_v<T, std::vector<float>>) {
        if (auto ev = mgr.create_array_expression_evaluator(raw_expression_)) {
            array_eval_ = std::move(ev);
        }
    }
}

template <typename T>
T Property<T>::evaluate(float frame, float frame_rate) const {
    if (has_override_) {
        return override_value_;
    }

    float time_sec = (frame_rate > 0.0f) ? (frame / frame_rate) : (frame / 60.0f);

    // Pluggable ExpressionManager evaluator takes precedence over built-in/keyframe evaluation
    if constexpr (std::is_same_v<T, float>) {
        if (number_eval_) {
            return number_eval_->evaluate(time_sec);
        }
    } else if constexpr (std::is_same_v<T, Point>) {
        if (array_eval_) {
            const auto v = array_eval_->evaluate(time_sec);
            if (v.size() >= 2) return Point::from_xy(v[0], v[1]);
            if (v.size() == 1) return Point::from_xy(v[0], v[0]);
        }
    } else if constexpr (std::is_same_v<T, Color>) {
        if (array_eval_) {
            const auto v = array_eval_->evaluate(time_sec);
            if (v.size() >= 3) {
                float r = v[0], g = v[1], b = v[2];
                float a = (v.size() >= 4) ? v[3] : 1.0f;
                if (r > 1.0f || g > 1.0f || b > 1.0f) {
                    r /= 255.0f; g /= 255.0f; b /= 255.0f;
                    if (a > 1.0f) a /= 255.0f;
                }
                return Color::from_rgba_unchecked(
                    std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f),
                    std::clamp(b, 0.0f, 1.0f), std::clamp(a, 0.0f, 1.0f)
                );
            }
        }
    } else if constexpr (std::is_same_v<T, std::vector<float>>) {
        if (array_eval_) {
            return array_eval_->evaluate(time_sec);
        }
    }

    if (!is_animated_ || keyframes_.empty()) {
        if (expression_.type == LottieExpression::Type::Wiggle) {
            return add_wiggle(static_value_, time_sec, expression_.freq, expression_.amp);
        }
        return static_value_;
    }

    float first_t = keyframes_.front().time;
    float last_t = keyframes_.back().time;
    float span_total = last_t - first_t;
    float offset_cycles = 0.0f;

    // Handle loopIn / loopOut expressions
    if (span_total > 1e-4f) {
        if (frame > last_t) {
            if (expression_.type == LottieExpression::Type::LoopOutCycle) {
                frame = first_t + std::fmod(frame - first_t, span_total);
            } else if (expression_.type == LottieExpression::Type::LoopOutPingPong) {
                float cycle = (frame - first_t) / span_total;
                int c_int = static_cast<int>( std::floor(cycle) );
                float rem = cycle - static_cast<float>(c_int);
                frame = (c_int % 2 == 0) ? (first_t + rem * span_total) : (last_t - rem * span_total);
            } else if (expression_.type == LottieExpression::Type::LoopOutOffset) {
                offset_cycles = std::floor((frame - first_t) / span_total);
                frame = first_t + std::fmod(frame - first_t, span_total);
            }
        } else if (frame < first_t) {
            if (expression_.type == LottieExpression::Type::LoopInCycle) {
                float deficit = first_t - frame;
                frame = last_t - std::fmod(deficit, span_total);
            } else if (expression_.type == LottieExpression::Type::LoopInPingPong) {
                float cycle = (first_t - frame) / span_total;
                int c_int = static_cast<int>( std::floor(cycle) );
                float rem = cycle - static_cast<float>(c_int);
                frame = (c_int % 2 == 0) ? (first_t + rem * span_total) : (last_t - rem * span_total);
            }
        }
    }

    if (frame <= first_t) {
        T val = keyframes_.front().start_value;
        if (expression_.type == LottieExpression::Type::Wiggle) {
            return add_wiggle(val, time_sec, expression_.freq, expression_.amp);
        }
        return val;
    }
    if (frame >= last_t) {
        T val = keyframes_.back().end_value;
        if (expression_.type == LottieExpression::Type::Wiggle) {
            return add_wiggle(val, time_sec, expression_.freq, expression_.amp);
        }
        return val;
    }

    // Fast O(1) cursor hint check before falling back to binary search
    size_t idx = last_kf_hint_;
    if (idx + 1 >= keyframes_.size() || frame < keyframes_[idx].time || frame >= keyframes_[idx + 1].time) {
        if (idx + 2 < keyframes_.size() && frame >= keyframes_[idx + 1].time && frame < keyframes_[idx + 2].time) {
            idx = idx + 1;
        } else {
            auto it = std::lower_bound(
                keyframes_.begin(), keyframes_.end(), frame,
                [](const Keyframe<T>& kf, float f) { return kf.time < f; }
            );
            if (it == keyframes_.begin()) return it->start_value;
            idx = static_cast<size_t>((it - 1) - keyframes_.begin());
        }
        last_kf_hint_ = idx;
    }

    const auto& prev = keyframes_[idx];
    const auto& curr = keyframes_[idx + 1];

    if (prev.is_hold) {
        return prev.start_value;
    }

    float span = curr.time - prev.time;
    if (span <= 1e-4f) return prev.end_value;

    float tau = (frame - prev.time) / span;
    float eased = solve_bezier_easing(tau, prev.out_handle, prev.in_handle);

    T result{};
    if constexpr (std::is_same_v<T, Point>) {
        if (prev.has_spatial_tangents) {
            result = eval_spatial_bezier(
                prev.start_value, prev.spatial_out_tangent, prev.spatial_in_tangent, prev.end_value, eased
            );
        } else {
            result = lerp(prev.start_value, prev.end_value, eased);
        }
    } else {
        result = lerp(prev.start_value, prev.end_value, eased);
    }

    if (offset_cycles != 0.0f) {
        result = add_offset(result, keyframes_.front().start_value, keyframes_.back().end_value, offset_cycles);
    }
    if (expression_.type == LottieExpression::Type::Wiggle) {
        result = add_wiggle(result, time_sec, expression_.freq, expression_.amp);
    }
    return result;
}

template <typename T>
Point Property<T>::evaluate_tangent(float frame) const noexcept {
    if constexpr (!std::is_same_v<T, Point>) {
        (void)frame;
        return Point::from_xy(1.0f, 0.0f);
    } else {
        if (!is_animated_ || keyframes_.size() < 2) {
            return Point::from_xy(1.0f, 0.0f);
        }
        float clamped_f = std::clamp(frame, keyframes_.front().time, keyframes_.back().time - 1e-3f);
        auto it = std::lower_bound(
            keyframes_.begin(), keyframes_.end(), clamped_f,
            [](const Keyframe<Point>& kf, float f) { return kf.time < f; }
        );
        size_t idx = (it == keyframes_.begin()) ? 0 : static_cast<size_t>((it - 1) - keyframes_.begin());
        if (idx + 1 >= keyframes_.size()) idx = keyframes_.size() - 2;

        const auto& prev = keyframes_[idx];
        const auto& curr = keyframes_[idx + 1];
        float span = curr.time - prev.time;
        float tau = (span > 1e-4f) ? std::clamp((frame - prev.time) / span, 0.0f, 1.0f) : 0.0f;
        float eased = solve_bezier_easing(tau, prev.out_handle, prev.in_handle);

        if (prev.has_spatial_tangents) {
            Point deriv{1.0f, 0.0f};
            (void)eval_spatial_bezier(
                prev.start_value, prev.spatial_out_tangent, prev.spatial_in_tangent, prev.end_value, eased, &deriv
            );
            return deriv;
        }
        return Point::from_xy(prev.end_value.x - prev.start_value.x, prev.end_value.y - prev.start_value.y);
    }
}

// Explicit template instantiations
template class Property<float>;
template class Property<Point>;
template class Property<Color>;
template class Property<BezierData>;
template class Property<std::vector<float>>;

// Helpers for JSON parsing
namespace {

EasingHandle parse_easing_coord(const json::JsonValue* val, float def_x, float def_y) {
    EasingHandle h{def_x, def_y};
    if (!val) return h;

    if (val->is_object()) {
        const auto* xv = val->get("x");
        const auto* yv = val->get("y");
        if (xv) {
            if (xv->is_number()) h.x = xv->to_float(def_x);
            else if (xv->is_array() && !xv->as_array().empty()) h.x = xv->as_array()[0].to_float(def_x);
        }
        if (yv) {
            if (yv->is_number()) h.y = yv->to_float(def_y);
            else if (yv->is_array() && !yv->as_array().empty()) h.y = yv->as_array()[0].to_float(def_y);
        }
    } else if (val->is_array()) {
        const auto& arr = val->as_array();
        if (!arr.empty()) h.x = arr[0].to_float(def_x);
        if (arr.size() > 1) h.y = arr[1].to_float(def_y);
    } else if (val->is_number()) {
        h.x = val->to_float(def_x);
        h.y = val->to_float(def_y);
    }
    return h;
}

Color parse_raw_color(const json::JsonValue& val) {
    if (val.is_array()) {
        const auto& arr = val.as_array();
        float r = (arr.size() > 0) ? arr[0].to_float() : 0.0f;
        float g = (arr.size() > 1) ? arr[1].to_float() : 0.0f;
        float b = (arr.size() > 2) ? arr[2].to_float() : 0.0f;
        float a = (arr.size() > 3) ? arr[3].to_float() : 1.0f;
        if (r > 1.0f || g > 1.0f || b > 1.0f) {
            return Color::from_rgba8(
                static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(a > 1.0f ? a : a * 255.0f, 0.0f, 255.0f))
            );
        }
        return Color::from_rgba_unchecked(
            std::clamp(r, 0.0f, 1.0f),
            std::clamp(g, 0.0f, 1.0f),
            std::clamp(b, 0.0f, 1.0f),
            std::clamp(a, 0.0f, 1.0f)
        );
    }
    return Color::WHITE;
}

BezierData parse_raw_bezier(const json::JsonValue& val) {
    BezierData bz;
    if (!val.is_object()) return bz;

    bz.closed = val.get_bool("c", false);

    const auto& v_arr = val.get_array("v");
    const auto& i_arr = val.get_array("i");
    const auto& o_arr = val.get_array("o");

    size_t count = v_arr.size();
    bz.vertices.reserve(count);
    bz.in_tangents.reserve(count);
    bz.out_tangents.reserve(count);

    for (size_t idx = 0; idx < count; ++idx) {
        const auto& v = v_arr[idx].as_array();
        bz.vertices.push_back(Point::from_xy(
            v.size() > 0 ? v[0].to_float() : 0.0f,
            v.size() > 1 ? v[1].to_float() : 0.0f
        ));

        if (idx < i_arr.size()) {
            const auto& in_t = i_arr[idx].as_array();
            bz.in_tangents.push_back(Point::from_xy(
                in_t.size() > 0 ? in_t[0].to_float() : 0.0f,
                in_t.size() > 1 ? in_t[1].to_float() : 0.0f
            ));
        } else {
            bz.in_tangents.push_back(Point::zero());
        }

        if (idx < o_arr.size()) {
            const auto& out_t = o_arr[idx].as_array();
            bz.out_tangents.push_back(Point::from_xy(
                out_t.size() > 0 ? out_t[0].to_float() : 0.0f,
                out_t.size() > 1 ? out_t[1].to_float() : 0.0f
            ));
        } else {
            bz.out_tangents.push_back(Point::zero());
        }
    }
    return bz;
}

std::vector<float> parse_raw_float_array(const json::JsonValue& val) {
    std::vector<float> res;
    if (val.is_array()) {
        const auto& arr = val.as_array();
        res.reserve(arr.size());
        for (const auto& item : arr) {
            res.push_back(item.to_float(0.0f));
        }
    }
    return res;
}

template <typename PropT>
void parse_common_property_metadata(PropT& prop, const json::JsonValue& val) {
    if (!val.is_object()) return;
    if (const auto* sid = val.get("sid")) {
        if (sid->is_string()) prop.set_slot_id(std::string(sid->as_string()));
    }
    if (const auto* expr = val.get("x")) {
        if (expr->is_string()) {
            prop.set_raw_expression(std::string(expr->as_string()));
            prop.set_expression(LottieExpression::parse(expr->as_string()));
        }
    }
}

} // anonymous namespace

FloatProperty parse_float_property(const json::JsonValue& val, float default_val) {
    if (val.is_number()) return FloatProperty(val.to_float(default_val));
    if (!val.is_object()) return FloatProperty(default_val);

    int animated = val.get_int("a", 0);
    const auto* k_val = val.get("k");
    if (!k_val) return FloatProperty(default_val);

    // Check if k_val is actually an array of keyframe objects even when "a" is omitted
    if (animated == 0 && k_val->is_array() && !k_val->as_array().empty() && k_val->as_array()[0].is_object()) {
        animated = 1;
    }

    if (animated == 0) {
        FloatProperty prop(default_val);
        if (k_val->is_number()) prop.set_static(k_val->to_float(default_val));
        else if (k_val->is_array() && !k_val->as_array().empty()) {
            prop.set_static(k_val->as_array()[0].to_float(default_val));
        }
        parse_common_property_metadata(prop, val);
        return prop;
    }

    FloatProperty prop;
    parse_common_property_metadata(prop, val);
    if (k_val->is_array()) {
        const auto& arr = k_val->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr[i];
            if (!item.is_object()) continue;

            Keyframe<float> kf;
            kf.time = item.get_float("t", 0.0f);
            kf.is_hold = (item.get_int("h", 0) != 0);

            const auto* s = item.get("s");
            if (s && s->is_array() && !s->as_array().empty()) {
                kf.start_value = s->as_array()[0].to_float();
            } else if (s && s->is_number()) {
                kf.start_value = s->to_float();
            } else if (i > 0 && !prop.keyframes().empty()) {
                kf.start_value = prop.keyframes().back().end_value;
            }

            const auto* e = item.get("e");
            if (e && e->is_array() && !e->as_array().empty()) {
                kf.end_value = e->as_array()[0].to_float();
            } else if (e && e->is_number()) {
                kf.end_value = e->to_float();
            } else if (i + 1 < arr.size() && arr[i + 1].has("s")) {
                const auto* next_s = arr[i + 1].get("s");
                if (next_s && next_s->is_array() && !next_s->as_array().empty()) {
                    kf.end_value = next_s->as_array()[0].to_float();
                } else if (next_s && next_s->is_number()) {
                    kf.end_value = next_s->to_float();
                }
            } else {
                kf.end_value = kf.start_value;
            }

            kf.out_handle = parse_easing_coord(item.get("o"), 0.167f, 0.167f);
            kf.in_handle = parse_easing_coord(item.get("i"), 0.833f, 0.833f);

            prop.add_keyframe(std::move(kf));
        }
    }
    return prop;
}

PointProperty parse_point_property(const json::JsonValue& val, Point default_val) {
    if (val.is_array()) {
        const auto& a = val.as_array();
        return PointProperty(Point::from_xy(
            a.size() > 0 ? a[0].to_float() : default_val.x,
            a.size() > 1 ? a[1].to_float() : default_val.y
        ));
    }
    if (!val.is_object()) return PointProperty(default_val);

    int animated = val.get_int("a", 0);
    const auto* k_val = val.get("k");
    if (!k_val) return PointProperty(default_val);

    if (animated == 0 && k_val->is_array() && !k_val->as_array().empty() && k_val->as_array()[0].is_object()) {
        animated = 1;
    }

    if (animated == 0) {
        PointProperty prop(default_val);
        if (k_val->is_array()) {
            const auto& a = k_val->as_array();
            prop.set_static(Point::from_xy(
                a.size() > 0 ? a[0].to_float() : default_val.x,
                a.size() > 1 ? a[1].to_float() : default_val.y
            ));
        }
        parse_common_property_metadata(prop, val);
        return prop;
    }

    PointProperty prop;
    parse_common_property_metadata(prop, val);
    if (k_val->is_array()) {
        const auto& arr = k_val->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr[i];
            if (!item.is_object()) continue;

            Keyframe<Point> kf;
            kf.time = item.get_float("t", 0.0f);
            kf.is_hold = (item.get_int("h", 0) != 0);

            const auto* s = item.get("s");
            if (s && s->is_array()) {
                const auto& a = s->as_array();
                kf.start_value = Point::from_xy(
                    a.size() > 0 ? a[0].to_float() : 0.0f,
                    a.size() > 1 ? a[1].to_float() : 0.0f
                );
            } else if (i > 0 && !prop.keyframes().empty()) {
                kf.start_value = prop.keyframes().back().end_value;
            }

            const auto* e = item.get("e");
            if (e && e->is_array()) {
                const auto& a = e->as_array();
                kf.end_value = Point::from_xy(
                    a.size() > 0 ? a[0].to_float() : 0.0f,
                    a.size() > 1 ? a[1].to_float() : 0.0f
                );
            } else if (i + 1 < arr.size() && arr[i + 1].has("s")) {
                const auto* next_s = arr[i + 1].get("s");
                if (next_s && next_s->is_array()) {
                    const auto& a = next_s->as_array();
                    kf.end_value = Point::from_xy(
                        a.size() > 0 ? a[0].to_float() : 0.0f,
                        a.size() > 1 ? a[1].to_float() : 0.0f
                    );
                }
            } else {
                kf.end_value = kf.start_value;
            }

            // Spatial Bezier tangents ("to" and "ti")
            if (const auto* to_val = item.get("to"); to_val && to_val->is_array()) {
                const auto& a = to_val->as_array();
                kf.spatial_out_tangent = Point::from_xy(
                    a.size() > 0 ? a[0].to_float() : 0.0f,
                    a.size() > 1 ? a[1].to_float() : 0.0f
                );
                if (std::abs(kf.spatial_out_tangent.x) > 1e-4f || std::abs(kf.spatial_out_tangent.y) > 1e-4f) {
                    kf.has_spatial_tangents = true;
                }
            }
            if (const auto* ti_val = item.get("ti"); ti_val && ti_val->is_array()) {
                const auto& a = ti_val->as_array();
                kf.spatial_in_tangent = Point::from_xy(
                    a.size() > 0 ? a[0].to_float() : 0.0f,
                    a.size() > 1 ? a[1].to_float() : 0.0f
                );
                if (std::abs(kf.spatial_in_tangent.x) > 1e-4f || std::abs(kf.spatial_in_tangent.y) > 1e-4f) {
                    kf.has_spatial_tangents = true;
                }
            }

            kf.out_handle = parse_easing_coord(item.get("o"), 0.167f, 0.167f);
            kf.in_handle = parse_easing_coord(item.get("i"), 0.833f, 0.833f);

            prop.add_keyframe(std::move(kf));
        }
    }
    return prop;
}

PointProperty parse_scale_property(const json::JsonValue& val, Point default_val) {
    return parse_point_property(val, default_val);
}

ColorProperty parse_color_property(const json::JsonValue& val, Color default_val) {
    if (val.is_array()) {
        return ColorProperty(parse_raw_color(val));
    }
    if (!val.is_object()) return ColorProperty(default_val);

    int animated = val.get_int("a", 0);
    const auto* k_val = val.get("k");
    if (!k_val) return ColorProperty(default_val);

    if (animated == 0 && k_val->is_array() && !k_val->as_array().empty() && k_val->as_array()[0].is_object()) {
        animated = 1;
    }

    if (animated == 0) {
        ColorProperty prop(parse_raw_color(*k_val));
        parse_common_property_metadata(prop, val);
        return prop;
    }

    ColorProperty prop;
    parse_common_property_metadata(prop, val);
    if (k_val->is_array()) {
        const auto& arr = k_val->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr[i];
            if (!item.is_object()) continue;

            Keyframe<Color> kf;
            kf.time = item.get_float("t", 0.0f);
            kf.is_hold = (item.get_int("h", 0) != 0);

            if (const auto* s = item.get("s")) {
                kf.start_value = parse_raw_color(*s);
            } else if (i > 0 && !prop.keyframes().empty()) {
                kf.start_value = prop.keyframes().back().end_value;
            }

            if (const auto* e = item.get("e")) {
                kf.end_value = parse_raw_color(*e);
            } else if (i + 1 < arr.size() && arr[i + 1].has("s")) {
                if (const auto* next_s = arr[i + 1].get("s")) {
                    kf.end_value = parse_raw_color(*next_s);
                }
            } else {
                kf.end_value = kf.start_value;
            }

            kf.out_handle = parse_easing_coord(item.get("o"), 0.167f, 0.167f);
            kf.in_handle = parse_easing_coord(item.get("i"), 0.833f, 0.833f);

            prop.add_keyframe(std::move(kf));
        }
    }
    return prop;
}

ShapeProperty parse_shape_property(const json::JsonValue& val) {
    ShapeProperty prop;
    if (!val.is_object()) return prop;

    parse_common_property_metadata(prop, val);
    int animated = val.get_int("a", 0);
    const auto* k_val = val.get("k");
    if (!k_val) return prop;

    if (animated == 0 && k_val->is_array() && !k_val->as_array().empty() && k_val->as_array()[0].has("t")) {
        animated = 1;
    }

    if (animated == 0) {
        if (k_val->is_object()) {
            prop.set_static(parse_raw_bezier(*k_val));
        } else if (k_val->is_array() && !k_val->as_array().empty() && k_val->as_array()[0].is_object()) {
            prop.set_static(parse_raw_bezier(k_val->as_array()[0]));
        }
        return prop;
    }

    if (k_val->is_array()) {
        const auto& arr = k_val->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr[i];
            if (!item.is_object()) continue;

            Keyframe<BezierData> kf;
            kf.time = item.get_float("t", 0.0f);
            kf.is_hold = (item.get_int("h", 0) != 0);

            if (const auto* s = item.get("s")) {
                if (s->is_array() && !s->as_array().empty()) {
                    kf.start_value = parse_raw_bezier(s->as_array()[0]);
                } else if (s->is_object()) {
                    kf.start_value = parse_raw_bezier(*s);
                }
            } else if (i > 0 && !prop.keyframes().empty()) {
                kf.start_value = prop.keyframes().back().end_value;
            }

            if (const auto* e = item.get("e")) {
                if (e->is_array() && !e->as_array().empty()) {
                    kf.end_value = parse_raw_bezier(e->as_array()[0]);
                } else if (e->is_object()) {
                    kf.end_value = parse_raw_bezier(*e);
                }
            } else if (i + 1 < arr.size() && arr[i + 1].has("s")) {
                const auto* next_s = arr[i + 1].get("s");
                if (next_s && next_s->is_array() && !next_s->as_array().empty()) {
                    kf.end_value = parse_raw_bezier(next_s->as_array()[0]);
                } else if (next_s && next_s->is_object()) {
                    kf.end_value = parse_raw_bezier(*next_s);
                }
            } else {
                kf.end_value = kf.start_value;
            }

            kf.out_handle = parse_easing_coord(item.get("o"), 0.167f, 0.167f);
            kf.in_handle = parse_easing_coord(item.get("i"), 0.833f, 0.833f);

            prop.add_keyframe(std::move(kf));
        }
    }
    return prop;
}

FloatArrayProperty parse_float_array_property(const json::JsonValue& val) {
    FloatArrayProperty prop;
    if (val.is_array()) {
        prop.set_static(parse_raw_float_array(val));
        return prop;
    }
    if (!val.is_object()) return prop;

    parse_common_property_metadata(prop, val);
    int animated = val.get_int("a", 0);
    const auto* k_val = val.get("k");
    if (!k_val) return prop;

    if (animated == 0 && k_val->is_array() && !k_val->as_array().empty() && k_val->as_array()[0].is_object()) {
        animated = 1;
    }

    if (animated == 0) {
        prop.set_static(parse_raw_float_array(*k_val));
        return prop;
    }

    if (k_val->is_array()) {
        const auto& arr = k_val->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr[i];
            if (!item.is_object()) continue;

            Keyframe<std::vector<float>> kf;
            kf.time = item.get_float("t", 0.0f);
            kf.is_hold = (item.get_int("h", 0) != 0);

            if (const auto* s = item.get("s")) {
                kf.start_value = parse_raw_float_array(*s);
            } else if (i > 0 && !prop.keyframes().empty()) {
                kf.start_value = prop.keyframes().back().end_value;
            }

            if (const auto* e = item.get("e")) {
                kf.end_value = parse_raw_float_array(*e);
            } else if (i + 1 < arr.size() && arr[i + 1].has("s")) {
                if (const auto* next_s = arr[i + 1].get("s")) {
                    kf.end_value = parse_raw_float_array(*next_s);
                }
            } else {
                kf.end_value = kf.start_value;
            }

            kf.out_handle = parse_easing_coord(item.get("o"), 0.167f, 0.167f);
            kf.in_handle = parse_easing_coord(item.get("i"), 0.833f, 0.833f);

            prop.add_keyframe(std::move(kf));
        }
    }
    return prop;
}

LottieTransform LottieTransform::parse(const json::JsonValue& val) {
    LottieTransform tr;
    if (!val.is_object()) return tr;

    if (const auto* a = val.get("a")) tr.anchor = parse_point_property(*a, Point::zero());
    if (const auto* s = val.get("s")) tr.scale = parse_scale_property(*s, Point::from_xy(100.0f, 100.0f));
    if (const auto* r = val.get("r")) tr.rotation = parse_float_property(*r, 0.0f);
    else if (const auto* rz = val.get("rz")) tr.rotation = parse_float_property(*rz, 0.0f);
    if (const auto* o = val.get("o")) tr.opacity = parse_float_property(*o, 100.0f);
    if (const auto* sk = val.get("sk")) tr.skew = parse_float_property(*sk, 0.0f);
    if (const auto* sa = val.get("sa")) tr.skew_axis = parse_float_property(*sa, 0.0f);
    if (const auto* so = val.get("so")) tr.start_opacity = parse_float_property(*so, 100.0f);
    if (const auto* eo = val.get("eo")) tr.end_opacity = parse_float_property(*eo, 100.0f);

    // 3D rotation properties
    if (const auto* rx = val.get("rx")) { tr.rx = parse_float_property(*rx, 0.0f); tr.is_3d = true; }
    if (const auto* ry = val.get("ry")) { tr.ry = parse_float_property(*ry, 0.0f); tr.is_3d = true; }
    if (const auto* rz = val.get("rz")) { tr.rz = parse_float_property(*rz, 0.0f); }
    if (const auto* or_val = val.get("or")) { tr.orientation = parse_point_property(*or_val, Point::zero()); tr.is_3d = true; }

    // Position or split dimensions
    if (const auto* p_val = val.get("p")) {
        if (p_val->is_object() && p_val->get_bool("s", false)) {
            if (const auto* xp = p_val->get("x")) tr.x_pos = parse_float_property(*xp, 0.0f);
            if (const auto* yp = p_val->get("y")) tr.y_pos = parse_float_property(*yp, 0.0f);
            if (const auto* zp = p_val->get("z")) { tr.z_pos = parse_float_property(*zp, 0.0f); tr.is_3d = true; }
        } else {
            tr.position = parse_point_property(*p_val, Point::zero());
        }
    }

    return tr;
}

Transform LottieTransform::matrix(float frame, bool auto_orient) const {
    Point pos = position.evaluate(frame);
    if (x_pos.has_value()) pos.x = x_pos->evaluate(frame);
    if (y_pos.has_value()) pos.y = y_pos->evaluate(frame);

    Point anc = anchor.evaluate(frame);
    Point sc = scale.evaluate(frame);
    float rot_deg = rotation.evaluate(frame);
    if (auto_orient && !x_pos.has_value() && position.is_animated()) {
        Point tan = position.evaluate_tangent(frame);
        if (std::abs(tan.x) > 1e-5f || std::abs(tan.y) > 1e-5f) {
            rot_deg += std::atan2(tan.y, tan.x) * static_cast<float>(180.0 / M_PI);
        }
    }

    float sk_deg = skew.evaluate(frame);
    float sa_deg = skew_axis.evaluate(frame);

    Transform t = Transform::from_translate(pos.x, pos.y);
    if (std::abs(rot_deg) > 1e-4f) {
        t = t.pre_rotate(rot_deg);
    }

    // Apply skew and skew axis according to Bodymovin matrix specification
    if (std::abs(sk_deg) > 1e-4f) {
        if (std::abs(sa_deg) > 1e-4f) {
            t = t.pre_rotate(-sa_deg);
        }
        float sk_tan = std::tan(-sk_deg * static_cast<float>(M_PI / 180.0));
        t = t.pre_concat(Transform::from_skew(sk_tan, 0.0f));
        if (std::abs(sa_deg) > 1e-4f) {
            t = t.pre_rotate(sa_deg);
        }
    }

    t = t.pre_scale(sc.x * 0.01f, sc.y * 0.01f);
    t = t.pre_translate(-anc.x, -anc.y);

    return t;
}

Transform4x4 LottieTransform::matrix_3d(float frame, float comp_w, float comp_h) const {
    Point pos = position.evaluate(frame);
    if (x_pos.has_value()) pos.x = x_pos->evaluate(frame);
    if (y_pos.has_value()) pos.y = y_pos->evaluate(frame);
    float pz = z_pos.evaluate(frame);

    Point anc = anchor.evaluate(frame);
    Point sc = scale.evaluate(frame);
    float rot_x = rx.evaluate(frame);
    float rot_y = ry.evaluate(frame);
    float rot_z = rotation.evaluate(frame);
    Point ori = orientation.evaluate(frame);

    float cx = comp_w * 0.5f;
    float cy = comp_h * 0.5f;

    Transform4x4 m = Transform4x4::identity();
    m = m * Transform4x4::from_translate(pos.x, pos.y, pz);
    m = m * Transform4x4::from_translate(cx - pos.x, cy - pos.y, 0.0f);
    m = m * Transform4x4::from_perspective(1200.0f);
    m = m * Transform4x4::from_translate(-(cx - pos.x), -(cy - pos.y), 0.0f);

    if (std::abs(ori.x) > 1e-4f) m = m * Transform4x4::from_rotate_x(ori.x);
    if (std::abs(ori.y) > 1e-4f) m = m * Transform4x4::from_rotate_y(ori.y);
    if (std::abs(rot_x) > 1e-4f) m = m * Transform4x4::from_rotate_x(rot_x);
    if (std::abs(rot_y) > 1e-4f) m = m * Transform4x4::from_rotate_y(rot_y);
    if (std::abs(rot_z) > 1e-4f) m = m * Transform4x4::from_rotate_z(rot_z);

    m = m * Transform4x4::from_scale(sc.x * 0.01f, sc.y * 0.01f, 1.0f);
    m = m * Transform4x4::from_translate(-anc.x, -anc.y, 0.0f);
    return m;
}

float LottieTransform::eval_opacity(float frame) const {
    return std::clamp(opacity.evaluate(frame) * 0.01f, 0.0f, 1.0f);
}

bool LottieTransform::is_animated() const noexcept {
    return anchor.is_animated() || position.is_animated() || scale.is_animated() ||
           rotation.is_animated() || opacity.is_animated() || skew.is_animated() ||
           skew_axis.is_animated() || rx.is_animated() || ry.is_animated() ||
           (x_pos.has_value() && x_pos->is_animated()) ||
           (y_pos.has_value() && y_pos->is_animated());
}

void LottieTransform::bind_expression_manager(ExpressionManager& mgr) {
    anchor.bind_expression_manager(mgr);
    position.bind_expression_manager(mgr);
    scale.bind_expression_manager(mgr);
    rotation.bind_expression_manager(mgr);
    opacity.bind_expression_manager(mgr);
    skew.bind_expression_manager(mgr);
    skew_axis.bind_expression_manager(mgr);
    rx.bind_expression_manager(mgr);
    ry.bind_expression_manager(mgr);
    rz.bind_expression_manager(mgr);
    z_pos.bind_expression_manager(mgr);
    z_anchor.bind_expression_manager(mgr);
    z_scale.bind_expression_manager(mgr);
    orientation.bind_expression_manager(mgr);
    orientation_z.bind_expression_manager(mgr);
    if (x_pos) x_pos->bind_expression_manager(mgr);
    if (y_pos) y_pos->bind_expression_manager(mgr);
    start_opacity.bind_expression_manager(mgr);
    end_opacity.bind_expression_manager(mgr);
}

} // namespace nisaba::lottie
