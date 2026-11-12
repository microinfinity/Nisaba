#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace nisaba::compositor {

template <typename T>
struct Point {
    T x{0};
    T y{0};

    constexpr bool operator==(const Point& other) const = default;
    constexpr Point operator+(const Point& other) const noexcept { return {x + other.x, y + other.y}; }
    constexpr Point operator-(const Point& other) const noexcept { return {x - other.x, y - other.y}; }
};

using PointI = Point<int32_t>;
using PointF = Point<double>;

template <typename T>
struct Size {
    T width{0};
    T height{0};

    constexpr bool operator==(const Size& other) const = default;
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
};

using SizeI = Size<int32_t>;
using SizeF = Size<double>;

template <typename T>
struct Rect {
    T x{0};
    T y{0};
    T width{0};
    T height{0};

    constexpr bool operator==(const Rect& other) const = default;

    [[nodiscard]] constexpr bool isEmpty() const noexcept {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] constexpr bool contains(Point<T> pt) const noexcept {
        return pt.x >= x && pt.x < (x + width) && pt.y >= y && pt.y < (y + height);
    }

    [[nodiscard]] constexpr bool intersects(const Rect& other) const noexcept {
        return !(other.x >= x + width ||
                 other.x + other.width <= x ||
                 other.y >= y + height ||
                 other.y + other.height <= y);
    }

    [[nodiscard]] constexpr Rect intersection(const Rect& other) const noexcept {
        if (!intersects(other)) {
            return Rect{0, 0, 0, 0};
        }
        T nx = std::max(x, other.x);
        T ny = std::max(y, other.y);
        T nw = std::min(x + width, other.x + other.width) - nx;
        T nh = std::min(y + height, other.y + other.height) - ny;
        return Rect{nx, ny, nw, nh};
    }
};

using RectI = Rect<int32_t>;
using RectF = Rect<double>;

namespace detail {

inline void subtractRectAppend(const RectI& a, const RectI& b, std::vector<RectI>& out) {
    if (!a.intersects(b)) {
        out.push_back(a);
        return;
    }
    const RectI clip = a.intersection(b);

    // Top slice
    if (clip.y > a.y) {
        out.push_back(RectI{a.x, a.y, a.width, clip.y - a.y});
    }
    // Bottom slice
    const int aBottom = a.y + a.height;
    const int clipBottom = clip.y + clip.height;
    if (clipBottom < aBottom) {
        out.push_back(RectI{a.x, clipBottom, a.width, aBottom - clipBottom});
    }
    // Left slice
    if (clip.x > a.x) {
        out.push_back(RectI{a.x, clip.y, clip.x - a.x, clip.height});
    }
    // Right slice
    const int aRight = a.x + a.width;
    const int clipRight = clip.x + clip.width;
    if (clipRight < aRight) {
        out.push_back(RectI{clipRight, clip.y, aRight - clipRight, clip.height});
    }
}

inline std::vector<RectI> subtractRect(const RectI& a, const RectI& b) {
    std::vector<RectI> result;
    result.reserve(4);
    subtractRectAppend(a, b, result);
    return result;
}

} // namespace detail

/**
 * @brief Sovereign, 100% Pure C++ pixel-accurate 2D Region using disjoint rectangle decomposition.
 * Zero external library dependencies (completely free from pixman or cairo).
 */
class Region {
public:
    Region() noexcept = default;

    explicit Region(const RectI& rect) noexcept {
        add(rect);
    }

    ~Region() noexcept = default;
    Region(const Region& other) = default;
    Region& operator=(const Region& other) = default;
    Region(Region&& other) noexcept = default;
    Region& operator=(Region&& other) noexcept = default;

    void clear() noexcept {
        m_rects.clear();
        m_extents = RectI{0, 0, 0, 0};
    }

    [[nodiscard]] bool isEmpty() const noexcept {
        return m_rects.empty();
    }

    void add(const RectI& rect) noexcept {
        if (rect.isEmpty()) return;
        if (m_rects.empty()) {
            m_rects.push_back(rect);
            m_extents = rect;
            return;
        }
        if (!m_extents.intersects(rect)) {
            m_rects.push_back(rect);
            expandExtents(rect);
            return;
        }

        std::vector<RectI> pieces;
        pieces.push_back(rect);
        std::vector<RectI> nextPieces;
        for (const auto& existing : m_rects) {
            if (!m_extents.intersects(existing)) continue;
            nextPieces.clear();
            for (const auto& piece : pieces) {
                detail::subtractRectAppend(piece, existing, nextPieces);
            }
            pieces.swap(nextPieces);
            if (pieces.empty()) break;
        }

        if (!pieces.empty()) {
            m_rects.insert(m_rects.end(), pieces.begin(), pieces.end());
            expandExtents(rect);
        }
    }

    void unite(const Region& other) noexcept {
        if (other.isEmpty()) return;
        if (m_rects.empty()) {
            *this = other;
            return;
        }
        for (const auto& r : other.m_rects) {
            add(r);
        }
    }

    void intersect(const RectI& rect) noexcept {
        if (m_rects.empty() || rect.isEmpty() || !m_extents.intersects(rect)) {
            clear();
            return;
        }
        if (m_rects.size() == 1) {
            m_rects[0] = m_rects[0].intersection(rect);
            if (m_rects[0].isEmpty()) {
                clear();
            } else {
                m_extents = m_rects[0];
            }
            return;
        }
        // Check if rect already covers full extents -> no-op
        if (rect.x <= m_extents.x && rect.y <= m_extents.y &&
            rect.x + rect.width >= m_extents.x + m_extents.width &&
            rect.y + rect.height >= m_extents.y + m_extents.height) {
            return;
        }
        size_t writeIdx = 0;
        for (size_t i = 0; i < m_rects.size(); ++i) {
            if (m_rects[i].intersects(rect)) {
                m_rects[writeIdx++] = m_rects[i].intersection(rect);
            }
        }
        m_rects.resize(writeIdx);
        updateExtents();
    }

    void intersect(const Region& other) noexcept {
        if (m_rects.empty() || other.m_rects.empty() || !m_extents.intersects(other.m_extents)) {
            clear();
            return;
        }
        if (m_rects.size() == 1 && other.m_rects.size() == 1) {
            intersect(other.m_rects[0]);
            return;
        }
        // Intersections of two pairwise-disjoint sets are already pairwise disjoint
        std::vector<RectI> newRects;
        newRects.reserve(std::min(m_rects.size(), other.m_rects.size()));
        for (const auto& a : m_rects) {
            if (!a.intersects(other.m_extents)) continue;
            for (const auto& b : other.m_rects) {
                if (a.intersects(b)) {
                    newRects.push_back(a.intersection(b));
                }
            }
        }
        m_rects = std::move(newRects);
        updateExtents();
    }

    void subtract(const RectI& rect) noexcept {
        if (m_rects.empty() || rect.isEmpty() || !m_extents.intersects(rect)) return;
        std::vector<RectI> newRects;
        newRects.reserve(m_rects.size() + 4);
        for (const auto& a : m_rects) {
            detail::subtractRectAppend(a, rect, newRects);
        }
        m_rects = std::move(newRects);
        updateExtents();
    }

    void subtract(const Region& other) noexcept {
        if (m_rects.empty() || other.m_rects.empty() || !m_extents.intersects(other.m_extents)) return;
        std::vector<RectI> newRects;
        newRects.reserve(m_rects.size() + 4);
        for (const auto& b : other.m_rects) {
            if (!m_extents.intersects(b)) continue;
            newRects.clear();
            for (const auto& a : m_rects) {
                detail::subtractRectAppend(a, b, newRects);
            }
            m_rects.swap(newRects);
            if (m_rects.empty()) {
                m_extents = RectI{0, 0, 0, 0};
                return;
            }
        }
        updateExtents();
    }

    [[nodiscard]] RectI extents() const noexcept {
        return m_extents;
    }

    [[nodiscard]] bool intersects(const RectI& box) const noexcept {
        if (m_rects.empty() || !m_extents.intersects(box)) return false;
        for (const auto& r : m_rects) {
            if (r.intersects(box)) return true;
        }
        return false;
    }

    [[nodiscard]] bool intersects(const Region& other) const noexcept {
        if (m_rects.empty() || other.m_rects.empty() || !m_extents.intersects(other.m_extents)) return false;
        for (const auto& a : m_rects) {
            if (!a.intersects(other.m_extents)) continue;
            for (const auto& b : other.m_rects) {
                if (a.intersects(b)) return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<RectI>& rects() const noexcept {
        return m_rects;
    }

private:
    void expandExtents(const RectI& rect) noexcept {
        const int x1 = std::min(m_extents.x, rect.x);
        const int y1 = std::min(m_extents.y, rect.y);
        const int x2 = std::max(m_extents.x + m_extents.width, rect.x + rect.width);
        const int y2 = std::max(m_extents.y + m_extents.height, rect.y + rect.height);
        m_extents = RectI{x1, y1, x2 - x1, y2 - y1};
    }

    void updateExtents() noexcept {
        if (m_rects.empty()) {
            m_extents = RectI{0, 0, 0, 0};
            return;
        }
        int x1 = m_rects[0].x;
        int y1 = m_rects[0].y;
        int x2 = m_rects[0].x + m_rects[0].width;
        int y2 = m_rects[0].y + m_rects[0].height;

        for (size_t i = 1; i < m_rects.size(); ++i) {
            x1 = std::min(x1, m_rects[i].x);
            y1 = std::min(y1, m_rects[i].y);
            x2 = std::max(x2, m_rects[i].x + m_rects[i].width);
            y2 = std::max(y2, m_rects[i].y + m_rects[i].height);
        }
        m_extents = RectI{x1, y1, x2 - x1, y2 - y1};
    }

    std::vector<RectI> m_rects;
    RectI m_extents{0, 0, 0, 0};
};

} // namespace nisaba::compositor
