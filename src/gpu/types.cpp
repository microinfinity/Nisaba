#include "nisaba/gpu/types.hpp"
#include <cmath>

namespace nisaba::gpu {

// -------------------------------------------------------------
// CompositeOperationState
// -------------------------------------------------------------
CompositeOperationState CompositeOperationState::fromCompositeOp(CompositeOperation op) {
	BlendFactor sfactor = BlendFactor::One;
	BlendFactor dfactor = BlendFactor::Zero;
	BlendMode mode = BlendMode::SourceOver;

	switch (op) {
		case CompositeOperation::SourceOver:
			sfactor = BlendFactor::One;
			dfactor = BlendFactor::OneMinusSrcAlpha;
			mode = BlendMode::SourceOver;
			break;
		case CompositeOperation::SourceIn:
			sfactor = BlendFactor::DstAlpha;
			dfactor = BlendFactor::Zero;
			mode = BlendMode::SourceIn;
			break;
		case CompositeOperation::SourceOut:
			sfactor = BlendFactor::OneMinusDstAlpha;
			dfactor = BlendFactor::Zero;
			mode = BlendMode::SourceOut;
			break;
		case CompositeOperation::Atop:
			sfactor = BlendFactor::DstAlpha;
			dfactor = BlendFactor::OneMinusSrcAlpha;
			mode = BlendMode::SourceAtop;
			break;
		case CompositeOperation::DestinationOver:
			sfactor = BlendFactor::OneMinusDstAlpha;
			dfactor = BlendFactor::One;
			mode = BlendMode::DestinationOver;
			break;
		case CompositeOperation::DestinationIn:
			sfactor = BlendFactor::Zero;
			dfactor = BlendFactor::SrcAlpha;
			mode = BlendMode::DestinationIn;
			break;
		case CompositeOperation::DestinationOut:
			sfactor = BlendFactor::Zero;
			dfactor = BlendFactor::OneMinusSrcAlpha;
			mode = BlendMode::DestinationOut;
			break;
		case CompositeOperation::DestinationAtop:
			sfactor = BlendFactor::OneMinusDstAlpha;
			dfactor = BlendFactor::SrcAlpha;
			mode = BlendMode::DestinationAtop;
			break;
		case CompositeOperation::Lighter:
			sfactor = BlendFactor::One;
			dfactor = BlendFactor::One;
			mode = BlendMode::Plus;
			break;
		case CompositeOperation::Copy:
			sfactor = BlendFactor::One;
			dfactor = BlendFactor::Zero;
			mode = BlendMode::Source;
			break;
		case CompositeOperation::Xor:
			sfactor = BlendFactor::OneMinusDstAlpha;
			dfactor = BlendFactor::OneMinusSrcAlpha;
			mode = BlendMode::Xor;
			break;
	}

	CompositeOperationState state;
	state.srcRGB = sfactor;
	state.dstRGB = dfactor;
	state.srcAlpha = sfactor;
	state.dstAlpha = dfactor;
	state.blendMode = mode;
	state.isProgrammableBlend = false;
	return state;
}

CompositeOperationState CompositeOperationState::fromBlendMode(BlendMode mode) noexcept {
	CompositeOperationState state;
	state.blendMode = mode;
	switch (mode) {
		case BlendMode::Destination:
			state.srcRGB = state.srcAlpha = BlendFactor::Zero;
			state.dstRGB = state.dstAlpha = BlendFactor::One;
			return state;
		case BlendMode::SourceOver:
			state.srcRGB = state.srcAlpha = BlendFactor::One;
			state.dstRGB = state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
			return state;
		case BlendMode::DestinationOver:
			state.srcRGB = state.srcAlpha = BlendFactor::OneMinusDstAlpha;
			state.dstRGB = state.dstAlpha = BlendFactor::One;
			return state;
		case BlendMode::Plus:
			state.srcRGB = state.srcAlpha = BlendFactor::One;
			state.dstRGB = state.dstAlpha = BlendFactor::One;
			return state;
		case BlendMode::DestinationOut:
			state.srcRGB = state.srcAlpha = BlendFactor::Zero;
			state.dstRGB = state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
			return state;
		case BlendMode::SourceAtop:
			state.srcRGB = state.srcAlpha = BlendFactor::DstAlpha;
			state.dstRGB = state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
			return state;
		case BlendMode::Xor:
			state.srcRGB = state.srcAlpha = BlendFactor::OneMinusDstAlpha;
			state.dstRGB = state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
			return state;
		case BlendMode::Screen:
			state.srcRGB = BlendFactor::One;
			state.dstRGB = BlendFactor::OneMinusSrcColor;
			state.srcAlpha = BlendFactor::One;
			state.dstAlpha = BlendFactor::OneMinusSrcAlpha;
			return state;
		default:
			// Remaining Porter-Duff (coverage != pre-scaled) & all advanced modes
			state.srcRGB = state.srcAlpha = BlendFactor::One;
			state.dstRGB = state.dstAlpha = BlendFactor::Zero;
			state.isProgrammableBlend = true;
			return state;
	}
}

// -------------------------------------------------------------
// Color HSL & HSV
// -------------------------------------------------------------
static float hueHelper(float h, float m1, float m2) {
	if (h < 0.0f) h += 1.0f;
	if (h > 1.0f) h -= 1.0f;
	if (h < 1.0f / 6.0f) return m1 + (m2 - m1) * h * 6.0f;
	if (h < 3.0f / 6.0f) return m2;
	if (h < 4.0f / 6.0f) return m1 + (m2 - m1) * (2.0f / 3.0f - h) * 6.0f;
	return m1;
}

Color Color::hsl(float h, float s, float l, float a) {
	h = std::fmod(h, 1.0f);
	if (h < 0.0f) h += 1.0f;
	s = std::clamp(s, 0.0f, 1.0f);
	l = std::clamp(l, 0.0f, 1.0f);
	float m2 = l <= 0.5f ? (l * (1.0f + s)) : (l + s - l * s);
	float m1 = 2.0f * l - m2;
	return Color(
		std::clamp(hueHelper(h + 1.0f / 3.0f, m1, m2), 0.0f, 1.0f),
		std::clamp(hueHelper(h, m1, m2), 0.0f, 1.0f),
		std::clamp(hueHelper(h - 1.0f / 3.0f, m1, m2), 0.0f, 1.0f),
		a
	);
}

Color Color::hsv(float h, float s, float v, float a) {
	h = std::fmod(h, 360.0f);
	if (h < 0.0f) h += 360.0f;
	s = std::clamp(s, 0.0f, 1.0f);
	v = std::clamp(v, 0.0f, 1.0f);
	int i = static_cast<int>(h / 60.0f);
	float f = (h / 60.0f) - i;
	float p = v * (1.0f - s);
	float q = v * (1.0f - s * f);
	float t = v * (1.0f - s * (1.0f - f));
	switch (i) {
		case 0: return Color(v, t, p, a);
		case 1: return Color(q, v, p, a);
		case 2: return Color(p, v, t, a);
		case 3: return Color(p, q, v, a);
		case 4: return Color(t, p, v, a);
		default: return Color(v, p, q, a);
	}
}

// -------------------------------------------------------------
// Transform2D Math
// -------------------------------------------------------------
Transform2D Transform2D::multiply(const Transform2D& s) const {
	Transform2D res;
	res.m[0] = m[0] * s.m[0] + m[1] * s.m[2];
	res.m[1] = m[0] * s.m[1] + m[1] * s.m[3];
	res.m[2] = m[2] * s.m[0] + m[3] * s.m[2];
	res.m[3] = m[2] * s.m[1] + m[3] * s.m[3];
	res.m[4] = m[4] * s.m[0] + m[5] * s.m[2] + s.m[4];
	res.m[5] = m[4] * s.m[1] + m[5] * s.m[3] + s.m[5];
	return res;
}

Transform2D Transform2D::premultiply(const Transform2D& s) const {
	return s.multiply(*this);
}

bool Transform2D::inverse(Transform2D& inv) const {
	double det = static_cast<double>(m[0]) * m[3] - static_cast<double>(m[2]) * m[1];
	if (det > -1e-6 && det < 1e-6) {
		inv = Transform2D::identity();
		return false;
	}
	double invdet = 1.0 / det;
	inv.m[0] = static_cast<float>(m[3] * invdet);
	inv.m[2] = static_cast<float>(-m[2] * invdet);
	inv.m[4] = static_cast<float>((static_cast<double>(m[2]) * m[5] - static_cast<double>(m[3]) * m[4]) * invdet);
	inv.m[1] = static_cast<float>(-m[1] * invdet);
	inv.m[3] = static_cast<float>(m[0] * invdet);
	inv.m[5] = static_cast<float>((static_cast<double>(m[1]) * m[4] - static_cast<double>(m[0]) * m[5]) * invdet);
	return true;
}

// -------------------------------------------------------------
// Paint Helpers
// -------------------------------------------------------------
Paint Paint::linearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol) {
	Paint p;
	float dx = ex - sx;
	float dy = ey - sy;
	float d = std::sqrt(dx * dx + dy * dy);
	if (d > 0.0001f) {
		dx /= d;
		dy /= d;
	} else {
		dx = 0.0f;
		dy = 1.0f;
	}
	p.xform.m[0] = dy;
	p.xform.m[1] = -dx;
	p.xform.m[2] = dx;
	p.xform.m[3] = dy;
	p.xform.m[4] = sx;
	p.xform.m[5] = sy;
	p.extent[0] = 0.0f;
	p.extent[1] = d;
	p.radius = 0.0f;
	p.feather = std::max(1.0f, d);
	p.innerColor = icol;
	p.outerColor = ocol;
	return p;
}

Paint Paint::radialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol) {
	Paint p;
	float r = (inr + outr) * 0.5f;
	float f = (outr - inr);
	p.xform = Transform2D::identity();
	p.xform.m[4] = cx;
	p.xform.m[5] = cy;
	p.extent[0] = r;
	p.extent[1] = r;
	p.radius = r;
	p.feather = std::max(1.0f, f);
	p.innerColor = icol;
	p.outerColor = ocol;
	return p;
}

Paint Paint::boxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol) {
	Paint p;
	p.xform = Transform2D::identity();
	p.xform.m[4] = x + w * 0.5f;
	p.xform.m[5] = y + h * 0.5f;
	p.extent[0] = w * 0.5f;
	p.extent[1] = h * 0.5f;
	p.radius = r;
	p.feather = std::max(1.0f, f);
	p.innerColor = icol;
	p.outerColor = ocol;
	return p;
}

Paint Paint::imagePattern(float cx, float cy, float w, float h, float angleRad, int image, float alpha) {
	Paint p;
	p.xform = Transform2D::rotate(angleRad);
	p.xform.m[4] = cx;
	p.xform.m[5] = cy;
	p.extent[0] = w;
	p.extent[1] = h;
	p.image = image;
	p.innerColor = Color::rgbaf(1.0f, 1.0f, 1.0f, alpha);
	p.outerColor = Color::rgbaf(1.0f, 1.0f, 1.0f, alpha);
	return p;
}

} // namespace nisaba::gpu
