#pragma once

/// @file layer.hpp
/// @brief Lottie layer hierarchy, masks, track mattes, effects, images, typography, 3D, and rendering.

#include "nisaba/lottie/lottie_types.hpp"
#include "nisaba/lottie/resources.hpp"
#include "nisaba/lottie/external_layer.hpp"
#include "nisaba/lottie/expression.hpp"
#include "nisaba/lottie/audio.hpp"
#include "nisaba/lottie/property.hpp"
#include "nisaba/lottie/effects.hpp"
#include "nisaba/lottie/shape.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/math/transform4x4.hpp"
#include "nisaba/text/font_system.hpp"

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <optional>

namespace nisaba::lottie {

class LottieLayer;

// ── Asset & Font Definitions ──────────────────────────────────────────────

struct LottieImageAsset {
    std::string id;
    std::string directory; // "u"
    std::string filename;  // "p" (or data URI)
    float width = 0.0f;
    float height = 0.0f;
    bool embedded = false; // "e" == 1
    std::optional<Pixmap> pixmap;
    std::shared_ptr<ImageAsset> asset;

    [[nodiscard]] bool is_multi_frame() const noexcept {
        return asset && asset->is_multi_frame();
    }

    [[nodiscard]] std::optional<PixmapRef> resolve_frame(float t_seconds) const {
        if (asset) {
            return asset->get_frame(t_seconds);
        }
        if (pixmap.has_value()) {
            return pixmap->as_ref();
        }
        return std::nullopt;
    }

    [[nodiscard]] uint64_t frame_token(float t_seconds) const noexcept {
        if (asset) {
            return asset->frame_token(t_seconds);
        }
        return pixmap.has_value() ? 1u : 0u;
    }
};

struct LottieFontDef {
    std::string name;      // "fName"
    std::string family;    // "fFamily"
    std::string style;     // "fStyle"
    std::string path;      // "fPath"
    float ascent = 75.0f;
    std::optional<uint32_t> loaded_font_id;
};

struct LottieGlyphChar {
    std::string ch;        // "ch"
    std::string style;     // "style"
    std::string family;    // "fFamily"
    float size = 100.0f;   // "size"
    float width = 0.0f;    // "w"
    std::vector<std::shared_ptr<ShapeGroup>> shapes;
};

struct LottieAssetRegistry {
    std::unordered_map<std::string, std::vector<std::shared_ptr<LottieLayer>>> precomps;
    std::unordered_map<std::string, LottieImageAsset> images;
    std::unordered_map<std::string, LottieFontDef> fonts;
    std::unordered_map<std::string, std::shared_ptr<AudioAsset>> audio_assets;
    std::vector<LottieGlyphChar> chars;
    std::shared_ptr<text::FontSystem> font_system;
    float comp_width = 512.0f;
    float comp_height = 512.0f;
    float frame_rate = 60.0f;

    [[nodiscard]] const LottieGlyphChar* find_char(std::string_view ch, std::string_view family, std::string_view style) const noexcept;
};

// ── Layer Mask ("masksProperties") ────────────────────────────────────────

struct Mask {
    MaskMode mode = MaskMode::Add;
    ShapeProperty shape;
    FloatProperty opacity{100.0f};
    FloatProperty expansion{0.0f}; // "x"
    PointProperty feather{Point::zero()}; // "f"
    bool inverted = false;

    static Mask parse(const json::JsonValue& val);
};

// ── Layer Effects ("ef") are defined in nisaba/lottie/effects.hpp ─────────

// ── Text Document & Animators ("ty": 5) ───────────────────────────────────

struct TextDocument {
    std::string text;               // "t" (lines separated by \r or \n)
    std::string font_name;          // "f"
    float font_size = 24.0f;        // "s"
    Color fill_color = Color::BLACK;// "fc"
    Color stroke_color = Color::BLACK; // "sc"
    float stroke_width = 0.0f;      // "sw"
    bool stroke_over_fill = false;  // "of"
    TextJustify justify = TextJustify::Left; // "j"
    float tracking = 0.0f;          // "tr"
    float line_height = 28.8f;      // "lh"
    float baseline_shift = 0.0f;    // "ls"
    Point box_position = Point::zero(); // "ps"
    Point box_size = Point::zero();     // "sz"

    static TextDocument parse(const json::JsonValue& val);
};

struct TextRangeSelector {
    FloatProperty start{0.0f};      // "s" [0..100]
    FloatProperty end{100.0f};      // "e" [0..100]
    FloatProperty offset{0.0f};     // "o" [-100..100]
    FloatProperty smoothness{100.0f}; // "sm" [0..100]%
    FloatProperty max_amount{100.0f}; // "a" [-100..100]%
    FloatProperty max_ease{0.0f};   // "xe" [-100..100]
    FloatProperty min_ease{0.0f};   // "ne" [-100..100]
    int units = 1;                  // "r": 1 = Percentage, 2 = Index
    int based_on = 1;               // "b": 1 = Characters, 2 = Chars excl. spaces, 3 = Words, 4 = Lines
    int shape = 1;                  // "sh": 1 = Square, 2 = Ramp Up, 3 = Ramp Down, 4 = Triangle, 5 = Round, 6 = Smooth
    int mode = 1;                   // "m": 1=Add, 2=Subtract, 3=Intersect, 4=Min, 5=Max, 6=Difference

    [[nodiscard]] float compute_factor(size_t index, size_t total_count, float frame) const noexcept;
    [[nodiscard]] bool is_animated() const noexcept;
    void bind_expression_manager(ExpressionManager& mgr);
    static TextRangeSelector parse(const json::JsonValue& val);
};

struct TextPathOptions {
    bool enabled = false;
    int mask_index = -1;            // "m": 0-based index into layer.masks
    FloatProperty first_margin{0.0f}; // "f": arc-length start offset along path
    FloatProperty last_margin{0.0f};  // "l": arc-length end offset along path
    bool perpendicular_to_path = true; // "p": rotate glyphs perpendicular to tangent
    bool reverse_path = false;         // "r": traverse path in reverse
    bool force_alignment = false;      // "a": spread glyphs evenly along available span

    [[nodiscard]] bool is_animated() const noexcept {
        return enabled && (first_margin.is_animated() || last_margin.is_animated());
    }
    void bind_expression_manager(ExpressionManager& mgr) {
        first_margin.bind_expression_manager(mgr);
        last_margin.bind_expression_manager(mgr);
    }
    static TextPathOptions parse(const json::JsonValue& val);
};

struct TextAnimator {
    std::string name;
    TextRangeSelector selector;
    std::vector<TextRangeSelector> selectors;
    PointProperty position{Point::zero()};
    PointProperty scale{Point::from_xy(100.0f, 100.0f)};
    FloatProperty rotation{0.0f};
    FloatProperty opacity{100.0f};
    ColorProperty fill_color{Color::BLACK};
    ColorProperty stroke_color{Color::BLACK};
    FloatProperty stroke_width{0.0f};
    FloatProperty tracking{0.0f};

    bool has_position = false;
    bool has_scale = false;
    bool has_rotation = false;
    bool has_opacity = false;
    bool has_fill_color = false;
    bool has_stroke_color = false;
    bool has_stroke_width = false;
    bool has_tracking = false;

    void bind_expression_manager(ExpressionManager& mgr);
    static TextAnimator parse(const json::JsonValue& val);
};

// ── Base Layer Class ──────────────────────────────────────────────────────

class LottieLayer {
public:
    virtual ~LottieLayer() = default;

    int index = -1;
    int parent_index = -1;
    std::string name;
    LayerType type = LayerType::Null;
    float in_point = 0.0f;
    float out_point = 0.0f;
    float start_time = 0.0f;
    float time_stretch = 1.0f;
    LottieTransform transform;
    nisaba::BlendMode blend_mode = nisaba::BlendMode::SourceOver;
    MatteType matte_type = MatteType::None;
    int matte_parent_index = -1; // "tp" in Lottie 1.0+
    bool is_matte_target = false;
    bool hidden = false;
    bool auto_orient = false;    // "ao" == 1
    std::vector<Mask> masks;
    std::vector<LayerEffect> effects;

    // Pre-linked raw pointers resolved once at load time (O(1), 0 heap alloc per frame)
    const LottieLayer* parent_ptr = nullptr;
    const LottieLayer* matte_layer_ptr = nullptr;

    [[nodiscard]] bool is_active(float frame) const noexcept {
        return !hidden && (frame >= in_point && frame < out_point);
    }

    /// @brief Compute global 2D transform using pre-linked parent_ptr (zero allocations).
    [[nodiscard]] Transform compute_global_transform(float frame) const noexcept;

    /// @brief Legacy overload with layer_map for backward compatibility.
    [[nodiscard]] Transform compute_global_transform(
        float frame,
        const std::unordered_map<int, std::shared_ptr<LottieLayer>>& layer_map) const;

    /// @brief Compute global 3D 4x4 transform matrix.
    [[nodiscard]] Transform4x4 compute_global_transform_3d(float frame, float comp_w = 512.0f, float comp_h = 512.0f) const noexcept;

    [[nodiscard]] bool has_3d_hierarchy() const noexcept;

    /// @brief Render layer content using full LottieAssetRegistry.
    virtual void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const;

    /// @brief Legacy render entry point (forwards to render_with_assets).
    virtual void render(
        RenderContext& ctx,
        float frame,
        const std::unordered_map<int, std::shared_ptr<LottieLayer>>& layer_map,
        const std::unordered_map<std::string, std::vector<std::shared_ptr<LottieLayer>>>& precomp_assets) const;

    /// @brief Collect global vector bounds / paths for hit-testing.
    virtual bool hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const;

    /// @brief Returns true if any property, shape, mask, effect, or referenced asset on this layer is animated.
    [[nodiscard]] virtual bool is_layer_animated(const LottieAssetRegistry& assets) const noexcept;

    /// @brief Computes tight local/global content bounds in composition coordinates at `frame`.
    [[nodiscard]] virtual std::optional<Rect> compute_bounds(float frame, const LottieAssetRegistry& assets) const;

    /// @brief Computes full visual bounds including 3D projection, layer effects (blur/drop shadow), and AA padding.
    [[nodiscard]] std::optional<Rect> compute_complete_bounds(float frame, const LottieAssetRegistry& assets) const;

    /// @brief Computes a deterministic content fingerprint at `frame` for incremental dirty tracking.
    [[nodiscard]] virtual uint64_t visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept;

    /// @brief Counts animated properties / animators owned by this layer (for build statistics).
    [[nodiscard]] virtual size_t count_animators() const noexcept;

    /// @brief Binds a pluggable ExpressionManager to all properties on this layer.
    virtual void bind_expression_manager(ExpressionManager& mgr);

    static std::shared_ptr<LottieLayer> parse(const json::JsonValue& val);
};

// Shape Layer ("ty": 4)
class ShapeLayer : public LottieLayer {
public:
    std::vector<std::shared_ptr<LottieShapeItem>> shapes;

    void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const override;

    bool hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const override;
    [[nodiscard]] bool is_layer_animated(const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] std::optional<Rect> compute_bounds(float frame, const LottieAssetRegistry& assets) const override;
    [[nodiscard]] uint64_t visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] size_t count_animators() const noexcept override;
    void bind_expression_manager(ExpressionManager& mgr) override;

    static std::shared_ptr<ShapeLayer> parse(const json::JsonValue& val);
};

// Solid Color Layer ("ty": 1)
class SolidLayer : public LottieLayer {
public:
    float width = 0.0f;
    float height = 0.0f;
    Color solid_color = Color::BLACK;
    std::optional<Color> color_override;

    void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const override;

    bool hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const override;
    [[nodiscard]] std::optional<Rect> compute_bounds(float frame, const LottieAssetRegistry& assets) const override;
    [[nodiscard]] uint64_t visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept override;

    static std::shared_ptr<SolidLayer> parse(const json::JsonValue& val);
};

// Precomposition Layer ("ty": 0)
class PrecompLayer : public LottieLayer {
public:
    std::string ref_id;
    float width = 0.0f;
    float height = 0.0f;
    FloatProperty time_remap{0.0f};
    bool has_time_remap = false;
    std::shared_ptr<ExternalLayer> external_layer;

    void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const override;

    bool hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const override;
    [[nodiscard]] bool is_layer_animated(const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] std::optional<Rect> compute_bounds(float frame, const LottieAssetRegistry& assets) const override;
    [[nodiscard]] uint64_t visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] size_t count_animators() const noexcept override;
    void bind_expression_manager(ExpressionManager& mgr) override;

    static std::shared_ptr<PrecompLayer> parse(const json::JsonValue& val);
};

// Null / Controller Layer ("ty": 3)
class NullLayer : public LottieLayer {
public:
    void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const override;

    static std::shared_ptr<NullLayer> parse(const json::JsonValue& val);
};

// Image / Asset Layer ("ty": 2)
class ImageLayer : public LottieLayer {
public:
    std::string ref_id;

    void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const override;

    bool hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const override;
    [[nodiscard]] bool is_layer_animated(const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] std::optional<Rect> compute_bounds(float frame, const LottieAssetRegistry& assets) const override;
    [[nodiscard]] uint64_t visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept override;

    static std::shared_ptr<ImageLayer> parse(const json::JsonValue& val);
};

// Text / Typography Layer ("ty": 5)
class TextLayer : public LottieLayer {
public:
    std::vector<Keyframe<TextDocument>> keyframes;
    TextDocument static_doc;
    std::optional<std::string> text_override;
    std::optional<TextDocument> doc_override;
    std::vector<TextAnimator> animators;
    TextPathOptions path_options;
    std::string text_expression;
    std::shared_ptr<ExpressionEvaluator<std::string>> text_evaluator;

    [[nodiscard]] const TextDocument& evaluate_document(float frame) const noexcept;

    void render_with_assets(
        RenderContext& ctx,
        float frame,
        const LottieAssetRegistry& assets) const override;

    bool hit_test(float x, float y, float frame, const LottieAssetRegistry& assets, std::string& out_hit_name) const override;
    [[nodiscard]] bool is_layer_animated(const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] std::optional<Rect> compute_bounds(float frame, const LottieAssetRegistry& assets) const override;
    [[nodiscard]] uint64_t visual_fingerprint(float frame, const LottieAssetRegistry& assets) const noexcept override;
    [[nodiscard]] size_t count_animators() const noexcept override;
    void bind_expression_manager(ExpressionManager& mgr) override;

    static std::shared_ptr<TextLayer> parse(const json::JsonValue& val);

private:
    struct TextRenderCache;
    mutable std::shared_ptr<TextRenderCache> render_cache_;
};

// Audio Layer ("ty": 6)
class AudioLayer : public LottieLayer {
public:
    std::string ref_id;
    PointProperty level{Point::zero()}; // "au.lv" stereo dB levels [left, right]

    void render_with_assets(
        RenderContext&,
        float,
        const LottieAssetRegistry&) const override {}

    [[nodiscard]] size_t count_animators() const noexcept override;
    void bind_expression_manager(ExpressionManager& mgr) override;

    static std::shared_ptr<AudioLayer> parse(const json::JsonValue& val);
};

/// @brief Pre-link parent_ptr and matte_layer_ptr across a layer list once at load time.
void link_layer_hierarchy(std::vector<std::shared_ptr<LottieLayer>>& layers);

/// @brief Full layer tree renderer with Track Mattes, Masks, Effects, BlendModes, and 3D.
void render_layer_tree_with_assets(
    ICanvas& canvas,
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    float frame,
    const LottieAssetRegistry& assets,
    float parent_opacity = 1.0f);

/// @brief Legacy layer tree renderer overload.
void render_layer_tree(
    ICanvas& canvas,
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    float frame,
    const std::unordered_map<std::string, std::vector<std::shared_ptr<LottieLayer>>>& precomp_assets,
    float parent_opacity = 1.0f);

} // namespace nisaba::lottie
