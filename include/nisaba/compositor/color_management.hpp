#pragma once

#include "nisaba/compositor/color.hpp"
#include "nisaba/compositor/matrix3.hpp"
#include "nisaba/compositor/texture.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace nisaba::compositor {

/**
 * @brief Standard electro-optical / opto-electronic transfer functions (EOTF / OETF).
 * Matches Wayland wp_color_management_v1 transfer function definitions.
 */
enum class TransferFunction : uint8_t {
    Srgb = 0,       ///< IEC 61966-2-1 piece-wise sRGB curve
    Linear = 1,     ///< Linear light (gamma = 1.0)
    Gamma22 = 2,    ///< Pure power law 2.2
    Gamma28 = 3,    ///< Pure power law 2.8
    Bt1886 = 4,     ///< ITU-R BT.1886 / Rec.709 broadcast curve
    PqSt2084 = 5,   ///< SMPTE ST 2084 Perceptual Quantizer (HDR10, up to 10,000 cd/m^2)
    Hlg = 6         ///< ITU-R BT.2100 Hybrid Log-Gamma
};

/**
 * @brief Standard color gamut primaries (chromaticity coordinates).
 */
enum class ColorPrimaries : uint8_t {
    Srgb = 0,       ///< ITU-R BT.709 / sRGB D65
    DisplayP3 = 1,  ///< SMPTE RP 431-2 / Apple Display-P3 D65
    Bt2020 = 2,     ///< ITU-R BT.2020 / Rec.2100 Wide Gamut D65
    AdobeRgb = 3,   ///< Adobe RGB (1998) D65
    Custom = 4      ///< User-specified CIE 1931 xy chromaticities
};

/**
 * @brief HDR-to-SDR or HDR-to-HDR tone-mapping operators.
 */
enum class TonemapOperator : uint8_t {
    None = 0,
    Reinhard = 1,       ///< Extended Reinhard luminance operator
    Bt2390Eetf = 2,     ///< ITU-R BT.2390 Hermite spline roll-off EETF
    HableFilmic = 3     ///< Uncharted 2 filmic curve
};

/**
 * @brief CIE 1931 (x, y) chromaticity coordinates for RGB primaries and white point.
 */
struct Chromaticities {
    float rx{0.640f}, ry{0.330f};
    float gx{0.300f}, gy{0.600f};
    float bx{0.150f}, by{0.060f};
    float wx{0.3127f}, wy{0.3290f}; // D65

    [[nodiscard]] static constexpr Chromaticities forPrimaries(ColorPrimaries p) noexcept {
        switch (p) {
            case ColorPrimaries::Srgb:
                return {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f, 0.3127f, 0.3290f};
            case ColorPrimaries::DisplayP3:
                return {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f};
            case ColorPrimaries::Bt2020:
                return {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
            case ColorPrimaries::AdobeRgb:
                return {0.640f, 0.330f, 0.210f, 0.710f, 0.150f, 0.060f, 0.3127f, 0.3290f};
            case ColorPrimaries::Custom:
            default:
                return {};
        }
    }
};

/**
 * @brief Complete colorimetry & mastering luminance description for a surface or output.
 */
struct ImageDescription {
    ColorPrimaries primaries{ColorPrimaries::Srgb};
    TransferFunction transferFunction{TransferFunction::Srgb};
    Chromaticities customChromaticities{};
    float minLuminanceNits{0.2f};
    float maxLuminanceNits{80.0f};
    float referenceWhiteNits{80.0f};
    float sdrBrightnessMultiplier{1.0f};
    float sdrSaturationMultiplier{1.0f};

    [[nodiscard]] Chromaticities resolveChromaticities() const noexcept {
        return (primaries == ColorPrimaries::Custom)
                   ? customChromaticities
                   : Chromaticities::forPrimaries(primaries);
    }

    [[nodiscard]] bool isStandardSrgb() const noexcept {
        return primaries == ColorPrimaries::Srgb &&
               transferFunction == TransferFunction::Srgb &&
               std::abs(sdrBrightnessMultiplier - 1.0f) < 1e-4f &&
               std::abs(sdrSaturationMultiplier - 1.0f) < 1e-4f;
    }

    [[nodiscard]] static ImageDescription srgbDefault() noexcept {
        return ImageDescription{};
    }

    [[nodiscard]] static ImageDescription hdr10Bt2020(float maxNits = 1000.0f, float refWhiteNits = 203.0f) noexcept {
        ImageDescription desc;
        desc.primaries = ColorPrimaries::Bt2020;
        desc.transferFunction = TransferFunction::PqSt2084;
        desc.minLuminanceNits = 0.005f;
        desc.maxLuminanceNits = maxNits;
        desc.referenceWhiteNits = refWhiteNits;
        return desc;
    }
};

/**
 * @brief Precomputed GPU shader parameters for converting between two ImageDescriptions.
 */
struct ColorTransformParams {
    bool enabled{false};
    TransferFunction sourceTf{TransferFunction::Srgb};
    TransferFunction targetTf{TransferFunction::Srgb};
    std::array<float, 9> gamutMatrix{
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    }; // Column-major 3x3 linear RGB conversion matrix
    float srcRefLuminance{80.0f};
    float dstRefLuminance{80.0f};
    float srcMaxLuminance{80.0f};
    float dstMaxLuminance{80.0f};
    float sdrBrightness{1.0f};
    float sdrSaturation{1.0f};
    TonemapOperator tonemap{TonemapOperator::None};
};

/**
 * @brief Sovereign Color Management mathematical engine.
 * Computes CIE 1931 XYZ primaries matrices, Bradford chromatic adaptation, and EOTF/OETF curves.
 */
class ColorManagement {
public:
    /**
     * @brief Computes the 3x3 column-major matrix mapping linear RGB from Chromaticities to CIE XYZ.
     */
    [[nodiscard]] static std::array<float, 9> rgbToXyzMatrix(const Chromaticities& c) noexcept;

    /**
     * @brief Inverts a 3x3 column-major matrix.
     */
    [[nodiscard]] static std::array<float, 9> invertMatrix3x3(const std::array<float, 9>& m) noexcept;

    /**
     * @brief Multiplies two 3x3 column-major matrices: result = A * B.
     */
    [[nodiscard]] static std::array<float, 9> multiplyMatrix3x3(
        const std::array<float, 9>& a,
        const std::array<float, 9>& b) noexcept;

    /**
     * @brief Computes the Bradford chromatic adaptation matrix from source white (wx1, wy1) to dest white (wx2, wy2).
     */
    [[nodiscard]] static std::array<float, 9> bradfordAdaptationMatrix(
        float wxSrc, float wySrc,
        float wxDst, float wyDst) noexcept;

    /**
     * @brief Computes the direct 3x3 column-major linear RGB conversion matrix from src to dst primaries.
     */
    [[nodiscard]] static std::array<float, 9> computeGamutMatrix(
        const Chromaticities& src,
        const Chromaticities& dst) noexcept;

    /**
     * @brief Builds complete ColorTransformParams between source and target ImageDescriptions.
     */
    [[nodiscard]] static ColorTransformParams buildTransform(
        const ImageDescription& source,
        const ImageDescription& target,
        TonemapOperator preferredTonemap = TonemapOperator::Bt2390Eetf) noexcept;

    /**
     * @brief Evaluates EOTF (encoded -> linear [0..1+]) on CPU for reference/color conversion.
     */
    [[nodiscard]] static float evaluateEotf(TransferFunction tf, float v) noexcept;

    /**
     * @brief Evaluates inverse EOTF / OETF (linear -> encoded) on CPU.
     */
    [[nodiscard]] static float evaluateOetf(TransferFunction tf, float l) noexcept;

    /**
     * @brief Converts a single Color from source ImageDescription to target ImageDescription on CPU.
     */
    [[nodiscard]] static Color convertColor(
        const Color& color,
        const ImageDescription& source,
        const ImageDescription& target) noexcept;
};

/**
 * @brief Hardware-accelerated 3D Look-Up Table (ICC Profile LUT) texture.
 * Packs an N x N x N 3D color cube into a horizontal 2D strip texture (width = N*N, height = N)
 * for universal GLES 2.0 / 3.0 trilinear interpolation.
 */
class Lut3DTexture {
public:
    /**
     * @brief Creates a 3D LUT texture from N*N*N RGB float triplets (size = N*N*N*3).
     */
    [[nodiscard]] static std::shared_ptr<Lut3DTexture> create(std::span<const float> rgbCube, uint32_t dimN);

    /**
     * @brief Creates an identity 3D LUT of dimension N for testing and baseline calibration.
     */
    [[nodiscard]] static std::shared_ptr<Lut3DTexture> createIdentity(uint32_t dimN = 17);

    Lut3DTexture(std::shared_ptr<Texture> atlasTex, uint32_t dimN) noexcept;
    ~Lut3DTexture() = default;

    [[nodiscard]] const std::shared_ptr<Texture>& texture() const noexcept { return m_atlasTexture; }
    [[nodiscard]] uint32_t dimension() const noexcept { return m_dimN; }

private:
    std::shared_ptr<Texture> m_atlasTexture;
    uint32_t m_dimN{0};
};

} // namespace nisaba::compositor

