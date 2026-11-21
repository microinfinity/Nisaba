#pragma once

/// @file lottie_types.hpp
/// @brief Core types and enumerations for the sovereign Lottie subsystem in Nisaba.

#include <cstdint>
#include "nisaba/color/blend_mode.hpp"

namespace nisaba::lottie {

enum class LayerType : uint8_t {
    Precomp = 0,
    Solid = 1,
    Image = 2,
    Null = 3,
    Shape = 4,
    Text = 5,
    Audio = 6,
    Camera = 13,
    Unknown = 255
};

enum class MatteType : uint8_t {
    None = 0,
    Alpha = 1,
    InvertedAlpha = 2,
    Luma = 3,
    InvertedLuma = 4
};

enum class LineCap : uint8_t {
    Butt = 1,
    Round = 2,
    Square = 3
};

enum class LineJoin : uint8_t {
    Miter = 1,
    Round = 2,
    Bevel = 3
};

enum class FillRule : uint8_t {
    NonZero = 1,
    EvenOdd = 2
};

enum class GradientType : uint8_t {
    Linear = 1,
    Radial = 2
};

enum class TrimType : uint8_t {
    Simultaneously = 1,
    Individually = 2
};

enum class MergeMode : uint8_t {
    Merge = 1,
    Add = 2,
    Subtract = 3,
    Intersect = 4,
    ExcludeIntersections = 5
};

enum class CompositeOrder : uint8_t {
    Above = 1,
    Below = 2
};

enum class MaskMode : char {
    None = 'n',
    Add = 'a',
    Subtract = 's',
    Intersect = 'i',
    Lighten = 'l',
    Darken = 'd',
    Difference = 'f'
};

enum class LoopMode : uint8_t {
    Once = 0,
    Loop = 1,
    Bounce = 2
};

enum class EffectType : int {
    Tint = 20,
    Fill = 21,
    Stroke = 22,
    Tritone = 23,
    ProLevels = 24,
    DropShadow = 25,
    RadialWipe = 26,
    DisplacementMap = 27,
    Matte3 = 28,
    GaussianBlur = 29,
    // Extended Color & Tone Effects
    BrightnessContrast = 100,
    HueSaturation = 101,
    BlackAndWhite = 102,
    Invert = 103,
    Threshold = 104,
    ShiftChannels = 105,
    CCToner = 106,
    EasyLevels = 107,
    GradientRamp = 108,
    FourColorGradient = 109,
    // Transition, Tiling, Distortion, Blur & Procedural Effects
    LinearWipe = 110,
    VenetianBlinds = 111,
    MotionTile = 112,
    TransformEffect = 113,
    CornerPin = 114,
    Bulge = 115,
    Sphere = 116,
    DirectionalBlur = 117,
    Sharpen = 118,
    FractalNoise = 119,
    Unknown = -1
};

enum class TextJustify : int {
    Left = 0,
    Right = 1,
    Center = 2,
    JustifyLastLineLeft = 3,
    JustifyLastLineRight = 4,
    JustifyLastLineCenter = 5,
    JustifyLastLineFull = 6
};

/// Maps a Bodymovin numeric blend mode ("bm") to nisaba::BlendMode.
[[nodiscard]] constexpr nisaba::BlendMode map_lottie_blend_mode(int bm) noexcept {
    switch (bm) {
        case 0:  return nisaba::BlendMode::SourceOver; // Normal
        case 1:  return nisaba::BlendMode::Multiply;
        case 2:  return nisaba::BlendMode::Screen;
        case 3:  return nisaba::BlendMode::Overlay;
        case 4:  return nisaba::BlendMode::Darken;
        case 5:  return nisaba::BlendMode::Lighten;
        case 6:  return nisaba::BlendMode::ColorDodge;
        case 7:  return nisaba::BlendMode::ColorBurn;
        case 8:  return nisaba::BlendMode::HardLight;
        case 9:  return nisaba::BlendMode::SoftLight;
        case 10: return nisaba::BlendMode::Difference;
        case 11: return nisaba::BlendMode::Exclusion;
        case 12: return nisaba::BlendMode::Hue;
        case 13: return nisaba::BlendMode::Saturation;
        case 14: return nisaba::BlendMode::Color;
        case 15: return nisaba::BlendMode::Luminosity;
        case 16: return nisaba::BlendMode::Plus;       // Add / Linear Dodge
        default: return nisaba::BlendMode::SourceOver;
    }
}

} // namespace nisaba::lottie
