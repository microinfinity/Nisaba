#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/transform.hpp"

namespace nisaba::effects {

/// Uniform scalar/vector/matrix type for post-process RuntimeFilter shaders.
enum class RuntimeUniformType : uint8_t {
    Float = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    Mat2 = 4,
    Mat3 = 5,
    Mat4 = 6,
    Int = 7,
};

/// Reflected uniform descriptor for a post-process RuntimeFilter shader.
struct RuntimeFilterUniform {
    std::string name{};
    size_t offset{0};
    size_t size_bytes{0};
    RuntimeUniformType type{RuntimeUniformType::Float};
    int count{1};
};

/// Reflected child-shader slot for a post-process RuntimeFilter shader.
struct RuntimeFilterChild {
    std::string name{};
    size_t index{0};
};

/// Convenience map for supplying named uniform values when building a RuntimeShader ImageFilter.
class RuntimeFilterUniforms {
public:
    template <typename T>
    RuntimeFilterUniforms& set(std::string name, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "Uniform value must be trivially copyable");
        std::vector<uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        values_[std::move(name)] = std::move(bytes);
        return *this;
    }

    RuntimeFilterUniforms& set_float(std::string name, float v) { return set(std::move(name), v); }
    RuntimeFilterUniforms& set_int(std::string name, int32_t v) { return set(std::move(name), v); }
    RuntimeFilterUniforms& set_vec2(std::string name, float x, float y) {
        float a[2] = {x, y};
        return set(std::move(name), a);
    }
    RuntimeFilterUniforms& set_vec3(std::string name, float x, float y, float z) {
        float a[3] = {x, y, z};
        return set(std::move(name), a);
    }
    RuntimeFilterUniforms& set_vec4(std::string name, float x, float y, float z, float w) {
        float a[4] = {x, y, z, w};
        return set(std::move(name), a);
    }

    [[nodiscard]] const std::unordered_map<std::string, std::vector<uint8_t>>& entries() const noexcept {
        return values_;
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> values_{};
};

/// Optional compiled C++ CPU fast-path callback matching a RuntimeFilterSpec's shader semantics.
using CpuRuntimeFilterFn = std::function<void(
    const PixmapRef& src_rgba,
    PixmapMut& dst_rgba,
    int32_t roi_origin_x,
    int32_t roi_origin_y,
    const Transform& layer_transform,
    const Transform& inv_layer_transform,
    int32_t cx0,
    int32_t cy0,
    int32_t cx1,
    int32_t cy1
)>;

/// Immutable, backend-independent representation of a custom post-processing shader
/// used by `ImageFilter::runtime_shader(...)` across CPU, OpenGL, and Vulkan.
struct RuntimeFilterSpec {
    std::string source_code{};
    std::string transformed_glsl{};
    std::vector<RuntimeFilterUniform> uniforms{};
    std::vector<RuntimeFilterChild> children{};
    std::vector<uint8_t> uniform_data{};
    size_t uniform_size{0};
    size_t input_child_index{0};
    bool has_input_child{false};
    bool has_color_in{false};
    uint64_t shader_hash{0};
    CpuRuntimeFilterFn cpu_fast_eval{};
};

/// Parses a Nisaba shader program into a backend-independent RuntimeFilterSpec.
/// Returns nullptr on empty or malformed source.
[[nodiscard]] std::shared_ptr<RuntimeFilterSpec> parse_runtime_filter_source(
    std::string_view source,
    const RuntimeFilterUniforms* overrides = nullptr,
    std::string_view input_child_name = {}
);

/// Clones a pre-parsed RuntimeFilterSpec with updated uniform values and an optional compiled CPU kernel.
[[nodiscard]] std::shared_ptr<RuntimeFilterSpec> clone_runtime_filter_with_uniforms(
    const RuntimeFilterSpec& base_spec,
    const RuntimeFilterUniforms& overrides,
    CpuRuntimeFilterFn cpu_eval = {}
);

/// Evaluates a RuntimeFilterSpec on CPU over an RGBA8888 premultiplied source ROI pixmap.
[[nodiscard]] Pixmap eval_runtime_filter_cpu(
    const RuntimeFilterSpec& spec,
    const Pixmap& src_rgba,
    int32_t roi_origin_x,
    int32_t roi_origin_y,
    const Transform& layer_transform,
    const std::optional<Rect>& crop_rect = std::nullopt
);

} // namespace nisaba::effects

