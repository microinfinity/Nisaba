#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::text;

namespace {

std::string resolve_font(std::string_view filename) {
    const std::vector<std::string> candidates = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename),
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return candidates[0];
}

std::string fmt1(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

template <typename DrawBackend, typename DrawBufferFn>
void render_showcase_scene(
    DrawBackend& backend,
    DrawBufferFn&& draw_buf,
    FontSystem& fs,
    ParagraphCache& pcache,
    uint32_t W,
    uint32_t H
) {
    // -------------------------------------------------------------------------
    // 1. Dark Obsidian Glass Background + Subtle Ambient Glows
    // -------------------------------------------------------------------------
    auto bg_grad = LinearGradient::create(
        Point::from_xy(0.0f, 0.0f),
        Point::from_xy(static_cast<float>(W), static_cast<float>(H)),
        {
            GradientStop::create(0.0f, Color::from_rgba8(11, 15, 25, 255)),
            GradientStop::create(0.55f, Color::from_rgba8(14, 19, 32, 255)),
            GradientStop::create(1.0f, Color::from_rgba8(8, 11, 20, 255)),
        }
    );
    Paint bg_paint = bg_grad ? Paint{Shader{*bg_grad}} : Paint(Color::from_rgba8(12, 16, 26, 255));
    backend.fill_rect(*Rect::from_xywh(0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H)), bg_paint);

    auto draw_card = [&](float x, float y, float w, float h, Color accent) {
        const Rect r = *Rect::from_xywh(x, y, w, h);
        auto card_grad = LinearGradient::create(
            Point::from_xy(x, y),
            Point::from_xy(x, y + h),
            {
                GradientStop::create(0.0f, Color::from_rgba8(24, 31, 48, 232)),
                GradientStop::create(1.0f, Color::from_rgba8(17, 22, 36, 238)),
            }
        );
        Paint fill_p = card_grad ? Paint{Shader{*card_grad}} : Paint(Color::from_rgba8(20, 26, 40, 235));
        backend.fill_round_rect(r, 16.0f, 16.0f, fill_p);

        Paint border_p(Color::from_rgba8(56, 72, 108, 170));
        backend.stroke_round_rect(r, 16.0f, 16.0f, border_p, Stroke(1.25f));

        // Top accent bar
        Paint acc_p(accent);
        backend.fill_round_rect(*Rect::from_xywh(x + 18.0f, y + 14.0f, 44.0f, 4.0f), 2.0f, 2.0f, acc_p);
    };

    auto draw_card_header = [&](float x, float y, std::string_view title, std::string_view badge, Color accent) {
        Attrs t_attrs;
        t_attrs.set_font_families({Family("InterVar"), Family("ArabicVar")})
               .add_font_variation("wght", 700.0f)
               .set_color(TextColor::rgb(236, 242, 255));
        auto t_buf = pcache.get_or_layout(fs, title, t_attrs, Metrics(17.0f, 24.0f));
        draw_buf(*t_buf, x + 18.0f, y + 26.0f, Color::WHITE);

        // Phase pill badge on top-right of card
        Attrs b_attrs;
        b_attrs.set_family(Family("Mono"))
               .set_color(TextColor::rgb(
                   static_cast<uint8_t>(accent.red() * 255.0f),
                   static_cast<uint8_t>(accent.green() * 255.0f),
                   static_cast<uint8_t>(accent.blue() * 255.0f)));
        auto b_buf = pcache.get_or_layout(fs, badge, b_attrs, Metrics(11.5f, 16.0f));
        const float bw = b_buf->max_line_width() + 16.0f;
        const float bx = x + 658.0f - bw - 18.0f;
        Paint pill_bg(Color::from_rgba8(
            static_cast<uint8_t>(accent.red() * 255.0f),
            static_cast<uint8_t>(accent.green() * 255.0f),
            static_cast<uint8_t>(accent.blue() * 255.0f), 34));
        backend.fill_round_rect(*Rect::from_xywh(bx, y + 22.0f, bw, 22.0f), 11.0f, 11.0f, pill_bg);
        draw_buf(*b_buf, bx + 8.0f, y + 26.0f, Color::WHITE);
    };

    // -------------------------------------------------------------------------
    // 2. Header Banner (Gradient Display Typography + Bilingual Subtitle)
    // -------------------------------------------------------------------------
    {
        const Rect hdr = *Rect::from_xywh(40.0f, 24.0f, 1360.0f, 104.0f);
        auto hdr_grad = LinearGradient::create(
            Point::from_xy(40.0f, 24.0f),
            Point::from_xy(1400.0f, 128.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(24, 34, 58, 245)),
                GradientStop::create(1.0f, Color::from_rgba8(18, 24, 42, 245)),
            }
        );
        backend.fill_round_rect(hdr, 18.0f, 18.0f, hdr_grad ? Paint{Shader{*hdr_grad}} : Paint(Color::from_rgba8(22, 30, 50, 245)));
        backend.stroke_round_rect(hdr, 18.0f, 18.0f, Paint(Color::from_rgba8(74, 108, 168, 190)), Stroke(1.5f));

        auto title_grad = LinearGradient::create(
            Point::from_xy(68.0f, 0.0f),
            Point::from_xy(880.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(80, 225, 255, 255)),
                GradientStop::create(0.5f, Color::from_rgba8(155, 140, 255, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(255, 135, 195, 255)),
            }
        );
        Paint title_fg = title_grad ? Paint{Shader{*title_grad}} : Paint(Color::WHITE);

        Buffer hero(Metrics(30.0f, 38.0f));
        Attrs hero_attrs;
        hero_attrs.set_family(Family("InterVar"))
                  .add_font_variation("wght", 820.0f)
                  .set_foreground_paint(title_fg);
        hero.set_text("NISABA SOVEREIGN TEXT ENGINE — ADVANCED TYPOGRAPHY", hero_attrs);
        hero.shape_until_scroll(fs);
        draw_buf(hero, 68.0f, 38.0f, Color::WHITE);

        Attrs sub_attrs;
        sub_attrs.set_font_families({Family("InterVar"), Family("ArabicVar")})
                 .set_locale("ar")
                 .set_color(TextColor::rgb(172, 190, 222));
        auto sub = pcache.get_or_layout(
            fs,
            "Variable Fonts • OpenType Features • Placeholders • Gap Underline • StrutStyle • Incremental Updates • محرك نيسابا السيادي",
            sub_attrs,
            Metrics(15.0f, 22.0f)
        );
        draw_buf(*sub, 68.0f, 86.0f, Color::WHITE);
    }

    // -------------------------------------------------------------------------
    // Card 1 (Top-Left): Phase 1 — Extended Metrics, Custom Ellipsis & RTL Direction
    // -------------------------------------------------------------------------
    {
        const float cx = 40.0f, cy = 148.0f, cw = 660.0f, ch = 266.0f;
        draw_card(cx, cy, cw, ch, Color::from_rgba8(56, 200, 255, 255));
        draw_card_header(cx, cy, "Extended Metrics, Custom Ellipsis & RTL Direction", "PHASE 1", Color::from_rgba8(56, 200, 255, 255));

        // Explicit RTL paragraph with custom Arabic ellipsis "… المزيد"
        Buffer rtl_buf(Metrics(18.0f, 28.0f));
        rtl_buf.set_size(604.0f, std::nullopt);
        rtl_buf.set_wrap(Wrap::Word);
        rtl_buf.set_text_direction(TextDirection::RTL);
        rtl_buf.set_align(Align::Start);
        rtl_buf.set_overflow(Overflow::Ellipsis);
        rtl_buf.set_max_lines(2);
        rtl_buf.set_ellipsis("\xE2\x80\xA6 \xD8\xA7\xD9\x84\xD9\x85\xD8\xB2\xD9\x8A\xD8\xAF");
        Attrs rtl_attrs;
        rtl_attrs.set_font_families({Family("ArabicVar"), Family("InterVar")})
                 .set_locale("ar")
                 .set_color(TextColor::rgb(235, 242, 255));
        rtl_buf.set_text(
            "يدعم محرك نيسابا الاتجاه الصريح للنصوص العربية مع إعادة ترتيب ثنائي الاتجاه UAX #9 L2 "
            "وقطع الأسطر الزائدة بعبارة مخصصة بالكامل مع قياس العرض الجوهري الأدنى والأقصى بدقة متناهية 2026",
            rtl_attrs
        );
        rtl_buf.shape_until_scroll(fs);

        const float rx = cx + 26.0f;
        const float ry = cy + 62.0f;
        // Draw alphabetic & ideographic baseline guides for line 0
        const float alpha_y = ry + rtl_buf.alphabetic_baseline();
        const float ideo_y = ry + rtl_buf.ideographic_baseline();
        backend.stroke_line(rx, alpha_y, rx + 604.0f, alpha_y, Paint(Color::from_rgba8(56, 200, 255, 90)), Stroke(1.0f));
        backend.stroke_line(rx, ideo_y, rx + 604.0f, ideo_y, Paint(Color::from_rgba8(255, 160, 80, 80)), Stroke(1.0f));
        draw_buf(rtl_buf, rx, ry, Color::WHITE);

        // LTR paragraph with custom "... read more" ellipsis
        Buffer ltr_buf(Metrics(15.5f, 23.0f));
        ltr_buf.set_size(604.0f, std::nullopt);
        ltr_buf.set_wrap(Wrap::Word);
        ltr_buf.set_text_direction(TextDirection::LTR);
        ltr_buf.set_overflow(Overflow::Ellipsis);
        ltr_buf.set_max_lines(1);
        ltr_buf.set_ellipsis("... read more");
        Attrs ltr_attrs;
        ltr_attrs.set_family(Family("InterVar")).set_color(TextColor::rgb(190, 208, 236));
        ltr_buf.set_text(
            "Single-line overflow truncation shapes arbitrary UTF-8 ellipsis strings cleanly without clipping characters in half",
            ltr_attrs
        );
        ltr_buf.shape_until_scroll(fs);
        draw_buf(ltr_buf, rx, ry + 72.0f, Color::WHITE);

        // Live metrics strip
        const float min_w = rtl_buf.min_intrinsic_width(fs);
        const float max_w = rtl_buf.max_intrinsic_width(fs);
        const auto lm = rtl_buf.line_metrics();
        std::ostringstream ms;
        ms << "min_intrinsic=" << fmt1(min_w) << "px  |  max_intrinsic=" << fmt1(max_w)
           << "px  |  lines=" << lm.size()
           << "  |  exceeded=" << (rtl_buf.did_exceed_max_lines() ? "true" : "false")
           << "\nalphabetic_base=" << fmt1(rtl_buf.alphabetic_baseline())
           << "px  |  ideographic_base=" << fmt1(rtl_buf.ideographic_baseline())
           << "px  |  unresolved_glyphs=" << rtl_buf.unresolved_glyphs();

        backend.fill_round_rect(*Rect::from_xywh(rx, cy + 178.0f, 608.0f, 66.0f), 10.0f, 10.0f, Paint(Color::from_rgba8(13, 18, 30, 220)));
        Attrs m_attrs;
        m_attrs.set_family(Family("Mono")).set_color(TextColor::rgb(122, 224, 255));
        Buffer m_buf(Metrics(12.0f, 22.0f));
        m_buf.set_text(ms.str(), m_attrs);
        m_buf.shape_until_scroll(fs);
        draw_buf(m_buf, rx + 12.0f, cy + 190.0f, Color::WHITE);
    }

    // -------------------------------------------------------------------------
    // Card 2 (Top-Right): Phase 2 — Inline Placeholders & Full Paint Spans
    // -------------------------------------------------------------------------
    {
        const float cx = 740.0f, cy = 148.0f, cw = 660.0f, ch = 266.0f;
        draw_card(cx, cy, cw, ch, Color::from_rgba8(255, 115, 180, 255));
        draw_card_header(cx, cy, "Inline Placeholders & Rich Paint Spans", "PHASE 2", Color::from_rgba8(255, 115, 180, 255));

        Buffer ph_buf(Metrics(18.5f, 30.0f));
        ph_buf.set_size(608.0f, std::nullopt);
        ph_buf.set_wrap(Wrap::Word);

        Attrs base_a;
        base_a.set_font_families({Family("InterVar"), Family("ArabicVar")})
              .set_color(TextColor::rgb(228, 235, 248));

        auto grad_fg = LinearGradient::create(
            Point::from_xy(cx + 26.0f, 0.0f),
            Point::from_xy(cx + 620.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 195, 65, 255)),
                GradientStop::create(0.5f, Color::from_rgba8(255, 95, 155, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(105, 190, 255, 255)),
            }
        );
        Attrs grad_a = base_a;
        grad_a.add_font_variation("wght", 780.0f)
              .set_foreground_paint(Paint{Shader{*grad_fg}});

        auto pill_grad = LinearGradient::create(
            Point::from_xy(cx + 26.0f, 0.0f),
            Point::from_xy(cx + 420.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(55, 42, 118, 235)),
                GradientStop::create(1.0f, Color::from_rgba8(28, 86, 128, 235)),
            }
        );
        Attrs bg_a = base_a;
        bg_a.set_background_paint(Paint{Shader{*pill_grad}})
            .set_color(TextColor::rgb(255, 255, 255));

        ph_buf.add_text("Status: ", base_a);
        ph_buf.add_placeholder(16.0f, 16.0f, PlaceholderAlignment::Middle);
        ph_buf.add_text(" Online with ", base_a);
        ph_buf.add_text("Gradient Paint", grad_a);
        ph_buf.add_text(" + ", base_a);
        ph_buf.add_placeholder(74.0f, 24.0f, PlaceholderAlignment::Middle);
        ph_buf.add_text(" inline widgets &\n", base_a);
        ph_buf.add_text(" Shader Highlight ", bg_a);
        ph_buf.add_text("  •  يدعم أيضاً ", base_a);
        ph_buf.add_placeholder(22.0f, 22.0f, PlaceholderAlignment::Middle);
        ph_buf.add_text(" العناصر المضمّنة داخل النص العربي.", base_a);
        ph_buf.shape_until_scroll(fs);

        const float px = cx + 26.0f;
        const float py = cy + 62.0f;
        draw_buf(ph_buf, px, py, Color::WHITE);

        // Render the 3 inline placeholders using exact laid-out PlaceholderRects
        const auto prects = ph_buf.placeholder_rects(px, py);
        for (const auto& pr : prects) {
            if (pr.placeholder_index == 0) {
                const float r = pr.rect.width() * 0.5f;
                backend.fill_circle(pr.rect.left() + r, pr.rect.top() + r, r, Paint(Color::from_rgba8(48, 225, 130, 255)));
            } else if (pr.placeholder_index == 1) {
                backend.fill_round_rect(pr.rect, 12.0f, 12.0f, Paint(Color::from_rgba8(255, 115, 180, 230)));
                Attrs chip_a;
                chip_a.set_family(Family("InterVar")).add_font_variation("wght", 700.0f).set_color(TextColor::rgb(18, 12, 24));
                auto chip_t = pcache.get_or_layout(fs, "NEW v2", chip_a, Metrics(12.0f, 16.0f));
                draw_buf(*chip_t, pr.rect.left() + 12.0f, pr.rect.top() + 4.0f, Color::BLACK);
            } else {
                backend.fill_round_rect(pr.rect, 6.0f, 6.0f, Paint(Color::from_rgba8(68, 170, 255, 240)));
                backend.stroke_round_rect(pr.rect, 6.0f, 6.0f, Paint(Color::WHITE), Stroke(1.2f));
            }
        }

        // Stroked / outlined display headline via PaintStyle::Stroke
        Paint stroke_p(Color::from_rgba8(110, 225, 255, 255));
        stroke_p.set_style(PaintStyle::Stroke);
        stroke_p.set_stroke_width(1.6f);

        Buffer outline_buf(Metrics(34.0f, 44.0f));
        Attrs out_a;
        out_a.set_family(Family("InterVar"))
             .add_font_variation("wght", 850.0f)
             .set_foreground_paint(stroke_p, Stroke(1.6f));
        outline_buf.set_text("OUTLINED VECTOR GLYPHS", out_a);
        outline_buf.shape_until_scroll(fs);
        draw_buf(outline_buf, px, cy + 198.0f, Color::WHITE);
    }

    // -------------------------------------------------------------------------
    // Card 3 (Mid-Left): Phase 2 — Smart Underline Gap Skipping & Shadows
    // -------------------------------------------------------------------------
    {
        const float cx = 40.0f, cy = 434.0f, cw = 660.0f, ch = 264.0f;
        draw_card(cx, cy, cw, ch, Color::from_rgba8(255, 195, 65, 255));
        draw_card_header(cx, cy, "Smart Underline Gap Skipping & Decorations", "PHASE 2.3", Color::from_rgba8(255, 195, 65, 255));

        const float lx = cx + 26.0f;

        // 1. DecorationMode::Through (reference)
        Attrs lbl_a;
        lbl_a.set_family(Family("Mono")).set_color(TextColor::rgb(138, 156, 190));
        auto l1 = pcache.get_or_layout(fs, "DecorationMode::Through:", lbl_a, Metrics(12.0f, 16.0f));
        draw_buf(*l1, lx, cy + 62.0f, Color::WHITE);

        Buffer through_buf(Metrics(26.0f, 36.0f));
        Attrs ta;
        ta.set_font_families({Family("InterVar"), Family("ArabicVar")})
          .set_color(TextColor::rgb(225, 232, 245))
          .set_underline(true)
          .set_decoration_mode(DecorationMode::Through)
          .set_decoration_color(TextColor::rgb(255, 110, 110))
          .set_decoration_thickness(2.2f);
        through_buf.set_text("Typography gypq  —  يرجى التوقيع", ta);
        through_buf.shape_until_scroll(fs);
        draw_buf(through_buf, lx, cy + 80.0f, Color::WHITE);

        // 2. DecorationMode::Gaps (exact descender skipping)
        auto l2 = pcache.get_or_layout(fs, "DecorationMode::Gaps (descender-aware vector intersection):", lbl_a, Metrics(12.0f, 16.0f));
        draw_buf(*l2, lx, cy + 128.0f, Color::WHITE);

        Buffer gaps_buf(Metrics(26.0f, 36.0f));
        Attrs ga = ta;
        ga.set_decoration_mode(DecorationMode::Gaps)
          .set_decoration_color(TextColor::rgb(85, 235, 155))
          .set_decoration_thickness(2.4f);
        gaps_buf.set_text("Typography gypq  —  يرجى التوقيع", ga);
        gaps_buf.shape_until_scroll(fs);
        draw_buf(gaps_buf, lx, cy + 146.0f, Color::WHITE);

        // 3. Wavy / Dashed / Double styles + Soft Drop Shadow
        Buffer dec_buf(Metrics(20.0f, 30.0f));
        Attrs wavy_a;
        wavy_a.set_family(Family("InterVar"))
              .set_color(TextColor::rgb(255, 215, 130))
              .set_underline(true)
              .set_decoration_style(DecorationStyle::Wavy)
              .set_decoration_color(TextColor::rgb(255, 95, 115))
              .set_decoration_thickness(1.8f);
        Attrs dash_a;
        dash_a.set_family(Family("InterVar"))
              .set_color(TextColor::rgb(145, 225, 255))
              .set_underline(true)
              .set_decoration_style(DecorationStyle::Dashed)
              .set_decoration_color(TextColor::rgb(95, 195, 255))
              .set_decoration_thickness(1.8f);
        Attrs shadow_a;
        shadow_a.set_family(Family("InterVar"))
                .add_font_variation("wght", 750.0f)
                .set_color(TextColor::rgb(255, 255, 255))
                .add_shadow(TextColor::rgba(0, 210, 255, 200), 0.0f, 3.0f, 3.5f);

        dec_buf.add_text("Wavy Spellcheck   ", wavy_a);
        dec_buf.add_text("Dashed Link   ", dash_a);
        dec_buf.add_text("Blurred Shadow", shadow_a);
        dec_buf.shape_until_scroll(fs);
        draw_buf(dec_buf, lx, cy + 206.0f, Color::WHITE);
    }

    // -------------------------------------------------------------------------
    // Card 4 (Mid-Right): Phase 3 — Variable Fonts, OpenType Features & Fallback Chains
    // -------------------------------------------------------------------------
    {
        const float cx = 740.0f, cy = 434.0f, cw = 660.0f, ch = 264.0f;
        draw_card(cx, cy, cw, ch, Color::from_rgba8(130, 145, 255, 255));
        draw_card_header(cx, cy, "Variable Fonts (gvar/HVAR) & OpenType Features", "PHASE 3.1-3.2", Color::from_rgba8(130, 145, 255, 255));

        const float rx = cx + 26.0f;

        // Latin Variable Weight Axis Spectrum (wght 200 -> 900)
        Buffer wght_buf(Metrics(24.0f, 34.0f));
        const std::vector<std::pair<float, std::string>> weights = {
            {200.0f, "200 Thin  "},
            {400.0f, "400 Reg  "},
            {700.0f, "700 Bold  "},
            {900.0f, "900 Black"}
        };
        for (const auto& [w, label] : weights) {
            Attrs wa;
            wa.set_family(Family("InterVar"))
              .add_font_variation("wght", w)
              .set_color(TextColor::rgb(230, 238, 255));
            wght_buf.add_text(label, wa);
        }
        wght_buf.shape_until_scroll(fs);
        draw_buf(wght_buf, rx, cy + 64.0f, Color::WHITE);

        // OpenType 'tnum' + 'zero' tabular telemetry counter
        Buffer ot_buf(Metrics(22.0f, 32.0f));
        Attrs lbl_ot;
        lbl_ot.set_family(Family("InterVar")).set_color(TextColor::rgb(156, 174, 210));
        Attrs tnum_a;
        tnum_a.set_family(Family("InterVar"))
              .add_font_variation("wght", 650.0f)
              .add_font_feature("tnum", 1)
              .add_font_feature("zero", 1)
              .set_color(TextColor::rgb(115, 240, 195));
        ot_buf.add_text("OT tnum+zero: ", lbl_ot);
        ot_buf.add_text("09:42:18.040  |  10,048.00", tnum_a);
        ot_buf.shape_until_scroll(fs);
        draw_buf(ot_buf, rx, cy + 114.0f, Color::WHITE);

        // Arabic Variable Font (wght + wdth axes) via ordered fallback chain + locale("ar")
        Buffer ar_var(Metrics(24.0f, 38.0f));
        Attrs ar_light;
        ar_light.set_font_families({Family("InterVar"), Family("ArabicVar")})
                .set_locale("ar")
                .add_font_variation("wght", 300.0f)
                .add_font_variation("wdth", 100.0f)
                .set_color(TextColor::rgb(185, 205, 245));
        Attrs ar_heavy;
        ar_heavy.set_font_families({Family("InterVar"), Family("ArabicVar")})
                .set_locale("ar")
                .add_font_variation("wght", 850.0f)
                .add_font_variation("wdth", 75.0f)
                .set_color(TextColor::rgb(255, 208, 120));
        ar_var.add_text("خط عربي متغير رفيع  •  ", ar_light);
        ar_var.add_text("عريض مضغوط 850/75", ar_heavy);
        ar_var.shape_until_scroll(fs);
        draw_buf(ar_var, rx, cy + 166.0f, Color::WHITE);

        Attrs chain_note;
        chain_note.set_family(Family("Mono")).set_color(TextColor::rgb(148, 166, 204));
        auto cn = pcache.get_or_layout(fs, "Chain: [InterVar -> ArabicVar] | locale=\"ar\" | tables: fvar/avar/gvar/HVAR", chain_note, Metrics(12.0f, 18.0f));
        draw_buf(*cn, rx, cy + 220.0f, Color::WHITE);
    }

    // -------------------------------------------------------------------------
    // Card 5 (Bottom-Left): Phase 3.3 — StrutStyle, HalfLeading & TextHeightBehavior
    // -------------------------------------------------------------------------
    {
        const float cx = 40.0f, cy = 718.0f, cw = 660.0f, ch = 268.0f;
        draw_card(cx, cy, cw, ch, Color::from_rgba8(68, 228, 168, 255));
        draw_card_header(cx, cy, "StrutStyle, HalfLeading & TextHeightBehavior", "PHASE 3.3", Color::from_rgba8(68, 228, 168, 255));

        const float sx = cx + 26.0f;
        const float sy = cy + 64.0f;

        Buffer strut_buf(Metrics(18.0f, 24.0f));
        StrutStyle strut;
        strut.enabled = true;
        strut.font_families = {Family("InterVar")};
        strut.font_size = 18.0f;
        strut.height_multiplier = 1.65f;
        strut.force_strut_height = true;
        strut.half_leading = true;
        strut_buf.set_strut_style(strut);

        Attrs a_small;
        a_small.set_font_families({Family("InterVar"), Family("ArabicVar")}).set_font_size(14.0f).set_color(TextColor::rgb(176, 200, 232));
        Attrs a_big;
        a_big.set_font_families({Family("InterVar"), Family("ArabicVar")}).set_font_size(24.0f).add_font_variation("wght", 700.0f).set_color(TextColor::rgb(255, 228, 150));
        Attrs a_ar;
        a_ar.set_font_families({Family("ArabicVar"), Family("InterVar")}).set_font_size(19.0f).set_color(TextColor::rgb(115, 240, 195));

        strut_buf.add_text("Line 1: Small 14px + ", a_small);
        strut_buf.add_text("Large 24px", a_big);
        strut_buf.add_text(" — Uniform Strut Grid\n", a_small);
        strut_buf.add_text("السطر 2: تباعد أسطر موحّد تماماً بين العربية واللاتينية\n", a_ar);
        strut_buf.add_text("Line 3: Identical baseline-to-baseline rhythm across scripts", a_small);
        strut_buf.shape_until_scroll(fs);

        // Draw uniform baseline grid lines directly from line_metrics()
        const auto slm = strut_buf.line_metrics();
        for (const auto& m : slm) {
            const float by = sy + m.baseline;
            backend.stroke_line(sx, by, sx + 604.0f, by, Paint(Color::from_rgba8(68, 228, 168, 70)), Stroke(1.0f));
        }
        draw_buf(strut_buf, sx, sy, Color::WHITE);

        // Optical vertical centering button demo with TextHeightBehavior{false, false}
        const float btn_y = cy + 198.0f;
        const Rect btn_r = *Rect::from_xywh(sx, btn_y, 290.0f, 44.0f);
        auto btn_grad = LinearGradient::create(
            Point::from_xy(sx, btn_y),
            Point::from_xy(sx + 290.0f, btn_y),
            {
                GradientStop::create(0.0f, Color::from_rgba8(32, 165, 130, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(42, 132, 220, 255)),
            }
        );
        if (auto btn_path = PathBuilder::from_rrect(RRect::from_rect_and_radius(btn_r, 12.0f))) {
            backend.fill_path(*btn_path, Paint{Shader{*btn_grad}});
        }

        Buffer btn_buf(Metrics(16.0f, 36.0f));
        btn_buf.set_half_leading(true);
        btn_buf.set_text_height_behavior(TextHeightBehavior{false, false});
        Attrs btn_a;
        btn_a.set_font_families({Family("InterVar"), Family("ArabicVar")})
             .add_font_variation("wght", 700.0f)
             .set_color(TextColor::rgb(255, 255, 255));
        btn_buf.set_text("Centered Button • زر متمركز", btn_a);
        btn_buf.shape_until_scroll(fs);
        const float btn_tx = btn_r.left() + (btn_r.width() - btn_buf.max_line_width()) * 0.5f;
        const float btn_ty = btn_r.top() + (btn_r.height() - btn_buf.total_height()) * 0.5f;
        draw_buf(btn_buf, btn_tx, btn_ty, Color::WHITE);

        Attrs btn_note;
        btn_note.set_family(Family("Mono")).set_color(TextColor::rgb(148, 175, 206));
        auto bn = pcache.get_or_layout(fs, "TextHeightBehavior{false, false}\nTrims top/bottom leading", btn_note, Metrics(12.0f, 18.0f));
        draw_buf(*bn, sx + 308.0f, btn_y + 4.0f, Color::WHITE);
    }

    // -------------------------------------------------------------------------
    // Card 6 (Bottom-Right): Phase 4 — Incremental Updates, ParagraphCache & visit()
    // -------------------------------------------------------------------------
    {
        const float cx = 740.0f, cy = 718.0f, cw = 660.0f, ch = 268.0f;
        draw_card(cx, cy, cw, ch, Color::from_rgba8(190, 125, 255, 255));
        draw_card_header(cx, cy, "Incremental Updates, ParagraphCache & visit()", "PHASE 4", Color::from_rgba8(190, 125, 255, 255));

        const float vx = cx + 26.0f;
        const float vy = cy + 64.0f;

        // Shape ONCE, then mutate align, background, gradient foreground, and font size IN-PLACE!
        Buffer live_buf(Metrics(18.0f, 28.0f));
        live_buf.set_size(604.0f, std::nullopt);
        live_buf.set_align(Align::Left);
        Attrs live_base;
        live_base.set_font_families({Family("InterVar"), Family("ArabicVar")})
                 .set_color(TextColor::rgb(228, 236, 250));
        live_buf.set_text("Zero-Reshape Live Editor — تحديث فوري", live_base);
        live_buf.shape_until_scroll(fs);

        auto live_grad = LinearGradient::create(
            Point::from_xy(vx + 60.0f, 0.0f),
            Point::from_xy(vx + 460.0f, 0.0f),
            {
                GradientStop::create(0.0f, Color::from_rgba8(255, 130, 210, 255)),
                GradientStop::create(1.0f, Color::from_rgba8(110, 215, 255, 255)),
            }
        );
        live_buf.update_text_align(Align::Center);
        live_buf.update_background_paint(0, 12, Paint(Color::from_rgba8(72, 54, 128, 215)));
        live_buf.update_foreground_paint(13, 24, Paint{Shader{*live_grad}});
        live_buf.update_font_size(13, 17, 26.0f); // Enlarge "Live" in-place without re-shaping

        draw_buf(live_buf, vx, vy, Color::WHITE);

        // Inspect laid-out runs via Buffer::visit() and overlay run origin & cluster markers
        size_t visited_runs = 0;
        size_t visited_glyphs = 0;
        size_t rtl_runs = 0;
        live_buf.visit(fs, [&](const GlyphRunInfo& run) {
            ++visited_runs;
            visited_glyphs += run.glyph_ids.size();
            if (run.is_rtl) ++rtl_runs;
            if (!run.glyph_ids.empty()) {
                const float x0 = run.origin.x + run.positions.front().x;
                const float x1 = x0 + run.advance_width;
                const float y_base = run.origin.y + 4.0f;
                const Color run_c = run.is_rtl
                    ? Color::from_rgba8(255, 185, 80, 180)
                    : Color::from_rgba8(190, 125, 255, 175);
                backend.stroke_line(x0, y_base, x1, y_base, Paint(run_c), Stroke(2.0f));
                backend.fill_circle(x0, y_base, 3.0f, Paint(run_c));
            }
        }, vx, vy);

        // Repeated label queries demonstrating instant ParagraphCache LRU hits
        Attrs cache_probe_a;
        cache_probe_a.set_family(Family("InterVar")).set_color(TextColor::rgb(220, 230, 245));
        for (int k = 0; k < 4; ++k) {
            (void)pcache.get_or_layout(fs, "Repeated UI Label", cache_probe_a, Metrics(14.0f, 20.0f), 200.0f);
        }

        // Telemetry console box proving shape_count() == 1 + ParagraphCache hits + visit() stats
        backend.fill_round_rect(*Rect::from_xywh(vx, cy + 128.0f, 608.0f, 114.0f), 10.0f, 10.0f, Paint(Color::from_rgba8(13, 18, 30, 225)));
        std::ostringstream ts;
        ts << "Incremental In-Place Mutations (update_text_align / paint / font_size):\n"
           << "  shape_count() = " << live_buf.shape_count() << "   (ZERO full re-shapes!)\n"
           << "Buffer::visit() Telemetry:\n"
           << "  runs=" << visited_runs << "  |  glyphs=" << visited_glyphs << "  |  rtl_runs=" << rtl_runs << "\n"
           << "ParagraphCache LRU Telemetry:  entries=" << pcache.size()
           << "  |  hits=" << pcache.hits() << "  |  misses=" << pcache.misses();

        Attrs t_note;
        t_note.set_family(Family("Mono")).set_color(TextColor::rgb(212, 176, 255));
        Buffer t_buf(Metrics(12.2f, 19.5f));
        t_buf.set_text(ts.str(), t_note);
        t_buf.shape_until_scroll(fs);
        draw_buf(t_buf, vx + 14.0f, cy + 138.0f, Color::WHITE);
    }
}

} // namespace

int main() {
    constexpr uint32_t W = 1440;
    constexpr uint32_t H = 1020;

    FontSystem fs;
    fs.set_auto_system_fonts(false);
    auto id_var_en = fs.load_font_file(resolve_font("InterVariable.ttf"), 0, "InterVar");
    auto id_var_ar = fs.load_font_file(resolve_font("NotoSansArabic.ttf"), 0, "ArabicVar");
    auto id_mono = fs.load_font_file(resolve_font("FiraMono-Medium.ttf"), 0, "Mono");
    if (!id_var_en || !id_var_ar || !id_mono) {
        std::cerr << "Failed to load bundled fonts from fonts/ directory." << std::endl;
        return 1;
    }
    fs.set_default_font(*id_var_en);

    ParagraphCache pcache(128);

    // -------------------------------------------------------------------------
    // 1. CPU Render (Canvas)
    // -------------------------------------------------------------------------
    auto cpu_pm = Pixmap::create(W, H);
    if (!cpu_pm) {
        std::cerr << "Failed to allocate CPU Pixmap." << std::endl;
        return 1;
    }
    {
        Canvas cpu_canvas(*cpu_pm);
        GlyphCache gcache(2048);
        // Prime & render once (cache misses on first pass, hits on GPU pass!)
        render_showcase_scene(
            cpu_canvas,
            [&](Buffer& b, float x, float y, Color c) { b.draw(cpu_canvas, gcache, fs, c, x, y); },
            fs, pcache, W, H
        );
    }

    const std::string cpu_out = "nisaba_text_advanced_cpu.png";
    cpu_pm->save_png(cpu_out);
    std::cout << "[CPU] Saved " << cpu_out << " (" << W << "x" << H << ")" << std::endl;

#ifdef NISABA_HAS_GPU
    // -------------------------------------------------------------------------
    // 2. GPU Render (GpuCanvas) + Live CPU-vs-GPU Parity Verification
    // -------------------------------------------------------------------------
    auto device = gpu::GpuDevice::create();
    if (device) {
        auto surface = gpu::GpuSurface::create(device, W, H);
        if (surface) {
            gpu::GpuCanvas gpu_canvas(surface);
            render_showcase_scene(
                gpu_canvas,
                [&](Buffer& b, float x, float y, Color c) { gpu_canvas.draw_text_buffer(b, fs, Point::from_xy(x, y), c); },
                fs, pcache, W, H
            );
            gpu_canvas.flush();
            if (auto gpu_pm = surface->to_pixmap()) {
                const std::string gpu_out = "nisaba_text_advanced_gpu.png";
                gpu_pm->save_png(gpu_out);

                const auto cr = cpu_pm->as_ref();
                const auto gr = gpu_pm->as_ref();
                double sum_abs = 0.0;
                for (uint32_t y = 0; y < H; ++y) {
                    for (uint32_t x = 0; x < W; ++x) {
                        auto cp = cr.pixel(x, y);
                        auto gp = gr.pixel(x, y);
                        if (cp && gp) {
                            sum_abs += std::abs(int(cp->r) - int(gp->r))
                                     + std::abs(int(cp->g) - int(gp->g))
                                     + std::abs(int(cp->b) - int(gp->b));
                        }
                    }
                }
                const double mae = sum_abs / (static_cast<double>(W) * static_cast<double>(H) * 3.0);
                std::cout << "[GPU] Saved " << gpu_out << " | CPU-vs-GPU Parity MAE = "
                          << std::fixed << std::setprecision(3) << mae << " / 255" << std::endl;
            }
        }
    }
#endif

    std::cout << "[ParagraphCache] size=" << pcache.size()
              << " hits=" << pcache.hits()
              << " misses=" << pcache.misses()
              << " hit_ratio=" << std::fixed << std::setprecision(1) << (pcache.hit_ratio() * 100.0) << "%" << std::endl;
    return 0;
}

