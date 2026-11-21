#pragma once

/// @file animation.hpp
/// @brief Root Lottie & dotLottie (.lottie ZIP) animation document, builder, dirty tracking, and dynamic overrides.

#include "nisaba/lottie/layer.hpp"
#include "nisaba/lottie/invalidation.hpp"
#include "nisaba/lottie/resources.hpp"
#include "nisaba/lottie/external_layer.hpp"
#include "nisaba/lottie/expression.hpp"
#include "nisaba/lottie/audio.hpp"
#include "nisaba/lottie/diagnostics.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/text/font_system.hpp"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <span>

namespace nisaba::lottie {

struct Marker {
    std::string name;
    float time = 0.0f;
    float duration = 0.0f;
};

class Animation {
public:
    /// Fluent builder for configuring resource providers, deferred image loading,
    /// diagnostic logging, build statistics, and O(1) property observers.
    class Builder {
    public:
        explicit Builder(uint32_t flags = 0) noexcept : flags_(flags) {}

        Builder& set_flags(uint32_t flags) noexcept {
            flags_ = flags;
            return *this;
        }

        Builder& set_resource_provider(std::shared_ptr<ResourceProvider> provider) {
            resource_provider_ = std::move(provider);
            return *this;
        }

        Builder& set_logger(std::shared_ptr<Logger> logger) {
            logger_ = std::move(logger);
            return *this;
        }

        Builder& set_stats(Stats* out_stats) noexcept {
            out_stats_ = out_stats;
            return *this;
        }

        Builder& set_property_observer(std::shared_ptr<PropertyObserver> observer) {
            property_observer_ = std::move(observer);
            return *this;
        }

        Builder& set_font_system(std::shared_ptr<text::FontSystem> fs) {
            font_system_ = std::move(fs);
            return *this;
        }

        Builder& set_precomp_interceptor(std::shared_ptr<PrecompInterceptor> interceptor) {
            precomp_interceptor_ = std::move(interceptor);
            return *this;
        }

        Builder& set_expression_manager(std::shared_ptr<ExpressionManager> manager) {
            expression_manager_ = std::move(manager);
            return *this;
        }

        [[nodiscard]] std::shared_ptr<Animation> make(
            std::string_view content,
            std::string_view base_dir = ""
        ) const;

        [[nodiscard]] std::shared_ptr<Animation> make_from_bytes(
            std::span<const uint8_t> bytes,
            std::string_view base_dir = ""
        ) const;

        [[nodiscard]] std::shared_ptr<Animation> make_from_file(
            const std::string& filepath
        ) const;

    private:
        uint32_t flags_ = 0;
        std::shared_ptr<ResourceProvider> resource_provider_;
        std::shared_ptr<Logger> logger_;
        Stats* out_stats_ = nullptr;
        std::shared_ptr<PropertyObserver> property_observer_;
        std::shared_ptr<text::FontSystem> font_system_;
        std::shared_ptr<PrecompInterceptor> precomp_interceptor_;
        std::shared_ptr<ExpressionManager> expression_manager_;
    };

    Animation() : mutation_gen_(std::make_shared<std::atomic<uint64_t>>(0)) {}
    ~Animation() = default;

    /// @brief Parse a Lottie animation from a JSON string or binary .lottie (ZIP) memory buffer.
    [[nodiscard]] static std::shared_ptr<Animation> load_from_data(std::string_view content, std::string_view base_dir = "");

    /// @brief Parse a Lottie or .lottie animation from raw binary bytes.
    [[nodiscard]] static std::shared_ptr<Animation> load_from_bytes(std::span<const uint8_t> bytes, std::string_view base_dir = "");

    /// @brief Load and parse a Lottie (.json) or dotLottie (.lottie) animation from disk.
    [[nodiscard]] static std::shared_ptr<Animation> load_from_file(const std::string& filepath);

    // ── Frame Seeking & Incremental Damage Tracking ───────────────────────

    /// @brief Seek animation to `frame`, lazily warming visible deferred image assets
    ///        and recording per-layer dirty regions into `ic` (when non-null).
    void seek_frame(float frame, InvalidationController* ic = nullptr);

    /// @brief Convenience overload returning the minimal dirty bounding box in `*out_dirty_rect`.
    /// @return True if any visible region changed (and `*out_dirty_rect` was populated).
    bool seek_frame(float frame, Rect* out_dirty_rect);

    /// @brief Resets cached per-layer damage state so the next `seek_frame` treats all active layers as dirty.
    void reset_invalidation_state() noexcept;

    /// @brief Returns the last frame passed to `seek_frame()`.
    [[nodiscard]] float sought_frame() const noexcept { return current_sought_frame_; }

    /// @brief Render animation at the last sought frame directly to canvas (1:1 scale).
    void render(ICanvas& canvas) const { render(canvas, current_sought_frame_); }

    /// @brief Render animation at a specific frame directly to canvas (1:1 scale).
    void render(ICanvas& canvas, float frame) const;

    /// @brief Render animation at a specific frame fitted into a destination rectangle.
    void render(ICanvas& canvas, float frame, const Rect& dest_rect, bool preserve_aspect_ratio = true) const;

    /// @brief Dispatches synchronized audio playback state for all AudioLayers at `frame`.
    void dispatch_audio(float frame, AudioTrack& track, bool is_playing) const;

    /// @brief Notifies `track` to stop all AudioLayers in this animation.
    void stop_audio(AudioTrack& track) const;

    /// @brief Attach a FontSystem for TrueType/OpenType text shaping in TextLayers.
    void set_font_system(std::shared_ptr<text::FontSystem> fs);
    [[nodiscard]] std::shared_ptr<text::FontSystem> font_system() const noexcept { return assets_.font_system; }

    // ── Dynamic Property Overrides & Slots ────────────────────────────────

    /// @brief Override fill/stroke/solid color by keypath ("LayerName.GroupName.FillName" or "*").
    bool set_color_override(std::string_view keypath, Color color);

    /// @brief Override opacity [0..100], stroke width, or rotation by keypath.
    bool set_scalar_override(std::string_view keypath, float value);

    /// @brief Override position or scale by keypath.
    bool set_point_override(std::string_view keypath, Point value);

    /// @brief Override text string on a TextLayer by layer name (or "*" for all text layers).
    bool set_text_override(std::string_view layer_name, std::string text);

    /// @brief Override a named Slot ID ("sid") color across the entire document.
    bool set_slot_color(std::string_view slot_id, Color color);

    /// @brief Clear all dynamic runtime overrides and restore original keyframe/static values.
    void clear_overrides();

    /// @brief Hit-test point (x, y) in composition coordinates at a given frame.
    /// @return Name of the topmost hit layer/shape, or std::nullopt if nothing hit.
    [[nodiscard]] std::optional<std::string> hit_test(float x, float y, float frame) const;

    /// @brief Find a named marker in the animation document.
    [[nodiscard]] const Marker* find_marker(std::string_view marker_name) const noexcept;

    // Document properties
    [[nodiscard]] std::string_view version() const noexcept { return version_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return height_; }
    [[nodiscard]] float in_point() const noexcept { return in_point_; }
    [[nodiscard]] float out_point() const noexcept { return out_point_; }
    [[nodiscard]] float frame_rate() const noexcept { return frame_rate_; }
    [[nodiscard]] uint32_t load_flags() const noexcept { return load_flags_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] float total_frames() const noexcept {
        return std::max(0.0f, out_point_ - in_point_);
    }

    [[nodiscard]] float duration_seconds() const noexcept {
        return (frame_rate_ > 0.0f) ? (total_frames() / frame_rate_) : 0.0f;
    }

    [[nodiscard]] const std::vector<std::shared_ptr<LottieLayer>>& layers() const noexcept {
        return layers_;
    }

    [[nodiscard]] const std::vector<Marker>& markers() const noexcept {
        return markers_;
    }

    [[nodiscard]] const LottieAssetRegistry& assets() const noexcept {
        return assets_;
    }

private:
    struct LayerDamageEntry {
        bool was_active = false;
        std::optional<Rect> prev_bounds;
        uint64_t prev_fingerprint = 0;
        uint64_t prev_mutation_gen = 0;
    };

    void warm_visible_deferred_assets(
        const std::vector<std::shared_ptr<LottieLayer>>& layers,
        float frame
    ) const;

    void bump_mutation_gen() noexcept {
        if (mutation_gen_) {
            mutation_gen_->fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::string version_;
    std::string name_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float in_point_ = 0.0f;
    float out_point_ = 0.0f;
    float frame_rate_ = 60.0f;
    uint32_t load_flags_ = 0;
    Stats stats_{};

    std::vector<std::shared_ptr<LottieLayer>> layers_;
    LottieAssetRegistry assets_;
    std::vector<Marker> markers_;

    // Incremental invalidation & handle mutation tracking
    std::shared_ptr<std::atomic<uint64_t>> mutation_gen_;
    float current_sought_frame_ = 0.0f;
    bool has_sought_once_ = false;
    std::vector<LayerDamageEntry> layer_damage_;
    mutable std::unordered_set<const AudioLayer*> active_audio_layers_;
};

using AnimationBuilder = Animation::Builder;

} // namespace nisaba::lottie
