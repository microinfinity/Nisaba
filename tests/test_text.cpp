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

    std::cout << "==========================================" << std::endl;
    std::cout << "All 12 text test suites passed successfully!" << std::endl;
    std::cout << "==========================================" << std::endl;
    return 0;
}
