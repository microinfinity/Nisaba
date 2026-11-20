#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <span>
#include "nisaba/math/point.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/text/attrs.hpp"
#include "nisaba/text/bidi.hpp"
#include "nisaba/text/layout.hpp"
#include "nisaba/text/font_system.hpp"
#include "nisaba/text/glyph_cache.hpp"

namespace nisaba::text {

/// Byte offset range [start, end) within flattened buffer coordinates.
struct TextRange {
    size_t start{0};
    size_t end{0};

    constexpr size_t length() const noexcept { return (end > start) ? (end - start) : 0; }
    constexpr bool empty() const noexcept { return start >= end; }
    constexpr bool operator==(const TextRange& other) const noexcept = default;
};

/// Result of hit-testing coordinates against a shaped text buffer.
struct GlyphPosition {
    size_t position{0};   /// Flattened UTF-8 byte offset across buffer lines
    bool upstream{false}; /// true = trailing/upstream affinity, false = leading/downstream affinity

    constexpr bool operator==(const GlyphPosition& other) const noexcept = default;
};

/// Vertical box extent mode for Buffer::range_rects.
enum class RectHeightStyle : uint8_t {
    Max,
    Tight
};

/// Shared CPU/GPU decoration geometry generated from a shaped LayoutRun.
struct DecorationGeometry {
    struct RectSpan {
        float x{0.0f};
        float y{0.0f};
        float w{0.0f};
        float h{0.0f};
        Color color{};
    };
    struct WaveStroke {
        std::vector<Point> points{};
        float stroke_width{1.0f};
        Color color{};
    };
    std::vector<RectSpan> rects{};
    std::vector<WaveStroke> waves{};
};

/// Returns true if a Paint requires vector path rendering instead of a solid alpha-mask blit.
[[nodiscard]] inline bool paint_requires_vector_path(const Paint& paint) noexcept {
    return !paint.is_solid_color() ||
           paint.paint_style() != PaintStyle::Fill ||
           paint.blend_mode != BlendMode::SourceOver ||
           paint.has_color_filter() ||
           paint.has_image_filter();
}

/// Builds continuous, span-merged decoration primitives for a single LayoutRun.
/// When font_system is provided, DecorationMode::Gaps skips underline strokes where glyph descenders intersect.
[[nodiscard]] DecorationGeometry build_run_decorations(
    const LayoutRun& run,
    float origin_x,
    float origin_y,
    float default_font_size,
    Color default_color,
    const FontSystem* font_system = nullptr
);

/// Inline placeholder record anchored at a byte offset within a BufferLine.
struct InlinePlaceholder {
    size_t byte_offset{0};
    size_t index{0};
    PlaceholderStyle style{};
};

/// Represents a single paragraph line of text with its attributes and laid out visual lines.
class BufferLine {
public:
    BufferLine(std::string_view line_text, const Attrs& default_attrs);

    std::string_view text() const noexcept { return text_; }
    const AttrsList& attrs_list() const noexcept { return attrs_list_; }
    AttrsList& attrs_list_mut() noexcept { return attrs_list_; }
    const std::vector<InlinePlaceholder>& placeholders() const noexcept { return placeholders_; }

    /// Appends styled text to the end of this paragraph line.
    void append_text(std::string_view segment, const Attrs& attrs);

    /// Appends an inline placeholder box (anchored via U+FFFC) to the end of this paragraph line.
    void append_placeholder(size_t index, const PlaceholderStyle& style, const Attrs& attrs);

    const std::vector<LayoutLine>& layout() const noexcept { return layout_lines_; }
    std::vector<LayoutLine>& layout_mut() noexcept { return layout_lines_; }
    bool is_shaped() const noexcept { return is_shaped_; }
    Direction resolved_base_direction() const noexcept { return resolved_base_dir_; }

    void reset_shaping() noexcept { is_shaped_ = false; layout_lines_.clear(); }

    void copy_shaped_state_from(const BufferLine& other) {
        layout_lines_ = other.layout_lines_;
        resolved_base_dir_ = other.resolved_base_dir_;
        is_shaped_ = other.is_shaped_;
    }

    /// Shapes and lays out this line into visual lines according to wrap, alignment, and direction.
    void shape_and_layout(
        FontSystem& font_system,
        float font_size,
        float line_height,
        std::optional<float> wrap_width,
        Wrap wrap,
        Align align,
        Shaping shaping,
        bool enable_kerning = true,
        bool enable_ligatures = false,
        TextDirection text_direction = TextDirection::Auto
    );

private:
    std::string text_{};
    AttrsList attrs_list_{};
    std::vector<InlinePlaceholder> placeholders_{};
    std::vector<LayoutLine> layout_lines_{};
    Direction resolved_base_dir_{Direction::LeftToRight};
    bool is_shaped_{false};
};

/// High-level text buffer managing multi-line text, wrapping, alignment, overflow, and rendering.
class Buffer {
public:
    Buffer(Metrics metrics = Metrics(16.0f, 20.0f));
    ~Buffer() = default;

    // --- Configuration ---
    const Metrics& metrics() const noexcept { return metrics_; }
    void set_metrics(const Metrics& m) noexcept { metrics_ = m; reset_shaping(); }

    std::optional<float> width() const noexcept { return width_; }
    std::optional<float> height() const noexcept { return height_; }
    void set_size(std::optional<float> width, std::optional<float> height) noexcept {
        width_ = width;
        height_ = height;
        reset_shaping();
    }

    Wrap wrap() const noexcept { return wrap_; }
    void set_wrap(Wrap w) noexcept { wrap_ = w; reset_shaping(); }

    Align align() const noexcept { return align_; }
    void set_align(Align a) noexcept { align_ = a; reset_shaping(); }

    Shaping shaping() const noexcept { return shaping_; }
    void set_shaping(Shaping s) noexcept { shaping_ = s; reset_shaping(); }

    Overflow overflow() const noexcept { return overflow_; }
    void set_overflow(Overflow o) noexcept { overflow_ = o; reset_shaping(); }

    TextDirection text_direction() const noexcept { return text_direction_; }
    void set_text_direction(TextDirection dir) noexcept { text_direction_ = dir; reset_shaping(); }

    const StrutStyle& strut_style() const noexcept { return strut_style_; }
    void set_strut_style(const StrutStyle& strut) { strut_style_ = strut; reset_shaping(); }

    bool half_leading() const noexcept { return half_leading_; }
    void set_half_leading(bool enabled) noexcept { half_leading_ = enabled; reset_shaping(); }

    const TextHeightBehavior& text_height_behavior() const noexcept { return text_height_behavior_; }
    void set_text_height_behavior(const TextHeightBehavior& behavior) noexcept {
        text_height_behavior_ = behavior;
        reset_shaping();
    }

    std::string_view ellipsis() const noexcept {
        return ellipsis_opt_.has_value() ? std::string_view(*ellipsis_opt_) : std::string_view("\xE2\x80\xA6");
    }
    bool has_custom_ellipsis() const noexcept { return ellipsis_opt_.has_value(); }
    void set_ellipsis(std::string_view ellipsis) {
        ellipsis_opt_ = std::string(ellipsis);
        reset_shaping();
    }
    void clear_ellipsis() noexcept {
        ellipsis_opt_.reset();
        reset_shaping();
    }

    std::optional<size_t> max_lines() const noexcept { return max_lines_; }
    void set_max_lines(std::optional<size_t> ml) noexcept { max_lines_ = ml; reset_shaping(); }

    bool kerning_enabled() const noexcept { return enable_kerning_; }
    void set_kerning_enabled(bool enabled) noexcept { enable_kerning_ = enabled; reset_shaping(); }

    bool ligatures_enabled() const noexcept { return enable_ligatures_; }
    void set_ligatures_enabled(bool enabled) noexcept { enable_ligatures_ = enabled; reset_shaping(); }

    float scroll() const noexcept { return scroll_; }
    void set_scroll(float s) noexcept { scroll_ = s; }

    // --- Content ---
    void set_text(std::string_view text, const Attrs& default_attrs = Attrs());

    /// Appends styled text (supporting embedded newlines) to the buffer without clearing existing spans or placeholders.
    void add_text(std::string_view text, const Attrs& attrs = Attrs());

    /// Appends an inline placeholder box that wraps and aligns naturally with text runs.
    /// Returns the zero-based placeholder index matching PlaceholderRect::placeholder_index.
    size_t add_placeholder(
        float width,
        float height,
        PlaceholderAlignment alignment = PlaceholderAlignment::Baseline,
        TextBaseline baseline = TextBaseline::Alphabetic,
        float baseline_offset = 0.0f
    );
    size_t add_placeholder(const PlaceholderStyle& style);

    /// Returns laid-out bounding boxes and resolved directions for all visible inline placeholders.
    [[nodiscard]] std::vector<PlaceholderRect> placeholder_rects(
        float origin_x = 0.0f,
        float origin_y = 0.0f
    ) const;

    [[nodiscard]] size_t placeholder_count() const noexcept { return next_placeholder_index_; }

    const std::vector<BufferLine>& lines() const noexcept { return lines_; }
    std::vector<BufferLine>& lines_mut() noexcept { return lines_; }

    void reset_shaping() noexcept;

    /// Runs text shaping and layout on all lines until valid.
    void shape_until_scroll(FontSystem& font_system);

    /// Number of full shaping passes executed on this buffer (diagnostics / verification).
    [[nodiscard]] uint64_t shape_count() const noexcept { return shape_count_; }

    /// Returns true if the buffer layout needs shaping before rendering.
    [[nodiscard]] bool is_layout_dirty() const noexcept { return layout_dirty_; }

    // --- Phase 4.1: Incremental In-Place Updates (No Full Re-Shape) ---

    /// Updates horizontal alignment in-place without re-shaping glyphs or re-running BiDi.
    void update_text_align(Align align) noexcept;

    /// Updates foreground Paint across flattened UTF-8 byte range [start, end) in-place without re-shaping.
    void update_foreground_paint(size_t start, size_t end, const Paint& paint);
    void update_foreground_paint(size_t start, size_t end, const Paint& paint, const Stroke& stroke);

    /// Updates background Paint across flattened UTF-8 byte range [start, end) in-place without re-shaping.
    void update_background_paint(size_t start, size_t end, const Paint& paint);

    /// Updates font size across flattened UTF-8 byte range [start, end) in-place without re-shaping.
    void update_font_size(size_t start, size_t end, float font_size);

    /// Hydrates this buffer's shaped layout state from an already-shaped cache entry.
    void copy_shaped_layout_from(const Buffer& cached);

    // --- Phase 4.2: Glyph Run Visitor ---
    using GlyphRunVisitor = std::function<void(const GlyphRunInfo&)>;

    /// Visits laid-out glyph runs in visual line order, exposing glyph IDs, fonts,
    /// positions, cluster indices, and line origins for custom renderers, PDF export, and accessibility.
    void visit(
        const GlyphRunVisitor& visitor,
        const FontSystem* font_system = nullptr,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    ) const;

    void visit(
        const FontSystem& font_system,
        const GlyphRunVisitor& visitor,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    ) const;

    /// Generates visual layout runs across all wrapped lines.
    std::vector<LayoutRun> layout_runs() const;

    /// Total visual height of all formatted lines.
    float total_height() const;

    /// Maximum visual width across all formatted lines.
    float max_line_width() const;

    /// Longest wrapped line width across the laid-out buffer.
    [[nodiscard]] float longest_line_width() const noexcept { return max_line_width(); }

    /// Maximum intrinsic width when shaped without line-wrapping constraints.
    [[nodiscard]] float max_intrinsic_width(FontSystem& font_system) const;

    /// Minimum intrinsic width (widest unbreakable word or cluster across the buffer).
    [[nodiscard]] float min_intrinsic_width(FontSystem& font_system) const;

    /// Reports whether layout truncation or max-lines/height overflow occurred.
    [[nodiscard]] bool did_exceed_max_lines() const noexcept { return did_exceed_max_lines_; }

    /// Returns detailed per-line typographic and layout metrics for all visual lines.
    [[nodiscard]] std::vector<LineMetrics> line_metrics() const;

    /// Distance from paragraph top to the alphabetic baseline of the first line.
    [[nodiscard]] float alphabetic_baseline() const noexcept;

    /// Distance from paragraph top to the ideographic bottom baseline of the first line.
    [[nodiscard]] float ideographic_baseline() const noexcept;

    /// Returns the number of unresolved (missing) glyphs in the laid-out buffer.
    [[nodiscard]] size_t unresolved_glyphs() const noexcept;

    /// Computes bounding rectangles covering flattened UTF-8 byte range [start, end).
    [[nodiscard]] std::vector<Rect> range_rects(
        size_t start,
        size_t end,
        float origin_x = 0.0f,
        float origin_y = 0.0f,
        RectHeightStyle height_style = RectHeightStyle::Max
    ) const;

    /// Maps visual coordinates (x, y) to the closest flattened byte position and edge affinity.
    [[nodiscard]] GlyphPosition glyph_position_at(
        float x,
        float y,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    ) const;

    /// Returns the flattened UTF-8 word range [start, end) containing or adjacent to byte_pos.
    [[nodiscard]] TextRange word_range_at(size_t byte_pos) const;

    /// Draws the buffer onto a Canvas using pre-rendered glyph masks, vector rich paints, decorations, and highlights.
    void draw(
        Canvas& canvas,
        GlyphCache& cache,
        FontSystem& font_system,
        Color default_color,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    );

    /// Draws the buffer onto a Canvas with a default Paint (supporting gradients, strokes, shaders, and blend modes).
    void draw(
        Canvas& canvas,
        GlyphCache& cache,
        FontSystem& font_system,
        const Paint& default_paint,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    );

private:
    void draw_internal(
        Canvas& canvas,
        GlyphCache& cache,
        FontSystem& font_system,
        Color default_color,
        const Paint* default_paint_override,
        float origin_x,
        float origin_y
    );

    void apply_vertical_line_metrics(const FontSystem* font_system);

    void apply_ellipsis_truncation(
        FontSystem& font_system,
        LayoutLine& line,
        const BufferLine& src_line,
        float max_width
    ) const;

    Metrics metrics_{16.0f, 20.0f};
    std::vector<BufferLine> lines_{};
    std::optional<float> width_{std::nullopt};
    std::optional<float> height_{std::nullopt};
    Wrap wrap_{Wrap::Word};
    Align align_{Align::Left};
    Shaping shaping_{Shaping::Advanced};
    Overflow overflow_{Overflow::Visible};
    TextDirection text_direction_{TextDirection::Auto};
    StrutStyle strut_style_{};
    TextHeightBehavior text_height_behavior_{};
    bool half_leading_{false};
    std::optional<std::string> ellipsis_opt_{std::nullopt};
    std::optional<size_t> max_lines_{std::nullopt};
    size_t next_placeholder_index_{0};
    bool enable_kerning_{true};
    bool enable_ligatures_{false};
    bool did_exceed_max_lines_{false};
    bool layout_dirty_{true};
    float scroll_{0.0f};
    uint64_t shape_count_{0};
    float cached_strut_asc_{0.0f};
    float cached_strut_desc_{0.0f};
    bool has_cached_strut_{false};
};

} // namespace nisaba::text
