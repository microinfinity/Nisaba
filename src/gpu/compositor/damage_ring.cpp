#include "nisaba/compositor/damage_ring.hpp"

#include <algorithm>

namespace nisaba::compositor {

void DamageRing::add(const Region& region) noexcept {
    m_current.unite(region);
}

void DamageRing::addRect(const RectI& box) noexcept {
    if (box.isEmpty()) return;
    m_current.add(box);
}

void DamageRing::addWhole(int width, int height) noexcept {
    if (width <= 0 || height <= 0) return;
    m_current.add(RectI{0, 0, width, height});
}

Region DamageRing::rotate(uintptr_t bufferId, int width, int height) {
    // Accumulate pending frame damage into every previously rendered buffer's history
    if (!m_current.isEmpty()) {
        for (auto& entry : m_entries) {
            entry.damage.unite(m_current);
            if (entry.damage.rects().size() > kMaxRects) {
                entry.damage = Region(entry.damage.extents());
            }
        }
    }

    auto it = std::find_if(m_entries.begin(), m_entries.end(), [bufferId](const BufferEntry& e) {
        return e.bufferId == bufferId;
    });

    if (it != m_entries.end()) {
        Region damage = std::move(it->damage);
        it->damage.clear();
        m_current.clear();

        damage.intersect(Region(RectI{0, 0, width, height}));
        if (damage.rects().size() > kMaxRects) {
            damage = Region(damage.extents());
        }
        return damage;
    }

    // New buffer seen for the first time: full redraw required
    Region fullDamage(RectI{0, 0, width, height});

    if (m_entries.size() >= kMaxTrackedBuffers) {
        m_entries.erase(m_entries.begin());
    }

    BufferEntry newEntry;
    newEntry.bufferId = bufferId;
    m_entries.push_back(std::move(newEntry));

    m_current.clear();
    return fullDamage;
}

void DamageRing::Transaction::rollback() noexcept {
    if (!m_ring || m_rolledBack) return;
    Region extraPending = std::move(m_ring->m_current);
    m_ring->m_entries = std::move(m_savedEntries);
    m_ring->m_current = std::move(m_savedCurrent);
    if (!extraPending.isEmpty()) {
        m_ring->m_current.unite(extraPending);
    }
    m_rolledBack = true;
    m_committed = false;
}

DamageRing::Transaction DamageRing::beginTransaction(uintptr_t bufferId, int width, int height) {
    Region savedCurrent = m_current;
    std::vector<BufferEntry> savedEntries = m_entries;
    Region rotated = rotate(bufferId, width, height);
    return Transaction(*this, bufferId, std::move(rotated), std::move(savedCurrent), std::move(savedEntries));
}

void DamageRing::expandPending(int radiusPixels) noexcept {
    if (radiusPixels <= 0 || m_current.isEmpty()) return;
    const auto rects = m_current.rects();
    Region expanded;
    for (const auto& r : rects) {
        expanded.add(RectI{
            r.x - radiusPixels,
            r.y - radiusPixels,
            r.width + radiusPixels * 2,
            r.height + radiusPixels * 2
        });
    }
    m_current = std::move(expanded);
}

void DamageRing::clear() noexcept {
    m_current.clear();
    m_entries.clear();
}

} // namespace nisaba::compositor
