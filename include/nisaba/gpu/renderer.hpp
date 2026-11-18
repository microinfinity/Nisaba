#pragma once

#include "nisaba/gpu/types.hpp"
#include "nisaba/gpu/gpu_types.hpp"
#include <cstring>
#include <span>
#include <vector>

namespace nisaba::gpu {

// -------------------------------------------------------------
// Vertex (GPU vertex buffer layout)
// -------------------------------------------------------------
struct Vertex {
	float x{0.0f};
	float y{0.0f};
	float u{0.0f};
	float v{0.0f};
	uint32_t color{0xffffffff};

	constexpr Vertex() = default;
	constexpr Vertex(float x_, float y_, float u_, float v_, uint32_t color_ = 0xffffffff)
		: x(x_), y(y_), u(u_), v(v_), color(color_) {}
};

// -------------------------------------------------------------
// RenderPath: Segment slice in pre-allocated contour buffer passed from Tessellator to Renderer
// -------------------------------------------------------------
struct RenderPath {
	int first{0};
	int count{0};
	bool closed{false};
	int nbevel{0};
	const Vertex* fill{nullptr};
	int fillCount{0};
	const Vertex* stroke{nullptr};
	int strokeCount{0};
	Winding winding{Winding::CounterClockwise};
	bool convex{false};
};

using Path = RenderPath;

enum class TextureType {
	Alpha,
	RGBA,
	BGRA
};

// -------------------------------------------------------------
// Abstract GPU Renderer Interface
// -------------------------------------------------------------
class Renderer {
public:
	virtual ~Renderer() = default;

	virtual bool init() = 0;
	virtual void shutdown() = 0;

	/// Invalidates all cached GL/backend state assumptions when sharing a GPU context with an external host.
	virtual void resetState() {}

	virtual bool edgeAntiAlias() const = 0;

	virtual int createTexture(TextureType type, int width, int height, int imageFlags, const unsigned char* data) = 0;
	virtual int createTextureWithStride(TextureType type, int width, int height, int strideBytes, int imageFlags, const unsigned char* data) {
		if (width <= 0 || height <= 0) return 0;
		int bpp = (type == TextureType::Alpha) ? 1 : 4;
		int tightStride = width * bpp;
		int effectiveStride = (strideBytes > 0) ? strideBytes : tightStride;
		if (!data || (effectiveStride == tightStride && type != TextureType::BGRA)) {
			return createTexture(type == TextureType::BGRA ? TextureType::RGBA : type, width, height, imageFlags, data);
		}
		if (effectiveStride < tightStride) return 0;
		std::vector<unsigned char> packed(static_cast<size_t>(height) * static_cast<size_t>(tightStride));
		if (type == TextureType::BGRA) {
			for (int y = 0; y < height; ++y) {
				const unsigned char* s = data + static_cast<size_t>(y) * static_cast<size_t>(effectiveStride);
				unsigned char* d = packed.data() + static_cast<size_t>(y) * static_cast<size_t>(tightStride);
				for (int x = 0; x < width; ++x) {
					d[x * 4 + 0] = s[x * 4 + 2];
					d[x * 4 + 1] = s[x * 4 + 1];
					d[x * 4 + 2] = s[x * 4 + 0];
					d[x * 4 + 3] = s[x * 4 + 3];
				}
			}
			return createTexture(TextureType::RGBA, width, height, imageFlags, packed.data());
		}
		for (int y = 0; y < height; ++y) {
			std::memcpy(packed.data() + static_cast<size_t>(y) * static_cast<size_t>(tightStride),
			            data + static_cast<size_t>(y) * static_cast<size_t>(effectiveStride),
			            static_cast<size_t>(tightStride));
		}
		return createTexture(type, width, height, imageFlags, packed.data());
	}
	virtual bool deleteTexture(int image) = 0;
	virtual bool updateTexture(int image, int x, int y, int width, int height, const unsigned char* data) = 0;
	virtual bool getTextureSize(int image, int& outWidth, int& outHeight) = 0;
	virtual int textureFlags(int image) const {
		(void)image;
		return 0;
	}
	virtual int createTextureFromNativeHandle(uint64_t handle, int w, int h, int imageFlags) {
		(void)handle; (void)w; (void)h; (void)imageFlags;
		return 0;
	}
	virtual GpuBackendType backendType() const {
		return GpuBackendType::OpenGL;
	}

	virtual void viewport(float width, float height, float devicePixelRatio) = 0;
	virtual void cancel() = 0;
	virtual void flush() = 0;

	virtual void renderFill(const Paint& paint, const CompositeOperationState& compOp,
	                       const Scissor& scissor, float fringe, const float* bounds,
	                       const RenderPath* paths, int npaths) = 0;

	virtual void renderStroke(const Paint& paint, const CompositeOperationState& compOp,
	                         const Scissor& scissor, float fringe, float strokeWidth,
	                         const RenderPath* paths, int npaths) = 0;

	virtual void renderTriangles(const Paint& paint, const CompositeOperationState& compOp,
	                            const Scissor& scissor, const Vertex* verts, int nverts,
	                            float fringe, int shaderType = 3) = 0;

	virtual void renderClip(const RenderPath* paths, int npaths, Winding winding, int targetDepth,
	                       const Scissor& scissor, const float* bounds) {
		(void)paths; (void)npaths; (void)winding; (void)targetDepth; (void)scissor; (void)bounds;
	}

	virtual void restoreClip(int targetDepth) {
		(void)targetDepth;
	}

	virtual void resetClip() {
	}

	virtual void pushLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) {
		(void)desc; (void)parentScissor; (void)parentClipDepth;
	}

	virtual void popLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) {
		(void)desc; (void)parentScissor; (void)parentClipDepth;
	}

	virtual bool supportsDRRectShader() const noexcept { return false; }
};

} // namespace nisaba::gpu
