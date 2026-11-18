#include "nisaba/gpu/vulkan_renderer.hpp"
#include "nisaba/gpu/vulkan_surface.hpp"
#include "../shader_uniforms.hpp"
#include "vulkan_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace nisaba::gpu {

namespace {

enum class CallType : uint8_t {
    None = 0,
    Fill,
    ConvexFill,
    Stroke,
    Triangles,
    Clip,
    RestoreClip
};

enum class VkTopologyKind : uint8_t {
    Fan   = 0,
    Strip = 1,
    List  = 2
};

enum class VkStencilMode : uint8_t {
    NoStencil = 0,      // Stencil disabled, RGBA color writes enabled
    ClipTest,           // EQUAL (clipDepth<<4) mask 0xf0, write 0x00, RGBA writes enabled
    FillStencilOdd,     // EQUAL (clipDepth<<4) mask 0xf0, write 0x0f, INVERT, colorMask = 0
    FillStencilNonZero, // EQUAL (clipDepth<<4) mask 0xf0, write 0x0f, INCR/DECR_WRAP, colorMask = 0
    FillAntiAlias,      // EQUAL (clipDepth<<4) mask 0xff, write 0x00, KEEP, RGBA writes enabled
    FillCover,          // NOTEQUAL 0 mask 0x0f, write 0x0f, ZERO, RGBA writes enabled
    ClipCover,          // NOTEQUAL (newDepth<<4) mask 0x0f, write 0xff, REPLACE, colorMask = 0
    RestoreClip         // LESS (targetDepth<<4) mask 0xf0, write 0xff, REPLACE, colorMask = 0
};

struct VkBlendState {
    VkBlendFactor srcRGB{VK_BLEND_FACTOR_ONE};
    VkBlendFactor dstRGB{VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    VkBlendFactor srcAlpha{VK_BLEND_FACTOR_ONE};
    VkBlendFactor dstAlpha{VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    VkBlendOp colorOp{VK_BLEND_OP_ADD};
    VkBlendOp alphaOp{VK_BLEND_OP_ADD};

    bool operator==(const VkBlendState& o) const noexcept {
        return srcRGB == o.srcRGB && dstRGB == o.dstRGB &&
               srcAlpha == o.srcAlpha && dstAlpha == o.dstAlpha &&
               colorOp == o.colorOp && alphaOp == o.alphaOp;
    }
};

VkBlendFactor convertVkBlendFactor(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::SrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendState convertVkBlend(const CompositeOperationState& op) {
    if (!op.isProgrammableBlend) {
        return {
            convertVkBlendFactor(op.srcRGB),
            convertVkBlendFactor(op.dstRGB),
            convertVkBlendFactor(op.srcAlpha),
            convertVkBlendFactor(op.dstAlpha),
            VK_BLEND_OP_ADD,
            VK_BLEND_OP_ADD
        };
    }

    switch (op.blendMode) {
        case BlendMode::Clear:
            return {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::Source:
            return {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::SourceIn:
            return {VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::DestinationIn:
            return {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::SourceOut:
            return {VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::DestinationAtop:
            return {VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::Modulate:
            return {VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        case BlendMode::Multiply:
            return {VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                    VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
        default:
            return {};
    }
}

uint64_t packPipelineKey(VkTopologyKind topo, VkStencilMode stencil, const VkBlendState& b) {
    uint64_t key = 0;
    key |= (static_cast<uint64_t>(topo) & 0x3ULL);
    key |= (static_cast<uint64_t>(stencil) & 0xFULL) << 2;
    key |= (static_cast<uint64_t>(b.srcRGB) & 0x1FULL) << 6;
    key |= (static_cast<uint64_t>(b.dstRGB) & 0x1FULL) << 11;
    key |= (static_cast<uint64_t>(b.srcAlpha) & 0x1FULL) << 16;
    key |= (static_cast<uint64_t>(b.dstAlpha) & 0x1FULL) << 21;
    key |= (static_cast<uint64_t>(b.colorOp) & 0xFULL) << 26;
    key |= (static_cast<uint64_t>(b.alphaOp) & 0xFULL) << 30;
    return key;
}

size_t bytesPerPixel(TextureType type) {
    return (type == TextureType::RGBA || type == TextureType::BGRA) ? 4 : 1;
}

VkFormat textureVkFormat(TextureType type) {
    switch (type) {
        case TextureType::RGBA:  return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureType::BGRA:  return VK_FORMAT_B8G8R8A8_UNORM;
        case TextureType::Alpha: return VK_FORMAT_R8_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VkSamplerAddressMode wrapModeFromFlags(int flags, int mirrorFlag, int repeatFlag) {
    if (flags & mirrorFlag) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    if (flags & repeatFlag) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

void appendTriFanVertices(Vertex* dst, const Vertex* fill, int count, uint32_t c) {
    const Vertex& v0 = fill[0];
    for (int k = 0; k < count - 2; ++k) {
        dst[0] = v0;          dst[0].color = c;
        dst[1] = fill[k + 1]; dst[1].color = c;
        dst[2] = fill[k + 2]; dst[2].color = c;
        dst += 3;
    }
}

void appendTriStripVertices(Vertex* dst, const Vertex* strip, int count, uint32_t c) {
    for (int k = 0; k < count - 2; ++k) {
        if ((k & 1) == 0) {
            dst[0] = strip[k];     dst[0].color = c;
            dst[1] = strip[k + 1]; dst[1].color = c;
            dst[2] = strip[k + 2]; dst[2].color = c;
        } else {
            dst[0] = strip[k + 1]; dst[0].color = c;
            dst[1] = strip[k];     dst[1].color = c;
            dst[2] = strip[k + 2]; dst[2].color = c;
        }
        dst += 3;
    }
}

struct Call {
    CallType type{CallType::None};
    int image{0};
    int pathOffset{0};
    int pathCount{0};
    int triangleOffset{0};
    int triangleCount{0};
    int uniformOffset{0};
    int clipDepth{0};
    bool evenOdd{false};
    Winding winding{Winding::CounterClockwise};
    VkBlendState blend{};
    int scissorRect[4]{-1, -1, -1, -1};
};

struct VkPath {
    int fillOffset{0};
    int fillCount{0};
    int strokeOffset{0};
    int strokeCount{0};
};

struct VulkanTexture {
    int id{0};
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    int width{0};
    int height{0};
    TextureType type{TextureType::RGBA};
    int flags{0};
    bool ownsImage{true};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

} // namespace

struct VulkanRenderer::Impl {
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    uint32_t queueFamilyIndex{0};
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    int flags{0};
    float view[2]{0.0f, 0.0f};

    VkShaderModule vertShaderModule{VK_NULL_HANDLE};
    VkShaderModule fragShaderModule{VK_NULL_HANDLE};

    VkDescriptorSetLayout descSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorPool descPool{VK_NULL_HANDLE};

    std::unordered_map<uint64_t, VkPipeline> pipelineCache;

    // Vertex & Uniform Memory Buffers
    VkBuffer vertBuf{VK_NULL_HANDLE};
    VkDeviceMemory vertMem{VK_NULL_HANDLE};
    size_t vertBufCap{0};

    VkBuffer fragBuf{VK_NULL_HANDLE};
    VkDeviceMemory fragMem{VK_NULL_HANDLE};
    size_t fragBufCap{0};
    size_t fragAlign{256};
    size_t fragSize{656};

    // Textures
    std::vector<VulkanTexture> textures;
    int nextTextureId{1};
    int dummyTex{0};

    // Batch recording state
    std::vector<Vertex> verts;
    std::vector<VkPath> paths;
    std::vector<Call> calls;
    std::vector<uint8_t> uniforms;
    int lastUniformOffset{-1};
    int currentClipDepth{0};
    int flushedClipDepth{0};

    VulkanSurface* targetSurface{nullptr};
    VkCommandBuffer externalCmd{VK_NULL_HANDLE};

    void clearBatchBuffers() {
        verts.clear();
        paths.clear();
        calls.clear();
        uniforms.clear();
        lastUniformOffset = -1;
    }

    VulkanTexture* allocTexture() {
        for (auto& t : textures) {
            if (t.id == 0) {
                VkDescriptorSet recycledSet = t.descriptorSet;
                t = VulkanTexture{};
                t.id = nextTextureId++;
                t.descriptorSet = recycledSet;
                return &t;
            }
        }
        VulkanTexture tex;
        tex.id = nextTextureId++;
        textures.push_back(tex);
        return &textures.back();
    }

    VulkanTexture* findTexture(int id) {
        for (auto& t : textures) {
            if (t.id == id) return &t;
        }
        return nullptr;
    }

    std::optional<TextureUniformInfo> lookupTextureInfo(int id) {
        VulkanTexture* tex = findTexture(id);
        if (!tex) return std::nullopt;
        return TextureUniformInfo{tex->type, tex->flags};
    }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return 0;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      VkBuffer& outBuf, VkDeviceMemory& outMem) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufInfo, nullptr, &outBuf) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, outBuf, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &outMem) != VK_SUCCESS) {
            vkDestroyBuffer(device, outBuf, nullptr);
            outBuf = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device, outBuf, outMem, 0);
        return true;
    }

    int allocFragUniforms(int n) {
        const int offset = static_cast<int>(uniforms.size());
        uniforms.resize(uniforms.size() + static_cast<size_t>(n) * fragSize, 0);
        return offset;
    }

    FragUniforms* fragUniformPtr(int offset) {
        return reinterpret_cast<FragUniforms*>(&uniforms[offset]);
    }

    int storeUniformOrReuse(const FragUniforms& tempFrag) {
        if (lastUniformOffset >= 0 && !uniforms.empty() &&
            std::memcmp(fragUniformPtr(lastUniformOffset), &tempFrag, sizeof(FragUniforms)) == 0) {
            return lastUniformOffset;
        }
        const int off = allocFragUniforms(1);
        *fragUniformPtr(off) = tempFrag;
        lastUniformOffset = off;
        return off;
    }

    bool getHardwareScissor(const Scissor& scissor, int outRect[4]) const {
        if (scissor.extent[0] < -0.5f || scissor.extent[1] < -0.5f) {
            outRect[0] = -1; outRect[1] = -1; outRect[2] = -1; outRect[3] = -1;
            return false;
        }
        if (std::abs(scissor.xform[1]) > 1e-4f || std::abs(scissor.xform[2]) > 1e-4f) {
            outRect[0] = -1;
            return false;
        }
        const float sx = scissor.xform[0];
        const float sy = scissor.xform[3];
        if (sx <= 0.0f || sy <= 0.0f) {
            outRect[0] = -1;
            return false;
        }
        const float cx = scissor.xform[4];
        const float cy = scissor.xform[5];
        const float hw = scissor.extent[0] * sx;
        const float hh = scissor.extent[1] * sy;

        const int vpW = std::max(0, static_cast<int>(std::ceil(view[0])));
        const int vpH = std::max(0, static_cast<int>(std::ceil(view[1])));
        const int x0 = std::clamp(static_cast<int>(std::floor(cx - hw)), 0, vpW);
        const int y0 = std::clamp(static_cast<int>(std::floor(cy - hh)), 0, vpH);
        const int x1 = std::clamp(static_cast<int>(std::ceil(cx + hw)), 0, vpW);
        const int y1 = std::clamp(static_cast<int>(std::ceil(cy + hh)), 0, vpH);

        outRect[0] = x0;
        outRect[1] = y0;
        outRect[2] = std::max(0, x1 - x0);
        outRect[3] = std::max(0, y1 - y0);
        return true;
    }

    bool convertPaint(FragUniforms* frag, const Paint& paint, const Scissor& scissor,
                      float width, float fringe, float strokeThr, bool hasHwScissor = false) {
        return convertPaintUniforms(frag, paint, scissor, width, fringe, strokeThr, hasHwScissor,
                                    0, [this](int id) { return lookupTextureInfo(id); });
    }

    bool createTextureSampler(int imageFlags, VkSampler& outSampler) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        const bool nearest = (imageFlags & (ImageFlags::ImageNearest | ImageFlags::ImageForceNearest)) != 0;
        samplerInfo.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.minFilter = samplerInfo.magFilter;
        samplerInfo.addressModeU = wrapModeFromFlags(imageFlags, ImageFlags::ImageMirrorX, ImageFlags::ImageRepeatX);
        samplerInfo.addressModeV = wrapModeFromFlags(imageFlags, ImageFlags::ImageMirrorY, ImageFlags::ImageRepeatY);
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        return vkCreateSampler(device, &samplerInfo, nullptr, &outSampler) == VK_SUCCESS;
    }

    void writeTextureDescriptorSet(VulkanTexture& tex) {
        if (tex.descriptorSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo dsAlloc{};
            dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsAlloc.descriptorPool = descPool;
            dsAlloc.descriptorSetCount = 1;
            dsAlloc.pSetLayouts = &descSetLayout;
            if (vkAllocateDescriptorSets(device, &dsAlloc, &tex.descriptorSet) != VK_SUCCESS) {
                tex.descriptorSet = VK_NULL_HANDLE;
                return;
            }
        }

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = fragBuf;
        dbi.offset = 0;
        dbi.range = sizeof(FragUniforms);

        VkDescriptorImageInfo dii{};
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dii.imageView = tex.view;
        dii.sampler = tex.sampler;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = tex.descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &dbi;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = tex.descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &dii;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    void refreshUniformDescriptorSets() {
        if (fragBuf == VK_NULL_HANDLE) return;
        VkDescriptorBufferInfo dbi{};
        dbi.buffer = fragBuf;
        dbi.offset = 0;
        dbi.range = sizeof(FragUniforms);

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(textures.size());
        for (const auto& t : textures) {
            if (t.id == 0 || t.descriptorSet == VK_NULL_HANDLE) continue;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = t.descriptorSet;
            w.dstBinding = 0;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w.descriptorCount = 1;
            w.pBufferInfo = &dbi;
            writes.push_back(w);
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    bool uploadImageRegion(VulkanTexture& tex, int x, int y, int width, int height,
                           int srcStrideBytes, const unsigned char* data) {
        if (!data || width <= 0 || height <= 0) return false;

        const size_t bpp = bytesPerPixel(tex.type);
        const size_t rowBytes = static_cast<size_t>(width) * bpp;
        const size_t effectiveStride = (srcStrideBytes > 0)
            ? static_cast<size_t>(srcStrideBytes)
            : static_cast<size_t>(tex.width) * bpp;
        if (effectiveStride < rowBytes) return false;

        const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(rowBytes) * height;
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        if (!createBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stagingBuf, stagingMem)) {
            return false;
        }

        void* mapped = nullptr;
        vkMapMemory(device, stagingMem, 0, uploadSize, 0, &mapped);
        if (effectiveStride == rowBytes && x == 0 && y == 0 && width == tex.width) {
            std::memcpy(mapped, data, uploadSize);
        } else {
            const uint8_t* srcPtr = data + static_cast<size_t>(y) * effectiveStride + static_cast<size_t>(x) * bpp;
            uint8_t* dstPtr = static_cast<uint8_t*>(mapped);
            for (int r = 0; r < height; ++r) {
                std::memcpy(dstPtr + r * rowBytes, srcPtr + r * effectiveStride, rowBytes);
            }
        }
        vkUnmapMemory(device, stagingMem);

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(device, &poolInfo, nullptr, &pool);

        VkCommandBufferAllocateInfo cbAlloc{};
        cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool = pool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &cbAlloc, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = tex.layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = (tex.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        const VkPipelineStageFlags srcStage = (tex.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {x, y, 0};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

        vkCmdCopyBufferToImage(cmd, stagingBuf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        vkFreeCommandBuffers(device, pool, 1, &cmd);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyBuffer(device, stagingBuf, nullptr);
        vkFreeMemory(device, stagingMem, nullptr);
        return true;
    }

    int createTextureInternal(TextureType type, int width, int height, int strideBytes,
                              int imageFlags, const unsigned char* data) {
        if (width <= 0 || height <= 0) return 0;
        VulkanTexture* tex = allocTexture();
        if (!tex) return 0;

        tex->type = type;
        tex->width = width;
        tex->height = height;
        tex->flags = imageFlags;
        tex->ownsImage = true;
        tex->layout = VK_IMAGE_LAYOUT_UNDEFINED;

        const VkFormat format = textureVkFormat(type);

        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.format = format;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imgInfo, nullptr, &tex->image) != VK_SUCCESS) {
            tex->id = 0;
            return 0;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, tex->image, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &tex->memory) != VK_SUCCESS) {
            vkDestroyImage(device, tex->image, nullptr);
            tex->image = VK_NULL_HANDLE;
            tex->id = 0;
            return 0;
        }
        vkBindImageMemory(device, tex->image, tex->memory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = tex->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &tex->view) != VK_SUCCESS) {
            vkDestroyImage(device, tex->image, nullptr);
            vkFreeMemory(device, tex->memory, nullptr);
            tex->image = VK_NULL_HANDLE;
            tex->memory = VK_NULL_HANDLE;
            tex->id = 0;
            return 0;
        }

        if (!createTextureSampler(imageFlags, tex->sampler)) {
            vkDestroyImageView(device, tex->view, nullptr);
            vkDestroyImage(device, tex->image, nullptr);
            vkFreeMemory(device, tex->memory, nullptr);
            tex->view = VK_NULL_HANDLE;
            tex->image = VK_NULL_HANDLE;
            tex->memory = VK_NULL_HANDLE;
            tex->id = 0;
            return 0;
        }

        writeTextureDescriptorSet(*tex);

        if (data != nullptr) {
            uploadImageRegion(*tex, 0, 0, width, height, strideBytes, data);
        }

        return tex->id;
    }

    VkPipeline getOrCreatePipeline(VkTopologyKind topo, VkStencilMode stencilMode, const VkBlendState& blend) {
        if (renderPass == VK_NULL_HANDLE && targetSurface != nullptr) {
            renderPass = targetSurface->render_pass();
        }
        if (renderPass == VK_NULL_HANDLE || pipelineLayout == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        const uint64_t key = packPipelineKey(topo, stencilMode, blend);
        if (auto it = pipelineCache.find(key); it != pipelineCache.end()) {
            return it->second;
        }

        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertShaderModule;
        shaderStages[0].pName = "main";

        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragShaderModule;
        shaderStages[1].pName = "main";

        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = 0;
        bindingDesc.stride = sizeof(Vertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 3> attribDescs{};
        attribDescs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attribDescs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 8};
        attribDescs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16};

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribDescs.size());
        vertexInputInfo.pVertexAttributeDescriptions = attribDescs.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        switch (topo) {
            case VkTopologyKind::Fan:   inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
            case VkTopologyKind::Strip: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            case VkTopologyKind::List:  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = samples;

        const bool colorWriteDisabled =
            (stencilMode == VkStencilMode::FillStencilOdd ||
             stencilMode == VkStencilMode::FillStencilNonZero ||
             stencilMode == VkStencilMode::ClipCover ||
             stencilMode == VkStencilMode::RestoreClip);

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = colorWriteDisabled
            ? 0
            : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
        colorBlendAttachment.blendEnable = colorWriteDisabled ? VK_FALSE : VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = blend.srcRGB;
        colorBlendAttachment.dstColorBlendFactor = blend.dstRGB;
        colorBlendAttachment.colorBlendOp = blend.colorOp;
        colorBlendAttachment.srcAlphaBlendFactor = blend.srcAlpha;
        colorBlendAttachment.dstAlphaBlendFactor = blend.dstAlpha;
        colorBlendAttachment.alphaBlendOp = blend.alphaOp;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depthStencil.stencilTestEnable = (stencilMode != VkStencilMode::NoStencil) ? VK_TRUE : VK_FALSE;

        auto makeStencilOp = [](VkStencilOp passOp, VkCompareOp cmpOp, uint32_t cmpMask, uint32_t writeMask) {
            VkStencilOpState s{};
            s.failOp = VK_STENCIL_OP_KEEP;
            s.depthFailOp = VK_STENCIL_OP_KEEP;
            s.passOp = passOp;
            s.compareOp = cmpOp;
            s.compareMask = cmpMask;
            s.writeMask = writeMask;
            s.reference = 0;
            return s;
        };

        switch (stencilMode) {
            case VkStencilMode::NoStencil:
                break;
            case VkStencilMode::ClipTest:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xf0u, 0x00u);
                depthStencil.back = depthStencil.front;
                break;
            case VkStencilMode::FillStencilOdd:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_INVERT, VK_COMPARE_OP_EQUAL, 0xf0u, 0x0fu);
                depthStencil.back = depthStencil.front;
                break;
            case VkStencilMode::FillStencilNonZero:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_COMPARE_OP_EQUAL, 0xf0u, 0x0fu);
                depthStencil.back  = makeStencilOp(VK_STENCIL_OP_DECREMENT_AND_WRAP, VK_COMPARE_OP_EQUAL, 0xf0u, 0x0fu);
                break;
            case VkStencilMode::FillAntiAlias:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0x00u);
                depthStencil.back = depthStencil.front;
                break;
            case VkStencilMode::FillCover:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_ZERO, VK_COMPARE_OP_NOT_EQUAL, 0x0fu, 0x0fu);
                depthStencil.back = depthStencil.front;
                break;
            case VkStencilMode::ClipCover:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_NOT_EQUAL, 0x0fu, 0xffu);
                depthStencil.back = depthStencil.front;
                break;
            case VkStencilMode::RestoreClip:
                depthStencil.front = makeStencilOp(VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_LESS, 0xf0u, 0xffu);
                depthStencil.back = depthStencil.front;
                break;
        }

        std::array<VkDynamicState, 3> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = (stencilMode == VkStencilMode::NoStencil) ? 2u : 3u;
        dynamicState.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        pipelineCache.emplace(key, pipeline);
        return pipeline;
    }

    void prewarmDefaultPipelines() {
        const VkBlendState defaultBlend{};
        getOrCreatePipeline(VkTopologyKind::Fan, VkStencilMode::NoStencil, defaultBlend);
        getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::NoStencil, defaultBlend);
        getOrCreatePipeline(VkTopologyKind::List, VkStencilMode::NoStencil, defaultBlend);
    }

    void destroyPipelines() {
        for (auto& [k, pipe] : pipelineCache) {
            if (pipe != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipe, nullptr);
            }
        }
        pipelineCache.clear();
    }
};

VulkanRenderer::VulkanRenderer(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkQueue queue,
    uint32_t queueFamilyIndex,
    VkRenderPass renderPass,
    VkSampleCountFlagBits samples,
    int flags
) : m_impl(std::make_unique<Impl>()) {
    m_impl->device = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->queue = queue;
    m_impl->queueFamilyIndex = queueFamilyIndex;
    m_impl->renderPass = renderPass;
    m_impl->samples = samples;
    m_impl->flags = flags;
}

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::edgeAntiAlias() const {
    return (m_impl->flags & CreateFlags::Antialias) != 0;
}

bool VulkanRenderer::init() {
    if (m_impl->device == VK_NULL_HANDLE) return false;
    VkDevice dev = m_impl->device;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_impl->physicalDevice, &props);
    m_impl->fragAlign = std::max<size_t>(props.limits.minUniformBufferOffsetAlignment, 16);
    m_impl->fragSize = (sizeof(FragUniforms) + m_impl->fragAlign - 1) & ~(m_impl->fragAlign - 1);

    VkShaderModuleCreateInfo vertModInfo{};
    vertModInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertModInfo.codeSize = vulkan::NISABA_VERT_SPV_SIZE;
    vertModInfo.pCode = vulkan::NISABA_VERT_SPV;
    if (vkCreateShaderModule(dev, &vertModInfo, nullptr, &m_impl->vertShaderModule) != VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo fragModInfo{};
    fragModInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragModInfo.codeSize = vulkan::NISABA_FRAG_SPV_SIZE;
    fragModInfo.pCode = vulkan::NISABA_FRAG_SPV;
    if (vkCreateShaderModule(dev, &fragModInfo, nullptr, &m_impl->fragShaderModule) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_impl->descSetLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_impl->descSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &m_impl->pipelineLayout) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[0].descriptorCount = 512;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 512;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 512;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_impl->descPool) != VK_SUCCESS) {
        return false;
    }

    m_impl->vertBufCap = 65536 * sizeof(Vertex);
    m_impl->createBuffer(m_impl->vertBufCap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_impl->vertBuf, m_impl->vertMem);

    m_impl->fragBufCap = 65536;
    m_impl->createBuffer(m_impl->fragBufCap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_impl->fragBuf, m_impl->fragMem);

    const unsigned char whitePixel[4] = {255, 255, 255, 255};
    m_impl->dummyTex = createTexture(TextureType::RGBA, 1, 1, 0, whitePixel);

    if (m_impl->renderPass != VK_NULL_HANDLE) {
        m_impl->prewarmDefaultPipelines();
    }

    return true;
}

void VulkanRenderer::shutdown() {
    if (!m_impl || m_impl->device == VK_NULL_HANDLE) return;
    VkDevice dev = m_impl->device;
    vkDeviceWaitIdle(dev);

    cancel();

    for (auto& t : m_impl->textures) {
        if (t.id != 0) {
            deleteTexture(t.id);
        }
    }
    m_impl->textures.clear();

    m_impl->destroyPipelines();

    if (m_impl->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_impl->pipelineLayout, nullptr);
        m_impl->pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_impl->descSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_impl->descSetLayout, nullptr);
        m_impl->descSetLayout = VK_NULL_HANDLE;
    }
    if (m_impl->descPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_impl->descPool, nullptr);
        m_impl->descPool = VK_NULL_HANDLE;
    }

    if (m_impl->vertBuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, m_impl->vertBuf, nullptr);
        m_impl->vertBuf = VK_NULL_HANDLE;
    }
    if (m_impl->vertMem != VK_NULL_HANDLE) {
        vkFreeMemory(dev, m_impl->vertMem, nullptr);
        m_impl->vertMem = VK_NULL_HANDLE;
    }
    if (m_impl->fragBuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, m_impl->fragBuf, nullptr);
        m_impl->fragBuf = VK_NULL_HANDLE;
    }
    if (m_impl->fragMem != VK_NULL_HANDLE) {
        vkFreeMemory(dev, m_impl->fragMem, nullptr);
        m_impl->fragMem = VK_NULL_HANDLE;
    }

    if (m_impl->vertShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_impl->vertShaderModule, nullptr);
        m_impl->vertShaderModule = VK_NULL_HANDLE;
    }
    if (m_impl->fragShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_impl->fragShaderModule, nullptr);
        m_impl->fragShaderModule = VK_NULL_HANDLE;
    }

    m_impl->device = VK_NULL_HANDLE;
}

void VulkanRenderer::viewport(float width, float height, float devicePixelRatio) {
    (void)devicePixelRatio;
    m_impl->view[0] = width;
    m_impl->view[1] = height;
}

void VulkanRenderer::cancel() {
    m_impl->clearBatchBuffers();
    m_impl->currentClipDepth = 0;
    m_impl->flushedClipDepth = 0;
}

int VulkanRenderer::createTexture(TextureType type, int width, int height, int imageFlags, const unsigned char* data) {
    return m_impl->createTextureInternal(type, width, height, 0, imageFlags, data);
}

int VulkanRenderer::createTextureWithStride(TextureType type, int width, int height, int strideBytes,
                                            int imageFlags, const unsigned char* data) {
    return m_impl->createTextureInternal(type, width, height, strideBytes, imageFlags, data);
}

bool VulkanRenderer::updateTexture(int image, int x, int y, int width, int height, const unsigned char* data) {
    VulkanTexture* tex = m_impl->findTexture(image);
    if (!tex || !data) return false;
    return m_impl->uploadImageRegion(*tex, x, y, width, height, 0, data);
}

bool VulkanRenderer::deleteTexture(int image) {
    VulkanTexture* tex = m_impl->findTexture(image);
    if (!tex) return false;

    VkDevice dev = m_impl->device;
    if (tex->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, tex->sampler, nullptr);
        tex->sampler = VK_NULL_HANDLE;
    }
    if (tex->view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, tex->view, nullptr);
        tex->view = VK_NULL_HANDLE;
    }
    if (tex->ownsImage) {
        if (tex->image != VK_NULL_HANDLE) {
            vkDestroyImage(dev, tex->image, nullptr);
            tex->image = VK_NULL_HANDLE;
        }
        if (tex->memory != VK_NULL_HANDLE) {
            vkFreeMemory(dev, tex->memory, nullptr);
            tex->memory = VK_NULL_HANDLE;
        }
    }
    tex->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex->id = 0;
    return true;
}

bool VulkanRenderer::getTextureSize(int image, int& outWidth, int& outHeight) {
    VulkanTexture* tex = m_impl->findTexture(image);
    if (!tex) return false;
    outWidth = tex->width;
    outHeight = tex->height;
    return true;
}

int VulkanRenderer::textureFlags(int image) const {
    VulkanTexture* tex = m_impl->findTexture(image);
    return tex ? tex->flags : 0;
}

int VulkanRenderer::createTextureFromNativeHandle(uint64_t handle, int w, int h, int imageFlags) {
    if (handle == 0) return 0;
    VulkanTexture* tex = m_impl->allocTexture();
    if (!tex) return 0;

    tex->type = TextureType::RGBA;
    tex->width = w;
    tex->height = h;
    tex->flags = imageFlags;
    tex->ownsImage = false;
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tex->view = reinterpret_cast<VkImageView>(handle);

    if (!m_impl->createTextureSampler(imageFlags, tex->sampler)) {
        tex->view = VK_NULL_HANDLE;
        tex->id = 0;
        return 0;
    }

    m_impl->writeTextureDescriptorSet(*tex);
    return tex->id;
}

void VulkanRenderer::setTargetSurface(VulkanSurface* surface) {
    m_impl->targetSurface = surface;
    if (surface && surface->render_pass() != VK_NULL_HANDLE && m_impl->pipelineLayout != VK_NULL_HANDLE) {
        m_impl->renderPass = surface->render_pass();
        m_impl->prewarmDefaultPipelines();
    }
}

void VulkanRenderer::setCommandBuffer(VkCommandBuffer cmd) {
    m_impl->externalCmd = cmd;
}

void VulkanRenderer::renderFill(const Paint& paint, const CompositeOperationState& compOp,
                                const Scissor& scissor, float fringe, const float* bounds,
                                const RenderPath* paths, int npaths) {
    if (npaths <= 0 || paths == nullptr || m_impl->fragSize == 0) return;

    const VkBlendState blend = convertVkBlend(compOp);
    int hwScissor[4]{-1, -1, -1, -1};
    bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);
    Scissor effectiveScissor = scissor;

    if (hasHwScissor && bounds != nullptr) {
        const float bMinX = bounds[0], bMinY = bounds[1];
        const float bMaxX = bounds[2], bMaxY = bounds[3];
        const float sMinX = static_cast<float>(hwScissor[0]);
        const float sMinY = static_cast<float>(hwScissor[1]);
        const float sMaxX = static_cast<float>(hwScissor[0] + hwScissor[2]);
        const float sMaxY = static_cast<float>(hwScissor[1] + hwScissor[3]);
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

    FragUniforms tempFrag;
    if (!m_impl->convertPaint(&tempFrag, paint, effectiveScissor, fringe, fringe, -1.0f, hasHwScissor)) {
        return;
    }
    const uint32_t c = paint.innerColor.premultiplied().toRGBA8();

    // Expand single convex fill into coalesced triangle list to cut VkCmdBindPipeline/VkCmdDraw churn
    if (npaths == 1 && paths[0].convex) {
        const RenderPath& p = paths[0];
        int numTriVerts = 0;
        if (p.fillCount >= 3 && p.fill != nullptr) numTriVerts += (p.fillCount - 2) * 3;
        if (p.strokeCount >= 3 && p.stroke != nullptr) numTriVerts += (p.strokeCount - 2) * 3;
        if (numTriVerts > 0) {
            if (!m_impl->calls.empty()) {
                Call& last = m_impl->calls.back();
                if (last.type == CallType::Triangles &&
                    last.image == paint.image &&
                    last.blend == blend &&
                    last.clipDepth == m_impl->currentClipDepth &&
                    last.scissorRect[0] == (hasHwScissor ? hwScissor[0] : -1) &&
                    last.scissorRect[1] == (hasHwScissor ? hwScissor[1] : -1) &&
                    last.scissorRect[2] == (hasHwScissor ? hwScissor[2] : -1) &&
                    last.scissorRect[3] == (hasHwScissor ? hwScissor[3] : -1)) {
                    const FragUniforms* lastFrag = m_impl->fragUniformPtr(last.uniformOffset);
                    if (std::memcmp(lastFrag, &tempFrag, sizeof(FragUniforms)) == 0) {
                        const int offset = static_cast<int>(m_impl->verts.size());
                        m_impl->verts.resize(offset + numTriVerts);
                        Vertex* dst = &m_impl->verts[offset];
                        if (p.fillCount >= 3 && p.fill != nullptr) {
                            appendTriFanVertices(dst, p.fill, p.fillCount, c);
                            dst += (p.fillCount - 2) * 3;
                        }
                        if (p.strokeCount >= 3 && p.stroke != nullptr) {
                            appendTriStripVertices(dst, p.stroke, p.strokeCount, c);
                        }
                        last.triangleCount += numTriVerts;
                        return;
                    }
                }
            }

            Call call;
            call.type = CallType::Triangles;
            call.clipDepth = m_impl->currentClipDepth;
            call.image = paint.image;
            call.blend = blend;
            call.triangleOffset = static_cast<int>(m_impl->verts.size());
            call.triangleCount = numTriVerts;
            if (hasHwScissor) std::memcpy(call.scissorRect, hwScissor, sizeof(hwScissor));

            m_impl->verts.resize(call.triangleOffset + numTriVerts);
            Vertex* dst = &m_impl->verts[call.triangleOffset];
            if (p.fillCount >= 3 && p.fill != nullptr) {
                appendTriFanVertices(dst, p.fill, p.fillCount, c);
                dst += (p.fillCount - 2) * 3;
            }
            if (p.strokeCount >= 3 && p.stroke != nullptr) {
                appendTriStripVertices(dst, p.stroke, p.strokeCount, c);
            }
            call.uniformOffset = m_impl->storeUniformOrReuse(tempFrag);
            m_impl->calls.push_back(call);
            return;
        }
    }

    Call call;
    call.type = CallType::Fill;
    call.evenOdd = (npaths > 0 && paths[0].winding == Winding::EvenOdd);
    call.clipDepth = m_impl->currentClipDepth;
    call.triangleCount = 4;
    call.pathOffset = static_cast<int>(m_impl->paths.size());
    call.pathCount = npaths;
    call.image = paint.image;
    call.blend = blend;
    if (hasHwScissor) std::memcpy(call.scissorRect, hwScissor, sizeof(hwScissor));

    m_impl->paths.resize(call.pathOffset + npaths);

    int maxverts = 4;
    for (int i = 0; i < npaths; ++i) {
        maxverts += paths[i].fillCount + paths[i].strokeCount;
    }

    const int offset = static_cast<int>(m_impl->verts.size());
    m_impl->verts.resize(offset + maxverts);

    int curOffset = offset;
    for (int i = 0; i < npaths; ++i) {
        const RenderPath& p = paths[i];
        VkPath& copy = m_impl->paths[call.pathOffset + i];
        copy = VkPath{};
        if (p.fillCount > 0 && p.fill != nullptr) {
            copy.fillOffset = curOffset;
            copy.fillCount = p.fillCount;
            std::memcpy(&m_impl->verts[curOffset], p.fill, p.fillCount * sizeof(Vertex));
            for (int k = 0; k < p.fillCount; ++k) m_impl->verts[curOffset + k].color = c;
            curOffset += p.fillCount;
        }
        if (p.strokeCount > 0 && p.stroke != nullptr) {
            copy.strokeOffset = curOffset;
            copy.strokeCount = p.strokeCount;
            std::memcpy(&m_impl->verts[curOffset], p.stroke, p.strokeCount * sizeof(Vertex));
            for (int k = 0; k < p.strokeCount; ++k) m_impl->verts[curOffset + k].color = c;
            curOffset += p.strokeCount;
        }
    }

    const float minX = bounds ? bounds[0] : 0.0f;
    const float minY = bounds ? bounds[1] : 0.0f;
    const float maxX = bounds ? bounds[2] : m_impl->view[0];
    const float maxY = bounds ? bounds[3] : m_impl->view[1];
    call.triangleOffset = curOffset;
    Vertex* quad = &m_impl->verts[curOffset];
    quad[0] = Vertex(maxX, maxY, 0.5f, 1.0f, c);
    quad[1] = Vertex(maxX, minY, 0.5f, 1.0f, c);
    quad[2] = Vertex(minX, maxY, 0.5f, 1.0f, c);
    quad[3] = Vertex(minX, minY, 0.5f, 1.0f, c);
    curOffset += 4;

    m_impl->verts.resize(curOffset);
    call.uniformOffset = m_impl->storeUniformOrReuse(tempFrag);
    m_impl->calls.push_back(call);
}

void VulkanRenderer::renderStroke(const Paint& paint, const CompositeOperationState& compOp,
                                  const Scissor& scissor, float fringe, float strokeWidth,
                                  const RenderPath* paths, int npaths) {
    if (npaths <= 0 || paths == nullptr || m_impl->fragSize == 0) return;

    const VkBlendState blend = convertVkBlend(compOp);
    int hwScissor[4]{-1, -1, -1, -1};
    const bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);

    FragUniforms tempFrag;
    if (!m_impl->convertPaint(&tempFrag, paint, scissor, strokeWidth, fringe, -1.0f, hasHwScissor)) {
        return;
    }
    const uint32_t c = paint.innerColor.premultiplied().toRGBA8();

    int numTriVerts = 0;
    for (int i = 0; i < npaths; ++i) {
        if (paths[i].strokeCount >= 3 && paths[i].stroke != nullptr) {
            numTriVerts += (paths[i].strokeCount - 2) * 3;
        }
    }
    if (numTriVerts <= 0) return;

    if (!m_impl->calls.empty()) {
        Call& last = m_impl->calls.back();
        if (last.type == CallType::Triangles &&
            last.image == paint.image &&
            last.blend == blend &&
            last.clipDepth == m_impl->currentClipDepth &&
            last.scissorRect[0] == (hasHwScissor ? hwScissor[0] : -1) &&
            last.scissorRect[1] == (hasHwScissor ? hwScissor[1] : -1) &&
            last.scissorRect[2] == (hasHwScissor ? hwScissor[2] : -1) &&
            last.scissorRect[3] == (hasHwScissor ? hwScissor[3] : -1)) {
            const FragUniforms* lastFrag = m_impl->fragUniformPtr(last.uniformOffset);
            if (std::memcmp(lastFrag, &tempFrag, sizeof(FragUniforms)) == 0) {
                const int offset = static_cast<int>(m_impl->verts.size());
                m_impl->verts.resize(offset + numTriVerts);
                Vertex* dst = &m_impl->verts[offset];
                for (int i = 0; i < npaths; ++i) {
                    const RenderPath& p = paths[i];
                    if (p.strokeCount >= 3 && p.stroke != nullptr) {
                        appendTriStripVertices(dst, p.stroke, p.strokeCount, c);
                        dst += (p.strokeCount - 2) * 3;
                    }
                }
                last.triangleCount += numTriVerts;
                return;
            }
        }
    }

    Call call;
    call.type = CallType::Triangles;
    call.clipDepth = m_impl->currentClipDepth;
    call.image = paint.image;
    call.blend = blend;
    call.triangleOffset = static_cast<int>(m_impl->verts.size());
    call.triangleCount = numTriVerts;
    if (hasHwScissor) std::memcpy(call.scissorRect, hwScissor, sizeof(hwScissor));

    m_impl->verts.resize(call.triangleOffset + numTriVerts);
    Vertex* dst = &m_impl->verts[call.triangleOffset];
    for (int i = 0; i < npaths; ++i) {
        const RenderPath& p = paths[i];
        if (p.strokeCount >= 3 && p.stroke != nullptr) {
            appendTriStripVertices(dst, p.stroke, p.strokeCount, c);
            dst += (p.strokeCount - 2) * 3;
        }
    }
    call.uniformOffset = m_impl->storeUniformOrReuse(tempFrag);
    m_impl->calls.push_back(call);
}

void VulkanRenderer::renderTriangles(const Paint& paint, const CompositeOperationState& compOp,
                                     const Scissor& scissor, const Vertex* verts, int nverts,
                                     float fringe, int shaderType) {
    if (nverts <= 0 || verts == nullptr || m_impl->fragSize == 0) return;

    const VkBlendState blend = convertVkBlend(compOp);
    int hwScissor[4]{-1, -1, -1, -1};
    const bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);

    FragUniforms tempFrag;
    applyTriangleShaderUniforms(&tempFrag, paint, scissor, fringe, hasHwScissor, 0,
                                shaderType, [this](int id) { return m_impl->lookupTextureInfo(id); });

    if (!m_impl->calls.empty()) {
        Call& last = m_impl->calls.back();
        if (last.type == CallType::Triangles &&
            last.image == paint.image &&
            last.blend == blend &&
            last.clipDepth == m_impl->currentClipDepth &&
            last.scissorRect[0] == (hasHwScissor ? hwScissor[0] : -1) &&
            last.scissorRect[1] == (hasHwScissor ? hwScissor[1] : -1) &&
            last.scissorRect[2] == (hasHwScissor ? hwScissor[2] : -1) &&
            last.scissorRect[3] == (hasHwScissor ? hwScissor[3] : -1)) {
            const FragUniforms* lastFrag = m_impl->fragUniformPtr(last.uniformOffset);
            if (std::memcmp(lastFrag, &tempFrag, sizeof(FragUniforms)) == 0) {
                const int offset = static_cast<int>(m_impl->verts.size());
                m_impl->verts.resize(offset + nverts);
                std::memcpy(&m_impl->verts[offset], verts, nverts * sizeof(Vertex));
                last.triangleCount += nverts;
                return;
            }
        }
    }

    Call call;
    call.type = CallType::Triangles;
    call.clipDepth = m_impl->currentClipDepth;
    call.image = paint.image;
    call.blend = blend;
    call.triangleOffset = static_cast<int>(m_impl->verts.size());
    call.triangleCount = nverts;
    if (hasHwScissor) std::memcpy(call.scissorRect, hwScissor, sizeof(hwScissor));

    m_impl->verts.resize(call.triangleOffset + nverts);
    std::memcpy(&m_impl->verts[call.triangleOffset], verts, nverts * sizeof(Vertex));

    call.uniformOffset = m_impl->storeUniformOrReuse(tempFrag);
    m_impl->calls.push_back(call);
}

void VulkanRenderer::renderClip(const RenderPath* paths, int npaths, Winding winding, int targetDepth,
                                const Scissor& scissor, const float* bounds) {
    if (npaths <= 0 || paths == nullptr || m_impl->fragSize == 0) return;

    int hwScissor[4]{-1, -1, -1, -1};
    const bool hasHwScissor = m_impl->getHardwareScissor(scissor, hwScissor);

    Call call;
    call.type = CallType::Clip;
    call.clipDepth = targetDepth;
    call.winding = winding;
    if (hasHwScissor) std::memcpy(call.scissorRect, hwScissor, sizeof(hwScissor));

    call.pathOffset = static_cast<int>(m_impl->paths.size());
    call.pathCount = npaths;
    m_impl->paths.resize(call.pathOffset + npaths);

    int maxverts = 4;
    for (int i = 0; i < npaths; ++i) maxverts += paths[i].fillCount;

    const int offset = static_cast<int>(m_impl->verts.size());
    m_impl->verts.resize(offset + maxverts);

    int curOffset = offset;
    for (int i = 0; i < npaths; ++i) {
        const RenderPath& p = paths[i];
        VkPath& copy = m_impl->paths[call.pathOffset + i];
        copy = VkPath{};
        if (p.fillCount > 0 && p.fill != nullptr) {
            copy.fillOffset = curOffset;
            copy.fillCount = p.fillCount;
            std::memcpy(&m_impl->verts[curOffset], p.fill, p.fillCount * sizeof(Vertex));
            curOffset += p.fillCount;
        }
    }

    call.triangleOffset = curOffset;
    call.triangleCount = 4;
    const float minX = bounds ? bounds[0] : 0.0f;
    const float minY = bounds ? bounds[1] : 0.0f;
    const float maxX = bounds ? bounds[2] : m_impl->view[0];
    const float maxY = bounds ? bounds[3] : m_impl->view[1];
    Vertex* quad = &m_impl->verts[curOffset];
    quad[0] = Vertex(maxX, maxY, 0.5f, 1.0f);
    quad[1] = Vertex(maxX, minY, 0.5f, 1.0f);
    quad[2] = Vertex(minX, maxY, 0.5f, 1.0f);
    quad[3] = Vertex(minX, minY, 0.5f, 1.0f);
    curOffset += 4;
    m_impl->verts.resize(curOffset);

    FragUniforms tempFrag{};
    tempFrag.strokeThr = -1.0f;
    tempFrag.type = ShaderSimple;
    call.uniformOffset = m_impl->storeUniformOrReuse(tempFrag);

    m_impl->calls.push_back(call);
    m_impl->currentClipDepth = targetDepth;
}

void VulkanRenderer::restoreClip(int targetDepth) {
    if (m_impl->fragSize == 0) return;

    Call call;
    call.type = CallType::RestoreClip;
    call.clipDepth = targetDepth;
    call.scissorRect[0] = -1;

    const int offset = static_cast<int>(m_impl->verts.size());
    m_impl->verts.resize(offset + 4);
    Vertex* quad = &m_impl->verts[offset];
    quad[0] = Vertex(m_impl->view[0], m_impl->view[1], 0.5f, 1.0f);
    quad[1] = Vertex(m_impl->view[0], 0.0f, 0.5f, 1.0f);
    quad[2] = Vertex(0.0f, m_impl->view[1], 0.5f, 1.0f);
    quad[3] = Vertex(0.0f, 0.0f, 0.5f, 1.0f);
    call.triangleOffset = offset;
    call.triangleCount = 4;

    FragUniforms tempFrag{};
    tempFrag.strokeThr = -1.0f;
    tempFrag.type = ShaderSimple;
    call.uniformOffset = m_impl->storeUniformOrReuse(tempFrag);

    m_impl->calls.push_back(call);
    m_impl->currentClipDepth = targetDepth;
}

void VulkanRenderer::resetClip() {
    restoreClip(0);
}

void VulkanRenderer::flush() {
    if (m_impl->calls.empty()) return;

    VkCommandBuffer cmd = m_impl->externalCmd;
    if (cmd == VK_NULL_HANDLE && m_impl->targetSurface != nullptr) {
        cmd = m_impl->targetSurface->current_command_buffer();
    }
    if (cmd == VK_NULL_HANDLE) {
        m_impl->clearBatchBuffers();
        return;
    }

    const size_t vertBytes = m_impl->verts.size() * sizeof(Vertex);
    if (vertBytes > 0) {
        if (vertBytes > m_impl->vertBufCap) {
            vkDestroyBuffer(m_impl->device, m_impl->vertBuf, nullptr);
            vkFreeMemory(m_impl->device, m_impl->vertMem, nullptr);
            m_impl->vertBufCap = vertBytes + 65536;
            m_impl->createBuffer(m_impl->vertBufCap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 m_impl->vertBuf, m_impl->vertMem);
        }
        void* mappedVerts = nullptr;
        vkMapMemory(m_impl->device, m_impl->vertMem, 0, vertBytes, 0, &mappedVerts);
        std::memcpy(mappedVerts, m_impl->verts.data(), vertBytes);
        vkUnmapMemory(m_impl->device, m_impl->vertMem);
    }

    const size_t uniformBytes = m_impl->uniforms.size();
    if (uniformBytes > 0) {
        if (uniformBytes > m_impl->fragBufCap) {
            vkDestroyBuffer(m_impl->device, m_impl->fragBuf, nullptr);
            vkFreeMemory(m_impl->device, m_impl->fragMem, nullptr);
            m_impl->fragBufCap = uniformBytes + 16384;
            m_impl->createBuffer(m_impl->fragBufCap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 m_impl->fragBuf, m_impl->fragMem);
            m_impl->refreshUniformDescriptorSets();
        }
        void* mappedUniforms = nullptr;
        vkMapMemory(m_impl->device, m_impl->fragMem, 0, uniformBytes, 0, &mappedUniforms);
        std::memcpy(mappedUniforms, m_impl->uniforms.data(), uniformBytes);
        vkUnmapMemory(m_impl->device, m_impl->fragMem);
    }

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = std::max(1.0f, m_impl->view[0]);
    vp.height = std::max(1.0f, m_impl->view[1]);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    vkCmdPushConstants(cmd, m_impl->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2, m_impl->view);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_impl->vertBuf, &offset);

    VulkanTexture* dummy = m_impl->findTexture(m_impl->dummyTex);
    const bool hasStencilAttachment = (m_impl->targetSurface != nullptr);
    int activeClipDepth = m_impl->flushedClipDepth;

    for (const auto& call : m_impl->calls) {
        VkRect2D sci{};
        if (call.scissorRect[0] >= 0) {
            sci.offset = {std::max(0, call.scissorRect[0]), std::max(0, call.scissorRect[1])};
            sci.extent = {static_cast<uint32_t>(std::max(0, call.scissorRect[2])),
                          static_cast<uint32_t>(std::max(0, call.scissorRect[3]))};
        } else {
            sci.offset = {0, 0};
            sci.extent = {static_cast<uint32_t>(std::max(0, static_cast<int>(std::ceil(m_impl->view[0])))),
                          static_cast<uint32_t>(std::max(0, static_cast<int>(std::ceil(m_impl->view[1]))))};
        }
        vkCmdSetScissor(cmd, 0, 1, &sci);

        VulkanTexture* tex = (call.image != 0) ? m_impl->findTexture(call.image) : dummy;
        if (!tex) tex = dummy;

        const uint32_t dynamicOffset = static_cast<uint32_t>(call.uniformOffset);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_impl->pipelineLayout,
                                0, 1, &tex->descriptorSet, 1, &dynamicOffset);

        const VkStencilMode normalStencil = (hasStencilAttachment && activeClipDepth > 0)
            ? VkStencilMode::ClipTest : VkStencilMode::NoStencil;
        const uint32_t clipRef = static_cast<uint32_t>(activeClipDepth << 4);

        auto bindAndSetStencil = [&](VkPipeline p, uint32_t ref, bool hasStencilDynamic) {
            if (p == VK_NULL_HANDLE) return false;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            if (hasStencilDynamic) {
                vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
            }
            return true;
        };

        if (call.type == CallType::Triangles) {
            if (call.triangleCount > 0) {
                VkPipeline p = m_impl->getOrCreatePipeline(VkTopologyKind::List, normalStencil, call.blend);
                if (bindAndSetStencil(p, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                    vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                }
            }
        } else if (call.type == CallType::ConvexFill) {
            VkPipeline pFan = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, normalStencil, call.blend);
            if (bindAndSetStencil(pFan, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                }
            }
            VkPipeline pStrip = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, normalStencil, call.blend);
            if (bindAndSetStencil(pStrip, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.strokeCount > 0) vkCmdDraw(cmd, path.strokeCount, 1, path.strokeOffset, 0);
                }
            }
        } else if (call.type == CallType::Stroke) {
            VkPipeline p = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, normalStencil, call.blend);
            if (bindAndSetStencil(p, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.strokeCount > 0) vkCmdDraw(cmd, path.strokeCount, 1, path.strokeOffset, 0);
                }
            }
        } else if (call.type == CallType::Fill) {
            if (hasStencilAttachment) {
                const VkStencilMode stencilFillMode = (call.evenOdd || call.pathCount == 1)
                    ? VkStencilMode::FillStencilOdd
                    : VkStencilMode::FillStencilNonZero;
                VkPipeline pStencil = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, stencilFillMode, VkBlendState{});
                if (bindAndSetStencil(pStencil, clipRef, true)) {
                    for (int i = 0; i < call.pathCount; ++i) {
                        const auto& path = m_impl->paths[call.pathOffset + i];
                        if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                    }
                }
                if (edgeAntiAlias()) {
                    VkPipeline pAA = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::FillAntiAlias, call.blend);
                    if (bindAndSetStencil(pAA, clipRef, true)) {
                        for (int i = 0; i < call.pathCount; ++i) {
                            const auto& path = m_impl->paths[call.pathOffset + i];
                            if (path.strokeCount > 0) vkCmdDraw(cmd, path.strokeCount, 1, path.strokeOffset, 0);
                        }
                    }
                }
                if (call.triangleCount > 0) {
                    VkPipeline pCover = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::FillCover, call.blend);
                    if (bindAndSetStencil(pCover, 0u, true)) {
                        vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                    }
                }
            } else {
                VkPipeline pFan = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, VkStencilMode::NoStencil, call.blend);
                if (bindAndSetStencil(pFan, 0u, false)) {
                    for (int i = 0; i < call.pathCount; ++i) {
                        const auto& path = m_impl->paths[call.pathOffset + i];
                        if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                    }
                }
                VkPipeline pStrip = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::NoStencil, call.blend);
                if (bindAndSetStencil(pStrip, 0u, false)) {
                    for (int i = 0; i < call.pathCount; ++i) {
                        const auto& path = m_impl->paths[call.pathOffset + i];
                        if (path.strokeCount > 0) vkCmdDraw(cmd, path.strokeCount, 1, path.strokeOffset, 0);
                    }
                }
            }
        } else if (call.type == CallType::Clip && hasStencilAttachment) {
            const uint32_t prevDepthVal = static_cast<uint32_t>((call.clipDepth - 1) << 4);
            const uint32_t newDepthVal = static_cast<uint32_t>(call.clipDepth << 4);
            const VkStencilMode clipFillMode = (call.winding == Winding::Clockwise)
                ? VkStencilMode::FillStencilOdd
                : VkStencilMode::FillStencilNonZero;
            VkPipeline pClip = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, clipFillMode, VkBlendState{});
            if (bindAndSetStencil(pClip, prevDepthVal, true)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                }
            }
            if (call.triangleCount > 0) {
                VkPipeline pCover = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::ClipCover, VkBlendState{});
                if (bindAndSetStencil(pCover, newDepthVal, true)) {
                    vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                }
            }
            activeClipDepth = call.clipDepth;
        } else if (call.type == CallType::RestoreClip && hasStencilAttachment) {
            if (call.clipDepth < activeClipDepth && call.triangleCount > 0) {
                const uint32_t targetDepthVal = static_cast<uint32_t>(call.clipDepth << 4);
                VkPipeline pRestore = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::RestoreClip, VkBlendState{});
                if (bindAndSetStencil(pRestore, targetDepthVal, true)) {
                    vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                }
            }
            activeClipDepth = call.clipDepth;
        }
    }

    m_impl->flushedClipDepth = activeClipDepth;
    m_impl->clearBatchBuffers();
}

std::unique_ptr<Context> createContextVulkan(
    std::shared_ptr<VulkanSurface> surface,
    int flags
) {
    if (!surface) return nullptr;
    auto renderer = std::make_unique<VulkanRenderer>(
        surface->device()->backend_type() == GpuBackendType::Vulkan ?
            std::static_pointer_cast<VulkanDevice>(surface->device())->device() : VK_NULL_HANDLE,
        surface->device()->backend_type() == GpuBackendType::Vulkan ?
            std::static_pointer_cast<VulkanDevice>(surface->device())->physical_device() : VK_NULL_HANDLE,
        surface->device()->backend_type() == GpuBackendType::Vulkan ?
            std::static_pointer_cast<VulkanDevice>(surface->device())->graphics_queue() : VK_NULL_HANDLE,
        surface->device()->backend_type() == GpuBackendType::Vulkan ?
            std::static_pointer_cast<VulkanDevice>(surface->device())->graphics_queue_family() : 0,
        surface->render_pass(),
        (surface->samples() >= 4) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
        flags
    );
    renderer->setTargetSurface(surface.get());
    if (!renderer->init()) return nullptr;
    return std::make_unique<Context>(std::move(renderer), flags);
}

std::unique_ptr<Context> createContextVulkan(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkQueue queue,
    uint32_t queueFamilyIndex,
    VkRenderPass renderPass,
    VkSampleCountFlagBits samples,
    int flags
) {
    auto renderer = std::make_unique<VulkanRenderer>(
        device, physicalDevice, queue, queueFamilyIndex, renderPass, samples, flags
    );
    if (!renderer->init()) return nullptr;
    return std::make_unique<Context>(std::move(renderer), flags);
}

} // namespace nisaba::gpu
