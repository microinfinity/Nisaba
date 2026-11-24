#include "nisaba/math/transform4x4.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/path_builder.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace nisaba {

namespace {

constexpr float DEG_TO_RAD = std::numbers::pi_v<float> / 180.0f;

} // namespace

Transform4x4 Transform4x4::from_rotate_x(float angle_degrees) noexcept {
    float rad = angle_degrees * DEG_TO_RAD;
    float c = std::cos(rad);
    float s = std::sin(rad);
    return Transform4x4(
        1.0f, 0.0f,  0.0f, 0.0f,
        0.0f,    c,    -s, 0.0f,
        0.0f,    s,     c, 0.0f,
        0.0f, 0.0f,  0.0f, 1.0f
    );
}

Transform4x4 Transform4x4::from_rotate_y(float angle_degrees) noexcept {
    float rad = angle_degrees * DEG_TO_RAD;
    float c = std::cos(rad);
    float s = std::sin(rad);
    return Transform4x4(
           c, 0.0f,     s, 0.0f,
        0.0f, 1.0f,  0.0f, 0.0f,
          -s, 0.0f,     c, 0.0f,
        0.0f, 0.0f,  0.0f, 1.0f
    );
}

Transform4x4 Transform4x4::from_rotate_z(float angle_degrees) noexcept {
    float rad = angle_degrees * DEG_TO_RAD;
    float c = std::cos(rad);
    float s = std::sin(rad);
    return Transform4x4(
           c,   -s,  0.0f, 0.0f,
           s,    c,  0.0f, 0.0f,
        0.0f, 0.0f,  1.0f, 0.0f,
        0.0f, 0.0f,  0.0f, 1.0f
    );
}

Transform4x4 Transform4x4::from_camera_orbit(
    float pivot_x, float pivot_y,
    float rot_x_deg, float rot_y_deg, float rot_z_deg,
    float perspective_distance
) noexcept {
    auto t_to_origin = from_translate(-pivot_x, -pivot_y, 0.0f);
    auto rx = from_rotate_x(rot_x_deg);
    auto ry = from_rotate_y(rot_y_deg);
    auto rz = from_rotate_z(rot_z_deg);
    auto p = from_perspective(perspective_distance);
    auto t_back = from_translate(pivot_x, pivot_y, 0.0f);

    // Compound transformation: T_back * P * Rz * Ry * Rx * T_to_origin
    return t_back * (p * (rz * (ry * (rx * t_to_origin))));
}

Transform4x4 Transform4x4::operator*(const Transform4x4& o) const noexcept {
    Transform4x4 res;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            res.m[r * 4 + c] =
                m[r * 4 + 0] * o.m[0 * 4 + c] +
                m[r * 4 + 1] * o.m[1 * 4 + c] +
                m[r * 4 + 2] * o.m[2 * 4 + c] +
                m[r * 4 + 3] * o.m[3 * 4 + c];
        }
    }
    return res;
}

std::optional<Transform4x4> Transform4x4::invert() const noexcept {
    float s0 = m[0] * m[5] - m[4] * m[1];
    float s1 = m[0] * m[6] - m[4] * m[2];
    float s2 = m[0] * m[7] - m[4] * m[3];
    float s3 = m[1] * m[6] - m[5] * m[2];
    float s4 = m[1] * m[7] - m[5] * m[3];
    float s5 = m[2] * m[7] - m[6] * m[3];

    float c5 = m[10] * m[15] - m[14] * m[11];
    float c4 = m[9]  * m[15] - m[13] * m[11];
    float c3 = m[9]  * m[14] - m[13] * m[10];
    float c2 = m[8]  * m[15] - m[12] * m[11];
    float c1 = m[8]  * m[14] - m[12] * m[10];
    float c0 = m[8]  * m[13] - m[12] * m[9];

    float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::abs(det) < 1e-12f) {
        return std::nullopt;
    }

    float inv_det = 1.0f / det;

    Transform4x4 b;
    b.m[0]  = ( m[5] * c5 - m[6] * c4 + m[7] * c3) * inv_det;
    b.m[1]  = (-m[1] * c5 + m[2] * c4 - m[3] * c3) * inv_det;
    b.m[2]  = ( m[13] * s5 - m[14] * s4 + m[15] * s3) * inv_det;
    b.m[3]  = (-m[9]  * s5 + m[10] * s4 - m[11] * s3) * inv_det;

    b.m[4]  = (-m[4] * c5 + m[6] * c2 - m[7] * c1) * inv_det;
    b.m[5]  = ( m[0] * c5 - m[2] * c2 + m[3] * c1) * inv_det;
    b.m[6]  = (-m[12] * s5 + m[14] * s2 - m[15] * s1) * inv_det;
    b.m[7]  = ( m[8]  * s5 - m[10] * s2 + m[11] * s1) * inv_det;

    b.m[8]  = ( m[4] * c4 - m[5] * c2 + m[7] * c0) * inv_det;
    b.m[9]  = (-m[0] * c4 + m[1] * c2 - m[3] * c0) * inv_det;
    b.m[10] = ( m[12] * s4 - m[13] * s2 + m[15] * s0) * inv_det;
    b.m[11] = (-m[8]  * s4 + m[9]  * s2 - m[11] * s0) * inv_det;

    b.m[12] = (-m[4] * c3 + m[5] * c1 - m[6] * c0) * inv_det;
    b.m[13] = ( m[0] * c3 - m[1] * c1 + m[2] * c0) * inv_det;
    b.m[14] = (-m[12] * s3 + m[13] * s1 - m[14] * s0) * inv_det;
    b.m[15] = ( m[8]  * s3 - m[9]  * s1 + m[10] * s0) * inv_det;

    return b;
}

Point Transform4x4::map_point(Point p) const noexcept {
    float x = m[0] * p.x + m[1] * p.y + m[3];
    float y = m[4] * p.x + m[5] * p.y + m[7];
    float w = m[12] * p.x + m[13] * p.y + m[15];

    if (std::abs(w) > 1e-7f) {
        float inv_w = 1.0f / w;
        return Point(x * inv_w, y * inv_w);
    }
    return Point(x, y);
}

std::optional<Point> Transform4x4::map_point_checked(Point p) const noexcept {
    float w = m[12] * p.x + m[13] * p.y + m[15];
    if (w <= 1e-6f) {
        return std::nullopt;
    }
    float x = m[0] * p.x + m[1] * p.y + m[3];
    float y = m[4] * p.x + m[5] * p.y + m[7];
    float inv_w = 1.0f / w;
    return Point(x * inv_w, y * inv_w);
}

Point Transform4x4::map_point3d(float px, float py, float pz) const noexcept {
    float x = m[0] * px + m[1] * py + m[2] * pz + m[3];
    float y = m[4] * px + m[5] * py + m[6] * pz + m[7];
    float w = m[12] * px + m[13] * py + m[14] * pz + m[15];

    if (std::abs(w) > 1e-7f) {
        float inv_w = 1.0f / w;
        return Point(x * inv_w, y * inv_w);
    }
    return Point(x, y);
}

std::optional<Transform4x4> Transform4x4::from_rect_to_quad(
    const Rect& src,
    Point p0, Point p1, Point p2, Point p3
) noexcept {
    float w = src.width();
    float h = src.height();
    if (w <= 0.0f || h <= 0.0f) {
        return std::nullopt;
    }

    float x0 = p0.x, y0 = p0.y;
    float x1 = p1.x, y1 = p1.y;
    float x2 = p2.x, y2 = p2.y;
    float x3 = p3.x, y3 = p3.y;

    float dx1 = x1 - x2;
    float dx2 = x3 - x2;
    float sx = x0 - x1 + x2 - x3;
    float dy1 = y1 - y2;
    float dy2 = y3 - y2;
    float sy = y0 - y1 + y2 - y3;

    float a11 = 0.0f, a12 = 0.0f, a13 = x0;
    float a21 = 0.0f, a22 = 0.0f, a23 = y0;
    float a31 = 0.0f, a32 = 0.0f, a33 = 1.0f;

    if (std::abs(sx) < 1e-6f && std::abs(sy) < 1e-6f) {
        // Affine mapping (parallelogram)
        a11 = x1 - x0;
        a12 = x3 - x0;
        a21 = y1 - y0;
        a22 = y3 - y0;
    } else {
        // Full projective mapping
        float det = dx1 * dy2 - dx2 * dy1;
        if (std::abs(det) < 1e-9f) {
            return std::nullopt;
        }

        a31 = (sx * dy2 - sy * dx2) / det;
        a32 = (dx1 * sy - dy1 * sx) / det;
        a11 = x1 - x0 + a31 * x1;
        a12 = x3 - x0 + a32 * x3;
        a21 = y1 - y0 + a31 * y1;
        a22 = y3 - y0 + a32 * y3;
    }

    // Now compose with map from src rect to unit square:
    // u' = (u - u0) / w, v' = (v - v0) / h
    float u0 = src.left();
    float v0 = src.top();

    float h00 = a11 / w;
    float h01 = a12 / h;
    float h02 = a13 - (a11 * u0 / w) - (a12 * v0 / h);

    float h10 = a21 / w;
    float h11 = a22 / h;
    float h12 = a23 - (a21 * u0 / w) - (a22 * v0 / h);

    float h20 = a31 / w;
    float h21 = a32 / h;
    float h22 = a33 - (a31 * u0 / w) - (a32 * v0 / h);

    return Transform4x4(
        h00,  h01,  0.0f, h02,
        h10,  h11,  0.0f, h12,
        0.0f, 0.0f, 1.0f, 0.0f,
        h20,  h21,  0.0f, h22
    );
}

namespace {

struct HomogPt {
    float x{0.0f};
    float y{0.0f};
    float w{1.0f};
};

inline HomogPt map_homog(const Transform4x4& t, Point p) noexcept {
    return HomogPt{
        t.m[0] * p.x + t.m[1] * p.y + t.m[3],
        t.m[4] * p.x + t.m[5] * p.y + t.m[7],
        t.m[12] * p.x + t.m[13] * p.y + t.m[15]
    };
}

constexpr float kNearW = 1e-4f;

// Sutherland-Hodgman clip of a homogeneous polygon against w >= kNearW
void clip_homog_poly(const std::vector<HomogPt>& in, std::vector<Point>& out) {
    out.clear();
    if (in.empty()) return;
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        const HomogPt& a = in[i];
        const HomogPt& b = in[(i + 1) % n];
        const bool a_in = (a.w >= kNearW);
        const bool b_in = (b.w >= kNearW);
        if (a_in && b_in) {
            out.emplace_back(b.x / b.w, b.y / b.w);
        } else if (a_in && !b_in) {
            const float t = (kNearW - a.w) / (b.w - a.w);
            const float x = a.x + t * (b.x - a.x);
            const float y = a.y + t * (b.y - a.y);
            out.emplace_back(x / kNearW, y / kNearW);
        } else if (!a_in && b_in) {
            const float t = (kNearW - a.w) / (b.w - a.w);
            const float x = a.x + t * (b.x - a.x);
            const float y = a.y + t * (b.y - a.y);
            out.emplace_back(x / kNearW, y / kNearW);
            out.emplace_back(b.x / b.w, b.y / b.w);
        }
    }
}

void flatten_quad_proj(
    const Transform4x4& t,
    Point p0, Point p1, Point p2,
    float tol_sq,
    int depth,
    std::vector<HomogPt>& out
) {
    const HomogPt h0 = map_homog(t, p0);
    const HomogPt h1 = map_homog(t, p1);
    const HomogPt h2 = map_homog(t, p2);
    if (depth < 8 && h0.w >= kNearW && h1.w >= kNearW && h2.w >= kNearW) {
        const Point s0(h0.x / h0.w, h0.y / h0.w);
        const Point s2(h2.x / h2.w, h2.y / h2.w);
        const Point mid_local(0.25f * p0.x + 0.5f * p1.x + 0.25f * p2.x,
                              0.25f * p0.y + 0.5f * p1.y + 0.25f * p2.y);
        const HomogPt hm = map_homog(t, mid_local);
        if (hm.w >= kNearW) {
            const Point sm(hm.x / hm.w, hm.y / hm.w);
            const float mx = 0.5f * (s0.x + s2.x);
            const float my = 0.5f * (s0.y + s2.y);
            const float dx = sm.x - mx;
            const float dy = sm.y - my;
            if (dx * dx + dy * dy > tol_sq) {
                const Point q01(0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y));
                const Point q12(0.5f * (p1.x + p2.x), 0.5f * (p1.y + p2.y));
                const Point m(0.5f * (q01.x + q12.x), 0.5f * (q01.y + q12.y));
                flatten_quad_proj(t, p0, q01, m, tol_sq, depth + 1, out);
                flatten_quad_proj(t, m, q12, p2, tol_sq, depth + 1, out);
                return;
            }
        }
    }
    out.push_back(h2);
}

void flatten_cubic_proj(
    const Transform4x4& t,
    Point p0, Point p1, Point p2, Point p3,
    float tol_sq,
    int depth,
    std::vector<HomogPt>& out
) {
    const HomogPt h0 = map_homog(t, p0);
    const HomogPt h3 = map_homog(t, p3);
    if (depth < 8 && h0.w >= kNearW && h3.w >= kNearW) {
        const Point s0(h0.x / h0.w, h0.y / h0.w);
        const Point s3(h3.x / h3.w, h3.y / h3.w);
        const Point m01(0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y));
        const Point m12(0.5f * (p1.x + p2.x), 0.5f * (p1.y + p2.y));
        const Point m23(0.5f * (p2.x + p3.x), 0.5f * (p2.y + p3.y));
        const Point m012(0.5f * (m01.x + m12.x), 0.5f * (m01.y + m12.y));
        const Point m123(0.5f * (m12.x + m23.x), 0.5f * (m12.y + m23.y));
        const Point mid(0.5f * (m012.x + m123.x), 0.5f * (m012.y + m123.y));

        const HomogPt hm = map_homog(t, mid);
        const HomogPt h1 = map_homog(t, p1);
        const HomogPt h2 = map_homog(t, p2);
        bool need_subdiv = (hm.w < kNearW || h1.w < kNearW || h2.w < kNearW);
        if (!need_subdiv) {
            const float dx = s3.x - s0.x;
            const float dy = s3.y - s0.y;
            const float len_sq = dx * dx + dy * dy;
            auto dist_to_seg_sq = [&](Point q) -> float {
                if (len_sq < 1e-8f) {
                    const float ex = q.x - s0.x;
                    const float ey = q.y - s0.y;
                    return ex * ex + ey * ey;
                }
                const float cross = (q.x - s0.x) * dy - (q.y - s0.y) * dx;
                return (cross * cross) / len_sq;
            };
            const Point s1(h1.x / h1.w, h1.y / h1.w);
            const Point s2(h2.x / h2.w, h2.y / h2.w);
            const Point sm(hm.x / hm.w, hm.y / hm.w);
            const float dmax = std::max({dist_to_seg_sq(s1), dist_to_seg_sq(s2), dist_to_seg_sq(sm)});
            need_subdiv = (dmax > tol_sq);
        }
        if (need_subdiv) {
            flatten_cubic_proj(t, p0, m01, m012, mid, tol_sq, depth + 1, out);
            flatten_cubic_proj(t, mid, m123, m23, p3, tol_sq, depth + 1, out);
            return;
        }
    }
    out.push_back(h3);
}

} // namespace

std::optional<Rect> Transform4x4::map_rect(const Rect& rect) const noexcept {
    if (!is_planar_perspective()) {
        return rect.transform(to_transform_2d());
    }
    const std::vector<HomogPt> quad = {
        map_homog(*this, Point::from_xy(rect.left(), rect.top())),
        map_homog(*this, Point::from_xy(rect.right(), rect.top())),
        map_homog(*this, Point::from_xy(rect.right(), rect.bottom())),
        map_homog(*this, Point::from_xy(rect.left(), rect.bottom()))
    };
    std::vector<Point> clipped;
    clipped.reserve(6);
    clip_homog_poly(quad, clipped);
    if (clipped.empty()) return std::nullopt;
    return Rect::from_points(clipped.data(), clipped.size());
}

Path Transform4x4::project_path(const Path& path, float flatness_tol) const {
    if (path.is_empty()) return Path();
    if (!is_planar_perspective()) {
        auto tr = path.transform(to_transform_2d());
        return tr ? std::move(*tr) : Path();
    }

    const float tol = std::max(0.1f, flatness_tol);
    const float tol_sq = tol * tol;

    PathBuilder pb;
    std::vector<HomogPt> contour;
    std::vector<Point> clipped;
    bool contour_closed = false;

    auto flush_contour = [&]() {
        if (contour.empty()) return;
        if (contour_closed && contour.size() >= 3) {
            clip_homog_poly(contour, clipped);
            if (clipped.size() >= 3) {
                pb.move_to(clipped[0]);
                for (size_t i = 1; i < clipped.size(); ++i) {
                    pb.line_to(clipped[i]);
                }
                pb.close();
            }
        } else if (contour.size() >= 2) {
            bool pen_down = false;
            for (size_t i = 0; i + 1 < contour.size(); ++i) {
                const HomogPt& a = contour[i];
                const HomogPt& b = contour[i + 1];
                const bool a_in = (a.w >= kNearW);
                const bool b_in = (b.w >= kNearW);
                if (!a_in && !b_in) {
                    pen_down = false;
                    continue;
                }
                Point pa, pb_pt;
                if (a_in) {
                    pa = Point(a.x / a.w, a.y / a.w);
                } else {
                    const float t = (kNearW - a.w) / (b.w - a.w);
                    pa = Point((a.x + t * (b.x - a.x)) / kNearW, (a.y + t * (b.y - a.y)) / kNearW);
                    pen_down = false;
                }
                if (b_in) {
                    pb_pt = Point(b.x / b.w, b.y / b.w);
                } else {
                    const float t = (kNearW - a.w) / (b.w - a.w);
                    pb_pt = Point((a.x + t * (b.x - a.x)) / kNearW, (a.y + t * (b.y - a.y)) / kNearW);
                }
                if (!pen_down) {
                    pb.move_to(pa);
                    pen_down = true;
                }
                pb.line_to(pb_pt);
                if (!b_in) pen_down = false;
            }
        }
        contour.clear();
        contour_closed = false;
    };

    Point cur_pt(0.0f, 0.0f);
    Point start_pt(0.0f, 0.0f);
    auto iter = path.segments();
    while (auto seg_opt = iter.next()) {
        const auto& seg = *seg_opt;
        switch (seg.type) {
            case PathSegment::Type::MoveTo:
                flush_contour();
                cur_pt = seg.p0;
                start_pt = seg.p0;
                contour.push_back(map_homog(*this, seg.p0));
                break;
            case PathSegment::Type::LineTo:
                if (contour.empty()) contour.push_back(map_homog(*this, cur_pt));
                contour.push_back(map_homog(*this, seg.p0));
                cur_pt = seg.p0;
                break;
            case PathSegment::Type::QuadTo:
                if (contour.empty()) contour.push_back(map_homog(*this, cur_pt));
                flatten_quad_proj(*this, cur_pt, seg.p0, seg.p1, tol_sq, 0, contour);
                cur_pt = seg.p1;
                break;
            case PathSegment::Type::CubicTo:
                if (contour.empty()) contour.push_back(map_homog(*this, cur_pt));
                flatten_cubic_proj(*this, cur_pt, seg.p0, seg.p1, seg.p2, tol_sq, 0, contour);
                cur_pt = seg.p2;
                break;
            case PathSegment::Type::Close:
                contour_closed = true;
                cur_pt = start_pt;
                flush_contour();
                break;
        }
    }
    flush_contour();

    auto finished = pb.finish();
    return finished ? std::move(*finished) : Path();
}

} // namespace nisaba
