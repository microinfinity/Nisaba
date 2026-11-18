#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/canvas/mask.hpp"

namespace nisaba::effects {

/// Computes the optimal integer box blur radii for a 3-pass box blur
/// approximating a 2D Gaussian filter with standard deviation sigma.
std::array<int32_t, 3> compute_box_blur_radii(float sigma) noexcept;

/// Performs a single-pass 2D box blur in-place on an 8-bit alpha mask.
void box_blur_mask(Mask& mask, int32_t radius);

/// Performs a 3-pass fast Gaussian blur approximation in-place on an 8-bit alpha mask.
void gaussian_blur_mask(Mask& mask, float sigma);

/// Creates a new blurred Mask from a source mask reference.
Mask blur_mask(const MaskRef& src, float sigma);

/// Performs a single-pass 2D box blur in-place on a 32-bit premultiplied RGBA pixmap.
void box_blur_pixmap(PixmapMut& pixmap, int32_t radius);

/// Performs a 3-pass fast Gaussian blur approximation in-place on a 32-bit premultiplied RGBA pixmap.
void gaussian_blur_pixmap(PixmapMut& pixmap, float sigma);

/// Performs an anisotropic separable 3-pass Gaussian blur in-place with independent X/Y sigmas.
void gaussian_blur_pixmap_xy(PixmapMut& pixmap, float sigma_x, float sigma_y);

/// Creates a new blurred Pixmap from a source pixmap reference.
Pixmap blur_pixmap(const PixmapRef& src, float sigma);

} // namespace nisaba::effects
