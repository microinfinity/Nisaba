#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include "nisaba/color/color.hpp"

namespace nisaba::text {

/// Text color representation (RGBA u8).
struct TextColor {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{255};

    constexpr TextColor() noexcept = default;
    constexpr TextColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) noexcept
        : r(red), g(green), b(blue), a(alpha) {}

    static constexpr TextColor rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return TextColor(r, g, b, 255);
    }

    static constexpr TextColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept {
        return TextColor(r, g, b, a);
    }

    constexpr uint32_t to_u32() const noexcept {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8)  |
               static_cast<uint32_t>(b);
    }

    static constexpr TextColor from_u32(uint32_t val) noexcept {
        return TextColor(
            static_cast<uint8_t>((val >> 16) & 0xFF),
            static_cast<uint8_t>((val >> 8) & 0xFF),
            static_cast<uint8_t>(val & 0xFF),
            static_cast<uint8_t>((val >> 24) & 0xFF)
        );
    }

    Color to_color() const noexcept {
        return Color::from_rgba8(r, g, b, a);
    }

    constexpr bool operator==(const TextColor& other) const noexcept = default;
};

/// Font weight values (CSS-aligned 100 to 900).
enum class Weight : uint16_t {
    Thin = 100,
    ExtraLight = 200,
    Light = 300,
    Normal = 400,
    Medium = 500,
    SemiBold = 600,
    Bold = 700,
    ExtraBold = 800,
    Black = 900
};

/// Font style variations.
enum class Style : uint8_t {
    Normal,
    Italic,
    Oblique
};

/// Font width stretching.
enum class Stretch : uint8_t {
    UltraCondensed = 1,
    ExtraCondensed = 2,
    Condensed = 3,
    SemiCondensed = 4,
    Normal = 5,
    SemiExpanded = 6,
    Expanded = 7,
    ExtraExpanded = 8,
    UltraExpanded = 9
};

/// Generic and named font families.
struct Family {
    enum class Kind : uint8_t {
        Name,
        Serif,
        SansSerif,
        Monospace,
        Cursive,
        Fantasy,
        SystemUI,
        Emoji,
        Math
    } kind{Kind::SansSerif};

    std::string name{};

    Family() noexcept = default;
    /* implicit */ Family(Kind k) noexcept : kind(k) {}
    /* implicit */ Family(std::string_view family_name) : kind(Kind::Name), name(family_name) {}

    static Family serif() noexcept { return Family(Kind::Serif); }
    static Family sans_serif() noexcept { return Family(Kind::SansSerif); }
    static Family monospace() noexcept { return Family(Kind::Monospace); }
    static Family cursive() noexcept { return Family(Kind::Cursive); }
    static Family fantasy() noexcept { return Family(Kind::Fantasy); }
    static Family system_ui() noexcept { return Family(Kind::SystemUI); }
    static Family emoji() noexcept { return Family(Kind::Emoji); }
    static Family math() noexcept { return Family(Kind::Math); }

    bool operator==(const Family& other) const noexcept {
        if (kind != other.kind) return false;
        if (kind == Kind::Name) return name == other.name;
        return true;
    }
};

/// Text layout metrics.
struct Metrics {
    float font_size{16.0f};
    float line_height{20.0f};

    constexpr Metrics() noexcept = default;
    constexpr Metrics(float size, float height) noexcept
        : font_size(size), line_height(height) {}

    constexpr bool operator==(const Metrics& other) const noexcept {
        return font_size == other.font_size && line_height == other.line_height;
    }
};

/// Text shaping strategy.
enum class Shaping : uint8_t {
    Basic,     /// Fast scalar advance shaping
    Advanced   /// Advanced script analysis, font fallback, GSUB/GPOS, and contextual shaping
};

/// Visual attributes associated with a run of text.
struct Attrs {
    Family family{Family::sans_serif()};
    Weight weight{Weight::Normal};
    Style style{Style::Normal};
    Stretch stretch{Stretch::Normal};
    std::optional<TextColor> color_opt{std::nullopt};
    std::optional<TextColor> background_color_opt{std::nullopt};
    std::optional<float> font_size_opt{std::nullopt};
    float letter_spacing{0.0f};
    float word_spacing{0.0f};
    float baseline_shift{0.0f};
    bool underline{false};
    bool strikethrough{false};
    size_t metadata{0};

    Attrs() noexcept = default;

    Attrs& set_family(const Family& fam) noexcept { family = fam; return *this; }
    Attrs& set_weight(Weight w) noexcept { weight = w; return *this; }
    Attrs& set_style(Style s) noexcept { style = s; return *this; }
    Attrs& set_stretch(Stretch st) noexcept { stretch = st; return *this; }
    Attrs& set_color(TextColor c) noexcept { color_opt = c; return *this; }
    Attrs& set_background_color(TextColor c) noexcept { background_color_opt = c; return *this; }
    Attrs& set_font_size(float sz) noexcept { font_size_opt = sz; return *this; }
    Attrs& set_letter_spacing(float ls) noexcept { letter_spacing = ls; return *this; }
    Attrs& set_word_spacing(float ws) noexcept { word_spacing = ws; return *this; }
    Attrs& set_baseline_shift(float bs) noexcept { baseline_shift = bs; return *this; }
    Attrs& set_underline(bool u = true) noexcept { underline = u; return *this; }
    Attrs& set_strikethrough(bool s = true) noexcept { strikethrough = s; return *this; }
    Attrs& set_metadata(size_t meta) noexcept { metadata = meta; return *this; }

    [[nodiscard]] bool has_custom_metrics_or_decorations() const noexcept {
        return font_size_opt.has_value() ||
               letter_spacing != 0.0f ||
               word_spacing != 0.0f ||
               baseline_shift != 0.0f ||
               underline ||
               strikethrough ||
               background_color_opt.has_value();
    }

    bool operator==(const Attrs& other) const noexcept = default;
};

/// Span associating byte range [start, end) with formatting attributes.
struct AttrsSpan {
    size_t start{0};
    size_t end{0};
    Attrs attrs{};
};

/// Manages rich text attributes across byte ranges.
class AttrsList {
public:
    explicit AttrsList(const Attrs& defaults = Attrs()) : defaults_(defaults) {}

    const Attrs& defaults() const noexcept { return defaults_; }
    void set_defaults(const Attrs& def) noexcept { defaults_ = def; }

    void add_span(size_t start, size_t end, const Attrs& attrs) {
        if (start >= end) return;
        spans_.push_back(AttrsSpan{start, end, attrs});
    }

    void clear_spans() noexcept {
        spans_.clear();
    }

    const std::vector<AttrsSpan>& spans() const noexcept {
        return spans_;
    }

    Attrs get(size_t byte_index) const {
        for (auto it = spans_.rbegin(); it != spans_.rend(); ++it) {
            if (byte_index >= it->start && byte_index < it->end) {
                return it->attrs;
            }
        }
        return defaults_;
    }

private:
    Attrs defaults_{};
    std::vector<AttrsSpan> spans_{};
};

} // namespace nisaba::text
