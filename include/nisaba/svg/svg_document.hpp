#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>
#include <map>
#include <unordered_map>
#include <array>
#include <cstdint>
#include "nisaba/math/rect.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/shaders/shader.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/text/font_system.hpp"
#include "nisaba/effects/color_matrix.hpp"

namespace nisaba {
    class ICanvas;
    class Canvas;
    namespace gpu {
        class GpuCanvas;
    }
}

namespace nisaba::svg {

/// Coordinate space interpretation for SVG gradients, patterns, masks, clipPaths, and filters.
enum class SvgUnits : uint8_t {
    ObjectBoundingBox, ///< Coordinates [0, 1] relative to the geometry bounding box
    UserSpaceOnUse,    ///< Absolute coordinates in the current user coordinate space
};

using SvgGradientUnits = SvgUnits;

/// Aspect ratio alignment modes per W3C SVG 1.1 / 2.0 specification.
enum class SvgAspectAlign : uint8_t {
    None,
    XMinYMin,
    XMidYMin,
    XMaxYMin,
    XMinYMid,
    XMidYMid, ///< Default W3C SVG alignment
    XMaxYMid,
    XMinYMax,
    XMidYMax,
    XMaxYMax,
};

enum class SvgAspectMeetOrSlice : uint8_t {
    Meet,  ///< Scale uniformly so the entire viewBox is visible within viewport (default)
    Slice, ///< Scale uniformly so the entire viewport is covered by viewBox
};

struct SvgPreserveAspectRatio {
    SvgAspectAlign align{SvgAspectAlign::XMidYMid};
    SvgAspectMeetOrSlice meet_or_slice{SvgAspectMeetOrSlice::Meet};

    [[nodiscard]] Transform compute_transform(const Rect& view_box, const Rect& viewport) const noexcept;
};

/// Dynamic theme & CSS variable injection dictionary for runtime re-coloring without re-parsing XML.
struct SvgTheme {
    std::optional<Color> current_color{std::nullopt};             ///< Overrides 'currentColor'
    std::optional<Color> tint_color{std::nullopt};                ///< Optional global monochrome tint override
    std::unordered_map<std::string, Color> css_variables{};       ///< Maps "--var-name" or "var-name" -> Color
    std::unordered_map<std::string, Color> id_color_overrides{};  ///< Maps element "id" -> fill/stroke override

    [[nodiscard]] bool empty() const noexcept {
        return !current_color.has_value() && !tint_color.has_value() &&
               css_variables.empty() && id_color_overrides.empty();
    }
};

/// An SVG linear or radial gradient definition.
struct SvgGradient {
    enum class Kind : uint8_t { Linear, Radial };

    Kind kind{Kind::Linear};
    std::string id{};
    std::string href_id{};
    SvgGradientUnits units{SvgGradientUnits::ObjectBoundingBox};
    SpreadMode spread_mode{SpreadMode::Pad};
    Transform gradient_transform{};
    std::vector<GradientStop> stops{};
    bool linear_interpolation{false}; ///< True if color-interpolation="linearRGB"

    // Linear gradient coordinates (defaults: 0,0 to 1,0)
    float x1{0.0f};
    float y1{0.0f};
    float x2{1.0f};
    float y2{0.0f};
    bool has_x1{false}, has_y1{false}, has_x2{false}, has_y2{false};

    // Radial gradient coordinates (defaults: cx=0.5, cy=0.5, r=0.5, fx=cx, fy=cy, fr=0)
    float cx{0.5f};
    float cy{0.5f};
    float r{0.5f};
    float fx{0.5f};
    float fy{0.5f};
    float fr{0.0f};
    bool has_cx{false}, has_cy{false}, has_r{false}, has_fx{false}, has_fy{false}, has_fr{false};
    bool has_units{false}, has_spread{false}, has_transform{false};
};

/// Paint order configuration per W3C SVG 2.0 'paint-order' property.
enum class SvgPaintPhase : uint8_t {
    Fill = 0,
    Stroke = 1,
    Markers = 2,
};

/// Text anchor horizontal alignment.
enum class SvgTextAnchor : uint8_t {
    Start,
    Middle,
    End,
};

/// Dominant baseline vertical alignment.
enum class SvgDominantBaseline : uint8_t {
    Auto,
    Middle,
    Central,
    Hanging,
    Alphabetic,
    Ideographic,
    Mathematical,
    TextBeforeEdge,
    TextAfterEdge,
};

/// Paint and styling properties for an SVG element or group.
struct SvgPaintStyle {
    std::optional<Color> fill{Color::BLACK};     ///< Solid fill color (nullopt if fill="none" or server reference)
    bool fill_is_current_color{false};           ///< True if fill="currentColor"
    std::string fill_url{};                      ///< Paint server URL id if fill="url(#id)"
    std::string fill_css_var{};                  ///< CSS variable name if fill="var(--name)"
    std::optional<Color> fill_fallback{};        ///< Fallback color for url() or var()

    std::optional<Color> stroke{std::nullopt};   ///< Solid stroke color (nullopt if no stroke or "none")
    bool stroke_is_current_color{false};         ///< True if stroke="currentColor"
    std::string stroke_url{};                    ///< Paint server URL id if stroke="url(#id)"
    std::string stroke_css_var{};                ///< CSS variable name if stroke="var(--name)"
    std::optional<Color> stroke_fallback{};      ///< Fallback color for url() or var()

    Color current_color{Color::BLACK};           ///< Inherited 'color' attribute for 'currentColor' resolution

    float stroke_width{1.0f};                    ///< Stroke line width
    float miter_limit{4.0f};                     ///< Stroke miter limit
    LineCap line_cap{LineCap::Butt};             ///< Line cap style
    LineJoin line_join{LineJoin::Miter};         ///< Line join style
    std::vector<float> dash_array{};             ///< Stroke dash pattern intervals
    float dash_offset{0.0f};                     ///< Stroke dash phase offset
    bool non_scaling_stroke{false};              ///< True if vector-effect="non-scaling-stroke"

    float opacity{1.0f};                         ///< Group/Element opacity multiplier (isolated on groups)
    float fill_opacity{1.0f};                    ///< Fill opacity multiplier
    float stroke_opacity{1.0f};                  ///< Stroke opacity multiplier
    FillRule fill_rule{FillRule::Winding};       ///< Fill rule (Winding or EvenOdd)
    FillRule clip_rule{FillRule::Winding};       ///< Clip rule when inside <clipPath>
    BlendMode blend_mode{BlendMode::SourceOver}; ///< CSS mix-blend-mode
    bool linear_color_interpolation{false};      ///< True if color-interpolation="linearRGB"

    std::string clip_path_url{};                 ///< ClipPath reference if clip-path="url(#id)"
    std::string mask_url{};                      ///< Mask reference if mask="url(#id)"
    std::string filter_url{};                    ///< Filter reference if filter="url(#id)"

    std::string marker_start_url{};              ///< Marker reference for first vertex
    std::string marker_mid_url{};                ///< Marker reference for middle vertices
    std::string marker_end_url{};                ///< Marker reference for final vertex

    std::array<SvgPaintPhase, 3> paint_order{SvgPaintPhase::Fill, SvgPaintPhase::Stroke, SvgPaintPhase::Markers};

    bool display_none{false};                    ///< True if display="none"
    bool visibility_hidden{false};               ///< True if visibility="hidden" or "collapse"

    // Typography properties
    std::string font_family{"sans-serif"};
    float font_size{16.0f};
    text::Weight font_weight{text::Weight::Normal};
    text::Style font_style{text::Style::Normal};
    float letter_spacing{0.0f};
    SvgTextAnchor text_anchor{SvgTextAnchor::Start};
    SvgDominantBaseline dominant_baseline{SvgDominantBaseline::Auto};
};

/// A child span within an SVG <text> element (<tspan> or <textPath>).
struct SvgTextSpan {
    std::string text{};
    std::optional<float> x{};
    std::optional<float> y{};
    float dx{0.0f};
    float dy{0.0f};
    float rotate{0.0f};
    std::string path_href{};        ///< Non-empty if this is a <textPath href="#id">
    float start_offset{0.0f};       ///< Arc-length offset along <textPath>
    bool start_offset_percent{false};
    SvgPaintStyle style{};
};

/// The discriminator type of an SVG scene element.
enum class SvgElementType : uint8_t {
    Path,
    Image,
    Group,
    Text,
    Use,
};

/// A single rendered vector element, group container, text block, or embedded raster image.
struct SvgElement {
    SvgElementType type{SvgElementType::Path};
    Path path{};
    SvgPaintStyle style{};
    Transform transform{};
    std::string id{};
    std::string class_name{};

    // Image-specific properties (when type == SvgElementType::Image)
    std::shared_ptr<Pixmap> image{};
    std::shared_ptr<class SvgDocument> nested_svg{};
    Rect image_bounds{};
    SvgPreserveAspectRatio preserve_aspect_ratio{};

    // Group / Nested SVG properties (when type == SvgElementType::Group)
    std::vector<SvgElement> children{};
    std::optional<Rect> group_view_box{};
    std::optional<Rect> group_viewport{};

    // Text-specific properties (when type == SvgElementType::Text)
    float text_x{0.0f};
    float text_y{0.0f};
    float text_dx{0.0f};
    float text_dy{0.0f};
    std::vector<SvgTextSpan> text_spans{};

    // Use-specific properties (when type == SvgElementType::Use)
    std::string use_href{};
    float use_x{0.0f};
    float use_y{0.0f};
    std::optional<float> use_width{};
    std::optional<float> use_height{};
};

/// An SVG <clipPath> resource supporting multiple paths, clip-rules, and clipPathUnits.
struct SvgClipPath {
    std::string id{};
    SvgUnits units{SvgUnits::UserSpaceOnUse};
    Transform transform{};
    Path combined_path{};
    FillRule clip_rule{FillRule::Winding};
    std::vector<SvgElement> elements{};
};

/// An SVG <mask> container with child elements to be rasterized as a luminance/alpha mask.
struct SvgMask {
    std::string id{};
    SvgUnits units{SvgUnits::ObjectBoundingBox};
    SvgUnits content_units{SvgUnits::UserSpaceOnUse};
    bool is_alpha_mask{false}; ///< False = Luminance (default W3C), True = Alpha
    float x{-0.1f}, y{-0.1f}, width{1.2f}, height{1.2f};
    std::vector<SvgElement> elements{};
};

/// An SVG <pattern> paint server.
struct SvgPattern {
    std::string id{};
    std::string href_id{};
    SvgUnits units{SvgUnits::ObjectBoundingBox};
    SvgUnits content_units{SvgUnits::UserSpaceOnUse};
    Transform pattern_transform{};
    float x{0.0f}, y{0.0f}, width{0.0f}, height{0.0f};
    std::optional<Rect> view_box{};
    SvgPreserveAspectRatio preserve_aspect_ratio{};
    std::vector<SvgElement> elements{};
};

/// An SVG <marker> definition for path vertices.
struct SvgMarker {
    enum class OrientMode : uint8_t { Angle, Auto, AutoStartReverse };

    std::string id{};
    Rect view_box{};
    bool has_view_box{false};
    float ref_x{0.0f};
    float ref_y{0.0f};
    float marker_width{3.0f};
    float marker_height{3.0f};
    bool stroke_width_units{true}; ///< True = strokeWidth (default), False = userSpaceOnUse
    OrientMode orient_mode{OrientMode::Angle};
    float orient_angle{0.0f};
    SvgPreserveAspectRatio preserve_aspect_ratio{};
    std::vector<SvgElement> elements{};
};

/// Filter primitives supported inside an SVG <filter> graph.
struct SvgFilterPrimitive {
    enum class Type : uint8_t {
        GaussianBlur,
        DropShadow,
        Offset,
        ColorMatrix,
        Flood,
        Blend,
        Composite,
        Merge,
    };

    Type type{Type::GaussianBlur};
    std::string in1{};     ///< Input 1 identifier ("SourceGraphic", "SourceAlpha", or named result)
    std::string in2{};     ///< Input 2 identifier (for Blend / Composite)
    std::string result{};  ///< Output identifier

    // GaussianBlur / DropShadow
    float std_dev_x{0.0f};
    float std_dev_y{0.0f};

    // Offset / DropShadow
    float dx{0.0f};
    float dy{0.0f};

    // Flood / DropShadow
    Color flood_color{Color::BLACK};
    float flood_opacity{1.0f};

    // ColorMatrix
    effects::ColorMatrix color_matrix{};
    bool luminance_to_alpha{false};

    // Blend
    BlendMode blend_mode{BlendMode::SourceOver};

    // Composite
    enum class CompositeOp : uint8_t { Over, In, Out, Atop, Xor, Arithmetic };
    CompositeOp composite_op{CompositeOp::Over};
    float k1{0.0f}, k2{0.0f}, k3{0.0f}, k4{0.0f};

    // Merge
    std::vector<std::string> merge_inputs{};
};

/// An SVG <filter> pipeline definition.
struct SvgFilter {
    std::string id{};
    SvgUnits filter_units{SvgUnits::ObjectBoundingBox};
    SvgUnits primitive_units{SvgUnits::UserSpaceOnUse};
    float x{-0.1f}, y{-0.1f}, width{1.2f}, height{1.2f};
    std::vector<SvgFilterPrimitive> primitives{};
};

/// CSS Rule parsed from <style> blocks.
struct SvgCssRule {
    std::string selector{};     ///< e.g. ".cls-1", "#my-id", "path", "g .cls-2"
    int specificity{0};         ///< CSS specificity score
    std::string declarations{}; ///< Raw "fill: red; stroke-width: 2"
};

/// An in-memory parsed representation of a W3C SVG vector document.
class SvgDocument {
public:
    SvgDocument() noexcept {
        auto r = Rect::from_xywh(0.0f, 0.0f, 100.0f, 100.0f);
        if (r) view_box_ = *r;
    }

    /// Parses an SVG XML string into an SvgDocument.
    static std::optional<SvgDocument> parse(std::string_view svg_xml);

    /// Reads and parses an SVG file from the filesystem.
    static std::optional<SvgDocument> from_file(const std::string& path);

    /// Renders the SVG document onto a CPU Canvas, scaling to fit target_bounds and applying optional tint or theme.
    void render(Canvas& canvas, const Rect* target_bounds = nullptr, std::optional<Color> tint = std::nullopt) const;
    void render(Canvas& canvas, const Rect* target_bounds, const SvgTheme& theme) const;

#if defined(NISABA_HAS_GPU) || defined(NISABA_GLEW)
    /// Renders the SVG document onto a hardware GpuCanvas with full 1:1 feature parity.
    void render_gpu(gpu::GpuCanvas& canvas, const Rect* target_bounds = nullptr, std::optional<Color> tint = std::nullopt) const;
    void render_gpu(gpu::GpuCanvas& canvas, const Rect* target_bounds, const SvgTheme& theme) const;
#endif

    /// Performs exact vector hit-testing at point `pt` (in target_bounds space if provided, otherwise viewBox space).
    /// Returns the `id` of the topmost hit element (or group), or std::nullopt if no element was hit.
    [[nodiscard]] std::optional<std::string> hit_test(Point pt, const Rect* target_bounds = nullptr) const;

    /// Computes the bounding box of an element by its `id` in SVG user coordinates.
    [[nodiscard]] std::optional<Rect> get_element_bounds(std::string_view id) const;

    [[nodiscard]] Rect view_box() const noexcept { return view_box_; }
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return height_; }
    [[nodiscard]] SvgPreserveAspectRatio preserve_aspect_ratio() const noexcept { return preserve_aspect_ratio_; }

    /// Flattened/top-level elements list (preserves backward compatibility while also holding hierarchical groups).
    [[nodiscard]] const std::vector<SvgElement>& elements() const noexcept { return elements_; }
    [[nodiscard]] const std::map<std::string, SvgGradient>& gradients() const noexcept { return gradients_; }
    [[nodiscard]] const std::map<std::string, Path>& clip_paths() const noexcept { return clip_paths_; }
    [[nodiscard]] const std::map<std::string, SvgClipPath>& rich_clip_paths() const noexcept { return rich_clip_paths_; }
    [[nodiscard]] const std::map<std::string, SvgMask>& masks() const noexcept { return masks_; }
    [[nodiscard]] const std::map<std::string, SvgPattern>& patterns() const noexcept { return patterns_; }
    [[nodiscard]] const std::map<std::string, SvgMarker>& markers() const noexcept { return markers_; }
    [[nodiscard]] const std::map<std::string, SvgFilter>& filters() const noexcept { return filters_; }
    [[nodiscard]] const std::map<std::string, SvgElement>& defined_elements() const noexcept { return defined_elements_; }
    [[nodiscard]] const std::unordered_map<std::string, Color>& root_css_variables() const noexcept { return root_css_vars_; }

    void set_view_box(Rect vb) noexcept { view_box_ = vb; }
    void set_dimensions(float w, float h) noexcept { width_ = w; height_ = h; }
    void set_preserve_aspect_ratio(SvgPreserveAspectRatio par) noexcept { preserve_aspect_ratio_ = par; }
    void add_element(SvgElement elem) { elements_.push_back(std::move(elem)); }
    void add_gradient(SvgGradient grad) { gradients_[grad.id] = std::move(grad); }
    void add_clip_path(std::string id, Path path) {
        SvgClipPath cp;
        cp.id = id;
        cp.combined_path = path;
        rich_clip_paths_[id] = std::move(cp);
        clip_paths_[std::move(id)] = std::move(path);
    }
    void add_rich_clip_path(SvgClipPath cp) {
        clip_paths_[cp.id] = cp.combined_path;
        rich_clip_paths_[cp.id] = std::move(cp);
    }
    void add_mask(SvgMask mask) { masks_[mask.id] = std::move(mask); }
    void add_pattern(SvgPattern pat) { patterns_[pat.id] = std::move(pat); }
    void add_marker(SvgMarker marker) { markers_[marker.id] = std::move(marker); }
    void add_filter(SvgFilter filter) { filters_[filter.id] = std::move(filter); }
    void add_defined_element(std::string id, SvgElement elem) { defined_elements_[std::move(id)] = std::move(elem); }
    void set_css_variable(std::string name, Color col) { root_css_vars_[std::move(name)] = col; }

private:
    void apply_clip_path_to_canvas(ICanvas& canvas, const SvgElement& elem) const;
    void render_element_dispatch(ICanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth, bool is_gpu) const;
    void render_element(Canvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth = 0) const;
    void render_element_raw(ICanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth, bool is_gpu = false) const;
    void render_markers(ICanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth, bool is_gpu = false) const;
#if defined(NISABA_HAS_GPU) || defined(NISABA_GLEW)
    void render_element_gpu(gpu::GpuCanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth = 0) const;
#endif
    Path build_text_path(const SvgElement& elem) const;
    Rect compute_element_local_bounds(const SvgElement& elem, int depth = 0) const;

    Rect view_box_{};
    float width_{100.0f};
    float height_{100.0f};
    SvgPreserveAspectRatio preserve_aspect_ratio_{};
    std::vector<SvgElement> elements_;
    std::map<std::string, SvgGradient> gradients_;
    std::map<std::string, Path> clip_paths_;
    std::map<std::string, SvgClipPath> rich_clip_paths_;
    std::map<std::string, SvgMask> masks_;
    std::map<std::string, SvgPattern> patterns_;
    std::map<std::string, SvgMarker> markers_;
    std::map<std::string, SvgFilter> filters_;
    std::map<std::string, SvgElement> defined_elements_;
    std::unordered_map<std::string, Color> root_css_vars_;
    mutable std::vector<std::shared_ptr<Pixmap>> pattern_cache_;
};

} // namespace nisaba::svg
