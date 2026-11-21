#include "nisaba/markdown/html_parser.hpp"
#include <cctype>
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace nisaba::markdown {

namespace {

inline std::string to_lower(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

uint8_t hex_to_byte(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0;
}

} // namespace

bool HtmlParser::is_tag_at(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '<') return false;
    if (pos + 1 >= text.size()) return false;

    // Check for comment <!--
    if (text.substr(pos).starts_with("<!--")) return true;

    char next = text[pos + 1];
    if (next == '/' && pos + 2 < text.size() && std::isalpha(static_cast<unsigned char>(text[pos + 2]))) {
        return true;
    }
    return std::isalpha(static_cast<unsigned char>(next)) != 0;
}

std::optional<HtmlTag> HtmlParser::parse_tag(std::string_view text, size_t pos) {
    if (!is_tag_at(text, pos)) return std::nullopt;

    // Handle HTML comments <!-- ... -->
    if (text.substr(pos).starts_with("<!--")) {
        size_t end = text.find("-->", pos + 4);
        if (end == std::string_view::npos) return std::nullopt;
        HtmlTag tag;
        tag.name = "!--";
        tag.raw_length = (end + 3) - pos;
        return tag;
    }

    size_t i = pos + 1;
    bool is_closing = false;
    if (i < text.size() && text[i] == '/') {
        is_closing = true;
        i++;
    }

    // Extract tag name
    size_t name_start = i;
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '-' || text[i] == '_')) {
        i++;
    }

    if (name_start == i) return std::nullopt;
    std::string tag_name = to_lower(text.substr(name_start, i - name_start));

    HtmlTag tag;
    tag.name = tag_name;
    tag.is_closing = is_closing;

    // Void / self-closing tags by HTML5 standard
    static const std::unordered_set<std::string> void_tags = {
        "br", "hr", "img", "input", "meta", "link", "wbr"
    };
    if (void_tags.find(tag_name) != void_tags.end()) {
        tag.is_self_closing = true;
    }

    // Parse attributes
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        if (i >= text.size()) break;

        if (text[i] == '>') {
            i++; // End of tag
            tag.raw_length = i - pos;
            return tag;
        }

        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '>') {
            tag.is_self_closing = true;
            i += 2;
            tag.raw_length = i - pos;
            return tag;
        }

        // Attribute key
        size_t key_start = i;
        while (i < text.size() && text[i] != '=' && text[i] != '>' && text[i] != '/' && !std::isspace(static_cast<unsigned char>(text[i]))) {
            i++;
        }
        std::string attr_key = to_lower(text.substr(key_start, i - key_start));
        if (attr_key.empty()) {
            i++;
            continue;
        }

        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;

        std::string attr_val;
        if (i < text.size() && text[i] == '=') {
            i++; // skip '='
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;

            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                char quote = text[i++];
                size_t val_start = i;
                while (i < text.size() && text[i] != quote) i++;
                attr_val = std::string(text.substr(val_start, i - val_start));
                if (i < text.size() && text[i] == quote) i++;
            } else {
                size_t val_start = i;
                while (i < text.size() && text[i] != '>' && text[i] != '/' && !std::isspace(static_cast<unsigned char>(text[i]))) {
                    i++;
                }
                attr_val = std::string(text.substr(val_start, i - val_start));
            }
        }

        tag.attributes[attr_key] = attr_val;
    }

    return std::nullopt;
}

bool HtmlParser::is_safe_tag(std::string_view tag_name) {
    static const std::unordered_set<std::string> safe_tags = {
        "kbd", "sub", "sup", "b", "strong", "i", "em", "u", "ins",
        "s", "del", "strike", "code", "mark", "span", "font", "a",
        "img", "br", "hr", "details", "summary", "div", "p", "blockquote",
        "table", "thead", "tbody", "tr", "th", "td", "pre", "h1", "h2",
        "h3", "h4", "h5", "h6", "ul", "ol", "li", "!--"
    };
    return safe_tags.find(to_lower(tag_name)) != safe_tags.end();
}

std::optional<Color> HtmlParser::parse_color(std::string_view color_str) {
    auto s = trim(color_str);
    if (s.empty()) return std::nullopt;

    // Named colors
    std::string lower = to_lower(s);
    if (lower == "black") return Color::from_rgba8(0, 0, 0, 255);
    if (lower == "white") return Color::from_rgba8(255, 255, 255, 255);
    if (lower == "red") return Color::from_rgba8(255, 0, 0, 255);
    if (lower == "green") return Color::from_rgba8(0, 180, 0, 255);
    if (lower == "blue") return Color::from_rgba8(0, 120, 255, 255);
    if (lower == "yellow") return Color::from_rgba8(255, 220, 0, 255);
    if (lower == "cyan") return Color::from_rgba8(0, 230, 255, 255);
    if (lower == "magenta") return Color::from_rgba8(255, 0, 255, 255);
    if (lower == "orange") return Color::from_rgba8(255, 140, 0, 255);
    if (lower == "purple") return Color::from_rgba8(160, 32, 240, 255);
    if (lower == "gray" || lower == "grey") return Color::from_rgba8(128, 128, 128, 255);
    if (lower == "transparent") return Color::from_rgba8(0, 0, 0, 0);

    // Hex colors
    if (s.front() == '#') {
        s.remove_prefix(1);
        if (s.size() == 3) {
            uint8_t r = hex_to_byte(s[0]) * 17;
            uint8_t g = hex_to_byte(s[1]) * 17;
            uint8_t b = hex_to_byte(s[2]) * 17;
            return Color::from_rgba8(r, g, b, 255);
        }
        if (s.size() == 4) {
            uint8_t r = hex_to_byte(s[0]) * 17;
            uint8_t g = hex_to_byte(s[1]) * 17;
            uint8_t b = hex_to_byte(s[2]) * 17;
            uint8_t a = hex_to_byte(s[3]) * 17;
            return Color::from_rgba8(r, g, b, a);
        }
        if (s.size() == 6) {
            uint8_t r = (hex_to_byte(s[0]) << 4) | hex_to_byte(s[1]);
            uint8_t g = (hex_to_byte(s[2]) << 4) | hex_to_byte(s[3]);
            uint8_t b = (hex_to_byte(s[4]) << 4) | hex_to_byte(s[5]);
            return Color::from_rgba8(r, g, b, 255);
        }
        if (s.size() == 8) {
            uint8_t r = (hex_to_byte(s[0]) << 4) | hex_to_byte(s[1]);
            uint8_t g = (hex_to_byte(s[2]) << 4) | hex_to_byte(s[3]);
            uint8_t b = (hex_to_byte(s[4]) << 4) | hex_to_byte(s[5]);
            uint8_t a = (hex_to_byte(s[6]) << 4) | hex_to_byte(s[7]);
            return Color::from_rgba8(r, g, b, a);
        }
    }

    // rgb(...) / rgba(...)
    if (lower.starts_with("rgb(") || lower.starts_with("rgba(")) {
        size_t start = lower.find('(') + 1;
        size_t end = lower.find(')');
        if (end != std::string_view::npos) {
            auto inner = lower.substr(start, end - start);
            int r = 0, g = 0, b = 0;
            float a = 1.0f;
            std::string str(inner);
            std::replace(str.begin(), str.end(), ',', ' ');
            std::stringstream ss(str);
            if (ss >> r >> g >> b) {
                if (ss >> a) {
                    if (a > 1.0f) a = a / 255.0f;
                }
                return Color::from_rgba8(
                    static_cast<uint8_t>(std::clamp(r, 0, 255)),
                    static_cast<uint8_t>(std::clamp(g, 0, 255)),
                    static_cast<uint8_t>(std::clamp(b, 0, 255)),
                    static_cast<uint8_t>(std::clamp(static_cast<int>(a * 255.0f), 0, 255))
                );
            }
        }
    }

    return std::nullopt;
}

std::unordered_map<std::string, std::string> HtmlParser::parse_style(std::string_view style_attr) {
    std::unordered_map<std::string, std::string> styles;
    size_t start = 0;
    while (start < style_attr.size()) {
        size_t colon = style_attr.find(':', start);
        if (colon == std::string_view::npos) break;

        size_t semi = style_attr.find(';', colon);
        if (semi == std::string_view::npos) semi = style_attr.size();

        auto key = trim(style_attr.substr(start, colon - start));
        auto val = trim(style_attr.substr(colon + 1, semi - colon - 1));

        if (!key.empty() && !val.empty()) {
            styles[to_lower(key)] = std::string(val);
        }
        start = semi + 1;
    }
    return styles;
}

bool HtmlParser::is_block_html_line(std::string_view line) {
    auto s = trim(line);
    if (!s.starts_with("<")) return false;
    std::string lower = to_lower(s);

    return lower.starts_with("<details") || lower.starts_with("</details") ||
           lower.starts_with("<summary") || lower.starts_with("</summary") ||
           lower.starts_with("<div") || lower.starts_with("</div") ||
           lower.starts_with("<p ") || lower.starts_with("<p>") || lower.starts_with("</p>") ||
           lower.starts_with("<blockquote") || lower.starts_with("</blockquote") ||
           lower.starts_with("<table") || lower.starts_with("</table") ||
           lower.starts_with("<!--");
}

bool HtmlParser::is_tagfilter_disallowed(std::string_view tag_name) {
    static const std::unordered_set<std::string> disallowed = {
        "title", "textarea", "style", "xmp", "iframe",
        "noembed", "noframes", "script", "plaintext"
    };
    return disallowed.find(to_lower(tag_name)) != disallowed.end();
}

std::string HtmlParser::apply_tagfilter(std::string_view html) {
    std::string out;
    out.reserve(html.size() + 16);

    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t j = i + 1;
            if (j < html.size() && html[j] == '/') {
                j++;
            }
            size_t name_start = j;
            while (j < html.size() && std::isalpha(static_cast<unsigned char>(html[j]))) {
                j++;
            }
            if (j > name_start) {
                std::string_view tag_name = html.substr(name_start, j - name_start);
                bool valid_boundary = (j >= html.size() ||
                                       html[j] == '>' ||
                                       html[j] == '/' ||
                                       std::isspace(static_cast<unsigned char>(html[j])));
                if (valid_boundary && is_tagfilter_disallowed(tag_name)) {
                    out += "&lt;";
                    i++;
                    continue;
                }
            }
        }
        out.push_back(html[i]);
        i++;
    }
    return out;
}

} // namespace nisaba::markdown
