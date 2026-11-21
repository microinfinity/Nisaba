#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include "nisaba/markdown/types.hpp"
#include "nisaba/markdown/extension.hpp"

namespace nisaba::markdown {

/// High-performance CommonMark 0.30 and GFM parser with incremental streaming support.
class MarkdownParser {
public:
    explicit MarkdownParser(MarkdownParseOptions options = {});
    ~MarkdownParser();

    MarkdownParser(const MarkdownParser&) = delete;
    MarkdownParser& operator=(const MarkdownParser&) = delete;
    MarkdownParser(MarkdownParser&&) noexcept;
    MarkdownParser& operator=(MarkdownParser&&) noexcept;

    /// Configures parser options (must be called before feeding input, or after reset()).
    void set_options(const MarkdownParseOptions& options);
    [[nodiscard]] const MarkdownParseOptions& options() const noexcept;

    /// Registers a pluggable block/inline syntax extension.
    void register_extension(std::shared_ptr<MarkdownExtension> ext);
    [[nodiscard]] const MarkdownExtensionRegistry& extensions() const noexcept;

    /// Resets internal streaming state to begin parsing a new document.
    void reset();

    /// Feeds an arbitrary UTF-8 byte chunk for incremental block-level parsing
    /// without re-parsing previously finalized blocks from byte 0.
    void feed(std::string_view chunk);

    /// Returns a live Document AST snapshot reflecting all blocks received so far
    /// (including any currently open trailing block) for live streaming preview.
    [[nodiscard]] std::shared_ptr<BlockNode> snapshot() const;

    /// Finalizes any pending partial line/open block, resolves forward reference links,
    /// and returns the completed root Document BlockNode.
    [[nodiscard]] std::shared_ptr<BlockNode> finish();

    /// One-shot convenience: parse markdown text into a root Document BlockNode.
    [[nodiscard]] static std::shared_ptr<BlockNode> parse(std::string_view markdown);

    /// One-shot convenience with explicit parse options and optional extension registry.
    [[nodiscard]] static std::shared_ptr<BlockNode> parse(
        std::string_view markdown,
        const MarkdownParseOptions& options,
        const MarkdownExtensionRegistry* extensions = nullptr
    );

    /// Parse a single line or segment of markdown into a sequence of inline spans.
    [[nodiscard]] static std::vector<InlineSpan> parse_inlines(
        std::string_view text,
        const std::unordered_map<std::string, std::pair<std::string, std::string>>* ref_map = nullptr
    );

    /// Parse inline spans with explicit parse options, optional extension registry, and base SourcePos.
    [[nodiscard]] static std::vector<InlineSpan> parse_inlines(
        std::string_view text,
        const MarkdownParseOptions& options,
        const std::unordered_map<std::string, std::pair<std::string, std::string>>* ref_map = nullptr,
        const MarkdownExtensionRegistry* extensions = nullptr,
        SourcePos base_pos = {}
    );

    /// Generate a GitHub-compatible kebab-case slug for headings (e.g. "My Heading!" -> "my-heading").
    [[nodiscard]] static std::string slugify(std::string_view text);

    /// Replace GitHub emoji shortcodes (e.g. ":smile:", ":rocket:") with UTF-8 emojis.
    [[nodiscard]] static std::string replace_emojis(std::string_view text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    static bool is_thematic_break(std::string_view line);
    static int parse_heading_level(std::string_view line, std::string_view& out_text);
    static bool parse_table_row(std::string_view line, std::vector<std::string>& out_cells);
    static bool parse_table_separator(std::string_view line, std::vector<TableAlign>& out_alignments);
    static AlertType parse_alert_marker(std::string_view line, std::string& out_title);
};

} // namespace nisaba::markdown
