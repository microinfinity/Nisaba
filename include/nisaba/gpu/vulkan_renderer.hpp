#pragma once

#include "nisaba/gpu/renderer.hpp"
#include "nisaba/gpu/context.hpp"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>

namespace nisaba::gpu {

class VulkanSurface;

class VulkanRenderer final : public Renderer {
public:
    VulkanRenderer(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkQueue queue,
        uint32_t queueFamilyIndex,
        VkRenderPass renderPass,
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
        int flags = CreateFlags::Antialias | CreateFlags::StencilStrokes
    );

    ~VulkanRenderer() override;

    bool init() override;
    void shutdown() override;

    bool edgeAntiAlias() const override;
    GpuBackendType backendType() const override { return GpuBackendType::Vulkan; }

    int createTexture(TextureType type, int width, int height, int imageFlags, const unsigned char* data) override;
    int createTextureWithStride(TextureType type, int width, int height, int strideBytes, int imageFlags, const unsigned char* data) override;
    bool deleteTexture(int image) override;
    bool updateTexture(int image, int x, int y, int width, int height, const unsigned char* data) override;
    bool getTextureSize(int image, int& outWidth, int& outHeight) override;
    int textureFlags(int image) const override;
    int createTextureFromNativeHandle(uint64_t handle, int w, int h, int imageFlags) override;

    void viewport(float width, float height, float devicePixelRatio) override;
    void cancel() override;
    void flush() override;

    void renderFill(const Paint& paint, const CompositeOperationState& compOp,
                    const Scissor& scissor, float fringe, const float* bounds,
                    const RenderPath* paths, int npaths) override;

    void renderStroke(const Paint& paint, const CompositeOperationState& compOp,
                      const Scissor& scissor, float fringe, float strokeWidth,
                      const RenderPath* paths, int npaths) override;

    void renderTriangles(const Paint& paint, const CompositeOperationState& compOp,
                         const Scissor& scissor, const Vertex* verts, int nverts,
                         float fringe, int shaderType = 3) override;

    void renderClip(const RenderPath* paths, int npaths, Winding winding, int targetDepth,
                    const Scissor& scissor, const float* bounds) override;
    void restoreClip(int targetDepth) override;
    void resetClip() override;

    void setTargetSurface(VulkanSurface* surface);
    void setCommandBuffer(VkCommandBuffer cmd);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Convenience factory function to create a Context initialized with Vulkan backend
std::unique_ptr<Context> createContextVulkan(
    std::shared_ptr<VulkanSurface> surface,
    int flags = CreateFlags::Antialias | CreateFlags::StencilStrokes
);

std::unique_ptr<Context> createContextVulkan(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkQueue queue,
    uint32_t queueFamilyIndex,
    VkRenderPass renderPass,
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
    int flags = CreateFlags::Antialias | CreateFlags::StencilStrokes
);

} // namespace nisaba::gpu
