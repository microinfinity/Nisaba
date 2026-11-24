#pragma once

#include <array>
#include <optional>
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"

namespace nisaba {

class Path;

/// A 4x4 projective transformation matrix for 3D spatial orientation,
/// perspective camera projection, and quad homographies.
///
/// Matrix layout in row-major order:
/// [ m0   m1   m2   m3  ]  (row 0: X axis)
/// [ m4   m5   m6   m7  ]  (row 1: Y axis)
/// [ m8   m9   m10  m11 ]  (row 2: Z axis)
/// [ m12  m13  m14  m15 ]  (row 3: Perspective W)
struct Transform4x4 {
    std::array<float, 16> m{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    constexpr Transform4x4() noexcept = default;

    constexpr Transform4x4(
        float m00, float m01, float m02, float m03,
        float m10, float m11, float m12, float m13,
        float m20, float m21, float m22, float m23,
        float m30, float m31, float m32, float m33
    ) noexcept : m{
        m00, m01, m02, m03,
        m10, m11, m12, m13,
        m20, m21, m22, m23,
        m30, m31, m32, m33
    } {}

    /// Constructs from a 2D affine Transform.
    constexpr explicit Transform4x4(const Transform& t) noexcept
        : m{
            t.sx, t.kx, 0.0f, t.tx,
            t.ky, t.sy, 0.0f, t.ty,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        } {}

    static constexpr Transform4x4 identity() noexcept {
        return Transform4x4();
    }

    static constexpr Transform4x4 from_translate(float tx, float ty, float tz = 0.0f) noexcept {
        return Transform4x4(
            1.0f, 0.0f, 0.0f, tx,
            0.0f, 1.0f, 0.0f, ty,
            0.0f, 0.0f, 1.0f, tz,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    static constexpr Transform4x4 from_scale(float sx, float sy, float sz = 1.0f) noexcept {
        return Transform4x4(
            sx,   0.0f, 0.0f, 0.0f,
            0.0f, sy,   0.0f, 0.0f,
            0.0f, 0.0f, sz,   0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    /// Rotation around X axis (pitch / vertical tilt) in degrees.
    static Transform4x4 from_rotate_x(float angle_degrees) noexcept;

    /// Rotation around Y axis (yaw / horizontal tilt) in degrees.
    static Transform4x4 from_rotate_y(float angle_degrees) noexcept;

    /// Rotation around Z axis (roll / 2D rotation) in degrees.
    static Transform4x4 from_rotate_z(float angle_degrees) noexcept;

    /// Perspective camera projection with focal distance 'd'.
    /// Foreshortens elements moving in Z according to W = 1 - z / d.
    static constexpr Transform4x4 from_perspective(float d) noexcept {
        if (d == 0.0f) return identity();
        return Transform4x4(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, -1.0f / d, 1.0f
        );
    }

    /// Convenience 3D camera setup looking at origin with focal distance and Euler angles.
    static Transform4x4 from_camera_orbit(
        float pivot_x, float pivot_y,
        float rot_x_deg, float rot_y_deg, float rot_z_deg,
        float perspective_distance
    ) noexcept;

    /// Calculates the 3x3 projective homography mapping a source Rect to 4 arbitrary destination points.
    static std::optional<Transform4x4> from_rect_to_quad(
        const Rect& src,
        Point p0, Point p1, Point p2, Point p3
    ) noexcept;

    /// Matrix multiplication.
    Transform4x4 operator*(const Transform4x4& o) const noexcept;

    Transform4x4 pre_translate(float tx, float ty, float tz = 0.0f) const noexcept {
        return *this * from_translate(tx, ty, tz);
    }

    Transform4x4 post_translate(float tx, float ty, float tz = 0.0f) const noexcept {
        return from_translate(tx, ty, tz) * *this;
    }

    Transform4x4 pre_scale(float sx, float sy, float sz = 1.0f) const noexcept {
        return *this * from_scale(sx, sy, sz);
    }

    Transform4x4 post_scale(float sx, float sy, float sz = 1.0f) const noexcept {
        return from_scale(sx, sy, sz) * *this;
    }

    Transform4x4 pre_rotate_x(float deg) const noexcept {
        return *this * from_rotate_x(deg);
    }

    Transform4x4 pre_rotate_y(float deg) const noexcept {
        return *this * from_rotate_y(deg);
    }

    Transform4x4 pre_rotate_z(float deg) const noexcept {
        return *this * from_rotate_z(deg);
    }

    /// Computes matrix inverse. Returns std::nullopt if matrix is singular.
    std::optional<Transform4x4> invert() const noexcept;

    /// Maps a 2D point (assumed z=0, w=1) through the matrix and applies perspective division (X/W, Y/W).
    Point map_point(Point p) const noexcept;

    /// Maps a 2D point with check for valid non-zero positive W (in front of camera).
    std::optional<Point> map_point_checked(Point p) const noexcept;

    /// Maps a 3D point (x, y, z) through the matrix with perspective division.
    Point map_point3d(float x, float y, float z) const noexcept;

    /// Returns true if this matrix has non-identity perspective row [m12, m13, m14, m15 != (0,0,0,1)].
    bool has_perspective() const noexcept {
        return m[12] != 0.0f || m[13] != 0.0f || m[14] != 0.0f || m[15] != 1.0f;
    }

    /// Returns true if transforming 2D planar geometry (z = 0) involves non-affine perspective division.
    [[nodiscard]] bool is_planar_perspective() const noexcept {
        return std::abs(m[12]) > 1e-7f || std::abs(m[13]) > 1e-7f || std::abs(m[15] - 1.0f) > 1e-7f;
    }

    /// Returns true if this matrix is identically a 2D affine matrix (no Z or perspective terms).
    [[nodiscard]] bool is_2d_affine() const noexcept {
        return m[2] == 0.0f && m[6] == 0.0f &&
               m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f &&
               m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f;
    }

    /// Extracts the 2D planar affine transform (exact when !is_planar_perspective()).
    [[nodiscard]] Transform to_transform_2d() const noexcept {
        if (std::abs(m[12]) <= 1e-7f && std::abs(m[13]) <= 1e-7f && std::abs(m[15]) > 1e-7f && m[15] != 1.0f) {
            const float inv_w = 1.0f / m[15];
            return Transform::from_row(m[0] * inv_w, m[4] * inv_w, m[1] * inv_w, m[5] * inv_w, m[3] * inv_w, m[7] * inv_w);
        }
        return Transform::from_row(m[0], m[4], m[1], m[5], m[3], m[7]);
    }

    /// Maps a 2D axis-aligned rectangle through this 4x4 matrix with near-plane clipping (w > 0),
    /// returning the enclosing projected 2D bounding box.
    [[nodiscard]] std::optional<Rect> map_rect(const Rect& rect) const noexcept;

    /// Projects a 2D vector Path through this 4x4 projective matrix with adaptive curve flattening
    /// and near-plane clipping against w >= eps.
    [[nodiscard]] Path project_path(const Path& path, float flatness_tol = 0.5f) const;

    constexpr bool operator==(const Transform4x4& o) const noexcept {
        return m == o.m;
    }
    constexpr bool operator!=(const Transform4x4& o) const noexcept {
        return m != o.m;
    }
};

} // namespace nisaba
