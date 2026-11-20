#include "nisaba/text/buffer.hpp"
#include "nisaba/text/bidi.hpp"
#include "nisaba/path/path_builder.hpp"
#include <algorithm>
#include <cmath>

namespace nisaba::text {

namespace {

constexpr uint32_t make_ot_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8)  |
            static_cast<uint32_t>(static_cast<uint8_t>(d));
}

uint32_t detect_ot_script_tag(char32_t cp) noexcept {
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
        (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
        (cp >= 0xFE70 && cp <= 0xFEFF)) {
        return make_ot_tag('a', 'r', 'a', 'b');
    }
    if (cp >= 0x0590 && cp <= 0x05FF) return make_ot_tag('h', 'e', 'b', 'r');
    if (cp >= 0x0400 && cp <= 0x052F) return make_ot_tag('c', 'y', 'r', 'l');
    if (cp >= 0x0370 && cp <= 0x03FF) return make_ot_tag('g', 'r', 'e', 'k');
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x20000 && cp <= 0x2A6DF)) {
        return make_ot_tag('h', 'a', 'n', 'i');
    }
    if ((cp >= 0x3040 && cp <= 0x30FF)) return make_ot_tag('k', 'a', 'n', 'a');
    if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF)) return make_ot_tag('h', 'a', 'n', 'g');
    return make_ot_tag('l', 'a', 't', 'n');
}

bool is_below_diacritic(char32_t cp) noexcept {
    // Arabic Kasra (0x0650), Kasratan (0x064D), Hamza below (0x0655), Subscript Alef (0x0656)
    // and Latin combining below marks (0x0316..0x0333)
    return cp == 0x064D || cp == 0x0650 || cp == 0x0655 || cp == 0x0656 ||
           (cp >= 0x0316 && cp <= 0x0333);
}

char32_t decode_codepoint_at(std::string_view text, size_t pos) noexcept {
    if (pos >= text.size()) return 0;
    const uint8_t b0 = static_cast<uint8_t>(text[pos]);
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && pos + 1 < text.size()) {
        return ((static_cast<char32_t>(b0 & 0x1F)) << 6) |
               static_cast<char32_t>(static_cast<uint8_t>(text[pos + 1]) & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0 && pos + 2 < text.size()) {
        return ((static_cast<char32_t>(b0 & 0x0F)) << 12) |
               ((static_cast<char32_t>(static_cast<uint8_t>(text[pos + 1]) & 0x3F)) << 6) |
               static_cast<char32_t>(static_cast<uint8_t>(text[pos + 2]) & 0x3F);
    }
    if ((b0 & 0xF8) == 0xF0 && pos + 3 < text.size()) {
        return ((static_cast<char32_t>(b0 & 0x07)) << 18) |
               ((static_cast<char32_t>(static_cast<uint8_t>(text[pos + 1]) & 0x3F)) << 12) |
               ((static_cast<char32_t>(static_cast<uint8_t>(text[pos + 2]) & 0x3F)) << 6) |
               static_cast<char32_t>(static_cast<uint8_t>(text[pos + 3]) & 0x3F);
    }
    return b0;
}

bool is_ignorable_for_unresolved(char32_t cp) noexcept {
    if (cp < 0x20 || cp == 0x7F || cp == 0x00AD || cp == 0xFEFF) return true;
    if (cp >= 0x200B && cp <= 0x200F) return true;
    if (cp >= 0x202A && cp <= 0x202E) return true;
    if (cp >= 0x2060 && cp <= 0x206F) return true;
    return false;
}

Direction resolve_direction(TextDirection text_direction, std::string_view text) noexcept {
    switch (text_direction) {
        case TextDirection::LTR: return Direction::LeftToRight;
        case TextDirection::RTL: return Direction::RightToLeft;
        case TextDirection::Auto: return Bidi::detect_base_direction(text);
    }
    return Direction::LeftToRight;
}

void update_layout_line_byte_bounds(LayoutLine& line) noexcept {
    bool found = false;
    size_t min_start = SIZE_MAX;
    size_t max_end = 0;
    for (const auto& g : line.glyphs) {
        if (g.end > g.start) {
            min_start = std::min(min_start, g.start);
            max_end = std::max(max_end, g.end);
            found = true;
        }
    }
    if (found) {
        line.start_byte = min_start;
        line.end_byte = max_end;
    } else if (!line.glyphs.empty()) {
        line.start_byte = line.glyphs.front().start;
        line.end_byte = line.glyphs.back().end;
    } else {
        line.start_byte = 0;
        line.end_byte = 0;
    }
}

void expand_line_metrics_for_placeholders(LayoutLine& line, float base_line_height) noexcept {
    bool has_ph = false;
    float asc = line.max_ascent;
    float desc = line.max_descent;
    float max_ph_h = 0.0f;
    for (const auto& g : line.glyphs) {
        if (!g.is_placeholder) continue;
        has_ph = true;
        const float h = std::max(0.0f, g.placeholder_height);
        max_ph_h = std::max(max_ph_h, h);
        switch (g.placeholder_alignment) {
            case PlaceholderAlignment::AboveBaseline:
                asc = std::max(asc, h);
                break;
            case PlaceholderAlignment::BelowBaseline:
                desc = std::max(desc, h);
                break;
            case PlaceholderAlignment::Baseline:
                if (g.placeholder_baseline == TextBaseline::Alphabetic) {
                    asc = std::max(asc, std::max(0.0f, g.placeholder_baseline_offset));
                    desc = std::max(desc, std::max(0.0f, h - g.placeholder_baseline_offset));
                } else {
                    const float req_above = g.placeholder_baseline_offset - line.max_descent;
                    const float req_below = line.max_descent + h - g.placeholder_baseline_offset;
                    if (req_above > 0.0f) asc = std::max(asc, req_above);
                    if (req_below > 0.0f) desc = std::max(desc, req_below);
                }
                break;
            case PlaceholderAlignment::Top:
            case PlaceholderAlignment::Bottom:
            case PlaceholderAlignment::Middle:
                break;
        }
    }
    if (has_ph) {
        line.max_ascent = asc;
        line.max_descent = desc;
        line.line_height_opt = std::max({base_line_height, asc + desc, max_ph_h});
    }
}

void apply_line_alignment(
    LayoutLine& line,
    std::string_view line_text,
    std::optional<float> wrap_width,
    Align align,
    Direction base_dir,
    bool is_last_line
) noexcept {
    line.left = 0.0f;
    update_layout_line_byte_bounds(line);
    if (line.glyphs.empty()) {
        line.w = 0.0f;
        return;
    }
    float lx = 0.0f;
    for (auto& g : line.glyphs) {
        if (g.justified_extra_w != 0.0f) {
            g.w = std::max(0.0f, g.w - g.justified_extra_w);
            g.justified_extra_w = 0.0f;
        }
        g.x = lx;
        lx += g.w;
    }
    line.w = lx;

    if (!wrap_width.has_value() || *wrap_width <= line.w) return;

    float spare = *wrap_width - line.w;

    if (align == Align::Justified && !is_last_line) {
        // Count inter-word spaces (excluding trailing spaces at the end of the line)
        size_t last_non_space = line.glyphs.size();
        while (last_non_space > 0) {
            const auto& lg = line.glyphs[last_non_space - 1];
            bool sp = (lg.start < line_text.size() && (line_text[lg.start] == ' ' || line_text[lg.start] == '\t'));
            if (!sp) break;
            --last_non_space;
        }

        size_t space_count = 0;
        for (size_t i = 0; i < last_non_space; ++i) {
            const auto& lg = line.glyphs[i];
            if (!lg.is_mark && lg.start < line_text.size() &&
                (line_text[lg.start] == ' ' || line_text[lg.start] == '\t')) {
                ++space_count;
            }
        }

        if (space_count > 0) {
            float trimmed_w = 0.0f;
            for (size_t i = 0; i < last_non_space; ++i) {
                trimmed_w += line.glyphs[i].w;
            }
            float actual_spare = *wrap_width - trimmed_w;
            if (actual_spare > 0.0f) {
                float extra_per_space = actual_spare / static_cast<float>(space_count);
                float cur_x = 0.0f;
                for (size_t i = 0; i < line.glyphs.size(); ++i) {
                    auto& lg = line.glyphs[i];
                    lg.x = cur_x;
                    if (i < last_non_space && !lg.is_mark && lg.start < line_text.size() &&
                        (line_text[lg.start] == ' ' || line_text[lg.start] == '\t')) {
                        lg.w += extra_per_space;
                        lg.justified_extra_w = extra_per_space;
                    }
                    cur_x += lg.w;
                }
                line.w = *wrap_width;
                line.left = 0.0f;
                return;
            }
        }
    }

    float offset = 0.0f;
    if (align == Align::Right) {
        offset = spare;
    } else if (align == Align::Center) {
        offset = spare * 0.5f;
    } else if (align == Align::Start) {
        offset = (base_dir == Direction::RightToLeft) ? spare : 0.0f;
    } else if (align == Align::End) {
        offset = (base_dir == Direction::RightToLeft) ? 0.0f : spare;
    }
    line.left = offset;
    if (offset > 0.0f) {
        for (auto& g : line.glyphs) {
            g.x += offset;
        }
    }
}

} // namespace

BufferLine::BufferLine(std::string_view line_text, const Attrs& default_attrs)
    : text_(line_text), attrs_list_(default_attrs) {}

void BufferLine::append_text(std::string_view segment, const Attrs& attrs) {
    if (segment.empty()) return;
    const size_t start = text_.size();
    text_.append(segment);
    const size_t end = text_.size();
    if (attrs != attrs_list_.defaults() || !attrs_list_.spans().empty()) {
        attrs_list_.add_span(start, end, attrs);
    }
    reset_shaping();
}

void BufferLine::append_placeholder(size_t index, const PlaceholderStyle& style, const Attrs& attrs) {
    const size_t start = text_.size();
    text_.append("\xEF\xBF\xBC", 3); // Unicode Object Replacement Character U+FFFC
    const size_t end = text_.size();
    placeholders_.push_back(InlinePlaceholder{start, index, style});
    if (attrs != attrs_list_.defaults() || !attrs_list_.spans().empty()) {
        attrs_list_.add_span(start, end, attrs);
    }
    reset_shaping();
}

void BufferLine::shape_and_layout(
    FontSystem& font_system,
    float font_size,
    float line_height,
    std::optional<float> wrap_width,
    Wrap wrap,
    Align align,
    Shaping shaping,
    bool enable_kerning,
    bool enable_ligatures,
    TextDirection text_direction
) {
    layout_lines_.clear();
    is_shaped_ = true;

    if (text_.empty()) {
        resolved_base_dir_ = (text_direction == TextDirection::RTL) ? Direction::RightToLeft : Direction::LeftToRight;
        LayoutLine empty_line;
        empty_line.line_height_opt = line_height;
        empty_line.max_ascent = font_size * 0.8f;
        empty_line.max_descent = font_size * 0.2f;
        empty_line.unscaled_ascent = empty_line.max_ascent;
        empty_line.hard_break = true;
        layout_lines_.push_back(std::move(empty_line));
        return;
    }

    bool is_pure_ascii = true;
    for (unsigned char c : text_) {
        if (c >= 128) {
            is_pure_ascii = false;
            break;
        }
    }

    const Attrs& def_attrs = attrs_list_.defaults();
    const bool has_custom_attrs = !attrs_list_.spans().empty() || !placeholders_.empty() || def_attrs.has_custom_metrics_or_decorations();

    // -------------------------------------------------------------------------
    // ULTRA-FAST PATH: Pure ASCII with Uniform Attributes (Standard Case)
    // -------------------------------------------------------------------------
    if (is_pure_ascii && !has_custom_attrs && !enable_ligatures && text_direction != TextDirection::RTL) {
        resolved_base_dir_ = Direction::LeftToRight;
        auto matched = font_system.match_font_styled(def_attrs.family, def_attrs.weight, def_attrs.style, def_attrs.stretch);
        uint32_t primary_fid = matched.font_id;
        const TtfFont* primary_font = matched.font;
        float scale = (primary_font != nullptr) ? primary_font->scale_for_size(font_size) : 0.0f;
        float asc = primary_font ? primary_font->ascent(font_size) : (font_size * 0.8f);
        float desc = primary_font ? -primary_font->descent(font_size) : (font_size * 0.2f);

        bool synth_bold = matched.synthetic_bold;
        bool synth_italic = matched.synthetic_italic;

        std::vector<LayoutGlyph> all_glyphs;
        all_glyphs.resize(text_.size());
        float cur_x = 0.0f;

        for (size_t i = 0; i < text_.size(); ++i) {
            uint8_t c = static_cast<uint8_t>(text_[i]);
            uint16_t gid = primary_font ? primary_font->ascii_glyph_index(c) : 0;
            float adv = primary_font ? (static_cast<float>(primary_font->ascii_advance(c)) * scale) : (font_size * 0.5f);

            LayoutGlyph& g = all_glyphs[i];
            g.start = i;
            g.end = i + 1;
            g.font_size = font_size;
            g.font_weight = def_attrs.weight;
            g.line_height_opt = line_height;
            g.font_id = primary_fid;
            g.glyph_id = gid;
            g.x = cur_x;
            g.y = 0.0f;
            g.w = adv;
            g.ascent = asc;
            g.descent = desc;
            g.level = 0;
            g.x_offset = 0.0f;
            g.y_offset = 0.0f;
            g.color_opt = def_attrs.color_opt;
            g.metadata = def_attrs.metadata;
            g.synthetic_bold = synth_bold;
            g.synthetic_italic = synth_italic;

            cur_x += adv;
        }

        // Single line without wrapping
        if (!wrap_width.has_value() || wrap == Wrap::None) {
            LayoutLine single_line;
            single_line.line_height_opt = line_height;
            single_line.max_ascent = asc;
            single_line.max_descent = desc;
            single_line.unscaled_ascent = asc;
            single_line.hard_break = true;
            single_line.w = cur_x;
            single_line.glyphs = std::move(all_glyphs);
            apply_line_alignment(single_line, text_, wrap_width, align, Direction::LeftToRight, true);
            layout_lines_.push_back(std::move(single_line));
            return;
        }

        float max_w = *wrap_width;

        if (wrap == Wrap::Glyph) {
            LayoutLine cur_line;
            cur_line.line_height_opt = line_height;
            cur_line.max_ascent = asc;
            cur_line.max_descent = desc;
            cur_line.unscaled_ascent = asc;
            float cur_line_w = 0.0f;

            for (auto& g : all_glyphs) {
                if (cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                    apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                    layout_lines_.push_back(std::move(cur_line));
                    cur_line = LayoutLine{};
                    cur_line.line_height_opt = line_height;
                    cur_line.max_ascent = asc;
                    cur_line.max_descent = desc;
                    cur_line.unscaled_ascent = asc;
                    cur_line_w = 0.0f;
                }
                cur_line_w += g.w;
                cur_line.glyphs.push_back(g);
            }
            if (!cur_line.glyphs.empty()) {
                cur_line.hard_break = true;
                apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, true);
                layout_lines_.push_back(std::move(cur_line));
            }
            return;
        } else {
            struct Word {
                std::vector<LayoutGlyph> glyphs;
                float width{0.0f};
                bool is_space{false};
            };

            std::vector<Word> words;
            words.reserve(all_glyphs.size() / 4 + 1);
            Word current_word;

            for (size_t i = 0; i < all_glyphs.size(); ++i) {
                char c = text_[i];
                bool is_sp = (c == ' ' || c == '\t');

                if (!current_word.glyphs.empty() && current_word.is_space != is_sp) {
                    words.push_back(std::move(current_word));
                    current_word = Word{};
                }

                current_word.is_space = is_sp;
                current_word.glyphs.push_back(all_glyphs[i]);
                current_word.width += all_glyphs[i].w;
            }
            if (!current_word.glyphs.empty()) {
                words.push_back(std::move(current_word));
            }

            LayoutLine cur_line;
            cur_line.line_height_opt = line_height;
            cur_line.max_ascent = asc;
            cur_line.max_descent = desc;
            cur_line.unscaled_ascent = asc;
            float cur_line_w = 0.0f;

            for (auto& w : words) {
                if (cur_line_w + w.width > max_w && !cur_line.glyphs.empty()) {
                    if (!w.is_space) {
                        apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                        layout_lines_.push_back(std::move(cur_line));
                        cur_line = LayoutLine{};
                        cur_line.line_height_opt = line_height;
                        cur_line.max_ascent = asc;
                        cur_line.max_descent = desc;
                        cur_line.unscaled_ascent = asc;
                        cur_line_w = 0.0f;
                    }
                }

                if (w.width > max_w && wrap == Wrap::WordOrGlyph && !w.is_space) {
                    for (auto& g : w.glyphs) {
                        if (cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                            apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                            layout_lines_.push_back(std::move(cur_line));
                            cur_line = LayoutLine{};
                            cur_line.line_height_opt = line_height;
                            cur_line.max_ascent = asc;
                            cur_line.max_descent = desc;
                            cur_line.unscaled_ascent = asc;
                            cur_line_w = 0.0f;
                        }
                        cur_line_w += g.w;
                        cur_line.glyphs.push_back(g);
                    }
                } else {
                    for (auto& g : w.glyphs) {
                        cur_line.glyphs.push_back(g);
                    }
                    cur_line_w += w.width;
                }
            }

            if (!cur_line.glyphs.empty()) {
                cur_line.hard_break = true;
                apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, true);
                layout_lines_.push_back(std::move(cur_line));
            }
            return;
        }
    }

    // -------------------------------------------------------------------------
    // FAST LTR PATH: UTF-8 without RTL / Combining Marks / Custom Spans
    // -------------------------------------------------------------------------
    auto decoded = Bidi::decode_utf8(text_);
    bool has_rtl_or_marks = false;
    for (const auto& [cp, _] : decoded) {
        if (Bidi::is_combining_mark(cp)) {
            has_rtl_or_marks = true;
            break;
        }
        if (cp >= 0x0590 && cp <= 0x08FF) {
            auto t = Bidi::classify_char(cp);
            if (t == CharType::ArabicLetter || t == CharType::RTL) {
                has_rtl_or_marks = true;
                break;
            }
        }
    }

    if (!has_rtl_or_marks && !has_custom_attrs && !enable_ligatures && text_direction != TextDirection::RTL) {
        resolved_base_dir_ = Direction::LeftToRight;
        auto matched = font_system.match_font_styled(def_attrs.family, def_attrs.weight, def_attrs.style, def_attrs.stretch);
        uint32_t primary_fid = matched.font_id;
        const TtfFont* primary_font = matched.font;
        float scale = (primary_font != nullptr) ? primary_font->scale_for_size(font_size) : 0.0f;
        float asc = primary_font ? primary_font->ascent(font_size) : (font_size * 0.8f);
        float desc = primary_font ? -primary_font->descent(font_size) : (font_size * 0.2f);
        bool synth_bold = matched.synthetic_bold;
        bool synth_italic = matched.synthetic_italic;

        std::vector<LayoutGlyph> all_glyphs;
        all_glyphs.resize(decoded.size());
        float cur_x = 0.0f;

        for (size_t i = 0; i < decoded.size(); ++i) {
            char32_t cp = decoded[i].first;
            size_t byte_idx = decoded[i].second;

            uint32_t actual_fid = primary_fid;
            uint16_t gid = 0;
            float adv = font_size * 0.5f;
            float g_asc = asc;
            float g_desc = desc;

            if (primary_font != nullptr) {
                gid = primary_font->glyph_index(cp);
                adv = primary_font->glyph_advance_scaled(gid, scale);
            }

            if (gid == 0 && font_system.font_count() > 1) {
                auto [fb_fid, fb_gid] = font_system.find_glyph_or_fallback(primary_fid, cp);
                actual_fid = fb_fid;
                const TtfFont* fb_font = font_system.get_font(fb_fid);
                if (fb_font != nullptr) {
                    gid = fb_gid;
                    adv = fb_font->glyph_advance(gid, font_size);
                    g_asc = fb_font->ascent(font_size);
                    g_desc = -fb_font->descent(font_size);
                }
            }

            LayoutGlyph& g = all_glyphs[i];
            g.start = byte_idx;
            g.end = (i + 1 < decoded.size()) ? decoded[i + 1].second : text_.size();
            g.font_size = font_size;
            g.font_weight = def_attrs.weight;
            g.line_height_opt = line_height;
            g.font_id = actual_fid;
            g.glyph_id = gid;
            g.x = cur_x;
            g.y = 0.0f;
            g.w = adv;
            g.ascent = g_asc;
            g.descent = g_desc;
            g.level = 0;
            g.x_offset = 0.0f;
            g.y_offset = 0.0f;
            g.color_opt = def_attrs.color_opt;
            g.metadata = def_attrs.metadata;
            g.synthetic_bold = synth_bold;
            g.synthetic_italic = synth_italic;

            cur_x += adv;
        }

        if (!wrap_width.has_value() || wrap == Wrap::None) {
            LayoutLine single_line;
            single_line.line_height_opt = line_height;
            single_line.max_ascent = asc;
            single_line.max_descent = desc;
            single_line.unscaled_ascent = asc;
            single_line.hard_break = true;
            single_line.w = cur_x;
            single_line.glyphs = std::move(all_glyphs);
            apply_line_alignment(single_line, text_, wrap_width, align, Direction::LeftToRight, true);
            layout_lines_.push_back(std::move(single_line));
            return;
        }

        float max_w = *wrap_width;

        if (wrap == Wrap::Glyph) {
            LayoutLine cur_line;
            cur_line.line_height_opt = line_height;
            cur_line.max_ascent = asc;
            cur_line.max_descent = desc;
            cur_line.unscaled_ascent = asc;
            float cur_line_w = 0.0f;

            for (auto& g : all_glyphs) {
                if (cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                    apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                    layout_lines_.push_back(std::move(cur_line));
                    cur_line = LayoutLine{};
                    cur_line.line_height_opt = line_height;
                    cur_line.max_ascent = asc;
                    cur_line.max_descent = desc;
                    cur_line.unscaled_ascent = asc;
                    cur_line_w = 0.0f;
                }
                cur_line_w += g.w;
                cur_line.glyphs.push_back(g);
            }
            if (!cur_line.glyphs.empty()) {
                cur_line.hard_break = true;
                apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, true);
                layout_lines_.push_back(std::move(cur_line));
            }
            return;
        } else {
            struct Word {
                std::vector<LayoutGlyph> glyphs;
                float width{0.0f};
                bool is_space{false};
            };

            std::vector<Word> words;
            words.reserve(all_glyphs.size() / 4 + 1);
            Word current_word;
            bool prev_was_cjk = false;
            bool prev_no_break_after = false;

            for (size_t i = 0; i < all_glyphs.size(); ++i) {
                char32_t cp = decoded[i].first;
                bool is_sp = (cp == ' ' || cp == '\t');
                bool is_cjk_char = Bidi::is_cjk(cp);
                bool no_break_before = is_cjk_char && Bidi::is_cjk_no_break_before(cp);
                bool no_break_after = is_cjk_char && Bidi::is_cjk_no_break_after(cp);

                bool should_split = false;
                if (!current_word.glyphs.empty()) {
                    if (current_word.is_space != is_sp) {
                        should_split = true;
                    } else if (!is_sp) {
                        if (is_cjk_char) {
                            if (!no_break_before && !prev_no_break_after) should_split = true;
                        } else if (prev_was_cjk && !no_break_before) {
                            should_split = true;
                        }
                    }
                }

                if (should_split) {
                    words.push_back(std::move(current_word));
                    current_word = Word{};
                }

                current_word.is_space = is_sp;
                current_word.glyphs.push_back(all_glyphs[i]);
                current_word.width += all_glyphs[i].w;

                prev_was_cjk = is_cjk_char;
                prev_no_break_after = no_break_after;
            }
            if (!current_word.glyphs.empty()) {
                words.push_back(std::move(current_word));
            }

            LayoutLine cur_line;
            cur_line.line_height_opt = line_height;
            cur_line.max_ascent = asc;
            cur_line.max_descent = desc;
            cur_line.unscaled_ascent = asc;
            float cur_line_w = 0.0f;

            for (auto& w : words) {
                if (cur_line_w + w.width > max_w && !cur_line.glyphs.empty()) {
                    if (!w.is_space) {
                        apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                        layout_lines_.push_back(std::move(cur_line));
                        cur_line = LayoutLine{};
                        cur_line.line_height_opt = line_height;
                        cur_line.max_ascent = asc;
                        cur_line.max_descent = desc;
                        cur_line.unscaled_ascent = asc;
                        cur_line_w = 0.0f;
                    }
                }

                if (w.width > max_w && wrap == Wrap::WordOrGlyph && !w.is_space) {
                    for (auto& g : w.glyphs) {
                        if (cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                            apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                            layout_lines_.push_back(std::move(cur_line));
                            cur_line = LayoutLine{};
                            cur_line.line_height_opt = line_height;
                            cur_line.max_ascent = asc;
                            cur_line.max_descent = desc;
                            cur_line.unscaled_ascent = asc;
                            cur_line_w = 0.0f;
                        }
                        cur_line_w += g.w;
                        cur_line.glyphs.push_back(g);
                    }
                } else {
                    for (auto& g : w.glyphs) {
                        cur_line.glyphs.push_back(g);
                    }
                    cur_line_w += w.width;
                }
            }

            if (!cur_line.glyphs.empty()) {
                cur_line.hard_break = true;
                apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, true);
                layout_lines_.push_back(std::move(cur_line));
            }
            return;
        }
    }

    // -------------------------------------------------------------------------
    // FULL ADVANCED SHAPING PIPELINE: BiDi, Rich Spans, GSUB/GPOS, Marks & Kerning
    // -------------------------------------------------------------------------
    Direction base_dir = resolve_direction(text_direction, text_);
    resolved_base_dir_ = base_dir;
    std::vector<ShapedChar> shaped_chars;

    if (is_pure_ascii && base_dir == Direction::LeftToRight) {
        shaped_chars.resize(text_.size());
        for (size_t i = 0; i < text_.size(); ++i) {
            char32_t cp = static_cast<char32_t>(static_cast<unsigned char>(text_[i]));
            shaped_chars[i] = ShapedChar{cp, i, 0, false, false, ArabicForm::None, cp};
        }
    } else {
        auto runs = Bidi::segment_runs(text_, base_dir);
        if (shaping == Shaping::Advanced) {
            shaped_chars = Bidi::shape_text(text_, runs);
        } else {
            shaped_chars.reserve(decoded.size());
            const uint8_t base_lvl = (base_dir == Direction::RightToLeft) ? 1 : 0;
            for (const auto& [cp, byte_idx] : decoded) {
                shaped_chars.push_back(ShapedChar{cp, byte_idx, base_lvl, base_lvl != 0, Bidi::is_combining_mark(cp), ArabicForm::None, cp});
            }
        }
    }

    if (shaped_chars.empty()) {
        LayoutLine empty_line;
        empty_line.line_height_opt = line_height;
        empty_line.max_ascent = font_size * 0.8f;
        empty_line.max_descent = font_size * 0.2f;
        empty_line.unscaled_ascent = empty_line.max_ascent;
        empty_line.hard_break = true;
        layout_lines_.push_back(std::move(empty_line));
        return;
    }

    // Map byte_index -> end byte_index from original UTF-8 stream
    auto find_char_end_byte = [&](size_t byte_idx) -> size_t {
        return Bidi::next_grapheme_cluster(text_, byte_idx);
    };

    std::vector<LayoutGlyph> all_glyphs;
    all_glyphs.reserve(shaped_chars.size());

    uint32_t last_primary_fid = 0;
    const TtfFont* last_primary_font = nullptr;
    std::optional<Attrs> last_attrs = std::nullopt;
    std::shared_ptr<const std::vector<FontVariation>> last_variations_ptr{nullptr};
    uint64_t last_variations_hash{0};
    bool last_has_wght_var{false};
    uint32_t last_lang_tag{0};
    bool any_user_gsub_features{false};

    float max_line_asc = font_size * 0.8f;
    float max_line_desc = font_size * 0.2f;

    auto find_placeholder_at = [&](size_t byte_idx) -> const InlinePlaceholder* {
        for (const auto& ph : placeholders_) {
            if (ph.byte_offset == byte_idx) return &ph;
        }
        return nullptr;
    };

    for (size_t i = 0; i < shaped_chars.size(); ++i) {
        const auto& sc = shaped_chars[i];
        Attrs attrs = attrs_list_.spans().empty() ? attrs_list_.defaults() : attrs_list_.get(sc.byte_index);
        float eff_font_size = attrs.font_size_opt.value_or(font_size);

        if (sc.codepoint == 0xFFFC && !placeholders_.empty()) {
            if (const InlinePlaceholder* ph = find_placeholder_at(sc.byte_index)) {
                LayoutGlyph pg;
                pg.start = sc.byte_index;
                pg.end = find_char_end_byte(sc.byte_index);
                pg.font_size = eff_font_size;
                pg.font_weight = attrs.weight;
                pg.line_height_opt = line_height;
                pg.font_id = 0;
                pg.glyph_id = 0;
                pg.y = attrs.baseline_shift;
                pg.w = std::max(0.0f, ph->style.width);
                pg.level = sc.level;
                pg.color_opt = attrs.color_opt;
                pg.background_color_opt = attrs.background_color_opt;
                pg.foreground_paint_opt = attrs.foreground_paint_opt;
                pg.background_paint_opt = attrs.background_paint_opt;
                pg.foreground_stroke_opt = attrs.foreground_stroke_opt;
                pg.metadata = attrs.metadata;
                pg.is_mark = false;
                pg.underline = attrs.underline;
                pg.strikethrough = attrs.strikethrough;
                pg.overline = attrs.overline;
                pg.decoration_style = attrs.decoration_style;
                pg.decoration_mode = attrs.decoration_mode;
                pg.decoration_color_opt = attrs.decoration_color_opt;
                pg.decoration_thickness = attrs.decoration_thickness;
                pg.is_placeholder = true;
                pg.placeholder_index = ph->index;
                pg.placeholder_height = std::max(0.0f, ph->style.height);
                pg.placeholder_alignment = ph->style.alignment;
                pg.placeholder_baseline = ph->style.baseline;
                pg.placeholder_baseline_offset = ph->style.baseline_offset;
                all_glyphs.push_back(std::move(pg));
                continue;
            }
        }

        if (!last_attrs.has_value() || *last_attrs != attrs) {
            FontMatchResult matched = attrs.font_families.empty()
                ? font_system.match_font_styled(attrs.family, attrs.weight, attrs.style, attrs.stretch)
                : font_system.match_font_chain_styled(attrs.font_families, attrs.family, attrs.weight, attrs.style, attrs.stretch, attrs.locale);
            last_primary_fid = matched.font_id;
            last_primary_font = matched.font;
            last_lang_tag = attrs.locale.empty()
                ? OpenTypeGsub::bcp47_to_ot_lang_tag(font_system.locale())
                : OpenTypeGsub::bcp47_to_ot_lang_tag(attrs.locale);
            if (!attrs.font_variations.empty()) {
                last_variations_ptr = std::make_shared<std::vector<FontVariation>>(attrs.font_variations);
                last_variations_hash = hash_font_variations(attrs.font_variations);
                last_has_wght_var = false;
                constexpr uint32_t TAG_WGHT = make_ot_tag('w', 'g', 'h', 't');
                for (const auto& fv : attrs.font_variations) {
                    if (fv.tag == TAG_WGHT) { last_has_wght_var = true; break; }
                }
            } else {
                last_variations_ptr.reset();
                last_variations_hash = 0;
                last_has_wght_var = false;
            }
            if (!attrs.font_features.empty() || !attrs.locale.empty()) {
                any_user_gsub_features = true;
            }
            last_attrs = attrs;
        }

        const uint32_t script_tag = detect_ot_script_tag(sc.original_codepoint != 0 ? sc.original_codepoint : sc.codepoint);

        uint32_t actual_fid = last_primary_fid;
        const TtfFont* font = last_primary_font;
        uint16_t gid = 0;

        if (font != nullptr) {
            gid = font->glyph_index(sc.codepoint);
            // If font lacks Arabic Presentation Forms (0xFE80..0xFEFC), fall back to original codepoint + OpenType GSUB!
            if (gid == 0 && sc.original_codepoint != 0 && sc.original_codepoint != sc.codepoint) {
                gid = font->glyph_index(sc.original_codepoint);
                if (gid != 0 && font->gsub() != nullptr && sc.arabic_form != ArabicForm::None) {
                    uint32_t feat_tag = 0;
                    switch (sc.arabic_form) {
                        case ArabicForm::Isolated: feat_tag = make_ot_tag('i', 's', 'o', 'l'); break;
                        case ArabicForm::Initial:  feat_tag = make_ot_tag('i', 'n', 'i', 't'); break;
                        case ArabicForm::Medial:   feat_tag = make_ot_tag('m', 'e', 'd', 'i'); break;
                        case ArabicForm::Final:    feat_tag = make_ot_tag('f', 'i', 'n', 'a'); break;
                        default: break;
                    }
                    if (feat_tag != 0) {
                        gid = font->gsub()->apply_single_glyph_feature(gid, feat_tag, script_tag, last_lang_tag);
                    }
                }
            }
        }

        if (gid == 0 && font_system.font_count() > 1) {
            auto lookup_fb = [&](char32_t cp) -> std::pair<uint32_t, uint16_t> {
                if (!attrs.font_families.empty() || !attrs.locale.empty()) {
                    return font_system.find_glyph_in_chain_or_fallback(
                        attrs.font_families, last_primary_fid, attrs.weight, attrs.style, attrs.stretch, cp, attrs.locale
                    );
                }
                return font_system.find_glyph_or_fallback(last_primary_fid, cp);
            };

            auto [fb_fid, fb_gid] = lookup_fb(sc.codepoint);
            if (fb_gid == 0 && sc.original_codepoint != 0 && sc.original_codepoint != sc.codepoint) {
                std::tie(fb_fid, fb_gid) = lookup_fb(sc.original_codepoint);
                const TtfFont* fb_font = font_system.get_font(fb_fid);
                if (fb_gid != 0 && fb_font && fb_font->gsub() && sc.arabic_form != ArabicForm::None) {
                    uint32_t feat_tag = 0;
                    switch (sc.arabic_form) {
                        case ArabicForm::Isolated: feat_tag = make_ot_tag('i', 's', 'o', 'l'); break;
                        case ArabicForm::Initial:  feat_tag = make_ot_tag('i', 'n', 'i', 't'); break;
                        case ArabicForm::Medial:   feat_tag = make_ot_tag('m', 'e', 'd', 'i'); break;
                        case ArabicForm::Final:    feat_tag = make_ot_tag('f', 'i', 'n', 'a'); break;
                        default: break;
                    }
                    if (feat_tag != 0) {
                        fb_gid = fb_font->gsub()->apply_single_glyph_feature(fb_gid, feat_tag, script_tag, last_lang_tag);
                    }
                }
            }
            actual_fid = fb_fid;
            gid = fb_gid;
            font = font_system.get_font(actual_fid);
        }

        // Apply localized forms ('locl') and single-glyph OpenType features (e.g. 'tnum', 'smcp', 'ss01', 'zero')
        if (gid != 0 && font != nullptr && font->gsub() != nullptr && shaping == Shaping::Advanced) {
            if (!attrs.locale.empty() && last_lang_tag != 0) {
                gid = font->gsub()->apply_single_glyph_feature(gid, make_ot_tag('l', 'o', 'c', 'l'), script_tag, last_lang_tag, 1);
            }
            for (const auto& feat : attrs.font_features) {
                if (feat.value == 0) continue;
                std::vector<uint16_t> single_seq{gid};
                const FontFeature single_feat[1] = {feat};
                if (font->gsub()->apply_features_for_script(single_seq, single_feat, script_tag, last_lang_tag) &&
                    single_seq.size() == 1) {
                    gid = single_seq[0];
                }
            }
        }

        const float glyph_asc = font ? font->ascent(eff_font_size) : (eff_font_size * 0.8f);
        const float glyph_desc = font ? -font->descent(eff_font_size) : (eff_font_size * 0.2f);
        if (font != nullptr) {
            max_line_asc = std::max(max_line_asc, glyph_asc);
            max_line_desc = std::max(max_line_desc, glyph_desc);
        }

        bool is_mark_glyph = sc.is_mark || (font && font->gdef() && font->gdef()->is_mark(gid));
        float adv = 0.0f;
        if (!is_mark_glyph) {
            if (font != nullptr) {
                adv = (last_variations_ptr && font->is_variable_font())
                    ? font->glyph_advance_varied(gid, eff_font_size, *last_variations_ptr)
                    : font->glyph_advance(gid, eff_font_size);
            } else {
                adv = eff_font_size * 0.5f;
            }
            adv += attrs.letter_spacing;
            if (sc.codepoint == ' ' || sc.codepoint == '\t') {
                adv += attrs.word_spacing;
            }
        }

        bool synth_bold = font && !last_has_wght_var &&
                          (static_cast<int>(attrs.weight) >= 600) && (static_cast<int>(font->weight()) < 600);
        bool synth_italic = font && (attrs.style != Style::Normal) && (font->style() == Style::Normal);

        LayoutGlyph g;
        g.start = sc.byte_index;
        g.end = find_char_end_byte(sc.byte_index);
        g.font_size = eff_font_size;
        g.font_weight = attrs.weight;
        g.line_height_opt = line_height;
        g.font_id = actual_fid;
        g.glyph_id = gid;
        g.y = attrs.baseline_shift;
        g.w = std::max(0.0f, adv);
        g.ascent = glyph_asc;
        g.descent = glyph_desc;
        g.level = sc.level;
        g.color_opt = attrs.color_opt;
        g.background_color_opt = attrs.background_color_opt;
        g.metadata = attrs.metadata;
        g.is_mark = is_mark_glyph;
        g.underline = attrs.underline;
        g.strikethrough = attrs.strikethrough;
        g.overline = attrs.overline;
        g.decoration_style = attrs.decoration_style;
        g.decoration_mode = attrs.decoration_mode;
        g.decoration_color_opt = attrs.decoration_color_opt;
        g.decoration_thickness = attrs.decoration_thickness;
        g.foreground_paint_opt = attrs.foreground_paint_opt;
        g.background_paint_opt = attrs.background_paint_opt;
        g.foreground_stroke_opt = attrs.foreground_stroke_opt;
        g.variation_coords_opt = last_variations_ptr;
        g.variation_hash = last_variations_hash;
        g.shadows = attrs.shadows;
        g.synthetic_bold = synth_bold;
        g.synthetic_italic = synth_italic;

        // Position combining marks / Arabic diacritics (Mark-to-Base and Mark-to-Mark)
        if (is_mark_glyph && !all_glyphs.empty() && font != nullptr) {
            float scale = font->scale_for_size(eff_font_size);
            // Find the preceding base glyph (and immediate previous mark if stacked)
            int prev_idx = static_cast<int>(all_glyphs.size()) - 1;
            int base_idx = prev_idx;
            while (base_idx >= 0 && all_glyphs[base_idx].is_mark) {
                --base_idx;
            }

            bool positioned_by_gpos = false;
            if (font->gpos() != nullptr) {
                int16_t dx_units = 0, dy_units = 0;
                if (prev_idx >= 0 && all_glyphs[prev_idx].is_mark &&
                    font->gpos()->get_mark_to_mark_offset(all_glyphs[prev_idx].glyph_id, gid, dx_units, dy_units)) {
                    g.x_offset = all_glyphs[prev_idx].x_offset + static_cast<float>(dx_units) * scale;
                    // OpenType Y is upwards positive; screen Y is downwards positive
                    g.y_offset = all_glyphs[prev_idx].y_offset - static_cast<float>(dy_units) * scale;
                    positioned_by_gpos = true;
                } else if (base_idx >= 0 &&
                           font->gpos()->get_mark_to_base_offset(all_glyphs[base_idx].glyph_id, gid, dx_units, dy_units)) {
                    const auto& base_g = all_glyphs[base_idx];
                    // Since mark is placed at cur_x = base_g.x + base_g.w, shift back by base_g.w + dx
                    g.x_offset = -base_g.w + static_cast<float>(dx_units) * scale;
                    g.y_offset = -static_cast<float>(dy_units) * scale;
                    positioned_by_gpos = true;
                }
            }

            if (!positioned_by_gpos && base_idx >= 0) {
                const auto& base_g = all_glyphs[base_idx];
                int16_t mark_adv = 0, mark_lsb = 0;
                font->get_glyph_metrics(gid, &mark_adv, &mark_lsb);
                // Center mark horizontally over the base glyph
                g.x_offset = -base_g.w * 0.5f;
                if (prev_idx > base_idx) {
                    // Stacked mark (e.g. Shadda + Fatha): shift slightly further vertically
                    float stack_dir = is_below_diacritic(sc.codepoint) ? 1.0f : -1.0f;
                    g.y_offset = all_glyphs[prev_idx].y_offset + stack_dir * (eff_font_size * 0.18f);
                }
            }
        } else if (enable_kerning && shaping == Shaping::Advanced && !all_glyphs.empty() && font != nullptr && font->has_kerning()) {
            // Apply pair kerning with previous base glyph of the same font and direction
            int prev_base = static_cast<int>(all_glyphs.size()) - 1;
            while (prev_base >= 0 && all_glyphs[prev_base].is_mark) {
                --prev_base;
            }
            if (prev_base >= 0) {
                auto& pg = all_glyphs[prev_base];
                if (pg.font_id == actual_fid && pg.level == g.level && pg.glyph_id != 0 && gid != 0) {
                    int16_t kern_units = font->get_kerning(pg.glyph_id, gid);
                    if (kern_units != 0) {
                        pg.w = std::max(0.0f, pg.w + static_cast<float>(kern_units) * font->scale_for_size(pg.font_size));
                    }
                }
            }
        }

        all_glyphs.push_back(g);
    }

    // Optional OpenType GSUB Ligatures / Multi-Glyph Feature pass on contiguous same-font runs
    if ((enable_ligatures || any_user_gsub_features) && shaping == Shaping::Advanced && all_glyphs.size() >= 2) {
        size_t i = 0;
        while (i + 1 < all_glyphs.size()) {
            const TtfFont* font = font_system.get_font(all_glyphs[i].font_id);
            if (!font || !font->gsub() || all_glyphs[i].is_mark || all_glyphs[i].is_placeholder) {
                ++i;
                continue;
            }
            const Attrs run_attrs = attrs_list_.spans().empty() ? attrs_list_.defaults() : attrs_list_.get(all_glyphs[i].start);
            std::vector<FontFeature> active_feats;
            if (enable_ligatures) {
                active_feats.push_back(FontFeature{make_ot_tag('r', 'l', 'i', 'g'), 1});
                active_feats.push_back(FontFeature{make_ot_tag('c', 'a', 'l', 't'), 1});
                active_feats.push_back(FontFeature{make_ot_tag('l', 'i', 'g', 'a'), 1});
                active_feats.push_back(FontFeature{make_ot_tag('c', 'l', 'i', 'g'), 1});
            }
            for (const auto& uf : run_attrs.font_features) {
                bool replaced = false;
                for (auto& af : active_feats) {
                    if (af.tag == uf.tag) {
                        af.value = uf.value;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced && uf.value > 0) {
                    active_feats.push_back(uf);
                }
            }
            // Strip explicitly disabled features (value == 0)
            active_feats.erase(
                std::remove_if(active_feats.begin(), active_feats.end(), [](const FontFeature& f) { return f.value == 0; }),
                active_feats.end()
            );
            if (active_feats.empty()) {
                ++i;
                continue;
            }

            const uint32_t run_script = detect_ot_script_tag(decode_codepoint_at(text_, all_glyphs[i].start));
            const uint32_t run_lang = run_attrs.locale.empty()
                ? OpenTypeGsub::bcp47_to_ot_lang_tag(font_system.locale())
                : OpenTypeGsub::bcp47_to_ot_lang_tag(run_attrs.locale);

            // Try 3-glyph and 2-glyph ligatures locally to preserve byte ranges
            bool merged = false;
            for (size_t span_len : {size_t(3), size_t(2)}) {
                if (i + span_len > all_glyphs.size()) continue;
                bool same_run = true;
                std::vector<uint16_t> seq;
                seq.reserve(span_len);
                for (size_t k = 0; k < span_len; ++k) {
                    if (all_glyphs[i + k].font_id != all_glyphs[i].font_id ||
                        all_glyphs[i + k].level != all_glyphs[i].level ||
                        all_glyphs[i + k].is_mark ||
                        all_glyphs[i + k].is_placeholder) {
                        same_run = false;
                        break;
                    }
                    seq.push_back(all_glyphs[i + k].glyph_id);
                }
                if (same_run &&
                    font->gsub()->apply_features_for_script(seq, active_feats, run_script, run_lang) &&
                    seq.size() == 1) {
                    all_glyphs[i].glyph_id = seq[0];
                    all_glyphs[i].end = all_glyphs[i + span_len - 1].end;
                    all_glyphs[i].w = (all_glyphs[i].variation_coords_opt && font->is_variable_font())
                        ? font->glyph_advance_varied(seq[0], all_glyphs[i].font_size, *all_glyphs[i].variation_coords_opt)
                        : font->glyph_advance(seq[0], all_glyphs[i].font_size);
                    all_glyphs.erase(
                        all_glyphs.begin() + static_cast<ptrdiff_t>(i + 1),
                        all_glyphs.begin() + static_cast<ptrdiff_t>(i + span_len)
                    );
                    merged = true;
                    break;
                }
            }
            ++i;
            (void)merged;
        }
    }

    if (!wrap_width.has_value() || wrap == Wrap::None) {
        LayoutLine single_line;
        single_line.line_height_opt = line_height;
        single_line.max_ascent = max_line_asc;
        single_line.max_descent = max_line_desc;
        single_line.unscaled_ascent = max_line_asc;
        single_line.hard_break = true;
        single_line.glyphs = std::move(all_glyphs);
        expand_line_metrics_for_placeholders(single_line, line_height);
        apply_line_alignment(single_line, text_, wrap_width, align, base_dir, true);
        layout_lines_.push_back(std::move(single_line));
        return;
    }

    float max_w = *wrap_width;

    if (wrap == Wrap::Glyph) {
        LayoutLine cur_line;
        cur_line.line_height_opt = line_height;
        cur_line.max_ascent = max_line_asc;
        cur_line.max_descent = max_line_desc;
        cur_line.unscaled_ascent = max_line_asc;
        float cur_line_w = 0.0f;

        for (auto& g : all_glyphs) {
            if (!g.is_mark && cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                expand_line_metrics_for_placeholders(cur_line, line_height);
                apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, false);
                layout_lines_.push_back(std::move(cur_line));
                cur_line = LayoutLine{};
                cur_line.line_height_opt = line_height;
                cur_line.max_ascent = max_line_asc;
                cur_line.max_descent = max_line_desc;
                cur_line.unscaled_ascent = max_line_asc;
                cur_line_w = 0.0f;
            }
            cur_line_w += g.w;
            cur_line.glyphs.push_back(g);
        }
        if (!cur_line.glyphs.empty()) {
            cur_line.hard_break = true;
            expand_line_metrics_for_placeholders(cur_line, line_height);
            apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, true);
            layout_lines_.push_back(std::move(cur_line));
        }
    } else {
        struct Word {
            std::vector<LayoutGlyph> glyphs;
            float width{0.0f};
            bool is_space{false};
            bool is_placeholder{false};
            uint8_t level{0};
            size_t min_start{SIZE_MAX};
        };

        std::vector<Word> words;
        Word current_word;
        bool prev_was_cjk = false;
        bool prev_no_break_after = false;

        for (size_t i = 0; i < all_glyphs.size(); ++i) {
            const auto& g = all_glyphs[i];
            if (g.is_mark && !current_word.glyphs.empty() && !current_word.is_placeholder) {
                current_word.glyphs.push_back(g);
                current_word.min_start = std::min(current_word.min_start, g.start);
                continue;
            }

            if (g.is_placeholder) {
                if (!current_word.glyphs.empty()) {
                    words.push_back(std::move(current_word));
                    current_word = Word{};
                }
                Word ph_word;
                ph_word.glyphs.push_back(g);
                ph_word.width = g.w;
                ph_word.is_space = false;
                ph_word.is_placeholder = true;
                ph_word.level = g.level;
                ph_word.min_start = g.start;
                words.push_back(std::move(ph_word));
                prev_was_cjk = false;
                prev_no_break_after = false;
                continue;
            }

            char32_t cp = (g.start < text_.size()) ? decode_codepoint_at(text_, g.start) : 0;
            bool is_sp = (cp == ' ' || cp == '\t');
            bool is_cjk_char = is_pure_ascii ? false : Bidi::is_cjk(cp);
            bool no_break_before = is_cjk_char && Bidi::is_cjk_no_break_before(cp);
            bool no_break_after = is_cjk_char && Bidi::is_cjk_no_break_after(cp);

            bool should_split = false;
            if (!current_word.glyphs.empty()) {
                if (current_word.is_space != is_sp || current_word.level != g.level) {
                    should_split = true;
                } else if (!is_sp) {
                    if (is_cjk_char) {
                        if (!no_break_before && !prev_no_break_after) {
                            should_split = true;
                        }
                    } else if (prev_was_cjk && !no_break_before) {
                        should_split = true;
                    }
                }
            }

            if (should_split) {
                words.push_back(std::move(current_word));
                current_word = Word{};
            }

            if (current_word.glyphs.empty()) {
                current_word.is_space = is_sp;
                current_word.level = g.level;
            }
            current_word.glyphs.push_back(g);
            current_word.width += g.w;
            current_word.min_start = std::min(current_word.min_start, g.start);

            prev_was_cjk = is_cjk_char;
            prev_no_break_after = no_break_after;
        }
        if (!current_word.glyphs.empty()) {
            words.push_back(std::move(current_word));
        }

        // For RTL base direction, pack wrapped lines in logical byte order and then
        // apply UAX #9 Rule L2 per line so line 0 holds the logical start of the paragraph.
        bool reorder_lines_bidi = (base_dir == Direction::RightToLeft && words.size() > 1);
        if (reorder_lines_bidi) {
            std::stable_sort(words.begin(), words.end(), [](const Word& a, const Word& b) {
                return a.min_start < b.min_start;
            });
        }

        auto emit_packed_line = [&](std::vector<Word>& line_words, bool is_last) {
            if (line_words.empty()) return;
            if (reorder_lines_bidi && line_words.size() > 1) {
                uint8_t max_lvl = 0;
                for (const auto& lw : line_words) {
                    max_lvl = std::max(max_lvl, lw.level);
                }
                for (uint8_t lvl = max_lvl; lvl >= 1; --lvl) {
                    size_t wi = 0;
                    while (wi < line_words.size()) {
                        if (line_words[wi].level >= lvl) {
                            size_t wj = wi + 1;
                            while (wj < line_words.size() && line_words[wj].level >= lvl) {
                                ++wj;
                            }
                            if (wj - wi > 1) {
                                std::reverse(line_words.begin() + static_cast<ptrdiff_t>(wi),
                                             line_words.begin() + static_cast<ptrdiff_t>(wj));
                            }
                            wi = wj;
                        } else {
                            ++wi;
                        }
                    }
                }
            }
            LayoutLine out_line;
            out_line.line_height_opt = line_height;
            out_line.max_ascent = max_line_asc;
            out_line.max_descent = max_line_desc;
            out_line.unscaled_ascent = max_line_asc;
            out_line.hard_break = is_last;
            for (const auto& lw : line_words) {
                for (const auto& g : lw.glyphs) {
                    out_line.glyphs.push_back(g);
                }
            }
            expand_line_metrics_for_placeholders(out_line, line_height);
            apply_line_alignment(out_line, text_, wrap_width, align, base_dir, is_last);
            layout_lines_.push_back(std::move(out_line));
            line_words.clear();
        };

        std::vector<Word> cur_words;
        float cur_line_w = 0.0f;

        for (auto& w : words) {
            if (cur_line_w + w.width > max_w && !cur_words.empty()) {
                if (!w.is_space) {
                    emit_packed_line(cur_words, false);
                    cur_line_w = 0.0f;
                }
            }

            if (w.width > max_w && wrap == Wrap::WordOrGlyph && !w.is_space && !w.is_placeholder) {
                if (!cur_words.empty()) {
                    emit_packed_line(cur_words, false);
                    cur_line_w = 0.0f;
                }
                LayoutLine cur_line;
                cur_line.line_height_opt = line_height;
                cur_line.max_ascent = max_line_asc;
                cur_line.max_descent = max_line_desc;
                cur_line.unscaled_ascent = max_line_asc;
                for (const auto& g : w.glyphs) {
                    if (!g.is_mark && cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                        expand_line_metrics_for_placeholders(cur_line, line_height);
                        apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, false);
                        layout_lines_.push_back(std::move(cur_line));
                        cur_line = LayoutLine{};
                        cur_line.line_height_opt = line_height;
                        cur_line.max_ascent = max_line_asc;
                        cur_line.max_descent = max_line_desc;
                        cur_line.unscaled_ascent = max_line_asc;
                        cur_line_w = 0.0f;
                    }
                    cur_line_w += g.w;
                    cur_line.glyphs.push_back(g);
                }
                if (!cur_line.glyphs.empty()) {
                    Word rem;
                    rem.glyphs = std::move(cur_line.glyphs);
                    rem.width = cur_line_w;
                    rem.is_space = false;
                    rem.level = w.level;
                    rem.min_start = w.min_start;
                    cur_words.push_back(std::move(rem));
                }
            } else {
                cur_line_w += w.width;
                cur_words.push_back(std::move(w));
            }
        }

        if (!cur_words.empty()) {
            emit_packed_line(cur_words, true);
        } else if (!layout_lines_.empty()) {
            layout_lines_.back().hard_break = true;
        }
    }
}

Buffer::Buffer(Metrics metrics) : metrics_(metrics) {}

void Buffer::reset_shaping() noexcept {
    did_exceed_max_lines_ = false;
    layout_dirty_ = true;
    for (auto& line : lines_) {
        line.reset_shaping();
    }
}

void Buffer::set_text(std::string_view text, const Attrs& default_attrs) {
    lines_.clear();
    next_placeholder_index_ = 0;
    did_exceed_max_lines_ = false;
    layout_dirty_ = true;
    size_t line_count = 1;
    for (char c : text) {
        if (c == '\n') ++line_count;
    }
    lines_.reserve(line_count);

    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') {
                end--;
            }
            lines_.emplace_back(text.substr(start, end - start), default_attrs);
            start = i + 1;
        }
    }
    if (start <= text.size()) {
        lines_.emplace_back(text.substr(start), default_attrs);
    }
}

void Buffer::add_text(std::string_view text, const Attrs& attrs) {
    did_exceed_max_lines_ = false;
    layout_dirty_ = true;
    if (lines_.empty()) {
        set_text(text, attrs);
        return;
    }
    size_t start = 0;
    bool first_seg = true;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') {
                --end;
            }
            std::string_view seg = text.substr(start, end - start);
            if (first_seg) {
                lines_.back().append_text(seg, attrs);
                first_seg = false;
            } else {
                lines_.emplace_back(seg, attrs);
            }
            start = i + 1;
        }
    }
}

size_t Buffer::add_placeholder(
    float width,
    float height,
    PlaceholderAlignment alignment,
    TextBaseline baseline,
    float baseline_offset
) {
    return add_placeholder(PlaceholderStyle{width, height, alignment, baseline, baseline_offset});
}

size_t Buffer::add_placeholder(const PlaceholderStyle& style) {
    did_exceed_max_lines_ = false;
    layout_dirty_ = true;
    if (lines_.empty()) {
        lines_.emplace_back("", Attrs{});
    }
    const size_t idx = next_placeholder_index_++;
    const Attrs& def_attrs = lines_.back().attrs_list().defaults();
    lines_.back().append_placeholder(idx, style, def_attrs);
    return idx;
}

std::vector<PlaceholderRect> Buffer::placeholder_rects(float origin_x, float origin_y) const {
    std::vector<PlaceholderRect> out;
    const auto runs = layout_runs();
    for (size_t line_no = 0; line_no < runs.size(); ++line_no) {
        const auto& run = runs[line_no];
        const float baseline_offset = (run.max_ascent > 0.0f) ? run.max_ascent : metrics_.font_size;
        const float line_top = origin_y + run.line_top;
        const float alpha_baseline = line_top + baseline_offset;
        const float ideo_baseline = alpha_baseline + run.max_descent;

        for (const auto& g : run.glyphs) {
            if (!g.is_placeholder) continue;
            const float ph_w = std::max(0.0f, g.w);
            const float ph_h = std::max(0.0f, g.placeholder_height);
            float ph_y = alpha_baseline - ph_h;
            switch (g.placeholder_alignment) {
                case PlaceholderAlignment::Baseline:
                    ph_y = ((g.placeholder_baseline == TextBaseline::Ideographic) ? ideo_baseline : alpha_baseline)
                           - g.placeholder_baseline_offset;
                    break;
                case PlaceholderAlignment::AboveBaseline:
                    ph_y = alpha_baseline - ph_h;
                    break;
                case PlaceholderAlignment::BelowBaseline:
                    ph_y = alpha_baseline;
                    break;
                case PlaceholderAlignment::Top:
                    ph_y = line_top;
                    break;
                case PlaceholderAlignment::Bottom:
                    ph_y = line_top + run.line_height - ph_h;
                    break;
                case PlaceholderAlignment::Middle:
                    ph_y = line_top + (run.line_height - ph_h) * 0.5f;
                    break;
            }
            const float ph_x = origin_x + g.x;
            Rect r = Rect::from_xywh(ph_x, ph_y, std::max(ph_w, 0.001f), std::max(ph_h, 0.001f)).value_or(Rect{});
            PlaceholderRect pr;
            pr.rect = r;
            pr.direction = ((g.level & 1) != 0) ? TextDirection::RTL : TextDirection::LTR;
            pr.placeholder_index = g.placeholder_index;
            pr.line_number = line_no;
            out.push_back(pr);
        }
    }
    std::sort(out.begin(), out.end(), [](const PlaceholderRect& a, const PlaceholderRect& b) {
        return a.placeholder_index < b.placeholder_index;
    });
    return out;
}

void Buffer::apply_ellipsis_truncation(
    FontSystem& font_system,
    LayoutLine& line,
    const BufferLine& src_line,
    float max_width
) const {
    if (line.glyphs.empty()) return;

    const Direction base_dir = src_line.resolved_base_direction();
    const bool is_rtl = (base_dir == Direction::RightToLeft);
    const LayoutGlyph proto = is_rtl ? line.glyphs.front() : line.glyphs.back();

    std::vector<LayoutGlyph> ell_glyphs;
    float ellipsis_w = 0.0f;

    if (ellipsis_opt_.has_value()) {
        if (!ellipsis_opt_->empty()) {
            Attrs ell_attrs = src_line.attrs_list().defaults();
            ell_attrs.set_font_size(proto.font_size)
                     .set_weight(proto.font_weight)
                     .set_underline(proto.underline)
                     .set_strikethrough(proto.strikethrough)
                     .set_overline(proto.overline)
                     .set_decoration_style(proto.decoration_style)
                     .set_decoration_thickness(proto.decoration_thickness)
                     .set_shadows(proto.shadows);
            if (proto.color_opt.has_value()) {
                ell_attrs.set_color(*proto.color_opt);
            }
            if (proto.background_color_opt.has_value()) {
                ell_attrs.set_background_color(*proto.background_color_opt);
            }
            if (proto.decoration_color_opt.has_value()) {
                ell_attrs.set_decoration_color(*proto.decoration_color_opt);
            }

            BufferLine ell_line(*ellipsis_opt_, ell_attrs);
            const float lh = line.line_height_opt.value_or(metrics_.line_height);
            const TextDirection ell_dir = is_rtl ? TextDirection::RTL : TextDirection::LTR;
            ell_line.shape_and_layout(
                font_system,
                proto.font_size,
                lh,
                std::nullopt,
                Wrap::None,
                Align::Left,
                shaping_,
                enable_kerning_,
                enable_ligatures_,
                ell_dir
            );
            if (!ell_line.layout().empty()) {
                const auto& el = ell_line.layout().front();
                ell_glyphs = el.glyphs;
                ellipsis_w = el.w;
                line.max_ascent = std::max(line.max_ascent, el.max_ascent);
                line.max_descent = std::max(line.max_descent, el.max_descent);
                line.unscaled_ascent = std::max(line.unscaled_ascent, el.unscaled_ascent);
            }
        }
    } else {
        const TtfFont* font = font_system.get_font(proto.font_id);
        if (!font && font_system.default_font_id().has_value()) {
            font = font_system.get_font(*font_system.default_font_id());
        }

        uint32_t ell_fid = proto.font_id;
        uint16_t ellipsis_gid = font ? font->glyph_index(0x2026) : 0; // '…'
        if (ellipsis_gid == 0 && font_system.font_count() > 1) {
            auto [fb_fid, fb_gid] = font_system.find_glyph_or_fallback(proto.font_id, 0x2026);
            if (fb_gid != 0) {
                ell_fid = fb_fid;
                ellipsis_gid = fb_gid;
                font = font_system.get_font(ell_fid);
            }
        }
        uint16_t dot_gid = font ? font->glyph_index('.') : 0;
        bool use_single_ellipsis = (ellipsis_gid != 0);

        if (use_single_ellipsis && font) {
            ellipsis_w = font->glyph_advance(ellipsis_gid, proto.font_size);
            LayoutGlyph eg = proto;
            eg.font_id = ell_fid;
            eg.glyph_id = ellipsis_gid;
            eg.w = ellipsis_w;
            eg.x_offset = 0.0f;
            eg.y_offset = 0.0f;
            eg.is_mark = false;
            ell_glyphs.push_back(std::move(eg));
        } else if (dot_gid != 0 && font) {
            float dot_w = font->glyph_advance(dot_gid, proto.font_size);
            ellipsis_w = dot_w * 3.0f;
            for (int d = 0; d < 3; ++d) {
                LayoutGlyph dg = proto;
                dg.font_id = ell_fid;
                dg.glyph_id = dot_gid;
                dg.w = dot_w;
                dg.x_offset = 0.0f;
                dg.y_offset = 0.0f;
                dg.is_mark = false;
                ell_glyphs.push_back(std::move(dg));
            }
        } else {
            ellipsis_w = proto.font_size * 0.8f;
            LayoutGlyph eg = proto;
            eg.glyph_id = 0;
            eg.w = ellipsis_w;
            eg.x_offset = 0.0f;
            eg.y_offset = 0.0f;
            eg.is_mark = false;
            ell_glyphs.push_back(std::move(eg));
        }
    }

    const float target_w = std::max(0.0f, max_width - ellipsis_w);
    const std::string_view src_text = src_line.text();

    float cur_w = 0.0f;
    for (const auto& g : line.glyphs) {
        cur_w += g.w;
    }

    size_t trunc_byte = 0;
    if (!is_rtl) {
        while (!line.glyphs.empty() && (cur_w > target_w || line.glyphs.back().is_mark)) {
            cur_w -= line.glyphs.back().w;
            line.glyphs.pop_back();
        }
        // Trim trailing whitespace before appending ellipsis
        while (!line.glyphs.empty()) {
            const auto& bk = line.glyphs.back();
            if (!bk.is_mark && bk.start < src_text.size() &&
                (src_text[bk.start] == ' ' || src_text[bk.start] == '\t')) {
                cur_w -= bk.w;
                line.glyphs.pop_back();
            } else {
                break;
            }
        }
        trunc_byte = line.glyphs.empty() ? 0 : line.glyphs.back().end;
        for (auto& eg : ell_glyphs) {
            eg.start = trunc_byte;
            eg.end = trunc_byte;
            line.glyphs.push_back(std::move(eg));
        }
    } else {
        // In RTL visual order, logical start is on the right (back) and logical end is on the left (front).
        size_t keep_from = 0;
        const size_t total = line.glyphs.size();
        while (keep_from < total && (cur_w > target_w || line.glyphs[keep_from].is_mark)) {
            cur_w -= line.glyphs[keep_from].w;
            ++keep_from;
        }
        // Drop any marks orphaned immediately after a dropped base glyph
        while (keep_from < total && line.glyphs[keep_from].is_mark) {
            cur_w -= line.glyphs[keep_from].w;
            ++keep_from;
        }
        // Trim visual-left (logical trailing) spaces before placing RTL ellipsis on the left
        while (keep_from < total) {
            const auto& fr = line.glyphs[keep_from];
            if (!fr.is_mark && fr.start < src_text.size() &&
                (src_text[fr.start] == ' ' || src_text[fr.start] == '\t')) {
                cur_w -= fr.w;
                ++keep_from;
            } else {
                break;
            }
        }
        if (keep_from > 0) {
            line.glyphs.erase(line.glyphs.begin(), line.glyphs.begin() + static_cast<ptrdiff_t>(keep_from));
        }
        for (const auto& g : line.glyphs) {
            trunc_byte = std::max(trunc_byte, g.end);
        }
        for (auto& eg : ell_glyphs) {
            eg.start = trunc_byte;
            eg.end = trunc_byte;
        }
        line.glyphs.insert(line.glyphs.begin(), ell_glyphs.begin(), ell_glyphs.end());
    }

    line.is_ellipsized = true;
    apply_line_alignment(line, src_text, width_, align_, base_dir, true);
}

void Buffer::apply_vertical_line_metrics(const FontSystem* font_system) {
    const bool apply_strut = strut_style_.enabled;
    const bool apply_half = half_leading_;
    const bool trim_first = !text_height_behavior_.apply_height_to_first_ascent;
    const bool trim_last = !text_height_behavior_.apply_height_to_last_descent;
    if (!apply_strut && !apply_half && !trim_first && !trim_last) {
        return;
    }

    float strut_asc = cached_strut_asc_;
    float strut_desc = cached_strut_desc_;
    if (apply_strut && (font_system != nullptr || !has_cached_strut_)) {
        const float s_size = strut_style_.font_size.value_or(metrics_.font_size);
        const TtfFont* s_font = nullptr;
        if (font_system != nullptr) {
            auto sm = strut_style_.font_families.empty()
                ? font_system->match_font_styled(Family::sans_serif(), strut_style_.weight, strut_style_.style)
                : font_system->match_font_chain_styled(strut_style_.font_families, Family::sans_serif(), strut_style_.weight, strut_style_.style, Stretch::Normal);
            s_font = sm.font;
        }
        const float raw_asc = s_font ? s_font->ascent(s_size) : (s_size * 0.8f);
        const float raw_desc = s_font ? -s_font->descent(s_size) : (s_size * 0.2f);
        const float raw_gap = s_font ? std::max(0.0f, s_font->line_gap(s_size)) : 0.0f;
        const float raw_h = raw_asc + raw_desc;
        const float mult = (strut_style_.height_multiplier > 0.0f) ? strut_style_.height_multiplier : 1.0f;
        const float extra_lead = (strut_style_.leading >= 0.0f) ? (strut_style_.leading * s_size) : raw_gap;
        const float target_h = raw_h * mult + extra_lead;
        if (strut_style_.half_leading || apply_half) {
            const float half = (target_h - raw_h) * 0.5f;
            strut_asc = raw_asc + half;
            strut_desc = raw_desc + half;
        } else {
            strut_asc = raw_asc * mult;
            strut_desc = raw_desc * mult + extra_lead;
        }
        cached_strut_asc_ = strut_asc;
        cached_strut_desc_ = strut_desc;
        has_cached_strut_ = true;
    }

    LayoutLine* first_ll = nullptr;
    LayoutLine* last_ll = nullptr;
    for (auto& line : lines_) {
        for (auto& ll : line.layout_mut()) {
            if (ll.unscaled_ascent <= 0.0f) ll.unscaled_ascent = ll.max_ascent;
            if (ll.unscaled_descent <= 0.0f) ll.unscaled_descent = ll.max_descent;

            if (apply_strut) {
                if (strut_style_.force_strut_height) {
                    ll.max_ascent = strut_asc;
                    ll.max_descent = strut_desc;
                    ll.line_height_opt = strut_asc + strut_desc;
                } else {
                    ll.max_ascent = std::max(ll.max_ascent, strut_asc);
                    ll.max_descent = std::max(ll.max_descent, strut_desc);
                    const float cur_lh = ll.line_height_opt.value_or(metrics_.line_height);
                    ll.line_height_opt = std::max(cur_lh, ll.max_ascent + ll.max_descent);
                }
            } else if (apply_half) {
                const float cur_lh = ll.line_height_opt.value_or(metrics_.line_height);
                const float raw_h = ll.unscaled_ascent + ll.unscaled_descent;
                if (cur_lh > raw_h) {
                    const float half = (cur_lh - raw_h) * 0.5f;
                    ll.max_ascent = ll.unscaled_ascent + half;
                    ll.max_descent = ll.unscaled_descent + half;
                }
            }

            if (!first_ll) first_ll = &ll;
            last_ll = &ll;
        }
    }

    if (first_ll && trim_first) {
        const float extra_top = first_ll->max_ascent - first_ll->unscaled_ascent;
        if (extra_top > 0.0f) {
            const float cur_lh = first_ll->line_height_opt.value_or(metrics_.line_height);
            first_ll->max_ascent = first_ll->unscaled_ascent;
            first_ll->line_height_opt = std::max(first_ll->max_ascent + first_ll->max_descent, cur_lh - extra_top);
        }
    }
    if (last_ll && trim_last) {
        const float cur_lh = last_ll->line_height_opt.value_or(metrics_.line_height);
        const float cur_bottom = cur_lh - last_ll->max_ascent;
        const float extra_bot = cur_bottom - last_ll->unscaled_descent;
        if (extra_bot > 0.0f) {
            last_ll->max_descent = last_ll->unscaled_descent;
            last_ll->line_height_opt = std::max(1.0f, last_ll->max_ascent + last_ll->unscaled_descent);
        }
    }
}

void Buffer::shape_until_scroll(FontSystem& font_system) {
    bool any_unshaped = false;
    for (const auto& line : lines_) {
        if (!line.is_shaped()) {
            any_unshaped = true;
            break;
        }
    }
    if (!layout_dirty_ && !any_unshaped) {
        return;
    }

    ++shape_count_;
    did_exceed_max_lines_ = false;
    has_cached_strut_ = false;
    for (auto& line : lines_) {
        if (!line.is_shaped()) {
            line.shape_and_layout(
                font_system,
                metrics_.font_size,
                metrics_.line_height,
                width_,
                wrap_,
                align_,
                shaping_,
                enable_kerning_,
                enable_ligatures_,
                text_direction_
            );
        }
    }

    // Apply StrutStyle, HalfLeading, and TextHeightBehavior across visual lines
    apply_vertical_line_metrics(&font_system);

    // Handle single-line Wrap::None overflow with Ellipsis
    if (overflow_ == Overflow::Ellipsis && width_.has_value() && wrap_ == Wrap::None) {
        for (auto& line : lines_) {
            for (auto& lline : line.layout_mut()) {
                if (lline.w > *width_) {
                    did_exceed_max_lines_ = true;
                    apply_ellipsis_truncation(font_system, lline, line, *width_);
                }
            }
        }
    }

    // Handle max_lines or height constraint with Clip / Ellipsis
    size_t max_visual_lines = max_lines_.value_or(SIZE_MAX);
    if (height_.has_value() && metrics_.line_height > 0.0f && overflow_ != Overflow::Visible) {
        size_t fit_lines = static_cast<size_t>(std::max(1.0f, std::floor(*height_ / metrics_.line_height)));
        max_visual_lines = std::min(max_visual_lines, fit_lines);
    }

    if (max_visual_lines < SIZE_MAX) {
        size_t total_visual = 0;
        for (const auto& line : lines_) {
            total_visual += line.layout().size();
        }

        if (total_visual > max_visual_lines) {
            did_exceed_max_lines_ = true;
            size_t seen = 0;
            for (auto& line : lines_) {
                auto& llines = line.layout_mut();
                if (seen >= max_visual_lines) {
                    llines.clear();
                    continue;
                }
                if (seen + llines.size() > max_visual_lines) {
                    size_t keep = max_visual_lines - seen;
                    llines.resize(keep);
                    if (!llines.empty()) {
                        llines.back().hard_break = false;
                        if (overflow_ == Overflow::Ellipsis) {
                            float max_w = width_.value_or(llines.back().w + metrics_.font_size * 2.0f);
                            apply_ellipsis_truncation(font_system, llines.back(), line, max_w);
                        }
                    }
                    seen = max_visual_lines;
                } else {
                    seen += llines.size();
                    if (seen == max_visual_lines && overflow_ == Overflow::Ellipsis && !llines.empty()) {
                        float max_w = width_.value_or(llines.back().w + metrics_.font_size * 2.0f);
                        apply_ellipsis_truncation(font_system, llines.back(), line, max_w);
                    }
                }
            }
        }
    }

    layout_dirty_ = false;
}

void Buffer::update_text_align(Align align) noexcept {
    align_ = align;
    if (layout_dirty_) {
        return;
    }
    for (auto& line : lines_) {
        if (!line.is_shaped()) continue;
        const Direction base_dir = line.resolved_base_direction();
        for (auto& ll : line.layout_mut()) {
            apply_line_alignment(ll, line.text(), width_, align_, base_dir, ll.hard_break || ll.is_ellipsized);
        }
    }
}

void Buffer::update_foreground_paint(size_t start, size_t end, const Paint& paint) {
    if (start >= end || lines_.empty()) return;
    auto paint_ptr = std::make_shared<const Paint>(paint);
    size_t line_base = 0;
    for (auto& line : lines_) {
        const size_t line_len = line.text().size();
        const size_t line_end = line_base + line_len;
        if (end > line_base && start < line_end) {
            const size_t local_start = (start > line_base) ? (start - line_base) : 0;
            const size_t local_end = std::min(line_len, end - line_base);
            line.attrs_list_mut().apply_range_modifier(local_start, local_end, [&](Attrs& a) {
                a.foreground_paint_opt = paint_ptr;
                a.foreground_stroke_opt.reset();
            });
            if (!layout_dirty_ && line.is_shaped()) {
                for (auto& ll : line.layout_mut()) {
                    for (auto& g : ll.glyphs) {
                        if (!g.is_placeholder && g.end > local_start && g.start < local_end) {
                            g.foreground_paint_opt = paint_ptr;
                            g.foreground_stroke_opt.reset();
                        }
                    }
                }
            }
        }
        line_base += line_len + 1;
    }
}

void Buffer::update_foreground_paint(size_t start, size_t end, const Paint& paint, const Stroke& stroke) {
    if (start >= end || lines_.empty()) return;
    auto paint_ptr = std::make_shared<const Paint>(paint);
    auto stroke_ptr = std::make_shared<const Stroke>(stroke);
    size_t line_base = 0;
    for (auto& line : lines_) {
        const size_t line_len = line.text().size();
        const size_t line_end = line_base + line_len;
        if (end > line_base && start < line_end) {
            const size_t local_start = (start > line_base) ? (start - line_base) : 0;
            const size_t local_end = std::min(line_len, end - line_base);
            line.attrs_list_mut().apply_range_modifier(local_start, local_end, [&](Attrs& a) {
                a.foreground_paint_opt = paint_ptr;
                a.foreground_stroke_opt = stroke_ptr;
            });
            if (!layout_dirty_ && line.is_shaped()) {
                for (auto& ll : line.layout_mut()) {
                    for (auto& g : ll.glyphs) {
                        if (!g.is_placeholder && g.end > local_start && g.start < local_end) {
                            g.foreground_paint_opt = paint_ptr;
                            g.foreground_stroke_opt = stroke_ptr;
                        }
                    }
                }
            }
        }
        line_base += line_len + 1;
    }
}

void Buffer::update_background_paint(size_t start, size_t end, const Paint& paint) {
    if (start >= end || lines_.empty()) return;
    auto bg_ptr = std::make_shared<const Paint>(paint);
    size_t line_base = 0;
    for (auto& line : lines_) {
        const size_t line_len = line.text().size();
        const size_t line_end = line_base + line_len;
        if (end > line_base && start < line_end) {
            const size_t local_start = (start > line_base) ? (start - line_base) : 0;
            const size_t local_end = std::min(line_len, end - line_base);
            line.attrs_list_mut().apply_range_modifier(local_start, local_end, [&](Attrs& a) {
                a.background_paint_opt = bg_ptr;
            });
            if (!layout_dirty_ && line.is_shaped()) {
                for (auto& ll : line.layout_mut()) {
                    for (auto& g : ll.glyphs) {
                        if (g.end > local_start && g.start < local_end) {
                            g.background_paint_opt = bg_ptr;
                        }
                    }
                }
            }
        }
        line_base += line_len + 1;
    }
}

void Buffer::update_font_size(size_t start, size_t end, float font_size) {
    if (start >= end || font_size <= 0.0f || lines_.empty()) return;
    bool any_line_modified = false;
    size_t line_base = 0;

    for (auto& line : lines_) {
        const size_t line_len = line.text().size();
        const size_t line_end = line_base + line_len;
        if (end > line_base && start < line_end) {
            const size_t local_start = (start > line_base) ? (start - line_base) : 0;
            const size_t local_end = std::min(line_len, end - line_base);
            line.attrs_list_mut().apply_range_modifier(local_start, local_end, [&](Attrs& a) {
                a.font_size_opt = font_size;
            });

            if (!layout_dirty_ && line.is_shaped()) {
                const Direction base_dir = line.resolved_base_direction();
                for (auto& ll : line.layout_mut()) {
                    bool touches_line = false;
                    for (const auto& g : ll.glyphs) {
                        if (!g.is_placeholder && g.end > local_start && g.start < local_end) {
                            touches_line = true;
                            break;
                        }
                    }
                    if (!touches_line) continue;

                    // Undo any justification expansion before scaling advances
                    for (auto& g : ll.glyphs) {
                        if (g.justified_extra_w != 0.0f) {
                            g.w = std::max(0.0f, g.w - g.justified_extra_w);
                            g.justified_extra_w = 0.0f;
                        }
                    }

                    for (auto& g : ll.glyphs) {
                        if (g.is_placeholder) continue;
                        if (g.end > local_start && g.start < local_end) {
                            const float old_sz = (g.font_size > 0.0f) ? g.font_size : metrics_.font_size;
                            const float ratio = (old_sz > 0.0f) ? (font_size / old_sz) : 1.0f;
                            g.font_size = font_size;
                            g.w *= ratio;
                            g.x_offset *= ratio;
                            g.y_offset *= ratio;
                            g.ascent = ((g.ascent > 0.0f) ? g.ascent : (old_sz * 0.8f)) * ratio;
                            g.descent = ((g.descent > 0.0f) ? g.descent : (old_sz * 0.2f)) * ratio;
                        }
                    }

                    float max_asc = 0.0f;
                    float max_desc = 0.0f;
                    for (const auto& g : ll.glyphs) {
                        if (g.is_placeholder) continue;
                        const float ga = (g.ascent > 0.0f) ? g.ascent : (g.font_size * 0.8f);
                        const float gd = (g.descent > 0.0f) ? g.descent : (g.font_size * 0.2f);
                        max_asc = std::max(max_asc, ga);
                        max_desc = std::max(max_desc, gd);
                    }
                    if (max_asc <= 0.0f) max_asc = metrics_.font_size * 0.8f;
                    if (max_desc <= 0.0f) max_desc = metrics_.font_size * 0.2f;

                    ll.max_ascent = max_asc;
                    ll.max_descent = max_desc;
                    ll.unscaled_ascent = max_asc;
                    ll.unscaled_descent = max_desc;
                    ll.line_height_opt = std::max(metrics_.line_height, max_asc + max_desc);
                    expand_line_metrics_for_placeholders(ll, metrics_.line_height);
                    apply_line_alignment(ll, line.text(), width_, align_, base_dir, ll.hard_break || ll.is_ellipsized);
                    any_line_modified = true;
                }
            }
        }
        line_base += line_len + 1;
    }

    if (any_line_modified) {
        apply_vertical_line_metrics(nullptr);
    }
}

void Buffer::copy_shaped_layout_from(const Buffer& cached) {
    if (lines_.size() != cached.lines_.size()) {
        return;
    }
    for (size_t i = 0; i < lines_.size(); ++i) {
        lines_[i].copy_shaped_state_from(cached.lines_[i]);
    }
    did_exceed_max_lines_ = cached.did_exceed_max_lines_;
    cached_strut_asc_ = cached.cached_strut_asc_;
    cached_strut_desc_ = cached.cached_strut_desc_;
    has_cached_strut_ = cached.has_cached_strut_;
    layout_dirty_ = cached.layout_dirty_;
}

void Buffer::visit(
    const GlyphRunVisitor& visitor,
    const FontSystem* font_system,
    float origin_x,
    float origin_y
) const {
    if (!visitor) return;

    std::vector<uint16_t> glyph_ids;
    std::vector<Point> positions;
    std::vector<float> advances;
    std::vector<uint32_t> clusters;

    size_t line_number = 0;
    size_t line_base = 0;
    float current_top = 0.0f;

    for (size_t line_i = 0; line_i < lines_.size(); ++line_i) {
        const auto& bl = lines_[line_i];
        const std::string_view text = bl.text();

        for (const auto& ll : bl.layout()) {
            const float lh = ll.line_height_opt.value_or(metrics_.line_height);
            const float asc = (ll.max_ascent > 0.0f) ? ll.max_ascent : metrics_.font_size;
            const float desc = (ll.max_descent > 0.0f) ? ll.max_descent : (metrics_.font_size * 0.2f);
            const float ltop = origin_y + current_top;
            const float lleft = origin_x + ll.left;
            const Point line_origin = Point::from_xy(lleft, ltop + asc);

            if (ll.glyphs.empty()) {
                GlyphRunInfo info;
                info.line_number = line_number;
                info.paragraph_line = line_i;
                info.font_size = metrics_.font_size;
                info.origin = line_origin;
                info.line_top = ltop;
                info.line_left = lleft;
                info.line_height = lh;
                info.line_width = ll.w;
                info.baseline = asc;
                info.ascent = asc;
                info.descent = desc;
                info.utf8_text = text;
                info.text_byte_offset = line_base;
                visitor(info);
            } else {
                const size_t n = ll.glyphs.size();
                size_t run_start = 0;
                while (run_start < n) {
                    const auto& first = ll.glyphs[run_start];
                    size_t run_end = run_start + 1;
                    if (!first.is_placeholder) {
                        const bool first_rtl = (first.level & 1) != 0;
                        while (run_end < n) {
                            const auto& cur = ll.glyphs[run_end];
                            if (cur.is_placeholder ||
                                cur.font_id != first.font_id ||
                                cur.font_size != first.font_size ||
                                cur.font_weight != first.font_weight ||
                                cur.synthetic_bold != first.synthetic_bold ||
                                cur.synthetic_italic != first.synthetic_italic ||
                                ((cur.level & 1) != 0) != first_rtl ||
                                cur.variation_hash != first.variation_hash) {
                                break;
                            }
                            ++run_end;
                        }
                    }

                    const size_t run_len = run_end - run_start;
                    glyph_ids.clear();
                    positions.clear();
                    advances.clear();
                    clusters.clear();
                    glyph_ids.reserve(run_len);
                    positions.reserve(run_len);
                    advances.reserve(run_len);
                    clusters.reserve(run_len);

                    float run_adv = 0.0f;
                    for (size_t k = run_start; k < run_end; ++k) {
                        const auto& g = ll.glyphs[k];
                        glyph_ids.push_back(g.glyph_id);
                        positions.push_back(Point::from_xy(
                            (origin_x + g.x + g.x_offset) - line_origin.x,
                            g.y + g.y_offset
                        ));
                        advances.push_back(g.w);
                        clusters.push_back(static_cast<uint32_t>(line_base + g.start));
                        run_adv += g.w;
                    }

                    GlyphRunInfo info;
                    info.line_number = line_number;
                    info.paragraph_line = line_i;
                    info.font_id = first.font_id;
                    info.font = (!first.is_placeholder && font_system) ? font_system->get_font(first.font_id) : nullptr;
                    info.font_size = first.font_size;
                    info.font_weight = first.font_weight;
                    info.synthetic_bold = first.synthetic_bold;
                    info.synthetic_italic = first.synthetic_italic;
                    info.is_rtl = (first.level & 1) != 0;
                    info.is_placeholder = first.is_placeholder;
                    info.placeholder_index = first.placeholder_index;
                    info.origin = line_origin;
                    info.line_top = ltop;
                    info.line_left = lleft;
                    info.line_height = lh;
                    info.line_width = ll.w;
                    info.baseline = asc;
                    info.ascent = asc;
                    info.descent = desc;
                    info.advance_width = run_adv;
                    info.utf8_text = text;
                    info.text_byte_offset = line_base;
                    info.glyph_ids = glyph_ids;
                    info.positions = positions;
                    info.advances = advances;
                    info.clusters = clusters;
                    info.glyphs = std::span<const LayoutGlyph>(ll.glyphs.data() + run_start, run_len);
                    visitor(info);

                    run_start = run_end;
                }
            }

            current_top += lh;
            ++line_number;
        }
        line_base += text.size() + 1;
    }
}

void Buffer::visit(
    const FontSystem& font_system,
    const GlyphRunVisitor& visitor,
    float origin_x,
    float origin_y
) const {
    visit(visitor, &font_system, origin_x, origin_y);
}

std::vector<LayoutRun> Buffer::layout_runs() const {
    std::vector<LayoutRun> runs;
    runs.reserve(lines_.size());
    float current_top = 0.0f;

    for (size_t line_i = 0; line_i < lines_.size(); ++line_i) {
        const auto& line = lines_[line_i];
        for (const auto& layout_line : line.layout()) {
            float lh = layout_line.line_height_opt.value_or(metrics_.line_height);
            runs.push_back(LayoutRun{
                line_i,
                line.text(),
                layout_line.glyphs,
                current_top,
                lh,
                layout_line.w,
                layout_line.max_ascent,
                layout_line.max_descent
            });
            current_top += lh;
        }
    }
    return runs;
}

float Buffer::total_height() const {
    float h = 0.0f;
    for (const auto& line : lines_) {
        for (const auto& layout_line : line.layout()) {
            h += layout_line.line_height_opt.value_or(metrics_.line_height);
        }
    }
    return h;
}

float Buffer::max_line_width() const {
    float max_w = 0.0f;
    for (const auto& line : lines_) {
        for (const auto& layout_line : line.layout()) {
            max_w = std::max(max_w, layout_line.w);
        }
    }
    return max_w;
}

float Buffer::max_intrinsic_width(FontSystem& font_system) const {
    if ((!width_.has_value() || (wrap_ == Wrap::None && overflow_ != Overflow::Ellipsis)) && !did_exceed_max_lines_) {
        bool all_shaped = true;
        for (const auto& line : lines_) {
            if (!line.is_shaped()) {
                all_shaped = false;
                break;
            }
        }
        if (all_shaped) {
            return max_line_width();
        }
    }
    float max_w = 0.0f;
    for (const auto& line : lines_) {
        BufferLine probe = line;
        probe.shape_and_layout(
            font_system,
            metrics_.font_size,
            metrics_.line_height,
            std::nullopt,
            Wrap::None,
            Align::Left,
            shaping_,
            enable_kerning_,
            enable_ligatures_,
            text_direction_
        );
        for (const auto& ll : probe.layout()) {
            max_w = std::max(max_w, ll.w);
        }
    }
    return max_w;
}

float Buffer::min_intrinsic_width(FontSystem& font_system) const {
    float min_w = 0.0f;
    for (const auto& line : lines_) {
        if (line.text().empty()) continue;

        BufferLine probe = line;
        probe.shape_and_layout(
            font_system,
            metrics_.font_size,
            metrics_.line_height,
            std::nullopt,
            Wrap::None,
            Align::Left,
            shaping_,
            enable_kerning_,
            enable_ligatures_,
            text_direction_
        );

        const std::string_view text = line.text();
        for (const auto& ll : probe.layout()) {
            if (wrap_ == Wrap::None) {
                min_w = std::max(min_w, ll.w);
                continue;
            }
            if (wrap_ == Wrap::Glyph) {
                float cluster_w = 0.0f;
                bool has_cluster = false;
                for (const auto& g : ll.glyphs) {
                    if (g.is_mark) {
                        cluster_w += g.w;
                    } else {
                        if (has_cluster) {
                            min_w = std::max(min_w, cluster_w);
                        }
                        cluster_w = g.w;
                        has_cluster = true;
                    }
                }
                if (has_cluster) {
                    min_w = std::max(min_w, cluster_w);
                }
                continue;
            }

            // Word / WordOrGlyph: find the widest unbreakable word (respecting CJK Kinsoku & combining marks)
            float cur_word_w = 0.0f;
            bool cur_is_space = false;
            bool has_word = false;
            bool prev_was_cjk = false;
            bool prev_no_break_after = false;

            for (const auto& g : ll.glyphs) {
                if (g.is_placeholder) {
                    if (has_word && !cur_is_space) {
                        min_w = std::max(min_w, cur_word_w);
                    }
                    min_w = std::max(min_w, g.w);
                    cur_word_w = 0.0f;
                    has_word = false;
                    prev_was_cjk = false;
                    prev_no_break_after = false;
                    continue;
                }
                if (g.is_mark && has_word) {
                    cur_word_w += g.w;
                    continue;
                }
                char32_t cp = (g.start < text.size()) ? decode_codepoint_at(text, g.start) : 0;
                bool is_sp = (cp == ' ' || cp == '\t');
                bool is_cjk_char = Bidi::is_cjk(cp);
                bool no_break_before = is_cjk_char && Bidi::is_cjk_no_break_before(cp);
                bool no_break_after = is_cjk_char && Bidi::is_cjk_no_break_after(cp);

                bool should_split = false;
                if (has_word) {
                    if (cur_is_space != is_sp) {
                        should_split = true;
                    } else if (!is_sp) {
                        if (is_cjk_char) {
                            if (!no_break_before && !prev_no_break_after) should_split = true;
                        } else if (prev_was_cjk && !no_break_before) {
                            should_split = true;
                        }
                    }
                }

                if (should_split) {
                    if (!cur_is_space) {
                        min_w = std::max(min_w, cur_word_w);
                    }
                    cur_word_w = 0.0f;
                }

                has_word = true;
                cur_is_space = is_sp;
                cur_word_w += g.w;
                prev_was_cjk = is_cjk_char;
                prev_no_break_after = no_break_after;
            }
            if (has_word && !cur_is_space) {
                min_w = std::max(min_w, cur_word_w);
            }
        }
    }
    return min_w;
}

std::vector<LineMetrics> Buffer::line_metrics() const {
    std::vector<LineMetrics> result;
    size_t line_base = 0;
    float current_top = 0.0f;
    size_t line_number = 0;

    for (size_t i = 0; i < lines_.size(); ++i) {
        const auto& bl = lines_[i];
        for (const auto& ll : bl.layout()) {
            const float lh = ll.line_height_opt.value_or(metrics_.line_height);
            const float asc = ll.max_ascent;
            const float desc = ll.max_descent;
            const float unscaled_asc = (ll.unscaled_ascent > 0.0f) ? ll.unscaled_ascent : asc;
            const float baseline_offset = (asc > 0.0f) ? asc : metrics_.font_size;

            LineMetrics lm;
            lm.start_index = line_base + ll.start_byte;
            lm.end_index = line_base + ll.end_byte;
            lm.ascent = asc;
            lm.descent = desc;
            lm.unscaled_ascent = unscaled_asc;
            lm.height = lh;
            lm.width = ll.w;
            lm.left = ll.left;
            lm.baseline = current_top + baseline_offset;
            lm.hard_break = ll.hard_break;
            lm.line_number = line_number++;
            result.push_back(lm);

            current_top += lh;
        }
        line_base += bl.text().size() + 1;
    }
    return result;
}

float Buffer::alphabetic_baseline() const noexcept {
    for (const auto& line : lines_) {
        if (!line.layout().empty()) {
            const auto& first = line.layout().front();
            return (first.max_ascent > 0.0f) ? first.max_ascent : metrics_.font_size;
        }
    }
    return 0.0f;
}

float Buffer::ideographic_baseline() const noexcept {
    for (const auto& line : lines_) {
        if (!line.layout().empty()) {
            const auto& first = line.layout().front();
            const float alpha = (first.max_ascent > 0.0f) ? first.max_ascent : metrics_.font_size;
            return alpha + first.max_descent;
        }
    }
    return 0.0f;
}

size_t Buffer::unresolved_glyphs() const noexcept {
    size_t missing = 0;
    for (const auto& line : lines_) {
        const std::string_view text = line.text();
        for (const auto& ll : line.layout()) {
            for (const auto& g : ll.glyphs) {
                if (g.is_placeholder) continue;
                if (g.glyph_id == 0) {
                    if (g.end > g.start && g.start < text.size()) {
                        char32_t cp = decode_codepoint_at(text, g.start);
                        if (!is_ignorable_for_unresolved(cp)) {
                            ++missing;
                        }
                    } else if (g.start == g.end) {
                        ++missing;
                    }
                }
            }
        }
    }
    return missing;
}

std::vector<Rect> Buffer::range_rects(
    size_t start,
    size_t end,
    float origin_x,
    float origin_y,
    RectHeightStyle height_style
) const {
    std::vector<Rect> rects;
    if (start >= end || lines_.empty()) {
        return rects;
    }

    std::vector<size_t> line_offsets(lines_.size(), 0);
    for (size_t i = 1; i < lines_.size(); ++i) {
        line_offsets[i] = line_offsets[i - 1] + lines_[i - 1].text().size() + 1;
    }

    auto push_rect = [&](float x, float y, float w, float h) {
        if (auto r = Rect::from_xywh(x, y, std::max(0.5f, w), std::max(0.5f, h))) {
            rects.push_back(*r);
        }
    };

    for (const auto& run : layout_runs()) {
        if (run.line_i >= lines_.size()) continue;
        const size_t base = line_offsets[run.line_i];
        const size_t line_end_global = base + lines_[run.line_i].text().size();
        if (end <= base || start > line_end_global) continue;

        const size_t local_start = (start > base) ? (start - base) : 0;
        const size_t local_end = (end > base) ? (end - base) : 0;

        float box_y = origin_y + run.line_top;
        float box_h = run.line_height;
        if (height_style == RectHeightStyle::Tight) {
            const float asc = (run.max_ascent > 0.0f) ? run.max_ascent : metrics_.font_size * 0.8f;
            const float desc = (run.max_descent > 0.0f) ? run.max_descent : metrics_.font_size * 0.2f;
            box_h = asc + desc;
        }

        if (run.glyphs.empty()) {
            if (start <= line_end_global && end > line_end_global) {
                push_rect(origin_x, box_y, 4.0f, box_h);
            }
            continue;
        }

        bool in_span = false;
        float span_x0 = 0.0f;
        float span_x1 = 0.0f;
        for (const auto& g : run.glyphs) {
            if (g.is_mark) continue;
            if (g.end > local_start && g.start < local_end) {
                const float gx0 = g.x;
                const float gx1 = g.x + g.w;
                if (!in_span) {
                    in_span = true;
                    span_x0 = gx0;
                    span_x1 = gx1;
                } else if (gx0 <= span_x1 + 1.5f && gx1 >= span_x0 - 1.5f) {
                    span_x0 = std::min(span_x0, gx0);
                    span_x1 = std::max(span_x1, gx1);
                } else {
                    push_rect(origin_x + span_x0, box_y, span_x1 - span_x0, box_h);
                    span_x0 = gx0;
                    span_x1 = gx1;
                }
            } else if (in_span) {
                push_rect(origin_x + span_x0, box_y, span_x1 - span_x0, box_h);
                in_span = false;
            }
        }
        if (in_span) {
            push_rect(origin_x + span_x0, box_y, span_x1 - span_x0, box_h);
        }
    }
    return rects;
}

GlyphPosition Buffer::glyph_position_at(
    float x,
    float y,
    float origin_x,
    float origin_y
) const {
    if (lines_.empty()) return {};
    const auto runs = layout_runs();
    if (runs.empty()) return {};

    std::vector<size_t> line_offsets(lines_.size(), 0);
    for (size_t i = 1; i < lines_.size(); ++i) {
        line_offsets[i] = line_offsets[i - 1] + lines_[i - 1].text().size() + 1;
    }

    const float local_x = x - origin_x;
    const float local_y = y - origin_y;

    const LayoutRun* best_run = &runs[0];
    for (const auto& run : runs) {
        if (local_y >= run.line_top && local_y < run.line_top + run.line_height) {
            best_run = &run;
            break;
        }
        if (local_y >= run.line_top) {
            best_run = &run;
        }
    }

    const size_t base = (best_run->line_i < line_offsets.size()) ? line_offsets[best_run->line_i] : 0;
    if (best_run->glyphs.empty()) {
        return GlyphPosition{base, false};
    }

    const auto& first_g = best_run->glyphs.front();
    const auto& last_g = best_run->glyphs.back();
    if (local_x <= first_g.x) {
        const bool rtl = (first_g.level & 1) != 0;
        return GlyphPosition{base + (rtl ? first_g.end : first_g.start), rtl};
    }
    if (local_x >= last_g.x + last_g.w) {
        const bool rtl = (last_g.level & 1) != 0;
        return GlyphPosition{base + (rtl ? last_g.start : last_g.end), true};
    }

    for (const auto& g : best_run->glyphs) {
        if (g.is_mark) continue;
        if (local_x >= g.x && local_x <= g.x + g.w) {
            const bool rtl = (g.level & 1) != 0;
            const bool left_half = (local_x < g.x + g.w * 0.5f);
            if (!rtl) {
                return left_half ? GlyphPosition{base + g.start, false}
                                 : GlyphPosition{base + g.end, true};
            }
            return left_half ? GlyphPosition{base + g.end, true}
                             : GlyphPosition{base + g.start, false};
        }
    }
    return GlyphPosition{base + last_g.end, true};
}

TextRange Buffer::word_range_at(size_t byte_pos) const {
    if (lines_.empty()) return {};

    size_t line_base = 0;
    size_t target_line = lines_.size() - 1;
    for (size_t i = 0; i < lines_.size(); ++i) {
        const size_t line_end = line_base + lines_[i].text().size();
        if (byte_pos <= line_end || i + 1 == lines_.size()) {
            target_line = i;
            break;
        }
        line_base = line_end + 1;
    }

    const std::string_view text = lines_[target_line].text();
    if (text.empty()) {
        return TextRange{line_base, line_base};
    }

    const size_t local_pos = std::min(byte_pos - line_base, text.size());
    size_t w_end = 0;
    size_t w_start = 0;
    if (local_pos >= text.size()) {
        w_start = Bidi::prev_word_boundary(text, text.size());
        w_end = Bidi::next_word_boundary(text, w_start);
    } else {
        w_end = Bidi::next_word_boundary(text, local_pos);
        w_start = Bidi::prev_word_boundary(text, w_end);
    }
    if (w_start > w_end) std::swap(w_start, w_end);
    return TextRange{line_base + w_start, line_base + w_end};
}

DecorationGeometry build_run_decorations(
    const LayoutRun& run,
    float origin_x,
    float origin_y,
    float default_font_size,
    Color default_color,
    const FontSystem* font_system
) {
    DecorationGeometry geom;
    if (run.glyphs.empty()) return geom;

    const float baseline_offset = (run.max_ascent > 0.0f) ? run.max_ascent : default_font_size;
    const float line_baseline = origin_y + run.line_top + baseline_offset;

    enum class DecKind { Underline, Strikethrough, Overline };

    auto collect_underline_gaps = [&](float x0, float x1, float y_band_top, float y_band_bottom, float pad_x)
        -> std::vector<std::pair<float, float>> {
        std::vector<std::pair<float, float>> gaps;
        if (!font_system || x1 <= x0) return gaps;

        const float y_mid = 0.5f * (y_band_top + y_band_bottom);
        std::vector<float> mid_crossings;

        auto process_segment = [&](Point a, Point b, std::vector<std::pair<float, float>>& local_spans) {
            const float min_y = std::min(a.y, b.y);
            const float max_y = std::max(a.y, b.y);
            if (max_y < y_band_top || min_y > y_band_bottom) return;

            // Scanline crossing at y_mid for solid glyph interior fill detection
            if ((a.y <= y_mid && b.y > y_mid) || (b.y <= y_mid && a.y > y_mid)) {
                const float t = (y_mid - a.y) / (b.y - a.y);
                mid_crossings.push_back(a.x + (b.x - a.x) * t);
            }

            // Clip segment to [y_band_top, y_band_bottom] for curves brushing the band
            const float dy = b.y - a.y;
            if (std::abs(dy) < 1e-4f) {
                local_spans.emplace_back(std::min(a.x, b.x), std::max(a.x, b.x));
            } else {
                const float t0 = std::clamp((y_band_top - a.y) / dy, 0.0f, 1.0f);
                const float t1 = std::clamp((y_band_bottom - a.y) / dy, 0.0f, 1.0f);
                const float ix0 = a.x + (b.x - a.x) * t0;
                const float ix1 = a.x + (b.x - a.x) * t1;
                local_spans.emplace_back(std::min(ix0, ix1), std::max(ix0, ix1));
            }
        };

        std::vector<std::pair<float, float>> local_spans;
        for (const auto& g : run.glyphs) {
            if (g.is_placeholder || g.glyph_id == 0) continue;
            const float gx = origin_x + g.x + g.x_offset;
            const float gy = line_baseline + g.y + g.y_offset;
            if (gx + std::max(g.w, g.font_size) + pad_x < x0 || gx - g.font_size - pad_x > x1) continue;

            const TtfFont* font = font_system->get_font(g.font_id);
            if (!font) continue;
            Path gpath;
            const bool got_path = (g.variation_coords_opt && font->is_variable_font())
                ? font->get_glyph_path_varied(g.glyph_id, gpath, g.font_size, *g.variation_coords_opt, g.synthetic_bold, g.synthetic_italic)
                : font->get_glyph_path_styled(g.glyph_id, gpath, g.font_size, g.synthetic_bold, g.synthetic_italic);
            if (!got_path) {
                continue;
            }
            const Rect gb = gpath.bounds();
            if (gy + gb.bottom() < y_band_top || gy + gb.top() > y_band_bottom) {
                continue;
            }

            mid_crossings.clear();
            local_spans.clear();

            PathSegmentsIter iter(gpath);
            Point cur{0.0f, 0.0f};
            Point start_pt{0.0f, 0.0f};
            bool has_start = false;

            while (auto seg = iter.next()) {
                switch (seg->type) {
                    case PathSegment::Type::MoveTo:
                        cur = Point::from_xy(gx + seg->p0.x, gy + seg->p0.y);
                        start_pt = cur;
                        has_start = true;
                        break;
                    case PathSegment::Type::LineTo: {
                        Point next = Point::from_xy(gx + seg->p0.x, gy + seg->p0.y);
                        process_segment(cur, next, local_spans);
                        cur = next;
                        break;
                    }
                    case PathSegment::Type::QuadTo: {
                        Point cp = Point::from_xy(gx + seg->p0.x, gy + seg->p0.y);
                        Point end = Point::from_xy(gx + seg->p1.x, gy + seg->p1.y);
                        Point prev = cur;
                        constexpr int kSteps = 6;
                        for (int s = 1; s <= kSteps; ++s) {
                            const float t = static_cast<float>(s) / static_cast<float>(kSteps);
                            const float u = 1.0f - t;
                            Point pt = Point::from_xy(
                                u * u * cur.x + 2.0f * u * t * cp.x + t * t * end.x,
                                u * u * cur.y + 2.0f * u * t * cp.y + t * t * end.y
                            );
                            process_segment(prev, pt, local_spans);
                            prev = pt;
                        }
                        cur = end;
                        break;
                    }
                    case PathSegment::Type::CubicTo: {
                        Point c1 = Point::from_xy(gx + seg->p0.x, gy + seg->p0.y);
                        Point c2 = Point::from_xy(gx + seg->p1.x, gy + seg->p1.y);
                        Point end = Point::from_xy(gx + seg->p2.x, gy + seg->p2.y);
                        Point prev = cur;
                        constexpr int kSteps = 8;
                        for (int s = 1; s <= kSteps; ++s) {
                            const float t = static_cast<float>(s) / static_cast<float>(kSteps);
                            const float u = 1.0f - t;
                            Point pt = Point::from_xy(
                                u*u*u*cur.x + 3.0f*u*u*t*c1.x + 3.0f*u*t*t*c2.x + t*t*t*end.x,
                                u*u*u*cur.y + 3.0f*u*u*t*c1.y + 3.0f*u*t*t*c2.y + t*t*t*end.y
                            );
                            process_segment(prev, pt, local_spans);
                            prev = pt;
                        }
                        cur = end;
                        break;
                    }
                    case PathSegment::Type::Close:
                        if (has_start) {
                            process_segment(cur, start_pt, local_spans);
                            cur = start_pt;
                        }
                        break;
                }
            }

            if (mid_crossings.size() >= 2) {
                std::sort(mid_crossings.begin(), mid_crossings.end());
                for (size_t k = 0; k + 1 < mid_crossings.size(); k += 2) {
                    local_spans.emplace_back(mid_crossings[k], mid_crossings[k + 1]);
                }
            }

            for (const auto& [lo, hi] : local_spans) {
                const float gl = std::max(x0, lo - pad_x);
                const float gh = std::min(x1, hi + pad_x);
                if (gh > gl) {
                    gaps.emplace_back(gl, gh);
                }
            }
        }

        if (gaps.size() > 1) {
            std::sort(gaps.begin(), gaps.end());
            std::vector<std::pair<float, float>> merged;
            merged.reserve(gaps.size());
            for (const auto& iv : gaps) {
                if (merged.empty() || iv.first > merged.back().second + 0.5f) {
                    merged.push_back(iv);
                } else {
                    merged.back().second = std::max(merged.back().second, iv.second);
                }
            }
            return merged;
        }
        return gaps;
    };

    auto emit_segment = [&](DecKind kind, float x0, float x1, float y_pos, float thickness, Color color, const LayoutGlyph& proto) {
        if (x1 <= x0) return;
        const float span_w = x1 - x0;
        switch (proto.decoration_style) {
            case DecorationStyle::Solid: {
                geom.rects.push_back(DecorationGeometry::RectSpan{x0, y_pos, span_w, thickness, color});
                break;
            }
            case DecorationStyle::Double: {
                const float bar_h = std::max(1.0f, thickness * 0.7f);
                const float gap = std::max(1.5f, thickness * 1.1f);
                geom.rects.push_back(DecorationGeometry::RectSpan{x0, y_pos, span_w, bar_h, color});
                geom.rects.push_back(DecorationGeometry::RectSpan{x0, y_pos + bar_h + gap, span_w, bar_h, color});
                break;
            }
            case DecorationStyle::Dotted: {
                const float dot_s = std::max(1.5f, thickness * 1.1f);
                const float step = dot_s * 2.1f;
                for (float px = x0; px < x1; px += step) {
                    geom.rects.push_back(DecorationGeometry::RectSpan{
                        px, y_pos, std::min(dot_s, x1 - px), dot_s, color
                    });
                }
                break;
            }
            case DecorationStyle::Dashed: {
                const float dash_w = std::max(3.5f, thickness * 3.0f);
                const float gap_w = std::max(2.0f, thickness * 1.8f);
                const float step = dash_w + gap_w;
                for (float px = x0; px < x1; px += step) {
                    geom.rects.push_back(DecorationGeometry::RectSpan{
                        px, y_pos, std::min(dash_w, x1 - px), thickness, color
                    });
                }
                break;
            }
            case DecorationStyle::Wavy: {
                const float amp = std::max(1.2f, thickness * 1.05f);
                const float period = std::max(6.0f, thickness * 5.0f);
                const float seg = std::max(1.5f, period * 0.125f);
                const float cy = y_pos + thickness * 0.5f;
                constexpr float two_pi = 6.28318530718f;
                DecorationGeometry::WaveStroke wave;
                wave.stroke_width = thickness;
                wave.color = color;
                for (float px = x0; px < x1; px += seg) {
                    float py = cy + amp * std::sin(two_pi * (px - x0) / period);
                    wave.points.push_back(Point::from_xy(px, py));
                }
                wave.points.push_back(Point::from_xy(
                    x1, cy + amp * std::sin(two_pi * span_w / period)
                ));
                if (wave.points.size() >= 2) {
                    geom.waves.push_back(std::move(wave));
                }
                break;
            }
        }
        (void)kind;
    };

    auto emit_span = [&](DecKind kind, float x0, float x1, const LayoutGlyph& proto) {
        if (x1 <= x0) return;
        const Color color = proto.decoration_color_opt.has_value()
            ? proto.decoration_color_opt->to_color()
            : (proto.color_opt.has_value() ? proto.color_opt->to_color() : default_color);
        const float thickness = std::max(1.0f, proto.font_size * 0.07f) * std::max(0.1f, proto.decoration_thickness);

        float y_pos = line_baseline + proto.y;
        if (kind == DecKind::Underline) {
            y_pos += proto.font_size * 0.12f;
        } else if (kind == DecKind::Strikethrough) {
            y_pos -= proto.font_size * 0.30f;
        } else {
            y_pos -= proto.font_size * 0.85f;
        }

        if (kind == DecKind::Underline && proto.decoration_mode == DecorationMode::Gaps && font_system != nullptr) {
            float band_h = thickness;
            if (proto.decoration_style == DecorationStyle::Double) {
                const float bar_h = std::max(1.0f, thickness * 0.7f);
                const float gap = std::max(1.5f, thickness * 1.1f);
                band_h = bar_h * 2.0f + gap;
            } else if (proto.decoration_style == DecorationStyle::Wavy) {
                const float amp = std::max(1.2f, thickness * 1.05f);
                band_h = thickness + amp * 2.0f;
            }
            const float pad_y = std::max(0.5f, thickness * 0.35f);
            const float pad_x = std::max(1.25f, thickness * 1.1f);
            const auto gaps = collect_underline_gaps(x0, x1, y_pos - pad_y, y_pos + band_h + pad_y, pad_x);
            if (!gaps.empty()) {
                float cur = x0;
                for (const auto& [gl, gh] : gaps) {
                    if (gl > cur + 1.0f) {
                        emit_segment(kind, cur, gl, y_pos, thickness, color, proto);
                    }
                    cur = std::max(cur, gh);
                }
                if (x1 > cur + 1.0f) {
                    emit_segment(kind, cur, x1, y_pos, thickness, color, proto);
                }
                return;
            }
        }

        emit_segment(kind, x0, x1, y_pos, thickness, color, proto);
    };

    auto collect_kind = [&](DecKind kind) {
        auto has_kind = [&](const LayoutGlyph& g) {
            if (kind == DecKind::Underline) return g.underline;
            if (kind == DecKind::Strikethrough) return g.strikethrough;
            return g.overline;
        };

        bool active = false;
        float span_x0 = 0.0f;
        float span_x1 = 0.0f;
        const LayoutGlyph* proto = nullptr;

        auto same_style = [&](const LayoutGlyph& a, const LayoutGlyph& b) {
            return a.decoration_style == b.decoration_style &&
                   a.decoration_mode == b.decoration_mode &&
                   a.decoration_color_opt == b.decoration_color_opt &&
                   a.decoration_thickness == b.decoration_thickness &&
                   a.font_size == b.font_size &&
                   a.y == b.y &&
                   a.color_opt == b.color_opt;
        };

        for (const auto& g : run.glyphs) {
            if (g.is_mark && g.w <= 0.0f) continue;
            if (has_kind(g) && g.w > 0.0f) {
                const float gx0 = origin_x + g.x;
                const float gx1 = origin_x + g.x + g.w;
                if (!active) {
                    active = true;
                    span_x0 = gx0;
                    span_x1 = gx1;
                    proto = &g;
                } else if (gx0 <= span_x1 + 1.5f && gx1 >= span_x0 - 1.5f && same_style(*proto, g)) {
                    span_x0 = std::min(span_x0, gx0);
                    span_x1 = std::max(span_x1, gx1);
                } else {
                    emit_span(kind, span_x0, span_x1, *proto);
                    span_x0 = gx0;
                    span_x1 = gx1;
                    proto = &g;
                }
            } else if (active) {
                emit_span(kind, span_x0, span_x1, *proto);
                active = false;
                proto = nullptr;
            }
        }
        if (active && proto != nullptr) {
            emit_span(kind, span_x0, span_x1, *proto);
        }
    };

    collect_kind(DecKind::Underline);
    collect_kind(DecKind::Strikethrough);
    collect_kind(DecKind::Overline);
    return geom;
}

void Buffer::draw(
    Canvas& canvas,
    GlyphCache& cache,
    FontSystem& font_system,
    Color default_color,
    float origin_x,
    float origin_y
) {
    draw_internal(canvas, cache, font_system, default_color, nullptr, origin_x, origin_y);
}

void Buffer::draw(
    Canvas& canvas,
    GlyphCache& cache,
    FontSystem& font_system,
    const Paint& default_paint,
    float origin_x,
    float origin_y
) {
    const Color fallback_color = default_paint.is_solid_color()
        ? default_paint.shader.solid_color()
        : Color::WHITE;
    const Paint* override_ptr = paint_requires_vector_path(default_paint) ? &default_paint : nullptr;
    draw_internal(canvas, cache, font_system, fallback_color, override_ptr, origin_x, origin_y);
}

void Buffer::draw_internal(
    Canvas& canvas,
    GlyphCache& cache,
    FontSystem& font_system,
    Color default_color,
    const Paint* default_paint_override,
    float origin_x,
    float origin_y
) {
    shape_until_scroll(font_system);
    auto runs = layout_runs();

    std::vector<ColorGlyphLayer> color_layers;

    auto render_glyph_solid = [&](const LayoutGlyph& glyph, const TtfFont* font, float gx, float gy,
                                  Color fg_color, bool force_solid_color) {
        const std::span<const FontVariation> vars = glyph.variation_coords_opt
            ? std::span<const FontVariation>(*glyph.variation_coords_opt)
            : std::span<const FontVariation>{};
        if (font->has_color_glyphs() && font->get_color_glyph_layers(glyph.glyph_id, color_layers)) {
            for (const auto& layer : color_layers) {
                const CachedGlyph* layer_cached = cache.get_or_render(
                    *font, glyph.font_id, layer.glyph_id, glyph.font_size,
                    gx, gy, glyph.synthetic_bold, glyph.synthetic_italic, vars, glyph.variation_hash
                );
                if (!layer_cached || layer_cached->data.empty()) continue;
                Point dst_pos = Point::from_xy(
                    gx + static_cast<float>(layer_cached->offset_x),
                    gy + static_cast<float>(layer_cached->offset_y)
                );
                Paint layer_paint;
                layer_paint.set_color((force_solid_color || layer.use_foreground) ? fg_color : layer.color.to_color());
                canvas.draw_glyph(dst_pos, layer_cached->as_mask_ref(), layer_paint);
            }
            return;
        }

        const CachedGlyph* cached = cache.get_or_render(
            *font, glyph.font_id, glyph.glyph_id, glyph.font_size,
            gx, gy, glyph.synthetic_bold, glyph.synthetic_italic, vars, glyph.variation_hash
        );
        if (!cached || cached->data.empty()) return;
        Point dst_pos = Point::from_xy(
            gx + static_cast<float>(cached->offset_x),
            gy + static_cast<float>(cached->offset_y)
        );
        Paint paint;
        paint.set_color(fg_color);
        canvas.draw_glyph(dst_pos, cached->as_mask_ref(), paint);
    };

    auto render_glyph_with_paint = [&](const LayoutGlyph& glyph, const TtfFont* font, float gx, float gy,
                                       const Paint& paint) {
        const Stroke stroke = glyph.foreground_stroke_opt
            ? *glyph.foreground_stroke_opt
            : Stroke(paint.get_stroke_width());
        const std::span<const FontVariation> vars = glyph.variation_coords_opt
            ? std::span<const FontVariation>(*glyph.variation_coords_opt)
            : std::span<const FontVariation>{};

        if (font->has_color_glyphs() && font->get_color_glyph_layers(glyph.glyph_id, color_layers)) {
            for (const auto& layer : color_layers) {
                Path lpath;
                const bool ok = (!vars.empty() && font->is_variable_font())
                    ? font->get_glyph_path_varied(layer.glyph_id, lpath, glyph.font_size, vars,
                                                  glyph.synthetic_bold, glyph.synthetic_italic)
                    : font->get_glyph_path_styled(layer.glyph_id, lpath, glyph.font_size,
                                                  glyph.synthetic_bold, glyph.synthetic_italic);
                if (!ok) continue;
                lpath.apply_transform(Transform::from_translate(gx, gy));
                if (layer.use_foreground) {
                    canvas.draw_path(lpath, paint, stroke, FillRule::Winding);
                } else {
                    Paint lp;
                    lp.set_color(layer.color.to_color());
                    lp.blend_mode = paint.blend_mode;
                    canvas.fill_path(lpath, lp, FillRule::Winding);
                }
            }
            return;
        }

        Path gpath;
        const bool ok = (!vars.empty() && font->is_variable_font())
            ? font->get_glyph_path_varied(glyph.glyph_id, gpath, glyph.font_size, vars,
                                          glyph.synthetic_bold, glyph.synthetic_italic)
            : font->get_glyph_path_styled(glyph.glyph_id, gpath, glyph.font_size,
                                          glyph.synthetic_bold, glyph.synthetic_italic);
        if (!ok) return;
        gpath.apply_transform(Transform::from_translate(gx, gy));
        canvas.draw_path(gpath, paint, stroke, FillRule::Winding);
    };

    // Collect distinct blurred shadow sigmas once across the buffer
    std::vector<float> blur_sigmas;
    bool has_sharp_shadows = false;
    for (const auto& run : runs) {
        for (const auto& glyph : run.glyphs) {
            if (glyph.is_placeholder) continue;
            for (const auto& sh : glyph.shadows) {
                if (sh.blur_sigma > 0.1f) {
                    if (std::find_if(blur_sigmas.begin(), blur_sigmas.end(), [&](float s) {
                            return std::abs(s - sh.blur_sigma) < 0.05f;
                        }) == blur_sigmas.end()) {
                        blur_sigmas.push_back(sh.blur_sigma);
                    }
                } else {
                    has_sharp_shadows = true;
                }
            }
        }
    }

    for (const auto& run : runs) {
        const float baseline_offset = (run.max_ascent > 0.0f) ? run.max_ascent : metrics_.font_size;
        const float line_baseline = origin_y + run.line_top + baseline_offset;

        // Pass 1: Span-merged background highlights (supports both full Paint and solid TextColor)
        if (run.line_height > 0.0f) {
            bool bg_active = false;
            float bg_x0 = 0.0f;
            float bg_x1 = 0.0f;
            const Paint* bg_paint_ptr = nullptr;
            std::optional<TextColor> bg_solid_opt = std::nullopt;

            auto flush_bg = [&]() {
                if (!bg_active || bg_x1 <= bg_x0) return;
                if (auto bg_rect = Rect::from_xywh(bg_x0, origin_y + run.line_top, bg_x1 - bg_x0, run.line_height)) {
                    if (bg_paint_ptr != nullptr) {
                        canvas.fill_rect(*bg_rect, *bg_paint_ptr);
                    } else if (bg_solid_opt.has_value()) {
                        Paint solid_p;
                        solid_p.set_color(bg_solid_opt->to_color());
                        canvas.fill_rect(*bg_rect, solid_p);
                    }
                }
                bg_active = false;
                bg_paint_ptr = nullptr;
                bg_solid_opt.reset();
            };

            for (const auto& glyph : run.glyphs) {
                if (glyph.is_mark && glyph.w <= 0.0f) continue;
                const bool has_bg = (glyph.background_paint_opt != nullptr || glyph.background_color_opt.has_value()) && glyph.w > 0.0f;
                if (!has_bg) {
                    flush_bg();
                    continue;
                }
                const float gx0 = origin_x + glyph.x;
                const float gx1 = origin_x + glyph.x + glyph.w;
                const Paint* g_paint = glyph.background_paint_opt.get();
                const std::optional<TextColor> g_solid = (g_paint == nullptr) ? glyph.background_color_opt : std::nullopt;

                if (!bg_active) {
                    bg_active = true;
                    bg_x0 = gx0;
                    bg_x1 = gx1;
                    bg_paint_ptr = g_paint;
                    bg_solid_opt = g_solid;
                } else if (gx0 <= bg_x1 + 1.5f && gx1 >= bg_x0 - 1.5f &&
                           bg_paint_ptr == g_paint && bg_solid_opt == g_solid) {
                    bg_x0 = std::min(bg_x0, gx0);
                    bg_x1 = std::max(bg_x1, gx1);
                } else {
                    flush_bg();
                    bg_active = true;
                    bg_x0 = gx0;
                    bg_x1 = gx1;
                    bg_paint_ptr = g_paint;
                    bg_solid_opt = g_solid;
                }
            }
            flush_bg();
        }

        // Pass 1.5a: Sharp text shadows
        if (has_sharp_shadows) {
            for (const auto& glyph : run.glyphs) {
                if (glyph.is_placeholder || glyph.glyph_id == 0 || glyph.shadows.empty()) continue;
                const TtfFont* font = font_system.get_font(glyph.font_id);
                if (!font) continue;
                const float gx = origin_x + glyph.x + glyph.x_offset;
                const float gy = line_baseline + glyph.y + glyph.y_offset;
                for (const auto& sh : glyph.shadows) {
                    if (sh.blur_sigma <= 0.1f) {
                        render_glyph_solid(glyph, font, gx + sh.offset_x, gy + sh.offset_y, sh.color.to_color(), true);
                    }
                }
            }
        }

        // Pass 1.5b: Blurred text shadows via save_layer_bounds + ImageFilter::blur
        for (float sigma : blur_sigmas) {
            float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
            bool any_for_sigma = false;
            for (const auto& glyph : run.glyphs) {
                if (glyph.is_placeholder || glyph.glyph_id == 0 || glyph.shadows.empty()) continue;
                const float gx = origin_x + glyph.x + glyph.x_offset;
                for (const auto& sh : glyph.shadows) {
                    if (sh.blur_sigma > 0.1f && std::abs(sh.blur_sigma - sigma) < 0.05f) {
                        any_for_sigma = true;
                        min_x = std::min(min_x, gx + sh.offset_x - glyph.font_size * 0.5f);
                        max_x = std::max(max_x, gx + sh.offset_x + std::max(glyph.w, glyph.font_size) + glyph.font_size * 0.5f);
                        min_y = std::min(min_y, origin_y + run.line_top + sh.offset_y - glyph.font_size * 0.5f);
                        max_y = std::max(max_y, origin_y + run.line_top + run.line_height + sh.offset_y + glyph.font_size * 0.5f);
                    }
                }
            }
            if (!any_for_sigma) continue;

            const float pad = std::ceil(sigma * 3.0f) + 2.0f;
            if (auto bnds = Rect::from_ltrb(min_x - pad, min_y - pad, max_x + pad, max_y + pad)) {
                Paint layer_paint;
                layer_paint.set_image_filter(effects::ImageFilter::blur(sigma));
                canvas.save_layer_bounds(*bnds, layer_paint);
                for (const auto& glyph : run.glyphs) {
                    if (glyph.is_placeholder || glyph.glyph_id == 0 || glyph.shadows.empty()) continue;
                    const TtfFont* font = font_system.get_font(glyph.font_id);
                    if (!font) continue;
                    const float gx = origin_x + glyph.x + glyph.x_offset;
                    const float gy = line_baseline + glyph.y + glyph.y_offset;
                    for (const auto& sh : glyph.shadows) {
                        if (sh.blur_sigma > 0.1f && std::abs(sh.blur_sigma - sigma) < 0.05f) {
                            render_glyph_solid(glyph, font, gx + sh.offset_x, gy + sh.offset_y, sh.color.to_color(), true);
                        }
                    }
                }
                canvas.restore();
            }
        }

        // Pass 2: Glyphs (fast solid alpha-mask path OR vector path for rich Paint)
        for (const auto& glyph : run.glyphs) {
            if (glyph.is_placeholder || glyph.glyph_id == 0) continue;
            const TtfFont* font = font_system.get_font(glyph.font_id);
            if (!font) continue;
            const float gx = origin_x + glyph.x + glyph.x_offset;
            const float gy = line_baseline + glyph.y + glyph.y_offset;

            const Paint* active_paint = glyph.foreground_paint_opt
                ? glyph.foreground_paint_opt.get()
                : (glyph.color_opt.has_value() ? nullptr : default_paint_override);

            if (active_paint != nullptr) {
                if (paint_requires_vector_path(*active_paint)) {
                    render_glyph_with_paint(glyph, font, gx, gy, *active_paint);
                } else {
                    render_glyph_solid(glyph, font, gx, gy, active_paint->shader.solid_color(), false);
                }
            } else {
                const Color fg_color = glyph.color_opt.has_value() ? glyph.color_opt->to_color() : default_color;
                render_glyph_solid(glyph, font, gx, gy, fg_color, false);
            }
        }

        // Pass 3: Decorations (underline + smart gap skipping, strikethrough, overline, all 5 styles)
        const auto dec = build_run_decorations(run, origin_x, origin_y, metrics_.font_size, default_color, &font_system);
        for (const auto& r : dec.rects) {
            if (auto rect = Rect::from_xywh(r.x, r.y, r.w, r.h)) {
                Paint dp;
                dp.set_color(r.color);
                canvas.fill_rect(*rect, dp);
            }
        }
        for (const auto& wv : dec.waves) {
            PathBuilder pb;
            pb.move_to(wv.points[0]);
            for (size_t i = 1; i < wv.points.size(); ++i) {
                pb.line_to(wv.points[i]);
            }
            if (auto path = pb.finish()) {
                Paint dp;
                dp.set_color(wv.color);
                canvas.stroke_path(*path, dp, Stroke(wv.stroke_width));
            }
        }
    }
}

} // namespace nisaba::text
