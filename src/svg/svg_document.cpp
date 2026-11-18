#include "nisaba/svg/svg_document.hpp"
#include "nisaba/svg/path_parser.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/image/image_io.hpp"
#include "nisaba/path/path_measure.hpp"
#include "nisaba/path/path_ops.hpp"
#include "nisaba/text/bidi.hpp"
#include "nisaba/text/ttf_font.hpp"
#include "nisaba/effects/blur.hpp"
#include "nisaba/effects/color_matrix.hpp"
#include "nisaba/effects/drop_shadow.hpp"

#if defined(NISABA_HAS_GPU) || defined(NISABA_GLEW)
#include "nisaba/gpu/gpu_canvas.hpp"
#endif

#include <fstream>
#include <sstream>
#include <map>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <array>
#include <mutex>

namespace nisaba::svg {

namespace {

constexpr float PI = 3.14159265358979323846f;

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string to_lower_ascii(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

/// Zero-allocation float parser with unit conversion support.
bool parse_raw_float(std::string_view s, float& out, size_t* consumed = nullptr) noexcept {
    s = trim(s);
    if (s.empty()) return false;

    size_t pos = 0;
    bool neg = false;
    if (s[pos] == '+' || s[pos] == '-') {
        neg = (s[pos] == '-');
        pos++;
    }

    uint64_t int_part = 0;
    bool has_digits = false;
    int int_digits = 0;
    while (pos < s.size() && (s[pos] >= '0' && s[pos] <= '9')) {
        has_digits = true;
        if (int_digits < 18) {
            int_part = int_part * 10 + static_cast<uint64_t>(s[pos] - '0');
        }
        int_digits++;
        pos++;
    }

    uint64_t frac_part = 0;
    int frac_digits = 0;
    if (pos < s.size() && s[pos] == '.') {
        pos++;
        while (pos < s.size() && (s[pos] >= '0' && s[pos] <= '9')) {
            has_digits = true;
            if (frac_digits < 18) {
                frac_part = frac_part * 10 + static_cast<uint64_t>(s[pos] - '0');
                frac_digits++;
            }
            pos++;
        }
    }

    if (!has_digits) return false;

    int exp_val = 0;
    bool exp_neg = false;
    if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
        size_t exp_start = pos;
        pos++;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
            exp_neg = (s[pos] == '-');
            pos++;
        }
        bool exp_digits = false;
        while (pos < s.size() && (s[pos] >= '0' && s[pos] <= '9')) {
            exp_digits = true;
            if (exp_val < 1000) {
                exp_val = exp_val * 10 + (s[pos] - '0');
            }
            pos++;
        }
        if (!exp_digits) {
            pos = exp_start;
            exp_val = 0;
            exp_neg = false;
        }
    }

    double val = static_cast<double>(int_part);
    if (int_digits > 18) val *= std::pow(10.0, int_digits - 18);
    if (frac_digits > 0) {
        static constexpr double pow10_inv[] = {
            1.0, 1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9,
            1e-10, 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18
        };
        val += static_cast<double>(frac_part) * pow10_inv[frac_digits];
    }
    if (exp_val != 0) val *= std::pow(10.0, exp_neg ? -exp_val : exp_val);
    if (neg) val = -val;

    out = static_cast<float>(val);
    if (consumed) *consumed = pos;
    return true;
}

float parse_float(std::string_view s, float default_val = 0.0f, float ref_length = 100.0f, float font_size = 16.0f) noexcept {
    s = trim(s);
    if (s.empty()) return default_val;

    float val = default_val;
    size_t consumed = 0;
    if (!parse_raw_float(s, val, &consumed)) return default_val;

    std::string_view unit = trim(s.substr(consumed));
    if (unit.empty() || unit == "px") return val;
    if (unit == "%") return (val * 0.01f) * ref_length;
    if (unit == "pt") return val * (96.0f / 72.0f);
    if (unit == "pc") return val * 16.0f;
    if (unit == "in") return val * 96.0f;
    if (unit == "cm") return val * (96.0f / 2.54f);
    if (unit == "mm") return val * (96.0f / 25.4f);
    if (unit == "em") return val * font_size;
    if (unit == "rem") return val * 16.0f;
    if (unit == "ex") return val * font_size * 0.5f;
    if (unit == "deg") return val;
    if (unit == "rad") return val * (180.0f / PI);
    if (unit == "grad") return val * 0.9f;
    if (unit == "turn") return val * 360.0f;
    return val;
}

float parse_coord(std::string_view s, float default_val = 0.0f, float ref_length = 1.0f) noexcept {
    s = trim(s);
    if (s.empty()) return default_val;
    if (s.back() == '%') {
        float pct = parse_float(s.substr(0, s.size() - 1), default_val * 100.0f);
        return (pct / 100.0f) * ref_length;
    }
    return parse_float(s, default_val, ref_length);
}

std::string decode_xml_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i + 1);
            if (semi != std::string_view::npos && semi - i <= 12) {
                std::string_view ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out.push_back('&'); i = semi; continue; }
                if (ent == "lt") { out.push_back('<'); i = semi; continue; }
                if (ent == "gt") { out.push_back('>'); i = semi; continue; }
                if (ent == "quot") { out.push_back('"'); i = semi; continue; }
                if (ent == "apos") { out.push_back('\''); i = semi; continue; }
                if (ent == "nbsp") { out.push_back(' '); i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    uint32_t cp = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        for (size_t k = 2; k < ent.size(); ++k) {
                            char c = ent[k];
                            cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= (c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= (c - 'A' + 10);
                        }
                    } else {
                        for (size_t k = 1; k < ent.size(); ++k) {
                            if (ent[k] >= '0' && ent[k] <= '9') cp = cp * 10 + (ent[k] - '0');
                        }
                    }
                    if (cp > 0) {
                        if (cp <= 0x7F) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp <= 0xFFFF) {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp <= 0x10FFFF) {
                            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        i = semi;
                        continue;
                    }
                }
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string extract_url_id(std::string_view v, std::string_view* out_fallback = nullptr) {
    v = trim(v);
    if (v.starts_with("url(")) {
        size_t close_p = v.find(')');
        if (close_p != std::string_view::npos) {
            if (out_fallback && close_p + 1 < v.size()) {
                *out_fallback = trim(v.substr(close_p + 1));
            }
            v = trim(v.substr(4, close_p - 4));
            if (!v.empty() && (v.front() == '\'' || v.front() == '"')) v.remove_prefix(1);
            if (!v.empty() && (v.back() == '\'' || v.back() == '"')) v.remove_suffix(1);
            v = trim(v);
            if (v.starts_with("#")) v.remove_prefix(1);
            return std::string(v);
        }
    }
    if (v.starts_with("#")) return std::string(v.substr(1));
    return std::string(v);
}

std::string extract_css_var(std::string_view v, std::string_view* out_fallback = nullptr) {
    v = trim(v);
    if (v.starts_with("var(") && v.ends_with(")")) {
        std::string_view inner = trim(v.substr(4, v.size() - 5));
        size_t comma = inner.find(',');
        if (comma != std::string_view::npos) {
            if (out_fallback) *out_fallback = trim(inner.substr(comma + 1));
            return std::string(trim(inner.substr(0, comma)));
        }
        return std::string(inner);
    }
    return "";
}

std::vector<uint8_t> base64_decode(std::string_view input) {
    std::vector<uint8_t> out;
    out.reserve((input.size() * 3) / 4);

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };

    uint32_t val = 0;
    int valb = -8;
    for (char c : input) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '=') break;
        int d = decode_char(c);
        if (d < 0) continue;
        val = (val << 6) | static_cast<uint32_t>(d);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

struct NamedColorEntry {
    std::string_view name;
    uint32_t rgb;
};

// Complete W3C SVG 1.1 / CSS Color Module Level 4 148 standard named colors (sorted alphabetically)
constexpr std::array<NamedColorEntry, 148> W3C_NAMED_COLORS = {{
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32}
}};

Color hsl_to_rgb(float h, float s, float l, float a) noexcept {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);
    a = std::clamp(a, 0.0f, 1.0f);

    float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = l - c * 0.5f;
    float r = 0.0f, g = 0.0f, b = 0.0f;

    if (h < 60.0f)      { r = c; g = x; b = 0; }
    else if (h < 120.0f){ r = x; g = c; b = 0; }
    else if (h < 180.0f){ r = 0; g = c; b = x; }
    else if (h < 240.0f){ r = 0; g = x; b = c; }
    else if (h < 300.0f){ r = x; g = 0; b = c; }
    else                { r = c; g = 0; b = x; }

    return Color::from_rgba8(
        static_cast<uint8_t>(std::clamp((r + m) * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp((g + m) * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp((b + m) * 255.0f + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(a * 255.0f + 0.5f)
    );
}

std::optional<Color> parse_color(std::string_view str, Color current_col = Color::BLACK) noexcept {
    str = trim(str);
    if (str.empty() || iequals(str, "none")) {
        return std::nullopt;
    }
    if (iequals(str, "transparent")) {
        return Color::TRANSPARENT;
    }
    if (iequals(str, "currentColor") || iequals(str, "inherit")) {
        return current_col;
    }

    if (str.front() == '#') {
        std::string_view hex = str.substr(1);
        auto hex_nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        auto hex_byte = [&](size_t idx) -> uint8_t {
            return static_cast<uint8_t>((hex_nibble(hex[idx]) << 4) | hex_nibble(hex[idx + 1]));
        };

        if (hex.size() == 3) {
            return Color::from_rgba8(hex_nibble(hex[0]) * 17, hex_nibble(hex[1]) * 17, hex_nibble(hex[2]) * 17, 255);
        }
        if (hex.size() == 4) {
            return Color::from_rgba8(hex_nibble(hex[0]) * 17, hex_nibble(hex[1]) * 17, hex_nibble(hex[2]) * 17, hex_nibble(hex[3]) * 17);
        }
        if (hex.size() == 6) {
            return Color::from_rgba8(hex_byte(0), hex_byte(2), hex_byte(4), 255);
        }
        if (hex.size() == 8) {
            return Color::from_rgba8(hex_byte(0), hex_byte(2), hex_byte(4), hex_byte(6));
        }
    }

    auto tokenize_func_args = [](std::string_view inner) {
        std::vector<std::string_view> tokens;
        size_t p = 0;
        while (p < inner.size()) {
            while (p < inner.size() && (std::isspace(static_cast<unsigned char>(inner[p])) || inner[p] == ',' || inner[p] == '/')) p++;
            if (p >= inner.size()) break;
            size_t end = inner.find_first_of(" ,\t\r\n/", p);
            if (end == std::string_view::npos) end = inner.size();
            tokens.push_back(inner.substr(p, end - p));
            p = end;
        }
        return tokens;
    };

    if ((str.starts_with("rgb(") || str.starts_with("rgba(")) && str.ends_with(")")) {
        size_t open_p = str.find('(');
        auto args = tokenize_func_args(str.substr(open_p + 1, str.size() - open_p - 2));
        if (args.size() >= 3) {
            auto parse_chan = [](std::string_view tok) -> uint8_t {
                float v = 0.0f;
                if (tok.ends_with("%")) {
                    v = parse_float(tok.substr(0, tok.size() - 1)) * 2.55f;
                } else {
                    v = parse_float(tok);
                }
                return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            };
            uint8_t r = parse_chan(args[0]);
            uint8_t g = parse_chan(args[1]);
            uint8_t b = parse_chan(args[2]);
            uint8_t a = 255;
            if (args.size() >= 4) {
                float av = args[3].ends_with("%")
                    ? parse_float(args[3].substr(0, args[3].size() - 1)) * 0.01f
                    : parse_float(args[3], 1.0f);
                a = static_cast<uint8_t>(std::clamp(av * 255.0f + 0.5f, 0.0f, 255.0f));
            }
            return Color::from_rgba8(r, g, b, a);
        }
    }

    if ((str.starts_with("hsl(") || str.starts_with("hsla(")) && str.ends_with(")")) {
        size_t open_p = str.find('(');
        auto args = tokenize_func_args(str.substr(open_p + 1, str.size() - open_p - 2));
        if (args.size() >= 3) {
            float h = parse_float(args[0], 0.0f);
            float s = parse_coord(args[1], 0.0f, 1.0f);
            float l = parse_coord(args[2], 0.0f, 1.0f);
            float a = (args.size() >= 4) ? parse_coord(args[3], 1.0f, 1.0f) : 1.0f;
            return hsl_to_rgb(h, s, l, a);
        }
    }

    std::string lower = to_lower_ascii(str);
    auto it = std::lower_bound(
        W3C_NAMED_COLORS.begin(),
        W3C_NAMED_COLORS.end(),
        std::string_view(lower),
        [](const NamedColorEntry& entry, std::string_view key) {
            return entry.name < key;
        }
    );
    if (it != W3C_NAMED_COLORS.end() && it->name == lower) {
        uint32_t v = it->rgb;
        return Color::from_rgba8(
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>(v & 0xFF),
            255
        );
    }

    return Color::BLACK;
}

SvgPreserveAspectRatio parse_preserve_aspect_ratio(std::string_view str) noexcept {
    SvgPreserveAspectRatio par;
    str = trim(str);
    if (str.empty()) return par;

    size_t pos = 0;
    while (pos < str.size()) {
        while (pos < str.size() && std::isspace(static_cast<unsigned char>(str[pos]))) pos++;
        if (pos >= str.size()) break;
        size_t end = str.find_first_of(" \t\r\n", pos);
        if (end == std::string_view::npos) end = str.size();
        std::string_view tok = str.substr(pos, end - pos);
        pos = end;

        if (tok == "none") par.align = SvgAspectAlign::None;
        else if (tok == "xMinYMin") par.align = SvgAspectAlign::XMinYMin;
        else if (tok == "xMidYMin") par.align = SvgAspectAlign::XMidYMin;
        else if (tok == "xMaxYMin") par.align = SvgAspectAlign::XMaxYMin;
        else if (tok == "xMinYMid") par.align = SvgAspectAlign::XMinYMid;
        else if (tok == "xMidYMid") par.align = SvgAspectAlign::XMidYMid;
        else if (tok == "xMaxYMid") par.align = SvgAspectAlign::XMaxYMid;
        else if (tok == "xMinYMax") par.align = SvgAspectAlign::XMinYMax;
        else if (tok == "xMidYMax") par.align = SvgAspectAlign::XMidYMax;
        else if (tok == "xMaxYMax") par.align = SvgAspectAlign::XMaxYMax;
        else if (tok == "meet") par.meet_or_slice = SvgAspectMeetOrSlice::Meet;
        else if (tok == "slice") par.meet_or_slice = SvgAspectMeetOrSlice::Slice;
    }
    return par;
}

BlendMode parse_blend_mode(std::string_view v) noexcept {
    v = trim(v);
    if (v == "multiply") return BlendMode::Multiply;
    if (v == "screen") return BlendMode::Screen;
    if (v == "overlay") return BlendMode::Overlay;
    if (v == "darken") return BlendMode::Darken;
    if (v == "lighten") return BlendMode::Lighten;
    if (v == "color-dodge") return BlendMode::ColorDodge;
    if (v == "color-burn") return BlendMode::ColorBurn;
    if (v == "hard-light") return BlendMode::HardLight;
    if (v == "soft-light") return BlendMode::SoftLight;
    if (v == "difference") return BlendMode::Difference;
    if (v == "exclusion") return BlendMode::Exclusion;
    if (v == "hue") return BlendMode::Hue;
    if (v == "saturation") return BlendMode::Saturation;
    if (v == "color") return BlendMode::Color;
    if (v == "luminosity") return BlendMode::Luminosity;
    if (v == "plus-lighter" || v == "lighter" || v == "add") return BlendMode::Plus;
    return BlendMode::SourceOver;
}

std::vector<float> parse_number_list(std::string_view s) {
    std::vector<float> nums;
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ',')) pos++;
        if (pos >= s.size()) break;
        size_t end = s.find_first_of(" ,\t\r\n", pos);
        if (end == std::string_view::npos) end = s.size();
        std::string_view tok = s.substr(pos, end - pos);
        if (iequals(tok, "none")) return {};
        float v = 0.0f;
        if (parse_raw_float(tok, v)) {
            nums.push_back(v);
        }
        pos = end;
    }
    return nums;
}

Transform parse_transform(std::string_view str) noexcept {
    Transform ts;
    size_t pos = 0;
    while (pos < str.size()) {
        size_t open_p = str.find('(', pos);
        if (open_p == std::string_view::npos) break;
        size_t close_p = str.find(')', open_p);
        if (close_p == std::string_view::npos) break;

        std::string_view name = trim(str.substr(pos, open_p - pos));
        std::string_view args_str = str.substr(open_p + 1, close_p - open_p - 1);

        std::vector<float> args = parse_number_list(args_str);

        if (name.ends_with("translate") && !args.empty()) {
            float tx = args[0];
            float ty = (args.size() > 1) ? args[1] : 0.0f;
            ts = ts.pre_translate(tx, ty);
        } else if (name.ends_with("scale") && !args.empty()) {
            float sx = args[0];
            float sy = (args.size() > 1) ? args[1] : sx;
            ts = ts.pre_scale(sx, sy);
        } else if (name.ends_with("rotate") && !args.empty()) {
            float deg = args[0];
            if (args.size() >= 3) {
                float cx = args[1], cy = args[2];
                ts = ts.pre_translate(cx, cy).pre_rotate(deg).pre_translate(-cx, -cy);
            } else {
                ts = ts.pre_rotate(deg);
            }
        } else if (name.ends_with("skewX") && !args.empty()) {
            float rad = args[0] * PI / 180.0f;
            ts = ts.pre_concat(Transform(1.0f, 0.0f, std::tan(rad), 1.0f, 0.0f, 0.0f));
        } else if (name.ends_with("skewY") && !args.empty()) {
            float rad = args[0] * PI / 180.0f;
            ts = ts.pre_concat(Transform(1.0f, std::tan(rad), 0.0f, 1.0f, 0.0f, 0.0f));
        } else if (name.ends_with("matrix") && args.size() >= 6) {
            Transform m(args[0], args[1], args[2], args[3], args[4], args[5]);
            ts = ts.pre_concat(m);
        }

        pos = close_p + 1;
    }
    return ts;
}

std::map<std::string, std::string> parse_attributes(std::string_view tag_content) {
    std::map<std::string, std::string> attrs;
    size_t pos = 0;
    while (pos < tag_content.size()) {
        while (pos < tag_content.size() && std::isspace(static_cast<unsigned char>(tag_content[pos]))) pos++;
        if (pos >= tag_content.size()) break;

        size_t eq_pos = tag_content.find('=', pos);
        if (eq_pos == std::string_view::npos) break;

        std::string_view key = trim(tag_content.substr(pos, eq_pos - pos));
        pos = eq_pos + 1;
        while (pos < tag_content.size() && std::isspace(static_cast<unsigned char>(tag_content[pos]))) pos++;
        if (pos >= tag_content.size()) break;

        char quote = tag_content[pos];
        if (quote == '"' || quote == '\'') {
            pos++;
            size_t close_q = tag_content.find(quote, pos);
            if (close_q == std::string_view::npos) break;
            attrs[std::string(key)] = decode_xml_entities(tag_content.substr(pos, close_q - pos));
            pos = close_q + 1;
        } else {
            size_t val_end = tag_content.find_first_of(" \t\r\n>", pos);
            if (val_end == std::string_view::npos) val_end = tag_content.size();
            attrs[std::string(key)] = decode_xml_entities(tag_content.substr(pos, val_end - pos));
            pos = val_end;
        }
    }
    return attrs;
}

void parse_paint_property(
    std::string_view v,
    Color current_color,
    const std::unordered_map<std::string, Color>& root_vars,
    std::optional<Color>& out_color,
    bool& out_is_current_color,
    std::string& out_url,
    std::string& out_css_var,
    std::optional<Color>& out_fallback
) {
    v = trim(v);
    out_is_current_color = false;
    out_url.clear();
    out_css_var.clear();
    out_fallback.reset();

    if (v.starts_with("url(")) {
        std::string_view fb;
        out_url = extract_url_id(v, &fb);
        out_color = std::nullopt;
        if (!fb.empty()) {
            out_fallback = parse_color(fb, current_color);
        }
        return;
    }

    if (v.starts_with("var(")) {
        std::string_view fb;
        out_css_var = extract_css_var(v, &fb);
        if (!fb.empty()) {
            if (iequals(fb, "currentColor")) {
                out_is_current_color = true;
                out_fallback = current_color;
            } else {
                out_fallback = parse_color(fb, current_color);
            }
        }
        auto it = root_vars.find(out_css_var);
        if (it != root_vars.end()) {
            out_color = it->second;
        } else {
            out_color = out_fallback;
        }
        return;
    }

    if (iequals(v, "currentColor")) {
        out_is_current_color = true;
        out_color = current_color;
        return;
    }

    out_color = parse_color(v, current_color);
}

void apply_style_property(
    SvgPaintStyle& style,
    std::string_view k,
    std::string_view v,
    float parent_opacity,
    const std::unordered_map<std::string, Color>& root_vars
) {
    k = trim(k);
    v = trim(v);
    if (v.ends_with("!important")) {
        v = trim(v.substr(0, v.size() - 10));
    }

    if (k == "color") {
        if (auto c = parse_color(v, style.current_color)) {
            style.current_color = *c;
            if (style.fill_is_current_color) style.fill = style.current_color;
            if (style.stroke_is_current_color) style.stroke = style.current_color;
        }
    } else if (k == "fill") {
        parse_paint_property(v, style.current_color, root_vars,
                             style.fill, style.fill_is_current_color, style.fill_url, style.fill_css_var, style.fill_fallback);
    } else if (k == "stroke") {
        parse_paint_property(v, style.current_color, root_vars,
                             style.stroke, style.stroke_is_current_color, style.stroke_url, style.stroke_css_var, style.stroke_fallback);
    } else if (k == "stroke-width") {
        style.stroke_width = parse_float(v, style.stroke_width, 100.0f, style.font_size);
    } else if (k == "stroke-miterlimit") {
        style.miter_limit = std::max(1.0f, parse_float(v, style.miter_limit));
    } else if (k == "stroke-dasharray") {
        style.dash_array = parse_number_list(v);
        if (style.dash_array.size() % 2 == 1) {
            // W3C SVG spec: odd-length dasharray is duplicated to become even-length
            size_t n = style.dash_array.size();
            style.dash_array.reserve(n * 2);
            for (size_t i = 0; i < n; ++i) style.dash_array.push_back(style.dash_array[i]);
        }
    } else if (k == "stroke-dashoffset") {
        style.dash_offset = parse_float(v, style.dash_offset);
    } else if (k == "vector-effect") {
        style.non_scaling_stroke = (v == "non-scaling-stroke");
    } else if (k == "opacity") {
        style.opacity = parent_opacity * std::clamp(parse_coord(v, 1.0f, 1.0f), 0.0f, 1.0f);
    } else if (k == "fill-opacity") {
        style.fill_opacity = std::clamp(parse_coord(v, style.fill_opacity, 1.0f), 0.0f, 1.0f);
    } else if (k == "stroke-opacity") {
        style.stroke_opacity = std::clamp(parse_coord(v, style.stroke_opacity, 1.0f), 0.0f, 1.0f);
    } else if (k == "stroke-linecap") {
        if (v == "round") style.line_cap = LineCap::Round;
        else if (v == "square") style.line_cap = LineCap::Square;
        else style.line_cap = LineCap::Butt;
    } else if (k == "stroke-linejoin") {
        if (v == "round") style.line_join = LineJoin::Round;
        else if (v == "bevel") style.line_join = LineJoin::Bevel;
        else if (v == "miter-clip") style.line_join = LineJoin::MiterClip;
        else style.line_join = LineJoin::Miter;
    } else if (k == "fill-rule") {
        if (v == "evenodd") style.fill_rule = FillRule::EvenOdd;
        else style.fill_rule = FillRule::Winding;
    } else if (k == "clip-rule") {
        if (v == "evenodd") style.clip_rule = FillRule::EvenOdd;
        else style.clip_rule = FillRule::Winding;
    } else if (k == "clip-path") {
        style.clip_path_url = (v == "none") ? "" : extract_url_id(v);
    } else if (k == "mask") {
        style.mask_url = (v == "none") ? "" : extract_url_id(v);
    } else if (k == "filter") {
        style.filter_url = (v == "none") ? "" : extract_url_id(v);
    } else if (k == "mix-blend-mode") {
        style.blend_mode = parse_blend_mode(v);
    } else if (k == "color-interpolation" || k == "color-interpolation-filters") {
        style.linear_color_interpolation = iequals(v, "linearRGB");
    } else if (k == "display") {
        style.display_none = iequals(v, "none");
    } else if (k == "visibility") {
        style.visibility_hidden = (iequals(v, "hidden") || iequals(v, "collapse"));
    } else if (k == "marker-start") {
        style.marker_start_url = (v == "none") ? "" : extract_url_id(v);
    } else if (k == "marker-mid") {
        style.marker_mid_url = (v == "none") ? "" : extract_url_id(v);
    } else if (k == "marker-end") {
        style.marker_end_url = (v == "none") ? "" : extract_url_id(v);
    } else if (k == "marker") {
        std::string m_id = (v == "none") ? "" : extract_url_id(v);
        style.marker_start_url = m_id;
        style.marker_mid_url = m_id;
        style.marker_end_url = m_id;
    } else if (k == "paint-order") {
        if (v == "normal" || v.empty()) {
            style.paint_order = {SvgPaintPhase::Fill, SvgPaintPhase::Stroke, SvgPaintPhase::Markers};
        } else {
            std::vector<SvgPaintPhase> order;
            size_t p = 0;
            while (p < v.size()) {
                while (p < v.size() && std::isspace(static_cast<unsigned char>(v[p]))) p++;
                if (p >= v.size()) break;
                size_t end = v.find_first_of(" \t\r\n", p);
                if (end == std::string_view::npos) end = v.size();
                std::string_view tok = v.substr(p, end - p);
                p = end;
                if (tok == "fill") order.push_back(SvgPaintPhase::Fill);
                else if (tok == "stroke") order.push_back(SvgPaintPhase::Stroke);
                else if (tok == "markers") order.push_back(SvgPaintPhase::Markers);
            }
            for (SvgPaintPhase ph : {SvgPaintPhase::Fill, SvgPaintPhase::Stroke, SvgPaintPhase::Markers}) {
                if (std::find(order.begin(), order.end(), ph) == order.end()) {
                    order.push_back(ph);
                }
            }
            for (size_t i = 0; i < 3; ++i) style.paint_order[i] = order[i];
        }
    } else if (k == "font-family") {
        std::string_view ff = v;
        size_t comma = ff.find(',');
        if (comma != std::string_view::npos) ff = trim(ff.substr(0, comma));
        if (!ff.empty() && (ff.front() == '\'' || ff.front() == '"')) ff.remove_prefix(1);
        if (!ff.empty() && (ff.back() == '\'' || ff.back() == '"')) ff.remove_suffix(1);
        style.font_family = std::string(trim(ff));
    } else if (k == "font-size") {
        style.font_size = std::max(1.0f, parse_float(v, style.font_size, style.font_size, style.font_size));
    } else if (k == "font-weight") {
        if (v == "bold" || v == "bolder" || v == "700") style.font_weight = text::Weight::Bold;
        else if (v == "normal" || v == "400") style.font_weight = text::Weight::Normal;
        else if (v == "light" || v == "lighter" || v == "300") style.font_weight = text::Weight::Light;
        else if (v == "500" || v == "medium") style.font_weight = text::Weight::Medium;
        else if (v == "600" || v == "semibold") style.font_weight = text::Weight::SemiBold;
        else if (v == "800" || v == "extrabold") style.font_weight = text::Weight::ExtraBold;
        else if (v == "900" || v == "black") style.font_weight = text::Weight::Black;
    } else if (k == "font-style") {
        if (v == "italic") style.font_style = text::Style::Italic;
        else if (v == "oblique") style.font_style = text::Style::Oblique;
        else style.font_style = text::Style::Normal;
    } else if (k == "letter-spacing") {
        style.letter_spacing = parse_float(v, 0.0f, style.font_size, style.font_size);
    } else if (k == "text-anchor") {
        if (v == "middle") style.text_anchor = SvgTextAnchor::Middle;
        else if (v == "end") style.text_anchor = SvgTextAnchor::End;
        else style.text_anchor = SvgTextAnchor::Start;
    } else if (k == "dominant-baseline" || k == "alignment-baseline") {
        if (v == "middle") style.dominant_baseline = SvgDominantBaseline::Middle;
        else if (v == "central") style.dominant_baseline = SvgDominantBaseline::Central;
        else if (v == "hanging" || v == "text-before-edge" || v == "before-edge") style.dominant_baseline = SvgDominantBaseline::Hanging;
        else if (v == "ideographic" || v == "text-after-edge" || v == "after-edge") style.dominant_baseline = SvgDominantBaseline::TextAfterEdge;
        else if (v == "mathematical") style.dominant_baseline = SvgDominantBaseline::Mathematical;
        else style.dominant_baseline = SvgDominantBaseline::Alphabetic;
    }
}

void apply_style_string(
    SvgPaintStyle& style,
    std::string_view style_str,
    float parent_opacity,
    const std::unordered_map<std::string, Color>& root_vars,
    std::unordered_map<std::string, Color>* out_custom_vars = nullptr
) {
    size_t pos = 0;
    while (pos < style_str.size()) {
        size_t sc = style_str.find(';', pos);
        if (sc == std::string_view::npos) sc = style_str.size();
        std::string_view prop = trim(style_str.substr(pos, sc - pos));
        pos = sc + 1;

        size_t colon = prop.find(':');
        if (colon != std::string_view::npos) {
            std::string_view k = trim(prop.substr(0, colon));
            std::string_view v = trim(prop.substr(colon + 1));
            if (k.starts_with("--")) {
                if (out_custom_vars) {
                    if (auto c = parse_color(v, style.current_color)) {
                        (*out_custom_vars)[std::string(k)] = *c;
                    }
                }
            } else {
                apply_style_property(style, k, v, parent_opacity, root_vars);
            }
        }
    }
}

void parse_css_stylesheet(
    std::string_view css_text,
    std::vector<SvgCssRule>& rules,
    std::unordered_map<std::string, Color>& root_vars
) {
    // Strip /* ... */ comments
    std::string clean;
    clean.reserve(css_text.size());
    size_t p = 0;
    while (p < css_text.size()) {
        if (p + 1 < css_text.size() && css_text[p] == '/' && css_text[p + 1] == '*') {
            size_t end_c = css_text.find("*/", p + 2);
            p = (end_c != std::string_view::npos) ? end_c + 2 : css_text.size();
        } else {
            clean.push_back(css_text[p++]);
        }
    }

    std::string_view sv(clean);
    size_t pos = 0;
    while (pos < sv.size()) {
        size_t open_b = sv.find('{', pos);
        if (open_b == std::string_view::npos) break;
        size_t close_b = sv.find('}', open_b);
        if (close_b == std::string_view::npos) break;

        std::string_view selectors_str = trim(sv.substr(pos, open_b - pos));
        std::string_view decls = trim(sv.substr(open_b + 1, close_b - open_b - 1));
        pos = close_b + 1;

        // Split comma-separated selectors
        size_t s_pos = 0;
        while (s_pos < selectors_str.size()) {
            size_t comma = selectors_str.find(',', s_pos);
            if (comma == std::string_view::npos) comma = selectors_str.size();
            std::string_view sel = trim(selectors_str.substr(s_pos, comma - s_pos));
            s_pos = comma + 1;

            if (sel.empty()) continue;
            if (sel == ":root" || sel == "svg" || sel == "*") {
                SvgPaintStyle dummy;
                apply_style_string(dummy, decls, 1.0f, root_vars, &root_vars);
            }

            int spec = 1;
            if (sel.front() == '#') spec = 100;
            else if (sel.front() == '.') spec = 10;
            else if (sel.find('.') != std::string_view::npos) spec = 11;

            rules.push_back(SvgCssRule{
                .selector = std::string(sel),
                .specificity = spec,
                .declarations = std::string(decls)
            });
        }
    }
}

bool selector_matches(
    std::string_view selector,
    std::string_view tag_name,
    std::string_view elem_id,
    std::string_view class_attr
) noexcept {
    selector = trim(selector);
    if (selector.empty()) return false;
    if (selector == "*") return true;

    // Handle descendant selector: take rightmost simple selector if space exists
    size_t last_space = selector.find_last_of(" \t>");
    if (last_space != std::string_view::npos) {
        selector = trim(selector.substr(last_space + 1));
    }

    if (selector.front() == '#') {
        return !elem_id.empty() && selector.substr(1) == elem_id;
    }

    auto has_class = [&](std::string_view target_cls) -> bool {
        if (class_attr.empty()) return false;
        size_t p = 0;
        while (p < class_attr.size()) {
            while (p < class_attr.size() && std::isspace(static_cast<unsigned char>(class_attr[p]))) p++;
            if (p >= class_attr.size()) break;
            size_t end = class_attr.find_first_of(" \t\r\n", p);
            if (end == std::string_view::npos) end = class_attr.size();
            if (class_attr.substr(p, end - p) == target_cls) return true;
            p = end;
        }
        return false;
    };

    if (selector.front() == '.') {
        return has_class(selector.substr(1));
    }

    size_t dot = selector.find('.');
    if (dot != std::string_view::npos) {
        std::string_view t = selector.substr(0, dot);
        std::string_view c = selector.substr(dot + 1);
        return (t == tag_name) && has_class(c);
    }

    return selector == tag_name;
}

/// Global lazy fallback FontSystem for SVG <text> rendering.
text::FontSystem& get_svg_font_system() {
    static text::FontSystem fs;
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        const char* candidate_paths[] = {
            "fonts/Inter-Regular.ttf",
            "fonts/NotoSans-Regular.ttf",
            "fonts/NotoSansArabic.ttf",
            "fonts/FiraMono-Medium.ttf",
            "fonts/SourceSans3-Regular.otf",
            "/home/x/Desktop/nisaba/fonts/Inter-Regular.ttf",
            "/home/x/Desktop/nisaba/fonts/NotoSans-Regular.ttf",
            "/home/x/Desktop/nisaba/fonts/NotoSansArabic.ttf",
            "/home/x/Desktop/nisaba/fonts/FiraMono-Medium.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf"
        };
        for (const char* p : candidate_paths) {
            auto id = fs.load_font_file(p);
            if (id && !fs.default_font_id().has_value()) {
                fs.set_default_font(*id);
            }
        }
    });
    return fs;
}

} // namespace

Transform SvgPreserveAspectRatio::compute_transform(const Rect& vb, const Rect& vp) const noexcept {
    if (vb.width() <= 0.0f || vb.height() <= 0.0f || vp.width() <= 0.0f || vp.height() <= 0.0f) {
        return Transform::identity();
    }

    float sx = vp.width() / vb.width();
    float sy = vp.height() / vb.height();

    if (align == SvgAspectAlign::None) {
        float tx = vp.left() - vb.left() * sx;
        float ty = vp.top() - vb.top() * sy;
        return Transform::from_row(sx, 0.0f, 0.0f, sy, tx, ty);
    }

    float scale = (meet_or_slice == SvgAspectMeetOrSlice::Slice) ? std::max(sx, sy) : std::min(sx, sy);
    float scaled_w = vb.width() * scale;
    float scaled_h = vb.height() * scale;
    float spare_x = vp.width() - scaled_w;
    float spare_y = vp.height() - scaled_h;

    float offset_x = 0.0f;
    float offset_y = 0.0f;

    switch (align) {
        case SvgAspectAlign::XMinYMin:
        case SvgAspectAlign::XMinYMid:
        case SvgAspectAlign::XMinYMax:
            offset_x = 0.0f;
            break;
        case SvgAspectAlign::XMidYMin:
        case SvgAspectAlign::XMidYMid:
        case SvgAspectAlign::XMidYMax:
        default:
            offset_x = spare_x * 0.5f;
            break;
        case SvgAspectAlign::XMaxYMin:
        case SvgAspectAlign::XMaxYMid:
        case SvgAspectAlign::XMaxYMax:
            offset_x = spare_x;
            break;
    }

    switch (align) {
        case SvgAspectAlign::XMinYMin:
        case SvgAspectAlign::XMidYMin:
        case SvgAspectAlign::XMaxYMin:
            offset_y = 0.0f;
            break;
        case SvgAspectAlign::XMinYMid:
        case SvgAspectAlign::XMidYMid:
        case SvgAspectAlign::XMaxYMid:
        default:
            offset_y = spare_y * 0.5f;
            break;
        case SvgAspectAlign::XMinYMax:
        case SvgAspectAlign::XMidYMax:
        case SvgAspectAlign::XMaxYMax:
            offset_y = spare_y;
            break;
    }

    float tx = vp.left() + offset_x - vb.left() * scale;
    float ty = vp.top() + offset_y - vb.top() * scale;
    return Transform::from_row(scale, 0.0f, 0.0f, scale, tx, ty);
}

namespace {

Color resolve_effective_color(
    const std::optional<Color>& base_col,
    bool is_current_color,
    const std::string& css_var_name,
    const std::optional<Color>& fallback_col,
    Color style_current_color,
    const std::string& elem_id,
    const SvgTheme& theme,
    const std::unordered_map<std::string, Color>& root_vars
) {
    if (theme.tint_color.has_value()) {
        return *theme.tint_color;
    }
    if (!elem_id.empty()) {
        auto it_id = theme.id_color_overrides.find(elem_id);
        if (it_id != theme.id_color_overrides.end()) {
            return it_id->second;
        }
    }
    if (!css_var_name.empty()) {
        auto it_v = theme.css_variables.find(css_var_name);
        if (it_v != theme.css_variables.end()) return it_v->second;
        if (css_var_name.starts_with("--")) {
            auto it_v2 = theme.css_variables.find(css_var_name.substr(2));
            if (it_v2 != theme.css_variables.end()) return it_v2->second;
        }
        auto it_rv = root_vars.find(css_var_name);
        if (it_rv != root_vars.end()) return it_rv->second;
        if (is_current_color) {
            return theme.current_color.value_or(style_current_color);
        }
        if (fallback_col.has_value()) return *fallback_col;
    }
    if (is_current_color) {
        return theme.current_color.value_or(style_current_color);
    }
    return base_col.value_or(Color::BLACK);
}

Shader create_shader_from_gradient(
    const SvgGradient& grad,
    const Rect& bounds,
    float opacity,
    const SvgTheme& theme
) {
    if (grad.stops.empty()) {
        return Shader::from_color(Color::TRANSPARENT);
    }

    std::vector<GradientStop> stops = grad.stops;
    if (stops.size() == 1) {
        stops.push_back(stops[0]);
        stops[0].position = NormalizedF32::ZERO;
        stops[1].position = NormalizedF32::ONE;
    }

    for (auto& s : stops) {
        if (theme.tint_color.has_value()) {
            float orig_a = s.color.alpha();
            s.color = *theme.tint_color;
            s.color.apply_opacity(orig_a);
        }
        if (opacity < 1.0f) {
            s.color.apply_opacity(opacity);
        }
    }

    if (grad.kind == SvgGradient::Kind::Linear) {
        Point p0, p1;
        Transform grad_ts = grad.gradient_transform;
        if (grad.units == SvgGradientUnits::ObjectBoundingBox) {
            float bx = bounds.left();
            float by = bounds.top();
            float bw = std::max(bounds.width(), 1e-4f);
            float bh = std::max(bounds.height(), 1e-4f);
            if (grad_ts.is_identity()) {
                p0 = Point::from_xy(bx + grad.x1 * bw, by + grad.y1 * bh);
                p1 = Point::from_xy(bx + grad.x2 * bw, by + grad.y2 * bh);
            } else {
                p0 = Point::from_xy(grad.x1, grad.y1);
                p1 = Point::from_xy(grad.x2, grad.y2);
                Transform bbox_ts = Transform::from_row(bw, 0.0f, 0.0f, bh, bx, by);
                grad_ts = bbox_ts.pre_concat(grad_ts);
            }
        } else {
            p0 = Point::from_xy(grad.x1, grad.y1);
            p1 = Point::from_xy(grad.x2, grad.y2);
        }

        if (std::abs(p1.x - p0.x) < 1e-5f && std::abs(p1.y - p0.y) < 1e-5f) {
            p1.x += 0.001f;
        }

        auto lin = LinearGradient::create(p0, p1, stops, grad.spread_mode, grad_ts);
        if (lin) return Shader(*lin);
    } else {
        Point center, focal;
        float radius = 0.0f, focal_radius = 0.0f;
        Transform grad_ts = grad.gradient_transform;

        if (grad.units == SvgGradientUnits::ObjectBoundingBox) {
            float bx = bounds.left();
            float by = bounds.top();
            float bw = std::max(bounds.width(), 1e-4f);
            float bh = std::max(bounds.height(), 1e-4f);
            if ( std::abs(bw - bh) < 1e-3f && grad_ts.is_identity() ) {
                center = Point::from_xy(bx + grad.cx * bw, by + grad.cy * bh);
                focal  = Point::from_xy(bx + grad.fx * bw, by + grad.fy * bh);
                radius = grad.r * bw;
                focal_radius = grad.fr * bw;
            } else {
                // True elliptical objectBoundingBox mapping via transform matrix
                center = Point::from_xy(grad.cx, grad.cy);
                focal  = Point::from_xy(grad.fx, grad.fy);
                radius = grad.r;
                focal_radius = grad.fr;
                Transform bbox_ts = Transform::from_row(bw, 0.0f, 0.0f, bh, bx, by);
                grad_ts = bbox_ts.pre_concat(grad_ts);
            }
        } else {
            center = Point::from_xy(grad.cx, grad.cy);
            focal  = Point::from_xy(grad.fx, grad.fy);
            radius = grad.r;
            focal_radius = grad.fr;
        }

        if (radius <= 0.0f) radius = 0.001f;

        bool has_focal_offset = (std::abs(focal.x - center.x) > 1e-5f ||
                                 std::abs(focal.y - center.y) > 1e-5f ||
                                 focal_radius > 1e-5f);
        if (has_focal_offset) {
            return Shader::create_two_point_conical(
                focal, std::max(0.0f, focal_radius),
                center, radius,
                stops, grad.spread_mode, grad_ts
            );
        }

        auto rad = RadialGradient::create(center, radius, stops, grad.spread_mode, grad_ts);
        if (rad) return Shader(*rad);
    }

    return Shader::from_color(stops.empty() ? Color::BLACK : stops.front().color);
}

enum class ScopeType : uint8_t {
    Svg,
    Defs,
    Group,
    Symbol,
    ClipPath,
    Mask,
    Pattern,
    Marker,
    Filter,
    LinearGradient,
    RadialGradient,
    Style,
    Text,
    TSpan,
    TextPath,
};

struct ParseScope {
    ScopeType type{ScopeType::Svg};
    std::string tag_name{};
    std::string id{};
    SvgPaintStyle style{};
    Transform transform{};
    std::optional<SvgGradient> gradient{};
    SvgClipPath clip_path{};
    SvgMask mask{};
    SvgPattern pattern{};
    SvgMarker marker{};
    SvgFilter filter{};
    SvgFilterPrimitive current_filter_prim{};
    bool in_fe_merge{false};
    SvgElement group_elem{};
    SvgElement text_elem{};
    SvgTextSpan active_span{};
};

void execute_filter_graph(
    const SvgFilter& filter,
    const PixmapRef& source_graphic,
    Pixmap& out_result
) {
    uint32_t w = source_graphic.width();
    uint32_t h = source_graphic.height();
    if (w == 0 || h == 0) return;

    std::map<std::string, Pixmap> results;

    auto make_source_alpha = [&]() -> Pixmap {
        auto pm = Pixmap::allocate(w, h);
        if (!pm) return Pixmap();
        const auto* src_p = source_graphic.pixels();
        auto* dst_p = pm->pixels_mut();
        size_t total = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < total; ++i) {
            uint8_t a = src_p[i].alpha();
            dst_p[i] = PremultipliedColorU8::from_rgba_unchecked(0, 0, 0, a);
        }
        return std::move(*pm);
    };

    auto get_input = [&](const std::string& name, const Pixmap* last_output) -> Pixmap {
        if (name == "SourceGraphic" || (name.empty() && last_output == nullptr)) {
            auto pm = Pixmap::allocate(w, h);
            if (pm) {
                std::copy_n(source_graphic.pixels(), static_cast<size_t>(w) * h, pm->pixels_mut());
                return std::move(*pm);
            }
            return Pixmap();
        }
        if (name == "SourceAlpha") {
            return make_source_alpha();
        }
        if (!name.empty()) {
            auto it = results.find(name);
            if (it != results.end()) {
                auto pm = Pixmap::allocate(w, h);
                if (pm) {
                    std::copy_n(it->second.pixels(), static_cast<size_t>(w) * h, pm->pixels_mut());
                    return std::move(*pm);
                }
            }
        }
        if (last_output && last_output->width() == w && last_output->height() == h) {
            auto pm = Pixmap::allocate(w, h);
            if (pm) {
                std::copy_n(last_output->pixels(), static_cast<size_t>(w) * h, pm->pixels_mut());
                return std::move(*pm);
            }
        }
        auto pm = Pixmap::allocate(w, h);
        if (pm) std::copy_n(source_graphic.pixels(), static_cast<size_t>(w) * h, pm->pixels_mut());
        return pm ? std::move(*pm) : Pixmap();
    };

    const Pixmap* last_ptr = nullptr;
    Pixmap last_temp;

    for (const auto& prim : filter.primitives) {
        auto dst_opt = Pixmap::allocate(w, h);
        if (!dst_opt) continue;
        Pixmap cur = std::move(*dst_opt);
        cur.fill(Color::TRANSPARENT);

        switch (prim.type) {
            case SvgFilterPrimitive::Type::GaussianBlur: {
                cur = get_input(prim.in1, last_ptr);
                float sigma = std::max(prim.std_dev_x, prim.std_dev_y);
                if (sigma > 0.0f) {
                    auto mut = cur.as_mut();
                    effects::gaussian_blur_pixmap(mut, sigma);
                }
                break;
            }
            case SvgFilterPrimitive::Type::Offset: {
                Pixmap src = get_input(prim.in1, last_ptr);
                Canvas c(cur);
                c.draw_pixmap(static_cast<int32_t>(std::round(prim.dx)),
                              static_cast<int32_t>(std::round(prim.dy)),
                              src.as_ref());
                break;
            }
            case SvgFilterPrimitive::Type::Flood: {
                Color fc = prim.flood_color;
                fc.apply_opacity(prim.flood_opacity);
                cur.fill(fc);
                break;
            }
            case SvgFilterPrimitive::Type::ColorMatrix: {
                cur = get_input(prim.in1, last_ptr);
                if (prim.luminance_to_alpha) {
                    auto* px = cur.pixels_mut();
                    size_t total = static_cast<size_t>(w) * h;
                    for (size_t i = 0; i < total; ++i) {
                        ColorU8 c = px[i].demultiply();
                        float lum = (0.2126f * (c.red() / 255.0f) + 0.7152f * (c.green() / 255.0f) + 0.0722f * (c.blue() / 255.0f)) * (c.alpha() / 255.0f);
                        uint8_t a = static_cast<uint8_t>(std::clamp(lum * 255.0f + 0.5f, 0.0f, 255.0f));
                        px[i] = PremultipliedColorU8::from_rgba_unchecked(0, 0, 0, a);
                    }
                } else {
                    prim.color_matrix.apply(cur);
                }
                break;
            }
            case SvgFilterPrimitive::Type::DropShadow: {
                Pixmap src = get_input(prim.in1, last_ptr);
                // 1. Create shadow from alpha
                auto shadow_opt = Pixmap::allocate(w, h);
                if (shadow_opt) {
                    Color sc = prim.flood_color;
                    sc.apply_opacity(prim.flood_opacity);
                    const auto* sp = src.pixels();
                    auto* dp = shadow_opt->pixels_mut();
                    size_t total = static_cast<size_t>(w) * h;
                    for (size_t i = 0; i < total; ++i) {
                        float a = (static_cast<float>(sp[i].alpha()) / 255.0f) * sc.alpha();
                        Color c = sc;
                        c.set_alpha(a);
                        dp[i] = c.premultiply().to_color_u8();
                    }
                    float sigma = std::max(prim.std_dev_x, prim.std_dev_y);
                    if (sigma > 0.0f) {
                        auto mut = shadow_opt->as_mut();
                        effects::gaussian_blur_pixmap(mut, sigma);
                    }
                    Canvas c(cur);
                    c.draw_pixmap(static_cast<int32_t>(std::round(prim.dx)),
                                  static_cast<int32_t>(std::round(prim.dy)),
                                  shadow_opt->as_ref());
                    c.draw_pixmap(0, 0, src.as_ref());
                }
                break;
            }
            case SvgFilterPrimitive::Type::Blend: {
                Pixmap in1 = get_input(prim.in1, last_ptr);
                Pixmap in2 = get_input(prim.in2, last_ptr);
                Canvas c(cur);
                c.draw_pixmap(0, 0, in2.as_ref());
                PixmapPaint pp;
                pp.blend_mode = prim.blend_mode;
                c.draw_pixmap(0, 0, in1.as_ref(), pp);
                break;
            }
            case SvgFilterPrimitive::Type::Composite: {
                Pixmap in1 = get_input(prim.in1, last_ptr);
                Pixmap in2 = get_input(prim.in2, last_ptr);
                if (prim.composite_op == SvgFilterPrimitive::CompositeOp::Arithmetic) {
                    const auto* p1 = in1.pixels();
                    const auto* p2 = in2.pixels();
                    auto* dst = cur.pixels_mut();
                    size_t total = static_cast<size_t>(w) * h;
                    for (size_t i = 0; i < total; ++i) {
                        float r1 = p1[i].red() / 255.0f, g1 = p1[i].green() / 255.0f, b1 = p1[i].blue() / 255.0f, a1 = p1[i].alpha() / 255.0f;
                        float r2 = p2[i].red() / 255.0f, g2 = p2[i].green() / 255.0f, b2 = p2[i].blue() / 255.0f, a2 = p2[i].alpha() / 255.0f;
                        float a = std::clamp(prim.k1 * a1 * a2 + prim.k2 * a1 + prim.k3 * a2 + prim.k4, 0.0f, 1.0f);
                        float r = std::clamp(prim.k1 * r1 * r2 + prim.k2 * r1 + prim.k3 * r2 + prim.k4, 0.0f, a);
                        float g = std::clamp(prim.k1 * g1 * g2 + prim.k2 * g1 + prim.k3 * g2 + prim.k4, 0.0f, a);
                        float b = std::clamp(prim.k1 * b1 * b2 + prim.k2 * b1 + prim.k3 * b2 + prim.k4, 0.0f, a);
                        dst[i] = PremultipliedColorU8::from_rgba_unchecked(
                            static_cast<uint8_t>(r * 255.0f + 0.5f),
                            static_cast<uint8_t>(g * 255.0f + 0.5f),
                            static_cast<uint8_t>(b * 255.0f + 0.5f),
                            static_cast<uint8_t>(a * 255.0f + 0.5f)
                        );
                    }
                } else {
                    Canvas c(cur);
                    c.draw_pixmap(0, 0, in2.as_ref());
                    PixmapPaint pp;
                    switch (prim.composite_op) {
                        case SvgFilterPrimitive::CompositeOp::Over: pp.blend_mode = BlendMode::SourceOver; break;
                        case SvgFilterPrimitive::CompositeOp::In:   pp.blend_mode = BlendMode::SourceIn; break;
                        case SvgFilterPrimitive::CompositeOp::Out:  pp.blend_mode = BlendMode::SourceOut; break;
                        case SvgFilterPrimitive::CompositeOp::Atop: pp.blend_mode = BlendMode::SourceAtop; break;
                        case SvgFilterPrimitive::CompositeOp::Xor:  pp.blend_mode = BlendMode::Xor; break;
                        default: pp.blend_mode = BlendMode::SourceOver; break;
                    }
                    c.draw_pixmap(0, 0, in1.as_ref(), pp);
                }
                break;
            }
            case SvgFilterPrimitive::Type::Merge: {
                Canvas c(cur);
                for (const auto& m_in : prim.merge_inputs) {
                    Pixmap layer = get_input(m_in, last_ptr);
                    c.draw_pixmap(0, 0, layer.as_ref());
                }
                break;
            }
        }

        if (!prim.result.empty()) {
            auto copy_opt = Pixmap::allocate(w, h);
            if (copy_opt) {
                std::copy_n(cur.pixels(), static_cast<size_t>(w) * h, copy_opt->pixels_mut());
                results[prim.result] = std::move(*copy_opt);
            }
        }
        last_temp = std::move(cur);
        last_ptr = &last_temp;
    }

    if (last_ptr) {
        std::copy_n(last_ptr->pixels(), static_cast<size_t>(w) * h, out_result.pixels_mut());
    } else {
        std::copy_n(source_graphic.pixels(), static_cast<size_t>(w) * h, out_result.pixels_mut());
    }
}

} // namespace

std::optional<SvgDocument> SvgDocument::parse(std::string_view xml) {
    SvgDocument doc;
    std::vector<SvgCssRule> css_rules;

    size_t pos = 0;
    std::vector<ParseScope> scope_stack;
    scope_stack.push_back(ParseScope{ .type = ScopeType::Svg, .tag_name = "svg" });

    auto in_defs = [&]() -> bool {
        for (const auto& s : scope_stack) {
            if (s.type == ScopeType::Defs || s.type == ScopeType::Symbol) return true;
        }
        return false;
    };

    auto find_enclosing_scope = [&](ScopeType st) -> ParseScope* {
        for (auto it = scope_stack.rbegin(); it != scope_stack.rend(); ++it) {
            if (it->type == st) return &(*it);
        }
        return nullptr;
    };

    auto current_gradient_scope = [&]() -> ParseScope* {
        for (auto it = scope_stack.rbegin(); it != scope_stack.rend(); ++it) {
            if (it->type == ScopeType::LinearGradient || it->type == ScopeType::RadialGradient) {
                return &(*it);
            }
        }
        return nullptr;
    };

    auto push_element_to_parent = [&](SvgElement elem) {
        if (!elem.id.empty()) {
            doc.add_defined_element(elem.id, elem);
        }

        if (auto* c_scope = find_enclosing_scope(ScopeType::ClipPath)) {
            if (elem.type == SvgElementType::Path) {
                Path transformed = elem.path;
                transformed.apply_transform(elem.transform);
                c_scope->clip_path.combined_path.add_path(transformed);
                c_scope->clip_path.clip_rule = elem.style.clip_rule;
                c_scope->clip_path.elements.push_back(std::move(elem));
            }
            return;
        }
        if (auto* m_scope = find_enclosing_scope(ScopeType::Mask)) {
            m_scope->mask.elements.push_back(std::move(elem));
            return;
        }
        if (auto* p_scope = find_enclosing_scope(ScopeType::Pattern)) {
            p_scope->pattern.elements.push_back(std::move(elem));
            return;
        }
        if (auto* mk_scope = find_enclosing_scope(ScopeType::Marker)) {
            mk_scope->marker.elements.push_back(std::move(elem));
            return;
        }

        // Check if inside a Group or Symbol
        for (auto it = scope_stack.rbegin(); it != scope_stack.rend(); ++it) {
            if (it->type == ScopeType::Group || it->type == ScopeType::Symbol) {
                it->group_elem.children.push_back(std::move(elem));
                return;
            }
            if (it->type == ScopeType::Defs) {
                return;
            }
        }

        if (!in_defs()) {
            doc.add_element(std::move(elem));
        }
    };

    while (pos < xml.size()) {
        size_t tag_start = xml.find('<', pos);

        // Capture text content between tags if inside <style> or <text>/<tspan>/<textPath>
        if (tag_start != pos && !scope_stack.empty()) {
            std::string_view raw_text = xml.substr(pos, (tag_start == std::string_view::npos ? xml.size() : tag_start) - pos);
            auto& top = scope_stack.back();
            if (top.type == ScopeType::Style) {
                parse_css_stylesheet(raw_text, css_rules, doc.root_css_vars_);
            } else if (top.type == ScopeType::Text || top.type == ScopeType::TSpan || top.type == ScopeType::TextPath) {
                std::string decoded = decode_xml_entities(raw_text);
                // Collapse consecutive whitespace per SVG xml:space="default"
                std::string collapsed;
                bool last_space = false;
                for (char c : decoded) {
                    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                        if (!last_space) {
                            collapsed.push_back(' ');
                            last_space = true;
                        }
                    } else {
                        collapsed.push_back(c);
                        last_space = false;
                    }
                }
                std::string_view trimmed_t = trim(collapsed);
                if (!trimmed_t.empty()) {
                    if (top.type == ScopeType::Text) {
                        SvgTextSpan sp;
                        sp.text = std::string(trimmed_t);
                        sp.style = top.style;
                        top.text_elem.text_spans.push_back(std::move(sp));
                    } else {
                        top.active_span.text += std::string(trimmed_t);
                    }
                }
            }
        }

        if (tag_start == std::string_view::npos) break;

        // Skip comments
        if (xml.substr(tag_start, 4) == "<!--") {
            size_t c_end = xml.find("-->", tag_start + 4);
            pos = (c_end != std::string_view::npos) ? c_end + 3 : xml.size();
            continue;
        }
        // Handle <![CDATA[ ... ]]>
        if (xml.substr(tag_start, 9) == "<![CDATA[") {
            size_t c_end = xml.find("]]>", tag_start + 9);
            if (c_end != std::string_view::npos && !scope_stack.empty() && scope_stack.back().type == ScopeType::Style) {
                parse_css_stylesheet(xml.substr(tag_start + 9, c_end - (tag_start + 9)), css_rules, doc.root_css_vars_);
            }
            pos = (c_end != std::string_view::npos) ? c_end + 3 : xml.size();
            continue;
        }

        size_t tag_end = xml.find('>', tag_start);
        if (tag_end == std::string_view::npos) break;

        std::string_view full_tag = xml.substr(tag_start + 1, tag_end - tag_start - 1);
        pos = tag_end + 1;
        full_tag = trim(full_tag);

        if (full_tag.empty() || full_tag.front() == '?' || full_tag.front() == '!') continue;

        bool is_closing = (full_tag.front() == '/');
        if (is_closing) {
            std::string_view close_name = trim(full_tag.substr(1));
            if (!scope_stack.empty()) {
                auto& top = scope_stack.back();
                if ((close_name == "linearGradient" || close_name == "radialGradient") && top.gradient) {
                    if (!top.gradient->id.empty()) {
                        doc.add_gradient(std::move(*top.gradient));
                    }
                    scope_stack.pop_back();
                } else if (close_name == "clipPath" && top.type == ScopeType::ClipPath) {
                    if (!top.clip_path.id.empty()) {
                        doc.add_rich_clip_path(std::move(top.clip_path));
                    }
                    scope_stack.pop_back();
                } else if (close_name == "mask" && top.type == ScopeType::Mask) {
                    if (!top.mask.id.empty()) {
                        doc.add_mask(std::move(top.mask));
                    }
                    scope_stack.pop_back();
                } else if (close_name == "pattern" && top.type == ScopeType::Pattern) {
                    if (!top.pattern.id.empty()) {
                        doc.add_pattern(std::move(top.pattern));
                    }
                    scope_stack.pop_back();
                } else if (close_name == "marker" && top.type == ScopeType::Marker) {
                    if (!top.marker.id.empty()) {
                        doc.add_marker(std::move(top.marker));
                    }
                    scope_stack.pop_back();
                } else if (close_name == "feMerge" && top.type == ScopeType::Filter && top.in_fe_merge) {
                    top.filter.primitives.push_back(std::move(top.current_filter_prim));
                    top.in_fe_merge = false;
                } else if (close_name == "filter" && top.type == ScopeType::Filter) {
                    if (!top.filter.id.empty()) {
                        doc.add_filter(std::move(top.filter));
                    }
                    scope_stack.pop_back();
                } else if ((close_name == "tspan" || close_name == "textPath") &&
                           (top.type == ScopeType::TSpan || top.type == ScopeType::TextPath)) {
                    SvgTextSpan finished_span = std::move(top.active_span);
                    scope_stack.pop_back();
                    if (auto* t_scope = find_enclosing_scope(ScopeType::Text)) {
                        if (!finished_span.text.empty()) {
                            t_scope->text_elem.text_spans.push_back(std::move(finished_span));
                        }
                    }
                } else if (close_name == "text" && top.type == ScopeType::Text) {
                    SvgElement finished_text = std::move(top.text_elem);
                    scope_stack.pop_back();
                    push_element_to_parent(std::move(finished_text));
                } else if ((close_name == "g" || close_name == "symbol" || close_name == "a" || (close_name == "svg" && scope_stack.size() > 1)) &&
                           (top.type == ScopeType::Group || top.type == ScopeType::Symbol)) {
                    SvgElement finished_group = std::move(top.group_elem);
                    bool was_symbol = (top.type == ScopeType::Symbol);
                    scope_stack.pop_back();
                    if (was_symbol) {
                        if (!finished_group.id.empty()) {
                            std::string sym_id = finished_group.id;
                            doc.add_defined_element(std::move(sym_id), std::move(finished_group));
                        }
                    } else {
                        // Flatten simple passthrough groups into top-level elements while keeping complex/isolated groups hierarchical
                        bool needs_group_node = (!finished_group.id.empty() ||
                                                 finished_group.style.opacity < 0.999f ||
                                                 !finished_group.style.clip_path_url.empty() ||
                                                 !finished_group.style.mask_url.empty() ||
                                                 !finished_group.style.filter_url.empty() ||
                                                 finished_group.style.blend_mode != BlendMode::SourceOver ||
                                                 finished_group.style.display_none ||
                                                 finished_group.group_view_box.has_value());
                        if (needs_group_node) {
                            push_element_to_parent(std::move(finished_group));
                        } else {
                            for (auto& ch : finished_group.children) {
                                push_element_to_parent(std::move(ch));
                            }
                        }
                    }
                } else if (close_name == "style" || close_name == "defs" || close_name == "svg") {
                    if (scope_stack.size() > 1) {
                        scope_stack.pop_back();
                    }
                }
            }
            continue;
        }

        bool is_self_closing = false;
        if (!full_tag.empty() && full_tag.back() == '/') {
            is_self_closing = true;
            full_tag.remove_suffix(1);
            full_tag = trim(full_tag);
        }

        size_t space_pos = full_tag.find_first_of(" \t\r\n/");
        std::string_view tag_name = (space_pos != std::string_view::npos)
            ? full_tag.substr(0, space_pos)
            : full_tag;

        std::string_view attr_str = (space_pos != std::string_view::npos)
            ? full_tag.substr(space_pos)
            : std::string_view();

        auto attrs = parse_attributes(attr_str);
        const auto& cur_scope = scope_stack.back();

        if (tag_name == "style") {
            if (!is_self_closing) {
                scope_stack.push_back(ParseScope{ .type = ScopeType::Style, .tag_name = "style", .style = cur_scope.style });
            }
            continue;
        }

        float vb_w_ref = doc.view_box().width() > 0.0f ? doc.view_box().width() : 100.0f;
        float vb_h_ref = doc.view_box().height() > 0.0f ? doc.view_box().height() : 100.0f;
        float vb_diag_ref = std::sqrt(vb_w_ref * vb_w_ref + vb_h_ref * vb_h_ref) / 1.41421356f;

        if (tag_name == "svg" && scope_stack.size() == 1) {
            if (attrs.count("viewBox")) {
                auto nums = parse_number_list(attrs["viewBox"]);
                if (nums.size() >= 4) {
                    if (auto r = Rect::from_xywh(nums[0], nums[1], nums[2], nums[3])) {
                        doc.set_view_box(*r);
                        vb_w_ref = r->width();
                        vb_h_ref = r->height();
                    }
                }
            }
            float w = attrs.count("width") ? parse_float(attrs["width"], doc.view_box().width(), vb_w_ref) : doc.view_box().width();
            float h = attrs.count("height") ? parse_float(attrs["height"], doc.view_box().height(), vb_h_ref) : doc.view_box().height();
            doc.set_dimensions(w, h);
            if (!attrs.count("viewBox") && w > 0.0f && h > 0.0f) {
                if (auto r = Rect::from_xywh(0.0f, 0.0f, w, h)) doc.set_view_box(*r);
            }
            if (attrs.count("preserveAspectRatio")) {
                doc.set_preserve_aspect_ratio(parse_preserve_aspect_ratio(attrs["preserveAspectRatio"]));
            }
            // Apply root SVG presentation attributes (fill, stroke, color, style, etc.)
            for (const auto& [k, v] : attrs) {
                if (k != "viewBox" && k != "width" && k != "height" && k != "style" && k != "xmlns") {
                    apply_style_property(scope_stack.back().style, k, v, 1.0f, doc.root_css_vars_);
                }
            }
            if (attrs.count("style")) {
                apply_style_string(scope_stack.back().style, attrs["style"], 1.0f, doc.root_css_vars_, &doc.root_css_vars_);
            }
            continue;
        }

        if (tag_name == "defs") {
            if (!is_self_closing) {
                scope_stack.push_back(ParseScope{ .type = ScopeType::Defs, .tag_name = "defs", .style = cur_scope.style, .transform = cur_scope.transform });
            }
            continue;
        }

        if (tag_name == "linearGradient" || tag_name == "radialGradient") {
            SvgGradient grad;
            grad.kind = (tag_name == "linearGradient") ? SvgGradient::Kind::Linear : SvgGradient::Kind::Radial;
            if (attrs.count("id")) grad.id = attrs["id"];
            if (attrs.count("href")) grad.href_id = extract_url_id(attrs["href"]);
            else if (attrs.count("xlink:href")) grad.href_id = extract_url_id(attrs["xlink:href"]);

            if (attrs.count("gradientUnits")) {
                grad.has_units = true;
                if (attrs["gradientUnits"] == "userSpaceOnUse") grad.units = SvgGradientUnits::UserSpaceOnUse;
                else grad.units = SvgGradientUnits::ObjectBoundingBox;
            }
            if (attrs.count("spreadMethod")) {
                grad.has_spread = true;
                auto sm = attrs["spreadMethod"];
                if (sm == "reflect") grad.spread_mode = SpreadMode::Reflect;
                else if (sm == "repeat") grad.spread_mode = SpreadMode::Repeat;
                else grad.spread_mode = SpreadMode::Pad;
            }
            if (attrs.count("gradientTransform")) {
                grad.has_transform = true;
                grad.gradient_transform = parse_transform(attrs["gradientTransform"]);
            }
            if (attrs.count("color-interpolation")) {
                grad.linear_interpolation = iequals(attrs["color-interpolation"], "linearRGB");
            }

            if (grad.kind == SvgGradient::Kind::Linear) {
                if (attrs.count("x1")) { grad.x1 = parse_coord(attrs["x1"], 0.0f); grad.has_x1 = true; }
                if (attrs.count("y1")) { grad.y1 = parse_coord(attrs["y1"], 0.0f); grad.has_y1 = true; }
                if (attrs.count("x2")) { grad.x2 = parse_coord(attrs["x2"], 1.0f); grad.has_x2 = true; }
                if (attrs.count("y2")) { grad.y2 = parse_coord(attrs["y2"], 0.0f); grad.has_y2 = true; }
            } else {
                if (attrs.count("cx")) { grad.cx = parse_coord(attrs["cx"], 0.5f); grad.has_cx = true; }
                if (attrs.count("cy")) { grad.cy = parse_coord(attrs["cy"], 0.5f); grad.has_cy = true; }
                if (attrs.count("r"))  { grad.r  = parse_coord(attrs["r"],  0.5f); grad.has_r  = true; }
                if (attrs.count("fx")) { grad.fx = parse_coord(attrs["fx"], grad.cx); grad.has_fx = true; }
                else { grad.fx = grad.cx; }
                if (attrs.count("fy")) { grad.fy = parse_coord(attrs["fy"], grad.cy); grad.has_fy = true; }
                else { grad.fy = grad.cy; }
                if (attrs.count("fr")) { grad.fr = parse_coord(attrs["fr"], 0.0f); grad.has_fr = true; }
            }

            if (is_self_closing) {
                if (!grad.id.empty()) doc.add_gradient(std::move(grad));
            } else {
                scope_stack.push_back(ParseScope{
                    .type = (grad.kind == SvgGradient::Kind::Linear) ? ScopeType::LinearGradient : ScopeType::RadialGradient,
                    .tag_name = std::string(tag_name),
                    .id = grad.id,
                    .gradient = std::move(grad)
                });
            }
            continue;
        }

        if (tag_name == "stop") {
            auto* g_scope = current_gradient_scope();
            if (g_scope && g_scope->gradient) {
                float offset = 0.0f;
                if (attrs.count("offset")) offset = parse_coord(attrs["offset"], 0.0f);
                Color stop_col = cur_scope.style.current_color;
                float stop_opacity = 1.0f;

                if (attrs.count("stop-color")) {
                    auto c = parse_color(attrs["stop-color"], cur_scope.style.current_color);
                    if (c) stop_col = *c;
                } else {
                    stop_col = Color::BLACK;
                }
                if (attrs.count("stop-opacity")) {
                    stop_opacity = parse_coord(attrs["stop-opacity"], 1.0f, 1.0f);
                }

                if (attrs.count("style")) {
                    std::string_view style_str = attrs["style"];
                    size_t s_pos = 0;
                    while (s_pos < style_str.size()) {
                        size_t sc = style_str.find(';', s_pos);
                        if (sc == std::string_view::npos) sc = style_str.size();
                        std::string_view prop = trim(style_str.substr(s_pos, sc - s_pos));
                        s_pos = sc + 1;
                        size_t colon = prop.find(':');
                        if (colon != std::string_view::npos) {
                            std::string_view k = trim(prop.substr(0, colon));
                            std::string_view v = trim(prop.substr(colon + 1));
                            if (k == "stop-color") {
                                auto c = parse_color(v, cur_scope.style.current_color);
                                if (c) stop_col = *c;
                            } else if (k == "stop-opacity") {
                                stop_opacity = parse_coord(v, 1.0f, 1.0f);
                            }
                        }
                    }
                }

                stop_col.apply_opacity(stop_opacity);
                g_scope->gradient->stops.push_back(GradientStop::create(offset, stop_col));
            }
            continue;
        }

        if (tag_name == "clipPath") {
            SvgClipPath cp;
            if (attrs.count("id")) cp.id = attrs["id"];
            if (attrs.count("clipPathUnits") && attrs["clipPathUnits"] == "objectBoundingBox") {
                cp.units = SvgUnits::ObjectBoundingBox;
            }
            if (attrs.count("transform")) cp.transform = parse_transform(attrs["transform"]);
            if (!is_self_closing) {
                scope_stack.push_back(ParseScope{
                    .type = ScopeType::ClipPath,
                    .tag_name = "clipPath",
                    .id = cp.id,
                    .style = cur_scope.style,
                    .transform = cp.transform,
                    .clip_path = std::move(cp)
                });
            }
            continue;
        }

        if (tag_name == "mask") {
            SvgMask m;
            if (attrs.count("id")) m.id = attrs["id"];
            if (attrs.count("maskUnits") && attrs["maskUnits"] == "userSpaceOnUse") m.units = SvgUnits::UserSpaceOnUse;
            if (attrs.count("maskContentUnits") && attrs["maskContentUnits"] == "objectBoundingBox") m.content_units = SvgUnits::ObjectBoundingBox;
            if (attrs.count("mask-type") && attrs["mask-type"] == "alpha") m.is_alpha_mask = true;
            if (attrs.count("style") && attrs["style"].find("mask-type:alpha") != std::string::npos) m.is_alpha_mask = true;
            if (attrs.count("style") && attrs["style"].find("mask-type: alpha") != std::string::npos) m.is_alpha_mask = true;
            if (!is_self_closing) {
                scope_stack.push_back(ParseScope{
                    .type = ScopeType::Mask,
                    .tag_name = "mask",
                    .id = m.id,
                    .style = cur_scope.style,
                    .transform = Transform::identity(),
                    .mask = std::move(m)
                });
            }
            continue;
        }

        if (tag_name == "pattern") {
            SvgPattern pat;
            if (attrs.count("id")) pat.id = attrs["id"];
            if (attrs.count("href")) pat.href_id = extract_url_id(attrs["href"]);
            else if (attrs.count("xlink:href")) pat.href_id = extract_url_id(attrs["xlink:href"]);
            if (attrs.count("patternUnits") && attrs["patternUnits"] == "userSpaceOnUse") pat.units = SvgUnits::UserSpaceOnUse;
            if (attrs.count("patternContentUnits") && attrs["patternContentUnits"] == "objectBoundingBox") pat.content_units = SvgUnits::ObjectBoundingBox;
            if (attrs.count("patternTransform")) pat.pattern_transform = parse_transform(attrs["patternTransform"]);
            pat.x = attrs.count("x") ? parse_coord(attrs["x"], 0.0f) : 0.0f;
            pat.y = attrs.count("y") ? parse_coord(attrs["y"], 0.0f) : 0.0f;
            pat.width = attrs.count("width") ? parse_coord(attrs["width"], 0.0f) : 0.0f;
            pat.height = attrs.count("height") ? parse_coord(attrs["height"], 0.0f) : 0.0f;
            if (attrs.count("viewBox")) {
                auto nums = parse_number_list(attrs["viewBox"]);
                if (nums.size() >= 4) pat.view_box = Rect::from_xywh(nums[0], nums[1], nums[2], nums[3]);
            }
            if (attrs.count("preserveAspectRatio")) {
                pat.preserve_aspect_ratio = parse_preserve_aspect_ratio(attrs["preserveAspectRatio"]);
            }
            if (is_self_closing) {
                if (!pat.id.empty()) doc.add_pattern(std::move(pat));
            } else {
                scope_stack.push_back(ParseScope{
                    .type = ScopeType::Pattern,
                    .tag_name = "pattern",
                    .id = pat.id,
                    .style = cur_scope.style,
                    .transform = Transform::identity(),
                    .pattern = std::move(pat)
                });
            }
            continue;
        }

        if (tag_name == "marker") {
            SvgMarker mk;
            if (attrs.count("id")) mk.id = attrs["id"];
            if (attrs.count("viewBox")) {
                auto nums = parse_number_list(attrs["viewBox"]);
                if (nums.size() >= 4) {
                    if (auto r = Rect::from_xywh(nums[0], nums[1], nums[2], nums[3])) {
                        mk.view_box = *r;
                        mk.has_view_box = true;
                    }
                }
            }
            mk.ref_x = attrs.count("refX") ? parse_float(attrs["refX"], 0.0f) : 0.0f;
            mk.ref_y = attrs.count("refY") ? parse_float(attrs["refY"], 0.0f) : 0.0f;
            mk.marker_width = attrs.count("markerWidth") ? parse_float(attrs["markerWidth"], 3.0f) : 3.0f;
            mk.marker_height = attrs.count("markerHeight") ? parse_float(attrs["markerHeight"], 3.0f) : 3.0f;
            if (attrs.count("markerUnits") && attrs["markerUnits"] == "userSpaceOnUse") {
                mk.stroke_width_units = false;
            }
            if (attrs.count("orient")) {
                std::string_view ov = trim(attrs["orient"]);
                if (ov == "auto") mk.orient_mode = SvgMarker::OrientMode::Auto;
                else if (ov == "auto-start-reverse") mk.orient_mode = SvgMarker::OrientMode::AutoStartReverse;
                else {
                    mk.orient_mode = SvgMarker::OrientMode::Angle;
                    mk.orient_angle = parse_float(ov, 0.0f);
                }
            }
            if (attrs.count("preserveAspectRatio")) {
                mk.preserve_aspect_ratio = parse_preserve_aspect_ratio(attrs["preserveAspectRatio"]);
            }
            if (!is_self_closing) {
                scope_stack.push_back(ParseScope{
                    .type = ScopeType::Marker,
                    .tag_name = "marker",
                    .id = mk.id,
                    .style = cur_scope.style,
                    .transform = Transform::identity(),
                    .marker = std::move(mk)
                });
            }
            continue;
        }

        if (tag_name == "filter") {
            SvgFilter flt;
            if (attrs.count("id")) flt.id = attrs["id"];
            if (attrs.count("filterUnits") && attrs["filterUnits"] == "userSpaceOnUse") flt.filter_units = SvgUnits::UserSpaceOnUse;
            if (attrs.count("primitiveUnits") && attrs["primitiveUnits"] == "objectBoundingBox") flt.primitive_units = SvgUnits::ObjectBoundingBox;
            if (!is_self_closing) {
                scope_stack.push_back(ParseScope{
                    .type = ScopeType::Filter,
                    .tag_name = "filter",
                    .id = flt.id,
                    .style = cur_scope.style,
                    .filter = std::move(flt)
                });
            }
            continue;
        }

        if (auto* f_scope = find_enclosing_scope(ScopeType::Filter)) {
            if (tag_name == "feGaussianBlur") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::GaussianBlur;
                if (attrs.count("in")) p.in1 = attrs["in"];
                if (attrs.count("result")) p.result = attrs["result"];
                if (attrs.count("stdDeviation")) {
                    auto devs = parse_number_list(attrs["stdDeviation"]);
                    if (!devs.empty()) {
                        p.std_dev_x = devs[0];
                        p.std_dev_y = (devs.size() > 1) ? devs[1] : devs[0];
                    }
                }
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feOffset") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::Offset;
                if (attrs.count("in")) p.in1 = attrs["in"];
                if (attrs.count("result")) p.result = attrs["result"];
                p.dx = attrs.count("dx") ? parse_float(attrs["dx"], 0.0f) : 0.0f;
                p.dy = attrs.count("dy") ? parse_float(attrs["dy"], 0.0f) : 0.0f;
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feFlood") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::Flood;
                if (attrs.count("result")) p.result = attrs["result"];
                if (attrs.count("flood-color")) {
                    if (auto c = parse_color(attrs["flood-color"], cur_scope.style.current_color)) p.flood_color = *c;
                }
                if (attrs.count("flood-opacity")) p.flood_opacity = parse_coord(attrs["flood-opacity"], 1.0f, 1.0f);
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feDropShadow") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::DropShadow;
                if (attrs.count("in")) p.in1 = attrs["in"];
                if (attrs.count("result")) p.result = attrs["result"];
                p.dx = attrs.count("dx") ? parse_float(attrs["dx"], 2.0f) : 2.0f;
                p.dy = attrs.count("dy") ? parse_float(attrs["dy"], 2.0f) : 2.0f;
                if (attrs.count("stdDeviation")) {
                    auto devs = parse_number_list(attrs["stdDeviation"]);
                    if (!devs.empty()) {
                        p.std_dev_x = devs[0];
                        p.std_dev_y = (devs.size() > 1) ? devs[1] : devs[0];
                    }
                } else {
                    p.std_dev_x = p.std_dev_y = 2.0f;
                }
                if (attrs.count("flood-color")) {
                    if (auto c = parse_color(attrs["flood-color"], cur_scope.style.current_color)) p.flood_color = *c;
                }
                if (attrs.count("flood-opacity")) p.flood_opacity = parse_coord(attrs["flood-opacity"], 1.0f, 1.0f);
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feColorMatrix") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::ColorMatrix;
                if (attrs.count("in")) p.in1 = attrs["in"];
                if (attrs.count("result")) p.result = attrs["result"];
                std::string mtype = attrs.count("type") ? attrs["type"] : "matrix";
                if (mtype == "luminanceToAlpha") {
                    p.luminance_to_alpha = true;
                } else if (mtype == "saturate") {
                    float s = attrs.count("values") ? parse_float(attrs["values"], 1.0f) : 1.0f;
                    p.color_matrix = effects::ColorMatrix::saturation(s);
                } else if (mtype == "hueRotate") {
                    float deg = attrs.count("values") ? parse_float(attrs["values"], 0.0f) : 0.0f;
                    p.color_matrix = effects::ColorMatrix::hue_rotate(deg);
                } else if (attrs.count("values")) {
                    auto vals = parse_number_list(attrs["values"]);
                    std::array<float, 20> arr{};
                    for (size_t i = 0; i < std::min<size_t>(20, vals.size()); ++i) arr[i] = vals[i];
                    if (vals.size() < 20) arr[0] = arr[6] = arr[12] = arr[18] = 1.0f;
                    p.color_matrix = effects::ColorMatrix(arr);
                }
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feBlend") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::Blend;
                if (attrs.count("in")) p.in1 = attrs["in"];
                if (attrs.count("in2")) p.in2 = attrs["in2"];
                if (attrs.count("result")) p.result = attrs["result"];
                if (attrs.count("mode")) p.blend_mode = parse_blend_mode(attrs["mode"]);
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feComposite") {
                SvgFilterPrimitive p;
                p.type = SvgFilterPrimitive::Type::Composite;
                if (attrs.count("in")) p.in1 = attrs["in"];
                if (attrs.count("in2")) p.in2 = attrs["in2"];
                if (attrs.count("result")) p.result = attrs["result"];
                std::string op = attrs.count("operator") ? attrs["operator"] : "over";
                if (op == "in") p.composite_op = SvgFilterPrimitive::CompositeOp::In;
                else if (op == "out") p.composite_op = SvgFilterPrimitive::CompositeOp::Out;
                else if (op == "atop") p.composite_op = SvgFilterPrimitive::CompositeOp::Atop;
                else if (op == "xor") p.composite_op = SvgFilterPrimitive::CompositeOp::Xor;
                else if (op == "arithmetic") {
                    p.composite_op = SvgFilterPrimitive::CompositeOp::Arithmetic;
                    p.k1 = attrs.count("k1") ? parse_float(attrs["k1"], 0.0f) : 0.0f;
                    p.k2 = attrs.count("k2") ? parse_float(attrs["k2"], 0.0f) : 0.0f;
                    p.k3 = attrs.count("k3") ? parse_float(attrs["k3"], 0.0f) : 0.0f;
                    p.k4 = attrs.count("k4") ? parse_float(attrs["k4"], 0.0f) : 0.0f;
                } else {
                    p.composite_op = SvgFilterPrimitive::CompositeOp::Over;
                }
                f_scope->filter.primitives.push_back(std::move(p));
                continue;
            }
            if (tag_name == "feMerge") {
                f_scope->current_filter_prim = SvgFilterPrimitive{};
                f_scope->current_filter_prim.type = SvgFilterPrimitive::Type::Merge;
                if (attrs.count("result")) f_scope->current_filter_prim.result = attrs["result"];
                f_scope->in_fe_merge = !is_self_closing;
                continue;
            }
            if (tag_name == "feMergeNode" && f_scope->in_fe_merge) {
                if (attrs.count("in")) {
                    f_scope->current_filter_prim.merge_inputs.push_back(attrs["in"]);
                }
                continue;
            }
        }

        // Compute cascaded styles & transform for element/group
        SvgPaintStyle elem_style = cur_scope.style;
        // Non-inherited properties reset to defaults for each element
        elem_style.opacity = 1.0f;
        elem_style.clip_path_url.clear();
        elem_style.mask_url.clear();
        elem_style.filter_url.clear();
        elem_style.blend_mode = BlendMode::SourceOver;
        elem_style.display_none = cur_scope.style.display_none;

        std::string elem_id = attrs.count("id") ? attrs["id"] : "";
        std::string elem_class = attrs.count("class") ? attrs["class"] : "";

        // 1. Presentation attributes (lowest specificity)
        for (const auto& [k, v] : attrs) {
            if (k != "style" && k != "id" && k != "class" && k != "transform" && k != "d" && k != "points") {
                apply_style_property(elem_style, k, v, 1.0f, doc.root_css_vars_);
            }
        }

        // 2. CSS <style> rules ordered by specificity
        if (!css_rules.empty()) {
            std::vector<const SvgCssRule*> matched;
            for (const auto& r : css_rules) {
                if (selector_matches(r.selector, tag_name, elem_id, elem_class)) {
                    matched.push_back(&r);
                }
            }
            std::stable_sort(matched.begin(), matched.end(), [](const SvgCssRule* a, const SvgCssRule* b) {
                return a->specificity < b->specificity;
            });
            for (const auto* r : matched) {
                apply_style_string(elem_style, r->declarations, 1.0f, doc.root_css_vars_);
            }
        }

        // 3. Inline style="..." attribute (highest specificity)
        if (attrs.count("style")) {
            apply_style_string(elem_style, attrs["style"], 1.0f, doc.root_css_vars_);
        }

        Transform local_ts = Transform::identity();
        if (attrs.count("transform")) {
            local_ts = parse_transform(attrs["transform"]);
        }

        // Parent transform is only accumulated when flattening into leaf elements;
        // Groups apply their own transform or pass it to children.
        Transform elem_ts = cur_scope.transform.pre_concat(local_ts);

        if (tag_name == "g" || tag_name == "symbol" || tag_name == "a" || (tag_name == "svg" && scope_stack.size() > 1)) {
            if (!is_self_closing) {
                ParseScope gs;
                gs.type = (tag_name == "symbol") ? ScopeType::Symbol : ScopeType::Group;
                gs.tag_name = std::string(tag_name);
                gs.id = elem_id;
                gs.style = elem_style;
                // Keep group's children in local coordinates if this group will be preserved as a Group node
                bool will_preserve_group = (tag_name == "symbol" || !elem_id.empty() ||
                                            elem_style.opacity < 0.999f ||
                                            !elem_style.clip_path_url.empty() ||
                                            !elem_style.mask_url.empty() ||
                                            !elem_style.filter_url.empty() ||
                                            elem_style.blend_mode != BlendMode::SourceOver ||
                                            elem_style.display_none ||
                                            tag_name == "svg");
                gs.transform = will_preserve_group ? Transform::identity() : elem_ts;
                gs.group_elem.type = SvgElementType::Group;
                gs.group_elem.id = elem_id;
                gs.group_elem.class_name = elem_class;
                gs.group_elem.style = elem_style;
                gs.group_elem.transform = elem_ts;

                if (tag_name == "svg" || tag_name == "symbol") {
                    if (attrs.count("viewBox")) {
                        auto nums = parse_number_list(attrs["viewBox"]);
                        if (nums.size() >= 4) {
                            gs.group_elem.group_view_box = Rect::from_xywh(nums[0], nums[1], nums[2], nums[3]);
                        }
                    }
                    if (tag_name == "svg") {
                        float nx = attrs.count("x") ? parse_float(attrs["x"], 0.0f, vb_w_ref) : 0.0f;
                        float ny = attrs.count("y") ? parse_float(attrs["y"], 0.0f, vb_h_ref) : 0.0f;
                        float nw = attrs.count("width") ? parse_float(attrs["width"], vb_w_ref, vb_w_ref) : vb_w_ref;
                        float nh = attrs.count("height") ? parse_float(attrs["height"], vb_h_ref, vb_h_ref) : vb_h_ref;
                        gs.group_elem.group_viewport = Rect::from_xywh(nx, ny, nw, nh);
                    }
                    if (attrs.count("preserveAspectRatio")) {
                        gs.group_elem.preserve_aspect_ratio = parse_preserve_aspect_ratio(attrs["preserveAspectRatio"]);
                    }
                }

                // Inside a non-preserved group, children inherit cascaded opacity
                if (!will_preserve_group) {
                    gs.style.opacity = cur_scope.style.opacity * elem_style.opacity;
                } else {
                    gs.style.opacity = 1.0f;
                }

                scope_stack.push_back(std::move(gs));
            }
            continue;
        }

        // For leaf elements inside a flattened group, multiply by scope opacity
        elem_style.opacity *= cur_scope.style.opacity;

        if (tag_name == "text") {
            SvgElement te;
            te.type = SvgElementType::Text;
            te.id = elem_id;
            te.class_name = elem_class;
            te.style = elem_style;
            te.transform = elem_ts;
            te.text_x = attrs.count("x") ? parse_float(attrs["x"], 0.0f, vb_w_ref, elem_style.font_size) : 0.0f;
            te.text_y = attrs.count("y") ? parse_float(attrs["y"], 0.0f, vb_h_ref, elem_style.font_size) : 0.0f;
            te.text_dx = attrs.count("dx") ? parse_float(attrs["dx"], 0.0f, vb_w_ref, elem_style.font_size) : 0.0f;
            te.text_dy = attrs.count("dy") ? parse_float(attrs["dy"], 0.0f, vb_h_ref, elem_style.font_size) : 0.0f;

            if (!is_self_closing) {
                ParseScope ts;
                ts.type = ScopeType::Text;
                ts.tag_name = "text";
                ts.id = elem_id;
                ts.style = elem_style;
                ts.transform = elem_ts;
                ts.text_elem = std::move(te);
                scope_stack.push_back(std::move(ts));
            }
            continue;
        }

        if (tag_name == "tspan" || tag_name == "textPath") {
            if (!is_self_closing) {
                ParseScope sps;
                sps.type = (tag_name == "textPath") ? ScopeType::TextPath : ScopeType::TSpan;
                sps.tag_name = std::string(tag_name);
                sps.style = elem_style;
                sps.active_span.style = elem_style;
                if (attrs.count("x")) sps.active_span.x = parse_float(attrs["x"], 0.0f, vb_w_ref, elem_style.font_size);
                if (attrs.count("y")) sps.active_span.y = parse_float(attrs["y"], 0.0f, vb_h_ref, elem_style.font_size);
                if (attrs.count("dx")) sps.active_span.dx = parse_float(attrs["dx"], 0.0f, vb_w_ref, elem_style.font_size);
                if (attrs.count("dy")) sps.active_span.dy = parse_float(attrs["dy"], 0.0f, vb_h_ref, elem_style.font_size);
                if (attrs.count("rotate")) sps.active_span.rotate = parse_float(attrs["rotate"], 0.0f);
                if (tag_name == "textPath") {
                    if (attrs.count("href")) sps.active_span.path_href = extract_url_id(attrs["href"]);
                    else if (attrs.count("xlink:href")) sps.active_span.path_href = extract_url_id(attrs["xlink:href"]);
                    if (attrs.count("startOffset")) {
                        std::string_view so = trim(attrs["startOffset"]);
                        if (so.ends_with("%")) {
                            sps.active_span.start_offset_percent = true;
                            sps.active_span.start_offset = parse_float(so.substr(0, so.size() - 1), 0.0f) * 0.01f;
                        } else {
                            sps.active_span.start_offset = parse_float(so, 0.0f);
                        }
                    }
                }
                scope_stack.push_back(std::move(sps));
            }
            continue;
        }

        if (tag_name == "use") {
            std::string target_id;
            if (attrs.count("href")) target_id = extract_url_id(attrs["href"]);
            else if (attrs.count("xlink:href")) target_id = extract_url_id(attrs["xlink:href"]);

            float ux = attrs.count("x") ? parse_float(attrs["x"], 0.0f, vb_w_ref) : 0.0f;
            float uy = attrs.count("y") ? parse_float(attrs["y"], 0.0f, vb_h_ref) : 0.0f;

            SvgElement use_elem;
            use_elem.type = SvgElementType::Use;
            use_elem.id = elem_id;
            use_elem.class_name = elem_class;
            use_elem.style = elem_style;
            // Detect whether fill/stroke were explicitly specified on <use> or inherited from root default
            if (!attrs.count("fill") && (!attrs.count("style") || attrs["style"].find("fill") == std::string::npos)) {
                if (cur_scope.type == ScopeType::Svg && !cur_scope.style.fill_is_current_color) {
                    use_elem.style.fill = std::nullopt;
                }
            }
            use_elem.transform = elem_ts;
            use_elem.use_href = std::move(target_id);
            use_elem.use_x = ux;
            use_elem.use_y = uy;
            if (attrs.count("width")) use_elem.use_width = parse_float(attrs["width"], 0.0f, vb_w_ref);
            if (attrs.count("height")) use_elem.use_height = parse_float(attrs["height"], 0.0f, vb_h_ref);

            push_element_to_parent(std::move(use_elem));
            continue;
        }

        if (tag_name == "image") {
            std::string href;
            if (attrs.count("href")) href = attrs["href"];
            else if (attrs.count("xlink:href")) href = attrs["xlink:href"];

            float x = attrs.count("x") ? parse_float(attrs["x"], 0.0f, vb_w_ref) : 0.0f;
            float y = attrs.count("y") ? parse_float(attrs["y"], 0.0f, vb_h_ref) : 0.0f;
            float w = attrs.count("width") ? parse_float(attrs["width"], 0.0f, vb_w_ref) : 0.0f;
            float h = attrs.count("height") ? parse_float(attrs["height"], 0.0f, vb_h_ref) : 0.0f;

            std::shared_ptr<Pixmap> pixmap_ptr;
            std::shared_ptr<SvgDocument> nested_svg_ptr;
            if (!href.empty()) {
                if (href.starts_with("data:")) {
                    size_t comma = href.find(',');
                    if (comma != std::string::npos) {
                        std::string_view header = std::string_view(href).substr(0, comma);
                        auto b64 = href.substr(comma + 1);
                        if (header.find("image/svg+xml") != std::string_view::npos) {
                            if (header.find(";base64") != std::string_view::npos) {
                                auto bytes = base64_decode(b64);
                                std::string_view svg_str(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                if (auto nd = SvgDocument::parse(svg_str)) {
                                    nested_svg_ptr = std::make_shared<SvgDocument>(std::move(*nd));
                                }
                            }
                        } else {
                            auto bytes = base64_decode(b64);
                            auto img_res = nisaba::image::load_image_from_memory(bytes);
                            if (img_res.has_value()) {
                                pixmap_ptr = std::make_shared<Pixmap>(std::move(*img_res));
                            }
                        }
                    }
                } else {
                    auto img_res = nisaba::image::load_image_file(href);
                    if (img_res.has_value()) {
                        pixmap_ptr = std::make_shared<Pixmap>(std::move(*img_res));
                    }
                }
            }

            if (pixmap_ptr) {
                if (w <= 0.0f) w = static_cast<float>(pixmap_ptr->width());
                if (h <= 0.0f) h = static_cast<float>(pixmap_ptr->height());
            } else if (nested_svg_ptr) {
                if (w <= 0.0f) w = nested_svg_ptr->width();
                if (h <= 0.0f) h = nested_svg_ptr->height();
            }

            auto r = Rect::from_xywh(x, y, w, h);
            Rect img_bounds = r ? *r : Rect();

            SvgElement img_elem{
                .type = SvgElementType::Image,
                .path = Path(),
                .style = elem_style,
                .transform = elem_ts,
                .id = elem_id,
                .class_name = elem_class,
                .image = pixmap_ptr,
                .nested_svg = nested_svg_ptr,
                .image_bounds = img_bounds,
            };
            if (attrs.count("preserveAspectRatio")) {
                img_elem.preserve_aspect_ratio = parse_preserve_aspect_ratio(attrs["preserveAspectRatio"]);
            }

            push_element_to_parent(std::move(img_elem));
            continue;
        }

        // Geometric shape parsing
        std::optional<Path> parsed_path;

        if (tag_name == "path" && attrs.count("d")) {
            parsed_path = PathParser::parse(attrs["d"]);
        } else if (tag_name == "rect") {
            float x = attrs.count("x") ? parse_float(attrs["x"], 0.0f, vb_w_ref) : 0.0f;
            float y = attrs.count("y") ? parse_float(attrs["y"], 0.0f, vb_h_ref) : 0.0f;
            float w = attrs.count("width") ? parse_float(attrs["width"], 0.0f, vb_w_ref) : 0.0f;
            float h = attrs.count("height") ? parse_float(attrs["height"], 0.0f, vb_h_ref) : 0.0f;
            bool has_rx = attrs.count("rx") > 0;
            bool has_ry = attrs.count("ry") > 0;
            float rx = has_rx ? parse_float(attrs["rx"], 0.0f, vb_w_ref) : 0.0f;
            float ry = has_ry ? parse_float(attrs["ry"], 0.0f, vb_h_ref) : 0.0f;
            if (has_rx && !has_ry) ry = rx;
            if (has_ry && !has_rx) rx = ry;

            auto r = Rect::from_xywh(x, y, w, h);
            if (r) {
                if (rx > 0.0f || ry > 0.0f) {
                    parsed_path = PathBuilder::from_rounded_rect(*r, rx, ry);
                } else {
                    parsed_path = PathBuilder::from_rect(*r);
                }
            }
        } else if (tag_name == "circle") {
            float cx = attrs.count("cx") ? parse_float(attrs["cx"], 0.0f, vb_w_ref) : 0.0f;
            float cy = attrs.count("cy") ? parse_float(attrs["cy"], 0.0f, vb_h_ref) : 0.0f;
            float r = attrs.count("r") ? parse_float(attrs["r"], 0.0f, vb_diag_ref) : 0.0f;
            parsed_path = PathBuilder::from_circle(cx, cy, r);
        } else if (tag_name == "ellipse") {
            float cx = attrs.count("cx") ? parse_float(attrs["cx"], 0.0f, vb_w_ref) : 0.0f;
            float cy = attrs.count("cy") ? parse_float(attrs["cy"], 0.0f, vb_h_ref) : 0.0f;
            float rx = attrs.count("rx") ? parse_float(attrs["rx"], 0.0f, vb_w_ref) : 0.0f;
            float ry = attrs.count("ry") ? parse_float(attrs["ry"], 0.0f, vb_h_ref) : 0.0f;
            auto r = Rect::from_xywh(cx - rx, cy - ry, rx * 2.0f, ry * 2.0f);
            if (r) parsed_path = PathBuilder::from_oval(*r);
        } else if (tag_name == "line") {
            float x1 = attrs.count("x1") ? parse_float(attrs["x1"], 0.0f, vb_w_ref) : 0.0f;
            float y1 = attrs.count("y1") ? parse_float(attrs["y1"], 0.0f, vb_h_ref) : 0.0f;
            float x2 = attrs.count("x2") ? parse_float(attrs["x2"], 0.0f, vb_w_ref) : 0.0f;
            float y2 = attrs.count("y2") ? parse_float(attrs["y2"], 0.0f, vb_h_ref) : 0.0f;
            PathBuilder pb;
            pb.move_to(x1, y1);
            pb.line_to(x2, y2);
            parsed_path = pb.finish();
        } else if ((tag_name == "polyline" || tag_name == "polygon") && attrs.count("points")) {
            auto pts = parse_number_list(attrs["points"]);
            if (pts.size() >= 2) {
                PathBuilder pb;
                pb.move_to(pts[0], pts[1]);
                for (size_t i = 2; i + 1 < pts.size(); i += 2) {
                    pb.line_to(pts[i], pts[i + 1]);
                }
                if (tag_name == "polygon") pb.close();
                parsed_path = pb.finish();
            }
        }

        if (parsed_path) {
            SvgElement elem{
                .type = SvgElementType::Path,
                .path = std::move(*parsed_path),
                .style = elem_style,
                .transform = elem_ts,
                .id = elem_id,
                .class_name = elem_class
            };
            push_element_to_parent(std::move(elem));
        }
    }

    // Multi-hop gradient inheritance (href="#parentGrad")
    for (auto& [id, grad] : doc.gradients_) {
        std::string cur_href = grad.href_id;
        int depth = 0;
        while (!cur_href.empty() && depth < 16) {
            auto it = doc.gradients_.find(cur_href);
            if (it == doc.gradients_.end()) break;
            const auto& parent = it->second;
            if (grad.stops.empty() && !parent.stops.empty()) grad.stops = parent.stops;
            if (!grad.has_units && parent.has_units) { grad.units = parent.units; grad.has_units = true; }
            if (!grad.has_spread && parent.has_spread) { grad.spread_mode = parent.spread_mode; grad.has_spread = true; }
            if (!grad.has_transform && parent.has_transform) { grad.gradient_transform = parent.gradient_transform; grad.has_transform = true; }
            if (grad.kind == SvgGradient::Kind::Linear && parent.kind == SvgGradient::Kind::Linear) {
                if (!grad.has_x1 && parent.has_x1) { grad.x1 = parent.x1; grad.has_x1 = true; }
                if (!grad.has_y1 && parent.has_y1) { grad.y1 = parent.y1; grad.has_y1 = true; }
                if (!grad.has_x2 && parent.has_x2) { grad.x2 = parent.x2; grad.has_x2 = true; }
                if (!grad.has_y2 && parent.has_y2) { grad.y2 = parent.y2; grad.has_y2 = true; }
            } else if (grad.kind == SvgGradient::Kind::Radial && parent.kind == SvgGradient::Kind::Radial) {
                if (!grad.has_cx && parent.has_cx) { grad.cx = parent.cx; grad.has_cx = true; }
                if (!grad.has_cy && parent.has_cy) { grad.cy = parent.cy; grad.has_cy = true; }
                if (!grad.has_r  && parent.has_r)  { grad.r  = parent.r;  grad.has_r  = true; }
                if (!grad.has_fx && parent.has_fx) { grad.fx = parent.fx; grad.has_fx = true; }
                if (!grad.has_fy && parent.has_fy) { grad.fy = parent.fy; grad.has_fy = true; }
                if (!grad.has_fr && parent.has_fr) { grad.fr = parent.fr; grad.has_fr = true; }
            }
            cur_href = parent.href_id;
            depth++;
        }
        if (!grad.has_fx) grad.fx = grad.cx;
        if (!grad.has_fy) grad.fy = grad.cy;
    }

    // Resolve top-level single-element <use> into concrete elements for backward compatibility with elements() inspection
    std::vector<SvgElement> resolved_top;
    resolved_top.reserve(doc.elements_.size());
    for (auto& el : doc.elements_) {
        if (el.type == SvgElementType::Use && !el.use_href.empty()) {
            auto it = doc.defined_elements_.find(el.use_href);
            if (it != doc.defined_elements_.end() && it->second.type != SvgElementType::Group) {
                SvgElement clone = it->second;
                clone.transform = el.transform.pre_translate(el.use_x, el.use_y).pre_concat(it->second.transform);
                if (!it->second.style.fill.has_value() && it->second.style.fill_url.empty()) {
                    clone.style.fill = el.style.fill;
                    clone.style.fill_url = el.style.fill_url;
                    clone.style.fill_is_current_color = el.style.fill_is_current_color;
                }
                if (!it->second.style.stroke.has_value() && it->second.style.stroke_url.empty()) {
                    clone.style.stroke = el.style.stroke;
                    clone.style.stroke_url = el.style.stroke_url;
                    clone.style.stroke_width = el.style.stroke_width;
                    clone.style.stroke_is_current_color = el.style.stroke_is_current_color;
                }
                clone.style.current_color = el.style.current_color;
                clone.style.opacity *= el.style.opacity;
                if (clone.style.clip_path_url.empty()) clone.style.clip_path_url = el.style.clip_path_url;
                if (clone.style.mask_url.empty()) clone.style.mask_url = el.style.mask_url;
                if (clone.style.filter_url.empty()) clone.style.filter_url = el.style.filter_url;
                resolved_top.push_back(std::move(clone));
                continue;
            }
        }
        resolved_top.push_back(std::move(el));
    }
    doc.elements_ = std::move(resolved_top);

    return doc;
}

std::optional<SvgDocument> SvgDocument::from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

Path SvgDocument::build_text_path(const SvgElement& elem) const {
    auto& fs = get_svg_font_system();
    if (fs.font_count() == 0) return Path();

    Path combined_text_path;
    float cur_x = elem.text_x + elem.text_dx;
    float cur_y = elem.text_y + elem.text_dy;

    for (const auto& span : elem.text_spans) {
        if (span.text.empty()) continue;
        if (span.x.has_value()) cur_x = *span.x;
        if (span.y.has_value()) cur_y = *span.y;
        cur_x += span.dx;
        cur_y += span.dy;

        const auto& st = span.style;
        text::Family fam(st.font_family);
        if (iequals(st.font_family, "sans-serif")) fam = text::Family::sans_serif();
        else if (iequals(st.font_family, "serif")) fam = text::Family::serif();
        else if (iequals(st.font_family, "monospace")) fam = text::Family::monospace();

        auto [primary_id, primary_font] = fs.match_font(fam, st.font_weight, st.font_style);
        if (!primary_font) continue;

        float fsize = st.font_size;
        bool synth_bold = (static_cast<uint16_t>(st.font_weight) >= 600 &&
                           static_cast<uint16_t>(primary_font->weight()) < 600);
        bool synth_italic = (st.font_style != text::Style::Normal &&
                             primary_font->style() == text::Style::Normal);

        // Shape text using Nisaba's BiDi & Arabic shaper
        auto runs = text::Bidi::segment_runs(span.text);
        auto shaped = text::Bidi::shape_text(span.text, runs);

        // Compute total advance width for text-anchor alignment
        struct GlyphPlacement {
            const text::TtfFont* font{nullptr};
            uint16_t glyph_id{0};
            float advance{0.0f};
        };
        std::vector<GlyphPlacement> placements;
        placements.reserve(shaped.size());

        float total_advance = 0.0f;
        uint16_t prev_gid = 0;
        const text::TtfFont* prev_f = nullptr;

        for (const auto& sc : shaped) {
            auto [fid, gid] = fs.find_glyph_or_fallback(primary_id, sc.codepoint);
            const text::TtfFont* f = fs.get_font(fid);
            if (!f) f = primary_font;
            if (prev_f == f && prev_gid != 0 && gid != 0) {
                total_advance += static_cast<float>(f->get_kerning(prev_gid, gid)) * f->scale_for_size(fsize);
            }
            float adv = sc.is_mark ? 0.0f : (f->glyph_advance(gid, fsize) + st.letter_spacing);
            placements.push_back(GlyphPlacement{ f, gid, adv });
            total_advance += adv;
            prev_gid = gid;
            prev_f = f;
        }

        // Baseline vertical shift
        float baseline_shift = 0.0f;
        float asc = primary_font->ascent(fsize);
        float desc = primary_font->descent(fsize); // negative
        switch (st.dominant_baseline) {
            case SvgDominantBaseline::Middle:
            case SvgDominantBaseline::Mathematical:
                baseline_shift = (asc + desc) * 0.35f;
                break;
            case SvgDominantBaseline::Central:
                baseline_shift = (asc + desc) * 0.5f;
                break;
            case SvgDominantBaseline::Hanging:
            case SvgDominantBaseline::TextBeforeEdge:
                baseline_shift = asc;
                break;
            case SvgDominantBaseline::TextAfterEdge:
            case SvgDominantBaseline::Ideographic:
                baseline_shift = desc;
                break;
            case SvgDominantBaseline::Auto:
            case SvgDominantBaseline::Alphabetic:
            default:
                baseline_shift = 0.0f;
                break;
        }

        // Check if this span is attached to a <textPath>
        if (!span.path_href.empty()) {
            auto it_p = defined_elements_.find(span.path_href);
            if (it_p != defined_elements_.end() && !it_p->second.path.is_empty()) {
                PathMeasure pm(it_p->second.path, false);
                float path_len = pm.length();
                float dist = span.start_offset_percent ? (span.start_offset * path_len) : span.start_offset;
                if (st.text_anchor == SvgTextAnchor::Middle) dist -= total_advance * 0.5f;
                else if (st.text_anchor == SvgTextAnchor::End) dist -= total_advance;

                for (const auto& pl : placements) {
                    float mid_d = dist + pl.advance * 0.5f;
                    if (mid_d >= 0.0f && mid_d <= path_len) {
                        Point pos_on_path, tan_on_path;
                        if (pm.get_pos_tan(mid_d, &pos_on_path, &tan_on_path)) {
                            Path g_path;
                            if (pl.font->get_glyph_path_styled(pl.glyph_id, g_path, fsize, synth_bold, synth_italic)) {
                                float angle_deg = std::atan2(tan_on_path.y, tan_on_path.x) * (180.0f / PI);
                                Transform char_ts = Transform::from_translate(pos_on_path.x, pos_on_path.y)
                                    .pre_rotate(angle_deg)
                                    .pre_translate(-pl.advance * 0.5f, baseline_shift);
                                g_path.apply_transform(char_ts);
                                combined_text_path.add_path(g_path);
                            }
                        }
                    }
                    dist += pl.advance;
                }
                continue;
            }
        }

        float align_offset_x = 0.0f;
        if (st.text_anchor == SvgTextAnchor::Middle) align_offset_x = -total_advance * 0.5f;
        else if (st.text_anchor == SvgTextAnchor::End) align_offset_x = -total_advance;

        float pen_x = cur_x + align_offset_x;
        float pen_y = cur_y + baseline_shift;

        for (const auto& pl : placements) {
            Path g_path;
            if (pl.font->get_glyph_path_styled(pl.glyph_id, g_path, fsize, synth_bold, synth_italic)) {
                Transform char_ts = Transform::from_translate(pen_x, pen_y);
                if (std::abs(span.rotate) > 1e-4f) {
                    char_ts = char_ts.pre_rotate(span.rotate);
                }
                g_path.apply_transform(char_ts);
                combined_text_path.add_path(g_path);
            }
            pen_x += pl.advance;
        }
        cur_x = pen_x - align_offset_x;
    }

    return combined_text_path;
}

Rect SvgDocument::compute_element_local_bounds(const SvgElement& elem, int depth) const {
    if (depth > 16 || elem.style.display_none) return Rect();

    switch (elem.type) {
        case SvgElementType::Path:
            return elem.path.bounds();
        case SvgElementType::Image:
            return elem.image_bounds;
        case SvgElementType::Text: {
            Path tp = build_text_path(elem);
            return tp.bounds();
        }
        case SvgElementType::Use: {
            auto it = defined_elements_.find(elem.use_href);
            if (it != defined_elements_.end()) {
                Rect b = compute_element_local_bounds(it->second, depth + 1);
                if (b.width() > 0.0f && b.height() > 0.0f) {
                    Transform t = Transform::from_translate(elem.use_x, elem.use_y).pre_concat(it->second.transform);
                    Point pts[4] = {
                        Point::from_xy(b.left(), b.top()),
                        Point::from_xy(b.right(), b.top()),
                        Point::from_xy(b.right(), b.bottom()),
                        Point::from_xy(b.left(), b.bottom())
                    };
                    t.map_points(pts, 4);
                    if (auto rb = Rect::from_points(pts, 4)) return *rb;
                }
            }
            return Rect();
        }
        case SvgElementType::Group: {
            float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
            bool any = false;
            for (const auto& ch : elem.children) {
                Rect cb = compute_element_local_bounds(ch, depth + 1);
                if (cb.width() > 0.0f || cb.height() > 0.0f) {
                    Point pts[4] = {
                        Point::from_xy(cb.left(), cb.top()),
                        Point::from_xy(cb.right(), cb.top()),
                        Point::from_xy(cb.right(), cb.bottom()),
                        Point::from_xy(cb.left(), cb.bottom())
                    };
                    if (!ch.transform.is_identity()) ch.transform.map_points(pts, 4);
                    for (const auto& p : pts) {
                        min_x = std::min(min_x, p.x);
                        min_y = std::min(min_y, p.y);
                        max_x = std::max(max_x, p.x);
                        max_y = std::max(max_y, p.y);
                        any = true;
                    }
                }
            }
            if (any) {
                if (auto r = Rect::from_ltrb(min_x, min_y, max_x + 0.001f, max_y + 0.001f)) return *r;
            }
            return view_box_;
        }
    }
    return Rect();
}

void SvgDocument::apply_clip_path_to_canvas(ICanvas& canvas, const SvgElement& elem) const {
    if (elem.style.clip_path_url.empty()) return;
    auto rit = rich_clip_paths_.find(elem.style.clip_path_url);
    if (rit != rich_clip_paths_.end()) {
        const SvgClipPath& cp = rit->second;
        Path cp_path = cp.combined_path;
        if (cp.units == SvgUnits::ObjectBoundingBox) {
            Rect b = compute_element_local_bounds(elem);
            if (b.width() > 0.0f && b.height() > 0.0f) {
                Transform bbox_ts = Transform::from_row(b.width(), 0.0f, 0.0f, b.height(), b.left(), b.top());
                cp_path.apply_transform(bbox_ts);
            }
        }
        canvas.clip_path(cp_path, cp.clip_rule);
    } else {
        auto it = clip_paths_.find(elem.style.clip_path_url);
        if (it != clip_paths_.end()) {
            canvas.clip_path(it->second);
        }
    }
}

void SvgDocument::render_element_dispatch(ICanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth, bool is_gpu) const {
#if defined(NISABA_HAS_GPU) || defined(NISABA_GLEW)
    if (is_gpu) {
        render_element_gpu(static_cast<gpu::GpuCanvas&>(canvas), elem, theme, use_depth);
        return;
    }
#else
    (void)is_gpu;
#endif
    render_element(static_cast<Canvas&>(canvas), elem, theme, use_depth);
}

void SvgDocument::render_markers(ICanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth, bool is_gpu) const {
    if (elem.path.is_empty() || use_depth > 16) return;
    if (elem.style.marker_start_url.empty() && elem.style.marker_mid_url.empty() && elem.style.marker_end_url.empty()) {
        return;
    }

    struct VertexTangent {
        Point pos;
        float in_dx{0.0f}, in_dy{0.0f};
        float out_dx{0.0f}, out_dy{0.0f};
        bool has_in{false}, has_out{false};
    };

    std::vector<VertexTangent> verts;
    PathSegmentsIter iter = elem.path.segments();
    Point cur_pt = Point::zero();
    Point move_pt = Point::zero();

    while (auto seg = iter.next()) {
        switch (seg->type) {
            case PathSegment::Type::MoveTo: {
                cur_pt = move_pt = seg->p0;
                verts.push_back(VertexTangent{ .pos = cur_pt });
                break;
            }
            case PathSegment::Type::LineTo: {
                float dx = seg->p0.x - cur_pt.x;
                float dy = seg->p0.y - cur_pt.y;
                float len = std::hypot(dx, dy);
                if (len > 1e-6f) { dx /= len; dy /= len; }
                if (!verts.empty()) {
                    verts.back().out_dx = dx;
                    verts.back().out_dy = dy;
                    verts.back().has_out = true;
                }
                cur_pt = seg->p0;
                verts.push_back(VertexTangent{ .pos = cur_pt, .in_dx = dx, .in_dy = dy, .has_in = true });
                break;
            }
            case PathSegment::Type::QuadTo: {
                float odx = seg->p0.x - cur_pt.x, ody = seg->p0.y - cur_pt.y;
                float olen = std::hypot(odx, ody);
                if (olen > 1e-6f) { odx /= olen; ody /= olen; }
                if (!verts.empty()) { verts.back().out_dx = odx; verts.back().out_dy = ody; verts.back().has_out = true; }

                float idx = seg->p1.x - seg->p0.x, idy = seg->p1.y - seg->p0.y;
                float ilen = std::hypot(idx, idy);
                if (ilen > 1e-6f) { idx /= ilen; idy /= ilen; }
                cur_pt = seg->p1;
                verts.push_back(VertexTangent{ .pos = cur_pt, .in_dx = idx, .in_dy = idy, .has_in = true });
                break;
            }
            case PathSegment::Type::CubicTo: {
                float odx = seg->p0.x - cur_pt.x, ody = seg->p0.y - cur_pt.y;
                float olen = std::hypot(odx, ody);
                if (olen > 1e-6f) { odx /= olen; ody /= olen; }
                if (!verts.empty()) { verts.back().out_dx = odx; verts.back().out_dy = ody; verts.back().has_out = true; }

                float idx = seg->p2.x - seg->p1.x, idy = seg->p2.y - seg->p1.y;
                float ilen = std::hypot(idx, idy);
                if (ilen > 1e-6f) { idx /= ilen; idy /= ilen; }
                cur_pt = seg->p2;
                verts.push_back(VertexTangent{ .pos = cur_pt, .in_dx = idx, .in_dy = idy, .has_in = true });
                break;
            }
            case PathSegment::Type::Close: {
                float dx = move_pt.x - cur_pt.x;
                float dy = move_pt.y - cur_pt.y;
                float len = std::hypot(dx, dy);
                if (len > 1e-6f) {
                    dx /= len; dy /= len;
                    if (!verts.empty()) { verts.back().out_dx = dx; verts.back().out_dy = dy; verts.back().has_out = true; }
                    cur_pt = move_pt;
                    verts.push_back(VertexTangent{ .pos = cur_pt, .in_dx = dx, .in_dy = dy, .has_in = true });
                }
                break;
            }
        }
    }

    if (verts.empty()) return;

    auto draw_single_marker = [&](const std::string& marker_id, const VertexTangent& vt, bool is_start) {
        if (marker_id.empty()) return;
        auto it = markers_.find(marker_id);
        if (it == markers_.end()) return;
        const SvgMarker& mk = it->second;

        float angle_deg = mk.orient_angle;
        if (mk.orient_mode == SvgMarker::OrientMode::Auto || mk.orient_mode == SvgMarker::OrientMode::AutoStartReverse) {
            float dx = 0.0f, dy = 0.0f;
            if (vt.has_in && vt.has_out) {
                dx = vt.in_dx + vt.out_dx;
                dy = vt.in_dy + vt.out_dy;
            } else if (vt.has_out) {
                dx = vt.out_dx; dy = vt.out_dy;
            } else if (vt.has_in) {
                dx = vt.in_dx; dy = vt.in_dy;
            }
            angle_deg = std::atan2(dy, dx) * (180.0f / PI);
            if (is_start && mk.orient_mode == SvgMarker::OrientMode::AutoStartReverse) {
                angle_deg += 180.0f;
            }
        }

        float unit_scale = mk.stroke_width_units ? elem.style.stroke_width : 1.0f;
        canvas.save();
        canvas.translate(vt.pos.x, vt.pos.y);
        canvas.rotate(angle_deg);
        canvas.scale(unit_scale, unit_scale);

        if (mk.has_view_box && mk.view_box.width() > 0.0f && mk.view_box.height() > 0.0f) {
            float sx = mk.marker_width / mk.view_box.width();
            float sy = mk.marker_height / mk.view_box.height();
            canvas.scale(sx, sy);
            canvas.translate(-mk.ref_x, -mk.ref_y);
        } else {
            canvas.translate(-mk.ref_x, -mk.ref_y);
        }

        SvgTheme marker_theme = theme;
        if (!marker_theme.current_color.has_value()) {
            marker_theme.current_color = elem.style.current_color;
        }
        for (const auto& m_el : mk.elements) {
            render_element_dispatch(canvas, m_el, marker_theme, use_depth + 1, is_gpu);
        }
        canvas.restore();
    };

    draw_single_marker(elem.style.marker_start_url, verts.front(), true);
    for (size_t i = 1; i + 1 < verts.size(); ++i) {
        draw_single_marker(elem.style.marker_mid_url, verts[i], false);
    }
    if (verts.size() > 1) {
        draw_single_marker(elem.style.marker_end_url, verts.back(), false);
    }
}

void SvgDocument::render_element_raw(ICanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth, bool is_gpu) const {
    if (elem.style.display_none || use_depth > 16) return;

    if (elem.type == SvgElementType::Group) {
        canvas.save();
        if (elem.group_viewport.has_value()) {
            canvas.translate(elem.group_viewport->left(), elem.group_viewport->top());
            if (elem.group_view_box.has_value()) {
                if (auto vp0 = Rect::from_xywh(0.0f, 0.0f, elem.group_viewport->width(), elem.group_viewport->height())) {
                    canvas.concat(elem.preserve_aspect_ratio.compute_transform(*elem.group_view_box, *vp0));
                }
            }
        }
        for (const auto& child : elem.children) {
            render_element_dispatch(canvas, child, theme, use_depth, is_gpu);
        }
        canvas.restore();
        return;
    }

    if (elem.type == SvgElementType::Use) {
        auto it = defined_elements_.find(elem.use_href);
        if (it != defined_elements_.end()) {
            canvas.save();
            canvas.translate(elem.use_x, elem.use_y);

            SvgElement clone = it->second;
            if (clone.type == SvgElementType::Group && clone.group_view_box.has_value() &&
                elem.use_width.has_value() && elem.use_height.has_value()) {
                clone.group_viewport = Rect::from_xywh(0.0f, 0.0f, *elem.use_width, *elem.use_height);
            }
            if (!clone.style.fill.has_value() && clone.style.fill_url.empty() && elem.style.fill.has_value()) {
                clone.style.fill = elem.style.fill;
                clone.style.fill_is_current_color = elem.style.fill_is_current_color;
            }
            if (!clone.style.stroke.has_value() && clone.style.stroke_url.empty() && elem.style.stroke.has_value()) {
                clone.style.stroke = elem.style.stroke;
                clone.style.stroke_width = elem.style.stroke_width;
                clone.style.stroke_is_current_color = elem.style.stroke_is_current_color;
            }
            SvgTheme use_theme = theme;
            if (!use_theme.current_color.has_value()) {
                use_theme.current_color = elem.style.current_color;
            }
            render_element_dispatch(canvas, clone, use_theme, use_depth + 1, is_gpu);
            canvas.restore();
        }
        return;
    }

    if (elem.style.visibility_hidden) return;

    if (elem.type == SvgElementType::Image) {
        float bw = elem.image_bounds.width();
        float bh = elem.image_bounds.height();
        if (bw <= 0.0f || bh <= 0.0f) return;

        if (elem.image) {
            PixmapPaint pp;
            pp.opacity = elem.style.opacity;
            pp.blend_mode = elem.style.blend_mode;
            pp.quality = FilterQuality::Bilinear;
            float iw = static_cast<float>(elem.image->width());
            float ih = static_cast<float>(elem.image->height());
            if (iw > 0.0f && ih > 0.0f) {
                canvas.save();
                if (elem.preserve_aspect_ratio.meet_or_slice == SvgAspectMeetOrSlice::Slice) {
                    canvas.clip_rect(elem.image_bounds);
                }
                if (auto img_vb = Rect::from_xywh(0.0f, 0.0f, iw, ih)) {
                    canvas.concat(elem.preserve_aspect_ratio.compute_transform(*img_vb, elem.image_bounds));
                    canvas.draw_pixmap(0, 0, elem.image->as_ref(), pp);
                }
                canvas.restore();
            }
        } else if (elem.nested_svg) {
#if defined(NISABA_HAS_GPU) || defined(NISABA_GLEW)
            if (is_gpu) {
                elem.nested_svg->render_gpu(static_cast<gpu::GpuCanvas&>(canvas), &elem.image_bounds, theme);
            } else {
                elem.nested_svg->render(static_cast<Canvas&>(canvas), &elem.image_bounds, theme);
            }
#else
            elem.nested_svg->render(static_cast<Canvas&>(canvas), &elem.image_bounds, theme);
#endif
        }
        return;
    }

    // Resolve geometry path (for Path or Text element)
    Path text_generated_path;
    const Path* target_path = &elem.path;
    if (elem.type == SvgElementType::Text) {
        text_generated_path = build_text_path(elem);
        target_path = &text_generated_path;
    }

    if (target_path->is_empty()) return;

    Rect geom_bounds = target_path->bounds();

    auto resolve_paint_server = [&](const std::string& url_id, float alpha_mult) -> std::optional<Shader> {
        if (url_id.empty()) return std::nullopt;
        auto git = gradients_.find(url_id);
        if (git != gradients_.end()) {
            return create_shader_from_gradient(git->second, geom_bounds, alpha_mult, theme);
        }
        auto pit = patterns_.find(url_id);
        if (pit != patterns_.end()) {
            const SvgPattern& pat = pit->second;
            float pw = pat.width;
            float ph = pat.height;
            float px = pat.x;
            float py = pat.y;
            if (pat.units == SvgUnits::ObjectBoundingBox) {
                pw *= geom_bounds.width();
                ph *= geom_bounds.height();
                px = geom_bounds.left() + pat.x * geom_bounds.width();
                py = geom_bounds.top() + pat.y * geom_bounds.height();
            }
            uint32_t tile_w = static_cast<uint32_t>(std::clamp(std::ceil(pw), 1.0f, 1024.0f));
            uint32_t tile_h = static_cast<uint32_t>(std::clamp(std::ceil(ph), 1.0f, 1024.0f));
            auto tile_opt = Pixmap::allocate(tile_w, tile_h);
            if (tile_opt) {
                tile_opt->fill(Color::TRANSPARENT);
                Canvas pat_canvas(*tile_opt);
                if (pat.view_box.has_value()) {
                    if (auto vp = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(tile_w), static_cast<float>(tile_h))) {
                        pat_canvas.concat(pat.preserve_aspect_ratio.compute_transform(*pat.view_box, *vp));
                    }
                } else if (pat.content_units == SvgUnits::ObjectBoundingBox) {
                    pat_canvas.scale(geom_bounds.width(), geom_bounds.height());
                }
                for (const auto& pel : pat.elements) {
                    render_element(pat_canvas, pel, theme, use_depth + 1);
                }
                auto shared_pm = std::make_shared<Pixmap>(std::move(*tile_opt));
                pattern_cache_.push_back(shared_pm);
                Transform pat_ts = Transform::from_translate(px, py).pre_concat(pat.pattern_transform);
                return Shader(Pattern(shared_pm->as_ref(), SpreadMode::Repeat, FilterQuality::Bilinear, alpha_mult, pat_ts));
            }
        }
        return std::nullopt;
    };

    auto do_fill = [&]() {
        float alpha = elem.style.opacity * elem.style.fill_opacity;
        if (!elem.style.fill_url.empty()) {
            if (auto sh = resolve_paint_server(elem.style.fill_url, alpha)) {
                Paint fill_paint(std::move(*sh));
                fill_paint.blend_mode = elem.style.blend_mode;
                fill_paint.set_linear_blending(elem.style.linear_color_interpolation);
                canvas.fill_path(*target_path, fill_paint, elem.style.fill_rule);
                return;
            }
        }
        if (elem.style.fill.has_value() || elem.style.fill_is_current_color || !elem.style.fill_css_var.empty() || elem.style.fill_fallback.has_value()) {
            Color fill_c = resolve_effective_color(
                elem.style.fill.has_value() ? elem.style.fill : elem.style.fill_fallback,
                elem.style.fill_is_current_color,
                elem.style.fill_css_var,
                elem.style.fill_fallback,
                elem.style.current_color,
                elem.id,
                theme,
                root_css_vars_
            );
            fill_c.apply_opacity(alpha);
            Paint fill_paint(fill_c);
            fill_paint.blend_mode = elem.style.blend_mode;
            fill_paint.set_linear_blending(elem.style.linear_color_interpolation);
            canvas.fill_path(*target_path, fill_paint, elem.style.fill_rule);
        }
    };

    auto do_stroke = [&]() {
        if (elem.style.stroke_width <= 0.0f) return;
        float sw = elem.style.stroke_width;
        if (elem.style.non_scaling_stroke) {
            const auto ct = canvas.current_transform();
            float scale_factor = std::sqrt(std::abs(ct.sx * ct.sy - ct.kx * ct.ky));
            if (scale_factor > 1e-6f) sw /= scale_factor;
        }

        Stroke stroke(sw);
        stroke.line_cap = elem.style.line_cap;
        stroke.line_join = elem.style.line_join;
        stroke.miter_limit = elem.style.miter_limit;
        if (!elem.style.dash_array.empty()) {
            stroke.dash = StrokeDash::create(elem.style.dash_array, elem.style.dash_offset);
        }

        float alpha = elem.style.opacity * elem.style.stroke_opacity;
        if (!elem.style.stroke_url.empty()) {
            if (auto sh = resolve_paint_server(elem.style.stroke_url, alpha)) {
                Paint stroke_paint(std::move(*sh));
                stroke_paint.blend_mode = elem.style.blend_mode;
                stroke_paint.set_linear_blending(elem.style.linear_color_interpolation);
                canvas.stroke_path(*target_path, stroke_paint, stroke);
                return;
            }
        }
        if (elem.style.stroke.has_value() || elem.style.stroke_is_current_color || !elem.style.stroke_css_var.empty() || elem.style.stroke_fallback.has_value()) {
            Color stroke_c = resolve_effective_color(
                elem.style.stroke.has_value() ? elem.style.stroke : elem.style.stroke_fallback,
                elem.style.stroke_is_current_color,
                elem.style.stroke_css_var,
                elem.style.stroke_fallback,
                elem.style.current_color,
                elem.id,
                theme,
                root_css_vars_
            );
            stroke_c.apply_opacity(alpha);
            Paint stroke_paint(stroke_c);
            stroke_paint.blend_mode = elem.style.blend_mode;
            stroke_paint.set_linear_blending(elem.style.linear_color_interpolation);
            canvas.stroke_path(*target_path, stroke_paint, stroke);
        }
    };

    for (SvgPaintPhase phase : elem.style.paint_order) {
        if (phase == SvgPaintPhase::Fill) do_fill();
        else if (phase == SvgPaintPhase::Stroke) do_stroke();
        else if (phase == SvgPaintPhase::Markers) render_markers(canvas, elem, theme, use_depth, is_gpu);
    }
}

void SvgDocument::render_element(Canvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth) const {
    if (elem.style.display_none || use_depth > 16) return;

    canvas.save();
    if (!elem.transform.is_identity()) {
        canvas.concat(elem.transform);
    }

    // 1. Vector ClipPath (supports clipPathUnits="objectBoundingBox" and clip-rule)
    apply_clip_path_to_canvas(canvas, elem);

    // 2. Alpha / Luminance Mask (preserving current Canvas transform!)
    if (!elem.style.mask_url.empty()) {
        auto it = masks_.find(elem.style.mask_url);
        if (it != masks_.end() && !it->second.elements.empty()) {
            const SvgMask& m_def = it->second;
            auto mask_pix = Pixmap::allocate(canvas.width(), canvas.height());
            if (mask_pix) {
                Canvas mask_canvas(*mask_pix);
                mask_canvas.clear(Color::TRANSPARENT);
                // Critical Fix: inherit current canvas transform so mask elements align with viewBox & element!
                mask_canvas.set_transform(canvas.transform());
                if (m_def.content_units == SvgUnits::ObjectBoundingBox) {
                    Rect b = compute_element_local_bounds(elem);
                    if (b.width() > 0.0f && b.height() > 0.0f) {
                        mask_canvas.translate(b.left(), b.top());
                        mask_canvas.scale(b.width(), b.height());
                    }
                }
                for (const auto& melem : m_def.elements) {
                    render_element(mask_canvas, melem, SvgTheme{}, use_depth + 1);
                }
                Mask m = Mask::from_pixmap(
                    mask_pix->as_ref(),
                    m_def.is_alpha_mask ? MaskType::Alpha : MaskType::Luminance
                );
                canvas.clip_mask(m);
            }
        }
    }

    // 3. Check if offscreen isolation layer is needed (Filter or Group Opacity/BlendMode)
    bool has_filter = !elem.style.filter_url.empty() && filters_.count(elem.style.filter_url) > 0;
    bool needs_group_isolation = (elem.type == SvgElementType::Group &&
                                  (elem.style.opacity < 0.999f || elem.style.blend_mode != BlendMode::SourceOver));

    if (has_filter || needs_group_isolation) {
        auto layer_pix = Pixmap::allocate(canvas.width(), canvas.height());
        if (layer_pix) {
            layer_pix->fill(Color::TRANSPARENT);
            Canvas layer_canvas(*layer_pix);
            layer_canvas.set_transform(canvas.transform());

            SvgElement isolated_elem = elem;
            isolated_elem.transform = Transform::identity();
            isolated_elem.style.opacity = 1.0f;
            isolated_elem.style.blend_mode = BlendMode::SourceOver;
            isolated_elem.style.filter_url.clear();
            isolated_elem.style.clip_path_url.clear();
            isolated_elem.style.mask_url.clear();

            render_element_raw(layer_canvas, isolated_elem, theme, use_depth);

            if (has_filter) {
                const SvgFilter& flt = filters_.at(elem.style.filter_url);
                auto filtered_pix = Pixmap::allocate(canvas.width(), canvas.height());
                if (filtered_pix) {
                    execute_filter_graph(flt, layer_pix->as_ref(), *filtered_pix);
                    layer_pix = std::move(filtered_pix);
                }
            }

            canvas.save();
            canvas.reset_transform();
            PixmapPaint pp;
            pp.opacity = elem.style.opacity;
            pp.blend_mode = elem.style.blend_mode;
            canvas.draw_pixmap(0, 0, layer_pix->as_ref(), pp);
            canvas.restore();
        }
        canvas.restore();
        return;
    }

    render_element_raw(canvas, elem, theme, use_depth, false);
    canvas.restore();
}

void SvgDocument::render(Canvas& canvas, const Rect* target_bounds, std::optional<Color> tint) const {
    SvgTheme theme;
    theme.tint_color = tint;
    render(canvas, target_bounds, theme);
}

void SvgDocument::render(Canvas& canvas, const Rect* target_bounds, const SvgTheme& theme) const {
    canvas.save();

    if (target_bounds != nullptr) {
        if (preserve_aspect_ratio_.meet_or_slice == SvgAspectMeetOrSlice::Slice) {
            canvas.clip_rect(*target_bounds);
        }
        canvas.concat(preserve_aspect_ratio_.compute_transform(view_box_, *target_bounds));
    }

    for (const auto& elem : elements_) {
        render_element(canvas, elem, theme, 0);
    }

    canvas.restore();
}

#if defined(NISABA_HAS_GPU) || defined(NISABA_GLEW)
void SvgDocument::render_element_gpu(gpu::GpuCanvas& canvas, const SvgElement& elem, const SvgTheme& theme, int use_depth) const {
    if (elem.style.display_none || use_depth > 16) return;

    canvas.save();
    if (!elem.transform.is_identity()) {
        canvas.concat(elem.transform);
    }

    apply_clip_path_to_canvas(canvas, elem);

    bool has_filter = !elem.style.filter_url.empty() && filters_.count(elem.style.filter_url) > 0;
    bool has_mask = !elem.style.mask_url.empty() && masks_.count(elem.style.mask_url) > 0;
    if (has_filter || has_mask) {
        uint32_t cw = canvas.width();
        uint32_t ch = canvas.height();
        if (cw > 0 && ch > 0) {
            if (auto pm = Pixmap::allocate(cw, ch)) {
                pm->fill(Color::TRANSPARENT);
                Canvas cpu_c(*pm);
                cpu_c.set_transform(canvas.transform());

                SvgElement isolated_elem = elem;
                isolated_elem.transform = Transform::identity();
                isolated_elem.style.opacity = 1.0f;
                isolated_elem.style.blend_mode = BlendMode::SourceOver;
                isolated_elem.style.clip_path_url.clear();
                render_element(cpu_c, isolated_elem, theme, use_depth);

                canvas.save();
                canvas.reset_transform();
                PixmapPaint pp;
                pp.opacity = elem.style.opacity;
                pp.blend_mode = elem.style.blend_mode;
                canvas.draw_pixmap(0, 0, pm->as_ref(), pp);
                canvas.restore();
            }
        }
        canvas.restore();
        return;
    }

    bool needs_group_isolation = (elem.type == SvgElementType::Group &&
                                  (elem.style.opacity < 0.999f || elem.style.blend_mode != BlendMode::SourceOver));
    if (needs_group_isolation) {
        Color layer_col = Color::WHITE;
        layer_col.apply_opacity(elem.style.opacity);
        Paint layer_paint(layer_col);
        layer_paint.blend_mode = elem.style.blend_mode;
        canvas.save_layer(layer_paint);
        render_element_raw(canvas, elem, theme, use_depth, true);
        canvas.restore();
        canvas.restore();
        return;
    }

    render_element_raw(canvas, elem, theme, use_depth, true);
    canvas.restore();
}

void SvgDocument::render_gpu(gpu::GpuCanvas& canvas, const Rect* target_bounds, std::optional<Color> tint) const {
    SvgTheme theme;
    theme.tint_color = tint;
    render_gpu(canvas, target_bounds, theme);
}

void SvgDocument::render_gpu(gpu::GpuCanvas& canvas, const Rect* target_bounds, const SvgTheme& theme) const {
    canvas.save();

    if (target_bounds != nullptr) {
        if (preserve_aspect_ratio_.meet_or_slice == SvgAspectMeetOrSlice::Slice) {
            canvas.clip_rect(*target_bounds);
        }
        canvas.concat(preserve_aspect_ratio_.compute_transform(view_box_, *target_bounds));
    }

    for (const auto& elem : elements_) {
        render_element_gpu(canvas, elem, theme, 0);
    }

    canvas.restore();
}
#endif

namespace {

bool point_in_path_geometry(const Path& path, Point pt, FillRule fill_rule) noexcept {
    if (path.is_empty()) return false;
    Rect b = path.bounds();
    if (pt.x < b.left() - 1.0f || pt.x > b.right() + 1.0f || pt.y < b.top() - 1.0f || pt.y > b.bottom() + 1.0f) {
        return false;
    }

    int winding = 0;
    PathSegmentsIter iter = path.segments();
    iter.set_auto_close(true);
    Point cur = Point::zero();

    auto ray_cross_segment = [&](Point p0, Point p1) {
        if ((p0.y <= pt.y && p1.y > pt.y) || (p1.y <= pt.y && p0.y > pt.y)) {
            float t = (pt.y - p0.y) / (p1.y - p0.y);
            float x_inter = p0.x + t * (p1.x - p0.x);
            if (pt.x < x_inter) {
                winding += (p1.y > p0.y) ? 1 : -1;
            }
        }
    };

    while (auto seg = iter.next()) {
        switch (seg->type) {
            case PathSegment::Type::MoveTo:
                cur = seg->p0;
                break;
            case PathSegment::Type::LineTo:
                ray_cross_segment(cur, seg->p0);
                cur = seg->p0;
                break;
            case PathSegment::Type::QuadTo: {
                constexpr int STEPS = 8;
                Point prev = cur;
                for (int i = 1; i <= STEPS; ++i) {
                    float t = static_cast<float>(i) / STEPS;
                    float mt = 1.0f - t;
                    Point next = Point::from_xy(
                        mt * mt * cur.x + 2.0f * mt * t * seg->p0.x + t * t * seg->p1.x,
                        mt * mt * cur.y + 2.0f * mt * t * seg->p0.y + t * t * seg->p1.y
                    );
                    ray_cross_segment(prev, next);
                    prev = next;
                }
                cur = seg->p1;
                break;
            }
            case PathSegment::Type::CubicTo: {
                constexpr int STEPS = 12;
                Point prev = cur;
                for (int i = 1; i <= STEPS; ++i) {
                    float t = static_cast<float>(i) / STEPS;
                    float mt = 1.0f - t;
                    Point next = Point::from_xy(
                        mt * mt * mt * cur.x + 3.0f * mt * mt * t * seg->p0.x + 3.0f * mt * t * t * seg->p1.x + t * t * t * seg->p2.x,
                        mt * mt * mt * cur.y + 3.0f * mt * mt * t * seg->p0.y + 3.0f * mt * t * t * seg->p1.y + t * t * t * seg->p2.y
                    );
                    ray_cross_segment(prev, next);
                    prev = next;
                }
                cur = seg->p2;
                break;
            }
            case PathSegment::Type::Close:
                break;
        }
    }

    if (fill_rule == FillRule::EvenOdd) {
        return (std::abs(winding) % 2) == 1;
    }
    return winding != 0;
}

std::optional<std::string> hit_test_element_recursive(
    const SvgDocument& doc,
    const SvgElement& elem,
    Point pt,
    int depth
) {
    if (elem.style.display_none || elem.style.visibility_hidden || depth > 16) {
        return std::nullopt;
    }

    Point local_pt = pt;
    if (!elem.transform.is_identity()) {
        auto inv = elem.transform.invert();
        if (!inv) return std::nullopt;
        inv->map_point(local_pt);
    }

    if (elem.type == SvgElementType::Group) {
        for (auto it = elem.children.rbegin(); it != elem.children.rend(); ++it) {
            if (auto hit = hit_test_element_recursive(doc, *it, local_pt, depth + 1)) {
                return (!hit->empty()) ? hit : (!elem.id.empty() ? std::optional<std::string>(elem.id) : std::nullopt);
            }
        }
        return std::nullopt;
    }

    if (elem.type == SvgElementType::Use) {
        auto it = doc.defined_elements().find(elem.use_href);
        if (it != doc.defined_elements().end()) {
            Point u_pt = Point::from_xy(local_pt.x - elem.use_x, local_pt.y - elem.use_y);
            if (auto hit = hit_test_element_recursive(doc, it->second, u_pt, depth + 1)) {
                return (!elem.id.empty()) ? std::optional<std::string>(elem.id) : hit;
            }
        }
        return std::nullopt;
    }

    if (elem.type == SvgElementType::Image) {
        if (local_pt.x >= elem.image_bounds.left() && local_pt.x <= elem.image_bounds.right() &&
            local_pt.y >= elem.image_bounds.top()  && local_pt.y <= elem.image_bounds.bottom()) {
            return elem.id;
        }
        return std::nullopt;
    }

    if (elem.type == SvgElementType::Path) {
        bool has_fill = (elem.style.fill.has_value() || !elem.style.fill_url.empty() || elem.style.fill_is_current_color);
        if (has_fill && point_in_path_geometry(elem.path, local_pt, elem.style.fill_rule)) {
            return elem.id;
        }
        bool has_stroke = (elem.style.stroke.has_value() || !elem.style.stroke_url.empty() || elem.style.stroke_is_current_color) &&
                          elem.style.stroke_width > 0.0f;
        if (has_stroke) {
            Stroke st(elem.style.stroke_width);
            st.line_cap = elem.style.line_cap;
            st.line_join = elem.style.line_join;
            if (auto sp = elem.path.stroke_to_fill(st)) {
                if (point_in_path_geometry(*sp, local_pt, FillRule::Winding)) {
                    return elem.id;
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace

std::optional<std::string> SvgDocument::hit_test(Point pt, const Rect* target_bounds) const {
    Point vb_pt = pt;
    if (target_bounds != nullptr) {
        Transform ts = preserve_aspect_ratio_.compute_transform(view_box_, *target_bounds);
        if (auto inv = ts.invert()) {
            inv->map_point(vb_pt);
        }
    }

    for (auto it = elements_.rbegin(); it != elements_.rend(); ++it) {
        if (auto hit = hit_test_element_recursive(*this, *it, vb_pt, 0)) {
            if (!hit->empty()) return hit;
        }
    }
    return std::nullopt;
}

std::optional<Rect> SvgDocument::get_element_bounds(std::string_view id) const {
    auto it = defined_elements_.find(std::string(id));
    if (it == defined_elements_.end()) return std::nullopt;
    const SvgElement& el = it->second;
    Rect b = compute_element_local_bounds(el, 0);
    if (b.width() <= 0.0f && b.height() <= 0.0f) return std::nullopt;
    if (!el.transform.is_identity()) {
        Point pts[4] = {
            Point::from_xy(b.left(), b.top()),
            Point::from_xy(b.right(), b.top()),
            Point::from_xy(b.right(), b.bottom()),
            Point::from_xy(b.left(), b.bottom())
        };
        el.transform.map_points(pts, 4);
        return Rect::from_points(pts, 4);
    }
    return b;
}

} // namespace nisaba::svg
