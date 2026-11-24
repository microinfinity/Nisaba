#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "nisaba/math/rect.hpp"
#include "nisaba/math/screen_int_rect.hpp"
#include "nisaba/path/path.hpp"

namespace nisaba {

/// Boolean set operations for integer pixel regions.
enum class RegionOp : uint8_t {
    Difference = 0,        ///< A - B
    Intersect = 1,         ///< A & B
    Union = 2,             ///< A | B
    Xor = 3,               ///< (A | B) - (A & B)
    ReverseDifference = 4, ///< B - A
    Replace = 5,           ///< B
};

/// Canonical Y-X banded integer pixel region composed of disjoint, scanline-sorted rectangles.
/// Adjacent horizontal bands with identical X-interval lists are coalesced automatically,
/// keeping representation minimal and exact for compositor damage masks and clipping.
class Region {
public:
    Region() noexcept = default;
    explicit Region(const IntRect& rect);
    explicit Region(const ScreenIntRect& rect);

    void set_empty() noexcept {
        rects_.clear();
        bounds_.reset();
    }

    bool set_rect(const IntRect& rect);
    bool set_rect(const ScreenIntRect& rect);
    bool set_rects(std::span<const IntRect> rects);
    bool set_rects(std::span<const ScreenIntRect> rects);

    [[nodiscard]] bool is_empty() const noexcept { return rects_.empty(); }
    [[nodiscard]] bool is_rect() const noexcept { return rects_.size() == 1; }
    [[nodiscard]] bool is_complex() const noexcept { return rects_.size() > 1; }
    [[nodiscard]] size_t rect_count() const noexcept { return rects_.size(); }
    [[nodiscard]] std::span<const IntRect> rects() const noexcept { return rects_; }
    [[nodiscard]] const std::optional<IntRect>& bounds() const noexcept { return bounds_; }

    [[nodiscard]] bool contains(int32_t x, int32_t y) const noexcept;
    [[nodiscard]] bool contains(const IntRect& rect) const noexcept;
    [[nodiscard]] bool intersects(const IntRect& rect) const noexcept;
    [[nodiscard]] bool intersects(const Region& other) const noexcept;
    [[nodiscard]] bool quick_reject(const IntRect& rect) const noexcept;
    [[nodiscard]] bool quick_reject(const Region& other) const noexcept;

    bool op(const IntRect& rect, RegionOp op_kind);
    bool op(const ScreenIntRect& rect, RegionOp op_kind);
    bool op(const Region& other, RegionOp op_kind);
    bool op(const Region& a, const Region& b, RegionOp op_kind);

    bool union_with(const IntRect& rect) { return op(rect, RegionOp::Union); }
    bool union_with(const ScreenIntRect& rect) { return op(rect, RegionOp::Union); }
    bool union_with(const Region& other) { return op(other, RegionOp::Union); }

    bool intersect_with(const IntRect& rect) { return op(rect, RegionOp::Intersect); }
    bool intersect_with(const ScreenIntRect& rect) { return op(rect, RegionOp::Intersect); }
    bool intersect_with(const Region& other) { return op(other, RegionOp::Intersect); }

    bool difference_with(const IntRect& rect) { return op(rect, RegionOp::Difference); }
    bool difference_with(const ScreenIntRect& rect) { return op(rect, RegionOp::Difference); }
    bool difference_with(const Region& other) { return op(other, RegionOp::Difference); }

    bool xor_with(const IntRect& rect) { return op(rect, RegionOp::Xor); }
    bool xor_with(const ScreenIntRect& rect) { return op(rect, RegionOp::Xor); }
    bool xor_with(const Region& other) { return op(other, RegionOp::Xor); }

    void translate(int32_t dx, int32_t dy);
    [[nodiscard]] Region translated(int32_t dx, int32_t dy) const;

    /// Converts all disjoint rectangles in this region into a single vector Path.
    [[nodiscard]] Path to_path() const;

    [[nodiscard]] bool operator==(const Region& other) const noexcept {
        return bounds_ == other.bounds_ && rects_ == other.rects_;
    }
    [[nodiscard]] bool operator!=(const Region& other) const noexcept {
        return !(*this == other);
    }

private:
    void recompute_bounds() noexcept;

    std::vector<IntRect> rects_{};
    std::optional<IntRect> bounds_{};
};

} // namespace nisaba

