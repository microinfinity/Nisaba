#include "nisaba/text/paragraph_cache.hpp"

#include <cstring>

namespace nisaba::text {

namespace {

inline uint32_t float_to_bits(float v) noexcept {
    if (v == 0.0f) return 0u; // normalize -0.0f and +0.0f
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline void hash_combine(uint64_t& h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

inline void hash_string(uint64_t& h, std::string_view s) noexcept {
    uint64_t fnv = 14695981039346656037ULL;
    for (char c : s) {
        fnv ^= static_cast<uint8_t>(c);
        fnv *= 1099511628211ULL;
    }
    hash_combine(h, fnv);
}

inline void hash_family(uint64_t& h, const Family& f) noexcept {
    hash_combine(h, static_cast<uint64_t>(f.kind));
    if (f.kind == Family::Kind::Name) {
        hash_string(h, f.name);
    }
}

inline bool paints_equal(const std::shared_ptr<const Paint>& a, const std::shared_ptr<const Paint>& b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->is_solid_color() && b->is_solid_color() &&
        !a->has_color_filter() && !b->has_color_filter() &&
        !a->has_image_filter() && !b->has_image_filter()) {
        const Color ca = a->shader.solid_color();
        const Color cb = b->shader.solid_color();
        return ca == cb &&
               a->blend_mode == b->blend_mode &&
               a->style == b->style &&
               a->stroke_width == b->stroke_width &&
               a->anti_alias == b->anti_alias;
    }
    return false;
}

inline bool strokes_equal(const std::shared_ptr<const Stroke>& a, const std::shared_ptr<const Stroke>& b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->width != b->width || a->miter_limit != b->miter_limit ||
        a->line_cap != b->line_cap || a->line_join != b->line_join ||
        a->dash.has_value() != b->dash.has_value()) {
        return false;
    }
    if (a->dash.has_value()) {
        return *a->dash == *b->dash;
    }
    return true;
}

inline bool attrs_equal(const Attrs& a, const Attrs& b) noexcept {
    return a.family == b.family &&
           a.font_families == b.font_families &&
           a.locale == b.locale &&
           a.weight == b.weight &&
           a.style == b.style &&
           a.stretch == b.stretch &&
           a.font_features == b.font_features &&
           a.font_variations == b.font_variations &&
           a.color_opt == b.color_opt &&
           a.background_color_opt == b.background_color_opt &&
           a.font_size_opt == b.font_size_opt &&
           a.letter_spacing == b.letter_spacing &&
           a.word_spacing == b.word_spacing &&
           a.baseline_shift == b.baseline_shift &&
           a.underline == b.underline &&
           a.strikethrough == b.strikethrough &&
           a.overline == b.overline &&
           a.decoration_style == b.decoration_style &&
           a.decoration_mode == b.decoration_mode &&
           a.decoration_color_opt == b.decoration_color_opt &&
           a.decoration_thickness == b.decoration_thickness &&
           paints_equal(a.foreground_paint_opt, b.foreground_paint_opt) &&
           paints_equal(a.background_paint_opt, b.background_paint_opt) &&
           strokes_equal(a.foreground_stroke_opt, b.foreground_stroke_opt) &&
           a.shadows == b.shadows &&
           a.metadata == b.metadata;
}

inline void hash_paint(uint64_t& h, const std::shared_ptr<const Paint>& p) noexcept {
    if (!p) {
        hash_combine(h, 0ULL);
        return;
    }
    if (p->is_solid_color() && !p->has_color_filter() && !p->has_image_filter()) {
        const Color c = p->shader.solid_color();
        hash_combine(h, 1ULL);
        hash_combine(h, float_to_bits(c.red()));
        hash_combine(h, float_to_bits(c.green()));
        hash_combine(h, float_to_bits(c.blue()));
        hash_combine(h, float_to_bits(c.alpha()));
        hash_combine(h, static_cast<uint64_t>(p->blend_mode));
        hash_combine(h, static_cast<uint64_t>(p->style));
        hash_combine(h, float_to_bits(p->stroke_width));
        hash_combine(h, p->anti_alias ? 1ULL : 0ULL);
    } else {
        hash_combine(h, 2ULL);
        hash_combine(h, reinterpret_cast<uintptr_t>(p.get()));
    }
}

inline void hash_stroke(uint64_t& h, const std::shared_ptr<const Stroke>& s) noexcept {
    if (!s) {
        hash_combine(h, 0ULL);
        return;
    }
    hash_combine(h, 1ULL);
    hash_combine(h, float_to_bits(s->width));
    hash_combine(h, float_to_bits(s->miter_limit));
    hash_combine(h, static_cast<uint64_t>(s->line_cap));
    hash_combine(h, static_cast<uint64_t>(s->line_join));
    if (s->dash.has_value()) {
        hash_combine(h, 1ULL);
        hash_combine(h, float_to_bits(s->dash->offset()));
        for (float iv : s->dash->array()) {
            hash_combine(h, float_to_bits(iv));
        }
    }
}

inline void hash_attrs(uint64_t& h, const Attrs& a) noexcept {
    hash_family(h, a.family);
    hash_combine(h, a.font_families.size());
    for (const auto& fam : a.font_families) {
        hash_family(h, fam);
    }
    hash_string(h, a.locale);
    hash_combine(h, static_cast<uint64_t>(a.weight));
    hash_combine(h, static_cast<uint64_t>(a.style));
    hash_combine(h, static_cast<uint64_t>(a.stretch));
    hash_combine(h, a.font_features.size());
    for (const auto& f : a.font_features) {
        hash_combine(h, (static_cast<uint64_t>(f.tag) << 32) | f.value);
    }
    hash_combine(h, hash_font_variations(a.font_variations));
    hash_combine(h, a.color_opt.has_value() ? (1ULL | (static_cast<uint64_t>(a.color_opt->to_u32()) << 1)) : 0ULL);
    hash_combine(h, a.background_color_opt.has_value() ? (1ULL | (static_cast<uint64_t>(a.background_color_opt->to_u32()) << 1)) : 0ULL);
    hash_combine(h, a.font_size_opt.has_value() ? (1ULL | (static_cast<uint64_t>(float_to_bits(*a.font_size_opt)) << 1)) : 0ULL);
    hash_combine(h, float_to_bits(a.letter_spacing));
    hash_combine(h, float_to_bits(a.word_spacing));
    hash_combine(h, float_to_bits(a.baseline_shift));
    const uint64_t dec_bits =
        (a.underline ? 1ULL : 0ULL) |
        (a.strikethrough ? 2ULL : 0ULL) |
        (a.overline ? 4ULL : 0ULL) |
        (static_cast<uint64_t>(a.decoration_style) << 3) |
        (static_cast<uint64_t>(a.decoration_mode) << 7);
    hash_combine(h, dec_bits);
    hash_combine(h, a.decoration_color_opt.has_value() ? (1ULL | (static_cast<uint64_t>(a.decoration_color_opt->to_u32()) << 1)) : 0ULL);
    hash_combine(h, float_to_bits(a.decoration_thickness));
    hash_paint(h, a.foreground_paint_opt);
    hash_paint(h, a.background_paint_opt);
    hash_stroke(h, a.foreground_stroke_opt);
    hash_combine(h, a.shadows.size());
    for (const auto& sh : a.shadows) {
        hash_combine(h, sh.color.to_u32());
        hash_combine(h, float_to_bits(sh.offset_x));
        hash_combine(h, float_to_bits(sh.offset_y));
        hash_combine(h, float_to_bits(sh.blur_sigma));
    }
    hash_combine(h, a.metadata);
}

ParagraphCacheKey build_key_from_buffer(const Buffer& buffer, size_t font_count) {
    ParagraphCacheKey key;
    key.metrics = buffer.metrics();
    key.width = buffer.width();
    key.height = buffer.height();
    key.wrap = buffer.wrap();
    key.align = buffer.align();
    key.shaping = buffer.shaping();
    key.overflow = buffer.overflow();
    key.text_direction = buffer.text_direction();
    key.strut_style = buffer.strut_style();
    key.half_leading = buffer.half_leading();
    key.text_height_behavior = buffer.text_height_behavior();
    if (buffer.has_custom_ellipsis()) {
        key.ellipsis = std::string(buffer.ellipsis());
    }
    key.max_lines = buffer.max_lines();
    key.enable_kerning = buffer.kerning_enabled();
    key.enable_ligatures = buffer.ligatures_enabled();
    key.font_count = font_count;

    const auto& lines = buffer.lines();
    if (!lines.empty()) {
        key.attrs = lines.front().attrs_list().defaults();
    }
    size_t line_base = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            key.text.push_back('\n');
            ++line_base;
        }
        const auto& bl = lines[i];
        const size_t len = bl.text().size();
        key.text.append(bl.text());
        if (i > 0 && !attrs_equal(bl.attrs_list().defaults(), key.attrs) && len > 0) {
            key.spans.push_back(AttrsSpan{line_base, line_base + len, bl.attrs_list().defaults()});
        }
        for (const auto& sp : bl.attrs_list().spans()) {
            key.spans.push_back(AttrsSpan{line_base + sp.start, line_base + sp.end, sp.attrs});
        }
        for (const auto& ph : bl.placeholders()) {
            key.placeholders.emplace_back(line_base + ph.byte_offset, ph.style);
        }
        line_base += len;
    }
    return key;
}

} // namespace

bool ParagraphCacheKey::operator==(const ParagraphCacheKey& other) const noexcept {
    if (font_count != other.font_count ||
        metrics != other.metrics ||
        width != other.width ||
        height != other.height ||
        wrap != other.wrap ||
        align != other.align ||
        shaping != other.shaping ||
        overflow != other.overflow ||
        text_direction != other.text_direction ||
        strut_style != other.strut_style ||
        half_leading != other.half_leading ||
        text_height_behavior != other.text_height_behavior ||
        ellipsis != other.ellipsis ||
        max_lines != other.max_lines ||
        enable_kerning != other.enable_kerning ||
        enable_ligatures != other.enable_ligatures ||
        placeholders != other.placeholders ||
        spans.size() != other.spans.size() ||
        text != other.text ||
        !attrs_equal(attrs, other.attrs)) {
        return false;
    }
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].start != other.spans[i].start ||
            spans[i].end != other.spans[i].end ||
            !attrs_equal(spans[i].attrs, other.spans[i].attrs)) {
            return false;
        }
    }
    return true;
}

size_t ParagraphCacheKeyHash::operator()(const ParagraphCacheKey& k) const noexcept {
    uint64_t h = 14695981039346656037ULL;
    hash_string(h, k.text);
    hash_attrs(h, k.attrs);
    hash_combine(h, k.spans.size());
    for (const auto& sp : k.spans) {
        hash_combine(h, sp.start);
        hash_combine(h, sp.end);
        hash_attrs(h, sp.attrs);
    }
    hash_combine(h, k.placeholders.size());
    for (const auto& [off, ps] : k.placeholders) {
        hash_combine(h, off);
        hash_combine(h, float_to_bits(ps.width));
        hash_combine(h, float_to_bits(ps.height));
        hash_combine(h, static_cast<uint64_t>(ps.alignment));
        hash_combine(h, static_cast<uint64_t>(ps.baseline));
        hash_combine(h, float_to_bits(ps.baseline_offset));
    }
    hash_combine(h, float_to_bits(k.metrics.font_size));
    hash_combine(h, float_to_bits(k.metrics.line_height));
    hash_combine(h, k.width.has_value() ? (1ULL | (static_cast<uint64_t>(float_to_bits(*k.width)) << 1)) : 0ULL);
    hash_combine(h, k.height.has_value() ? (1ULL | (static_cast<uint64_t>(float_to_bits(*k.height)) << 1)) : 0ULL);
    const uint64_t flags =
        static_cast<uint64_t>(k.wrap) |
        (static_cast<uint64_t>(k.align) << 4) |
        (static_cast<uint64_t>(k.shaping) << 8) |
        (static_cast<uint64_t>(k.overflow) << 12) |
        (static_cast<uint64_t>(k.text_direction) << 16) |
        (k.half_leading ? (1ULL << 20) : 0ULL) |
        (k.text_height_behavior.apply_height_to_first_ascent ? (1ULL << 21) : 0ULL) |
        (k.text_height_behavior.apply_height_to_last_descent ? (1ULL << 22) : 0ULL) |
        (k.enable_kerning ? (1ULL << 23) : 0ULL) |
        (k.enable_ligatures ? (1ULL << 24) : 0ULL);
    hash_combine(h, flags);
    if (k.strut_style.enabled) {
        hash_combine(h, 1ULL);
        for (const auto& f : k.strut_style.font_families) hash_family(h, f);
        hash_combine(h, static_cast<uint64_t>(k.strut_style.weight));
        hash_combine(h, static_cast<uint64_t>(k.strut_style.style));
        hash_combine(h, k.strut_style.font_size.has_value() ? (1ULL | (static_cast<uint64_t>(float_to_bits(*k.strut_style.font_size)) << 1)) : 0ULL);
        hash_combine(h, float_to_bits(k.strut_style.height_multiplier));
        hash_combine(h, float_to_bits(k.strut_style.leading));
        hash_combine(h, (k.strut_style.force_strut_height ? 1ULL : 0ULL) | (k.strut_style.half_leading ? 2ULL : 0ULL));
    }
    if (k.ellipsis.has_value()) {
        hash_combine(h, 1ULL);
        hash_string(h, *k.ellipsis);
    }
    if (k.max_lines.has_value()) {
        hash_combine(h, 1ULL);
        hash_combine(h, *k.max_lines);
    }
    hash_combine(h, k.font_count);
    return static_cast<size_t>(h);
}

ParagraphCache::ParagraphCache(size_t max_entries) noexcept
    : max_entries_(max_entries > 0 ? max_entries : 1) {}

void ParagraphCache::clear() noexcept {
    map_.clear();
    lru_list_.clear();
    hits_ = 0;
    misses_ = 0;
}

void ParagraphCache::set_max_entries(size_t max_entries) {
    max_entries_ = max_entries > 0 ? max_entries : 1;
    evict_if_needed();
}

void ParagraphCache::evict_if_needed() {
    while (map_.size() > max_entries_ && !lru_list_.empty()) {
        const ParagraphCacheKey& oldest = lru_list_.back();
        map_.erase(oldest);
        lru_list_.pop_back();
    }
}

std::shared_ptr<Buffer> ParagraphCache::get_or_layout(
    FontSystem& font_system,
    std::string_view text,
    const Attrs& attrs,
    Metrics metrics,
    std::optional<float> width,
    Wrap wrap,
    Align align,
    Overflow overflow,
    std::optional<size_t> max_lines,
    TextDirection text_direction
) {
    ParagraphCacheKey key;
    key.text = std::string(text);
    key.attrs = attrs;
    key.metrics = metrics;
    key.width = width;
    key.wrap = wrap;
    key.align = align;
    key.overflow = overflow;
    key.max_lines = max_lines;
    key.text_direction = text_direction;
    return get_or_layout(font_system, key);
}

std::shared_ptr<Buffer> ParagraphCache::get_or_layout(
    FontSystem& font_system,
    const ParagraphCacheKey& input_key
) {
    ParagraphCacheKey key = input_key;
    key.font_count = font_system.font_count();

    auto it = map_.find(key);
    if (it != map_.end()) {
        ++hits_;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
        return it->second.buffer;
    }

    ++misses_;
    auto buf = std::make_shared<Buffer>(key.metrics);
    buf->set_size(key.width, key.height);
    buf->set_wrap(key.wrap);
    buf->set_align(key.align);
    buf->set_shaping(key.shaping);
    buf->set_overflow(key.overflow);
    buf->set_text_direction(key.text_direction);
    buf->set_strut_style(key.strut_style);
    buf->set_half_leading(key.half_leading);
    buf->set_text_height_behavior(key.text_height_behavior);
    if (key.ellipsis.has_value()) {
        buf->set_ellipsis(*key.ellipsis);
    }
    buf->set_max_lines(key.max_lines);
    buf->set_kerning_enabled(key.enable_kerning);
    buf->set_ligatures_enabled(key.enable_ligatures);
    buf->set_text(key.text, key.attrs);

    // Apply any flattened extra spans onto corresponding BufferLines
    if (!key.spans.empty()) {
        size_t line_base = 0;
        for (auto& bl : buf->lines_mut()) {
            const size_t line_len = bl.text().size();
            const size_t line_end = line_base + line_len;
            for (const auto& sp : key.spans) {
                if (sp.end > line_base && sp.start < line_end) {
                    const size_t ls = (sp.start > line_base) ? (sp.start - line_base) : 0;
                    const size_t le = std::min(line_len, sp.end - line_base);
                    if (ls < le) {
                        bl.attrs_list_mut().add_span(ls, le, sp.attrs);
                    }
                }
            }
            line_base += line_len + 1;
        }
    }

    buf->shape_until_scroll(font_system);

    lru_list_.push_front(key);
    map_.emplace(std::move(key), Entry{buf, lru_list_.begin()});
    evict_if_needed();
    return buf;
}

bool ParagraphCache::lookup_or_shape(FontSystem& font_system, Buffer& buffer) {
    ParagraphCacheKey key = build_key_from_buffer(buffer, font_system.font_count());
    auto it = map_.find(key);
    if (it != map_.end()) {
        ++hits_;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
        buffer.copy_shaped_layout_from(*it->second.buffer);
        return true;
    }

    ++misses_;
    buffer.shape_until_scroll(font_system);
    auto snapshot = std::make_shared<Buffer>(buffer);
    lru_list_.push_front(key);
    map_.emplace(std::move(key), Entry{std::move(snapshot), lru_list_.begin()});
    evict_if_needed();
    return false;
}

} // namespace nisaba::text

