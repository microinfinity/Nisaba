#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <span>
#include "nisaba/text/attrs.hpp"
#include "nisaba/text/ttf_font.hpp"
#include "nisaba/text/linux_font_config.hpp"

namespace nisaba::text {

/// Public descriptor exposing metadata for a registered or discovered font face.
struct FontFaceInfo {
    uint32_t id{0};
    std::string family_name{};
    std::string style_name{};
    std::string postscript_name{};
    std::string file_path{};
    uint32_t face_index{0};
    Weight weight{Weight::Normal};
    Style style{Style::Normal};
    Stretch stretch{Stretch::Normal};
    bool is_monospace{false};
    bool is_serif{false};
    bool is_color{false};
    bool is_variable{false};
    bool is_loaded{false};
};

/// Result of styled font matching including synthetic bold/italic flags.
struct FontMatchResult {
    uint32_t font_id{0};
    const TtfFont* font{nullptr};
    bool synthetic_bold{false};
    bool synthetic_italic{false};
};

/// Sovereign Font Management, System Font Discovery & Multilingual Fallback System.
/// Integrates deeply with Linux system fonts (/usr/share/fonts, ~/.local/share/fonts, /etc/fonts/fonts.conf)
/// using zero-dependency header indexing, persistent binary caching, and lazy mmap loading.
class FontSystem {
public:
    FontSystem();
    ~FontSystem();

    FontSystem(const FontSystem&) = delete;
    FontSystem& operator=(const FontSystem&) = delete;
    FontSystem(FontSystem&&) noexcept;
    FontSystem& operator=(FontSystem&&) noexcept;

    /// Factory constructor that automatically discovers and indexes system fonts on Linux.
    static FontSystem with_system_fonts(bool use_disk_cache = true);

    /// Scans standard system font directories and Fontconfig XML rules on Linux.
    /// Indexes all faces lazily without loading glyph outline tables until requested.
    /// Returns the number of newly registered font faces.
    size_t scan_system_fonts(bool use_disk_cache = true);

    /// Scans a specific directory for .ttf, .otf, .ttc, and .otc font files and indexes their headers.
    /// Returns the number of newly registered font faces.
    size_t scan_font_directory(std::string_view dir_path, bool recursive = true);

    /// Loads a TrueType/OpenType font (or specific face of a .ttc collection) eagerly from a file path.
    /// Returns unique font ID if successful.
    std::optional<uint32_t> load_font_file(std::string_view path, uint32_t face_index = 0);

    /// Loads all faces from a TrueType/OpenType Collection (.ttc/.otc) or single font file.
    std::vector<uint32_t> load_font_collection(std::string_view path);

    /// Loads a TrueType/OpenType font from a memory buffer. Returns unique font ID if successful.
    std::optional<uint32_t> load_font_data(std::span<const uint8_t> data, uint32_t face_index = 0);

    /// Retrieves a loaded font by its unique ID (lazily instantiating it from disk/mmap if indexed).
    const TtfFont* get_font(uint32_t font_id) const noexcept;

    /// Matches the closest font based on Family, Weight, Style, and Stretch.
    std::pair<uint32_t, const TtfFont*> match_font(
        const Family& family,
        Weight weight = Weight::Normal,
        Style style = Style::Normal,
        Stretch stretch = Stretch::Normal
    ) const noexcept;

    /// Matches the closest font and determines whether synthetic bold or italic rendering is needed.
    FontMatchResult match_font_styled(
        const Family& family,
        Weight weight = Weight::Normal,
        Style style = Style::Normal,
        Stretch stretch = Stretch::Normal
    ) const noexcept;

    /// Searches for a glyph representing the codepoint, starting with the primary font,
    /// and falling back via OS/2 Unicode Range bitmasks and system alias chains.
    std::pair<uint32_t, uint16_t> find_glyph_or_fallback(
        uint32_t primary_font_id,
        char32_t codepoint
    ) const noexcept;

    /// Returns the total number of registered/indexed font faces.
    size_t font_count() const noexcept { return faces_.size(); }

    /// Returns the number of font faces currently instantiated in memory.
    size_t loaded_font_count() const noexcept;

    /// Lists all unique font family names registered in the system.
    std::vector<std::string> list_families() const;

    /// Lists metadata for all registered faces belonging to a given family name.
    std::vector<FontFaceInfo> list_faces_for_family(std::string_view family_name) const;

    /// Retrieves metadata for a specific font ID without forcing lazy loading.
    std::optional<FontFaceInfo> get_face_info(uint32_t font_id) const noexcept;

    /// Sets the active BCP-47 language/locale tag (e.g. "ar", "en", "ja", "zh-CN") for font matching.
    void set_locale(std::string_view bcp47_locale);
    [[nodiscard]] std::string_view locale() const noexcept { return linux_config_.locale(); }

    /// Accesses the underlying Linux Fontconfig rule engine.
    [[nodiscard]] const LinuxFontConfig& linux_config() const noexcept { return linux_config_; }
    [[nodiscard]] LinuxFontConfig& linux_config_mut() noexcept { return linux_config_; }

    /// Sets the default font ID to use when no family is specified.
    void set_default_font(uint32_t font_id) noexcept { default_font_id_ = font_id; }
    std::optional<uint32_t> default_font_id() const noexcept;

    /// Controls whether system fonts are automatically discovered on demand when no custom fonts were explicitly loaded (default: true).
    void set_auto_system_fonts(bool enabled) noexcept { auto_system_fonts_enabled_ = enabled; }
    [[nodiscard]] bool auto_system_fonts_enabled() const noexcept { return auto_system_fonts_enabled_; }
    [[nodiscard]] bool has_custom_fonts() const noexcept { return has_custom_fonts_; }

    /// Maps a Unicode codepoint to its OpenType OS/2 ulUnicodeRange bit index (0..127, or 0xFF if unmapped).
    static uint8_t codepoint_to_os2_range_bit(char32_t cp) noexcept;

private:
    struct FaceRecord {
        uint32_t id{0};
        std::string family_name{"Unknown"};
        std::string family_key{"unknown"};
        std::string style_name{"Regular"};
        std::string postscript_name{};
        std::string file_path{};
        uint32_t face_index{0};
        Weight weight{Weight::Normal};
        Style style{Style::Normal};
        Stretch stretch{Stretch::Normal};
        bool is_monospace{false};
        bool is_serif{false};
        bool is_color{false};
        bool is_variable{false};
        uint32_t unicode_ranges[4]{0, 0, 0, 0};

        mutable std::unique_ptr<TtfFont> font{nullptr};
        mutable bool load_failed{false};

        [[nodiscard]] bool has_unicode_bit(uint8_t bit) const noexcept {
            if (bit >= 128) return true;
            return (unicode_ranges[bit >> 5] & (1u << (bit & 31))) != 0;
        }

        [[nodiscard]] bool has_any_unicode_range() const noexcept {
            return (unicode_ranges[0] | unicode_ranges[1] | unicode_ranges[2] | unicode_ranges[3]) != 0;
        }
    };

    const TtfFont* ensure_face_loaded(const FaceRecord& rec) const noexcept;
    void ensure_default_system_fonts() const noexcept;
    size_t scan_font_directory_internal(std::string_view dir_path, bool recursive);
    const FaceRecord* find_face_record(uint32_t font_id) const noexcept;
    const FaceRecord* score_faces_in_family(
        const std::vector<size_t>& face_indices,
        Weight target_weight,
        Style target_style,
        Stretch target_stretch
    ) const noexcept;
    uint32_t register_probed_face(std::string_view file_path, const FontFaceHeaderInfo& info);
    void rebuild_lookup_tables();

    bool try_load_disk_cache(const std::vector<std::string>& dirs);
    void save_disk_cache(const std::vector<std::string>& dirs) const;

    std::vector<FaceRecord> faces_{};
    std::unordered_map<std::string, std::vector<size_t>> family_to_faces_{};
    mutable std::unordered_map<std::string, std::weak_ptr<MappedFontData>> mmap_pool_{};

    // Fast 256-entry fallback cache indexed by ((primary_font_id * 31) ^ (codepoint >> 8)) & 255
    struct FallbackPageSlot {
        uint32_t primary_id{0xFFFFFFFFu};
        uint32_t page{0xFFFFFFFFu};
        uint32_t matched_font_id{0};
    };
    static constexpr size_t FALLBACK_CACHE_SIZE = 256;
    mutable FallbackPageSlot fallback_page_cache_[FALLBACK_CACHE_SIZE]{};

    LinuxFontConfig linux_config_{};
    bool system_config_loaded_{false};
    bool auto_system_fonts_enabled_{true};
    bool has_custom_fonts_{false};
    mutable bool auto_system_scan_attempted_{false};
    uint32_t next_id_{1};
    std::optional<uint32_t> default_font_id_{std::nullopt};
};

} // namespace nisaba::text
