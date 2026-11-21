#include "nisaba/markdown/pdf_exporter.hpp"
#include "nisaba/markdown/math_engine.hpp"
#include "nisaba/markdown/diagram_engine.hpp"
#include "nisaba/pdf/pdf_writer.hpp"
#include <fstream>
#include <sstream>
#include <numeric>
#include <algorithm>

namespace nisaba::markdown {

namespace {

inline Rect make_rect(float x, float y, float w, float h) {
    return Rect::from_xywh(x, y, w, h).value_or(Rect());
}

std::vector<std::string> wrap_text(std::string_view text, float max_width, float font_size, float char_ratio = 0.52f) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    float max_chars = std::max(1.0f, max_width / (font_size * char_ratio));
    size_t limit = static_cast<size_t>(max_chars);

    std::istringstream iss{std::string(text)};
    std::string word;
    std::string cur;

    while (iss >> word) {
        if (cur.empty()) {
            cur = word;
        } else if (cur.size() + 1 + word.size() <= limit) {
            cur += " " + word;
        } else {
            lines.push_back(std::move(cur));
            cur = word;
        }
    }
    if (!cur.empty()) {
        lines.push_back(std::move(cur));
    }
    return lines;
}

std::string extract_plain_text(const std::vector<InlineSpan>& inlines) {
    std::string s;
    for (const auto& span : inlines) {
        if (span.type == InlineType::SoftBreak) {
            s.push_back(' ');
        } else if (span.type == InlineType::LineBreak) {
            s.push_back('\n');
        } else if (span.type == InlineType::HtmlInline) {
            continue;
        } else if (span.type == InlineType::Math) {
            auto m = MathEngine::parse(span.text, false);
            s += m ? MathEngine::to_plain_text(*m) : span.text;
        } else if (!span.children.empty()) {
            s += extract_plain_text(span.children);
        } else {
            s += span.text;
        }
    }
    return s;
}

} // namespace

std::vector<uint8_t> MarkdownPdfExporter::export_to_bytes(
    MarkdownDocument& doc,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache,
    const MarkdownPdfExportOptions& options
) {
    if (!doc.is_valid()) return {};

    pdf::PdfDocument pdf_doc;
    pdf_doc.set_title(options.doc_title);
    pdf_doc.set_author(options.doc_author);
    pdf_doc.set_subject(options.doc_subject);

    float page_w = options.page_width;
    float page_h = options.page_height;
    float margin_l = options.margin_left;
    float margin_r = options.margin_right;
    float margin_t = options.margin_top;
    float margin_b = options.margin_bottom;

    float content_w = page_w - margin_l - margin_r;
    float content_h = page_h - margin_t - margin_b;
    if (content_w <= 20.0f || content_h <= 20.0f) return {};

    // Layout markdown document for the target PDF content width
    doc.layout(content_w, style, font_system, glyph_cache);

    pdf::PdfPage* cur_page = pdf_doc.add_page(page_w, page_h);
    float cur_y = margin_t;

    auto ensure_space = [&](float required_h) {
        if (cur_y + required_h > margin_t + content_h && cur_y > margin_t) {
            cur_page = pdf_doc.add_page(page_w, page_h);
            cur_y = margin_t;
        }
    };

    for (const auto& block_ptr : doc.root()->children) {
        if (!block_ptr) continue;
        const auto& block = *block_ptr;

        switch (block.type) {
            case BlockType::Heading: {
                int lvl = std::clamp(block.heading_level, 1, 6);
                float font_sz = style.heading_sizes[lvl - 1];
                float top_m = style.heading_top_margins[lvl - 1] * 0.75f;
                float bot_m = style.heading_bottom_margins[lvl - 1] * 0.75f;

                std::string title = extract_plain_text(block.inlines);
                auto lines = wrap_text(title, content_w, font_sz, 0.58f);
                float block_h = static_cast<float>(lines.size()) * font_sz * 1.25f + top_m + bot_m;

                ensure_space(block_h);
                cur_y += top_m;

                for (const auto& line : lines) {
                    cur_page->canvas().draw_text(line, margin_l, cur_y, font_sz, style.heading_color, "Helvetica-Bold");
                    cur_y += font_sz * 1.25f;
                }

                // Decorative divider line for H1 and H2
                if (lvl <= 2) {
                    cur_page->canvas().stroke_rect(
                        make_rect(margin_l, cur_y + 2.0f, content_w, 0.75f),
                        Paint(style.divider_color),
                        Stroke(0.75f)
                    );
                    cur_y += 4.0f;
                }
                cur_y += bot_m;
                break;
            }

            case BlockType::Paragraph: {
                std::string full_text = extract_plain_text(block.inlines);
                auto lines = wrap_text(full_text, content_w, style.base_font_size);
                float line_h = style.base_font_size * style.line_height_multiplier;
                float block_h = static_cast<float>(lines.size()) * line_h + style.paragraph_spacing;

                ensure_space(block_h);

                for (const auto& line : lines) {
                    cur_page->canvas().draw_text(line, margin_l, cur_y, style.base_font_size, style.text_color, "Helvetica");
                    cur_y += line_h;
                }
                cur_y += style.paragraph_spacing;
                break;
            }

            case BlockType::Alert: {
                const auto& theme = (block.alert_type == AlertType::Tip) ? style.alert_tip :
                                    (block.alert_type == AlertType::Important) ? style.alert_important :
                                    (block.alert_type == AlertType::Warning) ? style.alert_warning :
                                    (block.alert_type == AlertType::Caution) ? style.alert_caution :
                                    style.alert_note;

                float alert_h = std::max(block.layout_height, 40.0f);
                ensure_space(alert_h);

                // Background card & bar
                Rect bg_rect = make_rect(margin_l, cur_y, content_w, alert_h);
                cur_page->canvas().fill_rect(bg_rect, Paint(theme.bg_color));

                Rect bar_rect = make_rect(margin_l, cur_y, 4.0f, alert_h);
                cur_page->canvas().fill_rect(bar_rect, Paint(theme.bar_color));

                // Title header
                std::string title_hdr = !block.alert_title.empty() ? block.alert_title : "Note";
                cur_page->canvas().draw_text(
                    title_hdr,
                    margin_l + 14.0f,
                    cur_y + 6.0f,
                    style.base_font_size * 0.95f,
                    theme.title_color,
                    "Helvetica-Bold"
                );

                // Inner content lines
                float sub_y = cur_y + 24.0f;
                float inner_w = content_w - 24.0f;

                for (const auto& child : block.children) {
                    if (!child) continue;
                    std::string txt = extract_plain_text(child->inlines);
                    auto lines = wrap_text(txt, inner_w, style.base_font_size * 0.92f);
                    float line_h = style.base_font_size * 1.3f;

                    for (const auto& line : lines) {
                        cur_page->canvas().draw_text(
                            line, margin_l + 14.0f, sub_y,
                            style.base_font_size * 0.92f, style.text_color, "Helvetica"
                        );
                        sub_y += line_h;
                    }
                    sub_y += 4.0f;
                }

                cur_y += alert_h + style.paragraph_spacing;
                break;
            }

            case BlockType::Details: {
                float det_h = std::max(block.layout_height, 32.0f);
                ensure_space(det_h);

                Rect box_rect = make_rect(margin_l, cur_y, content_w, det_h);
                cur_page->canvas().fill_rect(box_rect, Paint(style.details_bg_color));
                cur_page->canvas().stroke_rect(box_rect, Paint(style.details_border_color), Stroke(1.0f));

                std::string summary = block.is_open ? "▼ " : "► ";
                summary += block.summary_text.empty() ? "Details" : block.summary_text;
                cur_page->canvas().draw_text(
                    summary, margin_l + 10.0f, cur_y + 6.0f,
                    style.base_font_size * 0.95f, style.details_summary_color, "Helvetica-Bold"
                );

                float sub_y = cur_y + 24.0f;
                for (const auto& child : block.children) {
                    if (!child) continue;
                    std::string txt = extract_plain_text(child->inlines);
                    auto lines = wrap_text(txt, content_w - 20.0f, style.base_font_size * 0.9f);
                    float line_h = style.base_font_size * 1.3f;
                    for (const auto& l : lines) {
                        cur_page->canvas().draw_text(l, margin_l + 14.0f, sub_y, style.base_font_size * 0.9f, style.text_color, "Helvetica");
                        sub_y += line_h;
                    }
                }

                cur_y += det_h + style.paragraph_spacing;
                break;
            }

            case BlockType::CodeBlock: {
                float code_h = block.layout_height > 0.0f ? block.layout_height : (static_cast<float>(block.code_lines.size()) * 16.0f + 20.0f);
                ensure_space(code_h);

                Rect card_rect = make_rect(margin_l, cur_y, content_w, code_h);
                cur_page->canvas().fill_rect(card_rect, Paint(style.code_block_bg));
                cur_page->canvas().stroke_rect(card_rect, Paint(style.code_block_border), Stroke(1.0f));

                // Language tag badge
                if (!block.language.empty()) {
                    float badge_x = margin_l + content_w - static_cast<float>(block.language.size()) * 7.0f - 12.0f;
                    cur_page->canvas().draw_text(
                        block.language, badge_x, cur_y + 6.0f,
                        style.code_font_size * 0.85f, Color::from_rgba8(140, 165, 195, 200), "Courier-Bold"
                    );
                }

                float code_y = cur_y + style.code_block_padding;
                float line_h = style.code_font_size * style.code_line_height_multiplier;
                int line_num = 1;

                for (const auto& line : block.code_lines) {
                    float text_x = margin_l + style.code_block_padding;

                    if (style.show_line_numbers) {
                        std::string num_str = std::to_string(line_num++);
                        cur_page->canvas().draw_text(
                            num_str, text_x, code_y,
                            style.code_font_size * 0.85f, style.syn_linenumber, "Courier"
                        );
                        text_x += style.line_number_margin * 0.85f;
                    }

                    cur_page->canvas().draw_text(
                        line, text_x, code_y,
                        style.code_font_size, style.code_text_color, "Courier"
                    );
                    code_y += line_h;
                }

                cur_y += code_h + style.paragraph_spacing;
                break;
            }

            case BlockType::BlockQuote: {
                float quote_h = block.layout_height > 0.0f ? block.layout_height : 36.0f;
                ensure_space(quote_h);

                cur_page->canvas().fill_rect(
                    make_rect(margin_l, cur_y, style.quote_bar_width, quote_h),
                    Paint(style.quote_bar_color)
                );

                float sub_y = cur_y + style.quote_vertical_padding;
                for (const auto& child : block.children) {
                    if (!child) continue;
                    std::string txt = extract_plain_text(child->inlines);
                    auto lines = wrap_text(txt, content_w - style.quote_left_padding - 10.0f, style.quote_font_size);
                    float line_h = style.quote_font_size * 1.35f;

                    for (const auto& l : lines) {
                        cur_page->canvas().draw_text(
                            l, margin_l + style.quote_left_padding, sub_y,
                            style.quote_font_size, style.quote_text_color, "Helvetica-Oblique"
                        );
                        sub_y += line_h;
                    }
                }

                cur_y += quote_h + style.paragraph_spacing;
                break;
            }

            case BlockType::List: {
                int item_num = block.list_start_number;
                bool is_loose = block.is_loose_list || !block.is_tight_list;
                float item_gap = is_loose ? style.paragraph_spacing : style.list_item_spacing;

                for (const auto& item_ptr : block.children) {
                    if (!item_ptr) continue;
                    const auto& item = *item_ptr;

                    float indent = margin_l + static_cast<float>(item.list_level) * 18.0f;
                    std::string full_txt = extract_plain_text(item.inlines);
                    auto lines = wrap_text(full_txt, content_w - indent - 24.0f, style.base_font_size);
                    float line_h = style.base_font_size * style.line_height_multiplier;
                    float item_h = std::max(static_cast<float>(lines.size()) * line_h, line_h) + item_gap;

                    ensure_space(item_h);

                    if (item.is_task_item) {
                        Rect cb_rect = make_rect(indent, cur_y + 2.0f, 11.0f, 11.0f);
                        cur_page->canvas().stroke_rect(cb_rect, Paint(style.task_checkbox_border), Stroke(1.0f));
                        if (item.is_task_checked) {
                            Rect inner = make_rect(indent + 2.5f, cur_y + 4.5f, 6.0f, 6.0f);
                            cur_page->canvas().fill_rect(inner, Paint(style.task_checkbox_checked));
                        }
                    } else if (block.is_ordered_list) {
                        std::string num_str = std::to_string(item_num++) + ".";
                        cur_page->canvas().draw_text(
                            num_str, indent, cur_y,
                            style.base_font_size, style.quote_bar_color, "Helvetica-Bold"
                        );
                    } else {
                        cur_page->canvas().draw_text(
                            "•", indent + 2.0f, cur_y,
                            style.base_font_size * 1.2f, style.quote_bar_color, "Helvetica-Bold"
                        );
                    }

                    float text_x = indent + 20.0f;
                    float line_y = cur_y;
                    for (const auto& l : lines) {
                        cur_page->canvas().draw_text(
                            l, text_x, line_y,
                            style.base_font_size, style.text_color, "Helvetica"
                        );
                        line_y += line_h;
                    }

                    cur_y += item_h;
                }

                cur_y += style.paragraph_spacing * 0.5f;
                break;
            }

            case BlockType::Table: {
                size_t num_cols = block.table_alignments.size();
                if (num_cols == 0 && !block.table_rows.empty()) num_cols = block.table_rows[0].cells.size();
                if (num_cols == 0) break;

                // Dynamic Column Width Solver
                std::vector<float> desired_widths(num_cols, 40.0f);
                for (const auto& row : block.table_rows) {
                    for (size_t c = 0; c < row.cells.size() && c < num_cols; ++c) {
                        std::string txt = extract_plain_text(row.cells[c].inlines);
                        float txt_w = static_cast<float>(txt.size()) * (style.base_font_size * 0.58f) + 20.0f;
                        desired_widths[c] = std::max(desired_widths[c], txt_w);
                    }
                }

                float total_desired = std::accumulate(desired_widths.begin(), desired_widths.end(), 0.0f);
                float available_w = content_w - style.table_border_width * static_cast<float>(num_cols + 1);

                std::vector<float> col_widths(num_cols);
                for (size_t c = 0; c < num_cols; ++c) {
                    if (total_desired > 1.0f) {
                        col_widths[c] = std::max(45.0f, (desired_widths[c] / total_desired) * available_w);
                    } else {
                        col_widths[c] = available_w / static_cast<float>(num_cols);
                    }
                }

                float tbl_h = block.layout_height > 0.0f ? block.layout_height : (static_cast<float>(block.table_rows.size()) * 26.0f);
                ensure_space(tbl_h);

                // Table outer frame
                cur_page->canvas().stroke_rect(
                    make_rect(margin_l, cur_y, content_w, tbl_h),
                    Paint(style.table_border_color),
                    Stroke(style.table_border_width)
                );

                float row_top = cur_y;
                for (size_t r = 0; r < block.table_rows.size(); ++r) {
                    const auto& row = block.table_rows[r];
                    float row_h = row.layout_height > 0.0f ? row.layout_height : 26.0f;

                    // Row background
                    if (row.is_header) {
                        cur_page->canvas().fill_rect(make_rect(margin_l, row_top, content_w, row_h), Paint(style.table_header_bg));
                    } else if (r % 2 == 1) {
                        cur_page->canvas().fill_rect(make_rect(margin_l, row_top, content_w, row_h), Paint(style.table_row_alt_bg));
                    }

                    // Row border
                    if (r > 0) {
                        cur_page->canvas().stroke_rect(
                            make_rect(margin_l, row_top, content_w, style.table_border_width),
                            Paint(style.table_border_color),
                            Stroke(style.table_border_width)
                        );
                    }

                    // Cell content
                    float col_x = margin_l + style.table_border_width;
                    for (size_t c = 0; c < row.cells.size() && c < num_cols; ++c) {
                        std::string cell_txt = extract_plain_text(row.cells[c].inlines);
                        std::string_view font_n = row.is_header ? "Helvetica-Bold" : "Helvetica";
                        Color text_c = row.is_header ? style.table_header_text : style.text_color;

                        cur_page->canvas().draw_text(
                            cell_txt,
                            col_x + 6.0f,
                            row_top + 5.0f,
                            style.base_font_size * 0.9f,
                            text_c,
                            font_n
                        );

                        col_x += col_widths[c] + style.table_border_width;
                    }

                    row_top += row_h + style.table_border_width;
                }

                cur_y += tbl_h + style.paragraph_spacing;
                break;
            }

            case BlockType::ThematicBreak: {
                float div_h = 16.0f;
                ensure_space(div_h);
                cur_page->canvas().stroke_rect(
                    make_rect(margin_l, cur_y + 8.0f, content_w, style.divider_thickness),
                    Paint(style.divider_color),
                    Stroke(style.divider_thickness)
                );
                cur_y += div_h;
                break;
            }

            case BlockType::FootnoteDef: {
                std::string fn_txt = "[" + std::to_string(block.footnote_index) + "] ";
                fn_txt += extract_plain_text(block.inlines);
                auto lines = wrap_text(fn_txt, content_w, style.footnote_font_size);
                float line_h = style.footnote_font_size * 1.3f;
                float block_h = static_cast<float>(lines.size()) * line_h + 8.0f;

                ensure_space(block_h);

                for (const auto& l : lines) {
                    cur_page->canvas().draw_text(
                        l, margin_l, cur_y,
                        style.footnote_font_size, style.footnote_text_color, "Helvetica"
                    );
                    cur_y += line_h;
                }
                cur_y += 6.0f;
                break;
            }

            case BlockType::MathBlock: {
                if (block.math) {
                    float mh = block.layout_height > 0.0f ? block.layout_height : block.math->height() + 12.0f;
                    ensure_space(mh);
                    float mx = margin_l + (content_w - block.math->width) * 0.5f;
                    MathEngine::render(cur_page->canvas(), *block.math, mx, cur_y + 6.0f,
                                       style.text_color, font_system, glyph_cache);
                    cur_y += mh + style.paragraph_spacing;
                }
                break;
            }

            case BlockType::Diagram: {
                if (block.diagram) {
                    float dh = block.diagram->height;
                    ensure_space(dh);
                    DiagramEngine::render(cur_page->canvas(), *block.diagram, margin_l, cur_y,
                                          content_w, style, font_system, glyph_cache);
                    cur_y += dh + style.paragraph_spacing;
                }
                break;
            }

            default:
                break;
        }
    }

    // Apply Headers, Footers, and Page Numbers across all generated pages
    size_t total_pages = pdf_doc.page_count();
    for (size_t p = 0; p < total_pages; ++p) {
        auto* pg = pdf_doc.page(p);
        if (!pg) continue;

        if (options.show_header) {
            // Header text & divider
            pg->canvas().draw_text(
                options.header_left,
                margin_l,
                margin_t - 22.0f,
                9.0f,
                Color::from_rgba8(120, 135, 155, 200),
                "Helvetica"
            );
            if (!options.header_right.empty()) {
                float rw = static_cast<float>(options.header_right.size()) * 5.2f;
                pg->canvas().draw_text(
                    options.header_right,
                    page_w - margin_r - rw,
                    margin_t - 22.0f,
                    9.0f,
                    Color::from_rgba8(120, 135, 155, 200),
                    "Helvetica"
                );
            }
            pg->canvas().stroke_rect(
                make_rect(margin_l, margin_t - 14.0f, content_w, 0.5f),
                Paint(Color::from_rgba8(100, 115, 135, 120)),
                Stroke(0.5f)
            );
        }

        if (options.show_footer) {
            // Footer divider & text
            pg->canvas().stroke_rect(
                make_rect(margin_l, page_h - margin_b + 12.0f, content_w, 0.5f),
                Paint(Color::from_rgba8(100, 115, 135, 120)),
                Stroke(0.5f)
            );
            if (!options.footer_left.empty()) {
                pg->canvas().draw_text(
                    options.footer_left,
                    margin_l,
                    page_h - margin_b + 22.0f,
                    9.0f,
                    Color::from_rgba8(120, 135, 155, 200),
                    "Helvetica"
                );
            }
            if (options.show_page_numbers) {
                std::string page_str = "Page " + std::to_string(p + 1) + " of " + std::to_string(total_pages);
                float pw = static_cast<float>(page_str.size()) * 5.2f;
                pg->canvas().draw_text(
                    page_str,
                    page_w - margin_r - pw,
                    page_h - margin_b + 22.0f,
                    9.0f,
                    Color::from_rgba8(120, 135, 155, 200),
                    "Helvetica"
                );
            }
        }
    }

    return pdf_doc.save_to_bytes();
}

bool MarkdownPdfExporter::export_to_file(
    MarkdownDocument& doc,
    const std::string& output_path,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache,
    const MarkdownPdfExportOptions& options
) {
    auto bytes = export_to_bytes(doc, style, font_system, glyph_cache, options);
    if (bytes.empty()) return false;

    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out.good();
}

} // namespace nisaba::markdown
