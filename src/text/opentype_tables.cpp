#include "nisaba/text/opentype_tables.hpp"
#include <algorithm>
#include <cstring>
#include <bit>

namespace nisaba::text {

namespace {

inline constexpr uint32_t make_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
            static_cast<uint32_t>(static_cast<uint8_t>(d));
}

inline uint16_t read_u16_be(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline int16_t read_i16_be(const uint8_t* p) noexcept {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t read_u32_be(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

class SpanReader {
public:
    explicit SpanReader(std::span<const uint8_t> data) noexcept : data_(data), pos_(0) {}

    [[nodiscard]] bool can_read(size_t n) const noexcept {
        return pos_ + n <= data_.size();
    }

    [[nodiscard]] size_t tell() const noexcept { return pos_; }
    void seek(size_t p) noexcept { pos_ = std::min(p, data_.size()); }
    void skip(size_t n) noexcept { seek(pos_ + n); }

    uint8_t read_u8() noexcept {
        return (pos_ < data_.size()) ? data_[pos_++] : 0;
    }

    uint16_t read_u16() noexcept {
        if (pos_ + 2 <= data_.size()) {
            uint16_t v = read_u16_be(data_.data() + pos_);
            pos_ += 2;
            return v;
        }
        pos_ = data_.size();
        return 0;
    }

    int16_t read_i16() noexcept {
        return static_cast<int16_t>(read_u16());
    }

    uint32_t read_u32() noexcept {
        if (pos_ + 4 <= data_.size()) {
            uint32_t v = read_u32_be(data_.data() + pos_);
            pos_ += 4;
            return v;
        }
        pos_ = data_.size();
        return 0;
    }

    std::span<const uint8_t> subspan(size_t offset) const noexcept {
        return (offset < data_.size()) ? data_.subspan(offset) : std::span<const uint8_t>{};
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_;
};

/// Fast O(log N) binary search across OpenType Coverage Format 1 & Format 2 tables.
int32_t read_coverage_index(std::span<const uint8_t> table_data, size_t cov_offset, uint16_t glyph_id) noexcept {
    if (cov_offset + 4 > table_data.size()) return -1;
    const uint8_t* base = table_data.data() + cov_offset;
    size_t max_len = table_data.size() - cov_offset;
    uint16_t format = read_u16_be(base);
    uint16_t count = read_u16_be(base + 2);

    if (format == 1) {
        if (4 + static_cast<size_t>(count) * 2 > max_len) return -1;
        const uint8_t* arr = base + 4;
        int left = 0;
        int right = static_cast<int>(count) - 1;
        while (left <= right) {
            int mid = (left + right) >> 1;
            uint16_t g = read_u16_be(arr + mid * 2);
            if (g == glyph_id) return mid;
            if (g < glyph_id) left = mid + 1;
            else right = mid - 1;
        }
        return -1;
    } else if (format == 2) {
        if (4 + static_cast<size_t>(count) * 6 > max_len) return -1;
        const uint8_t* arr = base + 4;
        int left = 0;
        int right = static_cast<int>(count) - 1;
        while (left <= right) {
            int mid = (left + right) >> 1;
            const uint8_t* rec = arr + mid * 6;
            uint16_t start = read_u16_be(rec);
            if (glyph_id < start) {
                right = mid - 1;
            } else {
                uint16_t end = read_u16_be(rec + 2);
                if (glyph_id > end) {
                    left = mid + 1;
                } else {
                    uint16_t start_cov = read_u16_be(rec + 4);
                    return static_cast<int32_t>(start_cov + (glyph_id - start));
                }
            }
        }
        return -1;
    }
    return -1;
}

/// Fast O(log N) / O(1) lookup across OpenType ClassDef Format 1 & Format 2 tables.
uint16_t read_class_def(std::span<const uint8_t> table_data, size_t class_def_offset, uint16_t glyph_id) noexcept {
    if (class_def_offset + 4 > table_data.size()) return 0;
    const uint8_t* base = table_data.data() + class_def_offset;
    size_t max_len = table_data.size() - class_def_offset;
    uint16_t format = read_u16_be(base);

    if (format == 1) {
        if (6 > max_len) return 0;
        uint16_t start_glyph = read_u16_be(base + 2);
        uint16_t count = read_u16_be(base + 4);
        if (glyph_id >= start_glyph && glyph_id < start_glyph + count) {
            size_t off = 6 + static_cast<size_t>(glyph_id - start_glyph) * 2;
            if (off + 2 <= max_len) {
                return read_u16_be(base + off);
            }
        }
        return 0;
    } else if (format == 2) {
        uint16_t range_count = read_u16_be(base + 2);
        if (4 + static_cast<size_t>(range_count) * 6 > max_len) return 0;
        const uint8_t* arr = base + 4;
        int left = 0;
        int right = static_cast<int>(range_count) - 1;
        while (left <= right) {
            int mid = (left + right) >> 1;
            const uint8_t* rec = arr + mid * 6;
            uint16_t start = read_u16_be(rec);
            if (glyph_id < start) {
                right = mid - 1;
            } else {
                uint16_t end = read_u16_be(rec + 2);
                if (glyph_id > end) {
                    left = mid + 1;
                } else {
                    return read_u16_be(rec + 4);
                }
            }
        }
        return 0;
    }
    return 0;
}

inline size_t get_value_record_size(uint16_t format) noexcept {
    return static_cast<size_t>(std::popcount(static_cast<uint16_t>(format & 0x00FFu))) * 2;
}

inline void parse_value_record_ptr(const uint8_t*& p, uint16_t format, GlyphPlacementAdjustment& adj) noexcept {
    if (format & 0x0001) { adj.x_placement = read_i16_be(p); p += 2; }
    if (format & 0x0002) { adj.y_placement = read_i16_be(p); p += 2; }
    if (format & 0x0004) { adj.x_advance = read_i16_be(p); p += 2; }
    if (format & 0x0008) { adj.y_advance = read_i16_be(p); p += 2; }
    if (format & 0x0010) { p += 2; }
    if (format & 0x0020) { p += 2; }
    if (format & 0x0040) { p += 2; }
    if (format & 0x0080) { p += 2; }
}

} // namespace

// ============================================================================
// OpenType GDEF Implementation
// ============================================================================

OpenTypeGdef::OpenTypeGdef() = default;
OpenTypeGdef::~OpenTypeGdef() = default;
OpenTypeGdef::OpenTypeGdef(OpenTypeGdef&&) noexcept = default;
OpenTypeGdef& OpenTypeGdef::operator=(OpenTypeGdef&&) noexcept = default;

std::unique_ptr<OpenTypeGdef> OpenTypeGdef::from_bytes(std::span<const uint8_t> data) {
    if (data.size() < 12) return nullptr;
    uint16_t major = read_u16_be(data.data());
    if (major != 1) return nullptr;

    auto gdef = std::make_unique<OpenTypeGdef>();
    gdef->data_ = data;
    gdef->glyph_class_def_offset_ = read_u16_be(data.data() + 4);
    gdef->mark_attach_class_def_offset_ = read_u16_be(data.data() + 10);
    gdef->is_valid_ = true;
    return gdef;
}

GlyphClass OpenTypeGdef::get_glyph_class(uint16_t glyph_id) const noexcept {
    if (!is_valid_ || glyph_class_def_offset_ == 0) return GlyphClass::Unclassified;
    uint16_t cls = read_class_def(data_, glyph_class_def_offset_, glyph_id);
    if (cls >= 1 && cls <= 4) {
        return static_cast<GlyphClass>(cls);
    }
    return GlyphClass::Unclassified;
}

// ============================================================================
// OpenType GSUB Implementation
// ============================================================================

struct OpenTypeGsub::Impl {
    std::span<const uint8_t> data{};
    const OpenTypeGdef* gdef{nullptr};
    uint16_t script_list_offset{0};
    uint16_t feature_list_offset{0};
    uint16_t lookup_list_offset{0};

    bool parse(std::span<const uint8_t> d, const OpenTypeGdef* g) noexcept {
        data = d;
        gdef = g;
        if (data.size() < 10) return false;
        uint16_t major = read_u16_be(data.data());
        if (major != 1) return false;
        script_list_offset = read_u16_be(data.data() + 4);
        feature_list_offset = read_u16_be(data.data() + 6);
        lookup_list_offset = read_u16_be(data.data() + 8);
        return (feature_list_offset < data.size() && lookup_list_offset < data.size());
    }

    bool should_skip_glyph(uint16_t glyph_id, uint16_t lookup_flag) const noexcept {
        if (!gdef || (lookup_flag & 0x000E) == 0) return false;
        GlyphClass gc = gdef->get_glyph_class(glyph_id);
        if ((lookup_flag & 0x0002) && gc == GlyphClass::Base) return true;
        if ((lookup_flag & 0x0004) && gc == GlyphClass::Ligature) return true;
        if ((lookup_flag & 0x0008) && gc == GlyphClass::Mark) return true;
        return false;
    }

    std::vector<uint16_t> get_lookups_for_features(std::span<const uint32_t> requested_tags) const {
        std::vector<uint16_t> lookup_indices;
        if (feature_list_offset + 2u > data.size()) return lookup_indices;

        SpanReader fr(data.subspan(feature_list_offset));
        uint16_t feature_count = fr.read_u16();

        for (uint16_t i = 0; i < feature_count; ++i) {
            if (!fr.can_read(6)) break;
            uint32_t tag = fr.read_u32();
            uint16_t feat_offset = fr.read_u16();

            bool match = false;
            for (uint32_t req : requested_tags) {
                if (req == tag) { match = true; break; }
            }

            if (match && feature_list_offset + feat_offset < data.size()) {
                SpanReader tr(data.subspan(feature_list_offset + feat_offset));
                tr.skip(2); // featureParams
                uint16_t count = tr.read_u16();
                for (uint16_t k = 0; k < count; ++k) {
                    if (!tr.can_read(2)) break;
                    uint16_t l_idx = tr.read_u16();
                    if (std::find(lookup_indices.begin(), lookup_indices.end(), l_idx) == lookup_indices.end()) {
                        lookup_indices.push_back(l_idx);
                    }
                }
            }
        }
        std::sort(lookup_indices.begin(), lookup_indices.end());
        return lookup_indices;
    }

    uint16_t apply_single_on_glyph(uint16_t lookup_idx, uint16_t glyph_id) const noexcept {
        if (lookup_list_offset + 2u > data.size()) return glyph_id;
        uint16_t total_lookups = read_u16_be(data.data() + lookup_list_offset);
        if (lookup_idx >= total_lookups) return glyph_id;
        if (lookup_list_offset + 2 + static_cast<size_t>(lookup_idx) * 2 + 2 > data.size()) return glyph_id;

        uint16_t l_offset = read_u16_be(data.data() + lookup_list_offset + 2 + lookup_idx * 2);
        size_t abs_lookup_offset = lookup_list_offset + l_offset;
        if (abs_lookup_offset + 6 > data.size()) return glyph_id;

        uint16_t lookup_type = read_u16_be(data.data() + abs_lookup_offset);
        uint16_t subtable_count = read_u16_be(data.data() + abs_lookup_offset + 4);

        for (uint16_t s = 0; s < subtable_count; ++s) {
            if (abs_lookup_offset + 6 + static_cast<size_t>(s) * 2 + 2 > data.size()) break;
            uint16_t sub_offset = read_u16_be(data.data() + abs_lookup_offset + 6 + s * 2);
            size_t abs_sub_offset = abs_lookup_offset + sub_offset;
            if (abs_sub_offset + 6 > data.size()) continue;

            auto sub_span = data.subspan(abs_sub_offset);
            uint16_t effective_type = lookup_type;
            if (lookup_type == 7) {
                if (sub_span.size() < 8) continue;
                uint16_t ext_fmt = read_u16_be(sub_span.data());
                effective_type = read_u16_be(sub_span.data() + 2);
                uint32_t ext_offset = read_u32_be(sub_span.data() + 4);
                if (ext_fmt != 1 || ext_offset >= sub_span.size()) continue;
                sub_span = sub_span.subspan(ext_offset);
            }

            if (effective_type == 1 && sub_span.size() >= 6) {
                uint16_t subst_format = read_u16_be(sub_span.data());
                uint16_t cov_offset = read_u16_be(sub_span.data() + 2);
                int32_t cov = read_coverage_index(sub_span, cov_offset, glyph_id);
                if (cov < 0) continue;

                if (subst_format == 1) {
                    int16_t delta = read_i16_be(sub_span.data() + 4);
                    return static_cast<uint16_t>((static_cast<int32_t>(glyph_id) + delta) & 0xFFFF);
                } else if (subst_format == 2) {
                    uint16_t glyph_count = read_u16_be(sub_span.data() + 4);
                    if (static_cast<uint16_t>(cov) < glyph_count && 6 + static_cast<size_t>(cov) * 2 + 2 <= sub_span.size()) {
                        return read_u16_be(sub_span.data() + 6 + cov * 2);
                    }
                }
            }
        }
        return glyph_id;
    }

    void apply_lookup(uint16_t lookup_idx, std::vector<uint16_t>& glyphs, int depth = 0) const {
        if (depth > 4 || lookup_list_offset >= data.size()) return;
        SpanReader lr(data.subspan(lookup_list_offset));
        uint16_t total_lookups = lr.read_u16();
        if (lookup_idx >= total_lookups) return;

        lr.skip(lookup_idx * 2);
        if (!lr.can_read(2)) return;
        uint16_t l_offset = lr.read_u16();
        size_t abs_lookup_offset = lookup_list_offset + l_offset;
        if (abs_lookup_offset >= data.size()) return;

        SpanReader tr(data.subspan(abs_lookup_offset));
        uint16_t lookup_type = tr.read_u16();
        uint16_t lookup_flag = tr.read_u16();
        uint16_t subtable_count = tr.read_u16();

        for (uint16_t s = 0; s < subtable_count; ++s) {
            if (!tr.can_read(2)) break;
            uint16_t sub_offset = tr.read_u16();
            size_t abs_sub_offset = abs_lookup_offset + sub_offset;
            if (abs_sub_offset >= data.size()) continue;

            auto sub_span = data.subspan(abs_sub_offset);
            SpanReader sr(sub_span);
            uint16_t effective_type = lookup_type;
            if (lookup_type == 7) { // ExtensionSubst
                uint16_t ext_fmt = sr.read_u16();
                effective_type = sr.read_u16();
                uint32_t ext_offset = sr.read_u32();
                if (ext_fmt != 1 || ext_offset >= sub_span.size()) continue;
                sub_span = sub_span.subspan(ext_offset);
                sr = SpanReader(sub_span);
            }

            if (effective_type == 1) { // Single Substitution
                uint16_t subst_format = sr.read_u16();
                uint16_t cov_offset = sr.read_u16();
                if (subst_format == 1) {
                    int16_t delta = sr.read_i16();
                    for (auto& g : glyphs) {
                        if (should_skip_glyph(g, lookup_flag)) continue;
                        int32_t cov = read_coverage_index(sub_span, cov_offset, g);
                        if (cov >= 0) {
                            g = static_cast<uint16_t>((static_cast<int32_t>(g) + delta) & 0xFFFF);
                        }
                    }
                } else if (subst_format == 2) {
                    uint16_t glyph_count = sr.read_u16();
                    for (auto& g : glyphs) {
                        if (should_skip_glyph(g, lookup_flag)) continue;
                        int32_t cov = read_coverage_index(sub_span, cov_offset, g);
                        if (cov >= 0 && static_cast<uint16_t>(cov) < glyph_count) {
                            SpanReader gr(sub_span.subspan(6 + cov * 2));
                            g = gr.read_u16();
                        }
                    }
                }
            } else if (effective_type == 2) { // Multiple Substitution (1 -> N glyphs)
                uint16_t subst_format = sr.read_u16();
                if (subst_format != 1) continue;
                uint16_t cov_offset = sr.read_u16();
                uint16_t seq_count = sr.read_u16();

                for (size_t i = 0; i < glyphs.size(); ++i) {
                    if (should_skip_glyph(glyphs[i], lookup_flag)) continue;
                    int32_t cov = read_coverage_index(sub_span, cov_offset, glyphs[i]);
                    if (cov < 0 || static_cast<uint16_t>(cov) >= seq_count) continue;

                    SpanReader seq_off_r(sub_span.subspan(6 + cov * 2));
                    uint16_t seq_offset = seq_off_r.read_u16();
                    if (seq_offset >= sub_span.size()) continue;

                    SpanReader seq_r(sub_span.subspan(seq_offset));
                    uint16_t glyph_count = seq_r.read_u16();
                    if (glyph_count == 0 || !seq_r.can_read(glyph_count * 2)) continue;

                    glyphs[i] = seq_r.read_u16();
                    for (uint16_t m = 1; m < glyph_count; ++m) {
                        glyphs.insert(glyphs.begin() + static_cast<ptrdiff_t>(i + m), seq_r.read_u16());
                    }
                    i += glyph_count - 1;
                }
            } else if (effective_type == 3) { // Alternate Substitution (use first alternate)
                uint16_t subst_format = sr.read_u16();
                if (subst_format != 1) continue;
                uint16_t cov_offset = sr.read_u16();
                uint16_t alt_set_count = sr.read_u16();

                for (auto& g : glyphs) {
                    if (should_skip_glyph(g, lookup_flag)) continue;
                    int32_t cov = read_coverage_index(sub_span, cov_offset, g);
                    if (cov < 0 || static_cast<uint16_t>(cov) >= alt_set_count) continue;

                    SpanReader alt_off_r(sub_span.subspan(6 + cov * 2));
                    uint16_t alt_offset = alt_off_r.read_u16();
                    if (alt_offset + 4u <= sub_span.size()) {
                        uint16_t alt_count = read_u16_be(sub_span.data() + alt_offset);
                        if (alt_count > 0) {
                            g = read_u16_be(sub_span.data() + alt_offset + 2);
                        }
                    }
                }
            } else if (effective_type == 4) { // Ligature Substitution
                uint16_t subst_format = sr.read_u16();
                if (subst_format != 1) continue;
                uint16_t cov_offset = sr.read_u16();
                uint16_t lig_set_count = sr.read_u16();

                for (size_t i = 0; i < glyphs.size(); ) {
                    if (should_skip_glyph(glyphs[i], lookup_flag)) {
                        ++i;
                        continue;
                    }
                    int32_t cov = read_coverage_index(sub_span, cov_offset, glyphs[i]);
                    if (cov < 0 || static_cast<uint16_t>(cov) >= lig_set_count) {
                        ++i;
                        continue;
                    }

                    SpanReader lsr(sub_span.subspan(6 + cov * 2));
                    uint16_t lig_set_offset = lsr.read_u16();
                    if (lig_set_offset >= sub_span.size()) {
                        ++i;
                        continue;
                    }

                    auto lig_set_span = sub_span.subspan(lig_set_offset);
                    SpanReader set_r(lig_set_span);
                    uint16_t lig_count = set_r.read_u16();
                    bool substituted = false;

                    for (uint16_t k = 0; k < lig_count; ++k) {
                        if (!set_r.can_read(2)) break;
                        uint16_t lig_offset = set_r.read_u16();
                        if (lig_offset >= lig_set_span.size()) continue;

                        SpanReader lig_r(lig_set_span.subspan(lig_offset));
                        uint16_t lig_glyph = lig_r.read_u16();
                        uint16_t comp_count = lig_r.read_u16();

                        if (comp_count < 2 || (i + comp_count) > glyphs.size()) {
                            continue;
                        }

                        // Match components, skipping marks if IgnoreMarks is set in lookup_flag
                        size_t matched_indices[16];
                        matched_indices[0] = i;
                        bool match = true;
                        size_t scan_idx = i + 1;

                        for (uint16_t c = 1; c < comp_count && c < 16; ++c) {
                            while (scan_idx < glyphs.size() && should_skip_glyph(glyphs[scan_idx], lookup_flag)) {
                                ++scan_idx;
                            }
                            if (scan_idx >= glyphs.size()) {
                                match = false;
                                break;
                            }
                            uint16_t expected_comp = lig_r.read_u16();
                            if (glyphs[scan_idx] != expected_comp) {
                                match = false;
                                break;
                            }
                            matched_indices[c] = scan_idx;
                            ++scan_idx;
                        }

                        if (match && comp_count <= 16) {
                            glyphs[i] = lig_glyph;
                            // Erase matched component glyphs in reverse order so any intervening marks remain intact
                            for (int c = static_cast<int>(comp_count) - 1; c >= 1; --c) {
                                glyphs.erase(glyphs.begin() + static_cast<ptrdiff_t>(matched_indices[c]));
                            }
                            substituted = true;
                            break;
                        }
                    }

                    if (!substituted) {
                        ++i;
                    }
                }
            }
        }
    }
};

OpenTypeGsub::OpenTypeGsub() = default;
OpenTypeGsub::~OpenTypeGsub() = default;
OpenTypeGsub::OpenTypeGsub(OpenTypeGsub&&) noexcept = default;
OpenTypeGsub& OpenTypeGsub::operator=(OpenTypeGsub&&) noexcept = default;

std::unique_ptr<OpenTypeGsub> OpenTypeGsub::from_bytes(std::span<const uint8_t> data, const OpenTypeGdef* gdef) {
    auto gsub = std::make_unique<OpenTypeGsub>();
    gsub->impl_ = std::make_unique<Impl>();
    if (gsub->impl_->parse(data, gdef)) {
        gsub->is_valid_ = true;
        return gsub;
    }
    return nullptr;
}

bool OpenTypeGsub::apply_features(std::vector<uint16_t>& glyphs, std::span<const uint32_t> feature_tags) const {
    if (!is_valid_ || !impl_ || glyphs.empty()) return false;
    auto lookups = impl_->get_lookups_for_features(feature_tags);
    if (lookups.empty()) return false;
    for (uint16_t l_idx : lookups) {
        impl_->apply_lookup(l_idx, glyphs);
    }
    return true;
}

bool OpenTypeGsub::apply_ligatures(std::vector<uint16_t>& glyphs) const {
    const uint32_t standard_features[] = {
        make_tag('r', 'l', 'i', 'g'),
        make_tag('c', 'a', 'l', 't'),
        make_tag('l', 'i', 'g', 'a'),
        make_tag('c', 'l', 'i', 'g')
    };
    return apply_features(glyphs, standard_features);
}

uint16_t OpenTypeGsub::apply_single_glyph_feature(uint16_t glyph_id, uint32_t feature_tag) const noexcept {
    if (!is_valid_ || !impl_ || glyph_id == 0) return glyph_id;
    const uint32_t tags[1] = { feature_tag };
    auto lookups = impl_->get_lookups_for_features(tags);
    uint16_t cur = glyph_id;
    for (uint16_t l_idx : lookups) {
        uint16_t next = impl_->apply_single_on_glyph(l_idx, cur);
        if (next != cur) return next;
    }
    return cur;
}

// ============================================================================
// OpenType GPOS Implementation (Zero-Allocation Lazy Pre-Indexed Subtables)
// ============================================================================

struct OpenTypeGpos::Impl {
    struct PairPosSubtable {
        std::span<const uint8_t> span{};
        uint16_t pos_format{0};
        uint16_t cov_offset{0};
        uint16_t val_format1{0};
        uint16_t val_format2{0};
        uint8_t size1{0};
        uint8_t size2{0};
        // Format 1
        uint16_t pair_set_count{0};
        // Format 2
        uint16_t class_def1_offset{0};
        uint16_t class_def2_offset{0};
        uint16_t class1_count{0};
        uint16_t class2_count{0};
    };

    struct MarkPosSubtable {
        std::span<const uint8_t> span{};
        uint16_t lookup_type{0}; // 4 = MarkToBase, 6 = MarkToMark
        uint16_t mark_cov_offset{0};
        uint16_t base_cov_offset{0};
        uint16_t mark_class_count{0};
        uint16_t mark_array_offset{0};
        uint16_t base_array_offset{0};
    };

    std::span<const uint8_t> data{};
    uint16_t script_list_offset{0};
    uint16_t feature_list_offset{0};
    uint16_t lookup_list_offset{0};

    mutable bool kern_indexed{false};
    mutable bool mark_indexed{false};
    mutable std::vector<PairPosSubtable> kern_subtables{};
    mutable std::vector<MarkPosSubtable> mark_subtables{};

    bool parse(std::span<const uint8_t> d) noexcept {
        data = d;
        if (data.size() < 10) return false;
        uint16_t major = read_u16_be(data.data());
        if (major != 1) return false;
        script_list_offset = read_u16_be(data.data() + 4);
        feature_list_offset = read_u16_be(data.data() + 6);
        lookup_list_offset = read_u16_be(data.data() + 8);
        return (feature_list_offset < data.size() && lookup_list_offset < data.size());
    }

    void collect_feature_lookups(uint32_t target_tag, std::vector<uint16_t>& out_indices) const noexcept {
        if (feature_list_offset + 2u > data.size()) return;
        const uint8_t* fl_base = data.data() + feature_list_offset;
        size_t fl_max = data.size() - feature_list_offset;
        uint16_t feature_count = read_u16_be(fl_base);

        for (uint16_t i = 0; i < feature_count; ++i) {
            size_t rec_off = 2 + static_cast<size_t>(i) * 6;
            if (rec_off + 6 > fl_max) break;
            uint32_t tag = read_u32_be(fl_base + rec_off);
            if (tag != target_tag) continue;

            uint16_t feat_offset = read_u16_be(fl_base + rec_off + 4);
            if (feat_offset + 4u > fl_max) continue;

            const uint8_t* ft = fl_base + feat_offset;
            uint16_t count = read_u16_be(ft + 2);
            for (uint16_t k = 0; k < count; ++k) {
                if (feat_offset + 4 + static_cast<size_t>(k) * 2 + 2 > fl_max) break;
                uint16_t l_idx = read_u16_be(ft + 4 + k * 2);
                if (std::find(out_indices.begin(), out_indices.end(), l_idx) == out_indices.end()) {
                    out_indices.push_back(l_idx);
                }
            }
        }
    }

    void ensure_kern_indexed() const noexcept {
        if (kern_indexed) return;
        kern_indexed = true;
        if (lookup_list_offset + 2u > data.size()) return;

        std::vector<uint16_t> lookup_indices;
        lookup_indices.reserve(8);
        collect_feature_lookups(make_tag('k', 'e', 'r', 'n'), lookup_indices);
        if (lookup_indices.empty()) return;

        uint16_t total_lookups = read_u16_be(data.data() + lookup_list_offset);
        for (uint16_t lookup_idx : lookup_indices) {
            if (lookup_idx >= total_lookups) continue;
            size_t off_pos = lookup_list_offset + 2 + static_cast<size_t>(lookup_idx) * 2;
            if (off_pos + 2 > data.size()) continue;
            uint16_t l_offset = read_u16_be(data.data() + off_pos);
            size_t abs_lookup_offset = lookup_list_offset + l_offset;
            if (abs_lookup_offset + 6 > data.size()) continue;

            uint16_t lookup_type = read_u16_be(data.data() + abs_lookup_offset);
            if (lookup_type != 2 && lookup_type != 9) continue;
            uint16_t subtable_count = read_u16_be(data.data() + abs_lookup_offset + 4);

            for (uint16_t s = 0; s < subtable_count; ++s) {
                size_t sub_off_pos = abs_lookup_offset + 6 + static_cast<size_t>(s) * 2;
                if (sub_off_pos + 2 > data.size()) break;
                uint16_t sub_offset = read_u16_be(data.data() + sub_off_pos);
                size_t abs_sub_offset = abs_lookup_offset + sub_offset;
                if (abs_sub_offset >= data.size()) continue;

                auto sub_span = data.subspan(abs_sub_offset);
                uint16_t effective_type = lookup_type;
                if (lookup_type == 9) {
                    if (sub_span.size() < 8) continue;
                    uint16_t ext_fmt = read_u16_be(sub_span.data());
                    effective_type = read_u16_be(sub_span.data() + 2);
                    uint32_t ext_offset = read_u32_be(sub_span.data() + 4);
                    if (ext_fmt != 1 || ext_offset >= sub_span.size()) continue;
                    sub_span = sub_span.subspan(ext_offset);
                }

                if (effective_type != 2 || sub_span.size() < 10) continue;

                PairPosSubtable pst;
                pst.span = sub_span;
                pst.pos_format = read_u16_be(sub_span.data());
                pst.cov_offset = read_u16_be(sub_span.data() + 2);
                pst.val_format1 = read_u16_be(sub_span.data() + 4);
                pst.val_format2 = read_u16_be(sub_span.data() + 6);
                pst.size1 = static_cast<uint8_t>(get_value_record_size(pst.val_format1));
                pst.size2 = static_cast<uint8_t>(get_value_record_size(pst.val_format2));

                if (pst.pos_format == 1) {
                    pst.pair_set_count = read_u16_be(sub_span.data() + 8);
                    kern_subtables.push_back(pst);
                } else if (pst.pos_format == 2 && sub_span.size() >= 16) {
                    pst.class_def1_offset = read_u16_be(sub_span.data() + 8);
                    pst.class_def2_offset = read_u16_be(sub_span.data() + 10);
                    pst.class1_count = read_u16_be(sub_span.data() + 12);
                    pst.class2_count = read_u16_be(sub_span.data() + 14);
                    kern_subtables.push_back(pst);
                }
            }
        }
    }

    void ensure_mark_indexed() const noexcept {
        if (mark_indexed) return;
        mark_indexed = true;
        if (lookup_list_offset + 2u > data.size()) return;

        std::vector<uint16_t> lookup_indices;
        lookup_indices.reserve(8);
        collect_feature_lookups(make_tag('m', 'a', 'r', 'k'), lookup_indices);
        collect_feature_lookups(make_tag('m', 'k', 'm', 'k'), lookup_indices);
        if (lookup_indices.empty()) return;

        uint16_t total_lookups = read_u16_be(data.data() + lookup_list_offset);
        for (uint16_t lookup_idx : lookup_indices) {
            if (lookup_idx >= total_lookups) continue;
            size_t off_pos = lookup_list_offset + 2 + static_cast<size_t>(lookup_idx) * 2;
            if (off_pos + 2 > data.size()) continue;
            uint16_t l_offset = read_u16_be(data.data() + off_pos);
            size_t abs_lookup_offset = lookup_list_offset + l_offset;
            if (abs_lookup_offset + 6 > data.size()) continue;

            uint16_t lookup_type = read_u16_be(data.data() + abs_lookup_offset);
            if (lookup_type != 4 && lookup_type != 6 && lookup_type != 9) continue;
            uint16_t subtable_count = read_u16_be(data.data() + abs_lookup_offset + 4);

            for (uint16_t s = 0; s < subtable_count; ++s) {
                size_t sub_off_pos = abs_lookup_offset + 6 + static_cast<size_t>(s) * 2;
                if (sub_off_pos + 2 > data.size()) break;
                uint16_t sub_offset = read_u16_be(data.data() + sub_off_pos);
                size_t abs_sub_offset = abs_lookup_offset + sub_offset;
                if (abs_sub_offset >= data.size()) continue;

                auto sub_span = data.subspan(abs_sub_offset);
                uint16_t effective_type = lookup_type;
                if (lookup_type == 9) {
                    if (sub_span.size() < 8) continue;
                    uint16_t ext_fmt = read_u16_be(sub_span.data());
                    effective_type = read_u16_be(sub_span.data() + 2);
                    uint32_t ext_offset = read_u32_be(sub_span.data() + 4);
                    if (ext_fmt != 1 || ext_offset >= sub_span.size()) continue;
                    sub_span = sub_span.subspan(ext_offset);
                }

                if ((effective_type != 4 && effective_type != 6) || sub_span.size() < 12) continue;
                uint16_t pos_format = read_u16_be(sub_span.data());
                if (pos_format != 1) continue;

                MarkPosSubtable mst;
                mst.span = sub_span;
                mst.lookup_type = effective_type;
                mst.mark_cov_offset = read_u16_be(sub_span.data() + 2);
                mst.base_cov_offset = read_u16_be(sub_span.data() + 4);
                mst.mark_class_count = read_u16_be(sub_span.data() + 6);
                mst.mark_array_offset = read_u16_be(sub_span.data() + 8);
                mst.base_array_offset = read_u16_be(sub_span.data() + 10);
                mark_subtables.push_back(mst);
            }
        }
    }

    bool query_pair_pos(
        uint16_t left_glyph,
        uint16_t right_glyph,
        GlyphPlacementAdjustment& out_first,
        GlyphPlacementAdjustment& out_second
    ) const noexcept {
        ensure_kern_indexed();
        for (const auto& pst : kern_subtables) {
            int32_t cov = read_coverage_index(pst.span, pst.cov_offset, left_glyph);
            if (cov < 0) continue;

            const size_t size1 = pst.size1;
            const size_t size2 = pst.size2;
            const size_t pair_val_stride = 2 + size1 + size2;

            if (pst.pos_format == 1) {
                if (static_cast<uint16_t>(cov) >= pst.pair_set_count) continue;
                size_t ps_off_pos = 10 + static_cast<size_t>(cov) * 2;
                if (ps_off_pos + 2 > pst.span.size()) continue;
                uint16_t pair_set_offset = read_u16_be(pst.span.data() + ps_off_pos);
                if (pair_set_offset + 2u > pst.span.size()) continue;

                const uint8_t* ps_base = pst.span.data() + pair_set_offset;
                size_t ps_max = pst.span.size() - pair_set_offset;
                uint16_t pair_val_count = read_u16_be(ps_base);
                if (2 + static_cast<size_t>(pair_val_count) * pair_val_stride > ps_max) continue;

                const uint8_t* records = ps_base + 2;
                // Binary search over PairValueRecord array sorted by SecondGlyph
                int lo = 0;
                int hi = static_cast<int>(pair_val_count) - 1;
                while (lo <= hi) {
                    int mid = (lo + hi) >> 1;
                    const uint8_t* rec = records + static_cast<size_t>(mid) * pair_val_stride;
                    uint16_t second_g = read_u16_be(rec);
                    if (second_g == right_glyph) {
                        const uint8_t* p = rec + 2;
                        parse_value_record_ptr(p, pst.val_format1, out_first);
                        parse_value_record_ptr(p, pst.val_format2, out_second);
                        return true;
                    }
                    if (second_g < right_glyph) lo = mid + 1;
                    else hi = mid - 1;
                }
            } else if (pst.pos_format == 2) {
                uint16_t class1 = read_class_def(pst.span, pst.class_def1_offset, left_glyph);
                if (class1 >= pst.class1_count) continue;
                uint16_t class2 = read_class_def(pst.span, pst.class_def2_offset, right_glyph);
                if (class2 >= pst.class2_count) continue;

                size_t pair_size = size1 + size2;
                size_t record_offset = 16 + (static_cast<size_t>(class1) * pst.class2_count + class2) * pair_size;
                if (record_offset + pair_size <= pst.span.size()) {
                    const uint8_t* p = pst.span.data() + record_offset;
                    parse_value_record_ptr(p, pst.val_format1, out_first);
                    parse_value_record_ptr(p, pst.val_format2, out_second);
                    return true;
                }
            }
        }
        return false;
    }

    bool query_mark_attachment(
        uint16_t target_type,
        uint16_t base_glyph,
        uint16_t mark_glyph,
        int16_t& out_dx,
        int16_t& out_dy
    ) const noexcept {
        ensure_mark_indexed();
        for (const auto& mst : mark_subtables) {
            if (mst.lookup_type != target_type) continue;
            int32_t mark_cov = read_coverage_index(mst.span, mst.mark_cov_offset, mark_glyph);
            if (mark_cov < 0) continue;
            int32_t base_cov = read_coverage_index(mst.span, mst.base_cov_offset, base_glyph);
            if (base_cov < 0) continue;

            if (mst.mark_array_offset + 2u > mst.span.size()) continue;
            auto mark_array_span = mst.span.subspan(mst.mark_array_offset);
            uint16_t mark_count = read_u16_be(mark_array_span.data());
            if (static_cast<uint16_t>(mark_cov) >= mark_count) continue;

            size_t m_rec_off = 2 + static_cast<size_t>(mark_cov) * 4;
            if (m_rec_off + 4 > mark_array_span.size()) continue;
            uint16_t mark_class = read_u16_be(mark_array_span.data() + m_rec_off);
            uint16_t mark_anchor_offset = read_u16_be(mark_array_span.data() + m_rec_off + 2);
            if (mark_class >= mst.mark_class_count) continue;

            if (mst.base_array_offset + 2u > mst.span.size()) continue;
            auto base_array_span = mst.span.subspan(mst.base_array_offset);
            uint16_t base_count = read_u16_be(base_array_span.data());
            if (static_cast<uint16_t>(base_cov) >= base_count) continue;

            size_t base_record_size = static_cast<size_t>(mst.mark_class_count) * 2;
            size_t b_anch_pos = 2 + static_cast<size_t>(base_cov) * base_record_size + static_cast<size_t>(mark_class) * 2;
            if (b_anch_pos + 2 > base_array_span.size()) continue;

            uint16_t base_anchor_offset = read_u16_be(base_array_span.data() + b_anch_pos);
            if (base_anchor_offset == 0) continue;

            if (mark_anchor_offset + 6u > mark_array_span.size()) continue;
            int16_t mark_x = read_i16_be(mark_array_span.data() + mark_anchor_offset + 2);
            int16_t mark_y = read_i16_be(mark_array_span.data() + mark_anchor_offset + 4);

            if (base_anchor_offset + 6u > base_array_span.size()) continue;
            int16_t base_x = read_i16_be(base_array_span.data() + base_anchor_offset + 2);
            int16_t base_y = read_i16_be(base_array_span.data() + base_anchor_offset + 4);

            out_dx = static_cast<int16_t>(base_x - mark_x);
            out_dy = static_cast<int16_t>(base_y - mark_y);
            return true;
        }
        return false;
    }
};

OpenTypeGpos::OpenTypeGpos() = default;
OpenTypeGpos::~OpenTypeGpos() = default;
OpenTypeGpos::OpenTypeGpos(OpenTypeGpos&&) noexcept = default;
OpenTypeGpos& OpenTypeGpos::operator=(OpenTypeGpos&&) noexcept = default;

std::unique_ptr<OpenTypeGpos> OpenTypeGpos::from_bytes(std::span<const uint8_t> data) {
    auto gpos = std::make_unique<OpenTypeGpos>();
    gpos->impl_ = std::make_unique<Impl>();
    if (gpos->impl_->parse(data)) {
        gpos->is_valid_ = true;
        return gpos;
    }
    return nullptr;
}

bool OpenTypeGpos::has_kerning() const noexcept {
    if (!is_valid_ || !impl_) return false;
    impl_->ensure_kern_indexed();
    return !impl_->kern_subtables.empty();
}

bool OpenTypeGpos::has_mark_positioning() const noexcept {
    if (!is_valid_ || !impl_) return false;
    impl_->ensure_mark_indexed();
    return !impl_->mark_subtables.empty();
}

bool OpenTypeGpos::get_pair_adjustment(
    uint16_t left_glyph,
    uint16_t right_glyph,
    GlyphPlacementAdjustment& out_first,
    GlyphPlacementAdjustment& out_second
) const noexcept {
    if (!is_valid_ || !impl_) return false;
    return impl_->query_pair_pos(left_glyph, right_glyph, out_first, out_second);
}

bool OpenTypeGpos::get_kerning(uint16_t left_glyph, uint16_t right_glyph, int16_t& out_x_advance) const noexcept {
    if (!is_valid_ || !impl_) return false;
    GlyphPlacementAdjustment f{}, s{};
    if (impl_->query_pair_pos(left_glyph, right_glyph, f, s)) {
        out_x_advance = f.x_advance;
        return true;
    }
    return false;
}

bool OpenTypeGpos::get_mark_to_base_offset(
    uint16_t base_glyph,
    uint16_t mark_glyph,
    int16_t& out_dx,
    int16_t& out_dy
) const noexcept {
    if (!is_valid_ || !impl_) return false;
    return impl_->query_mark_attachment(4, base_glyph, mark_glyph, out_dx, out_dy);
}

bool OpenTypeGpos::get_mark_to_mark_offset(
    uint16_t base_mark_glyph,
    uint16_t attaching_mark_glyph,
    int16_t& out_dx,
    int16_t& out_dy
) const noexcept {
    if (!is_valid_ || !impl_) return false;
    return impl_->query_mark_attachment(6, base_mark_glyph, attaching_mark_glyph, out_dx, out_dy);
}

} // namespace nisaba::text
