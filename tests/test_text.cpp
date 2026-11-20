#include <cassert>
#include <cmath>
#include <iostream>
#include <fstream>
#include <string_view>
#include <vector>
#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::text;

namespace {

std::string resolve_font(std::string_view filename) {
    std::vector<std::string> candidates = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename),
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename)
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return candidates[0];
}

} // namespace

void test_attrs() {
    std::cout << "[Test] Text Attributes & Metrics..." << std::endl;

    TextColor c1 = TextColor::rgb(255, 128, 0);
    assert(c1.r == 255 && c1.g == 128 && c1.b == 0 && c1.a == 255);
    assert(c1.to_u32() != 0);

    Metrics m(18.0f, 24.0f);
    assert(m.font_size == 18.0f);
    assert(m.line_height == 24.0f);

    Attrs default_attrs;
    default_attrs.set_weight(Weight::Bold).set_color(c1);

    AttrsList list(default_attrs);
    assert(list.get(0).weight == Weight::Bold);

    Attrs span_attrs;
    span_attrs.set_weight(Weight::Normal).set_style(Style::Italic);
    list.add_span(5, 10, span_attrs);

    assert(list.get(2).weight == Weight::Bold);
    assert(list.get(5).style == Style::Italic);
    assert(list.get(9).style == Style::Italic);
    assert(list.get(10).style == Style::Normal);

    std::cout << "  -> PASS" << std::endl;
}

void test_ttf_font() {
    std::cout << "[Test] Sovereign TrueType Font Parser..." << std::endl;

    auto font = TtfFont::from_file(resolve_font("Inter-Regular.ttf"));
    assert(font != nullptr);
    assert(font->is_valid());
    assert(font->units_per_em() > 0);
    assert(font->num_glyphs() > 0);
    assert(!font->family_name().empty());

    std::cout << "  Loaded font: " << font->family_name()
              << " (" << font->style_name() << "), Glyphs: " << font->num_glyphs()
              << ", UPM: " << font->units_per_em() << std::endl;

    // Check glyph mapping
    uint16_t gid_A = font->glyph_index('A');
    uint16_t gid_B = font->glyph_index('B');
    uint16_t gid_space = font->glyph_index(' ');
    assert(gid_A != 0);
    assert(gid_B != 0);
    assert(gid_space != 0);

    // Check metrics
    float adv_A = font->glyph_advance(gid_A, 16.0f);
    float adv_sp = font->glyph_advance(gid_space, 16.0f);
    assert(adv_A > 0.0f);
    assert(adv_sp > 0.0f);

    // Vector outline extraction into Nisaba Path
    Path path_A;
    bool ok_A = font->get_glyph_path(gid_A, path_A, 32.0f);
    assert(ok_A);
    assert(!path_A.is_empty());
    assert(path_A.bounds().width() > 0.0f);
    assert(path_A.bounds().height() > 0.0f);

    std::cout << "  Glyph 'A' bounds: "
              << path_A.bounds().width() << "x" << path_A.bounds().height() << std::endl;

    std::cout << "  -> PASS" << std::endl;
}

void test_bidi_and_arabic() {
    std::cout << "[Test] BiDi Analysis & Arabic Contextual Shaping..." << std::endl;

    // 1. Direction detection
    assert(Bidi::detect_base_direction("Hello World") == Direction::LeftToRight);
    assert(Bidi::detect_base_direction("مرحبا بالعالم") == Direction::RightToLeft);

    // 2. Run segmentation
    std::string mixed = "English مرحبا بالعالم English";
    auto runs = Bidi::segment_runs(mixed);
    assert(runs.size() >= 3);
    assert(!runs[0].is_rtl());
    assert(runs[1].is_rtl());
    assert(!runs[2].is_rtl());

    // 3. Arabic Contextual Shaping
    std::string arabic_word = "سلام"; // Sin, Lam, Alif, Mim
    auto shaped = Bidi::shape_text(arabic_word, Bidi::segment_runs(arabic_word));
    assert(!shaped.empty());

    // In 'سلام', 'ل' + 'ا' forms Lam-Alif ligature (0xFEFB or 0xFEFC)!
    // Sin at start should be initial form (0xFEB3)
    // Lam-Alif in middle should be final form ligature (0xFEF6 or 0xFEFC)
    // Mim at end should be isolated (0xFEE1)
    bool has_lam_alif = false;
    for (const auto& sc : shaped) {
        if (sc.codepoint == 0xFEFB || sc.codepoint == 0xFEFC) {
            has_lam_alif = true;
        }
    }
    assert(has_lam_alif);

    std::cout << "  Arabic shaping successfully synthesized ligatures and forms." << std::endl;
    std::cout << "  -> PASS" << std::endl;
}

void test_font_system() {
    std::cout << "[Test] Font System & Multilingual Fallback..." << std::endl;

    FontSystem fs;
    auto id_inter = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    auto id_arabic = fs.load_font_file(resolve_font("NotoSansArabic.ttf"));

    assert(id_inter.has_value());
    assert(id_arabic.has_value());
    assert(fs.font_count() == 2);

    // Match font
    auto [matched_id, font_ptr] = fs.match_font(Family::sans_serif());
    assert(font_ptr != nullptr);

    // Multilingual Fallback:
    // Inter does not have Arabic glyphs (e.g. 0x0628 = Ba 'ب'), but NotoSansArabic does!
    auto [actual_fid, gid] = fs.find_glyph_or_fallback(*id_inter, 0x0628);
    assert(actual_fid == *id_arabic);
    assert(gid != 0);

    std::cout << "  Successfully resolved Arabic fallback glyph from NotoSansArabic." << std::endl;
    std::cout << "  -> PASS" << std::endl;
}

void test_buffer_layout() {
    std::cout << "[Test] Multi-line Buffer Layout & Word Wrapping..." << std::endl;

    FontSystem fs;
    fs.load_font_file(resolve_font("Inter-Regular.ttf"));

    Buffer buffer(Metrics(16.0f, 22.0f));
    buffer.set_text("First line of text.\nSecond line with several words that should wrap when narrow.");

    // Unconstrained layout
    buffer.shape_until_scroll(fs);
    auto runs = buffer.layout_runs();
    assert(runs.size() == 2);

    // Apply width constraint
    buffer.set_size(150.0f, std::nullopt);
    buffer.set_wrap(Wrap::Word);
    buffer.shape_until_scroll(fs);

    auto wrapped_runs = buffer.layout_runs();
    assert(wrapped_runs.size() > 2); // Should have wrapped into multiple lines!

    std::cout << "  Unconstrained lines: " << runs.size()
              << ", Wrapped lines at 150px: " << wrapped_runs.size() << std::endl;

    std::cout << "  -> PASS" << std::endl;
}

void test_rendering_to_canvas() {
    std::cout << "[Test] Canvas Text Rendering with Alpha Masks..." << std::endl;

    FontSystem fs;
    fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    fs.load_font_file(resolve_font("NotoSansArabic.ttf"));

    GlyphCache cache;

    auto pixmap = Pixmap::allocate(400, 200);
    assert(pixmap.has_value());

    Canvas canvas(*pixmap);
    canvas.clear(Color::from_rgba8(20, 24, 32, 255));

    Buffer buffer(Metrics(20.0f, 26.0f));
    Attrs title_attrs;
    title_attrs.set_color(TextColor::rgb(0, 210, 255));

    buffer.set_text("Nisaba Native Engine\nمحرك سيادي متكامل", title_attrs);
    buffer.draw(canvas, cache, fs, Color::WHITE, 20.0f, 20.0f);

    assert(cache.size() > 0);
    std::cout << "  Cached unique glyphs: " << cache.size() << std::endl;

    // Check that some pixels were rendered
    bool has_drawn_pixels = false;
    for (uint32_t y = 0; y < pixmap->height(); ++y) {
        for (uint32_t x = 0; x < pixmap->width(); ++x) {
            auto p = pixmap->pixel(x, y);
            if (p && (p->red() > 30 || p->green() > 30 || p->blue() > 40)) {
                has_drawn_pixels = true;
                break;
            }
        }
        if (has_drawn_pixels) break;
    }
    assert(has_drawn_pixels);

    std::cout << "  -> PASS" << std::endl;
}

void test_cursor_and_selection() {
    std::cout << "[Test] Interactive Cursor & Hit-Testing..." << std::endl;

    FontSystem fs;
    fs.load_font_file(resolve_font("Inter-Regular.ttf"));

    Buffer buffer(Metrics(16.0f, 20.0f));
    buffer.set_text("Hello World\nLine Two");
    buffer.shape_until_scroll(fs);

    // Hit test near top-left
    Cursor c_start = CursorController::hit_test(buffer, 0.0f, 5.0f);
    assert(c_start.line == 0);
    assert(c_start.index == 0);

    // Move cursor right
    Cursor c_next = CursorController::move(buffer, c_start, Motion::Right);
    assert(c_next.line == 0);
    assert(c_next.index == 1);

    // Move cursor down
    Cursor c_down = CursorController::move(buffer, c_next, Motion::Down);
    assert(c_down.line == 1);

    // Selection
    Selection sel(c_start, c_next);
    assert(!sel.is_empty());
    assert(sel.normalized().start.index <= sel.normalized().end.index);

    std::cout << "  -> PASS" << std::endl;
}

void test_multilingual_cjk_russian() {
    std::cout << "[Test] Global Multilingual Support (Japanese, Chinese, Russian, Korean)..." << std::endl;

    FontSystem fs;
    auto inter_id = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    auto arabic_id = fs.load_font_file(resolve_font("NotoSansArabic.ttf"));
    auto cjk_id = fs.load_font_file(resolve_font("DroidSansFallbackFull.ttf"));

    assert(inter_id.has_value());
    assert(arabic_id.has_value());
    assert(cjk_id.has_value());

    // 1. Russian (Cyrillic)
    const TtfFont* inter_font = fs.get_font(*inter_id);
    assert(inter_font != nullptr);
    uint16_t gid_cyr = inter_font->glyph_index(0x041F); // 'П'
    assert(gid_cyr != 0);

    // 2. Japanese & Chinese & Korean (CJK)
    const TtfFont* cjk_font = fs.get_font(*cjk_id);
    assert(cjk_font != nullptr);
    uint16_t gid_jp_ko = cjk_font->glyph_index(0x3053); // Hiragana 'こ'
    uint16_t gid_zh_ni = cjk_font->glyph_index(0x4F60); // Hanzi '你'
    uint16_t gid_kr_ga = cjk_font->glyph_index(0xAC00); // Hangul '가'
    assert(gid_jp_ko != 0);
    assert(gid_zh_ni != 0);
    assert(gid_kr_ga != 0);

    // 3. CJK Line Wrapping test (spaceless sentence should wrap at character boundaries!)
    Buffer cjk_buffer(Metrics(16.0f, 22.0f));
    std::string spaceless_jp = "今日は世界中のロボットと組み込みシステムを制御する日です。";
    cjk_buffer.set_text(spaceless_jp);
    cjk_buffer.set_size(120.0f, std::nullopt); // Narrow width
    cjk_buffer.set_wrap(Wrap::Word);
    cjk_buffer.shape_until_scroll(fs);

    auto runs = cjk_buffer.layout_runs();
    assert(runs.size() > 1); // Must have wrapped across multiple lines despite having NO spaces!

    // 4. Multilingual mixed rendering to Canvas
    auto pixmap = Pixmap::allocate(500, 250);
    assert(pixmap.has_value());
    Canvas canvas(*pixmap);
    canvas.clear(Color::BLACK);

    GlyphCache cache;
    Buffer mixed_buffer(Metrics(16.0f, 22.0f));
    std::string mixed_text =
        "English: Sovereign Robot\n"
        "Русский: Робототехника\n"
        "日本語: ロボット工学\n"
        "中文: 智能机器人操作系统\n"
        "العربية: محرك الأنظمة الذكية";
    mixed_buffer.set_text(mixed_text);
    mixed_buffer.draw(canvas, cache, fs, Color::WHITE, 20.0f, 20.0f);

    assert(cache.size() >= 10);
    std::cout << "  Rendered " << runs.size() << " CJK lines, and cached "
              << cache.size() << " global glyphs across all scripts." << std::endl;
    std::cout << "  -> PASS" << std::endl;
}

void test_extended_arabic_tashkeel_and_mirroring() {
    std::cout << "[Test] Extended Arabic/Persian/Urdu, Tashkeel Diacritics & RTL Mirroring..." << std::endl;

    // 1. RTL Bracket Mirroring
    assert(Bidi::mirror_char('(') == ')');
    assert(Bidi::mirror_char(')') == '(');
    assert(Bidi::mirror_char('[') == ']');
    assert(Bidi::mirror_char(0x00AB) == 0x00BB); // « -> »

    std::string rtl_parens = "(مرحبا)";
    auto shaped_parens = Bidi::shape_text(rtl_parens, Bidi::segment_runs(rtl_parens));
    assert(!shaped_parens.empty());
    // In visual order (left-to-right), the closing parenthesis of RTL '(مرحبا)' is at index 0 and mirrored to '('
    assert(shaped_parens.front().codepoint == '(');
    assert(shaped_parens.back().codepoint == ')');

    // 2. Persian / Urdu extended letters (پ چ گ ک ی)
    std::string persian = "پژوهشگر";
    auto shaped_persian = Bidi::shape_text(persian, Bidi::segment_runs(persian));
    assert(shaped_persian.size() == 7);
    // Verify Pe (0x067E) at initial position shaped to 0xFB58
    bool found_shaped_pe = false;
    for (const auto& sc : shaped_persian) {
        if (sc.original_codepoint == 0x067E && sc.codepoint == 0xFB58) {
            found_shaped_pe = true;
        }
    }
    assert(found_shaped_pe);

    // 3. Arabic with Tashkeel (Harakat) - marks must not break contextual joining!
    std::string vocalized = "سَلَامٌ"; // Sin + Fatha + Lam + Fatha + Alif + Mim + Dammatan
    auto shaped_voc = Bidi::shape_text(vocalized, Bidi::segment_runs(vocalized));
    bool found_lam_alif_with_tashkeel = false;
    size_t mark_count = 0;
    for (const auto& sc : shaped_voc) {
        if (sc.codepoint == 0xFEFB || sc.codepoint == 0xFEFC) {
            found_lam_alif_with_tashkeel = true;
        }
        if (sc.is_mark) {
            mark_count++;
        }
    }
    assert(found_lam_alif_with_tashkeel);
    assert(mark_count == 3);

    std::cout << "  Persian/Urdu joining, RTL bracket mirroring, and Tashkeel-preserving Lam-Alif verified." << std::endl;
    std::cout << "  -> PASS" << std::endl;
}

void test_rich_typography_and_layout() {
    std::cout << "[Test] Rich Typography (Justify, Ellipsis, Spans, Synthetic Bold/Italic, Decorations)..." << std::endl;

    FontSystem fs;
    auto fid = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    assert(fid.has_value());
    const TtfFont* font = fs.get_font(*fid);
    assert(font != nullptr);

    // 1. Synthetic Bold & Italic outlines
    uint16_t gid_H = font->glyph_index('H');
    Path normal_path, bold_path, italic_path;
    assert(font->get_glyph_path_styled(gid_H, normal_path, 32.0f, false, false));
    assert(font->get_glyph_path_styled(gid_H, bold_path, 32.0f, true, false));
    assert(font->get_glyph_path_styled(gid_H, italic_path, 32.0f, false, true));
    assert(bold_path.bounds().width() > normal_path.bounds().width());
    assert(italic_path.bounds().width() > normal_path.bounds().width());

    // 2. Full Justification (Align::Justified)
    Buffer just_buf(Metrics(16.0f, 22.0f));
    just_buf.set_size(220.0f, std::nullopt);
    just_buf.set_wrap(Wrap::Word);
    just_buf.set_align(Align::Justified);
    just_buf.set_text(
        "Nisaba engine supports full text justification across wrapped paragraph lines with ease."
    );
    just_buf.shape_until_scroll(fs);
    auto just_runs = just_buf.layout_runs();
    assert(just_runs.size() >= 2);
    // First wrapped line should be stretched close to 220px
    assert(just_runs[0].line_width > 200.0f);

    // 3. Overflow::Ellipsis + max_lines
    Buffer ell_buf(Metrics(16.0f, 22.0f));
    ell_buf.set_size(140.0f, std::nullopt);
    ell_buf.set_wrap(Wrap::Word);
    ell_buf.set_overflow(Overflow::Ellipsis);
    ell_buf.set_max_lines(1);
    ell_buf.set_text("This is a very long sentence that should be truncated with an ellipsis");
    ell_buf.shape_until_scroll(fs);
    auto ell_runs = ell_buf.layout_runs();
    assert(ell_runs.size() == 1);
    assert(!ell_runs[0].glyphs.empty());

    // 4. Rich inline spans (font_size, letter_spacing, underline, strikethrough, background)
    Attrs base_attrs;
    AttrsList rich_list(base_attrs);
    Attrs span1;
    span1.set_font_size(24.0f)
         .set_letter_spacing(2.0f)
         .set_underline(true)
         .set_strikethrough(true)
         .set_background_color(TextColor::rgb(60, 80, 120))
         .set_weight(Weight::Bold)
         .set_style(Style::Italic);
    rich_list.add_span(0, 6, span1);

    Buffer rich_buf(Metrics(16.0f, 26.0f));
    rich_buf.set_text("Nisaba Rich Typography", base_attrs);
    rich_buf.lines_mut()[0].attrs_list_mut() = rich_list;
    rich_buf.shape_until_scroll(fs);

    auto pixmap = Pixmap::allocate(400, 80);
    assert(pixmap.has_value());
    Canvas canvas(*pixmap);
    canvas.clear(Color::BLACK);
    GlyphCache cache(128);
    rich_buf.draw(canvas, cache, fs, Color::WHITE, 10.0f, 10.0f);
    assert(cache.size() > 0);

    std::cout << "  -> PASS" << std::endl;
}

void test_grapheme_word_cursor_and_selection_rects() {
    std::cout << "[Test] Grapheme Cluster Cursor, Word Motions & Selection Rects..." << std::endl;

    FontSystem fs;
    fs.load_font_file(resolve_font("Inter-Regular.ttf"));

    // 1. Grapheme cluster navigation over combining mark ('e' + U+0301 combining acute accent)
    std::string combining_text = "e\xCC\x81x"; // 'e' (1 byte) + U+0301 (2 bytes) + 'x' (1 byte) = 4 bytes
    assert(Bidi::next_grapheme_cluster(combining_text, 0) == 3);
    assert(Bidi::prev_grapheme_cluster(combining_text, 3) == 0);

    // 2. Word navigation & selection geometry
    Buffer buf(Metrics(16.0f, 22.0f));
    buf.set_text("Hello Sovereign World");
    buf.shape_until_scroll(fs);

    Cursor c0{0, 0};
    Cursor c_word1 = CursorController::move(buf, c0, Motion::WordRight);
    assert(c_word1.index == 5); // End of "Hello"

    Cursor c_word2 = CursorController::move(buf, c_word1, Motion::WordRight);
    assert(c_word2.index == 15); // End of "Sovereign"

    Cursor c_back = CursorController::move(buf, c_word2, Motion::WordLeft);
    assert(c_back.index == 6); // Start of "Sovereign"

    Rect caret = CursorController::cursor_rect(buf, c_word1);
    assert(caret.x() > 0.0f);
    assert(caret.height() == 22.0f);

    Selection sel(c0, c_word1);
    auto rects = CursorController::selection_rects(buf, sel);
    assert(rects.size() == 1);
    assert(rects[0].width() > 10.0f);

    std::cout << "  -> PASS" << std::endl;
}

void test_p16_partb_and_p17_text_features() {
    std::cout << "[Test] P1.6-PartB & P1.7 Align::Start, Queries, Decorations, Shadows & Aliases..." << std::endl;

    FontSystem fs;
    auto fid_inter = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    auto fid_arabic = fs.load_font_file(resolve_font("NotoSansArabic.ttf"));
    assert(fid_inter.has_value() && fid_arabic.has_value());

    // 1. Align::Start resolves to Left for LTR and Right for RTL base direction
    {
        Buffer ltr_buf(Metrics(18.0f, 24.0f));
        ltr_buf.set_size(240.0f, std::nullopt);
        ltr_buf.set_align(Align::Start);
        ltr_buf.set_text("Hello");
        ltr_buf.shape_until_scroll(fs);
        auto ltr_runs = ltr_buf.layout_runs();
        assert(!ltr_runs.empty() && !ltr_runs[0].glyphs.empty());
        assert(ltr_runs[0].line_width < 120.0f);
        assert(ltr_runs[0].glyphs.front().x < 4.0f);

        Buffer rtl_buf(Metrics(18.0f, 24.0f));
        rtl_buf.set_size(240.0f, std::nullopt);
        rtl_buf.set_align(Align::Start);
        rtl_buf.set_text("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"); // "مرحبا"
        rtl_buf.shape_until_scroll(fs);
        auto rtl_runs = rtl_buf.layout_runs();
        assert(!rtl_runs.empty() && !rtl_runs[0].glyphs.empty());
        assert(rtl_runs[0].line_width < 180.0f);
        assert(rtl_runs[0].glyphs.front().x > 100.0f);

        Buffer rtl_left(Metrics(18.0f, 24.0f));
        rtl_left.set_size(240.0f, std::nullopt);
        rtl_left.set_align(Align::Left);
        rtl_left.set_text("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
        rtl_left.shape_until_scroll(fs);
        assert(rtl_left.layout_runs()[0].glyphs.front().x < 4.0f);
    }

    // 2. max_intrinsic_width vs wrapped longest_line_width, word_range_at, range_rects, glyph_position_at
    {
        Buffer qbuf(Metrics(16.0f, 24.0f));
        const std::string sample = "Alpha Beta\nGamma Delta";
        qbuf.set_text(sample);
        qbuf.shape_until_scroll(fs);
        const float intrinsic_nl = qbuf.max_intrinsic_width(fs);
        assert(intrinsic_nl > 60.0f);

        Buffer wbuf(Metrics(16.0f, 24.0f));
        wbuf.set_text("Alpha Beta Gamma Delta Sovereign Typography");
        wbuf.set_size(120.0f, std::nullopt);
        wbuf.set_wrap(Wrap::Word);
        wbuf.shape_until_scroll(fs);
        assert(wbuf.layout_runs().size() >= 2);
        const float longest_wrapped = wbuf.longest_line_width();
        const float max_intrinsic = wbuf.max_intrinsic_width(fs);
        assert(longest_wrapped <= 125.0f);
        assert(max_intrinsic > longest_wrapped + 40.0f);

        // word_range_at
        TextRange w0 = qbuf.word_range_at(2); // inside "Alpha"
        assert(w0.start == 0 && w0.end == 5);
        assert(sample.substr(w0.start, w0.length()) == "Alpha");

        const size_t delta_pos = sample.find("Delta");
        TextRange w1 = qbuf.word_range_at(delta_pos + 2);
        assert(sample.substr(w1.start, w1.length()) == "Delta");

        // range_rects (Max vs Tight height styles across two lines)
        auto r_max = qbuf.range_rects(0, sample.size(), 10.0f, 20.0f, RectHeightStyle::Max);
        auto r_tight = qbuf.range_rects(0, sample.size(), 10.0f, 20.0f, RectHeightStyle::Tight);
        assert(r_max.size() >= 2 && r_tight.size() >= 2);
        assert(r_max[0].x() >= 9.9f && r_max[0].y() >= 19.9f);
        assert(r_max[0].height() >= r_tight[0].height() - 0.1f);
        assert(r_max[1].y() > r_max[0].y());

        // glyph_position_at hit testing on line 0 and line 1
        auto gp0 = qbuf.glyph_position_at(
            r_max[0].x() + r_max[0].width() * 0.25f,
            r_max[0].y() + r_max[0].height() * 0.5f,
            10.0f, 20.0f
        );
        assert(gp0.position <= 10);

        auto gp1 = qbuf.glyph_position_at(
            r_max[1].x() + r_max[1].width() * 0.5f,
            r_max[1].y() + r_max[1].height() * 0.5f,
            10.0f, 20.0f
        );
        assert(gp1.position >= 11);
    }

    // 3. FontSystem explicit aliases via load_font_file, load_font_data & register_family_alias
    {
        FontSystem alias_fs;
        auto f_inter = alias_fs.load_font_file(resolve_font("Inter-Regular.ttf"), 0, "BrandSans");
        assert(f_inter.has_value());

        std::ifstream fstream(resolve_font("FiraMono-Medium.ttf"), std::ios::binary | std::ios::ate);
        assert(fstream.is_open());
        const auto sz = static_cast<size_t>(fstream.tellg());
        fstream.seekg(0, std::ios::beg);
        std::vector<uint8_t> mono_bytes(sz);
        fstream.read(reinterpret_cast<char*>(mono_bytes.data()), static_cast<std::streamsize>(sz));
        assert(fstream.good());

        auto f_mono = alias_fs.load_font_data(mono_bytes, 0, "BrandMonoMem");
        assert(f_mono.has_value());

        alias_fs.register_family_alias("UiBody", "BrandSans");
        alias_fs.register_family_alias("UiMonoId", *f_mono);

        auto m1 = alias_fs.match_font_styled(Family("BrandSans"));
        auto m2 = alias_fs.match_font_styled(Family("UiBody"));
        auto m3 = alias_fs.match_font_styled(Family("BrandMonoMem"));
        auto m4 = alias_fs.match_font_styled(Family("UiMonoId"));
        assert(m1.font != nullptr && m1.font_id == *f_inter);
        assert(m2.font != nullptr && m2.font_id == *f_inter);
        assert(m3.font != nullptr && m3.font_id == *f_mono);
        assert(m4.font != nullptr && m4.font_id == *f_mono);
    }

    // 4. CPU rendering of overline, 5 decoration styles, custom decoration color/thickness, and sharp + blurred shadows
    {
        Buffer dec_buf(Metrics(22.0f, 32.0f));
        Attrs base;
        base.set_color(TextColor::rgb(255, 255, 255));
        dec_buf.set_text("OOO DDD TTT AAA WWW SHW", base);
        auto& alist = dec_buf.lines_mut()[0].attrs_list_mut();

        Attrs a_over = base;
        a_over.set_overline(true).set_decoration_color(TextColor::rgb(255, 50, 50)).set_decoration_thickness(1.5f);
        Attrs a_dbl = base;
        a_dbl.set_underline(true).set_decoration_style(DecorationStyle::Double).set_decoration_color(TextColor::rgb(50, 255, 50));
        Attrs a_dot = base;
        a_dot.set_underline(true).set_decoration_style(DecorationStyle::Dotted).set_decoration_color(TextColor::rgb(50, 240, 255));
        Attrs a_dash = base;
        a_dash.set_underline(true).set_decoration_style(DecorationStyle::Dashed).set_decoration_color(TextColor::rgb(255, 230, 40));
        Attrs a_wavy = base;
        a_wavy.set_underline(true).set_decoration_style(DecorationStyle::Wavy).set_decoration_color(TextColor::rgb(255, 60, 255));
        Attrs a_shw = base;
        a_shw.add_shadow(TextColor::rgb(0, 220, 0), 5.0f, 4.0f, 0.0f)
             .add_shadow(TextColor::rgb(0, 140, 255), -4.0f, 7.0f, 2.5f);

        alist.add_span(0, 3, a_over);
        alist.add_span(4, 7, a_dbl);
        alist.add_span(8, 11, a_dot);
        alist.add_span(12, 15, a_dash);
        alist.add_span(16, 19, a_wavy);
        alist.add_span(20, 23, a_shw);
        dec_buf.shape_until_scroll(fs);

        auto pixmap = Pixmap::allocate(480, 90);
        assert(pixmap.has_value());
        Canvas canvas(*pixmap);
        canvas.clear(Color::BLACK);
        GlyphCache cache(128);
        dec_buf.draw(canvas, cache, fs, Color::WHITE, 12.0f, 16.0f);

        size_t red_px = 0, green_px = 0, cyan_px = 0, yellow_px = 0, mag_px = 0, blue_glow_px = 0;
        const auto pref = pixmap->as_ref();
        for (uint32_t y = 0; y < 90; ++y) {
            for (uint32_t x = 0; x < 480; ++x) {
                auto p = pref.pixel(x, y);
                if (!p) continue;
                if (p->r > 180 && p->g < 80 && p->b < 80) ++red_px;
                if (p->g > 150 && p->r < 80 && p->b < 80) ++green_px;
                if (p->g > 180 && p->b > 180 && p->r < 80) ++cyan_px;
                if (p->r > 180 && p->g > 160 && p->b < 80) ++yellow_px;
                if (p->r > 180 && p->b > 180 && p->g < 80) ++mag_px;
                if (p->b > 20 && p->b < 200 && p->r < 25) ++blue_glow_px;
            }
        }
        assert(red_px > 20);
        assert(green_px > 40);
        assert(cyan_px > 8);
        assert(yellow_px > 12);
        assert(mag_px > 20);
        assert(blue_glow_px > 30);
    }

    std::cout << "  -> PASS" << std::endl;
}

void test_phase1_metrics_ellipsis_and_direction() {
    std::cout << "[Test] Phase 1 Extended Metrics, Custom Ellipsis & Explicit TextDirection..." << std::endl;

    FontSystem fs;
    fs.set_auto_system_fonts(false);
    auto fid_inter = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    auto fid_arabic = fs.load_font_file(resolve_font("NotoSansArabic.ttf"));
    assert(fid_inter.has_value() && fid_arabic.has_value());

    // 1. min_intrinsic_width vs max_intrinsic_width (Latin, Arabic, Wrap::Glyph, Wrap::None)
    {
        Buffer word_probe(Metrics(18.0f, 24.0f));
        word_probe.set_text("Supercalifragilistic");
        word_probe.shape_until_scroll(fs);
        const float expected_widest = word_probe.max_line_width();

        Buffer latin_buf(Metrics(18.0f, 24.0f));
        latin_buf.set_wrap(Wrap::Word);
        latin_buf.set_text("Short Supercalifragilistic Tiny");
        const float min_w = latin_buf.min_intrinsic_width(fs);
        const float max_w = latin_buf.max_intrinsic_width(fs);
        assert(min_w > 0.0f && min_w < max_w);
        assert(std::abs(min_w - expected_widest) < 0.5f);

        latin_buf.set_wrap(Wrap::Glyph);
        const float glyph_min_w = latin_buf.min_intrinsic_width(fs);
        assert(glyph_min_w > 0.0f && glyph_min_w < min_w);

        latin_buf.set_wrap(Wrap::None);
        assert(std::abs(latin_buf.min_intrinsic_width(fs) - max_w) < 0.5f);

        Buffer ar_buf(Metrics(18.0f, 28.0f));
        ar_buf.set_wrap(Wrap::Word);
        ar_buf.set_text("مرحبا بكم في محرك نيسابا المتكامل");
        const float ar_min = ar_buf.min_intrinsic_width(fs);
        const float ar_max = ar_buf.max_intrinsic_width(fs);
        assert(ar_min > 10.0f && ar_min < ar_max);
    }

    // 2. line_metrics(), alphabetic_baseline(), ideographic_baseline(), hard_break, left
    {
        Buffer buf(Metrics(16.0f, 24.0f));
        buf.set_size(150.0f, std::nullopt);
        buf.set_wrap(Wrap::Word);
        buf.set_align(Align::Center);
        const std::string text = "First\nSecond line wrapping across multiple lines\n\nEnd";
        buf.set_text(text);
        buf.shape_until_scroll(fs);

        assert(!buf.did_exceed_max_lines());
        auto lm = buf.line_metrics();
        assert(lm.size() >= 5);

        for (size_t i = 0; i < lm.size(); ++i) {
            assert(lm[i].line_number == i);
            assert(lm[i].ascent > 0.0f);
            assert(lm[i].descent > 0.0f);
            assert(lm[i].unscaled_ascent > 0.0f);
            assert(std::abs(lm[i].height - 24.0f) < 0.01f);
            assert(lm[i].start_index <= lm[i].end_index);
            if (i > 0) {
                assert(lm[i].baseline > lm[i - 1].baseline);
                assert(lm[i].start_index >= lm[i - 1].end_index);
            }
        }
        // First line ("First") is centered inside 150px and ends with a hard newline break
        assert(lm[0].start_index == 0 && lm[0].end_index == 5);
        assert(lm[0].hard_break);
        assert(lm[0].left > 20.0f);
        // Wrapped paragraph lines: first visual line of paragraph 2 is a soft wrap (hard_break == false)
        assert(!lm[1].hard_break);
        // Final line ("End") ends with hard_break == true
        assert(lm.back().hard_break);

        assert(std::abs(buf.alphabetic_baseline() - lm[0].baseline) < 0.01f);
        assert(std::abs(buf.ideographic_baseline() - (lm[0].baseline + lm[0].descent)) < 0.01f);
    }

    // 3. did_exceed_max_lines() persistence across repeated shape/draw + height/Wrap::None overflow
    {
        Buffer buf(Metrics(16.0f, 22.0f));
        buf.set_size(140.0f, std::nullopt);
        buf.set_wrap(Wrap::Word);
        buf.set_overflow(Overflow::Ellipsis);
        buf.set_max_lines(2);
        buf.set_text("One two three four five six seven eight nine ten eleven twelve");
        buf.shape_until_scroll(fs);
        assert(buf.did_exceed_max_lines());
        assert(buf.line_metrics().size() == 2);
        assert(!buf.line_metrics().back().hard_break);

        // Must remain true after subsequent shape_until_scroll / draw
        buf.shape_until_scroll(fs);
        assert(buf.did_exceed_max_lines());

        // Non-overflowing text
        buf.set_max_lines(10);
        buf.shape_until_scroll(fs);
        assert(!buf.did_exceed_max_lines());

        // Single-line Wrap::None + Ellipsis overflow
        Buffer single(Metrics(16.0f, 22.0f));
        single.set_size(100.0f, std::nullopt);
        single.set_wrap(Wrap::None);
        single.set_overflow(Overflow::Ellipsis);
        single.set_text("Very long single line that exceeds one hundred pixels");
        single.shape_until_scroll(fs);
        assert(single.did_exceed_max_lines());
        assert(single.max_intrinsic_width(fs) > 150.0f);

        // Height constraint + Overflow::Clip
        Buffer clipped(Metrics(16.0f, 20.0f));
        clipped.set_size(120.0f, 40.0f);
        clipped.set_wrap(Wrap::Word);
        clipped.set_overflow(Overflow::Clip);
        clipped.set_text("Line one\nLine two\nLine three\nLine four");
        clipped.shape_until_scroll(fs);
        assert(clipped.did_exceed_max_lines());
        assert(clipped.line_metrics().size() == 2);
    }

    // 4. unresolved_glyphs() detection
    {
        FontSystem latin_only_fs;
        latin_only_fs.set_auto_system_fonts(false);
        assert(latin_only_fs.load_font_file(resolve_font("Inter-Regular.ttf")).has_value());

        Buffer clean(Metrics(16.0f, 22.0f));
        clean.set_text("Hello\tWorld");
        clean.shape_until_scroll(latin_only_fs);
        assert(clean.unresolved_glyphs() == 0);

        Buffer missing_pua(Metrics(16.0f, 22.0f));
        missing_pua.set_text("Hi \xE1\x88\x80\xE1\x88\x81\xE1\x88\x82"); // U+1200..U+1202 (unmapped in Inter-Regular)
        missing_pua.shape_until_scroll(latin_only_fs);
        assert(missing_pua.unresolved_glyphs() == 3);

        Buffer ar_missing(Metrics(16.0f, 24.0f));
        ar_missing.set_text("مرحبا");
        ar_missing.shape_until_scroll(latin_only_fs);
        assert(ar_missing.unresolved_glyphs() > 0);

        assert(latin_only_fs.load_font_file(resolve_font("NotoSansArabic.ttf")).has_value());
        ar_missing.reset_shaping();
        ar_missing.shape_until_scroll(latin_only_fs);
        assert(ar_missing.unresolved_glyphs() == 0);
    }

    // 5. Custom Ellipsis (Latin "... more", Arabic "… المزيد", empty "")
    {
        Buffer custom_en(Metrics(18.0f, 24.0f));
        custom_en.set_size(170.0f, std::nullopt);
        custom_en.set_wrap(Wrap::Word);
        custom_en.set_overflow(Overflow::Ellipsis);
        custom_en.set_max_lines(1);
        custom_en.set_ellipsis("... more");
        assert(custom_en.has_custom_ellipsis());
        assert(custom_en.ellipsis() == "... more");
        custom_en.set_text("Nisaba sovereign text layout engine with custom ellipsis string");
        custom_en.shape_until_scroll(fs);
        assert(custom_en.did_exceed_max_lines());
        auto runs_en = custom_en.layout_runs();
        assert(runs_en.size() == 1);
        assert(runs_en[0].line_width <= 170.5f);
        // Trailing synthetic glyphs correspond to "... more" (8 glyphs)
        size_t synth_count = 0;
        for (const auto& g : runs_en[0].glyphs) {
            if (g.start == g.end) ++synth_count;
        }
        assert(synth_count == 8);

        // Arabic RTL custom ellipsis ("… المزيد") placed at visual left (start of visual glyph array)
        Buffer custom_ar(Metrics(18.0f, 28.0f));
        custom_ar.set_size(200.0f, std::nullopt);
        custom_ar.set_wrap(Wrap::Word);
        custom_ar.set_align(Align::Start);
        custom_ar.set_overflow(Overflow::Ellipsis);
        custom_ar.set_max_lines(1);
        custom_ar.set_ellipsis("\xE2\x80\xA6 \xD8\xA7\xD9\x84\xD9\x85\xD8\xB2\xD9\x8A\xD8\xAF");
        custom_ar.set_text("مرحبا بكم في محرك نيسابا المتكامل للنصوص العربية والطباعة المتقدمة");
        custom_ar.shape_until_scroll(fs);
        assert(custom_ar.did_exceed_max_lines());
        auto runs_ar = custom_ar.layout_runs();
        assert(runs_ar.size() == 1);
        assert(runs_ar[0].line_width <= 200.5f);
        assert(!runs_ar[0].glyphs.empty());
        // Ellipsis glyphs sit on the visual LEFT of the RTL line (start == end)
        assert(runs_ar[0].glyphs.front().start == runs_ar[0].glyphs.front().end);
        // Logical start of Arabic text remains on the visual RIGHT (start < end)
        assert(runs_ar[0].glyphs.back().start < runs_ar[0].glyphs.back().end);

        // Empty custom ellipsis truncates cleanly with zero synthetic glyphs
        custom_en.set_ellipsis("");
        custom_en.shape_until_scroll(fs);
        assert(custom_en.did_exceed_max_lines());
        size_t synth_empty = 0;
        for (const auto& g : custom_en.layout_runs()[0].glyphs) {
            if (g.start == g.end) ++synth_empty;
        }
        assert(synth_empty == 0);
        custom_en.clear_ellipsis();
        assert(!custom_en.has_custom_ellipsis());
    }

    // 6. Explicit TextDirection (LTR / RTL / Auto) independent of Align & BiDi ordering
    {
        // Forced RTL on pure ASCII aligns right under Align::Start and left under Align::End
        Buffer ascii_rtl(Metrics(18.0f, 24.0f));
        ascii_rtl.set_size(260.0f, std::nullopt);
        ascii_rtl.set_align(Align::Start);
        ascii_rtl.set_text_direction(TextDirection::RTL);
        ascii_rtl.set_text("Hello");
        ascii_rtl.shape_until_scroll(fs);
        auto lm_rtl_start = ascii_rtl.line_metrics();
        assert(lm_rtl_start.size() == 1 && lm_rtl_start[0].left > 100.0f);

        ascii_rtl.set_align(Align::End);
        ascii_rtl.shape_until_scroll(fs);
        auto lm_rtl_end = ascii_rtl.line_metrics();
        assert(lm_rtl_end.size() == 1 && lm_rtl_end[0].left < 1.0f);

        // Forced LTR on Arabic overrides auto-RTL for Align::Start
        Buffer ar_ltr(Metrics(18.0f, 26.0f));
        ar_ltr.set_size(260.0f, std::nullopt);
        ar_ltr.set_align(Align::Start);
        ar_ltr.set_text_direction(TextDirection::LTR);
        ar_ltr.set_text("مرحبا");
        ar_ltr.shape_until_scroll(fs);
        assert(ar_ltr.line_metrics()[0].left < 1.0f);

        // Mixed BiDi "مرحبا Hello": RTL base puts Latin left of Arabic; LTR base puts Arabic left of Latin
        const std::string bidi_mix = "مرحبا Hello";
        const size_t hello_byte = bidi_mix.find("Hello");
        assert(hello_byte != std::string::npos);

        auto avg_x_for_range = [&](const Buffer& b, size_t lo, size_t hi) {
            float sum = 0.0f;
            size_t cnt = 0;
            for (const auto& g : b.layout_runs()[0].glyphs) {
                if (!g.is_mark && g.start >= lo && g.start < hi) {
                    sum += g.x;
                    ++cnt;
                }
            }
            assert(cnt > 0);
            return sum / static_cast<float>(cnt);
        };

        Buffer mix_rtl(Metrics(18.0f, 26.0f));
        mix_rtl.set_text_direction(TextDirection::RTL);
        mix_rtl.set_text(bidi_mix);
        mix_rtl.shape_until_scroll(fs);
        assert(avg_x_for_range(mix_rtl, hello_byte, bidi_mix.size()) < avg_x_for_range(mix_rtl, 0, hello_byte));

        Buffer mix_ltr(Metrics(18.0f, 26.0f));
        mix_ltr.set_text_direction(TextDirection::LTR);
        mix_ltr.set_text(bidi_mix);
        mix_ltr.shape_until_scroll(fs);
        assert(avg_x_for_range(mix_ltr, 0, hello_byte) < avg_x_for_range(mix_ltr, hello_byte, bidi_mix.size()));
    }

    std::cout << "  -> PASS" << std::endl;
}

void test_phase2_placeholders_rich_paint_and_gap_underline() {
    std::cout << "[Test] Phase 2 Inline Placeholders, Full Paint Spans & DecorationMode::Gaps..." << std::endl;

    FontSystem fs;
    fs.set_auto_system_fonts(false);
    auto fid_inter = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    auto fid_arabic = fs.load_font_file(resolve_font("NotoSansArabic.ttf"));
    assert(fid_inter.has_value() && fid_arabic.has_value());

    // 1. Inline Placeholders (LTR alignments, line height expansion, wrapping, min_intrinsic_width, RTL BiDi)
    {
        Buffer buf(Metrics(18.0f, 24.0f));
        Attrs base;
        base.set_color(TextColor::rgb(255, 255, 255));
        buf.add_text("Hello ", base);
        size_t p0 = buf.add_placeholder(28.0f, 28.0f, PlaceholderAlignment::Middle);
        buf.add_text(" World ", base);
        size_t p1 = buf.add_placeholder(36.0f, 18.0f, PlaceholderAlignment::AboveBaseline);
        buf.add_text(" ", base);
        size_t p2 = buf.add_placeholder(20.0f, 14.0f, PlaceholderAlignment::BelowBaseline);
        buf.add_text(" ", base);
        size_t p3 = buf.add_placeholder(22.0f, 16.0f, PlaceholderAlignment::Top);
        buf.add_text(" ", base);
        size_t p4 = buf.add_placeholder(22.0f, 16.0f, PlaceholderAlignment::Bottom);
        buf.add_text(" ", base);
        size_t p5 = buf.add_placeholder(24.0f, 20.0f, PlaceholderAlignment::Baseline, TextBaseline::Alphabetic, 12.0f);
        assert(p0 == 0 && p1 == 1 && p2 == 2 && p3 == 3 && p4 == 4 && p5 == 5);
        assert(buf.placeholder_count() == 6);

        buf.shape_until_scroll(fs);
        assert(buf.unresolved_glyphs() == 0);
        auto lm = buf.line_metrics();
        assert(lm.size() == 1);
        assert(lm[0].height >= 28.0f);

        auto prects = buf.placeholder_rects(10.0f, 20.0f);
        assert(prects.size() == 6);
        for (size_t i = 0; i < 6; ++i) {
            assert(prects[i].placeholder_index == i);
            assert(prects[i].direction == TextDirection::LTR);
            if (i > 0) assert(prects[i].rect.x() > prects[i - 1].rect.x());
        }
        assert(std::abs(prects[0].rect.width() - 28.0f) < 0.1f && std::abs(prects[0].rect.height() - 28.0f) < 0.1f);
        // AboveBaseline: bottom edge sits on alphabetic baseline
        const float baseline_y = 20.0f + lm[0].baseline;
        assert(std::abs(prects[1].rect.bottom() - baseline_y) < 0.5f);
        // BelowBaseline: top edge sits on alphabetic baseline
        assert(std::abs(prects[2].rect.top() - baseline_y) < 0.5f);
        // Top: top edge sits at line top (origin_y = 20)
        assert(std::abs(prects[3].rect.top() - 20.0f) < 0.5f);
        // Bottom: bottom edge sits at line bottom (20 + height)
        assert(std::abs(prects[4].rect.bottom() - (20.0f + lm[0].height)) < 0.5f);
        // Baseline with baseline_offset = 12: top = baseline - 12
        assert(std::abs(prects[5].rect.top() - (baseline_y - 12.0f)) < 0.5f);

        // Wrapping with tall placeholder expands ONLY the line containing the placeholder
        Buffer wrap_buf(Metrics(16.0f, 22.0f));
        wrap_buf.set_size(130.0f, std::nullopt);
        wrap_buf.set_wrap(Wrap::Word);
        wrap_buf.add_text("Line one text ");
        wrap_buf.add_placeholder(90.0f, 44.0f, PlaceholderAlignment::Middle);
        wrap_buf.add_text(" trailing");
        wrap_buf.shape_until_scroll(fs);
        auto wlm = wrap_buf.line_metrics();
        auto wpr = wrap_buf.placeholder_rects();
        assert(wlm.size() >= 2 && wpr.size() == 1);
        assert(wlm[wpr[0].line_number].height >= 44.0f);
        assert(wrap_buf.min_intrinsic_width(fs) >= 90.0f);

        // Arabic RTL inline placeholder inherits RTL direction and sits between Arabic words
        Buffer ar_buf(Metrics(18.0f, 28.0f));
        ar_buf.set_text_direction(TextDirection::RTL);
        ar_buf.add_text("مرحبا ");
        ar_buf.add_placeholder(24.0f, 24.0f, PlaceholderAlignment::Middle);
        ar_buf.add_text(" بالعالم");
        ar_buf.shape_until_scroll(fs);
        assert(ar_buf.unresolved_glyphs() == 0);
        auto ar_pr = ar_buf.placeholder_rects();
        assert(ar_pr.size() == 1);
        assert(ar_pr[0].direction == TextDirection::RTL);
        const auto& ar_glyphs = ar_buf.layout_runs()[0].glyphs;
        assert(ar_pr[0].rect.x() > ar_glyphs.front().x && ar_pr[0].rect.right() < ar_glyphs.back().x + ar_glyphs.back().w);
    }

    // 2. Full Paint Foreground & Background per Span on CPU Canvas
    {
        Buffer pbuf(Metrics(32.0f, 46.0f));

        auto fg_grad = LinearGradient::create(
            Point::from_xy(10.0f, 0.0f),
            Point::from_xy(170.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 20, 20, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(20, 40, 255, 255))
            }
        );
        assert(fg_grad.has_value());
        Paint grad_fg{Shader{*fg_grad}};

        Paint stroke_fg(Color::from_rgba8(30, 255, 60, 255));
        stroke_fg.set_style(PaintStyle::Stroke);
        stroke_fg.set_stroke_width(2.0f);

        auto bg_grad = LinearGradient::create(
            Point::from_xy(130.0f, 0.0f),
            Point::from_xy(235.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(240, 220, 20, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(20, 220, 240, 255))
            }
        );
        assert(bg_grad.has_value());
        Paint grad_bg{Shader{*bg_grad}};

        Attrs a_grad;
        a_grad.set_foreground_paint(grad_fg);
        Attrs a_stroke;
        a_stroke.set_foreground_paint(stroke_fg).set_background_paint(grad_bg);

        pbuf.add_text("MMMM ", a_grad);
        pbuf.add_text("OOOO", a_stroke);
        pbuf.shape_until_scroll(fs);

        auto pm = Pixmap::allocate(380, 72);
        assert(pm.has_value());
        Canvas canvas(*pm);
        canvas.clear(Color::BLACK);
        GlyphCache cache(128);
        pbuf.draw(canvas, cache, fs, Color::WHITE, 10.0f, 12.0f);

        const auto pref = pm->as_ref();
        size_t red_left = 0, blue_right = 0, green_stroke = 0, yellow_bg = 0, cyan_bg = 0;
        for (uint32_t y = 0; y < 72; ++y) {
            for (uint32_t x = 0; x < 380; ++x) {
                auto p = pref.pixel(x, y);
                if (!p) continue;
                if (x < 90 && p->r > 160 && p->b < 100) ++red_left;
                if (x >= 90 && x < 180 && p->b > 140 && p->r < 120) ++blue_right;
                if (x >= 120 && p->g > 200 && p->r < 90 && p->b < 110) ++green_stroke;
                if (x >= 120 && x < 180 && p->r > 180 && p->g > 160 && p->b < 100) ++yellow_bg;
                if (x >= 185 && p->g > 160 && p->b > 180 && p->r < 100) ++cyan_bg;
            }
        }
        assert(red_left > 50);
        assert(blue_right > 40);
        assert(green_stroke > 40);
        assert(yellow_bg > 150);
        assert(cyan_bg > 150);
    }

    // 3. Smart Underline Gap Skipping (DecorationMode::Gaps) on Latin & Arabic descenders
    {
        auto measure_underline = [&](std::string_view txt, DecorationMode mode) {
            Buffer b(Metrics(28.0f, 40.0f));
            Attrs a;
            a.set_underline(true).set_decoration_mode(mode).set_decoration_color(TextColor::rgb(255, 60, 60));
            b.set_text(txt, a);
            b.shape_until_scroll(fs);
            auto runs = b.layout_runs();
            assert(runs.size() == 1);
            return build_run_decorations(runs[0], 0.0f, 0.0f, 28.0f, Color::WHITE, &fs);
        };

        auto sum_w = [](const DecorationGeometry& d) {
            float total = 0.0f;
            for (const auto& r : d.rects) total += r.w;
            return total;
        };

        // Latin descenders: g, y, p, q carve gaps out of underline
        auto lat_through = measure_underline("agypqa", DecorationMode::Through);
        auto lat_gaps = measure_underline("agypqa", DecorationMode::Gaps);
        assert(lat_through.rects.size() == 1);
        assert(lat_gaps.rects.size() >= 3);
        assert(sum_w(lat_gaps) < sum_w(lat_through) - 8.0f);

        // Non-descender uppercase letters stay unbroken
        auto caps_gaps = measure_underline("AEIOU", DecorationMode::Gaps);
        assert(caps_gaps.rects.size() == 1);

        // Arabic descenders (ر، ي، و، ق، ع) carve gaps out of underline
        auto ar_through = measure_underline("مرحبا يرجى التوقيع", DecorationMode::Through);
        auto ar_gaps = measure_underline("مرحبا يرجى التوقيع", DecorationMode::Gaps);
        assert(ar_through.rects.size() == 1);
        assert(ar_gaps.rects.size() >= 2);
        assert(sum_w(ar_gaps) < sum_w(ar_through) - 6.0f);
    }

    std::cout << "  -> PASS" << std::endl;
}

void test_phase3_fallback_features_variable_fonts_and_strut() {
    std::cout << "[Test] Phase 3 Fallback Chains, Locale, OpenType Features, Variable Fonts & StrutStyle..." << std::endl;

    // 1. Ordered Font Family Fallback Chains & Locale ('locl')
    {
        FontSystem fs;
        fs.set_auto_system_fonts(false);
        auto id_inter = fs.load_font_file(resolve_font("Inter-Regular.ttf"), 0, "PrimaryLatin");
        auto id_arabic = fs.load_font_file(resolve_font("NotoSansArabic.ttf"), 0, "ArabicBody");
        auto id_hebrew = fs.load_font_file(resolve_font("NotoSansHebrew.ttf"), 0, "HebrewBody");
        assert(id_inter.has_value() && id_arabic.has_value() && id_hebrew.has_value());

        Attrs chain_attrs;
        chain_attrs.set_font_families({
            Family("NonExistentFont"),
            Family("PrimaryLatin"),
            Family("ArabicBody"),
            Family("HebrewBody")
        });

        Buffer b(Metrics(18.0f, 28.0f));
        b.set_text("Hello مرحبا שלום", chain_attrs);
        b.shape_until_scroll(fs);
        assert(b.unresolved_glyphs() == 0);
        bool saw_latin = false, saw_arabic = false, saw_hebrew = false;
        for (const auto& g : b.layout_runs()[0].glyphs) {
            if (g.font_id == *id_inter) saw_latin = true;
            if (g.font_id == *id_arabic) saw_arabic = true;
            if (g.font_id == *id_hebrew) saw_hebrew = true;
        }
        assert(saw_latin && saw_arabic && saw_hebrew);

        // BCP-47 to OpenType LangSys tag mapping & Romanian 'locl' substitution in Inter
        assert(OpenTypeGsub::bcp47_to_ot_lang_tag("ro") == FontFeature::make_tag('R', 'O', 'M', ' '));
        assert(OpenTypeGsub::bcp47_to_ot_lang_tag("ur") == FontFeature::make_tag('U', 'R', 'D', ' '));
        assert(OpenTypeGsub::bcp47_to_ot_lang_tag("zh-Hant") == FontFeature::make_tag('Z', 'H', 'T', ' '));
        assert(OpenTypeGsub::bcp47_to_ot_lang_tag("ja") == FontFeature::make_tag('J', 'A', 'N', ' '));

        Attrs a_en; a_en.set_family(Family("PrimaryLatin")).set_locale("en");
        Attrs a_ro; a_ro.set_family(Family("PrimaryLatin")).set_locale("ro");
        Buffer b_en(Metrics(18.0f, 24.0f)); b_en.set_text("\xC5\x9E\xC5\xA3", a_en); b_en.shape_until_scroll(fs); // Şţ
        Buffer b_ro(Metrics(18.0f, 24.0f)); b_ro.set_text("\xC5\x9E\xC5\xA3", a_ro); b_ro.shape_until_scroll(fs);
        assert(b_en.layout_runs()[0].glyphs.size() == 2 && b_ro.layout_runs()[0].glyphs.size() == 2);
        assert(b_en.layout_runs()[0].glyphs[0].glyph_id != b_ro.layout_runs()[0].glyphs[0].glyph_id);
    }

    // 2. OpenType Font Features ('tnum', 'zero', 'liga'=0 override)
    {
        FontSystem fs;
        fs.set_auto_system_fonts(false);
        auto id_inter = fs.load_font_file(resolve_font("Inter-Regular.ttf"), 0, "InterFeat");
        auto id_otf = fs.load_font_file(resolve_font("SourceSans3-Regular.otf"), 0, "SourceFeat");
        assert(id_inter.has_value() && id_otf.has_value());

        // Proportional vs Tabular numerals ('tnum')
        Attrs a_prop; a_prop.set_family(Family("InterFeat"));
        Attrs a_tnum; a_tnum.set_family(Family("InterFeat")).add_font_feature("tnum", 1);

        Buffer p1(Metrics(20.0f, 26.0f)); p1.set_text("1111", a_prop); p1.shape_until_scroll(fs);
        Buffer p8(Metrics(20.0f, 26.0f)); p8.set_text("8888", a_prop); p8.shape_until_scroll(fs);
        assert(p1.max_line_width() < p8.max_line_width() - 4.0f);

        Buffer t1(Metrics(20.0f, 26.0f)); t1.set_text("1111", a_tnum); t1.shape_until_scroll(fs);
        Buffer t8(Metrics(20.0f, 26.0f)); t8.set_text("8888", a_tnum); t8.shape_until_scroll(fs);
        assert(std::abs(t1.max_line_width() - t8.max_line_width()) < 0.25f);

        // Slashed zero ('zero')
        Attrs a_zero; a_zero.set_family(Family("InterFeat")).add_font_feature("zero", 1);
        Buffer z0(Metrics(20.0f, 26.0f)); z0.set_text("0", a_prop); z0.shape_until_scroll(fs);
        Buffer z1(Metrics(20.0f, 26.0f)); z1.set_text("0", a_zero); z1.shape_until_scroll(fs);
        assert(z0.layout_runs()[0].glyphs[0].glyph_id != z1.layout_runs()[0].glyphs[0].glyph_id);

        // Explicit 'liga' = 0 disables ligatures even when Buffer::ligatures_enabled() == true
        Attrs a_noliga; a_noliga.set_family(Family("SourceFeat")).add_font_feature("liga", 0);
        Attrs a_liga;   a_liga.set_family(Family("SourceFeat"));
        Buffer bl_on(Metrics(20.0f, 26.0f)); bl_on.set_ligatures_enabled(true); bl_on.set_text("ff", a_liga); bl_on.shape_until_scroll(fs);
        Buffer bl_off(Metrics(20.0f, 26.0f)); bl_off.set_ligatures_enabled(true); bl_off.set_text("ff", a_noliga); bl_off.shape_until_scroll(fs);
        assert(bl_on.layout_runs()[0].glyphs.size() == 1);
        assert(bl_off.layout_runs()[0].glyphs.size() == 2);
    }

    // 3. Variable Font Axes ('fvar' / 'avar' / 'gvar' / 'HVAR') for Latin & Arabic
    {
        FontSystem fs;
        fs.set_auto_system_fonts(false);
        auto id_var_en = fs.load_font_file(resolve_font("InterVariable.ttf"), 0, "InterVar");
        auto id_var_ar = fs.load_font_file(resolve_font("NotoSansArabic.ttf"), 0, "ArabicVar");
        assert(id_var_en.has_value() && id_var_ar.has_value());
        assert(fs.get_font(*id_var_en)->is_variable_font());
        assert(fs.get_font(*id_var_ar)->is_variable_font());

        auto count_ink = [&](Buffer& buf) -> size_t {
            auto pm = Pixmap::allocate(260, 72);
            assert(pm.has_value());
            Canvas c(*pm);
            c.clear(Color::BLACK);
            GlyphCache cache(128);
            buf.draw(c, cache, fs, Color::WHITE, 12.0f, 12.0f);
            size_t ink = 0;
            const auto pref = pm->as_ref();
            for (uint32_t y = 0; y < 72; ++y) {
                for (uint32_t x = 0; x < 260; ++x) {
                    if (auto p = pref.pixel(x, y); p && p->r > 80) ++ink;
                }
            }
            return ink;
        };

        // Latin InterVariable: wght=100 (Thin) vs wght=900 (Black)
        Attrs a_thin;  a_thin.set_family(Family("InterVar")).add_font_variation("wght", 100.0f);
        Attrs a_black; a_black.set_family(Family("InterVar")).add_font_variation("wght", 900.0f);
        Buffer b_thin(Metrics(32.0f, 44.0f));  b_thin.set_text("Nisaba", a_thin);   b_thin.shape_until_scroll(fs);
        Buffer b_black(Metrics(32.0f, 44.0f)); b_black.set_text("Nisaba", a_black); b_black.shape_until_scroll(fs);
        assert(b_black.max_line_width() > b_thin.max_line_width() + 4.0f);
        const size_t ink_thin = count_ink(b_thin);
        const size_t ink_black = count_ink(b_black);
        assert(ink_thin > 100 && ink_black > ink_thin * 2);

        // Arabic NotoSansArabic: wdth=62.5 (Condensed) vs wdth=100, and wght=200 vs wght=850
        Attrs ar_cond; ar_cond.set_family(Family("ArabicVar")).add_font_variation("wdth", 62.5f).add_font_variation("wght", 400.0f);
        Attrs ar_norm; ar_norm.set_family(Family("ArabicVar")).add_font_variation("wdth", 100.0f).add_font_variation("wght", 400.0f);
        Attrs ar_bold; ar_bold.set_family(Family("ArabicVar")).add_font_variation("wdth", 100.0f).add_font_variation("wght", 850.0f);
        Buffer bar_c(Metrics(28.0f, 44.0f)); bar_c.set_text("مرحبا بالعالم", ar_cond); bar_c.shape_until_scroll(fs);
        Buffer bar_n(Metrics(28.0f, 44.0f)); bar_n.set_text("مرحبا بالعالم", ar_norm); bar_n.shape_until_scroll(fs);
        Buffer bar_b(Metrics(28.0f, 44.0f)); bar_b.set_text("مرحبا بالعالم", ar_bold); bar_b.shape_until_scroll(fs);
        assert(bar_c.max_line_width() < bar_n.max_line_width() - 8.0f);
        assert(count_ink(bar_b) > count_ink(bar_n) + 100);
    }

    // 4. StrutStyle, HalfLeading & TextHeightBehavior
    {
        FontSystem fs;
        fs.set_auto_system_fonts(false);
        assert(fs.load_font_file(resolve_font("Inter-Regular.ttf"), 0, "InterStrut").has_value());
        assert(fs.load_font_file(resolve_font("NotoSansArabic.ttf"), 0, "ArabicStrut").has_value());

        // StrutStyle with force_strut_height unifies heights across mixed sizes/scripts
        Buffer sb(Metrics(16.0f, 22.0f));
        StrutStyle strut;
        strut.enabled = true;
        strut.font_families = {Family("InterStrut")};
        strut.font_size = 20.0f;
        strut.height_multiplier = 1.4f;
        strut.force_strut_height = true;
        strut.half_leading = true;
        sb.set_strut_style(strut);

        Attrs a_small; a_small.set_font_size(12.0f);
        Attrs a_big;   a_big.set_font_size(28.0f);
        Attrs a_ar;    a_ar.set_font_families({Family("ArabicStrut")}).set_font_size(20.0f);
        sb.add_text("Small line\n", a_small);
        sb.add_text("Big line\n", a_big);
        sb.add_text("سطر عربي", a_ar);
        sb.shape_until_scroll(fs);
        auto slm = sb.line_metrics();
        assert(slm.size() == 3);
        assert(std::abs(slm[0].height - slm[1].height) < 0.05f);
        assert(std::abs(slm[1].height - slm[2].height) < 0.05f);
        assert(std::abs(slm[0].ascent - slm[2].ascent) < 0.05f);

        // HalfLeading shifts baseline down to center vertically inside tall line_height
        Buffer hl_off(Metrics(16.0f, 36.0f));
        hl_off.set_text("Button");
        hl_off.shape_until_scroll(fs);

        Buffer hl_on(Metrics(16.0f, 36.0f));
        hl_on.set_half_leading(true);
        hl_on.set_text("Button");
        hl_on.shape_until_scroll(fs);
        assert(hl_on.alphabetic_baseline() > hl_off.alphabetic_baseline() + 4.0f);

        // TextHeightBehavior trims top first-ascent and bottom last-descent leading
        Buffer trimmed(Metrics(16.0f, 36.0f));
        trimmed.set_half_leading(true);
        trimmed.set_text_height_behavior(TextHeightBehavior{false, false});
        trimmed.set_text("Button");
        trimmed.shape_until_scroll(fs);
        assert(trimmed.alphabetic_baseline() < hl_on.alphabetic_baseline() - 4.0f);
        assert(trimmed.total_height() < hl_on.total_height() - 8.0f);
    }

    std::cout << "  -> PASS" << std::endl;
}

void test_phase4_incremental_updates_cache_and_visitor() {
    std::cout << "[Test] Phase 4 Incremental Updates, ParagraphCache & Glyph Run Visitor..." << std::endl;

    FontSystem fs;
    fs.set_auto_system_fonts(false);
    auto inter_id = fs.load_font_file(resolve_font("Inter-Regular.ttf"));
    auto ar_id = fs.load_font_file(resolve_font("NotoSansArabic.ttf"));
    assert(inter_id.has_value() && ar_id.has_value());

    // -------------------------------------------------------------------------
    // 4.1a: update_text_align (Zero Re-Shape, Lossless Justified <-> Right/Center/Left)
    // -------------------------------------------------------------------------
    {
        Buffer buf(Metrics(18.0f, 24.0f));
        buf.set_size(240.0f, std::nullopt);
        buf.set_wrap(Wrap::Word);
        buf.set_align(Align::Left);
        buf.set_text("Nisaba text engine performs fast incremental alignment updates across wrapped lines");
        buf.shape_until_scroll(fs);
        assert(buf.shape_count() == 1);
        const auto left_metrics = buf.line_metrics();
        assert(left_metrics.size() >= 2);
        const float orig_line0_w = left_metrics[0].width;
        assert(std::abs(left_metrics[0].left) < 0.01f);

        // Switch to Justified without re-shaping
        buf.update_text_align(Align::Justified);
        assert(buf.shape_count() == 1);
        const auto just_metrics = buf.line_metrics();
        assert(just_metrics[0].width > orig_line0_w + 1.0f);
        assert(std::abs(just_metrics[0].width - 240.0f) < 0.5f);

        // Switch from Justified -> Right without re-shaping (must undo justified extra advance)
        buf.update_text_align(Align::Right);
        assert(buf.shape_count() == 1);
        const auto right_metrics = buf.line_metrics();
        assert(std::abs(right_metrics[0].width - orig_line0_w) < 0.05f);
        assert(std::abs(right_metrics[0].left - (240.0f - orig_line0_w)) < 0.1f);

        // Compare bit-accurately against a freshly shaped Align::Right buffer
        Buffer ref_right(Metrics(18.0f, 24.0f));
        ref_right.set_size(240.0f, std::nullopt);
        ref_right.set_wrap(Wrap::Word);
        ref_right.set_align(Align::Right);
        ref_right.set_text("Nisaba text engine performs fast incremental alignment updates across wrapped lines");
        ref_right.shape_until_scroll(fs);
        const auto ref_metrics = ref_right.line_metrics();
        assert(ref_metrics.size() == right_metrics.size());
        for (size_t i = 0; i < ref_metrics.size(); ++i) {
            assert(std::abs(ref_metrics[i].width - right_metrics[i].width) < 0.05f);
            assert(std::abs(ref_metrics[i].left - right_metrics[i].left) < 0.05f);
        }

        // Switch to Center without re-shaping
        buf.update_text_align(Align::Center);
        assert(buf.shape_count() == 1);
        const auto center_metrics = buf.line_metrics();
        assert(std::abs(center_metrics[0].left - (240.0f - orig_line0_w) * 0.5f) < 0.1f);

        // Arabic RTL paragraph: Align::Start (right) -> Align::End (left) without re-shaping
        Buffer ar_buf(Metrics(20.0f, 28.0f));
        ar_buf.set_size(260.0f, std::nullopt);
        ar_buf.set_text_direction(TextDirection::RTL);
        ar_buf.set_align(Align::Start);
        ar_buf.set_text("مرحبا نيسابا");
        ar_buf.shape_until_scroll(fs);
        assert(ar_buf.shape_count() == 1);
        const float ar_start_left = ar_buf.line_metrics()[0].left;
        assert(ar_start_left > 50.0f);

        ar_buf.update_text_align(Align::End);
        assert(ar_buf.shape_count() == 1);
        assert(std::abs(ar_buf.line_metrics()[0].left) < 0.1f);
    }

    // -------------------------------------------------------------------------
    // 4.1b: update_foreground_paint & update_background_paint (Zero Re-Shape + CPU Render)
    // -------------------------------------------------------------------------
    {
        Buffer buf(Metrics(24.0f, 34.0f));
        buf.set_text("Alpha Beta\nمرحبا بالعالم");
        buf.shape_until_scroll(fs);
        assert(buf.shape_count() == 1);

        auto grad = LinearGradient::create(
            Point::from_xy(0.0f, 0.0f),
            Point::from_xy(200.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 40, 40, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(40, 120, 255, 255))
            }
        );
        assert(grad.has_value());
        Paint fg_grad{Shader{*grad}};
        Paint bg_yellow(Color::from_rgba8(255, 230, 60, 255));

        // Update "Beta" [6, 10) foreground to gradient and "Alpha" [0, 5) background to yellow
        buf.update_foreground_paint(6, 10, fg_grad);
        buf.update_background_paint(0, 5, bg_yellow);

        // Update Arabic word "بالعالم" on second line (after "Alpha Beta\nمرحبا ")
        const std::string full = "Alpha Beta\nمرحبا بالعالم";
        const size_t ar_word_start = full.find("بالعالم");
        assert(ar_word_start != std::string::npos);
        Paint fg_green(Color::from_rgba8(40, 230, 90, 255));
        buf.update_foreground_paint(ar_word_start, full.size(), fg_green);

        // Must NOT have re-shaped!
        assert(buf.shape_count() == 1);

        // Verify glyph paint pointers on line 0 and line 1
        const auto runs = buf.layout_runs();
        assert(runs.size() == 2);
        size_t bg_glyphs = 0, fg_grad_glyphs = 0, ar_green_glyphs = 0;
        for (const auto& g : runs[0].glyphs) {
            if (g.start < 5 && g.background_paint_opt != nullptr) ++bg_glyphs;
            if (g.start >= 6 && g.start < 10 && g.foreground_paint_opt != nullptr) ++fg_grad_glyphs;
        }
        for (const auto& g : runs[1].glyphs) {
            if (g.foreground_paint_opt != nullptr) ++ar_green_glyphs;
        }
        assert(bg_glyphs >= 4);
        assert(fg_grad_glyphs >= 4);
        assert(ar_green_glyphs >= 3);

        // Render on CPU Canvas and verify painted pixels
        auto pm = Pixmap::create(300, 110);
        assert(pm.has_value());
        pm->fill(Color::from_rgba8(12, 14, 18, 255));
        Canvas canvas(*pm);
        GlyphCache gcache;
        buf.draw(canvas, gcache, fs, Color::WHITE, 12.0f, 12.0f);
        assert(buf.shape_count() == 1);

        const auto pref = pm->as_ref();
        size_t yellow_bg_px = 0, green_ar_px = 0;
        for (uint32_t y = 10; y < 50; ++y) {
            for (uint32_t x = 10; x < 100; ++x) {
                auto p = pref.pixel(x, y);
                if (p && p->r > 220 && p->g > 200 && p->b < 100) ++yellow_bg_px;
            }
        }
        for (uint32_t y = 46; y < 96; ++y) {
            for (uint32_t x = 10; x < 240; ++x) {
                auto p = pref.pixel(x, y);
                if (p && p->g > 180 && p->r < 120 && p->b < 140) ++green_ar_px;
            }
        }
        assert(yellow_bg_px > 400);
        assert(green_ar_px > 60);
    }

    // -------------------------------------------------------------------------
    // 4.1c: update_font_size (Zero Re-Shape, Expand & Shrink Back)
    // -------------------------------------------------------------------------
    {
        Buffer buf(Metrics(20.0f, 26.0f));
        buf.set_text("Hello World");
        buf.shape_until_scroll(fs);
        assert(buf.shape_count() == 1);
        const float orig_w = buf.max_line_width();
        const float orig_h = buf.total_height();

        buf.update_font_size(0, 5, 40.0f);
        assert(buf.shape_count() == 1);
        const float big_w = buf.max_line_width();
        const float big_h = buf.total_height();
        assert(big_w > orig_w + 30.0f);
        assert(big_h > orig_h + 10.0f);

        const auto runs = buf.layout_runs();
        assert(runs.size() == 1);
        for (const auto& g : runs[0].glyphs) {
            if (g.start < 5) assert(std::abs(g.font_size - 40.0f) < 0.01f);
            else if (g.start >= 6) assert(std::abs(g.font_size - 20.0f) < 0.01f);
        }

        // Shrink back to 20px and verify metrics restore cleanly without re-shaping
        buf.update_font_size(0, 5, 20.0f);
        assert(buf.shape_count() == 1);
        assert(std::abs(buf.max_line_width() - orig_w) < 0.1f);
        assert(std::abs(buf.total_height() - orig_h) < 0.1f);

        // Arabic in-place font size update
        const std::string ar_s = "السلام عليكم";
        const size_t sp_pos = ar_s.find(' ');
        Buffer ar_buf(Metrics(20.0f, 28.0f));
        ar_buf.set_text(ar_s);
        ar_buf.shape_until_scroll(fs);
        assert(ar_buf.shape_count() == 1);
        const float ar_orig_w = ar_buf.max_line_width();
        ar_buf.update_font_size(sp_pos + 1, ar_s.size(), 34.0f);
        assert(ar_buf.shape_count() == 1);
        assert(ar_buf.max_line_width() > ar_orig_w + 20.0f);
    }

    // -------------------------------------------------------------------------
    // 4.1d: ParagraphCache (LRU, Hits/Misses, Eviction, lookup_or_shape)
    // -------------------------------------------------------------------------
    {
        ParagraphCache pcache(2);
        Attrs a;
        a.set_color(TextColor::rgb(240, 240, 240));

        auto b1 = pcache.get_or_layout(fs, "Repeated Label", a, Metrics(16.0f, 22.0f), 200.0f);
        assert(b1 != nullptr);
        assert(pcache.misses() == 1 && pcache.hits() == 0 && pcache.size() == 1);

        auto b1_hit = pcache.get_or_layout(fs, "Repeated Label", a, Metrics(16.0f, 22.0f), 200.0f);
        assert(b1_hit == b1);
        assert(pcache.misses() == 1 && pcache.hits() == 1);

        // Different width => cache miss
        auto b2 = pcache.get_or_layout(fs, "Repeated Label", a, Metrics(16.0f, 22.0f), 120.0f);
        assert(b2 != b1);
        assert(pcache.misses() == 2 && pcache.size() == 2);

        // Touch b1 so b2 becomes LRU, then insert Arabic label -> evicts b2
        auto b1_touch = pcache.get_or_layout(fs, "Repeated Label", a, Metrics(16.0f, 22.0f), 200.0f);
        assert(b1_touch == b1 && pcache.hits() == 2);

        auto b3 = pcache.get_or_layout(fs, "نص عربي مخبأ", a, Metrics(18.0f, 26.0f), 240.0f, Wrap::Word, Align::Start, Overflow::Visible, std::nullopt, TextDirection::RTL);
        assert(b3 != nullptr && b3->max_line_width() > 20.0f);
        assert(pcache.size() == 2);

        // b1 should still hit; b2 should miss
        auto b1_again = pcache.get_or_layout(fs, "Repeated Label", a, Metrics(16.0f, 22.0f), 200.0f);
        assert(b1_again == b1 && pcache.hits() == 3);

        // lookup_or_shape hydrates an independent Buffer with zero shaping passes on hit
        ParagraphCache buf_cache(8);
        Buffer src1(Metrics(18.0f, 26.0f));
        src1.set_size(220.0f, std::nullopt);
        src1.set_text("Hello مرحبا Cached Buffer", a);
        const bool hit1 = buf_cache.lookup_or_shape(fs, src1);
        assert(!hit1 && src1.shape_count() == 1);

        Buffer src2(Metrics(18.0f, 26.0f));
        src2.set_size(220.0f, std::nullopt);
        src2.set_text("Hello مرحبا Cached Buffer", a);
        const bool hit2 = buf_cache.lookup_or_shape(fs, src2);
        assert(hit2);
        assert(src2.shape_count() == 0);
        assert(!src2.is_layout_dirty());
        assert(std::abs(src2.max_line_width() - src1.max_line_width()) < 0.01f);
        assert(std::abs(src2.total_height() - src1.total_height()) < 0.01f);
    }

    // -------------------------------------------------------------------------
    // 4.2: Buffer::visit (Glyph Run Visitor)
    // -------------------------------------------------------------------------
    {
        Buffer buf(Metrics(20.0f, 28.0f));
        buf.set_size(360.0f, std::nullopt);
        buf.set_align(Align::Center);
        Attrs a;
        a.set_color(TextColor::rgb(255, 255, 255));
        buf.add_text("Hello ", a);
        buf.add_placeholder(24.0f, 20.0f, PlaceholderAlignment::Middle);
        buf.add_text(" مرحبا\nSecond Line", a);
        buf.shape_until_scroll(fs);

        size_t run_count = 0;
        size_t ltr_runs = 0;
        size_t rtl_runs = 0;
        size_t placeholder_runs = 0;
        size_t line0_runs = 0;
        size_t line1_runs = 0;
        size_t total_visited_glyphs = 0;
        bool saw_line1_cluster_offset = false;

        const float ox = 15.0f;
        const float oy = 25.0f;
        buf.visit(fs, [&](const GlyphRunInfo& info) {
            ++run_count;
            assert(info.glyph_ids.size() == info.positions.size());
            assert(info.glyph_ids.size() == info.advances.size());
            assert(info.glyph_ids.size() == info.clusters.size());
            assert(info.glyph_ids.size() == info.glyphs.size());

            if (info.line_number == 0) ++line0_runs;
            if (info.line_number == 1) ++line1_runs;

            if (info.is_placeholder) {
                ++placeholder_runs;
                assert(info.glyph_ids.size() == 1);
                assert(info.placeholder_index == 0);
                assert(std::abs(info.advance_width - 24.0f) < 0.01f);
            } else if (!info.glyph_ids.empty()) {
                assert(info.font != nullptr);
                assert(info.font == fs.get_font(info.font_id));
                if (info.is_rtl) ++rtl_runs;
                else ++ltr_runs;
            }

            for (size_t i = 0; i < info.glyph_ids.size(); ++i) {
                const auto& g = info.glyphs[i];
                const float abs_x = info.origin.x + info.positions[i].x;
                const float expected_x = ox + g.x + g.x_offset;
                assert(std::abs(abs_x - expected_x) < 0.05f);
                assert(info.clusters[i] == static_cast<uint32_t>(info.text_byte_offset + g.start));
                if (info.line_number == 1 && info.clusters[i] > 10) {
                    saw_line1_cluster_offset = true;
                }
            }
            total_visited_glyphs += info.glyph_ids.size();
        }, ox, oy);

        assert(run_count >= 4);
        assert(ltr_runs >= 2);
        assert(rtl_runs >= 1);
        assert(placeholder_runs == 1);
        assert(line0_runs >= 3);
        assert(line1_runs >= 1);
        assert(total_visited_glyphs >= 18);
        assert(saw_line1_cluster_offset);
    }

    std::cout << "  -> PASS" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "   Nisaba Sovereign Text Engine Test Suite" << std::endl;
    std::cout << "==========================================" << std::endl;

    test_attrs();
    test_ttf_font();
    test_bidi_and_arabic();
    test_font_system();
    test_buffer_layout();
    test_rendering_to_canvas();
    test_cursor_and_selection();
    test_multilingual_cjk_russian();
    test_extended_arabic_tashkeel_and_mirroring();
    test_rich_typography_and_layout();
    test_grapheme_word_cursor_and_selection_rects();
    test_p16_partb_and_p17_text_features();
    test_phase1_metrics_ellipsis_and_direction();
    test_phase2_placeholders_rich_paint_and_gap_underline();
    test_phase3_fallback_features_variable_fonts_and_strut();
    test_phase4_incremental_updates_cache_and_visitor();

    std::cout << "==========================================" << std::endl;
    std::cout << "All 16 text test suites passed successfully!" << std::endl;
    std::cout << "==========================================" << std::endl;
    return 0;
}
