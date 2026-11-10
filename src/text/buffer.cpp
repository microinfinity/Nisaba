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

bool is_below_diacritic(char32_t cp) noexcept {
    // Arabic Kasra (0x0650), Kasratan (0x064D), Hamza below (0x0655), Subscript Alef (0x0656)
    // and Latin combining below marks (0x0316..0x0333)
    return cp == 0x064D || cp == 0x0650 || cp == 0x0655 || cp == 0x0656 ||
           (cp >= 0x0316 && cp <= 0x0333);
}

void apply_line_alignment(
    LayoutLine& line,
    std::string_view line_text,
    std::optional<float> wrap_width,
    Align align,
    Direction base_dir,
    bool is_last_line
) noexcept {
    if (line.glyphs.empty()) return;
    float lx = 0.0f;
    for (auto& g : line.glyphs) {
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
                    }
                    cur_x += lg.w;
                }
                line.w = *wrap_width;
                return;
            }
        }
    }

    float offset = 0.0f;
    if (align == Align::Right) {
        offset = spare;
    } else if (align == Align::Center) {
        offset = spare * 0.5f;
    } else if (align == Align::End) {
        offset = (base_dir == Direction::RightToLeft) ? 0.0f : spare;
    }
    if (offset > 0.0f) {
        for (auto& g : line.glyphs) {
            g.x += offset;
        }
    }
}

} // namespace

BufferLine::BufferLine(std::string_view line_text, const Attrs& default_attrs)
    : text_(line_text), attrs_list_(default_attrs) {}

void BufferLine::shape_and_layout(
    FontSystem& font_system,
    float font_size,
    float line_height,
    std::optional<float> wrap_width,
    Wrap wrap,
    Align align,
    Shaping shaping,
    bool enable_kerning,
    bool enable_ligatures
) {
    layout_lines_.clear();
    is_shaped_ = true;

    if (text_.empty()) {
        LayoutLine empty_line;
        empty_line.line_height_opt = line_height;
        empty_line.max_ascent = font_size * 0.8f;
        empty_line.max_descent = font_size * 0.2f;
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
    const bool has_custom_attrs = !attrs_list_.spans().empty() || def_attrs.has_custom_metrics_or_decorations();

    // -------------------------------------------------------------------------
    // ULTRA-FAST PATH: Pure ASCII with Uniform Attributes (Standard Case)
    // -------------------------------------------------------------------------
    if (is_pure_ascii && !has_custom_attrs && !enable_ligatures) {
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
            float cur_line_w = 0.0f;

            for (auto& g : all_glyphs) {
                if (cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                    apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                    layout_lines_.push_back(std::move(cur_line));
                    cur_line = LayoutLine{};
                    cur_line.line_height_opt = line_height;
                    cur_line.max_ascent = asc;
                    cur_line.max_descent = desc;
                    cur_line_w = 0.0f;
                }
                cur_line_w += g.w;
                cur_line.glyphs.push_back(g);
            }
            if (!cur_line.glyphs.empty()) {
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

    if (!has_rtl_or_marks && !has_custom_attrs && !enable_ligatures) {
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
            float cur_line_w = 0.0f;

            for (auto& g : all_glyphs) {
                if (cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                    apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, false);
                    layout_lines_.push_back(std::move(cur_line));
                    cur_line = LayoutLine{};
                    cur_line.line_height_opt = line_height;
                    cur_line.max_ascent = asc;
                    cur_line.max_descent = desc;
                    cur_line_w = 0.0f;
                }
                cur_line_w += g.w;
                cur_line.glyphs.push_back(g);
            }
            if (!cur_line.glyphs.empty()) {
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
                apply_line_alignment(cur_line, text_, wrap_width, align, Direction::LeftToRight, true);
                layout_lines_.push_back(std::move(cur_line));
            }
            return;
        }
    }

    // -------------------------------------------------------------------------
    // FULL ADVANCED SHAPING PIPELINE: BiDi, Rich Spans, GSUB/GPOS, Marks & Kerning
    // -------------------------------------------------------------------------
    Direction base_dir = Direction::LeftToRight;
    std::vector<ShapedChar> shaped_chars;

    if (is_pure_ascii) {
        shaped_chars.resize(text_.size());
        for (size_t i = 0; i < text_.size(); ++i) {
            char32_t cp = static_cast<char32_t>(static_cast<unsigned char>(text_[i]));
            shaped_chars[i] = ShapedChar{cp, i, 0, false, false, ArabicForm::None, cp};
        }
    } else {
        base_dir = Bidi::detect_base_direction(text_);
        auto runs = Bidi::segment_runs(text_, base_dir);
        if (shaping == Shaping::Advanced) {
            shaped_chars = Bidi::shape_text(text_, runs);
        } else {
            shaped_chars.reserve(decoded.size());
            for (const auto& [cp, byte_idx] : decoded) {
                shaped_chars.push_back(ShapedChar{cp, byte_idx, 0, false, Bidi::is_combining_mark(cp), ArabicForm::None, cp});
            }
        }
    }

    if (shaped_chars.empty()) {
        LayoutLine empty_line;
        empty_line.line_height_opt = line_height;
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

    float max_line_asc = font_size * 0.8f;
    float max_line_desc = font_size * 0.2f;

    for (size_t i = 0; i < shaped_chars.size(); ++i) {
        const auto& sc = shaped_chars[i];
        Attrs attrs = attrs_list_.spans().empty() ? attrs_list_.defaults() : attrs_list_.get(sc.byte_index);
        float eff_font_size = attrs.font_size_opt.value_or(font_size);

        if (!last_attrs.has_value() || *last_attrs != attrs) {
            auto matched = font_system.match_font_styled(attrs.family, attrs.weight, attrs.style, attrs.stretch);
            last_primary_fid = matched.font_id;
            last_primary_font = matched.font;
            last_attrs = attrs;
        }

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
                        gid = font->gsub()->apply_single_glyph_feature(gid, feat_tag);
                    }
                }
            }
        }

        if (gid == 0 && font_system.font_count() > 1) {
            auto [fb_fid, fb_gid] = font_system.find_glyph_or_fallback(last_primary_fid, sc.codepoint);
            if (fb_gid == 0 && sc.original_codepoint != 0 && sc.original_codepoint != sc.codepoint) {
                std::tie(fb_fid, fb_gid) = font_system.find_glyph_or_fallback(last_primary_fid, sc.original_codepoint);
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
                        fb_gid = fb_font->gsub()->apply_single_glyph_feature(fb_gid, feat_tag);
                    }
                }
            }
            actual_fid = fb_fid;
            gid = fb_gid;
            font = font_system.get_font(actual_fid);
        }

        if (font != nullptr) {
            max_line_asc = std::max(max_line_asc, font->ascent(eff_font_size));
            max_line_desc = std::max(max_line_desc, -font->descent(eff_font_size));
        }

        bool is_mark_glyph = sc.is_mark || (font && font->gdef() && font->gdef()->is_mark(gid));
        float adv = 0.0f;
        if (!is_mark_glyph) {
            adv = (font != nullptr) ? font->glyph_advance(gid, eff_font_size) : (eff_font_size * 0.5f);
            adv += attrs.letter_spacing;
            if (sc.codepoint == ' ' || sc.codepoint == '\t') {
                adv += attrs.word_spacing;
            }
        }

        bool synth_bold = font && (static_cast<int>(attrs.weight) >= 600) && (static_cast<int>(font->weight()) < 600);
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
        g.level = sc.level;
        g.color_opt = attrs.color_opt;
        g.background_color_opt = attrs.background_color_opt;
        g.metadata = attrs.metadata;
        g.is_mark = is_mark_glyph;
        g.underline = attrs.underline;
        g.strikethrough = attrs.strikethrough;
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

    // Optional OpenType GSUB Ligatures pass on contiguous same-font LTR runs
    if (enable_ligatures && shaping == Shaping::Advanced && all_glyphs.size() >= 2) {
        size_t i = 0;
        while (i + 1 < all_glyphs.size()) {
            const TtfFont* font = font_system.get_font(all_glyphs[i].font_id);
            if (!font || !font->gsub() || all_glyphs[i].is_mark) {
                ++i;
                continue;
            }
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
                        all_glyphs[i + k].is_mark) {
                        same_run = false;
                        break;
                    }
                    seq.push_back(all_glyphs[i + k].glyph_id);
                }
                if (same_run && font->apply_ligatures(seq) && seq.size() == 1) {
                    all_glyphs[i].glyph_id = seq[0];
                    all_glyphs[i].end = all_glyphs[i + span_len - 1].end;
                    all_glyphs[i].w = font->glyph_advance(seq[0], all_glyphs[i].font_size);
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
        single_line.glyphs = std::move(all_glyphs);
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
        float cur_line_w = 0.0f;

        for (auto& g : all_glyphs) {
            if (!g.is_mark && cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, false);
                layout_lines_.push_back(std::move(cur_line));
                cur_line = LayoutLine{};
                cur_line.line_height_opt = line_height;
                cur_line.max_ascent = max_line_asc;
                cur_line.max_descent = max_line_desc;
                cur_line_w = 0.0f;
            }
            cur_line_w += g.w;
            cur_line.glyphs.push_back(g);
        }
        if (!cur_line.glyphs.empty()) {
            apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, true);
            layout_lines_.push_back(std::move(cur_line));
        }
    } else {
        struct Word {
            std::vector<LayoutGlyph> glyphs;
            float width{0.0f};
            bool is_space{false};
        };

        std::vector<Word> words;
        Word current_word;
        bool prev_was_cjk = false;
        bool prev_no_break_after = false;

        for (size_t i = 0; i < all_glyphs.size(); ++i) {
            const auto& g = all_glyphs[i];
            if (g.is_mark && !current_word.glyphs.empty()) {
                current_word.glyphs.push_back(g);
                continue;
            }

            char32_t cp = (i < shaped_chars.size()) ? shaped_chars[i].codepoint : 0;
            bool is_sp = (cp == ' ' || cp == '\t');
            bool is_cjk_char = is_pure_ascii ? false : Bidi::is_cjk(cp);
            bool no_break_before = is_cjk_char && Bidi::is_cjk_no_break_before(cp);
            bool no_break_after = is_cjk_char && Bidi::is_cjk_no_break_after(cp);

            bool should_split = false;
            if (!current_word.glyphs.empty()) {
                if (current_word.is_space != is_sp) {
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

            current_word.is_space = is_sp;
            current_word.glyphs.push_back(g);
            current_word.width += g.w;

            prev_was_cjk = is_cjk_char;
            prev_no_break_after = no_break_after;
        }
        if (!current_word.glyphs.empty()) {
            words.push_back(std::move(current_word));
        }

        LayoutLine cur_line;
        cur_line.line_height_opt = line_height;
        cur_line.max_ascent = max_line_asc;
        cur_line.max_descent = max_line_desc;
        float cur_line_w = 0.0f;

        for (auto& w : words) {
            if (cur_line_w + w.width > max_w && !cur_line.glyphs.empty()) {
                if (!w.is_space) {
                    apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, false);
                    layout_lines_.push_back(std::move(cur_line));
                    cur_line = LayoutLine{};
                    cur_line.line_height_opt = line_height;
                    cur_line.max_ascent = max_line_asc;
                    cur_line.max_descent = max_line_desc;
                    cur_line_w = 0.0f;
                }
            }

            if (w.width > max_w && wrap == Wrap::WordOrGlyph && !w.is_space) {
                for (const auto& g : w.glyphs) {
                    if (!g.is_mark && cur_line_w + g.w > max_w && !cur_line.glyphs.empty()) {
                        apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, false);
                        layout_lines_.push_back(std::move(cur_line));
                        cur_line = LayoutLine{};
                        cur_line.line_height_opt = line_height;
                        cur_line.max_ascent = max_line_asc;
                        cur_line.max_descent = max_line_desc;
                        cur_line_w = 0.0f;
                    }
                    cur_line_w += g.w;
                    cur_line.glyphs.push_back(g);
                }
            } else {
                for (const auto& g : w.glyphs) {
                    cur_line.glyphs.push_back(g);
                }
                cur_line_w += w.width;
            }
        }

        if (!cur_line.glyphs.empty()) {
            apply_line_alignment(cur_line, text_, wrap_width, align, base_dir, true);
            layout_lines_.push_back(std::move(cur_line));
        }
    }
}

Buffer::Buffer(Metrics metrics) : metrics_(metrics) {}

void Buffer::reset_shaping() noexcept {
    for (auto& line : lines_) {
        line.reset_shaping();
    }
}

void Buffer::set_text(std::string_view text, const Attrs& default_attrs) {
    lines_.clear();
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

void Buffer::apply_ellipsis_truncation(FontSystem& font_system, LayoutLine& line, float max_width) const {
    if (line.glyphs.empty()) return;

    const auto& proto = line.glyphs.back();
    const TtfFont* font = font_system.get_font(proto.font_id);
    if (!font && font_system.default_font_id().has_value()) {
        font = font_system.get_font(*font_system.default_font_id());
    }

    uint16_t ellipsis_gid = font ? font->glyph_index(0x2026) : 0; // '…'
    uint16_t dot_gid = font ? font->glyph_index('.') : 0;
    float ellipsis_w = 0.0f;
    bool use_single_ellipsis = (ellipsis_gid != 0);

    if (use_single_ellipsis) {
        ellipsis_w = font->glyph_advance(ellipsis_gid, proto.font_size);
    } else if (dot_gid != 0) {
        ellipsis_w = font->glyph_advance(dot_gid, proto.font_size) * 3.0f;
    } else {
        ellipsis_w = proto.font_size * 0.8f;
    }

    float target_w = std::max(0.0f, max_width - ellipsis_w);
    while (!line.glyphs.empty() && (line.glyphs.back().x + line.glyphs.back().w > target_w || line.glyphs.back().is_mark)) {
        line.glyphs.pop_back();
    }

    // Trim trailing spaces before appending ellipsis
    float cur_x = line.glyphs.empty() ? 0.0f : (line.glyphs.back().x + line.glyphs.back().w);
    size_t end_byte = line.glyphs.empty() ? 0 : line.glyphs.back().end;

    if (use_single_ellipsis || dot_gid == 0) {
        LayoutGlyph eg = proto;
        eg.start = end_byte;
        eg.end = end_byte;
        eg.glyph_id = ellipsis_gid;
        eg.x = cur_x;
        eg.w = ellipsis_w;
        eg.x_offset = 0.0f;
        eg.y_offset = 0.0f;
        eg.is_mark = false;
        line.glyphs.push_back(eg);
        cur_x += ellipsis_w;
    } else {
        float dot_w = ellipsis_w / 3.0f;
        for (int d = 0; d < 3; ++d) {
            LayoutGlyph dg = proto;
            dg.start = end_byte;
            dg.end = end_byte;
            dg.glyph_id = dot_gid;
            dg.x = cur_x;
            dg.w = dot_w;
            dg.x_offset = 0.0f;
            dg.y_offset = 0.0f;
            dg.is_mark = false;
            line.glyphs.push_back(dg);
            cur_x += dot_w;
        }
    }
    line.w = cur_x;
}

void Buffer::shape_until_scroll(FontSystem& font_system) {
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
                enable_ligatures_
            );
        }
    }

    // Handle single-line Wrap::None overflow with Ellipsis
    if (overflow_ == Overflow::Ellipsis && width_.has_value() && wrap_ == Wrap::None) {
        for (auto& line : lines_) {
            for (auto& lline : line.layout_mut()) {
                if (lline.w > *width_) {
                    apply_ellipsis_truncation(font_system, lline, *width_);
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
                    if (overflow_ == Overflow::Ellipsis && !llines.empty()) {
                        float max_w = width_.value_or(llines.back().w + metrics_.font_size * 2.0f);
                        apply_ellipsis_truncation(font_system, llines.back(), max_w);
                    }
                    seen = max_visual_lines;
                } else {
                    seen += llines.size();
                    if (seen == max_visual_lines && overflow_ == Overflow::Ellipsis && !llines.empty()) {
                        float max_w = width_.value_or(llines.back().w + metrics_.font_size * 2.0f);
                        apply_ellipsis_truncation(font_system, llines.back(), max_w);
                    }
                }
            }
        }
    }
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

void Buffer::draw(
    Canvas& canvas,
    GlyphCache& cache,
    FontSystem& font_system,
    Color default_color,
    float origin_x,
    float origin_y
) {
    shape_until_scroll(font_system);
    auto runs = layout_runs();

    std::vector<ColorGlyphLayer> color_layers;

    for (const auto& run : runs) {
        float line_baseline = origin_y + run.line_top + metrics_.font_size;

        // 1. Pass 1: Draw span background highlights
        for (const auto& glyph : run.glyphs) {
            if (glyph.background_color_opt.has_value() && glyph.w > 0.0f) {
                auto bg_rect = Rect::from_xywh(
                    origin_x + glyph.x,
                    origin_y + run.line_top,
                    glyph.w,
                    run.line_height
                );
                if (bg_rect) {
                    Paint bg_paint;
                    bg_paint.set_color(glyph.background_color_opt->to_color());
                    canvas.fill_rect(*bg_rect, bg_paint);
                }
            }
        }

        // 2. Pass 2: Draw glyphs (including COLR/CPAL color layers, mark offsets, and synthetic styles)
        for (const auto& glyph : run.glyphs) {
            if (glyph.glyph_id == 0) continue;

            const TtfFont* font = font_system.get_font(glyph.font_id);
            if (!font) continue;

            float glyph_x = origin_x + glyph.x + glyph.x_offset;
            float glyph_y = line_baseline + glyph.y + glyph.y_offset;

            Color fg_color = glyph.color_opt.has_value() ? glyph.color_opt->to_color() : default_color;

            if (font->has_color_glyphs() && font->get_color_glyph_layers(glyph.glyph_id, color_layers)) {
                for (const auto& layer : color_layers) {
                    const CachedGlyph* layer_cached = cache.get_or_render(
                        *font,
                        glyph.font_id,
                        layer.glyph_id,
                        glyph.font_size,
                        glyph_x,
                        glyph_y,
                        glyph.synthetic_bold,
                        glyph.synthetic_italic
                    );
                    if (!layer_cached || layer_cached->data.empty()) continue;

                    Point dst_pos = Point::from_xy(
                        glyph_x + static_cast<float>(layer_cached->offset_x),
                        glyph_y + static_cast<float>(layer_cached->offset_y)
                    );
                    Paint layer_paint;
                    layer_paint.set_color(layer.use_foreground ? fg_color : layer.color.to_color());
                    canvas.draw_glyph(dst_pos, layer_cached->as_mask_ref(), layer_paint);
                }
                continue;
            }

            const CachedGlyph* cached = cache.get_or_render(
                *font,
                glyph.font_id,
                glyph.glyph_id,
                glyph.font_size,
                glyph_x,
                glyph_y,
                glyph.synthetic_bold,
                glyph.synthetic_italic
            );

            if (!cached || cached->data.empty()) continue;

            Point dst_pos = Point::from_xy(
                glyph_x + static_cast<float>(cached->offset_x),
                glyph_y + static_cast<float>(cached->offset_y)
            );

            Paint paint;
            paint.set_color(fg_color);
            canvas.draw_glyph(dst_pos, cached->as_mask_ref(), paint);
        }

        // 3. Pass 3: Draw underline and strikethrough decorations
        for (const auto& glyph : run.glyphs) {
            if ((!glyph.underline && !glyph.strikethrough) || glyph.w <= 0.0f) continue;

            Color dec_color = glyph.color_opt.has_value() ? glyph.color_opt->to_color() : default_color;
            Paint dec_paint;
            dec_paint.set_color(dec_color);
            float thickness = std::max(1.0f, glyph.font_size * 0.07f);

            if (glyph.underline) {
                float uy = line_baseline + glyph.y + glyph.font_size * 0.12f;
                if (auto urect = Rect::from_xywh(origin_x + glyph.x, uy, glyph.w, thickness)) {
                    canvas.fill_rect(*urect, dec_paint);
                }
            }
            if (glyph.strikethrough) {
                float sy = line_baseline + glyph.y - glyph.font_size * 0.30f;
                if (auto srect = Rect::from_xywh(origin_x + glyph.x, sy, glyph.w, thickness)) {
                    canvas.fill_rect(*srect, dec_paint);
                }
            }
        }
    }
}

} // namespace nisaba::text
