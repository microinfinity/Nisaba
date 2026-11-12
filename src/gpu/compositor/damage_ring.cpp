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
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [bufferId](const BufferEntry& e) {
        return e.bufferId == bufferId;
    });

    if (it != m_entries.end()) {
        Region damage = m_current;

        for (const auto& entry : m_entries) {
            if (entry.bufferId != bufferId) {
                damage.unite(entry.damage);
            }
        }

        damage.intersect(Region(RectI{0, 0, width, height}));

        auto rects = damage.rects();
        if (rects.size() > kMaxRects) {
            damage = Region(damage.extents());
        }

        it->damage = m_current;
        m_current.clear();
        return damage;
    }

    // New buffer seen for the first time: full redraw required
    Region fullDamage(RectI{0, 0, width, height});

    BufferEntry newEntry;
    newEntry.bufferId = bufferId;
    newEntry.damage = m_current;
    m_entries.push_back(std::move(newEntry));

    m_current.clear();
    return fullDamage;
}

void DamageRing::Transaction::rollback() noexcept {
    if (!m_ring || m_rolledBack) return;
    m_ring->add(m_savedCurrent);
    m_rolledBack = true;
    m_committed = false;
}

DamageRing::Transaction DamageRing::beginTransaction(uintptr_t bufferId, int width, int height) {
    Region saved = m_current;
    Region rotated = rotate(bufferId, width, height);
    return Transaction(*this, bufferId, std::move(rotated), std::move(saved));
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
