#include "nisaba/markdown/parser.hpp"
#include "nisaba/markdown/entities.hpp"
#include "nisaba/markdown/html_parser.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace nisaba::markdown {

namespace {

using RefMap = std::unordered_map<std::string, std::pair<std::string, std::string>>;

inline std::string_view trim_start(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

inline std::string_view trim_end(std::string_view s) noexcept {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

inline std::string_view trim(std::string_view s) noexcept {
    return trim_end(trim_start(s));
}

inline bool is_blank(std::string_view s) noexcept {
    return trim(s).empty();
}

inline std::string to_lower_ascii(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return res;
}

/// Counts leading indentation columns (tab expands to next multiple of 4).
/// Also writes the byte index of the first non-whitespace char into `out_byte_idx`.
size_t count_indent_cols(std::string_view line, size_t& out_byte_idx) noexcept {
    size_t cols = 0;
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ') {
            cols += 1;
            i++;
        } else if (line[i] == '\t') {
            cols += 4 - (cols % 4);
            i++;
        } else {
            break;
        }
    }
    out_byte_idx = i;
    return cols;
}

/// Strips up to `target_cols` leading indentation columns (expanding partial tabs into spaces).
std::string strip_indent_cols(std::string_view line, size_t target_cols) {
    size_t cols = 0;
    size_t i = 0;
    while (i < line.size() && cols < target_cols) {
        if (line[i] == ' ') {
            cols += 1;
            i++;
        } else if (line[i] == '\t') {
            size_t next_cols = cols + (4 - (cols % 4));
            if (next_cols > target_cols) {
                size_t rem_spaces = next_cols - target_cols;
                std::string out(rem_spaces, ' ');
                out.append(line.substr(i + 1));
                return out;
            }
            cols = next_cols;
            i++;
        } else {
            break;
        }
    }
    return std::string(line.substr(i));
}

/// Flattens plain display text from a sequence of inline spans.
std::string flatten_span_text(const std::vector<InlineSpan>& spans) {
    std::string out;
    for (const auto& s : spans) {
        if (!s.children.empty()) {
            out += flatten_span_text(s.children);
        } else if (s.type == InlineType::SoftBreak) {
            out.push_back('\n');
        } else {
            out += s.text;
        }
    }
    return out;
}

// Comprehensive GitHub Emoji shortcode map
static const std::unordered_map<std::string_view, std::string_view> kEmojiMap = {
    {"smile", "😄"}, {"rocket", "🚀"}, {"tada", "🎉"}, {"sparkles", "✨"},
    {"warning", "⚠️"}, {"+1", "👍"}, {"-1", "👎"}, {"heart", "❤️"},
    {"white_check_mark", "✅"}, {"x", "❌"}, {"fire", "🔥"}, {"star", "⭐"},
    {"bulb", "💡"}, {"zap", "⚡"}, {"lock", "🔒"}, {"key", "🔑"},
    {"memo", "📝"}, {"book", "📖"}, {"link", "🔗"}, {"package", "📦"},
    {"bug", "🐛"}, {"eyes", "👀"}, {"gear", "⚙️"}, {"wrench", "🔧"},
    {"construction", "🚧"}, {"heavy_check_mark", "✔️"}, {"information_source", "ℹ️"},
    {"octocat", "🐙"}, {"v", "✌️"}, {"100", "💯"}, {"check", "✓"},
    {"art", "🎨"}, {"coffee", "☕"}, {"beer", "🍺"}, {"trophy", "🏆"},
    {"chart_with_upwards_trend", "📈"}, {"hammer", "🔨"}, {"shield", "🛡️"},
    {"computer", "💻"}, {"books", "📚"}, {"globe_with_meridians", "🌐"},
    {"thinking", "🤔"}, {"clap", "👏"}, {"pray", "🙏"}, {"runner", "🏃"},
    {"muscle", "💪"}, {"wave", "👋"}, {"sparkler", "🎇"}, {"hourglass", "⏳"},
    {"mag", "🔍"}, {"bell", "🔔"}, {"speech_balloon", "💬"}, {"dart", "🎯"},
    {"gem", "💎"}, {"crown", "👑"}, {"battery", "🔋"}, {"radioactive", "☢️"},
    {"biohazard", "☣️"}, {"recycle", "♻️"}, {"ok_hand", "👌"}
};

/// Fast check whether `text` contains any newline.
inline bool contains_newline(std::string_view text) noexcept {
    return text.find('\n') != std::string_view::npos;
}

/// Computes 1-based (line, col) for byte offset `byte_off` inside `text`, starting from `base_pos`.
std::pair<uint32_t, uint32_t> offset_to_line_col(
    std::string_view text,
    size_t byte_off,
    SourcePos base_pos,
    bool has_nl = true
) noexcept {
    uint32_t line = base_pos.start_line > 0 ? base_pos.start_line : 1;
    uint32_t col = base_pos.start_col > 0 ? base_pos.start_col : 1;
    size_t lim = std::min(byte_off, text.size());
    if (!has_nl) {
        return {line, col + static_cast<uint32_t>(lim)};
    }
    for (size_t i = 0; i < lim; ++i) {
        if (text[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    return {line, col};
}

SourcePos make_span_sourcepos(
    std::string_view text,
    size_t start_off,
    size_t end_off,
    SourcePos base_pos,
    bool has_nl = true
) noexcept {
    uint32_t bline = base_pos.start_line > 0 ? base_pos.start_line : 1;
    uint32_t bcol = base_pos.start_col > 0 ? base_pos.start_col : 1;
    size_t eff_end = (end_off > start_off) ? (end_off - 1) : start_off;
    if (!has_nl) {
        return SourcePos{
            bline,
            bcol + static_cast<uint32_t>(start_off),
            bline,
            bcol + static_cast<uint32_t>(eff_end)
        };
    }
    base_pos.start_line = bline;
    base_pos.start_col = bcol;
    auto [sl, sc] = offset_to_line_col(text, start_off, base_pos, true);
    auto [el, ec] = offset_to_line_col(text, eff_end, base_pos, true);
    return SourcePos{sl, sc, el, ec};
}

inline bool has_smart_punct_trigger(std::string_view text) noexcept {
    for (char c : text) {
        if (c == '"' || c == '\'' || c == '-' || c == '.') return true;
    }
    return false;
}

// --- Smart Punctuation Transformation (CommonMark SmartyPants rules) ---
std::string apply_smart_punctuation(std::string_view text, uint32_t prev_cp_context, uint32_t next_cp_context) {
    if (!has_smart_punct_trigger(text)) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() + 16);

    size_t i = 0;
    size_t len = text.size();
    while (i < len) {
        // Ellipsis: ...
        if (text[i] == '.' && i + 2 < len && text[i + 1] == '.' && text[i + 2] == '.') {
            out += "\xe2\x80\xa6"; // U+2026 …
            i += 3;
            continue;
        }
        // Dashes: --- (em-dash), -- (en-dash), and runs of dashes per CommonMark smart rules
        if (text[i] == '-' && i + 1 < len && text[i + 1] == '-') {
            size_t run = 0;
            while (i + run < len && text[i + run] == '-') run++;
            size_t em_count = 0;
            size_t en_count = 0;
            if (run % 3 == 0) {
                em_count = run / 3;
            } else if (run % 2 == 0) {
                en_count = run / 2;
            } else if (run % 3 == 2) {
                em_count = (run - 2) / 3;
                en_count = 1;
            } else {
                em_count = (run - 4) / 3;
                en_count = 2;
            }
            for (size_t k = 0; k < em_count; ++k) out += "\xe2\x80\x94"; // —
            for (size_t k = 0; k < en_count; ++k) out += "\xe2\x80\x93"; // –
            i += run;
            continue;
        }
        // Smart double & single quotes
        if (text[i] == '"' || text[i] == '\'') {
            char q = text[i];
            uint32_t before = (i == 0) ? prev_cp_context : Entities::decode_utf8_prev(text, i);
            size_t after_pos = i + 1;
            uint32_t after = (after_pos >= len) ? next_cp_context : Entities::decode_utf8(text, after_pos);

            bool before_ws = Entities::is_unicode_whitespace(before);
            bool after_ws = Entities::is_unicode_whitespace(after);
            bool before_punct = Entities::is_unicode_punctuation(before);
            bool after_punct = Entities::is_unicode_punctuation(after);

            bool left_flanking = !after_ws && (!after_punct || before_ws || before_punct);
            bool right_flanking = !before_ws && (!before_punct || after_ws || after_punct);

            bool is_open = false;
            if (q == '\'') {
                // Apostrophe inside word (don't) or before digits ('90s) is right single quote
                if (left_flanking && !right_flanking) {
                    is_open = true;
                } else {
                    is_open = false;
                }
            } else {
                is_open = left_flanking && !right_flanking;
            }

            if (q == '"') {
                out += is_open ? "\xe2\x80\x9c" : "\xe2\x80\x9d"; // “ or ”
            } else {
                out += is_open ? "\xe2\x80\x98" : "\xe2\x80\x99"; // ‘ or ’
            }
            i++;
            continue;
        }

        out.push_back(text[i]);
        i++;
    }
    return out;
}

// --- Strict GFM Autolink Extension (GFM 6.5) ---
bool is_valid_domain_char(char c) noexcept {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '-' || c == '_';
}

bool is_valid_gfm_domain(std::string_view domain, bool require_dot) noexcept {
    if (domain.empty()) return false;
    size_t label_count = 0;
    size_t last_underscore_label = 0;
    bool has_underscore = false;

    size_t start = 0;
    while (start <= domain.size()) {
        size_t dot = domain.find('.', start);
        std::string_view lbl = (dot == std::string_view::npos)
            ? domain.substr(start)
            : domain.substr(start, dot - start);
        if (lbl.empty()) return false;
        for (char c : lbl) {
            if (!is_valid_domain_char(c)) return false;
            if (c == '_') {
                has_underscore = true;
                last_underscore_label = label_count;
            }
        }
        label_count++;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (require_dot && label_count < 2) return false;
    // Last two segments must not contain '_' per GFM 6.5
    if (has_underscore && last_underscore_label + 2 >= label_count) return false;
    return true;
}

bool is_email_local_char(char c) noexcept {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '.' || c == '-' || c == '_' || c == '+';
}

size_t trim_gfm_autolink_trailing(std::string_view candidate) noexcept {
    size_t end = candidate.size();
    while (end > 0) {
        char last = candidate[end - 1];
        if (last == '?' || last == '!' || last == '.' || last == ',' ||
            last == ':' || last == '*' || last == '_' || last == '~') {
            end--;
            continue;
        }
        if (last == ')') {
            int open_count = 0;
            int close_count = 0;
            for (size_t k = 0; k < end; ++k) {
                if (candidate[k] == '(') open_count++;
                else if (candidate[k] == ')') close_count++;
            }
            if (close_count > open_count) {
                end--;
                continue;
            }
        }
        if (last == ';') {
            // Strip trailing &entity; if present at the end of autolink
            size_t amp = end - 1;
            while (amp > 0 && std::isalnum(static_cast<unsigned char>(candidate[amp - 1]))) {
                amp--;
            }
            if (amp > 0 && candidate[amp - 1] == '&' && amp < end - 1) {
                end = amp - 1;
                continue;
            }
            end--;
            continue;
        }
        break;
    }
    return end;
}

bool is_gfm_autolink_boundary(std::string_view text, size_t pos) noexcept {
    if (pos == 0) return true;
    char prev = text[pos - 1];
    return std::isspace(static_cast<unsigned char>(prev)) ||
           prev == '(' || prev == '*' || prev == '_' || prev == '~' || prev == '[';
}

inline bool has_autolink_trigger(std::string_view s) noexcept {
    return s.find_first_of(":.@") != std::string_view::npos;
}

void split_text_with_gfm_autolinks(
    std::string_view processed,
    SourcePos span_pos,
    const MarkdownParseOptions& options,
    std::vector<InlineSpan>& out
) {
    if (!options.autolink || processed.empty() || !has_autolink_trigger(processed)) {
        if (!processed.empty()) {
            auto s = InlineSpan::make_text(std::string(processed));
            s.sourcepos = span_pos;
            out.push_back(std::move(s));
        }
        return;
    }

    size_t i = 0;
    size_t seg_start = 0;
    size_t len = processed.size();

    auto emit_plain = [&](size_t end) {
        if (end > seg_start) {
            auto s = InlineSpan::make_text(std::string(processed.substr(seg_start, end - seg_start)));
            s.sourcepos = span_pos;
            out.push_back(std::move(s));
        }
    };

    while (i < len) {
        // Jump directly to the next potential trigger character ('h', 'w', '@')
        size_t next_trig = processed.find_first_of("hw@", i);
        if (next_trig == std::string_view::npos) break;
        i = next_trig;

        // 1. Protocol URL: http:// or https://
        if (processed[i] == 'h' && is_gfm_autolink_boundary(processed, i) &&
            (processed.substr(i).starts_with("https://") || processed.substr(i).starts_with("http://"))) {
            size_t scheme_len = processed[i + 4] == 's' ? 8 : 7;
            size_t end = i + scheme_len;
            while (end < len && !std::isspace(static_cast<unsigned char>(processed[end])) &&
                   processed[end] != '<') {
                end++;
            }
            size_t raw_len = trim_gfm_autolink_trailing(processed.substr(i, end - i));
            if (raw_len > scheme_len) {
                std::string_view url = processed.substr(i, raw_len);
                // Extract host for basic validation
                size_t host_end = scheme_len;
                while (host_end < url.size() && url[host_end] != '/' && url[host_end] != '?' &&
                       url[host_end] != '#' && url[host_end] != ':') {
                    host_end++;
                }
                std::string_view host = url.substr(scheme_len, host_end - scheme_len);
                if (is_valid_gfm_domain(host, false)) {
                    emit_plain(i);
                    auto link = InlineSpan::make_link(std::string(url), std::string(url));
                    link.sourcepos = span_pos;
                    auto child = InlineSpan::make_text(std::string(url));
                    child.sourcepos = span_pos;
                    link.children.push_back(std::move(child));
                    out.push_back(std::move(link));
                    i += raw_len;
                    seg_start = i;
                    continue;
                }
            }
        }

        // 2. Extended WWW autolink: www.<valid-domain>
        if (processed[i] == 'w' && is_gfm_autolink_boundary(processed, i) && processed.substr(i).starts_with("www.")) {
            size_t end = i + 4;
            while (end < len && !std::isspace(static_cast<unsigned char>(processed[end])) &&
                   processed[end] != '<') {
                end++;
            }
            size_t raw_len = trim_gfm_autolink_trailing(processed.substr(i, end - i));
            if (raw_len > 4) {
                std::string_view full = processed.substr(i, raw_len);
                size_t host_end = 4;
                while (host_end < full.size() && full[host_end] != '/' && full[host_end] != '?' &&
                       full[host_end] != '#' && full[host_end] != ':') {
                    host_end++;
                }
                std::string_view host = full.substr(4, host_end - 4);
                if (is_valid_gfm_domain(host, true)) {
                    emit_plain(i);
                    std::string display(full);
                    auto link = InlineSpan::make_link(display, "http://" + display);
                    link.sourcepos = span_pos;
                    auto child = InlineSpan::make_text(display);
                    child.sourcepos = span_pos;
                    link.children.push_back(std::move(child));
                    out.push_back(std::move(link));
                    i += raw_len;
                    seg_start = i;
                    continue;
                }
            }
        }

        // 3. Extended Email autolink: local@domain.tld
        if (processed[i] == '@' && i > seg_start) {
            size_t local_start = i;
            while (local_start > seg_start && is_email_local_char(processed[local_start - 1])) {
                local_start--;
            }
            if (local_start < i && is_gfm_autolink_boundary(processed, local_start)) {
                size_t dom_end = i + 1;
                while (dom_end < len && (is_valid_domain_char(processed[dom_end]) || processed[dom_end] == '.')) {
                    dom_end++;
                }
                // Strip trailing '.' or '-' or '_'
                while (dom_end > i + 1 && (processed[dom_end - 1] == '.' || processed[dom_end - 1] == '-' ||
                                           processed[dom_end - 1] == '_')) {
                    dom_end--;
                }
                std::string_view domain = processed.substr(i + 1, dom_end - (i + 1));
                if (is_valid_gfm_domain(domain, true) && domain.find('_') == std::string_view::npos) {
                    // Reject if followed immediately by '-' or '_'
                    bool bad_after = (dom_end < len && (processed[dom_end] == '-' || processed[dom_end] == '_'));
                    if (!bad_after) {
                        emit_plain(local_start);
                        std::string email(processed.substr(local_start, dom_end - local_start));
                        auto link = InlineSpan::make_link(email, "mailto:" + email);
                        link.sourcepos = span_pos;
                        auto child = InlineSpan::make_text(email);
                        child.sourcepos = span_pos;
                        link.children.push_back(std::move(child));
                        out.push_back(std::move(link));
                        i = dom_end;
                        seg_start = i;
                        continue;
                    }
                }
            }
        }

        i++;
    }

    emit_plain(len);
}

// --- Link Destination & Title Parsers (CommonMark 0.30 Section 4.7 & 6.3) ---

bool parse_link_destination(
    std::string_view text,
    size_t& pos,
    std::string& out_dest
) {
    size_t i = pos;
    size_t len = text.size();
    if (i >= len) return false;

    if (text[i] == '<') {
        i++;
        std::string raw;
        while (i < len) {
            if (text[i] == '\n' || text[i] == '<') return false;
            if (text[i] == '\\' && i + 1 < len && Entities::is_ascii_punctuation(text[i + 1])) {
                raw.push_back(text[i]);
                raw.push_back(text[i + 1]);
                i += 2;
                continue;
            }
            if (text[i] == '>') {
                pos = i + 1;
                out_dest = Entities::unescape_string(raw);
                return true;
            }
            raw.push_back(text[i++]);
        }
        return false;
    }

    // Bare destination with balanced parentheses
    int paren_depth = 0;
    std::string raw;
    size_t start = i;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c <= 0x20 || c == 0x7F) break;
        if (text[i] == '\\' && i + 1 < len && Entities::is_ascii_punctuation(text[i + 1])) {
            raw.push_back(text[i]);
            raw.push_back(text[i + 1]);
            i += 2;
            continue;
        }
        if (text[i] == '(') {
            paren_depth++;
            if (paren_depth > 32) return false;
            raw.push_back('(');
            i++;
            continue;
        }
        if (text[i] == ')') {
            if (paren_depth == 0) break;
            paren_depth--;
            raw.push_back(')');
            i++;
            continue;
        }
        raw.push_back(text[i++]);
    }
    if (i == start || paren_depth != 0) return false;
    pos = i;
    out_dest = Entities::unescape_string(raw);
    return true;
}

bool parse_link_title(
    std::string_view text,
    size_t& pos,
    std::string& out_title
) {
    size_t i = pos;
    size_t len = text.size();
    if (i >= len) return false;

    char open_ch = text[i];
    char close_ch = 0;
    if (open_ch == '"') close_ch = '"';
    else if (open_ch == '\'') close_ch = '\'';
    else if (open_ch == '(') close_ch = ')';
    else return false;

    i++;
    std::string raw;
    while (i < len) {
        if (text[i] == '\\' && i + 1 < len && Entities::is_ascii_punctuation(text[i + 1])) {
            raw.push_back(text[i]);
            raw.push_back(text[i + 1]);
            i += 2;
            continue;
        }
        if (open_ch == '(' && text[i] == '(') return false;
        if (text[i] == close_ch) {
            pos = i + 1;
            out_title = Entities::unescape_string(raw);
            return true;
        }
        raw.push_back(text[i++]);
    }
    return false;
}

/// Parses inline link tail `(...)` starting at `pos` (where `text[pos] == '('`).
bool parse_inline_link_tail(
    std::string_view text,
    size_t& pos,
    std::string& out_url,
    std::string& out_title
) {
    size_t i = pos;
    size_t len = text.size();
    if (i >= len || text[i] != '(') return false;
    i++;

    auto skip_ws = [&]() {
        while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) i++;
    };

    skip_ws();
    out_url.clear();
    out_title.clear();

    if (i < len && text[i] == ')') {
        pos = i + 1;
        return true;
    }

    if (!parse_link_destination(text, i, out_url)) return false;

    size_t ws_before_title = i;
    skip_ws();
    if (i < len && text[i] != ')') {
        if (i == ws_before_title) return false; // Must have whitespace before title
        if (!parse_link_title(text, i, out_title)) return false;
        skip_ws();
    }

    if (i < len && text[i] == ')') {
        pos = i + 1;
        return true;
    }
    return false;
}

/// Parses a CommonMark angle-bracket autolink `<https://...>` or `<user@example.com>`.
bool try_parse_angle_autolink(
    std::string_view text,
    size_t pos,
    std::string& out_display,
    std::string& out_target,
    size_t& out_consumed
) {
    if (pos >= text.size() || text[pos] != '<') return false;
    size_t close = text.find('>', pos + 1);
    if (close == std::string_view::npos || close <= pos + 1) return false;

    std::string_view inner = text.substr(pos + 1, close - (pos + 1));
    for (char c : inner) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '<' || c == '>') return false;
    }

    // URI autolink: scheme (2..32 chars starting with letter) + ':'
    size_t colon = inner.find(':');
    if (colon != std::string_view::npos && colon >= 2 && colon <= 32 &&
        std::isalpha(static_cast<unsigned char>(inner[0]))) {
        bool valid_scheme = true;
        for (size_t k = 1; k < colon; ++k) {
            unsigned char uc = static_cast<unsigned char>(inner[k]);
            if (!std::isalnum(uc) && inner[k] != '+' && inner[k] != '.' && inner[k] != '-') {
                valid_scheme = false;
                break;
            }
        }
        if (valid_scheme) {
            out_display = std::string(inner);
            out_target = std::string(inner);
            out_consumed = (close + 1) - pos;
            return true;
        }
    }

    // Email autolink: local@domain
    size_t at = inner.find('@');
    if (at != std::string_view::npos && at > 0 && at + 1 < inner.size() &&
        inner.find('@', at + 1) == std::string_view::npos) {
        std::string_view dom = inner.substr(at + 1);
        if (is_valid_gfm_domain(dom, false) && dom.find('_') == std::string_view::npos) {
            out_display = std::string(inner);
            out_target = "mailto:" + std::string(inner);
            out_consumed = (close + 1) - pos;
            return true;
        }
    }

    return false;
}

// --- CommonMark 0.30 Inline Parser with Full Delimiter Stack ---

struct InlineNode {
    InlineSpan span{};
    size_t start_off{0};
    size_t end_off{0};
    // Delimiter / bracket metadata when node is a provisional text token
    bool is_delim{false};
    bool deleted{false};
    char delim_char{0};
    size_t delim_orig_len{0};
    size_t delim_count{0};
    bool can_open{false};
    bool can_close{false};
    bool is_escaped_or_literal{false}; // Do not apply smart quotes / autolink if true
};

struct BracketEntry {
    size_t node_index{0};
    size_t byte_pos{0};
    bool is_image{false};
    bool active{true};
    bool bracket_after{false};
};

void process_emphasis_range(
    std::vector<InlineNode>& nodes,
    size_t range_start,
    size_t range_end,
    std::string_view source_text,
    SourcePos base_pos,
    bool has_nl
) {
    // CommonMark 0.30 Section 6.2 & GFM Strikethrough Delimiter Stack algorithm.
    // Uses in-place deletion flags (zero vector::erase / vector::insert shifting!).
    std::array<size_t, 18> openers_bottom;
    openers_bottom.fill(range_start);

    auto bottom_key = [](char ch, size_t closer_orig_len, bool closer_can_open) -> size_t {
        size_t c_id = (ch == '*') ? 0 : (ch == '_') ? 1 : 2;
        return (c_id * 6) + (closer_orig_len % 3) * 2 + (closer_can_open ? 1 : 0);
    };

    size_t closer_idx = range_start;
    while (closer_idx < range_end && closer_idx < nodes.size()) {
        if (nodes[closer_idx].deleted || !nodes[closer_idx].is_delim ||
            !nodes[closer_idx].can_close || nodes[closer_idx].delim_count == 0) {
            closer_idx++;
            continue;
        }

        char ch = nodes[closer_idx].delim_char;
        size_t closer_orig = nodes[closer_idx].delim_orig_len;
        bool closer_can_open = nodes[closer_idx].can_open;
        size_t bkey = bottom_key(ch, closer_orig, closer_can_open);
        size_t min_open_idx = openers_bottom[bkey];

        bool found_opener = false;
        size_t opener_idx = closer_idx;
        while (opener_idx > min_open_idx) {
            opener_idx--;
            const auto& op = nodes[opener_idx];
            if (op.deleted || !op.is_delim || !op.can_open || op.delim_count == 0 || op.delim_char != ch) {
                continue;
            }

            if (ch == '~') {
                if (op.delim_count != nodes[closer_idx].delim_count || op.delim_count > 2) {
                    continue;
                }
            } else {
                if (op.can_close || closer_can_open) {
                    if ((op.delim_orig_len + closer_orig) % 3 == 0 &&
                        !(op.delim_orig_len % 3 == 0 && closer_orig % 3 == 0)) {
                        continue;
                    }
                }
            }

            found_opener = true;
            break;
        }

        if (!found_opener) {
            openers_bottom[bkey] = closer_idx;
            if (!nodes[closer_idx].can_open) {
                nodes[closer_idx].is_delim = false;
            }
            closer_idx++;
            continue;
        }

        size_t use_delims = 1;
        InlineType wrap_type = InlineType::Italic;
        if (ch == '~') {
            use_delims = nodes[closer_idx].delim_count;
            wrap_type = InlineType::Strikethrough;
        } else if (nodes[opener_idx].delim_count >= 2 && nodes[closer_idx].delim_count >= 2) {
            use_delims = 2;
            wrap_type = InlineType::Bold;
        } else {
            use_delims = 1;
            wrap_type = InlineType::Italic;
        }

        size_t wrap_start_off = nodes[opener_idx].end_off - use_delims;
        size_t wrap_end_off = nodes[closer_idx].start_off + use_delims;

        nodes[opener_idx].delim_count -= use_delims;
        nodes[opener_idx].span.text.resize(nodes[opener_idx].delim_count);
        nodes[opener_idx].end_off -= use_delims;

        nodes[closer_idx].delim_count -= use_delims;
        nodes[closer_idx].span.text.resize(nodes[closer_idx].delim_count);
        nodes[closer_idx].start_off += use_delims;

        // Collect live children strictly between opener_idx and closer_idx, marking slots deleted
        std::vector<InlineSpan> children;
        children.reserve(closer_idx - opener_idx);
        size_t first_inner_slot = closer_idx;
        for (size_t k = opener_idx + 1; k < closer_idx; ++k) {
            if (nodes[k].deleted) continue;
            if (nodes[k].is_delim && nodes[k].delim_count == 0) {
                nodes[k].deleted = true;
                continue;
            }
            if (first_inner_slot == closer_idx) first_inner_slot = k;
            nodes[k].is_delim = false;
            nodes[k].deleted = true;
            children.push_back(std::move(nodes[k].span));
        }

        InlineSpan wrapped;
        if ((wrap_type == InlineType::Italic && children.size() == 1 && children[0].type == InlineType::Bold) ||
            (wrap_type == InlineType::Bold && children.size() == 1 && children[0].type == InlineType::Italic)) {
            wrapped.type = InlineType::BoldItalic;
            wrapped.children = std::move(children[0].children);
            wrapped.text = flatten_span_text(wrapped.children);
            if (wrapped.text.empty()) wrapped.text = children[0].text;
        } else {
            wrapped.type = wrap_type;
            wrapped.children = std::move(children);
            wrapped.text = flatten_span_text(wrapped.children);
        }
        wrapped.sourcepos = make_span_sourcepos(source_text, wrap_start_off, wrap_end_off, base_pos, has_nl);

        if (nodes[opener_idx].delim_count == 0) {
            nodes[opener_idx].deleted = true;
            nodes[opener_idx].is_delim = false;
        }
        if (nodes[closer_idx].delim_count == 0) {
            nodes[closer_idx].deleted = true;
            nodes[closer_idx].is_delim = false;
        }

        // Place wrapped node into an existing slot in-place (zero shifting!)
        size_t target_slot = first_inner_slot;
        if (target_slot == closer_idx) {
            target_slot = opener_idx;
        }
        auto& dst = nodes[target_slot];
        dst.span = std::move(wrapped);
        dst.start_off = wrap_start_off;
        dst.end_off = wrap_end_off;
        dst.is_delim = false;
        dst.deleted = false;
        dst.is_escaped_or_literal = false;

        // If closer still has remaining delimiters, re-evaluate at closer_idx; otherwise advance
        if (nodes[closer_idx].deleted || nodes[closer_idx].delim_count == 0) {
            closer_idx++;
        }
    }

    // Clear delimiter flag for remaining unmatched delimiters in range
    for (size_t k = range_start; k < range_end && k < nodes.size(); ++k) {
        nodes[k].is_delim = false;
    }
}

std::vector<InlineSpan> parse_inlines_impl(
    std::string_view text,
    const MarkdownParseOptions& options,
    const RefMap* ref_map,
    const MarkdownExtensionRegistry* extensions,
    SourcePos base_pos
) {
    if (text.empty()) return {};

    const bool has_nl = contains_newline(text);

    // 256-byte fast-skip table for plain text runs inside parse_inlines_impl
    static constexpr std::array<uint8_t, 256> kInlineSpecial = []() {
        std::array<uint8_t, 256> t{};
        t[static_cast<uint8_t>('\n')] = 1;
        t[static_cast<uint8_t>('\\')] = 1;
        t[static_cast<uint8_t>('&')]  = 1;
        t[static_cast<uint8_t>('`')]  = 1;
        t[static_cast<uint8_t>('<')]  = 1;
        t[static_cast<uint8_t>('$')]  = 1;
        t[static_cast<uint8_t>('!')]  = 1;
        t[static_cast<uint8_t>('[')]  = 1;
        t[static_cast<uint8_t>(']')]  = 1;
        t[static_cast<uint8_t>('*')]  = 1;
        t[static_cast<uint8_t>('_')]  = 1;
        t[static_cast<uint8_t>('~')]  = 1;
        return t;
    }();
    const bool has_extensions = (extensions && !extensions->empty());

    // Ultra-fast path: plain text with zero special inline syntax chars (table cells, simple headings/paragraphs)
    if (!has_extensions) {
        bool any_special = false;
        for (char ch : text) {
            if (kInlineSpecial[static_cast<uint8_t>(ch)]) {
                any_special = true;
                break;
            }
        }
        if (!any_special) {
            SourcePos full_pos = make_span_sourcepos(text, 0, text.size(), base_pos, false);
            std::vector<InlineSpan> fast_out;
            if (options.autolink && has_autolink_trigger(text)) {
                split_text_with_gfm_autolinks(text, full_pos, options, fast_out);
                for (auto& piece : fast_out) {
                    if (piece.type == InlineType::Text) {
                        if (options.emojis && piece.text.find(':') != std::string::npos) {
                            piece.text = MarkdownParser::replace_emojis(piece.text);
                        }
                        if (options.smart_punctuation && has_smart_punct_trigger(piece.text)) {
                            piece.text = apply_smart_punctuation(piece.text, ' ', ' ');
                        }
                    }
                }
                return fast_out;
            }
            std::string s(text);
            if (options.emojis && s.find(':') != std::string::npos) {
                s = MarkdownParser::replace_emojis(s);
            }
            if (options.smart_punctuation && has_smart_punct_trigger(s)) {
                s = apply_smart_punctuation(s, ' ', ' ');
            }
            auto sp = InlineSpan::make_text(std::move(s));
            sp.sourcepos = full_pos;
            fast_out.push_back(std::move(sp));
            return fast_out;
        }
    }

    std::vector<InlineNode> nodes;
    nodes.reserve(32);
    std::vector<BracketEntry> brackets;

    size_t i = 0;
    size_t len = text.size();

    auto push_plain_text = [&](std::string s, size_t start_off, size_t end_off, bool literal = false) {
        if (s.empty()) return;
        InlineNode n;
        n.span = InlineSpan::make_text(std::move(s));
        n.span.sourcepos = make_span_sourcepos(text, start_off, end_off, base_pos, has_nl);
        n.start_off = start_off;
        n.end_off = end_off;
        n.is_escaped_or_literal = literal;
        nodes.push_back(std::move(n));
    };

    auto push_span = [&](InlineSpan sp, size_t start_off, size_t end_off) {
        if (!sp.sourcepos.is_valid()) {
            sp.sourcepos = make_span_sourcepos(text, start_off, end_off, base_pos, has_nl);
        }
        InlineNode n;
        n.span = std::move(sp);
        n.start_off = start_off;
        n.end_off = end_off;
        n.is_escaped_or_literal = true;
        nodes.push_back(std::move(n));
    };

    size_t plain_start = 0;
    auto flush_plain = [&](size_t end_off) {
        if (end_off > plain_start) {
            push_plain_text(std::string(text.substr(plain_start, end_off - plain_start)), plain_start, end_off, false);
        }
        plain_start = end_off;
    };

    while (i < len) {
        // Fast skip over ordinary text when no custom inline extensions are registered
        if (!has_extensions) {
            while (i < len && !kInlineSpecial[static_cast<uint8_t>(text[i])]) {
                i++;
            }
            if (i >= len) break;
        }

        char c = text[i];

        // 0. Custom inline extensions
        if (has_extensions) {
            auto [ln, cl] = offset_to_line_col(text, i, base_pos, has_nl);
            InlineExtensionContext ext_ctx{text, i, ln, cl, options};
            bool matched = false;
            for (const auto& ext : extensions->extensions()) {
                InlineSpan ext_span;
                size_t consumed = 0;
                if (ext->try_parse_inline(ext_ctx, ext_span, consumed) && consumed > 0) {
                    flush_plain(i);
                    push_span(std::move(ext_span), i, i + consumed);
                    i += consumed;
                    plain_start = i;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        // 1. Newline -> Hard LineBreak (2+ trailing spaces) or SoftBreak / no_breaks / hard_breaks
        if (c == '\n') {
            // Check for >= 2 trailing spaces before newline
            size_t space_end = i;
            size_t trailing_spaces = 0;
            while (space_end > plain_start && text[space_end - 1] == ' ') {
                space_end--;
                trailing_spaces++;
            }
            bool is_hard = (trailing_spaces >= 2) || options.hard_breaks;

            // Trim trailing spaces before newline
            size_t flush_end = (trailing_spaces >= 2) ? space_end : i;
            if (trailing_spaces == 1 && flush_end > plain_start) {
                // CommonMark strips trailing spaces before soft line breaks too
                flush_end--;
            }
            flush_plain(flush_end);

            // Also trim trailing spaces from previous plain text node if buffer was already flushed
            if (trailing_spaces == 0 && !nodes.empty() && nodes.back().span.type == InlineType::Text &&
                !nodes.back().is_delim && !nodes.back().is_escaped_or_literal) {
                auto& prev = nodes.back().span.text;
                size_t sp_cnt = 0;
                while (sp_cnt < prev.size() && prev[prev.size() - 1 - sp_cnt] == ' ') sp_cnt++;
                if (sp_cnt >= 2) is_hard = true;
                if (sp_cnt > 0) prev.resize(prev.size() - sp_cnt);
            }

            if (is_hard) {
                push_span(InlineSpan::make_linebreak(), i, i + 1);
            } else if (options.no_breaks) {
                push_plain_text(" ", i, i + 1, true);
            } else {
                push_span(InlineSpan::make_softbreak(), i, i + 1);
            }

            i++;
            // Skip leading spaces/tabs on the next line of paragraph
            while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
            plain_start = i;
            continue;
        }

        // 2. Backslash escapes (CommonMark 32 ASCII punctuation chars + hard line break)
        if (c == '\\' && i + 1 < len) {
            if (text[i + 1] == '\n') {
                flush_plain(i);
                push_span(InlineSpan::make_linebreak(), i, i + 2);
                i += 2;
                while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
                plain_start = i;
                continue;
            }
            if (Entities::is_ascii_punctuation(text[i + 1])) {
                flush_plain(i);
                push_plain_text(std::string(1, text[i + 1]), i, i + 2, true);
                i += 2;
                plain_start = i;
                continue;
            }
        }

        // 3. HTML5 Named Entities & Numeric/Hex Character References
        if (c == '&') {
            std::string decoded;
            size_t consumed = 0;
            if (Entities::decode_entity_at(text, i, decoded, consumed)) {
                flush_plain(i);
                push_plain_text(std::move(decoded), i, i + consumed, true);
                i += consumed;
                plain_start = i;
                continue;
            }
        }

        // 4. Multi-backtick Inline Code Spans (CommonMark 0.30 Section 6.1)
        if (c == '`') {
            size_t tick_len = 0;
            while (i + tick_len < len && text[i + tick_len] == '`') tick_len++;

            size_t search = i + tick_len;
            size_t close_pos = std::string_view::npos;
            while (search < len) {
                if (text[search] == '`') {
                    size_t run = 0;
                    while (search + run < len && text[search + run] == '`') run++;
                    if (run == tick_len) {
                        close_pos = search;
                        break;
                    }
                    search += run;
                } else {
                    search++;
                }
            }

            if (close_pos != std::string_view::npos) {
                flush_plain(i);
                std::string code(text.substr(i + tick_len, close_pos - (i + tick_len)));
                // Convert line endings to spaces
                for (char& ch : code) {
                    if (ch == '\n' || ch == '\r') ch = ' ';
                }
                // Strip one leading and one trailing space if both present and not all spaces
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') {
                    bool all_spaces = std::all_of(code.begin(), code.end(), [](char ch) { return ch == ' '; });
                    if (!all_spaces) {
                        code = code.substr(1, code.size() - 2);
                    }
                }
                push_span(InlineSpan::make_code(std::move(code)), i, close_pos + tick_len);
                i = close_pos + tick_len;
                plain_start = i;
                continue;
            } else {
                // Unmatched backtick run is literal text
                flush_plain(i);
                push_plain_text(std::string(tick_len, '`'), i, i + tick_len, true);
                i += tick_len;
                plain_start = i;
                continue;
            }
        }

        // 5. Angle-bracket Autolinks & Inline HTML Tags
        if (c == '<') {
            // 5a. CommonMark angle-bracket autolink: <https://...> or <user@domain.com>
            std::string auto_disp, auto_tgt;
            size_t auto_consumed = 0;
            if (try_parse_angle_autolink(text, i, auto_disp, auto_tgt, auto_consumed)) {
                flush_plain(i);
                auto link = InlineSpan::make_link(auto_disp, auto_tgt);
                auto ch = InlineSpan::make_text(auto_disp);
                ch.sourcepos = make_span_sourcepos(text, i + 1, i + auto_consumed - 1, base_pos);
                link.children.push_back(std::move(ch));
                push_span(std::move(link), i, i + auto_consumed);
                i += auto_consumed;
                plain_start = i;
                continue;
            }

            // 5b. Inline HTML Tags
            if (HtmlParser::is_tag_at(text, i)) {
                auto tag_opt = HtmlParser::parse_tag(text, i);
                if (tag_opt) {
                    const auto& tag = *tag_opt;
                    if (HtmlParser::is_safe_tag(tag.name)) {
                        // A. HTML Comments: <!-- ... -->
                        if (tag.name == "!--") {
                            flush_plain(i);
                            if (options.unsafe_html) {
                                push_span(InlineSpan::make_html_inline(std::string(text.substr(i, tag.raw_length))),
                                          i, i + tag.raw_length);
                            }
                            i += tag.raw_length;
                            plain_start = i;
                            continue;
                        }

                        // B. <br> or <br/>
                        if (tag.name == "br") {
                            flush_plain(i);
                            push_span(InlineSpan::make_linebreak(), i, i + tag.raw_length);
                            i += tag.raw_length;
                            plain_start = i;
                            continue;
                        }

                        // C. Standalone <img ...>
                        if (tag.name == "img" && !tag.is_closing) {
                            flush_plain(i);
                            push_span(InlineSpan::make_image(tag.get_attr("alt"), tag.get_attr("src"), tag.get_attr("title")),
                                      i, i + tag.raw_length);
                            i += tag.raw_length;
                            plain_start = i;
                            continue;
                        }

                        // D. Matched structured container tags: <kbd>, <sub>, <sup>, <mark>, <u>, <b>, <i>, <s>, <code>, <span>, <a>
                        if (!tag.is_closing && !tag.is_self_closing) {
                            std::string close_tag = "</" + tag.name + ">";
                            size_t close_pos = text.find(close_tag, i + tag.raw_length);
                            if (close_pos != std::string_view::npos) {
                                flush_plain(i);
                                auto inner_content = text.substr(i + tag.raw_length, close_pos - (i + tag.raw_length));
                                size_t end_all = close_pos + close_tag.size();

                                if (tag.name == "kbd") {
                                    push_span(InlineSpan::make_kbd(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "sub") {
                                    push_span(InlineSpan::make_subscript(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "sup") {
                                    push_span(InlineSpan::make_superscript(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "mark") {
                                    push_span(InlineSpan::make_highlight(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "u" || tag.name == "ins") {
                                    push_span(InlineSpan::make_underline(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "b" || tag.name == "strong") {
                                    auto sp = InlineSpan::make_bold(std::string(inner_content));
                                    sp.children = parse_inlines_impl(inner_content, options, ref_map, extensions,
                                        make_span_sourcepos(text, i + tag.raw_length, close_pos, base_pos));
                                    push_span(std::move(sp), i, end_all);
                                } else if (tag.name == "i" || tag.name == "em") {
                                    auto sp = InlineSpan::make_italic(std::string(inner_content));
                                    sp.children = parse_inlines_impl(inner_content, options, ref_map, extensions,
                                        make_span_sourcepos(text, i + tag.raw_length, close_pos, base_pos));
                                    push_span(std::move(sp), i, end_all);
                                } else if (tag.name == "s" || tag.name == "del" || tag.name == "strike") {
                                    push_span(InlineSpan::make_strike(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "code") {
                                    push_span(InlineSpan::make_code(std::string(inner_content)), i, end_all);
                                } else if (tag.name == "a") {
                                    push_span(InlineSpan::make_link(std::string(inner_content), tag.get_attr("href"), tag.get_attr("title")),
                                              i, end_all);
                                } else if (tag.name == "span" || tag.name == "font") {
                                    std::optional<Color> text_col = std::nullopt;
                                    std::optional<Color> bg_col = std::nullopt;
                                    std::string style_val = tag.get_attr("style");
                                    if (!style_val.empty()) {
                                        auto styles = HtmlParser::parse_style(style_val);
                                        if (styles.count("color")) text_col = HtmlParser::parse_color(styles["color"]);
                                        if (styles.count("background-color")) bg_col = HtmlParser::parse_color(styles["background-color"]);
                                        if (styles.count("background")) bg_col = HtmlParser::parse_color(styles["background"]);
                                    }
                                    std::string col_val = tag.get_attr("color");
                                    if (!col_val.empty()) text_col = HtmlParser::parse_color(col_val);

                                    auto inner_spans = parse_inlines_impl(
                                        inner_content, options, ref_map, extensions,
                                        make_span_sourcepos(text, i + tag.raw_length, close_pos, base_pos)
                                    );
                                    for (auto& s : inner_spans) {
                                        if (text_col) s.text_color = text_col;
                                        if (bg_col) s.bg_color = bg_col;
                                        push_span(std::move(s), i, end_all);
                                    }
                                } else if (options.unsafe_html) {
                                    std::string raw(text.substr(i, end_all - i));
                                    if (options.tagfilter) raw = HtmlParser::apply_tagfilter(raw);
                                    push_span(InlineSpan::make_html_inline(std::move(raw)), i, end_all);
                                }

                                i = end_all;
                                plain_start = i;
                                continue;
                            }
                        }

                        // Unmatched safe closing tag -> skip or emit raw if unsafe_html
                        if (tag.is_closing) {
                            flush_plain(i);
                            if (options.unsafe_html) {
                                std::string raw(text.substr(i, tag.raw_length));
                                if (options.tagfilter) raw = HtmlParser::apply_tagfilter(raw);
                                push_span(InlineSpan::make_html_inline(std::move(raw)), i, i + tag.raw_length);
                            }
                            i += tag.raw_length;
                            plain_start = i;
                            continue;
                        }
                    }

                    // Raw HTML inline tag (non-whitelisted or unmatched)
                    if (options.unsafe_html) {
                        flush_plain(i);
                        std::string raw(text.substr(i, tag.raw_length));
                        if (options.tagfilter) raw = HtmlParser::apply_tagfilter(raw);
                        push_span(InlineSpan::make_html_inline(std::move(raw)), i, i + tag.raw_length);
                        i += tag.raw_length;
                        plain_start = i;
                        continue;
                    }
                }
            }
        }

        // 6. Inline LaTeX Math: $...$ or $$...$$ (rejects currency like "$5 and $6")
        if (options.math && c == '$') {
            size_t open_len = (i + 1 < len && text[i + 1] == '$') ? 2 : 1;
            size_t content_start = i + open_len;
            if (content_start < len && !std::isspace(static_cast<unsigned char>(text[content_start]))) {
                size_t close = std::string_view::npos;
                for (size_t k = content_start; k < len; ++k) {
                    if (text[k] == '\\') { k++; continue; }
                    if (text[k] == '$' && (open_len == 1 || (k + 1 < len && text[k + 1] == '$'))) {
                        close = k;
                        break;
                    }
                }
                if (close != std::string_view::npos && close > content_start) {
                    bool space_before_close = std::isspace(static_cast<unsigned char>(text[close - 1])) != 0;
                    size_t after = close + open_len;
                    bool digit_after = after < len && std::isdigit(static_cast<unsigned char>(text[after]));
                    if (!space_before_close && !(open_len == 1 && digit_after)) {
                        flush_plain(i);
                        push_span(
                            InlineSpan::make_math(std::string(text.substr(content_start, close - content_start))),
                            i, close + open_len
                        );
                        i = close + open_len;
                        plain_start = i;
                        continue;
                    }
                }
            }
        }

        // 7. Footnote Reference: [^label]
        if (c == '[' && i + 2 < len && text[i + 1] == '^') {
            size_t fn_end = text.find(']', i + 2);
            if (fn_end != std::string_view::npos && fn_end > i + 2) {
                auto label = text.substr(i + 2, fn_end - (i + 2));
                if (label.find_first_of(" \t\n[") == std::string_view::npos) {
                    flush_plain(i);
                    int idx = 1;
                    if (std::isdigit(static_cast<unsigned char>(label[0]))) {
                        idx = std::atoi(std::string(label).c_str());
                    }
                    auto fn_span = InlineSpan::make_footnote_ref(idx, std::string(label));
                    fn_span.target = "#fn-" + std::string(label);
                    push_span(std::move(fn_span), i, fn_end + 1);
                    i = fn_end + 1;
                    plain_start = i;
                    continue;
                }
            }
        }

        // 8. Image Opener: ![
        if (c == '!' && i + 1 < len && text[i + 1] == '[') {
            flush_plain(i);
            size_t idx = nodes.size();
            push_plain_text("![", i, i + 2, true);
            brackets.push_back(BracketEntry{idx, i, true, true, false});
            i += 2;
            plain_start = i;
            continue;
        }

        // 9. Link Opener: [
        if (c == '[') {
            flush_plain(i);
            size_t idx = nodes.size();
            push_plain_text("[", i, i + 1, true);
            brackets.push_back(BracketEntry{idx, i, false, true, false});
            i++;
            plain_start = i;
            continue;
        }

        // 10. Link / Image Closer: ]
        if (c == ']') {
            if (!brackets.empty()) {
                BracketEntry opener = brackets.back();
                brackets.pop_back();

                if (opener.active) {
                    flush_plain(i);
                    size_t after_bracket = i + 1;
                    std::string url;
                    std::string title;
                    bool matched = false;
                    size_t end_pos = after_bracket;

                    // Extract raw label between opener and ']'
                    size_t inner_byte_start = opener.byte_pos + (opener.is_image ? 2 : 1);
                    std::string_view raw_label = text.substr(inner_byte_start, i - inner_byte_start);

                    // A. Inline link/image: [text](url "title")
                    if (after_bracket < len && text[after_bracket] == '(') {
                        size_t try_pos = after_bracket;
                        if (parse_inline_link_tail(text, try_pos, url, title)) {
                            matched = true;
                            end_pos = try_pos;
                        }
                    }

                    // B. Full [text][label] or Collapsed [text][] reference link/image
                    if (!matched && after_bracket < len && text[after_bracket] == '[') {
                        size_t ref_close = std::string_view::npos;
                        for (size_t k = after_bracket + 1; k < len; ++k) {
                            if (text[k] == '\\' && k + 1 < len) { k++; continue; }
                            if (text[k] == '[') break; // Unescaped '[' not allowed in label
                            if (text[k] == ']') { ref_close = k; break; }
                        }
                        if (ref_close != std::string_view::npos) {
                            std::string_view ref_id = text.substr(after_bracket + 1, ref_close - (after_bracket + 1));
                            if (ref_id.empty()) {
                                // Collapsed reference: [text][]
                                ref_id = raw_label;
                            }
                            std::string norm_key = Entities::normalize_reference_label(ref_id);
                            if (ref_map && !norm_key.empty()) {
                                auto it = ref_map->find(norm_key);
                                if (it != ref_map->end()) {
                                    url = it->second.first;
                                    title = it->second.second;
                                    matched = true;
                                    end_pos = ref_close + 1;
                                }
                            }
                        }
                    }

                    // C. Shortcut reference link/image: [text] (not followed by '[')
                    if (!matched && !(after_bracket < len && text[after_bracket] == '[')) {
                        std::string norm_key = Entities::normalize_reference_label(raw_label);
                        if (ref_map && !norm_key.empty()) {
                            auto it = ref_map->find(norm_key);
                            if (it != ref_map->end()) {
                                url = it->second.first;
                                title = it->second.second;
                                matched = true;
                                end_pos = after_bracket;
                            }
                        }
                    }

                    if (matched) {
                        // Resolve emphasis strictly inside the bracketed content
                        process_emphasis_range(nodes, opener.node_index + 1, nodes.size(), text, base_pos, has_nl);

                        // Extract inner spans (skipping deleted slots)
                        std::vector<InlineSpan> children;
                        for (size_t k = opener.node_index + 1; k < nodes.size(); ++k) {
                            auto& nd = nodes[k];
                            if (nd.deleted) continue;
                            if (nd.span.type == InlineType::Text && !nd.is_escaped_or_literal) {
                                std::string txt = std::move(nd.span.text);
                                if (options.emojis && txt.find(':') != std::string::npos) {
                                    txt = MarkdownParser::replace_emojis(txt);
                                }
                                if (options.smart_punctuation && has_smart_punct_trigger(txt)) {
                                    uint32_t prev_cp = (nd.start_off == 0) ? ' ' : Entities::decode_utf8_prev(text, nd.start_off);
                                    size_t np = nd.end_off;
                                    uint32_t next_cp = (np >= len) ? ' ' : Entities::decode_utf8(text, np);
                                    txt = apply_smart_punctuation(txt, prev_cp, next_cp);
                                }
                                auto child = InlineSpan::make_text(std::move(txt));
                                child.sourcepos = nd.span.sourcepos;
                                children.push_back(std::move(child));
                            } else {
                                children.push_back(std::move(nd.span));
                            }
                        }

                        std::string flat_text = flatten_span_text(children);
                        InlineSpan resolved = opener.is_image
                            ? InlineSpan::make_image(flat_text, std::move(url), std::move(title))
                            : InlineSpan::make_link(flat_text, std::move(url), std::move(title));
                        resolved.children = std::move(children);
                        resolved.sourcepos = make_span_sourcepos(text, opener.byte_pos, end_pos, base_pos, has_nl);

                        // Truncate tail back to opener.node_index + 1 and store resolved node in-place
                        nodes.resize(opener.node_index + 1);
                        auto& res_node = nodes[opener.node_index];
                        res_node.span = std::move(resolved);
                        res_node.start_off = opener.byte_pos;
                        res_node.end_off = end_pos;
                        res_node.is_delim = false;
                        res_node.deleted = false;
                        res_node.is_escaped_or_literal = true;

                        // CommonMark: links cannot contain other links -> deactivate all prior '[' openers
                        if (!opener.is_image) {
                            for (auto& b : brackets) {
                                if (!b.is_image) b.active = false;
                            }
                        }

                        i = end_pos;
                        plain_start = i;
                        continue;
                    }
                }
            }
        }

        // 11. Emphasis & Strikethrough Delimiter Runs: *, _, ~
        if (c == '*' || c == '_' || (c == '~' && options.strikethrough)) {
            size_t run = 0;
            while (i + run < len && text[i + run] == c) run++;

            if (c == '~' && run > 2) {
                // GFM strikethrough only allows 1 or 2 tildes
                flush_plain(i);
                push_plain_text(std::string(run, '~'), i, i + run, true);
                i += run;
                plain_start = i;
                continue;
            }

            flush_plain(i);

            uint32_t before = (i == 0) ? '\n' : Entities::decode_utf8_prev(text, i);
            size_t after_pos = i + run;
            uint32_t after = (after_pos >= len) ? '\n' : Entities::decode_utf8(text, after_pos);

            bool before_ws = Entities::is_unicode_whitespace(before);
            bool after_ws = Entities::is_unicode_whitespace(after);
            bool before_punct = Entities::is_unicode_punctuation(before);
            bool after_punct = Entities::is_unicode_punctuation(after);

            bool left_flanking = !after_ws && (!after_punct || before_ws || before_punct);
            bool right_flanking = !before_ws && (!before_punct || after_ws || after_punct);

            bool can_open = false;
            bool can_close = false;
            if (c == '_') {
                can_open = left_flanking && (!right_flanking || before_punct);
                can_close = right_flanking && (!left_flanking || after_punct);
            } else {
                can_open = left_flanking;
                can_close = right_flanking;
            }

            InlineNode dn;
            dn.span = InlineSpan::make_text(std::string(run, c));
            dn.span.sourcepos = make_span_sourcepos(text, i, i + run, base_pos);
            dn.start_off = i;
            dn.end_off = i + run;
            dn.is_delim = (can_open || can_close);
            dn.delim_char = c;
            dn.delim_orig_len = run;
            dn.delim_count = run;
            dn.can_open = can_open;
            dn.can_close = can_close;
            dn.is_escaped_or_literal = false;
            nodes.push_back(std::move(dn));

            i += run;
            plain_start = i;
            continue;
        }

        i++;
    }

    flush_plain(len);

    // Run full delimiter stack resolution on the entire top-level inline stream
    process_emphasis_range(nodes, 0, nodes.size(), text, base_pos, has_nl);

    // Coalesce adjacent live non-escaped Text nodes (including unmatched delimiters like '_' in URLs)
    // before running GFM autolink detection and smart punctuation.
    size_t write_idx = 0;
    for (size_t k = 0; k < nodes.size(); ++k) {
        if (nodes[k].deleted) continue;
        if (write_idx > 0 &&
            nodes[k].span.type == InlineType::Text && !nodes[k].is_escaped_or_literal &&
            nodes[write_idx - 1].span.type == InlineType::Text && !nodes[write_idx - 1].is_escaped_or_literal &&
            !nodes[k].span.text_color && !nodes[k].span.bg_color &&
            !nodes[write_idx - 1].span.text_color && !nodes[write_idx - 1].span.bg_color) {
            nodes[write_idx - 1].span.text += nodes[k].span.text;
            nodes[write_idx - 1].end_off = nodes[k].end_off;
            if (nodes[k].span.sourcepos.is_valid()) {
                nodes[write_idx - 1].span.sourcepos.end_line = nodes[k].span.sourcepos.end_line;
                nodes[write_idx - 1].span.sourcepos.end_col = nodes[k].span.sourcepos.end_col;
            }
        } else {
            if (write_idx != k) {
                nodes[write_idx] = std::move(nodes[k]);
            }
            write_idx++;
        }
    }
    nodes.resize(write_idx);

    // Post-process remaining plain text nodes: strict GFM autolinks, emojis, smart punctuation, and merge adjacent Text spans
    std::vector<InlineSpan> result;
    result.reserve(nodes.size());

    auto append_span_merging = [&](InlineSpan&& sp) {
        if (sp.type == InlineType::Text && !result.empty() && result.back().type == InlineType::Text &&
            !sp.text_color && !sp.bg_color && !result.back().text_color && !result.back().bg_color) {
            result.back().text += sp.text;
            if (sp.sourcepos.is_valid()) {
                if (!result.back().sourcepos.is_valid()) {
                    result.back().sourcepos = sp.sourcepos;
                } else {
                    result.back().sourcepos.end_line = sp.sourcepos.end_line;
                    result.back().sourcepos.end_col = sp.sourcepos.end_col;
                }
            }
        } else {
            result.push_back(std::move(sp));
        }
    };

    // Transform plain text: detect GFM autolinks FIRST so URLs aren't altered by smart punctuation/emojis
    auto decorate_plain_inplace = [&](std::string& s, size_t start_off, size_t end_off) {
        if (options.emojis && s.find(':') != std::string::npos) {
            s = MarkdownParser::replace_emojis(s);
        }
        if (options.smart_punctuation && has_smart_punct_trigger(s)) {
            uint32_t prev_cp = (start_off == 0) ? ' ' : Entities::decode_utf8_prev(text, start_off);
            size_t np = end_off;
            uint32_t next_cp = (np >= len) ? ' ' : Entities::decode_utf8(text, np);
            s = apply_smart_punctuation(s, prev_cp, next_cp);
        }
    };

    auto transform_span_tree = [&](auto& self, InlineSpan& sp, bool inside_link) -> void {
        if (sp.type == InlineType::CodeSpan || sp.type == InlineType::Math || sp.type == InlineType::HtmlInline) {
            return;
        }
        bool child_in_link = inside_link || (sp.type == InlineType::Link || sp.type == InlineType::Image);
        if (!sp.children.empty()) {
            bool needs_rebuild = false;
            for (auto& ch : sp.children) {
                if (ch.type == InlineType::Text && !ch.text.empty()) {
                    if (!child_in_link && options.autolink && has_autolink_trigger(ch.text)) {
                        needs_rebuild = true;
                        break;
                    }
                    if (options.emojis && ch.text.find(':') != std::string::npos) {
                        ch.text = MarkdownParser::replace_emojis(ch.text);
                    }
                    if (options.smart_punctuation && has_smart_punct_trigger(ch.text)) {
                        ch.text = apply_smart_punctuation(ch.text, ' ', ' ');
                    }
                } else {
                    self(self, ch, child_in_link);
                }
            }
            if (needs_rebuild) {
                std::vector<InlineSpan> new_children;
                new_children.reserve(sp.children.size() + 2);
                for (auto& ch : sp.children) {
                    if (ch.type == InlineType::Text && !ch.text.empty()) {
                        std::vector<InlineSpan> pieces;
                        split_text_with_gfm_autolinks(ch.text, ch.sourcepos, options, pieces);
                        for (auto& piece : pieces) {
                            if (piece.type == InlineType::Text) {
                                if (options.emojis && piece.text.find(':') != std::string::npos) {
                                    piece.text = MarkdownParser::replace_emojis(piece.text);
                                }
                                if (options.smart_punctuation && has_smart_punct_trigger(piece.text)) {
                                    piece.text = apply_smart_punctuation(piece.text, ' ', ' ');
                                }
                            }
                            new_children.push_back(std::move(piece));
                        }
                    } else {
                        self(self, ch, child_in_link);
                        new_children.push_back(std::move(ch));
                    }
                }
                sp.children = std::move(new_children);
            }
            if (sp.type == InlineType::Bold || sp.type == InlineType::Italic ||
                sp.type == InlineType::BoldItalic || sp.type == InlineType::Strikethrough ||
                sp.type == InlineType::Link || sp.type == InlineType::Image) {
                sp.text = flatten_span_text(sp.children);
            }
        }
    };

    for (auto& nd : nodes) {
        if (nd.span.type == InlineType::Text && !nd.is_escaped_or_literal) {
            if (options.autolink && has_autolink_trigger(nd.span.text)) {
                std::vector<InlineSpan> pieces;
                split_text_with_gfm_autolinks(nd.span.text, nd.span.sourcepos, options, pieces);
                for (auto& piece : pieces) {
                    if (piece.type == InlineType::Text) {
                        decorate_plain_inplace(piece.text, nd.start_off, nd.end_off);
                    }
                    append_span_merging(std::move(piece));
                }
            } else {
                decorate_plain_inplace(nd.span.text, nd.start_off, nd.end_off);
                append_span_merging(std::move(nd.span));
            }
        } else {
            transform_span_tree(transform_span_tree, nd.span, false);
            append_span_merging(std::move(nd.span));
        }
    }

    return result;
}

// --- Block-level Detection Helpers ---

/// Checks if a line is a Setext heading underline (`===+` -> 1, `---+` -> 2, else 0).
int parse_setext_underline(std::string_view line) noexcept {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (indent > 3 || first_non_ws >= line.size()) return 0;

    char ch = line[first_non_ws];
    if (ch != '=' && ch != '-') return 0;

    size_t i = first_non_ws;
    while (i < line.size() && line[i] == ch) i++;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    if (i == line.size()) {
        return (ch == '=') ? 1 : 2;
    }
    return 0;
}

/// Checks for fenced code opener (` ``` ` or `~~~`, >= 3 chars, 0..3 leading spaces).
bool parse_code_fence_opener(
    std::string_view line,
    char& out_char,
    size_t& out_len,
    size_t& out_indent,
    std::string& out_info
) {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (indent > 3 || first_non_ws >= line.size()) return false;

    char ch = line[first_non_ws];
    if (ch != '`' && ch != '~') return false;

    size_t run = 0;
    while (first_non_ws + run < line.size() && line[first_non_ws + run] == ch) run++;
    if (run < 3) return false;

    std::string_view info = trim(line.substr(first_non_ws + run));
    // CommonMark: backtick fence info string cannot contain backticks
    if (ch == '`' && info.find('`') != std::string_view::npos) return false;

    out_char = ch;
    out_len = run;
    out_indent = indent;
    // Extract first word of info string (unescaped) for language
    size_t sp = info.find_first_of(" \t");
    std::string_view lang_tok = (sp == std::string_view::npos) ? info : info.substr(0, sp);
    out_info = Entities::unescape_string(lang_tok);
    return true;
}

bool is_code_fence_closer(std::string_view line, char fence_char, size_t min_len) noexcept {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (indent > 3 || first_non_ws >= line.size()) return false;
    if (line[first_non_ws] != fence_char) return false;
    size_t run = 0;
    while (first_non_ws + run < line.size() && line[first_non_ws + run] == fence_char) run++;
    if (run < min_len) return false;
    return is_blank(line.substr(first_non_ws + run));
}

struct ListMarkerInfo {
    bool is_ordered{false};
    int start_num{1};
    char marker_char{'-'};
    size_t indent_cols{0};
    size_t content_col{0};      // Column where list item content starts
    size_t content_byte{0};     // Byte offset in line where item content starts
};

bool parse_list_marker(std::string_view line, bool interrupts_paragraph, ListMarkerInfo& out) noexcept {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (first_non_ws >= line.size()) return false;

    std::string_view rest = line.substr(first_non_ws);
    size_t marker_bytes = 0;
    bool is_ord = false;
    int start_num = 1;
    char mchar = 0;

    if (rest[0] == '-' || rest[0] == '*' || rest[0] == '+') {
        mchar = rest[0];
        marker_bytes = 1;
    } else if (std::isdigit(static_cast<unsigned char>(rest[0]))) {
        size_t d = 0;
        while (d < rest.size() && d < 9 && std::isdigit(static_cast<unsigned char>(rest[d]))) d++;
        if (d > 0 && d < rest.size() && (rest[d] == '.' || rest[d] == ')')) {
            is_ord = true;
            mchar = rest[d];
            start_num = 0;
            for (size_t k = 0; k < d; ++k) start_num = start_num * 10 + (rest[k] - '0');
            marker_bytes = d + 1;
        }
    }

    if (marker_bytes == 0) return false;

    // Must be followed by at least one space/tab or end of line
    size_t after_marker = first_non_ws + marker_bytes;
    if (after_marker < line.size() && line[after_marker] != ' ' && line[after_marker] != '\t') {
        return false;
    }

    // CommonMark: only an ordered list starting with 1 can interrupt a paragraph, and empty list item cannot interrupt paragraph
    if (interrupts_paragraph) {
        if (is_ord && start_num != 1) return false;
        if (is_blank(line.substr(after_marker))) return false;
    }

    // Calculate spaces after marker
    size_t col = indent + marker_bytes;
    size_t p = after_marker;
    size_t spaces_after = 0;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) {
        size_t step = (line[p] == '\t') ? (4 - (col % 4)) : 1;
        spaces_after += step;
        col += step;
        p++;
        if (spaces_after >= 5) break;
    }

    if (spaces_after == 0 || spaces_after >= 5 || p >= line.size()) {
        // CommonMark: if 5+ spaces or blank after marker, content indentation is marker + 1 space
        out.content_col = indent + marker_bytes + 1;
        out.content_byte = (after_marker < line.size()) ? (after_marker + 1) : line.size();
    } else {
        out.content_col = col;
        out.content_byte = p;
    }

    out.is_ordered = is_ord;
    out.start_num = start_num;
    out.marker_char = mchar;
    out.indent_cols = indent;
    return true;
}

/// Checks if a line starts a CommonMark raw HTML block (types 1..7) excluding structured <details> / <div align=...>.
/// Returns html_block_type (1..7) or 0 if not a raw HTML block.
int detect_raw_html_block_type(std::string_view line) {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (indent > 3 || first_non_ws >= line.size() || line[first_non_ws] != '<') return 0;

    std::string_view s = line.substr(first_non_ws);
    std::string lower = to_lower_ascii(s);

    // Type 2: <!-- ... -->
    if (lower.starts_with("<!--")) return 2;
    // Type 3: <? ... ?>
    if (lower.starts_with("<?")) return 3;
    // Type 5: <![CDATA[ ... ]]>
    if (s.starts_with("<![CDATA[")) return 5;
    // Type 4: <!UPPER ... >
    if (s.size() >= 3 && s[1] == '!' && std::isalpha(static_cast<unsigned char>(s[2]))) return 4;

    // Extract tag name
    size_t j = 1;
    bool is_closing = false;
    if (j < lower.size() && lower[j] == '/') { is_closing = true; j++; }
    size_t name_start = j;
    while (j < lower.size() && (std::isalnum(static_cast<unsigned char>(lower[j])) || lower[j] == '-')) j++;
    if (j == name_start) return 0;
    std::string_view tag_name = std::string_view(lower).substr(name_start, j - name_start);
    bool boundary = (j >= lower.size() || lower[j] == '>' || lower[j] == '/' ||
                     std::isspace(static_cast<unsigned char>(lower[j])));
    if (!boundary) return 0;

    // Type 1: <pre>, <script>, <style>, <textarea>
    if (!is_closing && (tag_name == "pre" || tag_name == "script" || tag_name == "style" || tag_name == "textarea")) {
        return 1;
    }

    // Type 6: Standard block tags + GFM tagfilter tags (iframe, xmp, title, noembed, noframes, plaintext)
    static const char* const kType6Tags[] = {
        "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption",
        "center", "col", "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt",
        "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset",
        "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr", "html", "iframe",
        "legend", "li", "link", "main", "menu", "menuitem", "nav", "noembed", "noframes",
        "ol", "optgroup", "option", "p", "param", "plaintext", "search", "section", "summary",
        "table", "tbody", "td", "tfoot", "th", "thead", "title", "tr", "track", "ul", "xmp"
    };
    for (const char* t : kType6Tags) {
        if (tag_name == t) return 6;
    }

    // Type 7: Complete single open/close tag on its own line
    auto parsed = HtmlParser::parse_tag(s, 0);
    if (parsed && is_blank(s.substr(parsed->raw_length))) {
        return 7;
    }

    return 0;
}

bool is_raw_html_block_end(std::string_view line, int html_type) {
    if (html_type == 6 || html_type == 7) {
        return is_blank(line);
    }
    std::string lower = to_lower_ascii(line);
    if (html_type == 1) {
        return lower.find("</pre>") != std::string::npos ||
               lower.find("</script>") != std::string::npos ||
               lower.find("</style>") != std::string::npos ||
               lower.find("</textarea>") != std::string::npos;
    }
    if (html_type == 2) return line.find("-->") != std::string_view::npos;
    if (html_type == 3) return line.find("?>") != std::string_view::npos;
    if (html_type == 4) return line.find('>') != std::string_view::npos;
    if (html_type == 5) return line.find("]]>") != std::string_view::npos;
    return false;
}

/// Attempts to parse a complete Link Reference Definition from `buf` (1 or more lines).
/// Returns:
///   1 = valid definition parsed (`out_key`, `out_url`, `out_title`, `out_lines_consumed`)
///   0 = incomplete (needs more lines, e.g., multi-line title or URL on next line)
///  -1 = definitely NOT a valid link reference definition
int try_parse_ref_definition(
    const std::vector<std::string>& lines,
    std::string& out_key,
    std::string& out_url,
    std::string& out_title,
    size_t& out_lines_consumed
) {
    if (lines.empty()) return -1;

    // Join lines with '\n'
    std::string joined;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k > 0) joined.push_back('\n');
        joined += lines[k];
    }

    std::string_view sv(joined);
    size_t i = 0;
    size_t len = sv.size();

    // Up to 3 leading spaces
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(lines[0], first_non_ws);
    if (indent > 3 || first_non_ws >= lines[0].size() || lines[0][first_non_ws] != '[') return -1;
    // Footnote definitions [^id]: are NOT link reference definitions
    if (first_non_ws + 1 < lines[0].size() && lines[0][first_non_ws + 1] == '^') return -1;

    i = first_non_ws + 1;
    // Scan label (1..999 chars, no unescaped '[' or ']', no blank line)
    size_t label_start = i;
    size_t label_end = std::string_view::npos;
    while (i < len && (i - label_start) <= 999) {
        if (sv[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if (sv[i] == '[') return -1;
        if (sv[i] == ']') { label_end = i; break; }
        i++;
    }
    if (label_end == std::string_view::npos) {
        // Could continue on next line unless last line was blank or >999 chars
        if (lines.size() > 1 && is_blank(lines.back())) return -1;
        if (len - label_start > 999) return -1;
        return 0;
    }

    std::string_view raw_label = sv.substr(label_start, label_end - label_start);
    std::string norm_key = Entities::normalize_reference_label(raw_label);
    if (norm_key.empty()) return -1;

    i = label_end + 1;
    if (i >= len || sv[i] != ':') return -1;
    i++;

    // Optional spaces/tabs and up to ONE newline before destination
    size_t newlines_seen = 0;
    while (i < len && (sv[i] == ' ' || sv[i] == '\t' || sv[i] == '\n')) {
        if (sv[i] == '\n') {
            newlines_seen++;
            if (newlines_seen > 1) return -1;
        }
        i++;
    }
    if (i >= len) {
        return (newlines_seen == 0) ? 0 : -1;
    }

    // Parse destination
    std::string dest;
    size_t dest_pos = i;
    if (!parse_link_destination(sv, dest_pos, dest)) return -1;
    i = dest_pos;

    // Count line index where destination ended
    auto count_lines_up_to = [&](size_t pos) -> size_t {
        size_t c = 1;
        for (size_t k = 0; k < pos && k < sv.size(); ++k) {
            if (sv[k] == '\n') c++;
        }
        return c;
    };

    size_t dest_line_count = count_lines_up_to(i);

    // Check rest of destination's line
    size_t after_dest_ws = i;
    bool had_ws_or_nl = false;
    bool had_nl_after_dest = false;
    while (after_dest_ws < len && (sv[after_dest_ws] == ' ' || sv[after_dest_ws] == '\t')) {
        had_ws_or_nl = true;
        after_dest_ws++;
    }

    // Case 1: Nothing else after destination on this line -> check if more lines exist for an optional title
    if (after_dest_ws == len) {
        // Complete as a title-less refdef if finalized, or could have a title on the next line
        out_key = std::move(norm_key);
        out_url = std::move(dest);
        out_title.clear();
        out_lines_consumed = dest_line_count;
        return 1;
    }

    if (sv[after_dest_ws] == '\n') {
        had_ws_or_nl = true;
        had_nl_after_dest = true;
        after_dest_ws++;
        while (after_dest_ws < len && (sv[after_dest_ws] == ' ' || sv[after_dest_ws] == '\t')) {
            after_dest_ws++;
        }
    }

    // Try parsing optional title
    if (had_ws_or_nl && after_dest_ws < len &&
        (sv[after_dest_ws] == '"' || sv[after_dest_ws] == '\'' || sv[after_dest_ws] == '(')) {
        size_t tpos = after_dest_ws;
        std::string title;
        if (parse_link_title(sv, tpos, title)) {
            // Must have only spaces/tabs to end of title's line
            size_t tail = tpos;
            while (tail < len && (sv[tail] == ' ' || sv[tail] == '\t')) tail++;
            if (tail == len || sv[tail] == '\n') {
                // Reject blank lines inside title
                if (title.find("\n\n") == std::string::npos) {
                    out_key = std::move(norm_key);
                    out_url = std::move(dest);
                    out_title = std::move(title);
                    out_lines_consumed = count_lines_up_to(tpos);
                    return 1;
                }
            }
            // Title had trailing garbage on its line: if title was on next line, fall back to title-less def!
            if (had_nl_after_dest) {
                out_key = std::move(norm_key);
                out_url = std::move(dest);
                out_title.clear();
                out_lines_consumed = dest_line_count;
                return 1;
            }
            return -1;
        } else {
            // Unclosed title: could span multiple lines if last line isn't blank
            if (!is_blank(lines.back())) {
                return 0; // Need more lines
            }
            if (had_nl_after_dest) {
                out_key = std::move(norm_key);
                out_url = std::move(dest);
                out_title.clear();
                out_lines_consumed = dest_line_count;
                return 1;
            }
            return -1;
        }
    }

    // Non-title content after destination:
    if (had_nl_after_dest) {
        // Next line is unrelated -> destination alone is a valid refdef!
        out_key = std::move(norm_key);
        out_url = std::move(dest);
        out_title.clear();
        out_lines_consumed = dest_line_count;
        return 1;
    }

    // Garbage on the same line after destination -> invalid refdef
    return -1;
}

} // namespace

// ============================================================================
// Incremental Streaming Block Parser State Machine (`MarkdownParser::Impl`)
// ============================================================================

struct MarkdownParser::Impl {
    MarkdownParseOptions options{};
    MarkdownExtensionRegistry extensions{};

    // Streaming input buffers
    std::string utf8_carry{};
    std::string pending_line{};
    bool pending_cr{false};
    uint32_t current_line_num{0};
    uint32_t last_nonempty_col{1};

    // Incremental document state
    std::shared_ptr<BlockNode> root{std::make_shared<BlockNode>(BlockType::Document)};
    RefMap ref_map{};
    const RefMap* external_ref_map{nullptr};
    const MarkdownExtensionRegistry* external_extensions{nullptr};
    uint64_t ref_map_version{0};
    bool is_finished{false};

    [[nodiscard]] const RefMap* active_ref_map() const noexcept {
        return external_ref_map ? external_ref_map : &ref_map;
    }
    [[nodiscard]] const MarkdownExtensionRegistry* active_extensions() const noexcept {
        return external_extensions ? external_extensions : &extensions;
    }

    // Blocks that may need deferred inline re-parse if forward reference definitions arrive later
    struct DeferredInlineRecord {
        std::weak_ptr<BlockNode> node;
        std::string raw_text;
        SourcePos base_pos;
        uint64_t parsed_ref_version{0};
        // For table cells: row & col index (-1 = block->inlines directly)
        int table_row{-1};
        int table_col{-1};
        bool is_footnote{false};
        std::string footnote_label{};
    };
    std::vector<DeferredInlineRecord> deferred_inlines{};

    // Currently open trailing block state
    enum class OpenKind : uint8_t {
        None,
        Paragraph,
        FencedCode,
        IndentedCode,
        MathBlock,
        Alert,
        BlockQuote,
        Details,
        AlignDiv,
        RawHtml,
        Table,
        List,
        RefDef
    };

    OpenKind open_kind{OpenKind::None};
    std::shared_ptr<BlockNode> open_node{nullptr};

    // Open block buffers
    uint32_t open_start_line{0};
    uint32_t open_start_col{1};
    uint32_t open_end_line{0};
    uint32_t open_end_col{1};

    // Paragraph buffer
    std::vector<std::string> para_lines{};

    // Fenced code state
    char fence_char{'`'};
    size_t fence_len{3};
    size_t fence_indent{0};

    // Indented code buffered blank lines
    std::vector<std::string> indented_blank_buf{};

    // Sub-document accumulation (BlockQuote, Alert, Details, AlignDiv)
    std::string sub_content{};
    uint32_t sub_base_line{0};
    uint32_t sub_col_offset{0};
    std::string align_end_tag{};
    uint8_t align_val{0};
    bool quote_last_was_blank{false};

    // Raw HTML block state
    int raw_html_type{0};

    // List state
    bool list_is_ordered{false};
    char list_marker_char{'-'};
    size_t list_pending_blanks{0};
    struct OpenListItem {
        std::shared_ptr<BlockNode> node;
        size_t content_col{2};
        uint32_t start_line{1};
        uint32_t content_start_col{1};
        std::vector<std::string> raw_lines;
        bool had_internal_blank{false};
        bool saw_blank_before_next{false};
    };
    std::vector<OpenListItem> open_list_items{};

    // Reference definition candidate buffer
    std::vector<std::string> refdef_lines{};
    uint32_t refdef_start_line{0};

    // Optional base line offset (for recursive sub-document parsing with accurate SourcePos!)
    uint32_t line_offset{0};
    uint32_t col_offset{0};

    void reset_state() {
        utf8_carry.clear();
        pending_line.clear();
        pending_cr = false;
        current_line_num = 0;
        last_nonempty_col = 1;
        root = std::make_shared<BlockNode>(BlockType::Document);
        ref_map.clear();
        ref_map_version = 0;
        is_finished = false;
        deferred_inlines.clear();
        clear_open_block();
    }

    void clear_open_block() {
        open_kind = OpenKind::None;
        open_node.reset();
        open_start_line = 0;
        open_start_col = 1;
        open_end_line = 0;
        open_end_col = 1;
        para_lines.clear();
        indented_blank_buf.clear();
        sub_content.clear();
        align_end_tag.clear();
        quote_last_was_blank = false;
        raw_html_type = 0;
        list_pending_blanks = 0;
        open_list_items.clear();
        refdef_lines.clear();
    }

    void parse_and_track_inlines(
        const std::shared_ptr<BlockNode>& node,
        const std::string& raw,
        SourcePos base_pos,
        bool is_footnote = false,
        const std::string& fn_label = {}
    ) {
        node->inlines = parse_inlines_impl(raw, options, active_ref_map(), active_extensions(), base_pos);
        if (is_footnote) {
            node->inlines.push_back(InlineSpan::make_link(" ↩", "#fnref-" + fn_label));
        }
        if (!external_ref_map && raw.find('[') != std::string::npos) {
            deferred_inlines.push_back(DeferredInlineRecord{
                node, raw, base_pos, ref_map_version, -1, -1, is_footnote, fn_label
            });
        }
    }

    void parse_and_track_table_cell(
        const std::shared_ptr<BlockNode>& tbl,
        int row_idx,
        int col_idx,
        const std::string& raw,
        SourcePos base_pos
    ) {
        tbl->table_rows[static_cast<size_t>(row_idx)].cells[static_cast<size_t>(col_idx)].inlines =
            parse_inlines_impl(raw, options, active_ref_map(), active_extensions(), base_pos);
        if (!external_ref_map && raw.find('[') != std::string::npos) {
            deferred_inlines.push_back(DeferredInlineRecord{
                tbl, raw, base_pos, ref_map_version, row_idx, col_idx, false, {}
            });
        }
    }

    std::shared_ptr<BlockNode> parse_subdocument(
        const std::string& content,
        uint32_t sub_line_base,
        uint32_t sub_col_base
    ) const {
        Impl sub;
        sub.options = options;
        sub.options.validate_utf8 = false; // Parent stream already validated UTF-8
        sub.external_extensions = active_extensions();
        sub.external_ref_map = active_ref_map();
        sub.ref_map_version = ref_map_version;
        sub.line_offset = sub_line_base;
        sub.col_offset = sub_col_base;
        sub.feed_validated_bytes(content);
        return sub.finalize_doc();
    }

    bool line_interrupts_paragraph(std::string_view raw_line) const {
        size_t first_non_ws = 0;
        size_t indent = count_indent_cols(raw_line, first_non_ws);
        if (first_non_ws >= raw_line.size()) return true; // blank
        if (indent > 3) return false; // Indented code CANNOT interrupt a paragraph!

        std::string_view s = raw_line.substr(first_non_ws);
        char c0 = s[0];
        switch (c0) {
            case '`':
            case '~': {
                char fc = 0; size_t fl = 0, fi = 0; std::string finfo;
                return parse_code_fence_opener(raw_line, fc, fl, fi, finfo);
            }
            case '$':
                return options.math && s.starts_with("$$");
            case '#': {
                std::string_view htxt;
                return MarkdownParser::parse_heading_level(raw_line, htxt) > 0;
            }
            case '>':
                return true;
            case '<': {
                if (s.starts_with("<details") || s.starts_with("<div") || s.starts_with("<p ") || s.starts_with("<p>")) return true;
                int html_t = detect_raw_html_block_type(raw_line);
                return html_t >= 1 && html_t <= 6;
            }
            case '-':
            case '*':
            case '_': {
                if (MarkdownParser::is_thematic_break(raw_line)) return true;
                if (c0 != '_') {
                    ListMarkerInfo lmi;
                    if (parse_list_marker(raw_line, true, lmi)) return true;
                }
                return false;
            }
            case '+': {
                ListMarkerInfo lmi;
                return parse_list_marker(raw_line, true, lmi);
            }
            case '[':
                return s.starts_with("[^") && s.find("]:") != std::string_view::npos;
            default:
                if (c0 >= '0' && c0 <= '9') {
                    ListMarkerInfo lmi;
                    return parse_list_marker(raw_line, true, lmi);
                }
                return false;
        }
    }

    void close_open_block() {
        switch (open_kind) {
            case OpenKind::None:
                return;

            case OpenKind::Paragraph: {
                if (!para_lines.empty()) {
                    auto para = std::make_shared<BlockNode>(BlockType::Paragraph);
                    para->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    if (para_lines.size() == 1) {
                        parse_and_track_inlines(para, para_lines[0], para->sourcepos);
                    } else {
                        std::string joined;
                        size_t total = para_lines.size();
                        for (const auto& pl : para_lines) total += pl.size();
                        joined.reserve(total);
                        for (size_t k = 0; k < para_lines.size(); ++k) {
                            if (k > 0) joined.push_back('\n');
                            joined += para_lines[k];
                        }
                        parse_and_track_inlines(para, joined, para->sourcepos);
                    }
                    root->children.push_back(std::move(para));
                }
                break;
            }

            case OpenKind::FencedCode: {
                if (open_node) {
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    std::string lang_lower = to_lower_ascii(open_node->language);
                    if (options.diagrams && lang_lower == "mermaid") {
                        std::string src;
                        for (const auto& cl : open_node->code_lines) { src += cl; src += '\n'; }
                        open_node->type = BlockType::Diagram;
                        open_node->source = std::move(src);
                    } else if (options.math && (lang_lower == "math" || lang_lower == "latex" || lang_lower == "tex")) {
                        std::string src;
                        for (const auto& cl : open_node->code_lines) { src += cl; src += '\n'; }
                        open_node->type = BlockType::MathBlock;
                        open_node->source = std::move(src);
                    }
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::IndentedCode: {
                if (open_node && !open_node->code_lines.empty()) {
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::MathBlock: {
                if (open_node) {
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::Alert:
            case OpenKind::BlockQuote:
            case OpenKind::Details: {
                if (open_node) {
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    if (!sub_content.empty()) {
                        auto sub_doc = parse_subdocument(sub_content, sub_base_line, sub_col_offset);
                        open_node->children = std::move(sub_doc->children);
                    }
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::AlignDiv: {
                if (!sub_content.empty()) {
                    auto sub_doc = parse_subdocument(sub_content, sub_base_line, sub_col_offset);
                    for (auto& child : sub_doc->children) {
                        if (child) {
                            child->block_align = align_val;
                            root->children.push_back(std::move(child));
                        }
                    }
                }
                break;
            }

            case OpenKind::RawHtml: {
                if (open_node && options.unsafe_html) {
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    if (options.tagfilter) {
                        open_node->raw_html = HtmlParser::apply_tagfilter(open_node->raw_html);
                    }
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::Table: {
                if (open_node) {
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::List: {
                if (open_node) {
                    bool is_loose = open_node->is_loose_list;
                    for (auto& item : open_list_items) {
                        if (item.had_internal_blank) is_loose = true;

                        // Determine if item is simple single-paragraph or multi-block
                        std::string item_src;
                        for (size_t k = 0; k < item.raw_lines.size(); ++k) {
                            if (k > 0) item_src.push_back('\n');
                            item_src += item.raw_lines[k];
                        }

                        // Check if item contains block-level sub-elements (sub-lists, code blocks, multiple paragraphs)
                        bool has_multi_blocks = item.had_internal_blank;
                        if (!has_multi_blocks && item.raw_lines.size() > 1) {
                            for (size_t k = 1; k < item.raw_lines.size(); ++k) {
                                if (line_interrupts_paragraph(item.raw_lines[k])) {
                                    has_multi_blocks = true;
                                    break;
                                }
                            }
                        }

                        if (has_multi_blocks) {
                            auto sub_doc = parse_subdocument(item_src, item.start_line - 1, item.content_start_col - 1);
                            if (!sub_doc->children.empty() && sub_doc->children[0]->type == BlockType::Paragraph) {
                                item.node->inlines = sub_doc->children[0]->inlines;
                            }
                            item.node->children = std::move(sub_doc->children);
                        } else {
                            SourcePos ipos{item.start_line, item.content_start_col,
                                           item.start_line + static_cast<uint32_t>(item.raw_lines.size() > 0 ? item.raw_lines.size() - 1 : 0),
                                           static_cast<uint32_t>(item.raw_lines.empty() ? item.content_start_col : item.content_start_col + item.raw_lines.back().size())};
                            parse_and_track_inlines(item.node, item_src, ipos);
                        }
                        open_node->children.push_back(std::move(item.node));
                    }
                    open_node->is_loose_list = is_loose;
                    open_node->is_tight_list = !is_loose;
                    open_node->sourcepos = SourcePos{open_start_line, open_start_col, open_end_line, open_end_col};
                    root->children.push_back(std::move(open_node));
                }
                break;
            }

            case OpenKind::RefDef: {
                if (!refdef_lines.empty()) {
                    std::string key, url, title;
                    size_t consumed = 0;
                    int res = try_parse_ref_definition(refdef_lines, key, url, title, consumed);
                    if (res == 1) {
                        if (ref_map.find(key) == ref_map.end()) {
                            ref_map[key] = {std::move(url), std::move(title)};
                            ref_map_version++;
                        }
                        // Replay any unconsumed trailing lines
                        auto leftover = std::vector<std::string>(
                            refdef_lines.begin() + static_cast<std::ptrdiff_t>(consumed),
                            refdef_lines.end()
                        );
                        uint32_t replay_line = refdef_start_line + static_cast<uint32_t>(consumed);
                        clear_open_block();
                        for (size_t k = 0; k < leftover.size(); ++k) {
                            process_single_line(leftover[k], replay_line + static_cast<uint32_t>(k));
                        }
                        return;
                    } else {
                        // Failed refdef -> replay buffered lines as paragraph / normal lines!
                        auto buffered = std::move(refdef_lines);
                        uint32_t start_ln = refdef_start_line;
                        clear_open_block();
                        for (size_t k = 0; k < buffered.size(); ++k) {
                            process_single_line_no_refdef(buffered[k], start_ln + static_cast<uint32_t>(k));
                        }
                        return;
                    }
                }
                break;
            }
        }
        clear_open_block();
    }

    void process_single_line(std::string_view raw_line, uint32_t line_num) {
        process_single_line_impl(raw_line, line_num, true);
    }

    void process_single_line_no_refdef(std::string_view raw_line, uint32_t line_num) {
        process_single_line_impl(raw_line, line_num, false);
    }

    void process_single_line_impl(std::string_view raw_line, uint32_t line_num, bool allow_refdef) {
        // Strip trailing \r if any
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.remove_suffix(1);
        }
        uint32_t line_end_col = static_cast<uint32_t>(raw_line.size() + col_offset);
        if (line_end_col == 0) line_end_col = 1;

        // ---- 1. Continue currently open block if applicable ----
        switch (open_kind) {
            case OpenKind::FencedCode: {
                if (is_code_fence_closer(raw_line, fence_char, fence_len)) {
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    close_open_block();
                    return;
                }
                open_node->code_lines.push_back(strip_indent_cols(raw_line, fence_indent));
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }

            case OpenKind::MathBlock: {
                auto ml = trim(raw_line);
                size_t close_pos = ml.find("$$");
                if (close_pos != std::string_view::npos) {
                    if (close_pos > 0) {
                        open_node->source += std::string(ml.substr(0, close_pos));
                    }
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    close_open_block();
                    return;
                }
                open_node->source += std::string(ml);
                open_node->source += '\n';
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }

            case OpenKind::Details: {
                auto d_line = trim(raw_line);
                if (d_line.starts_with("</details>")) {
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    close_open_block();
                    return;
                }
                if (d_line.starts_with("<summary>") || d_line.starts_with("<summary ")) {
                    size_t gt = d_line.find('>');
                    if (gt != std::string_view::npos) {
                        size_t s_start = gt + 1;
                        size_t s_end = d_line.find("</summary>");
                        if (s_end != std::string_view::npos) {
                            open_node->summary_text = std::string(trim(d_line.substr(s_start, s_end - s_start)));
                        } else {
                            open_node->summary_text = std::string(trim(d_line.substr(s_start)));
                        }
                    }
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                if (sub_content.empty()) {
                    sub_base_line = line_num - 1;
                }
                sub_content += std::string(raw_line);
                sub_content += '\n';
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }

            case OpenKind::AlignDiv: {
                if (trim(raw_line).starts_with(align_end_tag)) {
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    close_open_block();
                    return;
                }
                if (sub_content.empty()) {
                    sub_base_line = line_num - 1;
                }
                sub_content += std::string(raw_line);
                sub_content += '\n';
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }

            case OpenKind::RawHtml: {
                if (raw_html_type == 6 || raw_html_type == 7) {
                    if (is_blank(raw_line)) {
                        close_open_block();
                        return;
                    }
                    open_node->raw_html += '\n';
                    open_node->raw_html += std::string(raw_line);
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                open_node->raw_html += '\n';
                open_node->raw_html += std::string(raw_line);
                open_end_line = line_num;
                open_end_col = line_end_col;
                if (is_raw_html_block_end(raw_line, raw_html_type)) {
                    close_open_block();
                }
                return;
            }

            case OpenKind::IndentedCode: {
                if (is_blank(raw_line)) {
                    indented_blank_buf.push_back(strip_indent_cols(raw_line, 4));
                    return;
                }
                size_t fnw = 0;
                if (count_indent_cols(raw_line, fnw) >= 4) {
                    for (auto& bl : indented_blank_buf) {
                        open_node->code_lines.push_back(std::move(bl));
                    }
                    indented_blank_buf.clear();
                    open_node->code_lines.push_back(strip_indent_cols(raw_line, 4));
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                // Indented code ended
                close_open_block();
                break;
            }

            case OpenKind::Alert:
            case OpenKind::BlockQuote: {
                size_t fnw = 0;
                size_t indent = count_indent_cols(raw_line, fnw);
                if (indent <= 3 && fnw < raw_line.size() && raw_line[fnw] == '>') {
                    std::string_view q = raw_line.substr(fnw + 1);
                    if (!q.empty() && (q.front() == ' ' || q.front() == '\t')) {
                        if (q.front() == ' ') q.remove_prefix(1);
                    }
                    if (sub_content.empty()) {
                        sub_base_line = line_num - 1;
                    }
                    sub_content += std::string(q);
                    sub_content += '\n';
                    quote_last_was_blank = is_blank(q);
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                if (is_blank(raw_line)) {
                    close_open_block();
                    return;
                }
                // Lazy continuation: only if quote didn't just see a blank line AND this line doesn't start a new block
                if (!quote_last_was_blank && !line_interrupts_paragraph(raw_line)) {
                    sub_content += std::string(trim_start(raw_line));
                    sub_content += '\n';
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                close_open_block();
                break;
            }

            case OpenKind::Table: {
                std::vector<std::string> row_cells;
                if (!is_blank(raw_line) && MarkdownParser::parse_table_row(raw_line, row_cells) && !row_cells.empty()) {
                    TableRow data_row;
                    data_row.is_header = false;
                    size_t num_cols = open_node->table_alignments.size();
                    int row_idx = static_cast<int>(open_node->table_rows.size());
                    for (size_t c = 0; c < num_cols; ++c) {
                        TableCell cell;
                        cell.align = open_node->table_alignments[c];
                        data_row.cells.push_back(std::move(cell));
                    }
                    open_node->table_rows.push_back(std::move(data_row));
                    for (size_t c = 0; c < num_cols && c < row_cells.size(); ++c) {
                        parse_and_track_table_cell(
                            open_node, row_idx, static_cast<int>(c), row_cells[c],
                            SourcePos{line_num, 1 + col_offset, line_num, line_end_col}
                        );
                    }
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                close_open_block();
                break;
            }

            case OpenKind::List: {
                if (is_blank(raw_line)) {
                    list_pending_blanks++;
                    return;
                }

                size_t fnw = 0;
                size_t indent = count_indent_cols(raw_line, fnw);

                // Check if this line starts a new list item in the same list
                ListMarkerInfo lmi;
                bool is_item = parse_list_marker(raw_line, false, lmi);
                if (is_item && lmi.is_ordered == list_is_ordered) {
                    // Notice: keep flat depth_level items for same list kind (preserving Nisaba's existing list_level model!)
                    if (list_pending_blanks > 0) {
                        open_node->is_loose_list = true;
                        open_node->is_tight_list = false;
                        list_pending_blanks = 0;
                    }
                    int depth_level = static_cast<int>(lmi.indent_cols / 2);
                    auto item_node = std::make_shared<BlockNode>(BlockType::ListItem);
                    item_node->list_level = depth_level;
                    item_node->list_marker = lmi.marker_char;
                    item_node->sourcepos = SourcePos{line_num, static_cast<uint32_t>(lmi.indent_cols + 1 + col_offset), line_num, line_end_col};

                    std::string_view item_content = (lmi.content_byte < raw_line.size())
                        ? trim(raw_line.substr(lmi.content_byte))
                        : std::string_view{};
                    size_t content_col_start = lmi.content_col + 1 + col_offset;

                    if (options.task_lists &&
                        (item_content.starts_with("[ ] ") || item_content.starts_with("[x] ") || item_content.starts_with("[X] "))) {
                        item_node->is_task_item = true;
                        item_node->is_task_checked = (item_content[1] == 'x' || item_content[1] == 'X');
                        item_content = trim(item_content.substr(4));
                        content_col_start += 4;
                    }

                    OpenListItem oli;
                    oli.node = std::move(item_node);
                    oli.content_col = lmi.content_col;
                    oli.start_line = line_num;
                    oli.content_start_col = static_cast<uint32_t>(content_col_start);
                    oli.raw_lines.push_back(std::string(item_content));
                    open_list_items.push_back(std::move(oli));

                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }

                // Indented continuation of current list item (e.g. second paragraph, indented code, nested list)
                if (!open_list_items.empty() && indent >= open_list_items.back().content_col) {
                    auto& cur_item = open_list_items.back();
                    if (list_pending_blanks > 0) {
                        cur_item.had_internal_blank = true;
                        cur_item.raw_lines.push_back("");
                        list_pending_blanks = 0;
                    }
                    cur_item.raw_lines.push_back(strip_indent_cols(raw_line, cur_item.content_col));
                    cur_item.node->sourcepos.end_line = line_num;
                    cur_item.node->sourcepos.end_col = line_end_col;
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }

                // Lazy continuation of current list item's paragraph (when no blank line preceded and not a block interrupt)
                if (!open_list_items.empty() && list_pending_blanks == 0 && !line_interrupts_paragraph(raw_line)) {
                    auto& cur_item = open_list_items.back();
                    cur_item.raw_lines.push_back(std::string(trim_start(raw_line)));
                    cur_item.node->sourcepos.end_line = line_num;
                    cur_item.node->sourcepos.end_col = line_end_col;
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }

                close_open_block();
                break;
            }

            case OpenKind::RefDef: {
                refdef_lines.push_back(std::string(raw_line));
                std::string key, url, title;
                size_t consumed = 0;
                int res = try_parse_ref_definition(refdef_lines, key, url, title, consumed);
                if (res == 0) {
                    // Still accumulating multi-line refdef
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
                // Either res == 1 (complete!) or res == -1 (failed -> replay)
                close_open_block();
                return;
            }

            case OpenKind::Paragraph: {
                // Fast check for Setext Heading underline (only when first non-ws is '=' or '-')
                size_t pfnw = 0;
                while (pfnw < raw_line.size() && (raw_line[pfnw] == ' ' || raw_line[pfnw] == '\t')) pfnw++;
                if (pfnw < raw_line.size() && (raw_line[pfnw] == '=' || raw_line[pfnw] == '-')) {
                    int setext_lvl = parse_setext_underline(raw_line);
                    if (setext_lvl > 0) {
                        std::string joined;
                        for (size_t k = 0; k < para_lines.size(); ++k) {
                            if (k > 0) joined.push_back('\n');
                            joined += para_lines[k];
                        }
                        auto h_node = std::make_shared<BlockNode>(BlockType::Heading);
                        h_node->heading_level = setext_lvl;
                        h_node->is_setext_heading = true;
                        h_node->anchor_id = MarkdownParser::slugify(joined);
                        h_node->sourcepos = SourcePos{open_start_line, open_start_col, line_num, line_end_col};
                        parse_and_track_inlines(h_node, joined, SourcePos{open_start_line, open_start_col, open_end_line, open_end_col});
                        root->children.push_back(std::move(h_node));
                        clear_open_block();
                        return;
                    }
                }

                // Check for GFM Table separator following a single-line paragraph (fast '|' gate)
                if (options.tables && para_lines.size() == 1 &&
                    para_lines[0].find('|') != std::string::npos &&
                    raw_line.find('|') != std::string_view::npos) {
                    std::vector<std::string> header_cells;
                    std::vector<TableAlign> alignments;
                    if (MarkdownParser::parse_table_row(para_lines[0], header_cells) &&
                        MarkdownParser::parse_table_separator(raw_line, alignments) &&
                        !header_cells.empty() && header_cells.size() == alignments.size()) {
                        auto table_node = std::make_shared<BlockNode>(BlockType::Table);
                        table_node->table_alignments = alignments;

                        TableRow header_row;
                        header_row.is_header = true;
                        for (size_t c = 0; c < header_cells.size(); ++c) {
                            TableCell cell;
                            cell.align = alignments[c];
                            header_row.cells.push_back(std::move(cell));
                        }
                        table_node->table_rows.push_back(std::move(header_row));

                        uint32_t hdr_line = open_start_line;
                        uint32_t hdr_col = open_start_col;
                        for (size_t c = 0; c < header_cells.size(); ++c) {
                            parse_and_track_table_cell(
                                table_node, 0, static_cast<int>(c), header_cells[c],
                                SourcePos{hdr_line, hdr_col, hdr_line, open_end_col}
                            );
                        }

                        open_kind = OpenKind::Table;
                        open_node = std::move(table_node);
                        open_end_line = line_num;
                        open_end_col = line_end_col;
                        para_lines.clear();
                        return;
                    }
                }

                if (line_interrupts_paragraph(raw_line)) {
                    close_open_block();
                    break;
                }

                // Continue paragraph
                para_lines.push_back(std::string(trim_start(raw_line)));
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }

            case OpenKind::None:
                break;
        }

        // ---- 2. Start a new block ----
        size_t first_non_ws = 0;
        size_t indent = count_indent_cols(raw_line, first_non_ws);
        if (first_non_ws >= raw_line.size()) return; // blank line
        auto trimmed = trim_end(raw_line.substr(first_non_ws));
        if (trimmed.empty()) return;

        uint32_t start_col = static_cast<uint32_t>(first_non_ws + 1 + col_offset);
        char c0 = trimmed[0];

        // 2.0 Pluggable Block Extensions
        const auto* exts = active_extensions();
        if (exts && !exts->empty()) {
            std::vector<std::string_view> single_line_view{raw_line};
            BlockExtensionContext bctx{single_line_view, 0, line_num - 1, options};
            for (const auto& ext : exts->extensions()) {
                std::shared_ptr<BlockNode> ext_block;
                size_t consumed = 0;
                if (ext->try_parse_block(bctx, ext_block, consumed) && ext_block) {
                    if (!ext_block->sourcepos.is_valid()) {
                        ext_block->sourcepos = SourcePos{line_num, start_col, line_num, line_end_col};
                    }
                    root->children.push_back(std::move(ext_block));
                    return;
                }
            }
        }

        // 2.1 Indented Code Block (4+ spaces or leading tab)
        if (indent >= 4) {
            auto code_block = std::make_shared<BlockNode>(BlockType::CodeBlock);
            code_block->is_indented_code = true;
            code_block->code_lines.push_back(strip_indent_cols(raw_line, 4));
            open_kind = OpenKind::IndentedCode;
            open_node = std::move(code_block);
            open_start_line = line_num;
            open_start_col = 1 + col_offset;
            open_end_line = line_num;
            open_end_col = line_end_col;
            return;
        }

        // 2.2 Fenced Code Block (``` or ~~~)
        if (c0 == '`' || c0 == '~') {
            char fc = 0;
            size_t fl = 0, fi = 0;
            std::string lang;
            if (parse_code_fence_opener(raw_line, fc, fl, fi, lang)) {
                auto code_block = std::make_shared<BlockNode>(BlockType::CodeBlock);
                code_block->language = std::move(lang);
                open_kind = OpenKind::FencedCode;
                open_node = std::move(code_block);
                fence_char = fc;
                fence_len = fl;
                fence_indent = fi;
                open_start_line = line_num;
                open_start_col = start_col;
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }
        }

        // 2.3 Display Math Block: $$ ... $$
        if (c0 == '$' && options.math && trimmed.starts_with("$$")) {
            auto math_node = std::make_shared<BlockNode>(BlockType::MathBlock);
            auto after_open = trim(trimmed.substr(2));
            size_t close_same = after_open.find("$$");
            if (close_same != std::string_view::npos) {
                math_node->source = std::string(trim(after_open.substr(0, close_same)));
                math_node->sourcepos = SourcePos{line_num, start_col, line_num, line_end_col};
                root->children.push_back(std::move(math_node));
                return;
            }
            if (!after_open.empty()) {
                math_node->source += std::string(after_open);
                math_node->source += '\n';
            }
            open_kind = OpenKind::MathBlock;
            open_node = std::move(math_node);
            open_start_line = line_num;
            open_start_col = start_col;
            open_end_line = line_num;
            open_end_col = line_end_col;
            return;
        }

        // 2.4 Thematic Break: ---, ***, ___
        if ((c0 == '-' || c0 == '*' || c0 == '_') && MarkdownParser::is_thematic_break(raw_line)) {
            auto hr = std::make_shared<BlockNode>(BlockType::ThematicBreak);
            hr->sourcepos = SourcePos{line_num, start_col, line_num, line_end_col};
            root->children.push_back(std::move(hr));
            return;
        }

        // 2.5 ATX Heading: # .. ######
        if (c0 == '#') {
            std::string_view heading_text;
            int h_level = MarkdownParser::parse_heading_level(raw_line, heading_text);
            if (h_level > 0) {
                auto h_node = std::make_shared<BlockNode>(BlockType::Heading);
                h_node->heading_level = h_level;
                h_node->anchor_id = MarkdownParser::slugify(heading_text);
                h_node->sourcepos = SourcePos{line_num, start_col, line_num, line_end_col};
                size_t text_off = heading_text.empty()
                    ? first_non_ws + static_cast<size_t>(h_level)
                    : static_cast<size_t>(heading_text.data() - raw_line.data());
                SourcePos in_base{line_num, static_cast<uint32_t>(text_off + 1 + col_offset), line_num, line_end_col};
                parse_and_track_inlines(h_node, std::string(heading_text), in_base);
                root->children.push_back(std::move(h_node));
                return;
            }
        }

        // 2.6 & 2.7 GitHub Alert Callout or BlockQuote (> ...)
        if (c0 == '>') {
            std::string alert_title;
            AlertType alert_type = MarkdownParser::parse_alert_marker(trimmed, alert_title);
            if (alert_type != AlertType::None) {
                auto alert_node = std::make_shared<BlockNode>(BlockType::Alert);
                alert_node->alert_type = alert_type;
                alert_node->alert_title = std::move(alert_title);
                open_kind = OpenKind::Alert;
                open_node = std::move(alert_node);
                open_start_line = line_num;
                open_start_col = start_col;
                open_end_line = line_num;
                open_end_col = line_end_col;
                sub_base_line = line_num;
                sub_col_offset = col_offset + 2;
                quote_last_was_blank = false;
                return;
            }

            auto quote_node = std::make_shared<BlockNode>(BlockType::BlockQuote);
            std::string_view q = raw_line.substr(first_non_ws + 1);
            if (!q.empty() && q.front() == ' ') q.remove_prefix(1);
            open_kind = OpenKind::BlockQuote;
            open_node = std::move(quote_node);
            open_start_line = line_num;
            open_start_col = start_col;
            open_end_line = line_num;
            open_end_col = line_end_col;
            sub_base_line = line_num - 1;
            sub_col_offset = static_cast<uint32_t>(first_non_ws + 2 + col_offset);
            sub_content = std::string(q) + "\n";
            quote_last_was_blank = is_blank(q);
            return;
        }

        // 2.8, 2.9, 2.10 HTML Blocks (<details>, <div align=...>, Raw HTML)
        if (c0 == '<') {
            if (trimmed.starts_with("<details")) {
                auto details_node = std::make_shared<BlockNode>(BlockType::Details);
                details_node->is_open = trimmed.find("open") != std::string_view::npos;
                details_node->summary_text = "Details";
                open_kind = OpenKind::Details;
                open_node = std::move(details_node);
                open_start_line = line_num;
                open_start_col = start_col;
                open_end_line = line_num;
                open_end_col = line_end_col;
                sub_base_line = line_num;
                sub_col_offset = col_offset;
                return;
            }

            if (trimmed.starts_with("<div") || trimmed.starts_with("<p ") || trimmed.starts_with("<p>")) {
                auto tag_opt = HtmlParser::parse_tag(trimmed, 0);
                if (tag_opt && !tag_opt->get_attr("align").empty()) {
                    uint8_t av = 0;
                    std::string align_attr = tag_opt->get_attr("align");
                    if (align_attr == "center") av = 1;
                    else if (align_attr == "right") av = 2;

                    std::string end_t = "</" + tag_opt->name + ">";
                    size_t close_same = trimmed.find(end_t, tag_opt->raw_length);
                    if (close_same != std::string_view::npos) {
                        std::string inner(trimmed.substr(tag_opt->raw_length, close_same - tag_opt->raw_length));
                        auto sub_doc = parse_subdocument(inner, line_num - 1, col_offset);
                        for (auto& ch : sub_doc->children) {
                            if (ch) {
                                ch->block_align = av;
                                root->children.push_back(std::move(ch));
                            }
                        }
                        return;
                    }

                    open_kind = OpenKind::AlignDiv;
                    align_val = av;
                    align_end_tag = std::move(end_t);
                    open_start_line = line_num;
                    open_start_col = start_col;
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    sub_base_line = line_num;
                    sub_col_offset = col_offset;
                    return;
                }
            }

            int html_t = detect_raw_html_block_type(raw_line);
            if (html_t > 0) {
                auto html_node = std::make_shared<BlockNode>(BlockType::HtmlBlock);
                html_node->raw_html = std::string(raw_line);
                open_kind = OpenKind::RawHtml;
                open_node = std::move(html_node);
                raw_html_type = html_t;
                open_start_line = line_num;
                open_start_col = start_col;
                open_end_line = line_num;
                open_end_col = line_end_col;
                if (html_t <= 5 && is_raw_html_block_end(raw_line, html_t)) {
                    close_open_block();
                }
                return;
            }
        }

        // 2.11 Footnote Definition & 2.12 Link Reference Definition Candidate
        if (c0 == '[') {
            if (trimmed.starts_with("[^") && trimmed.find("]:") != std::string_view::npos) {
                size_t close_bracket = trimmed.find("]:");
                auto label = trimmed.substr(2, close_bracket - 2);
                auto content = trim(trimmed.substr(close_bracket + 2));

                auto fn_node = std::make_shared<BlockNode>(BlockType::FootnoteDef);
                fn_node->footnote_label = std::string(label);
                fn_node->footnote_index = std::atoi(std::string(label).c_str());
                fn_node->anchor_id = "fn-" + std::string(label);
                fn_node->sourcepos = SourcePos{line_num, start_col, line_num, line_end_col};
                parse_and_track_inlines(
                    fn_node, std::string(content),
                    SourcePos{line_num, static_cast<uint32_t>(first_non_ws + close_bracket + 3 + col_offset), line_num, line_end_col},
                    true, fn_node->footnote_label
                );
                root->children.push_back(std::move(fn_node));
                return;
            }

            if (allow_refdef && !trimmed.starts_with("[^")) {
                std::vector<std::string> candidate{std::string(raw_line)};
                std::string key, url, title;
                size_t consumed = 0;
                int res = try_parse_ref_definition(candidate, key, url, title, consumed);
                if (res == 1 || res == 0) {
                    if (res == 1 && !title.empty()) {
                        if (ref_map.find(key) == ref_map.end()) {
                            ref_map[key] = {std::move(url), std::move(title)};
                            ref_map_version++;
                        }
                        return;
                    }
                    open_kind = OpenKind::RefDef;
                    refdef_lines = std::move(candidate);
                    refdef_start_line = line_num;
                    open_start_line = line_num;
                    open_start_col = start_col;
                    open_end_line = line_num;
                    open_end_col = line_end_col;
                    return;
                }
            }
        }

        // 2.13 Lists (Unordered & Ordered)
        if (c0 == '-' || c0 == '*' || c0 == '+' || (c0 >= '0' && c0 <= '9')) {
            ListMarkerInfo lmi;
            if (parse_list_marker(raw_line, false, lmi)) {
                auto list_node = std::make_shared<BlockNode>(BlockType::List);
                list_node->is_ordered_list = lmi.is_ordered;
                list_node->list_start_number = lmi.start_num;
                list_node->list_marker = lmi.marker_char;
                list_node->is_loose_list = false;
                list_node->is_tight_list = true;

                auto item_node = std::make_shared<BlockNode>(BlockType::ListItem);
                item_node->list_level = static_cast<int>(lmi.indent_cols / 2);
                item_node->list_marker = lmi.marker_char;
                item_node->sourcepos = SourcePos{line_num, start_col, line_num, line_end_col};

                std::string_view item_content = (lmi.content_byte < raw_line.size())
                    ? trim(raw_line.substr(lmi.content_byte))
                    : std::string_view{};
                size_t content_col_start = lmi.content_col + 1 + col_offset;

                if (options.task_lists &&
                    (item_content.starts_with("[ ] ") || item_content.starts_with("[x] ") || item_content.starts_with("[X] "))) {
                    item_node->is_task_item = true;
                    item_node->is_task_checked = (item_content[1] == 'x' || item_content[1] == 'X');
                    item_content = trim(item_content.substr(4));
                    content_col_start += 4;
                }

                OpenListItem oli;
                oli.node = std::move(item_node);
                oli.content_col = lmi.content_col;
                oli.start_line = line_num;
                oli.content_start_col = static_cast<uint32_t>(content_col_start);
                oli.raw_lines.push_back(std::string(item_content));

                open_kind = OpenKind::List;
                open_node = std::move(list_node);
                list_is_ordered = lmi.is_ordered;
                list_marker_char = lmi.marker_char;
                list_pending_blanks = 0;
                open_list_items.clear();
                open_list_items.push_back(std::move(oli));

                open_start_line = line_num;
                open_start_col = start_col;
                open_end_line = line_num;
                open_end_col = line_end_col;
                return;
            }
        }

        // 2.14 Paragraph
        open_kind = OpenKind::Paragraph;
        para_lines.clear();
        para_lines.push_back(std::string(trim_start(raw_line)));
        open_start_line = line_num;
        open_start_col = start_col;
        open_end_line = line_num;
        open_end_col = line_end_col;
    }

    void feed_validated_bytes(std::string_view data) {
        size_t i = 0;
        if (pending_cr) {
            pending_cr = false;
            if (!data.empty() && data[0] == '\n') {
                i = 1;
            }
            current_line_num++;
            last_nonempty_col = static_cast<uint32_t>(std::max<size_t>(1, pending_line.size()));
            process_single_line(pending_line, current_line_num + line_offset);
            pending_line.clear();
        }

        while (i < data.size()) {
            size_t nl = data.find_first_of("\r\n", i);
            if (nl == std::string_view::npos) {
                pending_line.append(data.substr(i));
                break;
            }

            bool is_cr = (data[nl] == '\r');
            if (is_cr && nl + 1 >= data.size()) {
                pending_line.append(data.substr(i, nl - i));
                pending_cr = true;
                break;
            }

            size_t next_i = is_cr ? ((data[nl + 1] == '\n') ? (nl + 2) : (nl + 1)) : (nl + 1);
            if (pending_line.empty()) {
                // Zero-copy fast path: pass line slice directly without copying into pending_line
                std::string_view line_sv = data.substr(i, nl - i);
                current_line_num++;
                last_nonempty_col = static_cast<uint32_t>(std::max<size_t>(1, line_sv.size()));
                process_single_line(line_sv, current_line_num + line_offset);
            } else {
                pending_line.append(data.substr(i, nl - i));
                current_line_num++;
                last_nonempty_col = static_cast<uint32_t>(std::max<size_t>(1, pending_line.size()));
                process_single_line(pending_line, current_line_num + line_offset);
                pending_line.clear();
            }
            i = next_i;
        }
    }

    void feed_chunk(std::string_view chunk) {
        if (chunk.empty()) return;
        is_finished = false;

        if (!options.validate_utf8) {
            feed_validated_bytes(chunk);
            return;
        }

        // Hold incomplete trailing UTF-8 sequence across chunk boundaries
        std::string combined;
        if (!utf8_carry.empty()) {
            combined = std::move(utf8_carry);
            utf8_carry.clear();
            combined.append(chunk);
            chunk = combined;
        }

        size_t safe_len = chunk.size();
        // Check up to last 3 bytes for an incomplete multi-byte UTF-8 sequence
        for (size_t back = 1; back <= 3 && back <= chunk.size(); ++back) {
            uint8_t b = static_cast<uint8_t>(chunk[chunk.size() - back]);
            if ((b & 0xC0) == 0x80) continue; // Continuation byte
            size_t expected = 1;
            if ((b & 0xE0) == 0xC0) expected = 2;
            else if ((b & 0xF0) == 0xE0) expected = 3;
            else if ((b & 0xF8) == 0xF0) expected = 4;
            if (expected > back) {
                safe_len = chunk.size() - back;
                utf8_carry.assign(chunk.substr(safe_len));
            }
            break;
        }

        if (safe_len > 0) {
            std::string_view ready = chunk.substr(0, safe_len);
            if (Entities::is_valid_utf8(ready)) {
                feed_validated_bytes(ready);
            } else {
                std::string sanitized = Entities::sanitize_utf8(ready);
                feed_validated_bytes(sanitized);
            }
        }
    }

    void resolve_deferred_inlines() {
        for (auto& rec : deferred_inlines) {
            if (rec.parsed_ref_version == ref_map_version) continue;
            auto sp = rec.node.lock();
            if (!sp) continue;
            if (rec.table_row >= 0 && rec.table_col >= 0) {
                size_t r = static_cast<size_t>(rec.table_row);
                size_t c = static_cast<size_t>(rec.table_col);
                if (r < sp->table_rows.size() && c < sp->table_rows[r].cells.size()) {
                    sp->table_rows[r].cells[c].inlines =
                        parse_inlines_impl(rec.raw_text, options, &ref_map, &extensions, rec.base_pos);
                }
            } else {
                sp->inlines = parse_inlines_impl(rec.raw_text, options, &ref_map, &extensions, rec.base_pos);
                if (rec.is_footnote) {
                    sp->inlines.push_back(InlineSpan::make_link(" ↩", "#fnref-" + rec.footnote_label));
                }
            }
            rec.parsed_ref_version = ref_map_version;
        }
    }

    std::shared_ptr<BlockNode> finalize_doc() {
        if (!utf8_carry.empty()) {
            std::string sanitized = Entities::sanitize_utf8(utf8_carry);
            utf8_carry.clear();
            feed_validated_bytes(sanitized);
        }
        if (pending_cr) {
            pending_cr = false;
            current_line_num++;
            last_nonempty_col = static_cast<uint32_t>(std::max<size_t>(1, pending_line.size()));
            process_single_line(pending_line, current_line_num + line_offset);
            pending_line.clear();
        } else if (!pending_line.empty()) {
            current_line_num++;
            last_nonempty_col = static_cast<uint32_t>(std::max<size_t>(1, pending_line.size()));
            process_single_line(pending_line, current_line_num + line_offset);
            pending_line.clear();
        }

        close_open_block();
        resolve_deferred_inlines();

        if (current_line_num > 0) {
            root->sourcepos = SourcePos{1 + line_offset, 1 + col_offset, current_line_num + line_offset, last_nonempty_col};
        }
        is_finished = true;
        return root;
    }

    std::shared_ptr<BlockNode> build_snapshot() const {
        auto snap = std::make_shared<BlockNode>(BlockType::Document);
        snap->children = root->children;
        snap->sourcepos = root->sourcepos;

        // If there is an open block or pending partial line, finalize ONLY a lightweight tail copy
        if (open_kind != OpenKind::None || !pending_line.empty() || !utf8_carry.empty()) {
            Impl tail;
            tail.options = options;
            tail.extensions = extensions;
            tail.ref_map = ref_map;
            tail.ref_map_version = ref_map_version;
            tail.line_offset = line_offset;
            tail.col_offset = col_offset;
            tail.current_line_num = current_line_num;
            tail.last_nonempty_col = last_nonempty_col;
            tail.utf8_carry = utf8_carry;
            tail.pending_line = pending_line;
            tail.pending_cr = pending_cr;
            tail.open_kind = open_kind;
            tail.open_start_line = open_start_line;
            tail.open_start_col = open_start_col;
            tail.open_end_line = open_end_line;
            tail.open_end_col = open_end_col;
            tail.para_lines = para_lines;
            tail.fence_char = fence_char;
            tail.fence_len = fence_len;
            tail.fence_indent = fence_indent;
            tail.indented_blank_buf = indented_blank_buf;
            tail.sub_content = sub_content;
            tail.sub_base_line = sub_base_line;
            tail.sub_col_offset = sub_col_offset;
            tail.align_end_tag = align_end_tag;
            tail.align_val = align_val;
            tail.quote_last_was_blank = quote_last_was_blank;
            tail.raw_html_type = raw_html_type;
            tail.list_is_ordered = list_is_ordered;
            tail.list_marker_char = list_marker_char;
            tail.list_pending_blanks = list_pending_blanks;
            tail.refdef_lines = refdef_lines;
            tail.refdef_start_line = refdef_start_line;

            if (open_node) {
                tail.open_node = std::make_shared<BlockNode>(*open_node);
            }
            for (const auto& oli : open_list_items) {
                OpenListItem copy = oli;
                copy.node = std::make_shared<BlockNode>(*oli.node);
                tail.open_list_items.push_back(std::move(copy));
            }

            auto tail_doc = tail.finalize_doc();
            for (auto& ch : tail_doc->children) {
                snap->children.push_back(std::move(ch));
            }
            if (tail_doc->sourcepos.is_valid()) {
                snap->sourcepos = tail_doc->sourcepos;
            }
        }
        return snap;
    }
};

// ============================================================================
// MarkdownParser Public Methods
// ============================================================================

MarkdownParser::MarkdownParser(MarkdownParseOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
}

MarkdownParser::~MarkdownParser() = default;
MarkdownParser::MarkdownParser(MarkdownParser&&) noexcept = default;
MarkdownParser& MarkdownParser::operator=(MarkdownParser&&) noexcept = default;

void MarkdownParser::set_options(const MarkdownParseOptions& options) {
    impl_->options = options;
}

const MarkdownParseOptions& MarkdownParser::options() const noexcept {
    return impl_->options;
}

void MarkdownParser::register_extension(std::shared_ptr<MarkdownExtension> ext) {
    impl_->extensions.register_extension(std::move(ext));
}

const MarkdownExtensionRegistry& MarkdownParser::extensions() const noexcept {
    return impl_->extensions;
}

void MarkdownParser::reset() {
    impl_->reset_state();
}

void MarkdownParser::feed(std::string_view chunk) {
    impl_->feed_chunk(chunk);
}

std::shared_ptr<BlockNode> MarkdownParser::snapshot() const {
    return impl_->build_snapshot();
}

std::shared_ptr<BlockNode> MarkdownParser::finish() {
    return impl_->finalize_doc();
}

std::shared_ptr<BlockNode> MarkdownParser::parse(std::string_view markdown) {
    MarkdownParser parser;
    parser.feed(markdown);
    return parser.finish();
}

std::shared_ptr<BlockNode> MarkdownParser::parse(
    std::string_view markdown,
    const MarkdownParseOptions& options,
    const MarkdownExtensionRegistry* extensions
) {
    MarkdownParser parser(options);
    if (extensions) {
        for (const auto& ext : extensions->extensions()) {
            parser.register_extension(ext);
        }
    }
    parser.feed(markdown);
    return parser.finish();
}

std::vector<InlineSpan> MarkdownParser::parse_inlines(
    std::string_view text,
    const std::unordered_map<std::string, std::pair<std::string, std::string>>* ref_map
) {
    MarkdownParseOptions defaults;
    return parse_inlines_impl(text, defaults, ref_map, nullptr, SourcePos{1, 1, 1, 1});
}

std::vector<InlineSpan> MarkdownParser::parse_inlines(
    std::string_view text,
    const MarkdownParseOptions& options,
    const std::unordered_map<std::string, std::pair<std::string, std::string>>* ref_map,
    const MarkdownExtensionRegistry* extensions,
    SourcePos base_pos
) {
    std::string validated;
    if (options.validate_utf8) {
        validated = Entities::sanitize_utf8(text);
        text = validated;
    }
    if (!base_pos.is_valid()) {
        base_pos = SourcePos{1, 1, 1, 1};
    }
    return parse_inlines_impl(text, options, ref_map, extensions, base_pos);
}

std::string MarkdownParser::replace_emojis(std::string_view text) {
    if (text.find(':') == std::string_view::npos) {
        return std::string(text);
    }
    std::string result;
    result.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ':') {
            size_t end_colon = text.find(':', i + 1);
            if (end_colon != std::string_view::npos && end_colon - i > 1) {
                auto candidate = text.substr(i + 1, end_colon - i - 1);
                if (candidate.find(' ') == std::string_view::npos) {
                    auto it = kEmojiMap.find(candidate);
                    if (it != kEmojiMap.end()) {
                        result += it->second;
                        i = end_colon + 1;
                        continue;
                    }
                }
            }
        }
        result += text[i];
        i++;
    }
    return result;
}

std::string MarkdownParser::slugify(std::string_view text) {
    std::string slug;
    slug.reserve(text.size());

    bool prev_dash = false;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            prev_dash = false;
        } else if (c == ' ' || c == '-' || c == '_') {
            if (!prev_dash && !slug.empty()) {
                slug.push_back('-');
                prev_dash = true;
            }
        }
    }

    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug;
}

bool MarkdownParser::is_thematic_break(std::string_view line) {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (indent > 3) return false;
    auto s = trim(line.substr(first_non_ws));
    if (s.size() < 3) return false;
    char c = s[0];
    if (c != '-' && c != '*' && c != '_') return false;

    size_t count = 0;
    for (char ch : s) {
        if (ch == c) {
            count++;
        } else if (ch != ' ' && ch != '\t') {
            return false;
        }
    }
    return count >= 3;
}

int MarkdownParser::parse_heading_level(std::string_view line, std::string_view& out_text) {
    size_t first_non_ws = 0;
    size_t indent = count_indent_cols(line, first_non_ws);
    if (indent > 3 || first_non_ws >= line.size() || line[first_non_ws] != '#') return 0;

    auto s = line.substr(first_non_ws);
    int level = 0;
    while (level < 6 && static_cast<size_t>(level) < s.size() && s[level] == '#') {
        level++;
    }

    if (static_cast<size_t>(level) == s.size()) {
        out_text = {};
        return level;
    }

    if (s[level] == ' ' || s[level] == '\t') {
        auto content = trim(s.substr(level));
        // CommonMark: optional closing sequence of '#' must be preceded by space/tab or be entire content
        auto orig = content;
        while (!content.empty() && content.back() == '#') {
            content.remove_suffix(1);
        }
        if (content.empty() || content.back() == ' ' || content.back() == '\t') {
            out_text = trim(content);
        } else {
            out_text = orig;
        }
        return level;
    }

    return 0;
}

AlertType MarkdownParser::parse_alert_marker(std::string_view line, std::string& out_title) {
    auto s = trim(line);
    if (!s.starts_with(">")) return AlertType::None;
    s.remove_prefix(1);
    s = trim(s);

    if (!s.starts_with("[!")) return AlertType::None;

    size_t close_bracket = s.find(']');
    if (close_bracket == std::string_view::npos) return AlertType::None;

    auto tag = s.substr(2, close_bracket - 2);
    auto remainder = trim(s.substr(close_bracket + 1));

    std::string tag_upper;
    tag_upper.reserve(tag.size());
    for (char c : tag) tag_upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    AlertType type = AlertType::None;
    if (tag_upper == "NOTE") {
        type = AlertType::Note;
        out_title = remainder.empty() ? "Note" : std::string(remainder);
    } else if (tag_upper == "TIP") {
        type = AlertType::Tip;
        out_title = remainder.empty() ? "Tip" : std::string(remainder);
    } else if (tag_upper == "IMPORTANT") {
        type = AlertType::Important;
        out_title = remainder.empty() ? "Important" : std::string(remainder);
    } else if (tag_upper == "WARNING") {
        type = AlertType::Warning;
        out_title = remainder.empty() ? "Warning" : std::string(remainder);
    } else if (tag_upper == "CAUTION") {
        type = AlertType::Caution;
        out_title = remainder.empty() ? "Caution" : std::string(remainder);
    }

    return type;
}

bool MarkdownParser::parse_table_row(std::string_view line, std::vector<std::string>& out_cells) {
    auto s = trim(line);
    if (s.empty()) return false;
    if (s.find('|') == std::string_view::npos) return false;

    if (s.front() == '|') s.remove_prefix(1);
    if (!s.empty() && s.back() == '|') {
        // Only strip trailing pipe if not escaped
        size_t bs = 0;
        for (size_t k = s.size() - 1; k > 0 && s[k - 1] == '\\'; --k) bs++;
        if (bs % 2 == 0) s.remove_suffix(1);
    }

    out_cells.clear();
    size_t start = 0;
    bool in_code = false;
    bool is_escaped = false;

    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] == '\\') {
            is_escaped = !is_escaped;
            continue;
        }

        if (i < s.size() && s[i] == '`' && !is_escaped) {
            in_code = !in_code;
        } else if ((i == s.size() || (s[i] == '|' && !is_escaped)) && !in_code) {
            auto cell = trim(s.substr(start, i - start));
            std::string cell_str;
            cell_str.reserve(cell.size());
            for (size_t k = 0; k < cell.size(); ++k) {
                if (cell[k] == '\\' && k + 1 < cell.size() && cell[k + 1] == '|') {
                    cell_str.push_back('|');
                    k++;
                } else {
                    cell_str.push_back(cell[k]);
                }
            }
            out_cells.emplace_back(std::move(cell_str));
            start = i + 1;
        }
        is_escaped = false;
    }

    return !out_cells.empty();
}

bool MarkdownParser::parse_table_separator(std::string_view line, std::vector<TableAlign>& out_alignments) {
    std::vector<std::string> cells;
    if (!parse_table_row(line, cells) || cells.empty()) return false;

    out_alignments.clear();
    for (const auto& cell_str : cells) {
        auto c = trim(cell_str);
        if (c.empty()) return false;

        bool left_colon = (c.front() == ':');
        bool right_colon = (c.back() == ':');

        if (left_colon) c.remove_prefix(1);
        if (right_colon && !c.empty()) c.remove_suffix(1);

        c = trim(c);
        if (c.empty()) return false;

        for (char ch : c) {
            if (ch != '-') return false;
        }

        if (left_colon && right_colon) {
            out_alignments.push_back(TableAlign::Center);
        } else if (left_colon) {
            out_alignments.push_back(TableAlign::Left);
        } else if (right_colon) {
            out_alignments.push_back(TableAlign::Right);
        } else {
            out_alignments.push_back(TableAlign::None);
        }
    }

    return true;
}

} // namespace nisaba::markdown
