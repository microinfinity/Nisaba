#pragma once

/// @file property.hpp
/// @brief Animatable properties, spatial/temporal Bezier evaluators, expressions, and transforms for Lottie.

#include "nisaba/lottie/lottie_types.hpp"
#include "nisaba/lottie/expression.hpp"
#include "nisaba/json/json.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/math/transform4x4.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/path/path.hpp"

#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nisaba::lottie {

struct EasingHandle {
    float x = 0.0f;
    float y = 0.0f;
};

/// Lightweight expression configuration parsed from After Effects "x" strings
struct LottieExpression {
    enum class Type : uint8_t {
        None = 0,
        LoopOutCycle,
        LoopOutPingPong,
        LoopOutOffset,
        LoopInCycle,
        LoopInPingPong,
        Wiggle,
        InertialBounce
    };

    Type type = Type::None;
    float freq = 2.0f;
    float amp = 10.0f;
    float decay = 4.0f;

    [[nodiscard]] bool is_active() const noexcept { return type != Type::None; }
    static LottieExpression parse(std::string_view expr_str);
};

template <typename T>
struct Keyframe {
    float time = 0.0f;         // Frame number
    T start_value{};
    T end_value{};
    EasingHandle out_handle{0.167f, 0.167f}; // "o" handle leaving start
    EasingHandle in_handle{0.833f, 0.833f};  // "i" handle entering end
    Point spatial_out_tangent{0.0f, 0.0f};   // "to" spatial out tangent (for Point properties)
    Point spatial_in_tangent{0.0f, 0.0f};    // "ti" spatial in tangent (for Point properties)
    bool has_spatial_tangents = false;
    bool is_hold = false;      // Hold keyframe (step)
};

// Shape/Bezier keyframe data
struct BezierData {
    std::vector<Point> vertices;
    std::vector<Point> in_tangents;   // Relative to vertex
    std::vector<Point> out_tangents;  // Relative to vertex
    bool closed = false;

    [[nodiscard]] Path to_path(bool reverse_direction = false) const;
    [[nodiscard]] static BezierData interpolate(const BezierData& a, const BezierData& b, float t);
    static void interpolate_into(const BezierData& a, const BezierData& b, float t, BezierData& out);
};

// Evaluates cubic bezier timing curve Y given normalized time X in [0, 1]
[[nodiscard]] float solve_bezier_easing(float x, const EasingHandle& out_h, const EasingHandle& in_h) noexcept;

// Evaluates spatial cubic bezier point with arc-length reparameterization
[[nodiscard]] Point eval_spatial_bezier(
    Point p0, Point out_tan, Point in_tan, Point p1, float t, Point* out_derivative = nullptr
) noexcept;

template <typename T>
class Property {
public:
    Property() = default;
    explicit Property(T static_val) : static_value_(std::move(static_val)), is_animated_(false) {}

    [[nodiscard]] bool is_animated() const noexcept {
        return !has_override_ &&
               (is_animated_ || expression_.type == LottieExpression::Type::Wiggle ||
                number_eval_ != nullptr || array_eval_ != nullptr);
    }
    [[nodiscard]] const std::vector<Keyframe<T>>& keyframes() const noexcept { return keyframes_; }
    [[nodiscard]] const std::string& slot_id() const noexcept { return slot_id_; }
    void set_slot_id(std::string sid) { slot_id_ = std::move(sid); }

    [[nodiscard]] const LottieExpression& expression() const noexcept { return expression_; }
    void set_expression(LottieExpression expr) noexcept { expression_ = expr; }

    [[nodiscard]] const std::string& raw_expression() const noexcept { return raw_expression_; }
    void set_raw_expression(std::string expr_str) { raw_expression_ = std::move(expr_str); }

    /// Binds a pluggable ExpressionEvaluator from `mgr` when `raw_expression_` is non-empty.
    void bind_expression_manager(ExpressionManager& mgr);

    void set_static(T val) {
        static_value_ = std::move(val);
        is_animated_ = false;
        keyframes_.clear();
    }

    void set_override(T val) {
        override_value_ = std::move(val);
        has_override_ = true;
    }

    void clear_override() noexcept {
        has_override_ = false;
    }

    [[nodiscard]] bool has_override() const noexcept { return has_override_; }

    void add_keyframe(Keyframe<T> kf) {
        keyframes_.push_back(std::move(kf));
        is_animated_ = true;
    }

    [[nodiscard]] T evaluate(float frame, float frame_rate = 60.0f) const;

    // Evaluates spatial tangent vector (for Point properties with auto-orient)
    [[nodiscard]] Point evaluate_tangent(float frame) const noexcept;

private:
    T static_value_{};
    T override_value_{};
    std::vector<Keyframe<T>> keyframes_;
    std::string slot_id_;
    std::string raw_expression_;
    LottieExpression expression_{};
    std::shared_ptr<ExpressionEvaluator<float>> number_eval_;
    std::shared_ptr<ExpressionEvaluator<std::vector<float>>> array_eval_;
    mutable size_t last_kf_hint_ = 0; // O(1) sequential frame evaluation cursor
    bool is_animated_ = false;
    bool has_override_ = false;
};

// Typed aliases
using FloatProperty = Property<float>;
using PointProperty = Property<Point>;
using ColorProperty = Property<Color>;
using ShapeProperty = Property<BezierData>;
using FloatArrayProperty = Property<std::vector<float>>;

// JSON parsing helper functions for properties
FloatProperty parse_float_property(const json::JsonValue& val, float default_val = 0.0f);
PointProperty parse_point_property(const json::JsonValue& val, Point default_val = Point::zero());
PointProperty parse_scale_property(const json::JsonValue& val, Point default_val = Point::from_xy(100.0f, 100.0f));
ColorProperty parse_color_property(const json::JsonValue& val, Color default_val = Color::WHITE);
ShapeProperty parse_shape_property(const json::JsonValue& val);
FloatArrayProperty parse_float_array_property(const json::JsonValue& val);

// Transform definition (anchor, position, scale, rotation, opacity, skew, 3D rotations)
struct LottieTransform {
    PointProperty anchor{Point::zero()};
    PointProperty position{Point::zero()};
    PointProperty scale{Point::from_xy(100.0f, 100.0f)};
    FloatProperty rotation{0.0f};
    FloatProperty opacity{100.0f};
    FloatProperty skew{0.0f};
    FloatProperty skew_axis{0.0f};

    // 3D Layer properties ("ddd": 1)
    FloatProperty rx{0.0f};
    FloatProperty ry{0.0f};
    FloatProperty rz{0.0f};
    FloatProperty z_pos{0.0f};
    FloatProperty z_anchor{0.0f};
    FloatProperty z_scale{100.0f};
    PointProperty orientation{Point::zero()};
    FloatProperty orientation_z{0.0f};
    bool is_3d = false;

    // Split position dimensions
    std::optional<FloatProperty> x_pos;
    std::optional<FloatProperty> y_pos;

    // Repeater start/end opacity
    FloatProperty start_opacity{100.0f};
    FloatProperty end_opacity{100.0f};
    bool auto_orient = false;

    static LottieTransform parse(const json::JsonValue& val);
    [[nodiscard]] Transform matrix(float frame, bool auto_orient = false) const;
    [[nodiscard]] Transform4x4 matrix_3d(float frame, float comp_w = 500.0f, float comp_h = 500.0f) const;
    [[nodiscard]] float eval_opacity(float frame) const;
    [[nodiscard]] bool is_animated() const noexcept;
    void bind_expression_manager(ExpressionManager& mgr);
};

} // namespace nisaba::lottie
