#include "nisaba/text/linux_font_config.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace nisaba::text {

namespace {

std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string get_env_str(const char* name) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string{};
}

std::string expand_tilde(std::string_view path) {
    if (path.empty()) return {};
    if (path[0] == '~') {
        std::string home = get_env_str("HOME");
        if (home.empty()) return std::string(path);
        if (path.size() == 1) return home;
        if (path[1] == '/') return home + std::string(path.substr(1));
    }
    return std::string(path);
}

bool wildcard_match(std::string_view pattern, std::string_view text) noexcept {
    size_t p = 0, t = 0;
    size_t star_p = std::string_view::npos;
    size_t match_t = 0;

    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' ||
             std::tolower(static_cast<unsigned char>(pattern[p])) ==
             std::tolower(static_cast<unsigned char>(text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            match_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++match_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

std::string extract_attr(std::string_view tag_header, std::string_view attr_name) {
    size_t pos = 0;
    while (pos < tag_header.size()) {
        size_t found = tag_header.find(attr_name, pos);
        if (found == std::string_view::npos) break;
        size_t after = found + attr_name.size();
        // Ensure word boundary before attr_name
        if (found > 0 && (std::isalnum(static_cast<unsigned char>(tag_header[found - 1])) || tag_header[found - 1] == '_' || tag_header[found - 1] == '-')) {
            pos = after;
            continue;
        }
        while (after < tag_header.size() && std::isspace(static_cast<unsigned char>(tag_header[after]))) {
            ++after;
        }
        if (after < tag_header.size() && tag_header[after] == '=') {
            ++after;
            while (after < tag_header.size() && std::isspace(static_cast<unsigned char>(tag_header[after]))) {
                ++after;
            }
            if (after < tag_header.size() && (tag_header[after] == '"' || tag_header[after] == '\'')) {
                char quote = tag_header[after++];
                size_t end_q = tag_header.find(quote, after);
                if (end_q != std::string_view::npos) {
                    return std::string(tag_header.substr(after, end_q - after));
                }
            }
        }
        pos = after;
    }
    return {};
}

std::string unescape_xml(std::string_view sv) {
    sv = trim_sv(sv);
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '&') {
            if (sv.substr(i).starts_with("&amp;")) { out.push_back('&'); i += 4; continue; }
            if (sv.substr(i).starts_with("&lt;")) { out.push_back('<'); i += 3; continue; }
            if (sv.substr(i).starts_with("&gt;")) { out.push_back('>'); i += 3; continue; }
            if (sv.substr(i).starts_with("&quot;")) { out.push_back('"'); i += 5; continue; }
            if (sv.substr(i).starts_with("&apos;")) { out.push_back('\''); i += 5; continue; }
        }
        out.push_back(sv[i]);
    }
    return out;
}

void append_unique_str(std::vector<std::string>& vec, std::string_view item) {
    if (item.empty()) return;
    std::string key = LinuxFontConfig::normalize_key(item);
    for (const auto& existing : vec) {
        if (LinuxFontConfig::normalize_key(existing) == key) return;
    }
    vec.emplace_back(item);
}

void prepend_unique_list(std::vector<std::string>& dst, const std::vector<std::string>& items) {
    std::vector<std::string> combined;
    combined.reserve(items.size() + dst.size());
    for (const auto& it : items) {
        append_unique_str(combined, it);
    }
    for (const auto& it : dst) {
        append_unique_str(combined, it);
    }
    dst = std::move(combined);
}

} // namespace

std::string LinuxFontConfig::normalize_key(std::string_view name) {
    name = trim_sv(name);
    std::string res;
    res.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '_' || c == ' ' || c == '-') {
            // Normalize "sans serif", "sans-serif", "sans_serif" consistently:
            // Keep single '-' for standard CSS names if needed, or strip spaces/hyphens for loose matching
            if (!res.empty() && res.back() != '-') {
                res.push_back('-');
            }
        } else {
            res.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    // Canonicalize common CSS / Fontconfig generic synonyms
    if (res == "sans") return "sans-serif";
    if (res == "mono") return "monospace";
    return res;
}

std::string LinuxFontConfig::detect_system_locale() {
    for (const char* var : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        std::string val = get_env_str(var);
        if (!val.empty() && val != "C" && val != "POSIX" && !val.starts_with("C.")) {
            // Strip encoding suffix (.UTF-8) and modifier (@...)
            size_t dot = val.find('.');
            if (dot != std::string::npos) val.resize(dot);
            size_t at = val.find('@');
            if (at != std::string::npos) val.resize(at);
            for (char& c : val) {
                if (c == '_') c = '-';
            }
            return val;
        }
    }
    return "en";
}

void LinuxFontConfig::set_locale(std::string_view locale) {
    if (locale.empty()) {
        locale_ = detect_system_locale();
    } else {
        locale_ = std::string(locale);
        for (char& c : locale_) {
            if (c == '_') c = '-';
        }
    }
}

bool LinuxFontConfig::load_system_config(std::string_view locale_override) {
    font_dirs_.clear();
    reject_globs_.clear();
    aliases_.clear();
    lang_overrides_.clear();

    set_locale(locale_override);

    bool any_loaded = false;
    std::string fc_file = get_env_str("FONTCONFIG_FILE");
    if (!fc_file.empty()) {
        any_loaded |= parse_config_file(fc_file, 0);
    } else {
        any_loaded |= parse_config_file("/etc/fonts/fonts.conf", 0);
    }

    // Also check user fontconfig if not already included
    std::string xdg_config = get_env_str("XDG_CONFIG_HOME");
    if (xdg_config.empty()) {
        std::string home = get_env_str("HOME");
        if (!home.empty()) xdg_config = home + "/.config";
    }
    if (!xdg_config.empty()) {
        std::string user_conf = xdg_config + "/fontconfig/fonts.conf";
        if (fs::exists(user_conf)) {
            parse_config_file(user_conf, 0);
        }
    }

    // Ensure standard Linux font directories are present even on minimal/embedded systems without /etc/fonts
    add_directory_resolved("/usr/share/fonts", "", "");
    add_directory_resolved("/usr/local/share/fonts", "", "");
    add_directory_resolved("fonts", "xdg", "");
    add_directory_resolved("~/.local/share/fonts", "", "");
    add_directory_resolved("~/.fonts", "", "");

    // Also check $XDG_DATA_DIRS
    std::string xdg_data_dirs = get_env_str("XDG_DATA_DIRS");
    if (!xdg_data_dirs.empty()) {
        size_t start = 0;
        while (start < xdg_data_dirs.size()) {
            size_t colon = xdg_data_dirs.find(':', start);
            std::string_view dir = (colon == std::string::npos)
                ? std::string_view(xdg_data_dirs).substr(start)
                : std::string_view(xdg_data_dirs).substr(start, colon - start);
            if (!dir.empty()) {
                add_directory_resolved(std::string(dir) + "/fonts", "", "");
            }
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }

    ensure_standard_fallback_defaults();
    return any_loaded || !font_dirs_.empty();
}

void LinuxFontConfig::add_directory_resolved(
    std::string_view raw_dir,
    std::string_view prefix_attr,
    std::string_view base_dir
) {
    raw_dir = trim_sv(raw_dir);
    if (raw_dir.empty()) return;

    std::string resolved;
    if (prefix_attr == "xdg") {
        std::string xdg_data = get_env_str("XDG_DATA_HOME");
        if (xdg_data.empty()) {
            std::string home = get_env_str("HOME");
            if (!home.empty()) xdg_data = home + "/.local/share";
        }
        if (xdg_data.empty()) return;
        resolved = xdg_data + "/" + std::string(raw_dir);
    } else if (raw_dir[0] == '~') {
        resolved = expand_tilde(raw_dir);
    } else if (raw_dir[0] != '/' && !base_dir.empty()) {
        resolved = std::string(base_dir) + "/" + std::string(raw_dir);
    } else {
        resolved = std::string(raw_dir);
    }

    std::error_code ec;
    if (fs::exists(resolved, ec) && fs::is_directory(resolved, ec)) {
        std::string canonical_str = fs::weakly_canonical(resolved, ec).string();
        if (ec || canonical_str.empty()) canonical_str = resolved;
        if (std::find(font_dirs_.begin(), font_dirs_.end(), canonical_str) == font_dirs_.end()) {
            font_dirs_.push_back(std::move(canonical_str));
        }
    }
}

void LinuxFontConfig::merge_alias(
    std::string_view family,
    const std::vector<std::string>& prefer,
    const std::vector<std::string>& accept,
    const std::vector<std::string>& def
) {
    std::string key = normalize_key(family);
    if (key.empty()) return;
    auto& rule = aliases_[key];
    for (const auto& p : prefer) append_unique_str(rule.prefer, p);
    for (const auto& a : accept) append_unique_str(rule.accept, a);
    for (const auto& d : def)    append_unique_str(rule.defaults, d);
}

bool LinuxFontConfig::parse_config_file(std::string_view path, int depth) {
    if (depth > 8 || path.empty()) return false;
    std::error_code ec;
    fs::path fpath(path);
    if (!fs::exists(fpath, ec)) return false;

    if (fs::is_directory(fpath, ec)) {
        std::vector<fs::path> entries;
        for (const auto& entry : fs::directory_iterator(fpath, ec)) {
            if (entry.path().extension() == ".conf") {
                entries.push_back(entry.path());
            }
        }
        std::sort(entries.begin(), entries.end());
        bool ok = false;
        for (const auto& e : entries) {
            ok |= parse_config_file(e.string(), depth + 1);
        }
        return ok;
    }

    std::ifstream file(fpath, std::ios::binary);
    if (!file.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty()) return false;

    return parse_xml_content(content, fpath.parent_path().string(), depth);
}

bool LinuxFontConfig::parse_xml_content(std::string_view xml, std::string_view base_dir, int depth) {
    if (depth > 8 || xml.empty()) return false;

    // Simple state stacks for relevant Fontconfig elements
    bool in_alias = false;
    int alias_submode = 0; // 0 = direct <family>, 1 = <prefer>, 2 = <accept>, 3 = <default>
    std::string alias_family;
    std::vector<std::string> alias_prefer, alias_accept, alias_default;

    bool in_match = false;
    bool in_test = false;
    bool in_edit = false;
    std::string test_name;
    std::string test_compare;
    std::string edit_name;
    std::string edit_mode;
    std::string match_tested_family;
    std::string match_tested_lang;
    bool match_has_negated_test = false;
    std::vector<std::string> match_edit_families;

    bool in_rejectfont = false;

    size_t pos = 0;
    while (pos < xml.size()) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;

        // Skip XML comments <!-- ... -->
        if (xml.substr(lt).starts_with("<!--")) {
            size_t end_comment = xml.find("-->", lt + 4);
            if (end_comment == std::string_view::npos) break;
            pos = end_comment + 3;
            continue;
        }

        // Skip XML declarations / DOCTYPE <?...?> or <!...>
        if (lt + 1 < xml.size() && (xml[lt + 1] == '?' || xml[lt + 1] == '!')) {
            size_t gt = xml.find('>', lt + 2);
            if (gt == std::string_view::npos) break;
            pos = gt + 1;
            continue;
        }

        size_t gt = xml.find('>', lt + 1);
        if (gt == std::string_view::npos) break;

        std::string_view tag_raw = trim_sv(xml.substr(lt + 1, gt - lt - 1));
        if (tag_raw.empty()) {
            pos = gt + 1;
            continue;
        }

        bool is_closing = (tag_raw.front() == '/');
        bool is_self_closing = (!is_closing && tag_raw.back() == '/');
        if (is_closing) tag_raw.remove_prefix(1);
        if (is_self_closing) tag_raw.remove_suffix(1);
        tag_raw = trim_sv(tag_raw);

        size_t sp = 0;
        while (sp < tag_raw.size() && !std::isspace(static_cast<unsigned char>(tag_raw[sp]))) {
            ++sp;
        }
        std::string_view tag_name = tag_raw.substr(0, sp);
        std::string_view tag_attrs = tag_raw.substr(sp);

        // Helper to read text content up to the matching closing tag for leaf nodes (<dir>, <family>, <string>, <glob>, <include>)
        auto read_leaf_text = [&]() -> std::string {
            if (is_self_closing) return {};
            size_t close_lt = xml.find('<', gt + 1);
            if (close_lt == std::string_view::npos) return {};
            std::string txt = unescape_xml(xml.substr(gt + 1, close_lt - (gt + 1)));
            size_t close_gt = xml.find('>', close_lt + 1);
            if (close_gt != std::string_view::npos) {
                gt = close_gt;
            }
            return txt;
        };

        if (!is_closing) {
            if (tag_name == "dir") {
                std::string prefix = extract_attr(tag_attrs, "prefix");
                std::string dir_text = read_leaf_text();
                add_directory_resolved(dir_text, prefix, base_dir);
            } else if (tag_name == "include") {
                std::string prefix = extract_attr(tag_attrs, "prefix");
                std::string inc_path = read_leaf_text();
                if (!inc_path.empty()) {
                    std::string resolved_inc;
                    if (prefix == "xdg") {
                        std::string xdg_cfg = get_env_str("XDG_CONFIG_HOME");
                        if (xdg_cfg.empty()) {
                            std::string home = get_env_str("HOME");
                            if (!home.empty()) xdg_cfg = home + "/.config";
                        }
                        if (!xdg_cfg.empty()) resolved_inc = xdg_cfg + "/" + inc_path;
                    } else if (inc_path[0] == '~') {
                        resolved_inc = expand_tilde(inc_path);
                    } else if (inc_path[0] != '/' && !base_dir.empty()) {
                        resolved_inc = std::string(base_dir) + "/" + inc_path;
                        std::error_code ec;
                        if (!fs::exists(resolved_inc, ec)) {
                            std::string alt_inc = "/etc/fonts/" + inc_path;
                            if (fs::exists(alt_inc, ec)) {
                                resolved_inc = std::move(alt_inc);
                            }
                        }
                    } else {
                        resolved_inc = inc_path;
                    }
                    if (!resolved_inc.empty()) {
                        parse_config_file(resolved_inc, depth + 1);
                    }
                }
            } else if (tag_name == "rejectfont") {
                if (!is_self_closing) in_rejectfont = true;
            } else if (tag_name == "glob" && in_rejectfont) {
                std::string glob_pat = read_leaf_text();
                if (!glob_pat.empty()) {
                    reject_globs_.push_back(std::move(glob_pat));
                }
            } else if (tag_name == "alias") {
                if (!is_self_closing) {
                    in_alias = true;
                    alias_submode = 0;
                    alias_family.clear();
                    alias_prefer.clear();
                    alias_accept.clear();
                    alias_default.clear();
                }
            } else if (in_alias && tag_name == "prefer") {
                alias_submode = 1;
            } else if (in_alias && tag_name == "accept") {
                alias_submode = 2;
            } else if (in_alias && tag_name == "default") {
                alias_submode = 3;
            } else if (in_alias && tag_name == "family") {
                std::string fam = read_leaf_text();
                if (!fam.empty()) {
                    if (alias_submode == 0 && alias_family.empty()) {
                        alias_family = std::move(fam);
                    } else if (alias_submode == 1) {
                        alias_prefer.push_back(std::move(fam));
                    } else if (alias_submode == 2) {
                        alias_accept.push_back(std::move(fam));
                    } else if (alias_submode == 3) {
                        alias_default.push_back(std::move(fam));
                    }
                }
            } else if (tag_name == "match") {
                std::string target = extract_attr(tag_attrs, "target");
                if ((target.empty() || target == "pattern") && !is_self_closing) {
                    in_match = true;
                    in_test = false;
                    in_edit = false;
                    match_tested_family.clear();
                    match_tested_lang.clear();
                    match_has_negated_test = false;
                    match_edit_families.clear();
                }
            } else if (in_match && tag_name == "test") {
                if (!is_self_closing) {
                    in_test = true;
                    test_name = extract_attr(tag_attrs, "name");
                    test_compare = extract_attr(tag_attrs, "compare");
                    if (test_compare == "not_eq" || test_compare == "not_contains" ||
                        test_compare == "more" || test_compare == "more_eq" ||
                        test_compare == "less" || test_compare == "less_eq" ||
                        (!test_name.empty() && test_name != "family" && test_name != "lang")) {
                        match_has_negated_test = true;
                    }
                }
            } else if (in_match && tag_name == "edit") {
                if (!is_self_closing) {
                    in_edit = true;
                    edit_name = extract_attr(tag_attrs, "name");
                    edit_mode = extract_attr(tag_attrs, "mode");
                }
            } else if (in_match && tag_name == "string") {
                std::string s_val = read_leaf_text();
                if (!s_val.empty()) {
                    if (in_test) {
                        if (test_name == "family" && match_tested_family.empty() &&
                            test_compare != "not_eq" && test_compare != "not_contains") {
                            match_tested_family = std::move(s_val);
                        } else if (test_name == "lang" && match_tested_lang.empty() &&
                                   test_compare != "not_eq" && test_compare != "not_contains") {
                            match_tested_lang = std::move(s_val);
                        }
                    } else if (in_edit && edit_name == "family") {
                        match_edit_families.push_back(std::move(s_val));
                    }
                }
            }
        } else {
            // Closing tag
            if (tag_name == "rejectfont") {
                in_rejectfont = false;
            } else if (tag_name == "prefer" || tag_name == "accept" || tag_name == "default") {
                if (in_alias) alias_submode = 0;
            } else if (tag_name == "alias") {
                if (in_alias && !alias_family.empty()) {
                    merge_alias(alias_family, alias_prefer, alias_accept, alias_default);
                }
                in_alias = false;
            } else if (tag_name == "test") {
                in_test = false;
            } else if (tag_name == "edit") {
                in_edit = false;
            } else if (tag_name == "match") {
                if (in_match && !match_has_negated_test && !match_tested_family.empty() && !match_edit_families.empty()) {
                    if (!match_tested_lang.empty()) {
                        LangOverrideRule lor;
                        lor.lang_pattern = match_tested_lang;
                        lor.target_family_key = normalize_key(match_tested_family);
                        lor.prepend_families = match_edit_families;
                        lang_overrides_.push_back(std::move(lor));
                    } else {
                        std::string key = normalize_key(match_tested_family);
                        auto& rule = aliases_[key];
                        if (edit_mode == "prepend" || edit_mode == "assign" || edit_mode == "assign_replace" || edit_mode.empty()) {
                            prepend_unique_list(rule.prefer, match_edit_families);
                        } else {
                            for (const auto& f : match_edit_families) {
                                append_unique_str(rule.accept, f);
                            }
                        }
                    }
                }
                in_match = false;
            }
        }

        pos = gt + 1;
    }

    return true;
}

void LinuxFontConfig::ensure_standard_fallback_defaults() {
    // Ensure standard generic families always have well-known Linux fallback chains
    merge_alias("system-ui", {"sans-serif", "Inter", "Cantarell", "Noto Sans", "Ubuntu", "DejaVu Sans"}, {}, {});

    merge_alias("sans-serif", {
        "Noto Sans",
        "DejaVu Sans",
        "Liberation Sans",
        "Inter",
        "Ubuntu",
        "Arimo",
        "Cantarell",
        "FreeSans",
        "Nimbus Sans",
        "Droid Sans",
        "Noto Sans Arabic",
        "Noto Sans CJK SC",
        "Noto Sans CJK JP",
        "Noto Color Emoji",
        "Twitter Color Emoji"
    }, {}, {});

    merge_alias("serif", {
        "Noto Serif",
        "DejaVu Serif",
        "Liberation Serif",
        "Tinos",
        "FreeSerif",
        "Nimbus Roman",
        "Noto Naskh Arabic",
        "Noto Serif CJK SC"
    }, {}, {});

    merge_alias("monospace", {
        "Cascadia Code",
        "JetBrains Mono",
        "DejaVu Sans Mono",
        "Noto Sans Mono",
        "Liberation Mono",
        "Ubuntu Mono",
        "Fira Code",
        "Hack",
        "Cousine",
        "FreeMono",
        "Nimbus Mono PS",
        "Symbols Nerd Font",
        "Symbols Nerd Font Mono"
    }, {}, {});

    merge_alias("emoji", {
        "Noto Color Emoji",
        "Twitter Color Emoji",
        "Twemoji",
        "EmojiOne Mozilla",
        "JoyPixels",
        "Noto Emoji"
    }, {}, {});

    merge_alias("math", {
        "STIX Two Math",
        "XITS Math",
        "Latin Modern Math",
        "DejaVu Math TeX Gyre",
        "Noto Sans Math"
    }, {}, {});

    // Standard metric-compatible web/desktop aliases
    merge_alias("Arial", {"Liberation Sans", "Arimo", "Noto Sans", "DejaVu Sans"}, {}, {"sans-serif"});
    merge_alias("Helvetica", {"Nimbus Sans", "Liberation Sans", "Arimo", "Noto Sans"}, {}, {"sans-serif"});
    merge_alias("Times New Roman", {"Liberation Serif", "Tinos", "Noto Serif", "DejaVu Serif"}, {}, {"serif"});
    merge_alias("Times", {"Nimbus Roman", "Liberation Serif", "Tinos", "Noto Serif"}, {}, {"serif"});
    merge_alias("Courier New", {"Liberation Mono", "Cousine", "DejaVu Sans Mono", "Noto Sans Mono"}, {}, {"monospace"});
    merge_alias("Courier", {"Nimbus Mono PS", "Liberation Mono", "DejaVu Sans Mono"}, {}, {"monospace"});
}

bool LinuxFontConfig::is_rejected_file(std::string_view file_path) const noexcept {
    if (reject_globs_.empty()) return false;
    std::string_view filename = file_path;
    size_t slash = file_path.find_last_of('/');
    if (slash != std::string_view::npos) {
        filename = file_path.substr(slash + 1);
    }
    for (const auto& glob : reject_globs_) {
        if (wildcard_match(glob, file_path) || wildcard_match(glob, filename)) {
            return true;
        }
    }
    return false;
}

std::string LinuxFontConfig::get_default_generic_for_family(std::string_view family_name) const {
    std::string key = normalize_key(family_name);
    auto it = aliases_.find(key);
    if (it != aliases_.end() && !it->second.defaults.empty()) {
        return normalize_key(it->second.defaults.front());
    }
    return {};
}

std::vector<std::string> LinuxFontConfig::resolve_family_candidates(const Family& requested) const {
    switch (requested.kind) {
        case Family::Kind::Name:
            return resolve_family_name(requested.name);
        case Family::Kind::SansSerif:
            return resolve_family_name("sans-serif");
        case Family::Kind::Serif:
            return resolve_family_name("serif");
        case Family::Kind::Monospace:
            return resolve_family_name("monospace");
        case Family::Kind::SystemUI:
            return resolve_family_name("system-ui");
        case Family::Kind::Emoji:
            return resolve_family_name("emoji");
        case Family::Kind::Math:
            return resolve_family_name("math");
        case Family::Kind::Cursive:
            return resolve_family_name("cursive");
        case Family::Kind::Fantasy:
            return resolve_family_name("fantasy");
    }
    return resolve_family_name("sans-serif");
}

std::vector<std::string> LinuxFontConfig::resolve_family_name(std::string_view family_name) const {
    std::vector<std::string> result;
    std::unordered_set<std::string> expanded_keys;

    auto is_generic_key = [](std::string_view key) noexcept -> bool {
        return key == "sans-serif" || key == "serif" || key == "monospace" ||
               key == "system-ui" || key == "emoji" || key == "math" ||
               key == "cursive" || key == "fantasy";
    };

    auto append_concrete = [&](std::string_view fam) {
        std::string k = normalize_key(fam);
        if (!k.empty() && !is_generic_key(k)) {
            append_unique_str(result, fam);
        }
    };

    // Breadth-first expansion of prefer & accept chains (without jumping into child <default> generics)
    auto expand_prefer_accept = [&](auto& self, std::string_view fam, int depth) -> void {
        if (depth > 4 || fam.empty()) return;
        std::string key = normalize_key(fam);
        if (key.empty()) return;

        append_concrete(fam);

        if (expanded_keys.contains(key)) return;
        expanded_keys.insert(key);

        // 1. Check language-specific overrides first (e.g. lang="ja" or lang="ar" for sans-serif)
        if (!locale_.empty()) {
            std::string loc_lower = normalize_key(locale_);
            for (const auto& lor : lang_overrides_) {
                if (lor.target_family_key == key) {
                    std::string pat_lower = normalize_key(lor.lang_pattern);
                    if (!pat_lower.empty() &&
                        (loc_lower == pat_lower || loc_lower.starts_with(pat_lower + "-") || pat_lower.starts_with(loc_lower + "-"))) {
                        for (const auto& pf : lor.prepend_families) {
                            append_concrete(pf);
                        }
                    }
                }
            }
        }

        auto it = aliases_.find(key);
        if (it == aliases_.end()) return;

        // First add all direct concrete families in prefer and accept in exact priority order
        for (const auto& p : it->second.prefer) {
            append_concrete(p);
        }
        for (const auto& a : it->second.accept) {
            append_concrete(a);
        }

        // Then recursively expand any aliases inside prefer and accept
        for (const auto& p : it->second.prefer) {
            self(self, p, depth + 1);
        }
        for (const auto& a : it->second.accept) {
            self(self, a, depth + 1);
        }
    };

    expand_prefer_accept(expand_prefer_accept, family_name, 0);

    // Finally, if the root requested family itself has a <default> generic fallback (e.g. Arial -> sans-serif),
    // append that generic's candidates at the end of the chain.
    std::string root_key = normalize_key(family_name);
    auto root_it = aliases_.find(root_key);
    if (root_it != aliases_.end()) {
        for (const auto& d : root_it->second.defaults) {
            expand_prefer_accept(expand_prefer_accept, d, 1);
        }
    }

    return result;
}

} // namespace nisaba::text
