#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/text/ttf_font.hpp"

namespace nisaba {

/// Immutable, reference-counted container of pre-positioned glyph runs for zero-cost multi-frame replay.
/// Supports both 2D positioned runs (x, y per glyph) and per-glyph RSXform runs (text along curves/paths).
class TextBlob {
public:
    enum class RunPositioning : uint8_t {
        Positioned = 0,
        RSXform = 1
    };

    struct Run {
        std::shared_ptr<const text::TtfFont> owned_font{};
        const text::TtfFont* font{nullptr};
        float font_size{16.0f};
        RunPositioning positioning{RunPositioning::Positioned};
        std::vector<uint16_t> glyph_ids{};
        std::vector<Point> positions{};
        std::vector<RSXform> xforms{};
        Rect bounds{};

        [[nodiscard]] size_t glyph_count() const noexcept { return glyph_ids.size(); }
    };

    TextBlob() = default;
    TextBlob(std::vector<Run> runs, Rect bounds) noexcept;

    [[nodiscard]] uint32_t unique_id() const noexcept { return unique_id_; }
    [[nodiscard]] const Rect& bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool is_empty() const noexcept { return runs_.empty(); }
    [[nodiscard]] std::span<const Run> runs() const noexcept { return runs_; }
    [[nodiscard]] size_t glyph_count() const noexcept;

    /// Builds an immutable single-run TextBlob by decoding UTF-8 text and positioning glyphs
    /// with font advances and kerning starting from (origin_x, origin_y).
    [[nodiscard]] static std::shared_ptr<TextBlob> from_text(
        std::string_view utf8_text,
        const text::TtfFont& font,
        float font_size,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    );

    [[nodiscard]] static std::shared_ptr<TextBlob> from_text(
        std::string_view utf8_text,
        std::shared_ptr<const text::TtfFont> font,
        float font_size,
        float origin_x = 0.0f,
        float origin_y = 0.0f
    );

    /// Builds an immutable single-run TextBlob from explicit glyph IDs and 2D positions.
    [[nodiscard]] static std::shared_ptr<TextBlob> from_pos_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        const text::TtfFont& font,
        float font_size
    );

    [[nodiscard]] static std::shared_ptr<TextBlob> from_pos_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        std::shared_ptr<const text::TtfFont> font,
        float font_size
    );

    /// Builds an immutable single-run TextBlob from explicit glyph IDs and per-glyph RSXform transforms.
    [[nodiscard]] static std::shared_ptr<TextBlob> from_rsxform_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        const text::TtfFont& font,
        float font_size
    );

    [[nodiscard]] static std::shared_ptr<TextBlob> from_rsxform_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        std::shared_ptr<const text::TtfFont> font,
        float font_size
    );

private:
    static uint32_t next_unique_id() noexcept;

    std::vector<Run> runs_{};
    Rect bounds_{};
    uint32_t unique_id_{0};
};

/// Builder for constructing multi-run immutable TextBlob instances.
class TextBlobBuilder {
public:
    TextBlobBuilder() = default;

    /// Appends a 2D-positioned glyph run referencing a caller-owned font.
    void add_run_pos(
        const text::TtfFont& font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        std::optional<Rect> custom_bounds = std::nullopt
    );

    /// Appends a 2D-positioned glyph run holding a shared reference to the font.
    void add_run_pos(
        std::shared_ptr<const text::TtfFont> font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        std::optional<Rect> custom_bounds = std::nullopt
    );

    /// Appends a horizontally-positioned glyph run sharing a common baseline Y coordinate.
    void add_run_pos_h(
        const text::TtfFont& font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const float> x_positions,
        float const_y,
        std::optional<Rect> custom_bounds = std::nullopt
    );

    void add_run_pos_h(
        std::shared_ptr<const text::TtfFont> font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const float> x_positions,
        float const_y,
        std::optional<Rect> custom_bounds = std::nullopt
    );

    /// Appends an RSXform-positioned glyph run (rotation, uniform scale, translation per glyph).
    void add_run_rsxform(
        const text::TtfFont& font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        std::optional<Rect> custom_bounds = std::nullopt
    );

    void add_run_rsxform(
        std::shared_ptr<const text::TtfFont> font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        std::optional<Rect> custom_bounds = std::nullopt
    );

    /// Finalizes all accumulated runs into an immutable TextBlob and resets the builder.
    [[nodiscard]] std::shared_ptr<TextBlob> make();

    void reset() noexcept { runs_.clear(); }
    [[nodiscard]] bool is_empty() const noexcept { return runs_.empty(); }

private:
    void add_run_pos_impl(
        std::shared_ptr<const text::TtfFont> owned_font,
        const text::TtfFont* font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        std::optional<Rect> custom_bounds
    );

    void add_run_rsxform_impl(
        std::shared_ptr<const text::TtfFont> owned_font,
        const text::TtfFont* font,
        float font_size,
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        std::optional<Rect> custom_bounds
    );

    std::vector<TextBlob::Run> runs_{};
};

namespace text {
using TextBlob = nisaba::TextBlob;
using TextBlobBuilder = nisaba::TextBlobBuilder;
} // namespace text

} // namespace nisaba

