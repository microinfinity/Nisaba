#include "nisaba/compositor/color_management.hpp"
#include "nisaba/compositor/drm_formats.hpp"
#include "nisaba/compositor/log.hpp"
#include "nisaba/compositor/shm_texture.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nisaba::compositor {

namespace {

// SMPTE ST 2084 (PQ) constants
constexpr float kPqM1 = 0.1593017578125f;      // 2610 / 16384
constexpr float kPqM2 = 78.84375f;             // 2523 / 32 * 128
constexpr float kPqC1 = 0.8359375f;            // 3424 / 4096
constexpr float kPqC2 = 18.8515625f;           // 2413 / 128
constexpr float kPqC3 = 18.6875f;              // 2392 / 128

// ITU-R BT.2100 HLG constants
constexpr float kHlgA = 0.17883277f;
constexpr float kHlgB = 0.28466892f;           // 1 - 4 * a
constexpr float kHlgC = 0.55991073f;           // 0.5 - a * ln(4 * a)

constexpr std::array<float, 3> xyToXyz(float x, float y) noexcept {
    if (std::abs(y) < 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {x / y, 1.0f, (1.0f - x - y) / y};
}

constexpr std::array<float, 3> mulMat3Vec3(const std::array<float, 9>& m, const std::array<float, 3>& v) noexcept {
    // Column-major: m[col * 3 + row]
    return {
        m[0] * v[0] + m[3] * v[1] + m[6] * v[2],
        m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
        m[2] * v[0] + m[5] * v[1] + m[8] * v[2]
    };
}

} // namespace

std::array<float, 9> ColorManagement::invertMatrix3x3(const std::array<float, 9>& m) noexcept {
    // Column-major indices:
    // [ m0  m3  m6 ]
    // [ m1  m4  m7 ]
    // [ m2  m5  m8 ]
    const float c00 = m[4] * m[8] - m[7] * m[5];
    const float c10 = -(m[1] * m[8] - m[7] * m[2]);
    const float c20 = m[1] * m[5] - m[4] * m[2];

    const float det = m[0] * c00 + m[3] * c10 + m[6] * c20;
    if (std::abs(det) < 1e-8f) {
        return {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float invDet = 1.0f / det;
    const float c01 = -(m[3] * m[8] - m[6] * m[5]);
    const float c11 = m[0] * m[8] - m[6] * m[2];
    const float c21 = -(m[0] * m[5] - m[3] * m[2]);

    const float c02 = m[3] * m[7] - m[6] * m[4];
    const float c12 = -(m[0] * m[7] - m[6] * m[1]);
    const float c22 = m[0] * m[4] - m[3] * m[1];

    // Adjugate transpose in column-major: col 0 is (c00, c10, c20), col 1 is (c01, c11, c21), col 2 is (c02, c12, c22)
    return {
        c00 * invDet, c10 * invDet, c20 * invDet,
        c01 * invDet, c11 * invDet, c21 * invDet,
        c02 * invDet, c12 * invDet, c22 * invDet
    };
}

std::array<float, 9> ColorManagement::multiplyMatrix3x3(
    const std::array<float, 9>& a,
    const std::array<float, 9>& b) noexcept {
    std::array<float, 9> out{};
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            out[col * 3 + row] =
                a[0 * 3 + row] * b[col * 3 + 0] +
                a[1 * 3 + row] * b[col * 3 + 1] +
                a[2 * 3 + row] * b[col * 3 + 2];
        }
    }
    return out;
}

std::array<float, 9> ColorManagement::rgbToXyzMatrix(const Chromaticities& c) noexcept {
    const auto rXyz = xyToXyz(c.rx, c.ry);
    const auto gXyz = xyToXyz(c.gx, c.gy);
    const auto bXyz = xyToXyz(c.bx, c.by);
    const auto wXyz = xyToXyz(c.wx, c.wy);

    const std::array<float, 9> p = {
        rXyz[0], rXyz[1], rXyz[2],
        gXyz[0], gXyz[1], gXyz[2],
        bXyz[0], bXyz[1], bXyz[2]
    };

    const auto pInv = invertMatrix3x3(p);
    const auto s = mulMat3Vec3(pInv, wXyz);

    return {
        p[0] * s[0], p[1] * s[0], p[2] * s[0],
        p[3] * s[1], p[4] * s[1], p[5] * s[1],
        p[6] * s[2], p[7] * s[2], p[8] * s[2]
    };
}

std::array<float, 9> ColorManagement::bradfordAdaptationMatrix(
    float wxSrc, float wySrc,
    float wxDst, float wyDst) noexcept {
    if (std::abs(wxSrc - wxDst) < 1e-5f && std::abs(wySrc - wyDst) < 1e-5f) {
        return {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    }

    // Bradford cone response matrix (column-major)
    constexpr std::array<float, 9> kBradford = {
         0.8951f, -0.7502f,  0.0389f,
         0.2664f,  1.7135f, -0.0685f,
        -0.1614f,  0.0367f,  1.0296f
    };
    const auto kBradfordInv = invertMatrix3x3(kBradford);

    const auto wSrc = xyToXyz(wxSrc, wySrc);
    const auto wDst = xyToXyz(wxDst, wyDst);

    const auto coneSrc = mulMat3Vec3(kBradford, wSrc);
    const auto coneDst = mulMat3Vec3(kBradford, wDst);

    const std::array<float, 9> scale = {
        (std::abs(coneSrc[0]) > 1e-6f) ? (coneDst[0] / coneSrc[0]) : 1.0f, 0.0f, 0.0f,
        0.0f, (std::abs(coneSrc[1]) > 1e-6f) ? (coneDst[1] / coneSrc[1]) : 1.0f, 0.0f,
        0.0f, 0.0f, (std::abs(coneSrc[2]) > 1e-6f) ? (coneDst[2] / coneSrc[2]) : 1.0f
    };

    return multiplyMatrix3x3(kBradfordInv, multiplyMatrix3x3(scale, kBradford));
}

std::array<float, 9> ColorManagement::computeGamutMatrix(
    const Chromaticities& src,
    const Chromaticities& dst) noexcept {
    const auto srcToXyz = rgbToXyzMatrix(src);
    const auto dstToXyz = rgbToXyzMatrix(dst);
    const auto xyzToDst = invertMatrix3x3(dstToXyz);
    const auto adapt = bradfordAdaptationMatrix(src.wx, src.wy, dst.wx, dst.wy);

    return multiplyMatrix3x3(xyzToDst, multiplyMatrix3x3(adapt, srcToXyz));
}

ColorTransformParams ColorManagement::buildTransform(
    const ImageDescription& source,
    const ImageDescription& target,
    TonemapOperator preferredTonemap) noexcept {
    ColorTransformParams params;

    const auto srcChrom = source.resolveChromaticities();
    const auto dstChrom = target.resolveChromaticities();

    const bool sameGamut = (source.primaries == target.primaries) &&
                           (source.primaries != ColorPrimaries::Custom);
    const bool sameTf = (source.transferFunction == target.transferFunction);
    const bool needsLumScale = std::abs(source.referenceWhiteNits - target.referenceWhiteNits) > 0.5f ||
                               std::abs(source.sdrBrightnessMultiplier - 1.0f) > 1e-4f ||
                               std::abs(source.sdrSaturationMultiplier - 1.0f) > 1e-4f;
    const bool needsTonemap = source.maxLuminanceNits > target.maxLuminanceNits * 1.05f;

    params.enabled = !(sameGamut && sameTf && !needsLumScale && !needsTonemap);
    params.sourceTf = source.transferFunction;
    params.targetTf = target.transferFunction;
    params.gamutMatrix = sameGamut
        ? std::array<float, 9>{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}
        : computeGamutMatrix(srcChrom, dstChrom);
    params.srcRefLuminance = std::max(source.referenceWhiteNits, 1.0f);
    params.dstRefLuminance = std::max(target.referenceWhiteNits, 1.0f);
    params.srcMaxLuminance = std::max(source.maxLuminanceNits, params.srcRefLuminance);
    params.dstMaxLuminance = std::max(target.maxLuminanceNits, params.dstRefLuminance);
    params.sdrBrightness = source.sdrBrightnessMultiplier;
    params.sdrSaturation = source.sdrSaturationMultiplier;
    params.tonemap = needsTonemap ? preferredTonemap : TonemapOperator::None;

    return params;
}

float ColorManagement::evaluateEotf(TransferFunction tf, float v) noexcept {
    v = std::max(0.0f, v);
    switch (tf) {
        case TransferFunction::Linear:
            return v;
        case TransferFunction::Srgb:
            return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
        case TransferFunction::Gamma22:
            return std::pow(v, 2.2f);
        case TransferFunction::Gamma28:
            return std::pow(v, 2.8f);
        case TransferFunction::Bt1886:
            return std::pow(v, 2.4f);
        case TransferFunction::PqSt2084: {
            const float vp = std::pow(v, 1.0f / kPqM2);
            const float num = std::max(vp - kPqC1, 0.0f);
            const float den = kPqC2 - kPqC3 * vp;
            const float lNorm = std::pow(num / std::max(den, 1e-6f), 1.0f / kPqM1);
            // PQ 1.0 = 10,000 nits; normalize relative to 203 nits SDR reference white
            return (lNorm * 10000.0f) / 203.0f;
        }
        case TransferFunction::Hlg: {
            if (v <= 0.5f) {
                return (v * v) / 3.0f;
            }
            return (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12.0f;
        }
    }
    return v;
}

float ColorManagement::evaluateOetf(TransferFunction tf, float l) noexcept {
    l = std::max(0.0f, l);
    switch (tf) {
        case TransferFunction::Linear:
            return l;
        case TransferFunction::Srgb:
            return (l <= 0.0031308f) ? (12.92f * l) : (1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
        case TransferFunction::Gamma22:
            return std::pow(l, 1.0f / 2.2f);
        case TransferFunction::Gamma28:
            return std::pow(l, 1.0f / 2.8f);
        case TransferFunction::Bt1886:
            return std::pow(l, 1.0f / 2.4f);
        case TransferFunction::PqSt2084: {
            const float lNorm = std::clamp((l * 203.0f) / 10000.0f, 0.0f, 1.0f);
            const float lm = std::pow(lNorm, kPqM1);
            return std::pow((kPqC1 + kPqC2 * lm) / (1.0f + kPqC3 * lm), kPqM2);
        }
        case TransferFunction::Hlg: {
            if (l <= 1.0f / 12.0f) {
                return std::sqrt(3.0f * l);
            }
            return kHlgA * std::log(12.0f * l - kHlgB) + kHlgC;
        }
    }
    return l;
}

Color ColorManagement::convertColor(
    const Color& color,
    const ImageDescription& source,
    const ImageDescription& target) noexcept {
    const auto params = buildTransform(source, target);
    if (!params.enabled) return color;

    std::array<float, 3> lin = {
        evaluateEotf(params.sourceTf, color.r),
        evaluateEotf(params.sourceTf, color.g),
        evaluateEotf(params.sourceTf, color.b)
    };

    // Gamut conversion
    lin = mulMat3Vec3(params.gamutMatrix, lin);

    // Luminance scaling
    const float lumScale = (params.srcRefLuminance / params.dstRefLuminance) * params.sdrBrightness;
    lin[0] *= lumScale;
    lin[1] *= lumScale;
    lin[2] *= lumScale;

    // Simple Reinhard tone mapping if active
    if (params.tonemap != TonemapOperator::None) {
        const float peak = std::max(params.dstMaxLuminance / params.dstRefLuminance, 1.0f);
        for (float& ch : lin) {
            ch = std::max(0.0f, ch);
            ch = (ch * (1.0f + ch / (peak * peak))) / (1.0f + ch);
        }
    }

    return Color{
        std::clamp(evaluateOetf(params.targetTf, lin[0]), 0.0f, 1.0f),
        std::clamp(evaluateOetf(params.targetTf, lin[1]), 0.0f, 1.0f),
        std::clamp(evaluateOetf(params.targetTf, lin[2]), 0.0f, 1.0f),
        color.a
    };
}

// ==========================================
// Lut3DTexture Implementation
// ==========================================

std::shared_ptr<Lut3DTexture> Lut3DTexture::create(std::span<const float> rgbCube, uint32_t dimN) {
    if (dimN < 2 || rgbCube.size() < static_cast<size_t>(dimN) * dimN * dimN * 3) {
        logError("Lut3DTexture::create: invalid LUT dimension {} or cube size {}", dimN, rgbCube.size());
        return nullptr;
    }

    const uint32_t atlasWidth = dimN * dimN;
    const uint32_t atlasHeight = dimN;
    std::vector<uint8_t> rgbaPixels(static_cast<size_t>(atlasWidth) * atlasHeight * 4);

    // Pack 3D cube [b][g][r] into 2D strip (x = b * N + r, y = g)
    for (uint32_t b = 0; b < dimN; ++b) {
        for (uint32_t g = 0; g < dimN; ++g) {
            for (uint32_t r = 0; r < dimN; ++r) {
                const size_t srcIdx = (static_cast<size_t>(b) * dimN * dimN +
                                       static_cast<size_t>(g) * dimN + r) * 3;
                const uint32_t x = b * dimN + r;
                const uint32_t y = g;
                const size_t dstIdx = (static_cast<size_t>(y) * atlasWidth + x) * 4;

                rgbaPixels[dstIdx + 0] = static_cast<uint8_t>(std::clamp(rgbCube[srcIdx + 0], 0.0f, 1.0f) * 255.0f + 0.5f);
                rgbaPixels[dstIdx + 1] = static_cast<uint8_t>(std::clamp(rgbCube[srcIdx + 1], 0.0f, 1.0f) * 255.0f + 0.5f);
                rgbaPixels[dstIdx + 2] = static_cast<uint8_t>(std::clamp(rgbCube[srcIdx + 2], 0.0f, 1.0f) * 255.0f + 0.5f);
                rgbaPixels[dstIdx + 3] = 255;
            }
        }
    }

    auto tex = ShmTexture::create(
        atlasWidth,
        atlasHeight,
        DRM_FORMAT_ABGR8888,
        false,
        rgbaPixels.data(),
        atlasWidth * 4);
    if (!tex) return nullptr;

    return std::make_shared<Lut3DTexture>(std::move(tex), dimN);
}

std::shared_ptr<Lut3DTexture> Lut3DTexture::createIdentity(uint32_t dimN) {
    if (dimN < 2) dimN = 17;
    std::vector<float> cube(static_cast<size_t>(dimN) * dimN * dimN * 3);
    const float inv = 1.0f / static_cast<float>(dimN - 1);

    for (uint32_t b = 0; b < dimN; ++b) {
        for (uint32_t g = 0; g < dimN; ++g) {
            for (uint32_t r = 0; r < dimN; ++r) {
                const size_t idx = (static_cast<size_t>(b) * dimN * dimN +
                                    static_cast<size_t>(g) * dimN + r) * 3;
                cube[idx + 0] = static_cast<float>(r) * inv;
                cube[idx + 1] = static_cast<float>(g) * inv;
                cube[idx + 2] = static_cast<float>(b) * inv;
            }
        }
    }
    return create(cube, dimN);
}

Lut3DTexture::Lut3DTexture(std::shared_ptr<Texture> atlasTex, uint32_t dimN) noexcept
    : m_atlasTexture(std::move(atlasTex)), m_dimN(dimN) {}

} // namespace nisaba::compositor
