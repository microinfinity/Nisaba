#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/effects/color_filter.hpp"
#include "nisaba/effects/runtime_filter.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/shaders/shader.hpp"

namespace nisaba::effects {

/// Color channel selector for displacement map filters.
enum class ColorChannel : uint8_t {
    R = 0,
    G = 1,
    B = 2,
    A = 3,
};

/// Edge sampling policy for convolution and spatial image filters.
enum class FilterTileMode : uint8_t {
    Clamp = 0,
    Repeat = 1,
    ClampToBlack = 2,
};

/// 3D surface lighting model and light source type.
enum class LightingKind : uint8_t {
    DistantDiffuse = 0,
    DistantSpecular = 1,
    PointDiffuse = 2,
    PointSpecular = 3,
    SpotDiffuse = 4,
    SpotSpecular = 5,
};

/// 3D point or direction vector used by surface lighting filters.
struct Point3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/// Operation kind for a node inside a composable ImageFilter directed acyclic graph (DAG).
enum class ImageFilterKind : uint8_t {
    None = 0,
    Blur = 1,
    DropShadow = 2,
    Compose = 3,
    Merge = 4,
    Blend = 5,
    Offset = 6,
    ColorFilter = 7,
    RuntimeShader = 8,
    Dilate = 9,
    Erode = 10,
    MatrixConvolution = 11,
    DisplacementMap = 12,
    Magnifier = 13,
    Arithmetic = 14,
    MatrixTransform = 15,
    Tile = 16,
    AlphaThreshold = 17,
    Lighting = 18,
};

/// Immutable node in a composable ImageFilter DAG (RTTI-free).
struct ImageFilterNode {
    ImageFilterKind kind{ImageFilterKind::None};
    float sigma_x{0.0f};
    float sigma_y{0.0f};
    float dx{0.0f};
    float dy{0.0f};
    Color color{Color::TRANSPARENT};
    bool shadow_only{false};
    BlendMode blend_mode{BlendMode::SourceOver};
    ColorEffect color_effect{};
    std::shared_ptr<const RuntimeFilterSpec> runtime_spec{};
    // Phase 2 & 4 filter parameters
    int32_t kernel_w{0};
    int32_t kernel_h{0};
    std::vector<float> kernel{};
    float gain{1.0f};
    float bias{0.0f};
    int32_t kernel_offset_x{0};
    int32_t kernel_offset_y{0};
    FilterTileMode tile_mode{FilterTileMode::Clamp};
    bool convolve_alpha{false};
    ColorChannel x_channel{ColorChannel::R};
    ColorChannel y_channel{ColorChannel::G};
    float disp_scale{0.0f};
    Rect src_rect{};
    Rect dst_rect{};
    float magnifier_inset{0.0f};
    float k1{0.0f};
    float k2{0.0f};
    float k3{0.0f};
    float k4{0.0f};
    bool enforce_premul{true};
    Transform filter_transform{};
    FilterQuality filter_quality{FilterQuality::Bilinear};
    float inner_min{0.0f};
    float outer_max{1.0f};
    LightingKind lighting_kind{LightingKind::DistantDiffuse};
    Point3 light_pos{};
    Point3 light_target{};
    float surface_scale{1.0f};
    float light_kd_ks{1.0f};
    float shininess{1.0f};
    float specular_exponent{1.0f};
    float cutoff_angle{180.0f};
    std::optional<Rect> crop_rect{std::nullopt};
    /// Child inputs. A null entry references the current source image passed to the filter graph.
    /// - Blur / DropShadow / Offset / ColorFilter: 0 or 1 input (index 0).
    /// - Compose: 2 inputs: inputs[0] = outer, inputs[1] = inner.
    /// - Merge: N inputs composited back-to-front with SourceOver.
    /// - Blend: 2 inputs: inputs[0] = background, inputs[1] = foreground.
    std::vector<std::shared_ptr<const ImageFilterNode>> inputs{};
};

/// Value-typed handle wrapping an immutable composable ImageFilter DAG
/// with full CPU and GPU parity and optional per-node CropRect support.
class ImageFilter {
public:
    ImageFilter() noexcept = default;
    explicit ImageFilter(std::shared_ptr<const ImageFilterNode> node) noexcept
        : node_(std::move(node)) {}

    /// Creates a separable 2D Gaussian blur filter.
    [[nodiscard]] static ImageFilter blur(
        float sx,
        float sy,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Creates an isotropic Gaussian blur filter.
    [[nodiscard]] static ImageFilter blur(
        float sigma,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    ) {
        return blur(sigma, sigma, std::move(input), crop_rect);
    }

    /// Creates a drop-shadow filter with independent horizontal/vertical sigmas.
    [[nodiscard]] static ImageFilter drop_shadow(
        float dx_val,
        float dy_val,
        float sx,
        float sy,
        Color shadow_color,
        bool only_shadow = false,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Creates a drop-shadow filter with uniform blur sigma.
    [[nodiscard]] static ImageFilter drop_shadow(
        float dx_val,
        float dy_val,
        float sigma,
        Color shadow_color,
        bool only_shadow = false,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    ) {
        return drop_shadow(dx_val, dy_val, sigma, sigma, shadow_color, only_shadow, std::move(input), crop_rect);
    }

    /// Composes `outer(inner(src))`: evaluates `inner` on the source image and feeds its output as source to `outer`.
    [[nodiscard]] static ImageFilter compose(
        const ImageFilter& outer,
        const ImageFilter& inner,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Merges multiple filter outputs back-to-front using SourceOver compositing.
    /// Nullopt entries represent the unfiltered source image.
    [[nodiscard]] static ImageFilter merge(
        std::vector<std::optional<ImageFilter>> filters,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Initializer-list overload for merging filter outputs back-to-front.
    [[nodiscard]] static ImageFilter merge(
        std::initializer_list<std::optional<ImageFilter>> filters,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Variadic convenience overload for `ImageFilter::merge(f1, f2, ...)`.
    template <typename First, typename Second, typename... Rest>
        requires (std::is_convertible_v<First, std::optional<ImageFilter>> &&
                  std::is_convertible_v<Second, std::optional<ImageFilter>> &&
                  (std::is_convertible_v<Rest, std::optional<ImageFilter>> && ...))
    [[nodiscard]] static ImageFilter merge(First&& first, Second&& second, Rest&&... rest) {
        std::vector<std::optional<ImageFilter>> list;
        list.reserve(2 + sizeof...(Rest));
        list.emplace_back(std::forward<First>(first));
        list.emplace_back(std::forward<Second>(second));
        (list.emplace_back(std::forward<Rest>(rest)), ...);
        return merge(std::move(list));
    }

    /// Blends `foreground` over `background` using the specified BlendMode.
    /// Nullopt for either argument references the unfiltered source image.
    [[nodiscard]] static ImageFilter blend(
        BlendMode mode,
        std::optional<ImageFilter> background,
        std::optional<ImageFilter> foreground,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Translates the input image by `(dx, dy)` in local coordinates.
    [[nodiscard]] static ImageFilter offset(
        float dx_val,
        float dy_val,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Applies a ColorEffect to the input image.
    [[nodiscard]] static ImageFilter color_filter(
        const ColorEffect& effect,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Applies a custom post-processing shader program to the input layer image.
    [[nodiscard]] static ImageFilter runtime_shader(
        std::shared_ptr<const RuntimeFilterSpec> spec,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Compiles/parses a Nisaba shading program with optional uniforms into a post-process ImageFilter.
    [[nodiscard]] static ImageFilter runtime_shader(
        std::string_view source,
        const RuntimeFilterUniforms& uniforms = {},
        std::string_view input_child_name = {},
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Morphological dilation (per-channel neighborhood maximum).
    [[nodiscard]] static ImageFilter dilate(
        float rx,
        float ry,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Morphological erosion (per-channel neighborhood minimum).
    [[nodiscard]] static ImageFilter erode(
        float rx,
        float ry,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// 2D spatial matrix convolution (sharpen, emboss, edge detection).
    [[nodiscard]] static ImageFilter matrix_convolution(
        int32_t kernel_w,
        int32_t kernel_h,
        std::vector<float> kernel,
        float gain,
        float bias,
        int32_t target_x,
        int32_t target_y,
        FilterTileMode tile_mode = FilterTileMode::Clamp,
        bool convolve_alpha = false,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Displaces pixels of `color` input using channels of `displacement` input.
    [[nodiscard]] static ImageFilter displacement_map(
        ColorChannel x_channel,
        ColorChannel y_channel,
        float scale,
        std::optional<ImageFilter> displacement,
        std::optional<ImageFilter> color = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Magnifying glass lens effect over `src_rect` with smooth border `inset`.
    [[nodiscard]] static ImageFilter magnifier(
        const Rect& src_rect,
        float inset,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Arithmetic pixel combiner: `k1*fg*bg + k2*fg + k3*bg + k4`.
    [[nodiscard]] static ImageFilter arithmetic(
        float k1,
        float k2,
        float k3,
        float k4,
        bool enforce_premul,
        std::optional<ImageFilter> background,
        std::optional<ImageFilter> foreground = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Applies an affine 2D transform to the input image.
    [[nodiscard]] static ImageFilter matrix_transform(
        const Transform& transform,
        FilterQuality quality = FilterQuality::Bilinear,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Repeats pixels from `src` rectangle to fill `dst` rectangle.
    [[nodiscard]] static ImageFilter tile(
        const Rect& src,
        const Rect& dst,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Clamps alpha to at least `inner_min` inside `region` and at most `outer_max` outside.
    [[nodiscard]] static ImageFilter alpha_threshold(
        const Rect& region,
        float inner_min,
        float outer_max,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Distant directional diffuse lighting using the input alpha channel as a bump height map.
    [[nodiscard]] static ImageFilter distant_lit_diffuse(
        Point3 direction,
        Color light_color,
        float surface_scale,
        float kd,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Distant directional specular lighting using the input alpha channel as a bump height map.
    [[nodiscard]] static ImageFilter distant_lit_specular(
        Point3 direction,
        Color light_color,
        float surface_scale,
        float ks,
        float shininess,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Point-source diffuse lighting using the input alpha channel as a bump height map.
    [[nodiscard]] static ImageFilter point_lit_diffuse(
        Point3 location,
        Color light_color,
        float surface_scale,
        float kd,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Point-source specular lighting using the input alpha channel as a bump height map.
    [[nodiscard]] static ImageFilter point_lit_specular(
        Point3 location,
        Color light_color,
        float surface_scale,
        float ks,
        float shininess,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Spotlight diffuse lighting using the input alpha channel as a bump height map.
    [[nodiscard]] static ImageFilter spot_lit_diffuse(
        Point3 location,
        Point3 target,
        float specular_exponent,
        float cutoff_angle,
        Color light_color,
        float surface_scale,
        float kd,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Spotlight specular lighting using the input alpha channel as a bump height map.
    [[nodiscard]] static ImageFilter spot_lit_specular(
        Point3 location,
        Point3 target,
        float specular_exponent,
        float cutoff_angle,
        Color light_color,
        float surface_scale,
        float ks,
        float shininess,
        std::optional<ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    );

    /// Returns a copy of this filter node with the specified local-space crop rectangle applied.
    [[nodiscard]] ImageFilter with_crop(const Rect& crop) const;

    [[nodiscard]] const std::shared_ptr<const ImageFilterNode>& node() const noexcept { return node_; }
    [[nodiscard]] const std::shared_ptr<const RuntimeFilterSpec>& runtime_spec() const noexcept {
        static const std::shared_ptr<const RuntimeFilterSpec> kNull{};
        return node_ ? node_->runtime_spec : kNull;
    }
    [[nodiscard]] ImageFilterKind kind() const noexcept {
        return node_ ? node_->kind : ImageFilterKind::None;
    }
    [[nodiscard]] float sigma_x() const noexcept { return node_ ? node_->sigma_x : 0.0f; }
    [[nodiscard]] float sigma_y() const noexcept { return node_ ? node_->sigma_y : 0.0f; }
    [[nodiscard]] float dx() const noexcept { return node_ ? node_->dx : 0.0f; }
    [[nodiscard]] float dy() const noexcept { return node_ ? node_->dy : 0.0f; }
    [[nodiscard]] Color color() const noexcept { return node_ ? node_->color : Color::TRANSPARENT; }
    [[nodiscard]] bool shadow_only() const noexcept { return node_ ? node_->shadow_only : false; }
    [[nodiscard]] BlendMode blend_mode() const noexcept {
        return node_ ? node_->blend_mode : BlendMode::SourceOver;
    }
    [[nodiscard]] const std::optional<Rect>& crop_rect() const noexcept {
        static const std::optional<Rect> kEmpty{};
        return node_ ? node_->crop_rect : kEmpty;
    }

    /// Returns true if this filter DAG performs any visible raster operation.
    [[nodiscard]] bool is_active() const noexcept;

    /// Returns true if this filter is a single flat Blur or DropShadow with no child inputs and no crop rect.
    [[nodiscard]] bool is_simple_leaf() const noexcept;

    /// Maximum Gaussian blur sigma anywhere in the filter graph.
    [[nodiscard]] float max_sigma() const noexcept;

    /// Maps local source bounds to the filter's output bounds.
    [[nodiscard]] Rect map_bounds(const Rect& local_bounds) const noexcept;

    /// Computes the conservative offscreen layer bounds covering both the source draw
    /// and all intermediate/final filter regions before optional cropping.
    [[nodiscard]] Rect map_layer_bounds(const Rect& local_bounds) const noexcept;

private:
    std::shared_ptr<const ImageFilterNode> node_{nullptr};
};

/// Evaluates an ImageFilter DAG in-place on a device-space layer ROI pixmap.
/// @param layer_roi Mutable view of the layer ROI starting at canvas device coordinates (roi_origin_x, roi_origin_y).
/// @param filter Composable ImageFilter DAG to evaluate.
/// @param roi_origin_x Left device pixel coordinate of `layer_roi` in the target canvas.
/// @param roi_origin_y Top device pixel coordinate of `layer_roi` in the target canvas.
/// @param layer_transform Canvas transform active when the layer or draw was issued.
void apply_image_filter_cpu(
    PixmapMut& layer_roi,
    const ImageFilter& filter,
    int32_t roi_origin_x,
    int32_t roi_origin_y,
    const Transform& layer_transform = Transform()
);

} // namespace nisaba::effects
