#include "nisaba/text/glyph_cache.hpp"
#include "nisaba/raster/scan.hpp"
#include <algorithm>
#include <cmath>

namespace nisaba::text {

const CachedGlyph* GlyphCache::get_or_render(
    const TtfFont& font,
    uint32_t font_id,
    uint16_t glyph_id,
    float font_size,
    float x,
    float y,
    bool synthetic_bold,
    bool synthetic_italic,
    std::span<const FontVariation> variations,
    uint64_t variation_hash
) {
    auto [int_x, x_bin] = compute_subpixel_bin(x);
    auto [int_y, y_bin] = compute_subpixel_bin(y);

    uint32_t size_bits = 0;
    std::memcpy(&size_bits, &font_size, sizeof(float));

    uint8_t sflags = (synthetic_bold ? 1u : 0u) | (synthetic_italic ? 2u : 0u);
    CacheKey key{font_id, glyph_id, size_bits, x_bin, y_bin, sflags, variation_hash};

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return &it->second;
    }

    if (cache_.size() >= max_entries_) [[unlikely]] {
        evict_if_needed();
    }

    // Rasterize outline
    Path glyph_path;
    bool ok = false;
    if (!variations.empty() && font.is_variable_font()) {
        ok = font.get_glyph_path_varied(glyph_id, glyph_path, font_size, variations, synthetic_bold, synthetic_italic);
    } else {
        ok = (sflags == 0)
            ? font.get_glyph_path(glyph_id, glyph_path, font_size)
            : font.get_glyph_path_styled(glyph_id, glyph_path, font_size, synthetic_bold, synthetic_italic);
    }

    if (!ok || glyph_path.is_empty()) {
        CachedGlyph empty_glyph{0, 0, 0, 0, {}};
        lru_order_.push_back(key);
        auto [inserted_it, _] = cache_.emplace(key, std::move(empty_glyph));
        return &inserted_it->second;
    }

    Rect bounds = glyph_path.bounds();
    if (bounds.width() <= 0.0f || bounds.height() <= 0.0f) {
        CachedGlyph empty_glyph{0, 0, 0, 0, {}};
        lru_order_.push_back(key);
        auto [inserted_it, _] = cache_.emplace(key, std::move(empty_glyph));
        return &inserted_it->second;
    }

    float sub_x = bin_to_float(x_bin);
    float sub_y = bin_to_float(y_bin);

    float min_x = bounds.left() + sub_x;
    float min_y = bounds.top() + sub_y;
    float max_x = bounds.right() + sub_x;
    float max_y = bounds.bottom() + sub_y;

    int32_t i_left = static_cast<int32_t>(std::floor(min_x));
    int32_t i_top = static_cast<int32_t>(std::floor(min_y));
    int32_t i_right = static_cast<int32_t>(std::ceil(max_x));
    int32_t i_bottom = static_cast<int32_t>(std::ceil(max_y));

    uint32_t w = static_cast<uint32_t>(std::max(1, i_right - i_left + 1));
    uint32_t h = static_cast<uint32_t>(std::max(1, i_bottom - i_top + 1));

    // Pad by 1 pixel to prevent clipping antialiased borders
    float shift_x = sub_x - static_cast<float>(i_left);
    float shift_y = sub_y - static_cast<float>(i_top);

    auto trans_opt = glyph_path.transform(Transform::from_translate(shift_x, shift_y));
    if (!trans_opt) {
        CachedGlyph empty_glyph{0, 0, 0, 0, {}};
        lru_order_.push_back(key);
        auto [inserted_it, _] = cache_.emplace(key, std::move(empty_glyph));
        return &inserted_it->second;
    }

    auto mask_opt = Mask::allocate(w, h);
    if (!mask_opt) {
        CachedGlyph empty_glyph{0, 0, 0, 0, {}};
        lru_order_.push_back(key);
        auto [inserted_it, _] = cache_.emplace(key, std::move(empty_glyph));
        return &inserted_it->second;
    }

    Mask mask = std::move(*mask_opt);
    MaskBlitter blitter(mask.as_submask_mut());

    auto clip_opt = ScreenIntRect::from_xywh(0, 0, w, h);
    if (clip_opt) {
        scan::fill_path(*trans_opt, FillRule::Winding, *clip_opt, true, blitter);
    }

    CachedGlyph cached;
    cached.offset_x = i_left;
    cached.offset_y = i_top;
    cached.width = w;
    cached.height = h;
    cached.data = mask.take_data();

    lru_order_.push_back(key);
    auto [inserted_it, _] = cache_.emplace(key, std::move(cached));
    return &inserted_it->second;
}

} // namespace nisaba::text
