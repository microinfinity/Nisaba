#include "nisaba/text/bidi.hpp"
#include <algorithm>

namespace nisaba::text {

namespace {

struct ArabicLetterForms {
    char32_t isolated{0};
    char32_t final_form{0};
    char32_t initial{0};
    char32_t medial{0};
    bool right_joining{false}; // Only connects to previous (Alif, Dal, Ra, Waw, etc.)
    bool dual_joining{false};  // Connects to both previous and next (Ba, Jim, Sin, etc.)
};

ArabicLetterForms get_arabic_forms(char32_t c) noexcept {
    switch (c) {
        // Standard Arabic Letters (U+0621..U+064A)
        case 0x0621: return {0xFE80, 0, 0, 0, false, false}; // ء (Non-joining)
        case 0x0622: return {0xFE81, 0xFE82, 0, 0, true, false}; // آ
        case 0x0623: return {0xFE83, 0xFE84, 0, 0, true, false}; // أ
        case 0x0624: return {0xFE85, 0xFE86, 0, 0, true, false}; // ؤ
        case 0x0625: return {0xFE87, 0xFE88, 0, 0, true, false}; // إ
        case 0x0626: return {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, false, true}; // ئ
        case 0x0627: return {0xFE8D, 0xFE8E, 0, 0, true, false}; // ا
        case 0x0628: return {0xFE8F, 0xFE90, 0xFE91, 0xFE92, false, true}; // ب
        case 0x0629: return {0xFE93, 0xFE94, 0, 0, true, false}; // ة
        case 0x062A: return {0xFE95, 0xFE96, 0xFE97, 0xFE98, false, true}; // ت
        case 0x062B: return {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, false, true}; // ث
        case 0x062C: return {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, false, true}; // ج
        case 0x062D: return {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, false, true}; // ح
        case 0x062E: return {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, false, true}; // خ
        case 0x062F: return {0xFEA9, 0xFEAA, 0, 0, true, false}; // د
        case 0x0630: return {0xFEAB, 0xFEAC, 0, 0, true, false}; // ذ
        case 0x0631: return {0xFEAD, 0xFEAE, 0, 0, true, false}; // ر
        case 0x0632: return {0xFEAF, 0xFEB0, 0, 0, true, false}; // ز
        case 0x0633: return {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, false, true}; // س
        case 0x0634: return {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, false, true}; // ش
        case 0x0635: return {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, false, true}; // ص
        case 0x0636: return {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, false, true}; // ض
        case 0x0637: return {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, false, true}; // ط
        case 0x0638: return {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, false, true}; // ظ
        case 0x0639: return {0xFEC9, 0xFECA, 0xFECB, 0xFECC, false, true}; // ع
        case 0x063A: return {0xFECD, 0xFECE, 0xFECF, 0xFED0, false, true}; // غ
        case 0x0640: return {0x0640, 0x0640, 0x0640, 0x0640, false, true}; // ـ Tatweel / Kashida (dual-joining)
        case 0x0641: return {0xFED1, 0xFED2, 0xFED3, 0xFED4, false, true}; // ف
        case 0x0642: return {0xFED5, 0xFED6, 0xFED7, 0xFED8, false, true}; // ق
        case 0x0643: return {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, false, true}; // ك
        case 0x0644: return {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, false, true}; // ل
        case 0x0645: return {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, false, true}; // م
        case 0x0646: return {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, false, true}; // ن
        case 0x0647: return {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, false, true}; // ه
        case 0x0648: return {0xFEED, 0xFEEE, 0, 0, true, false}; // و
        case 0x0649: return {0xFEEF, 0xFEF0, 0, 0, true, false}; // ى
        case 0x064A: return {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, false, true}; // ي

        // Extended Arabic / Persian / Urdu / Kurdish Letters (Presentation Forms-A)
        case 0x0679: return {0xFB66, 0xFB67, 0xFB68, 0xFB69, false, true}; // ٹ (Tteh)
        case 0x067E: return {0xFB56, 0xFB57, 0xFB58, 0xFB59, false, true}; // پ (Peh)
        case 0x0686: return {0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, false, true}; // چ (Tcheh)
        case 0x0688: return {0xFB88, 0xFB89, 0, 0, true, false};           // ڈ (Ddal)
        case 0x0691: return {0xFB8C, 0xFB8D, 0, 0, true, false};           // ڑ (Rreh)
        case 0x0698: return {0xFB8A, 0xFB8B, 0, 0, true, false};           // ژ (Jeh)
        case 0x06A9: return {0xFB8E, 0xFB8F, 0xFB90, 0xFB91, false, true}; // ک (Keheh)
        case 0x06AF: return {0xFB92, 0xFB93, 0xFB94, 0xFB95, false, true}; // گ (Gaf)
        case 0x06BA: return {0xFB9E, 0xFB9F, 0, 0, true, false};           // ں (Noon Ghunna)
        case 0x06BE: return {0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD, false, true}; // ھ (Heh Doachashmee)
        case 0x06C0: return {0xFBA4, 0xFBA5, 0, 0, true, false};           // ۀ (Heh with Yeh above)
        case 0x06C1: return {0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9, false, true}; // ہ (Heh Goal)
        case 0x06CC: return {0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF, false, true}; // ی (Farsi Yeh)
        case 0x06D2: return {0xFBAE, 0xFBAF, 0, 0, true, false};           // ے (Yeh Barree)
        case 0x06D5: return {0xFBE9, 0xFBEA, 0, 0, true, false};           // ە (Ae)
        default: break;
    }
    return {c, 0, 0, 0, false, false};
}

char32_t get_lam_alif_ligature(char32_t alif, bool is_final) noexcept {
    switch (alif) {
        case 0x0622: return is_final ? 0xFEF6 : 0xFEF5; // لآ
        case 0x0623: return is_final ? 0xFEF8 : 0xFEF7; // لأ
        case 0x0625: return is_final ? 0xFEFA : 0xFEF9; // لإ
        case 0x0627: return is_final ? 0xFEFC : 0xFEFB; // لا
        default: return 0;
    }
}

char32_t decode_single_utf8(std::string_view text, size_t& i) noexcept {
    const size_t len = text.size();
    if (i >= len) return 0;
    uint8_t b0 = static_cast<uint8_t>(text[i++]);
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && i < len) {
        uint8_t b1 = static_cast<uint8_t>(text[i++]);
        return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0 && i + 1 < len) {
        uint8_t b1 = static_cast<uint8_t>(text[i++]);
        uint8_t b2 = static_cast<uint8_t>(text[i++]);
        return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    }
    if ((b0 & 0xF8) == 0xF0 && i + 2 < len) {
        uint8_t b1 = static_cast<uint8_t>(text[i++]);
        uint8_t b2 = static_cast<uint8_t>(text[i++]);
        uint8_t b3 = static_cast<uint8_t>(text[i++]);
        return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
    }
    return b0;
}

bool is_word_char(char32_t cp) noexcept {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return false;
    if ((cp >= '!' && cp <= '/') || (cp >= ':' && cp <= '@') ||
        (cp >= '[' && cp <= '^') || cp == '`' || (cp >= '{' && cp <= '~') ||
        cp == 0x060C || cp == 0x061B || cp == 0x061F || cp == 0x3001 || cp == 0x3002) {
        return false;
    }
    return true;
}

} // namespace

std::vector<std::pair<char32_t, size_t>> Bidi::decode_utf8(std::string_view text) {
    std::vector<std::pair<char32_t, size_t>> result;
    result.reserve(text.size());
    size_t i = 0;
    const size_t len = text.size();

    while (i < len) {
        size_t byte_pos = i;
        char32_t cp = decode_single_utf8(text, i);
        result.emplace_back(cp, byte_pos);
    }
    return result;
}

CharType Bidi::classify_char(char32_t c) noexcept {
    // Explicit directional marks
    if (c == 0x200E || c == 0x202A || c == 0x202D || c == 0x2066) {
        return CharType::LTR; // LRM, LRE, LRO, LRI
    }
    if (c == 0x200F || c == 0x202B || c == 0x202E || c == 0x2067) {
        return CharType::RTL; // RLM, RLE, RLO, RLI
    }
    if (c == 0x061C) {
        return CharType::ArabicLetter; // ALM (Arabic Letter Mark)
    }

    if (is_arabic(c)) {
        return CharType::ArabicLetter;
    }
    if ((c >= 0x0590 && c <= 0x05FF) || (c >= 0x0700 && c <= 0x074F) ||
        (c >= 0x0780 && c <= 0x07FF) || (c >= 0xFB1D && c <= 0xFB4F)) {
        return CharType::RTL; // Hebrew, Syriac, Thaana, NKo
    }
    if (c >= '0' && c <= '9') {
        return CharType::EuropeanNumber;
    }
    if ((c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9)) {
        return CharType::ArabicNumber;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0) {
        return CharType::Whitespace;
    }
    if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
        (c >= '[' && c <= '`') || (c >= '{' && c <= '~') ||
        c == 0x060C || c == 0x061B || c == 0x061F ||
        c == 0x00AB || c == 0x00BB || c == 0x2039 || c == 0x203A ||
        (c >= 0x2010 && c <= 0x2027) ||
        c == 0xFFFC) {
        return CharType::Neutral;
    }
    return CharType::LTR;
}

bool Bidi::is_arabic(char32_t c) noexcept {
    return (c >= 0x0600 && c <= 0x06FF) ||
           (c >= 0x0750 && c <= 0x077F) ||
           (c >= 0x08A0 && c <= 0x08FF) ||
           (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

bool Bidi::is_arabic_diacritic(char32_t c) noexcept {
    return (c >= 0x0610 && c <= 0x061A) ||
           (c >= 0x064B && c <= 0x065F) ||
           (c == 0x0670) ||
           (c >= 0x06D6 && c <= 0x06ED) ||
           (c >= 0x08D3 && c <= 0x08FF);
}

bool Bidi::is_combining_mark(char32_t c) noexcept {
    if (is_arabic_diacritic(c)) return true;
    return (c >= 0x0300 && c <= 0x036F) || // Combining Diacritical Marks
           (c >= 0x0483 && c <= 0x0489) || // Cyrillic Combining
           (c >= 0x0591 && c <= 0x05BD) || // Hebrew Points
           (c == 0x05BF || c == 0x05C1 || c == 0x05C2 || c == 0x05C4 || c == 0x05C5 || c == 0x05C7) ||
           (c >= 0x1AB0 && c <= 0x1AFF) ||
           (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0xFE00 && c <= 0xFE0F) || // Variation Selectors
           (c >= 0xFE20 && c <= 0xFE2F) ||
           (c >= 0xE0100 && c <= 0xE01EF); // Variation Selectors Supplement
}

char32_t Bidi::mirror_char(char32_t c) noexcept {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        case 0x00AB: return 0x00BB; // « -> »
        case 0x00BB: return 0x00AB; // » -> «
        case 0x2039: return 0x203A; // ‹ -> ›
        case 0x203A: return 0x2039; // › -> ‹
        case 0x2045: return 0x2046; // ⁅ -> ⁆
        case 0x2046: return 0x2045; // ⁆ -> ⁅
        case 0x2208: return 0x220B; // ∈ -> ∋
        case 0x220B: return 0x2208; // ∋ -> ∈
        case 0x2264: return 0x2265; // ≤ -> ≥
        case 0x2265: return 0x2264; // ≥ -> ≤
        case 0x3008: return 0x3009; // 〈 -> 〉
        case 0x3009: return 0x3008;
        case 0x300A: return 0x300B; // 《 -> 》
        case 0x300B: return 0x300A;
        case 0x300C: return 0x300D; // 「 -> 」
        case 0x300D: return 0x300C;
        case 0x300E: return 0x300F; // 『 -> 』
        case 0x300F: return 0x300E;
        case 0x3010: return 0x3011; // 【 -> 】
        case 0x3011: return 0x3010;
        case 0xFD3E: return 0xFD3F; // Ornate Arabic parentheses ﴾ -> ﴿
        case 0xFD3F: return 0xFD3E;
        default: return c;
    }
}

bool Bidi::is_cjk(char32_t c) noexcept {
    return (c >= 0x4E00 && c <= 0x9FFF) ||   // CJK Unified Ideographs
           (c >= 0x3400 && c <= 0x4DBF) ||   // CJK Extension A
           (c >= 0x20000 && c <= 0x2A6DF) || // CJK Extension B
           (c >= 0x3040 && c <= 0x309F) ||   // Hiragana
           (c >= 0x30A0 && c <= 0x30FF) ||   // Katakana
           (c >= 0x31F0 && c <= 0x31FF) ||   // Katakana Extensions
           (c >= 0xAC00 && c <= 0xD7AF) ||   // Hangul Syllables
           (c >= 0x1100 && c <= 0x11FF) ||   // Hangul Jamo
           (c >= 0x3000 && c <= 0x303F) ||   // CJK Symbols and Punctuation
           (c >= 0xFF00 && c <= 0xFFEF);     // Fullwidth Forms
}

bool Bidi::is_cjk_no_break_before(char32_t c) noexcept {
    switch (c) {
        case 0x3001: // 、
        case 0x3002: // 。
        case 0x300D: // 」
        case 0x300F: // 』
        case 0x30FB: // ・
        case 0xFF09: // ）
        case 0xFF0C: // ，
        case 0xFF0E: // ．
        case 0xFF01: // ！
        case 0xFF1F: // ？
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case ')': case ']': case '}': case '>':
        case '!': case '?': case ':': case ';':
        case '.': case ',':
            return true;
        default:
            return false;
    }
}

bool Bidi::is_cjk_no_break_after(char32_t c) noexcept {
    switch (c) {
        case 0x300C: // 「
        case 0x300E: // 『
        case 0xFF08: // （
        case '(': case '[': case '{': case '<':
            return true;
        default:
            return false;
    }
}

Direction Bidi::detect_base_direction(std::string_view text) noexcept {
    auto chars = decode_utf8(text);
    for (const auto& [cp, _] : chars) {
        if (is_combining_mark(cp)) continue;
        CharType type = classify_char(cp);
        if (type == CharType::ArabicLetter || type == CharType::RTL) {
            return Direction::RightToLeft;
        }
        if (type == CharType::LTR) {
            return Direction::LeftToRight;
        }
    }
    return Direction::LeftToRight;
}

std::vector<BidiRun> Bidi::segment_runs(std::string_view text, std::optional<Direction> base_dir) {
    std::vector<BidiRun> runs;
    if (text.empty()) return runs;

    Direction base = base_dir.value_or(detect_base_direction(text));
    uint8_t base_level = (base == Direction::RightToLeft) ? 1 : 0;

    auto chars = decode_utf8(text);
    if (chars.empty()) return runs;

    size_t run_start_byte = 0;
    uint8_t current_level = base_level;
    bool in_run = false;

    for (size_t idx = 0; idx < chars.size(); ++idx) {
        char32_t cp = chars[idx].first;
        size_t byte_pos = chars[idx].second;
        CharType type = is_combining_mark(cp) ? CharType::Neutral : classify_char(cp);

        uint8_t char_level = current_level;
        if (type == CharType::ArabicLetter || type == CharType::RTL) {
            char_level = 1;
        } else if (type == CharType::LTR) {
            char_level = (base_level == 1) ? 2 : 0;
        } else if (type == CharType::EuropeanNumber || type == CharType::ArabicNumber) {
            // Numbers inside RTL context remain level 2 (LTR number run embedded inside RTL)
            char_level = ((current_level % 2) != 0 || base_level == 1) ? 2 : 0;
        } else {
            // Neutral/whitespace: look ahead to next strong type (UAX #9 N1/N2)
            char_level = current_level;
            for (size_t next_idx = idx + 1; next_idx < chars.size(); ++next_idx) {
                char32_t ncp = chars[next_idx].first;
                if (is_combining_mark(ncp)) continue;
                CharType ntype = classify_char(ncp);
                if (ntype == CharType::ArabicLetter || ntype == CharType::RTL) {
                    if ((current_level % 2) != 0) char_level = 1;
                    break;
                } else if (ntype == CharType::LTR) {
                    if ((current_level % 2) == 0) char_level = (base_level == 1) ? 2 : 0;
                    break;
                } else if (ntype == CharType::EuropeanNumber || ntype == CharType::ArabicNumber) {
                    break;
                }
            }
        }

        if (!in_run) {
            run_start_byte = byte_pos;
            current_level = char_level;
            in_run = true;
        } else if (char_level != current_level && type != CharType::Neutral && type != CharType::Whitespace) {
            runs.push_back(BidiRun{run_start_byte, byte_pos, current_level});
            run_start_byte = byte_pos;
            current_level = char_level;
        }
    }

    if (in_run && run_start_byte < text.size()) {
        runs.push_back(BidiRun{run_start_byte, text.size(), current_level});
    }

    return runs;
}

std::vector<ShapedChar> Bidi::shape_text(std::string_view text, const std::vector<BidiRun>& runs) {
    std::vector<ShapedChar> result;
    auto chars = decode_utf8(text);
    if (chars.empty()) return result;

    // UAX #9 Rule L2 visual run reordering across embedding levels
    std::vector<BidiRun> visual_runs = runs;
    uint8_t max_level = 0;
    for (const auto& r : visual_runs) {
        max_level = std::max(max_level, r.level);
    }
    if (max_level > 0 && visual_runs.size() > 1) {
        for (uint8_t lvl = max_level; lvl >= 1; --lvl) {
            size_t i = 0;
            while (i < visual_runs.size()) {
                if (visual_runs[i].level >= lvl) {
                    size_t j = i + 1;
                    while (j < visual_runs.size() && visual_runs[j].level >= lvl) {
                        ++j;
                    }
                    if (j - i > 1) {
                        std::reverse(visual_runs.begin() + static_cast<ptrdiff_t>(i),
                                     visual_runs.begin() + static_cast<ptrdiff_t>(j));
                    }
                    i = j;
                } else {
                    ++i;
                }
            }
        }
    }

    for (const auto& run : visual_runs) {
        std::vector<std::pair<char32_t, size_t>> run_chars;
        for (const auto& c : chars) {
            if (c.second >= run.start_byte && c.second < run.end_byte) {
                run_chars.push_back(c);
            }
        }
        if (run_chars.empty()) continue;

        if (run.is_rtl()) {
            // Group into base clusters (each base character followed by its combining marks)
            // so that when the RTL run is reversed into visual order, marks remain attached
            // immediately after their base character!
            struct Cluster {
                std::vector<ShapedChar> items;
            };
            std::vector<Cluster> clusters;
            clusters.reserve(run_chars.size());

            for (size_t i = 0; i < run_chars.size(); ++i) {
                char32_t cur = run_chars[i].first;
                size_t byte_idx = run_chars[i].second;

                if (is_combining_mark(cur)) {
                    ShapedChar sc{cur, byte_idx, run.level, true, true, ArabicForm::None, cur};
                    if (!clusters.empty()) {
                        clusters.back().items.push_back(sc);
                    } else {
                        clusters.push_back(Cluster{{sc}});
                    }
                    continue;
                }

                if (!is_arabic(cur)) {
                    // Apply Unicode Bidi Bracket & Symbol Mirroring in RTL runs
                    char32_t mirrored = mirror_char(cur);
                    ShapedChar sc{mirrored, byte_idx, run.level, true, false, ArabicForm::None, cur};
                    clusters.push_back(Cluster{{sc}});
                    continue;
                }

                // Check for Lam-Alif ligature (even if diacritics sit on the Lam!)
                if (cur == 0x0644) { // Lam
                    size_t next_base_idx = i + 1;
                    while (next_base_idx < run_chars.size() && is_arabic_diacritic(run_chars[next_base_idx].first)) {
                        ++next_base_idx;
                    }
                    if (next_base_idx < run_chars.size()) {
                        char32_t next_ch = run_chars[next_base_idx].first;
                        bool prev_connects = false;
                        for (int p = static_cast<int>(i) - 1; p >= 0; --p) {
                            if (is_arabic_diacritic(run_chars[p].first)) continue;
                            auto prev_forms = get_arabic_forms(run_chars[p].first);
                            prev_connects = prev_forms.dual_joining;
                            break;
                        }
                        char32_t lig = get_lam_alif_ligature(next_ch, prev_connects);
                        if (lig != 0) {
                            ArabicForm form = prev_connects ? ArabicForm::Final : ArabicForm::Isolated;
                            Cluster cl;
                            cl.items.push_back(ShapedChar{lig, byte_idx, run.level, true, false, form, lig});
                            // Preserve any diacritics between Lam and Alif
                            for (size_t m = i + 1; m < next_base_idx; ++m) {
                                cl.items.push_back(ShapedChar{
                                    run_chars[m].first, run_chars[m].second, run.level, true, true, ArabicForm::None, run_chars[m].first
                                });
                            }
                            clusters.push_back(std::move(cl));
                            i = next_base_idx; // consume Alif (subsequent diacritics on Alif will attach to this cluster)
                            continue;
                        }
                    }
                }

                auto cur_forms = get_arabic_forms(cur);

                // Look backwards for connecting letter (skipping diacritics)
                bool prev_connects = false;
                for (int p = static_cast<int>(i) - 1; p >= 0; --p) {
                    if (is_arabic_diacritic(run_chars[p].first)) continue;
                    auto p_forms = get_arabic_forms(run_chars[p].first);
                    if (p_forms.dual_joining) {
                        prev_connects = true;
                    }
                    break;
                }

                // Look forwards for connecting letter (skipping diacritics)
                bool next_connects = false;
                for (size_t n = i + 1; n < run_chars.size(); ++n) {
                    if (is_arabic_diacritic(run_chars[n].first)) continue;
                    auto n_forms = get_arabic_forms(run_chars[n].first);
                    if (n_forms.dual_joining || n_forms.right_joining) {
                        next_connects = true;
                    }
                    break;
                }

                char32_t shaped_cp = cur;
                ArabicForm form = ArabicForm::Isolated;

                if (cur_forms.dual_joining) {
                    if (prev_connects && next_connects) {
                        shaped_cp = cur_forms.medial ? cur_forms.medial : cur;
                        form = ArabicForm::Medial;
                    } else if (prev_connects) {
                        shaped_cp = cur_forms.final_form ? cur_forms.final_form : cur;
                        form = ArabicForm::Final;
                    } else if (next_connects) {
                        shaped_cp = cur_forms.initial ? cur_forms.initial : cur;
                        form = ArabicForm::Initial;
                    } else {
                        shaped_cp = cur_forms.isolated ? cur_forms.isolated : cur;
                        form = ArabicForm::Isolated;
                    }
                } else if (cur_forms.right_joining) {
                    if (prev_connects) {
                        shaped_cp = cur_forms.final_form ? cur_forms.final_form : cur;
                        form = ArabicForm::Final;
                    } else {
                        shaped_cp = cur_forms.isolated ? cur_forms.isolated : cur;
                        form = ArabicForm::Isolated;
                    }
                } else {
                    shaped_cp = cur_forms.isolated ? cur_forms.isolated : cur;
                    form = ArabicForm::Isolated;
                }

                clusters.push_back(Cluster{{ShapedChar{shaped_cp, byte_idx, run.level, true, false, form, cur}}});
            }

            // Reverse clusters for visual left-to-right rendering order, keeping marks after their base glyph
            std::reverse(clusters.begin(), clusters.end());

            for (auto& cl : clusters) {
                for (auto& sc : cl.items) {
                    result.push_back(sc);
                }
            }
        } else {
            // LTR run - preserve original visual order, marking combining marks
            for (const auto& [cp, byte_idx] : run_chars) {
                bool mark = is_combining_mark(cp);
                result.push_back(ShapedChar{cp, byte_idx, run.level, false, mark, ArabicForm::None, cp});
            }
        }
    }

    return result;
}

size_t Bidi::next_grapheme_cluster(std::string_view text, size_t byte_index) noexcept {
    if (byte_index >= text.size()) return text.size();

    size_t pos = byte_index;
    char32_t prev_cp = decode_single_utf8(text, pos);

    // Handle CRLF as a single grapheme cluster (GB3)
    if (prev_cp == '\r' && pos < text.size() && text[pos] == '\n') {
        return pos + 1;
    }

    while (pos < text.size()) {
        size_t next_pos = pos;
        char32_t cp = decode_single_utf8(text, next_pos);

        // Extend with combining marks, variation selectors, or Zero-Width Joiner (ZWJ) sequences
        if (is_combining_mark(cp) || cp == 0x200D || prev_cp == 0x200D) {
            prev_cp = cp;
            pos = next_pos;
            continue;
        }
        // Regional Indicator pairs (flags)
        if ((prev_cp >= 0x1F1E6 && prev_cp <= 0x1F1FF) && (cp >= 0x1F1E6 && cp <= 0x1F1FF)) {
            pos = next_pos;
            break;
        }
        break;
    }

    return pos;
}

size_t Bidi::prev_grapheme_cluster(std::string_view text, size_t byte_index) noexcept {
    if (byte_index == 0 || text.empty()) return 0;
    size_t target = std::min(byte_index, text.size());

    size_t cur = 0;
    size_t prev = 0;
    while (cur < target) {
        prev = cur;
        cur = next_grapheme_cluster(text, cur);
    }
    return prev;
}

size_t Bidi::next_word_boundary(std::string_view text, size_t byte_index) noexcept {
    if (byte_index >= text.size()) return text.size();
    auto chars = decode_utf8(text);
    if (chars.empty()) return text.size();

    size_t idx = 0;
    while (idx < chars.size() && chars[idx].second < byte_index) {
        ++idx;
    }
    while (idx < chars.size() && (chars[idx].first == ' ' || chars[idx].first == '\t')) {
        ++idx;
    }
    if (idx >= chars.size()) return text.size();

    bool start_is_word = is_word_char(chars[idx].first);
    while (idx < chars.size() && is_word_char(chars[idx].first) == start_is_word &&
           chars[idx].first != ' ' && chars[idx].first != '\t') {
        ++idx;
    }
    return (idx < chars.size()) ? chars[idx].second : text.size();
}

size_t Bidi::prev_word_boundary(std::string_view text, size_t byte_index) noexcept {
    if (byte_index == 0 || text.empty()) return 0;
    auto chars = decode_utf8(text);
    if (chars.empty()) return 0;

    size_t idx = chars.size();
    while (idx > 0 && chars[idx - 1].second >= byte_index) {
        --idx;
    }
    if (idx == 0) return 0;

    while (idx > 0 && (chars[idx - 1].first == ' ' || chars[idx - 1].first == '\t')) {
        --idx;
    }
    if (idx == 0) return 0;

    bool target_is_word = is_word_char(chars[idx - 1].first);
    while (idx > 0 && is_word_char(chars[idx - 1].first) == target_is_word) {
        --idx;
    }
    return chars[idx].second;
}

} // namespace nisaba::text
