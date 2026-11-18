#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <span>
#include "nisaba/math/point.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/text/attrs.hpp"
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

/// Builds continuous, span-merged decoration primitives for a single LayoutRun.
[[nodiscard]] DecorationGeometry build_run_decorations(
    const LayoutRun& run,
    float origin_x,
    float origin_y,
    float default_font_size,
    Color default_color
);

/// Represents a single paragraph line of text with its attributes and laid out visual lines.
class BufferLine {
public:
    BufferLine(std::string_view line_text, const Attrs& default_attrs);

    std::string_view text() const noexcept { return text_; }
    const AttrsList& attrs_list() const noexcept { return attrs_list_; }
    AttrsList& attrs_list_mut() noexcept { return attrs_list_; }

    const std::vector<LayoutLine>& layout() const noexcept { return layout_lines_; }
    std::vector<LayoutLine>& layout_mut() noexcept { return layout_lines_; }
    bool is_shaped() const noexcept { return is_shaped_; }

    void reset_shaping() noexcept { is_shaped_ = false; layout_lines_.clear(); }

    /// Shapes and lays out this line into visual lines according to wrap and alignment.
    void shape_and_layout(
        FontSystem& font_system,
        float font_size,
        float line_height,
        std::optional<float> wrap_width,
        Wrap wrap,
        Align align,
        Shaping shaping,
        bool enable_kerning = true,
        bool enable_ligatures = false
    );

private:
    std::string text_{};
    AttrsList attrs_list_{};
    std::vector<LayoutLine> layout_lines_{};
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
    const std::vector<BufferLine>& lines() const noexcept { return lines_; }
    std::vector<BufferLine>& lines_mut() noexcept { return lines_; }

    void reset_shaping() noexcept;

    /// Runs text shaping and layout on all lines until valid.
    void shape_until_scroll(FontSystem& font_system);

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

    /// Draws the buffer onto a Canvas using pre-rendered glyph masks, decorations, and highlights.
    void draw(
        Canvas& canvas,
        GlyphCache& cache,
        FontSystem& font_system,
        Color default_color,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    );

private:
    void apply_ellipsis_truncation(FontSystem& font_system, LayoutLine& line, float max_width) const;

    Metrics metrics_{16.0f, 20.0f};
    std::vector<BufferLine> lines_{};
    std::optional<float> width_{std::nullopt};
    std::optional<float> height_{std::nullopt};
    Wrap wrap_{Wrap::Word};
    Align align_{Align::Left};
    Shaping shaping_{Shaping::Advanced};
    Overflow overflow_{Overflow::Visible};
    std::optional<size_t> max_lines_{std::nullopt};
    bool enable_kerning_{true};
    bool enable_ligatures_{false};
    float scroll_{0.0f};
};

} // namespace nisaba::text
