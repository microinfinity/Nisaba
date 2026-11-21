/// @file layer.cpp
/// @brief Implementation of Lottie layers, masks, track mattes, effects, images, typography, and 3D compositing.

#include "nisaba/lottie/layer.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/canvas/mask.hpp"
#include "nisaba/effects/blur.hpp"
#include "nisaba/effects/drop_shadow.hpp"
#include "nisaba/effects/color_matrix.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/path/path_measure.hpp"
#include "nisaba/path/path_ops.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/text/bidi.hpp"
#include "nisaba/math/rect.hpp"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <algorithm>
#include <charconv>
#include <cstring>

namespace nisaba::lottie {

// ── LottieAssetRegistry ───────────────────────────────────────────────────

const LottieGlyphChar* LottieAssetRegistry::find_char(std::string_view ch, std::string_view family, std::string_view style) const noexcept {
    for (const auto& gc : chars) {
        if (gc.ch == ch && (family.empty() || gc.family == family) && (style.empty() || gc.style == style)) {
            return &gc;
        }
    }
    for (const auto& gc : chars) {
        if (gc.ch == ch) return &gc;
    }
    return nullptr;
}

namespace {

Color parse_hex_color(std::string_view str) {
    if (str.starts_with('#')) {
        str.remove_prefix(1);
    }
    if (str.length() == 6) {
        uint32_t val = 0;
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val, 16);
        if (ec == std::errc()) {
            uint8_t r = static_cast<uint8_t>((val >> 16) & 0xFF);
            uint8_t g = static_cast<uint8_t>((val >> 8) & 0xFF);
            uint8_t b = static_cast<uint8_t>(val & 0xFF);
            return Color::from_rgba8(r, g, b, 255);
        }
    } else if (str.length() == 8) {
        uint32_t val = 0;
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val, 16);
        if (ec == std::errc()) {
            uint8_t r = static_cast<uint8_t>((val >> 24) & 0xFF);
            uint8_t g = static_cast<uint8_t>((val >> 16) & 0xFF);
            uint8_t b = static_cast<uint8_t>((val >> 8) & 0xFF);
            uint8_t a = static_cast<uint8_t>(val & 0xFF);
            return Color::from_rgba8(r, g, b, a);
        }
    }
    return Color::BLACK;
}

Color parse_json_rgb_array(const json::JsonValue& val, Color fallback = Color::BLACK) {
    if (!val.is_array()) return fallback;
    const auto& arr = val.as_array();
    if (arr.size() < 3) return fallback;
    float r = arr[0].to_float();
    float g = arr[1].to_float();
    float b = arr[2].to_float();
    float a = (arr.size() >= 4) ? arr[3].to_float() : 1.0f;
    if (r > 1.0f || g > 1.0f || b > 1.0f) {
        r /= 255.0f;
        g /= 255.0f;
        b /= 255.0f;
        if (a > 1.0f) a /= 255.0f;
    }
    return Color::from_rgba(
        std::clamp(r, 0.0f, 1.0f),
        std::clamp(g, 0.0f, 1.0f),
        std::clamp(b, 0.0f, 1.0f),
        std::clamp(a, 0.0f, 1.0f)
    ).value_or(fallback);
}

void parse_common_layer_props(LottieLayer& layer, const json::JsonValue& val) {
    layer.index = val.get_int("ind", -1);
    layer.parent_index = val.get_int("parent", -1);
    layer.name = val.get_string("nm", "");
    layer.in_point = val.get_float("ip", 0.0f);
    layer.out_point = val.get_float("op", 0.0f);
    layer.start_time = val.get_float("st", 0.0f);
    layer.time_stretch = val.get_float("sr", 1.0f);
    if (layer.time_stretch == 0.0f) layer.time_stretch = 1.0f;

    layer.blend_mode = map_lottie_blend_mode(val.get_int("bm", 0));
    layer.matte_type = static_cast<MatteType>(val.get_int("tt", 0));
    layer.matte_parent_index = val.get_int("tp", -1);
    layer.is_matte_target = (val.get_int("td", 0) != 0);
    layer.hidden = val.get_bool("hd", false);
    layer.auto_orient = (val.get_int("ao", 0) == 1);

    if (const auto* ks = val.get("ks")) {
        layer.transform = LottieTransform::parse(*ks);
        layer.transform.auto_orient = layer.auto_orient;
        if (val.get_int("ddd", 0) == 1) {
            layer.transform.is_3d = true;
        }
    }

    if (const auto* masks_val = val.get("masksProperties")) {
        if (masks_val->is_array()) {
            for (const auto& m_val : masks_val->as_array()) {
                if (m_val.is_object()) {
                    layer.masks.push_back(Mask::parse(m_val));
                }
            }
        }
    }

    if (const auto* ef_val = val.get("ef")) {
        if (ef_val->is_array()) {
            for (const auto& e_val : ef_val->as_array()) {
                if (e_val.is_object()) {
                    auto eff = LayerEffect::parse(e_val);
                    if (eff.type != EffectType::Unknown && eff.enabled) {
                        layer.effects.push_back(std::move(eff));
                    }
                }
            }
        }
    }
}

bool point_in_path_approx(const Path& p, float x, float y) {
    if (p.is_empty()) return false;
    Rect b = p.bounds();
    if (x < b.left() || x > b.right() || y < b.top() || y > b.bottom()) {
        return false;
    }
    // Ray-casting across path segments
    PathSegmentsIter iter = p.segments();
    iter.set_auto_close(true);
    Point last = Point::zero();
    int crossings = 0;

    auto test_edge = [&](Point p0, Point p1) {
        if ((p0.y <= y && p1.y > y) || (p1.y <= y && p0.y > y)) {
            float vt = (y - p0.y) / (p1.y - p0.y);
            if (x < p0.x + vt * (p1.x - p0.x)) {
                crossings++;
            }
        }
    };

    while (auto seg = iter.next()) {
        switch (seg->type) {
            case PathSegment::Type::MoveTo:
                last = seg->p0;
                break;
            case PathSegment::Type::LineTo:
                test_edge(last, seg->p0);
                last = seg->p0;
                break;
            case PathSegment::Type::QuadTo: {
                Point prev = last;
                for (int i = 1; i <= 4; ++i) {
                    float t = i * 0.25f;
                    float mt = 1.0f - t;
                    Point cur = Point::from_xy(
                        mt * mt * last.x + 2.0f * mt * t * seg->p0.x + t * t * seg->p1.x,
                        mt * mt * last.y + 2.0f * mt * t * seg->p0.y + t * t * seg->p1.y
                    );
                    test_edge(prev, cur);
                    prev = cur;
                }
                last = seg->p1;
                break;
            }
            case PathSegment::Type::CubicTo: {
                Point prev = last;
                for (int i = 1; i <= 6; ++i) {
                    float t = i / 6.0f;
                    float mt = 1.0f - t;
                    Point cur = Point::from_xy(
                        mt * mt * mt * last.x + 3.0f * mt * mt * t * seg->p0.x + 3.0f * mt * t * t * seg->p1.x + t * t * t * seg->p2.x,
                        mt * mt * mt * last.y + 3.0f * mt * mt * t * seg->p0.y + 3.0f * mt * t * t * seg->p1.y + t * t * t * seg->p2.y
                    );
                    test_edge(prev, cur);
                    prev = cur;
                }
                last = seg->p2;
                break;
            }
            case PathSegment::Type::Close:
                break;
        }
    }
    return (crossings % 2) != 0;
}

} // anonymous namespace

// ── Mask ──────────────────────────────────────────────────────────────────

Mask Mask::parse(const json::JsonValue& val) {
    Mask m;
    if (const auto* mode_val = val.get("mode")) {
        if (mode_val->is_string()) {
            std::string_view s = mode_val->as_string();
            if (s == "a") m.mode = MaskMode::Add;
            else if (s == "s") m.mode = MaskMode::Subtract;
            else if (s == "i") m.mode = MaskMode::Intersect;
            else if (s == "f") m.mode = MaskMode::Difference;
            else m.mode = MaskMode::None;
        } else if (mode_val->is_number()) {
            m.mode = static_cast<MaskMode>(mode_val->to_int());
        }
    }
    if (const auto* pt = val.get("pt")) {
        m.shape = parse_shape_property(*pt);
    }
    if (const auto* o = val.get("o")) {
        m.opacity = parse_float_property(*o, 100.0f);
    }
    if (const auto* x = val.get("x")) {
        m.expansion = parse_float_property(*x, 0.0f);
    }
    if (const auto* f = val.get("f")) {
        m.feather = parse_point_property(*f, Point::zero());
    }
    m.inverted = val.get_bool("inv", false);
    return m;
}

// ── LayerEffect ("ef") parsing & evaluation live in src/lottie/effects.cpp ──

// ── TextDocument & TextAnimator ("ty": 5) ─────────────────────────────────

TextDocument TextDocument::parse(const json::JsonValue& val) {
    TextDocument doc;
    if (!val.is_object()) return doc;
    doc.text = std::string(val.get_string("t", ""));
    doc.font_name = std::string(val.get_string("f", ""));
    doc.font_size = val.get_float("s", 24.0f);
    doc.line_height = val.get_float("lh", doc.font_size * 1.2f);
    doc.tracking = val.get_float("tr", 0.0f);
    doc.baseline_shift = val.get_float("ls", 0.0f);
    doc.stroke_width = val.get_float("sw", 0.0f);
    doc.stroke_over_fill = val.get_bool("of", false);
    int j = val.get_int("j", 0);
    if (j >= 0 && j <= 2) doc.justify = static_cast<TextJustify>(j);

    if (const auto* fc = val.get("fc")) doc.fill_color = parse_json_rgb_array(*fc, Color::BLACK);
    if (const auto* sc = val.get("sc")) doc.stroke_color = parse_json_rgb_array(*sc, Color::BLACK);
    if (const auto* ps = val.get("ps"); ps && ps->is_array() && ps->as_array().size() >= 2) {
        doc.box_position = Point::from_xy(ps->as_array()[0].to_float(), ps->as_array()[1].to_float());
    }
    if (const auto* sz = val.get("sz"); sz && sz->is_array() && sz->as_array().size() >= 2) {
        doc.box_size = Point::from_xy(sz->as_array()[0].to_float(), sz->as_array()[1].to_float());
    }
    return doc;
}

float TextRangeSelector::compute_factor(size_t index, size_t total_count, float frame) const noexcept {
    if (total_count == 0) return 0.0f;
    float s = start.evaluate(frame) + offset.evaluate(frame);
    float e = end.evaluate(frame) + offset.evaluate(frame);
    if (units == 1) {
        // Percentage [0..100] mapped to domain unit index
        s *= 0.01f * static_cast<float>(total_count);
        e *= 0.01f * static_cast<float>(total_count);
    }
    if (s > e) std::swap(s, e);

    float unit_start = static_cast<float>(index);
    float unit_end = static_cast<float>(index + 1);
    if (e <= unit_start || s >= unit_end) return 0.0f;

    float span = e - s;
    if (span <= 1e-5f) return 0.0f;

    float overlap_start = std::max(s, unit_start);
    float overlap_end = std::min(e, unit_end);
    float coverage = std::clamp(overlap_end - overlap_start, 0.0f, 1.0f);

    float raw = coverage;
    if (shape != 1) {
        float mid = std::clamp((unit_start + 0.5f - s) / span, 0.0f, 1.0f);
        switch (shape) {
            case 2: raw = mid; break;                                                       // Ramp Up
            case 3: raw = 1.0f - mid; break;                                                // Ramp Down
            case 4: raw = 1.0f - std::abs(2.0f * mid - 1.0f); break;                        // Triangle
            case 5: raw = std::sin(mid * static_cast<float>(M_PI)); break;                  // Round
            case 6: raw = 0.5f * (1.0f - std::cos(mid * 2.0f * static_cast<float>(M_PI))); break; // Smooth
            default: raw = coverage; break;
        }
    }

    // Apply ease high / ease low shaping ("xe" / "ne")
    const float xe = std::clamp(max_ease.evaluate(frame) * 0.01f, -1.0f, 1.0f);
    const float ne = std::clamp(min_ease.evaluate(frame) * 0.01f, -1.0f, 1.0f);
    if (std::abs(xe) > 1e-4f || std::abs(ne) > 1e-4f) {
        const EasingHandle out_h{
            std::clamp(0.333f + ne * 0.333f, 0.0f, 1.0f),
            std::clamp(0.333f - ne * 0.333f, 0.0f, 1.0f)
        };
        const EasingHandle in_h{
            std::clamp(0.667f - xe * 0.333f, 0.0f, 1.0f),
            std::clamp(0.667f + xe * 0.333f, 0.0f, 1.0f)
        };
        raw = solve_bezier_easing(raw, out_h, in_h);
    }

    // Apply smoothness ("sm" [0..100]%): 0% -> hard binary step, 100% -> full ramp
    const float sm = std::clamp(smoothness.evaluate(frame) * 0.01f, 0.0f, 1.0f);
    if (sm < 0.999f) {
        const float hard = (raw >= 0.5f) ? 1.0f : 0.0f;
        raw = hard + (raw - hard) * sm;
    }

    const float amt = std::clamp(max_amount.evaluate(frame) * 0.01f, -1.0f, 1.0f);
    return raw * amt;
}

bool TextRangeSelector::is_animated() const noexcept {
    return start.is_animated() || end.is_animated() || offset.is_animated() ||
           smoothness.is_animated() || max_amount.is_animated() ||
           max_ease.is_animated() || min_ease.is_animated();
}

void TextRangeSelector::bind_expression_manager(ExpressionManager& mgr) {
    start.bind_expression_manager(mgr);
    end.bind_expression_manager(mgr);
    offset.bind_expression_manager(mgr);
    smoothness.bind_expression_manager(mgr);
    max_amount.bind_expression_manager(mgr);
    max_ease.bind_expression_manager(mgr);
    min_ease.bind_expression_manager(mgr);
}

TextRangeSelector TextRangeSelector::parse(const json::JsonValue& val) {
    TextRangeSelector sel;
    if (!val.is_object()) return sel;
    if (const auto* s = val.get("s")) sel.start = parse_float_property(*s, 0.0f);
    if (const auto* e = val.get("e")) sel.end = parse_float_property(*e, 100.0f);
    if (const auto* o = val.get("o")) sel.offset = parse_float_property(*o, 0.0f);
    if (const auto* sm = val.get("sm")) sel.smoothness = parse_float_property(*sm, 100.0f);
    if (const auto* a = val.get("a")) sel.max_amount = parse_float_property(*a, 100.0f);
    if (const auto* xe = val.get("xe")) sel.max_ease = parse_float_property(*xe, 0.0f);
    if (const auto* ne = val.get("ne")) sel.min_ease = parse_float_property(*ne, 0.0f);
    sel.units = val.get_int("r", 1);
    sel.based_on = val.get_int("b", 1);
    sel.shape = val.get_int("sh", 1);
    sel.mode = val.get_int("m", 1);
    return sel;
}

TextPathOptions TextPathOptions::parse(const json::JsonValue& val) {
    TextPathOptions opt;
    if (!val.is_object()) return opt;
    if (const auto* m = val.get("m")) {
        int midx = m->to_int(-1);
        // In Lottie JSON, "m" is 0-based mask index (some exporters emit 1-based when > 0; check bounds at render)
        if (midx >= 0) {
            opt.mask_index = midx;
            opt.enabled = true;
        }
    }
    if (const auto* f = val.get("f")) opt.first_margin = parse_float_property(*f, 0.0f);
    if (const auto* l = val.get("l")) opt.last_margin = parse_float_property(*l, 0.0f);
    if (const auto* p = val.get("p")) opt.perpendicular_to_path = (p->to_int(1) != 0);
    if (const auto* r = val.get("r")) opt.reverse_path = (r->to_int(0) != 0);
    if (const auto* a = val.get("a")) opt.force_alignment = (a->to_int(0) != 0);
    return opt;
}

void TextAnimator::bind_expression_manager(ExpressionManager& mgr) {
    selector.bind_expression_manager(mgr);
    for (auto& s : selectors) s.bind_expression_manager(mgr);
    position.bind_expression_manager(mgr);
    scale.bind_expression_manager(mgr);
    rotation.bind_expression_manager(mgr);
    opacity.bind_expression_manager(mgr);
    fill_color.bind_expression_manager(mgr);
    stroke_color.bind_expression_manager(mgr);
    stroke_width.bind_expression_manager(mgr);
    tracking.bind_expression_manager(mgr);
}

TextAnimator TextAnimator::parse(const json::JsonValue& val) {
    TextAnimator anim;
    if (!val.is_object()) return anim;
    anim.name = val.get_string("nm", "");
    if (const auto* s = val.get("s")) {
        if (s->is_object()) {
            anim.selector = TextRangeSelector::parse(*s);
            anim.selectors.push_back(anim.selector);
        } else if (s->is_array() && !s->as_array().empty()) {
            for (const auto& s_item : s->as_array()) {
                if (s_item.is_object()) {
                    anim.selectors.push_back(TextRangeSelector::parse(s_item));
                }
            }
            if (!anim.selectors.empty()) anim.selector = anim.selectors.front();
        }
    }
    if (const auto* a = val.get("a"); a && a->is_object()) {
        if (const auto* p = a->get("p")) { anim.position = parse_point_property(*p, Point::zero()); anim.has_position = true; }
        if (const auto* sc = a->get("s")) { anim.scale = parse_point_property(*sc, Point::from_xy(100.0f, 100.0f)); anim.has_scale = true; }
        if (const auto* r = a->get("r")) { anim.rotation = parse_float_property(*r, 0.0f); anim.has_rotation = true; }
        if (const auto* o = a->get("o")) { anim.opacity = parse_float_property(*o, 100.0f); anim.has_opacity = true; }
        if (const auto* fc = a->get("fc")) { anim.fill_color = parse_color_property(*fc, Color::BLACK); anim.has_fill_color = true; }
        if (const auto* sc = a->get("sc")) { anim.stroke_color = parse_color_property(*sc, Color::BLACK); anim.has_stroke_color = true; }
        if (const auto* sw = a->get("sw")) { anim.stroke_width = parse_float_property(*sw, 0.0f); anim.has_stroke_width = true; }
        if (const auto* t = a->get("t")) { anim.tracking = parse_float_property(*t, 0.0f); anim.has_tracking = true; }
    }
    return anim;
}

// ── LottieLayer Hierarchy & Transforms ────────────────────────────────────

void link_layer_hierarchy(std::vector<std::shared_ptr<LottieLayer>>& layers) {
    std::unordered_map<int, const LottieLayer*> index_map;
    index_map.reserve(layers.size());
    for (const auto& l : layers) {
        if (l && l->index >= 0) {
            index_map[l->index] = l.get();
        }
    }

    for (size_t i = 0; i < layers.size(); ++i) {
        auto& l = layers[i];
        if (!l) continue;

        l->parent_ptr = nullptr;
        if (l->parent_index >= 0) {
            auto it = index_map.find(l->parent_index);
            if (it != index_map.end() && it->second != l.get()) {
                l->parent_ptr = it->second;
            }
        }

        l->matte_layer_ptr = nullptr;
        if (l->matte_type != MatteType::None) {
            if (l->matte_parent_index >= 0) {
                auto it = index_map.find(l->matte_parent_index);
                if (it != index_map.end()) {
                    l->matte_layer_ptr = it->second;
                }
            } else if (i > 0 && layers[i - 1]) {
                // In Bodymovin, if "tp" is omitted, the layer immediately above (i - 1) is the matte source
                l->matte_layer_ptr = layers[i - 1].get();
            }
        }
    }
}

Transform LottieLayer::compute_global_transform(float frame) const noexcept {
    Transform accum = transform.matrix(frame);
    const LottieLayer* curr = parent_ptr;
    int depth = 0;
    while (curr && depth < 32) {
        Transform p_tr = curr->transform.matrix(frame);
        accum = p_tr.pre_concat(accum);
        curr = curr->parent_ptr;
        depth++;
    }
    return accum;
}

Transform LottieLayer::compute_global_transform(
    float frame,
    const std::unordered_map<int, std::shared_ptr<LottieLayer>>& layer_map) const {
    if (parent_ptr || parent_index < 0) {
        return compute_global_transform(frame);
    }
    Transform accum = transform.matrix(frame);
    int curr_parent = parent_index;
    int depth = 0;
    while (curr_parent >= 0 && depth < 32) {
        auto it = layer_map.find(curr_parent);
        if (it == layer_map.end() || !it->second) break;
        Transform p_tr = it->second->transform.matrix(frame);
        accum = p_tr.pre_concat(accum);
        curr_parent = it->second->parent_index;
        depth++;
    }
    return accum;
}

bool LottieLayer::has_3d_hierarchy() const noexcept {
    const LottieLayer* curr = this;
    int depth = 0;
    while (curr && depth < 32) {
        if (curr->transform.is_3d) return true;
        curr = curr->parent_ptr;
        depth++;
    }
    return false;
}

Transform4x4 LottieLayer::compute_global_transform_3d(float frame, float comp_w, float comp_h) const noexcept {
    Transform4x4 accum = transform.matrix_3d(frame);
    const LottieLayer* curr = parent_ptr;
    int depth = 0;
    while (curr && depth < 32) {
        Transform4x4 p_tr = curr->transform.matrix_3d(frame);
        accum = p_tr * accum;
        curr = curr->parent_ptr;
        depth++;
    }
    // Apply centered perspective projection so Z rotations have realistic 3D foreshortening
    float cx = comp_w * 0.5f;
    float cy = comp_h * 0.5f;
    float fov_d = std::max(comp_w, comp_h) * 2.0f;
    Transform4x4 persp = Transform4x4::from_translate(cx, cy, 0.0f)
                       * Transform4x4::from_perspective(fov_d)
                       * Transform4x4::from_translate(-cx, -cy, 0.0f);
    return persp * accum;
}

void LottieLayer::render_with_assets(
    RenderContext& ctx,
    float frame,
    const LottieAssetRegistry& assets) const {
    std::unordered_map<int, std::shared_ptr<LottieLayer>> empty_map;
    render(ctx, frame, empty_map, assets.precomps);
}

void LottieLayer::render(
    RenderContext& ctx,
    float frame,
    const std::unordered_map<int, std::shared_ptr<LottieLayer>>&,
    const std::unordered_map<std::string, std::vector<std::shared_ptr<LottieLayer>>>& precomp_assets) const {
    LottieAssetRegistry temp_assets;
    temp_assets.precomps = precomp_assets;
    render_with_assets(ctx, frame, temp_assets);
}

bool LottieLayer::hit_test(float, float, float, const LottieAssetRegistry&, std::string&) const {
    return false;
}

std::shared_ptr<LottieLayer> LottieLayer::parse(const json::JsonValue& val) {
    if (!val.is_object()) return nullptr;

    int type_int = val.get_int("ty", 3);
    LayerType layer_type = static_cast<LayerType>(type_int);

    std::shared_ptr<LottieLayer> layer;
    switch (layer_type) {
        case LayerType::Precomp:
            layer = PrecompLayer::parse(val);
            break;
        case LayerType::Solid:
            layer = SolidLayer::parse(val);
            break;
        case LayerType::Image:
            layer = ImageLayer::parse(val);
            break;
        case LayerType::Null:
            layer = NullLayer::parse(val);
            break;
        case LayerType::Shape:
            layer = ShapeLayer::parse(val);
            break;
        case LayerType::Text:
            layer = TextLayer::parse(val);
            break;
        case LayerType::Audio:
            layer = AudioLayer::parse(val);
            break;
        default:
            layer = NullLayer::parse(val);
            break;
    }

    if (layer) {
        layer->type = layer_type;
    }
    return layer;
}

// ── ShapeLayer ("ty": 4) ──────────────────────────────────────────────────

void ShapeLayer::render_with_assets(
    RenderContext& ctx,
    float frame,
    const LottieAssetRegistry&) const {

    if (!is_active(frame)) return;

    ctx.canvas.save();
    Transform global_tr = compute_global_transform(frame);
    ctx.canvas.concat(global_tr);

    float layer_opacity = ctx.parent_opacity * transform.eval_opacity(frame);
    if (layer_opacity > 0.001f) {
        RenderContext layer_ctx(ctx.canvas, layer_opacity);
        layer_ctx.active_blend_mode = blend_mode;
        render_shape_items(shapes, layer_ctx, frame);
    }

    ctx.canvas.restore();
}

bool ShapeLayer::hit_test(float x, float y, float frame, const LottieAssetRegistry&, std::string& out_hit_name) const {
    if (!is_active(frame)) return false;
    Transform global_tr = compute_global_transform(frame);
    auto inv = global_tr.invert();
    if (!inv) return false;
    Point local_pt = Point::from_xy(x, y);
    inv->map_points(&local_pt, 1);

    for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
        const auto& item = *it;
        if (!item || item->hidden) continue;
        Path p;
        item->collect_geometry(frame, p);
        if (point_in_path_approx(p, local_pt.x, local_pt.y)) {
            out_hit_name = !name.empty() ? name : item->name;
            return true;
        }
    }
    return false;
}

std::shared_ptr<ShapeLayer> ShapeLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<ShapeLayer>();
    parse_common_layer_props(*layer, val);

    const json::JsonValue* items_val = val.get("shapes");
    if (!items_val) items_val = val.get("it");

    if (items_val && items_val->is_array()) {
        for (const auto& item_json : items_val->as_array()) {
            if (auto item = LottieShapeItem::parse(item_json)) {
                layer->shapes.push_back(std::move(item));
            }
        }
    }

    return layer;
}

// ── SolidLayer ("ty": 1) ──────────────────────────────────────────────────

void SolidLayer::render_with_assets(
    RenderContext& ctx,
    float frame,
    const LottieAssetRegistry&) const {

    if (!is_active(frame)) return;

    ctx.canvas.save();
    Transform global_tr = compute_global_transform(frame);
    ctx.canvas.concat(global_tr);

    float layer_opacity = ctx.parent_opacity * transform.eval_opacity(frame);
    if (width > 0.0f && height > 0.0f && layer_opacity > 0.001f) {
        Paint paint;
        Color c = color_override.value_or(solid_color);
        c.apply_opacity(layer_opacity);
        paint.set_color(c);
        paint.blend_mode = blend_mode;
        if (auto r = Rect::from_xywh(0.0f, 0.0f, width, height)) {
            ctx.canvas.fill_rect(*r, paint);
        }
    }

    ctx.canvas.restore();
}

bool SolidLayer::hit_test(float x, float y, float frame, const LottieAssetRegistry&, std::string& out_hit_name) const {
    if (!is_active(frame) || width <= 0.0f || height <= 0.0f) return false;
    Transform global_tr = compute_global_transform(frame);
    auto inv = global_tr.invert();
    if (!inv) return false;
    Point pt = Point::from_xy(x, y);
    inv->map_points(&pt, 1);
    if (pt.x >= 0.0f && pt.x <= width && pt.y >= 0.0f && pt.y <= height) {
        out_hit_name = name;
        return true;
    }
    return false;
}

std::shared_ptr<SolidLayer> SolidLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<SolidLayer>();
    parse_common_layer_props(*layer, val);
    layer->width = val.get_float("sw", 0.0f);
    layer->height = val.get_float("sh", 0.0f);
    layer->solid_color = parse_hex_color(val.get_string("sc", "#000000"));
    return layer;
}

// ── PrecompLayer ("ty": 0) ────────────────────────────────────────────────

void PrecompLayer::render_with_assets(
    RenderContext& ctx,
    float frame,
    const LottieAssetRegistry& assets) const {

    if (!is_active(frame)) return;

    const float layer_opacity = ctx.parent_opacity * transform.eval_opacity(frame);
    if (layer_opacity <= 0.001f) return;

    float sub_frame = (frame - start_time) / time_stretch;
    if (has_time_remap) {
        sub_frame = time_remap.evaluate(frame);
    }

    // Intercepted ExternalLayer path (live UI widget, video, camera feed, custom renderer)
    if (external_layer) {
        ctx.canvas.save();
        Transform global_tr = compute_global_transform(frame);
        ctx.canvas.concat(global_tr);

        std::optional<Rect> clip_r;
        if (width > 0.0f && height > 0.0f) {
            clip_r = Rect::from_xywh(0.0f, 0.0f, width, height);
            if (clip_r) {
                ctx.canvas.clip_rect(*clip_r);
            }
        }

        const bool needs_layer_wrap = (layer_opacity < 0.999f) || (blend_mode != nisaba::BlendMode::SourceOver);
        if (needs_layer_wrap) {
            Paint lp(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(layer_opacity, 0.0f, 1.0f)));
            lp.blend_mode = blend_mode;
            if (clip_r) {
                ctx.canvas.save_layer_bounds(*clip_r, lp);
            } else {
                ctx.canvas.save_layer(lp);
            }
        }

        const float fps = (assets.frame_rate > 0.0f) ? assets.frame_rate : 60.0f;
        external_layer->render(ctx.canvas, sub_frame / fps);

        if (needs_layer_wrap) {
            ctx.canvas.restore();
        }
        ctx.canvas.restore();
        return;
    }

    auto it = assets.precomps.find(ref_id);
    if (it == assets.precomps.end()) return;
    const auto& sub_layers = it->second;
    if (sub_layers.empty()) return;

    ctx.canvas.save();
    Transform global_tr = compute_global_transform(frame);
    ctx.canvas.concat(global_tr);

    if (width > 0.0f && height > 0.0f) {
        if (auto clip_r = Rect::from_xywh(0.0f, 0.0f, width, height)) {
            ctx.canvas.clip_rect(*clip_r);
        }
    }

    render_layer_tree_with_assets(ctx.canvas, sub_layers, sub_frame, assets, layer_opacity);

    ctx.canvas.restore();
}

bool PrecompLayer::hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const {
    if (!is_active(frame)) return false;

    Transform global_tr = compute_global_transform(frame);
    auto inv = global_tr.invert();
    if (!inv) return false;
    Point pt = Point::from_xy(x, y);
    inv->map_points(&pt, 1);

    if (external_layer) {
        if (width > 0.0f && height > 0.0f &&
            pt.x >= 0.0f && pt.x <= width && pt.y >= 0.0f && pt.y <= height) {
            out_hit_name = name;
            return true;
        }
        return false;
    }

    auto it = assets.precomps.find(ref_id);
    if (it == assets.precomps.end()) return false;

    float sub_frame = has_time_remap ? time_remap.evaluate(frame) : (frame - start_time) / time_stretch;
    for (const auto& sub : it->second) {
        if (sub && sub->hit_test(pt.x, pt.y, sub_frame, assets, out_hit_name)) {
            if (out_hit_name.empty()) out_hit_name = name;
            return true;
        }
    }
    return false;
}

std::shared_ptr<PrecompLayer> PrecompLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<PrecompLayer>();
    parse_common_layer_props(*layer, val);
    layer->ref_id = val.get_string("refId", "");
    layer->width = val.get_float("w", 0.0f);
    layer->height = val.get_float("h", 0.0f);
    if (const auto* tm_val = val.get("tm")) {
        layer->time_remap = parse_float_property(*tm_val, 0.0f);
        layer->has_time_remap = true;
    }
    return layer;
}

// ── NullLayer ("ty": 3) ───────────────────────────────────────────────────

void NullLayer::render_with_assets(
    RenderContext&,
    float,
    const LottieAssetRegistry&) const {
    // Null layers produce no visible geometry
}

std::shared_ptr<NullLayer> NullLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<NullLayer>();
    parse_common_layer_props(*layer, val);
    return layer;
}

// ── ImageLayer ("ty": 2) ──────────────────────────────────────────────────

void ImageLayer::render_with_assets(
    RenderContext& ctx,
    float frame,
    const LottieAssetRegistry& assets) const {

    if (!is_active(frame)) return;
    auto it = assets.images.find(ref_id);
    if (it == assets.images.end()) return;

    const float fps = (assets.frame_rate > 0.0f) ? assets.frame_rate : 60.0f;
    const float local_frame = (frame - start_time) / time_stretch;
    const float t_seconds = std::max(0.0f, local_frame / fps);
    auto frame_ref = it->second.resolve_frame(t_seconds);
    if (!frame_ref.has_value() || frame_ref->width() == 0 || frame_ref->height() == 0) return;

    float layer_opacity = ctx.parent_opacity * transform.eval_opacity(frame);
    if (layer_opacity <= 0.001f) return;

    ctx.canvas.save();
    Transform global_tr = compute_global_transform(frame);

    // Scale image to asset's declared dimensions if they differ from decoded pixmap size
    float sx = (it->second.width > 0.0f) ? (it->second.width / static_cast<float>(frame_ref->width())) : 1.0f;
    float sy = (it->second.height > 0.0f) ? (it->second.height / static_cast<float>(frame_ref->height())) : 1.0f;
    if (std::abs(sx - 1.0f) > 1e-4f || std::abs(sy - 1.0f) > 1e-4f) {
        global_tr = global_tr.pre_concat(Transform::from_scale(sx, sy));
    }

    ctx.canvas.concat(global_tr);

    PixmapPaint paint;
    paint.opacity = layer_opacity;
    paint.blend_mode = blend_mode;
    paint.quality = FilterQuality::Bilinear;

    const float fw = static_cast<float>(frame_ref->width());
    const float fh = static_cast<float>(frame_ref->height());
    if (auto sr = Rect::from_xywh(0.0f, 0.0f, fw, fh)) {
        ctx.canvas.draw_pixmap_rect(*frame_ref, *sr, *sr, paint);
    }
    ctx.canvas.restore();
}

bool ImageLayer::hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const {
    if (!is_active(frame)) return false;
    auto it = assets.images.find(ref_id);
    if (it == assets.images.end()) return false;
    float w = it->second.width;
    float h = it->second.height;
    if (w <= 0.0f && it->second.asset) w = static_cast<float>(it->second.asset->width());
    if (h <= 0.0f && it->second.asset) h = static_cast<float>(it->second.asset->height());
    if (w <= 0.0f && it->second.pixmap) w = static_cast<float>(it->second.pixmap->width());
    if (h <= 0.0f && it->second.pixmap) h = static_cast<float>(it->second.pixmap->height());
    if (w <= 0.0f || h <= 0.0f) return false;

    Transform global_tr = compute_global_transform(frame);
    auto inv = global_tr.invert();
    if (!inv) return false;
    Point pt = Point::from_xy(x, y);
    inv->map_points(&pt, 1);
    if (pt.x >= 0.0f && pt.x <= w && pt.y >= 0.0f && pt.y <= h) {
        out_hit_name = name;
        return true;
    }
    return false;
}

std::shared_ptr<ImageLayer> ImageLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<ImageLayer>();
    parse_common_layer_props(*layer, val);
    layer->ref_id = val.get_string("refId", "");
    return layer;
}

// ── TextLayer ("ty": 5) ───────────────────────────────────────────────────

const TextDocument& TextLayer::evaluate_document(float frame) const noexcept {
    if (doc_override.has_value()) return *doc_override;
    if (keyframes.empty()) return static_doc;
    for (size_t i = keyframes.size(); i > 0; --i) {
        if (frame >= keyframes[i - 1].time) {
            return keyframes[i - 1].start_value;
        }
    }
    return keyframes.front().start_value;
}

struct TextLayer::TextRenderCache {
    struct CachedGlyph {
        std::string utf8_cluster;
        char32_t shaped_cp = 0;
        const LottieGlyphChar* lottie_char = nullptr;
        float advance = 0.0f;
        size_t line_idx = 0;
        size_t global_char_idx = 0;
        size_t non_space_idx = 0;
        size_t word_idx = 0;
        bool is_space = false;
        Path static_path;
        bool has_animated_lottie_char = false;
    };

    // Layout cache key
    bool layout_valid = false;
    std::string cached_text;
    std::string cached_font_name;
    float cached_font_size = 0.0f;
    float cached_tracking = 0.0f;
    const text::FontSystem* cached_font_system = nullptr;
    size_t cached_assets_chars_count = 0;

    // Cached layout results
    std::vector<std::vector<CachedGlyph>> line_glyphs;
    std::vector<float> line_widths;
    size_t total_chars = 0;
    size_t total_non_spaces = 0;
    size_t total_words = 0;

    // Text-on-path cache key & result
    bool path_valid = false;
    int cached_mask_index = -1;
    bool cached_reverse_path = false;
    bool cached_mask_animated = false;
    float cached_mask_frame = -1e30f;
    PathMeasure path_measure;
    float path_total_len = 0.0f;
};

void TextLayer::render_with_assets(
    RenderContext& ctx,
    float frame,
    const LottieAssetRegistry& assets) const {

    if (!is_active(frame)) return;
    const TextDocument& doc = evaluate_document(frame);

    std::string evaluated_expr_text;
    std::string_view raw_text;
    if (text_override.has_value()) {
        raw_text = std::string_view(*text_override);
    } else if (!doc_override.has_value() && text_evaluator) {
        const float fps = (assets.frame_rate > 0.0f) ? assets.frame_rate : 60.0f;
        evaluated_expr_text = text_evaluator->evaluate(frame / fps);
        raw_text = evaluated_expr_text;
    } else {
        raw_text = std::string_view(doc.text);
    }
    if (raw_text.empty() || doc.font_size <= 0.0f) return;

    float base_opacity = ctx.parent_opacity * transform.eval_opacity(frame);
    if (base_opacity <= 0.001f) return;

    if (!render_cache_) {
        render_cache_ = std::make_shared<TextRenderCache>();
    }
    TextRenderCache& cache = *render_cache_;

    const float scale_100 = doc.font_size / 100.0f;
    const float tracking_px = (doc.tracking / 1000.0f) * doc.font_size;

    const bool layout_hit = cache.layout_valid
        && cache.cached_text == raw_text
        && cache.cached_font_name == doc.font_name
        && cache.cached_font_size == doc.font_size
        && cache.cached_tracking == doc.tracking
        && cache.cached_font_system == assets.font_system.get()
        && cache.cached_assets_chars_count == assets.chars.size();

    if (!layout_hit) {
        cache.layout_valid = true;
        cache.cached_text.assign(raw_text.data(), raw_text.size());
        cache.cached_font_name = doc.font_name;
        cache.cached_font_size = doc.font_size;
        cache.cached_tracking = doc.tracking;
        cache.cached_font_system = assets.font_system.get();
        cache.cached_assets_chars_count = assets.chars.size();

        // Split text into lines by '\r' or '\n'
        std::vector<std::string_view> lines;
        size_t pos = 0;
        while (pos <= raw_text.size()) {
            size_t next_r = raw_text.find_first_of("\r\n", pos);
            if (next_r == std::string_view::npos) {
                lines.push_back(raw_text.substr(pos));
                break;
            }
            lines.push_back(raw_text.substr(pos, next_r - pos));
            if (raw_text[next_r] == '\r' && next_r + 1 < raw_text.size() && raw_text[next_r + 1] == '\n') {
                pos = next_r + 2;
            } else {
                pos = next_r + 1;
            }
        }

        // Resolve font definition & TtfFont once per layout change
        std::string font_family;
        std::string font_style;
        const text::TtfFont* ttf_font = nullptr;
        auto fit = assets.fonts.find(doc.font_name);
        if (fit != assets.fonts.end()) {
            font_family = fit->second.family;
            font_style = fit->second.style;
            if (assets.font_system && fit->second.loaded_font_id) {
                ttf_font = assets.font_system->get_font(*fit->second.loaded_font_id);
            }
        }
        if (!ttf_font && assets.font_system) {
            auto [fid, fptr] = assets.font_system->match_font(
                font_family.empty() ? text::Family::sans_serif() : text::Family(font_family)
            );
            ttf_font = fptr;
        }

        cache.line_glyphs.assign(lines.size(), {});
        cache.line_widths.assign(lines.size(), 0.0f);
        cache.total_chars = 0;
        cache.total_non_spaces = 0;
        cache.total_words = 0;

        for (size_t li = 0; li < lines.size(); ++li) {
            bool in_word = false;
            std::string_view ln = lines[li];
            if (ln.empty()) continue;

            auto runs = text::Bidi::segment_runs(ln);
            auto shaped = text::Bidi::shape_text(ln, runs);
            cache.line_glyphs[li].reserve(shaped.size());

            for (size_t ci = 0; ci < shaped.size(); ++ci) {
                const auto& sc = shaped[ci];
                TextRenderCache::CachedGlyph gi;
                gi.shaped_cp = sc.codepoint;
                gi.line_idx = li;
                gi.global_char_idx = cache.total_chars++;
                gi.is_space = (sc.codepoint == ' ' || sc.codepoint == '\t');
                if (!gi.is_space) {
                    gi.non_space_idx = cache.total_non_spaces++;
                    if (!in_word) {
                        in_word = true;
                        ++cache.total_words;
                    }
                    gi.word_idx = cache.total_words - 1;
                } else {
                    in_word = false;
                    gi.non_space_idx = cache.total_non_spaces;
                    gi.word_idx = cache.total_words;
                }

                char32_t lookup_cp = (sc.original_codepoint != 0) ? sc.original_codepoint : sc.codepoint;
                char buf[5] = {0};
                if (lookup_cp < 0x80) {
                    buf[0] = static_cast<char>(lookup_cp);
                    gi.utf8_cluster = std::string(buf, 1);
                } else if (lookup_cp < 0x800) {
                    buf[0] = static_cast<char>(0xC0 | (lookup_cp >> 6));
                    buf[1] = static_cast<char>(0x80 | (lookup_cp & 0x3F));
                    gi.utf8_cluster = std::string(buf, 2);
                } else if (lookup_cp < 0x10000) {
                    buf[0] = static_cast<char>(0xE0 | (lookup_cp >> 12));
                    buf[1] = static_cast<char>(0x80 | ((lookup_cp >> 6) & 0x3F));
                    buf[2] = static_cast<char>(0x80 | (lookup_cp & 0x3F));
                    gi.utf8_cluster = std::string(buf, 3);
                } else {
                    buf[0] = static_cast<char>(0xF0 | (lookup_cp >> 18));
                    buf[1] = static_cast<char>(0x80 | ((lookup_cp >> 12) & 0x3F));
                    buf[2] = static_cast<char>(0x80 | ((lookup_cp >> 6) & 0x3F));
                    buf[3] = static_cast<char>(0x80 | (lookup_cp & 0x3F));
                    gi.utf8_cluster = std::string(buf, 4);
                }

                gi.lottie_char = assets.find_char(gi.utf8_cluster, font_family, font_style);
                if (gi.lottie_char) {
                    gi.advance = gi.lottie_char->width * scale_100;
                    for (const auto& sg : gi.lottie_char->shapes) {
                        if (sg && sg->is_animated()) {
                            gi.has_animated_lottie_char = true;
                            break;
                        }
                    }
                    if (!gi.has_animated_lottie_char) {
                        for (const auto& sg : gi.lottie_char->shapes) {
                            if (sg) sg->collect_geometry(0.0f, gi.static_path);
                        }
                        gi.static_path.apply_transform(Transform::from_scale(scale_100, scale_100));
                    }
                } else if (ttf_font) {
                    uint16_t gid = ttf_font->glyph_index(sc.codepoint);
                    if (gid == 0 && sc.original_codepoint != 0) {
                        gid = ttf_font->glyph_index(sc.original_codepoint);
                    }
                    gi.advance = sc.is_mark ? 0.0f : ttf_font->glyph_advance(gid, doc.font_size);
                    if (gid != 0) {
                        ttf_font->get_glyph_path(gid, gi.static_path, doc.font_size);
                    }
                } else {
                    gi.advance = gi.is_space ? (doc.font_size * 0.3f) : (doc.font_size * 0.6f);
                    if (!gi.is_space) {
                        float gw = gi.advance * 0.75f;
                        float gh = doc.font_size * 0.7f;
                        PathBuilder pb;
                        pb.move_to(0.0f, -gh);
                        pb.line_to(gw, -gh);
                        pb.line_to(gw, 0.0f);
                        pb.line_to(0.0f, 0.0f);
                        pb.close();
                        if (auto p = pb.finish()) gi.static_path = std::move(*p);
                    }
                }

                cache.line_widths[li] += gi.advance;
                if (ci + 1 < shaped.size()) {
                    cache.line_widths[li] += tracking_px;
                }
                cache.line_glyphs[li].push_back(std::move(gi));
            }
        }
    }

    const auto& line_glyphs = cache.line_glyphs;
    const auto& line_widths = cache.line_widths;
    const size_t total_chars = cache.total_chars;
    const size_t total_non_spaces = cache.total_non_spaces;
    const size_t total_words = cache.total_words;
    const size_t num_lines = line_glyphs.size();

    // Optional Text-on-Path setup ("t.p" referencing a cubic Bezier mask path) — cached across frames when static
    bool use_text_path = false;
    float path_total_len = 0.0f;
    float path_first_margin = 0.0f;
    float path_last_margin = 0.0f;
    if (path_options.enabled && !masks.empty()) {
        int midx = path_options.mask_index;
        if (midx >= static_cast<int>(masks.size()) && midx > 0) {
            midx -= 1; // Handle 1-based mask index exporters cleanly
        }
        if (midx >= 0 && midx < static_cast<int>(masks.size())) {
            const auto& mask_shape = masks[static_cast<size_t>(midx)].shape;
            const bool mask_anim = mask_shape.is_animated();
            const bool path_hit = cache.path_valid
                && cache.cached_mask_index == midx
                && cache.cached_reverse_path == path_options.reverse_path
                && cache.cached_mask_animated == mask_anim
                && (!mask_anim || cache.cached_mask_frame == frame);
            if (!path_hit) {
                cache.cached_mask_index = midx;
                cache.cached_reverse_path = path_options.reverse_path;
                cache.cached_mask_animated = mask_anim;
                cache.cached_mask_frame = frame;
                cache.path_total_len = 0.0f;
                cache.path_valid = true;
                const BezierData bz = mask_shape.evaluate(frame);
                const Path mask_path = bz.to_path(path_options.reverse_path);
                if (!mask_path.is_empty()) {
                    cache.path_measure.set_path(mask_path, false);
                    cache.path_total_len = cache.path_measure.length();
                }
            }
            if (cache.path_total_len > 1e-3f) {
                use_text_path = true;
                path_total_len = cache.path_total_len;
                path_first_margin = path_options.first_margin.evaluate(frame);
                path_last_margin = path_options.last_margin.evaluate(frame);
            }
        }
    }

    auto eval_selector_for_glyph = [&](const TextRangeSelector& sel, const TextRenderCache::CachedGlyph& gi) noexcept -> float {
        switch (sel.based_on) {
            case 2: // Characters excluding spaces
                return gi.is_space ? 0.0f : sel.compute_factor(gi.non_space_idx, total_non_spaces, frame);
            case 3: // Words
                return gi.is_space ? 0.0f : sel.compute_factor(gi.word_idx, total_words, frame);
            case 4: // Lines
                return sel.compute_factor(gi.line_idx, num_lines, frame);
            case 1:
            default:
                return sel.compute_factor(gi.global_char_idx, total_chars, frame);
        }
    };

    auto eval_animator_factor = [&](const TextAnimator& anim, const TextRenderCache::CachedGlyph& gi) noexcept -> float {
        if (anim.selectors.empty()) {
            return eval_selector_for_glyph(anim.selector, gi);
        }
        float combined = eval_selector_for_glyph(anim.selectors[0], gi);
        for (size_t si = 1; si < anim.selectors.size(); ++si) {
            const float f = eval_selector_for_glyph(anim.selectors[si], gi);
            switch (anim.selectors[si].mode) {
                case 2: combined = std::clamp(combined - f, -1.0f, 1.0f); break;          // Subtract
                case 3: combined = combined * f; break;                                   // Intersect
                case 4: combined = std::min(combined, f); break;                          // Min
                case 5: combined = std::max(combined, f); break;                          // Max
                case 6: combined = std::abs(combined - f); break;                         // Difference
                case 1:
                default: combined = std::clamp(combined + f, -1.0f, 1.0f); break;         // Add
            }
        }
        return combined;
    };

    ctx.canvas.save();
    Transform global_tr = compute_global_transform(frame);
    ctx.canvas.concat(global_tr);

    for (size_t li = 0; li < num_lines; ++li) {
        float x_cursor = doc.box_position.x;
        if (use_text_path) {
            const float avail = std::max(0.0f, path_total_len - path_first_margin - path_last_margin);
            if (doc.justify == TextJustify::Center) {
                x_cursor = path_first_margin + (avail - line_widths[li]) * 0.5f;
            } else if (doc.justify == TextJustify::Right) {
                x_cursor = path_total_len - path_last_margin - line_widths[li];
            } else {
                x_cursor = path_first_margin;
            }
        } else {
            if (doc.justify == TextJustify::Right) {
                x_cursor += doc.box_size.x - line_widths[li];
            } else if (doc.justify == TextJustify::Center) {
                x_cursor += (doc.box_size.x - line_widths[li]) * 0.5f;
            }
        }
        float y_cursor = doc.box_position.y + static_cast<float>(li) * doc.line_height - doc.baseline_shift;
        const float line_count_f = static_cast<float>(line_glyphs[li].size());
        const float forced_step = (use_text_path && path_options.force_alignment && line_count_f > 1.0f)
            ? std::max(0.0f, (path_total_len - path_first_margin - path_last_margin) / (line_count_f - 1.0f))
            : 0.0f;

        for (size_t ci = 0; ci < line_glyphs[li].size(); ++ci) {
            const auto& gi = line_glyphs[li][ci];

            Point anim_pos = Point::zero();
            Point anim_scale = Point::from_xy(1.0f, 1.0f);
            float anim_rot = 0.0f;
            float anim_op = 1.0f;
            Color fill_c = doc.fill_color;
            Color stroke_c = doc.stroke_color;
            float stroke_w = doc.stroke_width;
            float extra_tracking = 0.0f;

            for (const auto& anim : animators) {
                const float factor = eval_animator_factor(anim, gi);
                if (std::abs(factor) <= 1e-5f) continue;

                if (anim.has_position) {
                    Point p = anim.position.evaluate(frame);
                    anim_pos.x += p.x * factor;
                    anim_pos.y += p.y * factor;
                }
                if (anim.has_scale) {
                    Point s = anim.scale.evaluate(frame);
                    anim_scale.x *= 1.0f + ((s.x * 0.01f) - 1.0f) * factor;
                    anim_scale.y *= 1.0f + ((s.y * 0.01f) - 1.0f) * factor;
                }
                if (anim.has_rotation) {
                    anim_rot += anim.rotation.evaluate(frame) * factor;
                }
                if (anim.has_opacity) {
                    float target_op = std::clamp(anim.opacity.evaluate(frame) * 0.01f, 0.0f, 1.0f);
                    anim_op *= (1.0f - factor) + target_op * factor;
                }
                if (anim.has_fill_color) {
                    Color tc = anim.fill_color.evaluate(frame);
                    fill_c = Color::from_rgba(
                        fill_c.red() + (tc.red() - fill_c.red()) * factor,
                        fill_c.green() + (tc.green() - fill_c.green()) * factor,
                        fill_c.blue() + (tc.blue() - fill_c.blue()) * factor,
                        fill_c.alpha()
                    ).value_or(fill_c);
                }
                if (anim.has_stroke_color) {
                    Color tc = anim.stroke_color.evaluate(frame);
                    stroke_c = Color::from_rgba(
                        stroke_c.red() + (tc.red() - stroke_c.red()) * factor,
                        stroke_c.green() + (tc.green() - stroke_c.green()) * factor,
                        stroke_c.blue() + (tc.blue() - stroke_c.blue()) * factor,
                        stroke_c.alpha()
                    ).value_or(stroke_c);
                }
                if (anim.has_stroke_width) {
                    stroke_w += anim.stroke_width.evaluate(frame) * factor;
                }
                if (anim.has_tracking) {
                    extra_tracking += anim.tracking.evaluate(frame) * factor;
                }
            }

            float char_op = base_opacity * anim_op;
            if (char_op > 0.001f) {
                Path animated_path;
                const Path* glyph_path_ptr = &gi.static_path;
                if (gi.has_animated_lottie_char && gi.lottie_char) {
                    for (const auto& sg : gi.lottie_char->shapes) {
                        if (sg) sg->collect_geometry(frame, animated_path);
                    }
                    animated_path.apply_transform(Transform::from_scale(scale_100, scale_100));
                    glyph_path_ptr = &animated_path;
                }

                if (!glyph_path_ptr->is_empty()) {
                    ctx.canvas.save();
                    if (use_text_path) {
                        const float arc_d = path_options.force_alignment
                            ? (path_first_margin + static_cast<float>(ci) * forced_step + anim_pos.x)
                            : (x_cursor + gi.advance * 0.5f + anim_pos.x);
                        Point pt = Point::zero();
                        Point tan = Point::from_xy(1.0f, 0.0f);
                        if (cache.path_measure.get_pos_tan(std::clamp(arc_d, 0.0f, path_total_len), &pt, &tan)) {
                            const Point normal = Point::from_xy(-tan.y, tan.x);
                            const float perp_off = static_cast<float>(li) * doc.line_height - doc.baseline_shift + anim_pos.y;
                            ctx.canvas.translate(pt.x + normal.x * perp_off, pt.y + normal.y * perp_off);
                            float total_rot = anim_rot;
                            if (path_options.perpendicular_to_path) {
                                total_rot += std::atan2(tan.y, tan.x) * static_cast<float>(180.0 / M_PI);
                            }
                            if (std::abs(total_rot) > 1e-4f) {
                                ctx.canvas.concat(Transform::from_rotate(total_rot));
                            }
                            if (std::abs(anim_scale.x - 1.0f) > 1e-4f || std::abs(anim_scale.y - 1.0f) > 1e-4f) {
                                ctx.canvas.scale(anim_scale.x, anim_scale.y);
                            }
                            ctx.canvas.translate(-gi.advance * 0.5f, 0.0f);
                        }
                    } else {
                        ctx.canvas.translate(x_cursor + anim_pos.x, y_cursor + anim_pos.y);
                        if (std::abs(anim_rot) > 1e-4f) {
                            ctx.canvas.concat(Transform::from_rotate(anim_rot));
                        }
                        if (std::abs(anim_scale.x - 1.0f) > 1e-4f || std::abs(anim_scale.y - 1.0f) > 1e-4f) {
                            ctx.canvas.scale(anim_scale.x, anim_scale.y);
                        }
                    }

                    auto draw_fill = [&]() {
                        Color fc = fill_c;
                        fc.apply_opacity(char_op);
                        Paint fp(fc);
                        fp.blend_mode = blend_mode;
                        ctx.canvas.fill_path(*glyph_path_ptr, fp, nisaba::FillRule::Winding);
                    };
                    auto draw_stroke = [&]() {
                        if (stroke_w > 0.0f) {
                            Color sc = stroke_c;
                            sc.apply_opacity(char_op);
                            Paint sp(sc);
                            sp.blend_mode = blend_mode;
                            Stroke st(stroke_w);
                            st.line_cap = nisaba::LineCap::Round;
                            st.line_join = nisaba::LineJoin::Round;
                            ctx.canvas.stroke_path(*glyph_path_ptr, sp, st);
                        }
                    };

                    if (doc.stroke_over_fill) {
                        draw_fill();
                        draw_stroke();
                    } else {
                        draw_stroke();
                        draw_fill();
                    }

                    ctx.canvas.restore();
                }
            }

            x_cursor += gi.advance + tracking_px + extra_tracking;
        }
    }

    ctx.canvas.restore();
}

bool TextLayer::hit_test(float x, float y, float frame, const LottieAssetRegistry&, std::string& out_hit_name) const {
    if (!is_active(frame)) return false;
    const TextDocument& doc = evaluate_document(frame);
    if (doc.text.empty() || doc.font_size <= 0.0f) return false;

    Transform global_tr = compute_global_transform(frame);
    auto inv = global_tr.invert();
    if (!inv) return false;
    Point pt = Point::from_xy(x, y);
    inv->map_points(&pt, 1);

    float approx_w = static_cast<float>(doc.text.size()) * doc.font_size * 0.6f;
    float approx_h = doc.font_size * 1.2f;
    float x0 = doc.box_position.x;
    if (doc.justify == TextJustify::Center) x0 -= approx_w * 0.5f;
    else if (doc.justify == TextJustify::Right) x0 -= approx_w;
    float y0 = doc.box_position.y - doc.font_size;

    if (pt.x >= x0 && pt.x <= x0 + approx_w && pt.y >= y0 && pt.y <= y0 + approx_h) {
        out_hit_name = name;
        return true;
    }
    return false;
}

std::shared_ptr<TextLayer> TextLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<TextLayer>();
    parse_common_layer_props(*layer, val);

    if (const auto* t_obj = val.get("t"); t_obj && t_obj->is_object()) {
        if (const auto* d_obj = t_obj->get("d"); d_obj && d_obj->is_object()) {
            if (const auto* x_val = d_obj->get("x"); x_val && x_val->is_string()) {
                layer->text_expression = std::string(x_val->as_string());
            }
            if (const auto* k_val = d_obj->get("k")) {
                if (k_val->is_array()) {
                    for (const auto& kf_json : k_val->as_array()) {
                        if (!kf_json.is_object()) continue;
                        Keyframe<TextDocument> kf;
                        kf.time = kf_json.get_float("t", 0.0f);
                        kf.is_hold = true;
                        if (const auto* s_val = kf_json.get("s")) {
                            kf.start_value = TextDocument::parse(*s_val);
                        }
                        layer->keyframes.push_back(std::move(kf));
                    }
                    if (!layer->keyframes.empty()) {
                        layer->static_doc = layer->keyframes.front().start_value;
                    }
                } else if (k_val->is_object()) {
                    layer->static_doc = TextDocument::parse(*k_val);
                }
            }
        }
        if (const auto* p_obj = t_obj->get("p"); p_obj && p_obj->is_object()) {
            layer->path_options = TextPathOptions::parse(*p_obj);
        }
        if (const auto* a_arr = t_obj->get("a"); a_arr && a_arr->is_array()) {
            for (const auto& a_json : a_arr->as_array()) {
                if (a_json.is_object()) {
                    layer->animators.push_back(TextAnimator::parse(a_json));
                }
            }
        }
    }
    return layer;
}

// ── AudioLayer ("ty": 6) ──────────────────────────────────────────────────

size_t AudioLayer::count_animators() const noexcept {
    size_t n = LottieLayer::count_animators();
    if (level.is_animated()) ++n;
    return n;
}

void AudioLayer::bind_expression_manager(ExpressionManager& mgr) {
    LottieLayer::bind_expression_manager(mgr);
    level.bind_expression_manager(mgr);
}

std::shared_ptr<AudioLayer> AudioLayer::parse(const json::JsonValue& val) {
    auto layer = std::make_shared<AudioLayer>();
    parse_common_layer_props(*layer, val);
    layer->ref_id = std::string(val.get_string("refId", ""));
    if (const auto* au = val.get("au"); au && au->is_object()) {
        if (const auto* lv = au->get("lv")) {
            layer->level = parse_point_property(*lv, Point::zero());
        }
    }
    return layer;
}

// ── Compositing Helpers: Masks, Track Mattes, Effects, 3D ─────────────────

namespace {

Path build_expanded_mask_path(const Mask& m, float frame) {
    BezierData bz = m.shape.evaluate(frame);
    Path p = bz.to_path();
    if (p.is_empty()) return p;

    float exp = m.expansion.evaluate(frame);
    if (std::abs(exp) > 1e-3f) {
        Stroke st(std::abs(exp) * 2.0f);
        st.line_join = nisaba::LineJoin::Round;
        if (auto stroked = stroke_path(p, st, 1.0f)) {
            if (exp > 0.0f) {
                if (auto u = path_union(p, *stroked)) p = std::move(*u);
            } else {
                if (auto d = path_difference(p, *stroked)) p = std::move(*d);
            }
        }
    }
    return p;
}

std::optional<Path> combine_layer_masks_vector(const LottieLayer& layer, float frame, float comp_w, float comp_h) {
    if (layer.masks.empty()) return std::nullopt;

    Path accum;
    bool has_accum = false;

    Path full_comp;
    {
        PathBuilder pb;
        pb.move_to(-comp_w, -comp_h);
        pb.line_to(comp_w * 2.0f, -comp_h);
        pb.line_to(comp_w * 2.0f, comp_h * 2.0f);
        pb.line_to(-comp_w, comp_h * 2.0f);
        pb.close();
        if (auto fp = pb.finish()) full_comp = std::move(*fp);
    }

    for (const auto& m : layer.masks) {
        if (m.mode == MaskMode::None) continue;
        float op = m.opacity.evaluate(frame);
        if (op <= 0.0f && m.mode == MaskMode::Add) continue;

        Path mp = build_expanded_mask_path(m, frame);
        if (mp.is_empty()) continue;

        if (m.inverted) {
            if (auto inv_p = path_difference(full_comp, mp)) {
                mp = std::move(*inv_p);
            }
        }

        if (!has_accum) {
            if (m.mode == MaskMode::Subtract) {
                if (auto diff = path_difference(full_comp, mp)) {
                    accum = std::move(*diff);
                } else {
                    accum = std::move(mp);
                }
            } else {
                accum = std::move(mp);
            }
            has_accum = true;
            continue;
        }

        switch (m.mode) {
            case MaskMode::Add:
                if (auto u = path_union(accum, mp)) accum = std::move(*u);
                else accum.add_path(mp);
                break;
            case MaskMode::Subtract:
                if (auto d = path_difference(accum, mp)) accum = std::move(*d);
                break;
            case MaskMode::Intersect:
                if (auto in = path_intersect(accum, mp)) accum = std::move(*in);
                break;
            case MaskMode::Difference:
                if (auto x = path_xor(accum, mp)) accum = std::move(*x);
                break;
            default:
                break;
        }
    }

    if (!has_accum) return std::nullopt;
    Transform global_tr = layer.compute_global_transform(frame);
    accum.apply_transform(global_tr);
    return accum;
}

void apply_track_matte_to_pixmap(Pixmap& layer_pm, const Pixmap& matte_pm, MatteType matte_type) {
    if (matte_type == MatteType::None) return;
    uint32_t w = std::min(layer_pm.width(), matte_pm.width());
    uint32_t h = std::min(layer_pm.height(), matte_pm.height());
    if (w == 0 || h == 0) return;

    MaskType mtype = (matte_type == MatteType::Luma || matte_type == MatteType::InvertedLuma)
        ? MaskType::Luminance
        : MaskType::Alpha;

    nisaba::Mask mask = nisaba::Mask::from_pixmap(matte_pm.as_ref(), mtype);
    if (matte_type == MatteType::InvertedAlpha || matte_type == MatteType::InvertedLuma) {
        mask.invert();
    }

    PixmapMut mut = layer_pm.as_mut();
    for (uint32_t y = 0; y < h; ++y) {
        PremultipliedColorU8* row = mut.row(y);
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t ma = mask.get(x, y);
            if (ma == 0) {
                row[x] = PremultipliedColorU8::TRANSPARENT;
            } else if (ma < 255) {
                uint8_t r = static_cast<uint8_t>((row[x].red() * ma + 127) / 255);
                uint8_t g = static_cast<uint8_t>((row[x].green() * ma + 127) / 255);
                uint8_t b = static_cast<uint8_t>((row[x].blue() * ma + 127) / 255);
                uint8_t a = static_cast<uint8_t>((row[x].alpha() * ma + 127) / 255);
                row[x] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
            }
        }
    }
}

void render_single_layer_complete(
    ICanvas& canvas,
    const LottieLayer& layer,
    float frame,
    const LottieAssetRegistry& assets,
    float parent_opacity
) {
    bool needs_matte = (layer.matte_type != MatteType::None && layer.matte_layer_ptr != nullptr);
    bool needs_effects = !layer.effects.empty();
    bool needs_3d = layer.has_3d_hierarchy();
    bool has_feathered_mask = false;
    for (const auto& m : layer.masks) {
        Point f = m.feather.evaluate(frame);
        if (f.x > 0.1f || f.y > 0.1f) {
            has_feathered_mask = true;
            break;
        }
    }

    if (!needs_matte && !needs_3d && !has_feathered_mask) {
        // Zero-FBO lossless fast-path: SolidLayer with a single GradientRamp effect (blend_with_original == 0)
        // renders mathematically identically via native hardware LinearGradient/RadialGradient in 1 draw call.
        if (needs_effects && layer.type == LayerType::Solid && layer.masks.empty()) {
            const LayerEffect* sole_ramp = nullptr;
            int enabled_fx = 0;
            for (const auto& ef : layer.effects) {
                if (ef.enabled) {
                    ++enabled_fx;
                    if (ef.type == EffectType::GradientRamp) sole_ramp = &ef;
                }
            }
            if (enabled_fx == 1 && sole_ramp != nullptr) {
                const float blend_orig = std::clamp(sole_ramp->blend_with_original.evaluate(frame) * 0.01f, 0.0f, 1.0f);
                if (blend_orig <= 1e-4f) {
                    const auto& sl = static_cast<const SolidLayer&>(layer);
                    const float layer_op = parent_opacity * sl.transform.eval_opacity(frame);
                    if (sl.width > 0.0f && sl.height > 0.0f && layer_op > 0.001f) {
                        const Point sp = sole_ramp->ramp_start_point.evaluate(frame);
                        const Point ep = sole_ramp->ramp_end_point.evaluate(frame);
                        Color sc = sole_ramp->ramp_start_color.evaluate(frame);
                        Color ec = sole_ramp->ramp_end_color.evaluate(frame);
                        sc.apply_opacity(layer_op);
                        ec.apply_opacity(layer_op);
                        std::vector<GradientStop> stops = {
                            GradientStop::create(0.0f, sc),
                            GradientStop::create(1.0f, ec)
                        };
                        std::optional<Shader> grad_shader;
                        if (sole_ramp->ramp_shape == 2) {
                            const float radius = std::max(1e-3f, std::hypot(ep.x - sp.x, ep.y - sp.y));
                            if (auto rg = RadialGradient::create(sp, radius, stops)) {
                                grad_shader = Shader(*rg);
                            }
                        } else {
                            if (auto lg = LinearGradient::create(sp, ep, stops)) {
                                grad_shader = Shader(*lg);
                            }
                        }
                        if (grad_shader.has_value()) {
                            Paint gp(*grad_shader);
                            gp.blend_mode = sl.blend_mode;
                            const Transform gtr = sl.compute_global_transform(frame);
                            Point pts[4] = {
                                Point::from_xy(0.0f, 0.0f),
                                Point::from_xy(sl.width, 0.0f),
                                Point::from_xy(sl.width, sl.height),
                                Point::from_xy(0.0f, sl.height)
                            };
                            gtr.map_points(pts, 4);
                            PathBuilder pb;
                            pb.move_to(pts[0].x, pts[0].y);
                            pb.line_to(pts[1].x, pts[1].y);
                            pb.line_to(pts[2].x, pts[2].y);
                            pb.line_to(pts[3].x, pts[3].y);
                            pb.close();
                            if (auto quad = pb.finish()) {
                                canvas.fill_path(*quad, gp, nisaba::FillRule::Winding);
                                return;
                            }
                        }
                    } else {
                        return;
                    }
                }
            }
        }

        std::optional<effects::ImageFilter> fx_filter;
        if (needs_effects) {
            fx_filter = build_layer_effects_filter(layer.effects, frame, assets.comp_width, assets.comp_height);
        }

        // Fast direct vector / native save_layer ImageFilter path (0 CPU readback on GPU!)
        bool has_masks = !layer.masks.empty();
        if (has_masks) {
            canvas.save();
            if (auto mask_path = combine_layer_masks_vector(layer, frame, assets.comp_width, assets.comp_height)) {
                Path device_mask = *mask_path;
                device_mask.apply_transform(canvas.current_transform());
                canvas.clip_path(device_mask, nisaba::FillRule::Winding);
            }
        }

        if (fx_filter.has_value()) {
            Paint fx_paint(Color::WHITE);
            fx_paint.blend_mode = layer.blend_mode;
            fx_paint.set_image_filter(*fx_filter);

            // Use tight layer bounds whenever effects are purely local/in-bounds (massive GPU FBO savings)
            bool needs_full_comp_bounds = false;
            for (const auto& ef : layer.effects) {
                if (!ef.enabled) continue;
                switch (ef.type) {
                    case EffectType::MotionTile:
                    case EffectType::TransformEffect:
                    case EffectType::CornerPin:
                    case EffectType::Bulge:
                    case EffectType::Sphere:
                    case EffectType::FractalNoise:
                    case EffectType::GradientRamp:
                    case EffectType::FourColorGradient:
                        needs_full_comp_bounds = true;
                        break;
                    default:
                        break;
                }
                if (needs_full_comp_bounds) break;
            }

            std::optional<Rect> layer_save_bounds;
            const auto comp_b = Rect::from_xywh(0.0f, 0.0f, assets.comp_width, assets.comp_height);
            if (!needs_full_comp_bounds) {
                if (auto tight_b = layer.compute_complete_bounds(frame, assets)) {
                    const float x0 = std::max(0.0f, std::floor(tight_b->left() - 2.0f));
                    const float y0 = std::max(0.0f, std::floor(tight_b->top() - 2.0f));
                    const float x1 = std::min(assets.comp_width, std::ceil(tight_b->right() + 2.0f));
                    const float y1 = std::min(assets.comp_height, std::ceil(tight_b->bottom() + 2.0f));
                    if (x1 > x0 && y1 > y0) {
                        layer_save_bounds = Rect::from_ltrb(x0, y0, x1, y1);
                    }
                }
            }
            if (!layer_save_bounds.has_value()) {
                layer_save_bounds = comp_b;
            }

            if (layer_save_bounds.has_value()) {
                canvas.save_layer_bounds(*layer_save_bounds, fx_paint);
            } else {
                canvas.save_layer(fx_paint);
            }
            RenderContext ctx(canvas, parent_opacity);
            layer.render_with_assets(ctx, frame, assets);
            canvas.restore();
        } else {
            RenderContext ctx(canvas, parent_opacity);
            layer.render_with_assets(ctx, frame, assets);
        }

        if (has_masks) {
            canvas.restore();
        }
        return;
    }

    // Offscreen compositing path for Track Mattes, Layer Effects, Feathered Masks, or 3D Layers
    uint32_t off_w = static_cast<uint32_t>(std::max(1.0f, std::ceil(assets.comp_width)));
    uint32_t off_h = static_cast<uint32_t>(std::max(1.0f, std::ceil(assets.comp_height)));
    if (off_w > 4096) off_w = 4096;
    if (off_h > 4096) off_h = 4096;

    auto layer_pm_opt = Pixmap::allocate(off_w, off_h);
    if (!layer_pm_opt) return;
    Pixmap& layer_pm = *layer_pm_opt;

    {
        Canvas off_canvas(layer_pm);
        if (!layer.masks.empty() && !has_feathered_mask) {
            if (auto mask_path = combine_layer_masks_vector(layer, frame, assets.comp_width, assets.comp_height)) {
                off_canvas.clip_path(*mask_path, nisaba::FillRule::Winding);
            }
        }
        RenderContext off_ctx(off_canvas, needs_3d ? 1.0f : parent_opacity);
        layer.render_with_assets(off_ctx, frame, assets);
    }

    // Apply feathered masks if present
    if (has_feathered_mask) {
        if (auto mask_path = combine_layer_masks_vector(layer, frame, assets.comp_width, assets.comp_height)) {
            if (auto m_alloc = nisaba::Mask::allocate(off_w, off_h)) {
                m_alloc->fill_path(*mask_path, nisaba::FillRule::Winding, true);
                float max_f = 0.0f;
                for (const auto& m : layer.masks) {
                    Point f = m.feather.evaluate(frame);
                    max_f = std::max({max_f, f.x, f.y});
                }
                if (max_f > 0.2f) {
                    effects::gaussian_blur_mask(*m_alloc, max_f * 0.35f);
                }
                PixmapMut mut = layer_pm.as_mut();
                for (uint32_t y = 0; y < off_h; ++y) {
                    PremultipliedColorU8* row = mut.row(y);
                    for (uint32_t x = 0; x < off_w; ++x) {
                        uint8_t ma = m_alloc->get(x, y);
                        if (ma < 255) {
                            row[x] = PremultipliedColorU8::from_rgba_unchecked(
                                static_cast<uint8_t>((row[x].red() * ma + 127) / 255),
                                static_cast<uint8_t>((row[x].green() * ma + 127) / 255),
                                static_cast<uint8_t>((row[x].blue() * ma + 127) / 255),
                                static_cast<uint8_t>((row[x].alpha() * ma + 127) / 255)
                            );
                        }
                    }
                }
            }
        }
    }

    // Apply Layer Effects via unified ImageFilter DAG
    if (needs_effects) {
        apply_layer_effects_pixmap(layer_pm, layer.effects, frame, assets.comp_width, assets.comp_height);
    }

    // Apply Track Matte if active
    if (needs_matte && layer.matte_layer_ptr) {
        const LottieLayer& matte_layer = *layer.matte_layer_ptr;
        if (frame >= matte_layer.in_point && frame < matte_layer.out_point) {
            if (auto matte_pm = Pixmap::allocate(off_w, off_h)) {
                Canvas matte_canvas(*matte_pm);
                RenderContext matte_ctx(matte_canvas, 1.0f);
                // Temporarily render matte layer even if hidden/is_matte_target
                bool saved_hidden = matte_layer.hidden;
                const_cast<LottieLayer&>(matte_layer).hidden = false;
                matte_layer.render_with_assets(matte_ctx, frame, assets);
                const_cast<LottieLayer&>(matte_layer).hidden = saved_hidden;

                apply_track_matte_to_pixmap(layer_pm, *matte_pm, layer.matte_type);
            }
        } else {
            // Matte layer is inactive at this frame
            if (layer.matte_type == MatteType::Alpha || layer.matte_type == MatteType::Luma) {
                return; // Clipped out completely
            }
        }
    }

    // Composite onto target canvas (either 2D or 3D projective)
    PixmapPaint paint;
    paint.blend_mode = layer.blend_mode;
    paint.quality = FilterQuality::Bilinear;
    paint.opacity = needs_3d ? parent_opacity : 1.0f;

    if (needs_3d) {
        Transform tr2d = layer.compute_global_transform(frame);
        Transform4x4 tr3d = layer.compute_global_transform_3d(frame, assets.comp_width, assets.comp_height);
        if (auto inv2d = tr2d.invert()) {
            Transform4x4 undo2d(
                inv2d->sx, inv2d->kx, 0.0f, inv2d->tx,
                inv2d->ky, inv2d->sy, 0.0f, inv2d->ty,
                0.0f,      0.0f,      1.0f, 0.0f,
                0.0f,      0.0f,      0.0f, 1.0f
            );
            canvas.draw_pixmap_3d(layer_pm.as_ref(), tr3d * undo2d, paint.opacity, paint.blend_mode);
            return;
        }
    }

    canvas.draw_pixmap(0, 0, layer_pm.as_ref(), paint);
}

} // anonymous namespace

// ── Bounds, Animation Query & Fingerprinting for Incremental Damage ───────

namespace {

inline uint64_t lhash_mix(uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

inline uint64_t lhash_f32(uint64_t h, float f) noexcept {
    uint32_t bits = 0;
    if (f == 0.0f) f = 0.0f;
    std::memcpy(&bits, &f, sizeof(bits));
    return lhash_mix(h, static_cast<uint64_t>(bits));
}

inline uint64_t lhash_color(uint64_t h, Color c) noexcept {
    h = lhash_f32(h, c.red());
    h = lhash_f32(h, c.green());
    h = lhash_f32(h, c.blue());
    return lhash_f32(h, c.alpha());
}

inline uint64_t lhash_transform(uint64_t h, const Transform& tr) noexcept {
    h = lhash_f32(h, tr.sx);
    h = lhash_f32(h, tr.kx);
    h = lhash_f32(h, tr.ky);
    h = lhash_f32(h, tr.sy);
    h = lhash_f32(h, tr.tx);
    return lhash_f32(h, tr.ty);
}

inline uint64_t lhash_str(uint64_t h, std::string_view sv) noexcept {
    for (char c : sv) {
        h = lhash_mix(h, static_cast<uint64_t>(static_cast<unsigned char>(c)));
    }
    return lhash_mix(h, static_cast<uint64_t>(sv.size()));
}

std::optional<Rect> join_opt(const std::optional<Rect>& a, const std::optional<Rect>& b) noexcept {
    if (!a.has_value()) return b;
    if (!b.has_value()) return a;
    return a->join(*b);
}

size_t count_transform_animators(const LottieTransform& tr) noexcept {
    size_t n = 0;
    if (tr.anchor.is_animated()) ++n;
    if (tr.position.is_animated()) ++n;
    if (tr.x_pos && tr.x_pos->is_animated()) ++n;
    if (tr.y_pos && tr.y_pos->is_animated()) ++n;
    if (tr.scale.is_animated()) ++n;
    if (tr.rotation.is_animated()) ++n;
    if (tr.opacity.is_animated()) ++n;
    if (tr.skew.is_animated()) ++n;
    if (tr.skew_axis.is_animated()) ++n;
    if (tr.rx.is_animated()) ++n;
    if (tr.ry.is_animated()) ++n;
    if (tr.rz.is_animated()) ++n;
    if (tr.z_pos.is_animated()) ++n;
    if (tr.z_anchor.is_animated()) ++n;
    if (tr.z_scale.is_animated()) ++n;
    if (tr.orientation.is_animated()) ++n;
    if (tr.orientation_z.is_animated()) ++n;
    return n;
}

bool mask_is_animated(const Mask& m) noexcept {
    return m.shape.is_animated() || m.opacity.is_animated() ||
           m.expansion.is_animated() || m.feather.is_animated();
}

bool effect_is_animated(const LayerEffect& e) noexcept {
    return e.is_animated();
}

} // anonymous namespace

bool LottieLayer::is_layer_animated(const LottieAssetRegistry& assets) const noexcept {
    const LottieLayer* curr = this;
    int depth = 0;
    while (curr && depth < 32) {
        if (curr->transform.is_animated()) return true;
        curr = curr->parent_ptr;
        ++depth;
    }
    for (const auto& m : masks) {
        if (mask_is_animated(m)) return true;
    }
    for (const auto& e : effects) {
        if (e.enabled && effect_is_animated(e)) return true;
    }
    if (matte_type != MatteType::None && matte_layer_ptr) {
        if (matte_layer_ptr->is_layer_animated(assets)) return true;
    }
    return false;
}

std::optional<Rect> LottieLayer::compute_bounds(float, const LottieAssetRegistry&) const {
    return std::nullopt;
}

std::optional<Rect> LottieLayer::compute_complete_bounds(float frame, const LottieAssetRegistry& assets) const {
    if (!is_active(frame)) return std::nullopt;
    if (transform.eval_opacity(frame) <= 0.001f) return std::nullopt;

    auto base = compute_bounds(frame, assets);
    if (!base.has_value()) return std::nullopt;

    Rect r = *base;

    // Project 4 corners through 3D transform if this layer participates in 3D hierarchy
    if (has_3d_hierarchy()) {
        const Transform tr2d = compute_global_transform(frame);
        const Transform4x4 tr3d = compute_global_transform_3d(frame, assets.comp_width, assets.comp_height);
        if (auto inv2d = tr2d.invert()) {
            const Transform4x4 undo2d(
                inv2d->sx, inv2d->kx, 0.0f, inv2d->tx,
                inv2d->ky, inv2d->sy, 0.0f, inv2d->ty,
                0.0f,      0.0f,      1.0f, 0.0f,
                0.0f,      0.0f,      0.0f, 1.0f
            );
            const Transform4x4 m3 = tr3d * undo2d;
            Point corners[4] = {
                Point::from_xy(r.left(), r.top()),
                Point::from_xy(r.right(), r.top()),
                Point::from_xy(r.right(), r.bottom()),
                Point::from_xy(r.left(), r.bottom())
            };
            Point projected[4];
            for (int i = 0; i < 4; ++i) {
                projected[i] = m3.map_point(corners[i]);
            }
            if (auto proj_b = Rect::from_points(projected, 4)) {
                r = *proj_b;
            }
        }
    }

    // Expand for active effects (GaussianBlur / DropShadow halos)
    expand_bounds_for_effects(r, effects, frame);
    float pad_x = 1.0f; // 1px AA margin
    float pad_y = 1.0f;

    // Expand for feathered masks if any
    for (const auto& m : masks) {
        Point f = m.feather.evaluate(frame);
        if (f.x > 0.1f || f.y > 0.1f) {
            pad_x += std::ceil(f.x * 0.35f * 3.0f);
            pad_y += std::ceil(f.y * 0.35f * 3.0f);
        }
    }

    if (auto expanded = r.outset(pad_x, pad_y)) {
        return *expanded;
    }
    return r;
}

uint64_t LottieLayer::visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept {
    if (!is_active(frame)) return 0u;
    uint64_t h = 1469598103934665603ULL;
    h = lhash_mix(h, static_cast<uint64_t>(type));
    h = lhash_mix(h, static_cast<uint64_t>(blend_mode));
    h = lhash_transform(h, compute_global_transform(frame));
    h = lhash_f32(h, transform.eval_opacity(frame));
    if (has_3d_hierarchy()) {
        const Transform4x4 m3 = compute_global_transform_3d(frame, assets.comp_width, assets.comp_height);
        for (float v : m3.m) h = lhash_f32(h, v);
    }
    for (const auto& m : masks) {
        h = lhash_mix(h, static_cast<uint64_t>(m.mode));
        h = lhash_mix(h, m.inverted ? 1u : 0u);
        h = lhash_f32(h, m.opacity.evaluate(frame));
        h = lhash_f32(h, m.expansion.evaluate(frame));
        Point f = m.feather.evaluate(frame);
        h = lhash_f32(h, f.x);
        h = lhash_f32(h, f.y);
        BezierData bz = m.shape.evaluate(frame);
        h = lhash_mix(h, static_cast<uint64_t>(bz.vertices.size()));
        for (const auto& v : bz.vertices) {
            h = lhash_f32(h, v.x);
            h = lhash_f32(h, v.y);
        }
    }
    for (const auto& e : effects) {
        if (!e.enabled) continue;
        h = lhash_mix(h, e.visual_fingerprint(frame));
    }
    if (matte_type != MatteType::None && matte_layer_ptr) {
        h = lhash_mix(h, static_cast<uint64_t>(matte_type));
        h = lhash_mix(h, matte_layer_ptr->visual_fingerprint(frame, assets));
    }
    return h;
}

size_t LottieLayer::count_animators() const noexcept {
    size_t n = count_transform_animators(transform);
    for (const auto& m : masks) {
        if (m.shape.is_animated()) ++n;
        if (m.opacity.is_animated()) ++n;
        if (m.expansion.is_animated()) ++n;
        if (m.feather.is_animated()) ++n;
    }
    for (const auto& e : effects) {
        if (e.enabled && effect_is_animated(e)) ++n;
    }
    return n;
}

void LottieLayer::bind_expression_manager(ExpressionManager& mgr) {
    transform.bind_expression_manager(mgr);
    for (auto& m : masks) {
        m.opacity.bind_expression_manager(mgr);
        m.expansion.bind_expression_manager(mgr);
        m.feather.bind_expression_manager(mgr);
    }
    for (auto& e : effects) {
        e.bind_expression_manager(mgr);
    }
}

// ── ShapeLayer Bounds / Fingerprint / Animators ───────────────────────────

bool ShapeLayer::is_layer_animated(const LottieAssetRegistry& assets) const noexcept {
    if (LottieLayer::is_layer_animated(assets)) return true;
    for (const auto& s : shapes) {
        if (s && !s->hidden && s->is_animated()) return true;
    }
    return false;
}

std::optional<Rect> ShapeLayer::compute_bounds(float frame, const LottieAssetRegistry&) const {
    if (!is_active(frame)) return std::nullopt;
    auto local_b = compute_shape_items_bounds(shapes, frame);
    if (!local_b.has_value()) return std::nullopt;
    return local_b->transform(compute_global_transform(frame));
}

uint64_t ShapeLayer::visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept {
    uint64_t h = LottieLayer::visual_fingerprint(frame, assets);
    if (h == 0u) return 0u;
    for (const auto& item : shapes) {
        if (item && !item->hidden) {
            h = lhash_mix(h, item->visual_fingerprint(frame));
        }
    }
    return h;
}

size_t ShapeLayer::count_animators() const noexcept {
    size_t n = LottieLayer::count_animators();
    for (const auto& item : shapes) {
        if (item) n += item->count_animators();
    }
    return n;
}

void ShapeLayer::bind_expression_manager(ExpressionManager& mgr) {
    LottieLayer::bind_expression_manager(mgr);
    for (auto& item : shapes) {
        if (item) item->bind_expression_manager(mgr);
    }
}

// ── SolidLayer Bounds / Fingerprint ───────────────────────────────────────

std::optional<Rect> SolidLayer::compute_bounds(float frame, const LottieAssetRegistry&) const {
    if (!is_active(frame) || width <= 0.0f || height <= 0.0f) return std::nullopt;
    auto local = Rect::from_xywh(0.0f, 0.0f, width, height);
    if (!local.has_value()) return std::nullopt;
    return local->transform(compute_global_transform(frame));
}

uint64_t SolidLayer::visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept {
    uint64_t h = LottieLayer::visual_fingerprint(frame, assets);
    if (h == 0u) return 0u;
    h = lhash_color(h, color_override.value_or(solid_color));
    h = lhash_f32(h, width);
    return lhash_f32(h, height);
}

// ── PrecompLayer Bounds / Fingerprint / Animators ─────────────────────────

bool PrecompLayer::is_layer_animated(const LottieAssetRegistry& assets) const noexcept {
    if (LottieLayer::is_layer_animated(assets)) return true;
    if (has_time_remap && time_remap.is_animated()) return true;
    if (external_layer) return external_layer->is_animated();
    auto it = assets.precomps.find(ref_id);
    if (it != assets.precomps.end()) {
        for (const auto& sub : it->second) {
            if (sub && sub->is_layer_animated(assets)) return true;
        }
    }
    return false;
}

std::optional<Rect> PrecompLayer::compute_bounds(float frame, const LottieAssetRegistry& assets) const {
    if (!is_active(frame)) return std::nullopt;
    if (external_layer) {
        const float w = (width > 0.0f) ? width : assets.comp_width;
        const float h = (height > 0.0f) ? height : assets.comp_height;
        auto local = Rect::from_xywh(0.0f, 0.0f, w, h);
        if (!local.has_value()) return std::nullopt;
        return local->transform(compute_global_transform(frame));
    }
    auto it = assets.precomps.find(ref_id);
    if (it == assets.precomps.end()) return std::nullopt;

    const float sub_frame = has_time_remap ? time_remap.evaluate(frame) : (frame - start_time) / time_stretch;
    std::optional<Rect> local_b;
    for (const auto& sub : it->second) {
        if (!sub || sub->is_matte_target) continue;
        local_b = join_opt(local_b, sub->compute_complete_bounds(sub_frame, assets));
    }
    if (!local_b.has_value()) return std::nullopt;

    if (width > 0.0f && height > 0.0f) {
        if (auto clip_r = Rect::from_xywh(0.0f, 0.0f, width, height)) {
            local_b = local_b->intersect(*clip_r);
            if (!local_b.has_value()) return std::nullopt;
        }
    }
    return local_b->transform(compute_global_transform(frame));
}

uint64_t PrecompLayer::visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept {
    uint64_t h = LottieLayer::visual_fingerprint(frame, assets);
    if (h == 0u) return 0u;
    const float sub_frame = has_time_remap ? time_remap.evaluate(frame) : (frame - start_time) / time_stretch;
    if (external_layer) {
        const float fps = (assets.frame_rate > 0.0f) ? assets.frame_rate : 60.0f;
        return lhash_mix(h, external_layer->content_version(sub_frame / fps));
    }
    auto it = assets.precomps.find(ref_id);
    if (it != assets.precomps.end()) {
        for (const auto& sub : it->second) {
            if (sub && !sub->is_matte_target) {
                h = lhash_mix(h, sub->visual_fingerprint(sub_frame, assets));
            }
        }
    }
    return h;
}

size_t PrecompLayer::count_animators() const noexcept {
    size_t n = LottieLayer::count_animators();
    if (has_time_remap && time_remap.is_animated()) ++n;
    return n;
}

void PrecompLayer::bind_expression_manager(ExpressionManager& mgr) {
    LottieLayer::bind_expression_manager(mgr);
    if (has_time_remap) time_remap.bind_expression_manager(mgr);
}

// ── ImageLayer Bounds / Fingerprint / Animated ────────────────────────────

bool ImageLayer::is_layer_animated(const LottieAssetRegistry& assets) const noexcept {
    if (LottieLayer::is_layer_animated(assets)) return true;
    auto it = assets.images.find(ref_id);
    if (it != assets.images.end() && it->second.is_multi_frame()) {
        return true;
    }
    return false;
}

std::optional<Rect> ImageLayer::compute_bounds(float frame, const LottieAssetRegistry& assets) const {
    if (!is_active(frame)) return std::nullopt;
    auto it = assets.images.find(ref_id);
    if (it == assets.images.end()) return std::nullopt;

    float w = it->second.width;
    float h = it->second.height;
    if (w <= 0.0f && it->second.asset) w = static_cast<float>(it->second.asset->width());
    if (h <= 0.0f && it->second.asset) h = static_cast<float>(it->second.asset->height());
    if (w <= 0.0f && it->second.pixmap) w = static_cast<float>(it->second.pixmap->width());
    if (h <= 0.0f && it->second.pixmap) h = static_cast<float>(it->second.pixmap->height());
    if (w <= 0.0f || h <= 0.0f) return std::nullopt;

    auto local = Rect::from_xywh(0.0f, 0.0f, w, h);
    if (!local.has_value()) return std::nullopt;
    return local->transform(compute_global_transform(frame));
}

uint64_t ImageLayer::visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept {
    uint64_t h = LottieLayer::visual_fingerprint(frame, assets);
    if (h == 0u) return 0u;
    auto it = assets.images.find(ref_id);
    if (it != assets.images.end()) {
        const float fps = (assets.frame_rate > 0.0f) ? assets.frame_rate : 60.0f;
        const float t_sec = std::max(0.0f, ((frame - start_time) / time_stretch) / fps);
        h = lhash_mix(h, it->second.frame_token(t_sec));
    }
    return h;
}

// ── TextLayer Bounds / Fingerprint / Animators ────────────────────────────

bool TextLayer::is_layer_animated(const LottieAssetRegistry& assets) const noexcept {
    if (LottieLayer::is_layer_animated(assets)) return true;
    if (keyframes.size() > 1 || text_evaluator != nullptr || path_options.is_animated()) return true;
    for (const auto& a : animators) {
        if (a.selector.is_animated() ||
            a.position.is_animated() || a.scale.is_animated() || a.rotation.is_animated() ||
            a.opacity.is_animated() || a.fill_color.is_animated() || a.stroke_color.is_animated() ||
            a.stroke_width.is_animated() || a.tracking.is_animated()) {
            return true;
        }
        for (const auto& sel : a.selectors) {
            if (sel.is_animated()) return true;
        }
    }
    return false;
}

std::optional<Rect> TextLayer::compute_bounds(float frame, const LottieAssetRegistry&) const {
    if (!is_active(frame)) return std::nullopt;
    const TextDocument& doc = evaluate_document(frame);

    // If laid out along a mask path, expand mask path bounds by font size & stroke
    if (path_options.enabled && !masks.empty()) {
        int midx = path_options.mask_index;
        if (midx >= static_cast<int>(masks.size()) && midx > 0) midx -= 1;
        if (midx >= 0 && midx < static_cast<int>(masks.size())) {
            const Path mp = masks[static_cast<size_t>(midx)].shape.evaluate(frame).to_path(path_options.reverse_path);
            if (!mp.is_empty()) {
                Rect pb = mp.bounds();
                const float pad = doc.font_size * 1.5f + doc.stroke_width;
                if (auto ex = pb.outset(pad, pad)) pb = *ex;
                return pb.transform(compute_global_transform(frame));
            }
        }
    }

    const std::string_view raw_text = text_override.has_value()
        ? std::string_view(*text_override)
        : std::string_view(doc.text);
    if (raw_text.empty() || doc.font_size <= 0.0f) return std::nullopt;

    // Conservative text bounding box accounting for multi-line layout, tracking, stroke, and animator offsets
    size_t line_count = 1;
    size_t max_line_chars = 0;
    size_t cur_chars = 0;
    for (char c : raw_text) {
        if (c == '\r' || c == '\n') {
            max_line_chars = std::max(max_line_chars, cur_chars);
            cur_chars = 0;
            ++line_count;
        } else if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++cur_chars;
        }
    }
    max_line_chars = std::max(max_line_chars, cur_chars);

    const float tracking_px = (doc.tracking / 1000.0f) * doc.font_size;
    float est_w = static_cast<float>(max_line_chars) * (doc.font_size * 0.85f + std::max(0.0f, tracking_px));
    if (doc.box_size.x > 0.0f) est_w = std::max(est_w, doc.box_size.x);
    const float est_h = doc.font_size * 1.25f + static_cast<float>(line_count - 1) * std::max(doc.line_height, doc.font_size);

    float min_x = doc.box_position.x;
    if (doc.justify == TextJustify::Center) min_x -= est_w * 0.5f;
    else if (doc.justify == TextJustify::Right) min_x -= est_w;
    float max_x = min_x + est_w;
    float min_y = doc.box_position.y - doc.font_size * 1.15f - doc.baseline_shift;
    float max_y = min_y + est_h + doc.font_size * 0.35f;

    float extra_pad = doc.stroke_width * 0.5f + 2.0f;
    for (const auto& a : animators) {
        if (a.has_position) {
            Point p = a.position.evaluate(frame);
            min_x = std::min(min_x, min_x + p.x);
            max_x = std::max(max_x, max_x + p.x);
            min_y = std::min(min_y, min_y + p.y);
            max_y = std::max(max_y, max_y + p.y);
        }
        if (a.has_stroke_width) {
            extra_pad += std::abs(a.stroke_width.evaluate(frame)) * 0.5f;
        }
        if (a.has_scale) {
            Point sc = a.scale.evaluate(frame);
            const float factor = std::max(std::abs(sc.x), std::abs(sc.y)) * 0.01f;
            if (factor > 1.0f) {
                extra_pad += doc.font_size * (factor - 1.0f);
            }
        }
    }

    auto local = Rect::from_ltrb(min_x - extra_pad, min_y - extra_pad, max_x + extra_pad, max_y + extra_pad);
    if (!local.has_value()) return std::nullopt;
    return local->transform(compute_global_transform(frame));
}

uint64_t TextLayer::visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept {
    uint64_t h = LottieLayer::visual_fingerprint(frame, assets);
    if (h == 0u) return 0u;
    const TextDocument& doc = evaluate_document(frame);
    const std::string_view raw_text = text_override.has_value()
        ? std::string_view(*text_override)
        : std::string_view(doc.text);
    h = lhash_str(h, raw_text);
    h = lhash_str(h, doc.font_name);
    h = lhash_f32(h, doc.font_size);
    h = lhash_color(h, doc.fill_color);
    h = lhash_color(h, doc.stroke_color);
    h = lhash_f32(h, doc.stroke_width);
    h = lhash_f32(h, doc.tracking);
    h = lhash_f32(h, doc.line_height);
    for (const auto& a : animators) {
        h = lhash_f32(h, a.selector.start.evaluate(frame));
        h = lhash_f32(h, a.selector.end.evaluate(frame));
        h = lhash_f32(h, a.selector.offset.evaluate(frame));
        if (a.has_position) {
            Point p = a.position.evaluate(frame);
            h = lhash_f32(h, p.x);
            h = lhash_f32(h, p.y);
        }
        if (a.has_scale) {
            Point s = a.scale.evaluate(frame);
            h = lhash_f32(h, s.x);
            h = lhash_f32(h, s.y);
        }
        if (a.has_rotation) h = lhash_f32(h, a.rotation.evaluate(frame));
        if (a.has_opacity) h = lhash_f32(h, a.opacity.evaluate(frame));
        if (a.has_fill_color) h = lhash_color(h, a.fill_color.evaluate(frame));
        if (a.has_stroke_color) h = lhash_color(h, a.stroke_color.evaluate(frame));
        if (a.has_stroke_width) h = lhash_f32(h, a.stroke_width.evaluate(frame));
        if (a.has_tracking) h = lhash_f32(h, a.tracking.evaluate(frame));
    }
    return h;
}

size_t TextLayer::count_animators() const noexcept {
    size_t n = LottieLayer::count_animators();
    if (keyframes.size() > 1 || text_evaluator != nullptr) ++n;
    if (path_options.is_animated()) ++n;
    n += animators.size();
    return n;
}

void TextLayer::bind_expression_manager(ExpressionManager& mgr) {
    LottieLayer::bind_expression_manager(mgr);
    path_options.bind_expression_manager(mgr);
    if (!text_expression.empty()) {
        if (auto ev = mgr.create_string_expression_evaluator(text_expression)) {
            text_evaluator = std::move(ev);
        }
    }
    for (auto& a : animators) {
        a.bind_expression_manager(mgr);
    }
}

// ── Layer Tree Renderer ───────────────────────────────────────────────────

void render_layer_tree_with_assets(
    ICanvas& canvas,
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    float frame,
    const LottieAssetRegistry& assets,
    float parent_opacity) {

    if (layers.empty()) return;

    // Ensure hierarchy pointers are linked if caller created layers manually
    bool needs_linking = false;
    for (const auto& l : layers) {
        if (l && ((l->parent_index >= 0 && !l->parent_ptr) || (l->matte_type != MatteType::None && !l->matte_layer_ptr))) {
            needs_linking = true;
            break;
        }
    }
    if (needs_linking) {
        auto mutable_copy = layers;
        link_layer_hierarchy(mutable_copy);
    }

    // In Bodymovin JSON, layers are ordered from top to bottom.
    // To render back-to-front (painter's algorithm), traverse backwards from N-1 down to 0.
    for (size_t i = layers.size(); i > 0; --i) {
        const auto& layer = layers[i - 1];
        if (!layer || !layer->is_active(frame)) continue;
        if (layer->is_matte_target) continue; // Matte sources are rendered by their matte consumers

        render_single_layer_complete(canvas, *layer, frame, assets, parent_opacity);
    }
}

void render_layer_tree(
    ICanvas& canvas,
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    float frame,
    const std::unordered_map<std::string, std::vector<std::shared_ptr<LottieLayer>>>& precomp_assets,
    float parent_opacity) {

    LottieAssetRegistry assets;
    assets.precomps = precomp_assets;
    assets.comp_width = static_cast<float>(canvas.canvas_width() > 0 ? canvas.canvas_width() : 512);
    assets.comp_height = static_cast<float>(canvas.canvas_height() > 0 ? canvas.canvas_height() : 512);
    render_layer_tree_with_assets(canvas, layers, frame, assets, parent_opacity);
}

} // namespace nisaba::lottie
