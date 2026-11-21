#include "nisaba/gpu/vulkan_renderer.hpp"
#include "nisaba/gpu/vulkan_surface.hpp"
#include "../shader_uniforms.hpp"
#include "vulkan_shaders.hpp"
#include "vulkan_filter_shaders.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace nisaba::gpu {

namespace {

enum class CallType : uint8_t {
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

uint64_t packPipelineKey(VkTopologyKind topo, VkStencilMode stencil, const VkBlendState& b, bool offscreen = false, bool isPerlin = false) {
    uint64_t key = 0;
    key |= (static_cast<uint64_t>(topo) & 0x3ULL);
    key |= (static_cast<uint64_t>(stencil) & 0xFULL) << 2;
    key |= (static_cast<uint64_t>(b.srcRGB) & 0x1FULL) << 6;
    key |= (static_cast<uint64_t>(b.dstRGB) & 0x1FULL) << 11;
    key |= (static_cast<uint64_t>(b.srcAlpha) & 0x1FULL) << 16;
    key |= (static_cast<uint64_t>(b.dstAlpha) & 0x1FULL) << 21;
    key |= (static_cast<uint64_t>(b.colorOp) & 0xFULL) << 26;
    key |= (static_cast<uint64_t>(b.alphaOp) & 0xFULL) << 30;
    key |= (offscreen ? 1ULL : 0ULL) << 34;
    key |= (isPerlin ? 1ULL : 0ULL) << 35;
    return key;
}

struct VkBlurPushConstants {
    float uTexelOffset[2];
    int32_t uRadius;
    float uSigma;
    float uUvBounds[4];
};
static_assert(sizeof(VkBlurPushConstants) == 32, "VkBlurPushConstants must be 32 bytes");

struct VkBlendPushConstants {
    float uColor[4];
    float uRoiUv[4];
    float uCropUv[4];
    float uSrcOffsetUv[2];
    int32_t uMode;
    int32_t uPad;
};
static_assert(sizeof(VkBlendPushConstants) == 64, "VkBlendPushConstants must be 64 bytes");

struct VkColorFilterUbo {
    float cmat0[4];
    float cmat1[4];
    float cmat2[4];
    float cmat3[4];
    float cmatBias[4];
    float blendCol[4];
    float roiUv[4];
    float cropUv[4];
    float hcParams[4];
    int32_t flags0[4];
    int32_t flags1[4];
    uint32_t lut[256];
};
static_assert(sizeof(VkColorFilterUbo) == 1200, "VkColorFilterUbo must be 1200 bytes");

struct VkFilterOpsUbo {
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
static_assert(sizeof(VkFilterOpsUbo) == 256, "VkFilterOpsUbo must be 256 bytes");

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
    int layerIndex{-1};
    bool evenOdd{false};
    bool isPerlinNoise{false};
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
    VkShaderModule perlinFragShaderModule{VK_NULL_HANDLE};

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

    // Offscreen Layers & GPU Filter Graph Resources
    struct VkLayerTarget {
        int w{0};
        int h{0};
        bool inUse{false};
        VkImage colorImage{VK_NULL_HANDLE};
        VkDeviceMemory colorMem{VK_NULL_HANDLE};
        VkImageView colorView{VK_NULL_HANDLE};
        VkImage dsImage{VK_NULL_HANDLE};
        VkDeviceMemory dsMem{VK_NULL_HANDLE};
        VkImageView dsView{VK_NULL_HANDLE};
        VkFramebuffer layerFb{VK_NULL_HANDLE};
        VkFramebuffer filterFb{VK_NULL_HANDLE};
        VkDescriptorSet drawDescSet{VK_NULL_HANDLE};
        VkDescriptorSet blurDescSet{VK_NULL_HANDLE};
        VkDescriptorSet colorDescSet{VK_NULL_HANDLE};
    };
    struct VkActiveLayer {
        int poolIndex{-1};
        int parentClipDepth{0};
    };

    VkFormat offscreenDsFormat{VK_FORMAT_D24_UNORM_S8_UINT};
    VkRenderPass offscreenRenderPassClear{VK_NULL_HANDLE};
    VkRenderPass offscreenRenderPassLoad{VK_NULL_HANDLE};
    VkRenderPass filterRenderPassClear{VK_NULL_HANDLE};
    VkRenderPass filterRenderPassLoad{VK_NULL_HANDLE};
    VkSampler linearClampSampler{VK_NULL_HANDLE};

    VkShaderModule filterVertMod{VK_NULL_HANDLE};
    VkShaderModule filterBlurFragMod{VK_NULL_HANDLE};
    VkShaderModule filterBlendFragMod{VK_NULL_HANDLE};
    VkShaderModule filterColorFragMod{VK_NULL_HANDLE};
    VkShaderModule filterOpsFragMod{VK_NULL_HANDLE};

    VkDescriptorPool filterDescPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout filterBlurSetLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout filterBlendSetLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout filterColorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout filterOpsSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout filterBlurPipeLayout{VK_NULL_HANDLE};
    VkPipelineLayout filterBlendPipeLayout{VK_NULL_HANDLE};
    VkPipelineLayout filterColorPipeLayout{VK_NULL_HANDLE};
    VkPipelineLayout filterOpsPipeLayout{VK_NULL_HANDLE};

    VkPipeline filterBlurPipe{VK_NULL_HANDLE};
    VkPipeline filterBlendReplacePipe{VK_NULL_HANDLE};
    VkPipeline filterBlendOverPipe{VK_NULL_HANDLE};
    VkPipeline filterColorPipe{VK_NULL_HANDLE};
    VkPipeline filterOpsPipe{VK_NULL_HANDLE};

    VkBuffer filterColorBuf{VK_NULL_HANDLE};
    VkDeviceMemory filterColorMem{VK_NULL_HANDLE};
    size_t filterColorBufCap{0};
    size_t filterColorStride{256};
    size_t filterColorOffset{0};

    std::vector<LayerDesc> layerDescs;
    std::vector<VkLayerTarget> layerPool;
    std::vector<VkActiveLayer> activeLayers;
    std::unordered_map<uint64_t, VkDescriptorSet> filterBlendDescCache;
    std::unordered_map<uint64_t, VkDescriptorSet> filterOpsDescCache;
    struct VkRuntimeFilterEntry {
        VkShaderModule fragMod{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
    };
    std::unordered_map<uint64_t, VkRuntimeFilterEntry> runtimeFilterCache;
    static constexpr size_t kFilterUboMaxBytes = 1280;
    bool filterPipelineReady{false};

    void clearBatchBuffers() {
        verts.clear();
        paths.clear();
        calls.clear();
        uniforms.clear();
        layerDescs.clear();
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
        writes.reserve(textures.size() + layerPool.size());
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
        for (const auto& lt : layerPool) {
            if (lt.drawDescSet == VK_NULL_HANDLE) continue;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = lt.drawDescSet;
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

    VkPipeline getOrCreatePipeline(VkTopologyKind topo, VkStencilMode stencilMode, const VkBlendState& blend, bool offscreen = false, bool isPerlin = false) {
        if (!offscreen && renderPass == VK_NULL_HANDLE && targetSurface != nullptr) {
            renderPass = targetSurface->render_pass();
        }
        VkRenderPass activeRp = offscreen ? offscreenRenderPassClear : renderPass;
        if (activeRp == VK_NULL_HANDLE || pipelineLayout == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        const uint64_t key = packPipelineKey(topo, stencilMode, blend, offscreen, isPerlin);
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
        shaderStages[1].module = (isPerlin && perlinFragShaderModule != VK_NULL_HANDLE)
            ? perlinFragShaderModule : fragShaderModule;
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
        multisampling.rasterizationSamples = offscreen ? VK_SAMPLE_COUNT_1_BIT : samples;

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
        pipelineInfo.renderPass = activeRp;
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

    bool initFilterResources() {
        if (filterPipelineReady) return true;
        if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) return false;

        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        offscreenDsFormat = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            ? VK_FORMAT_D24_UNORM_S8_UINT
            : VK_FORMAT_D32_SFLOAT_S8_UINT;

        filterColorStride = (std::max(sizeof(VkColorFilterUbo), kFilterUboMaxBytes) + fragAlign - 1) & ~(fragAlign - 1);
        filterColorBufCap = filterColorStride * 256;
        if (!createBuffer(filterColorBufCap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          filterColorBuf, filterColorMem)) {
            return false;
        }

        VkSamplerCreateInfo sampInfo{};
        sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampInfo.magFilter = VK_FILTER_LINEAR;
        sampInfo.minFilter = VK_FILTER_LINEAR;
        sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampInfo.maxLod = 0.0f;
        if (vkCreateSampler(device, &sampInfo, nullptr, &linearClampSampler) != VK_SUCCESS) {
            return false;
        }

        auto makeOffscreenRp = [&](bool loadPrev, VkRenderPass& outRp) -> bool {
            std::array<VkAttachmentDescription, 2> atts{};
            atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
            atts[0].loadOp = loadPrev ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[0].initialLayout = loadPrev ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            atts[1].format = offscreenDsFormat;
            atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
            atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[1].stencilLoadOp = loadPrev ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            atts[1].initialLayout = loadPrev ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference dsRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &colorRef;
            sub.pDepthStencilAttachment = &dsRef;

            std::array<VkSubpassDependency, 2> deps{};
            deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            deps[0].dstSubpass = 0;
            deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            deps[1].srcSubpass = 0;
            deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

            VkRenderPassCreateInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpInfo.attachmentCount = static_cast<uint32_t>(atts.size());
            rpInfo.pAttachments = atts.data();
            rpInfo.subpassCount = 1;
            rpInfo.pSubpasses = &sub;
            rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
            rpInfo.pDependencies = deps.data();
            return vkCreateRenderPass(device, &rpInfo, nullptr, &outRp) == VK_SUCCESS;
        };

        auto makeFilterRp = [&](bool loadPrev, VkRenderPass& outRp) -> bool {
            VkAttachmentDescription att{};
            att.format = VK_FORMAT_R8G8B8A8_UNORM;
            att.samples = VK_SAMPLE_COUNT_1_BIT;
            att.loadOp = loadPrev ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att.initialLayout = loadPrev ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &colorRef;

            std::array<VkSubpassDependency, 2> deps{};
            deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            deps[0].dstSubpass = 0;
            deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            deps[1].srcSubpass = 0;
            deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

            VkRenderPassCreateInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpInfo.attachmentCount = 1;
            rpInfo.pAttachments = &att;
            rpInfo.subpassCount = 1;
            rpInfo.pSubpasses = &sub;
            rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
            rpInfo.pDependencies = deps.data();
            return vkCreateRenderPass(device, &rpInfo, nullptr, &outRp) == VK_SUCCESS;
        };

        if (!makeOffscreenRp(false, offscreenRenderPassClear) ||
            !makeOffscreenRp(true, offscreenRenderPassLoad) ||
            !makeFilterRp(false, filterRenderPassClear) ||
            !makeFilterRp(true, filterRenderPassLoad)) {
            return false;
        }

        auto makeShaderMod = [&](const uint32_t* code, size_t bytes, VkShaderModule& outMod) -> bool {
            VkShaderModuleCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            ci.codeSize = bytes;
            ci.pCode = code;
            return vkCreateShaderModule(device, &ci, nullptr, &outMod) == VK_SUCCESS;
        };
        if (!makeShaderMod(vulkan::NISABA_FILTER_VERT_SPV, vulkan::NISABA_FILTER_VERT_SPV_SIZE, filterVertMod) ||
            !makeShaderMod(vulkan::NISABA_FILTER_BLUR_FRAG_SPV, vulkan::NISABA_FILTER_BLUR_FRAG_SPV_SIZE, filterBlurFragMod) ||
            !makeShaderMod(vulkan::NISABA_FILTER_BLEND_FRAG_SPV, vulkan::NISABA_FILTER_BLEND_FRAG_SPV_SIZE, filterBlendFragMod) ||
            !makeShaderMod(vulkan::NISABA_FILTER_COLOR_FRAG_SPV, vulkan::NISABA_FILTER_COLOR_FRAG_SPV_SIZE, filterColorFragMod) ||
            !makeShaderMod(vulkan::NISABA_FILTER_OPS_FRAG_SPV, vulkan::NISABA_FILTER_OPS_FRAG_SPV_SIZE, filterOpsFragMod)) {
            return false;
        }

        // Descriptor pool for filter passes
        std::array<VkDescriptorPoolSize, 2> fPoolSizes{};
        fPoolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};
        fPoolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 256};
        VkDescriptorPoolCreateInfo fPoolInfo{};
        fPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        fPoolInfo.maxSets = 512;
        fPoolInfo.poolSizeCount = static_cast<uint32_t>(fPoolSizes.size());
        fPoolInfo.pPoolSizes = fPoolSizes.data();
        if (vkCreateDescriptorPool(device, &fPoolInfo, nullptr, &filterDescPool) != VK_SUCCESS) {
            return false;
        }

        // Blur set layout: binding 0 = sampler
        {
            VkDescriptorSetLayoutBinding b0{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ci.bindingCount = 1;
            ci.pBindings = &b0;
            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &filterBlurSetLayout) != VK_SUCCESS) return false;

            VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkBlurPushConstants)};
            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &filterBlurSetLayout;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(device, &pl, nullptr, &filterBlurPipeLayout) != VK_SUCCESS) return false;
        }

        // Blend set layout: binding 0 = src sampler, binding 1 = dst sampler
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bs{};
            bs[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bs[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ci.bindingCount = 2;
            ci.pBindings = bs.data();
            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &filterBlendSetLayout) != VK_SUCCESS) return false;

            VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkBlendPushConstants)};
            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &filterBlendSetLayout;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &pcr;
            if (vkCreatePipelineLayout(device, &pl, nullptr, &filterBlendPipeLayout) != VK_SUCCESS) return false;
        }

        // Color set layout: binding 0 = src sampler, binding 1 = dynamic UBO
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bs{};
            bs[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bs[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ci.bindingCount = 2;
            ci.pBindings = bs.data();
            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &filterColorSetLayout) != VK_SUCCESS) return false;

            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &filterColorSetLayout;
            if (vkCreatePipelineLayout(device, &pl, nullptr, &filterColorPipeLayout) != VK_SUCCESS) return false;
        }

        // FilterOps set layout: binding 0 = src sampler, binding 1 = src2 sampler, binding 2 = dynamic UBO
        {
            std::array<VkDescriptorSetLayoutBinding, 3> bs{};
            bs[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bs[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bs[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ci.bindingCount = 3;
            ci.pBindings = bs.data();
            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &filterOpsSetLayout) != VK_SUCCESS) return false;

            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &filterOpsSetLayout;
            if (vkCreatePipelineLayout(device, &pl, nullptr, &filterOpsPipeLayout) != VK_SUCCESS) return false;
        }

        auto makeFilterPipe = [&](VkShaderModule fragMod, VkPipelineLayout layout, bool sourceOverBlend, VkPipeline& outPipe) -> bool {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = filterVertMod;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragMod;
            stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vp{};
            vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineColorBlendAttachmentState cbAtt{};
            cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            cbAtt.blendEnable = sourceOverBlend ? VK_TRUE : VK_FALSE;
            cbAtt.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cbAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbAtt.colorBlendOp = VK_BLEND_OP_ADD;
            cbAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cbAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cbAtt.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 1;
            cb.pAttachments = &cbAtt;

            VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            ds.dynamicStateCount = 2;
            ds.pDynamicStates = dyn;

            VkGraphicsPipelineCreateInfo pi{};
            pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pi.stageCount = 2;
            pi.pStages = stages;
            pi.pVertexInputState = &vi;
            pi.pInputAssemblyState = &ia;
            pi.pViewportState = &vp;
            pi.pRasterizationState = &rs;
            pi.pMultisampleState = &ms;
            pi.pColorBlendState = &cb;
            pi.pDynamicState = &ds;
            pi.layout = layout;
            pi.renderPass = filterRenderPassClear;
            pi.subpass = 0;
            return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &outPipe) == VK_SUCCESS;
        };

        if (!makeFilterPipe(filterBlurFragMod, filterBlurPipeLayout, false, filterBlurPipe) ||
            !makeFilterPipe(filterBlendFragMod, filterBlendPipeLayout, false, filterBlendReplacePipe) ||
            !makeFilterPipe(filterBlendFragMod, filterBlendPipeLayout, true, filterBlendOverPipe) ||
            !makeFilterPipe(filterColorFragMod, filterColorPipeLayout, false, filterColorPipe) ||
            !makeFilterPipe(filterOpsFragMod, filterOpsPipeLayout, false, filterOpsPipe)) {
            return false;
        }

        filterPipelineReady = true;
        return true;
    }

    void destroyFilterResources() {
        if (device == VK_NULL_HANDLE) return;
        for (auto& lt : layerPool) {
            if (lt.layerFb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, lt.layerFb, nullptr);
            if (lt.filterFb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, lt.filterFb, nullptr);
            if (lt.colorView != VK_NULL_HANDLE) vkDestroyImageView(device, lt.colorView, nullptr);
            if (lt.colorImage != VK_NULL_HANDLE) vkDestroyImage(device, lt.colorImage, nullptr);
            if (lt.colorMem != VK_NULL_HANDLE) vkFreeMemory(device, lt.colorMem, nullptr);
            if (lt.dsView != VK_NULL_HANDLE) vkDestroyImageView(device, lt.dsView, nullptr);
            if (lt.dsImage != VK_NULL_HANDLE) vkDestroyImage(device, lt.dsImage, nullptr);
            if (lt.dsMem != VK_NULL_HANDLE) vkFreeMemory(device, lt.dsMem, nullptr);
        }
        layerPool.clear();
        activeLayers.clear();
        filterBlendDescCache.clear();
        filterOpsDescCache.clear();
        for (auto& [hash, entry] : runtimeFilterCache) {
            if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, entry.pipeline, nullptr);
            if (entry.fragMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, entry.fragMod, nullptr);
        }
        runtimeFilterCache.clear();

        auto destroyPipe = [&](VkPipeline& p) { if (p != VK_NULL_HANDLE) { vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; } };
        destroyPipe(filterBlurPipe);
        destroyPipe(filterBlendReplacePipe);
        destroyPipe(filterBlendOverPipe);
        destroyPipe(filterColorPipe);
        destroyPipe(filterOpsPipe);

        auto destroyPL = [&](VkPipelineLayout& p) { if (p != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, p, nullptr); p = VK_NULL_HANDLE; } };
        destroyPL(filterBlurPipeLayout);
        destroyPL(filterBlendPipeLayout);
        destroyPL(filterColorPipeLayout);
        destroyPL(filterOpsPipeLayout);

        auto destroySL = [&](VkDescriptorSetLayout& l) { if (l != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, l, nullptr); l = VK_NULL_HANDLE; } };
        destroySL(filterBlurSetLayout);
        destroySL(filterBlendSetLayout);
        destroySL(filterColorSetLayout);
        destroySL(filterOpsSetLayout);

        if (filterDescPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, filterDescPool, nullptr); filterDescPool = VK_NULL_HANDLE; }

        auto destroyMod = [&](VkShaderModule& m) { if (m != VK_NULL_HANDLE) { vkDestroyShaderModule(device, m, nullptr); m = VK_NULL_HANDLE; } };
        destroyMod(filterVertMod);
        destroyMod(filterBlurFragMod);
        destroyMod(filterBlendFragMod);
        destroyMod(filterColorFragMod);
        destroyMod(filterOpsFragMod);

        auto destroyRp = [&](VkRenderPass& r) { if (r != VK_NULL_HANDLE) { vkDestroyRenderPass(device, r, nullptr); r = VK_NULL_HANDLE; } };
        destroyRp(offscreenRenderPassClear);
        destroyRp(offscreenRenderPassLoad);
        destroyRp(filterRenderPassClear);
        destroyRp(filterRenderPassLoad);

        if (linearClampSampler != VK_NULL_HANDLE) { vkDestroySampler(device, linearClampSampler, nullptr); linearClampSampler = VK_NULL_HANDLE; }
        if (filterColorBuf != VK_NULL_HANDLE) { vkDestroyBuffer(device, filterColorBuf, nullptr); filterColorBuf = VK_NULL_HANDLE; }
        if (filterColorMem != VK_NULL_HANDLE) { vkFreeMemory(device, filterColorMem, nullptr); filterColorMem = VK_NULL_HANDLE; }
        filterPipelineReady = false;
    }

    int acquireVkLayerTarget(int w, int h) {
        if (!initFilterResources() || w <= 0 || h <= 0) return -1;
        for (size_t i = 0; i < layerPool.size(); ++i) {
            if (!layerPool[i].inUse && layerPool[i].w == w && layerPool[i].h == h) {
                layerPool[i].inUse = true;
                return static_cast<int>(i);
            }
        }

        auto allocImage = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage& img, VkDeviceMemory& mem) -> bool {
            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.format = fmt;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ci.usage = usage;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS) return false;
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device, img, &req);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) {
                vkDestroyImage(device, img, nullptr);
                img = VK_NULL_HANDLE;
                return false;
            }
            vkBindImageMemory(device, img, mem, 0);
            return true;
        };

        VkLayerTarget lt{};
        lt.w = w;
        lt.h = h;
        lt.inUse = true;
        if (!allocImage(VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        lt.colorImage, lt.colorMem)) {
            return -1;
        }
        if (!allocImage(offscreenDsFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, lt.dsImage, lt.dsMem)) {
            vkDestroyImage(device, lt.colorImage, nullptr);
            vkFreeMemory(device, lt.colorMem, nullptr);
            return -1;
        }

        VkImageViewCreateInfo cv{};
        cv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        cv.image = lt.colorImage;
        cv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        cv.format = VK_FORMAT_R8G8B8A8_UNORM;
        cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &cv, nullptr, &lt.colorView);

        VkImageViewCreateInfo dv{};
        dv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dv.image = lt.dsImage;
        dv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dv.format = offscreenDsFormat;
        dv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &dv, nullptr, &lt.dsView);

        std::array<VkImageView, 2> layerAtts = {lt.colorView, lt.dsView};
        VkFramebufferCreateInfo lfb{};
        lfb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        lfb.renderPass = offscreenRenderPassClear;
        lfb.attachmentCount = 2;
        lfb.pAttachments = layerAtts.data();
        lfb.width = static_cast<uint32_t>(w);
        lfb.height = static_cast<uint32_t>(h);
        lfb.layers = 1;
        vkCreateFramebuffer(device, &lfb, nullptr, &lt.layerFb);

        VkFramebufferCreateInfo ffb{};
        ffb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ffb.renderPass = filterRenderPassClear;
        ffb.attachmentCount = 1;
        ffb.pAttachments = &lt.colorView;
        ffb.width = static_cast<uint32_t>(w);
        ffb.height = static_cast<uint32_t>(h);
        ffb.layers = 1;
        vkCreateFramebuffer(device, &ffb, nullptr, &lt.filterFb);

        // Allocate drawDescSet (for compositing layer in main NISABA_FRAG_SPV)
        {
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descPool, 1, &descSetLayout};
            if (vkAllocateDescriptorSets(device, &da, &lt.drawDescSet) == VK_SUCCESS) {
                VkDescriptorBufferInfo dbi{fragBuf, 0, sizeof(FragUniforms)};
                VkDescriptorImageInfo dii{linearClampSampler, lt.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                std::array<VkWriteDescriptorSet, 2> ws{};
                ws[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lt.drawDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &dbi, nullptr};
                ws[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lt.drawDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
                vkUpdateDescriptorSets(device, 2, ws.data(), 0, nullptr);
            }
        }
        // Allocate blurDescSet (1 sampler)
        {
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, filterDescPool, 1, &filterBlurSetLayout};
            if (vkAllocateDescriptorSets(device, &da, &lt.blurDescSet) == VK_SUCCESS) {
                VkDescriptorImageInfo dii{linearClampSampler, lt.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lt.blurDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
                vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
            }
        }
        // Allocate colorDescSet (1 sampler + dynamic color UBO)
        {
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, filterDescPool, 1, &filterColorSetLayout};
            if (vkAllocateDescriptorSets(device, &da, &lt.colorDescSet) == VK_SUCCESS) {
                VkDescriptorImageInfo dii{linearClampSampler, lt.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkDescriptorBufferInfo dbi{filterColorBuf, 0, kFilterUboMaxBytes};
                std::array<VkWriteDescriptorSet, 2> ws{};
                ws[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lt.colorDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, nullptr, nullptr};
                ws[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lt.colorDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &dbi, nullptr};
                vkUpdateDescriptorSets(device, 2, ws.data(), 0, nullptr);
            }
        }

        layerPool.push_back(lt);
        return static_cast<int>(layerPool.size()) - 1;
    }

    VkDescriptorSet getOrCreateBlendDescSet(int srcPoolIdx, int dstPoolIdx) {
        const int safeDst = (dstPoolIdx >= 0) ? dstPoolIdx : srcPoolIdx;
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(srcPoolIdx)) << 16) |
                             static_cast<uint64_t>(static_cast<uint32_t>(safeDst));
        if (auto it = filterBlendDescCache.find(key); it != filterBlendDescCache.end()) {
            return it->second;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, filterDescPool, 1, &filterBlendSetLayout};
        if (vkAllocateDescriptorSets(device, &da, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

        VkDescriptorImageInfo dSrc{linearClampSampler, layerPool[srcPoolIdx].colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo dDst{linearClampSampler, layerPool[safeDst].colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2> ws{};
        ws[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dSrc, nullptr, nullptr};
        ws[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dDst, nullptr, nullptr};
        vkUpdateDescriptorSets(device, 2, ws.data(), 0, nullptr);
        filterBlendDescCache.emplace(key, set);
        return set;
    }

    VkDescriptorSet getOrCreateOpsDescSet(int srcPoolIdx, int src2PoolIdx) {
        const int safeSrc2 = (src2PoolIdx >= 0) ? src2PoolIdx : srcPoolIdx;
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(srcPoolIdx)) << 16) |
                             static_cast<uint64_t>(static_cast<uint32_t>(safeSrc2));
        if (auto it = filterOpsDescCache.find(key); it != filterOpsDescCache.end()) {
            return it->second;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, filterDescPool, 1, &filterOpsSetLayout};
        if (vkAllocateDescriptorSets(device, &da, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

        VkDescriptorImageInfo dSrc{linearClampSampler, layerPool[srcPoolIdx].colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo dSrc2{linearClampSampler, layerPool[safeSrc2].colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo dbi{filterColorBuf, 0, kFilterUboMaxBytes};
        std::array<VkWriteDescriptorSet, 3> ws{};
        ws[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dSrc, nullptr, nullptr};
        ws[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dSrc2, nullptr, nullptr};
        ws[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &dbi, nullptr};
        vkUpdateDescriptorSets(device, 3, ws.data(), 0, nullptr);
        filterOpsDescCache.emplace(key, set);
        return set;
    }

    void computeVkCropDev(const std::optional<nisaba::Rect>& cropRect,
                          const Transform2D& layerXform,
                          int w, int h,
                          float outDev[4]) const noexcept {
        if (!cropRect.has_value() || w <= 0 || h <= 0 || view[0] <= 0.0f || view[1] <= 0.0f) {
            outDev[0] = -1e6f; outDev[1] = -1e6f; outDev[2] = 1e6f; outDev[3] = 1e6f;
            return;
        }
        const nisaba::Transform ts = static_cast<nisaba::Transform>(layerXform);
        auto devCrop = cropRect->transform(ts);
        if (!devCrop.has_value()) {
            outDev[0] = 1e6f; outDev[1] = 1e6f; outDev[2] = -1e6f; outDev[3] = -1e6f;
            return;
        }
        const float sx = static_cast<float>(w) / view[0];
        const float sy = static_cast<float>(h) / view[1];
        outDev[0] = std::floor(devCrop->left() * sx);
        outDev[1] = std::floor(devCrop->top() * sy);
        outDev[2] = std::ceil(devCrop->right() * sx);
        outDev[3] = std::ceil(devCrop->bottom() * sy);
    }

    void runVkFilterOpsPass(VkCommandBuffer cmd, int dstPoolIdx, int srcPoolIdx, int src2PoolIdx,
                            const VkFilterOpsUbo& ubo, int w, int h) {
        if (filterColorOffset + filterColorStride > filterColorBufCap) return;
        VkDescriptorSet set = getOrCreateOpsDescSet(srcPoolIdx, src2PoolIdx);
        if (set == VK_NULL_HANDLE) return;

        void* mapped = nullptr;
        vkMapMemory(device, filterColorMem, filterColorOffset, sizeof(ubo), 0, &mapped);
        std::memcpy(mapped, &ubo, sizeof(ubo));
        vkUnmapMemory(device, filterColorMem);

        const uint32_t dynOff = static_cast<uint32_t>(filterColorOffset);
        filterColorOffset += filterColorStride;

        VkClearValue cv{};
        cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = filterRenderPassClear;
        rpb.framebuffer = layerPool[dstPoolIdx].filterFb;
        rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rpb.clearValueCount = 1;
        rpb.pClearValues = &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sci{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterOpsPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterOpsPipeLayout,
                                0, 1, &set, 1, &dynOff);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    void runVkFilterBlurPass(VkCommandBuffer cmd, int srcPoolIdx, int dstPoolIdx,
                             float offX, float offY, float sigma,
                             const float uvBounds[4], int w, int h) {
        VkClearValue cv{};
        cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = filterRenderPassClear;
        rpb.framebuffer = layerPool[dstPoolIdx].filterFb;
        rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rpb.clearValueCount = 1;
        rpb.pClearValues = &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sci{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterBlurPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterBlurPipeLayout,
                                0, 1, &layerPool[srcPoolIdx].blurDescSet, 0, nullptr);

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
        VkBlurPushConstants pc{};
        pc.uTexelOffset[0] = offX * step;
        pc.uTexelOffset[1] = offY * step;
        pc.uRadius = rad;
        pc.uSigma = effSigma;
        pc.uUvBounds[0] = uvBounds[0];
        pc.uUvBounds[1] = uvBounds[1];
        pc.uUvBounds[2] = uvBounds[2];
        pc.uUvBounds[3] = uvBounds[3];
        vkCmdPushConstants(cmd, filterBlurPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    int runVkGaussianBlur(VkCommandBuffer cmd, int srcPoolIdx, int w, int h,
                          float sigmaX, float sigmaY,
                          bool hasRoi, int roiX0, int roiY0, int roiX1, int roiY1) {
        const float invW = 1.0f / static_cast<float>(std::max(1, w));
        const float invH = 1.0f / static_cast<float>(std::max(1, h));
        const int ox0 = hasRoi ? std::clamp(roiX0, 0, w) : 0;
        const int oy0 = hasRoi ? std::clamp(roiY0, 0, h) : 0;
        const int ox1 = hasRoi ? std::clamp(roiX1, 0, w) : w;
        const int oy1 = hasRoi ? std::clamp(roiY1, 0, h) : h;
        const float uvBounds[4] = {
            (static_cast<float>(ox0) + 0.5f) * invW,
            (static_cast<float>(oy0) + 0.5f) * invH,
            (static_cast<float>(ox1) - 0.5f) * invW,
            (static_cast<float>(oy1) - 0.5f) * invH
        };
        const int tmpIdx = acquireVkLayerTarget(w, h);
        const int outIdx = acquireVkLayerTarget(w, h);
        if (tmpIdx < 0 || outIdx < 0) {
            if (tmpIdx >= 0) layerPool[tmpIdx].inUse = false;
            if (outIdx >= 0) layerPool[outIdx].inUse = false;
            return srcPoolIdx;
        }
        runVkFilterBlurPass(cmd, srcPoolIdx, tmpIdx, invW, 0.0f, sigmaX, uvBounds, w, h);
        runVkFilterBlurPass(cmd, tmpIdx, outIdx, 0.0f, invH, sigmaY, uvBounds, w, h);
        layerPool[tmpIdx].inUse = false;
        return outIdx;
    }

    void runVkFilterBlendPass(VkCommandBuffer cmd, int dstPoolIdx, int srcPoolIdx, int bgPoolIdx,
                              int mode, float offU, float offV, const Color& tintPremul,
                              const float roiUv[4], const float cropUv[4],
                              int w, int h, bool loadAndBlendOver) {
        VkDescriptorSet set = getOrCreateBlendDescSet(srcPoolIdx, bgPoolIdx);
        if (set == VK_NULL_HANDLE) return;

        VkClearValue cv{};
        cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = loadAndBlendOver ? filterRenderPassLoad : filterRenderPassClear;
        rpb.framebuffer = layerPool[dstPoolIdx].filterFb;
        rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rpb.clearValueCount = loadAndBlendOver ? 0u : 1u;
        rpb.pClearValues = loadAndBlendOver ? nullptr : &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sci{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          loadAndBlendOver ? filterBlendOverPipe : filterBlendReplacePipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterBlendPipeLayout, 0, 1, &set, 0, nullptr);

        VkBlendPushConstants pc{};
        pc.uColor[0] = tintPremul.r; pc.uColor[1] = tintPremul.g; pc.uColor[2] = tintPremul.b; pc.uColor[3] = tintPremul.a;
        std::memcpy(pc.uRoiUv, roiUv, sizeof(pc.uRoiUv));
        std::memcpy(pc.uCropUv, cropUv, sizeof(pc.uCropUv));
        pc.uSrcOffsetUv[0] = offU;
        pc.uSrcOffsetUv[1] = offV;
        pc.uMode = mode;
        pc.uPad = 0;
        vkCmdPushConstants(cmd, filterBlendPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    void runVkFilterColorPass(VkCommandBuffer cmd, int dstPoolIdx, int srcPoolIdx,
                              const effects::ColorEffect& eff,
                              const float roiUv[4], const float cropUv[4],
                              int w, int h) {
        if (filterColorOffset + filterColorStride > filterColorBufCap) {
            return;
        }
        VkColorFilterUbo ubo{};
        const auto* matPtr = &eff.matrix;
        int32_t extKind = 0;
        if (eff.is_extended()) {
            const auto& en = *eff.ext_node;
            if (en.kind == effects::ColorEffectExtKind::Table) {
                extKind = 1;
                for (int i = 0; i < 256; ++i) {
                    ubo.lut[i] = static_cast<uint32_t>(en.lut_r[i]) |
                                 (static_cast<uint32_t>(en.lut_g[i]) << 8) |
                                 (static_cast<uint32_t>(en.lut_b[i]) << 16) |
                                 (static_cast<uint32_t>(en.lut_a[i]) << 24);
                }
            } else if (en.kind == effects::ColorEffectExtKind::HslaMatrix) {
                extKind = 2;
                matPtr = &en.hsla_matrix;
            } else if (en.kind == effects::ColorEffectExtKind::Luma) {
                extKind = 3;
            } else if (en.kind == effects::ColorEffectExtKind::HighContrast) {
                extKind = 4;
                ubo.hcParams[0] = en.high_contrast.grayscale ? 1.0f : 0.0f;
                ubo.hcParams[1] = static_cast<float>(en.high_contrast.invert_style);
                ubo.hcParams[2] = en.high_contrast.contrast;
                ubo.hcParams[3] = 0.0f;
            }
        }
        const auto& m = matPtr->m;
        ubo.cmat0[0] = m[0];  ubo.cmat0[1] = m[1];  ubo.cmat0[2] = m[2];  ubo.cmat0[3] = m[3];
        ubo.cmat1[0] = m[5];  ubo.cmat1[1] = m[6];  ubo.cmat1[2] = m[7];  ubo.cmat1[3] = m[8];
        ubo.cmat2[0] = m[10]; ubo.cmat2[1] = m[11]; ubo.cmat2[2] = m[12]; ubo.cmat2[3] = m[13];
        ubo.cmat3[0] = m[15]; ubo.cmat3[1] = m[16]; ubo.cmat3[2] = m[17]; ubo.cmat3[3] = m[18];
        ubo.cmatBias[0] = m[4]; ubo.cmatBias[1] = m[9]; ubo.cmatBias[2] = m[14]; ubo.cmatBias[3] = m[19];
        const Color bc = Color(eff.blend_color).premultiplied();
        ubo.blendCol[0] = bc.r; ubo.blendCol[1] = bc.g; ubo.blendCol[2] = bc.b; ubo.blendCol[3] = bc.a;
        std::memcpy(ubo.roiUv, roiUv, sizeof(ubo.roiUv));
        std::memcpy(ubo.cropUv, cropUv, sizeof(ubo.cropUv));
        ubo.flags0[0] = eff.has_matrix ? 1 : 0;
        ubo.flags0[1] = static_cast<int32_t>(eff.transfer);
        ubo.flags0[2] = eff.has_blend ? 1 : 0;
        ubo.flags0[3] = eff.has_blend ? (static_cast<int32_t>(eff.blend_mode) + 1) : 0;
        ubo.flags1[0] = extKind;

        void* mapped = nullptr;
        vkMapMemory(device, filterColorMem, filterColorOffset, sizeof(ubo), 0, &mapped);
        std::memcpy(mapped, &ubo, sizeof(ubo));
        vkUnmapMemory(device, filterColorMem);

        const uint32_t dynOff = static_cast<uint32_t>(filterColorOffset);
        filterColorOffset += filterColorStride;

        VkClearValue cv{};
        cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = filterRenderPassClear;
        rpb.framebuffer = layerPool[dstPoolIdx].filterFb;
        rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rpb.clearValueCount = 1;
        rpb.pClearValues = &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sci{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterColorPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterColorPipeLayout,
                                0, 1, &layerPool[srcPoolIdx].colorDescSet, 1, &dynOff);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    int evalColorEffectDagVk(VkCommandBuffer cmd,
                             const effects::ColorEffect& eff,
                             int srcPoolIdx, int w, int h,
                             const float roiUv[4], const float cropUv[4],
                             bool hasRoi, int roiX0, int roiY0, int roiX1, int roiY1) {
        const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
        if (eff.is_extended()) {
            const auto& en = *eff.ext_node;
            if (en.kind == effects::ColorEffectExtKind::Compose) {
                const int innerIdx = (en.child1 && en.child1->is_active())
                    ? evalColorEffectDagVk(cmd, *en.child1, srcPoolIdx, w, h, roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1)
                    : srcPoolIdx;
                const int outIdx = (en.child0 && en.child0->is_active())
                    ? evalColorEffectDagVk(cmd, *en.child0, innerIdx, w, h, roiUv, cropUv, hasRoi, roiX0, roiY0, roiX1, roiY1)
                    : innerIdx;
                if (innerIdx != srcPoolIdx && innerIdx != outIdx) layerPool[innerIdx].inUse = false;
                return outIdx;
            }
            if (en.kind == effects::ColorEffectExtKind::Lerp) {
                const int idx0 = (en.child0 && en.child0->is_active())
                    ? evalColorEffectDagVk(cmd, *en.child0, srcPoolIdx, w, h, roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1)
                    : srcPoolIdx;
                const int idx1 = (en.child1 && en.child1->is_active())
                    ? evalColorEffectDagVk(cmd, *en.child1, srcPoolIdx, w, h, roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1)
                    : srcPoolIdx;
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    u.cropDev[0] = cropUv[0] * static_cast<float>(w);
                    u.cropDev[1] = cropUv[1] * static_cast<float>(h);
                    u.cropDev[2] = cropUv[2] * static_cast<float>(w);
                    u.cropDev[3] = cropUv[3] * static_cast<float>(h);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = 1.0f / static_cast<float>(std::max(1, w));
                    u.fbSize[3] = 1.0f / static_cast<float>(std::max(1, h));
                    u.i0[0] = 6; u.i0[1] = 1;
                    u.p0[0] = 0.0f; u.p0[1] = en.lerp_t; u.p0[2] = 1.0f - en.lerp_t; u.p0[3] = 0.0f;
                    runVkFilterOpsPass(cmd, outIdx, idx1, idx0, u, w, h);
                }
                if (idx0 != srcPoolIdx) layerPool[idx0].inUse = false;
                if (idx1 != srcPoolIdx && idx1 != idx0) layerPool[idx1].inUse = false;
                return (outIdx >= 0) ? outIdx : srcPoolIdx;
            }
        }
        const int outIdx = acquireVkLayerTarget(w, h);
        if (outIdx < 0) return srcPoolIdx;
        if (eff.is_active()) {
            runVkFilterColorPass(cmd, outIdx, srcPoolIdx, eff, roiUv, cropUv, w, h);
        } else {
            runVkFilterBlendPass(cmd, outIdx, srcPoolIdx, srcPoolIdx, 0, 0.0f, 0.0f,
                                 Color(1, 1, 1, 1), roiUv, cropUv, w, h, false);
        }
        return outIdx;
    }

    static std::string stripStandaloneUniformDecls(const std::string& src) {
        std::string out;
        out.reserve(src.size());
        const size_t n = src.size();
        size_t i = 0;
        auto isIdStart = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
        auto isIdChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        while (i < n) {
            if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
                size_t s = i; i += 2;
                while (i < n && src[i] != '\n') ++i;
                out.append(src, s, i - s);
                continue;
            }
            if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
                size_t s = i; i += 2;
                while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
                if (i + 1 < n) i += 2;
                out.append(src, s, i - s);
                continue;
            }
            if (isIdStart(src[i])) {
                size_t s = i;
                while (i < n && isIdChar(src[i])) ++i;
                if (i - s == 7 && src.compare(s, 7, "uniform") == 0) {
                    while (i < n && src[i] != ';') ++i;
                    if (i < n && src[i] == ';') ++i;
                    continue;
                }
                out.append(src, s, i - s);
                continue;
            }
            out.push_back(src[i++]);
        }
        return out;
    }

    static const char* glslTypeForRuntimeUniform(effects::RuntimeUniformType t) noexcept {
        switch (t) {
            case effects::RuntimeUniformType::Float: return "float";
            case effects::RuntimeUniformType::Float2: return "vec2";
            case effects::RuntimeUniformType::Float3: return "vec3";
            case effects::RuntimeUniformType::Float4: return "vec4";
            case effects::RuntimeUniformType::Mat2: return "mat2";
            case effects::RuntimeUniformType::Mat3: return "mat3";
            case effects::RuntimeUniformType::Mat4: return "mat4";
            case effects::RuntimeUniformType::Int: return "int";
        }
        return "float";
    }

    static void packRuntimeUniformsStd140(const effects::RuntimeFilterSpec& spec, uint8_t* dst, size_t maxBytes, size_t startOff = 112) noexcept {
        size_t off = startOff;
        auto alignUp = [](size_t v, size_t a) { return (v + a - 1) & ~(a - 1); };
        for (const auto& u : spec.uniforms) {
            const bool isArr = (u.count > 1);
            size_t align = 4;
            switch (u.type) {
                case effects::RuntimeUniformType::Float:
                case effects::RuntimeUniformType::Int: align = isArr ? 16 : 4; break;
                case effects::RuntimeUniformType::Float2: align = isArr ? 16 : 8; break;
                default: align = 16; break;
            }
            off = alignUp(off, align);
            if (u.offset + u.size_bytes > spec.uniform_data.size()) continue;
            const uint8_t* src = spec.uniform_data.data() + u.offset;
            for (int k = 0; k < u.count; ++k) {
                switch (u.type) {
                    case effects::RuntimeUniformType::Float:
                    case effects::RuntimeUniformType::Int:
                        if (off + 4 <= maxBytes) std::memcpy(dst + off, src + k * 4, 4);
                        off += isArr ? 16 : 4;
                        break;
                    case effects::RuntimeUniformType::Float2:
                        if (off + 8 <= maxBytes) std::memcpy(dst + off, src + k * 8, 8);
                        off += isArr ? 16 : 8;
                        break;
                    case effects::RuntimeUniformType::Float3:
                        if (off + 12 <= maxBytes) std::memcpy(dst + off, src + k * 12, 12);
                        off += isArr ? 16 : 12;
                        break;
                    case effects::RuntimeUniformType::Float4:
                        if (off + 16 <= maxBytes) std::memcpy(dst + off, src + k * 16, 16);
                        off += 16;
                        break;
                    case effects::RuntimeUniformType::Mat2:
                        for (int col = 0; col < 2; ++col) {
                            if (off + 8 <= maxBytes) std::memcpy(dst + off, src + k * 16 + col * 8, 8);
                            off += 16;
                        }
                        break;
                    case effects::RuntimeUniformType::Mat3:
                        for (int col = 0; col < 3; ++col) {
                            if (off + 12 <= maxBytes) std::memcpy(dst + off, src + k * 36 + col * 12, 12);
                            off += 16;
                        }
                        break;
                    case effects::RuntimeUniformType::Mat4:
                        if (off + 64 <= maxBytes) std::memcpy(dst + off, src + k * 64, 64);
                        off += 64;
                        break;
                }
            }
            if (isArr) off = alignUp(off, 16);
        }
    }

    VkPipeline getOrCreateVkRuntimeFilterPipe(const effects::RuntimeFilterSpec& spec) {
        if (auto it = runtimeFilterCache.find(spec.shader_hash); it != runtimeFilterCache.end()) {
            return it->second.pipeline;
        }
        VkRuntimeFilterEntry entry{};
        std::string glsl;
        glsl.reserve(spec.transformed_glsl.size() + 2048);
        glsl +=
            "#version 450\n"
            "layout(location = 0) in vec2 vUv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(binding = 0) uniform sampler2D uInputTex;\n"
            "layout(std140, binding = 1) uniform RuntimeFilterUbo {\n"
            "    vec4 _nisaba_fb;\n"
            "    vec4 _nisaba_roi;\n"
            "    vec4 _nisaba_crop;\n"
            "    vec4 _nisaba_lmat0;\n"
            "    vec4 _nisaba_lmat1;\n"
            "    vec4 _nisaba_ilmat0;\n"
            "    vec4 _nisaba_ilmat1;\n";
        for (const auto& u : spec.uniforms) {
            glsl += "    ";
            glsl += glslTypeForRuntimeUniform(u.type);
            glsl += " ";
            glsl += u.name;
            if (u.count > 1) {
                glsl += "[";
                glsl += std::to_string(u.count);
                glsl += "]";
            }
            glsl += ";\n";
        }
        glsl +=
            "};\n"
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
            "    vec2 dp = vec2(_nisaba_lmat0.x * lp.x + _nisaba_lmat0.z * lp.y + _nisaba_lmat1.x,\n"
            "                   _nisaba_lmat0.y * lp.x + _nisaba_lmat0.w * lp.y + _nisaba_lmat1.y);\n"
            "    vec2 uv = dp / _nisaba_fb.xy;\n"
            "    if (uv.x < _nisaba_roi.x || uv.x > _nisaba_roi.z || uv.y < _nisaba_roi.y || uv.y > _nisaba_roi.w) return vec4(0.0);\n"
            "    return texture(uInputTex, uv);\n"
            "}\n";
        for (const auto& ch : spec.children) {
            const std::string fn = "__nisaba_child_eval_" + std::to_string(ch.index);
            if (spec.has_input_child && ch.index == spec.input_child_index) {
                glsl += "vec4 " + fn + "(vec2 p) { return _nisaba_sample_input(p); }\n";
                glsl += "vec4 " + fn + "(vec2 p, vec4 c) { return _nisaba_sample_input(p); }\n";
            } else {
                glsl += "vec4 " + fn + "(vec2 p) { return vec4(0.0); }\n";
                glsl += "vec4 " + fn + "(vec2 p, vec4 c) { return vec4(0.0); }\n";
            }
        }
        glsl += stripStandaloneUniformDecls(spec.transformed_glsl);
        glsl +=
            "\nvoid main() {\n"
            "    if (vUv.x < _nisaba_crop.x || vUv.x > _nisaba_crop.z || vUv.y < _nisaba_crop.y || vUv.y > _nisaba_crop.w) {\n"
            "        outColor = vec4(0.0);\n"
            "        return;\n"
            "    }\n"
            "    vec2 devPos = vUv * _nisaba_fb.xy;\n"
            "    vec2 localPos = vec2(_nisaba_ilmat0.x * devPos.x + _nisaba_ilmat0.z * devPos.y + _nisaba_ilmat1.x,\n"
            "                         _nisaba_ilmat0.y * devPos.x + _nisaba_ilmat0.w * devPos.y + _nisaba_ilmat1.y);\n";
        if (spec.has_color_in) {
            glsl += "    vec4 c = user_main(localPos, vec4(1.0));\n";
        } else {
            glsl += "    vec4 c = user_main(localPos);\n";
        }
        glsl +=
            "    float a = clamp(c.a, 0.0, 1.0);\n"
            "    outColor = vec4(clamp(c.rgb, vec3(0.0), vec3(a)), a);\n"
            "}\n";

        const std::string base = "/tmp/nisaba_vkrf_" + std::to_string(static_cast<long>(getpid())) +
                                 "_" + std::to_string(spec.shader_hash);
        const std::string fragPath = base + ".frag";
        const std::string spvPath = base + ".spv";
        {
            std::ofstream ofs(fragPath, std::ios::binary);
            ofs.write(glsl.data(), static_cast<std::streamsize>(glsl.size()));
        }
        const std::string cmdStr = "glslangValidator -V \"" + fragPath + "\" -o \"" + spvPath + "\" >/dev/null 2>&1";
        const int rc = std::system(cmdStr.c_str());
        std::remove(fragPath.c_str());

        if (rc == 0) {
            std::ifstream ifs(spvPath, std::ios::binary | std::ios::ate);
            if (ifs) {
                const auto sz = ifs.tellg();
                if (sz > 0 && (sz % 4) == 0) {
                    std::vector<uint32_t> spv(static_cast<size_t>(sz) / 4);
                    ifs.seekg(0);
                    ifs.read(reinterpret_cast<char*>(spv.data()), sz);
                    VkShaderModuleCreateInfo smci{};
                    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    smci.codeSize = static_cast<size_t>(sz);
                    smci.pCode = spv.data();
                    if (vkCreateShaderModule(device, &smci, nullptr, &entry.fragMod) == VK_SUCCESS) {
                        VkPipelineShaderStageCreateInfo stages[2]{};
                        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                        stages[0].module = filterVertMod;
                        stages[0].pName = "main";
                        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                        stages[1].module = entry.fragMod;
                        stages[1].pName = "main";

                        VkPipelineVertexInputStateCreateInfo vi{};
                        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                        VkPipelineInputAssemblyStateCreateInfo ia{};
                        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                        VkPipelineViewportStateCreateInfo vp{};
                        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                        vp.viewportCount = 1; vp.scissorCount = 1;
                        VkPipelineRasterizationStateCreateInfo rs{};
                        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
                        VkPipelineMultisampleStateCreateInfo ms{};
                        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                        VkPipelineColorBlendAttachmentState cbAtt{};
                        cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                        VkPipelineColorBlendStateCreateInfo cb{};
                        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                        cb.attachmentCount = 1; cb.pAttachments = &cbAtt;
                        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                        VkPipelineDynamicStateCreateInfo ds{};
                        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

                        VkGraphicsPipelineCreateInfo pi{};
                        pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                        pi.stageCount = 2; pi.pStages = stages;
                        pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia;
                        pi.pViewportState = &vp; pi.pRasterizationState = &rs;
                        pi.pMultisampleState = &ms; pi.pColorBlendState = &cb;
                        pi.pDynamicState = &ds; pi.layout = filterColorPipeLayout;
                        pi.renderPass = filterRenderPassClear; pi.subpass = 0;
                        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &entry.pipeline);
                    }
                }
            }
        }
        std::remove(spvPath.c_str());

        auto [insIt, _] = runtimeFilterCache.emplace(spec.shader_hash, entry);
        return insIt->second.pipeline;
    }

    void runVkFilterRuntimePass(VkCommandBuffer cmd, int dstPoolIdx, int srcPoolIdx,
                                const effects::RuntimeFilterSpec& spec,
                                const Transform2D& layerXform,
                                const float roiUv[4], const float cropUv[4],
                                int w, int h) {
        if (filterColorOffset + filterColorStride > filterColorBufCap) return;
        VkPipeline pipe = getOrCreateVkRuntimeFilterPipe(spec);
        if (pipe == VK_NULL_HANDLE) return;

        alignas(16) uint8_t uboBytes[kFilterUboMaxBytes]{};
        float* hdr = reinterpret_cast<float*>(uboBytes);
        hdr[0] = static_cast<float>(w); hdr[1] = static_cast<float>(h); hdr[2] = 0.0f; hdr[3] = 0.0f;
        std::memcpy(hdr + 4, roiUv, 16);
        std::memcpy(hdr + 8, cropUv, 16);

        const float sxDev = (view[0] > 0.0f) ? (static_cast<float>(w) / view[0]) : 1.0f;
        const float syDev = (view[1] > 0.0f) ? (static_cast<float>(h) / view[1]) : 1.0f;
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
        hdr[12] = a;  hdr[13] = b;  hdr[14] = c;  hdr[15] = d;
        hdr[16] = tx; hdr[17] = ty; hdr[18] = 0.0f; hdr[19] = 0.0f;
        hdr[20] = ia; hdr[21] = ib; hdr[22] = ic; hdr[23] = id;
        hdr[24] = itx; hdr[25] = ity; hdr[26] = 0.0f; hdr[27] = 0.0f;

        packRuntimeUniformsStd140(spec, uboBytes, kFilterUboMaxBytes);

        void* mapped = nullptr;
        vkMapMemory(device, filterColorMem, filterColorOffset, kFilterUboMaxBytes, 0, &mapped);
        std::memcpy(mapped, uboBytes, kFilterUboMaxBytes);
        vkUnmapMemory(device, filterColorMem);

        const uint32_t dynOff = static_cast<uint32_t>(filterColorOffset);
        filterColorOffset += filterColorStride;

        VkClearValue cv{};
        cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = filterRenderPassClear;
        rpb.framebuffer = layerPool[dstPoolIdx].filterFb;
        rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rpb.clearValueCount = 1;
        rpb.pClearValues = &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sci{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterColorPipeLayout,
                                0, 1, &layerPool[srcPoolIdx].colorDescSet, 1, &dynOff);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    VkPipeline getOrCreateVkRuntimeBlenderPipe(const effects::RuntimeFilterSpec& spec) {
        const uint64_t key = spec.shader_hash ^ 0xB1E4D001ULL;
        if (auto it = runtimeFilterCache.find(key); it != runtimeFilterCache.end()) {
            return it->second.pipeline;
        }
        VkRuntimeFilterEntry entry{};
        std::string glsl;
        glsl.reserve(spec.transformed_glsl.size() + 1536);
        glsl +=
            "#version 450\n"
            "layout(location = 0) in vec2 vUv;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "layout(binding = 0) uniform sampler2D uSrcTex;\n"
            "layout(binding = 1) uniform sampler2D uDstTex;\n"
            "layout(std140, binding = 2) uniform RuntimeBlenderUbo {\n"
            "    vec4 _nisaba_op;\n";
        for (const auto& u : spec.uniforms) {
            glsl += "    ";
            glsl += glslTypeForRuntimeUniform(u.type);
            glsl += " ";
            glsl += u.name;
            if (u.count > 1) { glsl += "["; glsl += std::to_string(u.count); glsl += "]"; }
            glsl += ";\n";
        }
        glsl +=
            "};\n"
            "float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
            "vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
            "vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
            "vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
            "vec4 unpremul(vec4 c) { return vec4(c.a > 0.0001 ? c.rgb / c.a : vec3(0.0), c.a); }\n";
        glsl += stripStandaloneUniformDecls(spec.transformed_glsl);
        glsl +=
            "\nvoid main() {\n"
            "    vec4 src = texture(uSrcTex, vUv) * _nisaba_op.x;\n"
            "    vec4 dst = texture(uDstTex, vUv);\n"
            "    vec4 c = user_main(src, dst);\n"
            "    float a = clamp(c.a, 0.0, 1.0);\n"
            "    outColor = vec4(clamp(c.rgb, vec3(0.0), vec3(a)), a);\n"
            "}\n";

        const std::string base = "/tmp/nisaba_vkrb_" + std::to_string(static_cast<long>(getpid())) +
                                 "_" + std::to_string(key);
        const std::string fragPath = base + ".frag";
        const std::string spvPath = base + ".spv";
        {
            std::ofstream ofs(fragPath, std::ios::binary);
            ofs.write(glsl.data(), static_cast<std::streamsize>(glsl.size()));
        }
        const std::string cmdStr = "glslangValidator -V \"" + fragPath + "\" -o \"" + spvPath + "\" >/dev/null 2>&1";
        const int rc = std::system(cmdStr.c_str());
        std::remove(fragPath.c_str());
        if (rc == 0) {
            std::ifstream ifs(spvPath, std::ios::binary | std::ios::ate);
            if (ifs) {
                const auto sz = ifs.tellg();
                if (sz > 0 && (sz % 4) == 0) {
                    std::vector<uint32_t> spv(static_cast<size_t>(sz) / 4);
                    ifs.seekg(0);
                    ifs.read(reinterpret_cast<char*>(spv.data()), sz);
                    VkShaderModuleCreateInfo smci{};
                    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    smci.codeSize = static_cast<size_t>(sz);
                    smci.pCode = spv.data();
                    if (vkCreateShaderModule(device, &smci, nullptr, &entry.fragMod) == VK_SUCCESS) {
                        VkPipelineShaderStageCreateInfo stages[2]{};
                        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = filterVertMod; stages[0].pName = "main";
                        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = entry.fragMod; stages[1].pName = "main";
                        VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                        VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
                        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
                        VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                        VkPipelineColorBlendAttachmentState cbAtt{}; cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                        VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cbAtt;
                        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                        VkPipelineDynamicStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
                        VkGraphicsPipelineCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                        pi.stageCount = 2; pi.pStages = stages; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia;
                        pi.pViewportState = &vp; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
                        pi.pColorBlendState = &cb; pi.pDynamicState = &ds; pi.layout = filterOpsPipeLayout;
                        pi.renderPass = filterRenderPassClear; pi.subpass = 0;
                        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &entry.pipeline);
                    }
                }
            }
        }
        std::remove(spvPath.c_str());
        auto [insIt, _] = runtimeFilterCache.emplace(key, entry);
        return insIt->second.pipeline;
    }

    void runVkFilterRuntimeBlenderPass(VkCommandBuffer cmd, int dstPoolIdx, int srcPoolIdx, int bgPoolIdx,
                                       const effects::RuntimeFilterSpec& spec,
                                       float opacity, int w, int h) {
        if (filterColorOffset + filterColorStride > filterColorBufCap) return;
        VkPipeline pipe = getOrCreateVkRuntimeBlenderPipe(spec);
        if (pipe == VK_NULL_HANDLE) return;
        VkDescriptorSet set = getOrCreateOpsDescSet(srcPoolIdx, bgPoolIdx);
        if (set == VK_NULL_HANDLE) return;

        alignas(16) uint8_t uboBytes[kFilterUboMaxBytes]{};
        float* hdr = reinterpret_cast<float*>(uboBytes);
        hdr[0] = opacity;
        packRuntimeUniformsStd140(spec, uboBytes, kFilterUboMaxBytes, 16);

        void* mapped = nullptr;
        vkMapMemory(device, filterColorMem, filterColorOffset, kFilterUboMaxBytes, 0, &mapped);
        std::memcpy(mapped, uboBytes, kFilterUboMaxBytes);
        vkUnmapMemory(device, filterColorMem);

        const uint32_t dynOff = static_cast<uint32_t>(filterColorOffset);
        filterColorOffset += filterColorStride;

        VkClearValue cv{};
        cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = filterRenderPassClear;
        rpb.framebuffer = layerPool[dstPoolIdx].filterFb;
        rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        rpb.clearValueCount = 1;
        rpb.pClearValues = &cv;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sci{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterOpsPipeLayout,
                                0, 1, &set, 1, &dynOff);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    void computeVkCropUv(const std::optional<nisaba::Rect>& cropRect,
                         const Transform2D& layerXform,
                         int w, int h,
                         float outUv[4]) const noexcept {
        if (!cropRect.has_value() || w <= 0 || h <= 0 || view[0] <= 0.0f || view[1] <= 0.0f) {
            outUv[0] = -1.0f; outUv[1] = -1.0f; outUv[2] = 2.0f; outUv[3] = 2.0f;
            return;
        }
        const nisaba::Transform ts = static_cast<nisaba::Transform>(layerXform);
        auto devCrop = cropRect->transform(ts);
        if (!devCrop.has_value()) {
            outUv[0] = 2.0f; outUv[1] = 2.0f; outUv[2] = -1.0f; outUv[3] = -1.0f;
            return;
        }
        const float sx = static_cast<float>(w) / view[0];
        const float sy = static_cast<float>(h) / view[1];
        outUv[0] = std::floor(devCrop->left() * sx) / static_cast<float>(w);
        outUv[1] = std::floor(devCrop->top() * sy) / static_cast<float>(h);
        outUv[2] = std::ceil(devCrop->right() * sx) / static_cast<float>(w);
        outUv[3] = std::ceil(devCrop->bottom() * sy) / static_cast<float>(h);
    }

    int evalImageFilterDagVk(VkCommandBuffer cmd,
                             const effects::ImageFilterNode* node,
                             int currentSrcPoolIdx,
                             int w, int h,
                             const LayerDesc& desc,
                             bool hasRoi,
                             int roiX0, int roiY0, int roiX1, int roiY1) {
        if (!node || node->kind == effects::ImageFilterKind::None) {
            return currentSrcPoolIdx;
        }

        const float invW = 1.0f / static_cast<float>(std::max(1, w));
        const float invH = 1.0f / static_cast<float>(std::max(1, h));
        const float roiUv[4] = {
            hasRoi ? (static_cast<float>(roiX0) * invW) : 0.0f,
            hasRoi ? (static_cast<float>(roiY0) * invH) : 0.0f,
            hasRoi ? (static_cast<float>(roiX1) * invW) : 1.0f,
            hasRoi ? (static_cast<float>(roiY1) * invH) : 1.0f
        };
        const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
        float cropUv[4];
        computeVkCropUv(node->crop_rect, desc.layerTransform, w, h, cropUv);

        const float sxDev = (view[0] > 0.0f) ? (static_cast<float>(w) / view[0]) : 1.0f;
        const float syDev = (view[1] > 0.0f) ? (static_cast<float>(h) / view[1]) : 1.0f;
        const auto& m = desc.layerTransform.m;
        const float scaleX = std::hypot(m[0], m[1]) * sxDev;
        const float scaleY = std::hypot(m[2], m[3]) * syDev;

        auto releaseTemp = [&](int idx) {
            if (idx >= 0 && idx != currentSrcPoolIdx) {
                layerPool[idx].inUse = false;
            }
        };

        auto evalChild = [&](size_t idx) -> int {
            if (idx < node->inputs.size() && node->inputs[idx]) {
                return evalImageFilterDagVk(cmd, node->inputs[idx].get(), currentSrcPoolIdx,
                                            w, h, desc, hasRoi, roiX0, roiY0, roiX1, roiY1);
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
                int blurIdx = inIdx;
                if (effSx > 0.001f || effSy > 0.001f) {
                    blurIdx = runVkGaussianBlur(cmd, inIdx, w, h, effSx, effSy, hasRoi, roiX0, roiY0, roiX1, roiY1);
                }
                if (node->crop_rect.has_value() || blurIdx == inIdx) {
                    const int outIdx = acquireVkLayerTarget(w, h);
                    if (outIdx >= 0) {
                        runVkFilterBlendPass(cmd, outIdx, blurIdx, blurIdx, 0, 0.0f, 0.0f,
                                             Color(1, 1, 1, 1), roiUv, cropUv, w, h, false);
                        if (blurIdx != inIdx) releaseTemp(blurIdx);
                        releaseTemp(inIdx);
                        return outIdx;
                    }
                }
                if (blurIdx != inIdx) releaseTemp(inIdx);
                return blurIdx;
            }

            case effects::ImageFilterKind::DropShadow: {
                const int fgIdx = evalChild(0);
                const int tintIdx = acquireVkLayerTarget(w, h);
                if (tintIdx < 0) return fgIdx;
                const Color sc = Color(node->color).premultiplied();
                runVkFilterBlendPass(cmd, tintIdx, fgIdx, fgIdx, -1, 0.0f, 0.0f, sc,
                                     roiUv, noCropUv, w, h, false);

                const float effSx = node->sigma_x * scaleX;
                const float effSy = node->sigma_y * scaleY;
                int shIdx = tintIdx;
                if (effSx > 0.001f || effSy > 0.001f) {
                    shIdx = runVkGaussianBlur(cmd, tintIdx, w, h, effSx, effSy, hasRoi, roiX0, roiY0, roiX1, roiY1);
                }

                const float effDx = (node->dx * m[0] + node->dy * m[2]) * sxDev;
                const float effDy = (node->dx * m[1] + node->dy * m[3]) * syDev;
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    runVkFilterBlendPass(cmd, outIdx, shIdx, shIdx, 0, -effDx * invW, -effDy * invH,
                                         Color(1, 1, 1, 1), roiUv, cropUv, w, h, false);
                    if (!node->shadow_only) {
                        runVkFilterBlendPass(cmd, outIdx, fgIdx, fgIdx, 0, 0.0f, 0.0f,
                                             Color(1, 1, 1, 1), roiUv, cropUv, w, h, true);
                    }
                }
                if (shIdx != tintIdx) releaseTemp(shIdx);
                releaseTemp(tintIdx);
                releaseTemp(fgIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::Offset: {
                const int inIdx = evalChild(0);
                const float effDx = (node->dx * m[0] + node->dy * m[2]) * sxDev;
                const float effDy = (node->dx * m[1] + node->dy * m[3]) * syDev;
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    runVkFilterBlendPass(cmd, outIdx, inIdx, inIdx, 0, -effDx * invW, -effDy * invH,
                                         Color(1, 1, 1, 1), roiUv, cropUv, w, h, false);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::ColorFilter: {
                const int inIdx = evalChild(0);
                const int outIdx = evalColorEffectDagVk(cmd, node->color_effect, inIdx, w, h,
                                                        roiUv, cropUv, hasRoi, roiX0, roiY0, roiX1, roiY1);
                if (outIdx != inIdx) releaseTemp(inIdx);
                return outIdx;
            }

            case effects::ImageFilterKind::Compose: {
                const int innerIdx = (node->inputs.size() >= 2 && node->inputs[1])
                    ? evalImageFilterDagVk(cmd, node->inputs[1].get(), currentSrcPoolIdx,
                                           w, h, desc, hasRoi, roiX0, roiY0, roiX1, roiY1)
                    : currentSrcPoolIdx;
                int outIdx = (node->inputs.size() >= 1 && node->inputs[0])
                    ? evalImageFilterDagVk(cmd, node->inputs[0].get(), innerIdx,
                                           w, h, desc, hasRoi, roiX0, roiY0, roiX1, roiY1)
                    : innerIdx;
                if (innerIdx != outIdx) {
                    releaseTemp(innerIdx);
                }
                if (node->crop_rect.has_value()) {
                    const int croppedIdx = acquireVkLayerTarget(w, h);
                    if (croppedIdx >= 0) {
                        runVkFilterBlendPass(cmd, croppedIdx, outIdx, outIdx, 0, 0.0f, 0.0f,
                                             Color(1, 1, 1, 1), roiUv, cropUv, w, h, false);
                        releaseTemp(outIdx);
                        outIdx = croppedIdx;
                    }
                }
                return outIdx;
            }

            case effects::ImageFilterKind::Merge: {
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx < 0) return currentSrcPoolIdx;
                bool first = true;
                for (size_t i = 0; i < node->inputs.size(); ++i) {
                    const int childIdx = evalChild(i);
                    runVkFilterBlendPass(cmd, outIdx, childIdx, childIdx, 0, 0.0f, 0.0f,
                                         Color(1, 1, 1, 1), roiUv, cropUv, w, h, !first);
                    first = false;
                    releaseTemp(childIdx);
                }
                if (first) {
                    // Empty merge -> transparent target
                    VkClearValue cv{};
                    cv.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
                    VkRenderPassBeginInfo rpb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
                                              filterRenderPassClear, layerPool[outIdx].filterFb,
                                              {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}}, 1, &cv};
                    vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdEndRenderPass(cmd);
                }
                return outIdx;
            }

            case effects::ImageFilterKind::Blend: {
                const int bgIdx = evalChild(0);
                const int fgIdx = evalChild(1);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    runVkFilterBlendPass(cmd, outIdx, fgIdx, bgIdx,
                                         static_cast<int>(node->blend_mode) + 1,
                                         0.0f, 0.0f, Color(1, 1, 1, 1),
                                         roiUv, cropUv, w, h, false);
                }
                if (fgIdx != bgIdx) releaseTemp(fgIdx);
                releaseTemp(bgIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::RuntimeShader: {
                const int inIdx = evalChild(0);
                if (!node->runtime_spec) return inIdx;
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    runVkFilterRuntimePass(cmd, outIdx, inIdx, *node->runtime_spec,
                                           desc.layerTransform, roiUv, cropUv, w, h);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::Dilate:
            case effects::ImageFilterKind::Erode: {
                const int inIdx = evalChild(0);
                const int op = (node->kind == effects::ImageFilterKind::Dilate) ? 1 : 2;
                const int rx = std::clamp(static_cast<int>(std::round(node->sigma_x * scaleX)), 0, 64);
                const int ry = std::clamp(static_cast<int>(std::round(node->sigma_y * scaleY)), 0, 64);
                VkFilterOpsUbo u{};
                u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                u.fbSize[2] = invW; u.fbSize[3] = invH;
                const int tmpIdx = acquireVkLayerTarget(w, h);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (tmpIdx >= 0 && outIdx >= 0) {
                    u.i0[0] = op; u.i0[1] = rx;
                    u.p0[0] = 1.0f; u.p0[1] = 0.0f;
                    runVkFilterOpsPass(cmd, tmpIdx, inIdx, inIdx, u, w, h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.i0[1] = ry;
                    u.p0[0] = 0.0f; u.p0[1] = 1.0f;
                    runVkFilterOpsPass(cmd, outIdx, tmpIdx, tmpIdx, u, w, h);
                }
                if (tmpIdx >= 0) releaseTemp(tmpIdx);
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::MatrixConvolution: {
                const int inIdx = evalChild(0);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 3;
                    u.i0[1] = std::clamp(node->kernel_w, 1, 5);
                    u.i0[2] = std::clamp(node->kernel_h, 1, 5);
                    u.i0[3] = static_cast<int32_t>(node->tile_mode);
                    u.p0[0] = static_cast<float>(node->kernel_offset_x);
                    u.p0[1] = static_cast<float>(node->kernel_offset_y);
                    u.p0[2] = node->gain;
                    u.p0[3] = node->bias;
                    u.p1[0] = node->convolve_alpha ? 1.0f : 0.0f;
                    const size_t kn = std::min<size_t>(node->kernel.size(), 25);
                    for (size_t k = 0; k < kn; ++k) u.kernel[k] = node->kernel[k];
                    runVkFilterOpsPass(cmd, outIdx, inIdx, inIdx, u, w, h);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::DisplacementMap: {
                const int dispIdx = evalChild(0);
                const int colIdx = evalChild(1);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 4;
                    u.i0[1] = static_cast<int32_t>(node->x_channel);
                    u.i0[2] = static_cast<int32_t>(node->y_channel);
                    u.p0[0] = node->disp_scale * scaleX;
                    u.p0[1] = node->disp_scale * scaleY;
                    runVkFilterOpsPass(cmd, outIdx, colIdx, dispIdx, u, w, h);
                }
                if (colIdx != dispIdx) releaseTemp(colIdx);
                releaseTemp(dispIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::Magnifier: {
                const int inIdx = evalChild(0);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 5;
                    const nisaba::Transform ts = static_cast<nisaba::Transform>(desc.layerTransform);
                    if (auto dr = node->src_rect.transform(ts)) {
                        u.p0[0] = dr->left() * sxDev; u.p0[1] = dr->top() * syDev;
                        u.p0[2] = dr->right() * sxDev; u.p0[3] = dr->bottom() * syDev;
                    }
                    u.p1[0] = node->magnifier_inset * scaleX;
                    u.p1[1] = node->magnifier_inset * scaleY;
                    runVkFilterOpsPass(cmd, outIdx, inIdx, inIdx, u, w, h);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::Arithmetic: {
                const int bgIdx = evalChild(0);
                const int fgIdx = evalChild(1);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 6;
                    u.i0[1] = node->enforce_premul ? 1 : 0;
                    u.p0[0] = node->k1; u.p0[1] = node->k2; u.p0[2] = node->k3; u.p0[3] = node->k4;
                    runVkFilterOpsPass(cmd, outIdx, fgIdx, bgIdx, u, w, h);
                }
                if (fgIdx != bgIdx) releaseTemp(fgIdx);
                releaseTemp(bgIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::MatrixTransform: {
                const int inIdx = evalChild(0);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 7;
                    u.i0[1] = static_cast<int32_t>(node->filter_quality);
                    const nisaba::Transform devScale = nisaba::Transform::from_scale(sxDev, syDev);
                    const nisaba::Transform fullTs = devScale.pre_concat(static_cast<nisaba::Transform>(desc.layerTransform));
                    if (auto invF = node->filter_transform.invert()) {
                        if (auto invFull = fullTs.invert()) {
                            const nisaba::Transform devInv = fullTs.pre_concat(*invF).pre_concat(*invFull);
                            u.p0[0] = devInv.sx; u.p0[1] = devInv.ky; u.p0[2] = devInv.kx; u.p0[3] = devInv.sy;
                            u.p1[0] = devInv.tx; u.p1[1] = devInv.ty;
                            runVkFilterOpsPass(cmd, outIdx, inIdx, inIdx, u, w, h);
                        }
                    }
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::Tile: {
                const int inIdx = evalChild(0);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 8;
                    const nisaba::Transform ts = static_cast<nisaba::Transform>(desc.layerTransform);
                    if (auto ds = node->src_rect.transform(ts)) {
                        u.p0[0] = ds->left() * sxDev; u.p0[1] = ds->top() * syDev;
                        u.p0[2] = ds->width() * sxDev; u.p0[3] = ds->height() * syDev;
                    }
                    if (auto dd = node->dst_rect.transform(ts)) {
                        u.p1[0] = dd->left() * sxDev; u.p1[1] = dd->top() * syDev;
                        u.p1[2] = dd->right() * sxDev; u.p1[3] = dd->bottom() * syDev;
                    }
                    runVkFilterOpsPass(cmd, outIdx, inIdx, inIdx, u, w, h);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::AlphaThreshold: {
                const int inIdx = evalChild(0);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 9;
                    const nisaba::Transform ts = static_cast<nisaba::Transform>(desc.layerTransform);
                    if (auto dr = node->src_rect.transform(ts)) {
                        u.p0[0] = dr->left() * sxDev; u.p0[1] = dr->top() * syDev;
                        u.p0[2] = dr->right() * sxDev; u.p0[3] = dr->bottom() * syDev;
                    } else {
                        u.p0[0] = 1e6f; u.p0[1] = 1e6f; u.p0[2] = -1e6f; u.p0[3] = -1e6f;
                    }
                    u.p1[0] = node->inner_min;
                    u.p1[1] = node->outer_max;
                    runVkFilterOpsPass(cmd, outIdx, inIdx, inIdx, u, w, h);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }

            case effects::ImageFilterKind::Lighting: {
                const int inIdx = evalChild(0);
                const int outIdx = acquireVkLayerTarget(w, h);
                if (outIdx >= 0) {
                    VkFilterOpsUbo u{};
                    u.roiDev[0] = hasRoi ? static_cast<float>(roiX0) : 0.0f;
                    u.roiDev[1] = hasRoi ? static_cast<float>(roiY0) : 0.0f;
                    u.roiDev[2] = hasRoi ? static_cast<float>(roiX1) : static_cast<float>(w);
                    u.roiDev[3] = hasRoi ? static_cast<float>(roiY1) : static_cast<float>(h);
                    computeVkCropDev(node->crop_rect, desc.layerTransform, w, h, u.cropDev);
                    u.fbSize[0] = static_cast<float>(w); u.fbSize[1] = static_cast<float>(h);
                    u.fbSize[2] = invW; u.fbSize[3] = invH;
                    u.i0[0] = 10;
                    u.i0[1] = static_cast<int32_t>(node->lighting_kind);
                    u.p0[0] = node->light_pos.x; u.p0[1] = node->light_pos.y; u.p0[2] = node->light_pos.z; u.p0[3] = node->surface_scale;
                    u.p1[0] = node->light_target.x; u.p1[1] = node->light_target.y; u.p1[2] = node->light_target.z; u.p1[3] = node->light_kd_ks;
                    u.p2[0] = node->color.red(); u.p2[1] = node->color.green(); u.p2[2] = node->color.blue(); u.p2[3] = node->shininess;
                    u.p3[0] = node->specular_exponent;
                    u.p3[1] = std::cos(node->cutoff_angle * 3.14159265358979323846f / 180.0f);
                    u.p3[2] = node->cutoff_angle;
                    const nisaba::Transform devScale = nisaba::Transform::from_scale(sxDev, syDev);
                    const nisaba::Transform fullTs = devScale.pre_concat(static_cast<nisaba::Transform>(desc.layerTransform));
                    const nisaba::Transform invFull = fullTs.invert().value_or(nisaba::Transform());
                    u.p4[0] = invFull.sx; u.p4[1] = invFull.ky; u.p4[2] = invFull.kx; u.p4[3] = invFull.sy;
                    u.kernel[0] = invFull.tx; u.kernel[1] = invFull.ty;
                    runVkFilterOpsPass(cmd, outIdx, inIdx, inIdx, u, w, h);
                }
                releaseTemp(inIdx);
                return (outIdx >= 0) ? outIdx : currentSrcPoolIdx;
            }
        }
        return currentSrcPoolIdx;
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

    VkShaderModuleCreateInfo perlinModInfo{};
    perlinModInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    perlinModInfo.codeSize = vulkan::NISABA_PERLIN_FRAG_SPV_SIZE;
    perlinModInfo.pCode = vulkan::NISABA_PERLIN_FRAG_SPV;
    vkCreateShaderModule(dev, &perlinModInfo, nullptr, &m_impl->perlinFragShaderModule);

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
    m_impl->destroyFilterResources();

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
    if (m_impl->perlinFragShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_impl->perlinFragShaderModule, nullptr);
        m_impl->perlinFragShaderModule = VK_NULL_HANDLE;
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
            call.isPerlinNoise = (tempFrag.type == ShaderMultiGrad && (tempFrag.gradKind == 5 || tempFrag.gradKind == 6));
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
    call.isPerlinNoise = (tempFrag.type == ShaderMultiGrad && (tempFrag.gradKind == 5 || tempFrag.gradKind == 6));
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
    call.isPerlinNoise = (tempFrag.type == ShaderMultiGrad && (tempFrag.gradKind == 5 || tempFrag.gradKind == 6));
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
    call.isPerlinNoise = (tempFrag.type == ShaderMultiGrad && (tempFrag.gradKind == 5 || tempFrag.gradKind == 6));
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

void VulkanRenderer::pushLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) {
    (void)parentScissor;
    if (m_impl->fragSize == 0) return;

    Call call;
    call.type = CallType::PushLayer;
    call.clipDepth = parentClipDepth;
    call.layerIndex = static_cast<int>(m_impl->layerDescs.size());
    m_impl->layerDescs.push_back(desc);
    call.scissorRect[0] = -1;
    m_impl->calls.push_back(call);
    m_impl->currentClipDepth = 0;
}

void VulkanRenderer::popLayer(const LayerDesc& desc, const Scissor& parentScissor, int parentClipDepth) {
    if (m_impl->fragSize == 0) return;

    int hwScissor[4]{-1, -1, -1, -1};
    const bool hasHwScissor = m_impl->getHardwareScissor(parentScissor, hwScissor);

    Call call;
    call.type = CallType::PopLayer;
    call.layerIndex = static_cast<int>(m_impl->layerDescs.size());
    m_impl->layerDescs.push_back(desc);
    const bool hasCustomBlender = desc.blender.has_value() && desc.blender->is_custom();
    call.blend = hasCustomBlender
        ? VkBlendState{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO}
        : convertVkBlend(desc.compOp);
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

    if (maxX > minX && maxY > minY && m_impl->view[0] > 0.0f && m_impl->view[1] > 0.0f) {
        const float invW = 1.0f / m_impl->view[0];
        const float invH = 1.0f / m_impl->view[1];
        const float u0 = minX * invW;
        const float u1 = maxX * invW;
        const float v0 = minY * invH;
        const float v1 = maxY * invH;

        const int offset = static_cast<int>(m_impl->verts.size());
        m_impl->verts.resize(offset + 6);
        Vertex* q = &m_impl->verts[offset];
        q[0] = Vertex(minX, minY, u0, v0, 0xFFFFFFFFu);
        q[1] = Vertex(maxX, minY, u1, v0, 0xFFFFFFFFu);
        q[2] = Vertex(maxX, maxY, u1, v1, 0xFFFFFFFFu);
        q[3] = Vertex(minX, minY, u0, v0, 0xFFFFFFFFu);
        q[4] = Vertex(maxX, maxY, u1, v1, 0xFFFFFFFFu);
        q[5] = Vertex(minX, maxY, u0, v1, 0xFFFFFFFFu);
        call.triangleOffset = offset;
        call.triangleCount = 6;

        Paint popPaint;
        popPaint.innerColor = Color(1.0f, 1.0f, 1.0f, hasCustomBlender ? 1.0f : std::clamp(desc.opacity, 0.0f, 1.0f));
        popPaint.outerColor = popPaint.innerColor;
        popPaint.hasColorMatrix = desc.hasColorMatrix;
        popPaint.colorMatrix = desc.colorMatrix;
        popPaint.colorTransfer = desc.colorTransfer;
        popPaint.hasColorBlend = desc.hasColorBlend;
        popPaint.colorBlendColor = desc.colorBlendColor;
        popPaint.colorBlendMode = desc.colorBlendMode;

        FragUniforms frag{};
        m_impl->convertPaint(&frag, popPaint, parentScissor, 1.0f, 1.0f, -1.0f, hasHwScissor);
        frag.innerCol = popPaint.innerColor.premultiplied();
        frag.outerCol = frag.innerCol;
        frag.type = ShaderImg;
        frag.texType = 3;
        call.uniformOffset = m_impl->storeUniformOrReuse(frag);
    }

    m_impl->calls.push_back(call);
    m_impl->currentClipDepth = parentClipDepth;
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
    m_impl->filterColorOffset = 0;

    auto rebindDrawState = [&]() {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdPushConstants(cmd, m_impl->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2, m_impl->view);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_impl->vertBuf, &offset);
    };

    for (const auto& call : m_impl->calls) {
        if (call.type == CallType::PushLayer) {
            if (m_impl->targetSurface == nullptr || call.layerIndex < 0 ||
                call.layerIndex >= static_cast<int>(m_impl->layerDescs.size())) {
                continue;
            }
            const LayerDesc& layer = m_impl->layerDescs[call.layerIndex];
            const int fbW = static_cast<int>(m_impl->targetSurface->width());
            const int fbH = static_cast<int>(m_impl->targetSurface->height());
            if (fbW <= 0 || fbH <= 0) continue;

            const bool parentIsNested = !m_impl->activeLayers.empty();
            VkImage parentColorImg = parentIsNested
                ? m_impl->layerPool[m_impl->activeLayers.back().poolIndex].colorImage
                : m_impl->targetSurface->color_image();
            VkImageLayout parentColorLayout = parentIsNested
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            // End parent render pass before offscreen layer / backdrop ops
            vkCmdEndRenderPass(cmd);

            int poolIdx = m_impl->acquireVkLayerTarget(fbW, fbH);
            if (poolIdx < 0) {
                // Resume parent pass if allocation failed
                VkRenderPassBeginInfo rpb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
                    parentIsNested ? m_impl->offscreenRenderPassLoad : m_impl->targetSurface->render_pass_load(),
                    parentIsNested ? m_impl->layerPool[m_impl->activeLayers.back().poolIndex].layerFb : m_impl->targetSurface->framebuffer(),
                    {{0, 0}, {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH)}}, 0, nullptr};
                vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
                rebindDrawState();
                continue;
            }

            bool loadedBackdrop = false;
            if (layer.copyBackdrop && parentColorImg != VK_NULL_HANDLE) {
                int copyTargetIdx = (layer.backdropBlurSigma > 0.001f)
                    ? m_impl->acquireVkLayerTarget(fbW, fbH)
                    : poolIdx;
                if (copyTargetIdx >= 0) {
                    VkImage dstImg = m_impl->layerPool[copyTargetIdx].colorImage;
                    std::array<VkImageMemoryBarrier, 2> pre{};
                    pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    pre[0].oldLayout = parentColorLayout;
                    pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    pre[0].image = parentColorImg;
                    pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    pre[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                    pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                    pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    pre[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    pre[1].image = dstImg;
                    pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    pre[1].srcAccessMask = 0;
                    pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre.data());

                    VkImageCopy copyRegion{};
                    copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copyRegion.extent = {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH), 1};
                    vkCmdCopyImage(cmd, parentColorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

                    std::array<VkImageMemoryBarrier, 2> post{};
                    post[0] = pre[0];
                    post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    post[0].newLayout = parentColorLayout;
                    post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    post[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

                    post[1] = pre[1];
                    post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    post[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        0, 0, nullptr, 0, nullptr, 2, post.data());

                    if (layer.backdropBlurSigma > 0.001f) {
                        const int blurredIdx = m_impl->runVkGaussianBlur(
                            cmd, copyTargetIdx, fbW, fbH,
                            layer.backdropBlurSigma, layer.backdropBlurSigma,
                            false, 0, 0, fbW, fbH);
                        m_impl->layerPool[copyTargetIdx].inUse = false;
                        if (blurredIdx != poolIdx && blurredIdx >= 0) {
                            m_impl->layerPool[poolIdx].inUse = false;
                            poolIdx = blurredIdx;
                        }
                    }
                    // Transition dsImage to DEPTH_STENCIL_ATTACHMENT_OPTIMAL before load pass
                    VkImageMemoryBarrier dsB{};
                    dsB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    dsB.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    dsB.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    dsB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    dsB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    dsB.image = m_impl->layerPool[poolIdx].dsImage;
                    dsB.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
                    dsB.srcAccessMask = 0;
                    dsB.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &dsB);
                    loadedBackdrop = true;
                }
            }

            std::array<VkClearValue, 2> cvs{};
            cvs[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
            cvs[1].depthStencil = {1.0f, 0};
            VkRenderPassBeginInfo rpb{};
            rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpb.renderPass = loadedBackdrop ? m_impl->offscreenRenderPassLoad : m_impl->offscreenRenderPassClear;
            rpb.framebuffer = m_impl->layerPool[poolIdx].layerFb;
            rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH)}};
            rpb.clearValueCount = 2;
            rpb.pClearValues = cvs.data();
            vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            if (loadedBackdrop) {
                VkClearAttachment ca{VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
                ca.clearValue.depthStencil = {1.0f, 0};
                VkClearRect cr{{{0, 0}, {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH)}}, 0, 1};
                vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            }
            rebindDrawState();

            m_impl->activeLayers.push_back(Impl::VkActiveLayer{poolIdx, activeClipDepth});
            activeClipDepth = 0;
            continue;
        }

        if (call.type == CallType::PopLayer) {
            if (m_impl->activeLayers.empty() || m_impl->targetSurface == nullptr) continue;
            const auto rl = m_impl->activeLayers.back();
            m_impl->activeLayers.pop_back();
            const int fbW = m_impl->layerPool[rl.poolIndex].w;
            const int fbH = m_impl->layerPool[rl.poolIndex].h;

            // End offscreen layer render pass -> colorImage is now SHADER_READ_ONLY_OPTIMAL
            vkCmdEndRenderPass(cmd);

            int finalPoolIdx = rl.poolIndex;
            if (call.layerIndex >= 0 && call.layerIndex < static_cast<int>(m_impl->layerDescs.size())) {
                const LayerDesc& desc = m_impl->layerDescs[call.layerIndex];
                const bool doMaskBlur = desc.maskFilter.has_value() && desc.maskFilter->is_blur();
                const bool doImgFilter = desc.imageFilter.has_value() && desc.imageFilter->is_active();
                const bool doExtColor = desc.extColorEffect.has_value() && desc.extColorEffect->is_active();
                const bool doCustomBlender = desc.blender.has_value() && desc.blender->is_custom();
                if (doMaskBlur || doImgFilter || doExtColor || doCustomBlender) {
                    int roiX0 = 0, roiY0 = 0, roiX1 = fbW, roiY1 = fbH;
                    const bool hasRoi = desc.hasBounds && m_impl->view[0] > 0.0f && m_impl->view[1] > 0.0f;
                    if (hasRoi) {
                        const float sx = static_cast<float>(fbW) / m_impl->view[0];
                        const float sy = static_cast<float>(fbH) / m_impl->view[1];
                        roiX0 = std::clamp(static_cast<int>(std::floor(desc.bounds[0] * sx)), 0, fbW);
                        roiY0 = std::clamp(static_cast<int>(std::floor(desc.bounds[1] * sy)), 0, fbH);
                        roiX1 = std::clamp(static_cast<int>(std::ceil(desc.bounds[2] * sx)), 0, fbW);
                        roiY1 = std::clamp(static_cast<int>(std::ceil(desc.bounds[3] * sy)), 0, fbH);
                    }
                    if (doMaskBlur) {
                        const auto& mf = *desc.maskFilter;
                        const float sxDev = (m_impl->view[0] > 0.0f) ? (static_cast<float>(fbW) / m_impl->view[0]) : 1.0f;
                        const float syDev = (m_impl->view[1] > 0.0f) ? (static_cast<float>(fbH) / m_impl->view[1]) : 1.0f;
                        float effSx = mf.sigma * sxDev;
                        float effSy = mf.sigma * syDev;
                        if (mf.respect_ctm) {
                            const auto& m = desc.layerTransform.m;
                            effSx *= std::hypot(m[0], m[1]);
                            effSy *= std::hypot(m[2], m[3]);
                        }
                        int blurIdx = finalPoolIdx;
                        if (effSx > 0.001f || effSy > 0.001f) {
                            blurIdx = m_impl->runVkGaussianBlur(
                                cmd, finalPoolIdx, fbW, fbH, effSx, effSy,
                                hasRoi, roiX0, roiY0, roiX1, roiY1);
                        }
                        const int maskOutIdx = m_impl->acquireVkLayerTarget(fbW, fbH);
                        if (maskOutIdx >= 0) {
                            const float invW = 1.0f / static_cast<float>(std::max(1, fbW));
                            const float invH = 1.0f / static_cast<float>(std::max(1, fbH));
                            const float roiUv[4] = {
                                hasRoi ? (static_cast<float>(roiX0) * invW) : 0.0f,
                                hasRoi ? (static_cast<float>(roiY0) * invH) : 0.0f,
                                hasRoi ? (static_cast<float>(roiX1) * invW) : 1.0f,
                                hasRoi ? (static_cast<float>(roiY1) * invH) : 1.0f
                            };
                            const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
                            const int maskMode = -2 - static_cast<int>(mf.style);
                            m_impl->runVkFilterBlendPass(
                                cmd, maskOutIdx, blurIdx, finalPoolIdx,
                                maskMode, 0.0f, 0.0f, desc.maskTintColor.premultiplied(),
                                roiUv, noCropUv, fbW, fbH, false);
                            if (blurIdx != finalPoolIdx) m_impl->layerPool[blurIdx].inUse = false;
                            finalPoolIdx = maskOutIdx;
                        } else if (blurIdx != finalPoolIdx) {
                            m_impl->layerPool[blurIdx].inUse = false;
                        }
                    }
                    if (doImgFilter) {
                        const int nextIdx = m_impl->evalImageFilterDagVk(
                            cmd, desc.imageFilter->node().get(), finalPoolIdx, fbW, fbH,
                            desc, hasRoi, roiX0, roiY0, roiX1, roiY1);
                        if (finalPoolIdx != rl.poolIndex && finalPoolIdx != nextIdx) {
                            m_impl->layerPool[finalPoolIdx].inUse = false;
                        }
                        finalPoolIdx = nextIdx;
                    }
                    if (doExtColor) {
                        const float invW = 1.0f / static_cast<float>(std::max(1, fbW));
                        const float invH = 1.0f / static_cast<float>(std::max(1, fbH));
                        const float roiUv[4] = {
                            hasRoi ? (static_cast<float>(roiX0) * invW) : 0.0f,
                            hasRoi ? (static_cast<float>(roiY0) * invH) : 0.0f,
                            hasRoi ? (static_cast<float>(roiX1) * invW) : 1.0f,
                            hasRoi ? (static_cast<float>(roiY1) * invH) : 1.0f
                        };
                        const float noCropUv[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
                        const int nextIdx = m_impl->evalColorEffectDagVk(
                            cmd, *desc.extColorEffect, finalPoolIdx, fbW, fbH,
                            roiUv, noCropUv, hasRoi, roiX0, roiY0, roiX1, roiY1);
                        if (finalPoolIdx != rl.poolIndex && finalPoolIdx != nextIdx) {
                            m_impl->layerPool[finalPoolIdx].inUse = false;
                        }
                        finalPoolIdx = nextIdx;
                    }
                    if (doCustomBlender) {
                        const int dstSnapIdx = m_impl->acquireVkLayerTarget(fbW, fbH);
                        const int blendOutIdx = m_impl->acquireVkLayerTarget(fbW, fbH);
                        if (dstSnapIdx >= 0 && blendOutIdx >= 0) {
                            const bool parentIsNested = !m_impl->activeLayers.empty();
                            VkImage parentImg = parentIsNested
                                ? m_impl->layerPool[m_impl->activeLayers.back().poolIndex].colorImage
                                : m_impl->targetSurface->color_image();
                            const VkImageLayout parentOldLayout = parentIsNested
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                            const bool parentMsaa = !parentIsNested && (m_impl->targetSurface->samples() > 1);

                            VkImageMemoryBarrier pre[2]{};
                            pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            pre[0].image = parentImg;
                            pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            pre[0].oldLayout = parentOldLayout;
                            pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            pre[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                            pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                            pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            pre[1].image = m_impl->layerPool[dstSnapIdx].colorImage;
                            pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                            pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            pre[1].srcAccessMask = 0;
                            pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                            vkCmdPipelineBarrier(cmd,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);

                            if (parentMsaa) {
                                VkImageResolve res{};
                                res.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                res.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                res.extent = {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH), 1};
                                vkCmdResolveImage(cmd, parentImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                  m_impl->layerPool[dstSnapIdx].colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
                            } else {
                                VkImageCopy cp{};
                                cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                cp.extent = {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH), 1};
                                vkCmdCopyImage(cmd, parentImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               m_impl->layerPool[dstSnapIdx].colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
                            }

                            VkImageMemoryBarrier post[2]{};
                            post[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            post[0].image = parentImg;
                            post[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            post[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                            post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            post[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

                            post[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            post[1].image = m_impl->layerPool[dstSnapIdx].colorImage;
                            post[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                            post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            post[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                            vkCmdPipelineBarrier(cmd,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                0, 0, nullptr, 0, nullptr, 2, post);

                            const auto& b = *desc.blender;
                            const float op = std::clamp(desc.opacity, 0.0f, 1.0f);
                            if (b.kind == effects::BlenderKind::Arithmetic) {
                                VkFilterOpsUbo u{};
                                u.roiDev[0] = 0.0f; u.roiDev[1] = 0.0f;
                                u.roiDev[2] = static_cast<float>(fbW); u.roiDev[3] = static_cast<float>(fbH);
                                u.fbSize[0] = static_cast<float>(fbW); u.fbSize[1] = static_cast<float>(fbH);
                                u.fbSize[2] = 1.0f / static_cast<float>(std::max(1, fbW));
                                u.fbSize[3] = 1.0f / static_cast<float>(std::max(1, fbH));
                                u.i0[0] = 6; u.i0[1] = b.enforce_premul ? 1 : 0;
                                u.p0[0] = b.k1 * op; u.p0[1] = b.k2 * op; u.p0[2] = b.k3; u.p0[3] = b.k4;
                                m_impl->runVkFilterOpsPass(cmd, blendOutIdx, finalPoolIdx, dstSnapIdx, u, fbW, fbH);
                            } else if (b.kind == effects::BlenderKind::Runtime && b.runtime_spec) {
                                m_impl->runVkFilterRuntimeBlenderPass(cmd, blendOutIdx, finalPoolIdx, dstSnapIdx,
                                                                      *b.runtime_spec, op, fbW, fbH);
                            }
                            m_impl->layerPool[dstSnapIdx].inUse = false;
                            if (finalPoolIdx != rl.poolIndex) m_impl->layerPool[finalPoolIdx].inUse = false;
                            finalPoolIdx = blendOutIdx;
                        } else {
                            if (dstSnapIdx >= 0) m_impl->layerPool[dstSnapIdx].inUse = false;
                            if (blendOutIdx >= 0) m_impl->layerPool[blendOutIdx].inUse = false;
                        }
                    }
                }
            }

            // Resume parent render pass
            const bool parentIsNested = !m_impl->activeLayers.empty();
            VkRenderPassBeginInfo rpb{};
            rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpb.renderPass = parentIsNested
                ? m_impl->offscreenRenderPassLoad
                : m_impl->targetSurface->render_pass_load();
            rpb.framebuffer = parentIsNested
                ? m_impl->layerPool[m_impl->activeLayers.back().poolIndex].layerFb
                : m_impl->targetSurface->framebuffer();
            rpb.renderArea = {{0, 0}, {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH)}};
            vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            rebindDrawState();

            activeClipDepth = rl.parentClipDepth;

            if (call.triangleCount > 0 && finalPoolIdx >= 0) {
                VkRect2D sci{};
                if (call.scissorRect[0] >= 0) {
                    sci.offset = {std::max(0, call.scissorRect[0]), std::max(0, call.scissorRect[1])};
                    sci.extent = {static_cast<uint32_t>(std::max(0, call.scissorRect[2])),
                                  static_cast<uint32_t>(std::max(0, call.scissorRect[3]))};
                } else {
                    sci.offset = {0, 0};
                    sci.extent = {static_cast<uint32_t>(fbW), static_cast<uint32_t>(fbH)};
                }
                vkCmdSetScissor(cmd, 0, 1, &sci);

                const uint32_t dynamicOffset = static_cast<uint32_t>(call.uniformOffset);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_impl->pipelineLayout,
                                        0, 1, &m_impl->layerPool[finalPoolIdx].drawDescSet, 1, &dynamicOffset);

                const VkStencilMode normalStencil = (hasStencilAttachment && activeClipDepth > 0)
                    ? VkStencilMode::ClipTest : VkStencilMode::NoStencil;
                VkPipeline p = m_impl->getOrCreatePipeline(VkTopologyKind::List, normalStencil, call.blend, parentIsNested);
                if (p != VK_NULL_HANDLE) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                    if (normalStencil != VkStencilMode::NoStencil) {
                        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, static_cast<uint32_t>(activeClipDepth << 4));
                    }
                    vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                }
            }

            if (finalPoolIdx >= 0 && finalPoolIdx != rl.poolIndex) {
                m_impl->layerPool[finalPoolIdx].inUse = false;
            }
            m_impl->layerPool[rl.poolIndex].inUse = false;
            continue;
        }

        const bool inOffscreen = !m_impl->activeLayers.empty();

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
                VkPipeline p = m_impl->getOrCreatePipeline(VkTopologyKind::List, normalStencil, call.blend, inOffscreen, call.isPerlinNoise);
                if (bindAndSetStencil(p, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                    vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                }
            }
        } else if (call.type == CallType::ConvexFill) {
            VkPipeline pFan = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, normalStencil, call.blend, inOffscreen, call.isPerlinNoise);
            if (bindAndSetStencil(pFan, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                }
            }
            VkPipeline pStrip = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, normalStencil, call.blend, inOffscreen, call.isPerlinNoise);
            if (bindAndSetStencil(pStrip, clipRef, normalStencil != VkStencilMode::NoStencil)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.strokeCount > 0) vkCmdDraw(cmd, path.strokeCount, 1, path.strokeOffset, 0);
                }
            }
        } else if (call.type == CallType::Stroke) {
            VkPipeline p = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, normalStencil, call.blend, inOffscreen, call.isPerlinNoise);
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
                VkPipeline pStencil = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, stencilFillMode, VkBlendState{}, inOffscreen);
                if (bindAndSetStencil(pStencil, clipRef, true)) {
                    for (int i = 0; i < call.pathCount; ++i) {
                        const auto& path = m_impl->paths[call.pathOffset + i];
                        if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                    }
                }
                if (edgeAntiAlias()) {
                    VkPipeline pAA = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::FillAntiAlias, call.blend, inOffscreen, call.isPerlinNoise);
                    if (bindAndSetStencil(pAA, clipRef, true)) {
                        for (int i = 0; i < call.pathCount; ++i) {
                            const auto& path = m_impl->paths[call.pathOffset + i];
                            if (path.strokeCount > 0) vkCmdDraw(cmd, path.strokeCount, 1, path.strokeOffset, 0);
                        }
                    }
                }
                if (call.triangleCount > 0) {
                    VkPipeline pCover = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::FillCover, call.blend, inOffscreen, call.isPerlinNoise);
                    if (bindAndSetStencil(pCover, 0u, true)) {
                        vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                    }
                }
            } else {
                VkPipeline pFan = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, VkStencilMode::NoStencil, call.blend, inOffscreen, call.isPerlinNoise);
                if (bindAndSetStencil(pFan, 0u, false)) {
                    for (int i = 0; i < call.pathCount; ++i) {
                        const auto& path = m_impl->paths[call.pathOffset + i];
                        if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                    }
                }
                VkPipeline pStrip = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::NoStencil, call.blend, inOffscreen, call.isPerlinNoise);
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
            VkPipeline pClip = m_impl->getOrCreatePipeline(VkTopologyKind::Fan, clipFillMode, VkBlendState{}, inOffscreen);
            if (bindAndSetStencil(pClip, prevDepthVal, true)) {
                for (int i = 0; i < call.pathCount; ++i) {
                    const auto& path = m_impl->paths[call.pathOffset + i];
                    if (path.fillCount > 0) vkCmdDraw(cmd, path.fillCount, 1, path.fillOffset, 0);
                }
            }
            if (call.triangleCount > 0) {
                VkPipeline pCover = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::ClipCover, VkBlendState{}, inOffscreen);
                if (bindAndSetStencil(pCover, newDepthVal, true)) {
                    vkCmdDraw(cmd, call.triangleCount, 1, call.triangleOffset, 0);
                }
            }
            activeClipDepth = call.clipDepth;
        } else if (call.type == CallType::RestoreClip && hasStencilAttachment) {
            if (call.clipDepth < activeClipDepth && call.triangleCount > 0) {
                const uint32_t targetDepthVal = static_cast<uint32_t>(call.clipDepth << 4);
                VkPipeline pRestore = m_impl->getOrCreatePipeline(VkTopologyKind::Strip, VkStencilMode::RestoreClip, VkBlendState{}, inOffscreen);
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
