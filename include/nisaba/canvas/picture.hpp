#pragma once

/// @file picture.hpp
/// @brief Retained-mode display lists (Picture, PictureRecorder) and dynamic Drawable subtrees.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "nisaba/canvas/canvas_interface.hpp"

namespace nisaba {

/// Immutable retained display list of recorded 2D/3D canvas commands.
/// Safe to replay concurrently across multiple frames and canvas backends (CPU, GPU, PDF).
class Picture {
public:
    struct Impl;

    explicit Picture(std::shared_ptr<Impl> impl) noexcept;
    ~Picture();

    Picture(const Picture&) = default;
    Picture& operator=(const Picture&) = default;
    Picture(Picture&&) noexcept = default;
    Picture& operator=(Picture&&) noexcept = default;

    /// Replays all recorded commands onto the target canvas.
    void playback(ICanvas& canvas) const;

    /// Conservative local-space culling bounds supplied when recording began.
    [[nodiscard]] Rect cull_rect() const noexcept;

    /// Approximate number of recorded drawing and state commands.
    [[nodiscard]] size_t approximate_op_count() const noexcept;

    /// Approximate memory footprint in bytes (commands + owned resources).
    [[nodiscard]] size_t approximate_bytes_used() const noexcept;

    [[nodiscard]] bool is_empty() const noexcept { return approximate_op_count() == 0; }

private:
    std::shared_ptr<Impl> impl_{};
};

/// Abstract interface for dynamic procedural subtrees that can draw directly to any ICanvas,
/// be snapshotted into an immutable Picture, or be embedded by reference inside a Picture.
class Drawable {
public:
    virtual ~Drawable() = default;

    void draw(ICanvas& canvas, const Transform* transform = nullptr);

    [[nodiscard]] Rect bounds() const;

    /// Captures the current visual output of this Drawable as an immutable Picture.
    [[nodiscard]] std::shared_ptr<Picture> make_picture_snapshot();

    [[nodiscard]] uint32_t generation_id() const noexcept { return generation_id_; }

    /// Marks cached snapshots stale by bumping the generation counter.
    void notify_drawing_changed() noexcept {
        ++generation_id_;
        if (generation_id_ == 0) generation_id_ = 1;
    }

protected:
    virtual Rect on_get_bounds() const = 0;
    virtual void on_draw(ICanvas& canvas) = 0;

private:
    uint32_t generation_id_{1};
};

/// Records canvas commands into an immutable Picture for zero-overhead multi-frame replay.
class PictureRecorder {
public:
    PictureRecorder();
    ~PictureRecorder();

    PictureRecorder(const PictureRecorder&) = delete;
    PictureRecorder& operator=(const PictureRecorder&) = delete;
    PictureRecorder(PictureRecorder&&) noexcept;
    PictureRecorder& operator=(PictureRecorder&&) noexcept;

    /// Starts a new recording session with the specified culling bounds and returns
    /// the recording ICanvas reference.
    ICanvas& begin_recording(const Rect& bounds);

    /// Returns the active recording canvas, or nullptr if not currently recording.
    [[nodiscard]] ICanvas* recording_canvas() noexcept;

    /// Finishes the current recording session and produces an immutable Picture.
    [[nodiscard]] std::shared_ptr<Picture> finish_recording();

private:
    struct RecordingCanvas;
    std::unique_ptr<RecordingCanvas> recorder_{};
};

} // namespace nisaba

