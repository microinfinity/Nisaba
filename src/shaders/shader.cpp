#include "nisaba/shaders/shader.hpp"
#include "nisaba/color/blend.hpp"
#include <cmath>
#include <algorithm>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace nisaba {

namespace {

constexpr float DEGENERATE_THRESHOLD = 1.0f / (1 << 15);

inline Color average_gradient_color(const std::vector<GradientStop>& stops) noexcept {
    if (stops.empty()) return Color::TRANSPARENT;
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    for (const auto& s : stops) {
        r += s.color.red();
        g += s.color.green();
        b += s.color.blue();
        a += s.color.alpha();
    }
    float inv = 1.0f / static_cast<float>(stops.size());
    return Color::from_rgba_unchecked(r * inv, g * inv, b * inv, a * inv);
}

inline std::optional<Transform> points_to_unit_ts(Point start, Point end) noexcept {
    Point vec = end - start;
    float mag = vec.length();
    if (!std::isfinite(mag) || mag <= DEGENERATE_THRESHOLD) {
        return std::nullopt;
    }
    float inv = 1.0f / mag;
    vec.x *= inv;
    vec.y *= inv;

    auto rot = Transform::from_sin_cos_at(-vec.y, vec.x, start.x, start.y);
    if (!rot) return std::nullopt;

    return rot->post_translate(-start.x, -start.y).post_scale(inv, inv);
}

inline PremultipliedColor premul(Color c) noexcept {
    return c.premultiply();
}

inline std::optional<float> solve_conical_t(
    float px, float py,
    float start_radius, [[maybe_unused]] float end_radius,
    float dx, float dy, float dr, float A,
    bool is_concentric
) noexcept {
    if (is_concentric) {
        float dist = std::hypot(px, py);
        if (std::abs(dr) > 1e-6f) {
            return (dist - start_radius) / dr;
        }
        return 0.0f;
    }

    float B = -2.0f * (px * dx + py * dy + start_radius * dr);
    float C = px * px + py * py - start_radius * start_radius;

    if (std::abs(A) < 1e-7f) {
        if (std::abs(B) < 1e-7f) return std::nullopt;
        float t = -C / B;
        if (start_radius + t * dr < 0.0f) return std::nullopt;
        return t;
    }

    float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f) return std::nullopt;

    float sqrt_disc = std::sqrt(disc);
    float t1 = (-B - sqrt_disc) / (2.0f * A);
    float t2 = (-B + sqrt_disc) / (2.0f * A);

    float r1 = start_radius + t1 * dr;
    float r2 = start_radius + t2 * dr;

    bool v1 = (r1 >= -1e-4f);
    bool v2 = (r2 >= -1e-4f);

    if (v1 && !v2) return t1;
    if (v2 && !v1) return t2;
    if (v1 && v2) {
        return (dr >= 0.0f) ? t1 : t2;
    }
    return std::nullopt;
}

} // namespace

// =========================================================================
// Gradient
// =========================================================================

Gradient::Gradient(
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform,
    Transform points_to_unit
) : tile_mode_(mode),
    transform_(transform),
    points_to_unit_(points_to_unit)
{
    if (stops.empty()) return;

    bool dummy_first = stops.front().position.get() != 0.0f;
    bool dummy_last = stops.back().position.get() != 1.0f;

    stops_.reserve(stops.size() + 2);
    if (dummy_first) {
        stops_.push_back(GradientStop::create(0.0f, stops.front().color));
    }
    for (const auto& s : stops) {
        stops_.push_back(s);
    }
    if (dummy_last) {
        stops_.push_back(GradientStop::create(1.0f, stops.back().color));
    }

    update_alpha_flags();

    // Ensure monotonicity
    size_t start_index = dummy_first ? 0 : 1;
    float prev = 0.0f;
    has_uniform_stops_ = true;
    float uniform_step = stops_[start_index].position.get() - prev;

    for (size_t i = start_index; i < stops_.size(); ++i) {
        float curr = (i + 1 == stops_.size()) ? 1.0f : scalar::bound(stops_[i].position.get(), prev, 1.0f);
        has_uniform_stops_ &= scalar::is_nearly_equal(uniform_step, curr - prev);
        stops_[i].position = NormalizedF32::create_clamped(curr);
        prev = curr;
    }

    update_inv();
    bake_ramp();
}

void Gradient::bake_ramp() noexcept {
    if (stops_.empty()) {
        std::fill(ramp_.begin(), ramp_.end(), PremultipliedColorU8::TRANSPARENT);
        return;
    }
    if (stops_.size() == 1) {
        PremultipliedColorU8 c = stops_[0].color.premultiply().to_color_u8();
        std::fill(ramp_.begin(), ramp_.end(), c);
        return;
    }

    constexpr size_t N = RAMP_SIZE;
    for (size_t s = 0; s + 1 < stops_.size(); ++s) {
        float p0 = stops_[s].position.get();
        float p1 = stops_[s + 1].position.get();

        size_t idx0 = static_cast<size_t>(std::clamp(p0 * static_cast<float>(N - 1) + 0.5f, 0.0f, static_cast<float>(N - 1)));
        size_t idx1 = static_cast<size_t>(std::clamp(p1 * static_cast<float>(N - 1) + 0.5f, 0.0f, static_cast<float>(N - 1)));

        Color c0 = stops_[s].color;
        Color c1 = stops_[s + 1].color;

        if (idx0 == idx1) {
            ramp_[idx0] = c0.premultiply().to_color_u8();
            continue;
        }

        float inv_len = 1.0f / static_cast<float>(idx1 - idx0);
        float r0 = c0.red(), g0 = c0.green(), b0 = c0.blue(), a0 = c0.alpha();
        float dr = (c1.red() - r0) * inv_len;
        float dg = (c1.green() - g0) * inv_len;
        float db = (c1.blue() - b0) * inv_len;
        float da = (c1.alpha() - a0) * inv_len;

        float r = r0, g = g0, b = b0, a = a0;
        for (size_t i = idx0; i <= idx1; ++i) {
            Color cur = Color::from_rgba_unchecked(
                scalar::bound(r, 0.0f, 1.0f),
                scalar::bound(g, 0.0f, 1.0f),
                scalar::bound(b, 0.0f, 1.0f),
                scalar::bound(a, 0.0f, 1.0f)
            );
            ramp_[i] = cur.premultiply().to_color_u8();
            r += dr; g += dg; b += db; a += da;
        }
    }

    float first_p = stops_.front().position.get();
    size_t first_idx = static_cast<size_t>(std::clamp(first_p * static_cast<float>(N - 1) + 0.5f, 0.0f, static_cast<float>(N - 1)));
    if (first_idx > 0) {
        PremultipliedColorU8 c = ramp_[first_idx];
        std::fill_n(ramp_.begin(), first_idx, c);
    }
    float last_p = stops_.back().position.get();
    size_t last_idx = static_cast<size_t>(std::clamp(last_p * static_cast<float>(N - 1) + 0.5f, 0.0f, static_cast<float>(N - 1)));
    if (last_idx < N - 1) {
        PremultipliedColorU8 c = ramp_[last_idx];
        std::fill(ramp_.begin() + last_idx + 1, ramp_.end(), c);
    }
}

void Gradient::update_inv() noexcept {
    auto inv = transform_.invert();
    if (inv) {
        inv_combined_ = inv->post_concat(points_to_unit_);
    } else {
        inv_combined_ = std::nullopt;
    }
}

void Gradient::set_transform(const Transform& ts) noexcept {
    transform_ = ts;
    update_inv();
}

void Gradient::post_concat(const Transform& ts) noexcept {
    transform_ = transform_.post_concat(ts);
    update_inv();
}

void Gradient::update_alpha_flags() noexcept {
    colors_are_opaque_ = true;
    has_uniform_alpha_ = true;
    uint8_t first_a = stops_.empty() ? 255 : static_cast<uint8_t>(stops_[0].color.alpha() * 255.0f + 0.5f);
    uniform_alpha_ = first_a;

    for (const auto& s : stops_) {
        uint8_t a = static_cast<uint8_t>(s.color.alpha() * 255.0f + 0.5f);
        if (a != first_a) {
            has_uniform_alpha_ = false;
        }
        if (!s.color.is_opaque()) {
            colors_are_opaque_ = false;
        }
    }
}

void Gradient::apply_opacity(float opacity) noexcept {
    for (auto& s : stops_) {
        s.color.apply_opacity(opacity);
    }
    update_alpha_flags();
    bake_ramp();
}

float Gradient::map_spread(float t) const noexcept {
    switch (tile_mode_) {
        case SpreadMode::Pad:
            return std::clamp(t, 0.0f, 1.0f);

        case SpreadMode::Repeat: {
            float f = t - std::floor(t);
            return (f < 0.0f) ? f + 1.0f : f;
        }

        case SpreadMode::Reflect: {
            float t2 = t * 0.5f;
            float f = t2 - std::floor(t2);
            if (f < 0.5f) {
                return f * 2.0f;
            } else {
                return (1.0f - f) * 2.0f;
            }
        }
    }
    return std::clamp(t, 0.0f, 1.0f);
}

Color Gradient::eval(float t) const noexcept {
    if (stops_.empty()) return Color::TRANSPARENT;
    if (stops_.size() == 1) return stops_[0].color;

    float tm = map_spread(t);

    if (tm <= stops_.front().position.get()) {
        return stops_.front().color;
    }
    if (tm >= stops_.back().position.get()) {
        return stops_.back().color;
    }

    // Binary search or linear scan
    for (size_t i = 0; i + 1 < stops_.size(); ++i) {
        float p0 = stops_[i].position.get();
        float p1 = stops_[i + 1].position.get();
        if (tm >= p0 && tm <= p1) {
            float range = p1 - p0;
            float rel = (range > 1e-6f) ? ((tm - p0) / range) : 0.0f;

            const auto& c0 = stops_[i].color;
            const auto& c1 = stops_[i + 1].color;

            float r = c0.red() + (c1.red() - c0.red()) * rel;
            float g = c0.green() + (c1.green() - c0.green()) * rel;
            float b = c0.blue() + (c1.blue() - c0.blue()) * rel;
            float a = c0.alpha() + (c1.alpha() - c0.alpha()) * rel;

            return Color::from_rgba_unchecked(
                scalar::bound(r, 0.0f, 1.0f),
                scalar::bound(g, 0.0f, 1.0f),
                scalar::bound(b, 0.0f, 1.0f),
                scalar::bound(a, 0.0f, 1.0f)
            );
        }
    }

    return stops_.back().color;
}

// =========================================================================
// LinearGradient
// =========================================================================

LinearGradient::LinearGradient(
    Point start,
    Point end,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform,
    Transform points_to_unit
) : Gradient(std::move(stops), mode, transform, points_to_unit),
    start_(start),
    end_(end),
    dx_(end.x - start.x),
    dy_(end.y - start.y)
{
    float len_sq = dx_ * dx_ + dy_ * dy_;
    inv_len_sq_ = (len_sq > 1e-10f) ? (1.0f / len_sq) : 0.0f;
}

bool LinearGradient::is_horizontal() const noexcept {
    return inv_combined_ && std::abs(inv_combined_->kx) < 1e-6f;
}

std::optional<LinearGradient> LinearGradient::create(
    Point start,
    Point end,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform
) {
    if (stops.empty()) return std::nullopt;

    auto unit_ts = points_to_unit_ts(start, end);
    if (!unit_ts) {
        // Degenerate
        if (mode == SpreadMode::Pad) {
            stops = {GradientStop::create(0.0f, stops.back().color), GradientStop::create(1.0f, stops.back().color)};
        } else {
            Color avg = average_gradient_color(stops);
            stops = {GradientStop::create(0.0f, avg), GradientStop::create(1.0f, avg)};
        }
        unit_ts = Transform();
    }

    if (!transform.invert()) {
        return std::nullopt;
    }

    return LinearGradient(start, end, std::move(stops), mode, transform, *unit_ts);
}

Color LinearGradient::sample(float x, float y) const noexcept {
    if (!inv_combined_) return Color::TRANSPARENT;
    Point p = Point::from_xy(x, y);
    inv_combined_->map_point(p);
    return eval(p.x);
}

PremultipliedColor LinearGradient::sample_premul(float x, float y) const noexcept {
    return premul(sample(x, y));
}

void LinearGradient::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept {
    if (!inv_combined_ || count == 0) {
        std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        return;
    }

    const Transform& inv = *inv_combined_;
    float t0 = inv.sx * x + inv.kx * y + inv.tx;
    float dt = inv.sx;

    // Fixed-point 16.16 setup
    int32_t t = static_cast<int32_t>(t0 * 65536.0f + 0.5f);
    int32_t dt_fp = static_cast<int32_t>(dt * 65536.0f);

    if (tile_mode_ == SpreadMode::Pad) {
        if (dt_fp == 0) {
            PremultipliedColorU8 col = lookup_ramp(t0);
            std::fill_n(dst, count, col);
            return;
        }

        if (dt_fp > 0) {
            // 1. Prefix: t < 0
            if (t < 0) {
                int64_t steps = (-static_cast<int64_t>(t) + dt_fp - 1) / dt_fp;
                uint32_t n0 = static_cast<uint32_t>(std::min(static_cast<int64_t>(count), steps));
                if (n0 > 0) {
                    PremultipliedColorU8 c0 = ramp_[0];
                    std::fill_n(dst, n0, c0);
                    dst += n0;
                    count -= n0;
                    t += static_cast<int32_t>(n0) * dt_fp;
                }
            }
            // 2. Active ramp: 0 <= t < 65536
            if (count > 0 && t < 65536) {
                int64_t rem = 65536 - t;
                int64_t steps = (rem + dt_fp - 1) / dt_fp;
                uint32_t n1 = static_cast<uint32_t>(std::min(static_cast<int64_t>(count), steps));
#if defined(__AVX2__) || defined(NISABA_HAS_AVX2)
                uint32_t c8 = n1 & ~7u;
                if (c8 > 0) {
                    const int* ramp_i32 = reinterpret_cast<const int*>(ramp_.data());
                    __m256i v_t = _mm256_setr_epi32(
                        t,
                        t + dt_fp,
                        t + 2 * dt_fp,
                        t + 3 * dt_fp,
                        t + 4 * dt_fp,
                        t + 5 * dt_fp,
                        t + 6 * dt_fp,
                        t + 7 * dt_fp
                    );
                    __m256i v_step = _mm256_set1_epi32(8 * dt_fp);

                    for (uint32_t i = 0; i < c8; i += 8) {
                        __m256i v_idx = _mm256_srli_epi32(v_t, 8);
                        __m256i pix = _mm256_i32gather_epi32(ramp_i32, v_idx, 4);
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), pix);
                        v_t = _mm256_add_epi32(v_t, v_step);
                    }
                    t += static_cast<int32_t>(c8) * dt_fp;
                    dst += c8;
                    count -= c8;
                    n1 -= c8;
                }
#endif
                uint32_t c4 = n1 & ~3u;
                for (uint32_t i = 0; i < c4; i += 4) {
                    dst[i + 0] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                    dst[i + 1] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                    dst[i + 2] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                    dst[i + 3] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                }
                for (uint32_t i = c4; i < n1; ++i) {
                    dst[i] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                }
                dst += n1;
                count -= n1;
            }
            // 3. Suffix: t >= 65536
            if (count > 0) {
                PremultipliedColorU8 c255 = ramp_[255];
                std::fill_n(dst, count, c255);
            }
        } else {
            int32_t step = -dt_fp;
            // 1. Prefix: t >= 65536
            if (t >= 65536) {
                int64_t diff = t - 65535;
                int64_t steps = (diff + step - 1) / step;
                uint32_t n0 = static_cast<uint32_t>(std::min(static_cast<int64_t>(count), steps));
                if (n0 > 0) {
                    PremultipliedColorU8 c255 = ramp_[255];
                    std::fill_n(dst, n0, c255);
                    dst += n0;
                    count -= n0;
                    t += static_cast<int32_t>(n0) * dt_fp;
                }
            }
            // 2. Active ramp: 0 <= t < 65536
            if (count > 0 && t >= 0) {
                int64_t rem = t + 1;
                int64_t steps = (rem + step - 1) / step;
                uint32_t n1 = static_cast<uint32_t>(std::min(static_cast<int64_t>(count), steps));
#if defined(__AVX2__) || defined(NISABA_HAS_AVX2)
                uint32_t c8 = n1 & ~7u;
                if (c8 > 0) {
                    const int* ramp_i32 = reinterpret_cast<const int*>(ramp_.data());
                    __m256i v_t = _mm256_setr_epi32(
                        t,
                        t + dt_fp,
                        t + 2 * dt_fp,
                        t + 3 * dt_fp,
                        t + 4 * dt_fp,
                        t + 5 * dt_fp,
                        t + 6 * dt_fp,
                        t + 7 * dt_fp
                    );
                    __m256i v_step = _mm256_set1_epi32(8 * dt_fp);

                    for (uint32_t i = 0; i < c8; i += 8) {
                        __m256i v_idx = _mm256_srli_epi32(v_t, 8);
                        __m256i pix = _mm256_i32gather_epi32(ramp_i32, v_idx, 4);
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), pix);
                        v_t = _mm256_add_epi32(v_t, v_step);
                    }
                    t += static_cast<int32_t>(c8) * dt_fp;
                    dst += c8;
                    count -= c8;
                    n1 -= c8;
                }
#endif
                uint32_t c4 = n1 & ~3u;
                for (uint32_t i = 0; i < c4; i += 4) {
                    dst[i + 0] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                    dst[i + 1] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                    dst[i + 2] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                    dst[i + 3] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                }
                for (uint32_t i = c4; i < n1; ++i) {
                    dst[i] = ramp_[static_cast<uint32_t>(t) >> 8]; t += dt_fp;
                }
                dst += n1;
                count -= n1;
            }
            // 3. Suffix: t < 0
            if (count > 0) {
                PremultipliedColorU8 c0 = ramp_[0];
                std::fill_n(dst, count, c0);
            }
        }
    } else if (tile_mode_ == SpreadMode::Repeat) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t f = static_cast<uint32_t>(t) & 0xFFFF;
            uint32_t idx = (f * 255) >> 16;
            dst[i] = ramp_[idx];
            t += dt_fp;
        }
    } else { // SpreadMode::Reflect
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t f = static_cast<uint32_t>(t) & 0x1FFFF;
            if (f >= 0x10000) f = 0x20000 - f;
            uint32_t idx = (f * 255) >> 16;
            dst[i] = ramp_[idx];
            t += dt_fp;
        }
    }
}

// =========================================================================
// RadialGradient
// =========================================================================

std::optional<RadialGradient> RadialGradient::create(
    Point center,
    float radius,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform
) {
    if (stops.empty() || radius <= 0.0f || !std::isfinite(radius)) {
        return std::nullopt;
    }
    if (!transform.invert()) {
        return std::nullopt;
    }

    Transform unit_ts = Transform::from_translate(-center.x, -center.y).post_scale(1.0f / radius, 1.0f / radius);

    RadialGradient g;
    g.stops_ = std::move(stops);
    g.tile_mode_ = mode;
    g.transform_ = transform;
    g.points_to_unit_ = unit_ts;
    g.center_ = center;
    g.radius_ = radius;
    g.inv_radius_ = 1.0f / radius;
    g.is_2point_ = false;

    static_cast<Gradient&>(g) = Gradient(g.stops_, mode, transform, unit_ts);
    g.pad_color_ = g.lookup_ramp(1.0f);
    for (size_t i = 0; i < RAMP_SQ_SIZE; ++i) {
        float u2 = static_cast<float>(i) / static_cast<float>(RAMP_SQ_SIZE - 1);
        float u = std::sqrt(u2);
        g.ramp_sq_[i] = g.lookup_ramp(u);
    }
    return g;
}

std::optional<RadialGradient> RadialGradient::create_2point(
    Point start,
    float start_radius,
    Point end,
    float end_radius,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform
) {
    if (stops.empty()) return std::nullopt;
    if (!transform.invert()) return std::nullopt;

    RadialGradient g;
    g.stops_ = std::move(stops);
    g.tile_mode_ = mode;
    g.transform_ = transform;
    g.is_2point_ = true;
    g.start_ = start;
    g.start_radius_ = start_radius;
    g.end_ = end;
    g.end_radius_ = end_radius;
    g.center_ = start;
    g.radius_ = std::max(start_radius, end_radius);
    g.inv_radius_ = (g.radius_ > 0.0f) ? (1.0f / g.radius_) : 1.0f;

    Transform unit_ts = Transform();
    g.points_to_unit_ = unit_ts;

    static_cast<Gradient&>(g) = Gradient(g.stops_, mode, transform, unit_ts);
    g.pad_color_ = g.lookup_ramp(1.0f);
    for (size_t i = 0; i < RAMP_SQ_SIZE; ++i) {
        float u2 = static_cast<float>(i) / static_cast<float>(RAMP_SQ_SIZE - 1);
        float u = std::sqrt(u2);
        g.ramp_sq_[i] = g.lookup_ramp(u);
    }
    return g;
}

Color RadialGradient::sample(float x, float y) const noexcept {
    if (!inv_combined_) return Color::TRANSPARENT;
    Point p = Point::from_xy(x, y);
    inv_combined_->map_point(p);
    if (is_2point_) {
        float px = p.x - start_.x;
        float py = p.y - start_.y;
        float dx = end_.x - start_.x;
        float dy = end_.y - start_.y;
        float dr = end_radius_ - start_radius_;
        float A = dx * dx + dy * dy - dr * dr;
        bool is_concentric = (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f);
        auto t = solve_conical_t(px, py, start_radius_, end_radius_, dx, dy, dr, A, is_concentric);
        if (!t) return Color::TRANSPARENT;
        return eval(*t);
    }
    float d = std::sqrt(p.x * p.x + p.y * p.y);
    return eval(d);
}

PremultipliedColor RadialGradient::sample_premul(float x, float y) const noexcept {
    return premul(sample(x, y));
}

void RadialGradient::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept {
    if (!inv_combined_ || count == 0) {
        std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        return;
    }

    const Transform& inv = *inv_combined_;
    float px0 = inv.sx * x + inv.kx * y + inv.tx;
    float py0 = inv.ky * x + inv.sy * y + inv.ty;
    float dpx = inv.sx;
    float dpy = inv.ky;

    if (tile_mode_ == SpreadMode::Pad && !is_2point_) {
        // Zero-Sqrt Quadratic DDA Algorithm (Pure C++, 100% portable, MCU-ready)
        double A = static_cast<double>(dpx * dpx + dpy * dpy);
        double B = 2.0 * static_cast<double>(px0 * dpx + py0 * dpy);
        double C = static_cast<double>(px0 * px0 + py0 * py0);

        constexpr double S = static_cast<double>(RAMP_SQ_SIZE - 1);
        constexpr int64_t FP_ONE = 1 << 16;
        int64_t v_fp = static_cast<int64_t>(C * S * FP_ONE);
        int64_t dv1_fp = static_cast<int64_t>((A + B) * S * FP_ONE);
        int64_t dv2_fp = static_cast<int64_t>(2.0 * A * S * FP_ONE);

        const PremultipliedColorU8* lut = ramp_sq_.data();
        PremultipliedColorU8 pad_col = pad_color_;

        uint32_t i = 0;
        uint32_t count4 = count & ~3u;
        for (; i < count4; i += 4) {
            uint32_t idx0 = static_cast<uint32_t>(v_fp >> 16);
            dst[i + 0] = (idx0 < RAMP_SQ_SIZE) ? lut[idx0] : ((v_fp < 0) ? lut[0] : pad_col);
            v_fp += dv1_fp; dv1_fp += dv2_fp;

            uint32_t idx1 = static_cast<uint32_t>(v_fp >> 16);
            dst[i + 1] = (idx1 < RAMP_SQ_SIZE) ? lut[idx1] : ((v_fp < 0) ? lut[0] : pad_col);
            v_fp += dv1_fp; dv1_fp += dv2_fp;

            uint32_t idx2 = static_cast<uint32_t>(v_fp >> 16);
            dst[i + 2] = (idx2 < RAMP_SQ_SIZE) ? lut[idx2] : ((v_fp < 0) ? lut[0] : pad_col);
            v_fp += dv1_fp; dv1_fp += dv2_fp;

            uint32_t idx3 = static_cast<uint32_t>(v_fp >> 16);
            dst[i + 3] = (idx3 < RAMP_SQ_SIZE) ? lut[idx3] : ((v_fp < 0) ? lut[0] : pad_col);
            v_fp += dv1_fp; dv1_fp += dv2_fp;
        }

        for (; i < count; ++i) {
            uint32_t idx = static_cast<uint32_t>(v_fp >> 16);
            dst[i] = (idx < RAMP_SQ_SIZE) ? lut[idx] : ((v_fp < 0) ? lut[0] : pad_col);
            v_fp += dv1_fp; dv1_fp += dv2_fp;
        }
        return;
    }

    // Fallback for Repeat / Reflect / 2-point
    if (is_2point_) {
        float dx = end_.x - start_.x;
        float dy = end_.y - start_.y;
        float dr = end_radius_ - start_radius_;
        float A = dx * dx + dy * dy - dr * dr;
        bool is_concentric = (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f);
        float px = px0 - start_.x;
        float py = py0 - start_.y;
        for (uint32_t i = 0; i < count; ++i) {
            auto t = solve_conical_t(px, py, start_radius_, end_radius_, dx, dy, dr, A, is_concentric);
            dst[i] = t ? lookup_ramp(*t) : PremultipliedColorU8::TRANSPARENT;
            px += dpx;
            py += dpy;
        }
        return;
    }

    float px = px0;
    float py = py0;
    for (size_t i = 0; i < count; ++i) {
        float r2 = px * px + py * py;
        float d = std::sqrt(r2);
        dst[i] = lookup_ramp(d);
        px += dpx;
        py += dpy;
    }
}

// =========================================================================
// SweepGradient
// =========================================================================

std::optional<SweepGradient> SweepGradient::create(
    Point center,
    float start_angle_deg,
    float end_angle_deg,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform
) {
    if (stops.empty()) return std::nullopt;
    if (!transform.invert()) return std::nullopt;

    float start_rad = start_angle_deg * (FLOAT_PI / 180.0f);
    float end_rad = end_angle_deg * (FLOAT_PI / 180.0f);
    float range_rad = end_rad - start_rad;
    if (std::abs(range_rad) < 1e-6f) {
        range_rad = FLOAT_PI * 2.0f;
    }

    Transform unit_ts = Transform::from_translate(-center.x, -center.y);

    SweepGradient g;
    g.stops_ = std::move(stops);
    g.tile_mode_ = mode;
    g.transform_ = transform;
    g.points_to_unit_ = unit_ts;
    g.center_ = center;
    g.start_angle_rad_ = start_rad;
    g.inv_angle_range_ = 1.0f / range_rad;

    static_cast<Gradient&>(g) = Gradient(g.stops_, mode, transform, unit_ts);
    return g;
}

Color SweepGradient::sample(float x, float y) const noexcept {
    if (!inv_combined_) return Color::TRANSPARENT;
    Point p = Point::from_xy(x, y);
    inv_combined_->map_point(p);
    float angle = std::atan2(p.y, p.x);
    if (angle < 0.0f) {
        angle += FLOAT_PI * 2.0f;
    }
    float t = (angle - start_angle_rad_) * inv_angle_range_;
    return eval(t);
}

PremultipliedColor SweepGradient::sample_premul(float x, float y) const noexcept {
    return premul(sample(x, y));
}

// =========================================================================
// Pattern
// =========================================================================

Pattern::Pattern(
    PixmapRef pixmap,
    SpreadMode mode,
    FilterQuality quality,
    float opacity,
    Transform transform
) noexcept
    : pixmap_(pixmap),
      mode_(mode),
      quality_(quality),
      opacity_(std::clamp(opacity, 0.0f, 1.0f)),
      transform_(transform)
{
    update_inv();
    if (pixmap_.format() != PixelFormat::RGBA8888 &&
        pixmap_.width() > 0 && pixmap_.height() > 0 && pixmap_.data() != nullptr) {
        rgba_storage_ = std::make_shared<Pixmap>(pixmap_.to_rgba8888());
    }
    is_opaque_ = (opacity_ >= 0.999f && pixmap_.is_opaque());
}

void Pattern::update_inv() noexcept {
    inv_ts_ = transform_.invert();
}

void Pattern::set_transform(const Transform& ts) noexcept {
    transform_ = ts;
    update_inv();
}

void Pattern::post_concat(const Transform& ts) noexcept {
    transform_ = transform_.post_concat(ts);
    update_inv();
}

void Pattern::apply_opacity(float opacity) noexcept {
    opacity_ = std::clamp(opacity_ * opacity, 0.0f, 1.0f);
    if (opacity_ < 0.999f) is_opaque_ = false;
}

namespace {

inline uint32_t pattern_clamp_or_wrap(int32_t v, uint32_t len, SpreadMode mode) noexcept {
    if (len == 0) return 0;
    if (mode == SpreadMode::Repeat) {
        if ((len & (len - 1)) == 0) {
            return static_cast<uint32_t>(v) & (len - 1);
        }
        int32_t m = v % static_cast<int32_t>(len);
        if (m < 0) m += static_cast<int32_t>(len);
        return static_cast<uint32_t>(m);
    } else if (mode == SpreadMode::Reflect) {
        if ((len & (len - 1)) == 0) {
            uint32_t mask = (len << 1) - 1;
            uint32_t m = static_cast<uint32_t>(v) & mask;
            if (m >= len) m = mask - m;
            return m;
        }
        int32_t double_len = static_cast<int32_t>(len * 2);
        int32_t m = v % double_len;
        if (m < 0) m += double_len;
        if (m >= static_cast<int32_t>(len)) m = double_len - 1 - m;
        return static_cast<uint32_t>(m);
    } else {
        // SpreadMode::Pad
        return static_cast<uint32_t>(std::clamp(v, 0, static_cast<int32_t>(len - 1)));
    }
}

} // anonymous namespace

PremultipliedColor Pattern::sample_premul(float x, float y) const noexcept {
    if (!inv_ts_ || pixmap_.width() == 0 || pixmap_.height() == 0) {
        return PremultipliedColor();
    }

    Point pt = Point::from_xy(x, y);
    inv_ts_->map_point(pt);
    const PixmapRef active_pm = rgba_storage_ ? rgba_storage_->as_ref() : pixmap_;
    const uint32_t w = active_pm.width();
    const uint32_t h = active_pm.height();

    auto to_pm = [&](uint32_t px, uint32_t py) noexcept -> PremultipliedColor {
        auto opt = active_pm.pixel(px, py);
        if (!opt) return PremultipliedColor();
        constexpr float inv255 = 1.0f / 255.0f;
        return PremultipliedColor(
            NormalizedF32::create_unchecked(opt->red() * inv255),
            NormalizedF32::create_unchecked(opt->green() * inv255),
            NormalizedF32::create_unchecked(opt->blue() * inv255),
            NormalizedF32::create_unchecked(opt->alpha() * inv255)
        );
    };

    PremultipliedColor res;
    if (quality_ == FilterQuality::Nearest) {
        uint32_t ix = pattern_clamp_or_wrap(static_cast<int32_t>(std::floor(pt.x)), w, mode_);
        uint32_t iy = pattern_clamp_or_wrap(static_cast<int32_t>(std::floor(pt.y)), h, mode_);
        res = to_pm(ix, iy);
    } else {
        float uf = pt.x - 0.5f;
        float vf = pt.y - 0.5f;
        float fu_floor = std::floor(uf);
        float fv_floor = std::floor(vf);
        int32_t iu = static_cast<int32_t>(fu_floor);
        int32_t iv = static_cast<int32_t>(fv_floor);
        float fx = uf - fu_floor;
        float fy = vf - fv_floor;

        uint32_t x0 = pattern_clamp_or_wrap(iu, w, mode_);
        uint32_t y0 = pattern_clamp_or_wrap(iv, h, mode_);
        uint32_t x1 = pattern_clamp_or_wrap(iu + 1, w, mode_);
        uint32_t y1 = pattern_clamp_or_wrap(iv + 1, h, mode_);

        PremultipliedColor p00 = to_pm(x0, y0);
        PremultipliedColor p10 = to_pm(x1, y0);
        PremultipliedColor p01 = to_pm(x0, y1);
        PremultipliedColor p11 = to_pm(x1, y1);

        auto bilerp = [fx, fy](float c00, float c10, float c01, float c11) noexcept {
            float top = c00 + (c10 - c00) * fx;
            float bot = c01 + (c11 - c01) * fx;
            return top + (bot - top) * fy;
        };

        res = PremultipliedColor(
            NormalizedF32::create_unchecked(bilerp(p00.red(), p10.red(), p01.red(), p11.red())),
            NormalizedF32::create_unchecked(bilerp(p00.green(), p10.green(), p01.green(), p11.green())),
            NormalizedF32::create_unchecked(bilerp(p00.blue(), p10.blue(), p01.blue(), p11.blue())),
            NormalizedF32::create_unchecked(bilerp(p00.alpha(), p10.alpha(), p01.alpha(), p11.alpha()))
        );
    }

    if (opacity_ < 1.0f) {
        res = PremultipliedColor(
            NormalizedF32::create_unchecked(res.red() * opacity_),
            NormalizedF32::create_unchecked(res.green() * opacity_),
            NormalizedF32::create_unchecked(res.blue() * opacity_),
            NormalizedF32::create_unchecked(res.alpha() * opacity_)
        );
    }
    return res;
}

Color Pattern::sample(float x, float y) const noexcept {
    return sample_premul(x, y).demultiply();
}

static inline void apply_pattern_opacity(PremultipliedColorU8* dst, uint32_t count, float opacity) noexcept {
    if (opacity >= 0.999f) return;
    uint32_t op_u8 = static_cast<uint32_t>(opacity * 255.0f + 0.5f) + 1;
    for (uint32_t i = 0; i < count; ++i) {
        PremultipliedColorU8 c = dst[i];
        uint8_t r = static_cast<uint8_t>((c.red() * op_u8) >> 8);
        uint8_t g = static_cast<uint8_t>((c.green() * op_u8) >> 8);
        uint8_t b = static_cast<uint8_t>((c.blue() * op_u8) >> 8);
        uint8_t a = static_cast<uint8_t>((c.alpha() * op_u8) >> 8);
        dst[i] = PremultipliedColorU8::from_rgba_unchecked(r, g, b, a);
    }
}

void Pattern::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept {
    if (!inv_ts_ || pixmap_.width() == 0 || pixmap_.height() == 0 || count == 0) {
        std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        return;
    }

    const PixmapRef active_pm = rgba_storage_ ? rgba_storage_->as_ref() : pixmap_;
    const uint32_t w = active_pm.width();
    const uint32_t h = active_pm.height();
    const uint8_t* base = active_pm.data();
    const size_t stride = active_pm.stride_bytes();
    auto row_ptr = [base, stride](uint32_t py) noexcept -> const PremultipliedColorU8* {
        return reinterpret_cast<const PremultipliedColorU8*>(base + static_cast<size_t>(py) * stride);
    };

    // -------------------------------------------------------------------------
    // FAST PATH: Axis-Aligned 1:1 Identity / Translation (Test 33 & 34)
    // -------------------------------------------------------------------------
    if (std::abs(inv_ts_->sx - 1.0f) < 1e-5f &&
        std::abs(inv_ts_->sy - 1.0f) < 1e-5f &&
        std::abs(inv_ts_->kx) < 1e-5f &&
        std::abs(inv_ts_->ky) < 1e-5f) {

        float tx = inv_ts_->tx;
        float ty = inv_ts_->ty;
        float round_tx = std::round(tx);
        float round_ty = std::round(ty);
        bool is_pixel_aligned = (std::abs(tx - round_tx) < 1e-3f &&
                                 std::abs(ty - round_ty) < 1e-3f);

        if (quality_ == FilterQuality::Nearest || is_pixel_aligned) {
            int64_t start_x = static_cast<int64_t>(std::floor(x + tx));
            int64_t start_y = static_cast<int64_t>(std::floor(y + ty));

            if (mode_ == SpreadMode::Repeat) {
                uint32_t sy = pattern_clamp_or_wrap(static_cast<int32_t>(start_y), h, SpreadMode::Repeat);
                const PremultipliedColorU8* tile_row = row_ptr(sy);
                uint32_t sx = pattern_clamp_or_wrap(static_cast<int32_t>(start_x), w, SpreadMode::Repeat);

                uint32_t done = 0;
                while (done < count) {
                    uint32_t cur = std::min(count - done, w - sx);
                    std::memcpy(dst + done, tile_row + sx, cur * sizeof(PremultipliedColorU8));
                    done += cur;
                    sx = 0;
                }

                apply_pattern_opacity(dst, count, opacity_);
                return;
            } else if (mode_ == SpreadMode::Pad) {
                int64_t clamped_y = std::clamp(start_y, int64_t(0), static_cast<int64_t>(h - 1));
                const PremultipliedColorU8* pad_row = row_ptr(static_cast<uint32_t>(clamped_y));

                if (start_x >= 0 && start_x + count <= static_cast<int64_t>(w)) {
                    std::memcpy(dst, pad_row + start_x, count * sizeof(PremultipliedColorU8));
                } else {
                    int64_t x_end = start_x + static_cast<int64_t>(count);
                    int64_t left_len = std::max(int64_t(0), -start_x);
                    int64_t clamp_left = std::min(left_len, static_cast<int64_t>(count));
                    if (clamp_left > 0) {
                        simd::fill_solid_span(dst, pad_row[0], static_cast<size_t>(clamp_left));
                    }

                    int64_t mid_start = std::max(int64_t(0), start_x);
                    int64_t mid_end = std::min(static_cast<int64_t>(w), x_end);
                    if (mid_end > mid_start) {
                        size_t mid_count = static_cast<size_t>(mid_end - mid_start);
                        size_t dst_offset = static_cast<size_t>(mid_start - start_x);
                        std::memcpy(dst + dst_offset, pad_row + mid_start, mid_count * sizeof(PremultipliedColorU8));
                    }

                    int64_t right_start = std::max(static_cast<int64_t>(w), start_x);
                    if (x_end > right_start) {
                        size_t right_count = static_cast<size_t>(x_end - right_start);
                        size_t dst_offset = static_cast<size_t>(right_start - start_x);
                        simd::fill_solid_span(dst + dst_offset, pad_row[w - 1], right_count);
                    }
                }

                apply_pattern_opacity(dst, count, opacity_);
                return;
            }
        }
    }

    // -------------------------------------------------------------------------
    // GENERAL DDA SAMPLER: 16.16 Fixed-Point Incremental Stepper
    // -------------------------------------------------------------------------
    float sample_offset = (quality_ == FilterQuality::Nearest) ? 0.0f : -0.5f;

    float u0 = inv_ts_->sx * x + inv_ts_->kx * y + inv_ts_->tx + sample_offset;
    float v0 = inv_ts_->ky * x + inv_ts_->sy * y + inv_ts_->ty + sample_offset;
    float du = inv_ts_->sx;
    float dv = inv_ts_->ky;

    int64_t u_fixed = static_cast<int64_t>(std::round(u0 * 65536.0f));
    int64_t v_fixed = static_cast<int64_t>(std::round(v0 * 65536.0f));
    int64_t du_fixed = static_cast<int64_t>(std::round(du * 65536.0f));
    int64_t dv_fixed = static_cast<int64_t>(std::round(dv * 65536.0f));

    bool is_axis_aligned = (std::abs(dv) < 1e-5f);

    if (quality_ == FilterQuality::Nearest) {
        if (is_axis_aligned) {
            int32_t iv = static_cast<int32_t>(v_fixed >> 16);
            uint32_t py = pattern_clamp_or_wrap(iv, h, mode_);
            const PremultipliedColorU8* row = row_ptr(py);

            for (uint32_t i = 0; i < count; ++i) {
                int32_t iu = static_cast<int32_t>(u_fixed >> 16);
                uint32_t px;
                if (static_cast<uint32_t>(iu) < w) {
                    px = static_cast<uint32_t>(iu);
                } else {
                    px = pattern_clamp_or_wrap(iu, w, mode_);
                }
                dst[i] = row[px];
                u_fixed += du_fixed;
            }
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                int32_t iu = static_cast<int32_t>(u_fixed >> 16);
                int32_t iv = static_cast<int32_t>(v_fixed >> 16);
                uint32_t px = pattern_clamp_or_wrap(iu, w, mode_);
                uint32_t py = pattern_clamp_or_wrap(iv, h, mode_);
                dst[i] = row_ptr(py)[px];
                u_fixed += du_fixed;
                v_fixed += dv_fixed;
            }
        }
    } else {
        // Bilinear Filtering in Integer Premultiplied Space
        if (is_axis_aligned) {
            int32_t iv = static_cast<int32_t>(v_fixed >> 16);
            uint32_t fv = static_cast<uint32_t>((v_fixed >> 8) & 0xFF);
            uint32_t inv_fv = 256 - fv;

            uint32_t y0 = pattern_clamp_or_wrap(iv, h, mode_);
            uint32_t y1 = pattern_clamp_or_wrap(iv + 1, h, mode_);

            const PremultipliedColorU8* row0 = row_ptr(y0);
            const PremultipliedColorU8* row1 = row_ptr(y1);

            bool fast_done = false;
            if (du_fixed == 0x10000) {
                int32_t iu0 = static_cast<int32_t>(u_fixed >> 16);
                uint32_t fu = static_cast<uint32_t>((u_fixed >> 8) & 0xFF);
                uint32_t inv_fu = 256 - fu;
                if (iu0 >= 0 && iu0 + static_cast<int32_t>(count) + 1 <= static_cast<int32_t>(w)) {
                    if (fu == 0 && fv == 0) {
                        std::memcpy(dst, row0 + iu0, count * sizeof(PremultipliedColorU8));
                        apply_pattern_opacity(dst, count, opacity_);
                        return;
                    }
#if defined(__SSE2__)
                    __m128i zero = _mm_setzero_si128();
                    __m128i v_fu = _mm_set1_epi16(static_cast<short>(fu));
                    __m128i v_inv_fu = _mm_set1_epi16(static_cast<short>(inv_fu));
                    __m128i v_fv = _mm_set1_epi16(static_cast<short>(fv));
                    __m128i v_inv_fv = _mm_set1_epi16(static_cast<short>(inv_fv));

                    uint32_t i = 0;
                    for (; i + 1 < count; i += 2) {
                        int32_t x0 = iu0 + static_cast<int32_t>(i);
                        int32_t c00 = *reinterpret_cast<const int32_t*>(&row0[x0]);
                        int32_t c10 = *reinterpret_cast<const int32_t*>(&row0[x0 + 1]);
                        int32_t c20 = *reinterpret_cast<const int32_t*>(&row0[x0 + 2]);

                        int32_t c01 = *reinterpret_cast<const int32_t*>(&row1[x0]);
                        int32_t c11 = *reinterpret_cast<const int32_t*>(&row1[x0 + 1]);
                        int32_t c21 = *reinterpret_cast<const int32_t*>(&row1[x0 + 2]);

                        __m128i p00 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c00), zero);
                        __m128i p10 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c10), zero);
                        __m128i p20 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c20), zero);

                        __m128i p01 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c01), zero);
                        __m128i p11 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c11), zero);
                        __m128i p21 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c21), zero);

                        // Pixel 0
                        __m128i top0 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p00, v_inv_fu), _mm_mullo_epi16(p10, v_fu)), 8);
                        __m128i bot0 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p01, v_inv_fu), _mm_mullo_epi16(p11, v_fu)), 8);
                        __m128i res0 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top0, v_inv_fv), _mm_mullo_epi16(bot0, v_fv)), 8);

                        // Pixel 1
                        __m128i top1 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p10, v_inv_fu), _mm_mullo_epi16(p20, v_fu)), 8);
                        __m128i bot1 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p11, v_inv_fu), _mm_mullo_epi16(p21, v_fu)), 8);
                        __m128i res1 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top1, v_inv_fv), _mm_mullo_epi16(bot1, v_fv)), 8);

                        uint32_t px0 = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(res0, zero)));
                        uint32_t px1 = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(res1, zero)));
                        *reinterpret_cast<uint64_t*>(&dst[i]) = static_cast<uint64_t>(px0) | (static_cast<uint64_t>(px1) << 32);
                    }
                    if (i < count) {
                        int32_t x0 = iu0 + static_cast<int32_t>(i);
                        int32_t c00 = *reinterpret_cast<const int32_t*>(&row0[x0]);
                        int32_t c10 = *reinterpret_cast<const int32_t*>(&row0[x0 + 1]);
                        int32_t c01 = *reinterpret_cast<const int32_t*>(&row1[x0]);
                        int32_t c11 = *reinterpret_cast<const int32_t*>(&row1[x0 + 1]);

                        __m128i p00 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c00), zero);
                        __m128i p10 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c10), zero);
                        __m128i p01 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c01), zero);
                        __m128i p11 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c11), zero);

                        __m128i top = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p00, v_inv_fu), _mm_mullo_epi16(p10, v_fu)), 8);
                        __m128i bot = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p01, v_inv_fu), _mm_mullo_epi16(p11, v_fu)), 8);
                        __m128i res = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top, v_inv_fv), _mm_mullo_epi16(bot, v_fv)), 8);
                        *reinterpret_cast<int32_t*>(&dst[i]) = _mm_cvtsi128_si32(_mm_packus_epi16(res, zero));
                    }
                    fast_done = true;
#endif
                }
            }

            if (!fast_done) {
#if defined(__SSE2__)
            __m128i zero = _mm_setzero_si128();
            __m128i v_fv = _mm_set1_epi16(static_cast<short>(fv));
            __m128i v_inv_fv = _mm_set1_epi16(static_cast<short>(inv_fv));

            for (uint32_t i = 0; i < count; ++i) {
                int32_t iu = static_cast<int32_t>(u_fixed >> 16);
                uint32_t fu = static_cast<uint32_t>((u_fixed >> 8) & 0xFF);

                uint32_t x0, x1;
                if (iu >= 0 && iu + 1 < static_cast<int32_t>(w)) {
                    x0 = static_cast<uint32_t>(iu);
                    x1 = x0 + 1;
                } else {
                    x0 = pattern_clamp_or_wrap(iu, w, mode_);
                    x1 = pattern_clamp_or_wrap(iu + 1, w, mode_);
                }

                int32_t c00 = *reinterpret_cast<const int32_t*>(&row0[x0]);
                int32_t c10 = *reinterpret_cast<const int32_t*>(&row0[x1]);
                int32_t c01 = *reinterpret_cast<const int32_t*>(&row1[x0]);
                int32_t c11 = *reinterpret_cast<const int32_t*>(&row1[x1]);

                __m128i p00 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c00), zero);
                __m128i p10 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c10), zero);
                __m128i p01 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c01), zero);
                __m128i p11 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c11), zero);

                __m128i v_fu = _mm_set1_epi16(static_cast<short>(fu));
                __m128i v_inv_fu = _mm_set1_epi16(static_cast<short>(256 - fu));

                __m128i top = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p00, v_inv_fu), _mm_mullo_epi16(p10, v_fu)), 8);
                __m128i bot = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p01, v_inv_fu), _mm_mullo_epi16(p11, v_fu)), 8);

                __m128i res16 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top, v_inv_fv), _mm_mullo_epi16(bot, v_fv)), 8);
                __m128i res8 = _mm_packus_epi16(res16, zero);
                *reinterpret_cast<int32_t*>(&dst[i]) = _mm_cvtsi128_si32(res8);

                u_fixed += du_fixed;
            }
#else
            for (uint32_t i = 0; i < count; ++i) {
                int32_t iu = static_cast<int32_t>(u_fixed >> 16);
                uint32_t fu = static_cast<uint32_t>((u_fixed >> 8) & 0xFF);

                uint32_t x0, x1;
                if (iu >= 0 && iu + 1 < static_cast<int32_t>(w)) {
                    x0 = static_cast<uint32_t>(iu);
                    x1 = x0 + 1;
                } else {
                    x0 = pattern_clamp_or_wrap(iu, w, mode_);
                    x1 = pattern_clamp_or_wrap(iu + 1, w, mode_);
                }

                PremultipliedColorU8 p00 = row0[x0];
                PremultipliedColorU8 p10 = row0[x1];
                PremultipliedColorU8 p01 = row1[x0];
                PremultipliedColorU8 p11 = row1[x1];

                uint32_t inv_fu = 256 - fu;

                uint32_t top_r = (p00.red() * inv_fu + p10.red() * fu);
                uint32_t top_g = (p00.green() * inv_fu + p10.green() * fu);
                uint32_t top_b = (p00.blue() * inv_fu + p10.blue() * fu);
                uint32_t top_a = (p00.alpha() * inv_fu + p10.alpha() * fu);

                uint32_t bot_r = (p01.red() * inv_fu + p11.red() * fu);
                uint32_t bot_g = (p01.green() * inv_fu + p11.green() * fu);
                uint32_t bot_b = (p01.blue() * inv_fu + p11.blue() * fu);
                uint32_t bot_a = (p01.alpha() * inv_fu + p11.alpha() * fu);

                uint32_t r = (top_r * inv_fv + bot_r * fv + 32768) >> 16;
                uint32_t g = (top_g * inv_fv + bot_g * fv + 32768) >> 16;
                uint32_t b = (top_b * inv_fv + bot_b * fv + 32768) >> 16;
                uint32_t a = (top_a * inv_fv + bot_a * fv + 32768) >> 16;

                dst[i] = PremultipliedColorU8::from_rgba_unchecked(
                    static_cast<uint8_t>(r),
                    static_cast<uint8_t>(g),
                    static_cast<uint8_t>(b),
                    static_cast<uint8_t>(a)
                );

                u_fixed += du_fixed;
            }
#endif
            }
        } else {
            // General Affine Bilinear
#if defined(__SSE2__)
            __m128i zero = _mm_setzero_si128();

            for (uint32_t i = 0; i < count; ++i) {
                int32_t iu = static_cast<int32_t>(u_fixed >> 16);
                int32_t iv = static_cast<int32_t>(v_fixed >> 16);
                uint32_t fu = static_cast<uint32_t>((u_fixed >> 8) & 0xFF);
                uint32_t fv = static_cast<uint32_t>((v_fixed >> 8) & 0xFF);

                uint32_t x0 = pattern_clamp_or_wrap(iu, w, mode_);
                uint32_t y0 = pattern_clamp_or_wrap(iv, h, mode_);
                uint32_t x1 = pattern_clamp_or_wrap(iu + 1, w, mode_);
                uint32_t y1 = pattern_clamp_or_wrap(iv + 1, h, mode_);

                const PremultipliedColorU8* r0 = row_ptr(y0);
                const PremultipliedColorU8* r1 = row_ptr(y1);
                int32_t c00 = *reinterpret_cast<const int32_t*>(&r0[x0]);
                int32_t c10 = *reinterpret_cast<const int32_t*>(&r0[x1]);
                int32_t c01 = *reinterpret_cast<const int32_t*>(&r1[x0]);
                int32_t c11 = *reinterpret_cast<const int32_t*>(&r1[x1]);

                __m128i p00 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c00), zero);
                __m128i p10 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c10), zero);
                __m128i p01 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c01), zero);
                __m128i p11 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c11), zero);

                __m128i v_fu = _mm_set1_epi16(static_cast<short>(fu));
                __m128i v_inv_fu = _mm_set1_epi16(static_cast<short>(256 - fu));
                __m128i v_fv = _mm_set1_epi16(static_cast<short>(fv));
                __m128i v_inv_fv = _mm_set1_epi16(static_cast<short>(256 - fv));

                __m128i top = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p00, v_inv_fu), _mm_mullo_epi16(p10, v_fu)), 8);
                __m128i bot = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(p01, v_inv_fu), _mm_mullo_epi16(p11, v_fu)), 8);

                __m128i res16 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(top, v_inv_fv), _mm_mullo_epi16(bot, v_fv)), 8);
                __m128i res8 = _mm_packus_epi16(res16, zero);
                *reinterpret_cast<int32_t*>(&dst[i]) = _mm_cvtsi128_si32(res8);

                u_fixed += du_fixed;
                v_fixed += dv_fixed;
            }
#else
            for (uint32_t i = 0; i < count; ++i) {
                int32_t iu = static_cast<int32_t>(u_fixed >> 16);
                int32_t iv = static_cast<int32_t>(v_fixed >> 16);
                uint32_t fu = static_cast<uint32_t>((u_fixed >> 8) & 0xFF);
                uint32_t fv = static_cast<uint32_t>((v_fixed >> 8) & 0xFF);

                uint32_t x0 = pattern_clamp_or_wrap(iu, w, mode_);
                uint32_t y0 = pattern_clamp_or_wrap(iv, h, mode_);
                uint32_t x1 = pattern_clamp_or_wrap(iu + 1, w, mode_);
                uint32_t y1 = pattern_clamp_or_wrap(iv + 1, h, mode_);

                const PremultipliedColorU8* r0 = row_ptr(y0);
                const PremultipliedColorU8* r1 = row_ptr(y1);
                PremultipliedColorU8 p00 = r0[x0];
                PremultipliedColorU8 p10 = r0[x1];
                PremultipliedColorU8 p01 = r1[x0];
                PremultipliedColorU8 p11 = r1[x1];

                uint32_t inv_fu = 256 - fu;
                uint32_t inv_fv = 256 - fv;

                uint32_t top_r = (p00.red() * inv_fu + p10.red() * fu);
                uint32_t top_g = (p00.green() * inv_fu + p10.green() * fu);
                uint32_t top_b = (p00.blue() * inv_fu + p10.blue() * fu);
                uint32_t top_a = (p00.alpha() * inv_fu + p10.alpha() * fu);

                uint32_t bot_r = (p01.red() * inv_fu + p11.red() * fu);
                uint32_t bot_g = (p01.green() * inv_fu + p11.green() * fu);
                uint32_t bot_b = (p01.blue() * inv_fu + p11.blue() * fu);
                uint32_t bot_a = (p01.alpha() * inv_fu + p11.alpha() * fu);

                uint32_t r = (top_r * inv_fv + bot_r * fv + 32768) >> 16;
                uint32_t g = (top_g * inv_fv + bot_g * fv + 32768) >> 16;
                uint32_t b = (top_b * inv_fv + bot_b * fv + 32768) >> 16;
                uint32_t a = (top_a * inv_fv + bot_a * fv + 32768) >> 16;

                dst[i] = PremultipliedColorU8::from_rgba_unchecked(
                    static_cast<uint8_t>(r),
                    static_cast<uint8_t>(g),
                    static_cast<uint8_t>(b),
                    static_cast<uint8_t>(a)
                );

                u_fixed += du_fixed;
                v_fixed += dv_fixed;
            }
#endif
        }
    }

    apply_pattern_opacity(dst, count, opacity_);
}

// =========================================================================
// TwoPointConicalGradient
// =========================================================================

std::optional<TwoPointConicalGradient> TwoPointConicalGradient::create(
    Point start_center,
    float start_radius,
    Point end_center,
    float end_radius,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform
) {
    if (stops.empty()) return std::nullopt;
    if (start_radius < 0.0f || end_radius < 0.0f) return std::nullopt;
    if (!transform.invert()) return std::nullopt;

    TwoPointConicalGradient g;
    g.stops_ = std::move(stops);
    g.tile_mode_ = mode;
    g.transform_ = transform;
    g.start_center_ = start_center;
    g.start_radius_ = start_radius;
    g.end_center_ = end_center;
    g.end_radius_ = end_radius;

    g.dx_ = end_center.x - start_center.x;
    g.dy_ = end_center.y - start_center.y;
    g.dr_ = end_radius - start_radius;
    g.A_ = g.dx_ * g.dx_ + g.dy_ * g.dy_ - g.dr_ * g.dr_;
    g.is_concentric_ = (std::abs(g.dx_) < 1e-6f && std::abs(g.dy_) < 1e-6f);

    Transform unit_ts = Transform();
    g.points_to_unit_ = unit_ts;

    static_cast<Gradient&>(g) = Gradient(g.stops_, mode, transform, unit_ts);
    return g;
}

Color TwoPointConicalGradient::sample(float x, float y) const noexcept {
    if (!inv_combined_) return Color::TRANSPARENT;
    Point p = Point::from_xy(x, y);
    inv_combined_->map_point(p);

    float px = p.x - start_center_.x;
    float py = p.y - start_center_.y;

    auto t = solve_conical_t(px, py, start_radius_, end_radius_, dx_, dy_, dr_, A_, is_concentric_);
    if (!t) {
        return Color::TRANSPARENT;
    }
    return eval(*t);
}

PremultipliedColor TwoPointConicalGradient::sample_premul(float x, float y) const noexcept {
    return premul(sample(x, y));
}

void TwoPointConicalGradient::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept {
    if (!inv_combined_ || count == 0) {
        std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        dst[i] = sample_premul(x + static_cast<float>(i), y).to_color_u8();
    }
}

// =========================================================================
// ComposeShader
// =========================================================================

Color ComposeShader::sample(float x, float y) const noexcept {
    return sample_premul(x, y).demultiply();
}

PremultipliedColor ComposeShader::sample_premul(float x, float y) const noexcept {
    PremultipliedColor sc = src_.sample_premul(x, y);
    PremultipliedColor dc = dst_.sample_premul(x, y);
    return blend_colors(sc, dc, mode_);
}

void ComposeShader::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst_span) const noexcept {
    if (count == 0) return;
    dst_.shade_span(x, y, count, dst_span);

    std::vector<PremultipliedColorU8> src_buf(count);
    src_.shade_span(x, y, count, src_buf.data());

    for (uint32_t i = 0; i < count; ++i) {
        dst_span[i] = blend_pixels(src_buf[i], dst_span[i], mode_);
    }
}

void ComposeShader::apply_opacity(float opacity) noexcept {
    dst_.apply_opacity(opacity);
    src_.apply_opacity(opacity);
}

void ComposeShader::transform(const Transform& ts) noexcept {
    dst_.transform(ts);
    src_.transform(ts);
}

bool ComposeShader::is_opaque() const noexcept {
    if (mode_ == BlendMode::Source && src_.is_opaque()) return true;
    if (mode_ == BlendMode::Destination && dst_.is_opaque()) return true;
    if (mode_ == BlendMode::SourceOver && (src_.is_opaque() || dst_.is_opaque())) return true;
    return false;
}

// =========================================================================
// Shader
// =========================================================================

Shader::Shader() noexcept : type_(Type::SolidColor), solid_color_(Color::BLACK) {}
Shader::~Shader() = default;
Shader::Shader(const Shader&) = default;
Shader::Shader(Shader&&) noexcept = default;
Shader& Shader::operator=(const Shader&) = default;
Shader& Shader::operator=(Shader&&) noexcept = default;

Shader::Shader(Color color) noexcept : type_(Type::SolidColor), solid_color_(color) {}

Shader::Shader(LinearGradient grad)
    : type_(Type::LinearGradient),
      complex_(std::make_shared<LinearGradient>(std::move(grad))) {}

Shader::Shader(RadialGradient grad)
    : type_(Type::RadialGradient),
      complex_(std::make_shared<RadialGradient>(std::move(grad))) {}

Shader::Shader(SweepGradient grad)
    : type_(Type::SweepGradient),
      complex_(std::make_shared<SweepGradient>(std::move(grad))) {}

Shader::Shader(Pattern patt)
    : type_(Type::Pattern),
      complex_(std::make_shared<Pattern>(std::move(patt))) {}

Shader::Shader(TwoPointConicalGradient grad)
    : type_(Type::TwoPointConicalGradient),
      complex_(std::make_shared<TwoPointConicalGradient>(std::move(grad))) {}

Shader::Shader(ComposeShader comp)
    : type_(Type::ComposeShader),
      complex_(std::make_shared<ComposeShader>(std::move(comp))) {}

Shader::Shader(PerlinNoiseShader noise)
    : type_(Type::PerlinNoise),
      complex_(std::make_shared<PerlinNoiseShader>(std::move(noise))) {}

namespace {

inline int32_t perlin_wrap(int32_t v, int32_t period) noexcept {
    if (period <= 0) return v;
    int32_t m = v % period;
    return (m < 0) ? (m + period) : m;
}

inline void perlin_grad2(int32_t ix, int32_t iy, int32_t ch, uint32_t seed_u, float& gx, float& gy) noexcept {
    uint32_t h = static_cast<uint32_t>(ix) * 374761393u +
                 static_cast<uint32_t>(iy) * 668265263u +
                 static_cast<uint32_t>(ch) * 2147483647u +
                 seed_u * 1274126177u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    const uint32_t idx = (h ^ (h >> 16u)) & 7u;
    static constexpr float kGx[8] = {1.0f, -1.0f, 0.0f, 0.0f, 0.70710678f, -0.70710678f, 0.70710678f, -0.70710678f};
    static constexpr float kGy[8] = {0.0f, 0.0f, 1.0f, -1.0f, 0.70710678f, 0.70710678f, -0.70710678f, -0.70710678f};
    gx = kGx[idx];
    gy = kGy[idx];
}

inline float perlin_2d_channel(float px, float py, int32_t ch, uint32_t seed_u, int32_t period_x, int32_t period_y) noexcept {
    const float fx_floor = std::floor(px);
    const float fy_floor = std::floor(py);
    const int32_t ix = static_cast<int32_t>(fx_floor);
    const int32_t iy = static_cast<int32_t>(fy_floor);
    const float tx = px - fx_floor;
    const float ty = py - fy_floor;

    const int32_t x0 = perlin_wrap(ix, period_x);
    const int32_t x1 = perlin_wrap(ix + 1, period_x);
    const int32_t y0 = perlin_wrap(iy, period_y);
    const int32_t y1 = perlin_wrap(iy + 1, period_y);

    float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
    perlin_grad2(x0, y0, ch, seed_u, g00x, g00y);
    perlin_grad2(x1, y0, ch, seed_u, g10x, g10y);
    perlin_grad2(x0, y1, ch, seed_u, g01x, g01y);
    perlin_grad2(x1, y1, ch, seed_u, g11x, g11y);

    const float n00 = g00x * tx + g00y * ty;
    const float n10 = g10x * (tx - 1.0f) + g10y * ty;
    const float n01 = g01x * tx + g01y * (ty - 1.0f);
    const float n11 = g11x * (tx - 1.0f) + g11y * (ty - 1.0f);

    const float u = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const float v = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
    const float nx0 = n00 + u * (n10 - n00);
    const float nx1 = n01 + u * (n11 - n01);
    return nx0 + v * (nx1 - nx0);
}

} // namespace

std::optional<PerlinNoiseShader> PerlinNoiseShader::create_impl(
    PerlinNoiseType type,
    float base_freq_x,
    float base_freq_y,
    int num_octaves,
    float seed,
    std::optional<IntSize> tile_size,
    Transform transform
) {
    if (!std::isfinite(base_freq_x) || !std::isfinite(base_freq_y) ||
        base_freq_x < 0.0f || base_freq_y < 0.0f || num_octaves < 0) {
        return std::nullopt;
    }
    PerlinNoiseShader p;
    p.noise_type_ = type;
    p.base_freq_x_ = base_freq_x;
    p.base_freq_y_ = base_freq_y;
    p.num_octaves_ = std::clamp(num_octaves, 0, 16);
    p.seed_ = std::isfinite(seed) ? seed : 0.0f;
    p.tile_size_ = tile_size;
    p.transform_ = transform;

    if (tile_size.has_value() && tile_size->width() > 0 && tile_size->height() > 0) {
        const float tw = static_cast<float>(tile_size->width());
        const float th = static_cast<float>(tile_size->height());
        if (p.base_freq_x_ > 0.0f) {
            const float lo = std::floor(tw * p.base_freq_x_) / tw;
            const float hi = std::ceil(tw * p.base_freq_x_) / tw;
            p.base_freq_x_ = (lo > 0.0f && (p.base_freq_x_ / lo) < (hi / p.base_freq_x_)) ? lo : hi;
        }
        if (p.base_freq_y_ > 0.0f) {
            const float lo = std::floor(th * p.base_freq_y_) / th;
            const float hi = std::ceil(th * p.base_freq_y_) / th;
            p.base_freq_y_ = (lo > 0.0f && (p.base_freq_y_ / lo) < (hi / p.base_freq_y_)) ? lo : hi;
        }
    }

    p.update_inv();
    return p;
}

std::optional<PerlinNoiseShader> PerlinNoiseShader::create_fractal_noise(
    float base_freq_x, float base_freq_y, int num_octaves, float seed,
    std::optional<IntSize> tile_size, Transform transform
) {
    return create_impl(PerlinNoiseType::FractalNoise, base_freq_x, base_freq_y, num_octaves, seed, tile_size, transform);
}

std::optional<PerlinNoiseShader> PerlinNoiseShader::create_turbulence(
    float base_freq_x, float base_freq_y, int num_octaves, float seed,
    std::optional<IntSize> tile_size, Transform transform
) {
    return create_impl(PerlinNoiseType::Turbulence, base_freq_x, base_freq_y, num_octaves, seed, tile_size, transform);
}

void PerlinNoiseShader::update_inv() noexcept {
    inv_ts_ = transform_.invert();
}

void PerlinNoiseShader::set_transform(const Transform& ts) noexcept {
    transform_ = ts;
    update_inv();
}

void PerlinNoiseShader::post_concat(const Transform& ts) noexcept {
    transform_ = transform_.post_concat(ts);
    update_inv();
}

void PerlinNoiseShader::apply_opacity(float opacity) noexcept {
    opacity_ = std::clamp(opacity_ * opacity, 0.0f, 1.0f);
}

Color PerlinNoiseShader::sample(float x, float y) const noexcept {
    if (num_octaves_ <= 0) {
        return (noise_type_ == PerlinNoiseType::FractalNoise)
            ? Color::from_rgba_unchecked(0.5f, 0.5f, 0.5f, 0.5f * opacity_)
            : Color::TRANSPARENT;
    }

    Point lp = Point::from_xy(x, y);
    if (inv_ts_.has_value()) {
        inv_ts_->map_point(lp);
    }

    const uint32_t seed_u = static_cast<uint32_t>(static_cast<int32_t>(std::floor(seed_)));
    int32_t base_px = 0;
    int32_t base_py = 0;
    if (tile_size_.has_value() && tile_size_->width() > 0 && tile_size_->height() > 0) {
        base_px = std::max(1, static_cast<int32_t>(std::round(static_cast<float>(tile_size_->width()) * base_freq_x_)));
        base_py = std::max(1, static_cast<int32_t>(std::round(static_cast<float>(tile_size_->height()) * base_freq_y_)));
    }

    float ch_out[4]{0.0f, 0.0f, 0.0f, 0.0f};
    const bool is_turb = (noise_type_ == PerlinNoiseType::Turbulence);

    float fx = lp.x * base_freq_x_;
    float fy = lp.y * base_freq_y_;
    float amp = 1.0f;
    int32_t px = base_px;
    int32_t py = base_py;

    for (int o = 0; o < num_octaves_; ++o) {
        for (int c = 0; c < 4; ++c) {
            float n = perlin_2d_channel(fx, fy, c, seed_u, px, py);
            ch_out[c] += (is_turb ? std::abs(n) : n) * amp;
        }
        fx *= 2.0f;
        fy *= 2.0f;
        amp *= 0.5f;
        px *= 2;
        py *= 2;
    }

    for (int c = 0; c < 4; ++c) {
        float v = is_turb ? ch_out[c] : (ch_out[c] * 0.5f + 0.5f);
        ch_out[c] = std::clamp(v, 0.0f, 1.0f);
    }
    return Color::from_rgba_unchecked(ch_out[0], ch_out[1], ch_out[2], std::clamp(ch_out[3] * opacity_, 0.0f, 1.0f));
}

PremultipliedColor PerlinNoiseShader::sample_premul(float x, float y) const noexcept {
    return sample(x, y).premultiply();
}

void PerlinNoiseShader::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept {
    for (uint32_t i = 0; i < count; ++i) {
        dst[i] = sample_premul(x + static_cast<float>(i), y).to_color_u8();
    }
}

Shader Shader::create_fractal_noise(
    float base_freq_x, float base_freq_y, int num_octaves, float seed,
    std::optional<IntSize> tile_size, Transform transform
) {
    auto n = PerlinNoiseShader::create_fractal_noise(base_freq_x, base_freq_y, num_octaves, seed, tile_size, transform);
    if (!n) return Shader::from_color(Color::TRANSPARENT);
    return Shader(std::move(*n));
}

Shader Shader::create_turbulence(
    float base_freq_x, float base_freq_y, int num_octaves, float seed,
    std::optional<IntSize> tile_size, Transform transform
) {
    auto n = PerlinNoiseShader::create_turbulence(base_freq_x, base_freq_y, num_octaves, seed, tile_size, transform);
    if (!n) return Shader::from_color(Color::TRANSPARENT);
    return Shader(std::move(*n));
}

Shader Shader::create_compose(Shader dst, Shader src, BlendMode mode) {
    return Shader(ComposeShader(std::move(dst), std::move(src), mode));
}

Shader Shader::create_two_point_conical(
    Point start_center,
    float start_radius,
    Point end_center,
    float end_radius,
    std::vector<GradientStop> stops,
    SpreadMode mode,
    Transform transform
) {
    auto grad = TwoPointConicalGradient::create(start_center, start_radius, end_center, end_radius, std::move(stops), mode, transform);
    if (!grad) return Shader::from_color(Color::TRANSPARENT);
    return Shader(std::move(*grad));
}

const LinearGradient& Shader::linear_gradient() const noexcept {
    return *std::get<std::shared_ptr<LinearGradient>>(complex_);
}

const RadialGradient& Shader::radial_gradient() const noexcept {
    return *std::get<std::shared_ptr<RadialGradient>>(complex_);
}

const SweepGradient& Shader::sweep_gradient() const noexcept {
    return *std::get<std::shared_ptr<SweepGradient>>(complex_);
}

const TwoPointConicalGradient& Shader::conical_gradient() const noexcept {
    return *std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
}

const Pattern& Shader::pattern() const noexcept {
    return *std::get<std::shared_ptr<Pattern>>(complex_);
}

const ComposeShader& Shader::compose_shader() const noexcept {
    return *std::get<std::shared_ptr<ComposeShader>>(complex_);
}

const PerlinNoiseShader& Shader::perlin_noise() const noexcept {
    return *std::get<std::shared_ptr<PerlinNoiseShader>>(complex_);
}

bool Shader::is_opaque() const noexcept {
    switch (type_) {
        case Type::SolidColor:
            return solid_color_.is_opaque();
        case Type::LinearGradient: {
            const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
            return ptr ? ptr->colors_are_opaque() : false;
        }
        case Type::RadialGradient: {
            const auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
            return (ptr && !ptr->is_2point()) ? ptr->colors_are_opaque() : false;
        }
        case Type::SweepGradient: {
            const auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
            return ptr ? ptr->colors_are_opaque() : false;
        }
        case Type::Pattern: {
            const auto& ptr = std::get<std::shared_ptr<Pattern>>(complex_);
            return ptr ? ptr->is_opaque() : false;
        }
        case Type::TwoPointConicalGradient: {
            const auto& ptr = std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
            return ptr ? ptr->colors_are_opaque() : false;
        }
        case Type::ComposeShader: {
            const auto& ptr = std::get<std::shared_ptr<ComposeShader>>(complex_);
            return ptr ? ptr->is_opaque() : false;
        }
        case Type::PerlinNoise:
            return false;
    }
    return false;
}

bool Shader::has_uniform_alpha() const noexcept {
    if (type_ == Type::SolidColor) return true;
    if (type_ == Type::LinearGradient) {
        const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
        return ptr ? ptr->has_uniform_alpha() : true;
    }
    if (type_ == Type::RadialGradient) {
        const auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
        return (ptr && !ptr->is_2point()) ? ptr->has_uniform_alpha() : false;
    }
    if (type_ == Type::SweepGradient) {
        const auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
        return ptr ? ptr->has_uniform_alpha() : true;
    }
    return false;
}

uint8_t Shader::uniform_alpha() const noexcept {
    if (type_ == Type::SolidColor) {
        return static_cast<uint8_t>(solid_color_.alpha() * 255.0f + 0.5f);
    }
    if (type_ == Type::LinearGradient) {
        const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
        return ptr ? ptr->uniform_alpha() : 255;
    }
    if (type_ == Type::RadialGradient) {
        const auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
        return ptr ? ptr->uniform_alpha() : 255;
    }
    if (type_ == Type::SweepGradient) {
        const auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
        return ptr ? ptr->uniform_alpha() : 255;
    }
    return 255;
}

bool Shader::is_horizontal_gradient() const noexcept {
    if (type_ == Type::LinearGradient) {
        const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
        return ptr ? ptr->is_horizontal() : false;
    }
    return false;
}

void Shader::apply_opacity(float opacity) noexcept {
    switch (type_) {
        case Type::SolidColor:
            solid_color_.apply_opacity(opacity);
            break;
        case Type::LinearGradient: {
            auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<LinearGradient>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
        case Type::RadialGradient: {
            auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<RadialGradient>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
        case Type::SweepGradient: {
            auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<SweepGradient>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
        case Type::Pattern: {
            auto& ptr = std::get<std::shared_ptr<Pattern>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<Pattern>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
        case Type::TwoPointConicalGradient: {
            auto& ptr = std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<TwoPointConicalGradient>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
        case Type::ComposeShader: {
            auto& ptr = std::get<std::shared_ptr<ComposeShader>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<ComposeShader>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
        case Type::PerlinNoise: {
            auto& ptr = std::get<std::shared_ptr<PerlinNoiseShader>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<PerlinNoiseShader>(*ptr);
            }
            ptr->apply_opacity(opacity);
            break;
        }
    }
}

void Shader::transform(const Transform& ts) noexcept {
    switch (type_) {
        case Type::SolidColor:
            break;
        case Type::LinearGradient: {
            auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<LinearGradient>(*ptr);
            }
            ptr->post_concat(ts);
            break;
        }
        case Type::RadialGradient: {
            auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<RadialGradient>(*ptr);
            }
            ptr->post_concat(ts);
            break;
        }
        case Type::SweepGradient: {
            auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<SweepGradient>(*ptr);
            }
            ptr->post_concat(ts);
            break;
        }
        case Type::Pattern: {
            auto& ptr = std::get<std::shared_ptr<Pattern>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<Pattern>(*ptr);
            }
            ptr->post_concat(ts);
            break;
        }
        case Type::TwoPointConicalGradient: {
            auto& ptr = std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<TwoPointConicalGradient>(*ptr);
            }
            ptr->post_concat(ts);
            break;
        }
        case Type::ComposeShader: {
            auto& ptr = std::get<std::shared_ptr<ComposeShader>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<ComposeShader>(*ptr);
            }
            ptr->transform(ts);
            break;
        }
        case Type::PerlinNoise: {
            auto& ptr = std::get<std::shared_ptr<PerlinNoiseShader>>(complex_);
            if (ptr.use_count() > 1) {
                ptr = std::make_shared<PerlinNoiseShader>(*ptr);
            }
            ptr->post_concat(ts);
            break;
        }
    }
}

Color Shader::sample(float x, float y) const noexcept {
    switch (type_) {
        case Type::SolidColor:
            return solid_color_;
        case Type::LinearGradient: {
            const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
        case Type::RadialGradient: {
            const auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
        case Type::SweepGradient: {
            const auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
        case Type::Pattern: {
            const auto& ptr = std::get<std::shared_ptr<Pattern>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
        case Type::TwoPointConicalGradient: {
            const auto& ptr = std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
        case Type::ComposeShader: {
            const auto& ptr = std::get<std::shared_ptr<ComposeShader>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
        case Type::PerlinNoise: {
            const auto& ptr = std::get<std::shared_ptr<PerlinNoiseShader>>(complex_);
            return ptr ? ptr->sample(x, y) : Color::TRANSPARENT;
        }
    }
    return solid_color_;
}

PremultipliedColor Shader::sample_premul(float x, float y) const noexcept {
    switch (type_) {
        case Type::SolidColor:
            return solid_color_.premultiply();
        case Type::LinearGradient: {
            const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
        case Type::RadialGradient: {
            const auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
        case Type::SweepGradient: {
            const auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
        case Type::Pattern: {
            const auto& ptr = std::get<std::shared_ptr<Pattern>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
        case Type::TwoPointConicalGradient: {
            const auto& ptr = std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
        case Type::ComposeShader: {
            const auto& ptr = std::get<std::shared_ptr<ComposeShader>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
        case Type::PerlinNoise: {
            const auto& ptr = std::get<std::shared_ptr<PerlinNoiseShader>>(complex_);
            return ptr ? ptr->sample_premul(x, y) : PremultipliedColor{};
        }
    }
    return solid_color_.premultiply();
}

void Shader::shade_span(float x, float y, uint32_t count, PremultipliedColorU8* dst) const noexcept {
    if (type_ == Type::LinearGradient) {
        const auto& ptr = std::get<std::shared_ptr<LinearGradient>>(complex_);
        if (ptr) {
            ptr->shade_span(x, y, count, dst);
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::RadialGradient) {
        const auto& ptr = std::get<std::shared_ptr<RadialGradient>>(complex_);
        if (ptr) {
            ptr->shade_span(x, y, count, dst);
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::SweepGradient) {
        const auto& ptr = std::get<std::shared_ptr<SweepGradient>>(complex_);
        if (ptr) {
            for (uint32_t i = 0; i < count; ++i) {
                dst[i] = ptr->sample_premul(x + static_cast<float>(i), y).to_color_u8();
            }
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::TwoPointConicalGradient) {
        const auto& ptr = std::get<std::shared_ptr<TwoPointConicalGradient>>(complex_);
        if (ptr) {
            ptr->shade_span(x, y, count, dst);
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::ComposeShader) {
        const auto& ptr = std::get<std::shared_ptr<ComposeShader>>(complex_);
        if (ptr) {
            ptr->shade_span(x, y, count, dst);
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::Pattern) {
        const auto& ptr = std::get<std::shared_ptr<Pattern>>(complex_);
        if (ptr) {
            ptr->shade_span(x, y, count, dst);
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::PerlinNoise) {
        const auto& ptr = std::get<std::shared_ptr<PerlinNoiseShader>>(complex_);
        if (ptr) {
            ptr->shade_span(x, y, count, dst);
        } else {
            std::fill_n(dst, count, PremultipliedColorU8::TRANSPARENT);
        }
    } else if (type_ == Type::SolidColor) {
        PremultipliedColorU8 sc = solid_color_.premultiply().to_color_u8();
        std::fill_n(dst, count, sc);
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            dst[i] = sample_premul(x + static_cast<float>(i), y).to_color_u8();
        }
    }
}

} // namespace nisaba

