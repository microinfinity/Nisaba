#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include "nisaba/color/color.hpp"

namespace nisaba::markdown {

/// Structure representing a parsed HTML tag.
struct HtmlTag {
    std::string name{};                     // Tag name in lowercase (e.g. "kbd", "details", "span")
    bool is_closing{false};                 // </tag>
    bool is_self_closing{false};            // <tag/> or void element like <br>, <hr>, <img>
    std::unordered_map<std::string, std::string> attributes{};
    size_t raw_length{0};                   // Total byte length of the tag string <...>

    [[nodiscard]] std::string get_attr(const std::string& key) const {
        auto it = attributes.find(key);
        if (it != attributes.end()) return it->second;
        return "";
    }
};

/// High-performance, zero-dependency HTML parser and sanitizer for Markdown documents.
class HtmlParser {
public:
    /// Check if character at `pos` begins a valid HTML tag or comment.
    static bool is_tag_at(std::string_view text, size_t pos);

    /// Parse an HTML tag starting at `pos`. Returns nullopt if not a valid tag.
    static std::optional<HtmlTag> parse_tag(std::string_view text, size_t pos);

    /// Check if a tag name is safe for rendering (sanitization whitelist).
    static bool is_safe_tag(std::string_view tag_name);

    /// Parse a CSS or HTML color string (#rgb, #rrggbb, rgb(...), named colors).
    static std::optional<Color> parse_color(std::string_view color_str);

    /// Parse inline CSS style string (e.g. "color: #ff0000; background: yellow;") into key-value map.
    static std::unordered_map<std::string, std::string> parse_style(std::string_view style_attr);

    /// Checks if a line begins a block-level HTML element (e.g. <details>, <div, <p, <!--).
    static bool is_block_html_line(std::string_view line);

    /// Returns true if `tag_name` is disallowed by the GFM Tagfilter extension
    /// (title, textarea, style, xmp, iframe, noembed, noframes, script, plaintext).
    static bool is_tagfilter_disallowed(std::string_view tag_name);

    /// Neutralizes GFM Tagfilter disallowed raw HTML tags by replacing their leading '<' with "&lt;".
    [[nodiscard]] static std::string apply_tagfilter(std::string_view html);
};

} // namespace nisaba::markdown
