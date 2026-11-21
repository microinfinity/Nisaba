#pragma once

/// @file player.hpp
/// @brief Interactive playback controller, state machine, marker segments, dirty tracking, and hit-testing for Lottie animations.

#include "nisaba/lottie/animation.hpp"
#include "nisaba/lottie/invalidation.hpp"
#include "nisaba/lottie/audio.hpp"

#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace nisaba::lottie {

class Player {
public:
    explicit Player(std::shared_ptr<Animation> anim = nullptr);
    ~Player() = default;

    void set_animation(std::shared_ptr<Animation> anim);
    [[nodiscard]] std::shared_ptr<Animation> animation() const noexcept { return anim_; }

    void play() noexcept { is_playing_ = true; }
    void pause() noexcept { is_playing_ = false; }
    void stop() noexcept;

    void set_loop(bool loop) noexcept {
        loop_mode_ = loop ? LoopMode::Loop : LoopMode::Once;
    }
    void set_loop_mode(LoopMode mode) noexcept { loop_mode_ = mode; }
    [[nodiscard]] LoopMode loop_mode() const noexcept { return loop_mode_; }
    [[nodiscard]] bool is_looping() const noexcept { return loop_mode_ != LoopMode::Once; }
    [[nodiscard]] bool is_playing() const noexcept { return is_playing_; }

    void set_repeat_count(int count) noexcept { repeat_count_ = count; current_loops_ = 0; }
    [[nodiscard]] int repeat_count() const noexcept { return repeat_count_; }

    void set_speed(float speed) noexcept { speed_ = speed; }
    [[nodiscard]] float speed() const noexcept { return speed_; }

    /// @brief Attach an `AudioTrack` playback hook for synchronized `LayerType::Audio` layers.
    void set_audio_track(std::shared_ptr<AudioTrack> track) { audio_track_ = std::move(track); }
    [[nodiscard]] std::shared_ptr<AudioTrack> audio_track() const noexcept { return audio_track_; }

    /// @brief Restrict playback to a custom frame segment [start_frame, end_frame].
    void set_segment(float start_frame, float end_frame) noexcept;

    /// @brief Reset playback segment to the animation's full [in_point, out_point] range.
    void reset_segment() noexcept;

    /// @brief Play a named marker segment ("cm") defined in the Lottie document.
    bool play_marker(std::string_view marker_name) noexcept;

    /// @brief Seek directly to a frame number and optionally record dirty regions into `ic`.
    void seek_frame(float frame, InvalidationController* ic = nullptr) noexcept;

    /// @brief Seek directly to a frame number and output the minimal dirty bounding box into `*out_dirty_rect`.
    /// @return True if any visible region changed (and `*out_dirty_rect` was populated).
    bool seek_frame(float frame, Rect* out_dirty_rect) noexcept;

    /// @brief Seek to a normalized progress [0.0, 1.0] within the active segment.
    void seek_progress(float progress, InvalidationController* ic = nullptr) noexcept;

    /// @brief Current frame being displayed.
    [[nodiscard]] float current_frame() const noexcept { return current_frame_; }

    /// @brief Current normalized playback progress in [0.0, 1.0].
    [[nodiscard]] float progress() const noexcept;

    /// @brief Advance playback by delta time in seconds and optionally record dirty regions into `ic`.
    /// @return True if animation is still active/playing.
    bool advance(float delta_time_seconds, InvalidationController* ic = nullptr);

    /// @brief Advance playback by delta time in seconds and output minimal dirty bounding box into `*out_dirty_rect`.
    bool advance(float delta_time_seconds, Rect* out_dirty_rect);

    /// @brief Render current frame to canvas (1:1 scale).
    void render(ICanvas& canvas) const;

    /// @brief Render current frame fitted into destination bounds.
    void render(ICanvas& canvas, const Rect& dest_rect, bool preserve_aspect_ratio = true) const;

    /// @brief Hit-test point (x, y) in composition coordinates at the current frame.
    [[nodiscard]] std::optional<std::string> hit_test(float x, float y) const;

    // ── Dynamic Overrides Shortcuts ───────────────────────────────────────

    bool set_color_override(std::string_view keypath, Color color) {
        return anim_ ? anim_->set_color_override(keypath, color) : false;
    }
    bool set_scalar_override(std::string_view keypath, float value) {
        return anim_ ? anim_->set_scalar_override(keypath, value) : false;
    }
    bool set_point_override(std::string_view keypath, Point value) {
        return anim_ ? anim_->set_point_override(keypath, value) : false;
    }
    bool set_text_override(std::string_view layer_name, std::string text) {
        return anim_ ? anim_->set_text_override(layer_name, std::move(text)) : false;
    }
    bool set_slot_color(std::string_view slot_id, Color color) {
        return anim_ ? anim_->set_slot_color(slot_id, color) : false;
    }
    void clear_overrides() {
        if (anim_) anim_->clear_overrides();
    }

    // ── Event Callbacks ───────────────────────────────────────────────────

    std::function<void()> on_complete;
    std::function<void(int)> on_loop;
    std::function<void(float)> on_frame;
    std::function<void(std::string_view)> on_marker;

private:
    [[nodiscard]] float effective_in_point() const noexcept;
    [[nodiscard]] float effective_out_point() const noexcept;

    std::shared_ptr<Animation> anim_;
    bool is_playing_ = true;
    LoopMode loop_mode_ = LoopMode::Loop;
    int repeat_count_ = -1; // -1 = infinite when looping
    int current_loops_ = 0;
    float speed_ = 1.0f;
    float direction_ = 1.0f; // Used by LoopMode::Bounce
    float current_frame_ = 0.0f;
    std::optional<float> segment_in_;
    std::optional<float> segment_out_;
    std::shared_ptr<AudioTrack> audio_track_;
};

} // namespace nisaba::lottie
