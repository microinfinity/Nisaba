#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <atomic>
#include <mutex>
#include "nisaba/path/path.hpp"
#include "nisaba/text/attrs.hpp"
#include "nisaba/text/cff_font.hpp"
#include "nisaba/text/opentype_tables.hpp"

namespace nisaba::text {

/// Represents a single colored vector layer in an OpenType COLR/CPAL color glyph.
struct ColorGlyphLayer {
    uint16_t glyph_id{0};
    TextColor color{255, 255, 255, 255};
    bool use_foreground{false}; /// True if palette index is 0xFFFF (use current text color)
};

/// Represents a design variation axis from the OpenType 'fvar' table.
struct VariationAxis {
    uint32_t tag{0};
    float min_value{0.0f};
    float default_value{0.0f};
    float max_value{0.0f};
};

/// Lightweight font face metadata extracted from SFNT headers without parsing glyph outlines.
struct FontFaceHeaderInfo {
    uint32_t face_index{0};
    std::string family_name{"Unknown"};
    std::string style_name{"Regular"};
    std::string postscript_name{};
    Weight weight{Weight::Normal};
    Style style{Style::Normal};
    Stretch stretch{Stretch::Normal};
    bool is_monospace{false};
    bool is_serif{false};
    bool is_color{false};
    bool is_variable{false};
    uint32_t unicode_ranges[4]{0, 0, 0, 0}; /// 128-bit OS/2 ulUnicodeRange1..4 bitmask
};

/// Zero-copy memory-mapped font file buffer (uses POSIX mmap on Linux/Unix, fallback on others).
class MappedFontData {
public:
    MappedFontData() noexcept = default;
    ~MappedFontData();

    MappedFontData(const MappedFontData&) = delete;
    MappedFontData& operator=(const MappedFontData&) = delete;
    MappedFontData(MappedFontData&& other) noexcept;
    MappedFontData& operator=(MappedFontData&& other) noexcept;

    static std::shared_ptr<MappedFontData> open_file(std::string_view file_path);

    [[nodiscard]] bool is_valid() const noexcept { return !span_.empty(); }
    [[nodiscard]] std::span<const uint8_t> span() const noexcept { return span_; }
    [[nodiscard]] size_t size() const noexcept { return span_.size(); }

private:
    void* mapped_ptr_{nullptr};
    size_t mapped_size_{0};
    std::vector<uint8_t> fallback_buffer_{};
    std::span<const uint8_t> span_{};
};

/// Sovereign binary TrueType / OpenType font reader.
/// Completely independent with zero external dependencies.
class TtfFont {
public:
    TtfFont();
    ~TtfFont();

    TtfFont(const TtfFont&) = delete;
    TtfFont& operator=(const TtfFont&) = delete;
    TtfFont(TtfFont&&) noexcept;
    TtfFont& operator=(TtfFont&&) noexcept;

    /// Loads and parses a font from a memory buffer.
    /// If copy_data is true, the font takes an owned copy of the buffer.
    /// If copy_data is false, it borrows the buffer (caller must ensure data outlives the font).
    /// Supports both single SFNT (.ttf/.otf) and TrueType Collections (.ttc/.otc) via face_index.
    static std::unique_ptr<TtfFont> from_bytes(
        std::span<const uint8_t> data,
        bool copy_data = false,
        uint32_t face_index = 0
    );
    static std::unique_ptr<TtfFont> from_file(std::string_view file_path, uint32_t face_index = 0);
    static std::unique_ptr<TtfFont> from_mapped(
        std::shared_ptr<MappedFontData> mapped,
        uint32_t face_index = 0
    );

    /// Returns the number of font faces in a font buffer (1 for .ttf/.otf, N for .ttc/.otc, 0 if invalid).
    static uint32_t collection_face_count(std::span<const uint8_t> data) noexcept;

    /// Fast header-only probe extracting family, style, weight, stretch, monospace, and OS/2 Unicode ranges
    /// without parsing glyph outlines or GSUB/GPOS tables.
    static bool probe_face_header(
        std::span<const uint8_t> data,
        uint32_t face_index,
        FontFaceHeaderInfo& out_info
    );

    bool is_valid() const noexcept { return is_valid_; }

    // --- Font Metadata ---
    std::string_view family_name() const noexcept { return family_name_; }
    std::string_view style_name() const noexcept { return style_name_; }
    std::string_view postscript_name() const noexcept { return postscript_name_; }
    Weight weight() const noexcept { return weight_; }
    Style style() const noexcept { return style_; }
    Stretch stretch() const noexcept { return stretch_; }
    bool is_monospace() const noexcept { return is_monospace_; }
    bool is_serif() const noexcept { return is_serif_; }
    uint32_t face_index() const noexcept { return face_index_; }
    std::span<const uint32_t, 4> unicode_ranges() const noexcept { return unicode_ranges_; }
    bool supports_unicode_range_bit(uint8_t bit) const noexcept {
        if (bit >= 128) return false;
        return (unicode_ranges_[bit >> 5] & (1u << (bit & 31))) != 0;
    }

    uint16_t units_per_em() const noexcept { return units_per_em_; }
    int16_t ascent() const noexcept { return ascent_; }
    int16_t descent() const noexcept { return descent_; }
    int16_t line_gap() const noexcept { return line_gap_; }
    uint16_t num_glyphs() const noexcept { return num_glyphs_; }

    // --- Metrics at given pixel size ---
    float scale_for_size(float font_size) const noexcept {
        return (units_per_em_ > 0) ? (font_size / static_cast<float>(units_per_em_)) : 0.0f;
    }
    float ascent(float font_size) const noexcept {
        return static_cast<float>(ascent_) * scale_for_size(font_size);
    }
    float descent(float font_size) const noexcept {
        return static_cast<float>(descent_) * scale_for_size(font_size);
    }
    float line_gap(float font_size) const noexcept {
        return static_cast<float>(line_gap_) * scale_for_size(font_size);
    }
    float line_height(float font_size) const noexcept {
        return static_cast<float>(ascent_ - descent_ + line_gap_) * scale_for_size(font_size);
    }

    // --- Glyph Mapping & Metrics ---
    /// Maps a 32-bit Unicode codepoint to a glyph index (0 if not found).
    inline uint16_t glyph_index(char32_t codepoint) const noexcept {
        if (codepoint < 128) [[likely]] {
            return ascii_lut_[codepoint];
        }
        return lookup_glyph_raw(codepoint);
    }

    /// Maps a Unicode codepoint and Variation Selector (cmap Format 14) to a glyph index.
    uint16_t glyph_index_variation(char32_t codepoint, char32_t variation_selector) const noexcept;

    /// Fast O(1) ASCII glyph index lookup
    inline uint16_t ascii_glyph_index(uint8_t c) const noexcept {
        return (c < 128) ? ascii_lut_[c] : 0;
    }

    /// Fast O(1) ASCII horizontal advance in font design units
    inline int16_t ascii_advance(uint8_t c) const noexcept {
        return (c < 128) ? ascii_adv_lut_[c] : 0;
    }

    /// Retrieves horizontal advance in unscaled font design units.
    inline int16_t glyph_advance_units(uint16_t glyph_id) const noexcept {
        if (!hmtx_table_ || glyph_id >= num_glyphs_) [[unlikely]] return 0;
        if (glyph_id < num_h_metrics_) [[likely]] {
            const uint8_t* p = hmtx_table_ + (static_cast<size_t>(glyph_id) * 4);
            return static_cast<int16_t>((p[0] << 8) | p[1]);
        } else {
            const uint8_t* p = hmtx_table_ + (static_cast<size_t>(num_h_metrics_ - 1) * 4);
            return static_cast<int16_t>((p[0] << 8) | p[1]);
        }
    }

    /// Retrieves horizontal advance using a precalculated scale factor.
    inline float glyph_advance_scaled(uint16_t glyph_id, float scale) const noexcept {
        return static_cast<float>(glyph_advance_units(glyph_id)) * scale;
    }

    /// Retrieves horizontal advance and left side bearing for a glyph in font design units.
    inline void get_glyph_metrics(uint16_t glyph_id, int16_t* advance_width, int16_t* lsb) const noexcept {
        if (!hmtx_table_ || glyph_id >= num_glyphs_) [[unlikely]] {
            if (advance_width) *advance_width = 0;
            if (lsb) *lsb = 0;
            return;
        }

        if (glyph_id < num_h_metrics_) [[likely]] {
            const uint8_t* p = hmtx_table_ + (static_cast<size_t>(glyph_id) * 4);
            if (advance_width) {
                *advance_width = static_cast<int16_t>((p[0] << 8) | p[1]);
            }
            if (lsb) {
                *lsb = static_cast<int16_t>((p[2] << 8) | p[3]);
            }
        } else {
            if (advance_width) {
                const uint8_t* p = hmtx_table_ + (static_cast<size_t>(num_h_metrics_ - 1) * 4);
                *advance_width = static_cast<int16_t>((p[0] << 8) | p[1]);
            }
            if (lsb) {
                const uint8_t* p = hmtx_table_ + (static_cast<size_t>(num_h_metrics_) * 4) +
                                   (static_cast<size_t>(glyph_id - num_h_metrics_) * 2);
                *lsb = static_cast<int16_t>((p[0] << 8) | p[1]);
            }
        }
    }

    /// Retrieves horizontal advance at a given font pixel size.
    float glyph_advance(uint16_t glyph_id, float font_size) const noexcept {
        return glyph_advance_scaled(glyph_id, scale_for_size(font_size));
    }

    /// Retrieves kerning between two glyphs in font design units.
    /// Checks O(1) direct-mapped cache first, then OpenType GPOS PairPos and legacy 'kern' table.
    inline int16_t get_kerning(uint16_t left_glyph, uint16_t right_glyph) const noexcept {
        if (!has_kerning_) [[unlikely]] return 0;
        uint32_t key = (static_cast<uint32_t>(left_glyph) << 16) | static_cast<uint32_t>(right_glyph);
        if (!kern_fast_cache_.empty()) [[likely]] {
            size_t slot_idx = ((key * 2654435761u) >> 21) & (KERN_CACHE_SIZE - 1);
            const auto& slot = kern_fast_cache_[slot_idx];
            if (slot.key == key) [[likely]] {
                return slot.value;
            }
        }
        return compute_kerning_slow(left_glyph, right_glyph);
    }

    [[nodiscard]] bool has_kerning() const noexcept { return has_kerning_; }

    // --- OpenType & PostScript Features ---
    [[nodiscard]] bool is_cff() const noexcept { return is_cff_; }
    [[nodiscard]] const CffFont* cff() const noexcept { return cff_.get(); }
    [[nodiscard]] const OpenTypeGdef* gdef() const noexcept { return gdef_.get(); }
    [[nodiscard]] const OpenTypeGsub* gsub() const noexcept { return gsub_.get(); }
    [[nodiscard]] const OpenTypeGpos* gpos() const noexcept { return gpos_.get(); }

    /// Applies OpenType GSUB ligatures ('liga', 'clig', etc.) on a sequence of glyph IDs in-place.
    bool apply_ligatures(std::vector<uint16_t>& glyphs) const;

    /// Applies specific OpenType GSUB features.
    bool apply_gsub(std::vector<uint16_t>& glyphs, std::span<const uint32_t> feature_tags) const;

    // --- Color Glyphs (COLR / CPAL) & Variable Axes (fvar) ---
    [[nodiscard]] bool has_color_glyphs() const noexcept { return colr_offset_ != 0 && cpal_offset_ != 0; }
    bool get_color_glyph_layers(
        uint16_t glyph_id,
        std::vector<ColorGlyphLayer>& out_layers,
        uint16_t palette_index = 0
    ) const;

    [[nodiscard]] std::span<const VariationAxis> variation_axes() const noexcept { return variation_axes_; }

    // --- Vector Outline Extraction ---
    /// Extracts the vector outline for a glyph, scaled to the requested font pixel size.
    /// Supports both TrueType quadratic outlines ('glyf') and CFF / CFF2 PostScript cubic outlines ('CFF ').
    /// Y coordinates are flipped to match screen space (downwards positive).
    bool get_glyph_path(uint16_t glyph_id, Path& out_path, float font_size) const;

    /// Extracts the vector outline for a glyph with optional synthetic bold and synthetic italic styling.
    bool get_glyph_path_styled(
        uint16_t glyph_id,
        Path& out_path,
        float font_size,
        bool synthetic_bold = false,
        bool synthetic_italic = false
    ) const;

private:
    struct TableRecord {
        uint32_t tag{0};
        uint32_t offset{0};
        uint32_t length{0};
    };

    const TableRecord* find_table(uint32_t tag) const noexcept;

    bool parse_internal(std::span<const uint8_t> data, uint32_t face_index = 0);
    bool parse_head();
    bool parse_maxp();
    bool parse_hhea();
    bool parse_hmtx();
    bool parse_cmap();
    bool parse_name();
    bool parse_kern();
    bool parse_os2();
    bool parse_post();
    bool parse_fvar();

    bool extract_simple_glyph(const uint8_t* g_data, Path& path, float scale) const;
    bool extract_composite_glyph(const uint8_t* g_data, Path& path, float scale, int recursion_depth) const;

    uint16_t lookup_glyph_raw(char32_t codepoint) const noexcept;
    int16_t compute_kerning_slow(uint16_t left_glyph, uint16_t right_glyph) const noexcept;

    std::shared_ptr<MappedFontData> mapped_data_{nullptr};
    std::vector<uint8_t> owned_data_{};
    std::span<const uint8_t> data_{};
    uint32_t face_index_{0};
    bool is_valid_{false};

    // Flat zero-allocation table directory (up to 48 SFNT tables)
    static constexpr size_t MAX_SFNT_TABLES = 48;
    TableRecord tables_[MAX_SFNT_TABLES]{};
    uint16_t num_tables_{0};

    std::string family_name_{"Unknown"};
    std::string style_name_{"Regular"};
    std::string postscript_name_{};
    Weight weight_{Weight::Normal};
    Style style_{Style::Normal};
    Stretch stretch_{Stretch::Normal};
    bool is_monospace_{false};
    bool is_serif_{false};
    uint32_t unicode_ranges_[4]{0, 0, 0, 0};

    uint16_t units_per_em_{1000};
    int16_t index_to_loc_format_{0}; // 0 = 16-bit, 1 = 32-bit
    uint16_t num_glyphs_{0};
    int16_t ascent_{800};
    int16_t descent_{-200};
    int16_t line_gap_{200};
    uint16_t num_h_metrics_{0};

    // Table offsets & direct table pointers
    uint32_t loca_offset_{0};
    const uint8_t* loca_table_{nullptr};
    uint32_t glyf_offset_{0};
    const uint8_t* glyf_table_{nullptr};
    uint32_t hmtx_offset_{0};
    const uint8_t* hmtx_table_{nullptr};
    uint32_t kern_offset_{0};
    uint32_t colr_offset_{0};
    uint32_t colr_length_{0};
    uint32_t cpal_offset_{0};
    uint32_t cpal_length_{0};

    // Cmap subtable info & pre-cached offsets
    uint32_t cmap_format4_offset_{0};
    uint32_t cmap_format12_offset_{0};
    uint32_t cmap_format14_offset_{0};
    uint32_t cmap_format14_length_{0};
    const uint8_t* cmap_format12_groups_{nullptr};
    uint32_t cmap_format12_num_groups_{0};
    const uint8_t* cmap_format4_end_codes_{nullptr};
    const uint8_t* cmap_format4_start_codes_{nullptr};
    const uint8_t* cmap_format4_id_deltas_{nullptr};
    const uint8_t* cmap_format4_id_range_offsets_{nullptr};
    uint16_t cmap_format4_seg_count_{0};
    uint16_t cmap_format4_search_range_{0};
    uint16_t cmap_format4_entry_selector_{0};
    uint16_t cmap_format4_range_shift_{0};

    // Fast O(1) ASCII direct lookup tables (codepoints 0 to 127)
    uint16_t ascii_lut_[128]{0};
    int16_t ascii_adv_lut_[128]{0};

    // Sorted legacy 'kern' table pairs: (left << 16 | right) -> kern_value
    struct KernPair {
        uint32_t key{0};
        int16_t value{0};
    };
    std::vector<KernPair> kerning_pairs_{};
    bool has_kerning_{false};

    // Fast O(1) direct-mapped kerning cache (lazily allocated on first get_kerning query)
    static constexpr size_t KERN_CACHE_SIZE = 2048;
    struct KernCacheSlot {
        uint32_t key{0xFFFFFFFFu};
        int16_t value{0};
    };
    mutable std::vector<KernCacheSlot> kern_fast_cache_{};

    // Fast O(1) unscaled glyph outline path cache
    struct CachedPathEntry {
        Path path{};
        std::atomic<bool> valid{false};

        CachedPathEntry() = default;
        CachedPathEntry(const CachedPathEntry& o) : path(o.path), valid(o.valid.load()) {}
        CachedPathEntry& operator=(const CachedPathEntry& o) {
            path = o.path;
            valid.store(o.valid.load());
            return *this;
        }
    };
    mutable std::vector<CachedPathEntry> glyph_path_cache_{};
    mutable std::unique_ptr<std::recursive_mutex> cache_mutex_{nullptr};

    // OpenType & CFF extensions
    bool is_cff_{false};
    std::unique_ptr<CffFont> cff_{nullptr};
    std::unique_ptr<OpenTypeGdef> gdef_{nullptr};
    std::unique_ptr<OpenTypeGsub> gsub_{nullptr};
    std::unique_ptr<OpenTypeGpos> gpos_{nullptr};
    std::vector<VariationAxis> variation_axes_{};
};

/// Standard font alias for TtfFont in Nisaba typography subsystem.
using Font = TtfFont;

} // namespace nisaba::text
