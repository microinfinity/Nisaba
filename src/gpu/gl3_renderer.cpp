// -----------------------------------------------------------------------------
// Nisaba Sovereign 2D Graphics Engine - OpenGL 3.2 Core Pipeline Renderer
//
// Features:
// - Direct hardware rasterization with subpixel coverage anti-aliasing
// - High-throughput batch coalescing (merging consecutive primitive states)
// - Std140 uniform buffer object (UBO) streaming architecture
// - Analytical signed-distance field (SDF) evaluation for rounded rectangles
// - Hardware scissor acceleration and subpixel feathering
// -----------------------------------------------------------------------------

#include "nisaba/gpu/gl3_renderer.hpp"
#include "nisaba/gpu/runtime_effect.hpp"
#include "gl_headers.hpp"
#include "shader_uniforms.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cstddef>

namespace nisaba::gpu {

namespace {

enum UniformLoc {
	LocViewSize = 0,
	LocTex      = 1,
	LocDstTex   = 2,
	LocParams   = 3,
	MaxLocs     = 4
};

constexpr int ParamsBinding = 0;

struct GLShader {
	GLuint prog{0};
	GLuint frag{0};
	GLuint vert{0};
	GLint loc[MaxLocs]{0, 0, 0, 0};
};

struct Texture {
	int id{0};
	GLuint tex{0};
	int width{0};
	int height{0};
	TextureType type{TextureType::RGBA};
	int flags{0};
};

struct Blend {
	GLenum srcRGB{GL_INVALID_ENUM};
	GLenum dstRGB{GL_INVALID_ENUM};
	GLenum srcAlpha{GL_INVALID_ENUM};
	GLenum dstAlpha{GL_INVALID_ENUM};
};

enum class CallType {
	None = 0,
	Fill,
	ConvexFill,
	Stroke,
	Triangles,
	Clip,
	RestoreClip,
	PushLayer,
	PopLayer
};

enum class ShaderBucket : uint8_t {
	FastCore = 0,
	SimpleColor,
	RRectFast,
	DRRectFast,
	ImgFast,
	RingFast,
	LinearGradFast,
	RadialGradFast,
	MultiGradLutFast,
	MultiGradFast,
	ShadowFast,
	PerspectiveImgFast,
	BlendFast,
	EffectsFull,
	ZeroUboSolid,
	ZeroUboCircle,
	ZeroUboRect
};

struct Call {
	CallType type{CallType::None};
	ShaderBucket bucket{ShaderBucket::FastCore};
	int image{0};
	int pathOffset{0};
	int pathCount{0};
	int triangleOffset{0};
	int triangleCount{0};
	int uniformOffset{-1};
	int uniformStride{0};
	int layerIndex{-1};
	Blend blendFunc;
	bool programmableBlend{false};
	bool useStencil{false};
	bool evenOdd{false};
	bool hasCallBounds{false};
	int scissorRect[4]{-1, -1, -1, -1};
	float callBounds[4]{0.0f, 0.0f, 0.0f, 0.0f};
	int clipDepth{0};
	Winding winding{Winding::CounterClockwise};
	Transform2D paintXform{Transform2D::identity()};
	std::shared_ptr<nisaba::gpu::Shader> customShader{nullptr};
};

struct GLPath {
	int fillOffset{0};
	int fillCount{0};
	int strokeOffset{0};
	int strokeCount{0};
};

static void dumpShaderError(GLuint shader, const char* name, const char* type) {
	GLchar str[512 + 1];
	GLsizei len = 0;
	glGetShaderInfoLog(shader, 512, &len, str);
	if (len > 512) len = 512;
	str[len] = '\0';
	std::fprintf(stderr, "Shader %s/%s error:\n%s\n", name, type, str);
}

static void dumpProgramError(GLuint prog, const char* name) {
	GLchar str[512 + 1];
	GLsizei len = 0;
	glGetProgramInfoLog(prog, 512, &len, str);
	if (len > 512) len = 512;
	str[len] = '\0';
	std::fprintf(stderr, "Program %s error:\n%s\n", name, str);
}

static void checkGLError(int flags, const char* str) {
	if ((flags & CreateFlags::Debug) == 0) return;
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		std::fprintf(stderr, "OpenGL Error 0x%08x after %s\n", err, str);
	}
}

static bool createShader(GLShader& shader, const char* name, const char* header, const char* opts,
                         const char* vshader, const char* fshader) {
	GLint status = 0;
	const char* str[3];
	str[0] = header;
	str[1] = (opts != nullptr) ? opts : "";

	shader = GLShader{};

	GLuint prog = glCreateProgram();
	GLuint vert = glCreateShader(GL_VERTEX_SHADER);
	GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

	str[2] = vshader;
	glShaderSource(vert, 3, str, nullptr);
	str[2] = fshader;
	glShaderSource(frag, 3, str, nullptr);

	glCompileShader(vert);
	glGetShaderiv(vert, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		dumpShaderError(vert, name, "vert");
		glDeleteShader(vert);
		glDeleteShader(frag);
		glDeleteProgram(prog);
		return false;
	}

	glCompileShader(frag);
	glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		dumpShaderError(frag, name, "frag");
		glDeleteShader(vert);
		glDeleteShader(frag);
		glDeleteProgram(prog);
		return false;
	}

	glAttachShader(prog, vert);
	glAttachShader(prog, frag);

	glBindAttribLocation(prog, 0, "vertex");
	glBindAttribLocation(prog, 1, "tcoord");

	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		dumpProgramError(prog, name);
		glDeleteShader(vert);
		glDeleteShader(frag);
		glDeleteProgram(prog);
		return false;
	}

	shader.prog = prog;
	shader.vert = vert;
	shader.frag = frag;
	return true;
}

static void deleteShader(GLShader& shader) {
	if (shader.prog != 0) glDeleteProgram(shader.prog);
	if (shader.vert != 0) glDeleteShader(shader.vert);
	if (shader.frag != 0) glDeleteShader(shader.frag);
	shader = GLShader{};
}

static void getUniforms(GLShader& shader) {
	shader.loc[LocViewSize] = glGetUniformLocation(shader.prog, "viewSize");
	shader.loc[LocTex]      = glGetUniformLocation(shader.prog, "tex");
	shader.loc[LocDstTex]   = glGetUniformLocation(shader.prog, "dstTex");
	shader.loc[LocParams]   = glGetUniformBlockIndex(shader.prog, "NisabaShaderParams");
}

static GLenum convertBlendFactor(BlendFactor factor) {
	switch (factor) {
		case BlendFactor::Zero:                return GL_ZERO;
		case BlendFactor::One:                 return GL_ONE;
		case BlendFactor::SrcColor:            return GL_SRC_COLOR;
		case BlendFactor::OneMinusSrcColor:    return GL_ONE_MINUS_SRC_COLOR;
		case BlendFactor::DstColor:            return GL_DST_COLOR;
		case BlendFactor::OneMinusDstColor:    return GL_ONE_MINUS_DST_COLOR;
		case BlendFactor::SrcAlpha:            return GL_SRC_ALPHA;
		case BlendFactor::OneMinusSrcAlpha:    return GL_ONE_MINUS_SRC_ALPHA;
		case BlendFactor::DstAlpha:            return GL_DST_ALPHA;
		case BlendFactor::OneMinusDstAlpha:    return GL_ONE_MINUS_DST_ALPHA;
		case BlendFactor::SrcAlphaSaturate:    return GL_SRC_ALPHA_SATURATE;
	}
	return GL_INVALID_ENUM;
}

static Blend convertBlend(const CompositeOperationState& op) {
	Blend b;
	b.srcRGB   = convertBlendFactor(op.srcRGB);
	b.dstRGB   = convertBlendFactor(op.dstRGB);
	b.srcAlpha = convertBlendFactor(op.srcAlpha);
	b.dstAlpha = convertBlendFactor(op.dstAlpha);
	if (b.srcRGB == GL_INVALID_ENUM || b.dstRGB == GL_INVALID_ENUM ||
	    b.srcAlpha == GL_INVALID_ENUM || b.dstAlpha == GL_INVALID_ENUM) {
		b.srcRGB   = GL_ONE;
		b.dstRGB   = GL_ONE_MINUS_SRC_ALPHA;
		b.srcAlpha = GL_ONE;
		b.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;
	}
	return b;
}

} // namespace

// -------------------------------------------------------------
// GL3Renderer::Impl
// -------------------------------------------------------------
struct GL3Renderer::Impl {
	int flags{0};
	GLShader shader;
	GLShader rrectShader;
	GLShader drrectShader;
	GLShader imgShader;
	GLShader ringShader;
	GLShader linearGradShader;
	GLShader radialGradShader;
	GLShader multiGradLutShader;
	GLShader multiGradShader;
	GLShader shadowShader;
	GLShader perspectiveImgShader;
	GLShader blendFastShader;
	GLShader effectsShader;
	GLShader stencilStrokeShader;
	GLShader stencilEffectsShader;
	GLShader simpleShader;
	GLShader simpleColorShader;
	GLShader solidVertShader;
	GLShader circleVertShader;
	GLShader rectVertShader;
	GLuint activeProg{0};
	float view[2]{0.0f, 0.0f};

	std::vector<Texture> textures;
	int textureIdCounter{0};
	int dummyTex{0};

	GLuint vertArr{0};
	GLuint vertBuf{0};
	GLuint fragBuf{0};
	int fragSize{0};
	int fragFastSize{0};

	// Per-frame batches
	std::vector<Call> calls;
	std::vector<GLPath> paths;
	std::vector<Vertex> verts;
	std::vector<uint8_t> uniforms;

	// Cached state filter to eliminate redundant GL state binds
	GLuint boundTexture{0};
	GLuint stencilMaskVal{0xffffffff};
	GLenum stencilFuncMode{GL_ALWAYS};
	GLint stencilFuncRefVal{0};
	GLuint stencilFuncMaskVal{0xffffffff};
	Blend blendState;
	int boundUniformOffset{-1};
	size_t boundUniformSize{0};
	int lastTextureId{0};
	Texture* lastTexturePtr{nullptr};
	size_t fragBufCap{0};
	size_t vertBufCap{0};
	size_t fragRingOffset{0};
	int vertRingPos{0};
	int baseVertOffset{0};
	int baseUboOffset{0};
	bool scissorEnabled{false};
	bool stencilEnabled{false};
	bool isGLES{false};
	bool hasSamplerObjects{false};
	GLuint samplerCache[2][3]{{0, 0, 0}, {0, 0, 0}}; // [0=nearest, 1=linear][0=clamp, 1=repeat, 2=mirror]
	GLuint boundSampler{0};
	int currentScissor[4]{-1, -1, -1, -1};
	int lastUniformOffset{-1};
	size_t lastUniformUboBytes{0};
	int currentClipDepth{0};
	int flushedClipDepth{0};
	bool flushedTargetQueried{false};
	GLuint baseDrawFbo{0};
	GLuint baseReadFbo{0};
	GLint baseViewport[4]{0, 0, 0, 0};
	GLuint dstSnapFbo{0};
	GLuint dstSnapTex{0};
	int dstSnapW{0};
	int dstSnapH{0};

	struct LayerFbo {
		GLuint fbo{0};
		GLuint colorTex{0};
		GLuint depthStencilRb{0};
		int w{0};
		int h{0};
		bool inUse{false};
	};
	struct RuntimeLayer {
		GLuint parentFbo{0};
		int parentClipDepth{0};
		int poolIndex{-1};
	};
	std::vector<LayerDesc> layerDescs;
	std::vector<LayerFbo> layerPool;
	std::vector<RuntimeLayer> activeLayers;

	GLShader blurShader;
	GLint locBlurTex{-1};
	GLint locBlurTexelOffset{-1};
	GLint locBlurRadius{-1};
	GLint locBlurSigma{-1};
	GLint locBlurUvBounds{-1};
	GLShader filterBlendShader;
	GLint locFilterBlendSrc{-1};
	GLint locFilterBlendDst{-1};
	GLint locFilterBlendMode{-1};
	GLint locFilterBlendOffset{-1};
	GLint locFilterBlendColor{-1};
	GLint locFilterBlendRoiUv{-1};
	GLint locFilterBlendCropUv{-1};
	GLShader filterColorShader;
	GLint locFilterColorSrc{-1};
	GLint locFilterColorHasMat{-1};
	GLint locFilterColorMat0{-1};
	GLint locFilterColorMat1{-1};
	GLint locFilterColorMat2{-1};
	GLint locFilterColorMat3{-1};
	GLint locFilterColorMatBias{-1};
	GLint locFilterColorTransfer{-1};
	GLint locFilterColorHasBlend{-1};
	GLint locFilterColorBlendMode{-1};
	GLint locFilterColorBlendCol{-1};
	GLint locFilterColorRoiUv{-1};
	GLint locFilterColorCropUv{-1};
	GLint locFilterColorExtKind{-1};
	GLint locFilterColorHcParams{-1};
	GLint locFilterColorLut{-1};
	GLShader filterOpsShader;
	GLint locFilterOpsSrc{-1};
	GLint locFilterOpsSrc2{-1};
	GLint locFilterOpsRoiDev{-1};
	GLint locFilterOpsCropDev{-1};
	GLint locFilterOpsFbSize{-1};
	GLint locFilterOpsP0{-1};
	GLint locFilterOpsP1{-1};
	GLint locFilterOpsP2{-1};
	GLint locFilterOpsP3{-1};
	GLint locFilterOpsP4{-1};
	GLint locFilterOpsI0{-1};
	GLint locFilterOpsKernel{-1};
	struct RuntimeFilterProg {
		GLShader shader{};
		GLint locInputTex{-1};
		GLint locFbSize{-1};
		GLint locLayerMat0{-1};
		GLint locLayerMat1{-1};
		GLint locInvLayerMat0{-1};
		GLint locInvLayerMat1{-1};
		GLint locRoiUv{-1};
		GLint locCropUv{-1};
		std::vector<GLint> userUniformLocs{};
	};
	std::unordered_map<uint64_t, RuntimeFilterProg> runtimeFilterCache;
	struct RuntimeBlenderProg {
		GLShader shader{};
		GLint locSrcTex{-1};
		GLint locDstTex{-1};
		GLint locOpacity{-1};
		std::vector<GLint> userUniformLocs{};
	};
	std::unordered_map<uint64_t, RuntimeBlenderProg> runtimeBlenderCache;
	GLuint blurTierFbo[3][2]{{0, 0}, {0, 0}, {0, 0}};
	GLuint blurTierTex[3][2]{{0, 0}, {0, 0}, {0, 0}};
	int blurTierW[3]{0, 0, 0};
	int blurTierH[3]{0, 0, 0};

	struct BlurResult {
		GLuint fbo{0};
		GLuint tex{0};
		int scale{1};
		int bw{0};
		int bh{0};
	};

	void ensureBlurTier(int tier, int bw, int bh) {
		if (blurTierW[tier] == bw && blurTierH[tier] == bh &&
		    blurTierFbo[tier][0] != 0 && blurTierFbo[tier][1] != 0) {
			return;
		}
		for (int k = 0; k < 2; ++k) {
			if (blurTierFbo[tier][k] == 0) glGenFramebuffers(1, &blurTierFbo[tier][k]);
			if (blurTierTex[tier][k] == 0) glGenTextures(1, &blurTierTex[tier][k]);
			glBindTexture(GL_TEXTURE_2D, blurTierTex[tier][k]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindFramebuffer(GL_FRAMEBUFFER, blurTierFbo[tier][k]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTierTex[tier][k], 0);
		}
		blurTierW[tier] = bw;
		blurTierH[tier] = bh;
	}

	int acquireLayerFbo(int w, int h) {
		for (size_t i = 0; i < layerPool.size(); ++i) {
			if (!layerPool[i].inUse && layerPool[i].w == w && layerPool[i].h == h) {
				layerPool[i].inUse = true;
				return static_cast<int>(i);
			}
		}
		LayerFbo lf;
		lf.w = w;
		lf.h = h;
		lf.inUse = true;
		glGenTextures(1, &lf.colorTex);
		glBindTexture(GL_TEXTURE_2D, lf.colorTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glGenRenderbuffers(1, &lf.depthStencilRb);
		glBindRenderbuffer(GL_RENDERBUFFER, lf.depthStencilRb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

		glGenFramebuffers(1, &lf.fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, lf.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lf.colorTex, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, lf.depthStencilRb);

		glBindTexture(GL_TEXTURE_2D, boundTexture);
		layerPool.push_back(lf);
		return static_cast<int>(layerPool.size()) - 1;
	}

	BlurResult gaussianBlurToFbo(GLuint srcFbo, GLuint srcTex, int w, int h,
	                             float sigmaX, float sigmaY,
	                             bool hasRoi = false,
	                             int roiX0 = 0, int roiY0 = 0, int roiX1 = 0, int roiY1 = 0) {
		BlurResult res{};
		if (blurShader.prog == 0 || srcTex == 0 || w <= 0 || h <= 0) return res;

		int ox0 = hasRoi ? std::clamp(roiX0, 0, w) : 0;
		int oy0 = hasRoi ? std::clamp(roiY0, 0, h) : 0;
		int ox1 = hasRoi ? std::clamp(roiX1, 0, w) : w;
		int oy1 = hasRoi ? std::clamp(roiY1, 0, h) : h;
		if (ox1 <= ox0 || oy1 <= oy0) {
			ox0 = 0; oy0 = 0; ox1 = w; oy1 = h;
		}

		int ds = 1;
		int tier = 0;
		if (srcFbo != 0 && sigmaX >= 9.0f && sigmaY >= 9.0f &&
		    (w % 4 == 0) && (h % 4 == 0) && w >= 64 && h >= 64) {
			ds = 4;
			tier = 2;
		} else if (srcFbo != 0 && sigmaX >= 3.5f && sigmaY >= 3.5f &&
		           (w % 2 == 0) && (h % 2 == 0) && w >= 32 && h >= 32) {
			ds = 2;
			tier = 1;
		}

		const int bw = w / ds;
		const int bh = h / ds;
		if (ds == 4) {
			ensureBlurTier(1, w / 2, h / 2);
		}
		ensureBlurTier(tier, bw, bh);

		const int padX = (sigmaX > 0.001f) ? (static_cast<int>(std::ceil(sigmaX * 3.0f)) + 4) : 2;
		const int padY = (sigmaY > 0.001f) ? (static_cast<int>(std::ceil(sigmaY * 3.0f)) + 4) : 2;
		int sx0 = std::clamp(ox0 - padX, 0, w);
		int sy0 = std::clamp(oy0 - padY, 0, h);
		int sx1 = std::clamp(ox1 + padX, 0, w);
		int sy1 = std::clamp(oy1 + padY, 0, h);
		if (ds > 1) {
			sx0 = (sx0 / ds) * ds;
			sy0 = (sy0 / ds) * ds;
			sx1 = std::min(w, ((sx1 + ds - 1) / ds) * ds);
			sy1 = std::min(h, ((sy1 + ds - 1) / ds) * ds);
		}

		GLint prevVp[4]{0, 0, w, h};
		glGetIntegerv(GL_VIEWPORT, prevVp);
		glDisable(GL_SCISSOR_TEST);
		if (stencilEnabled) glDisable(GL_STENCIL_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		GLuint pass1InputTex = srcTex;
		if (ds == 2) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blurTierFbo[1][1]);
			glBlitFramebuffer(sx0, sy0, sx1, sy1, sx0 / 2, sy0 / 2, sx1 / 2, sy1 / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			pass1InputTex = blurTierTex[1][1];
		} else if (ds == 4) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blurTierFbo[1][1]);
			glBlitFramebuffer(sx0, sy0, sx1, sy1, sx0 / 2, sy0 / 2, sx1 / 2, sy1 / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, blurTierFbo[1][1]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blurTierFbo[2][1]);
			glBlitFramebuffer(sx0 / 2, sy0 / 2, sx1 / 2, sy1 / 2, sx0 / 4, sy0 / 4, sx1 / 4, sy1 / 4, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			pass1InputTex = blurTierTex[2][1];
		}

		useProgramCached(blurShader.prog);
		glUniform1i(locBlurTex, 0);
		glActiveTexture(GL_TEXTURE0);
		if (hasSamplerObjects && boundSampler != 0) {
			glBindSampler(0, 0);
			boundSampler = 0;
		}
		glViewport(0, 0, bw, bh);

		const int dx0 = std::clamp(ox0 / ds - 1, 0, bw);
		const int dy0 = std::clamp(oy0 / ds - 1, 0, bh);
		const int dx1 = std::clamp((ox1 + ds - 1) / ds + 1, 0, bw);
		const int dy1 = std::clamp((oy1 + ds - 1) / ds + 1, 0, bh);
		const int p1y0 = std::clamp(sy0 / ds, 0, bh);
		const int p1y1 = std::clamp((sy1 + ds - 1) / ds, 0, bh);
		const int srcDx0 = sx0 / ds;
		const int srcDx1 = (sx1 + ds - 1) / ds;

		const float invBw = 1.0f / static_cast<float>(bw);
		const float invBh = 1.0f / static_cast<float>(bh);
		const float esx = sigmaX / static_cast<float>(ds);
		const float esy = sigmaY / static_cast<float>(ds);

		auto runPass = [&](GLuint inTex, GLuint dstFbo, float offX, float offY, float sigma,
		                   int scX0, int scY0, int scX1, int scY1,
		                   float uvMinX, float uvMinY, float uvMaxX, float uvMaxY) {
			if (scX1 <= scX0 || scY1 <= scY0) return;
			int rad = 0;
			float step = 1.0f;
			float effSigma = sigma;
			if (sigma > 0.001f) {
				float idealRad = std::ceil(sigma * 2.5f);
				if (idealRad <= 32.0f) {
					rad = static_cast<int>(idealRad);
				} else {
					rad = 32;
					step = idealRad / 32.0f;
					effSigma = sigma / step;
				}
			}
			glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
			glEnable(GL_SCISSOR_TEST);
			glScissor(scX0, scY0, scX1 - scX0, scY1 - scY0);
			glBindTexture(GL_TEXTURE_2D, inTex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glUniform2f(locBlurTexelOffset, offX * step, offY * step);
			glUniform1i(locBlurRadius, rad);
			glUniform1f(locBlurSigma, effSigma);
			if (locBlurUvBounds >= 0) {
				glUniform4f(locBlurUvBounds, uvMinX, uvMinY, uvMaxX, uvMaxY);
			}
			glDrawArrays(GL_TRIANGLES, 0, 3);
		};

		// Pass 1: Horizontal pass1InputTex -> blurTierFbo[tier][0]
		runPass(pass1InputTex, blurTierFbo[tier][0], invBw, 0.0f, esx,
		        dx0, p1y0, dx1, p1y1,
		        (static_cast<float>(srcDx0) + 0.5f) * invBw,
		        (static_cast<float>(p1y0) + 0.5f) * invBh,
		        (static_cast<float>(srcDx1) - 0.5f) * invBw,
		        (static_cast<float>(p1y1) - 0.5f) * invBh);

		// Pass 2: Vertical blurTierTex[tier][0] -> blurTierFbo[tier][1]
		runPass(blurTierTex[tier][0], blurTierFbo[tier][1], 0.0f, invBh, esy,
		        dx0, dy0, dx1, dy1,
		        (static_cast<float>(dx0) + 0.5f) * invBw,
		        (static_cast<float>(p1y0) + 0.5f) * invBh,
		        (static_cast<float>(dx1) - 0.5f) * invBw,
		        (static_cast<float>(p1y1) - 0.5f) * invBh);

		glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
		glEnable(GL_BLEND);
		glEnable(GL_CULL_FACE);
		if (scissorEnabled) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(currentScissor[0], currentScissor[1], currentScissor[2], currentScissor[3]);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
		if (stencilEnabled) glEnable(GL_STENCIL_TEST);
		boundTexture = 0;
		res.fbo = blurTierFbo[tier][1];
		res.tex = blurTierTex[tier][1];
		res.scale = ds;
		res.bw = bw;
		res.bh = bh;
		return res;
	}

	void ensureBaseTargetInfo() {
		if (flushedTargetQueried) return;
		GLint drawFbo = 0;
		GLint readFbo = 0;
		glGetIntegerv(GL_VIEWPORT, baseViewport);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
		baseDrawFbo = static_cast<GLuint>(drawFbo);
		baseReadFbo = static_cast<GLuint>(readFbo);
		flushedTargetQueried = true;
	}

	void captureDstSnapshot(bool hasBounds, const float* bounds, const int* scissor) {
		ensureBaseTargetInfo();
		const bool inLayer = !activeLayers.empty();
		const int vpX = inLayer ? 0 : baseViewport[0];
		const int vpY = inLayer ? 0 : baseViewport[1];
		const int vpW = inLayer ? layerPool[activeLayers.back().poolIndex].w
		                        : ((baseViewport[2] > 0) ? baseViewport[2] : std::max(1, static_cast<int>(view[0])));
		const int vpH = inLayer ? layerPool[activeLayers.back().poolIndex].h
		                        : ((baseViewport[3] > 0) ? baseViewport[3] : std::max(1, static_cast<int>(view[1])));
		const int snapW = std::max(1, vpX + vpW);
		const int snapH = std::max(1, vpY + vpH);
		const GLuint curDrawFbo = inLayer ? layerPool[activeLayers.back().poolIndex].fbo : baseDrawFbo;

		if (dstSnapFbo == 0 || dstSnapTex == 0 || dstSnapW != snapW || dstSnapH != snapH) {
			if (dstSnapFbo == 0) glGenFramebuffers(1, &dstSnapFbo);
			if (dstSnapTex == 0) glGenTextures(1, &dstSnapTex);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, dstSnapTex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, snapW, snapH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindFramebuffer(GL_FRAMEBUFFER, dstSnapFbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstSnapTex, 0);
			dstSnapW = snapW;
			dstSnapH = snapH;
			glActiveTexture(GL_TEXTURE0);
		}

		int px0 = 0;
		int py0 = 0;
		int px1 = snapW;
		int py1 = snapH;
		if (hasBounds && bounds != nullptr && view[0] > 0.0f && view[1] > 0.0f) {
			const float sx = static_cast<float>(vpW) / view[0];
			const float sy = static_cast<float>(vpH) / view[1];
			px0 = std::clamp(vpX + static_cast<int>(std::floor(bounds[0] * sx)) - 2, 0, snapW);
			px1 = std::clamp(vpX + static_cast<int>(std::ceil(bounds[2] * sx)) + 2, 0, snapW);
			const int topY = vpY + vpH - static_cast<int>(std::ceil(bounds[3] * sy)) - 2;
			const int botY = vpY + vpH - static_cast<int>(std::floor(bounds[1] * sy)) + 2;
			py0 = std::clamp(topY, 0, snapH);
			py1 = std::clamp(botY, 0, snapH);
		}
		if (scissor != nullptr && scissor[0] >= 0) {
			px0 = std::max(px0, scissor[0]);
			py0 = std::max(py0, scissor[1]);
			px1 = std::min(px1, scissor[0] + scissor[2]);
			py1 = std::min(py1, scissor[1] + scissor[3]);
		}

		if (px1 > px0 && py1 > py0) {
			if (scissorEnabled) {
				glDisable(GL_SCISSOR_TEST);
			}
			glBindFramebuffer(GL_READ_FRAMEBUFFER, curDrawFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstSnapFbo);
			glBlitFramebuffer(px0, py0, px1, py1, px0, py0, px1, py1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, baseReadFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, curDrawFbo);
			if (scissorEnabled) {
				glEnable(GL_SCISSOR_TEST);
			}
		} else {
			glBindFramebuffer(GL_FRAMEBUFFER, curDrawFbo);
		}

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, dstSnapTex);
		if (hasSamplerObjects) glBindSampler(1, 0);
		glActiveTexture(GL_TEXTURE0);
	}

	void captureDstSnapshotForCall(const Call& call) {
		captureDstSnapshot(call.hasCallBounds, call.callBounds, call.scissorRect);
	}

	void resetCachedGLState() {
		activeProg = 0;
		boundTexture = 0;
		boundSampler = 0;
		lastTextureId = 0;
		lastTexturePtr = nullptr;
		stencilEnabled = false;
		stencilMaskVal = 0xffffffff;
		stencilFuncMode = GL_ALWAYS;
		stencilFuncRefVal = 0;
		stencilFuncMaskVal = 0xffffffff;
		blendState = Blend{};
		boundUniformOffset = -1;
		boundUniformSize = 0;
		lastUniformOffset = -1;
		lastUniformUboBytes = 0;
		flushedTargetQueried = false;
		scissorEnabled = false;
		currentScissor[0] = -1;
		currentScissor[1] = -1;
		currentScissor[2] = -1;
		currentScissor[3] = -1;
	}

	void clearBatchBuffers() {
		verts.clear();
		paths.clear();
		calls.clear();
		uniforms.clear();
		layerDescs.clear();
		boundUniformOffset = -1;
		boundUniformSize = 0;
		lastUniformOffset = -1;
		lastUniformUboBytes = 0;
	}

	void useProgramCached(GLuint prog) {
		if (activeProg != prog) {
			activeProg = prog;
			glUseProgram(prog);
		}
	}

	void enableStencil(bool enable) {
		if (stencilEnabled != enable) {
			stencilEnabled = enable;
			if (enable) {
				glEnable(GL_STENCIL_TEST);
			} else {
				glDisable(GL_STENCIL_TEST);
			}
		}
	}

	void bindTexture(GLuint tex) {
		if (boundTexture != tex) {
			boundTexture = tex;
			glBindTexture(GL_TEXTURE_2D, tex);
		}
	}

	void stencilMask(GLuint mask) {
		if (stencilMaskVal != mask) {
			stencilMaskVal = mask;
			glStencilMask(mask);
		}
	}

	void stencilFunc(GLenum func, GLint ref, GLuint mask) {
		if (stencilFuncMode != func || stencilFuncRefVal != ref || stencilFuncMaskVal != mask) {
			stencilFuncMode = func;
			stencilFuncRefVal = ref;
			stencilFuncMaskVal = mask;
			glStencilFunc(func, ref, mask);
		}
	}

	void blendFuncSeparate(const Blend& blend) {
		if (blendState.srcRGB != blend.srcRGB || blendState.dstRGB != blend.dstRGB ||
		    blendState.srcAlpha != blend.srcAlpha || blendState.dstAlpha != blend.dstAlpha) {
			blendState = blend;
			glBlendFuncSeparate(blend.srcRGB, blend.dstRGB, blend.srcAlpha, blend.dstAlpha);
		}
	}

	Texture* allocTexture() {
		for (auto& t : textures) {
			if (t.id == 0) {
				t = Texture{};
				t.id = ++textureIdCounter;
				return &t;
			}
		}
		textures.emplace_back();
		Texture* tex = &textures.back();
		tex->id = ++textureIdCounter;
		return tex;
	}

	Texture* findTexture(int id) {
		if (lastTexturePtr != nullptr && lastTextureId == id) return lastTexturePtr;
		for (auto& t : textures) {
			if (t.id == id) {
				lastTextureId = id;
				lastTexturePtr = &t;
				return &t;
			}
		}
		return nullptr;
	}

	bool deleteTexture(int id) {
		if (lastTextureId == id) {
			lastTextureId = 0;
			lastTexturePtr = nullptr;
		}
		for (auto& t : textures) {
			if (t.id == id) {
				if (t.tex != 0 && (t.flags & ImageFlagsGL::ImageNoDelete) == 0) {
					glDeleteTextures(1, &t.tex);
				}
				t = Texture{};
				return true;
			}
		}
		return false;
	}

	static constexpr bool bucketUsesZeroUbo(ShaderBucket b) noexcept {
		return b == ShaderBucket::ZeroUboSolid ||
		       b == ShaderBucket::ZeroUboCircle ||
		       b == ShaderBucket::ZeroUboRect;
	}

	static constexpr bool bucketNeedsFullUbo(ShaderBucket b) noexcept {
		return b == ShaderBucket::MultiGradLutFast ||
		       b == ShaderBucket::MultiGradFast ||
		       b == ShaderBucket::ShadowFast ||
		       b == ShaderBucket::PerspectiveImgFast ||
		       b == ShaderBucket::BlendFast ||
		       b == ShaderBucket::EffectsFull;
	}

	ShaderBucket classifyPaintBucket(const FragUniforms& f, int image, bool hasCustomShader, bool allowZeroUbo = true) const noexcept {
		if (hasCustomShader) {
			return ShaderBucket::EffectsFull;
		}
		if (f.type == ShaderPerspectiveImg) {
			if (f.blendMode == 0 && f.hasColorMatrix == 0 && f.colorTransfer == 0 && f.colorBlendEnabled == 0) {
				return ShaderBucket::PerspectiveImgFast;
			}
			return ShaderBucket::EffectsFull;
		}
		if (f.blendMode != 0) {
			if (f.hasColorMatrix == 0 && f.colorTransfer == 0 && f.colorBlendEnabled == 0 &&
			    (f.type == ShaderSolid || f.type == ShaderSimple || f.type == ShaderCircle ||
			     f.type == ShaderRect || f.type == ShaderRRect || f.type == ShaderImg)) {
				return ShaderBucket::BlendFast;
			}
			return ShaderBucket::EffectsFull;
		}
		if (uniformUsesFullEffectsShader(f)) {
			return ShaderBucket::EffectsFull;
		}
		if (f.type == ShaderMultiGrad) {
			return (f.gradStopCount < 0) ? ShaderBucket::MultiGradLutFast : ShaderBucket::MultiGradFast;
		}
		if (f.type == ShaderRRectShadow) return ShaderBucket::ShadowFast;
		if (allowZeroUbo && image == 0 && f.scissorExt[0] < 0.0f) {
			if (f.type == ShaderSolid && f.strokeMult == 1.0f && solidVertShader.prog != 0) return ShaderBucket::ZeroUboSolid;
			if (f.type == ShaderCircle && circleVertShader.prog != 0) return ShaderBucket::ZeroUboCircle;
			if (f.type == ShaderRect && rectVertShader.prog != 0) return ShaderBucket::ZeroUboRect;
		}
		if (f.type == ShaderSimple) return ShaderBucket::SimpleColor;
		if (f.type == ShaderRRect) return ShaderBucket::RRectFast;
		if (f.type == ShaderDRRect) return ShaderBucket::DRRectFast;
		if (f.type == ShaderImg) return ShaderBucket::ImgFast;
		if (f.type == ShaderRing) return ShaderBucket::RingFast;
		if (f.type == ShaderLinearGrad) return ShaderBucket::LinearGradFast;
		if (f.type == ShaderRadialGrad) return ShaderBucket::RadialGradFast;
		return ShaderBucket::FastCore;
	}

	int bucketStride(ShaderBucket b) const noexcept {
		if (bucketUsesZeroUbo(b)) return 0;
		return bucketNeedsFullUbo(b) ? fragSize : fragFastSize;
	}

	int allocFragUniforms(int n, int stride) {
		int ret = static_cast<int>(uniforms.size());
		uniforms.resize(ret + n * stride, 0);
		return ret;
	}

	int allocFragUniforms(int n) {
		return allocFragUniforms(n, fragSize);
	}

	FragUniforms* fragUniformPtr(int offset) {
		return reinterpret_cast<FragUniforms*>(&uniforms[offset]);
	}

	int storeOrReuseUniform(const FragUniforms& src, ShaderBucket bucket) {
		const size_t copyBytes = bucketNeedsFullUbo(bucket) ? sizeof(FragUniforms) : kFastUniformBytes;
		const int stride = bucketNeedsFullUbo(bucket) ? fragSize : fragFastSize;
		if (lastUniformOffset >= 0 && lastUniformUboBytes == copyBytes && !uniforms.empty() &&
		    std::memcmp(fragUniformPtr(lastUniformOffset), &src, copyBytes) == 0) {
			return lastUniformOffset;
		}
		int off = allocFragUniforms(1, stride);
		std::memcpy(&uniforms[off], &src, copyBytes);
		lastUniformOffset = off;
		lastUniformUboBytes = copyBytes;
		return off;
	}

	Texture* dummyTexPtr{nullptr};

	void bindUboSlice(int uniformOffset, size_t byteSize) {
		int actualOffset = baseUboOffset + uniformOffset;
		if (boundUniformOffset != actualOffset || boundUniformSize < byteSize) {
			boundUniformOffset = actualOffset;
			boundUniformSize = byteSize;
			glBindBufferRange(GL_UNIFORM_BUFFER, ParamsBinding, fragBuf, actualOffset, byteSize);
		}
	}

	void bindImageTexture(int image) {
		if (image == 0) return;
		Texture* tex = findTexture(image);
		bindTexture(tex != nullptr ? tex->tex : (dummyTexPtr != nullptr ? dummyTexPtr->tex : 0));
		if (hasSamplerObjects) {
			GLuint wantSampler = 0;
			if (tex != nullptr) {
				int filterIdx = -1;
				if ((tex->flags & ImageFlags::ImageForceNearest) != 0) filterIdx = 0;
				else if ((tex->flags & ImageFlags::ImageForceBilinear) != 0) filterIdx = 1;
				if (filterIdx >= 0) {
					int wrapIdx = (tex->flags & ImageFlags::ImageMirrorX) ? 2
					            : ((tex->flags & ImageFlags::ImageRepeatX) ? 1 : 0);
					wantSampler = samplerCache[filterIdx][wrapIdx];
				}
			}
			if (boundSampler != wantSampler) {
				glBindSampler(0, wantSampler);
				boundSampler = wantSampler;
			}
		}
		checkGLError(flags, "tex paint tex");
	}

	void setUniforms(ShaderBucket bucket, int uniformOffset, int image) {
		switch (bucket) {
			case ShaderBucket::ZeroUboSolid:
				useProgramCached(solidVertShader.prog);
				return;
			case ShaderBucket::ZeroUboCircle:
				useProgramCached(circleVertShader.prog);
				return;
			case ShaderBucket::ZeroUboRect:
				useProgramCached(rectVertShader.prog);
				return;
			default:
				break;
		}
		bool fullUbo = bucketNeedsFullUbo(bucket);
		GLuint targetProg = 0;
		switch (bucket) {
			case ShaderBucket::SimpleColor:
				targetProg = (simpleColorShader.prog != 0) ? simpleColorShader.prog : shader.prog;
				break;
			case ShaderBucket::RRectFast:
				targetProg = (rrectShader.prog != 0) ? rrectShader.prog : shader.prog;
				break;
			case ShaderBucket::DRRectFast:
				targetProg = (drrectShader.prog != 0) ? drrectShader.prog : shader.prog;
				break;
			case ShaderBucket::ImgFast:
				targetProg = (imgShader.prog != 0) ? imgShader.prog : shader.prog;
				break;
			case ShaderBucket::RingFast:
				targetProg = (ringShader.prog != 0) ? ringShader.prog : shader.prog;
				break;
			case ShaderBucket::LinearGradFast:
				targetProg = (linearGradShader.prog != 0) ? linearGradShader.prog : shader.prog;
				break;
			case ShaderBucket::RadialGradFast:
				targetProg = (radialGradShader.prog != 0) ? radialGradShader.prog : shader.prog;
				break;
			case ShaderBucket::MultiGradLutFast:
				targetProg = (multiGradLutShader.prog != 0) ? multiGradLutShader.prog : effectsShader.prog;
				break;
			case ShaderBucket::MultiGradFast:
				targetProg = (multiGradShader.prog != 0) ? multiGradShader.prog : effectsShader.prog;
				break;
			case ShaderBucket::ShadowFast:
				targetProg = (shadowShader.prog != 0) ? shadowShader.prog : effectsShader.prog;
				break;
			case ShaderBucket::PerspectiveImgFast:
				targetProg = (perspectiveImgShader.prog != 0) ? perspectiveImgShader.prog : effectsShader.prog;
				break;
			case ShaderBucket::BlendFast:
				targetProg = (blendFastShader.prog != 0) ? blendFastShader.prog : effectsShader.prog;
				break;
			case ShaderBucket::EffectsFull:
				targetProg = effectsShader.prog;
				break;
			case ShaderBucket::FastCore:
			default:
				targetProg = shader.prog;
				break;
		}
		if (targetProg == 0) {
			targetProg = effectsShader.prog;
			fullUbo = true;
		}
		useProgramCached(targetProg);
		bindUboSlice(uniformOffset, fullUbo ? sizeof(NisabaShaderUniforms) : kFastUniformBytes);
		bindImageTexture(image);
	}

	void bindCallShader(const Call& call, int uniformOffset, int image) {
		if (call.customShader && call.customShader->shader_kind() == ShaderKind::Runtime) {
			activeProg = 0;
			call.customShader->bind_shader(view, call.paintXform);
			return;
		}
		setUniforms(call.bucket, uniformOffset, image);
	}

	void unbindCallShader(const Call& call) {
		if (call.customShader) {
			useProgramCached(shader.prog);
			boundUniformOffset = -1;
			boundUniformSize = 0;
		}
	}

	Call* findCoalescableTrianglesCall(ShaderBucket bucket,
	                                   int image,
	                                   const std::shared_ptr<nisaba::gpu::Shader>& customShader,
	                                   const Transform2D& paintXform,
	                                   const Blend& blend,
	                                   bool hasHwScissor,
	                                   const int hwScissor[4],
	                                   bool programmableBlend,
	                                   const FragUniforms& tempFrag) {
		if (programmableBlend || calls.empty()) return nullptr;
		Call& last = calls.back();
		if (last.type != CallType::Triangles || last.bucket != bucket ||
		    last.programmableBlend || last.image != image || last.customShader != customShader ||
		    last.blendFunc.srcRGB != blend.srcRGB || last.blendFunc.dstRGB != blend.dstRGB ||
		    last.blendFunc.srcAlpha != blend.srcAlpha || last.blendFunc.dstAlpha != blend.dstAlpha ||
		    last.scissorRect[0] != (hasHwScissor ? hwScissor[0] : -1) ||
		    last.scissorRect[1] != (hasHwScissor ? hwScissor[1] : -1) ||
		    last.scissorRect[2] != (hasHwScissor ? hwScissor[2] : -1) ||
		    last.scissorRect[3] != (hasHwScissor ? hwScissor[3] : -1) ||
		    last.clipDepth != currentClipDepth) {
			return nullptr;
		}
		if (bucketUsesZeroUbo(bucket)) {
			return &last;
		}
		if (customShader != nullptr) {
			const bool xformMatch = std::memcmp(last.paintXform.m.data(), paintXform.m.data(), sizeof(paintXform.m)) == 0;
			return xformMatch ? &last : nullptr;
		}
		if (last.uniformOffset < 0) return nullptr;
		const size_t cmpBytes = bucketNeedsFullUbo(bucket) ? sizeof(FragUniforms) : kFastUniformBytes;
		return (std::memcmp(fragUniformPtr(last.uniformOffset), &tempFrag, cmpBytes) == 0) ? &last : nullptr;
	}

	void assignCallUniforms(Call& call, const FragUniforms& tempFrag) {
		if (bucketUsesZeroUbo(call.bucket)) {
			call.uniformOffset = -1;
			call.uniformStride = 0;
		} else {
			call.uniformStride = bucketStride(call.bucket);
			call.uniformOffset = storeOrReuseUniform(tempFrag, call.bucket);
		}
	}

	static void setCallBoundsFromVerts(Call& call, const Vertex* verts, int nverts, float pad) {
		if (!verts || nverts <= 0) return;
		float minX = verts[0].x, maxX = verts[0].x;
		float minY = verts[0].y, maxY = verts[0].y;
		for (int i = 1; i < nverts; ++i) {
			minX = std::min(minX, verts[i].x);
			maxX = std::max(maxX, verts[i].x);
			minY = std::min(minY, verts[i].y);
			maxY = std::max(maxY, verts[i].y);
		}
		call.hasCallBounds = true;
		call.callBounds[0] = minX - pad;
		call.callBounds[1] = minY - pad;
		call.callBounds[2] = maxX + pad;
		call.callBounds[3] = maxY + pad;
	}

	std::vector<GLint> fanFirst;
	std::vector<GLsizei> fanCount;
	std::vector<GLint> stripFirst;
	std::vector<GLsizei> stripCount;

	bool getHardwareScissor(const Scissor& scissor, int outRect[4]) {
		if (scissor.extent[0] < -0.5f || scissor.extent[1] < -0.5f) {
			outRect[0] = -1;
			outRect[1] = -1;
			outRect[2] = -1;
			outRect[3] = -1;
			return false;
		}
		if (std::abs(scissor.xform[1]) > 1e-4f || std::abs(scissor.xform[2]) > 1e-4f) {
			outRect[0] = -1;
			return false;
		}
		float sx = scissor.xform[0];
		float sy = scissor.xform[3];
		if (sx <= 0.0f || sy <= 0.0f) {
			outRect[0] = -1;
			return false;
		}
		float cx = scissor.xform[4];
		float cy = scissor.xform[5];
		float hw = scissor.extent[0] * sx;
		float hh = scissor.extent[1] * sy;

		float minX = cx - hw;
		float maxX = cx + hw;
		float minY = cy - hh;
		float maxY = cy + hh;

		int ix = static_cast<int>(std::floor(minX));
		int iy = static_cast<int>(std::floor(view[1] - maxY));
		int iw = static_cast<int>(std::ceil(maxX - minX));
		int ih = static_cast<int>(std::ceil(maxY - minY));

		outRect[0] = std::max(0, ix);
		outRect[1] = std::max(0, iy);
		outRect[2] = std::max(0, iw);
		outRect[3] = std::max(0, ih);
		return true;
	}

	std::optional<TextureUniformInfo> lookupTextureInfo(int imageId) {
		Texture* tex = findTexture(imageId);
		if (!tex) return std::nullopt;
		return TextureUniformInfo{tex->type, tex->flags};
	}

	bool convertPaint(FragUniforms* frag, const Paint& paint, const Scissor& scissor,
	                  float width, float fringe, float strokeThr, bool hasHwScissor = false,
	                  int blendModeId = 0, bool fastPrefixOnly = false) {
		return convertPaintUniforms(frag, paint, scissor, width, fringe, strokeThr, hasHwScissor,
		                            blendModeId, [this](int id) { return lookupTextureInfo(id); },
		                            fastPrefixOnly);
	}

	void fill(const Call& call, int activeClipDepth) {
		const GLPath* callPaths = &paths[call.pathOffset];
		int npaths = call.pathCount;

		if (activeClipDepth > 0) {
			int clipRef = activeClipDepth << 4;
			// 1. Draw shapes into Stencil Buffer (lower nibble bits 0..3)
			enableStencil(true);
			stencilMask(0x0f);
			stencilFunc(GL_EQUAL, clipRef, 0xf0);
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

			if (simpleShader.prog != 0) useProgramCached(simpleShader.prog);
			else if (call.uniformOffset >= 0) setUniforms(call.bucket, call.uniformOffset, 0);
			checkGLError(flags, "fill clip simple");

			glDisable(GL_CULL_FACE);
			if (call.evenOdd || npaths == 1) {
				glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
			} else {
				glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
				glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
			}
			if (npaths == 1) {
				glDrawArrays(GL_TRIANGLE_FAN, baseVertOffset + callPaths[0].fillOffset, callPaths[0].fillCount);
			} else {
				stripFirst.clear();
				stripCount.clear();
				stripFirst.reserve(npaths);
				stripCount.reserve(npaths);
				for (int i = 0; i < npaths; i++) {
					if (callPaths[i].fillCount > 0) {
						stripFirst.push_back(baseVertOffset + callPaths[i].fillOffset);
						stripCount.push_back(callPaths[i].fillCount);
					}
				}
				if (!stripFirst.empty()) {
					glMultiDrawArrays(GL_TRIANGLE_FAN, stripFirst.data(), stripCount.data(), static_cast<GLsizei>(stripFirst.size()));
				}
			}
			glEnable(GL_CULL_FACE);

			// 2. Draw anti-aliased pixels (AA Fringe)
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			bindCallShader(call, call.uniformOffset, call.image);
			checkGLError(flags, "fill clip fill");

			if ((flags & CreateFlags::Antialias) != 0) {
				stencilMask(0x00);
				stencilFunc(GL_EQUAL, clipRef, 0xff);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				if (npaths == 1) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[0].strokeOffset, callPaths[0].strokeCount);
				} else {
					stripFirst.clear();
					stripCount.clear();
					stripFirst.reserve(npaths);
					stripCount.reserve(npaths);
					for (int i = 0; i < npaths; i++) {
						if (callPaths[i].strokeCount > 0) {
							stripFirst.push_back(baseVertOffset + callPaths[i].strokeOffset);
							stripCount.push_back(callPaths[i].strokeCount);
						}
					}
					if (!stripFirst.empty()) {
						glMultiDrawArrays(GL_TRIANGLE_STRIP, stripFirst.data(), stripCount.data(), static_cast<GLsizei>(stripFirst.size()));
					}
				}
			}

			// 3. Draw fill cover quad
			glDisable(GL_CULL_FACE);
			stencilMask(0x0f);
			stencilFunc(GL_NOTEQUAL, clipRef, 0x0f);
			glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
			glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + call.triangleOffset, call.triangleCount);
			glEnable(GL_CULL_FACE);

			stencilMask(0x00);
			stencilFunc(GL_EQUAL, clipRef, 0xf0);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			unbindCallShader(call);
			return;
		}

		// 1. Draw shapes into Stencil Buffer (unclipped)
		enableStencil(true);
		stencilMask(0xff);
		stencilFunc(GL_ALWAYS, 0, 0xff);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

		if (simpleShader.prog != 0) useProgramCached(simpleShader.prog);
		else if (call.uniformOffset >= 0) setUniforms(call.bucket, call.uniformOffset, 0);
		checkGLError(flags, "fill simple");

		glDisable(GL_CULL_FACE);
		if (call.evenOdd || npaths == 1) {
			// Single contour concave polygon or EvenOdd multi-contour fill: GL_INVERT
			glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
		} else {
			glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
			glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
		}
		if (npaths == 1) {
			glDrawArrays(GL_TRIANGLE_FAN, baseVertOffset + callPaths[0].fillOffset, callPaths[0].fillCount);
		} else {
			stripFirst.clear();
			stripCount.clear();
			stripFirst.reserve(npaths);
			stripCount.reserve(npaths);
			for (int i = 0; i < npaths; i++) {
				if (callPaths[i].fillCount > 0) {
					stripFirst.push_back(baseVertOffset + callPaths[i].fillOffset);
					stripCount.push_back(callPaths[i].fillCount);
				}
			}
			if (!stripFirst.empty()) {
				glMultiDrawArrays(GL_TRIANGLE_FAN, stripFirst.data(), stripCount.data(), static_cast<GLsizei>(stripFirst.size()));
			}
		}
		glEnable(GL_CULL_FACE);

		// 2. Draw anti-aliased pixels (AA Fringe)
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		bindCallShader(call, call.uniformOffset, call.image);
		checkGLError(flags, "fill fill");

		if ((flags & CreateFlags::Antialias) != 0) {
			stencilFunc(GL_EQUAL, 0x00, 0xff);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			if (npaths == 1) {
				glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[0].strokeOffset, callPaths[0].strokeCount);
			} else {
				stripFirst.clear();
				stripCount.clear();
				stripFirst.reserve(npaths);
				stripCount.reserve(npaths);
				for (int i = 0; i < npaths; i++) {
					if (callPaths[i].strokeCount > 0) {
						stripFirst.push_back(baseVertOffset + callPaths[i].strokeOffset);
						stripCount.push_back(callPaths[i].strokeCount);
					}
				}
				if (!stripFirst.empty()) {
					glMultiDrawArrays(GL_TRIANGLE_STRIP, stripFirst.data(), stripCount.data(), static_cast<GLsizei>(stripFirst.size()));
				}
			}
		}

		// 3. Draw fill cover quad
		stencilFunc(GL_NOTEQUAL, 0x0, 0xff);
		glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
		glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + call.triangleOffset, call.triangleCount);

		enableStencil(false);
		unbindCallShader(call);
	}

	void convexFill(const Call& call, int activeClipDepth) {
		const GLPath* callPaths = &paths[call.pathOffset];
		int npaths = call.pathCount;

		if (activeClipDepth > 0) {
			enableStencil(true);
			stencilMask(0x00);
			stencilFunc(GL_EQUAL, activeClipDepth << 4, 0xf0);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		} else {
			enableStencil(false);
		}

		bindCallShader(call, call.uniformOffset, call.image);
		checkGLError(flags, "convex fill");

		if (npaths == 1) {
			if (callPaths[0].fillCount > 0) {
				glDrawArrays(GL_TRIANGLE_FAN, baseVertOffset + callPaths[0].fillOffset, callPaths[0].fillCount);
			}
			if (callPaths[0].strokeCount > 0) {
				glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[0].strokeOffset, callPaths[0].strokeCount);
			}
			unbindCallShader(call);
			return;
		}

		fanFirst.clear();
		fanCount.clear();
		stripFirst.clear();
		stripCount.clear();

		fanFirst.reserve(npaths);
		fanCount.reserve(npaths);
		stripFirst.reserve(npaths);
		stripCount.reserve(npaths);

		for (int i = 0; i < npaths; i++) {
			if (callPaths[i].fillCount > 0) {
				fanFirst.push_back(baseVertOffset + callPaths[i].fillOffset);
				fanCount.push_back(callPaths[i].fillCount);
			}
			if (callPaths[i].strokeCount > 0) {
				stripFirst.push_back(baseVertOffset + callPaths[i].strokeOffset);
				stripCount.push_back(callPaths[i].strokeCount);
			}
		}

		if (!fanFirst.empty()) {
			glMultiDrawArrays(GL_TRIANGLE_FAN, fanFirst.data(), fanCount.data(), static_cast<GLsizei>(fanFirst.size()));
		}
		if (!stripFirst.empty()) {
			glMultiDrawArrays(GL_TRIANGLE_STRIP, stripFirst.data(), stripCount.data(), static_cast<GLsizei>(stripFirst.size()));
		}
		unbindCallShader(call);
	}

	void stroke(const Call& call, int activeClipDepth) {
		const GLPath* callPaths = &paths[call.pathOffset];
		int npaths = call.pathCount;

		if (call.useStencil) {
			enableStencil(true);
			const GLuint stencilProg =
				((call.bucket == ShaderBucket::EffectsFull || call.bucket == ShaderBucket::MultiGradFast) &&
				 stencilEffectsShader.prog != 0)
					? stencilEffectsShader.prog
					: stencilStrokeShader.prog;
			bool switchedProg = (!call.customShader && stencilProg != 0);
			if (activeClipDepth > 0) {
				int clipRef = activeClipDepth << 4;
				stencilMask(0x0f);
				stencilFunc(GL_EQUAL, clipRef, 0xff);
				glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
				bindCallShader(call, call.uniformOffset + call.uniformStride, call.image);
				if (switchedProg) {
					useProgramCached(stencilProg);
				}
				checkGLError(flags, "stroke fill 0");
				for (int i = 0; i < npaths; i++) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[i].strokeOffset, callPaths[i].strokeCount);
				}

				bindCallShader(call, call.uniformOffset, call.image);
				stencilMask(0x00);
				stencilFunc(GL_EQUAL, clipRef, 0xf0);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				for (int i = 0; i < npaths; i++) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[i].strokeOffset, callPaths[i].strokeCount);
				}

				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				if (simpleShader.prog != 0) useProgramCached(simpleShader.prog);
				stencilMask(0x0f);
				stencilFunc(GL_NOTEQUAL, 0x00, 0x0f);
				glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
				checkGLError(flags, "stroke fill 1");
				for (int i = 0; i < npaths; i++) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[i].strokeOffset, callPaths[i].strokeCount);
				}
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				stencilMask(0x00);
				stencilFunc(GL_EQUAL, clipRef, 0xf0);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			} else {
				stencilMask(0xff);
				stencilFunc(GL_EQUAL, 0x0, 0xff);
				glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
				bindCallShader(call, call.uniformOffset + call.uniformStride, call.image);
				if (switchedProg) {
					useProgramCached(stencilProg);
				}
				checkGLError(flags, "stroke fill 0");
				for (int i = 0; i < npaths; i++) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[i].strokeOffset, callPaths[i].strokeCount);
				}

				bindCallShader(call, call.uniformOffset, call.image);
				stencilFunc(GL_EQUAL, 0x00, 0xff);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				for (int i = 0; i < npaths; i++) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[i].strokeOffset, callPaths[i].strokeCount);
				}

				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				if (simpleShader.prog != 0) useProgramCached(simpleShader.prog);
				stencilFunc(GL_ALWAYS, 0, 0xff);
				glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
				checkGLError(flags, "stroke fill 1");
				for (int i = 0; i < npaths; i++) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[i].strokeOffset, callPaths[i].strokeCount);
				}
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				enableStencil(false);
			}
			unbindCallShader(call);
		} else {
			if (activeClipDepth > 0) {
				enableStencil(true);
				stencilMask(0x00);
				stencilFunc(GL_EQUAL, activeClipDepth << 4, 0xf0);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			} else {
				enableStencil(false);
			}
			bindCallShader(call, call.uniformOffset, call.image);
			checkGLError(flags, "stroke");
			if (npaths == 1) {
				if (callPaths[0].strokeCount > 0) {
					glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + callPaths[0].strokeOffset, callPaths[0].strokeCount);
				}
			} else {
				stripFirst.clear();
				stripCount.clear();
				stripFirst.reserve(npaths);
				stripCount.reserve(npaths);
				for (int i = 0; i < npaths; i++) {
					if (callPaths[i].strokeCount > 0) {
						stripFirst.push_back(baseVertOffset + callPaths[i].strokeOffset);
						stripCount.push_back(callPaths[i].strokeCount);
					}
				}
				if (!stripFirst.empty()) {
					glMultiDrawArrays(GL_TRIANGLE_STRIP, stripFirst.data(), stripCount.data(), static_cast<GLsizei>(stripFirst.size()));
				}
			}
			unbindCallShader(call);
		}
	}

	void triangles(const Call& call, int activeClipDepth) {
		if (activeClipDepth > 0) {
			enableStencil(true);
			stencilMask(0x00);
			stencilFunc(GL_EQUAL, activeClipDepth << 4, 0xf0);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		} else {
			enableStencil(false);
		}
		bindCallShader(call, call.uniformOffset, call.image);
		checkGLError(flags, "triangles fill");
		glDisable(GL_CULL_FACE);
		glDrawArrays(GL_TRIANGLES, baseVertOffset + call.triangleOffset, call.triangleCount);
		glEnable(GL_CULL_FACE);
		unbindCallShader(call);
	}

	void clip(const Call& call, int& activeClipDepth) {
		const GLPath* callPaths = &paths[call.pathOffset];
		int npaths = call.pathCount;
		int prevDepth = call.clipDepth - 1;
		int prevDepthVal = prevDepth << 4;
		int newDepthVal = call.clipDepth << 4;

		enableStencil(true);
		glDisable(GL_CULL_FACE);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glDepthMask(GL_FALSE);

		stencilMask(0x0f);
		stencilFunc(GL_EQUAL, prevDepthVal, 0xf0);

		if (simpleShader.prog != 0) useProgramCached(simpleShader.prog);
		checkGLError(flags, "clip path");

		if (call.winding == Winding::Clockwise) {
			glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
		} else {
			glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
			glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
		}

		for (int i = 0; i < npaths; i++) {
			glDrawArrays(GL_TRIANGLE_FAN, baseVertOffset + callPaths[i].fillOffset, callPaths[i].fillCount);
		}

		stencilMask(0xff);
		stencilFunc(GL_NOTEQUAL, newDepthVal, 0x0f);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + call.triangleOffset, call.triangleCount);

		activeClipDepth = call.clipDepth;
		glEnable(GL_CULL_FACE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDepthMask(GL_TRUE);
		stencilMask(0x00);
		stencilFunc(GL_EQUAL, newDepthVal, 0xf0);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	}

	void restoreClip(const Call& call, int& activeClipDepth) {
		if (call.clipDepth < activeClipDepth) {
			int targetDepthVal = call.clipDepth << 4;
			enableStencil(true);
			glDisable(GL_CULL_FACE);
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glDepthMask(GL_FALSE);
			stencilMask(0xff);
			stencilFunc(GL_LESS, targetDepthVal, 0xf0);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

			if (simpleShader.prog != 0) useProgramCached(simpleShader.prog);
			checkGLError(flags, "restore clip");

			glDrawArrays(GL_TRIANGLE_STRIP, baseVertOffset + call.triangleOffset, call.triangleCount);

			activeClipDepth = call.clipDepth;
			glEnable(GL_CULL_FACE);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glDepthMask(GL_TRUE);
			stencilMask(0x00);

			if (activeClipDepth == 0) {
				enableStencil(false);
			} else {
				stencilFunc(GL_EQUAL, activeClipDepth << 4, 0xf0);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			}
		}
	}

	void pushLayer(const Call& call, int& activeClipDepth) {
		if (call.layerIndex < 0 || call.layerIndex >= static_cast<int>(layerDescs.size())) return;
		const LayerDesc& layer = layerDescs[call.layerIndex];

		ensureBaseTargetInfo();
		const bool inNestedLayer = !activeLayers.empty();
		const int fbW = inNestedLayer ? layerPool[activeLayers.back().poolIndex].w
		                              : ((baseViewport[2] > 0) ? baseViewport[2] : std::max(1, static_cast<int>(view[0])));
		const int fbH = inNestedLayer ? layerPool[activeLayers.back().poolIndex].h
		                              : ((baseViewport[3] > 0) ? baseViewport[3] : std::max(1, static_cast<int>(view[1])));
		const GLuint parentDrawFbo = inNestedLayer ? layerPool[activeLayers.back().poolIndex].fbo : baseDrawFbo;

		const float scaleX = (view[0] > 0.0f) ? (static_cast<float>(fbW) / view[0]) : 1.0f;
		const float scaleY = (view[1] > 0.0f) ? (static_cast<float>(fbH) / view[1]) : 1.0f;
		int roiX0 = 0, roiY0 = 0, roiX1 = fbW, roiY1 = fbH;
		if (layer.hasBounds) {
			roiX0 = std::clamp(static_cast<int>(std::floor(layer.bounds[0] * scaleX)), 0, fbW);
			roiX1 = std::clamp(static_cast<int>(std::ceil(layer.bounds[2] * scaleX)), 0, fbW);
			const int topY = std::clamp(static_cast<int>(std::floor(layer.bounds[1] * scaleY)), 0, fbH);
			const int botY = std::clamp(static_cast<int>(std::ceil(layer.bounds[3] * scaleY)), 0, fbH);
			roiY0 = fbH - botY;
			roiY1 = fbH - topY;
		}

		GLuint backdropFbo = 0;
		int backdropScale = 1;
		if (layer.copyBackdrop) {
			float snapBounds[4]{0.0f, 0.0f, view[0], view[1]};
			if (layer.hasBounds) {
				const float pad = (layer.backdropBlurSigma > 0.001f)
				                      ? (std::ceil(3.0f * layer.backdropBlurSigma) + 4.0f)
				                      : 2.0f;
				snapBounds[0] = layer.bounds[0] - pad;
				snapBounds[1] = layer.bounds[1] - pad;
				snapBounds[2] = layer.bounds[2] + pad;
				snapBounds[3] = layer.bounds[3] + pad;
			}
			captureDstSnapshot(layer.hasBounds, snapBounds, nullptr);
			backdropFbo = dstSnapFbo;
			if (layer.backdropBlurSigma > 0.001f) {
				BlurResult br = gaussianBlurToFbo(
					dstSnapFbo, dstSnapTex, fbW, fbH,
					layer.backdropBlurSigma, layer.backdropBlurSigma,
					layer.hasBounds, roiX0, roiY0, roiX1, roiY1);
				if (br.fbo != 0) {
					backdropFbo = br.fbo;
					backdropScale = br.scale;
				}
			}
		}

		const int poolIdx = acquireLayerFbo(fbW, fbH);
		const LayerFbo& lf = layerPool[poolIdx];
		glBindFramebuffer(GL_FRAMEBUFFER, lf.fbo);

		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		stencilMask(0xff);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClearStencil(0);
		if (layer.hasBounds) {
			float maxBlur = std::max(layer.postBlurSigmaX, layer.postBlurSigmaY);
			if (layer.imageFilter.has_value()) {
				maxBlur = std::max(maxBlur, layer.imageFilter->max_sigma());
			}
			if (layer.maskFilter.has_value() && layer.maskFilter->is_active()) {
				maxBlur = std::max(maxBlur, layer.maskFilter->sigma);
			}
			const int cPad = (maxBlur > 0.001f) ? (static_cast<int>(std::ceil(maxBlur * 3.0f)) + 8) : 4;
			const int cx0 = std::clamp(roiX0 - cPad, 0, fbW);
			const int cy0 = std::clamp(roiY0 - cPad, 0, fbH);
			const int cx1 = std::clamp(roiX1 + cPad, 0, fbW);
			const int cy1 = std::clamp(roiY1 + cPad, 0, fbH);
			if (cx1 > cx0 && cy1 > cy0) {
				glEnable(GL_SCISSOR_TEST);
				glScissor(cx0, cy0, cx1 - cx0, cy1 - cy0);
				glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			}
		} else {
			glDisable(GL_SCISSOR_TEST);
			glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		}

		if (layer.copyBackdrop && backdropFbo != 0 && roiX1 > roiX0 && roiY1 > roiY0) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, backdropFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lf.fbo);
			if (backdropScale > 1) {
				const int ax0 = (roiX0 / backdropScale) * backdropScale;
				const int ay0 = (roiY0 / backdropScale) * backdropScale;
				const int ax1 = std::min(fbW, ((roiX1 + backdropScale - 1) / backdropScale) * backdropScale);
				const int ay1 = std::min(fbH, ((roiY1 + backdropScale - 1) / backdropScale) * backdropScale);
				glEnable(GL_SCISSOR_TEST);
				glScissor(roiX0, roiY0, roiX1 - roiX0, roiY1 - roiY0);
				glBlitFramebuffer(ax0 / backdropScale, ay0 / backdropScale,
				                  ax1 / backdropScale, ay1 / backdropScale,
				                  ax0, ay0, ax1, ay1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			} else {
				glDisable(GL_SCISSOR_TEST);
				glBlitFramebuffer(roiX0, roiY0, roiX1, roiY1, roiX0, roiY0, roiX1, roiY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, lf.fbo);
		}

		if (scissorEnabled) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(currentScissor[0], currentScissor[1], currentScissor[2], currentScissor[3]);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
		activeLayers.push_back(RuntimeLayer{parentDrawFbo, activeClipDepth, poolIdx});
		activeClipDepth = 0;
		enableStencil(false);
	}

	void computeFilterCropUv(const std::optional<nisaba::Rect>& cropRect,
	                         const Transform2D& layerXform,
	                         int fbW, int fbH,
	                         float outUv[4]) const noexcept {
		if (!cropRect.has_value() || fbW <= 0 || fbH <= 0 || view[0] <= 0.0f || view[1] <= 0.0f) {
			outUv[0] = -1.0f; outUv[1] = -1.0f; outUv[2] = 2.0f; outUv[3] = 2.0f;
			return;
		}
		const nisaba::Transform ts = static_cast<nisaba::Transform>(layerXform);
		auto devCrop = cropRect->transform(ts);
		if (!devCrop.has_value()) {
			outUv[0] = 2.0f; outUv[1] = 2.0f; outUv[2] = -1.0f; outUv[3] = -1.0f;
			return;
		}
		const float sx = static_cast<float>(fbW) / view[0];
		const float sy = static_cast<float>(fbH) / view[1];
		const float px0 = std::floor(devCrop->left() * sx);
		const float px1 = std::ceil(devCrop->right() * sx);
		const float topY = std::floor(devCrop->top() * sy);
		const float botY = std::ceil(devCrop->bottom() * sy);
		const float glY0 = static_cast<float>(fbH) - botY;
		const float glY1 = static_cast<float>(fbH) - topY;
		outUv[0] = px0 / static_cast<float>(fbW);
		outUv[1] = glY0 / static_cast<float>(fbH);
		outUv[2] = px1 / static_cast<float>(fbW);
		outUv[3] = glY1 / static_cast<float>(fbH);
	}

	void runFilterBlendPass(GLuint dstFbo, GLuint srcTex, GLuint dstTex,
	                        int mode, float offU, float offV,
	                        const Color& tintPremul,
	                        const float roiUv[4], const float cropUv[4],
	                        int fbW, int fbH, bool additiveSourceOver) {
		glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
		glViewport(0, 0, fbW, fbH);
		glDisable(GL_SCISSOR_TEST);
		if (stencilEnabled) glDisable(GL_STENCIL_TEST);
		glDisable(GL_CULL_FACE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		if (additiveSourceOver) {
			glEnable(GL_BLEND);
			blendFuncSeparate(Blend{GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA});
		} else {
			glDisable(GL_BLEND);
		}

		useProgramCached(filterBlendShader.prog);
		if (hasSamplerObjects) {
			glBindSampler(0, 0);
			glBindSampler(1, 0);
			boundSampler = 0;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srcTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		if (dstTex != 0) {
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, dstTex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
		glActiveTexture(GL_TEXTURE0);
		glUniform1i(locFilterBlendSrc, 0);
		glUniform1i(locFilterBlendDst, 1);
		glUniform1i(locFilterBlendMode, mode);
		glUniform2f(locFilterBlendOffset, offU, offV);
		glUniform4f(locFilterBlendColor, tintPremul.r, tintPremul.g, tintPremul.b, tintPremul.a);
		glUniform4f(locFilterBlendRoiUv, roiUv[0], roiUv[1], roiUv[2], roiUv[3]);
		glUniform4f(locFilterBlendCropUv, cropUv[0], cropUv[1], cropUv[2], cropUv[3]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		boundTexture = 0;
	}

	void runFilterColorPass(GLuint dstFbo, GLuint srcTex,
	                        const effects::ColorEffect& eff,
	                        const float roiUv[4], const float cropUv[4],
	                        int fbW, int fbH,
	                        const int* scissorRect = nullptr) {
		glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
		glViewport(0, 0, fbW, fbH);
		if (scissorRect && scissorRect[2] > 0 && scissorRect[3] > 0) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
		if (stencilEnabled) glDisable(GL_STENCIL_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		useProgramCached(filterColorShader.prog);
		if (hasSamplerObjects) {
			glBindSampler(0, 0);
			boundSampler = 0;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srcTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glUniform1i(locFilterColorSrc, 0);
		int extKind = 0;
		const auto* matPtr = &eff.matrix;
		if (eff.is_extended()) {
			const auto& en = *eff.ext_node;
			if (en.kind == effects::ColorEffectExtKind::Table) {
				extKind = 1;
				int32_t packed[256];
				for (int i = 0; i < 256; ++i) {
					uint32_t w = static_cast<uint32_t>(en.lut_r[i]) |
					             (static_cast<uint32_t>(en.lut_g[i]) << 8) |
					             (static_cast<uint32_t>(en.lut_b[i]) << 16) |
					             (static_cast<uint32_t>(en.lut_a[i]) << 24);
					std::memcpy(&packed[i], &w, 4);
				}
				if (locFilterColorLut >= 0) glUniform4iv(locFilterColorLut, 64, packed);
			} else if (en.kind == effects::ColorEffectExtKind::HslaMatrix) {
				extKind = 2;
				matPtr = &en.hsla_matrix;
			} else if (en.kind == effects::ColorEffectExtKind::Luma) {
				extKind = 3;
			} else if (en.kind == effects::ColorEffectExtKind::HighContrast) {
				extKind = 4;
				if (locFilterColorHcParams >= 0) {
					glUniform4f(locFilterColorHcParams,
					            en.high_contrast.grayscale ? 1.0f : 0.0f,
					            static_cast<float>(en.high_contrast.invert_style),
					            en.high_contrast.contrast, 0.0f);
				}
			}
		}
		if (locFilterColorExtKind >= 0) glUniform1i(locFilterColorExtKind, extKind);
		glUniform1i(locFilterColorHasMat, eff.has_matrix ? 1 : 0);
		const auto& m = matPtr->m;
		glUniform4f(locFilterColorMat0, m[0], m[1], m[2], m[3]);
		glUniform4f(locFilterColorMat1, m[5], m[6], m[7], m[8]);
		glUniform4f(locFilterColorMat2, m[10], m[11], m[12], m[13]);
		glUniform4f(locFilterColorMat3, m[15], m[16], m[17], m[18]);
		glUniform4f(locFilterColorMatBias, m[4], m[9], m[14], m[19]);
		glUniform1i(locFilterColorTransfer, static_cast<int>(eff.transfer));
		glUniform1i(locFilterColorHasBlend, eff.has_blend ? 1 : 0);
		glUniform1i(locFilterColorBlendMode, eff.has_blend ? (static_cast<int>(eff.blend_mode) + 1) : 0);
		const Color bc = Color(eff.blend_color).premultiplied();
		glUniform4f(locFilterColorBlendCol, bc.r, bc.g, bc.b, bc.a);
		glUniform4f(locFilterColorRoiUv, roiUv[0], roiUv[1], roiUv[2], roiUv[3]);
		glUniform4f(locFilterColorCropUv, cropUv[0], cropUv[1], cropUv[2], cropUv[3]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		boundTexture = 0;
	}

	int evalColorEffectDag(const effects::ColorEffect& eff,
	                       int srcIdx, int fbW, int fbH,
	                       const float roiUv[4], const float cropUv[4],
	                       bool hasRoi, int roiX0, int roiY0, int roiX1, int roiY1,
	                       const int* scissorRect = nullptr) {
		const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
		auto acquireCleared = [&]() -> int {
			const int idx = acquireLayerFbo(fbW, fbH);
			glBindFramebuffer(GL_FRAMEBUFFER, layerPool[idx].fbo);
			if (scissorRect && scissorRect[2] > 0 && scissorRect[3] > 0) {
				glEnable(GL_SCISSOR_TEST);
				glScissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
			} else {
				glDisable(GL_SCISSOR_TEST);
			}
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			return idx;
		};
		if (eff.is_extended()) {
			const auto& en = *eff.ext_node;
			if (en.kind == effects::ColorEffectExtKind::Compose) {
				const int innerIdx = (en.child1 && en.child1->is_active())
					? evalColorEffectDag(*en.child1, srcIdx, fbW, fbH, roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1, scissorRect)
					: srcIdx;
				const int outIdx = (en.child0 && en.child0->is_active())
					? evalColorEffectDag(*en.child0, innerIdx, fbW, fbH, roiUv, cropUv, hasRoi, roiX0, roiY0, roiX1, roiY1, scissorRect)
					: innerIdx;
				if (innerIdx != srcIdx && innerIdx != outIdx) layerPool[innerIdx].inUse = false;
				return outIdx;
			}
			if (en.kind == effects::ColorEffectExtKind::Lerp) {
				const int idx0 = (en.child0 && en.child0->is_active())
					? evalColorEffectDag(*en.child0, srcIdx, fbW, fbH, roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1, scissorRect)
					: srcIdx;
				const int idx1 = (en.child1 && en.child1->is_active())
					? evalColorEffectDag(*en.child1, srcIdx, fbW, fbH, roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1, scissorRect)
					: srcIdx;
				const int outIdx = acquireCleared();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				p.cropDev[0] = cropUv[0] * static_cast<float>(fbW);
				p.cropDev[1] = (1.0f - cropUv[3]) * static_cast<float>(fbH);
				p.cropDev[2] = cropUv[2] * static_cast<float>(fbW);
				p.cropDev[3] = (1.0f - cropUv[1]) * static_cast<float>(fbH);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = 1.0f / static_cast<float>(std::max(1, fbW));
				p.fbSize[3] = 1.0f / static_cast<float>(std::max(1, fbH));
				p.i0[0] = 6; p.i0[1] = 1;
				p.p0[0] = 0.0f; p.p0[1] = en.lerp_t; p.p0[2] = 1.0f - en.lerp_t; p.p0[3] = 0.0f;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[idx1].colorTex, layerPool[idx0].colorTex, p, fbW, fbH);
				if (idx0 != srcIdx) layerPool[idx0].inUse = false;
				if (idx1 != srcIdx && idx1 != idx0) layerPool[idx1].inUse = false;
				return outIdx;
			}
		}
		if (eff.is_active()) {
			const int outIdx = acquireLayerFbo(fbW, fbH);
			runFilterColorPass(layerPool[outIdx].fbo, layerPool[srcIdx].colorTex, eff, roiUv, cropUv, fbW, fbH, scissorRect);
			return outIdx;
		}
		const int outIdx = acquireCleared();
		runFilterBlendPass(layerPool[outIdx].fbo, layerPool[srcIdx].colorTex, 0,
		                   0, 0.0f, 0.0f, Color(1, 1, 1, 1), roiUv, cropUv, fbW, fbH, false);
		return outIdx;
	}

	RuntimeBlenderProg* getOrCreateRuntimeBlenderProg(const effects::RuntimeFilterSpec& spec) {
		if (auto it = runtimeBlenderCache.find(spec.shader_hash); it != runtimeBlenderCache.end()) {
			return (it->second.shader.prog != 0) ? &it->second : nullptr;
		}
		const bool isGles = detail::is_gles_or_webgl();
		const char* header = isGles
			? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
			: "#version 150 core\n";
		static const char* rbVert =
			"out vec2 vUv;\n"
			"void main(void) {\n"
			"	vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);\n"
			"	vUv = 0.5 * pos + vec2(0.5);\n"
			"	gl_Position = vec4(pos, 0.0, 1.0);\n"
			"}\n";
		std::string frag;
		frag.reserve(spec.transformed_glsl.size() + 1024);
		frag +=
			"in vec2 vUv;\n"
			"out vec4 outColor;\n"
			"uniform sampler2D uSrcTex;\n"
			"uniform sampler2D uDstTex;\n"
			"uniform float uOpacity;\n"
			"float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
			"vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
			"vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
			"vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
			"vec4 unpremul(vec4 c) { return vec4(c.a > 0.0001 ? c.rgb / c.a : vec3(0.0), c.a); }\n";
		frag += spec.transformed_glsl;
		frag +=
			"\nvoid main(void) {\n"
			"	vec4 src = texture(uSrcTex, vUv) * uOpacity;\n"
			"	vec4 dst = texture(uDstTex, vUv);\n"
			"	vec4 c = user_main(src, dst);\n"
			"	float a = clamp(c.a, 0.0, 1.0);\n"
			"	outColor = vec4(clamp(c.rgb, vec3(0.0), vec3(a)), a);\n"
			"}\n";
		RuntimeBlenderProg entry{};
		if (createShader(entry.shader, "runtime_blender_shader", header, nullptr, rbVert, frag.c_str())) {
			const GLuint p = entry.shader.prog;
			entry.locSrcTex = glGetUniformLocation(p, "uSrcTex");
			entry.locDstTex = glGetUniformLocation(p, "uDstTex");
			entry.locOpacity = glGetUniformLocation(p, "uOpacity");
			entry.userUniformLocs.reserve(spec.uniforms.size());
			for (const auto& u : spec.uniforms) {
				entry.userUniformLocs.push_back(glGetUniformLocation(p, u.name.c_str()));
			}
		}
		auto [insIt, _] = runtimeBlenderCache.emplace(spec.shader_hash, std::move(entry));
		return (insIt->second.shader.prog != 0) ? &insIt->second : nullptr;
	}

	void runFilterRuntimeBlenderPass(GLuint dstFbo, GLuint srcTex, GLuint bgTex,
	                                 const effects::RuntimeFilterSpec& spec,
	                                 float opacity, int fbW, int fbH) {
		RuntimeBlenderProg* rbp = getOrCreateRuntimeBlenderProg(spec);
		if (!rbp) return;
		glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
		glViewport(0, 0, fbW, fbH);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_STENCIL_TEST);
		stencilEnabled = false;
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		useProgramCached(rbp->shader.prog);
		if (hasSamplerObjects) {
			glBindSampler(0, 0);
			glBindSampler(1, 0);
			boundSampler = 0;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srcTex);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, bgTex);
		glActiveTexture(GL_TEXTURE0);
		if (rbp->locSrcTex >= 0) glUniform1i(rbp->locSrcTex, 0);
		if (rbp->locDstTex >= 0) glUniform1i(rbp->locDstTex, 1);
		if (rbp->locOpacity >= 0) glUniform1f(rbp->locOpacity, opacity);
		for (size_t idx = 0; idx < spec.uniforms.size() && idx < rbp->userUniformLocs.size(); ++idx) {
			const GLint loc = rbp->userUniformLocs[idx];
			const auto& u = spec.uniforms[idx];
			if (loc < 0 || u.offset + u.size_bytes > spec.uniform_data.size()) continue;
			const uint8_t* ptr = spec.uniform_data.data() + u.offset;
			switch (u.type) {
				case effects::RuntimeUniformType::Float: glUniform1fv(loc, u.count, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Float2: glUniform2fv(loc, u.count, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Float3: glUniform3fv(loc, u.count, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Float4: glUniform4fv(loc, u.count, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Mat2: glUniformMatrix2fv(loc, u.count, GL_FALSE, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Mat3: glUniformMatrix3fv(loc, u.count, GL_FALSE, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Mat4: glUniformMatrix4fv(loc, u.count, GL_FALSE, reinterpret_cast<const float*>(ptr)); break;
				case effects::RuntimeUniformType::Int: glUniform1iv(loc, u.count, reinterpret_cast<const int*>(ptr)); break;
			}
		}
		glDrawArrays(GL_TRIANGLES, 0, 3);
		boundTexture = 0;
	}

	RuntimeFilterProg* getOrCreateRuntimeFilterProg(const effects::RuntimeFilterSpec& spec) {
		auto it = runtimeFilterCache.find(spec.shader_hash);
		if (it != runtimeFilterCache.end()) {
			return (it->second.shader.prog != 0) ? &it->second : nullptr;
		}

		const bool isGles = detail::is_gles_or_webgl();
		const char* header = isGles
			? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
			: "#version 150 core\n";
		static const char* rtVert =
			"out vec2 vUv;\n"
			"void main(void) {\n"
			"	vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);\n"
			"	vUv = 0.5 * pos + vec2(0.5);\n"
			"	gl_Position = vec4(pos, 0.0, 1.0);\n"
			"}\n";

		std::string frag;
		frag.reserve(spec.transformed_glsl.size() + 1536);
		frag +=
			"in vec2 vUv;\n"
			"out vec4 outColor;\n"
			"uniform sampler2D uInputTex;\n"
			"uniform vec2 uFbSize;\n"
			"uniform vec4 uLayerMat0;\n"
			"uniform vec2 uLayerMat1;\n"
			"uniform vec4 uInvLayerMat0;\n"
			"uniform vec2 uInvLayerMat1;\n"
			"uniform vec4 uRoiUv;\n"
			"uniform vec4 uCropUv;\n"
			"float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
			"vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
			"vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
			"vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
			"vec4 unpremul(vec4 c) { return vec4(c.a > 0.0001 ? c.rgb / c.a : vec3(0.0), c.a); }\n"
			"float _nisaba_to_linear_ch(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
			"vec3 toLinearSrgb(vec3 c) { return vec3(_nisaba_to_linear_ch(c.r), _nisaba_to_linear_ch(c.g), _nisaba_to_linear_ch(c.b)); }\n"
			"vec4 toLinearSrgb(vec4 c) { return vec4(toLinearSrgb(c.rgb), c.a); }\n"
			"float _nisaba_from_linear_ch(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055; }\n"
			"vec3 fromLinearSrgb(vec3 c) { return vec3(_nisaba_from_linear_ch(c.r), _nisaba_from_linear_ch(c.g), _nisaba_from_linear_ch(c.b)); }\n"
			"vec4 fromLinearSrgb(vec4 c) { return vec4(fromLinearSrgb(c.rgb), c.a); }\n"
			"vec4 _nisaba_sample_input(vec2 lp) {\n"
			"	vec2 dp = vec2(uLayerMat0.x * lp.x + uLayerMat0.z * lp.y + uLayerMat1.x,\n"
			"	               uLayerMat0.y * lp.x + uLayerMat0.w * lp.y + uLayerMat1.y);\n"
			"	vec2 uv = vec2(dp.x / uFbSize.x, 1.0 - dp.y / uFbSize.y);\n"
			"	if (uv.x < uRoiUv.x || uv.x > uRoiUv.z || uv.y < uRoiUv.y || uv.y > uRoiUv.w) return vec4(0.0);\n"
			"	return texture(uInputTex, uv);\n"
			"}\n";
		for (const auto& ch : spec.children) {
			const std::string fn = "__nisaba_child_eval_" + std::to_string(ch.index);
			if (spec.has_input_child && ch.index == spec.input_child_index) {
				frag += "vec4 " + fn + "(vec2 p) { return _nisaba_sample_input(p); }\n";
				frag += "vec4 " + fn + "(vec2 p, vec4 c) { return _nisaba_sample_input(p); }\n";
			} else {
				frag += "vec4 " + fn + "(vec2 p) { return vec4(0.0); }\n";
				frag += "vec4 " + fn + "(vec2 p, vec4 c) { return vec4(0.0); }\n";
			}
		}
		frag += spec.transformed_glsl;
		frag +=
			"\nvoid main(void) {\n"
			"	if (vUv.x < uCropUv.x || vUv.x > uCropUv.z || vUv.y < uCropUv.y || vUv.y > uCropUv.w) {\n"
			"		outColor = vec4(0.0);\n"
			"		return;\n"
			"	}\n"
			"	vec2 devPos = vec2(vUv.x * uFbSize.x, (1.0 - vUv.y) * uFbSize.y);\n"
			"	vec2 localPos = vec2(uInvLayerMat0.x * devPos.x + uInvLayerMat0.z * devPos.y + uInvLayerMat1.x,\n"
			"	                     uInvLayerMat0.y * devPos.x + uInvLayerMat0.w * devPos.y + uInvLayerMat1.y);\n";
		if (spec.has_color_in) {
			frag += "	vec4 c = user_main(localPos, vec4(1.0));\n";
		} else {
			frag += "	vec4 c = user_main(localPos);\n";
		}
		frag +=
			"	float a = clamp(c.a, 0.0, 1.0);\n"
			"	outColor = vec4(clamp(c.rgb, vec3(0.0), vec3(a)), a);\n"
			"}\n";

		RuntimeFilterProg entry{};
		if (createShader(entry.shader, "runtime_filter_shader", header, nullptr, rtVert, frag.c_str())) {
			const GLuint p = entry.shader.prog;
			entry.locInputTex = glGetUniformLocation(p, "uInputTex");
			entry.locFbSize = glGetUniformLocation(p, "uFbSize");
			entry.locLayerMat0 = glGetUniformLocation(p, "uLayerMat0");
			entry.locLayerMat1 = glGetUniformLocation(p, "uLayerMat1");
			entry.locInvLayerMat0 = glGetUniformLocation(p, "uInvLayerMat0");
			entry.locInvLayerMat1 = glGetUniformLocation(p, "uInvLayerMat1");
			entry.locRoiUv = glGetUniformLocation(p, "uRoiUv");
			entry.locCropUv = glGetUniformLocation(p, "uCropUv");
			entry.userUniformLocs.reserve(spec.uniforms.size());
			for (const auto& u : spec.uniforms) {
				entry.userUniformLocs.push_back(glGetUniformLocation(p, u.name.c_str()));
			}
		}
		auto [insIt, _] = runtimeFilterCache.emplace(spec.shader_hash, std::move(entry));
		return (insIt->second.shader.prog != 0) ? &insIt->second : nullptr;
	}

	void runFilterRuntimePass(GLuint dstFbo, GLuint srcTex,
	                          const effects::RuntimeFilterSpec& spec,
	                          const Transform2D& layerXform,
	                          const float roiUv[4], const float cropUv[4],
	                          int fbW, int fbH,
	                          const int* scissorRect = nullptr) {
		RuntimeFilterProg* rfp = getOrCreateRuntimeFilterProg(spec);
		if (!rfp) return;

		glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
		glViewport(0, 0, fbW, fbH);
		if (scissorRect && scissorRect[2] > 0 && scissorRect[3] > 0) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
		glDisable(GL_STENCIL_TEST);
		stencilEnabled = false;
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		useProgramCached(rfp->shader.prog);
		if (hasSamplerObjects) {
			glBindSampler(0, 0);
			boundSampler = 0;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srcTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		const float sxDev = (view[0] > 0.0f) ? (static_cast<float>(fbW) / view[0]) : 1.0f;
		const float syDev = (view[1] > 0.0f) ? (static_cast<float>(fbH) / view[1]) : 1.0f;
		const auto& m = layerXform.m;
		const float a = m[0] * sxDev, b = m[1] * syDev;
		const float c = m[2] * sxDev, d = m[3] * syDev;
		const float tx = m[4] * sxDev, ty = m[5] * syDev;
		const float det = a * d - b * c;
		float ia = 1.0f, ib = 0.0f, ic = 0.0f, id = 1.0f, itx = 0.0f, ity = 0.0f;
		if (std::abs(det) > 1e-8f) {
			const float invD = 1.0f / det;
			ia = d * invD; ib = -b * invD; ic = -c * invD; id = a * invD;
			itx = (c * ty - d * tx) * invD;
			ity = (b * tx - a * ty) * invD;
		}

		if (rfp->locInputTex >= 0) glUniform1i(rfp->locInputTex, 0);
		if (rfp->locFbSize >= 0) glUniform2f(rfp->locFbSize, static_cast<float>(fbW), static_cast<float>(fbH));
		if (rfp->locLayerMat0 >= 0) glUniform4f(rfp->locLayerMat0, a, b, c, d);
		if (rfp->locLayerMat1 >= 0) glUniform2f(rfp->locLayerMat1, tx, ty);
		if (rfp->locInvLayerMat0 >= 0) glUniform4f(rfp->locInvLayerMat0, ia, ib, ic, id);
		if (rfp->locInvLayerMat1 >= 0) glUniform2f(rfp->locInvLayerMat1, itx, ity);
		if (rfp->locRoiUv >= 0) glUniform4f(rfp->locRoiUv, roiUv[0], roiUv[1], roiUv[2], roiUv[3]);
		if (rfp->locCropUv >= 0) glUniform4f(rfp->locCropUv, cropUv[0], cropUv[1], cropUv[2], cropUv[3]);

		for (size_t idx = 0; idx < spec.uniforms.size() && idx < rfp->userUniformLocs.size(); ++idx) {
			const GLint loc = rfp->userUniformLocs[idx];
			const auto& u = spec.uniforms[idx];
			if (loc < 0 || u.offset + u.size_bytes > spec.uniform_data.size()) continue;
			const uint8_t* ptr = spec.uniform_data.data() + u.offset;
			switch (u.type) {
				case effects::RuntimeUniformType::Float:
					glUniform1fv(loc, u.count, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Float2:
					glUniform2fv(loc, u.count, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Float3:
					glUniform3fv(loc, u.count, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Float4:
					glUniform4fv(loc, u.count, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Mat2:
					glUniformMatrix2fv(loc, u.count, GL_FALSE, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Mat3:
					glUniformMatrix3fv(loc, u.count, GL_FALSE, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Mat4:
					glUniformMatrix4fv(loc, u.count, GL_FALSE, reinterpret_cast<const float*>(ptr));
					break;
				case effects::RuntimeUniformType::Int:
					glUniform1iv(loc, u.count, reinterpret_cast<const int*>(ptr));
					break;
			}
		}

		glDrawArrays(GL_TRIANGLES, 0, 3);
		boundTexture = 0;
	}

	struct FilterOpsParams {
		float roiDev[4]{};
		float cropDev[4]{-1e6f, -1e6f, 1e6f, 1e6f};
		float fbSize[4]{};
		float p0[4]{};
		float p1[4]{};
		float p2[4]{};
		float p3[4]{};
		float p4[4]{};
		int32_t i0[4]{};
		float kernel[28]{};
	};

	void computeFilterCropDev(const std::optional<nisaba::Rect>& cropRect,
	                          const Transform2D& layerXform,
	                          int fbW, int fbH,
	                          float outDev[4]) const noexcept {
		if (!cropRect.has_value() || fbW <= 0 || fbH <= 0 || view[0] <= 0.0f || view[1] <= 0.0f) {
			outDev[0] = -1e6f; outDev[1] = -1e6f; outDev[2] = 1e6f; outDev[3] = 1e6f;
			return;
		}
		const nisaba::Transform ts = static_cast<nisaba::Transform>(layerXform);
		auto devCrop = cropRect->transform(ts);
		if (!devCrop.has_value()) {
			outDev[0] = 1e6f; outDev[1] = 1e6f; outDev[2] = -1e6f; outDev[3] = -1e6f;
			return;
		}
		const float sx = static_cast<float>(fbW) / view[0];
		const float sy = static_cast<float>(fbH) / view[1];
		outDev[0] = std::floor(devCrop->left() * sx);
		outDev[1] = std::floor(devCrop->top() * sy);
		outDev[2] = std::ceil(devCrop->right() * sx);
		outDev[3] = std::ceil(devCrop->bottom() * sy);
	}

	void runFilterOpsPass(GLuint dstFbo, GLuint srcTex, GLuint src2Tex,
	                      const FilterOpsParams& params, int fbW, int fbH) {
		if (filterOpsShader.prog == 0) return;
		glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
		glViewport(0, 0, fbW, fbH);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_STENCIL_TEST);
		stencilEnabled = false;
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		useProgramCached(filterOpsShader.prog);
		if (hasSamplerObjects) {
			glBindSampler(0, 0);
			glBindSampler(1, 0);
			boundSampler = 0;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srcTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, src2Tex != 0 ? src2Tex : srcTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glActiveTexture(GL_TEXTURE0);

		if (locFilterOpsSrc >= 0) glUniform1i(locFilterOpsSrc, 0);
		if (locFilterOpsSrc2 >= 0) glUniform1i(locFilterOpsSrc2, 1);
		if (locFilterOpsRoiDev >= 0) glUniform4fv(locFilterOpsRoiDev, 1, params.roiDev);
		if (locFilterOpsCropDev >= 0) glUniform4fv(locFilterOpsCropDev, 1, params.cropDev);
		if (locFilterOpsFbSize >= 0) glUniform4fv(locFilterOpsFbSize, 1, params.fbSize);
		if (locFilterOpsP0 >= 0) glUniform4fv(locFilterOpsP0, 1, params.p0);
		if (locFilterOpsP1 >= 0) glUniform4fv(locFilterOpsP1, 1, params.p1);
		if (locFilterOpsP2 >= 0) glUniform4fv(locFilterOpsP2, 1, params.p2);
		if (locFilterOpsP3 >= 0) glUniform4fv(locFilterOpsP3, 1, params.p3);
		if (locFilterOpsP4 >= 0) glUniform4fv(locFilterOpsP4, 1, params.p4);
		if (locFilterOpsI0 >= 0) glUniform4iv(locFilterOpsI0, 1, params.i0);
		if (locFilterOpsKernel >= 0) glUniform4fv(locFilterOpsKernel, 7, params.kernel);

		glDrawArrays(GL_TRIANGLES, 0, 3);
		boundTexture = 0;
	}

	int evalImageFilterDag(const effects::ImageFilterNode* node,
	                       int currentSrcPoolIdx,
	                       int fbW, int fbH,
	                       const LayerDesc& desc,
	                       bool hasRoi,
	                       int roiX0, int roiY0, int roiX1, int roiY1) {
		if (!node || node->kind == effects::ImageFilterKind::None) {
			return currentSrcPoolIdx;
		}
		if (filterBlendShader.prog == 0 || filterColorShader.prog == 0) {
			return currentSrcPoolIdx;
		}

		const float invW = 1.0f / static_cast<float>(std::max(1, fbW));
		const float invH = 1.0f / static_cast<float>(std::max(1, fbH));
		const float roiUv[4] = {
			hasRoi ? (static_cast<float>(roiX0) * invW) : 0.0f,
			hasRoi ? (static_cast<float>(roiY0) * invH) : 0.0f,
			hasRoi ? (static_cast<float>(roiX1) * invW) : 1.0f,
			hasRoi ? (static_cast<float>(roiY1) * invH) : 1.0f
		};
		const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
		float cropUv[4];
		computeFilterCropUv(node->crop_rect, desc.layerTransform, fbW, fbH, cropUv);

		const float sxDev = (view[0] > 0.0f) ? (static_cast<float>(fbW) / view[0]) : 1.0f;
		const float syDev = (view[1] > 0.0f) ? (static_cast<float>(fbH) / view[1]) : 1.0f;
		const auto& m = desc.layerTransform.m;
		const float scaleX = std::hypot(m[0], m[1]) * sxDev;
		const float scaleY = std::hypot(m[2], m[3]) * syDev;

		auto dagSamplesOutsideRoi = [](const effects::ImageFilterNode* root) -> bool {
			auto walk = [](auto&& self, const effects::ImageFilterNode* n) -> bool {
				if (!n) return false;
				switch (n->kind) {
					case effects::ImageFilterKind::Offset:
					case effects::ImageFilterKind::DropShadow:
					case effects::ImageFilterKind::MatrixTransform:
					case effects::ImageFilterKind::Tile:
					case effects::ImageFilterKind::DisplacementMap:
					case effects::ImageFilterKind::Magnifier:
						return true;
					default:
						break;
				}
				for (const auto& ch : n->inputs) {
					if (self(self, ch.get())) return true;
				}
				return false;
			};
			return walk(walk, root);
		};

		int dagScissor[4] = {0, 0, fbW, fbH};
		const int* dagScissorPtr = nullptr;
		if (hasRoi && (!desc.imageFilter.has_value() || !dagSamplesOutsideRoi(desc.imageFilter->node().get()))) {
			const float maxBlur = desc.imageFilter.has_value() ? desc.imageFilter->max_sigma() : 0.0f;
			const int cPad = (maxBlur > 0.001f) ? (static_cast<int>(std::ceil(maxBlur * 3.0f)) + 8) : 4;
			const int sx0 = std::clamp(roiX0 - cPad, 0, fbW);
			const int sy0 = std::clamp(roiY0 - cPad, 0, fbH);
			const int sx1 = std::clamp(roiX1 + cPad, 0, fbW);
			const int sy1 = std::clamp(roiY1 + cPad, 0, fbH);
			if (sx1 > sx0 && sy1 > sy0) {
				dagScissor[0] = sx0;
				dagScissor[1] = sy0;
				dagScissor[2] = sx1 - sx0;
				dagScissor[3] = sy1 - sy0;
				dagScissorPtr = dagScissor;
			}
		}

		auto acquireClearedFbo = [&]() -> int {
			const int idx = acquireLayerFbo(fbW, fbH);
			glBindFramebuffer(GL_FRAMEBUFFER, layerPool[idx].fbo);
			if (dagScissorPtr) {
				glEnable(GL_SCISSOR_TEST);
				glScissor(dagScissorPtr[0], dagScissorPtr[1], dagScissorPtr[2], dagScissorPtr[3]);
			} else {
				glDisable(GL_SCISSOR_TEST);
			}
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			return idx;
		};

		auto releaseTemp = [&](int idx) {
			if (idx >= 0 && idx != currentSrcPoolIdx) {
				layerPool[idx].inUse = false;
			}
		};

		auto evalChild = [&](size_t idx) -> int {
			if (idx < node->inputs.size() && node->inputs[idx]) {
				return evalImageFilterDag(node->inputs[idx].get(), currentSrcPoolIdx,
				                          fbW, fbH, desc, hasRoi, roiX0, roiY0, roiX1, roiY1);
			}
			return currentSrcPoolIdx;
		};

		switch (node->kind) {
			case effects::ImageFilterKind::None:
				return currentSrcPoolIdx;

			case effects::ImageFilterKind::Blur: {
				const int inIdx = evalChild(0);
				const float effSx = node->sigma_x * scaleX;
				const float effSy = node->sigma_y * scaleY;
				GLuint blurredTex = layerPool[inIdx].colorTex;
				if (effSx > 0.001f || effSy > 0.001f) {
					BlurResult br = gaussianBlurToFbo(
						layerPool[inIdx].fbo, layerPool[inIdx].colorTex,
						fbW, fbH, effSx, effSy,
						hasRoi, roiX0, roiY0, roiX1, roiY1);
					if (br.tex != 0) blurredTex = br.tex;
				}
				const int outIdx = acquireClearedFbo();
				runFilterBlendPass(layerPool[outIdx].fbo, blurredTex, 0,
				                   0, 0.0f, 0.0f, Color(1, 1, 1, 1),
				                   roiUv, cropUv, fbW, fbH, false);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::DropShadow: {
				const int fgIdx = evalChild(0);
				const int tintIdx = acquireClearedFbo();
				const Color sc = Color(node->color).premultiplied();
				runFilterBlendPass(layerPool[tintIdx].fbo, layerPool[fgIdx].colorTex, 0,
				                   -1, 0.0f, 0.0f, sc,
				                   roiUv, noCropUv, fbW, fbH, false);
				const float effSx = node->sigma_x * scaleX;
				const float effSy = node->sigma_y * scaleY;
				GLuint shTex = layerPool[tintIdx].colorTex;
				if (effSx > 0.001f || effSy > 0.001f) {
					BlurResult br = gaussianBlurToFbo(
						layerPool[tintIdx].fbo, layerPool[tintIdx].colorTex,
						fbW, fbH, effSx, effSy,
						hasRoi, roiX0, roiY0, roiX1, roiY1);
					if (br.tex != 0) shTex = br.tex;
				}
				const float effDx = (node->dx * m[0] + node->dy * m[2]) * sxDev;
				const float effDy = (node->dx * m[1] + node->dy * m[3]) * syDev;
				const int outIdx = acquireClearedFbo();
				runFilterBlendPass(layerPool[outIdx].fbo, shTex, 0,
				                   0, -effDx * invW, effDy * invH, Color(1, 1, 1, 1),
				                   roiUv, cropUv, fbW, fbH, false);
				if (!node->shadow_only) {
					runFilterBlendPass(layerPool[outIdx].fbo, layerPool[fgIdx].colorTex, 0,
					                   0, 0.0f, 0.0f, Color(1, 1, 1, 1),
					                   roiUv, cropUv, fbW, fbH, true);
				}
				releaseTemp(tintIdx);
				releaseTemp(fgIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Offset: {
				const int inIdx = evalChild(0);
				const float effDx = (node->dx * m[0] + node->dy * m[2]) * sxDev;
				const float effDy = (node->dx * m[1] + node->dy * m[3]) * syDev;
				const int outIdx = acquireClearedFbo();
				runFilterBlendPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0,
				                   0, -effDx * invW, effDy * invH, Color(1, 1, 1, 1),
				                   roiUv, cropUv, fbW, fbH, false);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::ColorFilter: {
				const int inIdx = evalChild(0);
				const int outIdx = evalColorEffectDag(node->color_effect, inIdx, fbW, fbH,
				                                      roiUv, cropUv, hasRoi, roiX0, roiY0, roiX1, roiY1, dagScissorPtr);
				if (outIdx != inIdx) releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Compose: {
				const int innerIdx = (node->inputs.size() >= 2 && node->inputs[1])
					? evalImageFilterDag(node->inputs[1].get(), currentSrcPoolIdx,
					                     fbW, fbH, desc, hasRoi, roiX0, roiY0, roiX1, roiY1)
					: currentSrcPoolIdx;
				int outIdx = (node->inputs.size() >= 1 && node->inputs[0])
					? evalImageFilterDag(node->inputs[0].get(), innerIdx,
					                     fbW, fbH, desc, hasRoi, roiX0, roiY0, roiX1, roiY1)
					: innerIdx;
				if (innerIdx != outIdx) {
					releaseTemp(innerIdx);
				}
				if (node->crop_rect.has_value()) {
					const int croppedIdx = acquireClearedFbo();
					runFilterBlendPass(layerPool[croppedIdx].fbo, layerPool[outIdx].colorTex, 0,
					                   0, 0.0f, 0.0f, Color(1, 1, 1, 1),
					                   roiUv, cropUv, fbW, fbH, false);
					releaseTemp(outIdx);
					outIdx = croppedIdx;
				}
				return outIdx;
			}

			case effects::ImageFilterKind::Merge: {
				const int outIdx = acquireClearedFbo();
				for (size_t i = 0; i < node->inputs.size(); ++i) {
					const int childIdx = evalChild(i);
					runFilterBlendPass(layerPool[outIdx].fbo, layerPool[childIdx].colorTex, 0,
					                   0, 0.0f, 0.0f, Color(1, 1, 1, 1),
					                   roiUv, cropUv, fbW, fbH, true);
					releaseTemp(childIdx);
				}
				return outIdx;
			}

			case effects::ImageFilterKind::Blend: {
				const int bgIdx = evalChild(0);
				const int fgIdx = evalChild(1);
				const int outIdx = acquireClearedFbo();
				runFilterBlendPass(layerPool[outIdx].fbo,
				                   layerPool[fgIdx].colorTex,
				                   layerPool[bgIdx].colorTex,
				                   static_cast<int>(node->blend_mode) + 1,
				                   0.0f, 0.0f, Color(1, 1, 1, 1),
				                   roiUv, cropUv, fbW, fbH, false);
				if (fgIdx != bgIdx) {
					releaseTemp(fgIdx);
				}
				releaseTemp(bgIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::RuntimeShader: {
				const int inIdx = evalChild(0);
				if (!node->runtime_spec) return inIdx;
				const int outIdx = dagScissorPtr ? acquireLayerFbo(fbW, fbH) : acquireClearedFbo();
				runFilterRuntimePass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex,
				                     *node->runtime_spec, desc.layerTransform,
				                     roiUv, cropUv, fbW, fbH, dagScissorPtr);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Dilate:
			case effects::ImageFilterKind::Erode: {
				const int inIdx = evalChild(0);
				const int op = (node->kind == effects::ImageFilterKind::Dilate) ? 1 : 2;
				const int rx = std::clamp(static_cast<int>(std::round(node->sigma_x * scaleX)), 0, 64);
				const int ry = std::clamp(static_cast<int>(std::round(node->sigma_y * scaleY)), 0, 64);
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				// Horizontal pass (uncropped)
				const int tmpIdx = acquireClearedFbo();
				p.i0[0] = op; p.i0[1] = rx;
				p.p0[0] = 1.0f; p.p0[1] = 0.0f;
				runFilterOpsPass(layerPool[tmpIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
				// Vertical pass (cropped)
				const int outIdx = acquireClearedFbo();
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.i0[1] = ry;
				p.p0[0] = 0.0f; p.p0[1] = 1.0f;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[tmpIdx].colorTex, 0, p, fbW, fbH);
				releaseTemp(tmpIdx);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::MatrixConvolution: {
				const int inIdx = evalChild(0);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 3;
				p.i0[1] = std::clamp(node->kernel_w, 1, 5);
				p.i0[2] = std::clamp(node->kernel_h, 1, 5);
				p.i0[3] = static_cast<int32_t>(node->tile_mode);
				p.p0[0] = static_cast<float>(node->kernel_offset_x);
				p.p0[1] = static_cast<float>(node->kernel_offset_y);
				p.p0[2] = node->gain;
				p.p0[3] = node->bias;
				p.p1[0] = node->convolve_alpha ? 1.0f : 0.0f;
				const size_t kn = std::min<size_t>(node->kernel.size(), 25);
				for (size_t k = 0; k < kn; ++k) p.kernel[k] = node->kernel[k];
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::DisplacementMap: {
				const int dispIdx = evalChild(0);
				const int colIdx = evalChild(1);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 4;
				p.i0[1] = static_cast<int32_t>(node->x_channel);
				p.i0[2] = static_cast<int32_t>(node->y_channel);
				p.p0[0] = node->disp_scale * scaleX;
				p.p0[1] = node->disp_scale * scaleY;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[colIdx].colorTex, layerPool[dispIdx].colorTex, p, fbW, fbH);
				if (colIdx != dispIdx) releaseTemp(colIdx);
				releaseTemp(dispIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Magnifier: {
				const int inIdx = evalChild(0);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 5;
				const nisaba::Transform ts = static_cast<nisaba::Transform>(desc.layerTransform);
				if (auto dr = node->src_rect.transform(ts)) {
					p.p0[0] = dr->left() * sxDev; p.p0[1] = dr->top() * syDev;
					p.p0[2] = dr->right() * sxDev; p.p0[3] = dr->bottom() * syDev;
				}
				p.p1[0] = node->magnifier_inset * scaleX;
				p.p1[1] = node->magnifier_inset * scaleY;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Arithmetic: {
				const int bgIdx = evalChild(0);
				const int fgIdx = evalChild(1);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 6;
				p.i0[1] = node->enforce_premul ? 1 : 0;
				p.p0[0] = node->k1; p.p0[1] = node->k2; p.p0[2] = node->k3; p.p0[3] = node->k4;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[fgIdx].colorTex, layerPool[bgIdx].colorTex, p, fbW, fbH);
				if (fgIdx != bgIdx) releaseTemp(fgIdx);
				releaseTemp(bgIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::MatrixTransform: {
				const int inIdx = evalChild(0);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 7;
				p.i0[1] = static_cast<int32_t>(node->filter_quality);
				const nisaba::Transform devScale = nisaba::Transform::from_scale(sxDev, syDev);
				const nisaba::Transform fullTs = devScale.pre_concat(static_cast<nisaba::Transform>(desc.layerTransform));
				if (auto invF = node->filter_transform.invert()) {
					if (auto invFull = fullTs.invert()) {
						const nisaba::Transform devInv = fullTs.pre_concat(*invF).pre_concat(*invFull);
						p.p0[0] = devInv.sx; p.p0[1] = devInv.ky; p.p0[2] = devInv.kx; p.p0[3] = devInv.sy;
						p.p1[0] = devInv.tx; p.p1[1] = devInv.ty;
						runFilterOpsPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
					}
				}
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Tile: {
				const int inIdx = evalChild(0);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 8;
				const nisaba::Transform ts = static_cast<nisaba::Transform>(desc.layerTransform);
				if (auto ds = node->src_rect.transform(ts)) {
					p.p0[0] = ds->left() * sxDev; p.p0[1] = ds->top() * syDev;
					p.p0[2] = ds->width() * sxDev; p.p0[3] = ds->height() * syDev;
				}
				if (auto dd = node->dst_rect.transform(ts)) {
					p.p1[0] = dd->left() * sxDev; p.p1[1] = dd->top() * syDev;
					p.p1[2] = dd->right() * sxDev; p.p1[3] = dd->bottom() * syDev;
				}
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::AlphaThreshold: {
				const int inIdx = evalChild(0);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 9;
				const nisaba::Transform ts = static_cast<nisaba::Transform>(desc.layerTransform);
				if (auto dr = node->src_rect.transform(ts)) {
					p.p0[0] = dr->left() * sxDev; p.p0[1] = dr->top() * syDev;
					p.p0[2] = dr->right() * sxDev; p.p0[3] = dr->bottom() * syDev;
				} else {
					p.p0[0] = 1e6f; p.p0[1] = 1e6f; p.p0[2] = -1e6f; p.p0[3] = -1e6f;
				}
				p.p1[0] = node->inner_min;
				p.p1[1] = node->outer_max;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
				releaseTemp(inIdx);
				return outIdx;
			}

			case effects::ImageFilterKind::Lighting: {
				const int inIdx = evalChild(0);
				const int outIdx = acquireClearedFbo();
				FilterOpsParams p{};
				p.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
				p.roiDev[1] = hasRoi ? static_cast<float>(fbH - roiY1) : 0.0f;
				p.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(fbW);
				p.roiDev[3] = hasRoi ? static_cast<float>(fbH - roiY0) : static_cast<float>(fbH);
				computeFilterCropDev(node->crop_rect, desc.layerTransform, fbW, fbH, p.cropDev);
				p.fbSize[0] = static_cast<float>(fbW); p.fbSize[1] = static_cast<float>(fbH);
				p.fbSize[2] = invW; p.fbSize[3] = invH;
				p.i0[0] = 10;
				p.i0[1] = static_cast<int32_t>(node->lighting_kind);
				p.p0[0] = node->light_pos.x; p.p0[1] = node->light_pos.y; p.p0[2] = node->light_pos.z; p.p0[3] = node->surface_scale;
				p.p1[0] = node->light_target.x; p.p1[1] = node->light_target.y; p.p1[2] = node->light_target.z; p.p1[3] = node->light_kd_ks;
				p.p2[0] = node->color.red(); p.p2[1] = node->color.green(); p.p2[2] = node->color.blue(); p.p2[3] = node->shininess;
				p.p3[0] = node->specular_exponent;
				p.p3[1] = std::cos(node->cutoff_angle * 3.14159265358979323846f / 180.0f);
				p.p3[2] = node->cutoff_angle;
				const nisaba::Transform devScale = nisaba::Transform::from_scale(sxDev, syDev);
				const nisaba::Transform fullTs = devScale.pre_concat(static_cast<nisaba::Transform>(desc.layerTransform));
				const nisaba::Transform invFull = fullTs.invert().value_or(nisaba::Transform());
				p.p4[0] = invFull.sx; p.p4[1] = invFull.ky; p.p4[2] = invFull.kx; p.p4[3] = invFull.sy;
				p.kernel[0] = invFull.tx; p.kernel[1] = invFull.ty;
				runFilterOpsPass(layerPool[outIdx].fbo, layerPool[inIdx].colorTex, 0, p, fbW, fbH);
				releaseTemp(inIdx);
				return outIdx;
			}
		}
		return currentSrcPoolIdx;
	}

	void popLayer(const Call& call, int& activeClipDepth, bool& usedProgrammableBlend) {
		if (activeLayers.empty()) return;
		const RuntimeLayer rl = activeLayers.back();
		activeLayers.pop_back();
		const int lfW = layerPool[rl.poolIndex].w;
		const int lfH = layerPool[rl.poolIndex].h;

		LayerDesc desc;
		bool hasDesc = false;
		if (call.layerIndex >= 0 && call.layerIndex < static_cast<int>(layerDescs.size())) {
			desc = layerDescs[call.layerIndex];
			hasDesc = true;
		}

		const bool doMaskBlur = hasDesc && desc.maskFilter.has_value() && desc.maskFilter->is_blur();
		const bool useGeneralDag = hasDesc && desc.imageFilter.has_value() &&
		                           desc.imageFilter->is_active() &&
		                           !desc.isDropShadow &&
		                           !(desc.postBlurSigmaX > 0.001f || desc.postBlurSigmaY > 0.001f);
		const bool doExtColor = hasDesc && desc.extColorEffect.has_value() && desc.extColorEffect->is_active();
		const bool doCustomBlender = hasDesc && desc.blender.has_value() && desc.blender->is_custom();

		int dagResultPoolIdx = -1;
		if (doMaskBlur || useGeneralDag || doExtColor || doCustomBlender) {
			int roiX0 = 0, roiY0 = 0, roiX1 = lfW, roiY1 = lfH;
			const bool hasRoi = desc.hasBounds && view[0] > 0.0f && view[1] > 0.0f;
			if (hasRoi) {
				const float sx = static_cast<float>(lfW) / view[0];
				const float sy = static_cast<float>(lfH) / view[1];
				roiX0 = std::clamp(static_cast<int>(std::floor(desc.bounds[0] * sx)), 0, lfW);
				roiX1 = std::clamp(static_cast<int>(std::ceil(desc.bounds[2] * sx)), 0, lfW);
				const int topY = std::clamp(static_cast<int>(std::floor(desc.bounds[1] * sy)), 0, lfH);
				const int botY = std::clamp(static_cast<int>(std::ceil(desc.bounds[3] * sy)), 0, lfH);
				roiY0 = lfH - botY;
				roiY1 = lfH - topY;
			}
			const bool inNestedLayer = !activeLayers.empty();
			const int vpX = inNestedLayer ? 0 : baseViewport[0];
			const int vpY = inNestedLayer ? 0 : baseViewport[1];
			const int vpW = inNestedLayer ? layerPool[activeLayers.back().poolIndex].w : baseViewport[2];
			const int vpH = inNestedLayer ? layerPool[activeLayers.back().poolIndex].h : baseViewport[3];

			int curIdx = rl.poolIndex;
			if (doMaskBlur && filterBlendShader.prog != 0) {
				const auto& mf = *desc.maskFilter;
				const float sxDev = (view[0] > 0.0f) ? (static_cast<float>(lfW) / view[0]) : 1.0f;
				const float syDev = (view[1] > 0.0f) ? (static_cast<float>(lfH) / view[1]) : 1.0f;
				float effSx = mf.sigma * sxDev;
				float effSy = mf.sigma * syDev;
				if (mf.respect_ctm) {
					const auto& m = desc.layerTransform.m;
					effSx *= std::hypot(m[0], m[1]);
					effSy *= std::hypot(m[2], m[3]);
				}
				GLuint blurTex = layerPool[curIdx].colorTex;
				if (effSx > 0.001f || effSy > 0.001f) {
					BlurResult br = gaussianBlurToFbo(
						layerPool[curIdx].fbo, layerPool[curIdx].colorTex,
						lfW, lfH, effSx, effSy,
						hasRoi, roiX0, roiY0, roiX1, roiY1);
					if (br.tex != 0) blurTex = br.tex;
				}
				const int maskOutIdx = acquireLayerFbo(lfW, lfH);
				glBindFramebuffer(GL_FRAMEBUFFER, layerPool[maskOutIdx].fbo);
				glDisable(GL_SCISSOR_TEST);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
				glClear(GL_COLOR_BUFFER_BIT);

				const float invW = 1.0f / static_cast<float>(std::max(1, lfW));
				const float invH = 1.0f / static_cast<float>(std::max(1, lfH));
				const float roiUv[4] = {
					hasRoi ? (static_cast<float>(roiX0) * invW) : 0.0f,
					hasRoi ? (static_cast<float>(roiY0) * invH) : 0.0f,
					hasRoi ? (static_cast<float>(roiX1) * invW) : 1.0f,
					hasRoi ? (static_cast<float>(roiY1) * invH) : 1.0f
				};
				const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
				const int maskMode = -2 - static_cast<int>(mf.style);
				runFilterBlendPass(layerPool[maskOutIdx].fbo, blurTex, layerPool[curIdx].colorTex,
				                   maskMode, 0.0f, 0.0f, desc.maskTintColor.premultiplied(),
				                   roiUv, noCropUv, lfW, lfH, false);
				curIdx = maskOutIdx;
			}
			if (useGeneralDag) {
				const int nextIdx = evalImageFilterDag(desc.imageFilter->node().get(),
				                                       curIdx, lfW, lfH,
				                                       desc, hasRoi, roiX0, roiY0, roiX1, roiY1);
				if (curIdx != rl.poolIndex && curIdx != nextIdx) {
					layerPool[curIdx].inUse = false;
				}
				curIdx = nextIdx;
			}
			if (doExtColor && filterColorShader.prog != 0) {
				const float invW = 1.0f / static_cast<float>(std::max(1, lfW));
				const float invH = 1.0f / static_cast<float>(std::max(1, lfH));
				const float roiUv[4] = {
					hasRoi ? (static_cast<float>(roiX0) * invW) : 0.0f,
					hasRoi ? (static_cast<float>(roiY0) * invH) : 0.0f,
					hasRoi ? (static_cast<float>(roiX1) * invW) : 1.0f,
					hasRoi ? (static_cast<float>(roiY1) * invH) : 1.0f
				};
				const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
				const int nextIdx = evalColorEffectDag(*desc.extColorEffect, curIdx, lfW, lfH,
				                                       roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1);
				if (curIdx != rl.poolIndex && curIdx != nextIdx) {
					layerPool[curIdx].inUse = false;
				}
				curIdx = nextIdx;
			}
			if (doCustomBlender) {
				const int dstSnapIdx = acquireLayerFbo(lfW, lfH);
				glDisable(GL_SCISSOR_TEST);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, rl.parentFbo);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, layerPool[dstSnapIdx].fbo);
				glBlitFramebuffer(0, 0, lfW, lfH, 0, 0, lfW, lfH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

				const int blendOutIdx = acquireLayerFbo(lfW, lfH);
				const auto& b = *desc.blender;
				const float op = std::clamp(desc.opacity, 0.0f, 1.0f);
				if (b.kind == effects::BlenderKind::Arithmetic) {
					FilterOpsParams fp{};
					fp.roiDev[0] = 0.0f; fp.roiDev[1] = 0.0f;
					fp.roiDev[2] = static_cast<float>(lfW); fp.roiDev[3] = static_cast<float>(lfH);
					fp.fbSize[0] = static_cast<float>(lfW); fp.fbSize[1] = static_cast<float>(lfH);
					fp.fbSize[2] = 1.0f / static_cast<float>(std::max(1, lfW));
					fp.fbSize[3] = 1.0f / static_cast<float>(std::max(1, lfH));
					fp.i0[0] = 6;
					fp.i0[1] = b.enforce_premul ? 1 : 0;
					fp.p0[0] = b.k1 * op; fp.p0[1] = b.k2 * op; fp.p0[2] = b.k3; fp.p0[3] = b.k4;
					runFilterOpsPass(layerPool[blendOutIdx].fbo, layerPool[curIdx].colorTex,
					                 layerPool[dstSnapIdx].colorTex, fp, lfW, lfH);
				} else if (b.kind == effects::BlenderKind::Runtime && b.runtime_spec) {
					runFilterRuntimeBlenderPass(layerPool[blendOutIdx].fbo, layerPool[curIdx].colorTex,
					                            layerPool[dstSnapIdx].colorTex, *b.runtime_spec, op, lfW, lfH);
				}
				layerPool[dstSnapIdx].inUse = false;
				if (curIdx != rl.poolIndex) layerPool[curIdx].inUse = false;
				curIdx = blendOutIdx;
			}
			dagResultPoolIdx = curIdx;

			glViewport(vpX, vpY, vpW, vpH);
			glEnable(GL_BLEND);
			glEnable(GL_CULL_FACE);
			if (scissorEnabled) {
				glEnable(GL_SCISSOR_TEST);
				glScissor(currentScissor[0], currentScissor[1], currentScissor[2], currentScissor[3]);
			} else {
				glDisable(GL_SCISSOR_TEST);
			}
		}

		GLuint layerColorTex = (dagResultPoolIdx >= 0) ? layerPool[dagResultPoolIdx].colorTex : layerPool[rl.poolIndex].colorTex;
		const bool doBlur = !useGeneralDag && hasDesc && (desc.postBlurSigmaX > 0.001f || desc.postBlurSigmaY > 0.001f);
		GLuint blurredTex = 0;
		if (doBlur) {
			int roiX0 = 0, roiY0 = 0, roiX1 = lfW, roiY1 = lfH;
			const bool hasRoi = desc.hasBounds && view[0] > 0.0f && view[1] > 0.0f;
			if (hasRoi) {
				const float sx = static_cast<float>(lfW) / view[0];
				const float sy = static_cast<float>(lfH) / view[1];
				roiX0 = std::clamp(static_cast<int>(std::floor(desc.bounds[0] * sx)), 0, lfW);
				roiX1 = std::clamp(static_cast<int>(std::ceil(desc.bounds[2] * sx)), 0, lfW);
				const int topY = std::clamp(static_cast<int>(std::floor(desc.bounds[1] * sy)), 0, lfH);
				const int botY = std::clamp(static_cast<int>(std::ceil(desc.bounds[3] * sy)), 0, lfH);
				roiY0 = lfH - botY;
				roiY1 = lfH - topY;
			}
			BlurResult br = gaussianBlurToFbo(layerPool[rl.poolIndex].fbo, layerPool[rl.poolIndex].colorTex, lfW, lfH,
			                                  desc.postBlurSigmaX, desc.postBlurSigmaY,
			                                  hasRoi, roiX0, roiY0, roiX1, roiY1);
			if (br.tex != 0) {
				blurredTex = br.tex;
			} else {
				blurredTex = layerPool[rl.poolIndex].colorTex;
			}
		}

		auto drawQuad = [&](int triOff, int uniOff, GLuint srcTex, ShaderBucket bucket) {
			setUniforms(bucket, uniOff, 0);
			glActiveTexture(GL_TEXTURE0);
			bindTexture(srcTex);
			if (hasSamplerObjects && boundSampler != 0) {
				glBindSampler(0, 0);
				boundSampler = 0;
			}
			checkGLError(flags, "pop layer");
			glDisable(GL_CULL_FACE);
			glDrawArrays(GL_TRIANGLES, baseVertOffset + triOff, 6);
			glEnable(GL_CULL_FACE);
		};

		auto restoreParentState = [&]() {
			glBindFramebuffer(GL_FRAMEBUFFER, rl.parentFbo);
			activeClipDepth = rl.parentClipDepth;
			if (call.programmableBlend) {
				captureDstSnapshotForCall(call);
				usedProgrammableBlend = true;
			}
			if (activeClipDepth > 0) {
				enableStencil(true);
				stencilMask(0x00);
				stencilFunc(GL_EQUAL, activeClipDepth << 4, 0xf0);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			} else {
				enableStencil(false);
			}
		};

		if (hasDesc && desc.isDropShadow) {
			const GLuint shadowSrcTex = doBlur ? blurredTex : layerPool[rl.poolIndex].colorTex;
			if (call.triangleCount == 18) {
				const int compIdx = acquireLayerFbo(lfW, lfH);
				const bool inNestedLayer = !activeLayers.empty();
				const int vpX = inNestedLayer ? 0 : baseViewport[0];
				const int vpY = inNestedLayer ? 0 : baseViewport[1];
				const int vpW = inNestedLayer ? layerPool[activeLayers.back().poolIndex].w : baseViewport[2];
				const int vpH = inNestedLayer ? layerPool[activeLayers.back().poolIndex].h : baseViewport[3];
				glBindFramebuffer(GL_FRAMEBUFFER, layerPool[compIdx].fbo);
				glViewport(0, 0, lfW, lfH);
				if (scissorEnabled) glDisable(GL_SCISSOR_TEST);
				enableStencil(false);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
				glClear(GL_COLOR_BUFFER_BIT);
				blendFuncSeparate(Blend{GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA});
				drawQuad(call.triangleOffset, call.uniformOffset + call.uniformStride, shadowSrcTex, ShaderBucket::FastCore);
				drawQuad(call.triangleOffset + 6, call.uniformOffset + 2 * call.uniformStride, layerPool[rl.poolIndex].colorTex, ShaderBucket::FastCore);

				glViewport(vpX, vpY, vpW, vpH);
				if (scissorEnabled) glEnable(GL_SCISSOR_TEST);
				restoreParentState();
				blendFuncSeparate(call.blendFunc);
				drawQuad(call.triangleOffset + 12, call.uniformOffset, layerPool[compIdx].colorTex, call.bucket);
				layerPool[compIdx].inUse = false;
			} else {
				restoreParentState();
				if (call.triangleCount == 12) {
					blendFuncSeparate(Blend{GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA});
					drawQuad(call.triangleOffset, call.uniformOffset + call.uniformStride, shadowSrcTex, ShaderBucket::FastCore);
					drawQuad(call.triangleOffset + 6, call.uniformOffset, layerPool[rl.poolIndex].colorTex, call.bucket);
				} else if (call.triangleCount == 6) {
					drawQuad(call.triangleOffset, call.uniformOffset, shadowSrcTex, call.bucket);
				}
			}
			layerPool[rl.poolIndex].inUse = false;
			return;
		}

		if (doBlur) {
			layerColorTex = blurredTex;
		}

		restoreParentState();
		if (doCustomBlender) {
			blendFuncSeparate(Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO});
		} else {
			blendFuncSeparate(call.blendFunc);
		}

		if (call.triangleCount > 0) {
			drawQuad(call.triangleOffset, call.uniformOffset, layerColorTex, call.bucket);
		}

		if (dagResultPoolIdx >= 0 && dagResultPoolIdx != rl.poolIndex) {
			layerPool[dagResultPoolIdx].inUse = false;
		}
		layerPool[rl.poolIndex].inUse = false;
	}
};

// -------------------------------------------------------------
// GL3Renderer Implementation
// -------------------------------------------------------------
GL3Renderer::GL3Renderer(int flags)
	: m_impl(std::make_unique<Impl>()) {
	m_impl->flags = flags;
}

GL3Renderer::~GL3Renderer() {
	shutdown();
}

bool GL3Renderer::init() {
	detail::ensure_gl_functions_loaded();
	bool is_gles = detail::is_gles_or_webgl();
	m_impl->isGLES = is_gles;

	static const char* shaderHeaderGL =
		"#version 150 core\n"
		"#define NISABA_GL3 1\n"
		"#define USE_UNIFORMBUFFER 1\n\n";

	static const char* shaderHeaderGLES =
		"#version 300 es\n"
		"#define NISABA_GL3 1\n"
		"#define USE_UNIFORMBUFFER 1\n"
		"precision highp float;\n"
		"precision highp int;\n\n";

	const char* shaderHeader = is_gles ? shaderHeaderGLES : shaderHeaderGL;

	static const char* fillVertShader =
		"uniform vec2 viewSize;\n"
		"in vec2 vertex;\n"
		"in vec2 tcoord;\n"
		"in vec4 color;\n"
		"out vec2 ftcoord;\n"
		"out vec2 fpos;\n"
		"out vec4 fcolor;\n"
		"void main(void) {\n"
		"	ftcoord = tcoord;\n"
		"	fpos = vertex;\n"
		"	fcolor = color;\n"
		"	gl_Position = vec4(2.0*vertex.x/viewSize.x - 1.0, 1.0 - 2.0*vertex.y/viewSize.y, 0, 1);\n"
		"}\n";

	// Ultra-lightweight specialized shaders with ZERO UBO reads and ZERO branches!
	static const char* simpleVertShader =
		"uniform vec2 viewSize;\n"
		"in vec2 vertex;\n"
		"void main(void) {\n"
		"	gl_Position = vec4(2.0*vertex.x/viewSize.x - 1.0, 1.0 - 2.0*vertex.y/viewSize.y, 0, 1);\n"
		"}\n";
	static const char* simpleFragShader =
		"out vec4 outColor;\n"
		"void main(void) {\n"
		"	outColor = vec4(1.0);\n"
		"}\n";
	static const char* solidVertFragShader =
		"in vec2 ftcoord;\n"
		"in vec4 fcolor;\n"
		"out vec4 outColor;\n"
		"void main(void) {\n"
		"	float alpha = min(1.0, 1.0 - abs(ftcoord.x * 2.0 - 1.0)) * min(1.0, ftcoord.y);\n"
		"	outColor = fcolor * alpha;\n"
		"}\n";
	static const char* circleVertFragShader =
		"in vec2 ftcoord;\n"
		"in vec4 fcolor;\n"
		"out vec4 outColor;\n"
		"void main(void) {\n"
		"	float dist = length(ftcoord);\n"
		"	float alpha = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"	outColor = fcolor * alpha;\n"
		"}\n";
	static const char* rectVertFragShader =
		"in vec2 ftcoord;\n"
		"in vec4 fcolor;\n"
		"out vec4 outColor;\n"
		"void main(void) {\n"
		"	vec2 d = (vec2(1.0) - abs(ftcoord)) / max(fwidth(ftcoord), vec2(0.0001));\n"
		"	vec2 cov = clamp(d + vec2(0.5), 0.0, 1.0);\n"
		"	outColor = fcolor * (cov.x * cov.y);\n"
		"}\n";

	static const char* fastUboFields =
		"	mat3 scissorMat;\n"
		"	mat3 paintMat;\n"
		"	vec4 innerCol;\n"
		"	vec4 outerCol;\n"
		"	vec2 scissorExt;\n"
		"	vec2 scissorScale;\n"
		"	vec2 extent;\n"
		"	float radius;\n"
		"	float feather;\n"
		"	float strokeMult;\n"
		"	float strokeThr;\n"
		"	int texType;\n"
		"	int type;\n";

	static const char* fullUboTailFields =
		"	int blendMode;\n"
		"	int hasColorMatrix;\n"
		"	int gradKind;\n"
		"	int spreadMode;\n"
		"	vec4 colorMat0;\n"
		"	vec4 colorMat1;\n"
		"	vec4 colorMat2;\n"
		"	vec4 colorMat3;\n"
		"	vec4 colorMatBias;\n"
		"	int gradStopCount;\n"
		"	int colorTransfer;\n"
		"	int colorBlendEnabled;\n"
		"	int colorBlendMode;\n"
		"	vec4 gradParams0;\n"
		"	vec4 gradParams1;\n"
		"	vec4 gradStopPos[4];\n"
		"	vec4 gradStopColors[16];\n"
		"	vec4 colorBlendCol;\n";

	static const char* sdfAndScissorGlsl =
		"float sdf_rounded_rect(vec2 pt, vec2 ext, float rad) {\n"
		"	if (ext.x == rad && ext.y == rad) return length(pt) - rad;\n"
		"	vec2 ext2 = ext - vec2(rad, rad);\n"
		"	vec2 d = abs(pt) - ext2;\n"
		"	return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - rad;\n"
		"}\n"
		"float evaluate_scissor_clip(vec2 p) {\n"
		"	vec2 sc = (abs((scissorMat * vec3(p, 1.0)).xy) - scissorExt);\n"
		"	sc = vec2(0.5, 0.5) - sc * scissorScale;\n"
		"	return clamp(sc.x, 0.0, 1.0) * clamp(sc.y, 0.0, 1.0);\n"
		"}\n"
		"#ifdef EDGE_AA\n"
		"float evaluate_edge_coverage() {\n"
		"	return min(1.0, (1.0 - abs(ftcoord.x * 2.0 - 1.0)) * strokeMult) * min(1.0, ftcoord.y);\n"
		"}\n"
		"#endif\n";

	static const char* effectsHelpersGlsl =
		"float blend_channel_dodge(float sc, float dc, float sa, float da) {\n"
		"	if (dc <= 0.0) return sc * (1.0 - da);\n"
		"	float m = 0.0;\n"
		"	if (sa > 0.0) {\n"
		"		m = (sc >= sa) ? (sa * da) : min(sa * da, (dc * sa * sa) / (sa - sc));\n"
		"	}\n"
		"	return sc * (1.0 - da) + dc * (1.0 - sa) + m;\n"
		"}\n"
		"float blend_channel_burn(float sc, float dc, float sa, float da) {\n"
		"	if (dc >= da) return sa * da + sc * (1.0 - da) + dc * (1.0 - sa);\n"
		"	if (sc <= 0.0) return dc * (1.0 - sa);\n"
		"	float sub = min(sa * da, ((da - dc) * sa * sa) / sc);\n"
		"	return sc * (1.0 - da) + dc * (1.0 - sa) + (sa * da - sub);\n"
		"}\n"
		"float blend_channel_soft_light(float sc, float dc, float sa, float da) {\n"
		"	if (da <= 0.0) return sc;\n"
		"	if (sa <= 0.0) return dc;\n"
		"	float d_norm = clamp(dc / da, 0.0, 1.0);\n"
		"	float d_func = (d_norm <= 0.25)\n"
		"		? (((16.0 * d_norm - 12.0) * d_norm + 4.0) * d_norm)\n"
		"		: sqrt(d_norm);\n"
		"	float m = (2.0 * sc <= sa)\n"
		"		? (dc * (sa + (2.0 * sc - sa) * (1.0 - d_norm)))\n"
		"		: (dc * sa + (2.0 * sc - sa) * (d_func * da - dc));\n"
		"	return sc * (1.0 - da) + dc * (1.0 - sa) + m;\n"
		"}\n"
		"float hsl_lum(vec3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }\n"
		"float hsl_sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }\n"
		"vec3 hsl_clip_color(vec3 c) {\n"
		"	float l = hsl_lum(c);\n"
		"	float mn = min(min(c.r, c.g), c.b);\n"
		"	float mx = max(max(c.r, c.g), c.b);\n"
		"	if (mn < 0.0) {\n"
		"		float den = l - mn;\n"
		"		c = (den > 1e-5) ? (vec3(l) + (c - vec3(l)) * (l / den)) : vec3(l);\n"
		"	}\n"
		"	if (mx > 1.0) {\n"
		"		float den = mx - l;\n"
		"		c = (den > 1e-5) ? (vec3(l) + (c - vec3(l)) * ((1.0 - l) / den)) : vec3(l);\n"
		"	}\n"
		"	return c;\n"
		"}\n"
		"vec3 hsl_set_lum(vec3 c, float l) { return hsl_clip_color(c + vec3(l - hsl_lum(c))); }\n"
		"vec3 hsl_set_sat(vec3 c, float s) {\n"
		"	float cmin = min(min(c.r, c.g), c.b);\n"
		"	float cmax = max(max(c.r, c.g), c.b);\n"
		"	if (cmax <= cmin) return vec3(0.0);\n"
		"	return clamp((c - vec3(cmin)) * (s / (cmax - cmin)), 0.0, 1.0);\n"
		"}\n"
		"vec4 nisaba_blend(vec4 s, vec4 d, int mode) {\n"
		"	if (mode == 1) return vec4(0.0);\n"
		"	if (mode == 2) return s;\n"
		"	if (mode == 3) return d;\n"
		"	if (mode == 4) return clamp(s + d * (1.0 - s.a), 0.0, 1.0);\n"
		"	if (mode == 5) return clamp(d + s * (1.0 - d.a), 0.0, 1.0);\n"
		"	if (mode == 6) return s * d.a;\n"
		"	if (mode == 7) return d * s.a;\n"
		"	if (mode == 8) return s * (1.0 - d.a);\n"
		"	if (mode == 9) return d * (1.0 - s.a);\n"
		"	if (mode == 10) return clamp(vec4(s.rgb * d.a + d.rgb * (1.0 - s.a), d.a), 0.0, 1.0);\n"
		"	if (mode == 11) return vec4(d.rgb * s.a + s.rgb * (1.0 - d.a), s.a);\n"
		"	if (mode == 12) return clamp(s * (1.0 - d.a) + d * (1.0 - s.a), 0.0, 1.0);\n"
		"	if (mode == 13) return clamp(s + d, 0.0, 1.0);\n"
		"	if (mode == 14) return s * d;\n"
		"	if (mode == 15) return clamp(s + d - s * d, 0.0, 1.0);\n"
		"	float outA = s.a + d.a - s.a * d.a;\n"
		"	vec3 outRgb = s.rgb + d.rgb * (1.0 - s.a);\n"
		"	if (mode == 16) {\n"
		"		vec3 m = mix(vec3(s.a * d.a) - 2.0 * (d.a - d.rgb) * (s.a - s.rgb), 2.0 * s.rgb * d.rgb, lessThanEqual(2.0 * d.rgb, vec3(d.a)));\n"
		"		outRgb = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + m;\n"
		"	} else if (mode == 17) {\n"
		"		outRgb = s.rgb + d.rgb - max(s.rgb * d.a, d.rgb * s.a);\n"
		"	} else if (mode == 18) {\n"
		"		outRgb = s.rgb + d.rgb - min(s.rgb * d.a, d.rgb * s.a);\n"
		"	} else if (mode == 19) {\n"
		"		outRgb = vec3(blend_channel_dodge(s.r, d.r, s.a, d.a), blend_channel_dodge(s.g, d.g, s.a, d.a), blend_channel_dodge(s.b, d.b, s.a, d.a));\n"
		"	} else if (mode == 20) {\n"
		"		outRgb = vec3(blend_channel_burn(s.r, d.r, s.a, d.a), blend_channel_burn(s.g, d.g, s.a, d.a), blend_channel_burn(s.b, d.b, s.a, d.a));\n"
		"	} else if (mode == 21) {\n"
		"		vec3 m = mix(vec3(s.a * d.a) - 2.0 * (d.a - d.rgb) * (s.a - s.rgb), 2.0 * s.rgb * d.rgb, lessThanEqual(2.0 * s.rgb, vec3(s.a)));\n"
		"		outRgb = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + m;\n"
		"	} else if (mode == 22) {\n"
		"		outRgb = vec3(blend_channel_soft_light(s.r, d.r, s.a, d.a), blend_channel_soft_light(s.g, d.g, s.a, d.a), blend_channel_soft_light(s.b, d.b, s.a, d.a));\n"
		"	} else if (mode == 23) {\n"
		"		outRgb = s.rgb + d.rgb - 2.0 * min(s.rgb * d.a, d.rgb * s.a);\n"
		"	} else if (mode == 24) {\n"
		"		outRgb = s.rgb + d.rgb - 2.0 * s.rgb * d.rgb;\n"
		"	} else if (mode == 25) {\n"
		"		outRgb = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.rgb * d.rgb;\n"
		"	} else if (mode >= 26 && mode <= 29) {\n"
		"		if (s.a <= 0.0) return d;\n"
		"		if (d.a <= 0.0) return s;\n"
		"		vec3 sc = clamp(s.rgb / s.a, 0.0, 1.0);\n"
		"		vec3 dc = clamp(d.rgb / d.a, 0.0, 1.0);\n"
		"		vec3 c = dc;\n"
		"		if (mode == 26) c = hsl_set_lum(hsl_set_sat(sc, hsl_sat(dc)), hsl_lum(dc));\n"
		"		else if (mode == 27) c = hsl_set_lum(hsl_set_sat(dc, hsl_sat(sc)), hsl_lum(dc));\n"
		"		else if (mode == 28) c = hsl_set_lum(sc, hsl_lum(dc));\n"
		"		else c = hsl_set_lum(dc, hsl_lum(sc));\n"
		"		outRgb = s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.a * d.a * c;\n"
		"	}\n"
		"	outA = clamp(outA, 0.0, 1.0);\n"
		"	return vec4(clamp(outRgb, vec3(0.0), vec3(outA)), outA);\n"
		"}\n"
		"float srgb_ch_to_linear(float c) {\n"
		"	return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);\n"
		"}\n"
		"float linear_ch_to_srgb(float c) {\n"
		"	return (c <= 0.0031308) ? (c * 12.92) : (1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055);\n"
		"}\n"
		"vec4 apply_color_transfer(vec4 premul, int mode) {\n"
		"	if (mode == 0 || premul.a <= 1e-5) return premul;\n"
		"	vec3 s = clamp(premul.rgb / premul.a, 0.0, 1.0);\n"
		"	if (mode == 1) {\n"
		"		s = vec3(srgb_ch_to_linear(s.r), srgb_ch_to_linear(s.g), srgb_ch_to_linear(s.b));\n"
		"	} else if (mode == 2) {\n"
		"		s = vec3(linear_ch_to_srgb(s.r), linear_ch_to_srgb(s.g), linear_ch_to_srgb(s.b));\n"
		"	}\n"
		"	s = clamp(s, 0.0, 1.0);\n"
		"	return vec4(s * premul.a, premul.a);\n"
		"}\n";

	static const char* colorMatrixHelperGlsl =
		"vec4 apply_color_matrix(vec4 premul) {\n"
		"	vec4 s = (premul.a > 1e-5) ? vec4(premul.rgb / premul.a, premul.a) : vec4(0.0);\n"
		"	vec4 res = vec4(\n"
		"		dot(colorMat0, s) + colorMatBias.x,\n"
		"		dot(colorMat1, s) + colorMatBias.y,\n"
		"		dot(colorMat2, s) + colorMatBias.z,\n"
		"		dot(colorMat3, s) + colorMatBias.w\n"
		"	);\n"
		"	res = clamp(res, 0.0, 1.0);\n"
		"	return vec4(res.rgb * res.a, res.a);\n"
		"}\n";

	static const char* shadowHelpersGlsl =
		// Abramowitz & Stegun 7.1.27: exp()-free erf approximation (max error < 5e-4, exact for 8-bit color)
		"float erf_approx(float x) {\n"
		"	float ax = abs(x);\n"
		"	float p = 1.0 + ax * (0.278393 + ax * (0.230389 + ax * (0.000972 + ax * 0.078108)));\n"
		"	float p2 = p * p;\n"
		"	float y = 1.0 - 1.0 / (p2 * p2);\n"
		"	return (x < 0.0) ? -y : y;\n"
		"}\n"
		"vec4 norm_shadow_radii(vec4 r, vec2 he) {\n"
		"	vec2 sz = max(he * 2.0, vec2(1e-4));\n"
		"	vec4 c = max(r, vec4(0.0));\n"
		"	float scale = 1.0;\n"
		"	if (c.x + c.y > sz.x) scale = min(scale, sz.x / (c.x + c.y));\n"
		"	if (c.z + c.w > sz.x) scale = min(scale, sz.x / (c.z + c.w));\n"
		"	if (c.x + c.w > sz.y) scale = min(scale, sz.y / (c.x + c.w));\n"
		"	if (c.y + c.z > sz.y) scale = min(scale, sz.y / (c.y + c.z));\n"
		"	return c * scale;\n"
		"}\n"
		"float eval_rrect_shadow(vec2 p, vec2 he, vec4 radiiIn, float sigma) {\n"
		"	vec4 radii = max(radiiIn, vec4(0.0));\n"
		"	float maxR = max(max(radii.x, radii.y), max(radii.z, radii.w));\n"
		"	if (sigma < 0.1) {\n"
		"		float r = (p.y < 0.0) ? ((p.x < 0.0) ? radii.x : radii.y) : ((p.x < 0.0) ? radii.w : radii.z);\n"
		"		vec2 ext2 = he - vec2(r);\n"
		"		vec2 d = abs(p) - ext2;\n"
		"		float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"		float px = max(length(vec2(dFdx(p.x), dFdy(p.x))), 0.001);\n"
		"		return clamp(0.5 - dist / px, 0.0, 1.0);\n"
		"	}\n"
		"	float invS = 0.70710678 / sigma;\n"
		"	float spread = 3.0 * sigma;\n"
		"	float yLo = max(-he.y, p.y - spread);\n"
		"	float yHi = min( he.y, p.y + spread);\n"
		"	if (yHi <= yLo) return 0.0;\n"
		"	float xLo = max(-he.x, p.x - spread);\n"
		"	float xHi = min( he.x, p.x + spread);\n"
		"	if (xHi <= xLo) return 0.0;\n"
		"	float ay = 0.5 * (erf_approx((he.y - p.y) * invS) - erf_approx((-he.y - p.y) * invS));\n"
		"	float rTop = max(radii.x, radii.y);\n"
		"	float rBot = max(radii.w, radii.z);\n"
		"	float rLeft = max(radii.x, radii.w);\n"
		"	float rRight = max(radii.y, radii.z);\n"
		"	if (p.x >= -he.x + rLeft + spread && p.x <= he.x - rRight - spread) {\n"
		"		return clamp(ay, 0.0, 1.0);\n"
		"	}\n"
		"	if (maxR < 0.5 || (yLo >= -he.y + rTop && yHi <= he.y - rBot)) {\n"
		"		float ax = 0.5 * (erf_approx((he.x - p.x) * invS) - erf_approx((-he.x - p.x) * invS));\n"
		"		return clamp(ax * ay, 0.0, 1.0);\n"
		"	}\n"
		"	bool chkTL = (radii.x > 0.0 && yLo < -he.y + radii.x && xLo < -he.x + radii.x);\n"
		"	bool chkBL = (radii.w > 0.0 && yHi >  he.y - radii.w && xLo < -he.x + radii.w);\n"
		"	bool chkTR = (radii.y > 0.0 && yLo < -he.y + radii.y && xHi >  he.x - radii.y);\n"
		"	bool chkBR = (radii.z > 0.0 && yHi >  he.y - radii.z && xHi >  he.x - radii.z);\n"
		"	if (!chkTL && !chkBL && !chkTR && !chkBR) {\n"
		"		float ax = 0.5 * (erf_approx((he.x - p.x) * invS) - erf_approx((-he.x - p.x) * invS));\n"
		"		return clamp(ax * ay, 0.0, 1.0);\n"
		"	}\n"
		"	// 4-point Gauss-Legendre quadrature (exact to degree 7, 2x fewer iterations than 8-pt midpoint)\n"
		"	const vec4 glNodes = vec4(0.06943184, 0.33000948, 0.66999052, 0.93056816);\n"
		"	const vec4 glWeights = vec4(0.17392742, 0.32607258, 0.32607258, 0.17392742);\n"
		"	float ySpan = yHi - yLo;\n"
		"	float inv2Sig2 = 0.5 / (sigma * sigma);\n"
		"	float normG = ySpan / (2.50662827 * sigma);\n"
		"	float acc = 0.0;\n"
		"	for (int i = 0; i < 4; ++i) {\n"
		"		float y = yLo + glNodes[i] * ySpan;\n"
		"		float xL = -he.x;\n"
		"		if (chkTL && y < -he.y + radii.x) {\n"
		"			float ty = (-he.y + radii.x) - y;\n"
		"			xL = -he.x + radii.x - sqrt(max(0.0, radii.x * radii.x - ty * ty));\n"
		"		} else if (chkBL && y > he.y - radii.w) {\n"
		"			float ty = y - (he.y - radii.w);\n"
		"			xL = -he.x + radii.w - sqrt(max(0.0, radii.w * radii.w - ty * ty));\n"
		"		}\n"
		"		float xR = he.x;\n"
		"		if (chkTR && y < -he.y + radii.y) {\n"
		"			float ty = (-he.y + radii.y) - y;\n"
		"			xR = he.x - radii.y + sqrt(max(0.0, radii.y * radii.y - ty * ty));\n"
		"		} else if (chkBR && y > he.y - radii.z) {\n"
		"			float ty = y - (he.y - radii.z);\n"
		"			xR = he.x - radii.z + sqrt(max(0.0, radii.z * radii.z - ty * ty));\n"
		"		}\n"
		"		if (xR > xL) {\n"
		"			float hx = 0.5 * (erf_approx((xR - p.x) * invS) - erf_approx((xL - p.x) * invS));\n"
		"			float yd = y - p.y;\n"
		"			acc += (hx * exp(-yd * yd * inv2Sig2) * normG) * glWeights[i];\n"
		"		}\n"
		"	}\n"
		"	return clamp(acc, 0.0, 1.0);\n"
		"}\n";

	static const char* multiGradHelpersGlsl =
		"int perlin_wrap(int v, int period) {\n"
		"	if (period <= 0) return v;\n"
		"	int m = v % period;\n"
		"	return (m < 0) ? (m + period) : m;\n"
		"}\n"
		"vec2 perlin_grad2(int ix, int iy, int ch, uint seed_u) {\n"
		"	uint h = uint(ix) * 374761393u + uint(iy) * 668265263u + uint(ch) * 2147483647u + seed_u * 1274126177u;\n"
		"	h = (h ^ (h >> 13u)) * 1274126177u;\n"
		"	uint idx = (h ^ (h >> 16u)) & 7u;\n"
		"	if (idx == 0u) return vec2( 1.0,  0.0);\n"
		"	if (idx == 1u) return vec2(-1.0,  0.0);\n"
		"	if (idx == 2u) return vec2( 0.0,  1.0);\n"
		"	if (idx == 3u) return vec2( 0.0, -1.0);\n"
		"	if (idx == 4u) return vec2( 0.70710678,  0.70710678);\n"
		"	if (idx == 5u) return vec2(-0.70710678,  0.70710678);\n"
		"	if (idx == 6u) return vec2( 0.70710678, -0.70710678);\n"
		"	return vec2(-0.70710678, -0.70710678);\n"
		"}\n"
		"float perlin_2d_ch(vec2 p, int ch, uint seed_u, int px, int py) {\n"
		"	vec2 fl = floor(p);\n"
		"	int ix = int(fl.x);\n"
		"	int iy = int(fl.y);\n"
		"	vec2 t = p - fl;\n"
		"	int x0 = perlin_wrap(ix, px);\n"
		"	int x1 = perlin_wrap(ix + 1, px);\n"
		"	int y0 = perlin_wrap(iy, py);\n"
		"	int y1 = perlin_wrap(iy + 1, py);\n"
		"	float n00 = dot(perlin_grad2(x0, y0, ch, seed_u), t);\n"
		"	float n10 = dot(perlin_grad2(x1, y0, ch, seed_u), t - vec2(1.0, 0.0));\n"
		"	float n01 = dot(perlin_grad2(x0, y1, ch, seed_u), t - vec2(0.0, 1.0));\n"
		"	float n11 = dot(perlin_grad2(x1, y1, ch, seed_u), t - vec2(1.0, 1.0));\n"
		"	vec2 uv = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);\n"
		"	return mix(mix(n00, n10, uv.x), mix(n01, n11, uv.x), uv.y);\n"
		"}\n"
		"vec4 eval_perlin_noise(vec2 pt, bool isTurb) {\n"
		"	int octaves = clamp(gradStopCount, 0, 16);\n"
		"	float opacity = clamp(gradParams0.w, 0.0, 1.0);\n"
		"	if (octaves <= 0) {\n"
		"		if (isTurb) return vec4(0.0);\n"
		"		float a0 = 0.5 * opacity;\n"
		"		return vec4(0.5 * a0, 0.5 * a0, 0.5 * a0, a0);\n"
		"	}\n"
		"	uint seed_u = uint(int(floor(gradParams0.z)));\n"
		"	int px = (gradParams1.z > 0.5) ? max(1, int(gradParams1.x + 0.5)) : 0;\n"
		"	int py = (gradParams1.z > 0.5) ? max(1, int(gradParams1.y + 0.5)) : 0;\n"
		"	vec2 f = pt * gradParams0.xy;\n"
		"	float amp = 1.0;\n"
		"	vec4 acc = vec4(0.0);\n"
		"	for (int o = 0; o < 16; ++o) {\n"
		"		if (o >= octaves) break;\n"
		"		vec4 n = vec4(\n"
		"			perlin_2d_ch(f, 0, seed_u, px, py),\n"
		"			perlin_2d_ch(f, 1, seed_u, px, py),\n"
		"			perlin_2d_ch(f, 2, seed_u, px, py),\n"
		"			perlin_2d_ch(f, 3, seed_u, px, py)\n"
		"		);\n"
		"		acc += (isTurb ? abs(n) : n) * amp;\n"
		"		f *= 2.0;\n"
		"		amp *= 0.5;\n"
		"		px *= 2;\n"
		"		py *= 2;\n"
		"	}\n"
		"	vec4 col = clamp(isTurb ? acc : (acc * 0.5 + 0.5), 0.0, 1.0);\n"
		"	float a = clamp(col.a * opacity, 0.0, 1.0);\n"
		"	return vec4(col.rgb * a, a);\n"
		"}\n"
		"float get_stop_pos(int idx) {\n"
		"	vec4 v = gradStopPos[idx >> 2];\n"
		"	int r = idx & 3;\n"
		"	if (r == 0) return v.x;\n"
		"	if (r == 1) return v.y;\n"
		"	if (r == 2) return v.z;\n"
		"	return v.w;\n"
		"}\n"
		"vec4 eval_multi_gradient() {\n"
		"	vec2 pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"	if (gradKind == 5 || gradKind == 6) {\n"
		"		return eval_perlin_noise(pt, gradKind == 6);\n"
		"	}\n"
		"	float t = 0.0;\n"
		"	if (gradKind == 1) {\n"
		"		t = (gradParams0.z > 0.5) ? 1.0 : pt.x;\n"
		"	} else if (gradKind == 2) {\n"
		"		t = length(pt);\n"
		"	} else if (gradKind == 3) {\n"
		"		float angle = (pt.x == 0.0 && pt.y == 0.0) ? 0.0 : atan(pt.y, pt.x);\n"
		"		if (angle < 0.0) angle += 6.28318530718;\n"
		"		t = (angle - gradParams0.x) * gradParams0.y;\n"
		"	} else if (gradKind == 4) {\n"
		"		bool validT = true;\n"
		"		if (gradParams1.y > 0.5) {\n"
		"			float dist = length(pt);\n"
		"			t = (abs(gradParams0.w) > 1e-6) ? ((dist - gradParams0.z) / gradParams0.w) : 0.0;\n"
		"		} else {\n"
		"			float dx = gradParams0.x, dy = gradParams0.y, r0 = gradParams0.z, dr = gradParams0.w, A = gradParams1.x;\n"
		"			float B = -2.0 * (pt.x * dx + pt.y * dy + r0 * dr);\n"
		"			float C = dot(pt, pt) - r0 * r0;\n"
		"			if (abs(A) < 1e-7) {\n"
		"				if (abs(B) < 1e-7) {\n"
		"					validT = false;\n"
		"				} else {\n"
		"					t = -C / B;\n"
		"					if (r0 + t * dr < 0.0) validT = false;\n"
		"				}\n"
		"			} else {\n"
		"				float disc = B * B - 4.0 * A * C;\n"
		"				if (disc < 0.0) {\n"
		"					validT = false;\n"
		"				} else {\n"
		"					float sq = sqrt(disc);\n"
		"					float t1 = (-B - sq) / (2.0 * A);\n"
		"					float t2 = (-B + sq) / (2.0 * A);\n"
		"					bool v1 = (r0 + t1 * dr) >= -1e-4;\n"
		"					bool v2 = (r0 + t2 * dr) >= -1e-4;\n"
		"					if (v1 && !v2) t = t1;\n"
		"					else if (v2 && !v1) t = t2;\n"
		"					else if (v1 && v2) t = (dr >= 0.0) ? t1 : t2;\n"
		"					else validT = false;\n"
		"				}\n"
		"			}\n"
		"		}\n"
		"		if (!validT) return vec4(0.0);\n"
		"	}\n"
		"	float tm = t;\n"
		"	if (spreadMode == 0) {\n"
		"		tm = clamp(t, 0.0, 1.0);\n"
		"	} else if (spreadMode == 1) {\n"
		"		tm = fract(t);\n"
		"	} else {\n"
		"		float f = fract(t * 0.5);\n"
		"		tm = (f < 0.5) ? (f * 2.0) : ((1.0 - f) * 2.0);\n"
		"	}\n"
		"	if (gradStopCount < 0) {\n"
		"		return texture(tex, vec2((tm * 255.0 + 0.5) / 256.0, 0.5));\n"
		"	}\n"
		"	if (gradStopCount <= 0) return vec4(0.0);\n"
		"	vec4 c0 = gradStopColors[0];\n"
		"	if (gradStopCount == 1) {\n"
		"		return vec4(c0.rgb * c0.a, c0.a);\n"
		"	}\n"
		"	vec4 col = c0;\n"
		"	if (gradStopCount == 2) {\n"
		"		float p0 = gradStopPos[0].x;\n"
		"		float p1 = gradStopPos[0].y;\n"
		"		if (tm >= p1) {\n"
		"			col = gradStopColors[1];\n"
		"		} else if (tm > p0) {\n"
		"			float r = p1 - p0;\n"
		"			col = mix(c0, gradStopColors[1], (r > 1e-6) ? ((tm - p0) / r) : 0.0);\n"
		"		}\n"
		"	} else if (gradStopCount == 3) {\n"
		"		float p0 = gradStopPos[0].x;\n"
		"		float p1 = gradStopPos[0].y;\n"
		"		float p2 = gradStopPos[0].z;\n"
		"		if (tm >= p2) {\n"
		"			col = gradStopColors[2];\n"
		"		} else if (tm > p0) {\n"
		"			if (tm <= p1) {\n"
		"				float r = p1 - p0;\n"
		"				col = mix(c0, gradStopColors[1], (r > 1e-6) ? ((tm - p0) / r) : 0.0);\n"
		"			} else {\n"
		"				float r = p2 - p1;\n"
		"				col = mix(gradStopColors[1], gradStopColors[2], (r > 1e-6) ? ((tm - p1) / r) : 0.0);\n"
		"			}\n"
		"		}\n"
		"	} else if (gradStopCount == 4) {\n"
		"		vec4 p = gradStopPos[0];\n"
		"		if (tm >= p.w) {\n"
		"			col = gradStopColors[3];\n"
		"		} else if (tm > p.x) {\n"
		"			if (tm <= p.y) {\n"
		"				float r = p.y - p.x;\n"
		"				col = mix(c0, gradStopColors[1], (r > 1e-6) ? ((tm - p.x) / r) : 0.0);\n"
		"			} else if (tm <= p.z) {\n"
		"				float r = p.z - p.y;\n"
		"				col = mix(gradStopColors[1], gradStopColors[2], (r > 1e-6) ? ((tm - p.y) / r) : 0.0);\n"
		"			} else {\n"
		"				float r = p.w - p.z;\n"
		"				col = mix(gradStopColors[2], gradStopColors[3], (r > 1e-6) ? ((tm - p.z) / r) : 0.0);\n"
		"			}\n"
		"		}\n"
		"	} else {\n"
		"		if (tm >= get_stop_pos(gradStopCount - 1)) {\n"
		"			col = gradStopColors[gradStopCount - 1];\n"
		"		} else if (tm > get_stop_pos(0)) {\n"
		"			for (int i = 0; i < 15; ++i) {\n"
		"				if (i + 1 >= gradStopCount) break;\n"
		"				float p0 = get_stop_pos(i);\n"
		"				float p1 = get_stop_pos(i + 1);\n"
		"				if (tm <= p1) {\n"
		"					float range = p1 - p0;\n"
		"					float rel = (range > 1e-6) ? ((tm - p0) / range) : 0.0;\n"
		"					col = mix(gradStopColors[i], gradStopColors[i + 1], rel);\n"
		"					break;\n"
		"				}\n"
		"			}\n"
		"		}\n"
		"	}\n"
		"	return vec4(col.rgb * col.a, col.a);\n"
		"}\n";

	static const char* fastCoreBodyGlsl =
		"void main(void) {\n"
		"	float fragCov = 1.0;\n"
		"	if (type == 4) {\n"
		"#ifdef EDGE_AA\n"
		"		fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"		if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = fcolor * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 3) {\n"
		"		vec4 color = texture(tex, ftcoord);\n"
		"		vec4 fragCol;\n"
		"		if (texType == 2) {\n"
		"			fragCov = color.r;\n"
		"			fragCol = innerCol * fcolor;\n"
		"		} else if (texType == 3) {\n"
		"			fragCol = color * innerCol.a;\n"
		"		} else if (texType == 4) {\n"
		"			fragCol = outerCol * (color.a * innerCol.a);\n"
		"		} else {\n"
		"			if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"			fragCol = color * innerCol * fcolor;\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = fragCol * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 5) {\n"
		"		float d;\n"
		"		if (radius > 0.5) {\n"
		"			d = clamp(ftcoord.y, 0.0, 1.0);\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"			if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"			d = clamp((paintMat * vec3(fpos, 1.0)).y, 0.0, 1.0);\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = mix(innerCol, outerCol, d) * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 6) {\n"
		"		float d;\n"
		"		if (strokeMult > 3.5) {\n"
		"			float dist = length(ftcoord);\n"
		"			fragCov = clamp((strokeThr - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"			d = clamp(dist * extent.x + extent.y, 0.0, 1.0);\n"
		"		} else {\n"
		"			vec2 pt;\n"
		"			if (strokeMult > 2.5) {\n"
		"				pt = ftcoord;\n"
		"			} else if (strokeMult > 1.5) {\n"
		"				float dist = length(ftcoord);\n"
		"				fragCov = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"				pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"			} else {\n"
		"#ifdef EDGE_AA\n"
		"				fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"				if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"				pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"			}\n"
		"			d = clamp(length(pt) * extent.x + extent.y, 0.0, 1.0);\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = mix(innerCol, outerCol, d) * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 10) {\n"
		"		float dist;\n"
		"		if (extent.x > 0.0) {\n"
		"			vec2 ext2 = extent - vec2(radius, radius);\n"
		"			vec2 d = abs(ftcoord) - ext2;\n"
		"			dist = abs(min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius) - strokeThr;\n"
		"		} else if (extent.x < 0.0) {\n"
		"			vec2 rxry = vec2(-extent.x, extent.y);\n"
		"			float u = length(ftcoord);\n"
		"			float grad = max(length(ftcoord / rxry), 0.0001);\n"
		"			dist = abs(u - 1.0) / grad - strokeThr;\n"
		"		} else {\n"
		"			dist = abs(length(ftcoord) - radius) - strokeThr;\n"
		"		}\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = fcolor * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 11) {\n"
		"		float r = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"		vec2 ext2 = extent - vec2(r, r);\n"
		"		vec2 d = abs(ftcoord) - ext2;\n"
		"		float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		vec2 pt = (paintMat * vec3(ftcoord, 1.0)).xy;\n"
		"		outColor = mix(innerCol, outerCol, clamp(pt.y, 0.0, 1.0)) * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 9) {\n"
		"		float r = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"		vec2 ext2 = extent - vec2(r, r);\n"
		"		vec2 d = abs(ftcoord) - ext2;\n"
		"		float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = fcolor * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 8) {\n"
		"		vec2 d = (vec2(1.0) - abs(ftcoord)) / max(fwidth(ftcoord), vec2(0.0001));\n"
		"		vec2 cov = clamp(d + vec2(0.5), 0.0, 1.0);\n"
		"		fragCov = cov.x * cov.y;\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = fcolor * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 7) {\n"
		"		float dist = length(ftcoord);\n"
		"		fragCov = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		outColor = fcolor * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 0) {\n"
		"		vec2 pt;\n"
		"		if (strokeMult > 2.5) {\n"
		"			pt = ftcoord;\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"			if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"			pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		float d = clamp((sdf_rounded_rect(pt, extent, radius) + feather * 0.5) / feather, 0.0, 1.0);\n"
		"		outColor = mix(innerCol, outerCol, d) * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	if (type == 1) {\n"
		"		vec2 pt;\n"
		"		if (radius > 0.0) {\n"
		"			vec2 ext2 = extent - vec2(radius, radius);\n"
		"			vec2 d = abs(ftcoord) - ext2;\n"
		"			float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;\n"
		"			fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"			pt = (paintMat * vec3(ftcoord, 1.0)).xy;\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"			if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"			pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		vec4 color = texture(tex, pt);\n"
		"		if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"		else if (texType == 2) color = vec4(color.r);\n"
		"		outColor = (color * innerCol) * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	outColor = innerCol;\n"
		"}\n";

	static const char* effectsBodyGlsl =
		"void main(void) {\n"
		"	vec4 fragCol = vec4(1.0);\n"
		"	float fragCov = 1.0;\n"
		"	if (type == 2) {\n"
		"		fragCol = innerCol;\n"
		"	} else if (type == 4) {\n"
		"#ifdef EDGE_AA\n"
		"		fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"		if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 8) {\n"
		"		vec2 d = (vec2(1.0) - abs(ftcoord)) / max(fwidth(ftcoord), vec2(0.0001));\n"
		"		vec2 cov = clamp(d + vec2(0.5), 0.0, 1.0);\n"
		"		fragCov = cov.x * cov.y;\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 7) {\n"
		"		float dist = length(ftcoord);\n"
		"		fragCov = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 9) {\n"
		"		float r = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"		vec2 ext2 = extent - vec2(r, r);\n"
		"		vec2 d = abs(ftcoord) - ext2;\n"
		"		float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 3) {\n"
		"		vec4 color = texture(tex, ftcoord);\n"
		"		if (texType == 2) {\n"
		"			fragCov = color.r;\n"
		"			fragCol = innerCol * fcolor;\n"
		"		} else if (texType == 3) {\n"
		"			fragCol = color;\n"
		"		} else if (texType == 4) {\n"
		"			fragCol = outerCol * color.a;\n"
		"		} else {\n"
		"			if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"			fragCol = color * innerCol * fcolor;\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	} else if (type == 5) {\n"
		"		float d;\n"
		"		if (radius > 0.5) {\n"
		"			d = clamp(ftcoord.y, 0.0, 1.0);\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"			if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"			d = clamp((paintMat * vec3(fpos, 1.0)).y, 0.0, 1.0);\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = mix(innerCol, outerCol, d);\n"
		"	} else if (type == 6) {\n"
		"		float d;\n"
		"		if (strokeMult > 3.5) {\n"
		"			float dist = length(ftcoord);\n"
		"			fragCov = clamp((strokeThr - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"			d = clamp(dist * extent.x + extent.y, 0.0, 1.0);\n"
		"		} else {\n"
		"			vec2 pt;\n"
		"			if (strokeMult > 2.5) {\n"
		"				pt = ftcoord;\n"
		"			} else if (strokeMult > 1.5) {\n"
		"				float dist = length(ftcoord);\n"
		"				fragCov = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"				pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"			} else {\n"
		"#ifdef EDGE_AA\n"
		"				fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"				if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"				pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"			}\n"
		"			d = clamp(length(pt) * extent.x + extent.y, 0.0, 1.0);\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = mix(innerCol, outerCol, d);\n"
		"	} else if (type == 0) {\n"
		"		vec2 pt;\n"
		"		if (strokeMult > 2.5) {\n"
		"			pt = ftcoord;\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"			if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"			pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		float d = clamp((sdf_rounded_rect(pt, extent, radius) + feather * 0.5) / feather, 0.0, 1.0);\n"
		"		fragCol = mix(innerCol, outerCol, d);\n"
		"	} else if (type == 1) {\n"
		"		vec2 pt;\n"
		"		if (radius > 0.0) {\n"
		"			vec2 ext2 = extent - vec2(radius, radius);\n"
		"			vec2 d = abs(ftcoord) - ext2;\n"
		"			float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;\n"
		"			fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"			pt = (paintMat * vec3(ftcoord, 1.0)).xy;\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"			if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"			pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"		}\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		vec4 color = texture(tex, pt);\n"
		"		if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"		else if (texType == 2) color = vec4(color.r);\n"
		"		fragCol = color * innerCol;\n"
		"	} else if (type == 11) {\n"
		"		float r = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"		vec2 ext2 = extent - vec2(r, r);\n"
		"		vec2 d = abs(ftcoord) - ext2;\n"
		"		float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		vec2 pt = (paintMat * vec3(ftcoord, 1.0)).xy;\n"
		"		fragCol = mix(innerCol, outerCol, clamp(pt.y, 0.0, 1.0));\n"
		"	} else if (type == 10) {\n"
		"		float dist;\n"
		"		if (extent.x > 0.0) {\n"
		"			vec2 ext2 = extent - vec2(radius, radius);\n"
		"			vec2 d = abs(ftcoord) - ext2;\n"
		"			dist = abs(min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius) - strokeThr;\n"
		"		} else if (extent.x < 0.0) {\n"
		"			vec2 rxry = vec2(-extent.x, extent.y);\n"
		"			float u = length(ftcoord);\n"
		"			float grad = max(length(ftcoord / rxry), 0.0001);\n"
		"			dist = abs(u - 1.0) / grad - strokeThr;\n"
		"		} else {\n"
		"			dist = abs(length(ftcoord) - radius) - strokeThr;\n"
		"		}\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 12) {\n"
		"#ifdef EDGE_AA\n"
		"		fragCov = evaluate_edge_coverage();\n"
		"#ifdef STENCIL_STROKES\n"
		"		if (fragCov < strokeThr) discard;\n"
		"#endif\n"
		"#endif\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = eval_multi_gradient() * innerCol.a;\n"
		"	} else if (type == 13) {\n"
		"		fragCov = eval_rrect_shadow(ftcoord, extent, vec4(radius, feather, strokeMult, strokeThr), gradParams0.x);\n"
		"		if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		fragCol = innerCol;\n"
		"	} else if (type == 14) {\n"
		"		vec3 uvq = paintMat * vec3(fpos, 1.0);\n"
		"		if (abs(uvq.z) > 1e-7) {\n"
		"			vec2 uv = clamp((uvq.xy / uvq.z) / max(extent, vec2(1.0)), gradParams0.xy, gradParams0.zw);\n"
		"			vec4 color = texture(tex, uv);\n"
		"			if (texType == 2) {\n"
		"				fragCov = color.r;\n"
		"				fragCol = innerCol * fcolor;\n"
		"			} else if (texType == 3) {\n"
		"				fragCol = color;\n"
		"			} else if (texType == 4) {\n"
		"				fragCol = outerCol * color.a;\n"
		"			} else {\n"
		"				if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"				fragCol = color * innerCol * fcolor;\n"
		"			}\n"
		"			if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"		} else {\n"
		"			fragCov = 0.0;\n"
		"			fragCol = vec4(0.0);\n"
		"		}\n"
		"	}\n"
		"	if (hasColorMatrix != 0) {\n"
		"		fragCol = apply_color_matrix(fragCol);\n"
		"	}\n"
		"	if (colorTransfer != 0) {\n"
		"		fragCol = apply_color_transfer(fragCol, colorTransfer);\n"
		"	}\n"
		"	if (colorBlendEnabled != 0) {\n"
		"		fragCol = nisaba_blend(colorBlendCol, fragCol, colorBlendMode);\n"
		"	}\n"
		"	if ((type == 3 || type == 14) && (texType == 3 || texType == 4)) {\n"
		"		fragCol *= innerCol.a;\n"
		"	}\n"
		"	if (blendMode == 0) {\n"
		"		outColor = fragCol * fragCov;\n"
		"		return;\n"
		"	}\n"
		"	ivec2 dstSize = textureSize(dstTex, 0);\n"
		"	ivec2 dstCoord = clamp(ivec2(gl_FragCoord.xy), ivec2(0), dstSize - ivec2(1));\n"
		"	vec4 dst = texelFetch(dstTex, dstCoord, 0);\n"
		"	vec4 blended = nisaba_blend(fragCol, dst, blendMode);\n"
		"	outColor = mix(dst, blended, fragCov);\n"
		"}\n";

	const std::string fastUboBlock = std::string("layout(std140) uniform NisabaShaderParams {\n") + fastUboFields + "};\n";
	const std::string fullUboBlock = std::string("layout(std140) uniform NisabaShaderParams {\n") + fastUboFields + fullUboTailFields + "};\n";
	const std::string varyingsFull = "in vec2 ftcoord;\nin vec2 fpos;\nin vec4 fcolor;\nout vec4 outColor;\n";
	const std::string varyingsTex  = "uniform sampler2D tex;\n";

	const std::string simpleColorFrag = fastUboBlock + "out vec4 outColor;\nvoid main(void) {\n\toutColor = innerCol;\n}\n";
	const std::string rrectFrag = fastUboBlock + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float r = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"	vec2 ext2 = extent - vec2(r, r);\n"
		"	vec2 d = abs(ftcoord) - ext2;\n"
		"	float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"	float fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = fcolor * fragCov;\n"
		"}\n";
	const std::string drrectFrag = fastUboBlock + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float rO = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"	vec2 dO = abs(ftcoord) - (extent - vec2(rO));\n"
		"	float distO = min(max(dO.x, dO.y), 0.0) + length(max(dO, 0.0)) - rO;\n"
		"	vec2 pI = ftcoord - paintMat[0].xy;\n"
		"	vec2 heI = vec2(paintMat[0].z, paintMat[1].x);\n"
		"	float rI = (pI.x < 0.0) ? ((pI.y < 0.0) ? paintMat[1].y : paintMat[2].y) : ((pI.y < 0.0) ? paintMat[1].z : paintMat[2].x);\n"
		"	vec2 dI = abs(pI) - (heI - vec2(rI));\n"
		"	float distI = min(max(dI.x, dI.y), 0.0) + length(max(dI, 0.0)) - rI;\n"
		"	float dist = max(distO, -distI);\n"
		"	float fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = fcolor * fragCov;\n"
		"}\n";
	const std::string imgFrag = fastUboBlock + varyingsTex + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	vec4 color = texture(tex, ftcoord);\n"
		"	float fragCov = 1.0;\n"
		"	vec4 fragCol;\n"
		"	if (texType == 2) {\n"
		"		fragCov = color.r;\n"
		"		fragCol = innerCol * fcolor;\n"
		"	} else if (texType == 3) {\n"
		"		fragCol = color * innerCol.a;\n"
		"	} else if (texType == 4) {\n"
		"		fragCol = outerCol * (color.a * innerCol.a);\n"
		"	} else {\n"
		"		if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"		fragCol = color * innerCol * fcolor;\n"
		"	}\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = fragCol * fragCov;\n"
		"}\n";
	const std::string ringFrag = fastUboBlock + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float dist;\n"
		"	if (extent.x > 0.0) {\n"
		"		vec2 ext2 = extent - vec2(radius, radius);\n"
		"		vec2 d = abs(ftcoord) - ext2;\n"
		"		dist = abs(min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius) - strokeThr;\n"
		"	} else if (extent.x < 0.0) {\n"
		"		vec2 rxry = vec2(-extent.x, extent.y);\n"
		"		float u = length(ftcoord);\n"
		"		float grad = max(length(ftcoord / rxry), 0.0001);\n"
		"		dist = abs(u - 1.0) / grad - strokeThr;\n"
		"	} else {\n"
		"		dist = abs(length(ftcoord) - radius) - strokeThr;\n"
		"	}\n"
		"	float fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = fcolor * fragCov;\n"
		"}\n";
	const std::string linearGradFrag = fastUboBlock + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float fragCov = 1.0;\n"
		"	float d;\n"
		"	if (radius > 0.5) {\n"
		"		d = clamp(ftcoord.y, 0.0, 1.0);\n"
		"	} else {\n"
		"#ifdef EDGE_AA\n"
		"		fragCov = evaluate_edge_coverage();\n"
		"#endif\n"
		"		d = clamp((paintMat * vec3(fpos, 1.0)).y, 0.0, 1.0);\n"
		"	}\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = mix(innerCol, outerCol, d) * fragCov;\n"
		"}\n";
	const std::string radialGradFrag = fastUboBlock + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float fragCov = 1.0;\n"
		"	float d;\n"
		"	if (strokeMult > 3.5) {\n"
		"		float dist = length(ftcoord);\n"
		"		fragCov = clamp((strokeThr - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		d = clamp(dist * extent.x + extent.y, 0.0, 1.0);\n"
		"	} else {\n"
		"		vec2 pt;\n"
		"		if (strokeMult > 2.5) {\n"
		"			pt = ftcoord;\n"
		"		} else if (strokeMult > 1.5) {\n"
		"			float dist = length(ftcoord);\n"
		"			fragCov = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"			pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"		} else {\n"
		"#ifdef EDGE_AA\n"
		"			fragCov = evaluate_edge_coverage();\n"
		"#endif\n"
		"			pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"		}\n"
		"		d = clamp(length(pt) * extent.x + extent.y, 0.0, 1.0);\n"
		"	}\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = mix(innerCol, outerCol, d) * fragCov;\n"
		"}\n";
	const std::string multiGradLutFrag = fullUboBlock + varyingsTex + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float fragCov = 1.0;\n"
		"#ifdef EDGE_AA\n"
		"	fragCov = evaluate_edge_coverage();\n"
		"#endif\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	vec2 pt = (paintMat * vec3(fpos, 1.0)).xy;\n"
		"	float t = 0.0;\n"
		"	if (gradKind == 1) {\n"
		"		t = (gradParams0.z > 0.5) ? 1.0 : pt.x;\n"
		"	} else if (gradKind == 2) {\n"
		"		t = length(pt);\n"
		"	} else if (gradKind == 3) {\n"
		"		float angle = (pt.x == 0.0 && pt.y == 0.0) ? 0.0 : atan(pt.y, pt.x);\n"
		"		if (angle < 0.0) angle += 6.28318530718;\n"
		"		t = (angle - gradParams0.x) * gradParams0.y;\n"
		"	} else if (gradKind == 4) {\n"
		"		bool validT = true;\n"
		"		if (gradParams1.y > 0.5) {\n"
		"			float dist = length(pt);\n"
		"			t = (abs(gradParams0.w) > 1e-6) ? ((dist - gradParams0.z) / gradParams0.w) : 0.0;\n"
		"		} else {\n"
		"			float dx = gradParams0.x, dy = gradParams0.y, r0 = gradParams0.z, dr = gradParams0.w, A = gradParams1.x;\n"
		"			float B = -2.0 * (pt.x * dx + pt.y * dy + r0 * dr);\n"
		"			float C = dot(pt, pt) - r0 * r0;\n"
		"			if (abs(A) < 1e-7) {\n"
		"				if (abs(B) < 1e-7) validT = false;\n"
		"				else { t = -C / B; if (r0 + t * dr < 0.0) validT = false; }\n"
		"			} else {\n"
		"				float disc = B * B - 4.0 * A * C;\n"
		"				if (disc < 0.0) validT = false;\n"
		"				else {\n"
		"					float sq = sqrt(disc);\n"
		"					float t1 = (-B - sq) / (2.0 * A);\n"
		"					float t2 = (-B + sq) / (2.0 * A);\n"
		"					bool v1 = (r0 + t1 * dr) >= -1e-4;\n"
		"					bool v2 = (r0 + t2 * dr) >= -1e-4;\n"
		"					if (v1 && !v2) t = t1;\n"
		"					else if (v2 && !v1) t = t2;\n"
		"					else if (v1 && v2) t = (dr >= 0.0) ? t1 : t2;\n"
		"					else validT = false;\n"
		"				}\n"
		"			}\n"
		"		}\n"
		"		if (!validT) { outColor = vec4(0.0); return; }\n"
		"	}\n"
		"	float tm = (spreadMode == 0) ? clamp(t, 0.0, 1.0) : ((spreadMode == 1) ? fract(t) : (1.0 - abs(fract(t * 0.5) * 2.0 - 1.0)));\n"
		"	vec4 col = texture(tex, vec2((tm * 255.0 + 0.5) / 256.0, 0.5));\n"
		"	outColor = (col * innerCol.a) * fragCov;\n"
		"}\n";
	const std::string multiGradFrag = fullUboBlock + varyingsTex + varyingsFull + sdfAndScissorGlsl + multiGradHelpersGlsl +
		"void main(void) {\n"
		"	float fragCov = 1.0;\n"
		"#ifdef EDGE_AA\n"
		"	fragCov = evaluate_edge_coverage();\n"
		"#endif\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = (eval_multi_gradient() * innerCol.a) * fragCov;\n"
		"}\n";
	const std::string shadowFrag = fullUboBlock + varyingsFull + sdfAndScissorGlsl + shadowHelpersGlsl +
		"void main(void) {\n"
		"	float fragCov = eval_rrect_shadow(ftcoord, extent, vec4(radius, feather, strokeMult, strokeThr), gradParams0.x);\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = innerCol * fragCov;\n"
		"}\n";
	const std::string perspectiveImgFrag = fullUboBlock + varyingsTex + varyingsFull + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	vec3 uvq = paintMat * vec3(fpos, 1.0);\n"
		"	if (abs(uvq.z) <= 1e-7) { outColor = vec4(0.0); return; }\n"
		"	vec2 uv = clamp((uvq.xy / uvq.z) / max(extent, vec2(1.0)), gradParams0.xy, gradParams0.zw);\n"
		"	vec4 color = texture(tex, uv);\n"
		"	float fragCov = 1.0;\n"
		"	vec4 fragCol;\n"
		"	if (texType == 2) { fragCov = color.r; fragCol = innerCol * fcolor; }\n"
		"	else if (texType == 3) { fragCol = color * innerCol.a; }\n"
		"	else if (texType == 4) { fragCol = outerCol * (color.a * innerCol.a); }\n"
		"	else {\n"
		"		if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"		fragCol = color * innerCol * fcolor;\n"
		"	}\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	outColor = fragCol * fragCov;\n"
		"}\n";
	const std::string blendFastFrag = fullUboBlock + varyingsTex + "uniform sampler2D dstTex;\n" + varyingsFull +
		effectsHelpersGlsl + sdfAndScissorGlsl +
		"void main(void) {\n"
		"	float fragCov = 1.0;\n"
		"	vec4 fragCol = innerCol;\n"
		"	if (type == 4) {\n"
		"#ifdef EDGE_AA\n"
		"		fragCov = evaluate_edge_coverage();\n"
		"#endif\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 7) {\n"
		"		float dist = length(ftcoord);\n"
		"		fragCov = clamp((1.0 - dist) / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 8) {\n"
		"		vec2 d = (vec2(1.0) - abs(ftcoord)) / max(fwidth(ftcoord), vec2(0.0001));\n"
		"		vec2 cov = clamp(d + vec2(0.5), 0.0, 1.0);\n"
		"		fragCov = cov.x * cov.y;\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 9) {\n"
		"		float r = (ftcoord.x < 0.0) ? ((ftcoord.y < 0.0) ? radius : strokeThr) : ((ftcoord.y < 0.0) ? feather : strokeMult);\n"
		"		vec2 d = abs(ftcoord) - (extent - vec2(r));\n"
		"		float dist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
		"		fragCov = clamp(-dist / max(fwidth(dist), 0.0001) + 0.5, 0.0, 1.0);\n"
		"		fragCol = fcolor;\n"
		"	} else if (type == 3) {\n"
		"		vec4 color = texture(tex, ftcoord);\n"
		"		if (texType == 2) { fragCov = color.r; fragCol = innerCol * fcolor; }\n"
		"		else if (texType == 3) { fragCol = color * innerCol.a; }\n"
		"		else if (texType == 4) { fragCol = outerCol * (color.a * innerCol.a); }\n"
		"		else {\n"
		"			if (texType == 1) color = vec4(color.rgb * color.a, color.a);\n"
		"			fragCol = color * innerCol * fcolor;\n"
		"		}\n"
		"	}\n"
		"	if (scissorExt.x >= 0.0) fragCov *= evaluate_scissor_clip(fpos);\n"
		"	ivec2 dstSize = textureSize(dstTex, 0);\n"
		"	ivec2 dstCoord = clamp(ivec2(gl_FragCoord.xy), ivec2(0), dstSize - ivec2(1));\n"
		"	vec4 dst = texelFetch(dstTex, dstCoord, 0);\n"
		"	vec4 blended = nisaba_blend(fragCol, dst, blendMode);\n"
		"	outColor = mix(dst, blended, fragCov);\n"
		"}\n";
	const std::string fastCoreFrag = fastUboBlock + varyingsTex + varyingsFull + sdfAndScissorGlsl + fastCoreBodyGlsl;
	const std::string effectsFrag  = fullUboBlock + varyingsTex + "uniform sampler2D dstTex;\n" + varyingsFull +
		effectsHelpersGlsl + colorMatrixHelperGlsl + sdfAndScissorGlsl + shadowHelpersGlsl + multiGradHelpersGlsl + effectsBodyGlsl;

	checkGLError(m_impl->flags, "init start");

	auto finalizeShader = [&](GLShader& s, bool bindColorAttr) {
		if (s.prog == 0) return;
		if (bindColorAttr) {
			glBindAttribLocation(s.prog, 2, "color");
			glLinkProgram(s.prog);
		}
		getUniforms(s);
		if (static_cast<GLuint>(s.loc[LocParams]) != GL_INVALID_INDEX && s.loc[LocParams] >= 0) {
			glUniformBlockBinding(s.prog, static_cast<GLuint>(s.loc[LocParams]), ParamsBinding);
		}
		if (s.loc[LocTex] >= 0 || s.loc[LocDstTex] >= 0) {
			glUseProgram(s.prog);
			if (s.loc[LocTex] >= 0) glUniform1i(s.loc[LocTex], 0);
			if (s.loc[LocDstTex] >= 0) glUniform1i(s.loc[LocDstTex], 1);
			glUseProgram(0);
		}
	};

	std::string mainOpts;
	if (m_impl->flags & CreateFlags::Antialias) mainOpts += "#define EDGE_AA 1\n";
	const char* mainOptsPtr = mainOpts.empty() ? nullptr : mainOpts.c_str();

	if (!createShader(m_impl->shader, "shader", shaderHeader, mainOptsPtr, fillVertShader, fastCoreFrag.c_str())) {
		return false;
	}
	finalizeShader(m_impl->shader, true);

	if (createShader(m_impl->effectsShader, "effects_shader", shaderHeader, mainOptsPtr, fillVertShader, effectsFrag.c_str())) {
		finalizeShader(m_impl->effectsShader, true);
	}
	if (createShader(m_impl->rrectShader, "rrect_shader", shaderHeader, nullptr, fillVertShader, rrectFrag.c_str())) {
		finalizeShader(m_impl->rrectShader, true);
	}
	if (createShader(m_impl->drrectShader, "drrect_shader", shaderHeader, nullptr, fillVertShader, drrectFrag.c_str())) {
		finalizeShader(m_impl->drrectShader, true);
	}
	if (createShader(m_impl->imgShader, "img_shader", shaderHeader, nullptr, fillVertShader, imgFrag.c_str())) {
		finalizeShader(m_impl->imgShader, true);
	}
	if (createShader(m_impl->ringShader, "ring_shader", shaderHeader, nullptr, fillVertShader, ringFrag.c_str())) {
		finalizeShader(m_impl->ringShader, true);
	}
	if (createShader(m_impl->linearGradShader, "linear_grad_shader", shaderHeader, mainOptsPtr, fillVertShader, linearGradFrag.c_str())) {
		finalizeShader(m_impl->linearGradShader, true);
	}
	if (createShader(m_impl->radialGradShader, "radial_grad_shader", shaderHeader, mainOptsPtr, fillVertShader, radialGradFrag.c_str())) {
		finalizeShader(m_impl->radialGradShader, true);
	}
	if (createShader(m_impl->multiGradLutShader, "multi_grad_lut_shader", shaderHeader, mainOptsPtr, fillVertShader, multiGradLutFrag.c_str())) {
		finalizeShader(m_impl->multiGradLutShader, true);
	}
	if (createShader(m_impl->multiGradShader, "multi_grad_shader", shaderHeader, mainOptsPtr, fillVertShader, multiGradFrag.c_str())) {
		finalizeShader(m_impl->multiGradShader, true);
	}
	if (createShader(m_impl->shadowShader, "shadow_shader", shaderHeader, nullptr, fillVertShader, shadowFrag.c_str())) {
		finalizeShader(m_impl->shadowShader, true);
	}
	if (createShader(m_impl->perspectiveImgShader, "perspective_img_shader", shaderHeader, nullptr, fillVertShader, perspectiveImgFrag.c_str())) {
		finalizeShader(m_impl->perspectiveImgShader, true);
	}
	if (createShader(m_impl->blendFastShader, "blend_fast_shader", shaderHeader, mainOptsPtr, fillVertShader, blendFastFrag.c_str())) {
		finalizeShader(m_impl->blendFastShader, true);
	}
	if (createShader(m_impl->simpleShader, "simple_shader", shaderHeader, nullptr, simpleVertShader, simpleFragShader)) {
		finalizeShader(m_impl->simpleShader, false);
	}
	if (createShader(m_impl->simpleColorShader, "simple_color_shader", shaderHeader, nullptr, simpleVertShader, simpleColorFrag.c_str())) {
		finalizeShader(m_impl->simpleColorShader, false);
	}
	if (createShader(m_impl->solidVertShader, "solid_vert_shader", shaderHeader, nullptr, fillVertShader, solidVertFragShader)) {
		finalizeShader(m_impl->solidVertShader, true);
	}
	if (createShader(m_impl->circleVertShader, "circle_vert_shader", shaderHeader, nullptr, fillVertShader, circleVertFragShader)) {
		finalizeShader(m_impl->circleVertShader, true);
	}
	if (createShader(m_impl->rectVertShader, "rect_vert_shader", shaderHeader, nullptr, fillVertShader, rectVertFragShader)) {
		finalizeShader(m_impl->rectVertShader, true);
	}
	{
		static const char* blurVertShader =
			"out vec2 vUv;\n"
			"void main(void) {\n"
			"	vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);\n"
			"	vUv = 0.5 * pos + vec2(0.5);\n"
			"	gl_Position = vec4(pos, 0.0, 1.0);\n"
			"}\n";
		static const char* blurFragShader =
			"uniform sampler2D uTex;\n"
			"uniform vec2 uTexelOffset;\n"
			"uniform int uRadius;\n"
			"uniform float uSigma;\n"
			"uniform vec4 uUvBounds;\n"
			"in vec2 vUv;\n"
			"out vec4 outColor;\n"
			"void main(void) {\n"
			"	vec2 uvMin = uUvBounds.xy;\n"
			"	vec2 uvMax = uUvBounds.zw;\n"
			"	vec4 sum = texture(uTex, clamp(vUv, uvMin, uvMax));\n"
			"	float weightSum = 1.0;\n"
			"	if (uRadius > 0 && uSigma > 0.001) {\n"
			"		float inv2Sig2 = 1.0 / (2.0 * uSigma * uSigma);\n"
			"		// Bilinear tap pairing: exact mathematical Gaussian with 50% fewer texture fetches\n"
			"		for (int i = 1; i <= 32; i += 2) {\n"
			"			if (i > uRadius) break;\n"
			"			float f1 = float(i);\n"
			"			float w1 = exp(-(f1 * f1) * inv2Sig2);\n"
			"			if (i + 1 <= uRadius) {\n"
			"				float f2 = float(i + 1);\n"
			"				float w2 = exp(-(f2 * f2) * inv2Sig2);\n"
			"				float w12 = w1 + w2;\n"
			"				vec2 off = uTexelOffset * (f1 + w2 / max(w12, 1e-6));\n"
			"				sum += (texture(uTex, clamp(vUv + off, uvMin, uvMax)) + texture(uTex, clamp(vUv - off, uvMin, uvMax))) * w12;\n"
			"				weightSum += 2.0 * w12;\n"
			"			} else {\n"
			"				vec2 off = uTexelOffset * f1;\n"
			"				sum += (texture(uTex, clamp(vUv + off, uvMin, uvMax)) + texture(uTex, clamp(vUv - off, uvMin, uvMax))) * w1;\n"
			"				weightSum += 2.0 * w1;\n"
			"			}\n"
			"		}\n"
			"	}\n"
			"	outColor = sum / weightSum;\n"
			"}\n";
		if (createShader(m_impl->blurShader, "blur_shader", shaderHeader, nullptr, blurVertShader, blurFragShader)) {
			m_impl->locBlurTex = glGetUniformLocation(m_impl->blurShader.prog, "uTex");
			m_impl->locBlurTexelOffset = glGetUniformLocation(m_impl->blurShader.prog, "uTexelOffset");
			m_impl->locBlurRadius = glGetUniformLocation(m_impl->blurShader.prog, "uRadius");
			m_impl->locBlurSigma = glGetUniformLocation(m_impl->blurShader.prog, "uSigma");
			m_impl->locBlurUvBounds = glGetUniformLocation(m_impl->blurShader.prog, "uUvBounds");
		}
		const std::string filterBlendFrag =
			"uniform sampler2D uSrcTex;\n"
			"uniform sampler2D uDstTex;\n"
			"uniform int uMode;\n"
			"uniform vec2 uSrcOffsetUv;\n"
			"uniform vec4 uColor;\n"
			"uniform vec4 uRoiUv;\n"
			"uniform vec4 uCropUv;\n"
			"in vec2 vUv;\n"
			"out vec4 outColor;\n"
			+ std::string(effectsHelpersGlsl) +
			"void main(void) {\n"
			"	if (vUv.x < uCropUv.x || vUv.x > uCropUv.z || vUv.y < uCropUv.y || vUv.y > uCropUv.w) {\n"
			"		outColor = vec4(0.0);\n"
			"		return;\n"
			"	}\n"
			"	vec2 suv = vUv + uSrcOffsetUv;\n"
			"	vec4 s = (suv.x < uRoiUv.x || suv.x > uRoiUv.z || suv.y < uRoiUv.y || suv.y > uRoiUv.w)\n"
			"		? vec4(0.0) : texture(uSrcTex, suv);\n"
			"	if (uMode == 0) { outColor = s; return; }\n"
			"	if (uMode == -1) { outColor = uColor * s.a; return; }\n"
			"	vec4 d = (vUv.x < uRoiUv.x || vUv.x > uRoiUv.z || vUv.y < uRoiUv.y || vUv.y > uRoiUv.w)\n"
			"		? vec4(0.0) : texture(uDstTex, vUv);\n"
			"	if (uMode == -2) { outColor = uColor * s.a; return; }\n"
			"	if (uMode == -3) { outColor = uColor * max(d.a, s.a); return; }\n"
			"	if (uMode == -4) { outColor = uColor * (s.a * (1.0 - d.a)); return; }\n"
			"	if (uMode == -5) { outColor = uColor * (s.a * d.a); return; }\n"
			"	outColor = nisaba_blend(s, d, uMode);\n"
			"}\n";
		if (createShader(m_impl->filterBlendShader, "filter_blend_shader", shaderHeader, nullptr, blurVertShader, filterBlendFrag.c_str())) {
			m_impl->locFilterBlendSrc = glGetUniformLocation(m_impl->filterBlendShader.prog, "uSrcTex");
			m_impl->locFilterBlendDst = glGetUniformLocation(m_impl->filterBlendShader.prog, "uDstTex");
			m_impl->locFilterBlendMode = glGetUniformLocation(m_impl->filterBlendShader.prog, "uMode");
			m_impl->locFilterBlendOffset = glGetUniformLocation(m_impl->filterBlendShader.prog, "uSrcOffsetUv");
			m_impl->locFilterBlendColor = glGetUniformLocation(m_impl->filterBlendShader.prog, "uColor");
			m_impl->locFilterBlendRoiUv = glGetUniformLocation(m_impl->filterBlendShader.prog, "uRoiUv");
			m_impl->locFilterBlendCropUv = glGetUniformLocation(m_impl->filterBlendShader.prog, "uCropUv");
		}
		const std::string filterColorFrag =
			"uniform sampler2D uSrcTex;\n"
			"uniform int uHasMat;\n"
			"uniform vec4 uMat0;\n"
			"uniform vec4 uMat1;\n"
			"uniform vec4 uMat2;\n"
			"uniform vec4 uMat3;\n"
			"uniform vec4 uMatBias;\n"
			"uniform int uTransfer;\n"
			"uniform int uHasBlend;\n"
			"uniform int uBlendMode;\n"
			"uniform vec4 uBlendCol;\n"
			"uniform vec4 uRoiUv;\n"
			"uniform vec4 uCropUv;\n"
			"uniform int uExtKind;\n"
			"uniform vec4 uHcParams;\n"
			"uniform ivec4 uLut[64];\n"
			"in vec2 vUv;\n"
			"out vec4 outColor;\n"
			+ std::string(effectsHelpersGlsl) +
			"vec3 cf_rgb_to_hsl(vec3 c) {\n"
			"	float maxC = max(max(c.r, c.g), c.b);\n"
			"	float minC = min(min(c.r, c.g), c.b);\n"
			"	float l = 0.5 * (maxC + minC);\n"
			"	float d = maxC - minC;\n"
			"	if (d <= 1e-6) return vec3(0.0, 0.0, l);\n"
			"	float s = (l > 0.5) ? (d / max(2.0 - maxC - minC, 1e-6)) : (d / max(maxC + minC, 1e-6));\n"
			"	float h = (maxC == c.r) ? ((c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0))\n"
			"	        : ((maxC == c.g) ? ((c.b - c.r) / d + 2.0) : ((c.r - c.g) / d + 4.0));\n"
			"	return vec3(h / 6.0, s, l);\n"
			"}\n"
			"float cf_hue2rgb(float p, float q, float t) {\n"
			"	if (t < 0.0) t += 1.0;\n"
			"	if (t > 1.0) t -= 1.0;\n"
			"	if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;\n"
			"	if (t < 0.5) return q;\n"
			"	if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;\n"
			"	return p;\n"
			"}\n"
			"vec3 cf_hsl_to_rgb(vec3 hsl) {\n"
			"	float h = fract(hsl.x); float s = clamp(hsl.y, 0.0, 1.0); float l = clamp(hsl.z, 0.0, 1.0);\n"
			"	if (s <= 1e-6) return vec3(l);\n"
			"	float q = (l < 0.5) ? (l * (1.0 + s)) : (l + s - l * s);\n"
			"	float p = 2.0 * l - q;\n"
			"	return vec3(cf_hue2rgb(p, q, h + 1.0 / 3.0), cf_hue2rgb(p, q, h), cf_hue2rgb(p, q, h - 1.0 / 3.0));\n"
			"}\n"
			"int cf_fetch_lut(int idx) {\n"
			"	ivec4 v = uLut[idx >> 2];\n"
			"	int c = idx & 3;\n"
			"	if (c == 0) return v.x;\n"
			"	if (c == 1) return v.y;\n"
			"	if (c == 2) return v.z;\n"
			"	return v.w;\n"
			"}\n"
			"vec4 eval_filter_matrix(vec4 premul) {\n"
			"	vec4 s = (premul.a > 1e-5) ? vec4(premul.rgb / premul.a, premul.a) : vec4(0.0);\n"
			"	vec4 res = vec4(\n"
			"		dot(uMat0, s) + uMatBias.x,\n"
			"		dot(uMat1, s) + uMatBias.y,\n"
			"		dot(uMat2, s) + uMatBias.z,\n"
			"		dot(uMat3, s) + uMatBias.w\n"
			"	);\n"
			"	res = clamp(res, 0.0, 1.0);\n"
			"	return vec4(res.rgb * res.a, res.a);\n"
			"}\n"
			"void main(void) {\n"
			"	if (vUv.x < uCropUv.x || vUv.x > uCropUv.z || vUv.y < uCropUv.y || vUv.y > uCropUv.w) {\n"
			"		outColor = vec4(0.0);\n"
			"		return;\n"
			"	}\n"
			"	vec4 c = (vUv.x < uRoiUv.x || vUv.x > uRoiUv.z || vUv.y < uRoiUv.y || vUv.y > uRoiUv.w)\n"
			"		? vec4(0.0) : texture(uSrcTex, vUv);\n"
			"	if (uExtKind == 1) {\n"
			"		vec4 s = (c.a > 1e-5) ? vec4(clamp(c.rgb / c.a, 0.0, 1.0), c.a) : vec4(0.0);\n"
			"		int r8 = int(clamp(s.r * 255.0 + 0.5, 0.0, 255.0));\n"
			"		int g8 = int(clamp(s.g * 255.0 + 0.5, 0.0, 255.0));\n"
			"		int b8 = int(clamp(s.b * 255.0 + 0.5, 0.0, 255.0));\n"
			"		int a8 = int(clamp(s.a * 255.0 + 0.5, 0.0, 255.0));\n"
			"		float nr = float(cf_fetch_lut(r8) & 255) / 255.0;\n"
			"		float ng = float((cf_fetch_lut(g8) >> 8) & 255) / 255.0;\n"
			"		float nb = float((cf_fetch_lut(b8) >> 16) & 255) / 255.0;\n"
			"		float na = float((cf_fetch_lut(a8) >> 24) & 255) / 255.0;\n"
			"		outColor = vec4(vec3(nr, ng, nb) * na, na); return;\n"
			"	} else if (uExtKind == 2) {\n"
			"		vec4 s = (c.a > 1e-5) ? vec4(clamp(c.rgb / c.a, 0.0, 1.0), c.a) : vec4(0.0);\n"
			"		vec4 hsla = vec4(cf_rgb_to_hsl(s.rgb), s.a);\n"
			"		float nh = dot(uMat0, hsla) + uMatBias.x;\n"
			"		float ns = clamp(dot(uMat1, hsla) + uMatBias.y, 0.0, 1.0);\n"
			"		float nl = clamp(dot(uMat2, hsla) + uMatBias.z, 0.0, 1.0);\n"
			"		float na = clamp(dot(uMat3, hsla) + uMatBias.w, 0.0, 1.0);\n"
			"		outColor = vec4(cf_hsl_to_rgb(vec3(nh, ns, nl)) * na, na); return;\n"
			"	} else if (uExtKind == 3) {\n"
			"		float lum = clamp(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);\n"
			"		outColor = vec4(0.0, 0.0, 0.0, lum); return;\n"
			"	} else if (uExtKind == 4) {\n"
			"		vec4 s = (c.a > 1e-5) ? vec4(clamp(c.rgb / c.a, 0.0, 1.0), c.a) : vec4(0.0);\n"
			"		vec3 rgb = s.rgb;\n"
			"		if (uHcParams.x > 0.5) rgb = vec3(dot(rgb, vec3(0.2126, 0.7152, 0.0722)));\n"
			"		int invS = int(uHcParams.y + 0.5);\n"
			"		if (invS == 1) rgb = vec3(1.0) - rgb;\n"
			"		else if (invS == 2) { vec3 hsl = cf_rgb_to_hsl(rgb); rgb = cf_hsl_to_rgb(vec3(hsl.x, hsl.y, 1.0 - hsl.z)); }\n"
			"		if (abs(uHcParams.z) > 1e-5) {\n"
			"			float cf = clamp(uHcParams.z, -0.99, 0.99); float m = (1.0 + cf) / (1.0 - cf);\n"
			"			rgb = clamp(m * (rgb - vec3(0.5)) + vec3(0.5), 0.0, 1.0);\n"
			"		}\n"
			"		outColor = vec4(rgb * s.a, s.a); return;\n"
			"	}\n"
			"	if (uHasMat != 0) c = eval_filter_matrix(c);\n"
			"	if (uTransfer != 0) c = apply_color_transfer(c, uTransfer);\n"
			"	if (uHasBlend != 0) c = nisaba_blend(uBlendCol, c, uBlendMode);\n"
			"	outColor = c;\n"
			"}\n";
		if (createShader(m_impl->filterColorShader, "filter_color_shader", shaderHeader, nullptr, blurVertShader, filterColorFrag.c_str())) {
			m_impl->locFilterColorSrc = glGetUniformLocation(m_impl->filterColorShader.prog, "uSrcTex");
			m_impl->locFilterColorHasMat = glGetUniformLocation(m_impl->filterColorShader.prog, "uHasMat");
			m_impl->locFilterColorMat0 = glGetUniformLocation(m_impl->filterColorShader.prog, "uMat0");
			m_impl->locFilterColorMat1 = glGetUniformLocation(m_impl->filterColorShader.prog, "uMat1");
			m_impl->locFilterColorMat2 = glGetUniformLocation(m_impl->filterColorShader.prog, "uMat2");
			m_impl->locFilterColorMat3 = glGetUniformLocation(m_impl->filterColorShader.prog, "uMat3");
			m_impl->locFilterColorMatBias = glGetUniformLocation(m_impl->filterColorShader.prog, "uMatBias");
			m_impl->locFilterColorTransfer = glGetUniformLocation(m_impl->filterColorShader.prog, "uTransfer");
			m_impl->locFilterColorHasBlend = glGetUniformLocation(m_impl->filterColorShader.prog, "uHasBlend");
			m_impl->locFilterColorBlendMode = glGetUniformLocation(m_impl->filterColorShader.prog, "uBlendMode");
			m_impl->locFilterColorBlendCol = glGetUniformLocation(m_impl->filterColorShader.prog, "uBlendCol");
			m_impl->locFilterColorRoiUv = glGetUniformLocation(m_impl->filterColorShader.prog, "uRoiUv");
			m_impl->locFilterColorCropUv = glGetUniformLocation(m_impl->filterColorShader.prog, "uCropUv");
			m_impl->locFilterColorExtKind = glGetUniformLocation(m_impl->filterColorShader.prog, "uExtKind");
			m_impl->locFilterColorHcParams = glGetUniformLocation(m_impl->filterColorShader.prog, "uHcParams");
			m_impl->locFilterColorLut = glGetUniformLocation(m_impl->filterColorShader.prog, "uLut");
		}
		static const char* filterOpsFrag =
			"uniform sampler2D uSrcTex;\n"
			"uniform sampler2D uSrc2Tex;\n"
			"uniform vec4 uRoiDev;\n"
			"uniform vec4 uCropDev;\n"
			"uniform vec4 uFbSize;\n"
			"uniform vec4 uP0;\n"
			"uniform vec4 uP1;\n"
			"uniform vec4 uP2;\n"
			"uniform vec4 uP3;\n"
			"uniform vec4 uP4;\n"
			"uniform ivec4 uI0;\n"
			"uniform vec4 uKernel[7];\n"
			"in vec2 vUv;\n"
			"out vec4 outColor;\n"
			"vec2 dev_to_uv(vec2 dp) { return vec2(dp.x * uFbSize.z, 1.0 - dp.y * uFbSize.w); }\n"
			"vec4 sample_clamped(sampler2D t, vec2 dp) {\n"
			"	vec2 cp = clamp(dp, uRoiDev.xy + vec2(0.5), uRoiDev.zw - vec2(0.5));\n"
			"	return texture(t, dev_to_uv(cp));\n"
			"}\n"
			"vec4 sample_dev(sampler2D t, vec2 dp) {\n"
			"	if (dp.x < uRoiDev.x || dp.x > uRoiDev.z || dp.y < uRoiDev.y || dp.y > uRoiDev.w) return vec4(0.0);\n"
			"	return sample_clamped(t, dp);\n"
			"}\n"
			"float get_k(int idx) {\n"
			"	vec4 v = uKernel[idx >> 2];\n"
			"	int c = idx & 3;\n"
			"	if (c == 0) return v.x;\n"
			"	if (c == 1) return v.y;\n"
			"	if (c == 2) return v.z;\n"
			"	return v.w;\n"
			"}\n"
			"float read_ch(vec4 c, int ch) {\n"
			"	if (ch == 0) return c.r;\n"
			"	if (ch == 1) return c.g;\n"
			"	if (ch == 2) return c.b;\n"
			"	return c.a;\n"
			"}\n"
			"void main(void) {\n"
			"	vec2 devPos = vec2(vUv.x * uFbSize.x, (1.0 - vUv.y) * uFbSize.y);\n"
			"	if (devPos.x < uCropDev.x || devPos.x > uCropDev.z || devPos.y < uCropDev.y || devPos.y > uCropDev.w) {\n"
			"		outColor = vec4(0.0); return;\n"
			"	}\n"
			"	int op = uI0.x;\n"
			"	vec4 res = vec4(0.0);\n"
			"	if (op == 1 || op == 2) {\n"
			"		int r = clamp(uI0.y, 0, 64);\n"
			"		vec2 stepDir = uP0.xy;\n"
			"		vec4 acc = sample_clamped(uSrcTex, devPos - stepDir * float(r));\n"
			"		for (int k = -64; k <= 64; ++k) {\n"
			"			if (k < -r + 1 || k > r) continue;\n"
			"			vec4 s = sample_clamped(uSrcTex, devPos + stepDir * float(k));\n"
			"			acc = (op == 1) ? max(acc, s) : min(acc, s);\n"
			"		}\n"
			"		acc.rgb = min(acc.rgb, vec3(acc.a));\n"
			"		res = acc;\n"
			"	} else if (op == 3) {\n"
			"		int kw = clamp(uI0.y, 1, 5); int kh = clamp(uI0.z, 1, 5); int tileMode = uI0.w;\n"
			"		vec2 target = uP0.xy; float gain = uP0.z; float bias = uP0.w;\n"
			"		bool convAlpha = (uP1.x > 0.5);\n"
			"		vec4 sum = vec4(0.0);\n"
			"		vec2 roiSz = max(uRoiDev.zw - uRoiDev.xy, vec2(1.0));\n"
			"		for (int ky = 0; ky < 5; ++ky) {\n"
			"			if (ky >= kh) break;\n"
			"			for (int kx = 0; kx < 5; ++kx) {\n"
			"				if (kx >= kw) break;\n"
			"				float kv = get_k(ky * kw + kx);\n"
			"				if (kv == 0.0) continue;\n"
			"				vec2 sp = devPos + vec2(float(kx) - target.x, float(ky) - target.y);\n"
			"				vec4 s;\n"
			"				if (tileMode == 2) s = sample_dev(uSrcTex, sp);\n"
			"				else if (tileMode == 1) s = sample_clamped(uSrcTex, uRoiDev.xy + mod(sp - uRoiDev.xy, roiSz));\n"
			"				else s = sample_clamped(uSrcTex, sp);\n"
			"				sum += s * kv;\n"
			"			}\n"
			"		}\n"
			"		vec4 center = sample_clamped(uSrcTex, devPos);\n"
			"		float a = clamp(convAlpha ? (sum.a * gain + bias) : center.a, 0.0, 1.0);\n"
			"		res = vec4(clamp(sum.rgb * gain + vec3(bias), vec3(0.0), vec3(a)), a);\n"
			"	} else if (op == 4) {\n"
			"		vec4 d = sample_dev(uSrc2Tex, devPos);\n"
			"		vec4 du = vec4(d.a > 1e-4 ? clamp(d.rgb / d.a, 0.0, 1.0) : vec3(0.0), d.a);\n"
			"		float dx = (read_ch(du, uI0.y) - 0.5) * uP0.x;\n"
			"		float dy = (read_ch(du, uI0.z) - 0.5) * uP0.y;\n"
			"		res = sample_dev(uSrcTex, devPos + vec2(dx, dy));\n"
			"	} else if (op == 5) {\n"
			"		if (devPos.x >= uP0.x && devPos.x <= uP0.z && devPos.y >= uP0.y && devPos.y <= uP0.w) {\n"
			"			vec2 sz = max(uP0.zw - uP0.xy, vec2(1e-3));\n"
			"			vec2 t = (devPos - uP0.xy) / sz;\n"
			"			vec2 ins = min(max(uP1.xy, vec2(0.0)), sz * 0.45);\n"
			"			vec2 b = ins / sz;\n"
			"			float wt = 1.0;\n"
			"			if (b.x > 1e-4 || b.y > 1e-4) {\n"
			"				float wx = (b.x > 1e-4) ? (min(t.x, 1.0 - t.x) / b.x) : 1.0;\n"
			"				float wy = (b.y > 1e-4) ? (min(t.y, 1.0 - t.y) / b.y) : 1.0;\n"
			"				float m = clamp(min(wx, wy), 0.0, 1.0);\n"
			"				wt = m * m * (3.0 - 2.0 * m);\n"
			"			}\n"
			"			vec2 zpos = (uP0.xy + ins) + t * (sz - 2.0 * ins);\n"
			"			res = sample_dev(uSrcTex, mix(devPos, zpos, wt));\n"
			"		}\n"
			"	} else if (op == 6) {\n"
			"		vec4 fg = sample_dev(uSrcTex, devPos);\n"
			"		vec4 bg = sample_dev(uSrc2Tex, devPos);\n"
			"		res = clamp(uP0.x * fg * bg + uP0.y * fg + uP0.z * bg + vec4(uP0.w), 0.0, 1.0);\n"
			"		if (uI0.y != 0) res.rgb = min(res.rgb, vec3(res.a));\n"
			"	} else if (op == 7) {\n"
			"		vec2 sp = vec2(uP0.x * devPos.x + uP0.z * devPos.y + uP1.x,\n"
			"		               uP0.y * devPos.x + uP0.w * devPos.y + uP1.y);\n"
			"		if (sp.x >= uRoiDev.x && sp.x <= uRoiDev.z && sp.y >= uRoiDev.y && sp.y <= uRoiDev.w) {\n"
			"			if (uI0.y == 0) sp = floor(sp) + vec2(0.5);\n"
			"			res = sample_clamped(uSrcTex, sp);\n"
			"		}\n"
			"	} else if (op == 8) {\n"
			"		if (devPos.x >= uP1.x && devPos.x <= uP1.z && devPos.y >= uP1.y && devPos.y <= uP1.w) {\n"
			"			vec2 ssz = max(uP0.zw, vec2(1e-3));\n"
			"			vec2 sp = uP0.xy + mod(devPos - uP0.xy, ssz);\n"
			"			res = sample_dev(uSrcTex, sp);\n"
			"		}\n"
			"	} else if (op == 9) {\n"
			"		bool inside = (devPos.x >= uP0.x && devPos.x <= uP0.z && devPos.y >= uP0.y && devPos.y <= uP0.w);\n"
			"		vec4 c = sample_dev(uSrcTex, devPos);\n"
			"		float newA = clamp(inside ? max(c.a, uP1.x) : min(c.a, uP1.y), 0.0, 1.0);\n"
			"		vec3 rgb = (c.a > 1e-4) ? clamp(c.rgb * (newA / c.a), vec3(0.0), vec3(newA)) : vec3(0.0);\n"
			"		res = vec4(rgb, newA);\n"
			"	} else if (op == 10) {\n"
			"		int lkind = uI0.y; float surfScale = uP0.w;\n"
			"		float a00 = sample_clamped(uSrcTex, devPos + vec2(-1.0, -1.0)).a;\n"
			"		float a10 = sample_clamped(uSrcTex, devPos + vec2( 0.0, -1.0)).a;\n"
			"		float a20 = sample_clamped(uSrcTex, devPos + vec2( 1.0, -1.0)).a;\n"
			"		float a01 = sample_clamped(uSrcTex, devPos + vec2(-1.0,  0.0)).a;\n"
			"		float a11 = sample_clamped(uSrcTex, devPos).a;\n"
			"		float a21 = sample_clamped(uSrcTex, devPos + vec2( 1.0,  0.0)).a;\n"
			"		float a02 = sample_clamped(uSrcTex, devPos + vec2(-1.0,  1.0)).a;\n"
			"		float a12 = sample_clamped(uSrcTex, devPos + vec2( 0.0,  1.0)).a;\n"
			"		float a22 = sample_clamped(uSrcTex, devPos + vec2( 1.0,  1.0)).a;\n"
			"		float ddx = (-a00 + a20 - 2.0 * a01 + 2.0 * a21 - a02 + a22) * (surfScale * 0.25);\n"
			"		float ddy = (-a00 - 2.0 * a10 - a20 + a02 + 2.0 * a12 + a22) * (surfScale * 0.25);\n"
			"		vec3 N = normalize(vec3(-ddx, -ddy, 1.0));\n"
			"		vec2 lp = vec2(uP4.x * devPos.x + uP4.z * devPos.y + uKernel[0].x,\n"
			"		               uP4.y * devPos.x + uP4.w * devPos.y + uKernel[0].y);\n"
			"		vec3 L = (lkind == 0 || lkind == 1) ? normalize(uP0.xyz) : normalize(uP0.xyz - vec3(lp, surfScale * a11));\n"
			"		float atten = 1.0;\n"
			"		if (lkind == 4 || lkind == 5) {\n"
			"			vec3 S = normalize(uP1.xyz - uP0.xyz);\n"
			"			float cosA = dot(-L, S);\n"
			"			atten = (cosA <= 0.0 || (uP3.z < 180.0 && cosA < uP3.y)) ? 0.0 : pow(cosA, max(uP3.x, 0.0));\n"
			"		}\n"
			"		bool isSpec = (lkind == 1 || lkind == 3 || lkind == 5);\n"
			"		float inten = isSpec ? (uP1.w * pow(max(dot(N, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), max(uP2.w, 1e-4)) * atten)\n"
			"		                     : (uP1.w * max(dot(N, L), 0.0) * atten);\n"
			"		vec3 rgb = clamp(inten * uP2.rgb, 0.0, 1.0);\n"
			"		res = vec4(rgb, isSpec ? max(max(rgb.r, rgb.g), rgb.b) : 1.0);\n"
			"	}\n"
			"	outColor = res;\n"
			"}\n";
		if (createShader(m_impl->filterOpsShader, "filter_ops_shader", shaderHeader, nullptr, blurVertShader, filterOpsFrag)) {
			const GLuint p = m_impl->filterOpsShader.prog;
			m_impl->locFilterOpsSrc = glGetUniformLocation(p, "uSrcTex");
			m_impl->locFilterOpsSrc2 = glGetUniformLocation(p, "uSrc2Tex");
			m_impl->locFilterOpsRoiDev = glGetUniformLocation(p, "uRoiDev");
			m_impl->locFilterOpsCropDev = glGetUniformLocation(p, "uCropDev");
			m_impl->locFilterOpsFbSize = glGetUniformLocation(p, "uFbSize");
			m_impl->locFilterOpsP0 = glGetUniformLocation(p, "uP0");
			m_impl->locFilterOpsP1 = glGetUniformLocation(p, "uP1");
			m_impl->locFilterOpsP2 = glGetUniformLocation(p, "uP2");
			m_impl->locFilterOpsP3 = glGetUniformLocation(p, "uP3");
			m_impl->locFilterOpsP4 = glGetUniformLocation(p, "uP4");
			m_impl->locFilterOpsI0 = glGetUniformLocation(p, "uI0");
			m_impl->locFilterOpsKernel = glGetUniformLocation(p, "uKernel");
		}
	}

	if (m_impl->flags & CreateFlags::StencilStrokes) {
		std::string stencilOpts = mainOpts + "#define STENCIL_STROKES 1\n";
		if (createShader(m_impl->stencilStrokeShader, "stencil_stroke_shader", shaderHeader, stencilOpts.c_str(), fillVertShader, fastCoreFrag.c_str())) {
			finalizeShader(m_impl->stencilStrokeShader, true);
		}
		if (createShader(m_impl->stencilEffectsShader, "stencil_effects_shader", shaderHeader, stencilOpts.c_str(), fillVertShader, effectsFrag.c_str())) {
			finalizeShader(m_impl->stencilEffectsShader, true);
		}
	}

	glGenVertexArrays(1, &m_impl->vertArr);
	glGenBuffers(1, &m_impl->vertBuf);

	glBindVertexArray(m_impl->vertArr);
	glBindBuffer(GL_ARRAY_BUFFER, m_impl->vertBuf);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(0));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(2 * sizeof(float)));
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glGenBuffers(1, &m_impl->fragBuf);

	GLint align = 4;
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
	if (align <= 0) align = 4;
	auto alignUp = [align](size_t bytes) -> int {
		const size_t a = static_cast<size_t>(align);
		return static_cast<int>(((bytes + a - 1) / a) * a);
	};
	m_impl->fragFastSize = alignUp(kFastUniformBytes);
	m_impl->fragSize     = alignUp(sizeof(FragUniforms));

#if defined(NISABA_TARGET_GLES)
	constexpr bool samplers_supported = true;
#else
	const bool samplers_supported = (glGenSamplers && glDeleteSamplers && glBindSampler && glSamplerParameteri);
#endif
	if (samplers_supported) {
		m_impl->hasSamplerObjects = true;
		constexpr GLenum filters[2] = {GL_NEAREST, GL_LINEAR};
		constexpr GLenum wraps[3]   = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
		for (int fi = 0; fi < 2; ++fi) {
			for (int wi = 0; wi < 3; ++wi) {
				GLuint s = 0;
				glGenSamplers(1, &s);
				glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, filters[fi]);
				glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, filters[fi]);
				glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wraps[wi]);
				glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wraps[wi]);
				m_impl->samplerCache[fi][wi] = s;
			}
		}
	}

	// Pre-allocate ring buffers once at init to avoid per-frame GEM allocations
	m_impl->fragBufCap = (1048576 / m_impl->fragSize) * m_impl->fragSize;
	glBindBuffer(GL_UNIFORM_BUFFER, m_impl->fragBuf);
	glBufferData(GL_UNIFORM_BUFFER, m_impl->fragBufCap, nullptr, GL_STREAM_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	m_impl->vertBufCap = static_cast<size_t>(262144) * sizeof(Vertex);
	glBindBuffer(GL_ARRAY_BUFFER, m_impl->vertBuf);
	glBufferData(GL_ARRAY_BUFFER, m_impl->vertBufCap, nullptr, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Create dummy texture for unbound shaders
	m_impl->dummyTex = createTexture(TextureType::Alpha, 1, 1, 0, nullptr);
	m_impl->dummyTexPtr = m_impl->findTexture(m_impl->dummyTex);

	checkGLError(m_impl->flags, "init done");
	glFinish();
	return true;
}

void GL3Renderer::shutdown() {
	if (!m_impl) return;

	deleteShader(m_impl->shader);
	deleteShader(m_impl->rrectShader);
	deleteShader(m_impl->drrectShader);
	deleteShader(m_impl->imgShader);
	deleteShader(m_impl->ringShader);
	deleteShader(m_impl->linearGradShader);
	deleteShader(m_impl->radialGradShader);
	deleteShader(m_impl->multiGradLutShader);
	deleteShader(m_impl->multiGradShader);
	deleteShader(m_impl->shadowShader);
	deleteShader(m_impl->perspectiveImgShader);
	deleteShader(m_impl->blendFastShader);
	deleteShader(m_impl->effectsShader);
	deleteShader(m_impl->stencilStrokeShader);
	deleteShader(m_impl->stencilEffectsShader);
	deleteShader(m_impl->simpleShader);
	deleteShader(m_impl->simpleColorShader);
	deleteShader(m_impl->solidVertShader);
	deleteShader(m_impl->circleVertShader);
	deleteShader(m_impl->rectVertShader);
	deleteShader(m_impl->blurShader);
	deleteShader(m_impl->filterBlendShader);
	deleteShader(m_impl->filterColorShader);
	deleteShader(m_impl->filterOpsShader);
	for (auto& [hash, rfp] : m_impl->runtimeFilterCache) {
		deleteShader(rfp.shader);
	}
	m_impl->runtimeFilterCache.clear();
	for (auto& [hash, rbp] : m_impl->runtimeBlenderCache) {
		deleteShader(rbp.shader);
	}
	m_impl->runtimeBlenderCache.clear();
	for (int t = 0; t < 3; ++t) {
		for (int k = 0; k < 2; ++k) {
			if (m_impl->blurTierFbo[t][k] != 0) {
				glDeleteFramebuffers(1, &m_impl->blurTierFbo[t][k]);
				m_impl->blurTierFbo[t][k] = 0;
			}
			if (m_impl->blurTierTex[t][k] != 0) {
				glDeleteTextures(1, &m_impl->blurTierTex[t][k]);
				m_impl->blurTierTex[t][k] = 0;
			}
		}
		m_impl->blurTierW[t] = 0;
		m_impl->blurTierH[t] = 0;
	}
	for (auto& lf : m_impl->layerPool) {
		if (lf.fbo != 0) glDeleteFramebuffers(1, &lf.fbo);
		if (lf.colorTex != 0) glDeleteTextures(1, &lf.colorTex);
		if (lf.depthStencilRb != 0) glDeleteRenderbuffers(1, &lf.depthStencilRb);
	}
	m_impl->layerPool.clear();
	m_impl->activeLayers.clear();

	if (m_impl->fragBuf != 0) {
		glDeleteBuffers(1, &m_impl->fragBuf);
		m_impl->fragBuf = 0;
	}
	if (m_impl->vertArr != 0) {
		glDeleteVertexArrays(1, &m_impl->vertArr);
		m_impl->vertArr = 0;
	}
	if (m_impl->vertBuf != 0) {
		glDeleteBuffers(1, &m_impl->vertBuf);
		m_impl->vertBuf = 0;
	}
	if (m_impl->dstSnapFbo != 0) {
		glDeleteFramebuffers(1, &m_impl->dstSnapFbo);
		m_impl->dstSnapFbo = 0;
	}
	if (m_impl->dstSnapTex != 0) {
		glDeleteTextures(1, &m_impl->dstSnapTex);
		m_impl->dstSnapTex = 0;
	}
	m_impl->dstSnapW = 0;
	m_impl->dstSnapH = 0;

	for (auto& t : m_impl->textures) {
		if (t.tex != 0 && (t.flags & ImageFlagsGL::ImageNoDelete) == 0) {
			glDeleteTextures(1, &t.tex);
		}
	}
	m_impl->textures.clear();

	if (m_impl->hasSamplerObjects) {
		for (int fi = 0; fi < 2; ++fi) {
			for (int wi = 0; wi < 3; ++wi) {
				if (m_impl->samplerCache[fi][wi] != 0) {
					glDeleteSamplers(1, &m_impl->samplerCache[fi][wi]);
					m_impl->samplerCache[fi][wi] = 0;
				}
			}
		}
		m_impl->hasSamplerObjects = false;
	}
}

bool GL3Renderer::edgeAntiAlias() const {
	return (m_impl->flags & CreateFlags::Antialias) != 0;
}

int GL3Renderer::createTexture(TextureType type, int width, int height, int imageFlags, const unsigned char* data) {
	return createTextureWithStride(type, width, height, 0, imageFlags, data);
}

int GL3Renderer::createTextureWithStride(TextureType type, int width, int height, int strideBytes, int imageFlags, const unsigned char* data) {
	if (width <= 0 || height <= 0) return 0;

	int bpp = (type == TextureType::Alpha) ? 1 : 4;
	int tightStride = width * bpp;
	int effectiveStride = (strideBytes > 0) ? strideBytes : tightStride;
	if (effectiveStride < tightStride) return 0;

	if (data && ((type == TextureType::BGRA && m_impl->isGLES) || (effectiveStride % bpp != 0))) {
		return Renderer::createTextureWithStride(type, width, height, strideBytes, imageFlags, data);
	}

	Texture* tex = m_impl->allocTexture();
	if (!tex) return 0;

	glGenTextures(1, &tex->tex);
	tex->width = width;
	tex->height = height;
	tex->type = (type == TextureType::Alpha) ? TextureType::Alpha : TextureType::RGBA;
	tex->flags = imageFlags;
	glActiveTexture(GL_TEXTURE0);
	m_impl->bindTexture(tex->tex);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (data && effectiveStride != tightStride) ? (effectiveStride / bpp) : tex->width);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

	if (type == TextureType::Alpha) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, data);
	} else if (type == TextureType::BGRA) {
#ifdef GL_BGRA
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
#else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
#endif
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	}

	if ((imageFlags & ImageFlags::ImageGenerateMipmaps) != 0) {
		if ((imageFlags & ImageFlags::ImageNearest) != 0) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		}
	} else {
		if ((imageFlags & ImageFlags::ImageNearest) != 0) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		}
	}

	if ((imageFlags & ImageFlags::ImageNearest) != 0) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	const GLenum wrapS = (imageFlags & ImageFlags::ImageMirrorX)
	    ? GL_MIRRORED_REPEAT
	    : ((imageFlags & ImageFlags::ImageRepeatX) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	const GLenum wrapT = (imageFlags & ImageFlags::ImageMirrorY)
	    ? GL_MIRRORED_REPEAT
	    : ((imageFlags & ImageFlags::ImageRepeatY) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

	if ((imageFlags & ImageFlags::ImageGenerateMipmaps) != 0) {
		glGenerateMipmap(GL_TEXTURE_2D);
	}

	checkGLError(m_impl->flags, "create texture");
	m_impl->bindTexture(0);
	return tex->id;
}

bool GL3Renderer::deleteTexture(int image) {
	return m_impl->deleteTexture(image);
}

bool GL3Renderer::updateTexture(int image, int x, int y, int width, int height, const unsigned char* data) {
	Texture* tex = m_impl->findTexture(image);
	if (!tex) return false;

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex->tex);
	m_impl->boundTexture = tex->tex;

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, tex->width);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, y);

	if (tex->type == TextureType::RGBA) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RED, GL_UNSIGNED_BYTE, data);
	}

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

	glBindTexture(GL_TEXTURE_2D, 0);
	m_impl->boundTexture = 0;
	return true;
}

bool GL3Renderer::getTextureSize(int image, int& outWidth, int& outHeight) {
	Texture* tex = m_impl->findTexture(image);
	if (!tex) return false;
	outWidth = tex->width;
	outHeight = tex->height;
	return true;
}

int GL3Renderer::textureFlags(int image) const {
	Texture* tex = m_impl->findTexture(image);
	return tex ? tex->flags : 0;
}

int GL3Renderer::createTextureFromNativeHandle(uint64_t handle, int w, int h, int imageFlags) {
	return createImageFromHandle(static_cast<uint32_t>(handle), w, h, imageFlags | ImageFlagsGL::ImageNoDelete);
}

int GL3Renderer::createImageFromHandle(uint32_t textureId, int w, int h, int imageFlags) {
	Texture* tex = m_impl->allocTexture();
	if (!tex) return 0;
	tex->type = TextureType::RGBA;
	tex->tex = textureId;
	tex->flags = imageFlags;
	tex->width = w;
	tex->height = h;
	return tex->id;
}

uint32_t GL3Renderer::imageHandle(int image) const {
	Texture* tex = m_impl->findTexture(image);
	return tex ? tex->tex : 0;
}

void GL3Renderer::resetState() {
	if (!m_impl) return;
	m_impl->resetCachedGLState();
	glActiveTexture(GL_TEXTURE0);
	if (m_impl->hasSamplerObjects) {
		glBindSampler(0, 0);
		glBindSampler(1, 0);
	}
	glBlendEquation(GL_FUNC_ADD);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindVertexArray(0);
	glUseProgram(0);
}

void GL3Renderer::viewport(float width, float height, float devicePixelRatio) {
	(void)devicePixelRatio;
	if (m_impl->view[0] != width || m_impl->view[1] != height) {
		m_impl->view[0] = width;
		m_impl->view[1] = height;
		auto setView = [&](const GLShader& s) {
			if (s.prog != 0) {
				glUseProgram(s.prog);
				glUniform2fv(s.loc[LocViewSize], 1, m_impl->view);
			}
		};
		setView(m_impl->shader);
		setView(m_impl->rrectShader);
		setView(m_impl->drrectShader);
		setView(m_impl->imgShader);
		setView(m_impl->ringShader);
		setView(m_impl->linearGradShader);
		setView(m_impl->radialGradShader);
		setView(m_impl->multiGradLutShader);
		setView(m_impl->multiGradShader);
		setView(m_impl->shadowShader);
		setView(m_impl->perspectiveImgShader);
		setView(m_impl->blendFastShader);
		setView(m_impl->effectsShader);
		setView(m_impl->stencilStrokeShader);
		setView(m_impl->stencilEffectsShader);
		setView(m_impl->simpleShader);
		setView(m_impl->simpleColorShader);
		setView(m_impl->solidVertShader);
		setView(m_impl->circleVertShader);
		setView(m_impl->rectVertShader);
		glUseProgram(0);
		m_impl->activeProg = 0;
	}
}

void GL3Renderer::cancel() {
	m_impl->clearBatchBuffers();
	m_impl->activeLayers.clear();
	for (auto& lf : m_impl->layerPool) lf.inUse = false;
	m_impl->currentClipDepth = 0;
	m_impl->flushedClipDepth = 0;
}

void GL3Renderer::flush() {
	if (m_impl->calls.empty()) return;

	// Hardware fast-clear promotion: if the first call is a full-viewport opaque solid quad,
	// execute hardware glClear(GL_COLOR_BUFFER_BIT) (Intel CCS_E fast-clear metadata) just like Skia!
	size_t firstCallIdx = 0;
	{
		const Call& c0 = m_impl->calls[0];
		if (c0.type == CallType::Triangles && c0.triangleCount == 6 &&
		    !c0.programmableBlend && c0.uniformOffset >= 0 && !Impl::bucketNeedsFullUbo(c0.bucket) &&
		    c0.clipDepth == 0 && m_impl->flushedClipDepth == 0 && c0.scissorRect[0] < 0 && c0.customShader == nullptr &&
		    (c0.blendFunc.srcRGB == GL_ONE && (c0.blendFunc.dstRGB == GL_ONE_MINUS_SRC_ALPHA || c0.blendFunc.dstRGB == GL_ZERO))) {
			const FragUniforms* f0 = m_impl->fragUniformPtr(c0.uniformOffset);
			if (f0->type == ShaderSimple && f0->innerCol.a >= 0.999f) {
				glDisable(GL_SCISSOR_TEST);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glClearColor(f0->innerCol.r, f0->innerCol.g, f0->innerCol.b, f0->innerCol.a);
				glClear(GL_COLOR_BUFFER_BIT);
				firstCallIdx = 1;
				if (m_impl->calls.size() == 1) {
					m_impl->clearBatchBuffers();
					return;
				}
			}
		}
	}

	m_impl->resetCachedGLState();
	m_impl->useProgramCached(m_impl->shader.prog);

	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glActiveTexture(GL_TEXTURE0);
	if (m_impl->hasSamplerObjects) glBindSampler(0, 0);

	// Stream UBO via unsynchronized ring buffer (eliminates Mesa staging-BO copies & GPU stalls)
	size_t uboBytes = m_impl->uniforms.size();
	if (uboBytes > 0) {
		glBindBuffer(GL_UNIFORM_BUFFER, m_impl->fragBuf);
		if (m_impl->fragBufCap == 0 || m_impl->fragRingOffset + uboBytes > m_impl->fragBufCap) {
			size_t minCap = (2097152 / m_impl->fragSize) * m_impl->fragSize;
			m_impl->fragBufCap = std::max(m_impl->fragBufCap, std::max(uboBytes * 4, minCap));
			glBufferData(GL_UNIFORM_BUFFER, m_impl->fragBufCap, nullptr, GL_STREAM_DRAW);
			m_impl->fragRingOffset = 0;
		}
		m_impl->baseUboOffset = static_cast<int>(m_impl->fragRingOffset);
		bool mapped = false;
		if (glMapBufferRange != nullptr && glUnmapBuffer != nullptr) {
			void* dst = glMapBufferRange(GL_UNIFORM_BUFFER, m_impl->baseUboOffset, uboBytes,
			                             GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
			if (dst != nullptr) {
				std::memcpy(dst, m_impl->uniforms.data(), uboBytes);
				glUnmapBuffer(GL_UNIFORM_BUFFER);
				mapped = true;
			}
		}
		if (!mapped) {
			glBufferSubData(GL_UNIFORM_BUFFER, m_impl->baseUboOffset, uboBytes, m_impl->uniforms.data());
		}
		m_impl->fragRingOffset += uboBytes;
	}

	// Stream VBO via unsynchronized ring buffer (eliminates Mesa staging-BO copies & GPU stalls)
	glBindVertexArray(m_impl->vertArr);
	int nverts = static_cast<int>(m_impl->verts.size());
	size_t vertBytes = static_cast<size_t>(nverts) * sizeof(Vertex);
	if (vertBytes > 0) {
		glBindBuffer(GL_ARRAY_BUFFER, m_impl->vertBuf);
		if (m_impl->vertBufCap == 0 || (static_cast<size_t>(m_impl->vertRingPos + nverts) * sizeof(Vertex)) > m_impl->vertBufCap) {
			m_impl->vertBufCap = std::max(m_impl->vertBufCap, std::max(vertBytes * 4, static_cast<size_t>(262144) * sizeof(Vertex)));
			glBufferData(GL_ARRAY_BUFFER, m_impl->vertBufCap, nullptr, GL_STREAM_DRAW);
			m_impl->vertRingPos = 0;
		}
		m_impl->baseVertOffset = m_impl->vertRingPos;
		bool mapped = false;
		if (glMapBufferRange != nullptr && glUnmapBuffer != nullptr) {
			void* dst = glMapBufferRange(GL_ARRAY_BUFFER,
			                             static_cast<GLintptr>(m_impl->baseVertOffset) * sizeof(Vertex),
			                             vertBytes,
			                             GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
			if (dst != nullptr) {
				std::memcpy(dst, m_impl->verts.data(), vertBytes);
				glUnmapBuffer(GL_ARRAY_BUFFER);
				mapped = true;
			}
		}
		if (!mapped) {
			glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(m_impl->baseVertOffset) * sizeof(Vertex), vertBytes, m_impl->verts.data());
		}
		m_impl->vertRingPos += nverts;
	}

	int activeClipDepth = m_impl->flushedClipDepth;
	bool usedProgrammableBlend = false;
	struct SnapRect { float b[4]; };
	bool dstSnapValid = false;
	GLuint dstSnapFboBound = 0;
	float dstSnapRect[4]{0.0f, 0.0f, 0.0f, 0.0f};
	std::vector<SnapRect> dirtySnapRects;
	auto rectOverlaps = [](const float a[4], const float b[4]) noexcept -> bool {
		return !(a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1]);
	};
	auto rectContained = [](const float inner[4], const float outer[4]) noexcept -> bool {
		return inner[0] >= outer[0] && inner[1] >= outer[1] &&
		       inner[2] <= outer[2] && inner[3] <= outer[3];
	};
	m_impl->activeLayers.clear();
	for (auto& lf : m_impl->layerPool) lf.inUse = false;
	for (size_t idx = firstCallIdx; idx < m_impl->calls.size(); ++idx) {
		const auto& call = m_impl->calls[idx];
		if (call.programmableBlend && call.type != CallType::PopLayer) {
			m_impl->ensureBaseTargetInfo();
			const GLuint curFbo = !m_impl->activeLayers.empty()
				? m_impl->layerPool[m_impl->activeLayers.back().poolIndex].fbo
				: m_impl->baseDrawFbo;
			bool canReuseSnap = dstSnapValid && (dstSnapFboBound == curFbo) &&
			                    call.hasCallBounds && rectContained(call.callBounds, dstSnapRect);
			if (canReuseSnap) {
				for (const auto& dr : dirtySnapRects) {
					if (rectOverlaps(call.callBounds, dr.b)) {
						canReuseSnap = false;
						break;
					}
				}
			}
			if (!canReuseSnap) {
				if (call.hasCallBounds) {
					float unionB[4]{call.callBounds[0], call.callBounds[1], call.callBounds[2], call.callBounds[3]};
					int lookahead = 1;
					const size_t maxLook = std::min(m_impl->calls.size(), idx + 64);
					for (size_t j = idx + 1; j < maxLook; ++j) {
						const auto& nextC = m_impl->calls[j];
						if (nextC.type == CallType::PushLayer || nextC.type == CallType::PopLayer) break;
						if (nextC.programmableBlend && nextC.hasCallBounds) {
							unionB[0] = std::min(unionB[0], nextC.callBounds[0]);
							unionB[1] = std::min(unionB[1], nextC.callBounds[1]);
							unionB[2] = std::max(unionB[2], nextC.callBounds[2]);
							unionB[3] = std::max(unionB[3], nextC.callBounds[3]);
							++lookahead;
						}
					}
					m_impl->captureDstSnapshot(true, unionB, (lookahead == 1) ? call.scissorRect : nullptr);
					dstSnapValid = true;
					dstSnapFboBound = curFbo;
					dstSnapRect[0] = unionB[0];
					dstSnapRect[1] = unionB[1];
					dstSnapRect[2] = unionB[2];
					dstSnapRect[3] = unionB[3];
					dirtySnapRects.clear();
				} else {
					m_impl->captureDstSnapshotForCall(call);
					dstSnapValid = false;
					dirtySnapRects.clear();
				}
			}
			usedProgrammableBlend = true;
		}
		m_impl->blendFuncSeparate(call.blendFunc);
		bool needScissor = (call.scissorRect[0] >= 0);
		if (m_impl->scissorEnabled != needScissor) {
			m_impl->scissorEnabled = needScissor;
			if (needScissor) {
				glEnable(GL_SCISSOR_TEST);
			} else {
				glDisable(GL_SCISSOR_TEST);
			}
		}
		if (needScissor) {
			if (m_impl->currentScissor[0] != call.scissorRect[0] ||
			    m_impl->currentScissor[1] != call.scissorRect[1] ||
			    m_impl->currentScissor[2] != call.scissorRect[2] ||
			    m_impl->currentScissor[3] != call.scissorRect[3]) {
				m_impl->currentScissor[0] = call.scissorRect[0];
				m_impl->currentScissor[1] = call.scissorRect[1];
				m_impl->currentScissor[2] = call.scissorRect[2];
				m_impl->currentScissor[3] = call.scissorRect[3];
				glScissor(call.scissorRect[0], call.scissorRect[1], call.scissorRect[2], call.scissorRect[3]);
			}
		}
		switch (call.type) {
			case CallType::Clip:
				m_impl->clip(call, activeClipDepth);
				break;
			case CallType::RestoreClip:
				m_impl->restoreClip(call, activeClipDepth);
				break;
			case CallType::PushLayer:
				m_impl->pushLayer(call, activeClipDepth);
				dstSnapValid = false;
				dirtySnapRects.clear();
				break;
			case CallType::PopLayer:
				m_impl->popLayer(call, activeClipDepth, usedProgrammableBlend);
				dstSnapValid = false;
				dirtySnapRects.clear();
				break;
			case CallType::Fill:
				m_impl->fill(call, activeClipDepth);
				break;
			case CallType::ConvexFill:
				m_impl->convexFill(call, activeClipDepth);
				break;
			case CallType::Stroke:
				m_impl->stroke(call, activeClipDepth);
				break;
			case CallType::Triangles:
				m_impl->triangles(call, activeClipDepth);
				break;
			default:
				break;
		}
		if (dstSnapValid &&
		    (call.type == CallType::Fill || call.type == CallType::ConvexFill ||
		     call.type == CallType::Stroke || call.type == CallType::Triangles)) {
			if (call.hasCallBounds && dirtySnapRects.size() < 64) {
				dirtySnapRects.push_back(SnapRect{{
					call.callBounds[0] - 2.0f, call.callBounds[1] - 2.0f,
					call.callBounds[2] + 2.0f, call.callBounds[3] + 2.0f
				}});
			} else if (call.type == CallType::Triangles && call.triangleCount > 0 &&
			           call.triangleCount <= 64 && dirtySnapRects.size() < 64) {
				const Vertex* vp = &m_impl->verts[call.triangleOffset];
				float x0 = vp[0].x, y0 = vp[0].y, x1 = vp[0].x, y1 = vp[0].y;
				for (int vi = 1; vi < call.triangleCount; ++vi) {
					x0 = std::min(x0, vp[vi].x);
					y0 = std::min(y0, vp[vi].y);
					x1 = std::max(x1, vp[vi].x);
					y1 = std::max(y1, vp[vi].y);
				}
				dirtySnapRects.push_back(SnapRect{{x0 - 2.0f, y0 - 2.0f, x1 + 2.0f, y1 + 2.0f}});
			} else {
				dstSnapValid = false;
				dirtySnapRects.clear();
			}
		}
	}

	if (activeClipDepth > 0) {
		m_impl->enableStencil(true);
		m_impl->stencilMask(0x00);
		m_impl->stencilFunc(GL_EQUAL, activeClipDepth << 4, 0xf0);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	} else if (m_impl->stencilEnabled) {
		m_impl->enableStencil(false);
	}

	if (m_impl->scissorEnabled) {
		glDisable(GL_SCISSOR_TEST);
		m_impl->scissorEnabled = false;
	}
	if (m_impl->hasSamplerObjects && m_impl->boundSampler != 0) {
		glBindSampler(0, 0);
		m_impl->boundSampler = 0;
	}
	if (usedProgrammableBlend) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
	}
	glDisable(GL_CULL_FACE);

	if (!m_impl->activeLayers.empty()) {
		glBindFramebuffer(GL_FRAMEBUFFER, m_impl->activeLayers.front().parentFbo);
		m_impl->activeLayers.clear();
		for (auto& lf : m_impl->layerPool) lf.inUse = false;
	}

	glBindVertexArray(0);
	glUseProgram(0);
	m_impl->activeProg = 0;
	m_impl->boundTexture = 0;
	m_impl->flushedClipDepth = activeClipDepth;
	m_impl->clearBatchBuffers();
}

void GL3Renderer::renderFill(const Paint& paint, const CompositeOperationState& compOp,
                             const Scissor& scissor, float fringe, const float* bounds,
                             const Path* paths, int npaths) {
	const bool programmableBlend = compOp.isProgrammableBlend;
	const int blendModeId = programmableBlend ? (static_cast<int>(compOp.blendMode) + 1) : 0;
	Blend blend = programmableBlend ? Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO} : convertBlend(compOp);
	int hwScissor[4]{-1, -1, -1, -1};
	bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);
	Scissor effectiveScissor = scissor;

	if (hasHwScissor && bounds != nullptr) {
		float bMinX = bounds[0] - fringe;
		float bMinY = bounds[1] - fringe;
		float bMaxX = bounds[2] + fringe;
		float bMaxY = bounds[3] + fringe;

		float sMinX = static_cast<float>(hwScissor[0]);
		float sMaxX = static_cast<float>(hwScissor[0] + hwScissor[2]);
		float sMinY = m_impl->view[1] - static_cast<float>(hwScissor[1] + hwScissor[3]);
		float sMaxY = m_impl->view[1] - static_cast<float>(hwScissor[1]);

		if (bMaxX <= sMinX || bMinX >= sMaxX || bMaxY <= sMinY || bMinY >= sMaxY) {
			return;
		}

		if (bMinX >= sMinX && bMaxX <= sMaxX && bMinY >= sMinY && bMaxY <= sMaxY) {
			hasHwScissor = false;
			hwScissor[0] = -1;
			effectiveScissor.extent[0] = -1.0f;
			effectiveScissor.extent[1] = -1.0f;
		}
	}

	CallType callType = CallType::Fill;
	int triangleCount = 4;
	if (npaths == 1 && paths[0].convex) {
		callType = CallType::ConvexFill;
		triangleCount = 0;
	}

	const bool fastPrefixOnly = !paintUsesFullEffects(paint, blendModeId);
	FragUniforms tempFrag;
	m_impl->convertPaint(&tempFrag, paint, effectiveScissor, fringe, fringe, -1.0f, hasHwScissor, blendModeId, fastPrefixOnly);
	const ShaderBucket bucket = m_impl->classifyPaintBucket(tempFrag, paint.image, paint.customShader != nullptr, true);

	uint32_t c = paint.innerColor.premultiplied().toRGBA8();

	int numTriVerts = 0;
	if (npaths == 1 && paths[0].convex) {
		const Path& p = paths[0];
		if (p.fillCount >= 3) numTriVerts += (p.fillCount - 2) * 3;
		if (p.strokeCount >= 3) numTriVerts += (p.strokeCount - 2) * 3;
	}

	if (numTriVerts > 0) {
		const Path& p = paths[0];
		if (Call* last = m_impl->findCoalescableTrianglesCall(bucket, paint.image, paint.customShader, paint.xform,
		                                                      blend, hasHwScissor, hwScissor, programmableBlend, tempFrag)) {
			int offset = static_cast<int>(m_impl->verts.size());
			m_impl->verts.resize(offset + numTriVerts);
			Vertex* dst = &m_impl->verts[offset];

			if (p.fillCount >= 3 && p.fill != nullptr) {
				const Vertex& v0 = p.fill[0];
				for (int k = 0; k < p.fillCount - 2; ++k) {
					dst[0] = v0;            dst[0].color = c;
					dst[1] = p.fill[k + 1]; dst[1].color = c;
					dst[2] = p.fill[k + 2]; dst[2].color = c;
					dst += 3;
				}
			}
			if (p.strokeCount >= 3 && p.stroke != nullptr) {
				for (int k = 0; k < p.strokeCount - 2; ++k) {
					if ((k & 1) == 0) {
						dst[0] = p.stroke[k];     dst[0].color = c;
						dst[1] = p.stroke[k + 1]; dst[1].color = c;
						dst[2] = p.stroke[k + 2]; dst[2].color = c;
					} else {
						dst[0] = p.stroke[k + 1]; dst[0].color = c;
						dst[1] = p.stroke[k];     dst[1].color = c;
						dst[2] = p.stroke[k + 2]; dst[2].color = c;
					}
					dst += 3;
				}
			}
			last->triangleCount += numTriVerts;
			return;
		}

		// Create a new Triangles call for this convex shape
		Call call;
		call.type = CallType::Triangles;
		call.bucket = bucket;
		call.clipDepth = m_impl->currentClipDepth;
		call.image = paint.image;
		call.paintXform = paint.xform;
		call.customShader = paint.customShader;
		call.blendFunc = blend;
		call.programmableBlend = programmableBlend;
		if (programmableBlend && bounds != nullptr) {
			call.hasCallBounds = true;
			call.callBounds[0] = bounds[0] - fringe;
			call.callBounds[1] = bounds[1] - fringe;
			call.callBounds[2] = bounds[2] + fringe;
			call.callBounds[3] = bounds[3] + fringe;
		}
		call.triangleOffset = static_cast<int>(m_impl->verts.size());
		call.triangleCount = numTriVerts;
		if (hasHwScissor) {
			call.scissorRect[0] = hwScissor[0];
			call.scissorRect[1] = hwScissor[1];
			call.scissorRect[2] = hwScissor[2];
			call.scissorRect[3] = hwScissor[3];
		} else {
			call.scissorRect[0] = -1;
		}

		m_impl->verts.resize(call.triangleOffset + numTriVerts);
		Vertex* dst = &m_impl->verts[call.triangleOffset];
		if (p.fillCount >= 3 && p.fill != nullptr) {
			const Vertex& v0 = p.fill[0];
			for (int k = 0; k < p.fillCount - 2; ++k) {
				dst[0] = v0;            dst[0].color = c;
				dst[1] = p.fill[k + 1]; dst[1].color = c;
				dst[2] = p.fill[k + 2]; dst[2].color = c;
				dst += 3;
			}
		}
		if (p.strokeCount >= 3 && p.stroke != nullptr) {
			for (int k = 0; k < p.strokeCount - 2; ++k) {
				if ((k & 1) == 0) {
					dst[0] = p.stroke[k];     dst[0].color = c;
					dst[1] = p.stroke[k + 1]; dst[1].color = c;
					dst[2] = p.stroke[k + 2]; dst[2].color = c;
				} else {
					dst[0] = p.stroke[k + 1]; dst[0].color = c;
					dst[1] = p.stroke[k];     dst[1].color = c;
					dst[2] = p.stroke[k + 2]; dst[2].color = c;
				}
				dst += 3;
			}
		}

		m_impl->assignCallUniforms(call, tempFrag);
		m_impl->calls.push_back(call);
		return;
	}

	Call call;
	call.type = callType;
	call.bucket = bucket;
	call.evenOdd = (npaths > 0 && paths[0].winding == Winding::EvenOdd);
	call.clipDepth = m_impl->currentClipDepth;
	call.triangleCount = triangleCount;
	call.pathOffset = static_cast<int>(m_impl->paths.size());
	call.pathCount = npaths;
	call.image = paint.image;
	call.paintXform = paint.xform;
	call.customShader = paint.customShader;
	call.blendFunc = blend;
	call.programmableBlend = programmableBlend;
	if (programmableBlend && bounds != nullptr) {
		call.hasCallBounds = true;
		call.callBounds[0] = bounds[0] - fringe;
		call.callBounds[1] = bounds[1] - fringe;
		call.callBounds[2] = bounds[2] + fringe;
		call.callBounds[3] = bounds[3] + fringe;
	}
	if (hasHwScissor) {
		call.scissorRect[0] = hwScissor[0];
		call.scissorRect[1] = hwScissor[1];
		call.scissorRect[2] = hwScissor[2];
		call.scissorRect[3] = hwScissor[3];
	} else {
		call.scissorRect[0] = -1;
	}

	m_impl->paths.resize(call.pathOffset + npaths);

	int maxverts = call.triangleCount;
	for (int i = 0; i < npaths; i++) {
		maxverts += paths[i].fillCount + paths[i].strokeCount;
	}

	int offset = static_cast<int>(m_impl->verts.size());
	m_impl->verts.resize(offset + maxverts);

	int curOffset = offset;
	for (int i = 0; i < npaths; i++) {
		const Path& path = paths[i];
		GLPath& copy = m_impl->paths[call.pathOffset + i];
		copy = GLPath{};
		if (path.fillCount > 0 && path.fill != nullptr) {
			copy.fillOffset = curOffset;
			copy.fillCount = path.fillCount;
			std::memcpy(&m_impl->verts[curOffset], path.fill, path.fillCount * sizeof(Vertex));
			for (int k = 0; k < path.fillCount; ++k) m_impl->verts[curOffset + k].color = c;
			curOffset += path.fillCount;
		}
		if (path.strokeCount > 0 && path.stroke != nullptr) {
			copy.strokeOffset = curOffset;
			copy.strokeCount = path.strokeCount;
			std::memcpy(&m_impl->verts[curOffset], path.stroke, path.strokeCount * sizeof(Vertex));
			for (int k = 0; k < path.strokeCount; ++k) m_impl->verts[curOffset + k].color = c;
			curOffset += path.strokeCount;
		}
	}

	if (call.type == CallType::Fill) {
		call.triangleOffset = curOffset;
		Vertex* quad = &m_impl->verts[curOffset];
		quad[0] = Vertex(bounds[2], bounds[3], 0.5f, 1.0f, c);
		quad[1] = Vertex(bounds[2], bounds[1], 0.5f, 1.0f, c);
		quad[2] = Vertex(bounds[0], bounds[3], 0.5f, 1.0f, c);
		quad[3] = Vertex(bounds[0], bounds[1], 0.5f, 1.0f, c);
		curOffset += 4;
	}

	m_impl->assignCallUniforms(call, tempFrag);
	m_impl->verts.resize(curOffset);
	m_impl->calls.push_back(call);
}

void GL3Renderer::renderStroke(const Paint& paint, const CompositeOperationState& compOp,
                              const Scissor& scissor, float fringe, float strokeWidth,
                              const Path* paths, int npaths) {
	const bool programmableBlend = compOp.isProgrammableBlend;
	const int blendModeId = programmableBlend ? (static_cast<int>(compOp.blendMode) + 1) : 0;
	Blend blend = programmableBlend ? Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO} : convertBlend(compOp);
	int hwScissor[4]{-1, -1, -1, -1};
	bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);

	bool useStencil = ((m_impl->flags & CreateFlags::StencilStrokes) != 0);
	if (npaths == 1 && paths[0].convex) {
		useStencil = false; // Convex paths cannot self-intersect, bypass stencil overhead!
	}
	if (strokeWidth <= fringe * 1.5f || (paint.innerColor.a >= 0.999f && paint.outerColor.a >= 0.999f)) {
		useStencil = false; // Hairlines and fully opaque strokes do not produce semi-transparent overlap artifacts!
	}

	const bool fastPrefixOnly = !paintUsesFullEffects(paint, blendModeId);
	FragUniforms tempFrag;
	m_impl->convertPaint(&tempFrag, paint, scissor, strokeWidth, fringe, -1.0f, hasHwScissor, blendModeId, fastPrefixOnly);
	const ShaderBucket bucket = m_impl->classifyPaintBucket(tempFrag, paint.image, paint.customShader != nullptr, !useStencil);

	uint32_t c = paint.innerColor.premultiplied().toRGBA8();

	int numTriVerts = 0;
	if (!useStencil) {
		for (int i = 0; i < npaths; ++i) {
			if (paths[i].strokeCount >= 3) {
				numTriVerts += (paths[i].strokeCount - 2) * 3;
			}
		}
	}

	if (numTriVerts > 0) {
		if (Call* last = m_impl->findCoalescableTrianglesCall(bucket, paint.image, paint.customShader, paint.xform,
		                                                      blend, hasHwScissor, hwScissor, programmableBlend, tempFrag)) {
			int offset = static_cast<int>(m_impl->verts.size());
			m_impl->verts.resize(offset + numTriVerts);
			Vertex* dst = &m_impl->verts[offset];

			for (int i = 0; i < npaths; ++i) {
				const Path& p = paths[i];
				if (p.strokeCount >= 3 && p.stroke != nullptr) {
					for (int k = 0; k < p.strokeCount - 2; ++k) {
						if ((k & 1) == 0) {
							dst[0] = p.stroke[k];     dst[0].color = c;
							dst[1] = p.stroke[k + 1]; dst[1].color = c;
							dst[2] = p.stroke[k + 2]; dst[2].color = c;
						} else {
							dst[0] = p.stroke[k + 1]; dst[0].color = c;
							dst[1] = p.stroke[k];     dst[1].color = c;
							dst[2] = p.stroke[k + 2]; dst[2].color = c;
						}
						dst += 3;
					}
				}
			}
			last->triangleCount += numTriVerts;
			return;
		}

		// Create a new Triangles call for this stroke
		Call call;
		call.type = CallType::Triangles;
		call.bucket = bucket;
		call.clipDepth = m_impl->currentClipDepth;
		call.image = paint.image;
		call.paintXform = paint.xform;
		call.customShader = paint.customShader;
		call.blendFunc = blend;
		call.programmableBlend = programmableBlend;
		call.triangleOffset = static_cast<int>(m_impl->verts.size());
		call.triangleCount = numTriVerts;
		if (hasHwScissor) {
			call.scissorRect[0] = hwScissor[0];
			call.scissorRect[1] = hwScissor[1];
			call.scissorRect[2] = hwScissor[2];
			call.scissorRect[3] = hwScissor[3];
		} else {
			call.scissorRect[0] = -1;
		}

		m_impl->verts.resize(call.triangleOffset + numTriVerts);
		Vertex* dst = &m_impl->verts[call.triangleOffset];
		for (int i = 0; i < npaths; ++i) {
			const Path& p = paths[i];
			if (p.strokeCount >= 3 && p.stroke != nullptr) {
				for (int k = 0; k < p.strokeCount - 2; ++k) {
					if ((k & 1) == 0) {
						dst[0] = p.stroke[k];     dst[0].color = c;
						dst[1] = p.stroke[k + 1]; dst[1].color = c;
						dst[2] = p.stroke[k + 2]; dst[2].color = c;
					} else {
						dst[0] = p.stroke[k + 1]; dst[0].color = c;
						dst[1] = p.stroke[k];     dst[1].color = c;
						dst[2] = p.stroke[k + 2]; dst[2].color = c;
					}
					dst += 3;
				}
			}
		}

		if (programmableBlend) {
			Impl::setCallBoundsFromVerts(call, &m_impl->verts[call.triangleOffset], numTriVerts, fringe + 1.0f);
		}
		m_impl->assignCallUniforms(call, tempFrag);
		m_impl->calls.push_back(call);
		return;
	}

	Call call;
	call.type = CallType::Stroke;
	call.bucket = bucket;
	call.clipDepth = m_impl->currentClipDepth;
	call.pathOffset = static_cast<int>(m_impl->paths.size());
	call.pathCount = npaths;
	call.image = paint.image;
	call.paintXform = paint.xform;
	call.customShader = paint.customShader;
	call.blendFunc = blend;
	call.programmableBlend = programmableBlend;
	call.useStencil = useStencil;
	if (hasHwScissor) {
		call.scissorRect[0] = hwScissor[0];
		call.scissorRect[1] = hwScissor[1];
		call.scissorRect[2] = hwScissor[2];
		call.scissorRect[3] = hwScissor[3];
	} else {
		call.scissorRect[0] = -1;
	}

	m_impl->paths.resize(call.pathOffset + npaths);

	int maxverts = 0;
	for (int i = 0; i < npaths; i++) {
		maxverts += paths[i].strokeCount;
	}

	int offset = static_cast<int>(m_impl->verts.size());
	m_impl->verts.resize(offset + maxverts);

	int curOffset = offset;
	for (int i = 0; i < npaths; i++) {
		const Path& path = paths[i];
		GLPath& copy = m_impl->paths[call.pathOffset + i];
		copy = GLPath{};
		if (path.strokeCount > 0 && path.stroke != nullptr) {
			copy.strokeOffset = curOffset;
			copy.strokeCount = path.strokeCount;
			std::memcpy(&m_impl->verts[curOffset], path.stroke, path.strokeCount * sizeof(Vertex));
			for (int k = 0; k < path.strokeCount; ++k) m_impl->verts[curOffset + k].color = c;
			curOffset += path.strokeCount;
		}
	}

	if (programmableBlend && curOffset > offset) {
		Impl::setCallBoundsFromVerts(call, &m_impl->verts[offset], curOffset - offset, fringe + 1.0f);
	}

	if (useStencil) {
		const size_t copyBytes = Impl::bucketNeedsFullUbo(bucket) ? sizeof(FragUniforms) : kFastUniformBytes;
		const int stride = Impl::bucketNeedsFullUbo(bucket) ? m_impl->fragSize : m_impl->fragFastSize;
		call.uniformStride = stride;
		call.uniformOffset = m_impl->allocFragUniforms(2, stride);
		std::memcpy(&m_impl->uniforms[call.uniformOffset], &tempFrag, copyBytes);

		FragUniforms overlapFrag;
		m_impl->convertPaint(&overlapFrag, paint, scissor, strokeWidth, fringe, 1.0f - 0.5f / 255.0f,
		                     hasHwScissor, blendModeId, fastPrefixOnly);
		std::memcpy(&m_impl->uniforms[call.uniformOffset + stride], &overlapFrag, copyBytes);
	} else {
		m_impl->assignCallUniforms(call, tempFrag);
	}

	m_impl->verts.resize(curOffset);
	m_impl->calls.push_back(call);
}

void GL3Renderer::renderTriangles(const Paint& paint, const CompositeOperationState& compOp,
                                 const Scissor& scissor, const Vertex* verts, int nverts,
                                 float fringe, int shaderType) {
	const bool programmableBlend = compOp.isProgrammableBlend;
	const int blendModeId = programmableBlend ? (static_cast<int>(compOp.blendMode) + 1) : 0;
	Blend blend = programmableBlend ? Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO} : convertBlend(compOp);
	int hwScissor[4]{-1, -1, -1, -1};
	bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);

	const bool fastPrefixOnly = !paintUsesFullEffects(paint, blendModeId, shaderType);
	FragUniforms tempFrag;
	applyTriangleShaderUniforms(&tempFrag, paint, scissor, fringe, hasHwScissor, blendModeId,
	                            shaderType, [this](int id) { return m_impl->lookupTextureInfo(id); },
	                            fastPrefixOnly);
	const ShaderBucket bucket = m_impl->classifyPaintBucket(tempFrag, paint.image, paint.customShader != nullptr, true);

	if (Call* last = m_impl->findCoalescableTrianglesCall(bucket, paint.image, paint.customShader, paint.xform,
	                                                      blend, hasHwScissor, hwScissor, programmableBlend, tempFrag)) {
		int offset = static_cast<int>(m_impl->verts.size());
		m_impl->verts.resize(offset + nverts);
		std::memcpy(&m_impl->verts[offset], verts, nverts * sizeof(Vertex));
		last->triangleCount += nverts;
		return;
	}

	Call call;
	call.type = CallType::Triangles;
	call.bucket = bucket;
	call.clipDepth = m_impl->currentClipDepth;
	call.image = paint.image;
	call.paintXform = paint.xform;
	call.customShader = paint.customShader;
	call.blendFunc = blend;
	call.programmableBlend = programmableBlend;
	call.triangleOffset = static_cast<int>(m_impl->verts.size());
	call.triangleCount = nverts;
	if (hasHwScissor) {
		call.scissorRect[0] = hwScissor[0];
		call.scissorRect[1] = hwScissor[1];
		call.scissorRect[2] = hwScissor[2];
		call.scissorRect[3] = hwScissor[3];
	} else {
		call.scissorRect[0] = -1;
	}

	m_impl->verts.resize(call.triangleOffset + nverts);
	std::memcpy(&m_impl->verts[call.triangleOffset], verts, nverts * sizeof(Vertex));

	if (programmableBlend) {
		Impl::setCallBoundsFromVerts(call, verts, nverts, fringe + 1.0f);
	}
	m_impl->assignCallUniforms(call, tempFrag);
	m_impl->calls.push_back(call);
}

void GL3Renderer::renderClip(const RenderPath* paths, int npaths, Winding winding, int targetDepth,
                             const Scissor& scissor, const float* bounds) {
	if (npaths <= 0 || paths == nullptr || m_impl->fragSize <= 0) return;

	int hwScissor[4]{-1, -1, -1, -1};
	bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);

	Call call;
	call.type = CallType::Clip;
	call.bucket = ShaderBucket::SimpleColor;
	call.clipDepth = targetDepth;
	call.winding = winding;
	call.blendFunc = Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};
	if (hasHwScissor) {
		call.scissorRect[0] = hwScissor[0];
		call.scissorRect[1] = hwScissor[1];
		call.scissorRect[2] = hwScissor[2];
		call.scissorRect[3] = hwScissor[3];
	} else {
		call.scissorRect[0] = -1;
	}

	call.pathOffset = static_cast<int>(m_impl->paths.size());
	call.pathCount = npaths;
	m_impl->paths.resize(call.pathOffset + npaths);

	int maxverts = 4; // cover quad
	for (int i = 0; i < npaths; i++) {
		maxverts += paths[i].fillCount;
	}

	int offset = static_cast<int>(m_impl->verts.size());
	m_impl->verts.resize(offset + maxverts);

	int curOffset = offset;
	for (int i = 0; i < npaths; i++) {
		const RenderPath& path = paths[i];
		GLPath& copy = m_impl->paths[call.pathOffset + i];
		copy = GLPath{};
		if (path.fillCount > 0 && path.fill != nullptr) {
			copy.fillOffset = curOffset;
			copy.fillCount = path.fillCount;
			std::memcpy(&m_impl->verts[curOffset], path.fill, path.fillCount * sizeof(Vertex));
			curOffset += path.fillCount;
		}
	}

	call.triangleOffset = curOffset;
	call.triangleCount = 4;
	Vertex* quad = &m_impl->verts[curOffset];
	float minX = bounds ? bounds[0] : 0.0f;
	float minY = bounds ? bounds[1] : 0.0f;
	float maxX = bounds ? bounds[2] : m_impl->view[0];
	float maxY = bounds ? bounds[3] : m_impl->view[1];

	quad[0] = Vertex(maxX, maxY, 0.5f, 1.0f);
	quad[1] = Vertex(maxX, minY, 0.5f, 1.0f);
	quad[2] = Vertex(minX, maxY, 0.5f, 1.0f);
	quad[3] = Vertex(minX, minY, 0.5f, 1.0f);
	curOffset += 4;

	FragUniforms clipFrag{};
	clipFrag.strokeThr = -1.0f;
	clipFrag.type = ShaderSimple;
	m_impl->assignCallUniforms(call, clipFrag);

	m_impl->verts.resize(curOffset);
	m_impl->calls.push_back(call);
	m_impl->currentClipDepth = targetDepth;
}

void GL3Renderer::restoreClip(int targetDepth) {
	if (m_impl->fragSize <= 0) return;

	Call call;
	call.type = CallType::RestoreClip;
	call.bucket = ShaderBucket::SimpleColor;
	call.clipDepth = targetDepth;
	call.blendFunc = Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};
	call.scissorRect[0] = -1;

	int offset = static_cast<int>(m_impl->verts.size());
	m_impl->verts.resize(offset + 4);

	Vertex* quad = &m_impl->verts[offset];
	quad[0] = Vertex(m_impl->view[0], m_impl->view[1], 0.5f, 1.0f);
	quad[1] = Vertex(m_impl->view[0], 0.0f, 0.5f, 1.0f);
	quad[2] = Vertex(0.0f, m_impl->view[1], 0.5f, 1.0f);
	quad[3] = Vertex(0.0f, 0.0f, 0.5f, 1.0f);

	call.triangleOffset = offset;
	call.triangleCount = 4;

	FragUniforms clipFrag{};
	clipFrag.strokeThr = -1.0f;
	clipFrag.type = ShaderSimple;
	m_impl->assignCallUniforms(call, clipFrag);

	m_impl->calls.push_back(call);
	m_impl->currentClipDepth = targetDepth;
}

void GL3Renderer::resetClip() {
	restoreClip(0);
}

void GL3Renderer::pushLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) {
	(void)parentScissor;
	if (m_impl->fragSize <= 0) return;

	Call call;
	call.type = CallType::PushLayer;
	call.clipDepth = parentClipDepth;
	call.layerIndex = static_cast<int>(m_impl->layerDescs.size());
	m_impl->layerDescs.push_back(desc);
	call.blendFunc = Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};
	call.scissorRect[0] = -1;
	m_impl->calls.push_back(call);
	m_impl->currentClipDepth = 0;
}

void GL3Renderer::popLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) {
	if (m_impl->fragSize <= 0) return;

	const bool programmableBlend = desc.compOp.isProgrammableBlend;
	const int blendModeId = programmableBlend ? (static_cast<int>(desc.compOp.blendMode) + 1) : 0;
	Blend blend = programmableBlend ? Blend{GL_ONE, GL_ZERO, GL_ONE, GL_ZERO} : convertBlend(desc.compOp);

	int hwScissor[4]{-1, -1, -1, -1};
	bool hasHwScissor = m_impl->getHardwareScissor(parentScissor, hwScissor);

	Call call;
	call.type = CallType::PopLayer;
	call.layerIndex = static_cast<int>(m_impl->layerDescs.size());
	m_impl->layerDescs.push_back(desc);
	call.blendFunc = blend;
	call.programmableBlend = programmableBlend;
	call.clipDepth = parentClipDepth;
	if (hasHwScissor) {
		call.scissorRect[0] = hwScissor[0];
		call.scissorRect[1] = hwScissor[1];
		call.scissorRect[2] = hwScissor[2];
		call.scissorRect[3] = hwScissor[3];
	} else {
		call.scissorRect[0] = -1;
	}

	float minX = 0.0f;
	float minY = 0.0f;
	float maxX = m_impl->view[0];
	float maxY = m_impl->view[1];
	if (desc.hasBounds) {
		minX = desc.bounds[0];
		minY = desc.bounds[1];
		maxX = desc.bounds[2];
		maxY = desc.bounds[3];
	}

	Paint popPaint;
	const bool hasCustomBlender = desc.blender.has_value() && desc.blender->is_custom();
	popPaint.innerColor = Color(1.0f, 1.0f, 1.0f, hasCustomBlender ? 1.0f : std::clamp(desc.opacity, 0.0f, 1.0f));
	popPaint.outerColor = popPaint.innerColor;
	popPaint.hasColorMatrix = desc.hasColorMatrix;
	popPaint.colorMatrix = desc.colorMatrix;
	popPaint.colorTransfer = desc.colorTransfer;
	popPaint.hasColorBlend = desc.hasColorBlend;
	popPaint.colorBlendColor = desc.colorBlendColor;
	popPaint.colorBlendMode = desc.colorBlendMode;

	auto appendQuad = [&](float x0, float y0, float x1, float y1,
	                      float u0, float vTop, float u1, float vBot) {
		const int offset = static_cast<int>(m_impl->verts.size());
		m_impl->verts.resize(offset + 6);
		Vertex* q = &m_impl->verts[offset];
		q[0] = Vertex(x0, y0, u0, vTop, 0xFFFFFFFFu);
		q[1] = Vertex(x1, y0, u1, vTop, 0xFFFFFFFFu);
		q[2] = Vertex(x1, y1, u1, vBot, 0xFFFFFFFFu);
		q[3] = Vertex(x0, y0, u0, vTop, 0xFFFFFFFFu);
		q[4] = Vertex(x1, y1, u1, vBot, 0xFFFFFFFFu);
		q[5] = Vertex(x0, y1, u0, vBot, 0xFFFFFFFFu);
	};

	const bool validArea = (maxX > minX && maxY > minY && m_impl->view[0] > 0.0f && m_impl->view[1] > 0.0f);
	const bool fastPrefixOnly = !paintUsesFullEffects(popPaint, blendModeId);
	call.bucket = fastPrefixOnly ? ShaderBucket::FastCore : ShaderBucket::EffectsFull;
	const size_t copyBytes = fastPrefixOnly ? kFastUniformBytes : sizeof(FragUniforms);
	const int stride = fastPrefixOnly ? m_impl->fragFastSize : m_impl->fragSize;
	call.uniformStride = stride;

	if (validArea) {
		const float invW = 1.0f / m_impl->view[0];
		const float invH = 1.0f / m_impl->view[1];
		const float u0 = minX * invW;
		const float u1 = maxX * invW;
		const float vTop = 1.0f - minY * invH;
		const float vBot = 1.0f - maxY * invH;

		if (desc.isDropShadow) {
			if (desc.shadowOnly) {
				call.hasCallBounds = true;
				call.callBounds[0] = minX + desc.shadowDx;
				call.callBounds[1] = minY + desc.shadowDy;
				call.callBounds[2] = maxX + desc.shadowDx;
				call.callBounds[3] = maxY + desc.shadowDy;
				call.triangleOffset = static_cast<int>(m_impl->verts.size());
				appendQuad(minX + desc.shadowDx, minY + desc.shadowDy,
				           maxX + desc.shadowDx, maxY + desc.shadowDy,
				           u0, vTop, u1, vBot);
				call.triangleCount = 6;
				call.uniformOffset = m_impl->allocFragUniforms(1, stride);
				FragUniforms frag;
				m_impl->convertPaint(&frag, popPaint, parentScissor, 1.0f, 1.0f, -1.0f,
				                     hasHwScissor, blendModeId, fastPrefixOnly);
				frag.innerCol = popPaint.innerColor.premultiplied();
				frag.outerCol = desc.shadowColor.premultiplied();
				frag.type = ShaderImg;
				frag.texType = 4;
				std::memcpy(&m_impl->uniforms[call.uniformOffset], &frag, copyBytes);
			} else {
				const bool needsIntermediate = (desc.opacity < 0.999f || desc.hasColorMatrix ||
				                                 desc.colorTransfer != 0 || desc.hasColorBlend ||
				                                 desc.compOp.blendMode != BlendMode::SourceOver ||
				                                 desc.compOp.isProgrammableBlend);
				call.hasCallBounds = true;
				call.callBounds[0] = std::min(minX, minX + desc.shadowDx);
				call.callBounds[1] = std::min(minY, minY + desc.shadowDy);
				call.callBounds[2] = std::max(maxX, maxX + desc.shadowDx);
				call.callBounds[3] = std::max(maxY, maxY + desc.shadowDy);
				call.triangleOffset = static_cast<int>(m_impl->verts.size());
				appendQuad(minX + desc.shadowDx, minY + desc.shadowDy,
				           maxX + desc.shadowDx, maxY + desc.shadowDy,
				           u0, vTop, u1, vBot);
				appendQuad(minX, minY, maxX, maxY, u0, vTop, u1, vBot);

				if (!needsIntermediate) {
					call.triangleCount = 12;
					call.uniformOffset = m_impl->allocFragUniforms(2, stride);
					FragUniforms fgFrag;
					m_impl->convertPaint(&fgFrag, popPaint, parentScissor, 1.0f, 1.0f, -1.0f,
					                     hasHwScissor, blendModeId, fastPrefixOnly);
					fgFrag.innerCol = popPaint.innerColor.premultiplied();
					fgFrag.outerCol = fgFrag.innerCol;
					fgFrag.type = ShaderImg;
					fgFrag.texType = 3;
					std::memcpy(&m_impl->uniforms[call.uniformOffset], &fgFrag, copyBytes);

					FragUniforms shFrag;
					Paint shPaint;
					shPaint.innerColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
					m_impl->convertPaint(&shFrag, shPaint, parentScissor, 1.0f, 1.0f, -1.0f,
					                     hasHwScissor, 0, true);
					shFrag.innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
					shFrag.outerCol = desc.shadowColor.premultiplied();
					shFrag.type = ShaderImg;
					shFrag.texType = 4;
					std::memcpy(&m_impl->uniforms[call.uniformOffset + stride], &shFrag, kFastUniformBytes);
				} else {
					const float uMinX = std::clamp(call.callBounds[0], 0.0f, m_impl->view[0]);
					const float uMinY = std::clamp(call.callBounds[1], 0.0f, m_impl->view[1]);
					const float uMaxX = std::clamp(call.callBounds[2], 0.0f, m_impl->view[0]);
					const float uMaxY = std::clamp(call.callBounds[3], 0.0f, m_impl->view[1]);
					appendQuad(uMinX, uMinY, uMaxX, uMaxY,
					           uMinX * invW, 1.0f - uMinY * invH, uMaxX * invW, 1.0f - uMaxY * invH);
					call.triangleCount = 18;
					call.uniformOffset = m_impl->allocFragUniforms(3, stride);

					FragUniforms compFrag;
					m_impl->convertPaint(&compFrag, popPaint, parentScissor, 1.0f, 1.0f, -1.0f,
					                     hasHwScissor, blendModeId, fastPrefixOnly);
					compFrag.innerCol = popPaint.innerColor.premultiplied();
					compFrag.outerCol = compFrag.innerCol;
					compFrag.type = ShaderImg;
					compFrag.texType = 3;
					std::memcpy(&m_impl->uniforms[call.uniformOffset], &compFrag, copyBytes);

					Scissor noScissor;
					Paint idPaint;
					idPaint.innerColor = Color(1.0f, 1.0f, 1.0f, 1.0f);

					FragUniforms shFrag;
					m_impl->convertPaint(&shFrag, idPaint, noScissor, 1.0f, 1.0f, -1.0f, false, 0, true);
					shFrag.innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
					shFrag.outerCol = desc.shadowColor.premultiplied();
					shFrag.type = ShaderImg;
					shFrag.texType = 4;
					std::memcpy(&m_impl->uniforms[call.uniformOffset + stride], &shFrag, kFastUniformBytes);

					FragUniforms fgFrag;
					m_impl->convertPaint(&fgFrag, idPaint, noScissor, 1.0f, 1.0f, -1.0f, false, 0, true);
					fgFrag.innerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
					fgFrag.outerCol = Color(1.0f, 1.0f, 1.0f, 1.0f);
					fgFrag.type = ShaderImg;
					fgFrag.texType = 3;
					std::memcpy(&m_impl->uniforms[call.uniformOffset + 2 * stride], &fgFrag, kFastUniformBytes);
				}
			}
		} else {
			call.hasCallBounds = true;
			call.callBounds[0] = minX;
			call.callBounds[1] = minY;
			call.callBounds[2] = maxX;
			call.callBounds[3] = maxY;
			call.triangleOffset = static_cast<int>(m_impl->verts.size());
			appendQuad(minX, minY, maxX, maxY, u0, vTop, u1, vBot);
			call.triangleCount = 6;
			call.uniformOffset = m_impl->allocFragUniforms(1, stride);
			FragUniforms frag;
			m_impl->convertPaint(&frag, popPaint, parentScissor, 1.0f, 1.0f, -1.0f,
			                     hasHwScissor, blendModeId, fastPrefixOnly);
			frag.innerCol = popPaint.innerColor.premultiplied();
			frag.outerCol = frag.innerCol;
			frag.type = ShaderImg;
			frag.texType = 3; // PremultipliedLayer: colorFilters before layer opacity
			std::memcpy(&m_impl->uniforms[call.uniformOffset], &frag, copyBytes);
		}
	} else {
		call.uniformOffset = m_impl->allocFragUniforms(1, stride);
		FragUniforms frag;
		m_impl->convertPaint(&frag, popPaint, parentScissor, 1.0f, 1.0f, -1.0f,
		                     hasHwScissor, blendModeId, fastPrefixOnly);
		frag.type = ShaderImg;
		frag.texType = 3;
		std::memcpy(&m_impl->uniforms[call.uniformOffset], &frag, copyBytes);
	}

	m_impl->calls.push_back(call);
	m_impl->currentClipDepth = parentClipDepth;
}

std::unique_ptr<Context> createContextGL3(int flags) {
	auto renderer = std::make_unique<GL3Renderer>(flags);
	return std::make_unique<Context>(std::move(renderer));
}

} // namespace nisaba::gpu
