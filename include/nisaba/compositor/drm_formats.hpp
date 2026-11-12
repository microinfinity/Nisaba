#pragma once

#include <cstdint>
#include <drm_fourcc.h>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nisaba::compositor {

/**
 * @brief Representation of a DRM pixel format along with its hardware modifiers.
 */
struct DrmFormat {
    uint32_t format{DRM_FORMAT_INVALID};
    std::vector<uint64_t> modifiers;

    [[nodiscard]] bool hasModifier(uint64_t modifier) const noexcept {
        for (uint64_t mod : modifiers) {
            if (mod == modifier) return true;
        }
        return false;
    }

    bool addModifier(uint64_t modifier) {
        if (!hasModifier(modifier)) {
            modifiers.push_back(modifier);
            return true;
        }
        return false;
    }
};

/**
 * @brief A collection of DRM formats and their allowed modifiers.
 * 
 * Used for hardware plane capability negotiation and renderer buffer matching.
 */
class DrmFormatSet {
public:
    DrmFormatSet() = default;

    [[nodiscard]] bool empty() const noexcept {
        return m_formats.empty();
    }

    [[nodiscard]] size_t size() const noexcept {
        return m_formats.size();
    }

    [[nodiscard]] const std::vector<DrmFormat>& formats() const noexcept {
        return m_formats;
    }

    [[nodiscard]] const DrmFormat* get(uint32_t format) const noexcept {
        for (const auto& f : m_formats) {
            if (f.format == format) return &f;
        }
        return nullptr;
    }

    bool add(uint32_t format, uint64_t modifier) {
        for (auto& f : m_formats) {
            if (f.format == format) {
                return f.addModifier(modifier);
            }
        }
        m_formats.push_back(DrmFormat{format, {modifier}});
        return true;
    }

    [[nodiscard]] bool has(uint32_t format, uint64_t modifier) const noexcept {
        const auto* f = get(format);
        return f && f->hasModifier(modifier);
    }

    /**
     * @brief Computes the intersection of modifiers between two format sets.
     */
    [[nodiscard]] static DrmFormatSet intersect(const DrmFormatSet& a, const DrmFormatSet& b) {
        DrmFormatSet result;

        for (const auto& fa : a.formats()) {
            const auto* fb = b.get(fa.format);
            if (!fb) continue;

            for (uint64_t modA : fa.modifiers) {
                if (fb->hasModifier(modA)) {
                    result.add(fa.format, modA);
                }
            }
        }

        return result;
    }

    /**
     * @brief Helper to convert a FourCC code to a human-readable 4-character string.
     */
    [[nodiscard]] static std::string formatToString(uint32_t format) {
        char buf[5] = {
            static_cast<char>(format & 0xFF),
            static_cast<char>((format >> 8) & 0xFF),
            static_cast<char>((format >> 16) & 0xFF),
            static_cast<char>((format >> 24) & 0xFF),
            '\0'
        };
        return std::string(buf);
    }

private:
    std::vector<DrmFormat> m_formats;
};

inline std::string fourccToString(uint32_t format) {
    return DrmFormatSet::formatToString(format);
}

#ifndef DRM_FORMAT_XRGB16161616F
#define DRM_FORMAT_XRGB16161616F fourcc_code('X', 'R', '4', 'H')
#endif
#ifndef DRM_FORMAT_ARGB16161616F
#define DRM_FORMAT_ARGB16161616F fourcc_code('A', 'R', '4', 'H')
#endif
#ifndef DRM_FORMAT_XBGR16161616F
#define DRM_FORMAT_XBGR16161616F fourcc_code('X', 'B', '4', 'H')
#endif
#ifndef DRM_FORMAT_ABGR16161616F
#define DRM_FORMAT_ABGR16161616F fourcc_code('A', 'B', '4', 'H')
#endif

/**
 * @brief Checks whether a DRM FourCC format carries an active alpha channel.
 */
[[nodiscard]] constexpr bool drmFormatHasAlpha(uint32_t format) noexcept {
    switch (format) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_RGBA1010102:
        case DRM_FORMAT_BGRA1010102:
        case DRM_FORMAT_ARGB16161616F:
        case DRM_FORMAT_ABGR16161616F:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Checks whether a DRM FourCC format uses 16-bit floating-point channels (FP16).
 */
[[nodiscard]] constexpr bool drmFormatIsFp16(uint32_t format) noexcept {
    switch (format) {
        case DRM_FORMAT_XRGB16161616F:
        case DRM_FORMAT_ARGB16161616F:
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_ABGR16161616F:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Checks whether a DRM FourCC format uses 10-bit color channels.
 */
[[nodiscard]] constexpr bool drmFormatIs10Bit(uint32_t format) noexcept {
    switch (format) {
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_RGBA1010102:
        case DRM_FORMAT_BGRA1010102:
            return true;
        default:
            return false;
    }
}

} // namespace nisaba::compositor
