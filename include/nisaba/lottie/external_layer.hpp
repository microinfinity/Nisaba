#pragma once

/// @file external_layer.hpp
/// @brief External layer rendering and Precomp interception interfaces for embedding live host content in Lottie.

#include "nisaba/canvas/canvas_interface.hpp"
#include "nisaba/math/size.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace nisaba::lottie {

/// Abstract external renderable injected into a Lottie Precomp slot.
/// Host applications (e.g., reactive UI widgets, live video decoders, camera feeds,
/// or custom 3D viewports) implement this interface so their content inherits the
/// animation's transform, clip bounds, masks, track mattes, effects, and opacity hierarchy.
class ExternalLayer {
public:
    virtual ~ExternalLayer() = default;

    /// Renders custom content into `canvas` at local precomp time `t_seconds`.
    /// The canvas transform and clip bounds are already configured for the Precomp layer.
    virtual void render(ICanvas& canvas, float t_seconds) = 0;

    /// Returns true if the external content may change across frames (default: true).
    [[nodiscard]] virtual bool is_animated() const noexcept { return true; }

    /// Returns a monotonic version token for incremental dirty-region tracking.
    /// Override to avoid redundant invalidation when live content is momentarily static.
    [[nodiscard]] virtual uint64_t content_version(float t_seconds) const noexcept {
        return static_cast<uint64_t>(t_seconds * 1000.0f) + 1u;
    }
};

/// Convenience `ExternalLayer` adapter backed by a callable.
class CallbackExternalLayer final : public ExternalLayer {
public:
    using RenderFn = std::function<void(ICanvas&, float)>;

    explicit CallbackExternalLayer(RenderFn fn, bool animated = true)
        : fn_(std::move(fn)), animated_(animated) {}

    void render(ICanvas& canvas, float t_seconds) override {
        if (fn_) fn_(canvas, t_seconds);
    }

    [[nodiscard]] bool is_animated() const noexcept override { return animated_; }

private:
    RenderFn fn_;
    bool animated_ = true;
};

/// Interceptor notified while building Precomp layers so host applications can replace
/// any precomposition (by asset `id` or layer `name`) with a custom `ExternalLayer`.
class PrecompInterceptor {
public:
    virtual ~PrecompInterceptor() = default;

    /// Called for each Precomp layer encountered during document loading.
    /// Return a non-null `ExternalLayer` to intercept rendering of this precomp,
    /// or `nullptr` to fall back to standard Lottie precomp layers.
    [[nodiscard]] virtual std::shared_ptr<ExternalLayer> on_load_precomp(
        std::string_view id,
        std::string_view name,
        Size size
    ) = 0;
};

/// Convenience `PrecompInterceptor` adapter backed by a callable.
class CallbackPrecompInterceptor final : public PrecompInterceptor {
public:
    using Callback = std::function<std::shared_ptr<ExternalLayer>(std::string_view, std::string_view, Size)>;

    explicit CallbackPrecompInterceptor(Callback cb) : cb_(std::move(cb)) {}

    [[nodiscard]] std::shared_ptr<ExternalLayer> on_load_precomp(
        std::string_view id,
        std::string_view name,
        Size size
    ) override {
        return cb_ ? cb_(id, name, size) : nullptr;
    }

private:
    Callback cb_;
};

} // namespace nisaba::lottie

