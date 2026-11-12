#include "nisaba/compositor/surface_uv_solver.hpp"

#include <algorithm>
#include <cmath>

namespace nisaba::compositor {

namespace {

constexpr bool isRotated90Or270(wl_output_transform t) noexcept {
    return t == WL_OUTPUT_TRANSFORM_90 ||
           t == WL_OUTPUT_TRANSFORM_270 ||
           t == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
           t == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

RectF applyInverseBufferTransformToBox(
    const RectF& box,
    wl_output_transform transform,
    double transformedWidth,
    double transformedHeight) noexcept {
    const wl_output_transform inv = invertTransform(transform);
    double x = box.x;
    double y = box.y;
    double w = box.width;
    double h = box.height;

    switch (inv) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
            break;
        case WL_OUTPUT_TRANSFORM_90:
            x = box.y;
            y = transformedWidth - box.x - box.width;
            w = box.height;
            h = box.width;
            break;
        case WL_OUTPUT_TRANSFORM_180:
            x = transformedWidth - box.x - box.width;
            y = transformedHeight - box.y - box.height;
            break;
        case WL_OUTPUT_TRANSFORM_270:
            x = transformedHeight - box.y - box.height;
            y = box.x;
            w = box.height;
            h = box.width;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            x = transformedWidth - box.x - box.width;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            x = box.y;
            y = box.x;
            w = box.height;
            h = box.width;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            y = transformedHeight - box.y - box.height;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            x = transformedHeight - box.y - box.height;
            y = transformedWidth - box.x - box.width;
            w = box.height;
            h = box.width;
            break;
    }
    return RectF{x, y, w, h};
}

} // namespace

ResolvedSurfaceUv SurfaceUvSolver::resolve(
    const SurfaceViewportState& state,
    const SizeI& projectedPixelSize) noexcept {
    ResolvedSurfaceUv out{};
    if (state.bufferPixelSize.isEmpty() || projectedPixelSize.isEmpty()) {
        return out;
    }

    const double bufW = static_cast<double>(state.bufferPixelSize.width);
    const double bufH = static_cast<double>(state.bufferPixelSize.height);
    const double scale = static_cast<double>(std::max(state.bufferScale, 1));

    double tlU = 0.0;
    double tlV = 0.0;
    double brU = 1.0;
    double brV = 1.0;

    // 1. Apply wp_viewport.set_source if active (Viewporter spec order: transform -> scale -> source)
    if (state.viewportSource.has_value() && !state.viewportSource->isEmpty()) {
        RectF src = *state.viewportSource;
        src.x *= scale;
        src.y *= scale;
        src.width *= scale;
        src.height *= scale;

        const double trW = isRotated90Or270(state.bufferTransform) ? bufH : bufW;
        const double trH = isRotated90Or270(state.bufferTransform) ? bufW : bufH;
        const RectF rawSrc = applyInverseBufferTransformToBox(src, state.bufferTransform, trW, trH);

        tlU = std::clamp(rawSrc.x / bufW, 0.0, 1.0);
        tlV = std::clamp(rawSrc.y / bufH, 0.0, 1.0);
        brU = std::clamp((rawSrc.x + rawSrc.width) / bufW, 0.0, 1.0);
        brV = std::clamp((rawSrc.y + rawSrc.height) / bufH, 0.0, 1.0);

        if (brU - tlU < 1e-5 || brV - tlV < 1e-5) {
            tlU = 0.0;
            tlV = 0.0;
            brU = 1.0;
            brV = 1.0;
        }
    }

    // 2. Detect fractional scale 1-2px buffer misalignment (wp_fractional_scale_v1)
    const bool isFractionalOutput = std::abs( std::floor(state.outputScale) - state.outputScale ) > 1e-4;
    const int diffW = std::abs(projectedPixelSize.width - state.bufferPixelSize.width);
    const int diffH = std::abs(projectedPixelSize.height - state.bufferPixelSize.height);
    const bool misalignedFsV1 = isFractionalOutput &&
                                state.bufferScale == 1 &&
                                !state.isAnimating &&
                                (diffW > 0 || diffH > 0) &&
                                diffW <= 2 &&
                                diffH <= 2;

    if (misalignedFsV1) {
        const double texelU = 1.0 / bufW;
        const double texelV = 1.0 / bufH;
        const double misU = ((brU - tlU) * bufW) - static_cast<double>(projectedPixelSize.width);
        const double misV = ((brV - tlV) * bufH) - static_cast<double>(projectedPixelSize.height);

        brU -= misU * texelU;
        brV -= misV * texelV;
        out.preferNearestFilter = true;
        out.misalignmentFixed = true;
    } else if (diffW == 0 && diffH == 0 && !state.isAnimating) {
        out.preferNearestFilter = true;
    }

    out.uvBox = RectF{
        tlU,
        tlV,
        std::max(0.0001, brU - tlU),
        std::max(0.0001, brV - tlV)
    };
    return out;
}

} // namespace nisaba::compositor
