#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <cmath>
#include <algorithm>
#include <array>
#include <memory>
#include <variant>
#include "nisaba/types.hpp"
#include "nisaba/math/scalar.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/canvas/pixmap.hpp"

namespace nisaba {

enum class SpreadMode {
    Pad,
    Reflect,
    Repeat,
};

enum class FilterQuality {
    Nearest,
    Bilinear,
    Bicubic,
};

struct GradientStop {
    NormalizedF32 position{NormalizedF32::ZERO};
    Color color{Color::BLACK};

    constexpr GradientStop() noexcept = default;
    constexpr GradientStop(NormalizedF32 pos, Color col) noexcept
        : position(pos), color(col) {}

    static constexpr GradientStop create(float pos, Color col) noexcept {
        return GradientStop(NormalizedF32::create_clamped(pos), col);
    }
};

class Gradient {
public:
    Gradient() noexcept = default;
    Gradient(
        std::vector<GradientStop> stops,
        SpreadMode mode,
        Transform transform,
        Transform points_to_unit
    );

    [[nodiscard]] const std::vector<GradientStop>& stops() const noexcept { return stops_; }
    [[nodiscard]] SpreadMode spread_mode() const noexcept { return tile_mode_; }
    [[nodiscard]] const Transform& transform() const noexcept { return transform_; }
    [[nodiscard]] const std::optional<Transform>& inv_combined() const noexcept { return inv_combined_; }
    [[nodiscard]] bool colors_are_opaque() const noexcept { return colors_are_opaque_; }

    void set_transform(const Transform& ts) noexcept;
    void post_concat(const Transform& ts) noexcept;
    void apply_opacity(float opacity) noexcept;

    [[nodiscard]] Color eval(float t) const noexcept;
    [[nodiscard]] float map_spread(float t) const noexcept;

    [[nodiscard]] bool has_uniform_alpha() const noexcept { return has_uniform_alpha_; }
    [[nodiscard]] uint8_t uniform_alpha() const noexcept { return uniform_alpha_; }

    static constexpr size_t RAMP_SIZE = 256;
    [[nodiscard]] inline PremultipliedColorU8 lookup_ramp(float t) const noexcept {
        float tm = map_spread(t);
        constexpr float scale = static_cast<float>(RAMP_SIZE - 1);
        int32_t idx = static_cast<int32_t>(tm * scale + 0.5f);
        if (idx < 0) idx = 0;
        else if (static_cast<size_t>(idx) >= RAMP_SIZE) idx = RAMP_SIZE - 1;
        return ramp_[static_cast<size_t>(idx)];
    }
    [[nodiscard]] const std::array<PremultipliedColorU8, RAMP_SIZE>& ramp() const noexcept { return ramp_; }

protected:
    std::vector<GradientStop> stops_{};
    SpreadMode tile_mode_{SpreadMode::Pad};
    Transform transform_{};
    Transform points_to_unit_{};
    std::optional<Transform> inv_combined_{};
    bool colors_are_opaque_{false};
    bool has_uniform_stops_{false};
    bool has_uniform_alpha_{false};
    uint8_t uniform_alpha_{255};
    std::array<PremultipliedColorU8, RAMP_SIZE> ramp_{};

    void update_inv() noexcept;
    void update_alpha_flags() noexcept;
    void bake_ramp() noexcept;
};

class LinearGradient : public Gradient {
public:
    LinearGradient() noexcept = default;

    static std::optional<LinearGradient> create(
        Point start,
        Point end,
        std::vector<GradientStop> stops,
        SpreadMode mode = SpreadMode::Pad,
        Transform transform = Transform()
    );

    LinearGradient(
        Point start,
        Point end,
        std::vector<GradientStop> stops,
        SpreadMode mode,
        Transform transform,
        Transform points_to_unit
    );

    [[nodiscard]] bool is_horizontal() const noexcept;
    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;

    void shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept;

    [[nodiscard]] Point start() const noexcept { return start_; }
    [[nodiscard]] Point end() const noexcept { return end_; }

private:
    Point start_{};
    Point end_{};
    float dx_{0.0f};
    float dy_{0.0f};
    float inv_len_sq_{0.0f};
};

class RadialGradient : public Gradient {
public:
    RadialGradient() noexcept = default;

    static std::optional<RadialGradient> create(
        Point center,
        float radius,
        std::vector<GradientStop> stops,
        SpreadMode mode = SpreadMode::Pad,
        Transform transform = Transform()
    );

    static std::optional<RadialGradient> create_2point(
        Point start,
        float start_radius,
        Point end,
        float end_radius,
        std::vector<GradientStop> stops,
        SpreadMode mode = SpreadMode::Pad,
        Transform transform = Transform()
    );

    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;
    void shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept;

    [[nodiscard]] bool is_simple_radial() const noexcept { return !is_2point_; }
    [[nodiscard]] bool is_2point() const noexcept { return is_2point_; }
    [[nodiscard]] float radius() const noexcept { return radius_; }
    [[nodiscard]] Point center() const noexcept { return center_; }
    [[nodiscard]] Point start() const noexcept { return start_; }
    [[nodiscard]] float start_radius() const noexcept { return start_radius_; }
    [[nodiscard]] Point end() const noexcept { return end_; }
    [[nodiscard]] float end_radius() const noexcept { return end_radius_; }
    [[nodiscard]] PremultipliedColorU8 pad_color() const noexcept { return pad_color_; }
    [[nodiscard]] static constexpr size_t ramp_sq_size() noexcept { return RAMP_SQ_SIZE; }
    [[nodiscard]] const std::array<PremultipliedColorU8, 1024>& ramp_sq() const noexcept { return ramp_sq_; }

private:
    Point center_{};
    float radius_{0.0f};
    float inv_radius_{0.0f};
    bool is_2point_{false};
    Point start_{};
    float start_radius_{0.0f};
    Point end_{};
    float end_radius_{0.0f};

    static constexpr size_t RAMP_SQ_SIZE = 1024;
    std::array<PremultipliedColorU8, RAMP_SQ_SIZE> ramp_sq_{};
    PremultipliedColorU8 pad_color_{};
};

class SweepGradient : public Gradient {
public:
    SweepGradient() noexcept = default;

    static std::optional<SweepGradient> create(
        Point center,
        float start_angle_deg,
        float end_angle_deg,
        std::vector<GradientStop> stops,
        SpreadMode mode = SpreadMode::Pad,
        Transform transform = Transform()
    );

    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;

    [[nodiscard]] Point center() const noexcept { return center_; }
    [[nodiscard]] float start_angle_rad() const noexcept { return start_angle_rad_; }
    [[nodiscard]] float inv_angle_range() const noexcept { return inv_angle_range_; }

private:
    Point center_{};
    float start_angle_rad_{0.0f};
    float inv_angle_range_{0.0f};
};

class Pattern {
public:
    Pattern() noexcept = default;
    Pattern(
        PixmapRef pixmap,
        SpreadMode mode = SpreadMode::Pad,
        FilterQuality quality = FilterQuality::Bilinear,
        float opacity = 1.0f,
        Transform transform = Transform()
    ) noexcept;

    [[nodiscard]] const PixmapRef& pixmap() const noexcept { return pixmap_; }
    [[nodiscard]] SpreadMode spread_mode() const noexcept { return mode_; }
    [[nodiscard]] FilterQuality quality() const noexcept { return quality_; }
    [[nodiscard]] float opacity() const noexcept { return opacity_; }
    [[nodiscard]] const Transform& transform() const noexcept { return transform_; }

    void set_transform(const Transform& ts) noexcept;
    void post_concat(const Transform& ts) noexcept;
    void apply_opacity(float opacity) noexcept;

    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;
    void shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept;
    [[nodiscard]] bool is_opaque() const noexcept { return is_opaque_; }
    [[nodiscard]] const std::optional<Transform>& inv_transform() const noexcept { return inv_ts_; }

private:
    PixmapRef pixmap_{};
    std::shared_ptr<const Pixmap> rgba_storage_{};
    SpreadMode mode_{SpreadMode::Pad};
    FilterQuality quality_{FilterQuality::Bilinear};
    float opacity_{1.0f};
    bool is_opaque_{false};
    Transform transform_{};
    std::optional<Transform> inv_ts_{};

    void update_inv() noexcept;
};

class TwoPointConicalGradient : public Gradient {
public:
    TwoPointConicalGradient() noexcept = default;

    static std::optional<TwoPointConicalGradient> create(
        Point start_center,
        float start_radius,
        Point end_center,
        float end_radius,
        std::vector<GradientStop> stops,
        SpreadMode mode = SpreadMode::Pad,
        Transform transform = Transform()
    );

    [[nodiscard]] Point start_center() const noexcept { return start_center_; }
    [[nodiscard]] float start_radius() const noexcept { return start_radius_; }
    [[nodiscard]] Point end_center() const noexcept { return end_center_; }
    [[nodiscard]] float end_radius() const noexcept { return end_radius_; }

    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;
    void shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept;

private:
    Point start_center_{};
    float start_radius_{0.0f};
    Point end_center_{};
    float end_radius_{0.0f};

    float dx_{0.0f};
    float dy_{0.0f};
    float dr_{0.0f};
    float A_{0.0f};
    bool is_concentric_{false};
};

class ComposeShader;

class Shader {
public:
    enum class Type {
        SolidColor,
        LinearGradient,
        RadialGradient,
        SweepGradient,
        Pattern,
        TwoPointConicalGradient,
        ComposeShader,
    };

    using ComplexShader = std::variant<
        std::monostate,
        std::shared_ptr<LinearGradient>,
        std::shared_ptr<RadialGradient>,
        std::shared_ptr<SweepGradient>,
        std::shared_ptr<Pattern>,
        std::shared_ptr<TwoPointConicalGradient>,
        std::shared_ptr<ComposeShader>
    >;

    Shader() noexcept;
    ~Shader();
    Shader(const Shader&);
    Shader(Shader&&) noexcept;
    Shader& operator=(const Shader&);
    Shader& operator=(Shader&&) noexcept;

    explicit Shader(Color color) noexcept;
    explicit Shader(LinearGradient grad);
    explicit Shader(RadialGradient grad);
    explicit Shader(SweepGradient grad);
    explicit Shader(Pattern patt);
    explicit Shader(TwoPointConicalGradient grad);
    explicit Shader(ComposeShader comp);

    static Shader from_color(Color color) noexcept { return Shader(color); }
    static Shader from_color_rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
        return Shader(Color::from_rgba8(r, g, b, a));
    }
    static Shader create_compose(Shader dst, Shader src, BlendMode mode);
    static Shader create_blend(Shader dst, Shader src, BlendMode mode) {
        return create_compose(std::move(dst), std::move(src), mode);
    }
    static Shader create_two_point_conical(
        Point start_center,
        float start_radius,
        Point end_center,
        float end_radius,
        std::vector<GradientStop> stops,
        SpreadMode mode = SpreadMode::Pad,
        Transform transform = Transform()
    );

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_solid_color() const noexcept { return type_ == Type::SolidColor; }
    [[nodiscard]] Color solid_color() const noexcept { return solid_color_; }

    [[nodiscard]] bool is_opaque() const noexcept;
    void apply_opacity(float opacity) noexcept;
    void transform(const Transform& ts) noexcept;

    [[nodiscard]] const LinearGradient& linear_gradient() const noexcept;
    [[nodiscard]] const RadialGradient& radial_gradient() const noexcept;
    [[nodiscard]] const SweepGradient& sweep_gradient() const noexcept;
    [[nodiscard]] const TwoPointConicalGradient& conical_gradient() const noexcept;
    [[nodiscard]] const Pattern& pattern() const noexcept;
    [[nodiscard]] const ComposeShader& compose_shader() const noexcept;

    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;
    void shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept;

    [[nodiscard]] bool has_uniform_alpha() const noexcept;
    [[nodiscard]] uint8_t uniform_alpha() const noexcept;
    [[nodiscard]] bool is_horizontal_gradient() const noexcept;

private:
    Type type_{Type::SolidColor};
    Color solid_color_{Color::BLACK};
    ComplexShader complex_{};
};

class ComposeShader {
public:
    ComposeShader() noexcept = default;
    ComposeShader(Shader dst, Shader src, BlendMode mode) noexcept
        : dst_(std::move(dst)), src_(std::move(src)), mode_(mode) {}

    static std::optional<ComposeShader> create(Shader dst, Shader src, BlendMode mode) noexcept {
        return ComposeShader(std::move(dst), std::move(src), mode);
    }

    [[nodiscard]] const Shader& dst_shader() const noexcept { return dst_; }
    [[nodiscard]] const Shader& src_shader() const noexcept { return src_; }
    [[nodiscard]] BlendMode blend_mode() const noexcept { return mode_; }

    [[nodiscard]] Color sample(float x, float y) const noexcept;
    [[nodiscard]] PremultipliedColor sample_premul(float x, float y) const noexcept;
    void shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst_span) const noexcept;

    void apply_opacity(float opacity) noexcept;
    void transform(const Transform& ts) noexcept;

    [[nodiscard]] bool is_opaque() const noexcept;

private:
    Shader dst_;
    Shader src_;
    BlendMode mode_{BlendMode::SourceOver};
};

} // namespace nisaba
