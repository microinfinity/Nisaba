#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::markdown;

namespace {

std::string resolve_font_path(std::string_view font_name) {
    std::vector<std::string> candidates = {
        std::string("fonts/") + std::string(font_name),
        std::string("../fonts/") + std::string(font_name),
        std::string("../../fonts/") + std::string(font_name)
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return "";
}

class ShowcaseExtension final : public MarkdownExtension {
public:
    std::string_view name() const noexcept override { return "showcase_mentions_and_badges"; }

    bool try_parse_block(
        const BlockExtensionContext& ctx,
        std::shared_ptr<BlockNode>& out_block,
        size_t& out_lines_consumed
    ) const override {
        auto line = ctx.current_line();
        if (line.starts_with(":::release ")) {
            auto blk = std::make_shared<BlockNode>(BlockType::Custom);
            blk->custom_kind = "release";
            auto sp = InlineSpan::make_custom("release", std::string(line.substr(11)));
            sp.text_color = Color::from_rgba8(0, 230, 180, 255);
            blk->inlines.push_back(std::move(sp));
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
            // Avoid matching ATX headings or email middle '@'
            if (ctx.pos > 0 && std::isalnum(static_cast<unsigned char>(ctx.text[ctx.pos - 1]))) {
                return false;
            }
            size_t end = 1;
            while (end < rem.size() && (std::isalnum(static_cast<unsigned char>(rem[end])) || rem[end] == '_' || rem[end] == '-')) {
                end++;
            }
            if (end > 1) {
                std::string tok(rem.substr(0, end));
                std::string kind = (rem[0] == '@') ? "mention" : "issue";
                out_span = InlineSpan::make_custom(kind, tok, "/" + kind + "/" + tok.substr(1));
                out_span.text_color = (rem[0] == '@')
                    ? Color::from_rgba8(130, 200, 255, 255)
                    : Color::from_rgba8(255, 190, 90, 255);
                out_bytes_consumed = end;
                return true;
            }
        }
        return false;
    }

    bool render_html_block(const BlockNode& block, std::string& out_html) const override {
        if (block.custom_kind == "release") {
            out_html += "<div class=\"release-banner\">";
            if (!block.inlines.empty()) out_html += block.inlines[0].text;
            out_html += "</div>\n";
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

void write_text_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (out.is_open()) {
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
}

} // namespace

int main() {
    std::cout << "================================================================================\n";
    std::cout << "   NISABA MARKDOWN ENGINE — FULL COMMONMARK 0.30 + GFM VISUAL PARITY SHOWCASE\n";
    std::cout << "================================================================================\n";

    text::FontSystem font_system;
    text::GlyphCache glyph_cache;

    auto inter_path = resolve_font_path("Inter-Regular.ttf");
    auto fira_path = resolve_font_path("FiraMono-Medium.ttf");
    if (!inter_path.empty()) font_system.load_font_file(inter_path);
    if (!fira_path.empty()) font_system.load_font_file(fira_path);

    const std::string md_source =
        "Nisaba Markdown Engine — Complete Spec Parity\n"
        "=============================================\n"
        "\n"
        ":::release [RELEASE] Phase 1-4 CommonMark 0.30 + GFM Verified\n"
        "\n"
        "Smart punctuation & HTML5 entities: \"Nisaba\" --- it's fast -- really... "
        "&copy; 2026 &euro; &hearts; &#x1F600; and literal \\*escaped\\* punctuation.\n"
        "\n"
        "Delimiter Stack & Inline Edge Cases\n"
        "-----------------------------------\n"
        "\n"
        "- Intraword underscore preserved: foo_bar_baz and my_var_name\n"
        "- Intraword asterisk emphasis: foo*bar*baz\n"
        "- Nested emphasis: **bold *and italic* bold** plus ***bold italic*** and ~~strikethrough~~\n"
        "- Multi-backtick code span: ``code with ` inner backtick`` and <kbd>Ctrl</kbd> + <kbd>S</kbd>\n"
        "- Nested brackets & reference links: [Outer [Inner] **Link**][spec-ref], [Collapsed][], [Shortcut]\n"
        "- Strict GFM autolinks: (https://en.wikipedia.org/wiki/C_(programming_language)), www.nisaba.dev, dev@nisaba.dev\n"
        "- Custom extension hook: reviewed by @architect for issue #108\n"
        "\n"
        "> [!NOTE]\n"
        "> Lazy continuation & alerts:\n"
        "this second line lazily continues the alert paragraph seamlessly.\n"
        "\n"
        "> Classic BlockQuote line 1\n"
        "lazy continuation line 2 inside the same paragraph.\n"
        "\n"
        "### Loose List vs Tight List & Task Checkboxes\n"
        "\n"
        "- [x] Streaming `feed()` + `snapshot()` + `finish()`\n"
        "\n"
        "- [x] Exact `SourcePos` on every `BlockNode` and `InlineSpan`\n"
        "\n"
        "- [x] GFM `Tagfilter` sanitization & multi-format exporters\n"
        "\n"
        "### Indented Code Block & Syntax-Highlighted Fenced Code\n"
        "\n"
        "    // 4-space Indented Code Block (CommonMark 4.4)\n"
        "    auto parser = MarkdownParser(opts);\n"
        "    parser.feed(chunk);\n"
        "\n"
        "```cpp\n"
        "// Fenced C++20 Code Block\n"
        "MarkdownParseOptions opts{.smart_punctuation = true, .validate_utf8 = true};\n"
        "auto doc = MarkdownDocument::from_string(source, opts);\n"
        "```\n"
        "\n"
        "### Aligned GFM Table\n"
        "\n"
        "| Capability | Alignment | Status |\n"
        "| :--- | :---: | ---: |\n"
        "| Streaming `feed()` | Centered | **Verified** |\n"
        "| HTML5 Entities (2125) | &mdash; &copy; | **Verified** |\n"
        "| Exporters (`HTML`/`MD`/`TXT`) | Round-Trip | **Verified** |\n"
        "\n"
        "### Display Math & Diagram Blocks\n"
        "\n"
        "$$ E = \\frac{1}{2}mv^2 + \\sum_{i=1}^{n} \\alpha_i $$\n"
        "\n"
        "```mermaid\n"
        "flowchart LR\n"
        "  Stream[feed chunk] --> AST[BlockNode + SourcePos]\n"
        "  AST --> Canvas[CPU / GPU / PDF]\n"
        "  AST --> Export[HTML / Plain / MD]\n"
        "```\n"
        "\n"
        "<script>alert('must-be-filtered')</script>\n"
        "\n"
        "[spec-ref]: <https://nisaba.dev/markdown spec>\n"
        "  \"Multi-line\n"
        "reference title\"\n"
        "[collapsed]: https://nisaba.dev/collapsed\n"
        "[shortcut]: https://nisaba.dev/shortcut\n";

    MarkdownExtensionRegistry registry;
    registry.register_extension(std::make_shared<ShowcaseExtension>());

    MarkdownParseOptions opts;
    opts.smart_punctuation = true;
    opts.validate_utf8 = true;
    opts.unsafe_html = true;
    opts.tagfilter = true;

    // 1. Streaming parse in 19-byte chunks + compare against one-shot parse
    auto t0 = std::chrono::steady_clock::now();
    auto oneshot_root = MarkdownParser::parse(md_source, opts, &registry);
    auto t1 = std::chrono::steady_clock::now();

    MarkdownParser streamer(opts);
    streamer.register_extension(std::make_shared<ShowcaseExtension>());
    size_t mid_snapshot_blocks = 0;
    for (size_t off = 0; off < md_source.size(); off += 19) {
        size_t n = std::min<size_t>(19, md_source.size() - off);
        streamer.feed(std::string_view(md_source.data() + off, n));
        if (off >= md_source.size() / 2 && mid_snapshot_blocks == 0) {
            mid_snapshot_blocks = streamer.snapshot()->children.size();
        }
    }
    auto streamed_root = streamer.finish();
    auto t2 = std::chrono::steady_clock::now();

    MarkdownHtmlExportOptions html_opts;
    html_opts.unsafe_html = true;
    html_opts.tagfilter = true;
    html_opts.sourcepos = true;
    html_opts.extensions = &registry;

    std::string html_oneshot = MarkdownExporter::to_html(*oneshot_root, html_opts);
    std::string html_streamed = MarkdownExporter::to_html(*streamed_root, html_opts);
    bool stream_matches = (html_oneshot == html_streamed);

    double oneshot_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double stream_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
    std::cout << "[+] One-shot parse:  " << std::fixed << std::setprecision(1) << oneshot_us
              << " us (" << oneshot_root->children.size() << " blocks)\n";
    std::cout << "[+] Streaming 19B:   " << stream_us << " us (mid-stream snapshot="
              << mid_snapshot_blocks << " blocks, HTML exact match="
              << (stream_matches ? "YES" : "NO") << ")\n";

    // Print SourcePos inspection table for visual verification
    std::cout << "[+] Top-level AST SourcePos map:\n";
    for (size_t idx = 0; idx < streamed_root->children.size(); ++idx) {
        const auto& b = *streamed_root->children[idx];
        std::cout << "    [" << idx << "] type=" << static_cast<int>(b.type)
                  << " pos=" << b.sourcepos.start_line << ":" << b.sourcepos.start_col
                  << "-" << b.sourcepos.end_line << ":" << b.sourcepos.end_col << "\n";
    }

    // 2. Multi-format exports
    write_text_file("showcase/nisaba_markdown_parity.html", html_streamed);
    std::string plain_txt = MarkdownExporter::to_plaintext(*streamed_root);
    write_text_file("showcase/nisaba_markdown_parity_plain.txt", plain_txt);
    std::string roundtrip_md = MarkdownExporter::to_markdown(*streamed_root);
    write_text_file("showcase/nisaba_markdown_parity_roundtrip.md", roundtrip_md);
    std::cout << "[+] Exported HTML (" << html_streamed.size() << " B), Plaintext ("
              << plain_txt.size() << " B), Round-trip MD (" << roundtrip_md.size() << " B)\n";

    // 3. Render Dark Theme PNG
    constexpr float DOC_W = 980.0f;
    constexpr float PAD_X = 50.0f;
    constexpr float PAD_Y = 40.0f;
    constexpr uint32_t IMG_W = 1080;

    auto render_theme = [&](const MarkdownStyle& style, Color bg, const std::string& png_path) {
        MarkdownDocument doc(streamed_root);
        float h = doc.layout(DOC_W, style, font_system, glyph_cache);
        uint32_t img_h = static_cast<uint32_t>(std::ceil(h + 2.0f * PAD_Y));
        auto pixmap = Pixmap::create(IMG_W, img_h);
        if (!pixmap) return;
        Canvas canvas(*pixmap);
        canvas.clear(bg);
        MarkdownRenderer::render(canvas, doc, PAD_X, PAD_Y, DOC_W, style, font_system, glyph_cache);
        if (pixmap->save_image(png_path)) {
            std::cout << "[+] Saved " << png_path << " (" << IMG_W << "x" << img_h << ")\n";
        }
    };

    render_theme(MarkdownStyle::dark_theme(), Color::from_rgba8(11, 15, 25, 255), "showcase/nisaba_markdown_parity_dark.png");
    render_theme(MarkdownStyle::light_theme(), Color::from_rgba8(248, 250, 252, 255), "showcase/nisaba_markdown_parity_light.png");

    // 4. Export Multi-Page Vector PDF
    MarkdownDocument pdf_doc(streamed_root);
    MarkdownPdfExportOptions pdf_opts;
    pdf_opts.doc_title = "Nisaba Markdown Parity Showcase";
    pdf_opts.header_left = "Nisaba Sovereign Markdown Engine";
    pdf_opts.footer_left = "CommonMark 0.30 + GFM Full Parity";
    if (MarkdownPdfExporter::export_to_file(pdf_doc, "showcase/nisaba_markdown_parity.pdf",
                                            MarkdownStyle::light_theme(), font_system, glyph_cache, pdf_opts)) {
        std::cout << "[+] Saved showcase/nisaba_markdown_parity.pdf\n";
    }

    return stream_matches ? 0 : 1;
}

