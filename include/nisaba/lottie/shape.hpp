#pragma once

/// @file shape.hpp
/// @brief Complete Lottie vector shapes, gradients, and path modifiers for Nisaba.

#include "nisaba/lottie/lottie_types.hpp"
#include "nisaba/lottie/property.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/shaders/shader.hpp"
#include "nisaba/math/rect.hpp"

#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nisaba::lottie {

class RenderContext;

class LottieShapeItem {
public:
    virtual ~LottieShapeItem() = default;

    std::string name;
    bool hidden = false;
    int direction = 1; // 1 = Clockwise (default), 3 = Counter-Clockwise (reversed)
    nisaba::BlendMode blend_mode = nisaba::BlendMode::SourceOver;

    [[nodiscard]] virtual std::string_view type_name() const noexcept = 0;
    [[nodiscard]] virtual bool is_geometry() const noexcept { return false; }
    [[nodiscard]] virtual bool is_modifier() const noexcept { return false; }
    [[nodiscard]] virtual bool is_style() const noexcept { return false; }
    [[nodiscard]] virtual bool is_animated() const noexcept { return false; }

    virtual void collect_geometry(float frame, Path& out_path) const { (void)frame; (void)out_path; }
    virtual void modify_paths(float frame, std::vector<Path>& paths) const { (void)frame; (void)paths; }
    virtual void render(RenderContext& ctx, float frame) const { (void)ctx; (void)frame; }
    [[nodiscard]] virtual std::optional<Rect> compute_bounds(float frame) const;
    [[nodiscard]] virtual uint64_t visual_fingerprint(float frame) const noexcept;
    [[nodiscard]] virtual size_t count_animators() const noexcept { return is_animated() ? 1 : 0; }
    void bind_expression_manager(ExpressionManager& mgr);

    static std::shared_ptr<LottieShapeItem> parse(const json::JsonValue& val);
};

// Shape Group ("gr")
class ShapeGroup : public LottieShapeItem {
public:
    std::vector<std::shared_ptr<LottieShapeItem>> items;
    LottieTransform transform;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "gr"; }
    [[nodiscard]] bool is_animated() const noexcept override;
    void render(RenderContext& ctx, float frame) const override;
    void collect_geometry(float frame, Path& out_path) const override;
    [[nodiscard]] std::optional<Rect> compute_bounds(float frame) const override;
    [[nodiscard]] uint64_t visual_fingerprint(float frame) const noexcept override;
    [[nodiscard]] size_t count_animators() const noexcept override;

    static std::shared_ptr<ShapeGroup> parse(const json::JsonValue& val);
};

// Freeform Path Shape ("sh")
class PathShape : public LottieShapeItem {
public:
    ShapeProperty shape;
    Path cached_static_path;
    bool has_cached_static = false;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "sh"; }
    [[nodiscard]] bool is_geometry() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override { return shape.is_animated(); }
    void collect_geometry(float frame, Path& out_path) const override;

    static std::shared_ptr<PathShape> parse(const json::JsonValue& val);
};

// Rectangle Shape ("rc")
class RectShape : public LottieShapeItem {
public:
    PointProperty position{Point::zero()};
    PointProperty size{Point::zero()};
    FloatProperty roundness{0.0f};
    Path cached_static_path;
    bool has_cached_static = false;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "rc"; }
    [[nodiscard]] bool is_geometry() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return position.is_animated() || size.is_animated() || roundness.is_animated();
    }
    void collect_geometry(float frame, Path& out_path) const override;

    static std::shared_ptr<RectShape> parse(const json::JsonValue& val);
};

// Ellipse Shape ("el")
class EllipseShape : public LottieShapeItem {
public:
    PointProperty position{Point::zero()};
    PointProperty size{Point::zero()};
    Path cached_static_path;
    bool has_cached_static = false;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "el"; }
    [[nodiscard]] bool is_geometry() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return position.is_animated() || size.is_animated();
    }
    void collect_geometry(float frame, Path& out_path) const override;

    static std::shared_ptr<EllipseShape> parse(const json::JsonValue& val);
};

// Star / Polygon Shape ("sr")
class StarShape : public LottieShapeItem {
public:
    int type = 1; // 1 = Star, 2 = Polygon
    FloatProperty points{5.0f};
    PointProperty position{Point::zero()};
    FloatProperty rotation{0.0f};
    FloatProperty inner_radius{0.0f};
    FloatProperty inner_roundness{0.0f};
    FloatProperty outer_radius{0.0f};
    FloatProperty outer_roundness{0.0f};
    Path cached_static_path;
    bool has_cached_static = false;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "sr"; }
    [[nodiscard]] bool is_geometry() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return points.is_animated() || position.is_animated() || rotation.is_animated() ||
               inner_radius.is_animated() || inner_roundness.is_animated() ||
               outer_radius.is_animated() || outer_roundness.is_animated();
    }
    void collect_geometry(float frame, Path& out_path) const override;

    static std::shared_ptr<StarShape> parse(const json::JsonValue& val);
};

// Solid Fill ("fl")
class FillShape : public LottieShapeItem {
public:
    ColorProperty color{Color::WHITE};
    FloatProperty opacity{100.0f};
    FillRule fill_rule = FillRule::NonZero;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "fl"; }
    [[nodiscard]] bool is_style() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return color.is_animated() || opacity.is_animated();
    }
    void render(RenderContext& ctx, float frame) const override;

    static std::shared_ptr<FillShape> parse(const json::JsonValue& val);
};

// Solid Stroke ("st")
class StrokeShape : public LottieShapeItem {
public:
    ColorProperty color{Color::WHITE};
    FloatProperty opacity{100.0f};
    FloatProperty width{1.0f};
    LineCap cap = LineCap::Round;
    LineJoin join = LineJoin::Round;
    float miter_limit = 4.0f;
    std::vector<FloatProperty> dashes;
    FloatProperty dash_offset{0.0f};

    [[nodiscard]] std::string_view type_name() const noexcept override { return "st"; }
    [[nodiscard]] bool is_style() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override;
    void render(RenderContext& ctx, float frame) const override;

    static std::shared_ptr<StrokeShape> parse(const json::JsonValue& val);
};

// Gradient Stop Ramp ("g" inside "gf" and "gs")
struct GradientColorStops {
    int color_count = 0; // "p": Number of RGB color stops
    FloatArrayProperty raw_data;

    static GradientColorStops parse(const json::JsonValue& val);
    void evaluate_stops_into(float frame, float master_opacity, std::vector<GradientStop>& out_stops) const;
    [[nodiscard]] std::vector<GradientStop> evaluate_stops(float frame, float master_opacity = 1.0f) const;
    [[nodiscard]] bool is_animated() const noexcept { return raw_data.is_animated(); }
};

// Gradient Fill ("gf")
class GradientFillShape : public LottieShapeItem {
public:
    GradientType gradient_type = GradientType::Linear;
    PointProperty start_point{Point::zero()};
    PointProperty end_point{Point::from_xy(100.0f, 0.0f)};
    FloatProperty highlight_length{0.0f}; // "h" in [-100, 100]%
    FloatProperty highlight_angle{0.0f};  // "a" in degrees
    GradientColorStops stops;
    FloatProperty opacity{100.0f};
    FillRule fill_rule = FillRule::NonZero;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "gf"; }
    [[nodiscard]] bool is_style() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return start_point.is_animated() || end_point.is_animated() ||
               highlight_length.is_animated() || highlight_angle.is_animated() ||
               stops.is_animated() || opacity.is_animated();
    }
    void render(RenderContext& ctx, float frame) const override;

    static std::shared_ptr<GradientFillShape> parse(const json::JsonValue& val);
};

// Gradient Stroke ("gs")
class GradientStrokeShape : public LottieShapeItem {
public:
    GradientType gradient_type = GradientType::Linear;
    PointProperty start_point{Point::zero()};
    PointProperty end_point{Point::from_xy(100.0f, 0.0f)};
    FloatProperty highlight_length{0.0f};
    FloatProperty highlight_angle{0.0f};
    GradientColorStops stops;
    FloatProperty opacity{100.0f};
    FloatProperty width{1.0f};
    LineCap cap = LineCap::Round;
    LineJoin join = LineJoin::Round;
    float miter_limit = 4.0f;
    std::vector<FloatProperty> dashes;
    FloatProperty dash_offset{0.0f};

    [[nodiscard]] std::string_view type_name() const noexcept override { return "gs"; }
    [[nodiscard]] bool is_style() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override;
    void render(RenderContext& ctx, float frame) const override;

    static std::shared_ptr<GradientStrokeShape> parse(const json::JsonValue& val);
};

// Trim Path Modifier ("tm")
class TrimPathShape : public LottieShapeItem {
public:
    FloatProperty start{0.0f};    // [0, 100]%
    FloatProperty end{100.0f};    // [0, 100]%
    FloatProperty offset{0.0f};   // degrees (-360..360)
    TrimType trim_type = TrimType::Simultaneously;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "tm"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return start.is_animated() || end.is_animated() || offset.is_animated();
    }

    [[nodiscard]] Path apply_trim(const Path& in_path, float frame) const;
    void modify_paths(float frame, std::vector<Path>& paths) const override;

    static std::shared_ptr<TrimPathShape> parse(const json::JsonValue& val);
};

// Merge Paths Modifier ("mm")
class MergePathsShape : public LottieShapeItem {
public:
    MergeMode mode = MergeMode::Merge;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "mm"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    void modify_paths(float frame, std::vector<Path>& paths) const override;

    static std::shared_ptr<MergePathsShape> parse(const json::JsonValue& val);
};

// Round Corners Modifier ("rd")
class RoundCornersShape : public LottieShapeItem {
public:
    FloatProperty radius{0.0f};

    [[nodiscard]] std::string_view type_name() const noexcept override { return "rd"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override { return radius.is_animated(); }

    [[nodiscard]] static Path round_path_corners(const Path& in_path, float r);
    void modify_paths(float frame, std::vector<Path>& paths) const override;

    static std::shared_ptr<RoundCornersShape> parse(const json::JsonValue& val);
};

// Offset Paths Modifier ("op")
class OffsetPathShape : public LottieShapeItem {
public:
    FloatProperty amount{0.0f};
    LineJoin line_join = LineJoin::Miter;
    FloatProperty miter_limit{4.0f};

    [[nodiscard]] std::string_view type_name() const noexcept override { return "op"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return amount.is_animated() || miter_limit.is_animated();
    }
    void modify_paths(float frame, std::vector<Path>& paths) const override;

    static std::shared_ptr<OffsetPathShape> parse(const json::JsonValue& val);
};

// Pucker & Bloat Modifier ("pb")
class PuckerBloatShape : public LottieShapeItem {
public:
    FloatProperty amount{0.0f}; // [-100, 100]%

    [[nodiscard]] std::string_view type_name() const noexcept override { return "pb"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override { return amount.is_animated(); }
    void modify_paths(float frame, std::vector<Path>& paths) const override;

    static std::shared_ptr<PuckerBloatShape> parse(const json::JsonValue& val);
};

// Zig Zag Modifier ("zz")
class ZigZagShape : public LottieShapeItem {
public:
    FloatProperty size{10.0f};     // Amplitude ("s")
    FloatProperty ridges{5.0f};    // Ridges per segment ("r")
    int point_type = 1;            // 1 = Corner, 2 = Smooth ("pt")

    [[nodiscard]] std::string_view type_name() const noexcept override { return "zz"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return size.is_animated() || ridges.is_animated();
    }
    void modify_paths(float frame, std::vector<Path>& paths) const override;

    static std::shared_ptr<ZigZagShape> parse(const json::JsonValue& val);
};

// Repeater Modifier ("rp")
class RepeaterShape : public LottieShapeItem {
public:
    FloatProperty copies{1.0f};
    FloatProperty offset{0.0f};
    CompositeOrder composite = CompositeOrder::Above;
    LottieTransform transform;

    [[nodiscard]] std::string_view type_name() const noexcept override { return "rp"; }
    [[nodiscard]] bool is_modifier() const noexcept override { return true; }
    [[nodiscard]] bool is_animated() const noexcept override {
        return copies.is_animated() || offset.is_animated() || transform.is_animated();
    }

    static std::shared_ptr<RepeaterShape> parse(const json::JsonValue& val);
};

// Context passed through shape tree rendering (with reusable scratch buffers for 0-heap steady state)
class RenderContext {
public:
    explicit RenderContext(ICanvas& c, float parent_op = 1.0f)
        : canvas(c), parent_opacity(parent_op) {
        active_paths.reserve(16);
        scratch_stops.reserve(16);
        scratch_dashes.reserve(8);
    }

    ICanvas& canvas;
    float parent_opacity = 1.0f;
    nisaba::BlendMode active_blend_mode = nisaba::BlendMode::SourceOver;
    std::vector<std::shared_ptr<TrimPathShape>> active_trims;
    std::vector<Path> active_paths;
    Path accumulated_path;

    // Reusable scratch buffers
    mutable std::vector<GradientStop> scratch_stops;
    mutable std::vector<float> scratch_dashes;

    void add_path(const Path& p) {
        active_paths.push_back(p);
        accumulated_path.add_path(p);
    }

    [[nodiscard]] Path get_trimmed_path(float frame) const;
};

/// Evaluates and renders a flat list of Lottie shape items (used by both ShapeGroup and root ShapeLayer).
void render_shape_items(
    const std::vector<std::shared_ptr<LottieShapeItem>>& items,
    RenderContext& ctx,
    float frame
);

/// Computes tight bounds of a flat list of Lottie shape items (handling repeaters, modifiers, and stroke pads).
[[nodiscard]] std::optional<Rect> compute_shape_items_bounds(
    const std::vector<std::shared_ptr<LottieShapeItem>>& items,
    float frame
);

} // namespace nisaba::lottie
