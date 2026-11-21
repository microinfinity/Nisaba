#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "nisaba/markdown/types.hpp"

namespace nisaba::markdown {

/// Context passed to block-level Markdown extensions.
struct BlockExtensionContext {
    const std::vector<std::string_view>& lines;
    size_t line_index{0};
    uint32_t base_line_offset{0}; // 0-based line offset in the full document
    const MarkdownParseOptions& options;

    /// Convenience accessor for the current raw line.
    [[nodiscard]] std::string_view current_line() const noexcept {
        return line_index < lines.size() ? lines[line_index] : std::string_view{};
    }

    /// Number of lines remaining starting at `line_index`.
    [[nodiscard]] size_t remaining_lines() const noexcept {
        return line_index < lines.size() ? (lines.size() - line_index) : 0;
    }
};

/// Context passed to inline-level Markdown extensions.
struct InlineExtensionContext {
    std::string_view text;
    size_t pos{0};
    uint32_t line{1};
    uint32_t col{1};
    const MarkdownParseOptions& options;

    /// Remaining slice starting at `pos`.
    [[nodiscard]] std::string_view remaining() const noexcept {
        return pos < text.size() ? text.substr(pos) : std::string_view{};
    }
};

/// Pluggable syntax extension interface for custom block/inline directives
/// (e.g. @mentions, #issue-links, custom callouts) without modifying core parser code.
class MarkdownExtension {
public:
    virtual ~MarkdownExtension() = default;

    /// Unique name identifying this extension.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Attempts to parse a custom block starting at `ctx.line_index`.
    /// If matched, populates `out_block`, sets `out_lines_consumed` (>= 1), and returns true.
    virtual bool try_parse_block(
        const BlockExtensionContext& ctx,
        std::shared_ptr<BlockNode>& out_block,
        size_t& out_lines_consumed
    ) const {
        (void)ctx;
        (void)out_block;
        (void)out_lines_consumed;
        return false;
    }

    /// Attempts to parse a custom inline token starting at `ctx.pos`.
    /// If matched, populates `out_span`, sets `out_bytes_consumed` (>= 1), and returns true.
    virtual bool try_parse_inline(
        const InlineExtensionContext& ctx,
        InlineSpan& out_span,
        size_t& out_bytes_consumed
    ) const {
        (void)ctx;
        (void)out_span;
        (void)out_bytes_consumed;
        return false;
    }

    /// Optional HTML renderer hook for `BlockType::Custom` nodes produced by this extension.
    virtual bool render_html_block(const BlockNode& block, std::string& out_html) const {
        (void)block;
        (void)out_html;
        return false;
    }

    /// Optional HTML renderer hook for `InlineType::Custom` spans produced by this extension.
    virtual bool render_html_inline(const InlineSpan& span, std::string& out_html) const {
        (void)span;
        (void)out_html;
        return false;
    }
};

/// Registry holding ordered Markdown syntax extensions.
class MarkdownExtensionRegistry {
public:
    void register_extension(std::shared_ptr<MarkdownExtension> ext) {
        if (ext) extensions_.push_back(std::move(ext));
    }

    void clear() noexcept { extensions_.clear(); }

    [[nodiscard]] bool empty() const noexcept { return extensions_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return extensions_.size(); }

    [[nodiscard]] const std::vector<std::shared_ptr<MarkdownExtension>>& extensions() const noexcept {
        return extensions_;
    }

private:
    std::vector<std::shared_ptr<MarkdownExtension>> extensions_{};
};

} // namespace nisaba::markdown

