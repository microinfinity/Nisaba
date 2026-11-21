#pragma once

#include <cstdint>
#include <memory>
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/shaders/shader.hpp"
#include "nisaba/canvas/mask.hpp"
#include "nisaba/effects/color_matrix.hpp"
#include "nisaba/effects/color_filter.hpp"
#include "nisaba/effects/image_filter.hpp"
#include "nisaba/effects/mask_filter.hpp"
#include "nisaba/effects/path_effect.hpp"
#include "nisaba/effects/blender.hpp"

namespace nisaba {

namespace gpu {
    class Shader;
}

struct PixmapPaint {
    float opacity{1.0f};
    BlendMode blend_mode{BlendMode::SourceOver};
    FilterQuality quality{FilterQuality::Bilinear};
    Color tint{Color::WHITE};

    constexpr PixmapPaint() noexcept = default;
};

enum class PaintStyle : uint8_t {
    Fill,
    Stroke,
    StrokeAndFill,
};

enum class PointMode : uint8_t {
    Points,
    Lines,
    Polygon,
};

class Paint {
public:
    Shader shader{Shader::from_color(Color::BLACK)};
    BlendMode blend_mode{BlendMode::SourceOver};
    bool anti_alias{true};
    ColorSpace colorspace{ColorSpace::SimpleSRGB};
    bool force_hq_pipeline{false};
    std::optional<effects::ColorMatrix> color_filter{std::nullopt};
    std::optional<effects::ColorEffect> color_effect{std::nullopt};
    std::optional<effects::ImageFilter> image_filter{std::nullopt};
    std::optional<effects::MaskFilter> mask_filter{std::nullopt};
    std::optional<effects::PathEffect> path_effect{std::nullopt};
    std::optional<effects::Blender> blender{std::nullopt};
    PaintStyle style{PaintStyle::Fill};
    float stroke_width{1.0f};

    std::shared_ptr<gpu::Shader> custom_shader{nullptr};

    Paint() noexcept = default;

    explicit Paint(Color color) noexcept : shader(Shader::from_color(color)) {}
    explicit Paint(Shader sh) noexcept : shader(std::move(sh)) {}
    explicit Paint(std::shared_ptr<gpu::Shader> customSh) noexcept : custom_shader(std::move(customSh)) {}

    void set_shader(std::shared_ptr<gpu::Shader> s) noexcept {
        custom_shader = std::move(s);
    }
    void setShader(std::shared_ptr<gpu::Shader> s) noexcept {
        custom_shader = std::move(s);
    }
    [[nodiscard]] std::shared_ptr<gpu::Shader> getShader() const noexcept {
        return custom_shader;
    }
    [[nodiscard]] bool has_custom_shader() const noexcept {
        return custom_shader != nullptr;
    }

    void set_color(Color color) noexcept {
        custom_shader = nullptr;
        shader = Shader::from_color(color);
    }

    void set_color_rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept {
        custom_shader = nullptr;
        shader = Shader::from_color_rgba8(r, g, b, a);
    }

    [[nodiscard]] bool is_solid_color() const noexcept {
        return custom_shader == nullptr && shader.is_solid_color();
    }

    void set_color_filter(const effects::ColorMatrix& filter) noexcept {
        color_filter = filter;
        color_effect = std::nullopt;
    }

    void set_color_filter(const effects::ColorEffect& filter) noexcept {
        color_filter = std::nullopt;
        color_effect = filter;
    }

    void clear_color_filter() noexcept {
        color_filter = std::nullopt;
        color_effect = std::nullopt;
    }

    [[nodiscard]] bool has_color_filter() const noexcept {
        return color_filter.has_value() || (color_effect.has_value() && color_effect->is_active());
    }

    [[nodiscard]] std::optional<effects::ColorEffect> effective_color_effect() const noexcept {
        if (color_effect.has_value() && color_effect->is_active()) {
            return color_effect;
        }
        if (color_filter.has_value()) {
            return effects::ColorEffect::from_matrix(*color_filter);
        }
        return std::nullopt;
    }

    void set_image_filter(const effects::ImageFilter& filter) noexcept {
        image_filter = filter;
    }

    void clear_image_filter() noexcept {
        image_filter = std::nullopt;
    }

    [[nodiscard]] bool has_image_filter() const noexcept {
        return image_filter.has_value() && image_filter->is_active();
    }

    void set_mask_filter(const effects::MaskFilter& filter) noexcept {
        mask_filter = filter;
    }

    void clear_mask_filter() noexcept {
        mask_filter = std::nullopt;
    }

    [[nodiscard]] bool has_mask_filter() const noexcept {
        return mask_filter.has_value() && mask_filter->is_active();
    }

    void set_path_effect(const effects::PathEffect& effect) noexcept {
        path_effect = effect;
    }

    void clear_path_effect() noexcept {
        path_effect = std::nullopt;
    }

    [[nodiscard]] bool has_path_effect() const noexcept {
        return path_effect.has_value() && path_effect->is_active();
    }

    void set_blender(const effects::Blender& b) noexcept {
        blender = b;
    }

    void clear_blender() noexcept {
        blender = std::nullopt;
    }

    [[nodiscard]] bool has_custom_blender() const noexcept {
        return blender.has_value() && blender->is_custom();
    }

    [[nodiscard]] bool has_extended_color_filter() const noexcept {
        return color_effect.has_value() && color_effect->is_extended();
    }

    [[nodiscard]] bool has_layer_filter() const noexcept {
        return has_image_filter() || has_mask_filter() || has_custom_blender() ||
               (has_extended_color_filter() && !is_solid_color());
    }

    void set_linear_blending(bool enable) noexcept {
        colorspace = enable ? ColorSpace::Linear : ColorSpace::SimpleSRGB;
    }

    [[nodiscard]] bool is_linear_blending() const noexcept {
        return colorspace == ColorSpace::Linear || colorspace == ColorSpace::FullSRGBGamma;
    }

    void set_style(PaintStyle s) noexcept {
        style = s;
    }

    [[nodiscard]] PaintStyle paint_style() const noexcept {
        return style;
    }

    void set_stroke_width(float w) noexcept {
        stroke_width = (w > 0.0f) ? w : 0.0f;
    }

    [[nodiscard]] float get_stroke_width() const noexcept {
        return stroke_width;
    }
};

} // namespace nisaba
