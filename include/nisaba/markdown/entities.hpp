#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace nisaba::markdown {

/// CommonMark 0.30 HTML5 entities, backslash escapes, UTF-8 validation, and Unicode helpers.
class Entities {
public:
    /// Returns true if `c` is one of the 32 CommonMark ASCII punctuation characters.
    [[nodiscard]] static bool is_ascii_punctuation(char c) noexcept;

    /// Returns true if Unicode code point `cp` is Unicode whitespace (CommonMark 0.30).
    [[nodiscard]] static bool is_unicode_whitespace(uint32_t cp) noexcept;

    /// Returns true if Unicode code point `cp` is Unicode punctuation (ASCII + general punctuation).
    [[nodiscard]] static bool is_unicode_punctuation(uint32_t cp) noexcept;

    /// Encodes a Unicode code point as UTF-8 and appends it to `out`.
    /// Invalid code points (0, surrogates, > 0x10FFFF) are encoded as U+FFFD.
    static void append_utf8(std::string& out, uint32_t codepoint);

    /// Decodes one UTF-8 code point starting at `text[pos]`, advancing `pos`.
    /// Returns U+FFFD on invalid byte sequences and advances by 1 byte.
    static uint32_t decode_utf8(std::string_view text, size_t& pos) noexcept;

    /// Decodes the UTF-8 code point immediately preceding `pos` in `text`.
    static uint32_t decode_utf8_prev(std::string_view text, size_t pos) noexcept;

    /// Fast-path check: returns true if `input` is 100% valid UTF-8 with no embedded NUL bytes.
    [[nodiscard]] static bool is_valid_utf8(std::string_view input) noexcept;

    /// Validates UTF-8 input, replacing invalid sequences and NUL bytes with U+FFFD.
    [[nodiscard]] static std::string sanitize_utf8(std::string_view input);

    /// Attempts to decode an HTML5 named entity (`&name;`) or numeric/hex character reference
    /// (`&#123;`, `&#x1F600;`) starting at `text[pos]` (which must be '&').
    /// On success, appends UTF-8 output to `out`, sets `consumed` to the byte length of `&...;`,
    /// and returns true.
    static bool decode_entity_at(
        std::string_view text,
        size_t pos,
        std::string& out,
        size_t& consumed
    );

    /// Unescapes CommonMark ASCII punctuation backslash escapes and decodes HTML5 entities.
    [[nodiscard]] static std::string unescape_string(std::string_view text);

    /// Normalizes a CommonMark link reference label: trims leading/trailing whitespace,
    /// collapses internal whitespace runs to a single space, and performs Unicode case folding.
    [[nodiscard]] static std::string normalize_reference_label(std::string_view label);
};

} // namespace nisaba::markdown

