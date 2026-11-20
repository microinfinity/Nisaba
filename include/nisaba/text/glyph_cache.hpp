#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include <unordered_map>
#include <list>
#include <memory>
#include <cmath>
#include <cstring>
#include "nisaba/canvas/mask.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/text/ttf_font.hpp"

namespace nisaba::text {

/// Binning of subpixel position for rasterization caching.
enum class SubpixelBin : uint8_t {
    Zero = 0,
    One = 1,
    Two = 2,
    Three = 3
};

inline std::pair<int32_t, SubpixelBin> compute_subpixel_bin(float pos) noexcept {
    int32_t trunc = static_cast<int32_t>(std::floor(pos));
    float fract = pos - static_cast<float>(trunc);
    if (fract < 0.125f) {
        return {trunc, SubpixelBin::Zero};
    } else if (fract < 0.375f) {
        return {trunc, SubpixelBin::One};
    } else if (fract < 0.625f) {
        return {trunc, SubpixelBin::Two};
    } else if (fract < 0.875f) {
        return {trunc, SubpixelBin::Three};
    } else {
        return {trunc + 1, SubpixelBin::Zero};
    }
}

inline float bin_to_float(SubpixelBin bin) noexcept {
    switch (bin) {
        case SubpixelBin::Zero: return 0.0f;
        case SubpixelBin::One: return 0.25f;
        case SubpixelBin::Two: return 0.5f;
        case SubpixelBin::Three: return 0.75f;
    }
    return 0.0f;
}

/// Cache key uniquely identifying a rasterized glyph instance.
struct CacheKey {
    uint32_t font_id{0};
    uint16_t glyph_id{0};
    uint32_t font_size_bits{0};
    SubpixelBin x_bin{SubpixelBin::Zero};
    SubpixelBin y_bin{SubpixelBin::Zero};
    uint8_t style_flags{0}; // bit 0: synthetic_bold, bit 1: synthetic_italic
    uint64_t variation_hash{0};

    bool operator==(const CacheKey& other) const noexcept {
        return font_id == other.font_id &&
               glyph_id == other.glyph_id &&
               font_size_bits == other.font_size_bits &&
               x_bin == other.x_bin &&
               y_bin == other.y_bin &&
               style_flags == other.style_flags &&
               variation_hash == other.variation_hash;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const noexcept {
        size_t h = std::hash<uint32_t>{}(k.font_id);
        h ^= (std::hash<uint32_t>{}(k.font_size_bits) + 0x9e3779b9 + (h << 6) + (h >> 2));
        h ^= (std::hash<uint16_t>{}(k.glyph_id) + 0x9e3779b9 + (h << 6) + (h >> 2));
        h ^= (static_cast<size_t>(k.style_flags) << 4) ^ (static_cast<size_t>(k.x_bin) << 2) ^ static_cast<size_t>(k.y_bin);
        if (k.variation_hash != 0) {
            h ^= (static_cast<size_t>(k.variation_hash) + 0x9e3779b9 + (h << 6) + (h >> 2));
        }
        return h;
    }
};

/// A cached 8-bit alpha mask for a glyph with positioning offsets.
struct CachedGlyph {
    int32_t offset_x{0};
    int32_t offset_y{0};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> data{};

    MaskRef as_mask_ref() const noexcept {
        auto m = MaskRef::from_bytes(data.data(), data.size(), width, height);
        return m.value_or(MaskRef{});
    }
};

/// High-performance bounded cache for rasterized glyph alpha masks.
class GlyphCache {
public:
    explicit GlyphCache(size_t max_entries = 4096) noexcept : max_entries_(max_entries) {}
    ~GlyphCache() = default;

    /// Retrieves or rasterizes an 8-bit alpha mask for the glyph at (x, y) subpixel position.
    const CachedGlyph* get_or_render(
        const TtfFont& font,
        uint32_t font_id,
        uint16_t glyph_id,
        float font_size,
        float x,
        float y,
        bool synthetic_bold = false,
        bool synthetic_italic = false,
        std::span<const FontVariation> variations = {},
        uint64_t variation_hash = 0
    );

    void clear() noexcept {
        cache_.clear();
        lru_order_.clear();
    }

    size_t size() const noexcept {
        return cache_.size();
    }

    size_t max_entries() const noexcept {
        return max_entries_;
    }

    void set_max_entries(size_t max_entries) noexcept {
        max_entries_ = std::max<size_t>(64, max_entries);
        evict_if_needed();
    }

private:
    void evict_if_needed() noexcept {
        while (cache_.size() > max_entries_ && !lru_order_.empty()) {
            CacheKey oldest = lru_order_.front();
            lru_order_.pop_front();
            cache_.erase(oldest);
        }
    }

    size_t max_entries_{4096};
    std::list<CacheKey> lru_order_{};
    std::unordered_map<CacheKey, CachedGlyph, CacheKeyHash> cache_{};
};

} // namespace nisaba::text
