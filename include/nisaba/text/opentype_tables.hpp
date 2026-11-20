#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include <memory>
#include <string_view>
#include <optional>
#include "nisaba/text/attrs.hpp"

namespace nisaba::text {

/// Represents 2D glyph positioning adjustments according to OpenType GPOS ValueRecord.
struct GlyphPlacementAdjustment {
    int16_t x_placement{0};
    int16_t y_placement{0};
    int16_t x_advance{0};
    int16_t y_advance{0};
};

/// OpenType GDEF Glyph Class definitions.
enum class GlyphClass : uint16_t {
    Unclassified = 0,
    Base = 1,
    Ligature = 2,
    Mark = 3,
    Component = 4
};

/// Sovereign OpenType GDEF (Glyph Definition Table) parser.
/// Classifies glyphs into Base, Ligature, Mark, and Component for GSUB/GPOS LookupFlag filtering.
class OpenTypeGdef {
public:
    OpenTypeGdef();
    ~OpenTypeGdef();

    OpenTypeGdef(const OpenTypeGdef&) = delete;
    OpenTypeGdef& operator=(const OpenTypeGdef&) = delete;
    OpenTypeGdef(OpenTypeGdef&&) noexcept;
    OpenTypeGdef& operator=(OpenTypeGdef&&) noexcept;

    static std::unique_ptr<OpenTypeGdef> from_bytes(std::span<const uint8_t> data);

    [[nodiscard]] bool is_valid() const noexcept { return is_valid_; }
    [[nodiscard]] GlyphClass get_glyph_class(uint16_t glyph_id) const noexcept;
    [[nodiscard]] bool is_mark(uint16_t glyph_id) const noexcept {
        return get_glyph_class(glyph_id) == GlyphClass::Mark;
    }

private:
    std::span<const uint8_t> data_{};
    uint16_t glyph_class_def_offset_{0};
    uint16_t mark_attach_class_def_offset_{0};
    bool is_valid_{false};
};

/// Sovereign OpenType GSUB (Glyph Substitution Table) parser and executor.
/// Supports Single (Type 1), Multiple (Type 2), Alternate (Type 3), Ligature (Type 4),
/// Contextual (Type 5), and Chaining Contextual (Type 6) substitutions with GDEF filtering.
class OpenTypeGsub {
public:
    OpenTypeGsub();
    ~OpenTypeGsub();

    OpenTypeGsub(const OpenTypeGsub&) = delete;
    OpenTypeGsub& operator=(const OpenTypeGsub&) = delete;
    OpenTypeGsub(OpenTypeGsub&&) noexcept;
    OpenTypeGsub& operator=(OpenTypeGsub&&) noexcept;

    static std::unique_ptr<OpenTypeGsub> from_bytes(
        std::span<const uint8_t> data,
        const OpenTypeGdef* gdef = nullptr
    );

    [[nodiscard]] bool is_valid() const noexcept { return is_valid_; }

    /// Applies requested substitution features (e.g., 'liga', 'clig', 'dlig', 'rlig', 'isol', 'init', 'medi', 'fina')
    /// on a sequence of glyph IDs in-place.
    bool apply_features(std::vector<uint16_t>& glyphs, std::span<const uint32_t> feature_tags) const;

    /// Applies OpenType features with explicit integer values (0=off, >=1=on/alternate)
    /// filtered by OpenType ScriptList / LangSys (script_tag e.g. 'latn'/'arab', lang_tag e.g. 'ROM '/'URD ').
    bool apply_features_for_script(
        std::vector<uint16_t>& glyphs,
        std::span<const FontFeature> features,
        uint32_t script_tag = 0,
        uint32_t lang_tag = 0
    ) const;

    /// Maps a BCP-47 locale tag (e.g. "ar", "ur", "ro", "ja", "zh-Hant") to an OpenType 4-byte LangSys tag (or 0).
    static uint32_t bcp47_to_ot_lang_tag(std::string_view bcp47_locale) noexcept;

    /// Convenience wrapper applying standard ligature features ('rlig', 'calt', 'liga', 'clig').
    bool apply_ligatures(std::vector<uint16_t>& glyphs) const;

    /// Applies a single substitution feature on a single glyph ID if covered.
    uint16_t apply_single_glyph_feature(
        uint16_t glyph_id,
        uint32_t feature_tag,
        uint32_t script_tag = 0,
        uint32_t lang_tag = 0,
        uint32_t feature_value = 1
    ) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool is_valid_{false};
};

/// Sovereign OpenType GPOS (Glyph Positioning Table) parser and executor.
/// Supports Pair Adjustment (LookupType 2 Formats 1 & 2), Mark-to-Base (LookupType 4),
/// and Mark-to-Mark (LookupType 6) with zero external dependencies and O(log N) binary search.
class OpenTypeGpos {
public:
    OpenTypeGpos();
    ~OpenTypeGpos();

    OpenTypeGpos(const OpenTypeGpos&) = delete;
    OpenTypeGpos& operator=(const OpenTypeGpos&) = delete;
    OpenTypeGpos(OpenTypeGpos&&) noexcept;
    OpenTypeGpos& operator=(OpenTypeGpos&&) noexcept;

    static std::unique_ptr<OpenTypeGpos> from_bytes(std::span<const uint8_t> data);

    [[nodiscard]] bool is_valid() const noexcept { return is_valid_; }
    [[nodiscard]] bool has_kerning() const noexcept;
    [[nodiscard]] bool has_mark_positioning() const noexcept;

    /// Retrieves horizontal kerning advance adjustment in font design units between two glyphs.
    /// Returns true if a kerning pair adjustment was found in GPOS.
    bool get_kerning(uint16_t left_glyph, uint16_t right_glyph, int16_t& out_x_advance) const noexcept;

    /// Retrieves full pair positioning adjustments for both glyphs.
    bool get_pair_adjustment(
        uint16_t left_glyph,
        uint16_t right_glyph,
        GlyphPlacementAdjustment& out_first,
        GlyphPlacementAdjustment& out_second
    ) const noexcept;

    /// Retrieves mark-to-base anchor displacement in font design units.
    /// Returns true if a mark attachment anchor pair was found.
    bool get_mark_to_base_offset(
        uint16_t base_glyph,
        uint16_t mark_glyph,
        int16_t& out_dx,
        int16_t& out_dy
    ) const noexcept;

    /// Retrieves mark-to-mark anchor displacement in font design units (for stacked diacritics).
    /// Returns true if a mark-to-mark attachment anchor pair was found.
    bool get_mark_to_mark_offset(
        uint16_t base_mark_glyph,
        uint16_t attaching_mark_glyph,
        int16_t& out_dx,
        int16_t& out_dy
    ) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool is_valid_{false};
};

} // namespace nisaba::text
