#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <optional>
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/text/attrs.hpp"

namespace nisaba::text {

class TtfFont;

/// Wrapping modes for multi-line text layout.
enum class Wrap : uint8_t {
    None,
    Glyph,
    Word,
    WordOrGlyph
};

/// Text horizontal alignment modes.
enum class Align : uint8_t {
    Left,
    Right,
    Center,
    Justified,
    Start,
    End
};

/// Text overflow handling modes when constrained by width/height/max_lines.
enum class Overflow : uint8_t {
    Visible,
    Clip,
    Ellipsis
};

/// Explicit paragraph base text direction independent of horizontal alignment.
enum class TextDirection : uint8_t {
    Auto,
    LTR,
    RTL
};

/// Vertical alignment mode for inline placeholders within a text line.
enum class PlaceholderAlignment : uint8_t {
    Baseline,        /// Aligns placeholder baseline (top + baseline_offset) with the text baseline
    AboveBaseline,   /// Bottom edge of placeholder sits on the text baseline
    BelowBaseline,   /// Top edge of placeholder sits on the text baseline
    Top,             /// Top edge of placeholder aligns with line top
    Bottom,          /// Bottom edge of placeholder aligns with line bottom
    Middle           /// Vertical center of placeholder aligns with line vertical center
};

/// Baseline reference used by PlaceholderAlignment::Baseline.
enum class TextBaseline : uint8_t {
    Alphabetic,
    Ideographic
};

/// Describes an inline placeholder box embedded in a text buffer.
struct PlaceholderStyle {
    float width{0.0f};
    float height{0.0f};
    PlaceholderAlignment alignment{PlaceholderAlignment::Baseline};
    TextBaseline baseline{TextBaseline::Alphabetic};
    float baseline_offset{0.0f};

    constexpr bool operator==(const PlaceholderStyle& other) const noexcept = default;
};

/// Laid-out bounding rectangle and resolved direction for an inline placeholder.
struct PlaceholderRect {
    Rect rect{};
    TextDirection direction{TextDirection::LTR};
    size_t placeholder_index{0};
    size_t line_number{0};
};

/// Controls uniform baseline-to-baseline rhythm across mixed scripts and font sizes.
struct StrutStyle {
    bool enabled{false};
    std::vector<Family> font_families{};
    Weight weight{Weight::Normal};
    Style style{Style::Normal};
    std::optional<float> font_size{std::nullopt};
    float height_multiplier{1.0f};
    float leading{-1.0f};
    bool force_strut_height{false};
    bool half_leading{false};

    bool operator==(const StrutStyle& other) const noexcept = default;
};

/// Controls whether line-height expansion applies to the first line's top ascent and last line's bottom descent.
struct TextHeightBehavior {
    bool apply_height_to_first_ascent{true};
    bool apply_height_to_last_descent{true};

    constexpr bool operator==(const TextHeightBehavior& other) const noexcept = default;
};

/// Per-line layout and typographic metrics for a shaped visual line.
struct LineMetrics {
    size_t start_index{0};        /// Start byte offset in flattened buffer coordinates
    size_t end_index{0};          /// End byte offset (exclusive) in flattened buffer coordinates
    float ascent{0.0f};           /// Positive distance above baseline
    float descent{0.0f};          /// Positive distance below baseline
    float unscaled_ascent{0.0f};  /// Unscaled ascent above baseline
    float height{0.0f};           /// Total line box height
    float width{0.0f};            /// Laid-out visual width of the line
    float left{0.0f};             /// Horizontal offset from paragraph origin due to alignment
    float baseline{0.0f};         /// Y coordinate of the alphabetic baseline from paragraph top
    bool hard_break{false};       /// True if terminated by an explicit newline or end of paragraph
    size_t line_number{0};        /// Zero-based visual line index
};

/// Represents an individual placed glyph within a laid-out line.
struct LayoutGlyph {
    size_t start{0};                      /// Start byte index in line string
    size_t end{0};                        /// End byte index in line string
    float font_size{16.0f};               /// Font size in pixels
    Weight font_weight{Weight::Normal};   /// Font weight
    std::optional<float> line_height_opt{std::nullopt};
    uint32_t font_id{0};                  /// Associated Font ID
    uint16_t glyph_id{0};                 /// Glyph index in font
    float x{0.0f};                        /// X position along line baseline
    float y{0.0f};                        /// Y position relative to line baseline
    float w{0.0f};                        /// Advance width
    float ascent{0.0f};                   /// Glyph font ascent at current font_size
    float descent{0.0f};                  /// Glyph font descent (positive) at current font_size
    float justified_extra_w{0.0f};        /// Extra inter-word advance added by Align::Justified
    uint8_t level{0};                     /// BiDi level (odd = RTL)
    float x_offset{0.0f};
    float y_offset{0.0f};
    std::optional<TextColor> color_opt{std::nullopt};
    std::optional<TextColor> background_color_opt{std::nullopt};
    size_t metadata{0};
    bool is_mark{false};
    bool underline{false};
    bool strikethrough{false};
    bool overline{false};
    DecorationStyle decoration_style{DecorationStyle::Solid};
    DecorationMode decoration_mode{DecorationMode::Through};
    std::optional<TextColor> decoration_color_opt{std::nullopt};
    float decoration_thickness{1.0f};
    std::shared_ptr<const Paint> foreground_paint_opt{nullptr};
    std::shared_ptr<const Paint> background_paint_opt{nullptr};
    std::shared_ptr<const Stroke> foreground_stroke_opt{nullptr};
    std::shared_ptr<const std::vector<FontVariation>> variation_coords_opt{nullptr};
    uint64_t variation_hash{0};
    std::vector<TextShadow> shadows{};
    bool synthetic_bold{false};
    bool synthetic_italic{false};
    bool is_placeholder{false};
    size_t placeholder_index{0};
    float placeholder_height{0.0f};
    PlaceholderAlignment placeholder_alignment{PlaceholderAlignment::Baseline};
    TextBaseline placeholder_baseline{TextBaseline::Alphabetic};
    float placeholder_baseline_offset{0.0f};
};

/// A visually laid-out line of text containing placed glyphs.
struct LayoutLine {
    float w{0.0f};                        /// Visual width of line
    float left{0.0f};                     /// Horizontal alignment offset of line
    float max_ascent{0.0f};               /// Maximum ascent among glyphs
    float max_descent{0.0f};              /// Maximum descent among glyphs
    float unscaled_ascent{0.0f};          /// Unscaled maximum ascent among glyphs
    float unscaled_descent{0.0f};         /// Unscaled maximum descent among glyphs
    std::optional<float> line_height_opt{std::nullopt};
    size_t start_byte{0};                 /// Start byte offset within the BufferLine text
    size_t end_byte{0};                   /// End byte offset (exclusive) within the BufferLine text
    bool hard_break{false};               /// True if this is the final visual line of a BufferLine
    bool is_ellipsized{false};            /// True if this line was truncated with an ellipsis
    std::vector<LayoutGlyph> glyphs{};
};

/// Represents a visual run for iterating lines in a rendered buffer.
struct LayoutRun {
    size_t line_i{0};
    std::string_view text{};
    std::span<const LayoutGlyph> glyphs{};
    float line_top{0.0f};
    float line_height{20.0f};
    float line_width{0.0f};
    float max_ascent{0.0f};
    float max_descent{0.0f};
};

/// Describes a contiguous same-font, same-style run of laid-out glyphs on a visual line,
/// exposed by Buffer::visit() for custom renderers, PDF export, and accessibility trees.
struct GlyphRunInfo {
    size_t line_number{0};                /// Zero-based visual line index across the buffer
    size_t paragraph_line{0};             /// Source BufferLine index
    uint32_t font_id{0};                  /// Font ID in FontSystem
    const TtfFont* font{nullptr};         /// Resolved font pointer (non-null when FontSystem is provided)
    float font_size{16.0f};               /// Effective font size in pixels
    Weight font_weight{Weight::Normal};   /// Requested font weight
    bool synthetic_bold{false};
    bool synthetic_italic{false};
    bool is_rtl{false};                   /// True when run BiDi level is odd (RTL)
    bool is_placeholder{false};           /// True when this run represents an inline placeholder box
    size_t placeholder_index{0};          /// Placeholder index when is_placeholder == true
    Point origin{};                       /// Visual line baseline origin: (origin_x + line_left, origin_y + line_top + baseline)
    float line_top{0.0f};                 /// Visual line top Y (including origin_y)
    float line_left{0.0f};                /// Visual line left X (including origin_x)
    float line_height{0.0f};              /// Visual line height
    float line_width{0.0f};               /// Visual line width
    float baseline{0.0f};                 /// Baseline offset from line_top
    float ascent{0.0f};                   /// Line ascent (positive)
    float descent{0.0f};                  /// Line descent (positive)
    float advance_width{0.0f};            /// Total advance width of this run
    std::string_view utf8_text{};         /// Source BufferLine UTF-8 text view
    size_t text_byte_offset{0};           /// Base flattened UTF-8 byte offset of this BufferLine
    std::span<const uint16_t> glyph_ids{};    /// Glyph indices in `font`
    std::span<const Point> positions{};       /// Glyph baseline positions (x, y) relative to `origin`
    std::span<const float> advances{};        /// Glyph advance widths
    std::span<const uint32_t> clusters{};     /// Flattened UTF-8 byte start indices (cluster indices)
    std::span<const LayoutGlyph> glyphs{};    /// Underlying LayoutGlyph slice for full style inspection
};

} // namespace nisaba::text
