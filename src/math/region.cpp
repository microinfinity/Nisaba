#include "nisaba/math/region.hpp"
#include "nisaba/path/path_builder.hpp"
#include <algorithm>
#include <limits>

namespace nisaba {

namespace {

struct XSpan {
    int32_t x0{0};
    int32_t x1{0};
    constexpr bool operator==(const XSpan& o) const noexcept = default;
};

struct YBand {
    int32_t y0{0};
    int32_t y1{0};
    std::vector<XSpan> spans{};
};

void extract_bands(const std::vector<IntRect>& rects, std::vector<YBand>& bands) {
    bands.clear();
    for (const auto& r : rects) {
        if (!bands.empty() && bands.back().y0 == r.top() && bands.back().y1 == r.bottom()) {
            bands.back().spans.push_back(XSpan{r.left(), r.right()});
        } else {
            YBand b;
            b.y0 = r.top();
            b.y1 = r.bottom();
            b.spans.push_back(XSpan{r.left(), r.right()});
            bands.push_back(std::move(b));
        }
    }
}

void combine_x_spans(
    const std::vector<XSpan>& a,
    const std::vector<XSpan>& b,
    RegionOp op,
    std::vector<XSpan>& out
) {
    out.clear();
    if (a.empty() && b.empty()) return;

    // Collect sorted unique X coordinates from both interval sets
    std::vector<int32_t> xs;
    xs.reserve((a.size() + b.size()) * 2);
    for (const auto& s : a) { xs.push_back(s.x0); xs.push_back(s.x1); }
    for (const auto& s : b) { xs.push_back(s.x0); xs.push_back(s.x1); }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    if (xs.size() < 2) return;

    size_t ia = 0;
    size_t ib = 0;

    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        const int32_t x0 = xs[i];
        const int32_t x1 = xs[i + 1];
        if (x1 <= x0) continue;

        while (ia < a.size() && a[ia].x1 <= x0) ++ia;
        while (ib < b.size() && b[ib].x1 <= x0) ++ib;

        const bool in_a = (ia < a.size() && a[ia].x0 <= x0 && a[ia].x1 >= x1);
        const bool in_b = (ib < b.size() && b[ib].x0 <= x0 && b[ib].x1 >= x1);

        bool keep = false;
        switch (op) {
            case RegionOp::Difference:        keep = in_a && !in_b; break;
            case RegionOp::Intersect:         keep = in_a && in_b; break;
            case RegionOp::Union:             keep = in_a || in_b; break;
            case RegionOp::Xor:               keep = in_a != in_b; break;
            case RegionOp::ReverseDifference: keep = !in_a && in_b; break;
            case RegionOp::Replace:           keep = in_b; break;
        }

        if (keep) {
            if (!out.empty() && out.back().x1 == x0) {
                out.back().x1 = x1;
            } else {
                out.push_back(XSpan{x0, x1});
            }
        }
    }
}

void combine_bands(
    const std::vector<IntRect>& ra,
    const std::vector<IntRect>& rb,
    RegionOp op,
    std::vector<IntRect>& out_rects
) {
    std::vector<YBand> ba, bb;
    extract_bands(ra, ba);
    extract_bands(rb, bb);

    std::vector<int32_t> ys;
    ys.reserve((ba.size() + bb.size()) * 2);
    for (const auto& b : ba) { ys.push_back(b.y0); ys.push_back(b.y1); }
    for (const auto& b : bb) { ys.push_back(b.y0); ys.push_back(b.y1); }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    std::vector<YBand> result_bands;
    const std::vector<XSpan> empty_spans;
    std::vector<XSpan> combined_spans;

    size_t ia = 0;
    size_t ib = 0;

    for (size_t i = 0; i + 1 < ys.size(); ++i) {
        const int32_t y0 = ys[i];
        const int32_t y1 = ys[i + 1];
        if (y1 <= y0) continue;

        while (ia < ba.size() && ba[ia].y1 <= y0) ++ia;
        while (ib < bb.size() && bb[ib].y1 <= y0) ++ib;

        const bool has_a = (ia < ba.size() && ba[ia].y0 <= y0 && ba[ia].y1 >= y1);
        const bool has_b = (ib < bb.size() && bb[ib].y0 <= y0 && bb[ib].y1 >= y1);

        combine_x_spans(
            has_a ? ba[ia].spans : empty_spans,
            has_b ? bb[ib].spans : empty_spans,
            op,
            combined_spans
        );

        if (combined_spans.empty()) continue;

        if (!result_bands.empty() &&
            result_bands.back().y1 == y0 &&
            result_bands.back().spans == combined_spans) {
            result_bands.back().y1 = y1;
        } else {
            result_bands.push_back(YBand{y0, y1, combined_spans});
        }
    }

    out_rects.clear();
    for (const auto& band : result_bands) {
        for (const auto& sp : band.spans) {
            if (auto r = IntRect::from_ltrb(sp.x0, band.y0, sp.x1, band.y1)) {
                out_rects.push_back(*r);
            }
        }
    }
}

} // namespace

Region::Region(const IntRect& rect) {
    set_rect(rect);
}

Region::Region(const ScreenIntRect& rect) {
    set_rect(rect);
}

bool Region::set_rect(const IntRect& rect) {
    rects_.clear();
    rects_.push_back(rect);
    bounds_ = rect;
    return true;
}

bool Region::set_rect(const ScreenIntRect& rect) {
    return set_rect(rect.to_int_rect());
}

bool Region::set_rects(std::span<const IntRect> rects) {
    set_empty();
    if (rects.empty()) return false;
    if (rects.size() == 1) {
        return set_rect(rects[0]);
    }
    for (const auto& r : rects) {
        op(r, RegionOp::Union);
    }
    return !is_empty();
}

bool Region::set_rects(std::span<const ScreenIntRect> rects) {
    set_empty();
    if (rects.empty()) return false;
    if (rects.size() == 1) {
        return set_rect(rects[0]);
    }
    for (const auto& r : rects) {
        op(r.to_int_rect(), RegionOp::Union);
    }
    return !is_empty();
}

void Region::recompute_bounds() noexcept {
    if (rects_.empty()) {
        bounds_.reset();
        return;
    }
    int32_t l = rects_[0].left();
    int32_t t = rects_[0].top();
    int32_t r = rects_[0].right();
    int32_t b = rects_[0].bottom();
    for (size_t i = 1; i < rects_.size(); ++i) {
        l = std::min(l, rects_[i].left());
        t = std::min(t, rects_[i].top());
        r = std::max(r, rects_[i].right());
        b = std::max(b, rects_[i].bottom());
    }
    bounds_ = IntRect::from_ltrb(l, t, r, b);
}

bool Region::contains(int32_t x, int32_t y) const noexcept {
    if (!bounds_.has_value()) return false;
    if (x < bounds_->left() || x >= bounds_->right() ||
        y < bounds_->top() || y >= bounds_->bottom()) {
        return false;
    }
    for (const auto& r : rects_) {
        if (y < r.top()) break;
        if (y >= r.bottom()) continue;
        if (x >= r.left() && x < r.right()) return true;
    }
    return false;
}

bool Region::contains(const IntRect& rect) const noexcept {
    if (!bounds_.has_value() || !bounds_->contains(rect)) return false;
    if (is_rect()) return true;
    // rect is contained iff (rect - *this) is empty
    Region diff(rect);
    diff.op(*this, RegionOp::Difference);
    return diff.is_empty();
}

bool Region::intersects(const IntRect& rect) const noexcept {
    if (!bounds_.has_value() || !bounds_->intersect(rect).has_value()) return false;
    if (is_rect()) return true;
    for (const auto& r : rects_) {
        if (r.top() >= rect.bottom()) break;
        if (r.bottom() <= rect.top()) continue;
        if (r.intersect(rect).has_value()) return true;
    }
    return false;
}

bool Region::intersects(const Region& other) const noexcept {
    if (quick_reject(other)) return false;
    if (is_rect() && other.is_rect()) return true;
    for (const auto& r : other.rects_) {
        if (intersects(r)) return true;
    }
    return false;
}

bool Region::quick_reject(const IntRect& rect) const noexcept {
    if (!bounds_.has_value()) return true;
    return !bounds_->intersect(rect).has_value();
}

bool Region::quick_reject(const Region& other) const noexcept {
    if (!bounds_.has_value() || !other.bounds_.has_value()) return true;
    return !bounds_->intersect(*other.bounds_).has_value();
}

bool Region::op(const IntRect& rect, RegionOp op_kind) {
    Region tmp(rect);
    return op(*this, tmp, op_kind);
}

bool Region::op(const ScreenIntRect& rect, RegionOp op_kind) {
    return op(rect.to_int_rect(), op_kind);
}

bool Region::op(const Region& other, RegionOp op_kind) {
    return op(*this, other, op_kind);
}

bool Region::op(const Region& a, const Region& b, RegionOp op_kind) {
    if (op_kind == RegionOp::Replace) {
        if (this != &b) {
            rects_ = b.rects_;
            bounds_ = b.bounds_;
        }
        return !is_empty();
    }

    if (op_kind == RegionOp::Intersect) {
        if (a.quick_reject(b)) {
            set_empty();
            return false;
        }
        if (a.is_rect() && b.is_rect()) {
            auto inter = a.bounds_->intersect(*b.bounds_);
            if (inter) {
                return set_rect(*inter);
            }
            set_empty();
            return false;
        }
    } else if (op_kind == RegionOp::Difference) {
        if (a.is_empty()) {
            set_empty();
            return false;
        }
        if (a.quick_reject(b)) {
            if (this != &a) {
                rects_ = a.rects_;
                bounds_ = a.bounds_;
            }
            return !is_empty();
        }
    } else if (op_kind == RegionOp::ReverseDifference) {
        return op(b, a, RegionOp::Difference);
    } else if (op_kind == RegionOp::Union) {
        if (a.is_empty()) {
            if (this != &b) { rects_ = b.rects_; bounds_ = b.bounds_; }
            return !is_empty();
        }
        if (b.is_empty()) {
            if (this != &a) { rects_ = a.rects_; bounds_ = a.bounds_; }
            return !is_empty();
        }
    } else if (op_kind == RegionOp::Xor) {
        if (a.is_empty()) {
            if (this != &b) { rects_ = b.rects_; bounds_ = b.bounds_; }
            return !is_empty();
        }
        if (b.is_empty()) {
            if (this != &a) { rects_ = a.rects_; bounds_ = a.bounds_; }
            return !is_empty();
        }
    }

    std::vector<IntRect> out;
    combine_bands(a.rects_, b.rects_, op_kind, out);
    rects_ = std::move(out);
    recompute_bounds();
    return !is_empty();
}

void Region::translate(int32_t dx, int32_t dy) {
    if ((dx == 0 && dy == 0) || rects_.empty()) return;
    std::vector<IntRect> moved;
    moved.reserve(rects_.size());
    for (const auto& r : rects_) {
        if (auto tr = r.translate(dx, dy)) {
            moved.push_back(*tr);
        }
    }
    rects_ = std::move(moved);
    recompute_bounds();
}

Region Region::translated(int32_t dx, int32_t dy) const {
    Region copy = *this;
    copy.translate(dx, dy);
    return copy;
}

Path Region::to_path() const {
    if (rects_.empty()) return Path();
    PathBuilder pb;
    for (const auto& r : rects_) {
        pb.push_rect(r.to_rect());
    }
    auto p = pb.finish();
    return p ? std::move(*p) : Path();
}

} // namespace nisaba

