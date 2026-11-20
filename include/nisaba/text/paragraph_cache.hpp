#pragma once

/// @file paragraph_cache.hpp
/// @brief LRU Paragraph Layout Cache for Nisaba.
///
/// Caches shaped `Buffer` layouts keyed by `(text, attrs, width, ...)` so repeated
/// identical UI labels skip shaping, BiDi, font matching, and word wrapping on both
/// CPU (`Canvas`) and GPU (`GpuCanvas`) pipelines.

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nisaba/text/attrs.hpp"
#include "nisaba/text/buffer.hpp"
#include "nisaba/text/font_system.hpp"
#include "nisaba/text/layout.hpp"

namespace nisaba::text {

/// Key uniquely identifying a shaped paragraph layout configuration.
struct ParagraphCacheKey {
    std::string text{};
    Attrs attrs{};
    std::vector<AttrsSpan> spans{};
    std::vector<std::pair<size_t, PlaceholderStyle>> placeholders{};
    Metrics metrics{16.0f, 20.0f};
    std::optional<float> width{std::nullopt};
    std::optional<float> height{std::nullopt};
    Wrap wrap{Wrap::Word};
    Align align{Align::Left};
    Shaping shaping{Shaping::Advanced};
    Overflow overflow{Overflow::Visible};
    TextDirection text_direction{TextDirection::Auto};
    StrutStyle strut_style{};
    bool half_leading{false};
    TextHeightBehavior text_height_behavior{};
    std::optional<std::string> ellipsis{std::nullopt};
    std::optional<size_t> max_lines{std::nullopt};
    bool enable_kerning{true};
    bool enable_ligatures{false};
    size_t font_count{0};

    [[nodiscard]] bool operator==(const ParagraphCacheKey& other) const noexcept;
};

struct ParagraphCacheKeyHash {
    [[nodiscard]] size_t operator()(const ParagraphCacheKey& key) const noexcept;
};

/// LRU cache for shaped paragraph `Buffer` instances keyed by `(text, attrs, width, ...)`.
class ParagraphCache {
public:
    explicit ParagraphCache(size_t max_entries = 256) noexcept;
    ~ParagraphCache() = default;

    ParagraphCache(const ParagraphCache&) = delete;
    ParagraphCache& operator=(const ParagraphCache&) = delete;
    ParagraphCache(ParagraphCache&&) noexcept = default;
    ParagraphCache& operator=(ParagraphCache&&) noexcept = default;

    /// Returns a shaped `Buffer` from the cache on hit, or shapes, caches, and returns it on miss.
    [[nodiscard]] std::shared_ptr<Buffer> get_or_layout(
        FontSystem& font_system,
        std::string_view text,
        const Attrs& attrs = Attrs(),
        Metrics metrics = Metrics(16.0f, 20.0f),
        std::optional<float> width = std::nullopt,
        Wrap wrap = Wrap::Word,
        Align align = Align::Left,
        Overflow overflow = Overflow::Visible,
        std::optional<size_t> max_lines = std::nullopt,
        TextDirection text_direction = TextDirection::Auto
    );

    /// Full-configuration lookup using an explicit `ParagraphCacheKey`.
    [[nodiscard]] std::shared_ptr<Buffer> get_or_layout(
        FontSystem& font_system,
        const ParagraphCacheKey& key
    );

    /// Looks up a configured `Buffer` in the cache and hydrates its shaped layout on hit,
    /// or shapes and inserts a cached copy on miss. Returns true on cache hit.
    bool lookup_or_shape(FontSystem& font_system, Buffer& buffer);

    /// Clears all cached paragraph layouts and resets telemetry counters.
    void clear() noexcept;

    /// Updates maximum LRU capacity, evicting least-recently-used entries if needed.
    void set_max_entries(size_t max_entries);

    /// Resets hit/miss telemetry without clearing cached entries.
    void reset_stats() noexcept { hits_ = 0; misses_ = 0; }

    [[nodiscard]] size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] size_t hits() const noexcept { return hits_; }
    [[nodiscard]] size_t misses() const noexcept { return misses_; }
    [[nodiscard]] double hit_ratio() const noexcept {
        const size_t total = hits_ + misses_;
        return total > 0 ? (static_cast<double>(hits_) / static_cast<double>(total)) : 0.0;
    }

private:
    using ListType = std::list<ParagraphCacheKey>;
    struct Entry {
        std::shared_ptr<Buffer> buffer;
        ListType::iterator lru_it;
    };

    void evict_if_needed();

    size_t max_entries_{256};
    size_t hits_{0};
    size_t misses_{0};
    ListType lru_list_{};
    std::unordered_map<ParagraphCacheKey, Entry, ParagraphCacheKeyHash> map_{};
};

using ParagraphLayoutCache = ParagraphCache;

} // namespace nisaba::text

