#include <cassert>
#include <iostream>
#include <string>
#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::markdown;

void test_markdown_parser_blocks() {
    std::cout << "[Test] MarkdownParser: Block elements..." << std::endl;

    std::string md = 
        "# Heading 1\n"
        "## Heading 2\n"
        "### Heading 3\n"
        "#### Heading 4\n"
        "##### Heading 5\n"
        "###### Heading 6\n"
        "\n"
        "---\n"
        "\n"
        "This is a standard paragraph of text.\n"
        "It spans across two lines.\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->type == BlockType::Document);
    assert(doc->children.size() == 8); // 6 headings + 1 divider + 1 paragraph

    // Verify headings
    for (int i = 0; i < 6; ++i) {
        assert(doc->children[i]->type == BlockType::Heading);
        assert(doc->children[i]->heading_level == (i + 1));
        assert(!doc->children[i]->inlines.empty());
    }

    // Verify thematic break
    assert(doc->children[6]->type == BlockType::ThematicBreak);

    // Verify paragraph
    assert(doc->children[7]->type == BlockType::Paragraph);
    assert(!doc->children[7]->inlines.empty());

    std::cout << "  Block elements verified." << std::endl;
}

void test_markdown_inlines() {
    std::cout << "[Test] MarkdownParser: Inlines & formatting..." << std::endl;

    std::string text = "Hello **bold** and *italic* and ***bold italic*** and `code` and ~~strike~~ and [link](https://example.com) and ![img](pic.png)";
    auto spans = MarkdownParser::parse_inlines(text);

    assert(!spans.empty());
    bool found_bold = false;
    bool found_italic = false;
    bool found_bold_italic = false;
    bool found_code = false;
    bool found_strike = false;
    bool found_link = false;
    bool found_image = false;

    for (const auto& s : spans) {
        if (s.type == InlineType::Bold && s.text == "bold") found_bold = true;
        if (s.type == InlineType::Italic && s.text == "italic") found_italic = true;
        if (s.type == InlineType::BoldItalic && s.text == "bold italic") found_bold_italic = true;
        if (s.type == InlineType::CodeSpan && s.text == "code") found_code = true;
        if (s.type == InlineType::Strikethrough && s.text == "strike") found_strike = true;
        if (s.type == InlineType::Link && s.text == "link" && s.target == "https://example.com") found_link = true;
        if (s.type == InlineType::Image && s.text == "img" && s.target == "pic.png") found_image = true;
    }

    assert(found_bold);
    assert(found_italic);
    assert(found_bold_italic);
    assert(found_code);
    assert(found_strike);
    assert(found_link);
    assert(found_image);

    std::cout << "  Inline formatting verified." << std::endl;
}

void test_markdown_code_blocks() {
    std::cout << "[Test] MarkdownParser: Fenced code blocks..." << std::endl;

    std::string md = 
        "```cpp\n"
        "#include <iostream>\n"
        "int main() {\n"
        "    return 0;\n"
        "}\n"
        "```\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->children.size() == 1);
    const auto& cb = *doc->children[0];
    assert(cb.type == BlockType::CodeBlock);
    assert(cb.language == "cpp");
    assert(cb.code_lines.size() == 4);
    assert(cb.code_lines[0] == "#include <iostream>");
    assert(cb.code_lines[3] == "}");

    std::cout << "  Fenced code block verified." << std::endl;
}

void test_markdown_lists() {
    std::cout << "[Test] MarkdownParser: Lists and GFM task lists..." << std::endl;

    std::string md = 
        "- Item 1\n"
        "- Item 2\n"
        "- [ ] Unchecked task\n"
        "- [x] Checked task\n"
        "\n"
        "1. First\n"
        "2. Second\n"
        "3. Third\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->children.size() == 2); // 1 unordered list + 1 ordered list

    // Unordered list
    const auto& ul = *doc->children[0];
    assert(ul.type == BlockType::List);
    assert(!ul.is_ordered_list);
    assert(ul.children.size() == 4);
    assert(!ul.children[0]->is_task_item);
    assert(ul.children[2]->is_task_item && !ul.children[2]->is_task_checked);
    assert(ul.children[3]->is_task_item && ul.children[3]->is_task_checked);

    // Ordered list
    const auto& ol = *doc->children[1];
    assert(ol.type == BlockType::List);
    assert(ol.is_ordered_list);
    assert(ol.children.size() == 3);
    assert(ol.list_start_number == 1);

    std::cout << "  Lists and task lists verified." << std::endl;
}

void test_markdown_tables() {
    std::cout << "[Test] MarkdownParser: GFM Tables..." << std::endl;

    std::string md = 
        "| Name | Role | Score |\n"
        "| :--- | :--: | ----: |\n"
        "| Alice | Admin | 95 |\n"
        "| Bob | User | 82 |\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->children.size() == 1);

    const auto& tbl = *doc->children[0];
    assert(tbl.type == BlockType::Table);
    assert(tbl.table_alignments.size() == 3);
    assert(tbl.table_alignments[0] == TableAlign::Left);
    assert(tbl.table_alignments[1] == TableAlign::Center);
    assert(tbl.table_alignments[2] == TableAlign::Right);

    assert(tbl.table_rows.size() == 3); // 1 header + 2 data rows
    assert(tbl.table_rows[0].is_header);
    assert(!tbl.table_rows[1].is_header);
    assert(tbl.table_rows[1].cells.size() == 3);

    std::cout << "  GFM Tables verified." << std::endl;
}

void test_markdown_blockquotes() {
    std::cout << "[Test] MarkdownParser: BlockQuotes..." << std::endl;

    std::string md = 
        "> This is a blockquote.\n"
        "> Continued on second line.\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->children.size() == 1);

    const auto& bq = *doc->children[0];
    assert(bq.type == BlockType::BlockQuote);
    assert(!bq.children.empty());

    std::cout << "  BlockQuotes verified." << std::endl;
}

void test_markdown_document_and_render() {
    std::cout << "[Test] MarkdownDocument and MarkdownRenderer..." << std::endl;

    std::string md = 
        "# Welcome to Nisaba Markdown\n"
        "\n"
        "Nisaba provides **ultra-fast**, sovereign 2D document rendering.\n"
        "\n"
        "```cpp\n"
        "// Embedded C++20 code block\n"
        "auto pixmap = Pixmap::create(800, 600);\n"
        "```\n"
        "\n"
        "- [x] High performance software rasterizer\n"
        "- [x] Zero external dependencies\n"
        "- [ ] Native PDF export support\n"
        "\n"
        "| Feature | Speed | Status |\n"
        "| :--- | :---: | ---: |\n"
        "| Vector Core | 7.5x Cairo | Active |\n"
        "| Markdown | O(1) SIMD | Active |\n";

    auto doc = MarkdownDocument::from_string(md);
    assert(doc.is_valid());

    auto style = MarkdownStyle::dark_theme();
    text::FontSystem font_system;
    text::GlyphCache glyph_cache;

    float height = doc.layout(700.0f, style, font_system, glyph_cache);
    assert(height > 100.0f);
    assert(doc.total_height() == height);

    // Allocate pixmap and render document
    auto pixmap = Pixmap::create(800, static_cast<uint32_t>(height + 40.0f));
    assert(pixmap.has_value());
    Canvas canvas(*pixmap);
    canvas.clear(Color::from_rgba8(10, 14, 24, 255));

    MarkdownRenderer::render(canvas, doc, 50.0f, 20.0f, 700.0f, style, font_system, glyph_cache);

    std::cout << "  Document layout and render verified. Computed height: " << height << "px." << std::endl;
}

void test_markdown_html_tags() {
    std::cout << "[Test] Markdown: HTML Tags (Inline & Block)..." << std::endl;

    std::string inline_html = "Press <kbd>Ctrl</kbd> + <kbd>C</kbd> or use H<sub>2</sub>O and E=mc<sup>2</sup> with <mark>highlight</mark> and <u>underline</u> and <br> newline.";
    auto spans = MarkdownParser::parse_inlines(inline_html);

    bool found_kbd = false;
    bool found_sub = false;
    bool found_sup = false;
    bool found_mark = false;
    bool found_u = false;
    bool found_br = false;

    for (const auto& s : spans) {
        if (s.type == InlineType::Kbd && s.text == "Ctrl") found_kbd = true;
        if (s.type == InlineType::Subscript && s.text == "2") found_sub = true;
        if (s.type == InlineType::Superscript && s.text == "2") found_sup = true;
        if (s.type == InlineType::Highlight && s.text == "highlight") found_mark = true;
        if (s.type == InlineType::Underline && s.text == "underline") found_u = true;
        if (s.type == InlineType::LineBreak) found_br = true;
    }

    assert(found_kbd);
    assert(found_sub);
    assert(found_sup);
    assert(found_mark);
    assert(found_u);
    assert(found_br);

    // Block HTML: <details> and <summary>
    std::string block_html = 
        "<details open>\n"
        "<summary>Advanced Configuration</summary>\n"
        "Here is the collapsed or expanded content.\n"
        "</details>\n";

    auto doc = MarkdownParser::parse(block_html);
    assert(doc != nullptr);
    assert(doc->children.size() == 1);
    const auto& details = *doc->children[0];
    assert(details.type == BlockType::Details);
    assert(details.is_open);
    assert(details.summary_text == "Advanced Configuration");
    assert(!details.children.empty());

    std::cout << "  HTML tags (Inline <kbd>, <sub>, <sup>, <mark>, <br> & Block <details>) verified." << std::endl;
}

void test_markdown_github_alerts() {
    std::cout << "[Test] Markdown: GitHub Callout Alerts..." << std::endl;

    std::string md = 
        "> [!NOTE]\n"
        "> Useful information that users should know.\n"
        "\n"
        "> [!TIP]\n"
        "> Helpful advice for doing things better.\n"
        "\n"
        "> [!IMPORTANT]\n"
        "> Key information users need to know.\n"
        "\n"
        "> [!WARNING]\n"
        "> Urgent info that needs immediate attention.\n"
        "\n"
        "> [!CAUTION]\n"
        "> Advises about risks or negative outcomes.\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->children.size() == 5);

    assert(doc->children[0]->type == BlockType::Alert && doc->children[0]->alert_type == AlertType::Note);
    assert(doc->children[1]->type == BlockType::Alert && doc->children[1]->alert_type == AlertType::Tip);
    assert(doc->children[2]->type == BlockType::Alert && doc->children[2]->alert_type == AlertType::Important);
    assert(doc->children[3]->type == BlockType::Alert && doc->children[3]->alert_type == AlertType::Warning);
    assert(doc->children[4]->type == BlockType::Alert && doc->children[4]->alert_type == AlertType::Caution);

    std::cout << "  All 5 GitHub Callout Alerts verified ([!NOTE], [!TIP], [!IMPORTANT], [!WARNING], [!CAUTION])." << std::endl;
}

void test_markdown_syntax_highlighter() {
    std::cout << "[Test] Markdown: Syntax Highlighting Lexer..." << std::endl;

    bool in_comment = false;
    auto cpp_tokens = SyntaxHighlighter::tokenize_line("constexpr uint32_t count = 42; // Sovereign", "cpp", in_comment);
    assert(!cpp_tokens.empty());

    bool found_keyword = false;
    bool found_type = false;
    bool found_number = false;
    bool found_comment = false;

    for (const auto& tok : cpp_tokens) {
        if (tok.type == TokenType::Keyword && tok.text == "constexpr") found_keyword = true;
        if (tok.type == TokenType::Type && tok.text == "uint32_t") found_type = true;
        if (tok.type == TokenType::NumberLiteral && tok.text == "42") found_number = true;
        if (tok.type == TokenType::Comment) found_comment = true;
    }

    assert(found_keyword);
    assert(found_type);
    assert(found_number);
    assert(found_comment);

    // Python test
    in_comment = false;
    auto py_tokens = SyntaxHighlighter::tokenize_line("def render_frame(time_sec): return True", "python", in_comment);
    bool found_py_def = false;
    bool found_py_return = false;
    for (const auto& tok : py_tokens) {
        if (tok.type == TokenType::Keyword && tok.text == "def") found_py_def = true;
        if (tok.type == TokenType::Keyword && tok.text == "return") found_py_return = true;
    }
    assert(found_py_def && found_py_return);

    // Rust test
    in_comment = false;
    auto rust_tokens = SyntaxHighlighter::tokenize_line("pub fn create_buffer(mut len: usize) -> Result", "rust", in_comment);
    bool found_rust_fn = false;
    bool found_rust_pub = false;
    for (const auto& tok : rust_tokens) {
        if (tok.type == TokenType::Keyword && tok.text == "pub") found_rust_pub = true;
        if (tok.type == TokenType::Keyword && tok.text == "fn") found_rust_fn = true;
    }
    assert(found_rust_pub && found_rust_fn);

    // CMake test
    in_comment = false;
    auto cmake_tokens = SyntaxHighlighter::tokenize_line("project(Nisaba VERSION 2.0)", "cmake", in_comment);
    bool found_cmake_proj = false;
    for (const auto& tok : cmake_tokens) {
        if (tok.type == TokenType::Keyword && tok.text == "project") found_cmake_proj = true;
    }
    assert(found_cmake_proj);

    // HTML test
    in_comment = false;
    auto html_tokens = SyntaxHighlighter::tokenize_line("<div class=\"container\"><!-- test --></div>", "html", in_comment);
    bool found_html_tag = false;
    bool found_html_attr = false;
    bool found_html_cmt = false;
    for (const auto& tok : html_tokens) {
        if (tok.type == TokenType::Keyword && tok.text == "div") found_html_tag = true;
        if (tok.type == TokenType::Type && tok.text == "class") found_html_attr = true;
        if (tok.type == TokenType::Comment) found_html_cmt = true;
    }
    assert(found_html_tag && found_html_attr && found_html_cmt);

    // Theme color verification
    auto dark_st = MarkdownStyle::dark_theme();
    auto light_st = MarkdownStyle::light_theme();
    assert(SyntaxHighlighter::token_color(TokenType::Keyword, dark_st) == dark_st.syn_keyword);
    assert(SyntaxHighlighter::token_color(TokenType::Keyword, light_st) == light_st.syn_keyword);
    assert(SyntaxHighlighter::token_color(TokenType::Comment, dark_st) == dark_st.syn_comment);

    std::cout << "  Syntax Highlighting Lexer verified for C++, Python, Rust, CMake, HTML and themes." << std::endl;
}

void test_markdown_link_reference_definitions() {
    std::cout << "[Test] Markdown: Link Reference Definitions..." << std::endl;

    std::string md = 
        "Read the [documentation][docs] or visit [GitHub] directly.\n"
        "\n"
        "[docs]: https://nisaba.dev/docs \"Nisaba Docs\"\n"
        "[github]: https://github.com/vaxp/nisaba\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    // Link reference lines should be consumed into ref_map and not emitted as blocks
    assert(doc->children.size() == 1);
    assert(doc->children[0]->type == BlockType::Paragraph);

    const auto& inlines = doc->children[0]->inlines;
    bool found_docs_link = false;
    bool found_gh_link = false;

    for (const auto& s : inlines) {
        if (s.type == InlineType::Link && s.text == "documentation" && s.target == "https://nisaba.dev/docs") {
            found_docs_link = true;
        }
        if (s.type == InlineType::Link && s.text == "GitHub" && s.target == "https://github.com/vaxp/nisaba") {
            found_gh_link = true;
        }
    }

    assert(found_docs_link);
    assert(found_gh_link);

    std::cout << "  Link Reference Definitions verified." << std::endl;
}

void test_markdown_emojis_and_autolinks() {
    std::cout << "[Test] Markdown: Emojis and Autolinks..." << std::endl;

    std::string text = "Launch :rocket: and celebrate :tada: with :sparkles: Visit https://github.com/vaxp/nisaba for code!";
    auto spans = MarkdownParser::parse_inlines(text);

    bool found_rocket = false;
    bool found_tada = false;
    bool found_autolink = false;

    for (const auto& s : spans) {
        if (s.text.find("🚀") != std::string::npos) found_rocket = true;
        if (s.text.find("🎉") != std::string::npos) found_tada = true;
        if (s.type == InlineType::Link && s.target == "https://github.com/vaxp/nisaba") found_autolink = true;
    }

    assert(found_rocket);
    assert(found_tada);
    assert(found_autolink);

    std::cout << "  Emoji replacement and autolinks verified." << std::endl;
}

void test_markdown_footnotes_and_slugs() {
    std::cout << "[Test] Markdown: Footnotes and Anchor Slugs..." << std::endl;

    // Slugs
    assert(MarkdownParser::slugify("Nisaba 2D Engine: Fast & Sovereign!") == "nisaba-2d-engine-fast-sovereign");
    assert(MarkdownParser::slugify("Architecture & Highlights") == "architecture-highlights");

    // Footnotes
    std::string md = 
        "This is statement with footnote[^1].\n"
        "\n"
        "[^1]: The official Nisaba documentation citation.\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc != nullptr);
    assert(doc->children.size() == 2);

    // Verify FootnoteRef in paragraph
    const auto& para = *doc->children[0];
    bool found_fn_ref = false;
    for (const auto& s : para.inlines) {
        if (s.type == InlineType::FootnoteRef && s.footnote_index == 1) found_fn_ref = true;
    }
    assert(found_fn_ref);

    // Verify FootnoteDef block
    const auto& fn_def = *doc->children[1];
    assert(fn_def.type == BlockType::FootnoteDef);
    assert(fn_def.footnote_index == 1);
    assert(!fn_def.inlines.empty());

    std::cout << "  Footnotes and Anchor Slugs verified." << std::endl;
}

void test_markdown_interactive_hit_test() {
    std::cout << "[Test] Markdown: Interactive Hit-Testing and TOC..." << std::endl;

    std::string md = 
        "# Architecture Overview\n"
        "\n"
        "Check [Official Website](https://vaxp.org) for details.\n"
        "\n"
        "<details open>\n"
        "<summary>Show Engine Telemetry</summary>\n"
        "FPS: 144\n"
        "</details>\n"
        "\n"
        "- [ ] Review pull request\n"
        "- [x] Merge changes\n";

    auto doc = MarkdownDocument::from_string(md);
    assert(doc.is_valid());

    auto style = MarkdownStyle::dark_theme();
    text::FontSystem font_system;
    text::GlyphCache glyph_cache;
    doc.layout(800.0f, style, font_system, glyph_cache);

    // 1. Table of contents extraction
    auto toc = doc.table_of_contents();
    assert(toc.size() == 1);
    assert(toc[0].first == "Architecture Overview");
    assert(toc[0].second == "architecture-overview");

    // 2. Details toggle interaction
    const auto& details_node = doc.root()->children[2];
    assert(details_node->type == BlockType::Details);
    assert(details_node->is_open);

    bool toggled = doc.toggle_details(details_node.get());
    assert(toggled);
    assert(!details_node->is_open);

    // 3. Task list interaction
    const auto& list_node = doc.root()->children[3];
    assert(list_node->type == BlockType::List);
    auto& item0 = list_node->children[0];
    assert(item0->is_task_item && !item0->is_task_checked);

    bool item_toggled = doc.toggle_task_item(item0.get());
    assert(item_toggled);
    assert(item0->is_task_checked);

    // 4. Test handle_click and get_cursor_at
    std::string clicked_url;
    const auto& link_para = doc.root()->children[1];
    doc.handle_click(10.0f, link_para->layout_y + 5.0f, [&](const std::string& url) {
        clicked_url = url;
    });
    assert(clicked_url == "https://vaxp.org");

    // Click on details summary to toggle it back open
    bool details_reopened = doc.handle_click(10.0f, details_node->layout_y + 5.0f);
    assert(details_reopened);
    assert(details_node->is_open);

    // Cursor shape verification
    auto link_cursor = doc.get_cursor_at(10.0f, link_para->layout_y + 5.0f);
    assert(link_cursor == MarkdownDocument::CursorType::Pointer);

    std::cout << "  Interactive hit testing, TOC, and state toggles verified." << std::endl;
}

void test_markdown_advanced_layout_and_recursion() {
    std::cout << "[Test] Markdown: Recursive Inlines, Loose Lists & Advanced Tables..." << std::endl;

    // 1. Recursive inlines
    std::string inline_md = "Check out [the **sovereign** *Nisaba* graphics](https://nisaba.dev)";
    auto spans = MarkdownParser::parse_inlines(inline_md);
    assert(spans.size() == 2); // "Check out " + Link
    assert(spans[1].type == InlineType::Link);
    assert(!spans[1].children.empty());
    bool found_nested_bold = false;
    for (const auto& ch : spans[1].children) {
        if (ch.type == InlineType::Bold && ch.text == "sovereign") found_nested_bold = true;
    }
    assert(found_nested_bold);

    // 2. Loose lists
    std::string loose_md = 
        "- Item Alpha\n"
        "\n"
        "- Item Beta\n";
    auto doc_loose = MarkdownParser::parse(loose_md);
    assert(doc_loose != nullptr && doc_loose->children.size() == 1);
    const auto& list_node = doc_loose->children[0];
    assert(list_node->type == BlockType::List);
    assert(list_node->is_loose_list);
    assert(list_node->children.size() == 2);

    // 3. Multi-line table cell layout
    std::string table_md = 
        "| Feature | Description |\n"
        "| :--- | :--- |\n"
        "| Renderer | This is a very extensive multi-line description intended to wrap across multiple horizontal lines in the table layout |\n";

    auto doc_table = MarkdownDocument::from_string(table_md);
    assert(doc_table.is_valid());
    auto style = MarkdownStyle::dark_theme();
    text::FontSystem font_system;
    text::GlyphCache glyph_cache;

    doc_table.layout(350.0f, style, font_system, glyph_cache);
    const auto& tbl_block = doc_table.root()->children[0];
    assert(tbl_block->type == BlockType::Table);
    assert(tbl_block->table_rows.size() == 2);

    float default_min_h = style.base_font_size * style.line_height_multiplier + 2.0f * style.table_cell_padding_v;
    // Row 1 (data row) should have wrapped into multiple lines and thus be taller than a single line
    assert(tbl_block->table_rows[1].layout_height > default_min_h);

    std::cout << "  Recursive inlines, loose lists & advanced table wrapping verified." << std::endl;
}

void test_markdown_vector_pdf_exporter() {
    std::cout << "[Test] Markdown: True Vector PDF Export Pipeline..." << std::endl;

    std::string md = 
        "# Nisaba Sovereign Graphics Engine\n"
        "## Vector PDF Document Generation\n"
        "\n"
        "This document is dynamically generated by Nisaba's native PDF vector pipeline.\n"
        "\n"
        "> [!NOTE]\n"
        "> PDF export uses zero third-party dependencies, standard Type 1 fonts, and vector primitives.\n"
        "\n"
        "> [!TIP]\n"
        "> Tables and callout alerts format automatically across page breaks.\n"
        "\n"
        "```cpp\n"
        "// Embedded C++ snippet\n"
        "auto doc = MarkdownDocument::from_string(md);\n"
        "MarkdownPdfExporter::export_to_file(doc, \"nisaba_test_export.pdf\", style, fonts, cache);\n"
        "```\n"
        "\n"
        "- [x] High performance software rasterizer\n"
        "- [x] GitHub callout alerts\n"
        "- [ ] Multi-threaded GPU pipeline\n"
        "\n"
        "| Feature | Speed | Status |\n"
        "| :--- | :---: | ---: |\n"
        "| Vector Core | 7.5x Cairo | Verified |\n"
        "| PDF Exporter | ISO 32000-1 | Verified |\n";

    auto doc = MarkdownDocument::from_string(md);
    assert(doc.is_valid());

    auto style = MarkdownStyle::light_theme();
    text::FontSystem font_system;
    text::GlyphCache glyph_cache;

    MarkdownPdfExportOptions opts;
    opts.show_header = true;
    opts.show_footer = true;
    opts.show_page_numbers = true;
    opts.header_left = "Nisaba Sovereign Engine";
    opts.doc_title = "Nisaba Vector PDF Test";

    auto pdf_bytes = MarkdownPdfExporter::export_to_bytes(doc, style, font_system, glyph_cache, opts);
    assert(!pdf_bytes.empty());
    assert(pdf_bytes.size() > 500);

    // Verify PDF header magic
    std::string_view pdf_view(reinterpret_cast<const char*>(pdf_bytes.data()), pdf_bytes.size());
    assert(pdf_view.starts_with("%PDF-"));

    // Verify PDF EOF marker
    assert(pdf_view.find("%%EOF") != std::string_view::npos);

    // Verify write to file
    bool saved = MarkdownPdfExporter::export_to_file(doc, "nisaba_test_export.pdf", style, font_system, glyph_cache, opts);
    assert(saved);

    std::cout << "  Vector PDF Export Pipeline verified. Generated " << pdf_bytes.size() << " bytes." << std::endl;
}

void test_markdown_math_engine() {
    std::cout << "[Test] Markdown: LaTeX Math Engine..." << std::endl;

    // Parser: block and inline detection
    auto doc = MarkdownParser::parse(
        "$$\n\\frac{a}{b} + \\sqrt{x^2}\n$$\n"
        "\n"
        "$$ \\sum_{i=1}^{n} i $$\n"
        "\n"
        "```math\n\\begin{pmatrix} 1 & 2 \\\\ 3 & 4 \\end{pmatrix}\n```\n"
        "\n"
        "Euler: $e^{i\\pi} + 1 = 0$ costs $5 and $6 today.\n");
    assert(doc->children.size() == 4);
    assert(doc->children[0]->type == BlockType::MathBlock);
    assert(doc->children[1]->type == BlockType::MathBlock);
    assert(doc->children[2]->type == BlockType::MathBlock);
    assert(doc->children[3]->type == BlockType::Paragraph);

    int math_spans = 0;
    for (const auto& s : doc->children[3]->inlines) {
        if (s.type == InlineType::Math) math_spans++;
    }
    assert(math_spans == 1); // currency "$5 and $6" must not be treated as math

    // Engine: parse, layout, plain-text extraction
    text::FontSystem fonts;
    text::GlyphCache cache;
    auto frac = MathEngine::parse("\\frac{a}{b}", true);
    MathEngine::layout(*frac, 20.0f, true, fonts);
    assert(frac->width > 0.0f && frac->height() > 20.0f);
    assert(MathEngine::to_plain_text(*frac) == "(a)/(b)");

    auto mat = MathEngine::parse("\\begin{bmatrix} 1 & 2 \\\\ 3 & 4 \\end{bmatrix}", true);
    MathEngine::layout(*mat, 20.0f, true, fonts);
    assert(mat->height() > 40.0f);
    assert(MathEngine::to_plain_text(*mat) == "[1, 2; 3, 4]");

    auto sum = MathEngine::parse("\\sum_{i=1}^{n} i", true);
    assert(MathEngine::to_plain_text(*sum).find("∑") != std::string::npos);

    // Render onto CPU canvas
    auto pixmap = Pixmap::create(300, 120);
    assert(pixmap.has_value());
    Canvas canvas(*pixmap);
    canvas.clear(Color::from_rgba8(255, 255, 255, 255));
    MathEngine::render(canvas, *frac, 20.0f, 20.0f, Color::from_rgba8(0, 0, 0, 255), fonts, cache);

    std::cout << "  Math parser, layout, plain-text and render verified." << std::endl;
}

void test_markdown_diagram_engine() {
    std::cout << "[Test] Markdown: Mermaid Diagram Engine..." << std::endl;

    auto doc = MarkdownParser::parse(
        "```mermaid\nflowchart LR\n  A[Start] --> B{Ok?}\n  B -->|yes| C((Done))\n  B -->|no| A\n```\n");
    assert(doc->children.size() == 1);
    assert(doc->children[0]->type == BlockType::Diagram);

    auto flow = DiagramEngine::parse(doc->children[0]->source);
    assert(flow->kind == DiagramKind::Flowchart);
    assert(flow->direction == DiagramDirection::LeftRight);
    assert(flow->nodes.size() == 3);
    assert(flow->edges.size() == 3);
    assert(flow->edges[1].label == "yes");

    text::FontSystem fonts;
    text::GlyphCache cache;
    auto style = MarkdownStyle::dark_theme();
    float h = DiagramEngine::layout(*flow, 600.0f, style, fonts);
    assert(h > 40.0f);
    assert(flow->nodes[1].x > flow->nodes[0].x); // LR: B placed right of A

    auto seq = DiagramEngine::parse(
        "sequenceDiagram\n  participant A as Alice\n  A->>B: Hello\n  B-->>A: Hi\n");
    assert(seq->kind == DiagramKind::Sequence);
    assert(seq->actors.size() == 2);
    assert(seq->messages.size() == 2);

    auto chart = DiagramEngine::parse(
        "xychart-beta\n  title \"Sales\"\n  x-axis [jan, feb, mar]\n  bar [10, 40, 25]\n  line [12, 30, 28]\n");
    assert(chart->kind == DiagramKind::XYChart);
    assert(chart->x_categories.size() == 3);
    assert(chart->series.size() == 2);

    auto pie = DiagramEngine::parse("pie title Pets\n  \"Dogs\" : 40\n  \"Cats\" : 60\n");
    assert(pie->kind == DiagramKind::PieChart);
    assert(pie->pie_slices.size() == 2);

    // Render every diagram family onto a CPU canvas
    for (auto* model : {flow.get(), seq.get(), chart.get(), pie.get()}) {
        float mh = DiagramEngine::layout(*model, 600.0f, style, fonts);
        auto pixmap = Pixmap::create(640, static_cast<uint32_t>(mh + 20.0f));
        assert(pixmap.has_value());
        Canvas canvas(*pixmap);
        canvas.clear(Color::from_rgba8(10, 14, 24, 255));
        DiagramEngine::render(canvas, *model, 10.0f, 10.0f, 600.0f, style, fonts, cache);
    }

    std::cout << "  Flowchart, sequence, xychart and pie parsing, layout and render verified." << std::endl;
}

void test_markdown_math_diagram_document_and_pdf() {
    std::cout << "[Test] Markdown: Math & Diagram in Document + PDF..." << std::endl;

    auto doc = MarkdownDocument::from_string(
        "# Doc\n\n$$ x = \\frac{-b \\pm \\sqrt{b^2-4ac}}{2a} $$\n\n"
        "```mermaid\ngraph TD\n  A-->B\n```\n");
    assert(doc.is_valid());

    auto style = MarkdownStyle::dark_theme();
    text::FontSystem fonts;
    text::GlyphCache cache;
    float h = doc.layout(600.0f, style, fonts, cache);
    assert(h > 100.0f);
    assert(doc.root()->children[1]->math != nullptr);
    assert(doc.root()->children[2]->diagram != nullptr);

    auto pixmap = Pixmap::create(640, static_cast<uint32_t>(h + 20.0f));
    assert(pixmap.has_value());
    Canvas canvas(*pixmap);
    MarkdownRenderer::render(canvas, doc, 20.0f, 10.0f, 600.0f, style, fonts, cache);

    auto pdf = MarkdownPdfExporter::export_to_bytes(doc, MarkdownStyle::light_theme(), fonts, cache);
    assert(!pdf.empty());

    std::cout << "  Math & Diagram document layout, CPU render and PDF export verified." << std::endl;
}

// ============================================================================
// Phase 1–4 Feature Parity & Spec Compliance Test Suites
// ============================================================================

void test_phase1_streaming_parser() {
    std::cout << "[Test] Phase 1.1: Incremental Streaming Parser (feed + snapshot + finish)..." << std::endl;

    std::string md =
        "# Streaming Title\r\n"
        "\r\n"
        "Check [forward ref][f1] and **bold** text with UTF-8: — café 🚀.\r\n"
        "\r\n"
        "```cpp\r\n"
        "int x = 1 + 2;\r\n"
        "```\r\n"
        "\r\n"
        "| ColA | ColB |\r\n"
        "| :--- | ---: |\r\n"
        "| 10   | 20   |\r\n"
        "\r\n"
        "- Item A\r\n"
        "- Item B\r\n"
        "\r\n"
        "[f1]: https://nisaba.dev/stream \"Stream Target\"\r\n";

    MarkdownParseOptions opts;
    opts.validate_utf8 = true;
    auto expected = MarkdownParser::parse(md, opts);
    std::string expected_html = MarkdownExporter::to_html(*expected);

    // Feed in 1-byte, 7-byte, and 33-byte chunks (splitting CRLF and UTF-8 mid-sequence!)
    for (size_t chunk_sz : {size_t{1}, size_t{7}, size_t{33}}) {
        MarkdownParser streamer(opts);
        size_t max_snap_blocks = 0;
        for (size_t off = 0; off < md.size(); off += chunk_sz) {
            size_t n = std::min(chunk_sz, md.size() - off);
            streamer.feed(std::string_view(md.data() + off, n));
            if (off >= md.size() / 2 && max_snap_blocks == 0) {
                auto snap = streamer.snapshot();
                assert(snap != nullptr);
                max_snap_blocks = std::max(max_snap_blocks, snap->children.size());
            }
        }
        assert(max_snap_blocks >= 2); // Mid-stream snapshot already has committed blocks!
        auto streamed = streamer.finish();
        assert(streamed != nullptr);
        assert(streamed->children.size() == expected->children.size());
        std::string streamed_html = MarkdownExporter::to_html(*streamed);
        assert(streamed_html == expected_html);
    }

    // Verify reset() allows clean parser reuse
    MarkdownParser reusable(opts);
    reusable.feed("# First\n");
    auto first = reusable.finish();
    assert(first->children.size() == 1 && first->children[0]->type == BlockType::Heading);

    reusable.reset();
    reusable.feed("Second paragraph.\n");
    auto second = reusable.finish();
    assert(second->children.size() == 1 && second->children[0]->type == BlockType::Paragraph);

    std::cout << "  Incremental streaming parser (1B/7B/33B chunks, snapshot, reset) verified." << std::endl;
}

void test_phase1_sourcepos() {
    std::cout << "[Test] Phase 1.2: AST Source Positions (SourcePos)..." << std::endl;

    std::string md =
        "# Title\n"                   // line 1
        "\n"                          // line 2
        "Hello **world** here\n"      // line 3
        "second line.\n"              // line 4
        "\n"                          // line 5
        "```cpp\n"                    // line 6
        "auto a = 1;\n"               // line 7
        "```\n"                       // line 8
        "\n"                          // line 9
        "---\n";                      // line 10

    auto doc = MarkdownParser::parse(md);
    assert(doc->sourcepos.is_valid());
    assert(doc->sourcepos.start_line == 1);
    assert(doc->sourcepos.end_line == 10);

    // Heading on line 1
    const auto& h = *doc->children[0];
    assert(h.type == BlockType::Heading);
    assert(h.sourcepos.start_line == 1 && h.sourcepos.start_col == 1);
    assert(h.sourcepos.end_line == 1 && h.sourcepos.end_col == 7);

    // Multi-line paragraph on lines 3..4
    const auto& p = *doc->children[1];
    assert(p.type == BlockType::Paragraph);
    assert(p.sourcepos.start_line == 3 && p.sourcepos.start_col == 1);
    assert(p.sourcepos.end_line == 4);

    bool found_bold_pos = false;
    for (const auto& sp : p.inlines) {
        assert(sp.sourcepos.is_valid());
        if (sp.type == InlineType::Bold && sp.text == "world") {
            assert(sp.sourcepos.start_line == 3);
            assert(sp.sourcepos.start_col == 7); // "Hello " = 6 bytes, "**world**" starts at col 7
            assert(sp.sourcepos.end_line == 3);
            assert(sp.sourcepos.end_col == 15);
            found_bold_pos = true;
        }
    }
    assert(found_bold_pos);

    // Fenced code block on lines 6..8
    const auto& cb = *doc->children[2];
    assert(cb.type == BlockType::CodeBlock);
    assert(cb.sourcepos.start_line == 6 && cb.sourcepos.end_line == 8);

    // Thematic break on line 10
    const auto& hr = *doc->children[3];
    assert(hr.type == BlockType::ThematicBreak);
    assert(hr.sourcepos.start_line == 10 && hr.sourcepos.end_line == 10);

    std::cout << "  BlockNode & InlineSpan SourcePos verified." << std::endl;
}

void test_phase1_parse_options() {
    std::cout << "[Test] Phase 1.3: Parser Configuration Flags (MarkdownParseOptions)..." << std::endl;

    // 1. hard_breaks vs no_breaks vs default SoftBreak
    std::string two_line = "Alpha\nBeta\n";
    {
        auto d_def = MarkdownParser::parse(two_line);
        bool has_soft = false;
        for (const auto& s : d_def->children[0]->inlines) {
            if (s.type == InlineType::SoftBreak) has_soft = true;
        }
        assert(has_soft);

        MarkdownParseOptions opt_hard;
        opt_hard.hard_breaks = true;
        auto d_hard = MarkdownParser::parse(two_line, opt_hard);
        bool has_hard = false;
        for (const auto& s : d_hard->children[0]->inlines) {
            if (s.type == InlineType::LineBreak) has_hard = true;
        }
        assert(has_hard);

        MarkdownParseOptions opt_no;
        opt_no.no_breaks = true;
        auto d_no = MarkdownParser::parse(two_line, opt_no);
        for (const auto& s : d_no->children[0]->inlines) {
            assert(s.type != InlineType::SoftBreak && s.type != InlineType::LineBreak);
        }
        assert(MarkdownExporter::to_html(*d_no) == "<p>Alpha Beta</p>\n");
    }

    // 2. unsafe_html false vs true
    std::string raw_html_md = "<section>Raw HTML Block</section>\n";
    {
        MarkdownParseOptions opt_safe;
        opt_safe.unsafe_html = false;
        auto d_safe = MarkdownParser::parse(raw_html_md, opt_safe);
        assert(d_safe->children.empty());

        MarkdownParseOptions opt_unsafe;
        opt_unsafe.unsafe_html = true;
        auto d_unsafe = MarkdownParser::parse(raw_html_md, opt_unsafe);
        assert(d_unsafe->children.size() == 1);
        assert(d_unsafe->children[0]->type == BlockType::HtmlBlock);
        assert(d_unsafe->children[0]->raw_html == "<section>Raw HTML Block</section>");
    }

    // 3. validate_utf8 (invalid bytes & embedded NUL -> U+FFFD)
    {
        std::string bad_utf8("Valid ");
        bad_utf8.push_back('\xFF');
        bad_utf8.push_back('\xFE');
        bad_utf8.push_back(' ');
        bad_utf8.push_back('\0');
        bad_utf8 += " end";

        MarkdownParseOptions opt_utf8;
        opt_utf8.validate_utf8 = true;
        auto d = MarkdownParser::parse(bad_utf8, opt_utf8);
        std::string plain = MarkdownExporter::to_plaintext(*d);
        assert(plain.find("\xEF\xBF\xBD") != std::string::npos);
        assert(plain.find('\xFF') == std::string::npos);
        assert(plain.find('\0') == std::string::npos);
    }

    // 4. smart_punctuation
    {
        MarkdownParseOptions opt_smart;
        opt_smart.smart_punctuation = true;
        std::string smart_md = "Wait---this is \"smart\" -- it's 'cool'... and `\"code\" --` and \\\"escaped\\\".\n";
        auto d = MarkdownParser::parse(smart_md, opt_smart);
        std::string html = MarkdownExporter::to_html(*d);
        assert(html.find("\xe2\x80\x94") != std::string::npos); // — em-dash
        assert(html.find("\xe2\x80\x93") != std::string::npos); // – en-dash
        assert(html.find("\xe2\x80\xa6") != std::string::npos); // … ellipsis
        assert(html.find("\xe2\x80\x9csmart\xe2\x80\x9d") != std::string::npos); // “smart”
        assert(html.find("it\xe2\x80\x99s") != std::string::npos); // it’s
        assert(html.find("\xe2\x80\x98" "cool\xe2\x80\x99") != std::string::npos); // ‘cool’
        // Code span and escaped quotes must remain ASCII!
        assert(html.find("<code>&quot;code&quot; --</code>") != std::string::npos);
        assert(html.find("&quot;escaped&quot;") != std::string::npos);
    }

    std::cout << "  All MarkdownParseOptions flags verified." << std::endl;
}

void test_phase2_delimiter_stack() {
    std::cout << "[Test] Phase 2.1: Full Delimiter Stack Algorithm (Emphasis & Links)..." << std::endl;

    // 1. Intraword underscores MUST NOT create emphasis; intraword asterisks MUST
    {
        auto s_under = MarkdownParser::parse_inlines("foo_bar_baz and foo__bar__baz");
        for (const auto& s : s_under) {
            assert(s.type == InlineType::Text);
        }
        auto s_star = MarkdownParser::parse_inlines("foo*bar*baz");
        assert(s_star.size() == 3);
        assert(s_star[0].type == InlineType::Text && s_star[0].text == "foo");
        assert(s_star[1].type == InlineType::Italic && s_star[1].text == "bar");
        assert(s_star[2].type == InlineType::Text && s_star[2].text == "baz");
    }

    // 2. Underscores flanked by punctuation ARE valid emphasis
    {
        auto s_punct = MarkdownParser::parse_inlines("(_italic_)");
        assert(s_punct.size() == 3);
        assert(s_punct[1].type == InlineType::Italic && s_punct[1].text == "italic");
    }

    // 3. Nested emphasis: **bold *and italic* bold** and ***both***
    {
        auto d = MarkdownParser::parse("**bold *and italic* bold** and ***both***");
        std::string html = MarkdownExporter::to_html(*d);
        assert(html.find("<strong>bold <em>and italic</em> bold</strong>") != std::string::npos);
        assert(html.find("<em><strong>both</strong></em>") != std::string::npos);
    }

    // 4. Emphasis across links and balanced nested brackets in links
    {
        auto d = MarkdownParser::parse("*[outer [inner] text](https://example.com/a_(b))*");
        std::string html = MarkdownExporter::to_html(*d);
        assert(html.find("<em><a href=\"https://example.com/a_(b)\">outer [inner] text</a></em>") != std::string::npos);
    }

    // 5. Multi-backtick code spans & single space stripping
    {
        auto s = MarkdownParser::parse_inlines("``code with ` backtick`` and `` ` ``");
        int codes = 0;
        for (const auto& sp : s) {
            if (sp.type == InlineType::CodeSpan) {
                if (codes == 0) assert(sp.text == "code with ` backtick");
                if (codes == 1) assert(sp.text == "`");
                codes++;
            }
        }
        assert(codes == 2);
    }

    // 6. Backslash hard break & two-space hard break
    {
        auto d = MarkdownParser::parse("line1\\\nline2  \nline3");
        int brs = 0;
        for (const auto& sp : d->children[0]->inlines) {
            if (sp.type == InlineType::LineBreak) brs++;
        }
        assert(brs == 2);
    }

    std::cout << "  Full Delimiter Stack algorithm verified." << std::endl;
}

void test_phase2_entities_and_escapes() {
    std::cout << "[Test] Phase 2.2: Complete HTML5 Entities & Backslash Escapes..." << std::endl;

    // 1. Named HTML5 entities + numeric decimal + numeric hex + invalid codepoint -> U+FFFD
    auto spans = MarkdownParser::parse_inlines(
        "&copy; &mdash; &nbsp; &euro; &AElig; &#123; &#x1F600; &#0; &#xD800;"
    );
    assert(spans.size() == 1);
    const std::string& t = spans[0].text;
    assert(t.find("\xc2\xa9") != std::string::npos);         // ©
    assert(t.find("\xe2\x80\x94") != std::string::npos);     // —
    assert(t.find("\xc2\xa0") != std::string::npos);         // NBSP
    assert(t.find("\xe2\x82\xac") != std::string::npos);     // €
    assert(t.find("\xc3\x86") != std::string::npos);         // Æ
    assert(t.find('{') != std::string::npos);                // &#123;
    assert(t.find("\xf0\x9f\x98\x80") != std::string::npos); // 😀
    assert(t.find("\xef\xbf\xbd") != std::string::npos);     // U+FFFD for &#0; & &#xD800;

    // 2. All 32 CommonMark ASCII punctuation backslash escapes
    std::string punct = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    assert(punct.size() == 32);
    std::string escaped_input;
    for (char c : punct) {
        escaped_input.push_back('\\');
        escaped_input.push_back(c);
    }
    auto esc_spans = MarkdownParser::parse_inlines(escaped_input);
    assert(esc_spans.size() == 1);
    assert(esc_spans[0].text == punct);

    // 3. Non-punctuation backslash MUST remain literal
    auto non_punct = MarkdownParser::parse_inlines("\\a \\1 \\Z");
    assert(non_punct.size() == 1);
    assert(non_punct[0].text == "\\a \\1 \\Z");

    std::cout << "  HTML5 entities, numeric/hex refs, and 32 ASCII punctuation escapes verified." << std::endl;
}

void test_phase2_classic_blocks() {
    std::cout << "[Test] Phase 2.3: Setext Headings, Indented Code, Lazy Continuation & Tight/Loose Lists..." << std::endl;

    // 1. Setext Headings (H1 and H2)
    {
        auto doc = MarkdownParser::parse(
            "Setext H1\n"
            "=========\n"
            "\n"
            "Setext H2\n"
            "---------\n"
            "\n"
            "---\n"
        );
        assert(doc->children.size() == 3);
        assert(doc->children[0]->type == BlockType::Heading && doc->children[0]->heading_level == 1 && doc->children[0]->is_setext_heading);
        assert(doc->children[1]->type == BlockType::Heading && doc->children[1]->heading_level == 2 && doc->children[1]->is_setext_heading);
        assert(doc->children[2]->type == BlockType::ThematicBreak);
    }

    // 2. Indented Code Blocks (4 spaces and tab, preserving internal blank lines, stripping trailing blanks)
    {
        auto doc = MarkdownParser::parse(
            "    int x = 1;\n"
            "\n"
            "\treturn x;\n"
            "    \n"
        );
        assert(doc->children.size() == 1);
        const auto& cb = *doc->children[0];
        assert(cb.type == BlockType::CodeBlock);
        assert(cb.is_indented_code);
        assert(cb.code_lines.size() == 3);
        assert(cb.code_lines[0] == "int x = 1;");
        assert(cb.code_lines[1].empty());
        assert(cb.code_lines[2] == "return x;");
    }

    // 3. Lazy continuation in BlockQuote and ListItem
    {
        auto doc = MarkdownParser::parse(
            "> Quoted line 1\n"
            "lazy quoted line 2\n"
            "\n"
            "- List item line 1\n"
            "lazy list line 2\n"
        );
        assert(doc->children.size() == 2);
        assert(doc->children[0]->type == BlockType::BlockQuote);
        assert(doc->children[0]->children.size() == 1);
        std::string bq_plain = MarkdownExporter::to_plaintext(*doc->children[0]);
        assert(bq_plain.find("Quoted line 1 lazy quoted line 2") != std::string::npos);

        assert(doc->children[1]->type == BlockType::List);
        std::string li_plain = MarkdownExporter::to_plaintext(*doc->children[1]);
        assert(li_plain.find("List item line 1 lazy list line 2") != std::string::npos);
    }

    // 4. Tight vs Loose lists
    {
        auto tight_doc = MarkdownParser::parse("- one\n- two\n");
        assert(tight_doc->children[0]->type == BlockType::List);
        assert(tight_doc->children[0]->is_tight_list);
        assert(!tight_doc->children[0]->is_loose_list);
        assert(MarkdownExporter::to_html(*tight_doc) == "<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n");

        auto loose_doc = MarkdownParser::parse("- one\n\n- two\n");
        assert(loose_doc->children[0]->type == BlockType::List);
        assert(!loose_doc->children[0]->is_tight_list);
        assert(loose_doc->children[0]->is_loose_list);
        std::string loose_html = MarkdownExporter::to_html(*loose_doc);
        assert(loose_html.find("<li>\n<p>one</p>\n</li>") != std::string::npos);
    }

    std::cout << "  Setext headings, indented code, lazy continuation & tight/loose lists verified." << std::endl;
}

void test_phase2_reference_links() {
    std::cout << "[Test] Phase 2.4: Complete Reference Links (Full, Collapsed, Shortcut, Unicode, Multi-line)..." << std::endl;

    std::string md =
        "[Full][Foo   Bar], [Collapsed][], [Shortcut], [Unicode][STRASSE], and ![Pic][img1].\n"
        "\n"
        "[foo bar]: <https://example.com/a b>\n"
        "  \"Multi-line\n"
        "title\"\n"
        "[collapsed]: https://example.com/collapsed 'Single Title'\n"
        "[shortcut]: https://example.com/shortcut (Paren Title)\n"
        "[shortcut]: https://example.com/ignored-second\n"
        "[stra\xc3\x9f" "e]: https://example.com/strasse\n"
        "[img1]: https://example.com/pic.png \"Logo\"\n"
        "\n"
        "```md\n"
        "[in_code]: https://should.not/parse\n"
        "```\n";

    auto doc = MarkdownParser::parse(md);
    assert(doc->children.size() == 2); // Paragraph + CodeBlock
    const auto& inlines = doc->children[0]->inlines;

    bool found_full = false, found_collapsed = false, found_shortcut = false, found_unicode = false, found_img = false;
    for (const auto& s : inlines) {
        if (s.type == InlineType::Link && s.text == "Full" && s.target == "https://example.com/a b" && s.title == "Multi-line\ntitle") {
            found_full = true;
        }
        if (s.type == InlineType::Link && s.text == "Collapsed" && s.target == "https://example.com/collapsed" && s.title == "Single Title") {
            found_collapsed = true;
        }
        if (s.type == InlineType::Link && s.text == "Shortcut" && s.target == "https://example.com/shortcut" && s.title == "Paren Title") {
            found_shortcut = true;
        }
        if (s.type == InlineType::Link && s.text == "Unicode" && s.target == "https://example.com/strasse") {
            found_unicode = true;
        }
        if (s.type == InlineType::Image && s.text == "Pic" && s.target == "https://example.com/pic.png" && s.title == "Logo") {
            found_img = true;
        }
    }
    assert(found_full && found_collapsed && found_shortcut && found_unicode && found_img);

    std::cout << "  Full, collapsed, shortcut, angle-URL, multi-line title & Unicode reference links verified." << std::endl;
}

void test_phase3_tagfilter() {
    std::cout << "[Test] Phase 3.1: GFM Tagfilter Extension (HTML Sanitization)..." << std::endl;

    std::string raw =
        "<title>t</title><textarea>a</textarea><style>s</style>"
        "<xmp>x</xmp><iframe src=\"x\"></iframe><noembed>n</noembed>"
        "<noframes>f</noframes><SCRIPT>alert(1)</SCRIPT><plaintext>p"
        "<div>safe</div>";

    std::string filtered = HtmlParser::apply_tagfilter(raw);
    assert(filtered.find("&lt;title>") != std::string::npos);
    assert(filtered.find("&lt;textarea>") != std::string::npos);
    assert(filtered.find("&lt;style>") != std::string::npos);
    assert(filtered.find("&lt;xmp>") != std::string::npos);
    assert(filtered.find("&lt;iframe ") != std::string::npos);
    assert(filtered.find("&lt;noembed>") != std::string::npos);
    assert(filtered.find("&lt;noframes>") != std::string::npos);
    assert(filtered.find("&lt;SCRIPT>") != std::string::npos);
    assert(filtered.find("&lt;/SCRIPT>") != std::string::npos);
    assert(filtered.find("&lt;plaintext>") != std::string::npos);
    assert(filtered.find("<div>safe</div>") != std::string::npos);

    // End-to-end parser + HTML exporter verification
    MarkdownParseOptions parse_opts;
    parse_opts.unsafe_html = true;
    parse_opts.tagfilter = false; // Leave raw in AST to verify exporter-side tagfilter too
    auto doc = MarkdownParser::parse("<script>alert(1)</script>\n\n<div>ok</div>\n", parse_opts);

    MarkdownHtmlExportOptions safe_html_opts;
    safe_html_opts.unsafe_html = false;
    assert(MarkdownExporter::to_html(*doc, safe_html_opts).find("<!-- raw HTML omitted -->") != std::string::npos);

    MarkdownHtmlExportOptions tf_html_opts;
    tf_html_opts.unsafe_html = true;
    tf_html_opts.tagfilter = true;
    std::string tf_out = MarkdownExporter::to_html(*doc, tf_html_opts);
    assert(tf_out.find("&lt;script>alert(1)&lt;/script>") != std::string::npos);
    assert(tf_out.find("<div>ok</div>") != std::string::npos);

    std::cout << "  GFM Tagfilter (all 9 disallowed tags + safe tag preservation) verified." << std::endl;
}

void test_phase3_strict_autolinks() {
    std::cout << "[Test] Phase 3.2: Strict GFM Autolink Extension..." << std::endl;

    std::string md =
        "Visit www.example.com/docs! "
        "See (https://en.wikipedia.org/wiki/C_(programming_language)). "
        "Email user.name+tag@nisaba.dev, or bad www.bad_domain.com.";

    auto spans = MarkdownParser::parse_inlines(md);
    bool found_www = false, found_wiki = false, found_email = false, found_bad = false;

    for (const auto& s : spans) {
        if (s.type == InlineType::Link) {
            if (s.target == "http://www.example.com/docs") found_www = true;
            if (s.target == "https://en.wikipedia.org/wiki/C_(programming_language)") found_wiki = true;
            if (s.target == "mailto:user.name+tag@nisaba.dev") found_email = true;
            if (s.target.find("bad_domain") != std::string::npos) found_bad = true;
        }
    }
    assert(found_www);
    assert(found_wiki);
    assert(found_email);
    assert(!found_bad);

    std::cout << "  Strict GFM Autolinks (www, balanced parens, email, trailing punct) verified." << std::endl;
}

void test_phase3_extensions() {
    std::cout << "[Test] Phase 3.3: Extensible Syntax Extension Hook (MarkdownExtension)..." << std::endl;

    class MentionAndCalloutExtension final : public MarkdownExtension {
    public:
        std::string_view name() const noexcept override { return "mention_and_callout"; }

        bool try_parse_block(
            const BlockExtensionContext& ctx,
            std::shared_ptr<BlockNode>& out_block,
            size_t& out_lines_consumed
        ) const override {
            auto line = ctx.current_line();
            if (line.starts_with(":::banner ")) {
                auto blk = std::make_shared<BlockNode>(BlockType::Custom);
                blk->custom_kind = "banner";
                blk->inlines.push_back(InlineSpan::make_text(std::string(line.substr(10))));
                out_block = std::move(blk);
                out_lines_consumed = 1;
                return true;
            }
            return false;
        }

        bool try_parse_inline(
            const InlineExtensionContext& ctx,
            InlineSpan& out_span,
            size_t& out_bytes_consumed
        ) const override {
            auto rem = ctx.remaining();
            if (rem.size() >= 2 && (rem[0] == '@' || rem[0] == '#')) {
                size_t end = 1;
                while (end < rem.size() && (std::isalnum(static_cast<unsigned char>(rem[end])) || rem[end] == '_' || rem[end] == '-')) {
                    end++;
                }
                if (end > 1) {
                    std::string token(rem.substr(0, end));
                    std::string kind = (rem[0] == '@') ? "mention" : "issue";
                    out_span = InlineSpan::make_custom(kind, token, "/" + kind + "/" + token.substr(1));
                    out_bytes_consumed = end;
                    return true;
                }
            }
            return false;
        }

        bool render_html_block(const BlockNode& block, std::string& out_html) const override {
            if (block.custom_kind == "banner") {
                out_html += "<aside class=\"banner\">";
                if (!block.inlines.empty()) out_html += block.inlines[0].text;
                out_html += "</aside>\n";
                return true;
            }
            return false;
        }

        bool render_html_inline(const InlineSpan& span, std::string& out_html) const override {
            if (span.custom_kind == "mention" || span.custom_kind == "issue") {
                out_html += "<a class=\"" + span.custom_kind + "\" href=\"" + span.target + "\">" + span.text + "</a>";
                return true;
            }
            return false;
        }
    };

    MarkdownExtensionRegistry reg;
    reg.register_extension(std::make_shared<MentionAndCalloutExtension>());

    std::string md =
        ":::banner Sovereign Release\n"
        "\n"
        "Thanks @alice for fixing #42!\n";

    MarkdownParseOptions opts;
    auto doc = MarkdownParser::parse(md, opts, &reg);
    assert(doc->children.size() == 2);
    assert(doc->children[0]->type == BlockType::Custom && doc->children[0]->custom_kind == "banner");

    bool found_mention = false, found_issue = false;
    for (const auto& sp : doc->children[1]->inlines) {
        if (sp.type == InlineType::Custom && sp.custom_kind == "mention" && sp.text == "@alice") found_mention = true;
        if (sp.type == InlineType::Custom && sp.custom_kind == "issue" && sp.text == "#42") found_issue = true;
    }
    assert(found_mention && found_issue);

    MarkdownHtmlExportOptions hopts;
    hopts.extensions = &reg;
    std::string html = MarkdownExporter::to_html(*doc, hopts);
    assert(html.find("<aside class=\"banner\">Sovereign Release</aside>") != std::string::npos);
    assert(html.find("<a class=\"mention\" href=\"/mention/alice\">@alice</a>") != std::string::npos);
    assert(html.find("<a class=\"issue\" href=\"/issue/42\">#42</a>") != std::string::npos);

    std::cout << "  Pluggable MarkdownExtension (block + inline + HTML hooks) verified." << std::endl;
}

void test_phase4_exporters() {
    std::cout << "[Test] Phase 4: Multi-Format Exporters (to_html, to_plaintext, to_markdown)..." << std::endl;

    std::string md =
        "# Heading One\n"
        "\n"
        "A paragraph with **bold**, *italic*, `code`, ~~del~~, and [link](https://nisaba.dev \"Home\").\n"
        "\n"
        "- [x] Done task\n"
        "- [ ] Pending task\n"
        "\n"
        "| Key | Value |\n"
        "| :--- | ---: |\n"
        "| Engine | Nisaba |\n"
        "\n"
        "```cpp\n"
        "int v = 7;\n"
        "```\n"
        "\n"
        "> Quoted block.\n"
        "\n"
        "---\n";

    auto doc = MarkdownDocument::from_string(md);

    // 4.1 to_html (with sourcepos)
    MarkdownHtmlExportOptions hopts;
    hopts.sourcepos = true;
    std::string html = doc.to_html(hopts);
    assert(html.find("<h1 data-sourcepos=\"1:1-1:13\">Heading One</h1>") != std::string::npos);
    assert(html.find("<strong>bold</strong>") != std::string::npos);
    assert(html.find("<em>italic</em>") != std::string::npos);
    assert(html.find("<code>code</code>") != std::string::npos);
    assert(html.find("<del>del</del>") != std::string::npos);
    assert(html.find("<a href=\"https://nisaba.dev\" title=\"Home\">link</a>") != std::string::npos);
    assert(html.find("checked=\"\" disabled=\"\"") != std::string::npos);
    assert(html.find("<th align=\"right\">Value</th>") != std::string::npos);
    assert(html.find("<pre data-sourcepos=") != std::string::npos);
    assert(html.find("<code class=\"language-cpp\">int v = 7;\n</code>") != std::string::npos);
    assert(html.find("<hr data-sourcepos=") != std::string::npos);

    // 4.2 to_plaintext
    std::string plain = doc.to_plaintext();
    assert(plain.find("Heading One") != std::string::npos);
    assert(plain.find("A paragraph with bold, italic, code, del, and link.") != std::string::npos);
    assert(plain.find("[x] Done task") != std::string::npos);
    assert(plain.find("Engine\tNisaba") != std::string::npos);
    assert(plain.find("**") == std::string::npos);
    assert(plain.find("<h1>") == std::string::npos);

    // 4.3 to_markdown round-trip parity
    std::string formatted = doc.to_markdown();
    auto reparsed = MarkdownDocument::from_string(formatted);
    assert(doc.to_html() == reparsed.to_html());

    std::cout << "  to_html, to_plaintext, and to_markdown round-trip verified." << std::endl;
}

void test_canvas_icanvas_pdf_parity() {
    std::cout << "[Test] Verification: CPU Canvas, RTTI-free ICanvas & PDF Export Parity..." << std::endl;

    class RecordingCanvas final : public ICanvas {
    public:
        uint32_t canvas_width() const noexcept override { return 800; }
        uint32_t canvas_height() const noexcept override { return 1200; }
        void save() override {}
        void restore() override {}
        void translate(float, float) override {}
        void scale(float, float) override {}
        void concat(const Transform&) override {}
        void fill_rect(const Rect&, const Paint&) override { draw_ops++; }
        void fill_path(const Path&, const Paint&, FillRule) override { draw_ops++; }
        void stroke_path(const Path&, const Paint&, const Stroke&) override { draw_ops++; }
        size_t draw_ops{0};
    };

    std::string md =
        "Setext Title\n"
        "============\n"
        "\n"
        "Smart &copy; text with **bold**, `code`, and https://nisaba.dev.\n"
        "\n"
        "    indented_code();\n"
        "\n"
        "> [!TIP]\n"
        "> Parity check across render backends.\n"
        "\n"
        "- Tight 1\n"
        "- Tight 2\n"
        "\n"
        "| A | B |\n"
        "| :- | -: |\n"
        "| 1 | 2 |\n";

    MarkdownParseOptions opts;
    opts.smart_punctuation = true;
    auto doc = MarkdownDocument::from_string(md, opts);
    auto style = MarkdownStyle::dark_theme();
    text::FontSystem fonts;
    text::GlyphCache cache;

    float h = doc.layout(640.0f, style, fonts, cache);
    assert(h > 150.0f);

    // 1. CPU Canvas
    auto pixmap = Pixmap::create(700, static_cast<uint32_t>(h + 40.0f));
    assert(pixmap.has_value());
    Canvas cpu_canvas(*pixmap);
    cpu_canvas.clear(Color::from_rgba8(10, 14, 24, 255));
    MarkdownRenderer::render(cpu_canvas, doc, 20.0f, 10.0f, 640.0f, style, fonts, cache);

    // 2. Polymorphic RTTI-free ICanvas (exercises the exact GPU/ICanvas vector path)
    RecordingCanvas rec_canvas;
    MarkdownRenderer::render(static_cast<ICanvas&>(rec_canvas), doc, 20.0f, 10.0f, 640.0f, style, fonts, cache);
    assert(rec_canvas.draw_ops >= 8);

    // 3. PdfCanvas directly via ICanvas + MarkdownPdfExporter
    pdf::PdfCanvas pdf_canvas(595.28f, 841.89f);
    MarkdownRenderer::render(static_cast<ICanvas&>(pdf_canvas), doc, 20.0f, 10.0f, 550.0f, style, fonts, cache);
    assert(!pdf_canvas.content_stream().empty());

    auto pdf_bytes = MarkdownPdfExporter::export_to_bytes(doc, MarkdownStyle::light_theme(), fonts, cache);
    assert(!pdf_bytes.empty());

    std::cout << "  CPU Canvas, polymorphic ICanvas (" << rec_canvas.draw_ops << " vector ops) & PDF parity verified." << std::endl;
}

int main() {
    std::cout << "=== NISABA SOVEREIGN MARKDOWN ENGINE TESTS ===" << std::endl;

    test_markdown_parser_blocks();
    test_markdown_inlines();
    test_markdown_code_blocks();
    test_markdown_lists();
    test_markdown_tables();
    test_markdown_blockquotes();
    test_markdown_document_and_render();

    // GitHub-Grade Markdown test suites
    test_markdown_html_tags();
    test_markdown_github_alerts();
    test_markdown_syntax_highlighter();
    test_markdown_link_reference_definitions();
    test_markdown_emojis_and_autolinks();
    test_markdown_footnotes_and_slugs();
    test_markdown_interactive_hit_test();
    test_markdown_advanced_layout_and_recursion();
    test_markdown_vector_pdf_exporter();
    test_markdown_math_engine();
    test_markdown_diagram_engine();
    test_markdown_math_diagram_document_and_pdf();

    // Full CommonMark 0.30 + GFM Parity test suites (Phases 1-4 + Verification)
    test_phase1_streaming_parser();
    test_phase1_sourcepos();
    test_phase1_parse_options();
    test_phase2_delimiter_stack();
    test_phase2_entities_and_escapes();
    test_phase2_classic_blocks();
    test_phase2_reference_links();
    test_phase3_tagfilter();
    test_phase3_strict_autolinks();
    test_phase3_extensions();
    test_phase4_exporters();
    test_canvas_icanvas_pdf_parity();

    std::cout << "\n>>> ALL 31 MARKDOWN TEST SUITES PASSED! <<<\n" << std::endl;
    return 0;
}
