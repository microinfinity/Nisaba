#pragma once

#include "nisaba/effects/image_filter.hpp"
#include "nisaba/gpu/shader.hpp"
#include "nisaba/gpu/types.hpp"
#include "nisaba/math/transform.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <memory>
#include <type_traits>
#include <cstring>

namespace nisaba::gpu {

class RuntimeEffect : public std::enable_shared_from_this<RuntimeEffect> {
public:
    struct Uniform {
        enum class Type {
            Float,
            Float2,
            Float3,
            Float4,
            Mat2,
            Mat3,
            Mat4,
            Int
        };

        std::string name;
        size_t offset{0};
        size_t sizeInBytes{0};
        Type type{Type::Float};
        int count{1};
        int location{-1};
    };

    struct Child {
        std::string name;
        size_t index{0};
    };

    struct Result {
        std::shared_ptr<RuntimeEffect> effect{nullptr};
        std::string errorText{};
        explicit operator bool() const noexcept { return effect != nullptr; }
    };

    static Result MakeForShader(std::string_view sksl);

    ~RuntimeEffect();

    [[nodiscard]] const Uniform* findUniform(const char* name) const noexcept;
    [[nodiscard]] const Child* find_child(std::string_view name) const noexcept;
    [[nodiscard]] const std::vector<Uniform>& uniforms() const noexcept { return uniforms_; }
    [[nodiscard]] const std::vector<Child>& children() const noexcept { return children_; }
    [[nodiscard]] size_t uniformSize() const noexcept { return uniform_size_; }
    [[nodiscard]] const std::vector<uint8_t>& default_uniform_data() const noexcept { return default_uniforms_; }
    [[nodiscard]] uint32_t programId() const noexcept { return program_; }
    [[nodiscard]] int localMatrixLoc() const noexcept { return loc_local_mat_; }
    [[nodiscard]] int viewSizeLoc() const noexcept { return loc_view_size_; }
    [[nodiscard]] const std::string& source_code() const noexcept { return source_code_; }
    [[nodiscard]] const std::string& transformed_code() const noexcept { return transformed_code_; }
    [[nodiscard]] const std::vector<std::string>& declared_functions() const noexcept { return declared_functions_; }
    [[nodiscard]] bool has_color_in() const noexcept { return has_color_in_; }

    [[nodiscard]] std::shared_ptr<Shader> make_shader(
        std::span<const uint8_t> uniforms,
        std::span<const std::shared_ptr<Shader>> children = {},
        const Transform2D* local_matrix = nullptr
    ) const;

private:
    RuntimeEffect() = default;
    uint32_t program_{0};
    uint32_t vert_shader_{0};
    uint32_t frag_shader_{0};
    std::vector<Uniform> uniforms_{};
    std::vector<Child> children_{};
    std::vector<uint8_t> default_uniforms_{};
    std::vector<std::string> declared_functions_{};
    std::string source_code_{};
    std::string transformed_code_{};
    bool has_color_in_{false};
    size_t uniform_size_{0};
    int loc_local_mat_{-1};
    int loc_view_size_{-1};

    friend class RuntimeEffectParser;
};

class RuntimeShader : public Shader {
public:
    struct CompiledProgram;

    RuntimeShader(std::shared_ptr<const RuntimeEffect> effect,
                  std::vector<uint8_t> uniformData,
                  Transform2D localMatrix = Transform2D::identity(),
                  std::vector<std::shared_ptr<Shader>> children = {});

    void* getNativeHandle() const override { return const_cast<RuntimeShader*>(this); }
    [[nodiscard]] bool isOpaque() const override { return false; }
    [[nodiscard]] ShaderKind shader_kind() const noexcept override { return ShaderKind::Runtime; }

    [[nodiscard]] std::shared_ptr<Shader> with_local_transform(const Transform2D& local_matrix) const override;

    [[nodiscard]] const std::shared_ptr<const RuntimeEffect>& effect() const noexcept { return effect_; }
    [[nodiscard]] const std::vector<uint8_t>& uniformData() const noexcept { return uniform_data_; }
    [[nodiscard]] const Transform2D& localMatrix() const noexcept { return local_matrix_; }
    [[nodiscard]] const std::vector<std::shared_ptr<Shader>>& children() const noexcept { return children_; }

    void bind(const float* viewSize) const;
    bool bind_shader(const float view_size[2], const Transform2D& canvas_xform) const override;

private:
    std::shared_ptr<const RuntimeEffect> effect_;
    std::vector<uint8_t> uniform_data_;
    Transform2D local_matrix_;
    std::vector<std::shared_ptr<Shader>> children_;
    std::shared_ptr<CompiledProgram> bound_program_;
    float inv_m3_[9]{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

class RuntimeShaderBuilder {
public:
    struct BuilderUniform {
        RuntimeShaderBuilder* owner{nullptr};
        const RuntimeEffect::Uniform* var{nullptr};

        template <typename T>
        BuilderUniform& operator=(const T& val) {
            static_assert(std::is_trivially_copyable_v<T>, "Value must be trivially copyable");
            if (var && owner) {
                owner->writeUniformData(var->offset, &val, sizeof(T));
            }
            return *this;
        }

        template <typename T>
        bool set(const T val[], int count) {
            static_assert(std::is_trivially_copyable_v<T>, "Value must be trivially copyable");
            if (var && owner && sizeof(T) * count <= var->sizeInBytes) {
                owner->writeUniformData(var->offset, val, sizeof(T) * count);
                return true;
            }
            return false;
        }
    };

    struct BuilderChild {
        RuntimeShaderBuilder* owner{nullptr};
        const RuntimeEffect::Child* slot{nullptr};

        BuilderChild& operator=(std::shared_ptr<Shader> sh) {
            if (slot && owner) {
                owner->set_child(slot->index, std::move(sh));
            }
            return *this;
        }
    };

    explicit RuntimeShaderBuilder(std::shared_ptr<RuntimeEffect> effect);

    BuilderUniform uniform(const char* name) {
        return BuilderUniform{this, effect_ ? effect_->findUniform(name) : nullptr};
    }

    BuilderChild child(const char* name) {
        return BuilderChild{this, (effect_ && name) ? effect_->find_child(name) : nullptr};
    }

    void set_child(size_t index, std::shared_ptr<Shader> child_shader);
    void set_child(const char* name, std::shared_ptr<Shader> child_shader);

    std::shared_ptr<Shader> makeShader(const Transform2D* localMatrix = nullptr);
    std::shared_ptr<Shader> makeShader(const nisaba::Transform* localMatrix);
    std::shared_ptr<Shader> make_shader(const Transform2D* local_matrix = nullptr) {
        return makeShader(local_matrix);
    }

    /// Creates a composable post-processing ImageFilter from this builder's shader and uniform values.
    [[nodiscard]] effects::ImageFilter make_image_filter(
        std::string_view input_child_name = {},
        std::optional<effects::ImageFilter> input = std::nullopt,
        std::optional<Rect> crop_rect = std::nullopt
    ) const;

    void writeUniformData(size_t offset, const void* data, size_t size);

private:
    std::shared_ptr<RuntimeEffect> effect_;
    std::vector<uint8_t> uniform_data_;
    std::vector<std::shared_ptr<Shader>> children_;
};

// Aliases for framework drop-in compatibility
using SkRuntimeEffect = RuntimeEffect;
using SkRuntimeShaderBuilder = RuntimeShaderBuilder;

} // namespace nisaba::gpu
