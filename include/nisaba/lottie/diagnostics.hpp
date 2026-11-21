#pragma once

/// @file diagnostics.hpp
/// @brief Diagnostics logger, build statistics, and O(1) typed property observer handles for Lottie.

#include "nisaba/lottie/lottie_types.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/math/point.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace nisaba::lottie {

template <typename T>
class Property;

struct LottieTransform;
class SolidLayer;
class TextLayer;

// ── Diagnostics Logger ────────────────────────────────────────────────────

/// Diagnostic callback interface receiving JSON parse errors and unsupported feature warnings.
class Logger {
public:
    enum class Level : uint8_t {
        Warning = 0,
        Error = 1,
    };

    virtual ~Logger() = default;

    /// Invoked during document parsing and scene construction.
    virtual void log(Level level, std::string_view message, std::string_view json_context = {}) = 0;
};

/// Convenience `Logger` adapter backed by a callable.
class CallbackLogger final : public Logger {
public:
    using Callback = std::function<void(Level, std::string_view, std::string_view)>;

    explicit CallbackLogger(Callback cb) : callback_(std::move(cb)) {}

    void log(Level level, std::string_view message, std::string_view json_context = {}) override {
        if (callback_) {
            callback_(level, message, json_context);
        }
    }

private:
    Callback callback_;
};

// ── Build & Parse Statistics ──────────────────────────────────────────────

/// Performance and structural metrics populated during `Animation::Builder::make(...)`.
struct Stats {
    double json_parse_ms = 0.0;
    double scene_build_ms = 0.0;
    size_t animator_count = 0;
    size_t layer_count = 0;
    size_t json_size_bytes = 0;
    size_t precomp_count = 0;
    size_t image_asset_count = 0;
};

// ── Typed O(1) Runtime Property Handles ───────────────────────────────────

/// Direct O(1) handle for mutating a fill, stroke, or solid layer color without keypath lookups.
class ColorHandle {
public:
    ColorHandle() = default;

    [[nodiscard]] bool is_valid() const noexcept { return !owner_.expired() && (color_prop_ != nullptr || solid_layer_ != nullptr); }
    [[nodiscard]] Color get() const noexcept;
    void set(Color color);
    void clear();

private:
    friend class Animation;
    friend struct PropertyObserverBridge;

    ColorHandle(
        std::weak_ptr<void> owner,
        Property<Color>* color_prop,
        std::shared_ptr<std::atomic<uint64_t>> mutation_gen
    ) noexcept
        : owner_(std::move(owner)), color_prop_(color_prop), mutation_gen_(std::move(mutation_gen)) {}

    ColorHandle(
        std::weak_ptr<void> owner,
        SolidLayer* solid_layer,
        std::shared_ptr<std::atomic<uint64_t>> mutation_gen
    ) noexcept
        : owner_(std::move(owner)), solid_layer_(solid_layer), mutation_gen_(std::move(mutation_gen)) {}

    std::weak_ptr<void> owner_;
    Property<Color>* color_prop_ = nullptr;
    SolidLayer* solid_layer_ = nullptr;
    std::shared_ptr<std::atomic<uint64_t>> mutation_gen_;
};

/// Direct O(1) handle for mutating a layer, group, fill, or stroke opacity (`[0..100]`).
class OpacityHandle {
public:
    OpacityHandle() = default;

    [[nodiscard]] bool is_valid() const noexcept { return !owner_.expired() && opacity_prop_ != nullptr; }
    [[nodiscard]] float get() const noexcept;
    void set(float opacity_0_to_100);
    void clear();

private:
    friend class Animation;
    friend struct PropertyObserverBridge;

    OpacityHandle(
        std::weak_ptr<void> owner,
        Property<float>* opacity_prop,
        std::shared_ptr<std::atomic<uint64_t>> mutation_gen
    ) noexcept
        : owner_(std::move(owner)), opacity_prop_(opacity_prop), mutation_gen_(std::move(mutation_gen)) {}

    std::weak_ptr<void> owner_;
    Property<float>* opacity_prop_ = nullptr;
    std::shared_ptr<std::atomic<uint64_t>> mutation_gen_;
};

/// Snapshot of editable transform parameters for `TransformHandle`.
struct TransformPropertyValue {
    Point anchor = Point::zero();
    Point position = Point::zero();
    Point scale = Point::from_xy(100.0f, 100.0f);
    float rotation = 0.0f;
    float skew = 0.0f;
    float skew_axis = 0.0f;
};

/// Direct O(1) handle for mutating a layer or shape-group transform without keypath lookups.
class TransformHandle {
public:
    TransformHandle() = default;

    [[nodiscard]] bool is_valid() const noexcept { return !owner_.expired() && transform_ != nullptr; }
    [[nodiscard]] TransformPropertyValue get() const noexcept;
    void set(const TransformPropertyValue& value);
    void set_anchor(Point anchor);
    void set_position(Point position);
    void set_scale(Point scale);
    void set_rotation(float degrees);
    void set_skew(float skew_deg, float skew_axis_deg = 0.0f);
    void clear();

private:
    friend class Animation;
    friend struct PropertyObserverBridge;

    TransformHandle(
        std::weak_ptr<void> owner,
        LottieTransform* transform,
        std::shared_ptr<std::atomic<uint64_t>> mutation_gen
    ) noexcept
        : owner_(std::move(owner)), transform_(transform), mutation_gen_(std::move(mutation_gen)) {}

    std::weak_ptr<void> owner_;
    LottieTransform* transform_ = nullptr;
    std::shared_ptr<std::atomic<uint64_t>> mutation_gen_;
};

/// Snapshot of editable typography parameters for `TextHandle`.
struct TextPropertyValue {
    std::string text;
    std::string font_name;
    float font_size = 24.0f;
    Color fill_color = Color::BLACK;
    Color stroke_color = Color::BLACK;
    float stroke_width = 0.0f;
    bool stroke_over_fill = false;
    TextJustify justify = TextJustify::Left;
    float tracking = 0.0f;
    float line_height = 28.8f;
    float baseline_shift = 0.0f;
};

/// Direct O(1) handle for mutating a `TextLayer`'s text and styling at runtime.
class TextHandle {
public:
    TextHandle() = default;

    [[nodiscard]] bool is_valid() const noexcept { return !owner_.expired() && text_layer_ != nullptr; }
    [[nodiscard]] TextPropertyValue get() const;
    void set(const TextPropertyValue& value);
    void set_text(std::string text);
    void set_fill_color(Color color);
    void set_font_size(float size);
    void clear();

private:
    friend class Animation;
    friend struct PropertyObserverBridge;

    TextHandle(
        std::weak_ptr<void> owner,
        TextLayer* text_layer,
        std::shared_ptr<std::atomic<uint64_t>> mutation_gen
    ) noexcept
        : owner_(std::move(owner)), text_layer_(text_layer), mutation_gen_(std::move(mutation_gen)) {}

    std::weak_ptr<void> owner_;
    TextLayer* text_layer_ = nullptr;
    std::shared_ptr<std::atomic<uint64_t>> mutation_gen_;
};

// ── PropertyObserver ──────────────────────────────────────────────────────

/// Observer notified during scene construction to bind typed O(1) property handles.
/// `keypath` is formatted as dot-separated node names (e.g. `"LayerName.GroupName.Fill 1"`).
class PropertyObserver {
public:
    virtual ~PropertyObserver() = default;

    virtual void on_node_enter(std::string_view node_name) { (void)node_name; }
    virtual void on_node_leave(std::string_view node_name) { (void)node_name; }

    virtual void on_color_property(
        std::string_view keypath,
        const std::function<std::unique_ptr<ColorHandle>()>& lazy_handle
    ) {
        (void)keypath;
        (void)lazy_handle;
    }

    virtual void on_opacity_property(
        std::string_view keypath,
        const std::function<std::unique_ptr<OpacityHandle>()>& lazy_handle
    ) {
        (void)keypath;
        (void)lazy_handle;
    }

    virtual void on_transform_property(
        std::string_view keypath,
        const std::function<std::unique_ptr<TransformHandle>()>& lazy_handle
    ) {
        (void)keypath;
        (void)lazy_handle;
    }

    virtual void on_text_property(
        std::string_view keypath,
        const std::function<std::unique_ptr<TextHandle>()>& lazy_handle
    ) {
        (void)keypath;
        (void)lazy_handle;
    }
};

} // namespace nisaba::lottie

