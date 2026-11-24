#pragma once

#include <cmath>
#include <optional>
#include <utility>
#include "nisaba/math/scalar.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"

namespace nisaba {

/// An affine 2D transformation matrix.
/// We use column-major-column-vector notation:
/// [ sx, kx, tx ]
/// [ ky, sy, ty ]
/// [  0,  0,  1 ]
struct Transform {
    float sx{1.0f};
    float ky{0.0f};
    float kx{0.0f};
    float sy{1.0f};
    float tx{0.0f};
    float ty{0.0f};

    constexpr Transform() noexcept = default;

    constexpr Transform(float sx_, float ky_, float kx_, float sy_, float tx_, float ty_) noexcept
        : sx(sx_), ky(ky_), kx(kx_), sy(sy_), tx(tx_), ty(ty_) {}

    static constexpr Transform identity() noexcept {
        return Transform(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    }

    static constexpr Transform from_row(float sx, float ky, float kx, float sy, float tx, float ty) noexcept {
        return Transform(sx, ky, kx, sy, tx, ty);
    }

    static constexpr Transform from_translate(float tx, float ty) noexcept {
        return from_row(1.0f, 0.0f, 0.0f, 1.0f, tx, ty);
    }

    static constexpr Transform from_scale(float sx, float sy) noexcept {
        return from_row(sx, 0.0f, 0.0f, sy, 0.0f, 0.0f);
    }

    static constexpr Transform from_skew(float kx, float ky) noexcept {
        return from_row(1.0f, ky, kx, 1.0f, 0.0f, 0.0f);
    }

    static Transform from_rotate(float angle_degrees) noexcept {
        float rad = scalar::to_radians(angle_degrees);
        float a = std::cos(rad);
        float b = std::sin(rad);
        float c = -b;
        float d = a;
        return from_row(a, b, c, d, 0.0f, 0.0f);
    }

    static Transform from_rotate_at(float angle, float tx, float ty) noexcept {
        Transform ts = identity();
        ts = ts.pre_translate(tx, ty);
        ts = ts.pre_concat(from_rotate(angle));
        ts = ts.pre_translate(-tx, -ty);
        return ts;
    }

    static constexpr Transform from_bbox(const NonZeroRect& bbox) noexcept {
        return from_row(bbox.width(), 0.0f, 0.0f, bbox.height(), bbox.x(), bbox.y());
    }

    static constexpr Transform from_sin_cos(float sin, float cos) noexcept {
        return from_row(cos, sin, -sin, cos, 0.0f, 0.0f);
    }

    static std::optional<Transform> from_sin_cos_at(float sin, float cos, float px, float py) noexcept {
        Transform ts = from_sin_cos(sin, cos);
        ts.tx = px - cos * px + sin * py;
        ts.ty = py - sin * px - cos * py;
        if (ts.is_valid()) {
            return ts;
        }
        return std::nullopt;
    }

    bool is_finite() const noexcept {
        return std::isfinite(sx) && std::isfinite(ky) && std::isfinite(kx) &&
               std::isfinite(sy) && std::isfinite(tx) && std::isfinite(ty);
    }

    std::pair<float, float> get_scale() const noexcept {
        float x_scale = std::sqrt(sx * sx + kx * kx);
        float y_scale = std::sqrt(ky * ky + sy * sy);
        return {x_scale, y_scale};
    }

    bool is_valid() const noexcept {
        if (is_finite()) {
            auto [scale_x, scale_y] = get_scale();
            return !(scalar::is_nearly_zero_within_tolerance(scale_x, std::numeric_limits<float>::epsilon()) ||
                     scalar::is_nearly_zero_within_tolerance(scale_y, std::numeric_limits<float>::epsilon()));
        }
        return false;
    }

    constexpr bool is_identity() const noexcept {
        return sx == 1.0f && ky == 0.0f && kx == 0.0f && sy == 1.0f && tx == 0.0f && ty == 0.0f;
    }

    constexpr bool has_scale() const noexcept {
        return sx != 1.0f || sy != 1.0f;
    }

    constexpr bool has_skew() const noexcept {
        return kx != 0.0f || ky != 0.0f;
    }

    constexpr bool has_translate() const noexcept {
        return tx != 0.0f || ty != 0.0f;
    }

    constexpr bool is_scale() const noexcept {
        return has_scale() && !has_skew() && !has_translate();
    }

    constexpr bool is_skew() const noexcept {
        return !has_scale() && has_skew() && !has_translate();
    }

    constexpr bool is_translate() const noexcept {
        return !has_scale() && !has_skew() && has_translate();
    }

    constexpr bool is_scale_translate() const noexcept {
        return (has_scale() || has_translate()) && !has_skew();
    }

    Transform pre_scale(float sx_, float sy_) const noexcept {
        return pre_concat(from_scale(sx_, sy_));
    }

    Transform post_scale(float sx_, float sy_) const noexcept {
        return post_concat(from_scale(sx_, sy_));
    }

    Transform pre_skew(float kx, float ky) const noexcept {
        return pre_concat(from_skew(kx, ky));
    }

    Transform post_skew(float kx, float ky) const noexcept {
        return post_concat(from_skew(kx, ky));
    }

    Transform pre_translate(float tx_, float ty_) const noexcept {
        return pre_concat(from_translate(tx_, ty_));
    }

    Transform post_translate(float tx_, float ty_) const noexcept {
        return post_concat(from_translate(tx_, ty_));
    }

    Transform pre_rotate(float angle) const noexcept {
        return pre_concat(from_rotate(angle));
    }

    Transform post_rotate(float angle) const noexcept {
        return post_concat(from_rotate(angle));
    }

    Transform pre_rotate_at(float angle, float tx_, float ty_) const noexcept {
        return pre_concat(from_rotate_at(angle, tx_, ty_));
    }

    Transform post_rotate_at(float angle, float tx_, float ty_) const noexcept {
        return post_concat(from_rotate_at(angle, tx_, ty_));
    }

    Transform pre_concat(const Transform& other) const noexcept;
    Transform post_concat(const Transform& other) const noexcept;

    void map_point(Point& point) const noexcept {
        if (is_identity()) {
            return;
        } else if (is_translate()) {
            point.x += tx;
            point.y += ty;
        } else if (is_scale_translate()) {
            point.x = point.x * sx + tx;
            point.y = point.y * sy + ty;
        } else {
            float nx = point.x * sx + point.y * kx + tx;
            float ny = point.x * ky + point.y * sy + ty;
            point.x = nx;
            point.y = ny;
        }
    }

    void map_points(Point* points, size_t count) const noexcept {
        if (!points || count == 0) return;
        if (is_identity()) {
            return;
        } else if (is_translate()) {
            for (size_t i = 0; i < count; ++i) {
                points[i].x += tx;
                points[i].y += ty;
            }
        } else if (is_scale_translate()) {
            for (size_t i = 0; i < count; ++i) {
                points[i].x = points[i].x * sx + tx;
                points[i].y = points[i].y * sy + ty;
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                float nx = points[i].x * sx + points[i].y * kx + tx;
                float ny = points[i].x * ky + points[i].y * sy + ty;
                points[i].x = nx;
                points[i].y = ny;
            }
        }
    }

    std::optional<Transform> invert() const noexcept;

    constexpr void to_mat3x4(float* m3) const noexcept {
        m3[0]  = sx;
        m3[1]  = ky;
        m3[2]  = 0.0f;
        m3[3]  = 0.0f;
        m3[4]  = kx;
        m3[5]  = sy;
        m3[6]  = 0.0f;
        m3[7]  = 0.0f;
        m3[8]  = tx;
        m3[9]  = ty;
        m3[10] = 1.0f;
        m3[11] = 0.0f;
    }

    constexpr bool operator==(const Transform& o) const noexcept {
        return sx == o.sx && ky == o.ky && kx == o.kx && sy == o.sy && tx == o.tx && ty == o.ty;
    }

    constexpr bool operator!=(const Transform& o) const noexcept {
        return !(*this == o);
    }
};

namespace detail {

inline double dcross(double a, double b, double c, double d) noexcept {
    return a * b - c * d;
}

inline float dcross_dscale(float a, float b, float c, float d, double scale) noexcept {
    return static_cast<float>(dcross(a, b, c, d) * scale);
}

inline float mul_add_mul(float a, float b, float c, float d) noexcept {
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) +
                              static_cast<double>(c) * static_cast<double>(d));
}

inline Transform concat(const Transform& a, const Transform& b) noexcept {
    if (a.is_identity()) {
        return b;
    } else if (b.is_identity()) {
        return a;
    } else if (!a.has_skew() && !b.has_skew()) {
        return Transform::from_row(
            a.sx * b.sx,
            0.0f,
            0.0f,
            a.sy * b.sy,
            a.sx * b.tx + a.tx,
            a.sy * b.ty + a.ty
        );
    } else {
        return Transform::from_row(
            mul_add_mul(a.sx, b.sx, a.kx, b.ky),
            mul_add_mul(a.ky, b.sx, a.sy, b.ky),
            mul_add_mul(a.sx, b.kx, a.kx, b.sy),
            mul_add_mul(a.ky, b.kx, a.sy, b.sy),
            mul_add_mul(a.sx, b.tx, a.kx, b.ty) + a.tx,
            mul_add_mul(a.ky, b.tx, a.sy, b.ty) + a.ty
        );
    }
}

inline std::optional<double> inv_determinant(const Transform& ts) noexcept {
    double det = dcross(ts.sx, ts.sy, ts.kx, ts.ky);
    float tolerance = SCALAR_NEARLY_ZERO * SCALAR_NEARLY_ZERO * SCALAR_NEARLY_ZERO;
    if (scalar::is_nearly_zero_within_tolerance(static_cast<float>(det), tolerance)) {
        return std::nullopt;
    }
    return 1.0 / det;
}

inline Transform compute_inv(const Transform& ts, double inv_det) noexcept {
    return Transform::from_row(
        static_cast<float>(ts.sy * inv_det),
        static_cast<float>(-ts.ky * inv_det),
        static_cast<float>(-ts.kx * inv_det),
        static_cast<float>(ts.sx * inv_det),
        dcross_dscale(ts.kx, ts.ty, ts.sy, ts.tx, inv_det),
        dcross_dscale(ts.ky, ts.tx, ts.sx, ts.ty, inv_det)
    );
}

} // namespace detail

inline Transform Transform::pre_concat(const Transform& other) const noexcept {
    return detail::concat(*this, other);
}

inline Transform Transform::post_concat(const Transform& other) const noexcept {
    return detail::concat(other, *this);
}

inline std::optional<Transform> Transform::invert() const noexcept {
    if (is_identity()) {
        return *this;
    }
    if (is_scale_translate()) {
        if (has_scale()) {
            float inv_x = scalar::invert(sx);
            float inv_y = scalar::invert(sy);
            return Transform::from_row(
                inv_x, 0.0f, 0.0f, inv_y, -tx * inv_x, -ty * inv_y
            );
        } else {
            return Transform::from_translate(-tx, -ty);
        }
    } else {
        auto inv_det = detail::inv_determinant(*this);
        if (!inv_det) {
            return std::nullopt;
        }
        Transform inv_ts = detail::compute_inv(*this, *inv_det);
        if (inv_ts.is_finite()) {
            return inv_ts;
        }
        return std::nullopt;
    }
}

inline std::optional<Rect> Rect::transform(const Transform& ts) const noexcept {
    if (ts.is_identity()) {
        return *this;
    } else if (ts.has_skew()) {
        Point pts[4] = {
            Point::from_xy(left(), top()),
            Point::from_xy(right(), top()),
            Point::from_xy(left(), bottom()),
            Point::from_xy(right(), bottom())
        };
        ts.map_points(pts, 4);
        return Rect::from_points(pts, 4);
    } else {
        Point pts[2] = {
            Point::from_xy(left(), top()),
            Point::from_xy(right(), bottom())
        };
        ts.map_points(pts, 2);
        return Rect::from_points(pts, 2);
    }
}

inline std::optional<NonZeroRect> NonZeroRect::transform(const Transform& ts) const noexcept {
    if (ts.is_identity()) {
        return *this;
    } else {
        auto r = to_rect().transform(ts);
        if (r) {
            return r->to_non_zero_rect();
        }
        return std::nullopt;
    }
}

/// Compact 2D similarity transform (uniform scale, rotation, and translation).
/// Maps a local point (x, y) to:
///   x' = scos * x - ssin * y + tx
///   y' = ssin * x + scos * y + ty
struct RSXform {
    float scos{1.0f};
    float ssin{0.0f};
    float tx{0.0f};
    float ty{0.0f};

    constexpr RSXform() noexcept = default;
    constexpr RSXform(float scos_, float ssin_, float tx_, float ty_) noexcept
        : scos(scos_), ssin(ssin_), tx(tx_), ty(ty_) {}

    static RSXform from_radians(
        float scale,
        float radians,
        float tx,
        float ty,
        float ax = 0.0f,
        float ay = 0.0f
    ) noexcept {
        const float s = std::sin(radians) * scale;
        const float c = std::cos(radians) * scale;
        return RSXform(c, s, tx - c * ax + s * ay, ty - s * ax - c * ay);
    }

    static RSXform from_degrees(
        float scale,
        float degrees,
        float tx,
        float ty,
        float ax = 0.0f,
        float ay = 0.0f
    ) noexcept {
        return from_radians(scale, scalar::to_radians(degrees), tx, ty, ax, ay);
    }

    [[nodiscard]] constexpr Transform to_transform() const noexcept {
        return Transform::from_row(scos, ssin, -ssin, scos, tx, ty);
    }

    void to_quad(float width, float height, Point quad[4]) const noexcept {
        const float dx_w = scos * width;
        const float dy_w = ssin * width;
        const float dx_h = -ssin * height;
        const float dy_h = scos * height;
        quad[0] = Point(tx, ty);
        quad[1] = Point(tx + dx_w, ty + dy_w);
        quad[2] = Point(tx + dx_w + dx_h, ty + dy_w + dy_h);
        quad[3] = Point(tx + dx_h, ty + dy_h);
    }
};

} // namespace nisaba
