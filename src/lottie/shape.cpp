/// @file shape.cpp
/// @brief Implementation of Lottie shape primitives, gradients, path modifiers, and rendering.

#include "nisaba/lottie/shape.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/path/path_measure.hpp"
#include "nisaba/path/path_ops.hpp"
#include "nisaba/math/transform.hpp"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <algorithm>
#include <cstring>

namespace nisaba::lottie {

namespace {

constexpr float KAPPA = 0.5522847498307935f;

void parse_common_item_fields(LottieShapeItem& item, const json::JsonValue& val) {
    item.name = val.get_string("nm", "");
    item.hidden = val.get_bool("hd", false);
    item.direction = val.get_int("d", 1);
    item.blend_mode = map_lottie_blend_mode(val.get_int("bm", 0));
}

} // anonymous namespace

// ── PathShape ("sh") ──────────────────────────────────────────────────────

void PathShape::collect_geometry(float frame, Path& out_path) const {
    if (hidden) return;
    if (!is_animated() && has_cached_static) {
        out_path.add_path(cached_static_path);
        return;
    }
    BezierData bz = shape.evaluate(frame);
    Path p = bz.to_path(direction == 3);
    out_path.add_path(p);
}

std::shared_ptr<PathShape> PathShape::parse(const json::JsonValue& val) {
    auto ps = std::make_shared<PathShape>();
    parse_common_item_fields(*ps, val);
    if (const auto* ks = val.get("ks")) {
        ps->shape = parse_shape_property(*ks);
    }
    if (!ps->is_animated()) {
        ps->cached_static_path = ps->shape.evaluate(0.0f).to_path(ps->direction == 3);
        ps->has_cached_static = true;
    }
    return ps;
}

// ── RectShape ("rc") ──────────────────────────────────────────────────────

void RectShape::collect_geometry(float frame, Path& out_path) const {
    if (hidden) return;
    if (!is_animated() && has_cached_static) {
        out_path.add_path(cached_static_path);
        return;
    }

    Point pos = position.evaluate(frame);
    Point sz = size.evaluate(frame);
    float r = roundness.evaluate(frame);

    if (sz.x <= 0.0f || sz.y <= 0.0f) return;

    float x0 = pos.x - sz.x * 0.5f;
    float y0 = pos.y - sz.y * 0.5f;
    float x1 = pos.x + sz.x * 0.5f;
    float y1 = pos.y + sz.y * 0.5f;

    r = std::clamp(r, 0.0f, std::min(sz.x, sz.y) * 0.5f);

    PathBuilder builder;
    if (direction == 3) {
        // Counter-clockwise winding
        if (r <= 0.0f) {
            builder.move_to(x1, y0);
            builder.line_to(x0, y0);
            builder.line_to(x0, y1);
            builder.line_to(x1, y1);
            builder.close();
        } else {
            float ox = r * KAPPA;
            float oy = r * KAPPA;
            builder.move_to(x1, y0 + r);
            builder.cubic_to(x1, y0 + r - oy, x1 - r + ox, y0, x1 - r, y0);
            builder.line_to(x0 + r, y0);
            builder.cubic_to(x0 + r - ox, y0, x0, y0 + r - oy, x0, y0 + r);
            builder.line_to(x0, y1 - r);
            builder.cubic_to(x0, y1 - r + oy, x0 + r - ox, y1, x0 + r, y1);
            builder.line_to(x1 - r, y1);
            builder.cubic_to(x1 - r + ox, y1, x1, y1 - r + oy, x1, y1 - r);
            builder.close();
        }
    } else {
        // Clockwise winding (Lottie standard starts at top-right corner for Rect)
        if (r <= 0.0f) {
            builder.move_to(x0, y0);
            builder.line_to(x1, y0);
            builder.line_to(x1, y1);
            builder.line_to(x0, y1);
            builder.close();
        } else {
            float ox = r * KAPPA;
            float oy = r * KAPPA;
            builder.move_to(x0 + r, y0);
            builder.line_to(x1 - r, y0);
            builder.cubic_to(x1 - r + ox, y0, x1, y0 + r - oy, x1, y0 + r);
            builder.line_to(x1, y1 - r);
            builder.cubic_to(x1, y1 - r + oy, x1 - r + ox, y1, x1 - r, y1);
            builder.line_to(x0 + r, y1);
            builder.cubic_to(x0 + r - ox, y1, x0, y1 - r + oy, x0, y1 - r);
            builder.line_to(x0, y0 + r);
            builder.cubic_to(x0, y0 + r - oy, x0 + r - ox, y0, x0 + r, y0);
            builder.close();
        }
    }

    if (auto p = builder.finish()) {
        out_path.add_path(*p);
    }
}

std::shared_ptr<RectShape> RectShape::parse(const json::JsonValue& val) {
    auto rs = std::make_shared<RectShape>();
    parse_common_item_fields(*rs, val);
    if (const auto* p = val.get("p")) rs->position = parse_point_property(*p, Point::zero());
    if (const auto* s = val.get("s")) rs->size = parse_point_property(*s, Point::zero());
    if (const auto* r = val.get("r")) rs->roundness = parse_float_property(*r, 0.0f);
    if (!rs->is_animated()) {
        rs->collect_geometry(0.0f, rs->cached_static_path);
        rs->has_cached_static = true;
    }
    return rs;
}

// ── EllipseShape ("el") ───────────────────────────────────────────────────

void EllipseShape::collect_geometry(float frame, Path& out_path) const {
    if (hidden) return;
    if (!is_animated() && has_cached_static) {
        out_path.add_path(cached_static_path);
        return;
    }

    Point pos = position.evaluate(frame);
    Point sz = size.evaluate(frame);

    if (sz.x <= 0.0f || sz.y <= 0.0f) return;

    float rx = sz.x * 0.5f;
    float ry = sz.y * 0.5f;
    float ox = rx * KAPPA;
    float oy = ry * KAPPA;

    PathBuilder builder;
    if (direction == 3) {
        builder.move_to(pos.x, pos.y - ry);
        builder.cubic_to(pos.x - ox, pos.y - ry, pos.x - rx, pos.y - oy, pos.x - rx, pos.y);
        builder.cubic_to(pos.x - rx, pos.y + oy, pos.x - ox, pos.y + ry, pos.x, pos.y + ry);
        builder.cubic_to(pos.x + ox, pos.y + ry, pos.x + rx, pos.y + oy, pos.x + rx, pos.y);
        builder.cubic_to(pos.x + rx, pos.y - oy, pos.x + ox, pos.y - ry, pos.x, pos.y - ry);
        builder.close();
    } else {
        builder.move_to(pos.x, pos.y - ry);
        builder.cubic_to(pos.x + ox, pos.y - ry, pos.x + rx, pos.y - oy, pos.x + rx, pos.y);
        builder.cubic_to(pos.x + rx, pos.y + oy, pos.x + ox, pos.y + ry, pos.x, pos.y + ry);
        builder.cubic_to(pos.x - ox, pos.y + ry, pos.x - rx, pos.y + oy, pos.x - rx, pos.y);
        builder.cubic_to(pos.x - rx, pos.y - oy, pos.x - ox, pos.y - ry, pos.x, pos.y - ry);
        builder.close();
    }

    if (auto p = builder.finish()) {
        out_path.add_path(*p);
    }
}

std::shared_ptr<EllipseShape> EllipseShape::parse(const json::JsonValue& val) {
    auto es = std::make_shared<EllipseShape>();
    parse_common_item_fields(*es, val);
    if (const auto* p = val.get("p")) es->position = parse_point_property(*p, Point::zero());
    if (const auto* s = val.get("s")) es->size = parse_point_property(*s, Point::zero());
    if (!es->is_animated()) {
        es->collect_geometry(0.0f, es->cached_static_path);
        es->has_cached_static = true;
    }
    return es;
}

// ── StarShape ("sr") ──────────────────────────────────────────────────────

void StarShape::collect_geometry(float frame, Path& out_path) const {
    if (hidden) return;
    if (!is_animated() && has_cached_static) {
        out_path.add_path(cached_static_path);
        return;
    }

    Point pos = position.evaluate(frame);
    float num_pts = std::max(3.0f, points.evaluate(frame));
    float rot_rad = (rotation.evaluate(frame) - 90.0f) * static_cast<float>(M_PI / 180.0);
    float orad = outer_radius.evaluate(frame);
    float irad = (type == 1) ? inner_radius.evaluate(frame) : orad;
    float oround = outer_roundness.evaluate(frame) * 0.01f;
    float iround = (type == 1) ? inner_roundness.evaluate(frame) * 0.01f : oround;

    if (orad <= 0.0f) return;

    int total_vertices = (type == 1) ? static_cast<int>( std::round(num_pts) * 2.0f ) : static_cast<int>(std::round(num_pts));
    if (total_vertices < 3) total_vertices = 3;
    float dir_sign = (direction == 3) ? -1.0f : 1.0f;
    float angle_step = dir_sign * static_cast<float>(2.0 * M_PI) / static_cast<float>(total_vertices);

    bool has_roundness = (std::abs(oround) > 1e-4f) || (type == 1 && std::abs(iround) > 1e-4f);

    PathBuilder builder;
    if (!has_roundness) {
        for (int i = 0; i < total_vertices; ++i) {
            float r = (type == 1 && (i % 2 == 1)) ? irad : orad;
            float a = rot_rad + i * angle_step;
            float x = pos.x + r * std::cos(a);
            float y = pos.y + r * std::sin(a);
            if (i == 0) builder.move_to(x, y);
            else builder.line_to(x, y);
        }
        builder.close();
    } else {
        // Compute vertices and perpendicular cubic tangents for rounded stars/polygons
        float perp_scale = static_cast<float>(M_PI * 2.0) / static_cast<float>(total_vertices * 2);
        auto get_vertex = [&](int idx) {
            int i = (idx % total_vertices + total_vertices) % total_vertices;
            bool is_inner = (type == 1 && (i % 2 == 1));
            float r = is_inner ? irad : orad;
            float rnd = is_inner ? iround : oround;
            float a = rot_rad + i * angle_step;
            float cx = std::cos(a);
            float cy = std::sin(a);
            Point v = Point::from_xy(pos.x + r * cx, pos.y + r * cy);
            // Tangent perpendicular to radial vector
            float tx = -cy * dir_sign * r * rnd * perp_scale;
            float ty =  cx * dir_sign * r * rnd * perp_scale;
            return std::make_pair(v, Point::from_xy(tx, ty));
        };

        auto [v0, t0] = get_vertex(0);
        builder.move_to(v0.x, v0.y);
        Point prev_v = v0;
        Point prev_t = t0;

        for (int i = 1; i <= total_vertices; ++i) {
            auto [curr_v, curr_t] = get_vertex(i);
            Point c1 = Point::from_xy(prev_v.x + prev_t.x, prev_v.y + prev_t.y);
            Point c2 = Point::from_xy(curr_v.x - curr_t.x, curr_v.y - curr_t.y);
            builder.cubic_to(c1.x, c1.y, c2.x, c2.y, curr_v.x, curr_v.y);
            prev_v = curr_v;
            prev_t = curr_t;
        }
        builder.close();
    }

    if (auto p = builder.finish()) {
        out_path.add_path(*p);
    }
}

std::shared_ptr<StarShape> StarShape::parse(const json::JsonValue& val) {
    auto ss = std::make_shared<StarShape>();
    parse_common_item_fields(*ss, val);
    ss->type = val.get_int("sy", 1);
    if (const auto* pt = val.get("pt")) ss->points = parse_float_property(*pt, 5.0f);
    if (const auto* p = val.get("p")) ss->position = parse_point_property(*p, Point::zero());
    if (const auto* r = val.get("r")) ss->rotation = parse_float_property(*r, 0.0f);
    if (const auto* ir = val.get("ir")) ss->inner_radius = parse_float_property(*ir, 0.0f);
    if (const auto* is = val.get("is")) ss->inner_roundness = parse_float_property(*is, 0.0f);
    if (const auto* or_val = val.get("or")) ss->outer_radius = parse_float_property(*or_val, 0.0f);
    if (const auto* os_val = val.get("os")) ss->outer_roundness = parse_float_property(*os_val, 0.0f);
    if (!ss->is_animated()) {
        ss->collect_geometry(0.0f, ss->cached_static_path);
        ss->has_cached_static = true;
    }
    return ss;
}

// ── FillShape ("fl") ──────────────────────────────────────────────────────

void FillShape::render(RenderContext& ctx, float frame) const {
    if (hidden) return;
    Path path = ctx.get_trimmed_path(frame);
    if (path.is_empty()) return;

    Color c = color.evaluate(frame);
    float op = opacity.evaluate(frame) * 0.01f * ctx.parent_opacity;
    if (op <= 0.0f) return;

    c.apply_opacity(op);

    Paint paint;
    paint.set_color(c);
    paint.blend_mode = (blend_mode != nisaba::BlendMode::SourceOver) ? blend_mode : ctx.active_blend_mode;

    nisaba::FillRule fr = (fill_rule == FillRule::EvenOdd)
        ? nisaba::FillRule::EvenOdd
        : nisaba::FillRule::Winding;

    ctx.canvas.fill_path(path, paint, fr);
}

std::shared_ptr<FillShape> FillShape::parse(const json::JsonValue& val) {
    auto fs = std::make_shared<FillShape>();
    parse_common_item_fields(*fs, val);
    if (const auto* c = val.get("c")) fs->color = parse_color_property(*c, Color::WHITE);
    if (const auto* o = val.get("o")) fs->opacity = parse_float_property(*o, 100.0f);
    fs->fill_rule = (val.get_int("r", 1) == 2) ? FillRule::EvenOdd : FillRule::NonZero;
    return fs;
}

// ── StrokeShape ("st") ────────────────────────────────────────────────────

bool StrokeShape::is_animated() const noexcept {
    if (color.is_animated() || opacity.is_animated() || width.is_animated() || dash_offset.is_animated()) {
        return true;
    }
    for (const auto& d : dashes) {
        if (d.is_animated()) return true;
    }
    return false;
}

void StrokeShape::render(RenderContext& ctx, float frame) const {
    if (hidden) return;
    Path path = ctx.get_trimmed_path(frame);
    if (path.is_empty()) return;

    float w = width.evaluate(frame);
    if (w <= 0.0f) return;

    Color c = color.evaluate(frame);
    float op = opacity.evaluate(frame) * 0.01f * ctx.parent_opacity;
    if (op <= 0.0f) return;

    c.apply_opacity(op);

    Paint paint;
    paint.set_color(c);
    paint.blend_mode = (blend_mode != nisaba::BlendMode::SourceOver) ? blend_mode : ctx.active_blend_mode;

    Stroke stroke(w);
    switch (cap) {
        case LineCap::Butt:   stroke.line_cap = nisaba::LineCap::Butt; break;
        case LineCap::Round:  stroke.line_cap = nisaba::LineCap::Round; break;
        case LineCap::Square: stroke.line_cap = nisaba::LineCap::Square; break;
    }
    switch (join) {
        case LineJoin::Miter: stroke.line_join = nisaba::LineJoin::Miter; break;
        case LineJoin::Round: stroke.line_join = nisaba::LineJoin::Round; break;
        case LineJoin::Bevel: stroke.line_join = nisaba::LineJoin::Bevel; break;
    }
    stroke.miter_limit = miter_limit;

    if (!dashes.empty()) {
        ctx.scratch_dashes.clear();
        for (const auto& d : dashes) {
            ctx.scratch_dashes.push_back(std::max(0.0f, d.evaluate(frame)));
        }
        // If odd number of dash entries, duplicate to make an even on/off pattern
        if (ctx.scratch_dashes.size() % 2 == 1) {
            size_t n = ctx.scratch_dashes.size();
            for (size_t i = 0; i < n; ++i) {
                ctx.scratch_dashes.push_back(ctx.scratch_dashes[i]);
            }
        }
        float doffset = dash_offset.evaluate(frame);
        stroke.dash = StrokeDash::create(ctx.scratch_dashes, doffset);
    }

    ctx.canvas.stroke_path(path, paint, stroke);
}

std::shared_ptr<StrokeShape> StrokeShape::parse(const json::JsonValue& val) {
    auto ss = std::make_shared<StrokeShape>();
    parse_common_item_fields(*ss, val);
    if (const auto* c = val.get("c")) ss->color = parse_color_property(*c, Color::WHITE);
    if (const auto* o = val.get("o")) ss->opacity = parse_float_property(*o, 100.0f);
    if (const auto* w = val.get("w")) ss->width = parse_float_property(*w, 1.0f);
    ss->cap = static_cast<LineCap>(val.get_int("lc", 2));
    ss->join = static_cast<LineJoin>(val.get_int("lj", 2));
    ss->miter_limit = val.get_float("ml", 4.0f);

    if (const auto* d_arr = val.get("d")) {
        if (d_arr->is_array()) {
            for (const auto& d_item : d_arr->as_array()) {
                if (!d_item.is_object()) continue;
                std::string_view n = d_item.get_string("n");
                if (const auto* v = d_item.get("v")) {
                    if (n == "o") {
                        ss->dash_offset = parse_float_property(*v, 0.0f);
                    } else if (n == "d" || n == "g") {
                        ss->dashes.push_back(parse_float_property(*v, 0.0f));
                    }
                }
            }
        }
    }
    return ss;
}

// ── GradientColorStops ("g") ──────────────────────────────────────────────

GradientColorStops GradientColorStops::parse(const json::JsonValue& val) {
    GradientColorStops gcs;
    if (!val.is_object()) return gcs;
    gcs.color_count = val.get_int("p", 0);
    if (const auto* k = val.get("k")) {
        gcs.raw_data = parse_float_array_property(*k);
    }
    return gcs;
}

void GradientColorStops::evaluate_stops_into(float frame, float master_opacity, std::vector<GradientStop>& out_stops) const {
    out_stops.clear();
    std::vector<float> flat = raw_data.evaluate(frame);
    if (flat.empty()) {
        Color c0 = Color::WHITE; c0.apply_opacity(master_opacity);
        Color c1 = Color::BLACK; c1.apply_opacity(master_opacity);
        out_stops.push_back(GradientStop::create(0.0f, c0));
        out_stops.push_back(GradientStop::create(1.0f, c1));
        return;
    }

    int num_colors = color_count;
    if (num_colors <= 0 || static_cast<size_t>(num_colors * 4) > flat.size()) {
        num_colors = static_cast<int>(flat.size() / 4);
    }
    if (num_colors <= 0) {
        Color c = Color::WHITE; c.apply_opacity(master_opacity);
        out_stops.push_back(GradientStop::create(0.0f, c));
        out_stops.push_back(GradientStop::create(1.0f, c));
        return;
    }

    struct RgbStop { float t, r, g, b; };
    struct AlphaStop { float t, a; };

    std::vector<RgbStop> rgb_stops;
    rgb_stops.reserve(static_cast<size_t>(num_colors));
    for (int i = 0; i < num_colors; ++i) {
        size_t base = static_cast<size_t>(i * 4);
        rgb_stops.push_back({
            std::clamp(flat[base + 0], 0.0f, 1.0f),
            std::clamp(flat[base + 1], 0.0f, 1.0f),
            std::clamp(flat[base + 2], 0.0f, 1.0f),
            std::clamp(flat[base + 3], 0.0f, 1.0f)
        });
    }

    size_t alpha_start = static_cast<size_t>(num_colors * 4);
    size_t num_alphas = (flat.size() > alpha_start) ? (flat.size() - alpha_start) / 2 : 0;

    std::vector<AlphaStop> alpha_stops;
    alpha_stops.reserve(num_alphas);
    for (size_t i = 0; i < num_alphas; ++i) {
        size_t base = alpha_start + i * 2;
        alpha_stops.push_back({
            std::clamp(flat[base + 0], 0.0f, 1.0f),
            std::clamp(flat[base + 1], 0.0f, 1.0f)
        });
    }

    auto sample_rgb = [&](float t) -> std::tuple<float, float, float> {
        if (rgb_stops.empty()) return {1.0f, 1.0f, 1.0f};
        if (t <= rgb_stops.front().t) return {rgb_stops.front().r, rgb_stops.front().g, rgb_stops.front().b};
        if (t >= rgb_stops.back().t) return {rgb_stops.back().r, rgb_stops.back().g, rgb_stops.back().b};
        for (size_t i = 0; i + 1 < rgb_stops.size(); ++i) {
            if (t >= rgb_stops[i].t && t <= rgb_stops[i + 1].t) {
                float span = rgb_stops[i + 1].t - rgb_stops[i].t;
                float u = (span > 1e-6f) ? (t - rgb_stops[i].t) / span : 0.0f;
                return {
                    rgb_stops[i].r + (rgb_stops[i + 1].r - rgb_stops[i].r) * u,
                    rgb_stops[i].g + (rgb_stops[i + 1].g - rgb_stops[i].g) * u,
                    rgb_stops[i].b + (rgb_stops[i + 1].b - rgb_stops[i].b) * u
                };
            }
        }
        return {rgb_stops.back().r, rgb_stops.back().g, rgb_stops.back().b};
    };

    auto sample_alpha = [&](float t) -> float {
        if (alpha_stops.empty()) return 1.0f;
        if (t <= alpha_stops.front().t) return alpha_stops.front().a;
        if (t >= alpha_stops.back().t) return alpha_stops.back().a;
        for (size_t i = 0; i + 1 < alpha_stops.size(); ++i) {
            if (t >= alpha_stops[i].t && t <= alpha_stops[i + 1].t) {
                float span = alpha_stops[i + 1].t - alpha_stops[i].t;
                float u = (span > 1e-6f) ? (t - alpha_stops[i].t) / span : 0.0f;
                return alpha_stops[i].a + (alpha_stops[i + 1].a - alpha_stops[i].a) * u;
            }
        }
        return alpha_stops.back().a;
    };

    if (alpha_stops.empty()) {
        for (const auto& cs : rgb_stops) {
            Color c = Color::from_rgba(cs.r, cs.g, cs.b, master_opacity).value_or(Color::WHITE);
            out_stops.push_back(GradientStop::create(cs.t, c));
        }
    } else {
        // Merge sorted positions from both rgb_stops and alpha_stops
        std::vector<float> positions;
        positions.reserve(rgb_stops.size() + alpha_stops.size());
        for (const auto& cs : rgb_stops) positions.push_back(cs.t);
        for (const auto& as : alpha_stops) positions.push_back(as.t);
        std::sort(positions.begin(), positions.end());

        float prev_t = -1.0f;
        for (float t : positions) {
            if (prev_t >= 0.0f && std::abs(t - prev_t) < 1e-5f) continue;
            prev_t = t;
            auto [r, g, b] = sample_rgb(t);
            float a = sample_alpha(t) * master_opacity;
            Color c = Color::from_rgba(r, g, b, std::clamp(a, 0.0f, 1.0f)).value_or(Color::WHITE);
            out_stops.push_back(GradientStop::create(t, c));
        }
    }

    if (out_stops.size() == 1) {
        out_stops.push_back(GradientStop::create(1.0f, out_stops[0].color));
    }
}

std::vector<GradientStop> GradientColorStops::evaluate_stops(float frame, float master_opacity) const {
    std::vector<GradientStop> res;
    evaluate_stops_into(frame, master_opacity, res);
    return res;
}

namespace {

Shader build_gradient_shader(
    GradientType gradient_type,
    Point s,
    Point e,
    float h_len_pct,
    float h_ang_deg,
    const std::vector<GradientStop>& stops_vec
) {
    if (gradient_type == GradientType::Linear) {
        if (auto grad = LinearGradient::create(s, e, stops_vec)) {
            return Shader(*grad);
        }
    } else {
        float radius = s.distance(e);
        if (radius <= 1e-4f) radius = 0.001f;
        float h = std::clamp(h_len_pct * 0.01f, -0.99f, 0.99f);
        if (std::abs(h) > 1e-3f) {
            // Focal radial gradient via TwoPointConicalGradient
            float base_angle = std::atan2(e.y - s.y, e.x - s.x);
            float focal_angle = base_angle + h_ang_deg * static_cast<float>(M_PI / 180.0);
            float focal_dist = radius * h;
            Point focal_pt = Point::from_xy(
                s.x + std::cos(focal_angle) * focal_dist,
                s.y + std::sin(focal_angle) * focal_dist
            );
            return Shader::create_two_point_conical(focal_pt, 0.0f, s, radius, stops_vec);
        } else {
            if (auto grad = RadialGradient::create(s, radius, stops_vec)) {
                return Shader(*grad);
            }
        }
    }
    return Shader::from_color(stops_vec.empty() ? Color::WHITE : stops_vec.front().color);
}

} // anonymous namespace

// ── GradientFillShape ("gf") ──────────────────────────────────────────────

void GradientFillShape::render(RenderContext& ctx, float frame) const {
    if (hidden) return;
    Path path = ctx.get_trimmed_path(frame);
    if (path.is_empty()) return;

    float op = opacity.evaluate(frame) * 0.01f * ctx.parent_opacity;
    if (op <= 0.0f) return;

    Point s = start_point.evaluate(frame);
    Point e = end_point.evaluate(frame);
    float h_len = highlight_length.evaluate(frame);
    float h_ang = highlight_angle.evaluate(frame);

    stops.evaluate_stops_into(frame, op, ctx.scratch_stops);

    Paint paint;
    paint.shader = build_gradient_shader(gradient_type, s, e, h_len, h_ang, ctx.scratch_stops);
    paint.blend_mode = (blend_mode != nisaba::BlendMode::SourceOver) ? blend_mode : ctx.active_blend_mode;

    nisaba::FillRule fr = (fill_rule == FillRule::EvenOdd)
        ? nisaba::FillRule::EvenOdd
        : nisaba::FillRule::Winding;

    ctx.canvas.fill_path(path, paint, fr);
}

std::shared_ptr<GradientFillShape> GradientFillShape::parse(const json::JsonValue& val) {
    auto gs = std::make_shared<GradientFillShape>();
    parse_common_item_fields(*gs, val);
    gs->gradient_type = (val.get_int("t", 1) == 2) ? GradientType::Radial : GradientType::Linear;
    if (const auto* s = val.get("s")) gs->start_point = parse_point_property(*s, Point::zero());
    if (const auto* e = val.get("e")) gs->end_point = parse_point_property(*e, Point::from_xy(100.0f, 0.0f));
    if (const auto* h = val.get("h")) gs->highlight_length = parse_float_property(*h, 0.0f);
    if (const auto* a = val.get("a")) gs->highlight_angle = parse_float_property(*a, 0.0f);
    if (const auto* g = val.get("g")) gs->stops = GradientColorStops::parse(*g);
    if (const auto* o = val.get("o")) gs->opacity = parse_float_property(*o, 100.0f);
    gs->fill_rule = (val.get_int("r", 1) == 2) ? FillRule::EvenOdd : FillRule::NonZero;
    return gs;
}

// ── GradientStrokeShape ("gs") ────────────────────────────────────────────

bool GradientStrokeShape::is_animated() const noexcept {
    if (start_point.is_animated() || end_point.is_animated() ||
        highlight_length.is_animated() || highlight_angle.is_animated() ||
        stops.is_animated() || opacity.is_animated() || width.is_animated() ||
        dash_offset.is_animated()) {
        return true;
    }
    for (const auto& d : dashes) {
        if (d.is_animated()) return true;
    }
    return false;
}

void GradientStrokeShape::render(RenderContext& ctx, float frame) const {
    if (hidden) return;
    Path path = ctx.get_trimmed_path(frame);
    if (path.is_empty()) return;

    float w = width.evaluate(frame);
    if (w <= 0.0f) return;

    float op = opacity.evaluate(frame) * 0.01f * ctx.parent_opacity;
    if (op <= 0.0f) return;

    Point s = start_point.evaluate(frame);
    Point e = end_point.evaluate(frame);
    float h_len = highlight_length.evaluate(frame);
    float h_ang = highlight_angle.evaluate(frame);

    stops.evaluate_stops_into(frame, op, ctx.scratch_stops);

    Paint paint;
    paint.shader = build_gradient_shader(gradient_type, s, e, h_len, h_ang, ctx.scratch_stops);
    paint.blend_mode = (blend_mode != nisaba::BlendMode::SourceOver) ? blend_mode : ctx.active_blend_mode;

    Stroke stroke(w);
    switch (cap) {
        case LineCap::Butt:   stroke.line_cap = nisaba::LineCap::Butt; break;
        case LineCap::Round:  stroke.line_cap = nisaba::LineCap::Round; break;
        case LineCap::Square: stroke.line_cap = nisaba::LineCap::Square; break;
    }
    switch (join) {
        case LineJoin::Miter: stroke.line_join = nisaba::LineJoin::Miter; break;
        case LineJoin::Round: stroke.line_join = nisaba::LineJoin::Round; break;
        case LineJoin::Bevel: stroke.line_join = nisaba::LineJoin::Bevel; break;
    }
    stroke.miter_limit = miter_limit;

    if (!dashes.empty()) {
        ctx.scratch_dashes.clear();
        for (const auto& d : dashes) {
            ctx.scratch_dashes.push_back(std::max(0.0f, d.evaluate(frame)));
        }
        if (ctx.scratch_dashes.size() % 2 == 1) {
            size_t n = ctx.scratch_dashes.size();
            for (size_t i = 0; i < n; ++i) {
                ctx.scratch_dashes.push_back(ctx.scratch_dashes[i]);
            }
        }
        float doffset = dash_offset.evaluate(frame);
        stroke.dash = StrokeDash::create(ctx.scratch_dashes, doffset);
    }

    ctx.canvas.stroke_path(path, paint, stroke);
}

std::shared_ptr<GradientStrokeShape> GradientStrokeShape::parse(const json::JsonValue& val) {
    auto gs = std::make_shared<GradientStrokeShape>();
    parse_common_item_fields(*gs, val);
    gs->gradient_type = (val.get_int("t", 1) == 2) ? GradientType::Radial : GradientType::Linear;
    if (const auto* s = val.get("s")) gs->start_point = parse_point_property(*s, Point::zero());
    if (const auto* e = val.get("e")) gs->end_point = parse_point_property(*e, Point::from_xy(100.0f, 0.0f));
    if (const auto* h = val.get("h")) gs->highlight_length = parse_float_property(*h, 0.0f);
    if (const auto* a = val.get("a")) gs->highlight_angle = parse_float_property(*a, 0.0f);
    if (const auto* g = val.get("g")) gs->stops = GradientColorStops::parse(*g);
    if (const auto* o = val.get("o")) gs->opacity = parse_float_property(*o, 100.0f);
    if (const auto* w = val.get("w")) gs->width = parse_float_property(*w, 1.0f);
    gs->cap = static_cast<LineCap>(val.get_int("lc", 2));
    gs->join = static_cast<LineJoin>(val.get_int("lj", 2));
    gs->miter_limit = val.get_float("ml", 4.0f);

    if (const auto* d_arr = val.get("d")) {
        if (d_arr->is_array()) {
            for (const auto& d_item : d_arr->as_array()) {
                if (!d_item.is_object()) continue;
                std::string_view n = d_item.get_string("n");
                if (const auto* v = d_item.get("v")) {
                    if (n == "o") {
                        gs->dash_offset = parse_float_property(*v, 0.0f);
                    } else if (n == "d" || n == "g") {
                        gs->dashes.push_back(parse_float_property(*v, 0.0f));
                    }
                }
            }
        }
    }
    return gs;
}

// ── TrimPathShape ("tm") via nisaba::PathMeasure ──────────────────────────

namespace {

Path trim_single_path_simultaneous(const Path& in_path, float s_norm, float e_norm) {
    if (in_path.is_empty()) return Path();

    // Trim each contour simultaneously in normalized [0, 1] space
    Path dst;
    PathMeasure pm(in_path, false);
    do {
        float len = pm.length();
        if (len <= 1e-5f) continue;
        if (e_norm >= s_norm) {
            pm.get_segment(s_norm * len, e_norm * len, &dst, true);
        } else {
            // Wrap around
            pm.get_segment(s_norm * len, len, &dst, true);
            pm.get_segment(0.0f, e_norm * len, &dst, true);
        }
    } while (pm.next_contour());
    return dst;
}

Path trim_contours_individually(const Path& in_path, float s_norm, float e_norm) {
    if (in_path.is_empty()) return Path();

    // Measure total arc length across all contours
    PathMeasure pm(in_path, false);
    float total_len = 0.0f;
    do {
        total_len += pm.length();
    } while (pm.next_contour());

    if (total_len <= 1e-5f) return Path();

    auto extract_global_range = [&](float d_start, float d_end, Path& dst) {
        if (d_end <= d_start) return;
        PathMeasure iter(in_path, false);
        float accum = 0.0f;
        do {
            float clen = iter.length();
            float c_start = accum;
            float c_end = accum + clen;
            accum = c_end;

            if (d_end <= c_start || d_start >= c_end) continue;
            float local_s = std::max(0.0f, d_start - c_start);
            float local_e = std::min(clen, d_end - c_start);
            if (local_e > local_s) {
                iter.get_segment(local_s, local_e, &dst, true);
            }
        } while (iter.next_contour());
    };

    Path dst;
    if (e_norm >= s_norm) {
        extract_global_range(s_norm * total_len, e_norm * total_len, dst);
    } else {
        extract_global_range(s_norm * total_len, total_len, dst);
        extract_global_range(0.0f, e_norm * total_len, dst);
    }
    return dst;
}

bool compute_trim_window(float raw_start_pct, float raw_end_pct, float raw_offset_deg, float& out_s, float& out_e, bool& out_is_full) {
    out_is_full = false;
    float s = std::clamp(raw_start_pct, 0.0f, 100.0f) * 0.01f;
    float e = std::clamp(raw_end_pct, 0.0f, 100.0f) * 0.01f;
    if (s > e) std::swap(s, e);

    float span = e - s;
    if (span <= 1e-5f) {
        return false; // Empty
    }
    if (span >= 1.0f - 1e-5f) {
        out_is_full = true;
        return true;
    }

    float o = std::fmod(raw_offset_deg / 360.0f, 1.0f);
    if (o < 0.0f) o += 1.0f;

    out_s = std::fmod(s + o, 1.0f);
    out_e = std::fmod(e + o, 1.0f);
    if (out_s < 0.0f) out_s += 1.0f;
    if (out_e < 0.0f) out_e += 1.0f;
    return true;
}

} // anonymous namespace

Path TrimPathShape::apply_trim(const Path& in_path, float frame) const {
    if (hidden || in_path.is_empty()) return in_path;

    float s_norm = 0.0f, e_norm = 1.0f;
    bool is_full = false;
    if (!compute_trim_window(start.evaluate(frame), end.evaluate(frame), offset.evaluate(frame), s_norm, e_norm, is_full)) {
        return Path();
    }
    if (is_full) return in_path;

    if (trim_type == TrimType::Individually) {
        return trim_contours_individually(in_path, s_norm, e_norm);
    }
    return trim_single_path_simultaneous(in_path, s_norm, e_norm);
}

void TrimPathShape::modify_paths(float frame, std::vector<Path>& paths) const {
    if (hidden || paths.empty()) return;

    float s_norm = 0.0f, e_norm = 1.0f;
    bool is_full = false;
    if (!compute_trim_window(start.evaluate(frame), end.evaluate(frame), offset.evaluate(frame), s_norm, e_norm, is_full)) {
        paths.clear();
        return;
    }
    if (is_full) return;

    if (trim_type == TrimType::Simultaneously) {
        for (auto& p : paths) {
            p = trim_single_path_simultaneous(p, s_norm, e_norm);
        }
    } else {
        // Individually across all paths in the group
        Path combined;
        for (const auto& p : paths) {
            combined.add_path(p);
        }
        Path trimmed = trim_contours_individually(combined, s_norm, e_norm);
        paths.clear();
        if (!trimmed.is_empty()) {
            paths.push_back(std::move(trimmed));
        }
    }
}

std::shared_ptr<TrimPathShape> TrimPathShape::parse(const json::JsonValue& val) {
    auto ts = std::make_shared<TrimPathShape>();
    parse_common_item_fields(*ts, val);
    if (const auto* s = val.get("s")) ts->start = parse_float_property(*s, 0.0f);
    if (const auto* e = val.get("e")) ts->end = parse_float_property(*e, 100.0f);
    if (const auto* o = val.get("o")) ts->offset = parse_float_property(*o, 0.0f);
    ts->trim_type = (val.get_int("m", 1) == 2) ? TrimType::Individually : TrimType::Simultaneously;
    return ts;
}

// ── MergePathsShape ("mm") via nisaba::path_ops ───────────────────────────

void MergePathsShape::modify_paths(float frame, std::vector<Path>& paths) const {
    (void)frame;
    if (hidden || paths.size() <= 1) return;

    if (mode == MergeMode::Merge) {
        Path merged;
        for (const auto& p : paths) {
            merged.add_path(p);
        }
        paths.clear();
        paths.push_back(std::move(merged));
        return;
    }

    PathOp op = PathOp::Union;
    switch (mode) {
        case MergeMode::Add:              op = PathOp::Union; break;
        case MergeMode::Subtract:         op = PathOp::Difference; break;
        case MergeMode::Intersect:            op = PathOp::Intersect; break;
        case MergeMode::ExcludeIntersections: op = PathOp::Xor; break;
        default:                              op = PathOp::Union; break;
    }

    Path accum = paths[0];
    for (size_t i = 1; i < paths.size(); ++i) {
        if (paths[i].is_empty()) continue;
        if (accum.is_empty()) {
            if (mode == MergeMode::Add || mode == MergeMode::ExcludeIntersections) {
                accum = paths[i];
            }
            continue;
        }
        if (auto res = path_op(accum, paths[i], op)) {
            accum = std::move(*res);
        } else if (mode == MergeMode::Add) {
            accum.add_path(paths[i]);
        }
    }

    paths.clear();
    if (!accum.is_empty()) {
        paths.push_back(std::move(accum));
    }
}

std::shared_ptr<MergePathsShape> MergePathsShape::parse(const json::JsonValue& val) {
    auto ms = std::make_shared<MergePathsShape>();
    parse_common_item_fields(*ms, val);
    int m = val.get_int("mm", 1);
    if (m >= 1 && m <= 5) {
        ms->mode = static_cast<MergeMode>(m);
    }
    return ms;
}

// ── RoundCornersShape ("rd") ──────────────────────────────────────────────

Path RoundCornersShape::round_path_corners(const Path& in_path, float r) {
    if (r <= 1e-3f || in_path.is_empty()) return in_path;

    // Extract polygonal contours from line-based segments and round sharp vertices
    PathBuilder builder;
    PathSegmentsIter iter = in_path.segments();

    struct ContourData {
        std::vector<Point> pts;
        bool has_curves = false;
        bool closed = false;
        std::vector<PathSegment> raw_segs;
    };

    std::vector<ContourData> contours;
    while (auto seg = iter.next()) {
        switch (seg->type) {
            case PathSegment::Type::MoveTo:
                contours.push_back(ContourData{});
                contours.back().pts.push_back(seg->p0);
                contours.back().raw_segs.push_back(*seg);
                break;
            case PathSegment::Type::LineTo:
                if (contours.empty()) contours.push_back(ContourData{});
                contours.back().pts.push_back(seg->p0);
                contours.back().raw_segs.push_back(*seg);
                break;
            case PathSegment::Type::QuadTo:
            case PathSegment::Type::CubicTo:
                if (contours.empty()) contours.push_back(ContourData{});
                contours.back().has_curves = true;
                contours.back().raw_segs.push_back(*seg);
                break;
            case PathSegment::Type::Close:
                if (!contours.empty()) {
                    contours.back().closed = true;
                    contours.back().raw_segs.push_back(*seg);
                }
                break;
        }
    }

    for (auto& c : contours) {
        if (c.has_curves || c.pts.size() < 3) {
            for (const auto& s : c.raw_segs) {
                switch (s.type) {
                    case PathSegment::Type::MoveTo: builder.move_to(s.p0.x, s.p0.y); break;
                    case PathSegment::Type::LineTo: builder.line_to(s.p0.x, s.p0.y); break;
                    case PathSegment::Type::QuadTo: builder.quad_to(s.p0.x, s.p0.y, s.p1.x, s.p1.y); break;
                    case PathSegment::Type::CubicTo: builder.cubic_to(s.p0.x, s.p0.y, s.p1.x, s.p1.y, s.p2.x, s.p2.y); break;
                    case PathSegment::Type::Close: builder.close(); break;
                }
            }
            continue;
        }

        // Remove duplicate closing point if present
        if (c.closed && c.pts.size() > 2 && c.pts.front().distance(c.pts.back()) < 1e-4f) {
            c.pts.pop_back();
        }
        size_t n = c.pts.size();
        if (n < 3) continue;

        bool started = false;
        for (size_t i = 0; i < n; ++i) {
            if (!c.closed && (i == 0 || i == n - 1)) {
                if (!started) {
                    builder.move_to(c.pts[i].x, c.pts[i].y);
                    started = true;
                } else {
                    builder.line_to(c.pts[i].x, c.pts[i].y);
                }
                continue;
            }

            Point prev = c.pts[(i + n - 1) % n];
            Point curr = c.pts[i];
            Point next = c.pts[(i + 1) % n];

            Point v_in = Point::from_xy(prev.x - curr.x, prev.y - curr.y);
            Point v_out = Point::from_xy(next.x - curr.x, next.y - curr.y);
            float len_in = std::hypot(v_in.x, v_in.y);
            float len_out = std::hypot(v_out.x, v_out.y);

            if (len_in < 1e-4f || len_out < 1e-4f) continue;

            float cut_in = std::min(r, len_in * 0.5f);
            float cut_out = std::min(r, len_out * 0.5f);

            Point p_start = Point::from_xy(curr.x + (v_in.x / len_in) * cut_in, curr.y + (v_in.y / len_in) * cut_in);
            Point p_end = Point::from_xy(curr.x + (v_out.x / len_out) * cut_out, curr.y + (v_out.y / len_out) * cut_out);

            Point c1 = Point::from_xy(p_start.x - (p_start.x - curr.x) * KAPPA, p_start.y - (p_start.y - curr.y) * KAPPA);
            Point c2 = Point::from_xy(p_end.x - (p_end.x - curr.x) * KAPPA, p_end.y - (p_end.y - curr.y) * KAPPA);

            if (!started) {
                builder.move_to(p_start.x, p_start.y);
                started = true;
            } else {
                builder.line_to(p_start.x, p_start.y);
            }
            builder.cubic_to(c1.x, c1.y, c2.x, c2.y, p_end.x, p_end.y);
        }
        if (c.closed && started) {
            builder.close();
        }
    }

    if (auto res = builder.finish()) {
        return *res;
    }
    return in_path;
}

void RoundCornersShape::modify_paths(float frame, std::vector<Path>& paths) const {
    if (hidden || paths.empty()) return;
    float r = radius.evaluate(frame);
    if (r <= 1e-3f) return;
    for (auto& p : paths) {
        p = round_path_corners(p, r);
    }
}

std::shared_ptr<RoundCornersShape> RoundCornersShape::parse(const json::JsonValue& val) {
    auto rd = std::make_shared<RoundCornersShape>();
    parse_common_item_fields(*rd, val);
    if (const auto* r = val.get("r")) rd->radius = parse_float_property(*r, 0.0f);
    return rd;
}

// ── OffsetPathShape ("op") ────────────────────────────────────────────────

void OffsetPathShape::modify_paths(float frame, std::vector<Path>& paths) const {
    if (hidden || paths.empty()) return;
    float amt = amount.evaluate(frame);
    if (std::abs(amt) <= 1e-3f) return;

    Stroke stroke(std::abs(amt) * 2.0f);
    stroke.miter_limit = miter_limit.evaluate(frame);
    switch (line_join) {
        case LineJoin::Miter: stroke.line_join = nisaba::LineJoin::Miter; break;
        case LineJoin::Round: stroke.line_join = nisaba::LineJoin::Round; break;
        case LineJoin::Bevel: stroke.line_join = nisaba::LineJoin::Bevel; break;
    }

    for (auto& p : paths) {
        if (p.is_empty()) continue;
        if (auto stroked = stroke_path(p, stroke, 1.0f)) {
            if (amt > 0.0f) {
                if (auto united = path_union(p, *stroked)) {
                    p = std::move(*united);
                }
            } else {
                if (auto diffed = path_difference(p, *stroked)) {
                    p = std::move(*diffed);
                }
            }
        }
    }
}

std::shared_ptr<OffsetPathShape> OffsetPathShape::parse(const json::JsonValue& val) {
    auto op = std::make_shared<OffsetPathShape>();
    parse_common_item_fields(*op, val);
    if (const auto* a = val.get("a")) op->amount = parse_float_property(*a, 0.0f);
    op->line_join = static_cast<LineJoin>(val.get_int("lj", 1));
    if (const auto* ml = val.get("ml")) op->miter_limit = parse_float_property(*ml, 4.0f);
    return op;
}

// ── PuckerBloatShape ("pb") ───────────────────────────────────────────────

void PuckerBloatShape::modify_paths(float frame, std::vector<Path>& paths) const {
    if (hidden || paths.empty()) return;
    float amt = amount.evaluate(frame) * 0.01f;
    if (std::abs(amt) <= 1e-4f) return;

    for (auto& p : paths) {
        if (p.is_empty()) continue;
        Rect b = p.bounds();
        Point center = Point::from_xy(b.left() + b.width() * 0.5f, b.top() + b.height() * 0.5f);

        auto pull_vertex = [&](Point pt) -> Point {
            return Point::from_xy(
                pt.x + (center.x - pt.x) * amt,
                pt.y + (center.y - pt.y) * amt
            );
        };
        auto push_control = [&](Point pt) -> Point {
            return Point::from_xy(
                pt.x - (center.x - pt.x) * amt,
                pt.y - (center.y - pt.y) * amt
            );
        };

        PathBuilder builder;
        PathSegmentsIter iter = p.segments();
        Point last_pt = Point::zero();
        while (auto seg = iter.next()) {
            switch (seg->type) {
                case PathSegment::Type::MoveTo: {
                    Point v = pull_vertex(seg->p0);
                    builder.move_to(v.x, v.y);
                    last_pt = seg->p0;
                    break;
                }
                case PathSegment::Type::LineTo: {
                    Point v = pull_vertex(seg->p0);
                    Point c1 = push_control(Point::from_xy(last_pt.x + (seg->p0.x - last_pt.x) * 0.333f,
                                                           last_pt.y + (seg->p0.y - last_pt.y) * 0.333f));
                    Point c2 = push_control(Point::from_xy(last_pt.x + (seg->p0.x - last_pt.x) * 0.666f,
                                                           last_pt.y + (seg->p0.y - last_pt.y) * 0.666f));
                    builder.cubic_to(c1.x, c1.y, c2.x, c2.y, v.x, v.y);
                    last_pt = seg->p0;
                    break;
                }
                case PathSegment::Type::QuadTo: {
                    Point c = push_control(seg->p0);
                    Point v = pull_vertex(seg->p1);
                    builder.quad_to(c.x, c.y, v.x, v.y);
                    last_pt = seg->p1;
                    break;
                }
                case PathSegment::Type::CubicTo: {
                    Point c1 = push_control(seg->p0);
                    Point c2 = push_control(seg->p1);
                    Point v = pull_vertex(seg->p2);
                    builder.cubic_to(c1.x, c1.y, c2.x, c2.y, v.x, v.y);
                    last_pt = seg->p2;
                    break;
                }
                case PathSegment::Type::Close:
                    builder.close();
                    break;
            }
        }
        if (auto out = builder.finish()) {
            p = std::move(*out);
        }
    }
}

std::shared_ptr<PuckerBloatShape> PuckerBloatShape::parse(const json::JsonValue& val) {
    auto pb = std::make_shared<PuckerBloatShape>();
    parse_common_item_fields(*pb, val);
    if (const auto* a = val.get("a")) pb->amount = parse_float_property(*a, 0.0f);
    return pb;
}

// ── ZigZagShape ("zz") ────────────────────────────────────────────────────

void ZigZagShape::modify_paths(float frame, std::vector<Path>& paths) const {
    if (hidden || paths.empty()) return;
    float amp = size.evaluate(frame);
    int r_cnt = std::max(1, static_cast<int>(std::round(ridges.evaluate(frame))));
    if (std::abs(amp) <= 1e-3f) return;

    for (auto& p : paths) {
        if (p.is_empty()) continue;
        PathMeasure pm(p, false);
        PathBuilder builder;
        do {
            float clen = pm.length();
            if (clen <= 1e-4f) continue;
            int total_steps = std::max(2, r_cnt * 2);
            float step_d = clen / static_cast<float>(total_steps);

            Point prev_pt = Point::zero();
            Point prev_tan = Point::zero();
            for (int i = 0; i <= total_steps; ++i) {
                float d = std::min(clen, i * step_d);
                Point pos, tan;
                if (!pm.get_pos_tan(d, &pos, &tan)) continue;

                float sign = (i % 2 == 0) ? 1.0f : -1.0f;
                // Normal perpendicular to tangent
                Point norm = Point::from_xy(-tan.y, tan.x);
                Point displaced = Point::from_xy(pos.x + norm.x * amp * sign, pos.y + norm.y * amp * sign);

                if (i == 0) {
                    builder.move_to(displaced.x, displaced.y);
                } else if (point_type == 2) {
                    // Smooth cubic wave
                    float handle_len = step_d * 0.35f;
                    Point c1 = Point::from_xy(prev_pt.x + prev_tan.x * handle_len, prev_pt.y + prev_tan.y * handle_len);
                    Point c2 = Point::from_xy(displaced.x - tan.x * handle_len, displaced.y - tan.y * handle_len);
                    builder.cubic_to(c1.x, c1.y, c2.x, c2.y, displaced.x, displaced.y);
                } else {
                    builder.line_to(displaced.x, displaced.y);
                }
                prev_pt = displaced;
                prev_tan = tan;
            }
            if (pm.is_closed()) {
                builder.close();
            }
        } while (pm.next_contour());

        if (auto out = builder.finish()) {
            p = std::move(*out);
        }
    }
}

std::shared_ptr<ZigZagShape> ZigZagShape::parse(const json::JsonValue& val) {
    auto zz = std::make_shared<ZigZagShape>();
    parse_common_item_fields(*zz, val);
    if (const auto* s = val.get("s")) zz->size = parse_float_property(*s, 10.0f);
    if (const auto* r = val.get("r")) zz->ridges = parse_float_property(*r, 5.0f);
    zz->point_type = val.get_int("pt", 1);
    return zz;
}

// ── RepeaterShape ("rp") ──────────────────────────────────────────────────

std::shared_ptr<RepeaterShape> RepeaterShape::parse(const json::JsonValue& val) {
    auto rp = std::make_shared<RepeaterShape>();
    parse_common_item_fields(*rp, val);
    if (const auto* c = val.get("c")) rp->copies = parse_float_property(*c, 1.0f);
    if (const auto* o = val.get("o")) rp->offset = parse_float_property(*o, 0.0f);
    rp->composite = (val.get_int("m", 1) == 2) ? CompositeOrder::Below : CompositeOrder::Above;
    if (const auto* tr = val.get("tr")) rp->transform = LottieTransform::parse(*tr);
    return rp;
}

// ── ShapeGroup ("gr") ─────────────────────────────────────────────────────

bool ShapeGroup::is_animated() const noexcept {
    if (transform.is_animated()) return true;
    for (const auto& it : items) {
        if (it && it->is_animated()) return true;
    }
    return false;
}

void ShapeGroup::collect_geometry(float frame, Path& out_path) const {
    if (hidden) return;
    std::vector<Path> local_paths;
    for (const auto& it : items) {
        if (!it || it->hidden) continue;
        if (it->is_geometry() || it->type_name() == "gr") {
            Path p;
            it->collect_geometry(frame, p);
            if (!p.is_empty()) local_paths.push_back(std::move(p));
        } else if (it->is_modifier() && it->type_name() != "rp") {
            it->modify_paths(frame, local_paths);
        }
    }
    Transform tr = transform.matrix(frame);
    for (auto& p : local_paths) {
        p.apply_transform(tr);
        out_path.add_path(p);
    }
}

namespace {

Transform interpolate_repeater_transform(const LottieTransform& tr, float frame, float instance_index) {
    Point anchor = tr.anchor.evaluate(frame);
    Point pos = tr.position.evaluate(frame);
    Point sc = tr.scale.evaluate(frame);
    float rot = tr.rotation.evaluate(frame);

    Transform result = Transform::identity();
    result = result.post_concat(Transform::from_translate(-anchor.x, -anchor.y));

    float sx = std::pow(sc.x * 0.01f, instance_index);
    float sy = std::pow(sc.y * 0.01f, instance_index);
    result = result.post_concat(Transform::from_scale(sx, sy));

    float r = rot * instance_index;
    if (std::abs(r) > 1e-5f) {
        result = result.post_concat(Transform::from_rotate(r));
    }

    Point delta = Point::from_xy((pos.x - anchor.x) * instance_index, (pos.y - anchor.y) * instance_index);
    result = result.post_concat(Transform::from_translate(anchor.x + delta.x, anchor.y + delta.y));
    return result;
}

void render_shape_slice(
    const std::vector<std::shared_ptr<LottieShapeItem>>& items,
    size_t start_idx,
    size_t end_idx,
    RenderContext& ctx,
    float frame
) {
    // Check if there is a Repeater ("rp") in [start_idx, end_idx)
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& item = items[i];
        if (!item || item->hidden) continue;
        if (item->type_name() == "rp") {
            auto rp = std::static_pointer_cast<RepeaterShape>(item);
            float num_copies_f = rp->copies.evaluate(frame);
            int num_copies = static_cast<int>(std::ceil(num_copies_f));
            if (num_copies > 0) {
                float off = rp->offset.evaluate(frame);
                float start_op = rp->transform.start_opacity.evaluate(frame) * 0.01f;
                float end_op = rp->transform.end_opacity.evaluate(frame) * 0.01f;

                auto render_copy = [&](int c_idx) {
                    float inst = static_cast<float>(c_idx) + off;
                    float t_op = (num_copies > 1)
                        ? static_cast<float>(c_idx) / static_cast<float>(num_copies - 1)
                        : 0.0f;
                    float copy_op = start_op + (end_op - start_op) * t_op;
                    if (copy_op <= 0.0f) return;

                    ctx.canvas.save();
                    Transform rep_tr = interpolate_repeater_transform(rp->transform, frame, inst);
                    ctx.canvas.concat(rep_tr);
                    float saved_op = ctx.parent_opacity;
                    ctx.parent_opacity *= copy_op;

                    render_shape_slice(items, start_idx, i, ctx, frame);

                    ctx.parent_opacity = saved_op;
                    ctx.canvas.restore();
                };

                if (rp->composite == CompositeOrder::Below) {
                    for (int c = num_copies - 1; c >= 0; --c) render_copy(c);
                } else {
                    for (int c = 0; c < num_copies; ++c) render_copy(c);
                }
            }
            // Continue rendering items after the repeater if any
            if (i + 1 < end_idx) {
                render_shape_slice(items, i + 1, end_idx, ctx, frame);
            }
            return;
        }
    }

    // Standard slice without repeater: collect geometry, apply modifiers in order, render styles
    std::vector<Path> current_paths;
    current_paths.reserve(8);

    // Check if there are any styles in this group slice
    bool has_styles_or_modifiers = false;
    for (size_t i = start_idx; i < end_idx; ++i) {
        if (items[i] && !items[i]->hidden && (items[i]->is_style() || items[i]->is_modifier())) {
            has_styles_or_modifiers = true;
            break;
        }
    }

    // In Lottie, within a group:
    // - Geometries and child groups accumulate paths.
    // - Modifiers (Trim, Merge, RoundCorners, Offset, PuckerBloat, ZigZag) transform the accumulated paths above them.
    // - Notice: in Bodymovin JSON, styles (fl, st, gf, gs) can appear *after* or *before* modifiers in the array,
    //   or modifiers can appear at the end of the group and apply to all styles in that group!
    //   Wait: if a TrimPath ("tm") or RoundCorners ("rd") is placed at the bottom of a group (after Fill/Stroke),
    //   in After Effects it still modifies the group's geometry before the group's Fill/Stroke draws it!
    //   Unless a modifier is placed between geometries.
    //   Let's check if modifiers appear *after* styles in `items`:
    std::vector<std::shared_ptr<LottieShapeItem>> trailing_modifiers;
    size_t last_style_idx = start_idx;
    for (size_t i = start_idx; i < end_idx; ++i) {
        if (items[i] && !items[i]->hidden && items[i]->is_style()) {
            last_style_idx = i;
        }
    }
    for (size_t i = last_style_idx + 1; i < end_idx; ++i) {
        if (items[i] && !items[i]->hidden && items[i]->is_modifier()) {
            trailing_modifiers.push_back(items[i]);
        }
    }

    // Also collect geometries first if styles appear *after* geometries, or if styles appear at the end in reverse order.
    // Wait: in Bodymovin exported JSON, `it` array is ordered top-to-bottom as in AE timeline,
    // where geometries are at the top (index 0, 1...) and Fill/Stroke are below them (index 2, 3...),
    // OR in some exporters styles are listed after geometries.
    // Wait: what if `gr` contains child `gr`s that have their own fills/strokes vs child `gr`s that only have geometry?
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& item = items[i];
        if (!item || item->hidden) continue;

        if (item->is_geometry()) {
            Path p;
            item->collect_geometry(frame, p);
            if (!p.is_empty()) {
                current_paths.push_back(std::move(p));
            }
        } else if (item->type_name() == "gr") {
            // Render child group (for its own styles) AND if this parent group has styles/modifiers,
            // also collect its geometry!
            item->render(ctx, frame);
            if (has_styles_or_modifiers) {
                Path p;
                item->collect_geometry(frame, p);
                if (!p.is_empty()) {
                    current_paths.push_back(std::move(p));
                }
            }
        } else if (item->is_modifier() && i <= last_style_idx) {
            item->modify_paths(frame, current_paths);
        } else if (item->is_style()) {
            // Apply any trailing modifiers (like a TrimPath placed at the bottom of the group) to a copy for rendering
            std::vector<Path> styled_paths = current_paths;
            for (const auto& mod : trailing_modifiers) {
                mod->modify_paths(frame, styled_paths);
            }
            ctx.accumulated_path.reset();
            for (const auto& sp : styled_paths) {
                ctx.accumulated_path.add_path(sp);
            }
            item->render(ctx, frame);
        }
    }
}

} // anonymous namespace

void ShapeGroup::render(RenderContext& ctx, float frame) const {
    if (hidden) return;
    float grp_op = transform.eval_opacity(frame);
    if (grp_op <= 0.0f) return;

    ctx.canvas.save();
    Transform tr = transform.matrix(frame);
    ctx.canvas.concat(tr);

    float old_opacity = ctx.parent_opacity;
    nisaba::BlendMode old_bm = ctx.active_blend_mode;
    ctx.parent_opacity *= grp_op;
    if (blend_mode != nisaba::BlendMode::SourceOver) {
        ctx.active_blend_mode = blend_mode;
    }

    render_shape_slice(items, 0, items.size(), ctx, frame);

    ctx.active_blend_mode = old_bm;
    ctx.parent_opacity = old_opacity;
    ctx.canvas.restore();
}

std::shared_ptr<ShapeGroup> ShapeGroup::parse(const json::JsonValue& val) {
    auto grp = std::make_shared<ShapeGroup>();
    parse_common_item_fields(*grp, val);

    if (const auto* it_arr = val.get("it")) {
        if (it_arr->is_array()) {
            for (const auto& it_val : it_arr->as_array()) {
                if (!it_val.is_object()) continue;
                std::string_view ty = it_val.get_string("ty");
                if (ty == "tr") {
                    grp->transform = LottieTransform::parse(it_val);
                } else if (auto item = LottieShapeItem::parse(it_val)) {
                    grp->items.push_back(item);
                }
            }
        }
    }
    return grp;
}

// ── ShapeItem Factory ─────────────────────────────────────────────────────

std::shared_ptr<LottieShapeItem> LottieShapeItem::parse(const json::JsonValue& val) {
    if (!val.is_object()) return nullptr;
    std::string_view ty = val.get_string("ty");

    if (ty == "gr") return ShapeGroup::parse(val);
    if (ty == "sh") return PathShape::parse(val);
    if (ty == "rc") return RectShape::parse(val);
    if (ty == "el") return EllipseShape::parse(val);
    if (ty == "sr") return StarShape::parse(val);
    if (ty == "fl") return FillShape::parse(val);
    if (ty == "st") return StrokeShape::parse(val);
    if (ty == "gf") return GradientFillShape::parse(val);
    if (ty == "gs") return GradientStrokeShape::parse(val);
    if (ty == "tm") return TrimPathShape::parse(val);
    if (ty == "mm") return MergePathsShape::parse(val);
    if (ty == "rd") return RoundCornersShape::parse(val);
    if (ty == "op") return OffsetPathShape::parse(val);
    if (ty == "pb") return PuckerBloatShape::parse(val);
    if (ty == "zz") return ZigZagShape::parse(val);
    if (ty == "rp") return RepeaterShape::parse(val);

    return nullptr;
}

namespace {

inline uint64_t hash_mix(uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

inline uint64_t hash_f32(uint64_t h, float f) noexcept {
    uint32_t bits = 0;
    // Canonicalize -0.0f to +0.0f
    if (f == 0.0f) f = 0.0f;
    std::memcpy(&bits, &f, sizeof(bits));
    return hash_mix(h, static_cast<uint64_t>(bits));
}

inline uint64_t hash_point(uint64_t h, Point p) noexcept {
    return hash_f32(hash_f32(h, p.x), p.y);
}

inline uint64_t hash_color(uint64_t h, Color c) noexcept {
    h = hash_f32(h, c.red());
    h = hash_f32(h, c.green());
    h = hash_f32(h, c.blue());
    return hash_f32(h, c.alpha());
}

inline uint64_t hash_transform(uint64_t h, const Transform& tr) noexcept {
    h = hash_f32(h, tr.sx);
    h = hash_f32(h, tr.kx);
    h = hash_f32(h, tr.ky);
    h = hash_f32(h, tr.sy);
    h = hash_f32(h, tr.tx);
    return hash_f32(h, tr.ty);
}

std::optional<Rect> join_opt_rect(const std::optional<Rect>& a, const std::optional<Rect>& b) noexcept {
    if (!a.has_value()) return b;
    if (!b.has_value()) return a;
    return a->join(*b);
}

std::optional<Rect> compute_slice_bounds(
    const std::vector<std::shared_ptr<LottieShapeItem>>& items,
    size_t start_idx,
    size_t end_idx,
    float frame
) {
    // Handle Repeater ("rp") in [start_idx, end_idx)
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& item = items[i];
        if (!item || item->hidden) continue;
        if (item->type_name() == "rp") {
            const auto& rp = static_cast<const RepeaterShape&>(*item);
            std::optional<Rect> result;
            const auto base_bounds = compute_slice_bounds(items, start_idx, i, frame);
            if (base_bounds.has_value()) {
                const int num_copies = static_cast<int>(std::ceil(rp.copies.evaluate(frame)));
                const float off = rp.offset.evaluate(frame);
                for (int c = 0; c < num_copies; ++c) {
                    const float inst = static_cast<float>(c) + off;
                    const Transform rep_tr = interpolate_repeater_transform(rp.transform, frame, inst);
                    if (auto tr_b = base_bounds->transform(rep_tr)) {
                        result = join_opt_rect(result, tr_b);
                    }
                }
            }
            if (i + 1 < end_idx) {
                result = join_opt_rect(result, compute_slice_bounds(items, i + 1, end_idx, frame));
            }
            return result;
        }
    }

    std::vector<Path> current_paths;
    std::optional<Rect> child_group_bounds;
    float max_stroke_pad = 0.0f;

    std::vector<std::shared_ptr<LottieShapeItem>> trailing_modifiers;
    size_t last_style_idx = start_idx;
    bool has_styles_or_modifiers = false;
    for (size_t i = start_idx; i < end_idx; ++i) {
        if (items[i] && !items[i]->hidden) {
            if (items[i]->is_style()) {
                last_style_idx = i;
                has_styles_or_modifiers = true;
            } else if (items[i]->is_modifier()) {
                has_styles_or_modifiers = true;
            }
        }
    }
    for (size_t i = last_style_idx + 1; i < end_idx; ++i) {
        if (items[i] && !items[i]->hidden && items[i]->is_modifier()) {
            trailing_modifiers.push_back(items[i]);
        }
    }

    std::optional<Rect> styled_bounds;
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& item = items[i];
        if (!item || item->hidden) continue;

        if (item->is_geometry()) {
            Path p;
            item->collect_geometry(frame, p);
            if (!p.is_empty()) current_paths.push_back(std::move(p));
        } else if (item->type_name() == "gr") {
            child_group_bounds = join_opt_rect(child_group_bounds, item->compute_bounds(frame));
            if (has_styles_or_modifiers) {
                Path p;
                item->collect_geometry(frame, p);
                if (!p.is_empty()) current_paths.push_back(std::move(p));
            }
        } else if (item->is_modifier() && i <= last_style_idx) {
            item->modify_paths(frame, current_paths);
        } else if (item->is_style()) {
            float pad = 0.0f;
            if (item->type_name() == "st") {
                const auto& st = static_cast<const StrokeShape&>(*item);
                const float w = std::max(0.0f, st.width.evaluate(frame));
                const float miter = (st.join == LineJoin::Miter) ? std::max(1.0f, st.miter_limit) : 1.0f;
                pad = w * 0.5f * miter;
            } else if (item->type_name() == "gs") {
                const auto& gs = static_cast<const GradientStrokeShape&>(*item);
                const float w = std::max(0.0f, gs.width.evaluate(frame));
                const float miter = (gs.join == LineJoin::Miter) ? std::max(1.0f, gs.miter_limit) : 1.0f;
                pad = w * 0.5f * miter;
            }
            max_stroke_pad = std::max(max_stroke_pad, pad);

            std::vector<Path> eval_paths = current_paths;
            for (const auto& mod : trailing_modifiers) {
                mod->modify_paths(frame, eval_paths);
            }
            for (const auto& ep : eval_paths) {
                if (ep.is_empty()) continue;
                Rect b = ep.bounds();
                if (pad > 0.0f) {
                    if (auto expanded = b.outset(pad, pad)) b = *expanded;
                }
                styled_bounds = join_opt_rect(styled_bounds, b);
            }
        }
    }

    // If a group has geometry without styles (e.g. used as a geometry container), include raw geometry bounds
    if (!styled_bounds.has_value() && !current_paths.empty()) {
        for (const auto& ep : current_paths) {
            if (!ep.is_empty()) {
                styled_bounds = join_opt_rect(styled_bounds, ep.bounds());
            }
        }
    }
    (void)max_stroke_pad;
    return join_opt_rect(child_group_bounds, styled_bounds);
}

} // anonymous namespace

std::optional<Rect> LottieShapeItem::compute_bounds(float frame) const {
    if (hidden) return std::nullopt;
    Path p;
    collect_geometry(frame, p);
    if (p.is_empty()) return std::nullopt;
    return p.bounds();
}

uint64_t LottieShapeItem::visual_fingerprint(float frame) const noexcept {
    if (hidden) return 0u;
    uint64_t h = 1469598103934665603ULL;
    const std::string_view tn = type_name();
    for (char c : tn) h = hash_mix(h, static_cast<uint64_t>(static_cast<unsigned char>(c)));
    h = hash_mix(h, static_cast<uint64_t>(blend_mode));

    if (tn == "rc") {
        const auto& rc = static_cast<const RectShape&>(*this);
        h = hash_point(h, rc.position.evaluate(frame));
        h = hash_point(h, rc.size.evaluate(frame));
        h = hash_f32(h, rc.roundness.evaluate(frame));
    } else if (tn == "el") {
        const auto& el = static_cast<const EllipseShape&>(*this);
        h = hash_point(h, el.position.evaluate(frame));
        h = hash_point(h, el.size.evaluate(frame));
    } else if (tn == "sr") {
        const auto& sr = static_cast<const StarShape&>(*this);
        h = hash_f32(h, sr.points.evaluate(frame));
        h = hash_point(h, sr.position.evaluate(frame));
        h = hash_f32(h, sr.rotation.evaluate(frame));
        h = hash_f32(h, sr.inner_radius.evaluate(frame));
        h = hash_f32(h, sr.outer_radius.evaluate(frame));
        h = hash_f32(h, sr.inner_roundness.evaluate(frame));
        h = hash_f32(h, sr.outer_roundness.evaluate(frame));
    } else if (tn == "sh") {
        const auto& sh = static_cast<const PathShape&>(*this);
        BezierData bz = sh.shape.evaluate(frame);
        h = hash_mix(h, static_cast<uint64_t>(bz.vertices.size()));
        for (size_t i = 0; i < bz.vertices.size(); ++i) {
            h = hash_point(h, bz.vertices[i]);
            if (i < bz.in_tangents.size()) h = hash_point(h, bz.in_tangents[i]);
            if (i < bz.out_tangents.size()) h = hash_point(h, bz.out_tangents[i]);
        }
    } else if (tn == "fl") {
        const auto& fl = static_cast<const FillShape&>(*this);
        h = hash_color(h, fl.color.evaluate(frame));
        h = hash_f32(h, fl.opacity.evaluate(frame));
    } else if (tn == "st") {
        const auto& st = static_cast<const StrokeShape&>(*this);
        h = hash_color(h, st.color.evaluate(frame));
        h = hash_f32(h, st.opacity.evaluate(frame));
        h = hash_f32(h, st.width.evaluate(frame));
        h = hash_f32(h, st.dash_offset.evaluate(frame));
        for (const auto& d : st.dashes) h = hash_f32(h, d.evaluate(frame));
    } else if (tn == "gf") {
        const auto& gf = static_cast<const GradientFillShape&>(*this);
        h = hash_point(h, gf.start_point.evaluate(frame));
        h = hash_point(h, gf.end_point.evaluate(frame));
        h = hash_f32(h, gf.highlight_length.evaluate(frame));
        h = hash_f32(h, gf.highlight_angle.evaluate(frame));
        h = hash_f32(h, gf.opacity.evaluate(frame));
        const auto raw = gf.stops.raw_data.evaluate(frame);
        for (float v : raw) h = hash_f32(h, v);
    } else if (tn == "gs") {
        const auto& gs = static_cast<const GradientStrokeShape&>(*this);
        h = hash_point(h, gs.start_point.evaluate(frame));
        h = hash_point(h, gs.end_point.evaluate(frame));
        h = hash_f32(h, gs.width.evaluate(frame));
        h = hash_f32(h, gs.opacity.evaluate(frame));
        h = hash_f32(h, gs.dash_offset.evaluate(frame));
        const auto raw = gs.stops.raw_data.evaluate(frame);
        for (float v : raw) h = hash_f32(h, v);
    } else if (tn == "tm") {
        const auto& tm = static_cast<const TrimPathShape&>(*this);
        h = hash_f32(h, tm.start.evaluate(frame));
        h = hash_f32(h, tm.end.evaluate(frame));
        h = hash_f32(h, tm.offset.evaluate(frame));
    } else if (tn == "rd") {
        h = hash_f32(h, static_cast<const RoundCornersShape&>(*this).radius.evaluate(frame));
    } else if (tn == "op") {
        const auto& op = static_cast<const OffsetPathShape&>(*this);
        h = hash_f32(h, op.amount.evaluate(frame));
        h = hash_f32(h, op.miter_limit.evaluate(frame));
    } else if (tn == "pb") {
        h = hash_f32(h, static_cast<const PuckerBloatShape&>(*this).amount.evaluate(frame));
    } else if (tn == "zz") {
        const auto& zz = static_cast<const ZigZagShape&>(*this);
        h = hash_f32(h, zz.size.evaluate(frame));
        h = hash_f32(h, zz.ridges.evaluate(frame));
    } else if (tn == "rp") {
        const auto& rp = static_cast<const RepeaterShape&>(*this);
        h = hash_f32(h, rp.copies.evaluate(frame));
        h = hash_f32(h, rp.offset.evaluate(frame));
        h = hash_transform(h, rp.transform.matrix(frame));
        h = hash_f32(h, rp.transform.start_opacity.evaluate(frame));
        h = hash_f32(h, rp.transform.end_opacity.evaluate(frame));
    }
    return h;
}

std::optional<Rect> ShapeGroup::compute_bounds(float frame) const {
    if (hidden) return std::nullopt;
    if (transform.eval_opacity(frame) <= 0.0f) return std::nullopt;
    auto local_b = compute_slice_bounds(items, 0, items.size(), frame);
    if (!local_b.has_value()) return std::nullopt;
    return local_b->transform(transform.matrix(frame));
}

uint64_t ShapeGroup::visual_fingerprint(float frame) const noexcept {
    if (hidden) return 0u;
    uint64_t h = 0x517cc1b727220a95ULL;
    h = hash_transform(h, transform.matrix(frame));
    h = hash_f32(h, transform.eval_opacity(frame));
    for (const auto& item : items) {
        if (item && !item->hidden) {
            h = hash_mix(h, item->visual_fingerprint(frame));
        }
    }
    return h;
}

size_t ShapeGroup::count_animators() const noexcept {
    size_t n = 0;
    if (transform.anchor.is_animated()) ++n;
    if (transform.position.is_animated()) ++n;
    if (transform.x_pos && transform.x_pos->is_animated()) ++n;
    if (transform.y_pos && transform.y_pos->is_animated()) ++n;
    if (transform.scale.is_animated()) ++n;
    if (transform.rotation.is_animated()) ++n;
    if (transform.opacity.is_animated()) ++n;
    if (transform.skew.is_animated()) ++n;
    if (transform.skew_axis.is_animated()) ++n;
    for (const auto& item : items) {
        if (item) n += item->count_animators();
    }
    return n;
}

void LottieShapeItem::bind_expression_manager(ExpressionManager& mgr) {
    const std::string_view tn = type_name();
    if (tn == "gr") {
        auto& grp = static_cast<ShapeGroup&>(*this);
        grp.transform.bind_expression_manager(mgr);
        for (auto& child : grp.items) {
            if (child) child->bind_expression_manager(mgr);
        }
    } else if (tn == "rc") {
        auto& rc = static_cast<RectShape&>(*this);
        rc.position.bind_expression_manager(mgr);
        rc.size.bind_expression_manager(mgr);
        rc.roundness.bind_expression_manager(mgr);
        if (rc.is_animated()) rc.has_cached_static = false;
    } else if (tn == "el") {
        auto& el = static_cast<EllipseShape&>(*this);
        el.position.bind_expression_manager(mgr);
        el.size.bind_expression_manager(mgr);
        if (el.is_animated()) el.has_cached_static = false;
    } else if (tn == "sr") {
        auto& sr = static_cast<StarShape&>(*this);
        sr.points.bind_expression_manager(mgr);
        sr.position.bind_expression_manager(mgr);
        sr.rotation.bind_expression_manager(mgr);
        sr.inner_radius.bind_expression_manager(mgr);
        sr.inner_roundness.bind_expression_manager(mgr);
        sr.outer_radius.bind_expression_manager(mgr);
        sr.outer_roundness.bind_expression_manager(mgr);
        if (sr.is_animated()) sr.has_cached_static = false;
    } else if (tn == "fl") {
        auto& fl = static_cast<FillShape&>(*this);
        fl.color.bind_expression_manager(mgr);
        fl.opacity.bind_expression_manager(mgr);
    } else if (tn == "st") {
        auto& st = static_cast<StrokeShape&>(*this);
        st.color.bind_expression_manager(mgr);
        st.opacity.bind_expression_manager(mgr);
        st.width.bind_expression_manager(mgr);
        for (auto& d : st.dashes) d.bind_expression_manager(mgr);
        st.dash_offset.bind_expression_manager(mgr);
    } else if (tn == "gf") {
        auto& gf = static_cast<GradientFillShape&>(*this);
        gf.start_point.bind_expression_manager(mgr);
        gf.end_point.bind_expression_manager(mgr);
        gf.highlight_length.bind_expression_manager(mgr);
        gf.highlight_angle.bind_expression_manager(mgr);
        gf.stops.raw_data.bind_expression_manager(mgr);
        gf.opacity.bind_expression_manager(mgr);
    } else if (tn == "gs") {
        auto& gs = static_cast<GradientStrokeShape&>(*this);
        gs.start_point.bind_expression_manager(mgr);
        gs.end_point.bind_expression_manager(mgr);
        gs.highlight_length.bind_expression_manager(mgr);
        gs.highlight_angle.bind_expression_manager(mgr);
        gs.stops.raw_data.bind_expression_manager(mgr);
        gs.opacity.bind_expression_manager(mgr);
        gs.width.bind_expression_manager(mgr);
        for (auto& d : gs.dashes) d.bind_expression_manager(mgr);
        gs.dash_offset.bind_expression_manager(mgr);
    } else if (tn == "tm") {
        auto& tm = static_cast<TrimPathShape&>(*this);
        tm.start.bind_expression_manager(mgr);
        tm.end.bind_expression_manager(mgr);
        tm.offset.bind_expression_manager(mgr);
    } else if (tn == "rd") {
        static_cast<RoundCornersShape&>(*this).radius.bind_expression_manager(mgr);
    } else if (tn == "op") {
        auto& op = static_cast<OffsetPathShape&>(*this);
        op.amount.bind_expression_manager(mgr);
        op.miter_limit.bind_expression_manager(mgr);
    } else if (tn == "pb") {
        static_cast<PuckerBloatShape&>(*this).amount.bind_expression_manager(mgr);
    } else if (tn == "zz") {
        auto& zz = static_cast<ZigZagShape&>(*this);
        zz.size.bind_expression_manager(mgr);
        zz.ridges.bind_expression_manager(mgr);
    } else if (tn == "rp") {
        auto& rp = static_cast<RepeaterShape&>(*this);
        rp.copies.bind_expression_manager(mgr);
        rp.offset.bind_expression_manager(mgr);
        rp.transform.bind_expression_manager(mgr);
    }
}

void render_shape_items(
    const std::vector<std::shared_ptr<LottieShapeItem>>& items,
    RenderContext& ctx,
    float frame
) {
    render_shape_slice(items, 0, items.size(), ctx, frame);
}

std::optional<Rect> compute_shape_items_bounds(
    const std::vector<std::shared_ptr<LottieShapeItem>>& items,
    float frame
) {
    return compute_slice_bounds(items, 0, items.size(), frame);
}

Path RenderContext::get_trimmed_path(float frame) const {
    Path p = accumulated_path;
    for (const auto& trim : active_trims) {
        if (trim) {
            p = trim->apply_trim(p, frame);
        }
    }
    return p;
}

} // namespace nisaba::lottie
