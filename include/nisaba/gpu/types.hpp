#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <array>
#include <optional>
#include <memory>
#include "nisaba/color/color.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/math/point.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"
#include "nisaba/effects/image_filter.hpp"
#include "nisaba/effects/mask_filter.hpp"
#include "nisaba/effects/blender.hpp"

namespace nisaba::gpu {

class Shader;

inline constexpr float PI = 3.14159265358979323846f;
inline constexpr float KAPPA90 = 0.5522847493f; // Length proportional to radius of cubic bezier handle for 90deg arcs

// -------------------------------------------------------------
// Enums
// -------------------------------------------------------------
enum class Winding : int {
	CounterClockwise = 1, // Default winding for solid shapes
	Clockwise = 2,        // Winding for holes
	Hole = 2,             // Alias for Clockwise
	Preserve = 3,         // Preserve path subpath relative orientations (holes stay opposite outer)
	EvenOdd = 4           // Even-odd parity fill rule
};

enum class LineCap : int {
	Butt = 0,
	Round = 1,
	Square = 2
};

enum class LineJoin : int {
	Miter = 0,
	Round = 1,
	Bevel = 2
};

enum class Solidity : int {
	Solid = 1, // CCW
	Hole = 2   // CW
};

// -------------------------------------------------------------
// Sovereign GPU Path Commands
// -------------------------------------------------------------
enum class PathCommandType : uint8_t {
	MoveTo,
	LineTo,
	BezierTo,
	Close,
	Winding
};

struct PathCommand {
	PathCommandType type{PathCommandType::MoveTo};
	Point p0{0.0f, 0.0f};
	Point p1{0.0f, 0.0f};
	Point p2{0.0f, 0.0f};
	Winding winding{Winding::CounterClockwise};

	static constexpr PathCommand makeMoveTo(Point p) noexcept {
		return PathCommand{PathCommandType::MoveTo, p, Point(), Point(), Winding::CounterClockwise};
	}
	static constexpr PathCommand makeLineTo(Point p) noexcept {
		return PathCommand{PathCommandType::LineTo, p, Point(), Point(), Winding::CounterClockwise};
	}
	static constexpr PathCommand makeBezierTo(Point cp1, Point cp2, Point ep) noexcept {
		return PathCommand{PathCommandType::BezierTo, cp1, cp2, ep, Winding::CounterClockwise};
	}
	static constexpr PathCommand makeClose() noexcept {
		return PathCommand{PathCommandType::Close, Point(), Point(), Point(), Winding::CounterClockwise};
	}
	static constexpr PathCommand makeWinding(Winding w) noexcept {
		return PathCommand{PathCommandType::Winding, Point(), Point(), Point(), w};
	}
};

enum class Align : int {
	// Horizontal
	Left     = 1 << 0,
	Center   = 1 << 1,
	Right    = 1 << 2,
	// Vertical
	Top      = 1 << 3,
	Middle   = 1 << 4,
	Bottom   = 1 << 5,
	Baseline = 1 << 6
};

inline constexpr Align operator|(Align a, Align b) {
	return static_cast<Align>(static_cast<int>(a) | static_cast<int>(b));
}
inline constexpr Align operator&(Align a, Align b) {
	return static_cast<Align>(static_cast<int>(a) & static_cast<int>(b));
}
inline constexpr bool hasFlag(Align a, Align b) {
	return (static_cast<int>(a) & static_cast<int>(b)) != 0;
}

enum class BlendFactor : int {
	Zero = 1 << 0,
	One = 1 << 1,
	SrcColor = 1 << 2,
	OneMinusSrcColor = 1 << 3,
	DstColor = 1 << 4,
	OneMinusDstColor = 1 << 5,
	SrcAlpha = 1 << 6,
	OneMinusSrcAlpha = 1 << 7,
	DstAlpha = 1 << 8,
	OneMinusDstAlpha = 1 << 9,
	SrcAlphaSaturate = 1 << 10
};

enum class CompositeOperation : int {
	SourceOver,
	SourceIn,
	SourceOut,
	Atop,
	DestinationOver,
	DestinationIn,
	DestinationOut,
	DestinationAtop,
	Lighter,
	Copy,
	Xor
};

struct CompositeOperationState {
	BlendFactor srcRGB{BlendFactor::One};
	BlendFactor dstRGB{BlendFactor::OneMinusSrcAlpha};
	BlendFactor srcAlpha{BlendFactor::One};
	BlendFactor dstAlpha{BlendFactor::OneMinusSrcAlpha};
	BlendMode blendMode{BlendMode::SourceOver};
	bool isProgrammableBlend{false};

	static CompositeOperationState fromCompositeOp(CompositeOperation op);
	static CompositeOperationState fromBlendMode(BlendMode mode) noexcept;
};

enum ImageFlags : int {
	ImageGenerateMipmaps = 1 << 0,
	ImageRepeatX         = 1 << 1,
	ImageRepeatY         = 1 << 2,
	ImageFlipY           = 1 << 3,
	ImagePremultiplied   = 1 << 4,
	ImageNearest         = 1 << 5,
	ImageForceBilinear   = 1 << 6,
	ImageForceNearest    = 1 << 7,
	ImageMirrorX         = 1 << 8,
	ImageMirrorY         = 1 << 9
};

enum CreateFlags : int {
	Antialias       = 1 << 0,
	StencilStrokes  = 1 << 1,
	Debug           = 1 << 2
};

// -------------------------------------------------------------
// Color (RGBA Float [0.0 - 1.0])
// -------------------------------------------------------------
struct Color {
	float r{0.0f};
	float g{0.0f};
	float b{0.0f};
	float a{1.0f};

	constexpr Color() = default;
	constexpr Color(float red, float green, float blue, float alpha = 1.0f)
		: r(red), g(green), b(blue), a(alpha) {}

	constexpr Color(const nisaba::Color& c) noexcept
		: r(c.red()), g(c.green()), b(c.blue()), a(c.alpha()) {}

	constexpr operator nisaba::Color() const noexcept {
		return nisaba::Color::from_rgba_unchecked(r, g, b, a);
	}

	static constexpr Color rgb(uint8_t red, uint8_t green, uint8_t blue) {
		return Color(red / 255.0f, green / 255.0f, blue / 255.0f, 1.0f);
	}

	static constexpr Color rgba(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
		return Color(red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
	}

	static constexpr Color rgbaf(float r, float g, float b, float a) {
		return Color(r, g, b, a);
	}

	static Color hsl(float h, float s, float l, float a = 1.0f);
	static Color hsv(float h, float s, float v, float a = 1.0f);

	Color lerp(const Color& other, float u) const {
		u = std::clamp(u, 0.0f, 1.0f);
		return Color(
			r + (other.r - r) * u,
			g + (other.g - g) * u,
			b + (other.b - b) * u,
			a + (other.a - a) * u
		);
	}

	Color withAlpha(float newAlpha) const {
		return Color(r, g, b, newAlpha);
	}

	Color premultiplied() const {
		return Color(r * a, g * a, b * a, a);
	}

	constexpr uint32_t toRGBA8() const {
		uint32_t ur = static_cast<uint32_t>(std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f));
		uint32_t ug = static_cast<uint32_t>(std::clamp(g * 255.0f + 0.5f, 0.0f, 255.0f));
		uint32_t ub = static_cast<uint32_t>(std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f));
		uint32_t ua = static_cast<uint32_t>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
		return (ua << 24) | (ub << 16) | (ug << 8) | ur;
	}
};

// -------------------------------------------------------------
// Point & Rect
// -------------------------------------------------------------
using Point = nisaba::Point;

struct Rect {
	float x{0.0f};
	float y{0.0f};
	float width{0.0f};
	float height{0.0f};

	constexpr Rect() = default;
	constexpr Rect(float x_, float y_, float w_, float h_)
		: x(x_), y(y_), width(w_), height(h_) {}

	constexpr Rect(const nisaba::Rect& r) noexcept
		: x(r.x()), y(r.y()), width(r.width()), height(r.height()) {}

	operator nisaba::Rect() const noexcept {
		auto opt = nisaba::Rect::from_xywh(x, y, width, height);
		return opt.value_or(nisaba::Rect());
	}

	constexpr float minX() const { return x; }
	constexpr float minY() const { return y; }
	constexpr float maxX() const { return x + width; }
	constexpr float maxY() const { return y + height; }
	constexpr bool isEmpty() const { return width <= 0.0f || height <= 0.0f; }

	bool contains(float px, float py) const {
		return px >= x && px <= x + width && py >= y && py <= y + height;
	}
};

// -------------------------------------------------------------
// Transform2D (2x3 Affine Matrix)
// | m[0]  m[2]  m[4] |   | x |   | m[0]*x + m[2]*y + m[4] |
// | m[1]  m[3]  m[5] | * | y | = | m[1]*x + m[3]*y + m[5] |
// |  0     0     1   |   | 1 |   |           1            |
// -------------------------------------------------------------
struct Transform2D {
	std::array<float, 6> m{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

	constexpr Transform2D() = default;
	constexpr explicit Transform2D(const std::array<float, 6>& values) : m(values) {}
	constexpr Transform2D(float m0, float m1, float m2, float m3, float m4, float m5)
		: m{m0, m1, m2, m3, m4, m5} {}

	constexpr explicit Transform2D(const nisaba::Transform& t) noexcept
		: m{t.sx, t.ky, t.kx, t.sy, t.tx, t.ty} {}

	constexpr operator nisaba::Transform() const noexcept {
		return nisaba::Transform(m[0], m[1], m[2], m[3], m[4], m[5]);
	}

	constexpr float operator[](size_t idx) const { return m[idx]; }
	constexpr float& operator[](size_t idx) { return m[idx]; }

	static constexpr Transform2D identity() {
		return Transform2D(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	}

	constexpr bool isIdentity() const noexcept {
		return m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f &&
		       m[3] == 1.0f && m[4] == 0.0f && m[5] == 0.0f;
	}

	static Transform2D translate(float tx, float ty) {
		return Transform2D(1.0f, 0.0f, 0.0f, 1.0f, tx, ty);
	}

	static Transform2D scale(float sx, float sy) {
		return Transform2D(sx, 0.0f, 0.0f, sy, 0.0f, 0.0f);
	}

	static Transform2D rotate(float angleRad) {
		float cs = std::cos(angleRad);
		float sn = std::sin(angleRad);
		return Transform2D(cs, sn, -sn, cs, 0.0f, 0.0f);
	}

	static Transform2D skewX(float angleRad) {
		return Transform2D(1.0f, 0.0f, std::tan(angleRad), 1.0f, 0.0f, 0.0f);
	}

	static Transform2D skewY(float angleRad) {
		return Transform2D(1.0f, std::tan(angleRad), 0.0f, 1.0f, 0.0f, 0.0f);
	}

	Transform2D multiply(const Transform2D& src) const;
	Transform2D premultiply(const Transform2D& src) const;
	bool inverse(Transform2D& outInverse) const;

	void toMat3x4(float* m3) const noexcept {
		m3[0]  = m[0];
		m3[1]  = m[1];
		m3[2]  = 0.0f;
		m3[3]  = 0.0f;
		m3[4]  = m[2];
		m3[5]  = m[3];
		m3[6]  = 0.0f;
		m3[7]  = 0.0f;
		m3[8]  = m[4];
		m3[9]  = m[5];
		m3[10] = 1.0f;
		m3[11] = 0.0f;
	}

	Point transformPoint(float px, float py) const {
		return Point(
			px * m[0] + py * m[2] + m[4],
			px * m[1] + py * m[3] + m[5]
		);
	}

	float averageScale() const {
		float sx = std::sqrt(m[0] * m[0] + m[1] * m[1]);
		float sy = std::sqrt(m[2] * m[2] + m[3] * m[3]);
		return (sx + sy) * 0.5f;
	}
};

// -------------------------------------------------------------
// Scissor Clipping
// -------------------------------------------------------------
struct Scissor {
	Transform2D xform{Transform2D::identity()};
	float extent[2]{-1.0f, -1.0f}; // extent[0] < 0 means scissor is disabled
};

struct LayerDesc {
	bool hasBounds{false};
	float bounds[4]{0.0f, 0.0f, 0.0f, 0.0f}; // device-space AABB [x0, y0, x1, y1]
	bool copyBackdrop{false};
	float backdropBlurSigma{0.0f};
	float opacity{1.0f};
	CompositeOperationState compOp{};
	bool hasColorMatrix{false};
	std::array<float, 20> colorMatrix{};
	uint8_t colorTransfer{0}; // 0 = None, 1 = SrgbToLinear, 2 = LinearToSrgb
	bool hasColorBlend{false};
	Color colorBlendColor{0.0f, 0.0f, 0.0f, 0.0f};
	BlendMode colorBlendMode{BlendMode::SourceOver};
	float postBlurSigmaX{0.0f};
	float postBlurSigmaY{0.0f};
	bool isDropShadow{false};
	float shadowDx{0.0f};
	float shadowDy{0.0f};
	Color shadowColor{0.0f, 0.0f, 0.0f, 0.0f};
	bool shadowOnly{false};
	std::optional<effects::ImageFilter> imageFilter{std::nullopt};
	std::optional<effects::MaskFilter> maskFilter{std::nullopt};
	std::optional<effects::ColorEffect> extColorEffect{std::nullopt};
	std::optional<effects::Blender> blender{std::nullopt};
	Color maskTintColor{1.0f, 1.0f, 1.0f, 1.0f};
	Transform2D layerTransform{Transform2D::identity()};
};

// -------------------------------------------------------------
// Paint (Gradients, Solid Colors, Image Patterns)
// -------------------------------------------------------------
enum class GradientKind : uint8_t {
	None = 0,
	Linear = 1,
	Radial = 2,
	Sweep = 3,
	TwoPointConical = 4,
	PerlinFractal = 5,
	PerlinTurbulence = 6
};

enum class SpreadKind : uint8_t {
	Pad = 0,
	Repeat = 1,
	Reflect = 2
};

struct GradientStopGPU {
	float position{0.0f};
	Color color{};
};

struct Paint {
	Transform2D xform{Transform2D::identity()};
	Transform2D gradUnitXform{Transform2D::identity()};
	float extent[2]{0.0f, 0.0f};
	float radius{0.0f};
	float feather{0.0f};
	Color innerColor{0.0f, 0.0f, 0.0f, 1.0f};
	Color outerColor{0.0f, 0.0f, 0.0f, 1.0f};
	int image{0};
	bool hasColorMatrix{false};
	std::array<float, 20> colorMatrix{};
	uint8_t colorTransfer{0}; // 0 = None, 1 = SrgbToLinear, 2 = LinearToSrgb
	bool hasColorBlend{false};
	Color colorBlendColor{0.0f, 0.0f, 0.0f, 0.0f};
	BlendMode colorBlendMode{BlendMode::SourceOver};
	GradientKind gradientKind{GradientKind::None};
	SpreadKind gradientSpread{SpreadKind::Pad};
	int gradStopCount{0}; // 0..16 inline stops, -1 = 256x1 RGBA LUT in `image`
	std::array<float, 4> gradParams0{};
	std::array<float, 4> gradParams1{};
	std::array<GradientStopGPU, 16> gradStops{};
	std::shared_ptr<nisaba::gpu::Shader> customShader{nullptr};

	[[nodiscard]] bool hasAdvancedGradient() const noexcept {
		return gradientKind != GradientKind::None;
	}

	static Paint color(Color col) {
		Paint p;
		p.innerColor = col;
		p.outerColor = col;
		return p;
	}

	static Paint custom(std::shared_ptr<nisaba::gpu::Shader> sh) {
		Paint p;
		p.customShader = std::move(sh);
		return p;
	}

	static Paint linearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	static Paint boxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
	static Paint radialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	static Paint imagePattern(float cx, float cy, float w, float h, float angleRad, int image, float alpha);
};

// -------------------------------------------------------------
// Text Metrics & Positions
// -------------------------------------------------------------
struct TextRow {
	const char* start{nullptr};
	const char* end{nullptr};
	const char* next{nullptr};
	float width{0.0f};
	float minx{0.0f};
	float maxx{0.0f};
};

struct GlyphPosition {
	const char* str{nullptr};
	float x{0.0f};
	float minx{0.0f};
	float maxx{0.0f};
};

} // namespace nisaba::gpu

