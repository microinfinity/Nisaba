#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>
#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/gpu/vulkan_device.hpp"

namespace nisaba::gpu {

class VulkanSurface : public GpuSurface {
public:
    static std::shared_ptr<VulkanSurface> create(
        std::shared_ptr<VulkanDevice> device,
        uint32_t width,
        uint32_t height,
        uint32_t samples = 4
    );

    VulkanSurface(
        std::shared_ptr<VulkanDevice> device,
        uint32_t width,
        uint32_t height,
        uint32_t samples
    );

    ~VulkanSurface() override;

    [[nodiscard]] GpuBackendType backend_type() const noexcept override {
        return GpuBackendType::Vulkan;
    }

    std::unique_ptr<Context> create_context(int flags = 0) override;

    void bind() override;
    void resolve() override;
    void clear(nisaba::Color color) override;
    void read_pixels(void* dst_rgba) override;
    void read_pixels(PixmapMut& dst) override;
    [[nodiscard]] std::optional<Pixmap> to_pixmap() override;

    [[nodiscard]] VkRenderPass render_pass() const noexcept {
        return (preserve_contents_ && render_pass_load_ != VK_NULL_HANDLE) ? render_pass_load_ : render_pass_;
    }
    [[nodiscard]] VkRenderPass render_pass_load() const noexcept { return render_pass_load_; }
    [[nodiscard]] VkFramebuffer framebuffer() const noexcept { return framebuffer_; }
    [[nodiscard]] VkCommandBuffer current_command_buffer() const noexcept { return command_buffer_; }
    [[nodiscard]] VkImage color_image() const noexcept { return color_image_; }
    [[nodiscard]] VkImageView color_view() const noexcept { return color_view_; }
    [[nodiscard]] bool in_render_pass() const noexcept { return in_render_pass_; }

    void begin_render_pass(nisaba::Color clear_color, bool load_previous = false);
    void end_render_pass();

private:

    std::shared_ptr<VulkanDevice> vk_device_;

    VkImage color_image_{VK_NULL_HANDLE};
    VkDeviceMemory color_memory_{VK_NULL_HANDLE};
    VkImageView color_view_{VK_NULL_HANDLE};

    VkImage msaa_color_image_{VK_NULL_HANDLE};
    VkDeviceMemory msaa_color_memory_{VK_NULL_HANDLE};
    VkImageView msaa_color_view_{VK_NULL_HANDLE};

    VkImage depth_stencil_image_{VK_NULL_HANDLE};
    VkDeviceMemory depth_stencil_memory_{VK_NULL_HANDLE};
    VkImageView depth_stencil_view_{VK_NULL_HANDLE};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkRenderPass render_pass_load_{VK_NULL_HANDLE};
    VkFramebuffer framebuffer_{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer_{VK_NULL_HANDLE};

    bool in_render_pass_{false};

    bool init_surface();
    void cleanup();
};

} // namespace nisaba::gpu
