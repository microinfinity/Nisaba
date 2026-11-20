#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>
#include <algorithm>
#include "nisaba/color/color.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/stroker.hpp"

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

/// Line style for text decorations (underline, overline, strikethrough).
enum class DecorationStyle : uint8_t {
    Solid,
    Double,
    Dotted,
    Dashed,
    Wavy
};

/// Controls how underline decorations interact with glyph descenders.
enum class DecorationMode : uint8_t {
    Through,  /// Continuous decoration stroke across the entire span
    Gaps      /// Skip underline ink where glyph descenders intersect the underline region
};

/// Drop shadow attached to a run of text.
struct TextShadow {
    TextColor color{TextColor::rgba(0, 0, 0, 128)};
    float offset_x{0.0f};
    float offset_y{0.0f};
    float blur_sigma{0.0f};

    constexpr bool operator==(const TextShadow& other) const noexcept = default;
};

/// OpenType font feature setting (4-byte tag + integer value: 0 = off, >=1 = enabled / alternate index).
struct FontFeature {
    uint32_t tag{0};
    uint32_t value{1};

    constexpr FontFeature() noexcept = default;
    constexpr FontFeature(uint32_t feature_tag, uint32_t feature_value = 1) noexcept
        : tag(feature_tag), value(feature_value) {}

    static constexpr uint32_t make_tag(char a, char b, char c, char d) noexcept {
        return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8)  |
                static_cast<uint32_t>(static_cast<uint8_t>(d));
    }

    static constexpr FontFeature from_string(std::string_view tag_str, uint32_t val = 1) noexcept {
        const char c0 = (tag_str.size() > 0) ? tag_str[0] : ' ';
        const char c1 = (tag_str.size() > 1) ? tag_str[1] : ' ';
        const char c2 = (tag_str.size() > 2) ? tag_str[2] : ' ';
        const char c3 = (tag_str.size() > 3) ? tag_str[3] : ' ';
        return FontFeature(make_tag(c0, c1, c2, c3), val);
    }

    constexpr bool operator==(const FontFeature& other) const noexcept = default;
};

/// Variable font axis coordinate (e.g., "wght"=700, "wdth"=80, "slnt"=-10, "opsz"=24).
struct FontVariation {
    uint32_t tag{0};
    float value{0.0f};

    constexpr FontVariation() noexcept = default;
    constexpr FontVariation(uint32_t axis_tag, float axis_value) noexcept
        : tag(axis_tag), value(axis_value) {}

    static constexpr FontVariation from_string(std::string_view tag_str, float val) noexcept {
        return FontVariation(FontFeature::from_string(tag_str, 0).tag, val);
    }

    constexpr bool operator==(const FontVariation& other) const noexcept = default;
};

/// Computes a stable 64-bit hash for a span of variable font axis coordinates.
[[nodiscard]] inline uint64_t hash_font_variations(const std::vector<FontVariation>& vars) noexcept {
    if (vars.empty()) return 0;
    uint64_t h = 14695981039346656037ULL;
    for (const auto& v : vars) {
        h ^= static_cast<uint64_t>(v.tag);
        h *= 1099511628211ULL;
        const int32_t q = static_cast<int32_t>(v.value * 64.0f + (v.value >= 0.0f ? 0.5f : -0.5f));
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(q));
        h *= 1099511628211ULL;
    }
    return h != 0 ? h : 1ULL;
}

/// Visual attributes associated with a run of text.
struct Attrs {
    Family family{Family::sans_serif()};
    std::vector<Family> font_families{};
    std::string locale{};
    Weight weight{Weight::Normal};
    Style style{Style::Normal};
    Stretch stretch{Stretch::Normal};
    std::vector<FontFeature> font_features{};
    std::vector<FontVariation> font_variations{};
    std::optional<TextColor> color_opt{std::nullopt};
    std::optional<TextColor> background_color_opt{std::nullopt};
    std::optional<float> font_size_opt{std::nullopt};
    float letter_spacing{0.0f};
    float word_spacing{0.0f};
    float baseline_shift{0.0f};
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
    std::vector<TextShadow> shadows{};
    size_t metadata{0};

    Attrs() noexcept = default;

    Attrs& set_family(const Family& fam) noexcept { family = fam; font_families.clear(); return *this; }
    Attrs& set_font_families(std::vector<Family> families) {
        font_families = std::move(families);
        if (!font_families.empty()) {
            family = font_families.front();
        }
        return *this;
    }
    Attrs& add_font_family(const Family& fam) {
        if (font_families.empty()) {
            family = fam;
        }
        font_families.push_back(fam);
        return *this;
    }
    Attrs& set_locale(std::string_view bcp47_locale) {
        locale = std::string(bcp47_locale);
        return *this;
    }
    Attrs& add_font_feature(std::string_view tag, uint32_t value = 1) {
        return add_font_feature(FontFeature::from_string(tag, value).tag, value);
    }
    Attrs& add_font_feature(uint32_t tag, uint32_t value = 1) {
        for (auto& f : font_features) {
            if (f.tag == tag) {
                f.value = value;
                return *this;
            }
        }
        font_features.push_back(FontFeature{tag, value});
        return *this;
    }
    Attrs& set_font_features(std::vector<FontFeature> features) {
        font_features = std::move(features);
        return *this;
    }
    Attrs& clear_font_features() noexcept { font_features.clear(); return *this; }
    Attrs& add_font_variation(std::string_view tag, float value) {
        return add_font_variation(FontVariation::from_string(tag, value).tag, value);
    }
    Attrs& add_font_variation(uint32_t tag, float value) {
        for (auto& v : font_variations) {
            if (v.tag == tag) {
                v.value = value;
                return *this;
            }
        }
        font_variations.push_back(FontVariation{tag, value});
        return *this;
    }
    Attrs& set_font_variations(std::vector<FontVariation> variations) {
        font_variations = std::move(variations);
        return *this;
    }
    Attrs& clear_font_variations() noexcept { font_variations.clear(); return *this; }
    Attrs& set_weight(Weight w) noexcept { weight = w; return *this; }
    Attrs& set_style(Style s) noexcept { style = s; return *this; }
    Attrs& set_stretch(Stretch st) noexcept { stretch = st; return *this; }
    Attrs& set_color(TextColor c) noexcept { color_opt = c; return *this; }
    Attrs& set_background_color(TextColor c) noexcept { background_color_opt = c; return *this; }
    Attrs& set_foreground_paint(const Paint& paint) {
        foreground_paint_opt = std::make_shared<Paint>(paint);
        foreground_stroke_opt.reset();
        return *this;
    }
    Attrs& set_foreground_paint(const Paint& paint, const Stroke& stroke) {
        foreground_paint_opt = std::make_shared<Paint>(paint);
        foreground_stroke_opt = std::make_shared<Stroke>(stroke);
        return *this;
    }
    Attrs& clear_foreground_paint() noexcept {
        foreground_paint_opt.reset();
        foreground_stroke_opt.reset();
        return *this;
    }
    Attrs& set_background_paint(const Paint& paint) {
        background_paint_opt = std::make_shared<Paint>(paint);
        return *this;
    }
    Attrs& clear_background_paint() noexcept {
        background_paint_opt.reset();
        return *this;
    }
    Attrs& set_font_size(float sz) noexcept { font_size_opt = sz; return *this; }
    Attrs& set_letter_spacing(float ls) noexcept { letter_spacing = ls; return *this; }
    Attrs& set_word_spacing(float ws) noexcept { word_spacing = ws; return *this; }
    Attrs& set_baseline_shift(float bs) noexcept { baseline_shift = bs; return *this; }
    Attrs& set_underline(bool u = true) noexcept { underline = u; return *this; }
    Attrs& set_strikethrough(bool s = true) noexcept { strikethrough = s; return *this; }
    Attrs& set_overline(bool o = true) noexcept { overline = o; return *this; }
    Attrs& set_decoration_style(DecorationStyle ds) noexcept { decoration_style = ds; return *this; }
    Attrs& set_decoration_mode(DecorationMode dm) noexcept { decoration_mode = dm; return *this; }
    Attrs& set_decoration_color(TextColor c) noexcept { decoration_color_opt = c; return *this; }
    Attrs& set_decoration_thickness(float t) noexcept { decoration_thickness = t; return *this; }
    Attrs& add_shadow(const TextShadow& shadow) { shadows.push_back(shadow); return *this; }
    Attrs& add_shadow(TextColor color, float dx, float dy, float blur_sigma = 0.0f) {
        shadows.push_back(TextShadow{color, dx, dy, blur_sigma});
        return *this;
    }
    Attrs& set_shadows(std::vector<TextShadow> list) { shadows = std::move(list); return *this; }
    Attrs& clear_shadows() noexcept { shadows.clear(); return *this; }
    Attrs& set_metadata(size_t meta) noexcept { metadata = meta; return *this; }

    [[nodiscard]] bool has_custom_metrics_or_decorations() const noexcept {
        return !font_families.empty() ||
               !locale.empty() ||
               !font_features.empty() ||
               !font_variations.empty() ||
               font_size_opt.has_value() ||
               letter_spacing != 0.0f ||
               word_spacing != 0.0f ||
               baseline_shift != 0.0f ||
               underline ||
               strikethrough ||
               overline ||
               decoration_style != DecorationStyle::Solid ||
               decoration_mode != DecorationMode::Through ||
               decoration_color_opt.has_value() ||
               decoration_thickness != 1.0f ||
               foreground_paint_opt != nullptr ||
               background_paint_opt != nullptr ||
               !shadows.empty() ||
               background_color_opt.has_value();
    }

    bool operator==(const Attrs& other) const noexcept = default;
};

/// Span associating byte range [start, end) with formatting attributes.
struct AttrsSpan {
    size_t start{0};
    size_t end{0};
    Attrs attrs{};

    bool operator==(const AttrsSpan& other) const noexcept = default;
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

    /// Modifies attributes across [start, end) while preserving existing orthogonal span properties
    /// and compacting superseded/overlapping spans to prevent unbounded growth during interactive edits.
    template <typename Fn>
    void apply_range_modifier(size_t start, size_t end, Fn&& modifier) {
        if (start >= end) return;
        std::vector<size_t> cuts;
        cuts.reserve(spans_.size() * 2 + 2);
        cuts.push_back(start);
        cuts.push_back(end);
        for (const auto& sp : spans_) {
            if (sp.start > start && sp.start < end) cuts.push_back(sp.start);
            if (sp.end > start && sp.end < end) cuts.push_back(sp.end);
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        std::vector<AttrsSpan> updates;
        updates.reserve(cuts.size());
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const size_t a = cuts[i];
            const size_t b = cuts[i + 1];
            if (a >= b) continue;
            Attrs updated = get(a);
            modifier(updated);
            updates.push_back(AttrsSpan{a, b, std::move(updated)});
        }

        // Trim or remove existing spans overlapped by [start, end) so repeated edits stay O(active_spans)
        std::vector<AttrsSpan> compacted;
        compacted.reserve(spans_.size() + updates.size());
        for (auto& sp : spans_) {
            if (sp.end <= start || sp.start >= end) {
                compacted.push_back(std::move(sp));
            } else if (sp.start < start && sp.end > end) {
                AttrsSpan right_part{end, sp.end, sp.attrs};
                sp.end = start;
                compacted.push_back(std::move(sp));
                compacted.push_back(std::move(right_part));
            } else if (sp.start < start) {
                sp.end = start;
                compacted.push_back(std::move(sp));
            } else if (sp.end > end) {
                sp.start = end;
                compacted.push_back(std::move(sp));
            }
            // else: sp is fully inside [start, end) and superseded by `updates`
        }
        for (auto& u : updates) {
            compacted.push_back(std::move(u));
        }
        spans_ = std::move(compacted);
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
