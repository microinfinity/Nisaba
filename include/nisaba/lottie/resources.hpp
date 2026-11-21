#pragma once

/// @file resources.hpp
/// @brief Pluggable resource provider, deferred image decoding, and multi-frame asset interfaces for Lottie.

#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/math/transform.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nisaba::lottie {

/// Bitmask flags controlling Lottie document loading behavior.
enum class LoadFlags : uint32_t {
    None = 0,
    /// Defer decoding of image assets until the first visible seek/render of a layer referencing them.
    DeferImageLoading = 1u << 0,
    /// Disable automatic clipping of top-level rendering to composition bounds.
    DisableTopLevelClipping = 1u << 1,
};

/// Flag constant alias matching roadmap naming conventions.
inline constexpr uint32_t kDeferImageLoading = static_cast<uint32_t>(LoadFlags::DeferImageLoading);
inline constexpr uint32_t kDisableTopLevelClipping = static_cast<uint32_t>(LoadFlags::DisableTopLevelClipping);

/// Abstract interface representing a single-frame or multi-frame image asset (static image,
/// animated WebP, animated GIF, or host-supplied video/procedural frame stream).
class ImageAsset {
public:
    virtual ~ImageAsset() = default;

    /// Returns true if the asset produces different frames over time (animated WebP/GIF/video).
    [[nodiscard]] virtual bool is_multi_frame() const noexcept { return false; }

    /// Returns the image frame at local time `t_seconds` (decoding lazily if deferred).
    [[nodiscard]] virtual std::optional<PixmapRef> get_frame(float t_seconds) = 0;

    /// Returns an opaque monotonic token identifying the frame selected at `t_seconds`.
    /// Used by incremental damage tracking to detect frame transitions in multi-frame assets.
    [[nodiscard]] virtual uint64_t frame_token(float t_seconds) const noexcept {
        (void)t_seconds;
        return 0;
    }

    /// Returns intrinsic pixel width if known (0 if not yet decoded and not probed).
    [[nodiscard]] virtual uint32_t width() const noexcept { return 0; }

    /// Returns intrinsic pixel height if known (0 if not yet decoded and not probed).
    [[nodiscard]] virtual uint32_t height() const noexcept { return 0; }

    /// Returns true if underlying pixel data has been decoded into memory.
    [[nodiscard]] virtual bool is_decoded() const noexcept { return true; }
};

/// Built-in image asset supporting static images (PNG/JPEG/WebP/QOI/BMP/PPM/HDR),
/// animated WebP, animated GIF (GIF87a/GIF89a), custom multi-frame sequences,
/// and lazy deferred decoding (`kDeferImageLoading`).
class DecodedImageAsset final : public ImageAsset {
public:
    struct TimedFrame {
        Pixmap pixmap;
        float duration_seconds = 1.0f / 24.0f;
    };

    /// Wraps an already-decoded single Pixmap.
    [[nodiscard]] static std::shared_ptr<DecodedImageAsset> from_pixmap(Pixmap pixmap);

    /// Wraps a sequence of timed frames (e.g. procedural animation or video frames).
    [[nodiscard]] static std::shared_ptr<DecodedImageAsset> from_frames(std::vector<TimedFrame> frames);

    /// Constructs from raw encoded image bytes (PNG, JPEG, WebP, animated WebP, GIF, QOI, BMP, PPM, HDR).
    /// When `defer_decode` is true, bytes are retained and decoded lazily on first `get_frame()`.
    [[nodiscard]] static std::shared_ptr<DecodedImageAsset> from_encoded_bytes(
        std::vector<uint8_t> encoded_bytes,
        bool defer_decode = false
    );

    /// Constructs from a lazy byte loader callback invoked only when decoded.
    [[nodiscard]] static std::shared_ptr<DecodedImageAsset> from_lazy_loader(
        std::function<std::vector<uint8_t>()> loader,
        bool defer_decode = true
    );

    [[nodiscard]] bool is_multi_frame() const noexcept override;
    [[nodiscard]] std::optional<PixmapRef> get_frame(float t_seconds) override;
    [[nodiscard]] uint64_t frame_token(float t_seconds) const noexcept override;
    [[nodiscard]] uint32_t width() const noexcept override;
    [[nodiscard]] uint32_t height() const noexcept override;
    [[nodiscard]] bool is_decoded() const noexcept override { return decoded_; }

    /// Returns number of decoded frames (0 before lazy decode finishes).
    [[nodiscard]] size_t frame_count() const noexcept { return frames_.size(); }

    /// Returns total animation duration in seconds for multi-frame assets.
    [[nodiscard]] float total_duration_seconds() const noexcept { return total_duration_; }

private:
    void ensure_decoded() const;
    [[nodiscard]] size_t select_frame_index(float t_seconds) const noexcept;
    void decode_bytes_locked(std::span<const uint8_t> data) const;
    void probe_header_locked(std::span<const uint8_t> data);

    mutable std::vector<uint8_t> encoded_bytes_;
    mutable std::function<std::vector<uint8_t>()> lazy_loader_;
    mutable std::vector<TimedFrame> frames_;
    mutable std::vector<float> cumulative_times_;
    mutable float total_duration_ = 0.0f;
    mutable uint32_t probed_width_ = 0;
    mutable uint32_t probed_height_ = 0;
    mutable bool probed_multi_frame_ = false;
    mutable bool decoded_ = false;
};

/// Abstract interface for audio resources referenced by Lottie audio assets/layers.
class AudioAsset {
public:
    virtual ~AudioAsset() = default;

    /// Seeks audio playback to local time `t_seconds`.
    virtual void seek(float t_seconds) { (void)t_seconds; }

    /// Updates playback gain/volume (normalized, typically 0.0..1.0).
    virtual void set_volume(float volume) { (void)volume; }

    /// Optional access to raw audio bytes if loaded from memory/VFS.
    [[nodiscard]] virtual std::span<const uint8_t> raw_data() const noexcept { return {}; }
};

/// Simple in-memory AudioAsset holding raw audio stream bytes.
class MemoryAudioAsset final : public AudioAsset {
public:
    explicit MemoryAudioAsset(std::vector<uint8_t> data) : data_(std::move(data)) {}

    [[nodiscard]] std::span<const uint8_t> raw_data() const noexcept override { return data_; }

private:
    std::vector<uint8_t> data_;
};

/// Pluggable external resource loader interface for images, fonts, and audio.
/// Allows loading from memory buffers, virtual filesystems, archives, or network
/// without touching the host OS filesystem.
class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;

    /// Generic binary resource loader. Override this to back all default asset loaders
    /// from a custom VFS, bundle, or memory store.
    [[nodiscard]] virtual std::vector<uint8_t> load(
        std::string_view resource_path,
        std::string_view resource_name
    ) {
        (void)resource_path;
        (void)resource_name;
        return {};
    }

    /// Loads an image asset referenced by `"u"` (`resource_path`), `"p"` (`resource_name`), and `"id"`.
    [[nodiscard]] virtual std::shared_ptr<ImageAsset> load_image_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    );

    /// Loads TrueType/OpenType font bytes for a font descriptor.
    [[nodiscard]] virtual std::vector<uint8_t> load_font(
        std::string_view font_name,
        std::string_view font_url
    );

    /// Loads an audio asset referenced by `"u"`, `"p"`, and `"id"`.
    [[nodiscard]] virtual std::shared_ptr<AudioAsset> load_audio_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    );

    /// Controls whether default `load_image_asset` defers pixel decoding.
    void set_defer_image_decoding(bool defer) noexcept { defer_image_decoding_ = defer; }
    [[nodiscard]] bool defer_image_decoding() const noexcept { return defer_image_decoding_; }

protected:
    bool defer_image_decoding_ = false;
};

/// Filesystem-backed `ResourceProvider` rooted at `base_dir`.
class FileResourceProvider final : public ResourceProvider {
public:
    explicit FileResourceProvider(std::string base_dir = "", bool defer_decode = false);

    [[nodiscard]] std::vector<uint8_t> load(
        std::string_view resource_path,
        std::string_view resource_name
    ) override;

    [[nodiscard]] std::shared_ptr<ImageAsset> load_image_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    ) override;

    [[nodiscard]] std::vector<uint8_t> load_font(
        std::string_view font_name,
        std::string_view font_url
    ) override;

private:
    std::string base_dir_;
};

/// Pure in-memory / VFS `ResourceProvider` that never touches the host filesystem.
class MemoryResourceProvider final : public ResourceProvider {
public:
    explicit MemoryResourceProvider(bool defer_decode = false) {
        defer_image_decoding_ = defer_decode;
    }

    /// Registers a raw binary blob under a path/name key (e.g. `"images/hero.png"` or `"hero.png"`).
    void add_resource(std::string key, std::vector<uint8_t> bytes);

    /// Registers a custom pre-built `ImageAsset` by asset ID (`"id"`) or filename (`"p"`).
    void add_image_asset(std::string id_or_name, std::shared_ptr<ImageAsset> asset);

    /// Registers font binary data by font name (`"fName"`) or URL (`"fPath"`).
    void add_font(std::string name_or_url, std::vector<uint8_t> ttf_bytes);

    /// Registers a custom `AudioAsset` by asset ID or filename.
    void add_audio_asset(std::string id_or_name, std::shared_ptr<AudioAsset> asset);

    [[nodiscard]] std::vector<uint8_t> load(
        std::string_view resource_path,
        std::string_view resource_name
    ) override;

    [[nodiscard]] std::shared_ptr<ImageAsset> load_image_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    ) override;

    [[nodiscard]] std::vector<uint8_t> load_font(
        std::string_view font_name,
        std::string_view font_url
    ) override;

    [[nodiscard]] std::shared_ptr<AudioAsset> load_audio_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    ) override;

private:
    std::unordered_map<std::string, std::vector<uint8_t>> blobs_;
    std::unordered_map<std::string, std::shared_ptr<ImageAsset>> images_;
    std::unordered_map<std::string, std::vector<uint8_t>> fonts_;
    std::unordered_map<std::string, std::shared_ptr<AudioAsset>> audio_;
};

/// Proxy provider that intercepts RFC 2397 `data:...;base64,...` URIs for images, fonts, and audio,
/// forwarding all non-data-URI lookups to the wrapped fallback provider.
class DataURIResourceProviderProxy final : public ResourceProvider {
public:
    explicit DataURIResourceProviderProxy(
        std::shared_ptr<ResourceProvider> fallback = nullptr,
        bool defer_decode = false
    );

    [[nodiscard]] std::vector<uint8_t> load(
        std::string_view resource_path,
        std::string_view resource_name
    ) override;

    [[nodiscard]] std::shared_ptr<ImageAsset> load_image_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    ) override;

    [[nodiscard]] std::vector<uint8_t> load_font(
        std::string_view font_name,
        std::string_view font_url
    ) override;

    [[nodiscard]] std::shared_ptr<AudioAsset> load_audio_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    ) override;

private:
    std::shared_ptr<ResourceProvider> fallback_;
};

/// Decodes RFC 4648 base64 payloads (shared helper for data URIs).
[[nodiscard]] std::vector<uint8_t> decode_base64_bytes(std::string_view input);

} // namespace nisaba::lottie

