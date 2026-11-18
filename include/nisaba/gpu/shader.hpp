#pragma once

#include <cstdint>
#include <memory>

namespace nisaba {
struct Transform;
}

namespace nisaba::gpu {

struct Transform2D;

enum class ShaderKind : uint8_t {
    Base = 0,
    Runtime = 1,
};

/**
 * @brief Base polymorphic Shader interface for GPU pipelines.
 */
class Shader {
public:
    virtual ~Shader() = default;
    virtual void* getNativeHandle() const = 0;
    virtual bool isOpaque() const { return false; }

    [[nodiscard]] virtual ShaderKind shader_kind() const noexcept { return ShaderKind::Base; }

    [[nodiscard]] virtual std::shared_ptr<Shader> with_local_transform(const Transform2D& local_matrix) const {
        (void)local_matrix;
        return nullptr;
    }

    virtual bool bind_shader(const float view_size[2], const Transform2D& canvas_xform) const {
        (void)view_size;
        (void)canvas_xform;
        return false;
    }
};

/// 1:1 drop-in compatibility alias for ENKI framework
using SkShader = Shader;

} // namespace nisaba::gpu
