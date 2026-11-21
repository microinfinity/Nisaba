#include "nisaba/markdown/document.hpp"
#include "nisaba/markdown/parser.hpp"
#include "nisaba/markdown/math_engine.hpp"
#include "nisaba/markdown/diagram_engine.hpp"
#include "nisaba/text/buffer.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <numeric>

namespace nisaba::markdown {

namespace {

void flatten_inlines_rec(
    const std::vector<InlineSpan>& inlines,
    std::string& out_text,
    text::AttrsList& out_attrs,
    const text::Attrs& inherited,
    const MarkdownStyle& style,
    float font_size
) {
    auto to_tc = [](const Color& c) noexcept {
        return text::TextColor::rgba(
            static_cast<uint8_t>(std::clamp(c.red() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(c.green() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(c.blue() * 255.0f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(c.alpha() * 255.0f, 0.0f, 255.0f))
        );
    };

    for (const auto& span : inlines) {
        size_t start = out_text.size();
        text::Attrs cur = inherited;

        if (span.text_color) cur.set_color(to_tc(*span.text_color));
        if (span.bg_color) cur.set_background_color(to_tc(*span.bg_color));

        auto emit_leaf = [&](std::string_view leaf) {
            if (leaf.empty()) return;
            size_t s0 = out_text.size();
            out_text.append(leaf);
            out_attrs.add_span(s0, out_text.size(), cur);
        };

        switch (span.type) {
            case InlineType::Text:
            case InlineType::Emoji:
            case InlineType::Custom:
                emit_leaf(span.text);
                break;

            case InlineType::Bold:
                cur.set_weight(text::Weight::Bold);
                if (!span.children.empty()) flatten_inlines_rec(span.children, out_text, out_attrs, cur, style, font_size);
                else emit_leaf(span.text);
                break;

            case InlineType::Italic:
                cur.set_style(text::Style::Italic);
                if (!span.children.empty()) flatten_inlines_rec(span.children, out_text, out_attrs, cur, style, font_size);
                else emit_leaf(span.text);
                break;

            case InlineType::BoldItalic:
                cur.set_weight(text::Weight::Bold);
                cur.set_style(text::Style::Italic);
                if (!span.children.empty()) flatten_inlines_rec(span.children, out_text, out_attrs, cur, style, font_size);
                else emit_leaf(span.text);
                break;

            case InlineType::CodeSpan:
                cur.set_family(text::Family::monospace());
                cur.set_font_size(font_size * 0.92f);
                if (!span.text_color) cur.set_color(to_tc(style.code_span_text));
                out_text.push_back(' ');
                out_text += span.text;
                out_text.push_back(' ');
                out_attrs.add_span(start, out_text.size(), cur);
                break;

            case InlineType::Kbd:
                cur.set_family(text::Family::monospace());
                cur.set_font_size(font_size * 0.88f);
                cur.set_weight(text::Weight::Bold);
                cur.set_color(to_tc(style.kbd_text_color));
                out_text.push_back(' ');
                out_text += span.text;
                out_text.push_back(' ');
                out_attrs.add_span(start, out_text.size(), cur);
                break;

            case InlineType::Link:
                if (!span.text_color) cur.set_color(to_tc(style.link_color));
                cur.set_underline(true);
                if (!span.children.empty()) flatten_inlines_rec(span.children, out_text, out_attrs, cur, style, font_size);
                else emit_leaf(span.text);
                break;

            case InlineType::Strikethrough:
                cur.set_strikethrough(true);
                if (!span.children.empty()) flatten_inlines_rec(span.children, out_text, out_attrs, cur, style, font_size);
                else emit_leaf(span.text);
                break;

            case InlineType::Underline:
                cur.set_underline(true);
                if (!span.children.empty()) flatten_inlines_rec(span.children, out_text, out_attrs, cur, style, font_size);
                else emit_leaf(span.text);
                break;

            case InlineType::Highlight:
                cur.set_color(to_tc(style.mark_text_color));
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
                cur.set_color(to_tc(style.footnote_link_color));
                cur.set_font_size(font_size * 0.78f);
                out_text.push_back('[');
                out_text += span.text.empty() ? std::to_string(span.footnote_index) : span.text;
                out_text.push_back(']');
                out_attrs.add_span(start, out_text.size(), cur);
                break;

            case InlineType::LineBreak:
                out_text.push_back('\n');
                break;

            case InlineType::SoftBreak:
                out_text.push_back(' ');
                break;

            case InlineType::Math: {
                cur.set_style(text::Style::Italic);
                auto m = MathEngine::parse(span.text, false);
                emit_leaf(m ? MathEngine::to_plain_text(*m) : span.text);
                break;
            }

            case InlineType::Image:
                out_text += "[Image: ";
                out_text += span.text;
                out_text.push_back(']');
                out_attrs.add_span(start, out_text.size(), cur);
                break;

            case InlineType::HtmlInline:
                break;
        }
    }
}

void flatten_inlines(
    const std::vector<InlineSpan>& inlines,
    std::string& out_text,
    text::AttrsList& out_attrs,
    const MarkdownStyle& style,
    float font_size
) {
    text::Attrs base;
    base.set_family(text::Family::sans_serif());
    flatten_inlines_rec(inlines, out_text, out_attrs, base, style, font_size);
}

float measure_inline_block(
    const std::vector<InlineSpan>& inlines,
    float width,
    float font_size,
    float line_height_mult,
    const MarkdownStyle& style,
    text::FontSystem& font_system
) {
    if (inlines.empty()) return 0.0f;

    std::string text;
    text::AttrsList attrs;
    flatten_inlines(inlines, text, attrs, style, font_size);

    if (text.empty()) return 0.0f;

    float line_h = font_size * line_height_mult;
    text::Buffer buf(text::Metrics(font_size, line_h));
    buf.set_wrap(text::Wrap::Word);
    buf.set_size(width > 10.0f ? std::optional<float>(width) : std::nullopt, std::nullopt);
    buf.set_text(text, attrs.defaults());

    if (!buf.lines().empty()) {
        auto& first_line = buf.lines_mut()[0];
        first_line.attrs_list_mut() = attrs;
    }

    buf.shape_until_scroll(font_system);
    return std::max(line_h, buf.total_height());
}

} // namespace

MarkdownDocument::MarkdownDocument()
    : root_(std::make_shared<BlockNode>(BlockType::Document)) {}

MarkdownDocument::MarkdownDocument(std::shared_ptr<BlockNode> root)
    : root_(std::move(root)) {}

MarkdownDocument MarkdownDocument::from_string(std::string_view markdown_text) {
    return MarkdownDocument(MarkdownParser::parse(markdown_text));
}

MarkdownDocument MarkdownDocument::from_string(
    std::string_view markdown_text,
    const MarkdownParseOptions& options,
    const MarkdownExtensionRegistry* extensions
) {
    return MarkdownDocument(MarkdownParser::parse(markdown_text, options, extensions));
}

MarkdownDocument MarkdownDocument::from_file(std::string_view file_path) {
    return from_file(file_path, MarkdownParseOptions{}, nullptr);
}

MarkdownDocument MarkdownDocument::from_file(
    std::string_view file_path,
    const MarkdownParseOptions& options,
    const MarkdownExtensionRegistry* extensions
) {
    std::ifstream file(std::string(file_path), std::ios::binary);
    if (!file.is_open()) {
        return MarkdownDocument();
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return from_string(ss.str(), options, extensions);
}

std::string MarkdownDocument::to_html(const MarkdownHtmlExportOptions& options) const {
    return MarkdownExporter::to_html(*this, options);
}

std::string MarkdownDocument::to_plaintext() const {
    return MarkdownExporter::to_plaintext(*this);
}

std::string MarkdownDocument::to_markdown(const MarkdownFormatOptions& options) const {
    return MarkdownExporter::to_markdown(*this, options);
}

float MarkdownDocument::layout(
    float max_width,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& cache
) {
    (void)cache;
    layout_width_ = max_width;
    total_height_ = 0.0f;
    if (!root_) return 0.0f;

    float current_y = 0.0f;

    for (auto& block_ptr : root_->children) {
        if (!block_ptr) continue;
        auto& block = *block_ptr;
        block.layout_x = 0.0f;
        block.layout_width = max_width;

        switch (block.type) {
            case BlockType::Heading: {
                int lvl = std::clamp(block.heading_level, 1, 6);
                float fsize = style.heading_sizes[static_cast<size_t>(lvl - 1)];
                float top_margin = style.heading_top_margins[static_cast<size_t>(lvl - 1)];
                float bot_margin = style.heading_bottom_margins[static_cast<size_t>(lvl - 1)];

                current_y += top_margin;
                block.layout_y = current_y;

                float h = measure_inline_block(
                    block.inlines, max_width, fsize,
                    style.heading_line_height_multiplier, style, font_system
                );
                block.layout_height = h;
                current_y += h + bot_margin;
                break;
            }

            case BlockType::Paragraph: {
                block.layout_y = current_y;
                float h = measure_inline_block(
                    block.inlines, max_width, style.base_font_size,
                    style.line_height_multiplier, style, font_system
                );
                block.layout_height = h;
                current_y += h + style.paragraph_spacing;
                break;
            }

            case BlockType::CodeBlock: {
                block.layout_y = current_y;
                float line_h = style.code_font_size * style.code_line_height_multiplier;
                size_t n_lines = std::max(size_t{1}, block.code_lines.size());
                float h = static_cast<float>(n_lines) * line_h + 2.0f * style.code_block_padding;
                block.layout_height = h;
                current_y += h + style.paragraph_spacing;
                break;
            }

            case BlockType::Alert: {
                block.layout_y = current_y;
                float inner_w = max_width - style.quote_left_padding - 20.0f;
                float sub_y = 32.0f; // Space for alert icon and title header

                for (auto& child : block.children) {
                    if (!child) continue;
                    child->layout_x = style.quote_left_padding + 16.0f;
                    child->layout_y = sub_y;
                    child->layout_width = inner_w;
                    float ch = measure_inline_block(
                        child->inlines, inner_w, style.base_font_size,
                        style.line_height_multiplier, style, font_system
                    );
                    child->layout_height = ch;
                    sub_y += ch + style.paragraph_spacing * 0.5f;
                }

                float h = sub_y + 12.0f;
                block.layout_height = h;
                current_y += h + style.paragraph_spacing;
                break;
            }

            case BlockType::BlockQuote: {
                block.layout_y = current_y;
                float quote_inner_w = max_width - style.quote_left_padding - style.quote_bar_width;
                float sub_y = 0.0f;

                for (auto& child : block.children) {
                    if (!child) continue;
                    child->layout_x = style.quote_left_padding + style.quote_bar_width;
                    child->layout_y = sub_y;
                    child->layout_width = quote_inner_w;
                    float ch = measure_inline_block(
                        child->inlines, quote_inner_w, style.quote_font_size,
                        style.line_height_multiplier, style, font_system
                    );
                    child->layout_height = ch;
                    sub_y += ch + style.paragraph_spacing * 0.5f;
                }

                float h = sub_y + 2.0f * style.quote_vertical_padding;
                block.layout_height = h;
                current_y += h + style.paragraph_spacing;
                break;
            }

            case BlockType::Details: {
                block.layout_y = current_y;
                float summary_h = 36.0f;
                float details_h = summary_h;

                if (block.is_open) {
                    float sub_y = summary_h + 8.0f;
                    float inner_w = max_width - 32.0f;
                    for (auto& child : block.children) {
                        if (!child) continue;
                        child->layout_x = 20.0f;
                        child->layout_y = sub_y;
                        child->layout_width = inner_w;
                        float ch = measure_inline_block(
                            child->inlines, inner_w, style.base_font_size,
                            style.line_height_multiplier, style, font_system
                        );
                        child->layout_height = ch;
                        sub_y += ch + style.paragraph_spacing;
                    }
                    details_h = sub_y + 8.0f;
                }

                block.layout_height = details_h;
                current_y += details_h + style.paragraph_spacing;
                break;
            }

            case BlockType::List: {
                block.layout_y = current_y;
                float list_h = 0.0f;
                bool is_loose = block.is_loose_list || !block.is_tight_list;
                float item_gap = is_loose ? style.paragraph_spacing : style.list_item_spacing;

                for (size_t idx = 0; idx < block.children.size(); ++idx) {
                    auto& item = block.children[idx];
                    if (!item) continue;
                    float indent = style.list_indent * static_cast<float>(item->list_level + 1);
                    float item_w = max_width - indent;

                    item->layout_x = indent;
                    item->layout_y = list_h;
                    item->layout_width = item_w;

                    float ih = 0.0f;
                    if (!item->children.empty()) {
                        for (size_t c = 0; c < item->children.size(); ++c) {
                            auto& ch = item->children[c];
                            if (!ch) continue;
                            float ch_h = 0.0f;
                            if (ch->type == BlockType::CodeBlock) {
                                float lh = style.code_font_size * style.code_line_height_multiplier;
                                ch_h = static_cast<float>(std::max<size_t>(1, ch->code_lines.size())) * lh + 2.0f * style.code_block_padding;
                            } else {
                                ch_h = measure_inline_block(
                                    ch->inlines, item_w, style.base_font_size,
                                    style.line_height_multiplier, style, font_system
                                );
                            }
                            ch->layout_x = 0.0f;
                            ch->layout_y = ih;
                            ch->layout_width = item_w;
                            ch->layout_height = ch_h;
                            ih += ch_h + (c + 1 < item->children.size() ? style.paragraph_spacing * 0.5f : 0.0f);
                        }
                    } else {
                        ih = measure_inline_block(
                            item->inlines, item_w, style.base_font_size,
                            style.line_height_multiplier, style, font_system
                        );
                    }
                    item->layout_height = std::max(ih, style.base_font_size * style.line_height_multiplier);
                    list_h += item->layout_height + (idx + 1 < block.children.size() ? item_gap : 0.0f);
                }

                block.layout_height = list_h;
                current_y += list_h + style.paragraph_spacing;
                break;
            }

            case BlockType::Table: {
                block.layout_y = current_y;
                size_t num_cols = block.table_alignments.size();
                if (num_cols == 0 && !block.table_rows.empty()) {
                    num_cols = block.table_rows[0].cells.size();
                }
                if (num_cols == 0) num_cols = 1;

                // Dynamic Column Width Solver:
                // 1. Measure max desired width for each column based on cell text lengths
                std::vector<float> desired_widths(num_cols, 40.0f);
                for (const auto& row : block.table_rows) {
                    for (size_t c = 0; c < row.cells.size() && c < num_cols; ++c) {
                        std::string cell_txt;
                        text::AttrsList dummy_attrs;
                        flatten_inlines(row.cells[c].inlines, cell_txt, dummy_attrs, style, style.base_font_size);
                        float txt_w = static_cast<float>(cell_txt.size()) * (style.base_font_size * 0.58f) + 2.0f * style.table_cell_padding_h;
                        desired_widths[c] = std::max(desired_widths[c], txt_w);
                    }
                }

                float total_desired = std::accumulate(desired_widths.begin(), desired_widths.end(), 0.0f);
                float available_w = max_width - style.table_border_width * static_cast<float>(num_cols + 1);

                // Proportional expansion or scaling
                std::vector<float> col_widths(num_cols);
                for (size_t c = 0; c < num_cols; ++c) {
                    if (total_desired > 1.0f) {
                        col_widths[c] = std::max(50.0f, (desired_widths[c] / total_desired) * available_w);
                    } else {
                        col_widths[c] = available_w / static_cast<float>(num_cols);
                    }
                }

                float table_h = style.table_border_width;
                for (auto& row : block.table_rows) {
                    float row_h = style.base_font_size * style.line_height_multiplier + 2.0f * style.table_cell_padding_v;
                    for (size_t c = 0; c < row.cells.size() && c < num_cols; ++c) {
                        float cell_h = measure_inline_block(
                            row.cells[c].inlines, col_widths[c] - 2.0f * style.table_cell_padding_h,
                            style.base_font_size, style.line_height_multiplier, style, font_system
                        ) + 2.0f * style.table_cell_padding_v;
                        row_h = std::max(row_h, cell_h);
                    }
                    row.layout_height = row_h;
                    table_h += row_h + style.table_border_width;
                }

                block.layout_height = table_h;
                current_y += table_h + style.paragraph_spacing;
                break;
            }

            case BlockType::ThematicBreak: {
                current_y += style.divider_margin;
                block.layout_y = current_y;
                block.layout_height = style.divider_thickness;
                current_y += style.divider_thickness + style.divider_margin;
                break;
            }

            case BlockType::FootnoteDef: {
                block.layout_y = current_y;
                float h = measure_inline_block(
                    block.inlines, max_width - 30.0f, style.footnote_font_size,
                    style.line_height_multiplier, style, font_system
                );
                block.layout_height = h + 4.0f;
                current_y += block.layout_height + 4.0f;
                break;
            }

            case BlockType::MathBlock: {
                if (!block.math) {
                    block.math = MathEngine::parse(block.source, true);
                }
                float math_size = style.base_font_size * 1.35f;
                float h = 0.0f;
                if (block.math) {
                    MathEngine::layout(*block.math, math_size, true, font_system);
                    h = block.math->height();
                }
                block.layout_y = current_y + style.paragraph_spacing * 0.5f;
                block.layout_height = h + 12.0f;
                current_y = block.layout_y + block.layout_height + style.paragraph_spacing;
                break;
            }

            case BlockType::Diagram: {
                if (!block.diagram) {
                    block.diagram = DiagramEngine::parse(block.source);
                }
                block.layout_y = current_y;
                float h = 0.0f;
                if (block.diagram) {
                    h = DiagramEngine::layout(*block.diagram, max_width, style, font_system);
                }
                block.layout_height = h;
                current_y += h + style.paragraph_spacing;
                break;
            }

            case BlockType::Custom: {
                block.layout_y = current_y;
                float h = measure_inline_block(
                    block.inlines, max_width, style.base_font_size,
                    style.line_height_multiplier, style, font_system
                );
                block.layout_height = h;
                current_y += h + (h > 0.0f ? style.paragraph_spacing : 0.0f);
                break;
            }

            case BlockType::HtmlBlock:
            case BlockType::ListItem:
            case BlockType::Document:
                break;
        }
    }

    total_height_ = current_y;
    return total_height_;
}

HitTestResult MarkdownDocument::hit_test(float x, float y) const {
    HitTestResult res;
    if (!root_) return res;

    for (const auto& block_ptr : root_->children) {
        if (!block_ptr) continue;
        const auto& block = *block_ptr;

        if (y < block.layout_y || y > block.layout_y + block.layout_height) {
            continue;
        }

        // 1. Details summary hit test
        if (block.type == BlockType::Details) {
            if (y >= block.layout_y && y <= block.layout_y + 36.0f) {
                res.type = HitTargetType::DetailsToggle;
                res.node_ref = const_cast<BlockNode*>(&block);
                res.bounds = *Rect::from_xywh(block.layout_x, block.layout_y, block.layout_width, 36.0f);
                res.is_checked = block.is_open;
                return res;
            }
        }

        // 2. Task list item checkbox hit test
        if (block.type == BlockType::List) {
            for (const auto& item_ptr : block.children) {
                if (!item_ptr) continue;
                const auto& item = *item_ptr;
                float item_top = block.layout_y + item.layout_y;
                if (y >= item_top && y <= item_top + item.layout_height) {
                    if (item.is_task_item && x >= item.layout_x - 22.0f && x <= item.layout_x + 2.0f) {
                        res.type = HitTargetType::TaskCheckbox;
                        res.node_ref = const_cast<BlockNode*>(&item);
                        res.bounds = *Rect::from_xywh(item.layout_x - 20.0f, item_top + 2.0f, 16.0f, 16.0f);
                        res.is_checked = item.is_task_checked;
                        return res;
                    }
                }
            }
        }

        // 3. Inlines hit test (links, footnotes, images)
        auto check_inlines = [&](const std::vector<InlineSpan>& inlines, float base_y) -> bool {
            for (const auto& span : inlines) {
                if (span.type == InlineType::Link) {
                    res.type = HitTargetType::Link;
                    res.target = span.target;
                    res.title = span.title;
                    res.bounds = *Rect::from_xywh(block.layout_x, base_y, block.layout_width, 24.0f);
                    return true;
                }
                if (span.type == InlineType::FootnoteRef) {
                    res.type = HitTargetType::Footnote;
                    res.index = span.footnote_index;
                    res.target = std::to_string(span.footnote_index);
                    return true;
                }
                if (span.type == InlineType::Image) {
                    res.type = HitTargetType::Image;
                    res.target = span.target;
                    res.title = span.title;
                    return true;
                }
            }
            return false;
        };

        if (check_inlines(block.inlines, block.layout_y)) {
            return res;
        }
    }

    return res;
}

std::vector<std::pair<std::string, std::string>> MarkdownDocument::table_of_contents() const {
    std::vector<std::pair<std::string, std::string>> toc;
    if (!root_) return toc;

    for (const auto& block_ptr : root_->children) {
        if (!block_ptr) continue;
        if (block_ptr->type == BlockType::Heading) {
            std::string title;
            for (const auto& span : block_ptr->inlines) {
                title += span.text;
            }
            toc.emplace_back(std::move(title), block_ptr->anchor_id);
        }
    }
    return toc;
}

bool MarkdownDocument::toggle_details(void* node_ref) {
    if (!node_ref || !root_) return false;
    for (auto& block_ptr : root_->children) {
        if (block_ptr.get() == node_ref) {
            block_ptr->is_open = !block_ptr->is_open;
            return true;
        }
    }
    return false;
}

bool MarkdownDocument::toggle_task_item(void* node_ref) {
    if (!node_ref || !root_) return false;
    for (auto& block_ptr : root_->children) {
        if (block_ptr->type == BlockType::List) {
            for (auto& item : block_ptr->children) {
                if (item.get() == node_ref) {
                    item->is_task_checked = !item->is_task_checked;
                    return true;
                }
            }
        }
    }
    return false;
}

bool MarkdownDocument::handle_click(
    float x,
    float y,
    const std::function<void(const std::string& target)>& on_link_click
) {
    auto hit = hit_test(x, y);
    if (hit.type == HitTargetType::TaskCheckbox) {
        return toggle_task_item(hit.node_ref);
    }
    if (hit.type == HitTargetType::DetailsToggle) {
        return toggle_details(hit.node_ref);
    }
    if (hit.type == HitTargetType::Link || hit.type == HitTargetType::Footnote || hit.type == HitTargetType::HeadingAnchor) {
        if (on_link_click && !hit.target.empty()) {
            on_link_click(hit.target);
        }
        return false;
    }
    return false;
}

MarkdownDocument::CursorType MarkdownDocument::get_cursor_at(float x, float y) const {
    auto hit = hit_test(x, y);
    if (hit.type == HitTargetType::Link ||
        hit.type == HitTargetType::TaskCheckbox ||
        hit.type == HitTargetType::DetailsToggle ||
        hit.type == HitTargetType::Footnote ||
        hit.type == HitTargetType::HeadingAnchor) {
        return CursorType::Pointer;
    }

    if (root_) {
        for (const auto& block : root_->children) {
            if (!block) continue;
            if (y >= block->layout_y && y <= block->layout_y + block->layout_height &&
                x >= block->layout_x && x <= block->layout_x + block->layout_width) {
                if (block->type == BlockType::Paragraph || block->type == BlockType::Heading || block->type == BlockType::CodeBlock) {
                    return CursorType::Text;
                }
            }
        }
    }
    return CursorType::Default;
}

} // namespace nisaba::markdown
