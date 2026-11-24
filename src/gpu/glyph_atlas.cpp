#include "glyph_atlas.hpp"

namespace nisaba::gpu {

SovereignGlyphAtlas::SovereignGlyphAtlas(int width, int height)
    : m_width(width), m_height(height) {
    m_data.resize(static_cast<size_t>(m_width) * static_cast<size_t>(m_height), 0);
    reset();
}

void SovereignGlyphAtlas::reset() {
    ++m_generation;
    std::fill(m_data.begin(), m_data.end(), 0);
    m_entries.clear();
    m_fastTable.assign(kFastTableSize, FastEntry{});
    m_nodes.clear();
    m_nodes.push_back(SkylineNode{0, 0, m_width});

    m_dirty = true;
    m_dirtyMinX = 0;
    m_dirtyMinY = 0;
    m_dirtyMaxX = m_width;
    m_dirtyMaxY = m_height;
}

bool SovereignGlyphAtlas::resize(int newWidth, int newHeight) {
    if (newWidth <= 0 || newHeight <= 0) return false;
    m_width = newWidth;
    m_height = newHeight;
    m_data.assign(static_cast<size_t>(m_width) * static_cast<size_t>(m_height), 0);
    reset();
    return true;
}

const AtlasGlyphEntry* SovereignGlyphAtlas::findSlow(const text::CacheKey& key) const noexcept {
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        size_t idx = (static_cast<size_t>(key.glyph_id) * 16u +
                      (static_cast<size_t>(key.x_bin) << 2) +
                      static_cast<size_t>(key.y_bin) +
                      (static_cast<size_t>(key.font_size_bits) * 2654435761u)) & (kFastTableSize - 1);
        m_fastTable[idx] = FastEntry{key, it->second, true};
        return &m_fastTable[idx].entry;
    }
    return nullptr;
}

int SovereignGlyphAtlas::rectFits(size_t idx, int w, int h) const noexcept {
    int x = m_nodes[idx].x;
    if (x + w > m_width) return -1;

    int y = m_nodes[idx].y;
    int widthLeft = w;
    size_t i = idx;

    while (widthLeft > 0) {
        if (i >= m_nodes.size()) return -1;
        y = std::max(y, m_nodes[i].y);
        if (y + h > m_height) return -1;
        widthLeft -= m_nodes[i].width;
        ++i;
    }
    return y;
}

void SovereignGlyphAtlas::addSkylineLevel(size_t idx, int x, int y, int w, int h) {
    SkylineNode newNode{x, y + h, w};
    m_nodes.insert(m_nodes.begin() + idx, newNode);

    for (size_t i = idx + 1; i < m_nodes.size(); ++i) {
        if (m_nodes[i].x < m_nodes[i - 1].x + m_nodes[i - 1].width) {
            int shrink = m_nodes[i - 1].x + m_nodes[i - 1].width - m_nodes[i].x;
            m_nodes[i].x += shrink;
            m_nodes[i].width -= shrink;
            if (m_nodes[i].width <= 0) {
                m_nodes.erase(m_nodes.begin() + i);
                --i;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    for (size_t i = 0; i + 1 < m_nodes.size(); ++i) {
        if (m_nodes[i].y == m_nodes[i + 1].y) {
            m_nodes[i].width += m_nodes[i + 1].width;
            m_nodes.erase(m_nodes.begin() + i + 1);
            --i;
        }
    }
}

std::optional<AtlasGlyphEntry> SovereignGlyphAtlas::insert(const text::CacheKey& key, const text::CachedGlyph* glyph) {
    size_t fastIdx = (static_cast<size_t>(key.glyph_id) * 16u +
                      (static_cast<size_t>(key.x_bin) << 2) +
                      static_cast<size_t>(key.y_bin) +
                      (static_cast<size_t>(key.font_size_bits) * 2654435761u)) & (kFastTableSize - 1);
    if (!glyph || glyph->width == 0 || glyph->height == 0 || glyph->data.empty()) {
        AtlasGlyphEntry emptyEntry{0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0};
        m_entries.emplace(key, emptyEntry);
        m_fastTable[fastIdx] = FastEntry{key, emptyEntry, true};
        return emptyEntry;
    }

    int pw = static_cast<int>(glyph->width) + 2;
    int ph = static_cast<int>(glyph->height) + 2;

    int bestIdx = -1;
    int bestY = m_height + 1;
    int bestWidth = m_width + 1;

    for (size_t i = 0; i < m_nodes.size(); ++i) {
        int y = rectFits(i, pw, ph);
        if (y != -1) {
            if (y < bestY || (y == bestY && m_nodes[i].width < bestWidth)) {
                bestY = y;
                bestWidth = m_nodes[i].width;
                bestIdx = static_cast<int>(i);
            }
        }
    }

    if (bestIdx == -1) {
        return std::nullopt;
    }

    int rx = m_nodes[bestIdx].x;
    int ry = bestY;
    addSkylineLevel(static_cast<size_t>(bestIdx), rx, ry, pw, ph);

    int gx = rx + 1;
    int gy = ry + 1;

    for (uint32_t r = 0; r < glyph->height; ++r) {
        std::memcpy(&m_data[(gy + r) * m_width + gx],
                    &glyph->data[r * glyph->width],
                    glyph->width);
    }

    if (!m_dirty) {
        m_dirty = true;
        m_dirtyMinX = gx;
        m_dirtyMinY = gy;
        m_dirtyMaxX = gx + static_cast<int>(glyph->width);
        m_dirtyMaxY = gy + static_cast<int>(glyph->height);
    } else {
        m_dirtyMinX = std::min(m_dirtyMinX, gx);
        m_dirtyMinY = std::min(m_dirtyMinY, gy);
        m_dirtyMaxX = std::max(m_dirtyMaxX, gx + static_cast<int>(glyph->width));
        m_dirtyMaxY = std::max(m_dirtyMaxY, gy + static_cast<int>(glyph->height));
    }

    AtlasGlyphEntry entry;
    entry.u0 = static_cast<float>(gx) / static_cast<float>(m_width);
    entry.v0 = static_cast<float>(gy) / static_cast<float>(m_height);
    entry.u1 = static_cast<float>(gx + glyph->width) / static_cast<float>(m_width);
    entry.v1 = static_cast<float>(gy + glyph->height) / static_cast<float>(m_height);
    entry.offsetX = glyph->offset_x;
    entry.offsetY = glyph->offset_y;
    entry.width = static_cast<int>(glyph->width);
    entry.height = static_cast<int>(glyph->height);

    auto [insertedIt, _] = m_entries.emplace(key, entry);
    m_fastTable[fastIdx] = FastEntry{key, entry, true};
    return insertedIt->second;
}

void SovereignGlyphAtlas::flushToGpu(Renderer* renderer, int textureId) {
    if (!m_dirty || textureId == 0 || !renderer) return;

    int w = m_dirtyMaxX - m_dirtyMinX;
    int h = m_dirtyMaxY - m_dirtyMinY;
    if (w > 0 && h > 0) {
        renderer->updateTexture(textureId, m_dirtyMinX, m_dirtyMinY, w, h, m_data.data());
    }

    m_dirty = false;
    m_dirtyMinX = m_width;
    m_dirtyMinY = m_height;
    m_dirtyMaxX = 0;
    m_dirtyMaxY = 0;
}

} // namespace nisaba::gpu
