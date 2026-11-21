#include "nisaba/markdown/exporter.hpp"
#include "nisaba/markdown/document.hpp"
#include "nisaba/markdown/html_parser.hpp"
#include "nisaba/markdown/math_engine.hpp"
#include <algorithm>
#include <sstream>

namespace nisaba::markdown {

namespace {

void append_html_escaped(std::string& out, std::string_view text) {
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
}

void append_url_escaped(std::string& out, std::string_view url) {
    for (char c : url) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "%3C"; break;
            case '>': out += "%3E"; break;
            case ' ': out += "%20"; break;
            default: out.push_back(c); break;
        }
    }
}

void append_sourcepos_attr(std::string& out, const SourcePos& pos, bool enabled) {
    if (!enabled || !pos.is_valid()) return;
    out += " data-sourcepos=\"";
    out += std::to_string(pos.start_line);
    out.push_back(':');
    out += std::to_string(pos.start_col);
    out.push_back('-');
    out += std::to_string(pos.end_line);
    out.push_back(':');
    out += std::to_string(pos.end_col);
    out.push_back('"');
}

// ============================================================================
// 4.1 HTML5 Exporter
// ============================================================================

void render_inlines_html(
    const std::vector<InlineSpan>& inlines,
    const MarkdownHtmlExportOptions& opts,
    std::string& out
) {
    for (const auto& span : inlines) {
        switch (span.type) {
            case InlineType::Text:
            case InlineType::Emoji:
                append_html_escaped(out, span.text);
                break;

            case InlineType::SoftBreak:
                if (opts.hard_breaks) out += "<br />\n";
                else if (opts.no_breaks) out.push_back(' ');
                else out.push_back('\n');
                break;

            case InlineType::LineBreak:
                out += "<br />\n";
                break;

            case InlineType::Bold:
                out += "<strong>";
                if (!span.children.empty()) render_inlines_html(span.children, opts, out);
                else append_html_escaped(out, span.text);
                out += "</strong>";
                break;

            case InlineType::Italic:
                out += "<em>";
                if (!span.children.empty()) render_inlines_html(span.children, opts, out);
                else append_html_escaped(out, span.text);
                out += "</em>";
                break;

            case InlineType::BoldItalic:
                out += "<em><strong>";
                if (!span.children.empty()) render_inlines_html(span.children, opts, out);
                else append_html_escaped(out, span.text);
                out += "</strong></em>";
                break;

            case InlineType::Strikethrough:
                out += "<del>";
                if (!span.children.empty()) render_inlines_html(span.children, opts, out);
                else append_html_escaped(out, span.text);
                out += "</del>";
                break;

            case InlineType::CodeSpan:
                out += "<code>";
                append_html_escaped(out, span.text);
                out += "</code>";
                break;

            case InlineType::Link:
                out += "<a href=\"";
                append_url_escaped(out, span.target);
                out += "\"";
                if (!span.title.empty()) {
                    out += " title=\"";
                    append_html_escaped(out, span.title);
                    out += "\"";
                }
                out += ">";
                if (!span.children.empty()) render_inlines_html(span.children, opts, out);
                else append_html_escaped(out, span.text);
                out += "</a>";
                break;

            case InlineType::Image:
                out += "<img src=\"";
                append_url_escaped(out, span.target);
                out += "\" alt=\"";
                append_html_escaped(out, span.text);
                out += "\"";
                if (!span.title.empty()) {
                    out += " title=\"";
                    append_html_escaped(out, span.title);
                    out += "\"";
                }
                out += " />";
                break;

            case InlineType::Kbd:
                out += "<kbd>";
                append_html_escaped(out, span.text);
                out += "</kbd>";
                break;

            case InlineType::Subscript:
                out += "<sub>";
                append_html_escaped(out, span.text);
                out += "</sub>";
                break;

            case InlineType::Superscript:
                out += "<sup>";
                append_html_escaped(out, span.text);
                out += "</sup>";
                break;

            case InlineType::Underline:
                out += "<u>";
                if (!span.children.empty()) render_inlines_html(span.children, opts, out);
                else append_html_escaped(out, span.text);
                out += "</u>";
                break;

            case InlineType::Highlight:
                out += "<mark>";
                append_html_escaped(out, span.text);
                out += "</mark>";
                break;

            case InlineType::FootnoteRef:
                out += "<sup class=\"footnote-ref\"><a href=\"";
                append_url_escaped(out, span.target);
                out += "\">";
                append_html_escaped(out, span.text.empty() ? std::to_string(span.footnote_index) : span.text);
                out += "</a></sup>";
                break;

            case InlineType::Math:
                out += "<span class=\"math math-inline\">";
                append_html_escaped(out, span.text);
                out += "</span>";
                break;

            case InlineType::HtmlInline:
                if (opts.unsafe_html) {
                    if (opts.tagfilter) out += HtmlParser::apply_tagfilter(span.text);
                    else out += span.text;
                } else {
                    out += "<!-- raw HTML omitted -->";
                }
                break;

            case InlineType::Custom: {
                bool handled = false;
                if (opts.extensions) {
                    for (const auto& ext : opts.extensions->extensions()) {
                        if (ext->render_html_inline(span, out)) {
                            handled = true;
                            break;
                        }
                    }
                }
                if (!handled) {
                    out += "<span class=\"md-custom md-";
                    append_html_escaped(out, span.custom_kind);
                    out += "\">";
                    append_html_escaped(out, span.text);
                    out += "</span>";
                }
                break;
            }
        }
    }
}

void render_block_html(
    const BlockNode& block,
    const MarkdownHtmlExportOptions& opts,
    std::string& out
) {
    switch (block.type) {
        case BlockType::Document:
            for (const auto& child : block.children) {
                if (child) render_block_html(*child, opts, out);
            }
            break;

        case BlockType::Heading: {
            int lvl = std::clamp(block.heading_level, 1, 6);
            out += "<h" + std::to_string(lvl);
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += ">";
            render_inlines_html(block.inlines, opts, out);
            out += "</h" + std::to_string(lvl) + ">\n";
            break;
        }

        case BlockType::Paragraph: {
            out += "<p";
            if (block.block_align == 1) out += " align=\"center\"";
            else if (block.block_align == 2) out += " align=\"right\"";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += ">";
            render_inlines_html(block.inlines, opts, out);
            out += "</p>\n";
            break;
        }

        case BlockType::CodeBlock: {
            out += "<pre";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += "><code";
            if (!block.language.empty()) {
                out += " class=\"language-";
                append_html_escaped(out, block.language);
                out += "\"";
            }
            out += ">";
            for (const auto& line : block.code_lines) {
                append_html_escaped(out, line);
                out.push_back('\n');
            }
            out += "</code></pre>\n";
            break;
        }

        case BlockType::BlockQuote: {
            out += "<blockquote";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += ">\n";
            for (const auto& child : block.children) {
                if (child) render_block_html(*child, opts, out);
            }
            out += "</blockquote>\n";
            break;
        }

        case BlockType::Alert: {
            const char* cls = "note";
            if (block.alert_type == AlertType::Tip) cls = "tip";
            else if (block.alert_type == AlertType::Important) cls = "important";
            else if (block.alert_type == AlertType::Warning) cls = "warning";
            else if (block.alert_type == AlertType::Caution) cls = "caution";

            out += "<div class=\"markdown-alert markdown-alert-";
            out += cls;
            out += "\"";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += ">\n<p class=\"markdown-alert-title\">";
            append_html_escaped(out, block.alert_title);
            out += "</p>\n";
            for (const auto& child : block.children) {
                if (child) render_block_html(*child, opts, out);
            }
            out += "</div>\n";
            break;
        }

        case BlockType::List: {
            if (block.is_ordered_list) {
                out += "<ol";
                if (block.list_start_number != 1) {
                    out += " start=\"" + std::to_string(block.list_start_number) + "\"";
                }
                append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
                out += ">\n";
            } else {
                out += "<ul";
                append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
                out += ">\n";
            }

            bool tight = block.is_tight_list && !block.is_loose_list;
            for (const auto& item : block.children) {
                if (!item) continue;
                out += "<li";
                append_sourcepos_attr(out, item->sourcepos, opts.sourcepos);
                out += ">";

                auto emit_checkbox = [&]() {
                    if (item->is_task_item) {
                        out += item->is_task_checked
                            ? "<input type=\"checkbox\" checked=\"\" disabled=\"\" /> "
                            : "<input type=\"checkbox\" disabled=\"\" /> ";
                    }
                };

                if (!item->children.empty()) {
                    if (tight) {
                        bool first = true;
                        for (const auto& ch : item->children) {
                            if (!ch) continue;
                            if (ch->type == BlockType::Paragraph) {
                                if (first) emit_checkbox();
                                render_inlines_html(ch->inlines, opts, out);
                                first = false;
                            } else {
                                out.push_back('\n');
                                render_block_html(*ch, opts, out);
                            }
                        }
                    } else {
                        out.push_back('\n');
                        bool first = true;
                        for (const auto& ch : item->children) {
                            if (!ch) continue;
                            if (first && item->is_task_item && ch->type == BlockType::Paragraph) {
                                out += "<p>";
                                emit_checkbox();
                                render_inlines_html(ch->inlines, opts, out);
                                out += "</p>\n";
                                first = false;
                            } else {
                                render_block_html(*ch, opts, out);
                                first = false;
                            }
                        }
                    }
                } else {
                    if (tight) {
                        emit_checkbox();
                        render_inlines_html(item->inlines, opts, out);
                    } else {
                        out += "\n<p>";
                        emit_checkbox();
                        render_inlines_html(item->inlines, opts, out);
                        out += "</p>\n";
                    }
                }
                out += "</li>\n";
            }

            out += block.is_ordered_list ? "</ol>\n" : "</ul>\n";
            break;
        }

        case BlockType::Table: {
            out += "<table";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += ">\n";
            bool in_tbody = false;
            for (const auto& row : block.table_rows) {
                if (row.is_header) {
                    out += "<thead>\n<tr>\n";
                } else {
                    if (!in_tbody) {
                        out += "<tbody>\n";
                        in_tbody = true;
                    }
                    out += "<tr>\n";
                }
                for (const auto& cell : row.cells) {
                    out += row.is_header ? "<th" : "<td";
                    if (cell.align == TableAlign::Left) out += " align=\"left\"";
                    else if (cell.align == TableAlign::Center) out += " align=\"center\"";
                    else if (cell.align == TableAlign::Right) out += " align=\"right\"";
                    out += ">";
                    render_inlines_html(cell.inlines, opts, out);
                    out += row.is_header ? "</th>\n" : "</td>\n";
                }
                out += "</tr>\n";
                if (row.is_header) {
                    out += "</thead>\n";
                }
            }
            if (in_tbody) out += "</tbody>\n";
            out += "</table>\n";
            break;
        }

        case BlockType::ThematicBreak:
            out += "<hr";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += " />\n";
            break;

        case BlockType::Details: {
            out += block.is_open ? "<details open" : "<details";
            append_sourcepos_attr(out, block.sourcepos, opts.sourcepos);
            out += ">\n<summary>";
            append_html_escaped(out, block.summary_text);
            out += "</summary>\n";
            for (const auto& child : block.children) {
                if (child) render_block_html(*child, opts, out);
            }
            out += "</details>\n";
            break;
        }

        case BlockType::HtmlBlock: {
            if (opts.unsafe_html) {
                if (opts.tagfilter) out += HtmlParser::apply_tagfilter(block.raw_html);
                else out += block.raw_html;
                if (!out.empty() && out.back() != '\n') out.push_back('\n');
            } else {
                out += "<!-- raw HTML omitted -->\n";
            }
            break;
        }

        case BlockType::FootnoteDef: {
            out += "<div class=\"footnote-def\" id=\"";
            append_html_escaped(out, block.anchor_id);
            out += "\">";
            render_inlines_html(block.inlines, opts, out);
            out += "</div>\n";
            break;
        }

        case BlockType::MathBlock: {
            out += "<div class=\"math math-display\">";
            append_html_escaped(out, block.source);
            out += "</div>\n";
            break;
        }

        case BlockType::Diagram: {
            out += "<pre class=\"diagram\"><code class=\"language-mermaid\">";
            append_html_escaped(out, block.source);
            out += "</code></pre>\n";
            break;
        }

        case BlockType::Custom: {
            bool handled = false;
            if (opts.extensions) {
                for (const auto& ext : opts.extensions->extensions()) {
                    if (ext->render_html_block(block, out)) {
                        handled = true;
                        break;
                    }
                }
            }
            if (!handled) {
                out += "<div class=\"md-custom md-";
                append_html_escaped(out, block.custom_kind);
                out += "\">";
                render_inlines_html(block.inlines, opts, out);
                for (const auto& child : block.children) {
                    if (child) render_block_html(*child, opts, out);
                }
                out += "</div>\n";
            }
            break;
        }

        case BlockType::ListItem:
            render_inlines_html(block.inlines, opts, out);
            break;
    }
}

// ============================================================================
// 4.2 Plaintext Extractor
// ============================================================================

void render_inlines_plaintext(const std::vector<InlineSpan>& inlines, std::string& out) {
    for (const auto& span : inlines) {
        switch (span.type) {
            case InlineType::Text:
            case InlineType::Emoji:
            case InlineType::CodeSpan:
            case InlineType::Kbd:
            case InlineType::Subscript:
            case InlineType::Superscript:
            case InlineType::Highlight:
            case InlineType::Custom:
                out += span.text;
                break;

            case InlineType::Bold:
            case InlineType::Italic:
            case InlineType::BoldItalic:
            case InlineType::Strikethrough:
            case InlineType::Underline:
            case InlineType::Link:
            case InlineType::Image:
                if (!span.children.empty()) {
                    render_inlines_plaintext(span.children, out);
                } else {
                    out += span.text;
                }
                break;

            case InlineType::SoftBreak:
                out.push_back(' ');
                break;

            case InlineType::LineBreak:
                out.push_back('\n');
                break;

            case InlineType::FootnoteRef:
                out.push_back('[');
                out += span.text.empty() ? std::to_string(span.footnote_index) : span.text;
                out.push_back(']');
                break;

            case InlineType::Math: {
                auto m = MathEngine::parse(span.text, false);
                out += m ? MathEngine::to_plain_text(*m) : span.text;
                break;
            }

            case InlineType::HtmlInline:
                break; // Strip raw HTML tags in plaintext mode
        }
    }
}

void render_block_plaintext(const BlockNode& block, std::string& out) {
    auto ensure_block_sep = [&]() {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        if (out.size() >= 2 && out[out.size() - 2] != '\n') out.push_back('\n');
    };

    switch (block.type) {
        case BlockType::Document:
            for (const auto& ch : block.children) {
                if (ch) render_block_plaintext(*ch, out);
            }
            break;

        case BlockType::Heading:
        case BlockType::Paragraph:
            ensure_block_sep();
            render_inlines_plaintext(block.inlines, out);
            out.push_back('\n');
            break;

        case BlockType::CodeBlock:
            ensure_block_sep();
            for (const auto& line : block.code_lines) {
                out += line;
                out.push_back('\n');
            }
            break;

        case BlockType::BlockQuote:
            for (const auto& ch : block.children) {
                if (ch) render_block_plaintext(*ch, out);
            }
            break;

        case BlockType::Alert:
            ensure_block_sep();
            if (!block.alert_title.empty()) {
                out += block.alert_title;
                out.push_back('\n');
            }
            for (const auto& ch : block.children) {
                if (ch) render_block_plaintext(*ch, out);
            }
            break;

        case BlockType::Details:
            ensure_block_sep();
            if (!block.summary_text.empty()) {
                out += block.summary_text;
                out.push_back('\n');
            }
            for (const auto& ch : block.children) {
                if (ch) render_block_plaintext(*ch, out);
            }
            break;

        case BlockType::List: {
            ensure_block_sep();
            int num = block.list_start_number;
            for (const auto& item : block.children) {
                if (!item) continue;
                for (int d = 0; d < item->list_level; ++d) out += "  ";
                if (item->is_task_item) {
                    out += item->is_task_checked ? "[x] " : "[ ] ";
                } else if (block.is_ordered_list) {
                    out += std::to_string(num++) + ". ";
                } else {
                    out += "- ";
                }
                if (!item->children.empty()) {
                    bool first = true;
                    for (const auto& ch : item->children) {
                        if (!ch) continue;
                        if (!first) out.push_back('\n');
                        if (ch->type == BlockType::Paragraph) {
                            render_inlines_plaintext(ch->inlines, out);
                        } else {
                            render_block_plaintext(*ch, out);
                        }
                        first = false;
                    }
                } else {
                    render_inlines_plaintext(item->inlines, out);
                }
                if (out.empty() || out.back() != '\n') out.push_back('\n');
            }
            break;
        }

        case BlockType::Table:
            ensure_block_sep();
            for (const auto& row : block.table_rows) {
                for (size_t c = 0; c < row.cells.size(); ++c) {
                    if (c > 0) out += '\t';
                    render_inlines_plaintext(row.cells[c].inlines, out);
                }
                out.push_back('\n');
            }
            break;

        case BlockType::ThematicBreak:
            ensure_block_sep();
            break;

        case BlockType::FootnoteDef:
            ensure_block_sep();
            out += "[" + std::to_string(block.footnote_index) + "] ";
            render_inlines_plaintext(block.inlines, out);
            out.push_back('\n');
            break;

        case BlockType::MathBlock: {
            ensure_block_sep();
            auto m = MathEngine::parse(block.source, true);
            out += m ? MathEngine::to_plain_text(*m) : block.source;
            out.push_back('\n');
            break;
        }

        case BlockType::Diagram:
            ensure_block_sep();
            out += block.source;
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            break;

        case BlockType::Custom:
            ensure_block_sep();
            render_inlines_plaintext(block.inlines, out);
            for (const auto& ch : block.children) {
                if (ch) render_block_plaintext(*ch, out);
            }
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            break;

        case BlockType::HtmlBlock:
        case BlockType::ListItem:
            break;
    }
}

// ============================================================================
// 4.3 CommonMark / GFM Round-Trip Formatter
// ============================================================================

std::string format_code_span_md(std::string_view code) {
    size_t max_ticks = 0;
    size_t cur = 0;
    for (char c : code) {
        if (c == '`') {
            cur++;
            max_ticks = std::max(max_ticks, cur);
        } else {
            cur = 0;
        }
    }
    size_t ticks = max_ticks + 1;
    std::string fence(ticks, '`');
    bool need_pad = (!code.empty() && (code.front() == '`' || code.back() == '`' ||
                    (code.front() == ' ' && code.back() == ' ')));
    std::string out = fence;
    if (need_pad) out.push_back(' ');
    out.append(code);
    if (need_pad) out.push_back(' ');
    out += fence;
    return out;
}

void render_inlines_markdown(
    const std::vector<InlineSpan>& inlines,
    const MarkdownFormatOptions& opts,
    std::string& out
) {
    std::string em1(1, opts.emphasis_char);
    std::string em2(2, opts.emphasis_char);
    std::string em3(3, opts.emphasis_char);

    for (const auto& span : inlines) {
        switch (span.type) {
            case InlineType::Text:
            case InlineType::Emoji:
                out += span.text;
                break;

            case InlineType::SoftBreak:
                out.push_back('\n');
                break;

            case InlineType::LineBreak:
                out += "  \n";
                break;

            case InlineType::Bold:
                out += em2;
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += em2;
                break;

            case InlineType::Italic:
                out += em1;
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += em1;
                break;

            case InlineType::BoldItalic:
                out += em3;
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += em3;
                break;

            case InlineType::Strikethrough:
                out += "~~";
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += "~~";
                break;

            case InlineType::CodeSpan:
                out += format_code_span_md(span.text);
                break;

            case InlineType::Link: {
                out.push_back('[');
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += "](";
                if (span.target.find(' ') != std::string::npos) {
                    out.push_back('<');
                    out += span.target;
                    out.push_back('>');
                } else {
                    out += span.target;
                }
                if (!span.title.empty()) {
                    out += " \"";
                    for (char c : span.title) {
                        if (c == '"' || c == '\\') out.push_back('\\');
                        out.push_back(c);
                    }
                    out.push_back('"');
                }
                out.push_back(')');
                break;
            }

            case InlineType::Image: {
                out += "![";
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += "](";
                out += span.target;
                if (!span.title.empty()) {
                    out += " \"" + span.title + "\"";
                }
                out.push_back(')');
                break;
            }

            case InlineType::Kbd:
                out += "<kbd>" + span.text + "</kbd>";
                break;

            case InlineType::Subscript:
                out += "<sub>" + span.text + "</sub>";
                break;

            case InlineType::Superscript:
                out += "<sup>" + span.text + "</sup>";
                break;

            case InlineType::Underline:
                out += "<u>";
                if (!span.children.empty()) render_inlines_markdown(span.children, opts, out);
                else out += span.text;
                out += "</u>";
                break;

            case InlineType::Highlight:
                out += "<mark>" + span.text + "</mark>";
                break;

            case InlineType::FootnoteRef:
                out += "[^" + (span.text.empty() ? std::to_string(span.footnote_index) : span.text) + "]";
                break;

            case InlineType::Math:
                out += "$" + span.text + "$";
                break;

            case InlineType::HtmlInline:
                out += span.text;
                break;

            case InlineType::Custom:
                out += span.text;
                break;
        }
    }
}

void render_block_markdown(
    const BlockNode& block,
    const MarkdownFormatOptions& opts,
    const std::string& prefix,
    std::string& out
) {
    switch (block.type) {
        case BlockType::Document: {
            for (size_t idx = 0; idx < block.children.size(); ++idx) {
                if (!block.children[idx]) continue;
                if (idx > 0) out += prefix + "\n";
                render_block_markdown(*block.children[idx], opts, prefix, out);
            }
            break;
        }

        case BlockType::Heading: {
            int lvl = std::clamp(block.heading_level, 1, 6);
            std::string content;
            render_inlines_markdown(block.inlines, opts, content);
            if (!opts.prefer_atx_headings && block.is_setext_heading && lvl <= 2) {
                out += prefix + content + "\n";
                out += prefix + std::string(std::max<size_t>(3, content.size()), lvl == 1 ? '=' : '-') + "\n";
            } else {
                out += prefix + std::string(static_cast<size_t>(lvl), '#') + " " + content + "\n";
            }
            break;
        }

        case BlockType::Paragraph: {
            std::string body;
            render_inlines_markdown(block.inlines, opts, body);
            std::istringstream iss(body);
            std::string line;
            while (std::getline(iss, line)) {
                out += prefix + line + "\n";
            }
            break;
        }

        case BlockType::CodeBlock: {
            size_t max_ticks = 2;
            for (const auto& l : block.code_lines) {
                size_t run = 0;
                for (char c : l) {
                    if (c == '`') { run++; max_ticks = std::max(max_ticks, run); }
                    else run = 0;
                }
            }
            std::string fence(max_ticks + 1, '`');
            out += prefix + fence + block.language + "\n";
            for (const auto& l : block.code_lines) {
                out += prefix + l + "\n";
            }
            out += prefix + fence + "\n";
            break;
        }

        case BlockType::BlockQuote: {
            std::string qprefix = prefix + "> ";
            for (size_t idx = 0; idx < block.children.size(); ++idx) {
                if (!block.children[idx]) continue;
                if (idx > 0) out += prefix + ">\n";
                render_block_markdown(*block.children[idx], opts, qprefix, out);
            }
            break;
        }

        case BlockType::Alert: {
            const char* tag = "NOTE";
            if (block.alert_type == AlertType::Tip) tag = "TIP";
            else if (block.alert_type == AlertType::Important) tag = "IMPORTANT";
            else if (block.alert_type == AlertType::Warning) tag = "WARNING";
            else if (block.alert_type == AlertType::Caution) tag = "CAUTION";
            out += prefix + "> [!" + tag + "]\n";
            std::string qprefix = prefix + "> ";
            for (size_t idx = 0; idx < block.children.size(); ++idx) {
                if (!block.children[idx]) continue;
                if (idx > 0) out += prefix + ">\n";
                render_block_markdown(*block.children[idx], opts, qprefix, out);
            }
            break;
        }

        case BlockType::List: {
            int num = block.list_start_number;
            bool loose = block.is_loose_list || !block.is_tight_list;
            for (size_t idx = 0; idx < block.children.size(); ++idx) {
                const auto& item = block.children[idx];
                if (!item) continue;
                if (idx > 0 && loose) out += prefix + "\n";

                std::string indent(static_cast<size_t>(item->list_level) * 2, ' ');
                std::string marker = block.is_ordered_list
                    ? (std::to_string(num++) + ". ")
                    : (std::string(1, opts.bullet_char) + " ");
                if (item->is_task_item) {
                    marker += item->is_task_checked ? "[x] " : "[ ] ";
                }

                if (!item->children.empty()) {
                    bool first = true;
                    std::string cont_prefix = prefix + indent + std::string(marker.size(), ' ');
                    for (const auto& ch : item->children) {
                        if (!ch) continue;
                        if (first && ch->type == BlockType::Paragraph) {
                            std::string body;
                            render_inlines_markdown(ch->inlines, opts, body);
                            out += prefix + indent + marker + body + "\n";
                            first = false;
                        } else {
                            if (!first) out += prefix + "\n";
                            render_block_markdown(*ch, opts, cont_prefix, out);
                            first = false;
                        }
                    }
                } else {
                    std::string body;
                    render_inlines_markdown(item->inlines, opts, body);
                    out += prefix + indent + marker + body + "\n";
                }
            }
            break;
        }

        case BlockType::Table: {
            size_t num_cols = block.table_alignments.size();
            if (num_cols == 0 && !block.table_rows.empty()) num_cols = block.table_rows[0].cells.size();
            for (size_t r = 0; r < block.table_rows.size(); ++r) {
                const auto& row = block.table_rows[r];
                out += prefix + "|";
                for (size_t c = 0; c < num_cols; ++c) {
                    out.push_back(' ');
                    if (c < row.cells.size()) {
                        render_inlines_markdown(row.cells[c].inlines, opts, out);
                    }
                    out += " |";
                }
                out.push_back('\n');
                if (r == 0) {
                    out += prefix + "|";
                    for (size_t c = 0; c < num_cols; ++c) {
                        TableAlign al = c < block.table_alignments.size() ? block.table_alignments[c] : TableAlign::None;
                        if (al == TableAlign::Left) out += " :--- |";
                        else if (al == TableAlign::Center) out += " :---: |";
                        else if (al == TableAlign::Right) out += " ---: |";
                        else out += " --- |";
                    }
                    out.push_back('\n');
                }
            }
            break;
        }

        case BlockType::ThematicBreak:
            out += prefix + "---\n";
            break;

        case BlockType::Details:
            out += prefix + (block.is_open ? "<details open>\n" : "<details>\n");
            out += prefix + "<summary>" + block.summary_text + "</summary>\n";
            for (const auto& ch : block.children) {
                if (ch) render_block_markdown(*ch, opts, prefix, out);
            }
            out += prefix + "</details>\n";
            break;

        case BlockType::HtmlBlock:
            out += prefix + block.raw_html;
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            break;

        case BlockType::FootnoteDef: {
            std::string body;
            render_inlines_markdown(block.inlines, opts, body);
            out += prefix + "[^" + block.footnote_label + "]: " + body + "\n";
            break;
        }

        case BlockType::MathBlock:
            out += prefix + "$$\n" + block.source;
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            out += prefix + "$$\n";
            break;

        case BlockType::Diagram:
            out += prefix + "```mermaid\n" + block.source;
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            out += prefix + "```\n";
            break;

        case BlockType::Custom: {
            std::string body;
            render_inlines_markdown(block.inlines, opts, body);
            if (!body.empty()) out += prefix + body + "\n";
            for (const auto& ch : block.children) {
                if (ch) render_block_markdown(*ch, opts, prefix, out);
            }
            break;
        }

        case BlockType::ListItem:
            break;
    }
}

} // namespace

std::string MarkdownExporter::to_html(const BlockNode& root, const MarkdownHtmlExportOptions& options) {
    std::string out;
    out.reserve(512);
    render_block_html(root, options, out);
    return out;
}

std::string MarkdownExporter::to_html(const MarkdownDocument& doc, const MarkdownHtmlExportOptions& options) {
    if (!doc.is_valid()) return {};
    return to_html(*doc.root(), options);
}

std::string MarkdownExporter::to_plaintext(const BlockNode& root) {
    std::string out;
    out.reserve(512);
    render_block_plaintext(root, out);
    // Trim trailing excess blank lines to single trailing newline
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
        out.pop_back();
    }
    return out;
}

std::string MarkdownExporter::to_plaintext(const MarkdownDocument& doc) {
    if (!doc.is_valid()) return {};
    return to_plaintext(*doc.root());
}

std::string MarkdownExporter::to_markdown(const BlockNode& root, const MarkdownFormatOptions& options) {
    std::string out;
    out.reserve(512);
    render_block_markdown(root, options, "", out);
    return out;
}

std::string MarkdownExporter::to_markdown(const MarkdownDocument& doc, const MarkdownFormatOptions& options) {
    if (!doc.is_valid()) return {};
    return to_markdown(*doc.root(), options);
}

} // namespace nisaba::markdown

