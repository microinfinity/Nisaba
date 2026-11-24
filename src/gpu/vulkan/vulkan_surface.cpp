#include "nisaba/gpu/vulkan_surface.hpp"
#include "nisaba/gpu/vulkan_renderer.hpp"
#include <iostream>
#include <cstring>
#include <array>

namespace nisaba::gpu {

VulkanSurface::VulkanSurface(
    std::shared_ptr<VulkanDevice> device,
    uint32_t width,
    uint32_t height,
    uint32_t samples
) : GpuSurface(device, 0, 0, 0, samples, width, height, nullptr),
    vk_device_(std::move(device)) {}

VulkanSurface::~VulkanSurface() {
    cleanup();
}

void VulkanSurface::cleanup() {
    if (!vk_device_ || vk_device_->device() == VK_NULL_HANDLE) return;
    VkDevice dev = vk_device_->device();

    vkDeviceWaitIdle(dev);

    if (command_buffer_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(dev, vk_device_->command_pool(), 1, &command_buffer_);
        command_buffer_ = VK_NULL_HANDLE;
    }
    if (framebuffer_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(dev, framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (render_pass_load_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, render_pass_load_, nullptr);
        render_pass_load_ = VK_NULL_HANDLE;
    }
    if (render_pass_dont_care_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, render_pass_dont_care_, nullptr);
        render_pass_dont_care_ = VK_NULL_HANDLE;
    }

    if (depth_stencil_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, depth_stencil_view_, nullptr);
        depth_stencil_view_ = VK_NULL_HANDLE;
    }
    if (depth_stencil_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(dev, depth_stencil_image_, nullptr);
        depth_stencil_image_ = VK_NULL_HANDLE;
    }
    if (depth_stencil_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, depth_stencil_memory_, nullptr);
        depth_stencil_memory_ = VK_NULL_HANDLE;
    }

    if (msaa_color_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, msaa_color_view_, nullptr);
        msaa_color_view_ = VK_NULL_HANDLE;
    }
    if (msaa_color_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(dev, msaa_color_image_, nullptr);
        msaa_color_image_ = VK_NULL_HANDLE;
    }
    if (msaa_color_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, msaa_color_memory_, nullptr);
        msaa_color_memory_ = VK_NULL_HANDLE;
    }

    if (color_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, color_view_, nullptr);
        color_view_ = VK_NULL_HANDLE;
    }
    if (color_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(dev, color_image_, nullptr);
        color_image_ = VK_NULL_HANDLE;
    }
    if (color_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, color_memory_, nullptr);
        color_memory_ = VK_NULL_HANDLE;
    }
}

bool VulkanSurface::init_surface() {
    if (!vk_device_ || !vk_device_->is_valid()) return false;
    VkDevice dev = vk_device_->device();

    VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT;
    if (samples_ >= 4) sample_count = VK_SAMPLE_COUNT_4_BIT;
    else if (samples_ >= 2) sample_count = VK_SAMPLE_COUNT_2_BIT;

    // 1. Create Resolve/Color Image (1 sample)
    VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!vk_device_->create_image(
            width_, height_, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
            color_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SAMPLE_COUNT_1_BIT,
            color_image_, color_memory_)) {
        std::cerr << "[Nisaba Vulkan] Failed to create color image\n";
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = color_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev, &view_info, nullptr, &color_view_) != VK_SUCCESS) {
        return false;
    }

    // 2. Create MSAA Color Image if samples > 1
    if (sample_count != VK_SAMPLE_COUNT_1_BIT) {
        VkImageUsageFlags msaa_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!vk_device_->create_image(
                width_, height_, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                msaa_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sample_count,
                msaa_color_image_, msaa_color_memory_)) {
            return false;
        }

        VkImageViewCreateInfo msaa_view_info = view_info;
        msaa_view_info.image = msaa_color_image_;
        if (vkCreateImageView(dev, &msaa_view_info, nullptr, &msaa_color_view_) != VK_SUCCESS) {
            return false;
        }
    }

    // 3. Create Depth/Stencil Image
    VkFormat ds_format = VK_FORMAT_D24_UNORM_S8_UINT;
    // Verify format support
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(vk_device_->physical_device(), ds_format, &format_props);
    if (!(format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        ds_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    if (!vk_device_->create_image(
            width_, height_, ds_format, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            sample_count, depth_stencil_image_, depth_stencil_memory_)) {
        return false;
    }

    VkImageViewCreateInfo ds_view_info{};
    ds_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ds_view_info.image = depth_stencil_image_;
    ds_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ds_view_info.format = ds_format;
    ds_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    ds_view_info.subresourceRange.baseMipLevel = 0;
    ds_view_info.subresourceRange.levelCount = 1;
    ds_view_info.subresourceRange.baseArrayLayer = 0;
    ds_view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev, &ds_view_info, nullptr, &depth_stencil_view_) != VK_SUCCESS) {
        return false;
    }

    // 4. Create Render Pass
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkImageView> fb_attachments;

    VkAttachmentReference color_ref{};
    VkAttachmentReference depth_ref{};
    VkAttachmentReference resolve_ref{};

    if (sample_count != VK_SAMPLE_COUNT_1_BIT) {
        // Attachment 0: MSAA Color
        VkAttachmentDescription msaa_desc{};
        msaa_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
        msaa_desc.samples = sample_count;
        msaa_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        msaa_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        msaa_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        msaa_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        msaa_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        msaa_desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(msaa_desc);
        fb_attachments.push_back(msaa_color_view_);

        // Attachment 1: Depth/Stencil
        VkAttachmentDescription ds_desc{};
        ds_desc.format = ds_format;
        ds_desc.samples = sample_count;
        ds_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ds_desc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ds_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ds_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        ds_desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ds_desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(ds_desc);
        fb_attachments.push_back(depth_stencil_view_);

        // Attachment 2: Resolve Color
        VkAttachmentDescription resolve_desc{};
        resolve_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
        resolve_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve_desc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve_desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve_desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(resolve_desc);
        fb_attachments.push_back(color_view_);

        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        depth_ref.attachment = 1;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        resolve_ref.attachment = 2;
        resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        // Attachment 0: Color (1 sample)
        VkAttachmentDescription color_desc{};
        color_desc.format = VK_FORMAT_R8G8B8A8_UNORM;
        color_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        color_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(color_desc);
        fb_attachments.push_back(color_view_);

        // Attachment 1: Depth/Stencil
        VkAttachmentDescription ds_desc{};
        ds_desc.format = ds_format;
        ds_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        ds_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ds_desc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ds_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ds_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        ds_desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ds_desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(ds_desc);
        fb_attachments.push_back(depth_stencil_view_);

        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        depth_ref.attachment = 1;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    if (sample_count != VK_SAMPLE_COUNT_1_BIT) {
        subpass.pResolveAttachments = &resolve_ref;
    }

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    rp_info.pAttachments = attachments.data();
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    if (vkCreateRenderPass(dev, &rp_info, nullptr, &render_pass_) != VK_SUCCESS) {
        return false;
    }

    // Create Load/Preserve Render Pass (for incremental Damage Tracking & Layer Resumption)
    std::vector<VkAttachmentDescription> attachments_load = attachments;
    if (sample_count != VK_SAMPLE_COUNT_1_BIT) {
        attachments_load[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments_load[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments_load[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments_load[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments_load[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments_load[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        attachments_load[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments_load[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments_load[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments_load[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkRenderPassCreateInfo rp_info_load = rp_info;
    rp_info_load.pAttachments = attachments_load.data();
    if (vkCreateRenderPass(dev, &rp_info_load, nullptr, &render_pass_load_) != VK_SUCCESS) {
        render_pass_load_ = VK_NULL_HANDLE;
    }

    // Create Discard / DONT_CARE Render Pass for tile-based GPU framebuffer discard hints.
    // Color/depth loadOp is DONT_CARE to skip tile memory readback; stencil still clears to 0
    // so stencil-and-cover vector paths operate with a clean stencil plane.
    std::vector<VkAttachmentDescription> attachments_dc = attachments;
    if (sample_count != VK_SAMPLE_COUNT_1_BIT) {
        attachments_dc[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments_dc[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments_dc[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments_dc[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    } else {
        attachments_dc[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments_dc[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments_dc[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    }

    VkRenderPassCreateInfo rp_info_dc = rp_info;
    rp_info_dc.pAttachments = attachments_dc.data();
    if (vkCreateRenderPass(dev, &rp_info_dc, nullptr, &render_pass_dont_care_) != VK_SUCCESS) {
        render_pass_dont_care_ = VK_NULL_HANDLE;
    }

    // 5. Create Framebuffer
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass_;
    fb_info.attachmentCount = static_cast<uint32_t>(fb_attachments.size());
    fb_info.pAttachments = fb_attachments.data();
    fb_info.width = width_;
    fb_info.height = height_;
    fb_info.layers = 1;

    if (vkCreateFramebuffer(dev, &fb_info, nullptr, &framebuffer_) != VK_SUCCESS) {
        return false;
    }

    // 6. Transition color image initially to COLOR_ATTACHMENT_OPTIMAL
    {
        VkCommandBuffer cmd = vk_device_->begin_single_time_commands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = color_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (sample_count != VK_SAMPLE_COUNT_1_BIT) {
            barrier.image = msaa_color_image_;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        VkImageMemoryBarrier ds_barrier{};
        ds_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ds_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ds_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ds_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ds_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ds_barrier.image = depth_stencil_image_;
        ds_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        ds_barrier.subresourceRange.baseMipLevel = 0;
        ds_barrier.subresourceRange.levelCount = 1;
        ds_barrier.subresourceRange.baseArrayLayer = 0;
        ds_barrier.subresourceRange.layerCount = 1;
        ds_barrier.srcAccessMask = 0;
        ds_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &ds_barrier);

        vk_device_->end_single_time_commands(cmd);
    }

    // 7. Allocate command buffer
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = vk_device_->command_pool();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(dev, &alloc_info, &command_buffer_) != VK_SUCCESS) {
        return false;
    }

    return true;
}

std::shared_ptr<VulkanSurface> VulkanSurface::create(
    std::shared_ptr<VulkanDevice> device,
    uint32_t width,
    uint32_t height,
    uint32_t samples
) {
    if (!device || width == 0 || height == 0) return nullptr;

    auto surf = std::make_shared<VulkanSurface>(device, width, height, samples);
    if (!surf->init_surface()) {
        return nullptr;
    }
    return surf;
}

void VulkanSurface::begin_render_pass(nisaba::Color clear_color, bool load_previous, bool dont_care) {
    if (in_render_pass_) return;

    VkRenderPass pass_to_use = render_pass_;
    if (dont_care && render_pass_dont_care_ != VK_NULL_HANDLE) {
        pass_to_use = render_pass_dont_care_;
    } else if ((load_previous || preserve_contents_) && render_pass_load_ != VK_NULL_HANDLE) {
        pass_to_use = render_pass_load_;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer_, &begin_info);

    std::array<VkClearValue, 3> clear_values{};
    clear_values[0].color = {{clear_color.red(), clear_color.green(), clear_color.blue(), clear_color.alpha()}};
    clear_values[1].depthStencil = {1.0f, 0};
    clear_values[2].color = {{clear_color.red(), clear_color.green(), clear_color.blue(), clear_color.alpha()}};

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = pass_to_use;
    rp_begin.framebuffer = framebuffer_;
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {width_, height_};
    rp_begin.clearValueCount = static_cast<uint32_t>(clear_values.size());
    rp_begin.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    in_render_pass_ = true;
}

void VulkanSurface::end_render_pass() {
    if (!in_render_pass_) return;

    vkCmdEndRenderPass(command_buffer_);
    vkEndCommandBuffer(command_buffer_);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;

    vkQueueSubmit(vk_device_->graphics_queue(), 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_device_->graphics_queue());

    in_render_pass_ = false;
}

void VulkanSurface::bind() {
    if (!in_render_pass_) {
        if (discarded_) {
            discarded_ = false;
            begin_render_pass(nisaba::Color::TRANSPARENT, false, true);
        } else {
            begin_render_pass(nisaba::Color::TRANSPARENT, preserve_contents_, false);
        }
    }
}

void VulkanSurface::resolve() {
    if (in_render_pass_) {
        end_render_pass();
    }
}

void VulkanSurface::discard() {
    if (in_render_pass_) {
        if (command_buffer_ != VK_NULL_HANDLE) {
            vkResetCommandBuffer(command_buffer_, 0);
        }
        in_render_pass_ = false;
    }
    discarded_ = true;
}

void VulkanSurface::clear(nisaba::Color color) {
    discarded_ = false;
    if (!in_render_pass_) {
        begin_render_pass(color, false, false);
        return;
    }

    std::array<VkClearAttachment, 2> clear_atts{};
    clear_atts[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_atts[0].colorAttachment = 0;
    clear_atts[0].clearValue.color = {{color.red(), color.green(), color.blue(), color.alpha()}};

    clear_atts[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    clear_atts[1].clearValue.depthStencil = {1.0f, 0};

    VkClearRect clear_rect{};
    clear_rect.rect.offset = {0, 0};
    clear_rect.rect.extent = {width_, height_};
    clear_rect.baseArrayLayer = 0;
    clear_rect.layerCount = 1;

    vkCmdClearAttachments(command_buffer_, 2, clear_atts.data(), 1, &clear_rect);
}

void VulkanSurface::read_pixels(void* dst_rgba) {
    if (!dst_rgba || !vk_device_) return;
    resolve();

    VkDeviceSize image_bytes = static_cast<VkDeviceSize>(width_) * height_ * 4;

    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;

    if (!vk_device_->create_buffer(
            image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging_buf, staging_mem)) {
        return;
    }

    VkCommandBuffer cmd = vk_device_->begin_single_time_commands();

    // Transition color_image_ to TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy_region{};
    copy_region.bufferOffset = 0;
    copy_region.bufferRowLength = 0;
    copy_region.bufferImageHeight = 0;
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.mipLevel = 0;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageOffset = {0, 0, 0};
    copy_region.imageExtent = {width_, height_, 1};

    vkCmdCopyImageToBuffer(cmd, color_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_buf, 1, &copy_region);

    // Transition back to COLOR_ATTACHMENT_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_device_->end_single_time_commands(cmd);

    // Map and copy
    void* mapped = nullptr;
    vkMapMemory(vk_device_->device(), staging_mem, 0, image_bytes, 0, &mapped);
    if (mapped) {
        std::memcpy(dst_rgba, mapped, image_bytes);
        vkUnmapMemory(vk_device_->device(), staging_mem);
    }

    vkDestroyBuffer(vk_device_->device(), staging_buf, nullptr);
    vkFreeMemory(vk_device_->device(), staging_mem, nullptr);
}

void VulkanSurface::read_pixels(PixmapMut& dst) {
    if (dst.data_mut()) {
        read_pixels(dst.data_mut());
    }
}

std::optional<Pixmap> VulkanSurface::to_pixmap() {
    auto p = Pixmap::allocate(width_, height_);
    if (p) {
        read_pixels(p->data_mut());
    }
    return p;
}

std::unique_ptr<Context> VulkanSurface::create_context(int flags) {
    VkSampleCountFlagBits sc = (samples_ >= 4) ? VK_SAMPLE_COUNT_4_BIT :
                               ((samples_ >= 2) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT);

    auto renderer = std::make_unique<VulkanRenderer>(
        vk_device_->device(),
        vk_device_->physical_device(),
        vk_device_->graphics_queue(),
        vk_device_->graphics_queue_family(),
        render_pass_,
        sc,
        flags
    );
    if (!renderer->init()) return nullptr;
    renderer->setTargetSurface(this);
    return std::make_unique<Context>(std::move(renderer), flags);
}

} // namespace nisaba::gpu
