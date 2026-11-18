#pragma once

#include "nisaba/gpu/renderer.hpp"
#include "nisaba/gpu/context.hpp"
#include <memory>
#include <vector>

namespace nisaba::gpu {

enum ImageFlagsGL : int {
	ImageNoDelete = 1 << 16 // Do not delete OpenGL texture handle when image is deleted
};

class GL3Renderer final : public Renderer {
public:
	explicit GL3Renderer(int flags = CreateFlags::Antialias | CreateFlags::StencilStrokes);
	~GL3Renderer() override;

	bool init() override;
	void shutdown() override;
	void resetState() override;

	bool edgeAntiAlias() const override;

	int createTexture(TextureType type, int width, int height, int imageFlags, const unsigned char* data) override;
	int createTextureWithStride(TextureType type, int width, int height, int strideBytes, int imageFlags, const unsigned char* data) override;
	bool deleteTexture(int image) override;
	bool updateTexture(int image, int x, int y, int width, int height, const unsigned char* data) override;
	bool getTextureSize(int image, int& outWidth, int& outHeight) override;
	int textureFlags(int image) const override;

	void viewport(float width, float height, float devicePixelRatio) override;
	void cancel() override;
	void flush() override;

	void renderFill(const Paint& paint, const CompositeOperationState& compOp,
	                const Scissor& scissor, float fringe, const float* bounds,
	                const Path* paths, int npaths) override;

	void renderStroke(const Paint& paint, const CompositeOperationState& compOp,
	                  const Scissor& scissor, float fringe, float strokeWidth,
	                  const Path* paths, int npaths) override;

	void renderTriangles(const Paint& paint, const CompositeOperationState& compOp,
	                     const Scissor& scissor, const Vertex* verts, int nverts,
	                     float fringe, int shaderType = 3) override;

	void renderClip(const RenderPath* paths, int npaths, Winding winding, int targetDepth,
	                const Scissor& scissor, const float* bounds) override;
	void restoreClip(int targetDepth) override;
	void resetClip() override;
	void pushLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) override;
	void popLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) override;
	bool supportsDRRectShader() const noexcept override { return true; }

	// OpenGL specific utilities
	int createTextureFromNativeHandle(uint64_t handle, int w, int h, int imageFlags) override;
	GpuBackendType backendType() const override { return GpuBackendType::OpenGL; }
	int createImageFromHandle(uint32_t textureId, int w, int h, int imageFlags);
	uint32_t imageHandle(int image) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Convenience factory function to create a Context initialized with GL3 backend
std::unique_ptr<Context> createContextGL3(int flags = CreateFlags::Antialias | CreateFlags::StencilStrokes);

} // namespace nisaba::gpu
