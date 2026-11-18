#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <optional>
#include "nisaba/text/attrs.hpp"

namespace nisaba::text {

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
    std::optional<TextColor> decoration_color_opt{std::nullopt};
    float decoration_thickness{1.0f};
    std::vector<TextShadow> shadows{};
    bool synthetic_bold{false};
    bool synthetic_italic{false};
};

/// A visually laid-out line of text containing placed glyphs.
struct LayoutLine {
    float w{0.0f};                        /// Visual width of line
    float max_ascent{0.0f};               /// Maximum ascent among glyphs
    float max_descent{0.0f};              /// Maximum descent among glyphs
    std::optional<float> line_height_opt{std::nullopt};
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

} // namespace nisaba::text
