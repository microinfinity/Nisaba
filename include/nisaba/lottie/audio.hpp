#pragma once

/// @file audio.hpp
/// @brief Synchronized AudioTrack playback hook for Lottie AudioLayer ("ty": 6).

#include "nisaba/lottie/resources.hpp"

#include <cmath>
#include <functional>
#include <memory>
#include <string_view>

namespace nisaba::lottie {

/// Converts decibel audio level (`"au.lv"`) to linear amplitude gain (`0 dB -> 1.0`, `<= -96 dB -> 0.0`).
[[nodiscard]] inline float audio_db_to_linear_gain(float db) noexcept {
    if (db <= -96.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

/// Playback hook notified by `Player` during `advance()`, `seek_frame()`, and `stop()`
/// to drive synchronized host audio mixers for `LayerType::Audio` layers.
class AudioTrack {
public:
    virtual ~AudioTrack() = default;

    /// Invoked for each active `AudioLayer` at the current animation frame.
    /// @param asset_id Audio asset reference ID (`"refId"`).
    /// @param local_time_seconds Synchronized local playback timestamp in seconds.
    /// @param left_db Left channel level in decibels (animated via `"au.lv"`).
    /// @param right_db Right channel level in decibels (animated via `"au.lv"`).
    /// @param linear_gain Combined linear volume gain (`audio_db_to_linear_gain(avg_db)`).
    /// @param is_playing True when called during active playback, false when paused/scrubbing.
    /// @param asset Optional resolved `AudioAsset` from `ResourceProvider` (may be null).
    virtual void on_audio_frame(
        std::string_view asset_id,
        float local_time_seconds,
        float left_db,
        float right_db,
        float linear_gain,
        bool is_playing,
        const std::shared_ptr<AudioAsset>& asset
    ) = 0;

    /// Invoked when playback stops or an active `AudioLayer` exits its `[in_point, out_point)` window.
    virtual void on_audio_stop(std::string_view asset_id) {
        (void)asset_id;
    }
};

/// Convenience `AudioTrack` adapter backed by callbacks.
class CallbackAudioTrack final : public AudioTrack {
public:
    using FrameCallback = std::function<void(
        std::string_view, float, float, float, float, bool, const std::shared_ptr<AudioAsset>&
    )>;
    using StopCallback = std::function<void(std::string_view)>;

    explicit CallbackAudioTrack(FrameCallback on_frame, StopCallback on_stop = nullptr)
        : on_frame_(std::move(on_frame)), on_stop_(std::move(on_stop)) {}

    void on_audio_frame(
        std::string_view asset_id,
        float local_time_seconds,
        float left_db,
        float right_db,
        float linear_gain,
        bool is_playing,
        const std::shared_ptr<AudioAsset>& asset
    ) override {
        if (on_frame_) {
            on_frame_(asset_id, local_time_seconds, left_db, right_db, linear_gain, is_playing, asset);
        }
    }

    void on_audio_stop(std::string_view asset_id) override {
        if (on_stop_) {
            on_stop_(asset_id);
        }
    }

private:
    FrameCallback on_frame_;
    StopCallback on_stop_;
};

} // namespace nisaba::lottie

