#include "nisaba/markdown/renderer.hpp"
#include "nisaba/markdown/syntax_highlighter.hpp"
#include "nisaba/markdown/math_engine.hpp"
#include "nisaba/markdown/diagram_engine.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/text/buffer.hpp"
#include "nisaba/pdf/pdf_canvas.hpp"
#ifdef NISABA_GLEW
#include "nisaba/gpu/gpu_canvas.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nisaba::markdown {

namespace {

inline text::TextColor to_text_color(const Color& c) noexcept {
    return text::TextColor::rgba(
        static_cast<uint8_t>(std::clamp(c.red() * 255.0f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(c.green() * 255.0f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(c.blue() * 255.0f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(c.alpha() * 255.0f, 0.0f, 255.0f))
    );
}

void stroke_line_ic(ICanvas& canvas, float x0, float y0, float x1, float y1, const Paint& paint, const Stroke& stroke) {
    PathBuilder pb;
    pb.move_to(Point::from_xy(x0, y0));
    pb.line_to(Point::from_xy(x1, y1));
    if (auto p = pb.finish(); p && !p->is_empty()) {
        canvas.stroke_path(*p, paint, stroke);
    }
}

void fill_circle_ic(ICanvas& canvas, float cx, float cy, float r, const Paint& paint) {
    if (auto oval = Rect::from_xywh(cx - r, cy - r, r * 2.0f, r * 2.0f)) {
        canvas.fill_oval(*oval, paint);
    }
}

void stroke_circle_ic(ICanvas& canvas, float cx, float cy, float r, const Paint& paint, const Stroke& stroke) {
    if (auto oval = Rect::from_xywh(cx - r, cy - r, r * 2.0f, r * 2.0f)) {
        canvas.stroke_oval(*oval, paint, stroke);
    }
}

void draw_buffer_on_canvas(
    ICanvas& canvas,
    text::Buffer& buf,
    std::string_view fallback_text,
    float x,
    float y,
    float font_size,
    bool monospace,
    bool bold,
    Color default_color,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    if (canvas.canvas_kind() == CanvasKind::Software) {
        auto& cpu_canvas = static_cast<Canvas&>(canvas);
        buf.draw(cpu_canvas, glyph_cache, font_system, default_color, x, y);
        return;
    }

#ifdef NISABA_GLEW
    if (canvas.canvas_kind() == CanvasKind::Gpu) {
        auto& gpu_canvas = static_cast<gpu::GpuCanvas&>(canvas);
        gpu_canvas.draw_text_buffer(buf, font_system, Point::from_xy(x, y), default_color);
        return;
    }
#endif

    if (canvas.canvas_kind() == CanvasKind::Pdf) {
        auto& pdf_canvas = static_cast<pdf::PdfCanvas&>(canvas);
        std::string_view font_name = monospace
            ? (bold ? "Courier-Bold" : "Courier")
            : (bold ? "Helvetica-Bold" : "Helvetica");
        pdf_canvas.draw_text(fallback_text, x, y, font_size, default_color, font_name);
    }
}

void draw_styled_label(
    ICanvas& canvas,
    std::string_view text,
    float x,
    float y,
    float font_size,
    bool monospace,
    bool bold,
    Color color,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    if (text.empty()) return;

    text::Attrs attrs;
    attrs.set_family(monospace ? text::Family::monospace() : text::Family::sans_serif());
    if (bold) attrs.set_weight(text::Weight::Bold);
    attrs.set_color(to_text_color(color));

    text::Buffer buf(text::Metrics(font_size, font_size * 1.25f));
    buf.set_wrap(text::Wrap::None);
    buf.set_text(text, attrs);
    buf.shape_until_scroll(font_system);

    draw_buffer_on_canvas(canvas, buf, text, x, y, font_size, monospace, bold, color, font_system, glyph_cache);
}

void render_inlines_to_canvas(
    ICanvas& canvas,
    const std::vector<InlineSpan>& inlines,
    float x,
    float y,
    float width,
    float font_size,
    float line_height_mult,
    Color default_color,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache,
    text::Align text_align = text::Align::Left
) {
    if (inlines.empty()) return;

    std::string text;
    text::Attrs default_attr;
    default_attr.set_family(text::Family::sans_serif());
    default_attr.set_color(to_text_color(default_color));
    text::AttrsList attrs(default_attr);

    auto append_span = [&](auto& self, const InlineSpan& span, const text::Attrs& inherited) -> void {
        size_t start = text.size();
        text::Attrs cur = inherited;

        if (span.text_color) {
            cur.set_color(to_text_color(*span.text_color));
        }
        if (span.bg_color) {
            cur.set_background_color(to_text_color(*span.bg_color));
        }

        auto emit_leaf = [&](std::string_view leaf_str) {
            if (leaf_str.empty()) return;
            size_t s0 = text.size();
            text.append(leaf_str);
            attrs.add_span(s0, text.size(), cur);
        };

        switch (span.type) {
            case InlineType::Text:
            case InlineType::Emoji:
            case InlineType::Custom:
                emit_leaf(span.text);
                break;

            case InlineType::Bold:
                cur.set_weight(text::Weight::Bold);
                if (!span.children.empty()) {
                    for (const auto& ch : span.children) self(self, ch, cur);
                } else {
                    emit_leaf(span.text);
                }
                break;

            case InlineType::Italic:
                cur.set_style(text::Style::Italic);
                if (!span.children.empty()) {
                    for (const auto& ch : span.children) self(self, ch, cur);
                } else {
                    emit_leaf(span.text);
                }
                break;

            case InlineType::BoldItalic:
                cur.set_weight(text::Weight::Bold);
                cur.set_style(text::Style::Italic);
                if (!span.children.empty()) {
                    for (const auto& ch : span.children) self(self, ch, cur);
                } else {
                    emit_leaf(span.text);
                }
                break;

            case InlineType::Strikethrough:
                cur.set_strikethrough(true);
                if (!span.children.empty()) {
                    for (const auto& ch : span.children) self(self, ch, cur);
                } else {
                    emit_leaf(span.text);
                }
                break;

            case InlineType::Underline:
                cur.set_underline(true);
                if (!span.children.empty()) {
                    for (const auto& ch : span.children) self(self, ch, cur);
                } else {
                    emit_leaf(span.text);
                }
                break;

            case InlineType::CodeSpan:
                cur.set_family(text::Family::monospace());
                cur.set_font_size(font_size * 0.92f);
                if (!span.text_color) cur.set_color(to_text_color(style.code_span_text));
                if (!span.bg_color) cur.set_background_color(to_text_color(style.code_span_bg));
                text.push_back(' ');
                text += span.text;
                text.push_back(' ');
                attrs.add_span(start, text.size(), cur);
                break;

            case InlineType::Kbd:
                cur.set_family(text::Family::monospace());
                cur.set_font_size(font_size * 0.88f);
                cur.set_weight(text::Weight::Bold);
                if (!span.text_color) cur.set_color(to_text_color(style.kbd_text_color));
                if (!span.bg_color) cur.set_background_color(to_text_color(style.kbd_bg_color));
                text.push_back(' ');
                text += span.text;
                text.push_back(' ');
                attrs.add_span(start, text.size(), cur);
                break;

            case InlineType::Link:
                if (!span.text_color) cur.set_color(to_text_color(style.link_color));
                cur.set_underline(true);
                if (!span.children.empty()) {
                    for (const auto& ch : span.children) self(self, ch, cur);
                } else {
                    emit_leaf(span.text);
                }
                break;

            case InlineType::Highlight:
                if (!span.text_color) cur.set_color(to_text_color(style.mark_text_color));
                if (!span.bg_color) cur.set_background_color(to_text_color(style.mark_bg_color));
                emit_leaf(span.text);
                break;

            case InlineType::Subscript:
                cur.set_font_size(font_size * 0.78f);
                cur.set_baseline_shift(font_size * 0.22f);
                emit_leaf(span.text);
                break;

            case InlineType::Superscript:
                cur.set_font_size(font_size * 0.78f);
                cur.set_baseline_shift(-font_size * 0.28f);
                emit_leaf(span.text);
                break;

            case InlineType::FootnoteRef:
                cur.set_color(to_text_color(style.footnote_link_color));
                cur.set_font_size(font_size * 0.78f);
                cur.set_baseline_shift(-font_size * 0.28f);
                text.push_back('[');
                text += span.text.empty() ? std::to_string(span.footnote_index) : span.text;
                text.push_back(']');
                attrs.add_span(start, text.size(), cur);
                break;

            case InlineType::LineBreak:
                text.push_back('\n');
                break;

            case InlineType::SoftBreak:
                text.push_back(' ');
                break;

            case InlineType::Math: {
                cur.set_style(text::Style::Italic);
                auto m = MathEngine::parse(span.text, false);
                emit_leaf(m ? MathEngine::to_plain_text(*m) : span.text);
                break;
            }

            case InlineType::Image:
                text += "[Image: ";
                text += span.text;
                text.push_back(']');
                attrs.add_span(start, text.size(), cur);
                break;

            case InlineType::HtmlInline:
                break;
        }
    };

    for (const auto& span : inlines) {
        append_span(append_span, span, default_attr);
    }

    if (text.empty()) return;

    float line_h = font_size * line_height_mult;
    text::Buffer buf(text::Metrics(font_size, line_h));
    buf.set_wrap(text::Wrap::Word);
    buf.set_align(text_align);
    buf.set_size(width > 10.0f ? std::optional<float>(width) : std::nullopt, std::nullopt);
    buf.set_text(text, attrs.defaults());

    if (!buf.lines().empty()) {
        buf.lines_mut()[0].attrs_list_mut() = attrs;
    }

    buf.shape_until_scroll(font_system);
    draw_buffer_on_canvas(canvas, buf, text, x, y, font_size, false, false, default_color, font_system, glyph_cache);
}

void draw_alert_icon(ICanvas& canvas, AlertType type, float cx, float cy, float r, Color color) {
    Paint p(color);
    Stroke stroke(1.5f);

    switch (type) {
        case AlertType::Note: {
            stroke_circle_ic(canvas, cx, cy, r, p, stroke);
            fill_circle_ic(canvas, cx, cy - r * 0.45f, 1.4f, p);
            stroke_line_ic(canvas, cx, cy - r * 0.15f, cx, cy + r * 0.45f, p, stroke);
            break;
        }
        case AlertType::Tip: {
            stroke_circle_ic(canvas, cx, cy - 2.0f, r * 0.8f, p, stroke);
            stroke_line_ic(canvas, cx - 3.0f, cy + 4.0f, cx + 3.0f, cy + 4.0f, p, stroke);
            stroke_line_ic(canvas, cx - 2.0f, cy + 6.5f, cx + 2.0f, cy + 6.5f, p, stroke);
            break;
        }
        case AlertType::Important: {
            auto rect = Rect::from_xywh(cx - r, cy - r, 2.0f * r, 2.0f * r);
            if (rect) canvas.stroke_round_rect(*rect, 3.0f, 3.0f, p, stroke);
            stroke_line_ic(canvas, cx, cy - r * 0.45f, cx, cy + r * 0.15f, p, stroke);
            fill_circle_ic(canvas, cx, cy + r * 0.45f, 1.4f, p);
            break;
        }
        case AlertType::Warning: {
            float h = r * 1.7f;
            stroke_line_ic(canvas, cx, cy - h * 0.5f, cx - r, cy + h * 0.5f, p, stroke);
            stroke_line_ic(canvas, cx - r, cy + h * 0.5f, cx + r, cy + h * 0.5f, p, stroke);
            stroke_line_ic(canvas, cx + r, cy + h * 0.5f, cx, cy - h * 0.5f, p, stroke);
            stroke_line_ic(canvas, cx, cy - h * 0.15f, cx, cy + h * 0.15f, p, stroke);
            fill_circle_ic(canvas, cx, cy + h * 0.35f, 1.2f, p);
            break;
        }
        case AlertType::Caution: {
            float s = r * 0.85f;
            stroke_circle_ic(canvas, cx, cy, r, p, stroke);
            stroke_line_ic(canvas, cx - s * 0.6f, cy, cx + s * 0.6f, cy, p, Stroke(2.0f));
            break;
        }
        case AlertType::None:
            break;
    }
}

void draw_chevron(ICanvas& canvas, float cx, float cy, float s, bool is_open, Color color) {
    Paint p(color);
    Stroke stroke(1.8f);

    if (is_open) {
        stroke_line_ic(canvas, cx - s, cy - s * 0.4f, cx, cy + s * 0.5f, p, stroke);
        stroke_line_ic(canvas, cx, cy + s * 0.5f, cx + s, cy - s * 0.4f, p, stroke);
    } else {
        stroke_line_ic(canvas, cx - s * 0.4f, cy - s, cx + s * 0.5f, cy, p, stroke);
        stroke_line_ic(canvas, cx + s * 0.5f, cy, cx - s * 0.4f, cy + s, p, stroke);
    }
}

} // namespace

void MarkdownRenderer::render(
    Canvas& canvas,
    MarkdownDocument& doc,
    float x,
    float y,
    float width,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache,
    float scroll_y
) {
    render(static_cast<ICanvas&>(canvas), doc, x, y, width, style, font_system, glyph_cache, scroll_y);
}

void MarkdownRenderer::render(
    ICanvas& canvas,
    MarkdownDocument& doc,
    float x,
    float y,
    float width,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache,
    float scroll_y
) {
    if (!doc.is_valid()) return;

    if (doc.layout_width() != width) {
        doc.layout(width, style, font_system, glyph_cache);
    }

    float origin_y = y - scroll_y;
    uint32_t viewport_h = canvas.canvas_height();

    for (const auto& block_ptr : doc.root()->children) {
        if (!block_ptr) continue;
        const auto& block = *block_ptr;

        float block_top = origin_y + block.layout_y;
        float block_bot = block_top + block.layout_height;

        if (viewport_h > 0 && (block_bot < 0.0f || block_top > static_cast<float>(viewport_h))) {
            continue;
        }

        render_block(canvas, block, x, block_top, width, style, font_system, glyph_cache);
    }
}

void MarkdownRenderer::render_block(
    ICanvas& canvas,
    const BlockNode& block,
    float x,
    float block_top,
    float width,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    float block_bot = block_top + block.layout_height;

    switch (block.type) {
        case BlockType::Heading: {
            int lvl = std::clamp(block.heading_level, 1, 6);
            float fsize = style.heading_sizes[static_cast<size_t>(lvl - 1)];

            render_inlines_to_canvas(
                canvas, block.inlines, x, block_top, width, fsize,
                style.heading_line_height_multiplier, style.heading_color,
                style, font_system, glyph_cache
            );

            if (lvl <= 2) {
                float underline_y = block_bot - 2.0f;
                Paint line_p(style.divider_color);
                auto r = Rect::from_xywh(x, underline_y, width, lvl == 1 ? 2.0f : 1.0f);
                if (r) canvas.fill_rect(*r, line_p);
            }
            break;
        }

        case BlockType::Paragraph:
        case BlockType::Custom: {
            text::Align align = text::Align::Left;
            if (block.block_align == 1) align = text::Align::Center;
            else if (block.block_align == 2) align = text::Align::Right;

            render_inlines_to_canvas(
                canvas, block.inlines, x, block_top, width, style.base_font_size,
                style.line_height_multiplier, style.text_color,
                style, font_system, glyph_cache, align
            );
            break;
        }

        case BlockType::Alert: {
            const MarkdownStyle::AlertTheme* theme = &style.alert_note;
            if (block.alert_type == AlertType::Tip) theme = &style.alert_tip;
            else if (block.alert_type == AlertType::Important) theme = &style.alert_important;
            else if (block.alert_type == AlertType::Warning) theme = &style.alert_warning;
            else if (block.alert_type == AlertType::Caution) theme = &style.alert_caution;

            auto card = Rect::from_xywh(x, block_top, width, block.layout_height);
            if (card) {
                Paint bg_p(theme->bg_color);
                canvas.fill_round_rect(*card, 6.0f, 6.0f, bg_p);

                auto bar = Rect::from_xywh(x, block_top, 4.0f, block.layout_height);
                if (bar) {
                    Paint bar_p(theme->bar_color);
                    canvas.fill_round_rect(*bar, 2.0f, 2.0f, bar_p);
                }
            }

            draw_alert_icon(canvas, block.alert_type, x + 20.0f, block_top + 17.0f, 7.5f, theme->title_color);
            draw_styled_label(canvas, block.alert_title, x + 34.0f, block_top + 8.0f,
                              style.base_font_size * 0.95f, false, true, theme->title_color, font_system, glyph_cache);

            for (const auto& child : block.children) {
                if (!child) continue;
                float c_top = block_top + child->layout_y;
                render_inlines_to_canvas(
                    canvas, child->inlines, x + child->layout_x, c_top, child->layout_width,
                    style.base_font_size, style.line_height_multiplier,
                    style.text_color, style, font_system, glyph_cache
                );
            }
            break;
        }

        case BlockType::Details: {
            auto card = Rect::from_xywh(x, block_top, width, block.layout_height);
            if (card) {
                Paint bg_p(style.details_bg_color);
                canvas.fill_round_rect(*card, 6.0f, 6.0f, bg_p);

                Paint border_p(style.details_border_color);
                canvas.stroke_round_rect(*card, 6.0f, 6.0f, border_p, Stroke(1.0f));
            }

            draw_chevron(canvas, x + 16.0f, block_top + 18.0f, 6.0f, block.is_open, style.details_chevron_color);
            draw_styled_label(canvas, block.summary_text, x + 28.0f, block_top + 9.0f,
                              style.base_font_size * 0.95f, false, true, style.details_summary_color,
                              font_system, glyph_cache);

            if (block.is_open) {
                for (const auto& child : block.children) {
                    if (!child) continue;
                    float c_top = block_top + child->layout_y;
                    render_inlines_to_canvas(
                        canvas, child->inlines, x + child->layout_x, c_top, child->layout_width,
                        style.base_font_size, style.line_height_multiplier,
                        style.text_color, style, font_system, glyph_cache
                    );
                }
            }
            break;
        }

        case BlockType::CodeBlock: {
            auto card_rect = Rect::from_xywh(x, block_top, width, block.layout_height);
            if (card_rect) {
                Paint bg_p(style.code_block_bg);
                canvas.fill_round_rect(*card_rect, style.code_block_corner_radius, style.code_block_corner_radius, bg_p);

                Paint border_p(style.code_block_border);
                canvas.stroke_round_rect(*card_rect, style.code_block_corner_radius, style.code_block_corner_radius, border_p, Stroke(1.0f));
            }

            if (!block.language.empty()) {
                float badge_pad = 14.0f;
                float badge_x = x + width - badge_pad - static_cast<float>(block.language.size()) * 7.5f;
                draw_styled_label(canvas, block.language, badge_x, block_top + 8.0f,
                                  style.code_font_size * 0.82f, true, true,
                                  Color::from_rgba8(140, 165, 195, 200), font_system, glyph_cache);
            }

            float code_x = x + style.code_block_padding;
            float code_y = block_top + style.code_block_padding;
            float line_h = style.code_font_size * style.code_line_height_multiplier;

            bool in_multi_comment = false;
            int line_num = 1;

            text::Attrs base_code_attr;
            base_code_attr.set_family(text::Family::monospace());
            base_code_attr.set_color(to_text_color(style.code_text_color));

            for (const auto& line : block.code_lines) {
                float cur_x = code_x;

                if (style.show_line_numbers) {
                    std::string num_str = std::to_string(line_num++);
                    draw_styled_label(canvas, num_str, cur_x, code_y,
                                      style.code_font_size * 0.85f, true, false,
                                      style.syn_linenumber, font_system, glyph_cache);
                    cur_x += style.line_number_margin;
                }

                if (!line.empty()) {
                    text::Buffer buf(text::Metrics(style.code_font_size, line_h));
                    buf.set_wrap(text::Wrap::None);

                    if (style.enable_syntax_highlighting && !block.language.empty()) {
                        auto tokens = SyntaxHighlighter::tokenize_line(line, block.language, in_multi_comment);
                        std::string full_line;
                        full_line.reserve(line.size());
                        text::AttrsList tok_attrs(base_code_attr);
                        for (const auto& tok : tokens) {
                            size_t s0 = full_line.size();
                            full_line += tok.text;
                            text::Attrs ta = base_code_attr;
                            ta.set_color(to_text_color(SyntaxHighlighter::token_color(tok.type, style)));
                            tok_attrs.add_span(s0, full_line.size(), ta);
                        }
                        buf.set_text(full_line, base_code_attr);
                        if (!buf.lines().empty()) {
                            buf.lines_mut()[0].attrs_list_mut() = std::move(tok_attrs);
                        }
                        buf.shape_until_scroll(font_system);
                        draw_buffer_on_canvas(canvas, buf, full_line, cur_x, code_y,
                                              style.code_font_size, true, false, style.code_text_color,
                                              font_system, glyph_cache);
                    } else {
                        buf.set_text(line, base_code_attr);
                        buf.shape_until_scroll(font_system);
                        draw_buffer_on_canvas(canvas, buf, line, cur_x, code_y,
                                              style.code_font_size, true, false, style.code_text_color,
                                              font_system, glyph_cache);
                    }
                }

                code_y += line_h;
            }
            break;
        }

        case BlockType::BlockQuote: {
            auto quote_rect = Rect::from_xywh(x, block_top, width, block.layout_height);
            if (quote_rect) {
                Paint bg_p(style.quote_bg_color);
                canvas.fill_round_rect(*quote_rect, 4.0f, 4.0f, bg_p);

                auto bar_rect = Rect::from_xywh(x, block_top, style.quote_bar_width, block.layout_height);
                if (bar_rect) {
                    Paint bar_p(style.quote_bar_color);
                    canvas.fill_round_rect(*bar_rect, 2.0f, 2.0f, bar_p);
                }
            }

            float child_x = x + style.quote_left_padding + style.quote_bar_width;
            for (const auto& child : block.children) {
                if (!child) continue;
                float c_top = block_top + child->layout_y + style.quote_vertical_padding;
                render_inlines_to_canvas(
                    canvas, child->inlines, child_x, c_top, child->layout_width,
                    style.quote_font_size, style.line_height_multiplier,
                    style.quote_text_color, style, font_system, glyph_cache
                );
            }
            break;
        }

        case BlockType::List: {
            int item_num = block.list_start_number;
            for (const auto& item_ptr : block.children) {
                if (!item_ptr) continue;
                const auto& item = *item_ptr;
                float i_top = block_top + item.layout_y;
                float bullet_x = x + item.layout_x - 14.0f;

                if (item.is_task_item) {
                    float box_size = 14.0f;
                    float box_x = x + item.layout_x - 22.0f;
                    float box_y = i_top + 4.0f;

                    auto cb_rect = Rect::from_xywh(box_x, box_y, box_size, box_size);
                    if (cb_rect) {
                        Paint cb_border(style.task_checkbox_border);
                        canvas.stroke_round_rect(*cb_rect, 3.0f, 3.0f, cb_border, Stroke(1.3f));

                        if (item.is_task_checked) {
                            auto inner = Rect::from_xywh(box_x + 3.0f, box_y + 3.0f, box_size - 6.0f, box_size - 6.0f);
                            if (inner) {
                                Paint fill_p(style.task_checkbox_checked);
                                canvas.fill_round_rect(*inner, 2.0f, 2.0f, fill_p);
                            }
                        }
                    }
                } else if (block.is_ordered_list) {
                    std::string num_str = std::to_string(item_num++) + ".";
                    draw_styled_label(canvas, num_str, bullet_x - 8.0f, i_top,
                                      style.base_font_size * 0.92f, false, true,
                                      style.quote_bar_color, font_system, glyph_cache);
                } else {
                    Paint bullet_p(style.quote_bar_color);
                    if (item.list_level == 0) {
                        fill_circle_ic(canvas, bullet_x, i_top + style.base_font_size * 0.62f, 3.0f, bullet_p);
                    } else if (item.list_level == 1) {
                        stroke_circle_ic(canvas, bullet_x, i_top + style.base_font_size * 0.62f, 2.8f, bullet_p, Stroke(1.2f));
                    } else {
                        auto sq = Rect::from_xywh(bullet_x - 2.5f, i_top + style.base_font_size * 0.62f - 2.5f, 5.0f, 5.0f);
                        if (sq) canvas.fill_rect(*sq, bullet_p);
                    }
                }

                if (!item.children.empty()) {
                    for (const auto& ch : item.children) {
                        if (!ch) continue;
                        render_block(canvas, *ch, x + item.layout_x, i_top + ch->layout_y,
                                     item.layout_width, style, font_system, glyph_cache);
                    }
                } else {
                    render_inlines_to_canvas(
                        canvas, item.inlines, x + item.layout_x, i_top, item.layout_width,
                        style.base_font_size, style.line_height_multiplier,
                        style.text_color, style, font_system, glyph_cache
                    );
                }
            }
            break;
        }

        case BlockType::Table: {
            size_t num_cols = block.table_alignments.size();
            if (num_cols == 0 && !block.table_rows.empty()) num_cols = block.table_rows[0].cells.size();
            if (num_cols == 0) break;

            std::vector<float> desired_widths(num_cols, 40.0f);
            for (const auto& row : block.table_rows) {
                for (size_t c = 0; c < row.cells.size() && c < num_cols; ++c) {
                    std::string cell_txt;
                    for (const auto& span : row.cells[c].inlines) cell_txt += span.text;
                    float txt_w = static_cast<float>(cell_txt.size()) * (style.base_font_size * 0.58f) + 2.0f * style.table_cell_padding_h;
                    desired_widths[c] = std::max(desired_widths[c], txt_w);
                }
            }

            float total_desired = std::accumulate(desired_widths.begin(), desired_widths.end(), 0.0f);
            float available_w = width - style.table_border_width * static_cast<float>(num_cols + 1);

            std::vector<float> col_widths(num_cols);
            for (size_t c = 0; c < num_cols; ++c) {
                if (total_desired > 1.0f) {
                    col_widths[c] = std::max(50.0f, (desired_widths[c] / total_desired) * available_w);
                } else {
                    col_widths[c] = available_w / static_cast<float>(num_cols);
                }
            }

            auto tbl_rect = Rect::from_xywh(x, block_top, width, block.layout_height);
            if (tbl_rect) {
                Paint tbl_border(style.table_border_color);
                canvas.stroke_round_rect(*tbl_rect, style.table_corner_radius, style.table_corner_radius, tbl_border, Stroke(style.table_border_width));
            }

            float cur_y = block_top + style.table_border_width;
            bool is_first_row = true;

            for (const auto& row : block.table_rows) {
                float row_h = row.layout_height > 0.0f ? row.layout_height : (style.base_font_size * style.line_height_multiplier + 2.0f * style.table_cell_padding_v);

                auto r_bg = Rect::from_xywh(x, cur_y, width, row_h);
                if (r_bg) {
                    if (row.is_header) {
                        Paint hdr_p(style.table_header_bg);
                        canvas.fill_rect(*r_bg, hdr_p);
                    } else if (!is_first_row) {
                        Paint alt_p(style.table_row_alt_bg);
                        canvas.fill_rect(*r_bg, alt_p);
                    }
                }

                float cur_x = x + style.table_border_width;
                for (size_t c = 0; c < row.cells.size() && c < num_cols; ++c) {
                    const auto& cell = row.cells[c];
                    text::Align align = text::Align::Left;
                    if (cell.align == TableAlign::Center) align = text::Align::Center;
                    else if (cell.align == TableAlign::Right) align = text::Align::Right;

                    render_inlines_to_canvas(
                        canvas, cell.inlines, cur_x + style.table_cell_padding_h,
                        cur_y + style.table_cell_padding_v,
                        col_widths[c] - 2.0f * style.table_cell_padding_h,
                        style.base_font_size, style.line_height_multiplier,
                        row.is_header ? style.table_header_text : style.text_color,
                        style, font_system, glyph_cache, align
                    );

                    cur_x += col_widths[c] + style.table_border_width;
                }

                Paint hdiv_p(style.table_border_color);
                stroke_line_ic(canvas, x, cur_y + row_h, x + width, cur_y + row_h, hdiv_p, Stroke(style.table_border_width));

                cur_y += row_h + style.table_border_width;
                is_first_row = false;
            }
            break;
        }

        case BlockType::ThematicBreak: {
            Paint div_p(style.divider_color);
            stroke_line_ic(canvas, x, block_top, x + width, block_top, div_p, Stroke(style.divider_thickness));
            break;
        }

        case BlockType::FootnoteDef: {
            std::string prefix = "[" + std::to_string(block.footnote_index) + "] ";
            draw_styled_label(canvas, prefix, x, block_top,
                              style.footnote_font_size, false, true,
                              style.footnote_link_color, font_system, glyph_cache);

            render_inlines_to_canvas(
                canvas, block.inlines, x + 26.0f, block_top, width - 26.0f,
                style.footnote_font_size, style.line_height_multiplier,
                style.footnote_text_color, style, font_system, glyph_cache
            );
            break;
        }

        case BlockType::MathBlock: {
            if (block.math) {
                float mx = x + (width - block.math->width) * 0.5f;
                float my = block_top + 6.0f;
                MathEngine::render(canvas, *block.math, mx, my, style.text_color, font_system, glyph_cache);
            }
            break;
        }

        case BlockType::Diagram: {
            if (block.diagram) {
                DiagramEngine::render(canvas, *block.diagram, x, block_top, width, style, font_system, glyph_cache);
            }
            break;
        }

        case BlockType::HtmlBlock:
        case BlockType::ListItem:
        case BlockType::Document:
            break;
    }
}

} // namespace nisaba::markdown
