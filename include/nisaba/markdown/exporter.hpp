#pragma once

#include <string>
#include "nisaba/markdown/types.hpp"
#include "nisaba/markdown/extension.hpp"

namespace nisaba::markdown {

class MarkdownDocument;

/// Configuration options for HTML5 AST export.
struct MarkdownHtmlExportOptions {
    bool unsafe_html{false};       // Emit raw HTML blocks/inlines; if false, emit <!-- raw HTML omitted -->
    bool tagfilter{true};          // Apply GFM Tagfilter to disallowed raw HTML tags
    bool hard_breaks{false};       // Render SoftBreak as <br />\n
    bool no_breaks{false};         // Render SoftBreak as single space
    bool sourcepos{false};         // Emit data-sourcepos="sl:sc-el:ec" attributes on blocks
    const MarkdownExtensionRegistry* extensions{nullptr};
};

/// Configuration options for normalized CommonMark/GFM round-trip formatting.
struct MarkdownFormatOptions {
    char bullet_char{'-'};         // Unordered list bullet character ('-', '*', '+')
    char emphasis_char{'*'};       // Emphasis delimiter character ('*' or '_')
    bool prefer_atx_headings{true};// Emit ATX headings (# ...) instead of Setext headings
};

/// Multi-format AST exporters (HTML5, Plaintext, and CommonMark/GFM Round-Trip).
class MarkdownExporter {
public:
    /// Renders a parsed BlockNode AST to clean GFM-compatible HTML5.
    [[nodiscard]] static std::string to_html(
        const BlockNode& root,
        const MarkdownHtmlExportOptions& options = {}
    );

    /// Renders a MarkdownDocument to clean GFM-compatible HTML5.
    [[nodiscard]] static std::string to_html(
        const MarkdownDocument& doc,
        const MarkdownHtmlExportOptions& options = {}
    );

    /// Extracts clean unformatted UTF-8 text from an AST (clipboard, indexing, accessibility).
    [[nodiscard]] static std::string to_plaintext(const BlockNode& root);

    /// Extracts clean unformatted UTF-8 text from a MarkdownDocument.
    [[nodiscard]] static std::string to_plaintext(const MarkdownDocument& doc);

    /// Serializes an AST back to normalized CommonMark/GFM Markdown source.
    [[nodiscard]] static std::string to_markdown(
        const BlockNode& root,
        const MarkdownFormatOptions& options = {}
    );

    /// Serializes a MarkdownDocument back to normalized CommonMark/GFM Markdown source.
    [[nodiscard]] static std::string to_markdown(
        const MarkdownDocument& doc,
        const MarkdownFormatOptions& options = {}
    );
};

} // namespace nisaba::markdown

