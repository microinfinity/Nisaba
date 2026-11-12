#pragma once

#include "nisaba/compositor/geometry.hpp"
#include "nisaba/compositor/matrix3.hpp"

#include <optional>
#include <wayland-server-protocol.h>

namespace nisaba::compositor {

/**
 * @brief Wayland surface buffer and wp_viewporter state for UV resolution.
 */
struct SurfaceViewportState {
    SizeI bufferPixelSize{0, 0};
    int32_t bufferScale{1};
    wl_output_transform bufferTransform{WL_OUTPUT_TRANSFORM_NORMAL};
    std::optional<RectF> viewportSource{};      ///< wp_viewport.set_source in surface-local coords
    std::optional<SizeI> viewportDestination{}; ///< wp_viewport.set_destination in surface-local coords
    double outputScale{1.0};
    bool isAnimating{false};
    bool expandUndersizedEdges{true};
};

/**
 * @brief Resolved UV coordinates and texture sampling filter recommendation.
 */
struct ResolvedSurfaceUv {
    RectF uvBox{0.0, 0.0, 1.0, 1.0};
    bool preferNearestFilter{false};
    bool misalignmentFixed{false};
};

/**
 * @brief Sovereign Wayland wp_viewporter & wp_fractional_scale_v1 UV coordinate solver.
 *
 * Implements the exact Wayland Viewporter specification order:
 *   1. Buffer Transform inversion
 *   2. Buffer Scale application
 *   3. Viewport source cropping & fractional scale off-by-one texel alignment.
 */
class SurfaceUvSolver {
public:
    [[nodiscard]] static ResolvedSurfaceUv resolve(
        const SurfaceViewportState& state,
        const SizeI& projectedPixelSize) noexcept;
};

} // namespace nisaba::compositor

