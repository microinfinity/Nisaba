#pragma once

#include "nisaba/compositor/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nisaba::compositor {

/**
 * @brief Multi-buffer damage tracking ring for swapchains and double/triple buffering.
 * Accumulates damage regions between frames and calculates minimal redraw regions.
 */
class DamageRing {
public:
    DamageRing() noexcept = default;
    ~DamageRing() noexcept = default;

    DamageRing(const DamageRing&) = delete;
    DamageRing& operator=(const DamageRing&) = delete;

    /**
     * @brief Adds a damaged region to the current frame.
     */
    void add(const Region& region) noexcept;

    /**
     * @brief Adds a damaged rectangle to the current frame.
     */
    void addRect(const RectI& box) noexcept;

    /**
     * @brief Marks the entire output area as damaged.
     */
    void addWhole(int width, int height) noexcept;

    /**
     * @brief RAII transaction allowing atomic KMS commit rollback without losing frame damage.
     */
    class Transaction {
    public:
        Transaction(DamageRing& ring, uintptr_t bufferId, Region rotatedDamage, Region previousCurrent) noexcept
            : m_ring(&ring),
              m_bufferId(bufferId),
              m_rotatedDamage(std::move(rotatedDamage)),
              m_savedCurrent(std::move(previousCurrent)) {}

        ~Transaction() noexcept {
            if (!m_committed && m_rolledBack && m_ring) {
                // Already handled by rollback()
            }
        }

        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&& other) noexcept
            : m_ring(other.m_ring),
              m_bufferId(other.m_bufferId),
              m_rotatedDamage(std::move(other.m_rotatedDamage)),
              m_savedCurrent(std::move(other.m_savedCurrent)),
              m_committed(other.m_committed),
              m_rolledBack(other.m_rolledBack) {
            other.m_ring = nullptr;
        }

        [[nodiscard]] const Region& damage() const noexcept { return m_rotatedDamage; }
        void commit() noexcept { m_committed = true; }
        void rollback() noexcept;

    private:
        DamageRing* m_ring{nullptr};
        uintptr_t m_bufferId{0};
        Region m_rotatedDamage;
        Region m_savedCurrent;
        bool m_committed{false};
        bool m_rolledBack{false};
    };

    /**
     * @brief Calculates accumulated damage needed to update target buffer and rotates ring.
     * @param bufferId Unique identifier of buffer (pointer address or index).
     * @param width Buffer width in pixels.
     * @param height Buffer height in pixels.
     * @return Minimal region requiring redraw on this buffer.
     */
    [[nodiscard]] Region rotate(uintptr_t bufferId, int width, int height);

    /**
     * @brief Rotates the damage ring wrapped in a rollback-capable Transaction.
     */
    [[nodiscard]] Transaction beginTransaction(uintptr_t bufferId, int width, int height);

    /**
     * @brief Expands all pending damage rectangles outward by radiusPixels (for blur halo coverage).
     */
    void expandPending(int radiusPixels) noexcept;

    /**
     * @brief Clears all damage state.
     */
    void clear() noexcept;

    [[nodiscard]] bool hasPendingDamage() const noexcept { return !m_current.isEmpty(); }
    [[nodiscard]] const Region& pendingDamage() const noexcept { return m_current; }

private:
    struct BufferEntry {
        uintptr_t bufferId{0};
        Region damage;
    };

    Region m_current;
    std::vector<BufferEntry> m_entries;
    static constexpr size_t kMaxRects = 20;
};

} // namespace nisaba::compositor
