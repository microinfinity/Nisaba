#include "nisaba/gpu/runtime_effect.hpp"
#include "gl_headers.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nisaba::gpu {

namespace {

bool is_gles_context() {
    return detail::is_gles_or_webgl();
}

GLuint compile_vertex_shader(bool is_gles) {
    static const char* kVertSourceGL =
        "#version 150 core\n"
        "uniform vec2 viewSize;\n"
        "uniform mat3 u_localMatrix;\n"
        "in vec2 vertex;\n"
        "in vec2 tcoord;\n"
        "in vec4 color;\n"
        "out vec2 fpos;\n"
        "out vec2 ftcoord;\n"
        "out vec4 fcolor;\n"
        "out vec2 v_localCoord;\n"
        "void main(void) {\n"
        "    fpos = vertex;\n"
        "    ftcoord = tcoord;\n"
        "    fcolor = color;\n"
        "    v_localCoord = (u_localMatrix * vec3(vertex, 1.0)).xy;\n"
        "    gl_Position = vec4(2.0 * vertex.x / viewSize.x - 1.0, 1.0 - 2.0 * vertex.y / viewSize.y, 0.0, 1.0);\n"
        "}\n";

    static const char* kVertSourceGLES =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "uniform vec2 viewSize;\n"
        "uniform mat3 u_localMatrix;\n"
        "in vec2 vertex;\n"
        "in vec2 tcoord;\n"
        "in vec4 color;\n"
        "out vec2 fpos;\n"
        "out vec2 ftcoord;\n"
        "out vec4 fcolor;\n"
        "out vec2 v_localCoord;\n"
        "void main(void) {\n"
        "    fpos = vertex;\n"
        "    ftcoord = tcoord;\n"
        "    fcolor = color;\n"
        "    v_localCoord = (u_localMatrix * vec3(vertex, 1.0)).xy;\n"
        "    gl_Position = vec4(2.0 * vertex.x / viewSize.x - 1.0, 1.0 - 2.0 * vertex.y / viewSize.y, 0.0, 1.0);\n"
        "}\n";

    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    const char* v_ptr = is_gles ? kVertSourceGLES : kVertSourceGL;
    glShaderSource(vert, 1, &v_ptr, nullptr);
    glCompileShader(vert);
    return vert;
}

const char* kSkslBuiltinsGLSL =
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
    "vec4 fromLinearSrgb(vec4 c) { return vec4(fromLinearSrgb(c.rgb), c.a); }\n\n";

struct TypeMapping {
    const char* glsl_type;
    RuntimeEffect::Uniform::Type uniform_type;
    size_t elem_bytes;
    int scalar_count;
};

const TypeMapping* map_sksl_type(std::string_view t) noexcept {
    static constexpr TypeMapping kTable[] = {
        {"float", RuntimeEffect::Uniform::Type::Float, 4, 1},
        {"float", RuntimeEffect::Uniform::Type::Float, 4, 1},
        {"vec2", RuntimeEffect::Uniform::Type::Float2, 8, 2},
        {"vec2", RuntimeEffect::Uniform::Type::Float2, 8, 2},
        {"vec2", RuntimeEffect::Uniform::Type::Float2, 8, 2},
        {"vec3", RuntimeEffect::Uniform::Type::Float3, 12, 3},
        {"vec3", RuntimeEffect::Uniform::Type::Float3, 12, 3},
        {"vec3", RuntimeEffect::Uniform::Type::Float3, 12, 3},
        {"vec4", RuntimeEffect::Uniform::Type::Float4, 16, 4},
        {"vec4", RuntimeEffect::Uniform::Type::Float4, 16, 4},
        {"vec4", RuntimeEffect::Uniform::Type::Float4, 16, 4},
        {"mat2", RuntimeEffect::Uniform::Type::Mat2, 16, 4},
        {"mat2", RuntimeEffect::Uniform::Type::Mat2, 16, 4},
        {"mat2", RuntimeEffect::Uniform::Type::Mat2, 16, 4},
        {"mat3", RuntimeEffect::Uniform::Type::Mat3, 36, 9},
        {"mat3", RuntimeEffect::Uniform::Type::Mat3, 36, 9},
        {"mat3", RuntimeEffect::Uniform::Type::Mat3, 36, 9},
        {"mat4", RuntimeEffect::Uniform::Type::Mat4, 64, 16},
        {"mat4", RuntimeEffect::Uniform::Type::Mat4, 64, 16},
        {"mat4", RuntimeEffect::Uniform::Type::Mat4, 64, 16},
        {"int", RuntimeEffect::Uniform::Type::Int, 4, 1},
    };
    if (t == "float") return &kTable[0];
    if (t == "half") return &kTable[1];
    if (t == "vec2") return &kTable[2];
    if (t == "float2") return &kTable[3];
    if (t == "half2") return &kTable[4];
    if (t == "vec3") return &kTable[5];
    if (t == "float3") return &kTable[6];
    if (t == "half3") return &kTable[7];
    if (t == "vec4") return &kTable[8];
    if (t == "float4") return &kTable[9];
    if (t == "half4") return &kTable[10];
    if (t == "mat2") return &kTable[11];
    if (t == "float2x2") return &kTable[12];
    if (t == "half2x2") return &kTable[13];
    if (t == "mat3") return &kTable[14];
    if (t == "float3x3") return &kTable[15];
    if (t == "half3x3") return &kTable[16];
    if (t == "mat4") return &kTable[17];
    if (t == "float4x4") return &kTable[18];
    if (t == "half4x4") return &kTable[19];
    if (t == "int") return &kTable[20];
    return nullptr;
}

std::string_view map_sksl_ident_type(std::string_view ident) noexcept {
    if (const auto* tm = map_sksl_type(ident)) return tm->glsl_type;
    if (ident == "int2") return "ivec2";
    if (ident == "int3") return "ivec3";
    if (ident == "int4") return "ivec4";
    if (ident == "bool2") return "bvec2";
    if (ident == "bool3") return "bvec3";
    if (ident == "bool4") return "bvec4";
    return ident;
}

bool is_ident_start(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void skip_ws_and_comments(std::string_view src, size_t& idx) noexcept {
    const size_t n = src.size();
    while (idx < n) {
        if (std::isspace(static_cast<unsigned char>(src[idx]))) {
            ++idx;
        } else if (idx + 1 < n && src[idx] == '/' && src[idx + 1] == '/') {
            idx += 2;
            while (idx < n && src[idx] != '\n') ++idx;
        } else if (idx + 1 < n && src[idx] == '/' && src[idx + 1] == '*') {
            idx += 2;
            while (idx + 1 < n && !(src[idx] == '*' && src[idx + 1] == '/')) ++idx;
            if (idx + 1 < n) idx += 2;
        } else {
            break;
        }
    }
}

void parse_numeric_initializer(
    std::string_view init_expr,
    RuntimeEffect::Uniform::Type type,
    uint8_t* dst,
    size_t max_bytes
) {
    std::vector<double> nums;
    const size_t n = init_expr.size();
    size_t i = 0;
    while (i < n) {
        const char ch = init_expr[i];
        if (is_ident_start(ch)) {
            while (i < n && is_ident_char(init_expr[i])) ++i;
            // Skip size expressions like float[4](...)
            size_t p = i;
            skip_ws_and_comments(init_expr, p);
            if (p < n && init_expr[p] == '[') {
                while (p < n && init_expr[p] != ']') ++p;
                if (p < n) i = p + 1;
            }
            continue;
        }
        bool is_signed_num = (ch == '-' || ch == '+') && (i + 1 < n) &&
                             (std::isdigit(static_cast<unsigned char>(init_expr[i + 1])) || init_expr[i + 1] == '.');
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || is_signed_num) {
            std::string tmp(init_expr.substr(i, std::min<size_t>(48, n - i)));
            char* end_ptr = nullptr;
            double val = std::strtod(tmp.c_str(), &end_ptr);
            if (end_ptr && end_ptr != tmp.c_str()) {
                size_t consumed = static_cast<size_t>(end_ptr - tmp.c_str());
                if (i + consumed < n && (init_expr[i + consumed] == 'f' || init_expr[i + consumed] == 'F')) {
                    ++consumed;
                }
                i += consumed;
                nums.push_back(val);
                continue;
            }
        }
        ++i;
    }

    if (nums.empty()) return;
    if (type == RuntimeEffect::Uniform::Type::Int) {
        const size_t max_ints = max_bytes / sizeof(int);
        int* out = reinterpret_cast<int*>(dst);
        for (size_t k = 0; k < std::min(nums.size(), max_ints); ++k) {
            out[k] = static_cast<int>(nums[k]);
        }
    } else {
        const size_t max_floats = max_bytes / sizeof(float);
        float* out = reinterpret_cast<float*>(dst);
        if (nums.size() == 1 && max_floats > 1 && max_floats <= 4) {
            // Splat scalar initializer like vec4(1.0)
            for (size_t k = 0; k < max_floats; ++k) {
                out[k] = static_cast<float>(nums[0]);
            }
        } else {
            for (size_t k = 0; k < std::min(nums.size(), max_floats); ++k) {
                out[k] = static_cast<float>(nums[k]);
            }
        }
    }
}

std::string rewrite_identifiers(
    std::string_view code,
    const std::unordered_map<std::string, std::string>& rename_map
) {
    std::string out;
    out.reserve(code.size() + 64);
    const size_t n = code.size();
    size_t i = 0;
    char prev_non_ws = '\0';

    while (i < n) {
        if (i + 1 < n && code[i] == '/' && code[i + 1] == '/') {
            const size_t s = i;
            i += 2;
            while (i < n && code[i] != '\n') ++i;
            out.append(code.substr(s, i - s));
            continue;
        }
        if (i + 1 < n && code[i] == '/' && code[i + 1] == '*') {
            const size_t s = i;
            i += 2;
            while (i + 1 < n && !(code[i] == '*' && code[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            out.append(code.substr(s, i - s));
            continue;
        }
        if (is_ident_start(code[i])) {
            const size_t s = i;
            while (i < n && is_ident_char(code[i])) ++i;
            std::string_view ident = code.substr(s, i - s);
            if (prev_non_ws != '.') {
                auto it = rename_map.find(std::string(ident));
                if (it != rename_map.end()) {
                    out.append(it->second);
                    prev_non_ws = 'a';
                    continue;
                }
            }
            out.append(ident);
            prev_non_ws = 'a';
            continue;
        }
        const char ch = code[i];
        out.push_back(ch);
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            prev_non_ws = ch;
        }
        ++i;
    }
    return out;
}

void to_inv_mat3(const Transform2D& m, float out[9]) noexcept {
    Transform2D inv;
    if (!m.inverse(inv)) {
        inv = Transform2D::identity();
    }
    out[0] = inv[0]; out[1] = inv[1]; out[2] = 0.0f;
    out[3] = inv[2]; out[4] = inv[3]; out[5] = 0.0f;
    out[6] = inv[4]; out[7] = inv[5]; out[8] = 1.0f;
}

void upload_uniform(int loc, RuntimeEffect::Uniform::Type type, int count, const uint8_t* ptr) {
    if (loc < 0 || !ptr || count <= 0) return;
    switch (type) {
        case RuntimeEffect::Uniform::Type::Float:
            glUniform1fv(loc, count, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Float2:
            glUniform2fv(loc, count, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Float3:
            glUniform3fv(loc, count, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Float4:
            glUniform4fv(loc, count, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Mat2:
            glUniformMatrix2fv(loc, count, GL_FALSE, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Mat3:
            glUniformMatrix3fv(loc, count, GL_FALSE, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Mat4:
            glUniformMatrix4fv(loc, count, GL_FALSE, reinterpret_cast<const float*>(ptr));
            break;
        case RuntimeEffect::Uniform::Type::Int:
            glUniform1iv(loc, count, reinterpret_cast<const int*>(ptr));
            break;
    }
}

bool compile_and_link_fragment(
    const std::string& frag_src,
    GLuint& out_prog,
    GLuint& out_frag,
    std::string& out_error
) {
    const GLuint vert = compile_vertex_shader(is_gles_context());
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    const char* f_ptr = frag_src.c_str();
    glShaderSource(frag, 1, &f_ptr, nullptr);
    glCompileShader(frag);

    GLint status = 0;
    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048];
        GLsizei len = 0;
        glGetShaderInfoLog(frag, sizeof(log), &len, log);
        glDeleteShader(vert);
        glDeleteShader(frag);
        out_error = std::string("Fragment shader compile error: ") + log;
        return false;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glBindAttribLocation(prog, 0, "vertex");
    glBindAttribLocation(prog, 1, "tcoord");
    glBindAttribLocation(prog, 2, "color");
    glLinkProgram(prog);
    glDetachShader(prog, vert);
    glDetachShader(prog, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        glDeleteProgram(prog);
        out_error = std::string("Program link error: ") + log;
        return false;
    }

    out_prog = prog;
    out_frag = 0;
    return true;
}

std::string build_frag_preamble(bool is_gles) {
    std::string hdr;
    hdr.reserve(1024);
    hdr += is_gles ? "#version 300 es\n" : "#version 150 core\n";
    hdr +=
        "precision highp float;\n\n"
        "uniform vec2 viewSize;\n\n"
        "in vec2 fpos;\n"
        "in vec2 ftcoord;\n"
        "in vec4 fcolor;\n"
        "in vec2 v_localCoord;\n"
        "out vec4 outColor;\n\n";
    hdr += kSkslBuiltinsGLSL;
    return hdr;
}

} // namespace

struct RuntimeShader::CompiledProgram {
    struct BoundUniform {
        int location{-1};
        RuntimeEffect::Uniform::Type type{RuntimeEffect::Uniform::Type::Float};
        int count{1};
        std::vector<uint8_t> data;
    };

    struct BoundMatrix {
        int location{-1};
        float inv_m3[9]{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    };

    GLuint program{0};
    GLuint frag_shader{0};
    int loc_local_mat{-1};
    int loc_view_size{-1};
    std::vector<int> root_uniform_locs;
    std::vector<BoundUniform> child_uniforms;
    std::vector<BoundMatrix> child_matrices;

    ~CompiledProgram() {
        if (program != 0) glDeleteProgram(program);
        if (frag_shader != 0) glDeleteShader(frag_shader);
    }
};

namespace {

struct PendingChildUniform {
    std::string glsl_name;
    RuntimeEffect::Uniform::Type type;
    int count;
    std::vector<uint8_t> bytes;
};

struct PendingChildMatrix {
    std::string glsl_name;
    Transform2D local_matrix;
};

void emit_child_module(
    const RuntimeShader* child_rts,
    const std::string& prefix,
    const std::string& target_eval_fn,
    std::string& out_code,
    std::vector<PendingChildUniform>& out_uniforms,
    std::vector<PendingChildMatrix>& out_matrices
) {
    if (!child_rts || !child_rts->effect()) {
        out_code += "vec4 " + target_eval_fn + "(vec2 _p, vec4 _c) { return vec4(0.0); }\n";
        out_code += "vec4 " + target_eval_fn + "(vec2 _p) { return vec4(0.0); }\n";
        return;
    }

    const auto& eff = child_rts->effect();
    const auto& sub_children = child_rts->children();

    // Recursively emit nested children first so wrappers are defined before use
    for (size_t k = 0; k < eff->children().size(); ++k) {
        const Shader* sub = (k < sub_children.size()) ? sub_children[k].get() : nullptr;
        const RuntimeShader* sub_rts = (sub && sub->shader_kind() == ShaderKind::Runtime)
            ? static_cast<const RuntimeShader*>(sub)
            : nullptr;
        const std::string sub_prefix = prefix + "c" + std::to_string(k) + "_";
        const std::string sub_eval_fn = prefix + "__nisaba_child_eval_" + std::to_string(k);
        emit_child_module(sub_rts, sub_prefix, sub_eval_fn, out_code, out_uniforms, out_matrices);
    }

    std::unordered_map<std::string, std::string> rename_map;
    rename_map.emplace("user_main", prefix + "user_main");
    for (const auto& u : eff->uniforms()) {
        const std::string mapped = prefix + u.name;
        rename_map.emplace(u.name, mapped);
        std::vector<uint8_t> bytes(u.sizeInBytes, 0);
        if (u.offset + u.sizeInBytes <= child_rts->uniformData().size()) {
            std::memcpy(bytes.data(), child_rts->uniformData().data() + u.offset, u.sizeInBytes);
        }
        out_uniforms.push_back(PendingChildUniform{mapped, u.type, u.count, std::move(bytes)});
    }
    for (const auto& fn : eff->declared_functions()) {
        rename_map.emplace(fn, prefix + fn);
    }
    for (size_t k = 0; k < eff->children().size(); ++k) {
        const std::string orig = "__nisaba_child_eval_" + std::to_string(k);
        rename_map.emplace(orig, prefix + orig);
    }

    out_code += rewrite_identifiers(eff->transformed_code(), rename_map);
    out_code += "\n";

    const std::string mat_name = prefix + "u_localMatrix";
    out_code += "uniform mat3 " + mat_name + ";\n";
    out_matrices.push_back(PendingChildMatrix{mat_name, child_rts->localMatrix()});

    const std::string child_main = prefix + "user_main";
    if (eff->has_color_in()) {
        out_code += "vec4 " + target_eval_fn + "(vec2 _p, vec4 _c) {\n"
                    "    vec2 _lc = (" + mat_name + " * vec3(_p, 1.0)).xy;\n"
                    "    return " + child_main + "(_lc, _c);\n"
                    "}\n";
        out_code += "vec4 " + target_eval_fn + "(vec2 _p) {\n"
                    "    return " + target_eval_fn + "(_p, vec4(0.0));\n"
                    "}\n\n";
    } else {
        out_code += "vec4 " + target_eval_fn + "(vec2 _p, vec4 _c) {\n"
                    "    vec2 _lc = (" + mat_name + " * vec3(_p, 1.0)).xy;\n"
                    "    return " + child_main + "(_lc);\n"
                    "}\n";
        out_code += "vec4 " + target_eval_fn + "(vec2 _p) {\n"
                    "    vec2 _lc = (" + mat_name + " * vec3(_p, 1.0)).xy;\n"
                    "    return " + child_main + "(_lc);\n"
                    "}\n\n";
    }
}

std::shared_ptr<RuntimeShader::CompiledProgram> build_bound_program(
    const RuntimeEffect& parent_eff,
    const std::vector<std::shared_ptr<Shader>>& children
) {
    const bool is_gles = is_gles_context();
    std::string frag_src = build_frag_preamble(is_gles);

    std::vector<PendingChildUniform> pending_uniforms;
    std::vector<PendingChildMatrix> pending_matrices;

    for (size_t idx = 0; idx < parent_eff.children().size(); ++idx) {
        const Shader* ch = (idx < children.size()) ? children[idx].get() : nullptr;
        const RuntimeShader* ch_rts = (ch && ch->shader_kind() == ShaderKind::Runtime)
            ? static_cast<const RuntimeShader*>(ch)
            : nullptr;
        const std::string prefix = "__nisaba_c" + std::to_string(idx) + "_";
        const std::string target_eval_fn = "__nisaba_child_eval_" + std::to_string(idx);
        emit_child_module(ch_rts, prefix, target_eval_fn, frag_src, pending_uniforms, pending_matrices);
    }

    frag_src += parent_eff.transformed_code();
    frag_src += "\n\nvoid main(void) {\n";
    if (parent_eff.has_color_in()) {
        frag_src += "    outColor = user_main(v_localCoord, fcolor);\n";
    } else {
        frag_src += "    outColor = user_main(v_localCoord);\n";
    }
    frag_src += "}\n";

    GLuint prog = 0;
    GLuint frag = 0;
    std::string err;
    if (!compile_and_link_fragment(frag_src, prog, frag, err)) {
        return nullptr;
    }

    auto compiled = std::make_shared<RuntimeShader::CompiledProgram>();
    compiled->program = prog;
    compiled->frag_shader = frag;
    compiled->loc_local_mat = glGetUniformLocation(prog, "u_localMatrix");
    compiled->loc_view_size = glGetUniformLocation(prog, "viewSize");

    compiled->root_uniform_locs.reserve(parent_eff.uniforms().size());
    for (const auto& u : parent_eff.uniforms()) {
        compiled->root_uniform_locs.push_back(glGetUniformLocation(prog, u.name.c_str()));
    }

    compiled->child_uniforms.reserve(pending_uniforms.size());
    for (auto& pu : pending_uniforms) {
        RuntimeShader::CompiledProgram::BoundUniform bu;
        bu.location = glGetUniformLocation(prog, pu.glsl_name.c_str());
        bu.type = pu.type;
        bu.count = pu.count;
        bu.data = std::move(pu.bytes);
        compiled->child_uniforms.push_back(std::move(bu));
    }

    compiled->child_matrices.reserve(pending_matrices.size());
    for (const auto& pm : pending_matrices) {
        RuntimeShader::CompiledProgram::BoundMatrix bm;
        bm.location = glGetUniformLocation(prog, pm.glsl_name.c_str());
        to_inv_mat3(pm.local_matrix, bm.inv_m3);
        compiled->child_matrices.push_back(bm);
    }

    return compiled;
}

} // namespace

RuntimeEffect::~RuntimeEffect() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    vert_shader_ = 0;
    if (frag_shader_ != 0) {
        glDeleteShader(frag_shader_);
        frag_shader_ = 0;
    }
}

const RuntimeEffect::Uniform* RuntimeEffect::findUniform(const char* name) const noexcept {
    if (!name) return nullptr;
    for (const auto& u : uniforms_) {
        if (u.name == name) return &u;
    }
    return nullptr;
}

const RuntimeEffect::Child* RuntimeEffect::find_child(std::string_view name) const noexcept {
    for (const auto& c : children_) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

RuntimeEffect::Result RuntimeEffect::MakeForShader(std::string_view sksl) {
    detail::ensure_gl_functions_loaded();
    if (sksl.empty()) {
        return Result{nullptr, "Empty shader source string."};
    }

    const size_t n = sksl.size();
    std::string transformed_code;
    transformed_code.reserve(n + 128);

    std::vector<Uniform> uniforms;
    uniforms.reserve(16);
    std::vector<Child> children;
    std::vector<uint8_t> default_uniforms;
    std::vector<std::string> declared_functions;
    size_t current_offset = 0;
    bool has_color_in = false;

    auto child_index_for = [&](std::string_view name) -> int {
        for (const auto& c : children) {
            if (c.name == name) return static_cast<int>(c.index);
        }
        return -1;
    };

    size_t i = 0;
    int brace_depth = 0;
    std::string_view last_ident;
    char prev_non_ws = '\0';

    while (i < n) {
        if (i + 1 < n && sksl[i] == '/' && sksl[i + 1] == '/') {
            const size_t start = i;
            i += 2;
            while (i < n && sksl[i] != '\n') ++i;
            transformed_code.append(sksl.substr(start, i - start));
            continue;
        }
        if (i + 1 < n && sksl[i] == '/' && sksl[i + 1] == '*') {
            const size_t start = i;
            i += 2;
            while (i + 1 < n && !(sksl[i] == '*' && sksl[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            transformed_code.append(sksl.substr(start, i - start));
            continue;
        }

        if (is_ident_start(sksl[i])) {
            const size_t start = i;
            while (i < n && is_ident_char(sksl[i])) ++i;
            std::string_view ident = sksl.substr(start, i - start);

            if (ident == "uniform") {
                size_t p = i;
                skip_ws_and_comments(sksl, p);
                if (p < n && is_ident_start(sksl[p])) {
                    const size_t type_start = p;
                    while (p < n && is_ident_char(sksl[p])) ++p;
                    std::string_view type_str = sksl.substr(type_start, p - type_start);

                    skip_ws_and_comments(sksl, p);
                    if (p < n && is_ident_start(sksl[p])) {
                        const size_t name_start = p;
                        while (p < n && is_ident_char(sksl[p])) ++p;
                        std::string_view name_str = sksl.substr(name_start, p - name_start);

                        skip_ws_and_comments(sksl, p);

                        int arr_count = 1;
                        if (p < n && sksl[p] == '[') {
                            ++p;
                            skip_ws_and_comments(sksl, p);
                            size_t val = 0;
                            while (p < n && std::isdigit(static_cast<unsigned char>(sksl[p]))) {
                                val = val * 10 + static_cast<size_t>(sksl[p] - '0');
                                ++p;
                            }
                            if (val > 0) arr_count = static_cast<int>(val);
                            while (p < n && sksl[p] != ']') ++p;
                            if (p < n) ++p;
                            skip_ws_and_comments(sksl, p);
                        }

                        std::string_view init_expr;
                        if (p < n && sksl[p] == '=') {
                            ++p;
                            const size_t init_start = p;
                            int paren = 0;
                            int brace = 0;
                            while (p < n) {
                                if (sksl[p] == '(') ++paren;
                                else if (sksl[p] == ')') --paren;
                                else if (sksl[p] == '{') ++brace;
                                else if (sksl[p] == '}') --brace;
                                else if (sksl[p] == ';' && paren == 0 && brace == 0) break;
                                ++p;
                            }
                            init_expr = sksl.substr(init_start, p - init_start);
                        }

                        if (p < n && sksl[p] == ';') {
                            if (type_str == "shader") {
                                children.push_back(Child{std::string(name_str), children.size()});
                            } else {
                                const TypeMapping* tm = map_sksl_type(type_str);
                                Uniform u;
                                u.name = std::string(name_str);
                                u.offset = current_offset;
                                u.count = arr_count;
                                if (tm) {
                                    u.type = tm->uniform_type;
                                    u.sizeInBytes = tm->elem_bytes * static_cast<size_t>(arr_count);
                                } else {
                                    u.type = Uniform::Type::Float;
                                    u.sizeInBytes = 4 * static_cast<size_t>(arr_count);
                                }
                                current_offset += u.sizeInBytes;
                                default_uniforms.resize(current_offset, 0);
                                if (!init_expr.empty()) {
                                    parse_numeric_initializer(
                                        init_expr, u.type, default_uniforms.data() + u.offset, u.sizeInBytes);
                                }
                                uniforms.push_back(std::move(u));

                                transformed_code.append("uniform ");
                                transformed_code.append(tm ? tm->glsl_type : type_str);
                                transformed_code.push_back(' ');
                                transformed_code.append(name_str);
                                if (arr_count > 1) {
                                    transformed_code.push_back('[');
                                    transformed_code.append(std::to_string(arr_count));
                                    transformed_code.push_back(']');
                                }
                                transformed_code.push_back(';');
                            }
                            i = p + 1;
                            last_ident = {};
                            prev_non_ws = ';';
                            continue;
                        }
                    }
                }
                transformed_code.append(ident);
            } else if (prev_non_ws != '.' && child_index_for(ident) >= 0) {
                const int cidx = child_index_for(ident);
                size_t p = i;
                skip_ws_and_comments(sksl, p);
                if (p < n && sksl[p] == '.') {
                    ++p;
                    skip_ws_and_comments(sksl, p);
                    if (p + 4 <= n && sksl.substr(p, 4) == "eval" &&
                        (p + 4 == n || !is_ident_char(sksl[p + 4]))) {
                        p += 4;
                        skip_ws_and_comments(sksl, p);
                        if (p < n && sksl[p] == '(') {
                            transformed_code.append("__nisaba_child_eval_");
                            transformed_code.append(std::to_string(cidx));
                            transformed_code.push_back('(');
                            i = p + 1;
                            last_ident = "eval";
                            prev_non_ws = '(';
                            continue;
                        }
                    }
                }
                transformed_code.append(ident);
            } else if (ident == "main") {
                size_t p = i;
                skip_ws_and_comments(sksl, p);
                const bool valid_ret = (last_ident == "vec4" || last_ident == "half4" || last_ident == "float4");
                if (p < n && sksl[p] == '(' && valid_ret) {
                    int paren_depth = 1;
                    size_t k = p + 1;
                    while (k < n && paren_depth > 0) {
                        if (sksl[k] == '(') ++paren_depth;
                        else if (sksl[k] == ')') --paren_depth;
                        else if (sksl[k] == ',' && paren_depth == 1) has_color_in = true;
                        ++k;
                    }
                    transformed_code.append("user_main");
                } else {
                    transformed_code.append(ident);
                }
            } else {
                if (brace_depth == 0 && prev_non_ws != '.' && !last_ident.empty() && last_ident != "return") {
                    size_t p = i;
                    skip_ws_and_comments(sksl, p);
                    if (p < n && sksl[p] == '(' &&
                        std::find(declared_functions.begin(), declared_functions.end(), ident) ==
                            declared_functions.end()) {
                        declared_functions.emplace_back(ident);
                    }
                }
                if (prev_non_ws != '.') {
                    transformed_code.append(map_sksl_ident_type(ident));
                } else {
                    transformed_code.append(ident);
                }
            }

            last_ident = ident;
            prev_non_ws = 'a';
            continue;
        }

        const char ch = sksl[i];
        if (ch == '{') ++brace_depth;
        else if (ch == '}' && brace_depth > 0) --brace_depth;
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            prev_non_ws = ch;
            if (ch == ';' || ch == '}' || ch == ')') last_ident = {};
        }
        transformed_code.push_back(ch);
        ++i;
    }

    const bool is_gles = is_gles_context();
    std::string kFragSource = build_frag_preamble(is_gles);
    for (const auto& ch : children) {
        const std::string fn = "__nisaba_child_eval_" + std::to_string(ch.index);
        kFragSource += "vec4 " + fn + "(vec2 _p, vec4 _c) { return vec4(0.0); }\n";
        kFragSource += "vec4 " + fn + "(vec2 _p) { return vec4(0.0); }\n";
    }
    kFragSource += transformed_code;
    kFragSource += "\n\nvoid main(void) {\n";
    if (has_color_in) {
        kFragSource += "    outColor = user_main(v_localCoord, fcolor);\n";
    } else {
        kFragSource += "    outColor = user_main(v_localCoord);\n";
    }
    kFragSource += "}\n";

    GLuint prog = 0;
    GLuint frag = 0;
    std::string compile_error;
    if (!compile_and_link_fragment(kFragSource, prog, frag, compile_error)) {
        return Result{nullptr, std::move(compile_error)};
    }

    const GLuint vert = 0;
    const int loc_mat = glGetUniformLocation(prog, "u_localMatrix");
    const int loc_view = glGetUniformLocation(prog, "viewSize");
    for (auto& u : uniforms) {
        u.location = glGetUniformLocation(prog, u.name.c_str());
    }

    std::shared_ptr<RuntimeEffect> effect(new RuntimeEffect());
    effect->program_ = prog;
    effect->vert_shader_ = vert;
    effect->frag_shader_ = frag;
    effect->uniforms_ = std::move(uniforms);
    effect->children_ = std::move(children);
    effect->default_uniforms_ = std::move(default_uniforms);
    effect->declared_functions_ = std::move(declared_functions);
    effect->transformed_code_ = std::move(transformed_code);
    effect->has_color_in_ = has_color_in;
    effect->uniform_size_ = current_offset;
    effect->loc_local_mat_ = loc_mat;
    effect->loc_view_size_ = loc_view;

    return Result{effect, ""};
}

std::shared_ptr<Shader> RuntimeEffect::make_shader(
    std::span<const uint8_t> uniforms,
    std::span<const std::shared_ptr<Shader>> children,
    const Transform2D* local_matrix
) const {
    std::vector<uint8_t> buf = default_uniforms_;
    if (buf.size() < uniform_size_) buf.resize(uniform_size_, 0);
    if (!uniforms.empty() && !buf.empty()) {
        std::memcpy(buf.data(), uniforms.data(), std::min(uniforms.size(), buf.size()));
    }
    std::vector<std::shared_ptr<Shader>> ch(children_.size(), nullptr);
    for (size_t i = 0; i < std::min(children.size(), children_.size()); ++i) {
        ch[i] = children[i];
    }
    Transform2D lm = local_matrix ? *local_matrix : Transform2D::identity();
    return std::make_shared<RuntimeShader>(shared_from_this(), std::move(buf), lm, std::move(ch));
}

RuntimeShader::RuntimeShader(std::shared_ptr<const RuntimeEffect> effect,
                             std::vector<uint8_t> uniformData,
                             Transform2D localMatrix,
                             std::vector<std::shared_ptr<Shader>> children)
    : effect_(std::move(effect)),
      uniform_data_(std::move(uniformData)),
      local_matrix_(localMatrix),
      children_(std::move(children)) {
    to_inv_mat3(local_matrix_, inv_m3_);
    if (effect_ && !effect_->children().empty()) {
        bool has_any_child = false;
        for (const auto& ch : children_) {
            if (ch != nullptr) {
                has_any_child = true;
                break;
            }
        }
        if (has_any_child) {
            bound_program_ = build_bound_program(*effect_, children_);
        }
    }
}

std::shared_ptr<Shader> RuntimeShader::with_local_transform(const Transform2D& local_matrix) const {
    return std::make_shared<RuntimeShader>(
        effect_,
        uniform_data_,
        local_matrix_.multiply(local_matrix),
        children_
    );
}

bool RuntimeShader::bind_shader(const float view_size[2], const Transform2D& canvas_xform) const {
    if (!effect_) return false;

    const GLuint prog = bound_program_ ? bound_program_->program : effect_->programId();
    if (prog == 0) return false;

    glUseProgram(prog);

    const int loc_view = bound_program_ ? bound_program_->loc_view_size : effect_->viewSizeLoc();
    const int loc_mat = bound_program_ ? bound_program_->loc_local_mat : effect_->localMatrixLoc();

    if (loc_view >= 0 && view_size) {
        glUniform2fv(loc_view, 1, view_size);
    }

    if (loc_mat >= 0) {
        float inv_combined[9];
        const Transform2D total = local_matrix_.multiply(canvas_xform);
        to_inv_mat3(total, inv_combined);
        glUniformMatrix3fv(loc_mat, 1, GL_FALSE, inv_combined);
    }

    const auto& eff_uniforms = effect_->uniforms();
    for (size_t idx = 0; idx < eff_uniforms.size(); ++idx) {
        const auto& u = eff_uniforms[idx];
        const int loc = (bound_program_ && idx < bound_program_->root_uniform_locs.size())
            ? bound_program_->root_uniform_locs[idx]
            : u.location;
        if (loc < 0 || u.offset + u.sizeInBytes > uniform_data_.size()) continue;
        upload_uniform(loc, u.type, u.count, uniform_data_.data() + u.offset);
    }

    if (bound_program_) {
        for (const auto& bm : bound_program_->child_matrices) {
            if (bm.location >= 0) {
                glUniformMatrix3fv(bm.location, 1, GL_FALSE, bm.inv_m3);
            }
        }
        for (const auto& bu : bound_program_->child_uniforms) {
            upload_uniform(bu.location, bu.type, bu.count, bu.data.data());
        }
    }

    return true;
}

void RuntimeShader::bind(const float* viewSize) const {
    (void)bind_shader(viewSize, Transform2D::identity());
}

RuntimeShaderBuilder::RuntimeShaderBuilder(std::shared_ptr<RuntimeEffect> effect)
    : effect_(std::move(effect)) {
    if (effect_) {
        uniform_data_ = effect_->default_uniform_data();
        if (uniform_data_.size() < effect_->uniformSize()) {
            uniform_data_.resize(effect_->uniformSize(), 0);
        }
        children_.resize(effect_->children().size(), nullptr);
    }
}

void RuntimeShaderBuilder::writeUniformData(size_t offset, const void* data, size_t size) {
    if (offset + size <= uniform_data_.size() && data != nullptr) {
        std::memcpy(uniform_data_.data() + offset, data, size);
    }
}

void RuntimeShaderBuilder::set_child(size_t index, std::shared_ptr<Shader> child_shader) {
    if (index < children_.size()) {
        children_[index] = std::move(child_shader);
    }
}

void RuntimeShaderBuilder::set_child(const char* name, std::shared_ptr<Shader> child_shader) {
    if (!effect_ || !name) return;
    if (const auto* slot = effect_->find_child(name)) {
        set_child(slot->index, std::move(child_shader));
    }
}

std::shared_ptr<Shader> RuntimeShaderBuilder::makeShader(const Transform2D* localMatrix) {
    Transform2D lm = localMatrix ? *localMatrix : Transform2D::identity();
    return std::make_shared<RuntimeShader>(effect_, uniform_data_, lm, children_);
}

std::shared_ptr<Shader> RuntimeShaderBuilder::makeShader(const nisaba::Transform* localMatrix) {
    Transform2D lm = localMatrix ? Transform2D(*localMatrix) : Transform2D::identity();
    return std::make_shared<RuntimeShader>(effect_, uniform_data_, lm, children_);
}

} // namespace nisaba::gpu
