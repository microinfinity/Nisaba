#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include "nisaba/text/attrs.hpp"

namespace nisaba::text {

/// Sovereign Zero-Dependency Linux Fontconfig XML & System Font Environment Parser.
/// Parses /etc/fonts/fonts.conf, /etc/fonts/conf.d/*.conf, /etc/fonts/local.conf,
/// and user fontconfig files without linking to libfontconfig or libexpat.
class LinuxFontConfig {
public:
    LinuxFontConfig() = default;
    ~LinuxFontConfig() = default;

    /// Loads and parses standard system and user fontconfig XML configuration files on Linux.
    /// Automatically detects system locale from $LC_ALL, $LC_CTYPE, or $LANG if locale is empty.
    bool load_system_config(std::string_view locale_override = "");

    /// Parses a single fontconfig XML file (and recursively resolves <include> directives).
    bool parse_config_file(std::string_view path, int depth = 0);

    /// Parses raw fontconfig XML content in-memory.
    bool parse_xml_content(std::string_view xml_content, std::string_view base_dir = "", int depth = 0);

    /// Sets the active BCP-47 language/locale tag (e.g. "ar", "en", "zh-CN", "ja") for language-specific rules.
    void set_locale(std::string_view locale);
    [[nodiscard]] std::string_view locale() const noexcept { return locale_; }

    /// Returns the ordered list of existing font directories discovered from config and XDG environment.
    [[nodiscard]] const std::vector<std::string>& font_directories() const noexcept { return font_dirs_; }

    /// Returns reject glob patterns (from <selectfont><rejectfont><glob>).
    [[nodiscard]] const std::vector<std::string>& reject_globs() const noexcept { return reject_globs_; }

    /// Checks if a file path or filename matches any reject glob.
    [[nodiscard]] bool is_rejected_file(std::string_view file_path) const noexcept;

    /// Resolves an ordered candidate list of concrete font family names for a requested Family.
    /// Expands generic families (sans-serif, serif, monospace, system-ui, emoji, math)
    /// as well as metric-compatible aliases (e.g. Arial -> Liberation Sans / Noto Sans).
    [[nodiscard]] std::vector<std::string> resolve_family_candidates(const Family& requested) const;

    /// Resolves an ordered candidate list of concrete font family names for a family name string.
    [[nodiscard]] std::vector<std::string> resolve_family_name(std::string_view family_name) const;

    /// Maps a specific font family to its generic category if declared in config (e.g. "Nazli" -> "serif").
    [[nodiscard]] std::string get_default_generic_for_family(std::string_view family_name) const;

    /// Normalizes a family name to lowercase trimmed key for case-insensitive lookup.
    static std::string normalize_key(std::string_view name);

    /// Detects the primary system language tag from environment variables ($LC_ALL, $LC_CTYPE, $LANG).
    static std::string detect_system_locale();

private:
    struct AliasRule {
        std::vector<std::string> prefer{};
        std::vector<std::string> accept{};
        std::vector<std::string> defaults{};
    };

    struct LangOverrideRule {
        std::string lang_pattern{};
        std::string target_family_key{};
        std::vector<std::string> prepend_families{};
    };

    void ensure_standard_fallback_defaults();
    void add_directory_resolved(std::string_view raw_dir, std::string_view prefix_attr, std::string_view base_dir);
    void merge_alias(
        std::string_view family,
        const std::vector<std::string>& prefer,
        const std::vector<std::string>& accept,
        const std::vector<std::string>& def
    );

    std::string locale_{"en"};
    std::vector<std::string> font_dirs_{};
    std::vector<std::string> reject_globs_{};
    std::unordered_map<std::string, AliasRule> aliases_{};
    std::vector<LangOverrideRule> lang_overrides_{};
};

} // namespace nisaba::text
