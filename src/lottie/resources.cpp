/// @file resources.cpp
/// @brief Pluggable ResourceProvider, deferred image decoding, animated WebP & GIF89a multi-frame decoding.

#include "nisaba/lottie/resources.hpp"
#include "nisaba/image/image_io.hpp"
#include "nisaba/image/webp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nisaba::lottie {

// ── Base64 Decoding (RFC 4648 / RFC 2397) ─────────────────────────────────

std::vector<uint8_t> decode_base64_bytes(std::string_view input) {
    static constexpr uint8_t LUT[256] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,62,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,63,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };

    std::vector<uint8_t> out;
    out.reserve((input.size() * 3) / 4);

    uint32_t accum = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=') break;
        const uint8_t val = LUT[c];
        if (val >= 64) continue;
        accum = (accum << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFFu));
        }
    }
    return out;
}

namespace {

// ── Sovereign GIF87a / GIF89a Multi-Frame Decoder ─────────────────────────

bool is_gif_signature(std::span<const uint8_t> data) noexcept {
    if (data.size() < 13) return false;
    return std::memcmp(data.data(), "GIF87a", 6) == 0 ||
           std::memcmp(data.data(), "GIF89a", 6) == 0;
}

bool probe_gif_header(
    std::span<const uint8_t> data,
    uint32_t& out_w,
    uint32_t& out_h,
    bool& out_multi_frame
) noexcept {
    if (!is_gif_signature(data)) return false;
    out_w = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8);
    out_h = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8);
    out_multi_frame = false;

    // Quick scan for >1 image descriptors (0x2C)
    size_t pos = 13;
    const uint8_t packed = data[10];
    if (packed & 0x80u) {
        const size_t gct_entries = size_t{1} << ((packed & 0x07u) + 1u);
        pos += gct_entries * 3u;
    }
    int image_count = 0;
    while (pos < data.size()) {
        const uint8_t intro = data[pos++];
        if (intro == 0x3Bu) break; // Trailer
        if (intro == 0x21u) {      // Extension
            if (pos >= data.size()) break;
            ++pos; // label
            while (pos < data.size()) {
                const uint8_t sub_len = data[pos++];
                if (sub_len == 0) break;
                pos += sub_len;
            }
        } else if (intro == 0x2Cu) { // Image descriptor
            if (pos + 9 > data.size()) break;
            ++image_count;
            if (image_count >= 2) {
                out_multi_frame = true;
                return true;
            }
            const uint8_t ipacked = data[pos + 8];
            pos += 9;
            if (ipacked & 0x80u) {
                const size_t lct_entries = size_t{1} << ((ipacked & 0x07u) + 1u);
                pos += lct_entries * 3u;
            }
            if (pos >= data.size()) break;
            ++pos; // LZW min code size
            while (pos < data.size()) {
                const uint8_t sub_len = data[pos++];
                if (sub_len == 0) break;
                pos += sub_len;
            }
        } else {
            break;
        }
    }
    return out_w > 0 && out_h > 0;
}

bool decode_gif_lzw(
    const std::vector<uint8_t>& lzw_bytes,
    uint8_t min_code_size,
    size_t expected_pixels,
    std::vector<uint8_t>& out_indices
) {
    if (min_code_size < 2 || min_code_size > 11) return false;

    const uint32_t clear_code = 1u << min_code_size;
    const uint32_t eoi_code = clear_code + 1u;
    uint32_t code_size = static_cast<uint32_t>(min_code_size) + 1u;
    uint32_t next_code = eoi_code + 1u;

    std::array<int32_t, 4096> prefix{};
    std::array<uint8_t, 4096> suffix{};
    std::array<uint8_t, 4096> stack{};

    auto reset_table = [&]() {
        code_size = static_cast<uint32_t>(min_code_size) + 1u;
        next_code = eoi_code + 1u;
        for (uint32_t i = 0; i < clear_code; ++i) {
            prefix[i] = -1;
            suffix[i] = static_cast<uint8_t>(i);
        }
    };
    reset_table();

    out_indices.clear();
    out_indices.reserve(expected_pixels);

    size_t byte_pos = 0;
    uint32_t bit_buf = 0;
    uint32_t bit_count = 0;

    auto read_code = [&](uint32_t& out_code) -> bool {
        while (bit_count < code_size) {
            if (byte_pos >= lzw_bytes.size()) return false;
            bit_buf |= static_cast<uint32_t>(lzw_bytes[byte_pos++]) << bit_count;
            bit_count += 8u;
        }
        out_code = bit_buf & ((1u << code_size) - 1u);
        bit_buf >>= code_size;
        bit_count -= code_size;
        return true;
    };

    int32_t prev_code = -1;
    uint8_t first_char = 0;
    uint32_t code = 0;

    while (out_indices.size() < expected_pixels && read_code(code)) {
        if (code == clear_code) {
            reset_table();
            prev_code = -1;
            continue;
        }
        if (code == eoi_code) {
            break;
        }

        size_t stack_top = 0;
        uint32_t cur = code;
        if (cur >= next_code) {
            if (cur != next_code || prev_code < 0) {
                break;
            }
            stack[stack_top++] = first_char;
            cur = static_cast<uint32_t>(prev_code);
        }

        while (cur >= clear_code) {
            if (stack_top >= stack.size() || cur >= 4096) break;
            stack[stack_top++] = suffix[cur];
            const int32_t p = prefix[cur];
            if (p < 0) break;
            cur = static_cast<uint32_t>(p);
        }
        first_char = suffix[cur];
        if (stack_top < stack.size()) {
            stack[stack_top++] = first_char;
        }

        while (stack_top > 0 && out_indices.size() < expected_pixels) {
            out_indices.push_back(stack[--stack_top]);
        }

        if (prev_code >= 0 && next_code < 4096) {
            prefix[next_code] = prev_code;
            suffix[next_code] = first_char;
            ++next_code;
            if (next_code == (1u << code_size) && code_size < 12u) {
                ++code_size;
            }
        }
        prev_code = static_cast<int32_t>(code);
    }

    return !out_indices.empty();
}

std::vector<DecodedImageAsset::TimedFrame> decode_gif_frames(std::span<const uint8_t> data) {
    std::vector<DecodedImageAsset::TimedFrame> result;
    if (!is_gif_signature(data)) return result;

    const uint32_t canvas_w = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8);
    const uint32_t canvas_h = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8);
    if (canvas_w == 0 || canvas_h == 0 || canvas_w > 8192 || canvas_h > 8192) return result;

    const uint8_t lsd_packed = data[10];
    const bool has_gct = (lsd_packed & 0x80u) != 0;
    const size_t gct_count = has_gct ? (size_t{1} << ((lsd_packed & 0x07u) + 1u)) : 0;

    size_t pos = 13;
    std::vector<PremultipliedColorU8> gct(gct_count, PremultipliedColorU8::TRANSPARENT);
    if (has_gct) {
        if (pos + gct_count * 3u > data.size()) return result;
        for (size_t i = 0; i < gct_count; ++i) {
            gct[i] = PremultipliedColorU8::from_rgba_unchecked(
                data[pos + i * 3u],
                data[pos + i * 3u + 1u],
                data[pos + i * 3u + 2u],
                255
            );
        }
        pos += gct_count * 3u;
    }

    auto canvas_opt = Pixmap::allocate(canvas_w, canvas_h);
    if (!canvas_opt) return result;
    Pixmap canvas = std::move(*canvas_opt);

    uint8_t disposal = 0;
    bool has_transparency = false;
    uint8_t transparent_idx = 0;
    uint16_t delay_cs = 10; // default 100ms

    std::vector<uint8_t> lzw_stream;
    std::vector<uint8_t> pixel_indices;

    while (pos < data.size()) {
        const uint8_t intro = data[pos++];
        if (intro == 0x3Bu) {
            break; // Trailer
        }
        if (intro == 0x21u) {
            if (pos >= data.size()) break;
            const uint8_t label = data[pos++];
            if (label == 0xF9u && pos + 5 <= data.size() && data[pos] == 4) {
                const uint8_t gce_packed = data[pos + 1];
                disposal = (gce_packed >> 2) & 0x07u;
                has_transparency = (gce_packed & 0x01u) != 0;
                delay_cs = static_cast<uint16_t>(data[pos + 2]) |
                           (static_cast<uint16_t>(data[pos + 3]) << 8);
                transparent_idx = data[pos + 4];
            }
            while (pos < data.size()) {
                const uint8_t sub_len = data[pos++];
                if (sub_len == 0) break;
                pos += sub_len;
            }
            continue;
        }
        if (intro != 0x2Cu) {
            break;
        }
        if (pos + 9 > data.size()) break;

        const uint32_t fx = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8);
        const uint32_t fy = static_cast<uint32_t>(data[pos + 2]) | (static_cast<uint32_t>(data[pos + 3]) << 8);
        const uint32_t fw = static_cast<uint32_t>(data[pos + 4]) | (static_cast<uint32_t>(data[pos + 5]) << 8);
        const uint32_t fh = static_cast<uint32_t>(data[pos + 6]) | (static_cast<uint32_t>(data[pos + 7]) << 8);
        const uint8_t ipacked = data[pos + 8];
        pos += 9;

        const bool has_lct = (ipacked & 0x80u) != 0;
        const bool interlaced = (ipacked & 0x40u) != 0;
        std::vector<PremultipliedColorU8> lct;
        if (has_lct) {
            const size_t lct_count = size_t{1} << ((ipacked & 0x07u) + 1u);
            if (pos + lct_count * 3u > data.size()) break;
            lct.resize(lct_count);
            for (size_t i = 0; i < lct_count; ++i) {
                lct[i] = PremultipliedColorU8::from_rgba_unchecked(
                    data[pos + i * 3u],
                    data[pos + i * 3u + 1u],
                    data[pos + i * 3u + 2u],
                    255
                );
            }
            pos += lct_count * 3u;
        }

        if (pos >= data.size()) break;
        const uint8_t min_code_size = data[pos++];

        lzw_stream.clear();
        while (pos < data.size()) {
            const uint8_t sub_len = data[pos++];
            if (sub_len == 0) break;
            if (pos + sub_len > data.size()) {
                pos = data.size();
                break;
            }
            lzw_stream.insert(lzw_stream.end(), data.data() + pos, data.data() + pos + sub_len);
            pos += sub_len;
        }

        const auto& palette = has_lct ? lct : gct;
        const size_t expected_px = static_cast<size_t>(fw) * static_cast<size_t>(fh);
        if (expected_px == 0 || palette.empty()) continue;

        std::optional<Pixmap> saved_prev;
        if (disposal == 3) {
            saved_prev = canvas.as_ref().to_owned();
        }

        if (decode_gif_lzw(lzw_stream, min_code_size, expected_px, pixel_indices)) {
            PixmapMut mut = canvas.as_mut();
            auto write_row = [&](uint32_t src_y, uint32_t dst_y) {
                if (dst_y >= canvas_h) return;
                PremultipliedColorU8* row = mut.row(dst_y);
                const size_t base = static_cast<size_t>(src_y) * fw;
                for (uint32_t x = 0; x < fw; ++x) {
                    const uint32_t dst_x = fx + x;
                    if (dst_x >= canvas_w) break;
                    const size_t idx_pos = base + x;
                    if (idx_pos >= pixel_indices.size()) break;
                    const uint8_t cidx = pixel_indices[idx_pos];
                    if (has_transparency && cidx == transparent_idx) continue;
                    if (cidx < palette.size()) {
                        row[dst_x] = palette[cidx];
                    }
                }
            };

            if (!interlaced) {
                for (uint32_t y = 0; y < fh; ++y) {
                    write_row(y, fy + y);
                }
            } else {
                static constexpr uint32_t kStart[4] = {0, 4, 2, 1};
                static constexpr uint32_t kStep[4]  = {8, 8, 4, 2};
                uint32_t src_y = 0;
                for (int pass = 0; pass < 4; ++pass) {
                    for (uint32_t dy = kStart[pass]; dy < fh; dy += kStep[pass]) {
                        write_row(src_y++, fy + dy);
                    }
                }
            }
        }

        const float duration_s = (delay_cs > 0) ? (static_cast<float>(delay_cs) * 0.01f) : 0.1f;
        result.push_back(DecodedImageAsset::TimedFrame{
            .pixmap = canvas.as_ref().to_owned(),
            .duration_seconds = duration_s,
        });

        // Apply disposal method for subsequent frame
        if (disposal == 2) {
            PixmapMut mut = canvas.as_mut();
            const uint32_t y_end = std::min(canvas_h, fy + fh);
            const uint32_t x_end = std::min(canvas_w, fx + fw);
            for (uint32_t y = fy; y < y_end; ++y) {
                PremultipliedColorU8* row = mut.row(y);
                for (uint32_t x = fx; x < x_end; ++x) {
                    row[x] = PremultipliedColorU8::TRANSPARENT;
                }
            }
        } else if (disposal == 3 && saved_prev.has_value()) {
            canvas = std::move(*saved_prev);
        }

        // Reset GCE state for next image block
        disposal = 0;
        has_transparency = false;
        transparent_idx = 0;
        delay_cs = 10;
    }

    return result;
}

std::vector<uint8_t> read_binary_file(const std::filesystem::path& p) {
    std::ifstream file(p, std::ios::in | std::ios::binary);
    if (!file.is_open()) return {};
    file.seekg(0, std::ios::end);
    const std::streamoff sz = file.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(out.data()), sz);
    if (!file) return {};
    return out;
}

std::string normalize_resource_key(std::string_view p, std::string_view n) {
    std::string combined;
    combined.reserve(p.size() + n.size() + 1);
    if (!p.empty()) {
        combined.append(p);
        if (combined.back() != '/' && combined.back() != '\\' && !n.empty()) {
            combined.push_back('/');
        }
    }
    combined.append(n);
    std::replace(combined.begin(), combined.end(), '\\', '/');
    if (combined.starts_with("./")) combined.erase(0, 2);
    if (combined.starts_with("/")) combined.erase(0, 1);
    return combined;
}

} // anonymous namespace

// ── DecodedImageAsset ─────────────────────────────────────────────────────

std::shared_ptr<DecodedImageAsset> DecodedImageAsset::from_pixmap(Pixmap pixmap) {
    auto asset = std::make_shared<DecodedImageAsset>();
    asset->probed_width_ = pixmap.width();
    asset->probed_height_ = pixmap.height();
    asset->probed_multi_frame_ = false;
    asset->frames_.push_back(TimedFrame{std::move(pixmap), 0.0f});
    asset->decoded_ = true;
    return asset;
}

std::shared_ptr<DecodedImageAsset> DecodedImageAsset::from_frames(std::vector<TimedFrame> frames) {
    auto asset = std::make_shared<DecodedImageAsset>();
    if (!frames.empty()) {
        asset->probed_width_ = frames.front().pixmap.width();
        asset->probed_height_ = frames.front().pixmap.height();
        asset->probed_multi_frame_ = frames.size() > 1;
        float accum = 0.0f;
        asset->cumulative_times_.reserve(frames.size());
        for (auto& f : frames) {
            if (f.duration_seconds <= 0.0f) f.duration_seconds = 1.0f / 24.0f;
            accum += f.duration_seconds;
            asset->cumulative_times_.push_back(accum);
        }
        asset->total_duration_ = accum;
        asset->frames_ = std::move(frames);
    }
    asset->decoded_ = true;
    return asset;
}

std::shared_ptr<DecodedImageAsset> DecodedImageAsset::from_encoded_bytes(
    std::vector<uint8_t> encoded_bytes,
    bool defer_decode
) {
    if (encoded_bytes.empty()) return nullptr;
    auto asset = std::make_shared<DecodedImageAsset>();
    asset->probe_header_locked(encoded_bytes);
    if (defer_decode) {
        asset->encoded_bytes_ = std::move(encoded_bytes);
        asset->decoded_ = false;
    } else {
        asset->decode_bytes_locked(encoded_bytes);
        asset->decoded_ = true;
        if (asset->frames_.empty()) return nullptr;
    }
    return asset;
}

std::shared_ptr<DecodedImageAsset> DecodedImageAsset::from_lazy_loader(
    std::function<std::vector<uint8_t>()> loader,
    bool defer_decode
) {
    if (!loader) return nullptr;
    auto asset = std::make_shared<DecodedImageAsset>();
    asset->lazy_loader_ = std::move(loader);
    if (!defer_decode) {
        asset->ensure_decoded();
        if (asset->frames_.empty()) return nullptr;
    }
    return asset;
}

void DecodedImageAsset::probe_header_locked(std::span<const uint8_t> data) {
    if (is_gif_signature(data)) {
        probe_gif_header(data, probed_width_, probed_height_, probed_multi_frame_);
        return;
    }
    if (image::is_animated_webp(data)) {
        probed_multi_frame_ = true;
    }
    if (auto info = image::probe_image_from_memory(data); info.has_value()) {
        probed_width_ = info->width;
        probed_height_ = info->height;
    }
}

void DecodedImageAsset::decode_bytes_locked(std::span<const uint8_t> data) const {
    frames_.clear();
    cumulative_times_.clear();
    total_duration_ = 0.0f;
    if (data.empty()) return;

    if (is_gif_signature(data)) {
        auto gif_frames = decode_gif_frames(data);
        if (!gif_frames.empty()) {
            probed_width_ = gif_frames.front().pixmap.width();
            probed_height_ = gif_frames.front().pixmap.height();
            probed_multi_frame_ = gif_frames.size() > 1;
            float accum = 0.0f;
            cumulative_times_.reserve(gif_frames.size());
            for (const auto& f : gif_frames) {
                accum += std::max(f.duration_seconds, 0.01f);
                cumulative_times_.push_back(accum);
            }
            total_duration_ = accum;
            frames_ = std::move(gif_frames);
            return;
        }
    }

    if (image::is_animated_webp(data)) {
        if (auto anim_res = image::decode_webp_animation(data); anim_res.has_value() && !anim_res->empty()) {
            probed_width_ = anim_res->canvas_width;
            probed_height_ = anim_res->canvas_height;
            probed_multi_frame_ = anim_res->frames.size() > 1;
            frames_.reserve(anim_res->frames.size());
            cumulative_times_.reserve(anim_res->frames.size());
            float accum = 0.0f;
            for (auto& wf : anim_res->frames) {
                const float dur = (wf.duration_ms > 0) ? (static_cast<float>(wf.duration_ms) * 0.001f) : 0.1f;
                accum += dur;
                cumulative_times_.push_back(accum);
                frames_.push_back(TimedFrame{std::move(wf.pixmap), dur});
            }
            total_duration_ = accum;
            return;
        }
    }

    if (auto img_res = image::load_image_from_memory(data); img_res.has_value()) {
        probed_width_ = img_res->width();
        probed_height_ = img_res->height();
        probed_multi_frame_ = false;
        frames_.push_back(TimedFrame{std::move(*img_res), 0.0f});
    }
}

void DecodedImageAsset::ensure_decoded() const {
    if (decoded_) return;
    if (encoded_bytes_.empty() && lazy_loader_) {
        encoded_bytes_ = lazy_loader_();
        lazy_loader_ = nullptr;
    }
    if (!encoded_bytes_.empty()) {
        decode_bytes_locked(encoded_bytes_);
        encoded_bytes_.clear();
        encoded_bytes_.shrink_to_fit();
    }
    decoded_ = true;
}

size_t DecodedImageAsset::select_frame_index(float t_seconds) const noexcept {
    if (frames_.size() <= 1 || total_duration_ <= 0.0f) return 0;
    float wrapped = std::fmod(std::max(0.0f, t_seconds), total_duration_);
    if (wrapped < 0.0f) wrapped += total_duration_;
    auto it = std::upper_bound(cumulative_times_.begin(), cumulative_times_.end(), wrapped);
    size_t idx = static_cast<size_t>(std::distance(cumulative_times_.begin(), it));
    if (idx >= frames_.size()) idx = frames_.size() - 1;
    return idx;
}

bool DecodedImageAsset::is_multi_frame() const noexcept {
    return decoded_ ? (frames_.size() > 1) : probed_multi_frame_;
}

std::optional<PixmapRef> DecodedImageAsset::get_frame(float t_seconds) {
    ensure_decoded();
    if (frames_.empty()) return std::nullopt;
    const size_t idx = select_frame_index(t_seconds);
    return frames_[idx].pixmap.as_ref();
}

uint64_t DecodedImageAsset::frame_token(float t_seconds) const noexcept {
    if (!decoded_) {
        // Before decode: return a stable non-zero token if multi-frame is suspected based on time bucket
        if (probed_multi_frame_) {
            return 1u + static_cast<uint64_t>(std::max(0.0f, t_seconds) * 1000.0f);
        }
        return 1u;
    }
    if (frames_.empty()) return 0u;
    return 1u + static_cast<uint64_t>(select_frame_index(t_seconds));
}

uint32_t DecodedImageAsset::width() const noexcept {
    if (probed_width_ > 0) return probed_width_;
    if (decoded_ && !frames_.empty()) return frames_.front().pixmap.width();
    return 0;
}

uint32_t DecodedImageAsset::height() const noexcept {
    if (probed_height_ > 0) return probed_height_;
    if (decoded_ && !frames_.empty()) return frames_.front().pixmap.height();
    return 0;
}

// ── ResourceProvider Default Implementations ──────────────────────────────

std::shared_ptr<ImageAsset> ResourceProvider::load_image_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    (void)resource_id;
    auto bytes = load(resource_path, resource_name);
    if (bytes.empty()) return nullptr;
    return DecodedImageAsset::from_encoded_bytes(std::move(bytes), defer_image_decoding_);
}

std::vector<uint8_t> ResourceProvider::load_font(
    std::string_view font_name,
    std::string_view font_url
) {
    if (!font_url.empty()) {
        auto bytes = load("", font_url);
        if (!bytes.empty()) return bytes;
    }
    if (!font_name.empty()) {
        return load("", font_name);
    }
    return {};
}

std::shared_ptr<AudioAsset> ResourceProvider::load_audio_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    (void)resource_id;
    auto bytes = load(resource_path, resource_name);
    if (bytes.empty()) return nullptr;
    return std::make_shared<MemoryAudioAsset>(std::move(bytes));
}

// ── FileResourceProvider ──────────────────────────────────────────────────

FileResourceProvider::FileResourceProvider(std::string base_dir, bool defer_decode)
    : base_dir_(std::move(base_dir)) {
    defer_image_decoding_ = defer_decode;
}

std::vector<uint8_t> FileResourceProvider::load(
    std::string_view resource_path,
    std::string_view resource_name
) {
    if (resource_name.empty() && resource_path.empty()) return {};
    std::filesystem::path full;
    if (!base_dir_.empty()) {
        full = std::filesystem::path(base_dir_) / std::filesystem::path(resource_path) / std::filesystem::path(resource_name);
    } else {
        full = std::filesystem::path(resource_path) / std::filesystem::path(resource_name);
    }
    return read_binary_file(full);
}

std::shared_ptr<ImageAsset> FileResourceProvider::load_image_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    (void)resource_id;
    if (resource_name.empty()) return nullptr;
    std::filesystem::path full;
    if (!base_dir_.empty()) {
        full = std::filesystem::path(base_dir_) / std::filesystem::path(resource_path) / std::filesystem::path(resource_name);
    } else {
        full = std::filesystem::path(resource_path) / std::filesystem::path(resource_name);
    }

    if (defer_image_decoding_) {
        std::error_code ec;
        if (!std::filesystem::exists(full, ec)) return nullptr;
        return DecodedImageAsset::from_lazy_loader(
            [full]() { return read_binary_file(full); },
            /*defer_decode=*/true
        );
    }
    auto bytes = read_binary_file(full);
    if (bytes.empty()) return nullptr;
    return DecodedImageAsset::from_encoded_bytes(std::move(bytes), false);
}

std::vector<uint8_t> FileResourceProvider::load_font(
    std::string_view font_name,
    std::string_view font_url
) {
    (void)font_name;
    if (font_url.empty()) return {};
    std::filesystem::path fp = base_dir_.empty()
        ? std::filesystem::path(font_url)
        : (std::filesystem::path(base_dir_) / std::filesystem::path(font_url));
    return read_binary_file(fp);
}

// ── MemoryResourceProvider ────────────────────────────────────────────────

void MemoryResourceProvider::add_resource(std::string key, std::vector<uint8_t> bytes) {
    std::replace(key.begin(), key.end(), '\\', '/');
    if (key.starts_with("./")) key.erase(0, 2);
    if (key.starts_with("/")) key.erase(0, 1);
    blobs_[std::move(key)] = std::move(bytes);
}

void MemoryResourceProvider::add_image_asset(std::string id_or_name, std::shared_ptr<ImageAsset> asset) {
    images_[std::move(id_or_name)] = std::move(asset);
}

void MemoryResourceProvider::add_font(std::string name_or_url, std::vector<uint8_t> ttf_bytes) {
    fonts_[std::move(name_or_url)] = std::move(ttf_bytes);
}

void MemoryResourceProvider::add_audio_asset(std::string id_or_name, std::shared_ptr<AudioAsset> asset) {
    audio_[std::move(id_or_name)] = std::move(asset);
}

std::vector<uint8_t> MemoryResourceProvider::load(
    std::string_view resource_path,
    std::string_view resource_name
) {
    const std::string key = normalize_resource_key(resource_path, resource_name);
    if (auto it = blobs_.find(key); it != blobs_.end()) {
        return it->second;
    }
    const std::string name_only = normalize_resource_key("", resource_name);
    if (auto it = blobs_.find(name_only); it != blobs_.end()) {
        return it->second;
    }
    // Check basename match
    const auto slash = name_only.find_last_of('/');
    const std::string_view base = (slash != std::string::npos)
        ? std::string_view(name_only).substr(slash + 1)
        : std::string_view(name_only);
    for (const auto& [k, v] : blobs_) {
        if (k == base || (k.size() > base.size() && k[k.size() - base.size() - 1] == '/' &&
                          std::string_view(k).ends_with(base))) {
            return v;
        }
    }
    return {};
}

std::shared_ptr<ImageAsset> MemoryResourceProvider::load_image_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    if (!resource_id.empty()) {
        if (auto it = images_.find(std::string(resource_id)); it != images_.end()) {
            return it->second;
        }
    }
    const std::string full_key = normalize_resource_key(resource_path, resource_name);
    if (auto it = images_.find(full_key); it != images_.end()) {
        return it->second;
    }
    if (!resource_name.empty()) {
        if (auto it = images_.find(std::string(resource_name)); it != images_.end()) {
            return it->second;
        }
    }
    return ResourceProvider::load_image_asset(resource_path, resource_name, resource_id);
}

std::vector<uint8_t> MemoryResourceProvider::load_font(
    std::string_view font_name,
    std::string_view font_url
) {
    if (!font_name.empty()) {
        if (auto it = fonts_.find(std::string(font_name)); it != fonts_.end()) {
            return it->second;
        }
    }
    if (!font_url.empty()) {
        if (auto it = fonts_.find(std::string(font_url)); it != fonts_.end()) {
            return it->second;
        }
    }
    return ResourceProvider::load_font(font_name, font_url);
}

std::shared_ptr<AudioAsset> MemoryResourceProvider::load_audio_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    if (!resource_id.empty()) {
        if (auto it = audio_.find(std::string(resource_id)); it != audio_.end()) {
            return it->second;
        }
    }
    const std::string full_key = normalize_resource_key(resource_path, resource_name);
    if (auto it = audio_.find(full_key); it != audio_.end()) {
        return it->second;
    }
    if (!resource_name.empty()) {
        if (auto it = audio_.find(std::string(resource_name)); it != audio_.end()) {
            return it->second;
        }
    }
    return ResourceProvider::load_audio_asset(resource_path, resource_name, resource_id);
}

// ── DataURIResourceProviderProxy ──────────────────────────────────────────

DataURIResourceProviderProxy::DataURIResourceProviderProxy(
    std::shared_ptr<ResourceProvider> fallback,
    bool defer_decode
) : fallback_(std::move(fallback)) {
    defer_image_decoding_ = defer_decode;
}

std::vector<uint8_t> DataURIResourceProviderProxy::load(
    std::string_view resource_path,
    std::string_view resource_name
) {
    if (resource_name.starts_with("data:")) {
        const auto comma = resource_name.find(',');
        if (comma != std::string_view::npos) {
            return decode_base64_bytes(resource_name.substr(comma + 1));
        }
        return {};
    }
    return fallback_ ? fallback_->load(resource_path, resource_name) : std::vector<uint8_t>{};
}

std::shared_ptr<ImageAsset> DataURIResourceProviderProxy::load_image_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    if (resource_name.starts_with("data:")) {
        const auto comma = resource_name.find(',');
        if (comma == std::string_view::npos) return nullptr;
        auto bytes = decode_base64_bytes(resource_name.substr(comma + 1));
        if (bytes.empty()) return nullptr;
        return DecodedImageAsset::from_encoded_bytes(std::move(bytes), defer_image_decoding_);
    }
    if (fallback_) {
        return fallback_->load_image_asset(resource_path, resource_name, resource_id);
    }
    return nullptr;
}

std::vector<uint8_t> DataURIResourceProviderProxy::load_font(
    std::string_view font_name,
    std::string_view font_url
) {
    if (font_url.starts_with("data:")) {
        const auto comma = font_url.find(',');
        if (comma != std::string_view::npos) {
            return decode_base64_bytes(font_url.substr(comma + 1));
        }
        return {};
    }
    return fallback_ ? fallback_->load_font(font_name, font_url) : std::vector<uint8_t>{};
}

std::shared_ptr<AudioAsset> DataURIResourceProviderProxy::load_audio_asset(
    std::string_view resource_path,
    std::string_view resource_name,
    std::string_view resource_id
) {
    if (resource_name.starts_with("data:")) {
        const auto comma = resource_name.find(',');
        if (comma != std::string_view::npos) {
            auto bytes = decode_base64_bytes(resource_name.substr(comma + 1));
            if (!bytes.empty()) {
                return std::make_shared<MemoryAudioAsset>(std::move(bytes));
            }
        }
        return nullptr;
    }
    return fallback_ ? fallback_->load_audio_asset(resource_path, resource_name, resource_id) : nullptr;
}

} // namespace nisaba::lottie

