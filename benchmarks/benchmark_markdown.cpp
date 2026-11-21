#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::markdown;

namespace {

std::string build_large_document(size_t repeat_sections) {
    static const std::string_view kSection =
        "# Nisaba Full-Stack Markdown Specification\n"
        "\n"
        "Paragraph with **bold**, *italic*, ***bold italic***, ~~strike~~, `inline code`, "
        "intraword foo_bar_baz, HTML5 entities &copy; &mdash; &#x1F600;, smart quotes \"hello\" -- 'world'..., "
        "an autolink https://nisaba.dev/docs/C_(v2). and email dev@nisaba.dev, plus [reference][ref1].\n"
        "\n"
        "Setext Subheading\n"
        "-----------------\n"
        "\n"
        "> [!NOTE]\n"
        "> Streaming parser incrementally commits finalized blocks without reparsing byte 0.\n"
        "\n"
        "- [x] Incremental `feed()` + `finish()`\n"
        "- [x] Complete `SourcePos` tracking\n"
        "- [ ] Next milestone\n"
        "\n"
        "| Module | Throughput | Status |\n"
        "| :--- | :---: | ---: |\n"
        "| Parser | High | Ready |\n"
        "| Exporter | High | Ready |\n"
        "\n"
        "```cpp\n"
        "int main() { return 0; }\n"
        "```\n"
        "\n"
        "    indented_code_block(42);\n"
        "\n"
        "[ref1]: <https://nisaba.dev/spec> \"Specification Title\"\n"
        "\n";

    std::string doc;
    doc.reserve(kSection.size() * repeat_sections);
    for (size_t i = 0; i < repeat_sections; ++i) {
        doc.append(kSection);
    }
    return doc;
}

template <typename Fn>
double measure_ms(int iterations, Fn&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        fn();
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iterations);
}

} // namespace

int main() {
    std::cout << "=== Nisaba Markdown Engine Throughput Benchmark ===" << std::endl;

    const std::string large_md = build_large_document(200); // ~160 KB rich CommonMark + GFM doc
    const double doc_mb = static_cast<double>(large_md.size()) / (1024.0 * 1024.0);
    std::cout << "Document size: " << large_md.size() << " bytes ("
              << std::fixed << std::setprecision(2) << (large_md.size() / 1024.0) << " KB)" << std::endl;

    MarkdownParseOptions opts;
    opts.smart_punctuation = true;
    opts.validate_utf8 = true;
    opts.tagfilter = true;

    // 1. One-shot full parse
    size_t block_count = 0;
    double full_ms = measure_ms(20, [&]() {
        auto root = MarkdownParser::parse(large_md, opts);
        block_count = root->children.size();
    });
    std::cout << "1. One-shot parse (" << block_count << " blocks): "
              << std::setprecision(3) << full_ms << " ms ("
              << std::setprecision(1) << (doc_mb / (full_ms / 1000.0)) << " MB/s)" << std::endl;

    // 2. Streaming parse with various chunk sizes (simulating AI token streaming & network I/O)
    for (size_t chunk_sz : {size_t{16}, size_t{128}, size_t{4096}}) {
        double stream_ms = measure_ms(10, [&]() {
            MarkdownParser parser(opts);
            for (size_t off = 0; off < large_md.size(); off += chunk_sz) {
                size_t n = std::min(chunk_sz, large_md.size() - off);
                parser.feed(std::string_view(large_md.data() + off, n));
            }
            auto root = parser.finish();
            block_count = root->children.size();
        });
        std::cout << "2. Streaming parse (chunk=" << chunk_sz << " B): "
                  << std::setprecision(3) << stream_ms << " ms ("
                  << std::setprecision(1) << (doc_mb / (stream_ms / 1000.0)) << " MB/s)" << std::endl;
    }

    // 3. Multi-format Exporters (HTML, Plaintext, CommonMark Round-Trip)
    auto parsed = MarkdownParser::parse(large_md, opts);
    size_t html_len = 0, plain_len = 0, md_len = 0;

    double html_ms = measure_ms(20, [&]() {
        auto out = MarkdownExporter::to_html(*parsed);
        html_len = out.size();
    });
    double plain_ms = measure_ms(20, [&]() {
        auto out = MarkdownExporter::to_plaintext(*parsed);
        plain_len = out.size();
    });
    double roundtrip_ms = measure_ms(20, [&]() {
        auto out = MarkdownExporter::to_markdown(*parsed);
        md_len = out.size();
    });

    std::cout << "3. Exporter to_html (" << html_len << " B): "
              << std::setprecision(3) << html_ms << " ms ("
              << std::setprecision(1) << (doc_mb / (html_ms / 1000.0)) << " MB/s)" << std::endl;
    std::cout << "4. Exporter to_plaintext (" << plain_len << " B): "
              << std::setprecision(3) << plain_ms << " ms ("
              << std::setprecision(1) << (doc_mb / (plain_ms / 1000.0)) << " MB/s)" << std::endl;
    std::cout << "5. Exporter to_markdown (" << md_len << " B): "
              << std::setprecision(3) << roundtrip_ms << " ms ("
              << std::setprecision(1) << (doc_mb / (roundtrip_ms / 1000.0)) << " MB/s)" << std::endl;

    return 0;
}

