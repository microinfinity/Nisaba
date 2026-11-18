#pragma once

#include "nisaba/gpu/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>

namespace nisaba::gpu {

enum ShaderType {
	ShaderFillGrad       = 0,
	ShaderFillImg        = 1,
	ShaderSimple         = 2,
	ShaderImg            = 3,
	ShaderSolid          = 4,
	ShaderLinearGrad     = 5,
	ShaderRadialGrad     = 6,
	ShaderCircle         = 7,
	ShaderRect           = 8,
	ShaderRRect          = 9,
	ShaderRing           = 10,
	ShaderRRectGradient  = 11,
	ShaderMultiGrad      = 12,
	ShaderRRectShadow    = 13,
	ShaderPerspectiveImg = 14,
	ShaderDRRect         = 15,
	MaxShaders           = 16
};

// std140 layout shared identically by OpenGL GLSL and Vulkan SPIR-V NisabaShaderParams
struct NisabaShaderUniforms {
	float scissorMat[12]; // 3 x vec4
	float paintMat[12];   // 3 x vec4
	Color innerCol;       // 4 floats (premultiplied)
	Color outerCol;       // 4 floats (premultiplied)
	float scissorExt[2];
	float scissorScale[2];
	float extent[2];
	float radius;
	float feather;
	float strokeMult;
	float strokeThr;
	int texType;
	int type;
	int blendMode;        // 0 = fixed-function hardware, >0 = programmable BlendMode + 1
	int hasColorMatrix;   // 0 = disabled, 1 = active 4x5 color matrix
	int gradKind;         // 0 = none, 1 = linear, 2 = radial, 3 = sweep, 4 = conical
	int spreadMode;       // 0 = pad, 1 = repeat, 2 = reflect
	float colorMat0[4];   // row 0: [m0, m1, m2, m3]
	float colorMat1[4];   // row 1: [m5, m6, m7, m8]
	float colorMat2[4];   // row 2: [m10, m11, m12, m13]
	float colorMat3[4];   // row 3: [m15, m16, m17, m18]
	float colorMatBias[4];// bias:  [m4, m9, m14, m19]
	int gradStopCount;    // 0..16 inline stops, -1 = 256x1 LUT in `tex`
	int colorTransfer;    // 0 = none, 1 = srgb_to_linear, 2 = linear_to_srgb
	int colorBlendEnabled;// 0 = disabled, 1 = active blend color filter
	int colorBlendMode;   // BlendMode + 1
	float gradParams0[4];
	float gradParams1[4];
	float gradStopPos[4][4];
	Color gradStopColors[16];
	Color colorBlendCol;  // premultiplied blend filter color
};

static_assert(sizeof(NisabaShaderUniforms) == 656, "NisabaShaderUniforms must match std140 UBO size");

inline constexpr size_t kFastUniformBytes = 176;
static_assert(offsetof(NisabaShaderUniforms, blendMode) == kFastUniformBytes,
              "Fast uniform prefix must match first 176 bytes of NisabaShaderUniforms");

using FragUniforms = NisabaShaderUniforms;

[[nodiscard]] inline bool paintUsesFullEffects(const Paint& paint, int blendModeId, int shaderType = -1) noexcept {
	return blendModeId != 0 || paint.hasColorMatrix || paint.colorTransfer != 0 ||
	       paint.hasColorBlend || paint.gradientKind != GradientKind::None ||
	       shaderType == 16 || shaderType == 17 ||
	       shaderType == ShaderMultiGrad || shaderType == ShaderRRectShadow ||
	       shaderType == ShaderPerspectiveImg;
}

[[nodiscard]] inline bool uniformUsesFullEffectsShader(const FragUniforms& f) noexcept {
	return f.blendMode != 0 || f.hasColorMatrix != 0 || f.colorTransfer != 0 ||
	       f.colorBlendEnabled != 0 || f.type == ShaderPerspectiveImg;
}

struct TextureUniformInfo {
	TextureType type{TextureType::RGBA};
	int flags{0};
};

inline void xformToMat3x4(float* m3, const Transform2D& t) {
	t.toMat3x4(m3);
}

/// Computes normalized [0, 1]^2 UV transform for an image pattern, applying ImageFlipY around extent.y.
[[nodiscard]] inline Transform2D computeImageUvTransform(const Transform2D& paintXform,
                                                         float extX,
                                                         float extY,
                                                         bool flipY) {
	Transform2D invxform;
	if (flipY) {
		Transform2D m1 = Transform2D::translate(0.0f, extY * 0.5f).multiply(paintXform);
		Transform2D m2 = Transform2D::scale(1.0f, -1.0f).multiply(m1);
		Transform2D m3 = Transform2D::translate(0.0f, -extY * 0.5f).multiply(m2);
		m3.inverse(invxform);
	} else {
		paintXform.inverse(invxform);
	}
	const float invExtX = (extX > 1e-4f) ? (1.0f / extX) : 1.0f;
	const float invExtY = (extY > 1e-4f) ? (1.0f / extY) : 1.0f;
	return invxform.multiply(Transform2D::scale(invExtX, invExtY));
}

template <typename LookupTexFn>
inline bool convertPaintUniforms(FragUniforms* frag,
                                 const Paint& paint,
                                 const Scissor& scissor,
                                 float width,
                                 float fringe,
                                 float strokeThr,
                                 bool hasHwScissor,
                                 int blendModeId,
                                 LookupTexFn&& lookupTex,
                                 bool fastPrefixOnly = false) {
	if (fastPrefixOnly) {
		std::memset(static_cast<void*>(frag), 0, kFastUniformBytes);
		// Keep tail predicate fields queried by uniformUsesFullEffectsShader() initialized
		// without zeroing all 656 bytes for fast-bucket draws.
		frag->blendMode = 0;
		frag->hasColorMatrix = 0;
		frag->colorTransfer = 0;
		frag->colorBlendEnabled = 0;
	} else {
		std::memset(static_cast<void*>(frag), 0, sizeof(FragUniforms));
		frag->blendMode = blendModeId;
		if (paint.hasColorMatrix) {
			frag->hasColorMatrix = 1;
			const auto& m = paint.colorMatrix;
			frag->colorMat0[0] = m[0];  frag->colorMat0[1] = m[1];  frag->colorMat0[2] = m[2];  frag->colorMat0[3] = m[3];
			frag->colorMat1[0] = m[5];  frag->colorMat1[1] = m[6];  frag->colorMat1[2] = m[7];  frag->colorMat1[3] = m[8];
			frag->colorMat2[0] = m[10]; frag->colorMat2[1] = m[11]; frag->colorMat2[2] = m[12]; frag->colorMat2[3] = m[13];
			frag->colorMat3[0] = m[15]; frag->colorMat3[1] = m[16]; frag->colorMat3[2] = m[17]; frag->colorMat3[3] = m[18];
			frag->colorMatBias[0] = m[4];
			frag->colorMatBias[1] = m[9];
			frag->colorMatBias[2] = m[14];
			frag->colorMatBias[3] = m[19];
		}
		frag->colorTransfer = static_cast<int>(paint.colorTransfer);
		frag->colorBlendEnabled = paint.hasColorBlend ? 1 : 0;
		frag->colorBlendMode = paint.hasColorBlend ? (static_cast<int>(paint.colorBlendMode) + 1) : 0;
		frag->colorBlendCol = paint.colorBlendColor.premultiplied();
	}

	frag->innerCol = paint.innerColor.premultiplied();
	frag->outerCol = paint.outerColor.premultiplied();

	const float effFringe = (fringe > 1e-4f) ? fringe : 1.0f;
	if (hasHwScissor || scissor.extent[0] < -0.5f || scissor.extent[1] < -0.5f) {
		std::memset(frag->scissorMat, 0, sizeof(frag->scissorMat));
		frag->scissorExt[0] = -1.0f;
		frag->scissorExt[1] = -1.0f;
		frag->scissorScale[0] = 1.0f;
		frag->scissorScale[1] = 1.0f;
	} else {
		Transform2D invxform;
		scissor.xform.inverse(invxform);
		xformToMat3x4(frag->scissorMat, invxform);
		frag->scissorExt[0] = scissor.extent[0];
		frag->scissorExt[1] = scissor.extent[1];
		frag->scissorScale[0] = std::sqrt(scissor.xform[0] * scissor.xform[0] + scissor.xform[2] * scissor.xform[2]) / effFringe;
		frag->scissorScale[1] = std::sqrt(scissor.xform[1] * scissor.xform[1] + scissor.xform[3] * scissor.xform[3]) / effFringe;
	}

	frag->extent[0] = paint.extent[0];
	frag->extent[1] = paint.extent[1];
	frag->strokeMult = (fringe > 1e-4f) ? ((width * 0.5f + fringe * 0.5f) / fringe) : 1.0f;
	frag->strokeThr = strokeThr;

	Transform2D invxform;
	if (paint.gradientKind != GradientKind::None) {
		frag->type = ShaderMultiGrad;
		frag->gradKind = static_cast<int>(paint.gradientKind);
		frag->spreadMode = static_cast<int>(paint.gradientSpread);
		frag->gradStopCount = paint.gradStopCount;
		for (int i = 0; i < 4; ++i) {
			frag->gradParams0[i] = paint.gradParams0[i];
			frag->gradParams1[i] = paint.gradParams1[i];
			for (int j = 0; j < 4; ++j) frag->gradStopPos[i][j] = 0.0f;
		}
		const int sc = std::clamp(paint.gradStopCount, 0, 16);
		for (int i = 0; i < sc; ++i) {
			frag->gradStopPos[i >> 2][i & 3] = paint.gradStops[i].position;
			frag->gradStopColors[i] = paint.gradStops[i].color;
		}
		paint.xform.inverse(invxform);
		Transform2D gradXform = invxform.multiply(paint.gradUnitXform);
		xformToMat3x4(frag->paintMat, gradXform);
	} else if (paint.image != 0) {
		std::optional<TextureUniformInfo> tex = lookupTex(paint.image);
		if (!tex.has_value()) return false;

		frag->type = ShaderFillImg;

		if (tex->type == TextureType::RGBA || tex->type == TextureType::BGRA) {
			frag->texType = (tex->flags & ImageFlags::ImagePremultiplied) ? 0 : 1;
		} else {
			frag->texType = 2;
		}
		const bool flipY = (tex->flags & ImageFlags::ImageFlipY) != 0;
		const Transform2D uvXform = computeImageUvTransform(paint.xform, frag->extent[0], frag->extent[1], flipY);
		xformToMat3x4(frag->paintMat, uvXform);
	} else if (paint.extent[0] == 0.0f && paint.extent[1] == 0.0f && paint.radius == 0.0f &&
	           paint.innerColor.r == paint.outerColor.r &&
	           paint.innerColor.g == paint.outerColor.g &&
	           paint.innerColor.b == paint.outerColor.b &&
	           paint.innerColor.a == paint.outerColor.a) {
		frag->type = ShaderSolid;
		frag->radius = 0.0f;
		frag->feather = 1.0f;
		frag->innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		frag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
	} else if (paint.radius == 0.0f && paint.extent[0] == 0.0f) {
		frag->type = ShaderLinearGrad;
		frag->radius = 0.0f;
		frag->feather = paint.feather;
		paint.xform.inverse(invxform);
		float invFeather = (paint.feather > 1e-4f) ? (1.0f / paint.feather) : 1.0f;
		Transform2D gradXform = invxform.multiply(Transform2D::scale(1.0f, invFeather));
		xformToMat3x4(frag->paintMat, gradXform);
	} else if (paint.radius > 0.0f && paint.extent[0] == paint.radius && paint.extent[1] == paint.radius) {
		frag->type = ShaderRadialGrad;
		frag->radius = paint.radius;
		frag->feather = paint.feather;
		float invFeather = (paint.feather > 1e-4f) ? (1.0f / paint.feather) : 1.0f;
		frag->extent[0] = invFeather;
		frag->extent[1] = 0.5f - paint.radius * invFeather;
		paint.xform.inverse(invxform);
		xformToMat3x4(frag->paintMat, invxform);
	} else {
		frag->type = ShaderFillGrad;
		frag->radius = paint.radius;
		frag->feather = paint.feather;
		paint.xform.inverse(invxform);
		xformToMat3x4(frag->paintMat, invxform);
	}

	return true;
}

template <typename LookupTexFn>
inline void applyTriangleShaderUniforms(FragUniforms* tempFrag,
                                        const Paint& paint,
                                        const Scissor& scissor,
                                        float fringe,
                                        bool hasHwScissor,
                                        int blendModeId,
                                        int shaderType,
                                        LookupTexFn&& lookupTex,
                                        bool fastPrefixOnly = false) {
	const float widthForMult = (shaderType == -2) ? paint.feather : 1.0f;
	convertPaintUniforms(tempFrag, paint, scissor, widthForMult, fringe, -1.0f,
	                     hasHwScissor, blendModeId, lookupTex, fastPrefixOnly);

	if (shaderType == -2) {
		tempFrag->type = ShaderSolid;
		tempFrag->radius = 0.0f;
		tempFrag->feather = 1.0f;
		tempFrag->innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		tempFrag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
	} else if (shaderType == ShaderSolid) {
		tempFrag->type = ShaderSolid;
		tempFrag->radius = 0.0f;
		tempFrag->feather = 1.0f;
		tempFrag->strokeMult = 1.0f;
	} else if (shaderType == 2) {
		tempFrag->type = ShaderSimple;
		tempFrag->innerCol = paint.innerColor.premultiplied();
		tempFrag->outerCol = tempFrag->innerCol;
	} else if (shaderType == 15) {
		// Concentric Circle Radial Gradient (ftcoord in [-Rq, Rq], circle radius in paint.xform[0])
		tempFrag->type = ShaderRadialGrad;
		tempFrag->strokeMult = 4.0f;
		tempFrag->strokeThr = paint.xform[0];
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
	} else if (shaderType == 12) {
		// Vertex-interpolated Radial Gradient (ftcoord = pt)
		tempFrag->type = ShaderRadialGrad;
		tempFrag->strokeMult = 3.0f;
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
	} else if (shaderType == 13) {
		// Vertex-interpolated Box Gradient (ftcoord = pt)
		tempFrag->type = ShaderFillGrad;
		tempFrag->strokeMult = 3.0f;
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
	} else if (shaderType == 14) {
		// Vertex-interpolated Linear Gradient (ftcoord.y = pt.y)
		tempFrag->type = ShaderLinearGrad;
		tempFrag->radius = 1.0f;
		tempFrag->feather = 1.0f;
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
	} else if (shaderType == 16) {
		// Analytical Asymmetric RRect Drop Shadow
		tempFrag->type = ShaderRRectShadow;
		tempFrag->extent[0] = paint.extent[0];
		tempFrag->extent[1] = paint.extent[1];
		tempFrag->radius = paint.radius;
		tempFrag->feather = paint.feather;
		tempFrag->strokeMult = paint.xform[0];
		tempFrag->strokeThr = paint.xform[1];
		tempFrag->gradParams0[0] = paint.gradParams0[0];
		tempFrag->innerCol = paint.innerColor.premultiplied();
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
	} else if (shaderType == 17) {
		// Perspective-Correct Homography Textured Quad
		if (tempFrag->texType == 0 && paint.image != 0) {
			if (auto tex = lookupTex(paint.image); tex && tex->type == TextureType::Alpha) {
				tempFrag->texType = 2;
			}
		}
		tempFrag->type = ShaderPerspectiveImg;
		tempFrag->extent[0] = paint.extent[0];
		tempFrag->extent[1] = paint.extent[1];
		tempFrag->gradParams0[0] = paint.gradStops[0].color.r;
		tempFrag->gradParams0[1] = paint.gradStops[0].color.g;
		tempFrag->gradParams0[2] = paint.gradStops[0].color.b;
		tempFrag->gradParams0[3] = paint.gradStops[0].color.a;
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
		// Pack row-major 3x3 inverse homography (gradParams0, gradParams1, radius) as std140 column-major mat3
		tempFrag->paintMat[0]  = paint.gradParams0[0];
		tempFrag->paintMat[1]  = paint.gradParams0[3];
		tempFrag->paintMat[2]  = paint.gradParams1[2];
		tempFrag->paintMat[4]  = paint.gradParams0[1];
		tempFrag->paintMat[5]  = paint.gradParams1[0];
		tempFrag->paintMat[6]  = paint.gradParams1[3];
		tempFrag->paintMat[8]  = paint.gradParams0[2];
		tempFrag->paintMat[9]  = paint.gradParams1[1];
		tempFrag->paintMat[10] = paint.radius;
	} else if (shaderType == 18) {
		// Analytical Nested DRRect SDF (packed cleanly into fast 176-byte UBO)
		tempFrag->type = ShaderDRRect;
		tempFrag->extent[0] = paint.extent[0];
		tempFrag->extent[1] = paint.extent[1];
		tempFrag->radius = paint.radius;
		tempFrag->feather = paint.feather;
		tempFrag->strokeMult = paint.xform[0];
		tempFrag->strokeThr = paint.xform[1];
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
		// col 0: (inner_dx, inner_dy, inner_hw), col 1: (inner_hh, iTL, iTR), col 2: (iBR, iBL, 0)
		tempFrag->paintMat[0] = paint.gradParams0[0];
		tempFrag->paintMat[1] = paint.gradParams0[1];
		tempFrag->paintMat[2] = paint.gradParams0[2];
		tempFrag->paintMat[4] = paint.gradParams0[3];
		tempFrag->paintMat[5] = paint.gradParams1[0];
		tempFrag->paintMat[6] = paint.gradParams1[1];
		tempFrag->paintMat[8] = paint.gradParams1[2];
		tempFrag->paintMat[9] = paint.gradParams1[3];
	} else if (shaderType >= 0) {
		tempFrag->type = shaderType;
	}

	if (shaderType == 3) {
		if (tempFrag->texType == 0 && paint.image != 0) {
			if (auto tex = lookupTex(paint.image); tex && tex->type == TextureType::Alpha) {
				tempFrag->texType = 2;
			}
		}
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
		tempFrag->extent[0] = 0.0f;
		tempFrag->extent[1] = 0.0f;
		tempFrag->radius = 0.0f;
		tempFrag->feather = 1.0f;
		if (paint.radius > 0.5f) {
			tempFrag->innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
			tempFrag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		}
	} else if (shaderType == 7 || shaderType == 8) {
		tempFrag->innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		tempFrag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
	} else if (shaderType == 9) {
		tempFrag->extent[0] = paint.extent[0];
		tempFrag->extent[1] = paint.extent[1];
		tempFrag->radius = paint.radius;
		tempFrag->feather = paint.feather;
		tempFrag->strokeMult = paint.xform[0];
		tempFrag->strokeThr = paint.xform[1];
		tempFrag->innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		tempFrag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		std::memset(tempFrag->paintMat, 0, sizeof(tempFrag->paintMat));
	} else if (shaderType == 10) {
		tempFrag->extent[0] = paint.extent[0];
		tempFrag->extent[1] = paint.extent[1];
		tempFrag->radius = paint.radius;
		tempFrag->strokeThr = paint.feather;
		tempFrag->innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
		tempFrag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
	} else if (shaderType == 11) {
		tempFrag->extent[0] = paint.extent[0];
		tempFrag->extent[1] = paint.radius;
		tempFrag->radius = paint.feather;
		tempFrag->feather = paint.feather;
		tempFrag->strokeMult = paint.feather;
		tempFrag->strokeThr = paint.feather;
	} else if (shaderType == 6) {
		tempFrag->strokeMult = 2.0f;
	} else if (shaderType == 1) {
		if (paint.radius > 0.0f) {
			tempFrag->radius = paint.radius;
			tempFrag->extent[0] = paint.feather;
			tempFrag->extent[1] = (paint.outerColor.r > 0.0f) ? paint.outerColor.r : paint.feather;
			tempFrag->outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
			tempFrag->feather = 1.0f;
			tempFrag->strokeMult = 1.0f;
		}
	}
}

} // namespace nisaba::gpu
