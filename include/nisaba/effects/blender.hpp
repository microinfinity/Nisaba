#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/color/blend_mode.hpp"
#include "nisaba/color/color.hpp"
#include "nisaba/effects/runtime_filter.hpp"

namespace nisaba::effects {

/// Kind of compositing Blender attached to a Paint or layer.
enum class BlenderKind : uint8_t {
    Mode = 0,
    Arithmetic = 1,
    Runtime = 2,
};

/// Value-typed programmable Blender supporting standard BlendModes,
/// arithmetic combination (`k1*src*dst + k2*src + k3*dst + k4`),
/// and custom GLSL/Nisaba `vec4 main(vec4 src, vec4 dst)` shaders across CPU, OpenGL, and Vulkan.
class Blender {
public:
    BlenderKind kind{BlenderKind::Mode};
    BlendMode blend_mode{BlendMode::SourceOver};
    float k1{0.0f};
    float k2{0.0f};
    float k3{0.0f};
    float k4{0.0f};
    bool enforce_premul{true};
    std::shared_ptr<const RuntimeFilterSpec> runtime_spec{};

    Blender() noexcept = default;

    [[nodiscard]] static Blender mode(BlendMode m) noexcept {
        Blender b;
        b.kind = BlenderKind::Mode;
        b.blend_mode = m;
        return b;
    }

    [[nodiscard]] static Blender arithmetic(
        float k1_val,
        float k2_val,
        float k3_val,
        float k4_val,
        bool enforce_premul_val = true
    ) noexcept {
        Blender b;
        b.kind = BlenderKind::Arithmetic;
        b.k1 = k1_val;
        b.k2 = k2_val;
        b.k3 = k3_val;
        b.k4 = k4_val;
        b.enforce_premul = enforce_premul_val;
        return b;
    }

    /// Creates a custom shader blender with entry point `vec4 main(vec4 src, vec4 dst)`.
    [[nodiscard]] static Blender runtime(
        std::string_view source,
        const RuntimeFilterUniforms& uniforms = {}
    );

    [[nodiscard]] bool is_custom() const noexcept {
        return kind == BlenderKind::Arithmetic ||
               (kind == BlenderKind::Runtime && runtime_spec != nullptr);
    }

    /// Blends a premultiplied source pixel over a premultiplied destination pixel on CPU.
    [[nodiscard]] PremultipliedColorU8 blend_pixel(
        PremultipliedColorU8 src,
        PremultipliedColorU8 dst
    ) const noexcept;

    /// Composites a premultiplied RGBA8888/BGRA8888 source layer over `dst` within the ROI on CPU.
    void composite_cpu(
        PixmapMut& dst,
        const PixmapRef& src,
        int32_t x0,
        int32_t y0,
        int32_t x1,
        int32_t y1,
        float opacity = 1.0f,
        const uint8_t* clip_mask = nullptr,
        uint32_t clip_stride = 0
    ) const;
};

} // namespace nisaba::effects

