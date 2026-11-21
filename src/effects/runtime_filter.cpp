#include "nisaba/effects/runtime_filter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nisaba::effects {

namespace {

uint64_t fnv1a_hash(std::string_view s) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

bool is_ident_start(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void skip_ws_and_comments(std::string_view src, size_t& i) noexcept {
    const size_t n = src.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(src[i]))) {
            ++i;
        } else if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') ++i;
        } else if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
        } else {
            break;
        }
    }
}

struct TypeMap {
    const char* glsl;
    RuntimeUniformType utype;
    size_t bytes;
};

const TypeMap* map_uniform_type(std::string_view t) noexcept {
    static constexpr TypeMap kFloat{"float", RuntimeUniformType::Float, 4};
    static constexpr TypeMap kVec2{"vec2", RuntimeUniformType::Float2, 8};
    static constexpr TypeMap kVec3{"vec3", RuntimeUniformType::Float3, 12};
    static constexpr TypeMap kVec4{"vec4", RuntimeUniformType::Float4, 16};
    static constexpr TypeMap kMat2{"mat2", RuntimeUniformType::Mat2, 16};
    static constexpr TypeMap kMat3{"mat3", RuntimeUniformType::Mat3, 36};
    static constexpr TypeMap kMat4{"mat4", RuntimeUniformType::Mat4, 64};
    static constexpr TypeMap kInt{"int", RuntimeUniformType::Int, 4};
    if (t == "float" || t == "half") return &kFloat;
    if (t == "vec2" || t == "float2" || t == "half2") return &kVec2;
    if (t == "vec3" || t == "float3" || t == "half3") return &kVec3;
    if (t == "vec4" || t == "float4" || t == "half4") return &kVec4;
    if (t == "mat2" || t == "float2x2" || t == "half2x2") return &kMat2;
    if (t == "mat3" || t == "float3x3" || t == "half3x3") return &kMat3;
    if (t == "mat4" || t == "float4x4" || t == "half4x4") return &kMat4;
    if (t == "int") return &kInt;
    return nullptr;
}

std::string_view map_ident_type(std::string_view id) noexcept {
    if (const auto* tm = map_uniform_type(id)) return tm->glsl;
    if (id == "int2") return "ivec2";
    if (id == "int3") return "ivec3";
    if (id == "int4") return "ivec4";
    if (id == "bool2") return "bvec2";
    if (id == "bool3") return "bvec3";
    if (id == "bool4") return "bvec4";
    return id;
}

void parse_default_initializer(std::string_view expr, RuntimeUniformType type, uint8_t* dst, size_t max_bytes) {
    std::vector<double> nums;
    const size_t n = expr.size();
    size_t i = 0;
    while (i < n) {
        const char ch = expr[i];
        if (is_ident_start(ch)) {
            while (i < n && is_ident_char(expr[i])) ++i;
            continue;
        }
        const bool signed_num = (ch == '-' || ch == '+') && (i + 1 < n) &&
                                (std::isdigit(static_cast<unsigned char>(expr[i + 1])) || expr[i + 1] == '.');
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || signed_num) {
            std::string tmp(expr.substr(i, std::min<size_t>(48, n - i)));
            char* end_p = nullptr;
            double val = std::strtod(tmp.c_str(), &end_p);
            if (end_p && end_p != tmp.c_str()) {
                size_t consumed = static_cast<size_t>(end_p - tmp.c_str());
                if (i + consumed < n && (expr[i + consumed] == 'f' || expr[i + consumed] == 'F')) ++consumed;
                i += consumed;
                nums.push_back(val);
                continue;
            }
        }
        ++i;
    }
    if (nums.empty()) return;
    if (type == RuntimeUniformType::Int) {
        int32_t* out = reinterpret_cast<int32_t*>(dst);
        const size_t cnt = max_bytes / sizeof(int32_t);
        for (size_t k = 0; k < std::min(cnt, nums.size()); ++k) out[k] = static_cast<int32_t>(nums[k]);
    } else {
        float* out = reinterpret_cast<float*>(dst);
        const size_t cnt = max_bytes / sizeof(float);
        if (nums.size() == 1 && cnt > 1 && cnt <= 4) {
            for (size_t k = 0; k < cnt; ++k) out[k] = static_cast<float>(nums[0]);
        } else {
            for (size_t k = 0; k < std::min(cnt, nums.size()); ++k) out[k] = static_cast<float>(nums[k]);
        }
    }
}

// =========================================================================
// Lightweight RTTI-free CPU Evaluator for RuntimeFilter Shaders
// =========================================================================

struct Val {
    int dim{1}; // 1=float/scalar, 2=vec2, 3=vec3, 4=vec4
    float v[4]{0.0f, 0.0f, 0.0f, 0.0f};

    static Val scalar(float s) noexcept { Val r; r.dim = 1; r.v[0] = s; return r; }
    static Val vec2(float x, float y) noexcept { Val r; r.dim = 2; r.v[0] = x; r.v[1] = y; return r; }
    static Val vec3(float x, float y, float z) noexcept { Val r; r.dim = 3; r.v[0] = x; r.v[1] = y; r.v[2] = z; return r; }
    static Val vec4(float x, float y, float z, float w) noexcept { Val r; r.dim = 4; r.v[0] = x; r.v[1] = y; r.v[2] = z; r.v[3] = w; return r; }

    float get(int i) const noexcept { return (dim == 1) ? v[0] : v[std::clamp(i, 0, dim - 1)]; }
};

enum class TokKind : uint8_t {
    Eof, Ident, Number,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semi, Dot, Question, Colon,
    Plus, Minus, Star, Slash,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign,
    Eq, Neq, Lt, Le, Gt, Ge,
    AndAnd, OrOr, Bang, PlusPlus, MinusMinus
};

struct Tok {
    TokKind kind{TokKind::Eof};
    std::string text{};
    float num{0.0f};
};

std::vector<Tok> tokenize(std::string_view src) {
    std::vector<Tok> out;
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        skip_ws_and_comments(src, i);
        if (i >= n) break;
        const char c = src[i];
        if (is_ident_start(c)) {
            const size_t s = i;
            while (i < n && is_ident_char(src[i])) ++i;
            out.push_back(Tok{TokKind::Ident, std::string(src.substr(s, i - s)), 0.0f});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            const size_t s = i;
            bool seen_dot = (c == '.');
            ++i;
            while (i < n && (std::isdigit(static_cast<unsigned char>(src[i])) || (!seen_dot && src[i] == '.'))) {
                if (src[i] == '.') seen_dot = true;
                ++i;
            }
            if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                ++i;
                if (i < n && (src[i] == '+' || src[i] == '-')) ++i;
                while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
            }
            const std::string num_str(src.substr(s, i - s));
            if (i < n && (src[i] == 'f' || src[i] == 'F' || src[i] == 'u' || src[i] == 'U')) ++i;
            out.push_back(Tok{TokKind::Number, num_str, std::strtof(num_str.c_str(), nullptr)});
            continue;
        }
        auto two = [&](char a, char b, TokKind k) -> bool {
            if (i + 1 < n && src[i] == a && src[i + 1] == b) {
                out.push_back(Tok{k, {}, 0.0f});
                i += 2;
                return true;
            }
            return false;
        };
        if (two('+', '=', TokKind::PlusAssign) || two('-', '=', TokKind::MinusAssign) ||
            two('*', '=', TokKind::StarAssign) || two('/', '=', TokKind::SlashAssign) ||
            two('=', '=', TokKind::Eq) || two('!', '=', TokKind::Neq) ||
            two('<', '=', TokKind::Le) || two('>', '=', TokKind::Ge) ||
            two('&', '&', TokKind::AndAnd) || two('|', '|', TokKind::OrOr) ||
            two('+', '+', TokKind::PlusPlus) || two('-', '-', TokKind::MinusMinus)) {
            continue;
        }
        TokKind k = TokKind::Eof;
        switch (c) {
            case '(': k = TokKind::LParen; break;
            case ')': k = TokKind::RParen; break;
            case '{': k = TokKind::LBrace; break;
            case '}': k = TokKind::RBrace; break;
            case '[': k = TokKind::LBracket; break;
            case ']': k = TokKind::RBracket; break;
            case ',': k = TokKind::Comma; break;
            case ';': k = TokKind::Semi; break;
            case '.': k = TokKind::Dot; break;
            case '?': k = TokKind::Question; break;
            case ':': k = TokKind::Colon; break;
            case '+': k = TokKind::Plus; break;
            case '-': k = TokKind::Minus; break;
            case '*': k = TokKind::Star; break;
            case '/': k = TokKind::Slash; break;
            case '=': k = TokKind::Assign; break;
            case '<': k = TokKind::Lt; break;
            case '>': k = TokKind::Gt; break;
            case '!': k = TokKind::Bang; break;
            default: ++i; continue;
        }
        out.push_back(Tok{k, {}, 0.0f});
        ++i;
    }
    out.push_back(Tok{TokKind::Eof, {}, 0.0f});
    return out;
}

bool is_type_name(const std::string& s) noexcept {
    return s == "float" || s == "int" || s == "bool" || s == "void" ||
           s == "vec2" || s == "vec3" || s == "vec4" ||
           s == "ivec2" || s == "ivec3" || s == "ivec4" ||
           s == "bvec2" || s == "bvec3" || s == "bvec4" ||
           s == "half" || s == "half2" || s == "half3" || s == "half4" ||
           s == "float2" || s == "float3" || s == "float4";
}

int type_dim(const std::string& s) noexcept {
    if (s == "vec2" || s == "ivec2" || s == "bvec2" || s == "half2" || s == "float2") return 2;
    if (s == "vec3" || s == "ivec3" || s == "bvec3" || s == "half3" || s == "float3") return 3;
    if (s == "vec4" || s == "ivec4" || s == "bvec4" || s == "half4" || s == "float4") return 4;
    return 1;
}

int swizzle_idx(char c) noexcept {
    switch (c) {
        case 'x': case 'r': case 's': return 0;
        case 'y': case 'g': case 't': return 1;
        case 'z': case 'b': case 'p': return 2;
        case 'w': case 'a': case 'q': return 3;
        default: return -1;
    }
}

Val sample_src_bilinear(
    const PixmapRef& src,
    float local_x,
    float local_y,
    int32_t roi_ox,
    int32_t roi_oy,
    const Transform& ts
) noexcept {
    Point dp = Point::from_xy(local_x, local_y);
    ts.map_point(dp);
    // Match GPU normalized UV texture sampling: texel center is at (x + 0.5, y + 0.5)
    const float rx = dp.x - static_cast<float>(roi_ox) - 0.5f;
    const float ry = dp.y - static_cast<float>(roi_oy) - 0.5f;
    const int32_t w = static_cast<int32_t>(src.width());
    const int32_t h = static_cast<int32_t>(src.height());
    if (dp.x < static_cast<float>(roi_ox) || dp.x > static_cast<float>(roi_ox + w) ||
        dp.y < static_cast<float>(roi_oy) || dp.y > static_cast<float>(roi_oy + h)) {
        return Val::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const int32_t x0 = static_cast<int32_t>(std::floor(rx));
    const int32_t y0 = static_cast<int32_t>(std::floor(ry));
    const float fx = rx - static_cast<float>(x0);
    const float fy = ry - static_cast<float>(y0);

    auto fetch = [&](int32_t x, int32_t y) noexcept -> Val {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        const auto p = src.row(static_cast<size_t>(y))[x];
        constexpr float inv = 1.0f / 255.0f;
        return Val::vec4(p.r * inv, p.g * inv, p.b * inv, p.a * inv);
    };

    const Val p00 = fetch(x0, y0);
    const Val p10 = fetch(x0 + 1, y0);
    const Val p01 = fetch(x0, y0 + 1);
    const Val p11 = fetch(x0 + 1, y0 + 1);

    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;

    return Val::vec4(
        p00.v[0] * w00 + p10.v[0] * w10 + p01.v[0] * w01 + p11.v[0] * w11,
        p00.v[1] * w00 + p10.v[1] * w10 + p01.v[1] * w01 + p11.v[1] * w11,
        p00.v[2] * w00 + p10.v[2] * w10 + p01.v[2] * w01 + p11.v[2] * w11,
        p00.v[3] * w00 + p10.v[3] * w10 + p01.v[3] * w01 + p11.v[3] * w11
    );
}

struct FuncDef {
    std::string name;
    std::vector<std::string> params;
    size_t body_start{0};
    size_t body_end{0};
};

struct CpuEvalContext {
    const std::vector<Tok>& toks;
    const std::unordered_map<std::string, Val>& uniforms;
    const std::unordered_map<std::string, FuncDef>& funcs;
    const PixmapRef& src;
    int32_t roi_ox;
    int32_t roi_oy;
    const Transform& ts;
    size_t input_child_idx;
    bool has_input_child;

    std::vector<std::unordered_map<std::string, Val>> scopes;
    bool returned{false};
    Val return_val{};
    size_t pos{0};

    const Tok& peek(size_t offset = 0) const noexcept {
        return (pos + offset < toks.size()) ? toks[pos + offset] : toks.back();
    }
    const Tok& advance() noexcept {
        const Tok& t = peek();
        if (pos + 1 < toks.size()) ++pos;
        return t;
    }
    bool match(TokKind k) noexcept {
        if (peek().kind == k) {
            advance();
            return true;
        }
        return false;
    }

    Val* find_var(const std::string& name) {
        for (size_t i = scopes.size(); i > 0; --i) {
            auto it = scopes[i - 1].find(name);
            if (it != scopes[i - 1].end()) return &it->second;
        }
        return nullptr;
    }

    Val lookup_var(const std::string& name) {
        if (Val* v = find_var(name)) return *v;
        if (auto it = uniforms.find(name); it != uniforms.end()) return it->second;
        return Val::scalar(0.0f);
    }

    Val apply_bin(TokKind op, const Val& a, const Val& b) const noexcept {
        const int d = std::max(a.dim, b.dim);
        Val r;
        r.dim = d;
        for (int i = 0; i < d; ++i) {
            const float x = a.get(i);
            const float y = b.get(i);
            switch (op) {
                case TokKind::Plus:  r.v[i] = x + y; break;
                case TokKind::Minus: r.v[i] = x - y; break;
                case TokKind::Star:  r.v[i] = x * y; break;
                case TokKind::Slash: r.v[i] = (std::abs(y) > 1e-12f) ? (x / y) : 0.0f; break;
                default: r.v[i] = 0.0f; break;
            }
        }
        return r;
    }

    Val call_builtin(const std::string& name, const std::vector<Val>& args) {
        // Child sampler call: __nisaba_child_eval_N(vec2 p)
        if (name.rfind("__nisaba_child_eval_", 0) == 0) {
            const size_t cidx = static_cast<size_t>(std::strtoull(name.c_str() + 20, nullptr, 10));
            if (has_input_child && cidx == input_child_idx && !args.empty()) {
                return sample_src_bilinear(src, args[0].get(0), args[0].get(1), roi_ox, roi_oy, ts);
            }
            return Val::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        // Vector constructors
        if (is_type_name(name)) {
            const int d = type_dim(name);
            if (args.empty()) {
                Val r; r.dim = d; return r;
            }
            if (args.size() == 1 && args[0].dim == 1) {
                Val r; r.dim = d;
                for (int i = 0; i < d; ++i) r.v[i] = args[0].v[0];
                return r;
            }
            Val r; r.dim = d;
            int out_i = 0;
            for (const auto& a : args) {
                for (int k = 0; k < a.dim && out_i < d; ++k) {
                    r.v[out_i++] = a.v[k];
                }
            }
            return r;
        }

        auto unary = [&](auto fn) -> Val {
            if (args.empty()) return Val::scalar(0.0f);
            Val r; r.dim = args[0].dim;
            for (int i = 0; i < r.dim; ++i) r.v[i] = fn(args[0].v[i]);
            return r;
        };
        auto binary = [&](auto fn) -> Val {
            if (args.size() < 2) return Val::scalar(0.0f);
            const int d = std::max(args[0].dim, args[1].dim);
            Val r; r.dim = d;
            for (int i = 0; i < d; ++i) r.v[i] = fn(args[0].get(i), args[1].get(i));
            return r;
        };

        if (name == "abs") return unary([](float x) { return std::abs(x); });
        if (name == "floor") return unary([](float x) { return std::floor(x); });
        if (name == "ceil") return unary([](float x) { return std::ceil(x); });
        if (name == "fract") return unary([](float x) { return x - std::floor(x); });
        if (name == "sin") return unary([](float x) { return std::sin(x); });
        if (name == "cos") return unary([](float x) { return std::cos(x); });
        if (name == "tan") return unary([](float x) { return std::tan(x); });
        if (name == "sqrt") return unary([](float x) { return std::sqrt(std::max(0.0f, x)); });
        if (name == "exp") return unary([](float x) { return std::exp(x); });
        if (name == "log") return unary([](float x) { return std::log(std::max(1e-8f, x)); });
        if (name == "sign") return unary([](float x) { return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f); });
        if (name == "saturate") return unary([](float x) { return std::clamp(x, 0.0f, 1.0f); });
        if (name == "min") return binary([](float a, float b) { return std::min(a, b); });
        if (name == "max") return binary([](float a, float b) { return std::max(a, b); });
        if (name == "pow") return binary([](float a, float b) { return std::pow(std::max(0.0f, a), b); });
        if (name == "mod") return binary([](float a, float b) { return (std::abs(b) > 1e-12f) ? (a - b * std::floor(a / b)) : 0.0f; });
        if (name == "step") return binary([](float e, float x) { return (x < e) ? 0.0f : 1.0f; });
        if (name == "atan") {
            if (args.size() >= 2) return binary([](float y, float x) { return std::atan2(y, x); });
            return unary([](float x) { return std::atan(x); });
        }
        if (name == "clamp" && args.size() >= 3) {
            const int d = std::max({args[0].dim, args[1].dim, args[2].dim});
            Val r; r.dim = d;
            for (int i = 0; i < d; ++i) r.v[i] = std::clamp(args[0].get(i), args[1].get(i), args[2].get(i));
            return r;
        }
        if (name == "mix" && args.size() >= 3) {
            const int d = std::max({args[0].dim, args[1].dim, args[2].dim});
            Val r; r.dim = d;
            for (int i = 0; i < d; ++i) {
                float t = args[2].get(i);
                r.v[i] = args[0].get(i) * (1.0f - t) + args[1].get(i) * t;
            }
            return r;
        }
        if (name == "smoothstep" && args.size() >= 3) {
            const int d = std::max({args[0].dim, args[1].dim, args[2].dim});
            Val r; r.dim = d;
            for (int i = 0; i < d; ++i) {
                float e0 = args[0].get(i), e1 = args[1].get(i), x = args[2].get(i);
                float den = e1 - e0;
                float t = (std::abs(den) > 1e-8f) ? std::clamp((x - e0) / den, 0.0f, 1.0f) : 0.0f;
                r.v[i] = t * t * (3.0f - 2.0f * t);
            }
            return r;
        }
        if (name == "dot" && args.size() >= 2) {
            const int d = std::max(args[0].dim, args[1].dim);
            float sum = 0.0f;
            for (int i = 0; i < d; ++i) sum += args[0].get(i) * args[1].get(i);
            return Val::scalar(sum);
        }
        if (name == "length" && !args.empty()) {
            float s = 0.0f;
            for (int i = 0; i < args[0].dim; ++i) s += args[0].v[i] * args[0].v[i];
            return Val::scalar(std::sqrt(s));
        }
        if (name == "distance" && args.size() >= 2) {
            const int d = std::max(args[0].dim, args[1].dim);
            float s = 0.0f;
            for (int i = 0; i < d; ++i) {
                float df = args[0].get(i) - args[1].get(i);
                s += df * df;
            }
            return Val::scalar(std::sqrt(s));
        }
        if (name == "normalize" && !args.empty()) {
            float s = 0.0f;
            for (int i = 0; i < args[0].dim; ++i) s += args[0].v[i] * args[0].v[i];
            float inv = (s > 1e-12f) ? (1.0f / std::sqrt(s)) : 0.0f;
            Val r; r.dim = args[0].dim;
            for (int i = 0; i < r.dim; ++i) r.v[i] = args[0].v[i] * inv;
            return r;
        }
        if (name == "unpremul" && !args.empty()) {
            float a = args[0].get(3);
            if (a > 0.0001f) {
                return Val::vec4(args[0].get(0) / a, args[0].get(1) / a, args[0].get(2) / a, a);
            }
            return Val::vec4(0.0f, 0.0f, 0.0f, a);
        }
        if (name == "toLinearSrgb" && !args.empty()) {
            auto conv = [](float c) { return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f); };
            Val r = args[0];
            for (int i = 0; i < std::min(3, r.dim); ++i) r.v[i] = conv(r.v[i]);
            return r;
        }
        if (name == "fromLinearSrgb" && !args.empty()) {
            auto conv = [](float c) { return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(std::max(c, 0.0f), 1.0f / 2.4f) - 0.055f); };
            Val r = args[0];
            for (int i = 0; i < std::min(3, r.dim); ++i) r.v[i] = conv(r.v[i]);
            return r;
        }

        // User-defined helper function
        if (auto it = funcs.find(name); it != funcs.end()) {
            const FuncDef& fn = it->second;
            const size_t saved_pos = pos;
            const bool saved_ret = returned;
            const Val saved_rval = return_val;
            scopes.emplace_back();
            for (size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
                scopes.back()[fn.params[i]] = args[i];
            }
            returned = false;
            return_val = Val::scalar(0.0f);
            pos = fn.body_start;
            exec_block();
            Val res = return_val;
            scopes.pop_back();
            pos = saved_pos;
            returned = saved_ret;
            return_val = saved_rval;
            return res;
        }
        return Val::scalar(0.0f);
    }

    Val eval_primary() {
        if (peek().kind == TokKind::Number) {
            return Val::scalar(advance().num);
        }
        if (peek().kind == TokKind::Ident) {
            std::string id = advance().text;
            if (id == "true") return Val::scalar(1.0f);
            if (id == "false") return Val::scalar(0.0f);
            if (match(TokKind::LParen)) {
                std::vector<Val> args;
                if (peek().kind != TokKind::RParen) {
                    do {
                        args.push_back(eval_expr());
                    } while (match(TokKind::Comma));
                }
                match(TokKind::RParen);
                return call_builtin(id, args);
            }
            return lookup_var(id);
        }
        if (match(TokKind::LParen)) {
            Val v = eval_expr();
            match(TokKind::RParen);
            return v;
        }
        advance();
        return Val::scalar(0.0f);
    }

    Val eval_postfix() {
        Val v = eval_primary();
        while (true) {
            if (match(TokKind::Dot)) {
                if (peek().kind == TokKind::Ident) {
                    std::string sw = advance().text;
                    Val r;
                    r.dim = std::clamp(static_cast<int>(sw.size()), 1, 4);
                    for (int i = 0; i < r.dim; ++i) {
                        int idx = swizzle_idx(sw[i]);
                        r.v[i] = (idx >= 0) ? v.get(idx) : 0.0f;
                    }
                    v = r;
                }
            } else if (match(TokKind::LBracket)) {
                Val idx_v = eval_expr();
                match(TokKind::RBracket);
                v = Val::scalar(v.get(static_cast<int>(idx_v.v[0])));
            } else {
                break;
            }
        }
        return v;
    }

    Val eval_unary() {
        if (match(TokKind::Minus)) {
            Val v = eval_unary();
            for (int i = 0; i < v.dim; ++i) v.v[i] = -v.v[i];
            return v;
        }
        if (match(TokKind::Plus)) return eval_unary();
        if (match(TokKind::Bang)) {
            Val v = eval_unary();
            return Val::scalar(v.v[0] == 0.0f ? 1.0f : 0.0f);
        }
        return eval_postfix();
    }

    Val eval_mul() {
        Val v = eval_unary();
        while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash) {
            TokKind op = advance().kind;
            v = apply_bin(op, v, eval_unary());
        }
        return v;
    }

    Val eval_add() {
        Val v = eval_mul();
        while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
            TokKind op = advance().kind;
            v = apply_bin(op, v, eval_mul());
        }
        return v;
    }

    Val eval_rel() {
        Val v = eval_add();
        while (peek().kind == TokKind::Lt || peek().kind == TokKind::Le ||
               peek().kind == TokKind::Gt || peek().kind == TokKind::Ge) {
            TokKind op = advance().kind;
            Val r = eval_add();
            bool b = false;
            if (op == TokKind::Lt) b = v.v[0] < r.v[0];
            else if (op == TokKind::Le) b = v.v[0] <= r.v[0];
            else if (op == TokKind::Gt) b = v.v[0] > r.v[0];
            else if (op == TokKind::Ge) b = v.v[0] >= r.v[0];
            v = Val::scalar(b ? 1.0f : 0.0f);
        }
        return v;
    }

    Val eval_eq() {
        Val v = eval_rel();
        while (peek().kind == TokKind::Eq || peek().kind == TokKind::Neq) {
            TokKind op = advance().kind;
            Val r = eval_rel();
            bool b = (op == TokKind::Eq) ? (v.v[0] == r.v[0]) : (v.v[0] != r.v[0]);
            v = Val::scalar(b ? 1.0f : 0.0f);
        }
        return v;
    }

    Val eval_and() {
        Val v = eval_eq();
        while (match(TokKind::AndAnd)) {
            Val r = eval_eq();
            v = Val::scalar((v.v[0] != 0.0f && r.v[0] != 0.0f) ? 1.0f : 0.0f);
        }
        return v;
    }

    Val eval_or() {
        Val v = eval_and();
        while (match(TokKind::OrOr)) {
            Val r = eval_and();
            v = Val::scalar((v.v[0] != 0.0f || r.v[0] != 0.0f) ? 1.0f : 0.0f);
        }
        return v;
    }

    Val eval_ternary() {
        Val cond = eval_or();
        if (match(TokKind::Question)) {
            Val a = eval_expr();
            match(TokKind::Colon);
            Val b = eval_ternary();
            return (cond.v[0] != 0.0f) ? a : b;
        }
        return cond;
    }

    Val eval_expr() {
        // Check assignment: ident [ .swizzle ] (=|+=|-=|*=|/=) expr
        if (peek().kind == TokKind::Ident) {
            size_t look = 1;
            std::string sw;
            if (peek(look).kind == TokKind::Dot && peek(look + 1).kind == TokKind::Ident) {
                sw = peek(look + 1).text;
                look += 2;
            }
            TokKind op = peek(look).kind;
            if (op == TokKind::Assign || op == TokKind::PlusAssign ||
                op == TokKind::MinusAssign || op == TokKind::StarAssign || op == TokKind::SlashAssign) {
                std::string name = advance().text;
                if (!sw.empty()) { advance(); advance(); }
                advance(); // op
                Val rhs = eval_expr();
                Val* dst = find_var(name);
                if (!dst && !scopes.empty()) {
                    dst = &scopes.back()[name];
                    *dst = rhs;
                }
                if (dst) {
                    if (sw.empty()) {
                        if (op == TokKind::Assign) *dst = rhs;
                        else if (op == TokKind::PlusAssign) *dst = apply_bin(TokKind::Plus, *dst, rhs);
                        else if (op == TokKind::MinusAssign) *dst = apply_bin(TokKind::Minus, *dst, rhs);
                        else if (op == TokKind::StarAssign) *dst = apply_bin(TokKind::Star, *dst, rhs);
                        else if (op == TokKind::SlashAssign) *dst = apply_bin(TokKind::Slash, *dst, rhs);
                    } else {
                        for (size_t k = 0; k < sw.size(); ++k) {
                            int idx = swizzle_idx(sw[k]);
                            if (idx < 0 || idx >= dst->dim) continue;
                            float rv = rhs.get(static_cast<int>(k));
                            if (op == TokKind::Assign) dst->v[idx] = rv;
                            else if (op == TokKind::PlusAssign) dst->v[idx] += rv;
                            else if (op == TokKind::MinusAssign) dst->v[idx] -= rv;
                            else if (op == TokKind::StarAssign) dst->v[idx] *= rv;
                            else if (op == TokKind::SlashAssign) dst->v[idx] = (std::abs(rv) > 1e-12f) ? (dst->v[idx] / rv) : 0.0f;
                        }
                    }
                    return *dst;
                }
                return rhs;
            }
            if (peek(1).kind == TokKind::PlusPlus || peek(1).kind == TokKind::MinusMinus) {
                std::string name = advance().text;
                const bool inc = (advance().kind == TokKind::PlusPlus);
                if (Val* dst = find_var(name)) {
                    Val old = *dst;
                    dst->v[0] += inc ? 1.0f : -1.0f;
                    return old;
                }
                return Val::scalar(0.0f);
            }
        }
        if (peek().kind == TokKind::PlusPlus || peek().kind == TokKind::MinusMinus) {
            const bool inc = (advance().kind == TokKind::PlusPlus);
            if (peek().kind == TokKind::Ident) {
                std::string name = advance().text;
                if (Val* dst = find_var(name)) {
                    dst->v[0] += inc ? 1.0f : -1.0f;
                    return *dst;
                }
            }
            return Val::scalar(0.0f);
        }
        return eval_ternary();
    }

    void skip_stmt() {
        if (match(TokKind::LBrace)) {
            int depth = 1;
            while (peek().kind != TokKind::Eof && depth > 0) {
                if (peek().kind == TokKind::LBrace) ++depth;
                else if (peek().kind == TokKind::RBrace) --depth;
                advance();
            }
            return;
        }
        if (peek().kind == TokKind::Ident && peek().text == "if") {
            advance();
            if (match(TokKind::LParen)) {
                int d = 1;
                while (peek().kind != TokKind::Eof && d > 0) {
                    if (peek().kind == TokKind::LParen) ++d;
                    else if (peek().kind == TokKind::RParen) --d;
                    advance();
                }
            }
            skip_stmt();
            if (peek().kind == TokKind::Ident && peek().text == "else") {
                advance();
                skip_stmt();
            }
            return;
        }
        while (peek().kind != TokKind::Eof && peek().kind != TokKind::Semi && peek().kind != TokKind::RBrace) {
            advance();
        }
        match(TokKind::Semi);
    }

    void exec_stmt() {
        if (returned || peek().kind == TokKind::Eof) return;
        if (peek().kind == TokKind::LBrace) {
            exec_block();
            return;
        }
        if (peek().kind == TokKind::Semi) {
            advance();
            return;
        }
        if (peek().kind == TokKind::Ident && peek().text == "return") {
            advance();
            if (peek().kind != TokKind::Semi) {
                return_val = eval_expr();
            }
            match(TokKind::Semi);
            returned = true;
            return;
        }
        if (peek().kind == TokKind::Ident && peek().text == "if") {
            advance();
            match(TokKind::LParen);
            Val cond = eval_expr();
            match(TokKind::RParen);
            if (cond.v[0] != 0.0f) {
                exec_stmt();
                if (peek().kind == TokKind::Ident && peek().text == "else") {
                    advance();
                    skip_stmt();
                }
            } else {
                skip_stmt();
                if (peek().kind == TokKind::Ident && peek().text == "else") {
                    advance();
                    exec_stmt();
                }
            }
            return;
        }
        if (peek().kind == TokKind::Ident && peek().text == "for") {
            advance();
            match(TokKind::LParen);
            scopes.emplace_back();
            if (peek().kind != TokKind::Semi) {
                exec_stmt(); // init (consumes ;)
            } else {
                advance();
            }
            const size_t cond_pos = pos;
            while (peek().kind != TokKind::Eof && peek().kind != TokKind::Semi) advance();
            match(TokKind::Semi);
            const size_t step_pos = pos;
            int pd = 1;
            while (peek().kind != TokKind::Eof && pd > 0) {
                if (peek().kind == TokKind::LParen) ++pd;
                else if (peek().kind == TokKind::RParen) {
                    --pd;
                    if (pd == 0) break;
                }
                advance();
            }
            match(TokKind::RParen);
            const size_t body_pos = pos;
            int guard = 0;
            while (!returned && guard++ < 256) {
                pos = cond_pos;
                bool cont = true;
                if (peek().kind != TokKind::Semi) {
                    cont = (eval_expr().v[0] != 0.0f);
                }
                if (!cont) break;
                pos = body_pos;
                exec_stmt();
                if (returned) break;
                pos = step_pos;
                if (peek().kind != TokKind::RParen) {
                    eval_expr();
                }
            }
            pos = body_pos;
            skip_stmt();
            scopes.pop_back();
            return;
        }
        // Local variable declaration: [const] <type> <name> [= expr] [, ...];
        size_t save_p = pos;
        if (peek().kind == TokKind::Ident && peek().text == "const") advance();
        if (peek().kind == TokKind::Ident && is_type_name(peek().text) && peek(1).kind == TokKind::Ident) {
            const int dim = type_dim(advance().text);
            do {
                std::string name = advance().text;
                Val init;
                init.dim = dim;
                if (match(TokKind::Assign)) {
                    Val rhs = eval_expr();
                    if (rhs.dim == dim) {
                        init = rhs;
                    } else {
                        for (int i = 0; i < dim; ++i) init.v[i] = rhs.get(i);
                    }
                }
                if (!scopes.empty()) scopes.back()[name] = init;
            } while (match(TokKind::Comma));
            match(TokKind::Semi);
            return;
        }
        pos = save_p;
        eval_expr();
        match(TokKind::Semi);
    }

    void exec_block() {
        if (!match(TokKind::LBrace)) return;
        scopes.emplace_back();
        while (!returned && peek().kind != TokKind::Eof && peek().kind != TokKind::RBrace) {
            exec_stmt();
        }
        match(TokKind::RBrace);
        scopes.pop_back();
    }
};

std::unordered_map<std::string, FuncDef> parse_functions(const std::vector<Tok>& toks) {
    std::unordered_map<std::string, FuncDef> funcs;
    size_t i = 0;
    const size_t n = toks.size();
    while (i < n && toks[i].kind != TokKind::Eof) {
        if (toks[i].kind == TokKind::Ident && toks[i].text == "uniform") {
            while (i < n && toks[i].kind != TokKind::Semi && toks[i].kind != TokKind::Eof) ++i;
            if (i < n) ++i;
            continue;
        }
        if (toks[i].kind == TokKind::Ident && is_type_name(toks[i].text) &&
            i + 2 < n && toks[i + 1].kind == TokKind::Ident && toks[i + 2].kind == TokKind::LParen) {
            FuncDef fn;
            fn.name = toks[i + 1].text;
            i += 3;
            while (i < n && toks[i].kind != TokKind::RParen && toks[i].kind != TokKind::Eof) {
                if (toks[i].kind == TokKind::Ident && is_type_name(toks[i].text)) {
                    ++i;
                    if (i < n && toks[i].kind == TokKind::Ident) {
                        fn.params.push_back(toks[i].text);
                        ++i;
                    }
                }
                if (i < n && toks[i].kind == TokKind::Comma) ++i;
                else if (i < n && toks[i].kind != TokKind::RParen) ++i;
            }
            if (i < n && toks[i].kind == TokKind::RParen) ++i;
            if (i < n && toks[i].kind == TokKind::LBrace) {
                fn.body_start = i;
                int depth = 1;
                ++i;
                while (i < n && depth > 0 && toks[i].kind != TokKind::Eof) {
                    if (toks[i].kind == TokKind::LBrace) ++depth;
                    else if (toks[i].kind == TokKind::RBrace) --depth;
                    ++i;
                }
                fn.body_end = i;
                funcs[fn.name] = std::move(fn);
                continue;
            }
        }
        ++i;
    }
    return funcs;
}

} // namespace

std::shared_ptr<RuntimeFilterSpec> parse_runtime_filter_source(
    std::string_view source,
    const RuntimeFilterUniforms* overrides,
    std::string_view input_child_name
) {
    if (source.empty()) return nullptr;

    auto spec = std::make_shared<RuntimeFilterSpec>();
    spec->source_code = std::string(source);

    size_t current_offset = 0;
    std::string transformed;
    transformed.reserve(source.size() + 128);

    const size_t n = source.size();
    size_t i = 0;
    char prev_non_ws = '\0';
    std::string_view last_ident;

    auto child_index_for = [&](std::string_view name) -> int {
        for (const auto& c : spec->children) {
            if (c.name == name) return static_cast<int>(c.index);
        }
        return -1;
    };

    while (i < n) {
        if (i + 1 < n && source[i] == '/' && source[i + 1] == '/') {
            const size_t s = i;
            i += 2;
            while (i < n && source[i] != '\n') ++i;
            transformed.append(source.substr(s, i - s));
            continue;
        }
        if (i + 1 < n && source[i] == '/' && source[i + 1] == '*') {
            const size_t s = i;
            i += 2;
            while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            transformed.append(source.substr(s, i - s));
            continue;
        }
        if (is_ident_start(source[i])) {
            const size_t start = i;
            while (i < n && is_ident_char(source[i])) ++i;
            std::string_view ident = source.substr(start, i - start);

            if (ident == "uniform") {
                size_t p = i;
                skip_ws_and_comments(source, p);
                const size_t ts = p;
                while (p < n && is_ident_char(source[p])) ++p;
                std::string_view type_str = source.substr(ts, p - ts);
                skip_ws_and_comments(source, p);
                const size_t ns = p;
                while (p < n && is_ident_char(source[p])) ++p;
                std::string_view name_str = source.substr(ns, p - ns);
                skip_ws_and_comments(source, p);

                if (!type_str.empty() && !name_str.empty()) {
                    int arr_count = 1;
                    if (p < n && source[p] == '[') {
                        ++p;
                        skip_ws_and_comments(source, p);
                        size_t val = 0;
                        while (p < n && std::isdigit(static_cast<unsigned char>(source[p]))) {
                            val = val * 10 + static_cast<size_t>(source[p] - '0');
                            ++p;
                        }
                        if (val > 0) arr_count = static_cast<int>(val);
                        while (p < n && source[p] != ']') ++p;
                        if (p < n) ++p;
                        skip_ws_and_comments(source, p);
                    }
                    std::string_view init_expr;
                    if (p < n && source[p] == '=') {
                        ++p;
                        const size_t is = p;
                        int par = 0, br = 0;
                        while (p < n) {
                            if (source[p] == '(') ++par;
                            else if (source[p] == ')') --par;
                            else if (source[p] == '{') ++br;
                            else if (source[p] == '}') --br;
                            else if (source[p] == ';' && par == 0 && br == 0) break;
                            ++p;
                        }
                        init_expr = source.substr(is, p - is);
                    }
                    if (p < n && source[p] == ';') {
                        if (type_str == "shader") {
                            spec->children.push_back(RuntimeFilterChild{std::string(name_str), spec->children.size()});
                        } else {
                            const TypeMap* tm = map_uniform_type(type_str);
                            RuntimeFilterUniform u;
                            u.name = std::string(name_str);
                            u.offset = current_offset;
                            u.count = arr_count;
                            u.type = tm ? tm->utype : RuntimeUniformType::Float;
                            u.size_bytes = (tm ? tm->bytes : 4u) * static_cast<size_t>(arr_count);
                            current_offset += u.size_bytes;
                            spec->uniform_data.resize(current_offset, 0);
                            if (!init_expr.empty()) {
                                parse_default_initializer(init_expr, u.type, spec->uniform_data.data() + u.offset, u.size_bytes);
                            }
                            spec->uniforms.push_back(std::move(u));

                            transformed.append("uniform ");
                            transformed.append(tm ? tm->glsl : type_str);
                            transformed.push_back(' ');
                            transformed.append(name_str);
                            if (arr_count > 1) {
                                transformed.push_back('[');
                                transformed.append(std::to_string(arr_count));
                                transformed.push_back(']');
                            }
                            transformed.push_back(';');
                        }
                        i = p + 1;
                        last_ident = {};
                        prev_non_ws = ';';
                        continue;
                    }
                }
                transformed.append(ident);
            } else if (prev_non_ws != '.' && child_index_for(ident) >= 0) {
                const int cidx = child_index_for(ident);
                size_t p = i;
                skip_ws_and_comments(source, p);
                if (p < n && source[p] == '.') {
                    ++p;
                    skip_ws_and_comments(source, p);
                    if (p + 4 <= n && source.substr(p, 4) == "eval" &&
                        (p + 4 == n || !is_ident_char(source[p + 4]))) {
                        p += 4;
                        skip_ws_and_comments(source, p);
                        if (p < n && source[p] == '(') {
                            transformed.append("__nisaba_child_eval_");
                            transformed.append(std::to_string(cidx));
                            transformed.push_back('(');
                            i = p + 1;
                            last_ident = "eval";
                            prev_non_ws = '(';
                            continue;
                        }
                    }
                }
                transformed.append(ident);
            } else if (ident == "main") {
                size_t p = i;
                skip_ws_and_comments(source, p);
                const bool valid_ret = (last_ident == "vec4" || last_ident == "half4" || last_ident == "float4");
                if (p < n && source[p] == '(' && valid_ret) {
                    int pd = 1;
                    size_t k = p + 1;
                    while (k < n && pd > 0) {
                        if (source[k] == '(') ++pd;
                        else if (source[k] == ')') --pd;
                        else if (source[k] == ',' && pd == 1) spec->has_color_in = true;
                        ++k;
                    }
                    transformed.append("user_main");
                } else {
                    transformed.append(ident);
                }
            } else {
                if (prev_non_ws != '.') {
                    transformed.append(map_ident_type(ident));
                } else {
                    transformed.append(ident);
                }
            }

            last_ident = ident;
            prev_non_ws = 'a';
            continue;
        }
        const char ch = source[i];
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            prev_non_ws = ch;
            if (ch == ';' || ch == '}' || ch == ')') last_ident = {};
        }
        transformed.push_back(ch);
        ++i;
    }

    spec->transformed_glsl = std::move(transformed);
    spec->uniform_size = current_offset;

    // Resolve input child index (first child by default, or named)
    if (!spec->children.empty()) {
        spec->has_input_child = true;
        spec->input_child_index = 0;
        if (!input_child_name.empty()) {
            for (const auto& c : spec->children) {
                if (c.name == input_child_name) {
                    spec->input_child_index = c.index;
                    break;
                }
            }
        }
    }

    // Apply uniform overrides if provided
    if (overrides != nullptr) {
        for (const auto& u : spec->uniforms) {
            auto it = overrides->entries().find(u.name);
            if (it != overrides->entries().end() && !it->second.empty()) {
                const size_t ncopy = std::min(u.size_bytes, it->second.size());
                if (u.offset + ncopy <= spec->uniform_data.size()) {
                    std::memcpy(spec->uniform_data.data() + u.offset, it->second.data(), ncopy);
                }
            }
        }
    }

    spec->shader_hash = fnv1a_hash(spec->transformed_glsl) ^ (static_cast<uint64_t>(spec->input_child_index) * 0x9e3779b97f4a7c15ULL);
    return spec;
}

Pixmap eval_runtime_filter_cpu(
    const RuntimeFilterSpec& spec,
    const Pixmap& src_rgba,
    int32_t roi_origin_x,
    int32_t roi_origin_y,
    const Transform& layer_transform,
    const std::optional<Rect>& crop_rect
) {
    const uint32_t w = src_rgba.width();
    const uint32_t h = src_rgba.height();
    auto out_opt = Pixmap::allocate(w, h, PixelFormat::RGBA8888);
    if (!out_opt) return Pixmap{};
    auto out_mut = out_opt->as_mut();
    const auto src_ref = src_rgba.as_ref();

    const std::vector<Tok> toks = tokenize(spec.transformed_glsl);
    const auto funcs = parse_functions(toks);
    auto main_it = funcs.find("user_main");
    if (main_it == funcs.end()) {
        for (uint32_t y = 0; y < h; ++y) {
            std::copy_n(src_ref.row(y), w, out_mut.row(y));
        }
        return std::move(*out_opt);
    }

    // Unpack uniforms into Val map
    std::unordered_map<std::string, Val> uni_map;
    for (const auto& u : spec.uniforms) {
        if (u.offset + u.size_bytes > spec.uniform_data.size()) continue;
        const uint8_t* ptr = spec.uniform_data.data() + u.offset;
        Val val;
        switch (u.type) {
            case RuntimeUniformType::Int: {
                int32_t iv = 0;
                std::memcpy(&iv, ptr, sizeof(iv));
                val = Val::scalar(static_cast<float>(iv));
                break;
            }
            case RuntimeUniformType::Float: {
                float fv = 0.0f;
                std::memcpy(&fv, ptr, sizeof(fv));
                val = Val::scalar(fv);
                break;
            }
            case RuntimeUniformType::Float2: {
                float fv[2]{};
                std::memcpy(fv, ptr, sizeof(fv));
                val = Val::vec2(fv[0], fv[1]);
                break;
            }
            case RuntimeUniformType::Float3: {
                float fv[3]{};
                std::memcpy(fv, ptr, sizeof(fv));
                val = Val::vec3(fv[0], fv[1], fv[2]);
                break;
            }
            case RuntimeUniformType::Float4: {
                float fv[4]{};
                std::memcpy(fv, ptr, sizeof(fv));
                val = Val::vec4(fv[0], fv[1], fv[2], fv[3]);
                break;
            }
            default:
                break;
        }
        uni_map[u.name] = val;
    }

    const auto inv_ts_opt = layer_transform.invert();
    const Transform inv_ts = inv_ts_opt.value_or(Transform());

    int32_t cx0 = 0, cy0 = 0, cx1 = static_cast<int32_t>(w), cy1 = static_cast<int32_t>(h);
    if (crop_rect.has_value()) {
        if (auto dev_crop = crop_rect->transform(layer_transform)) {
            cx0 = std::clamp(static_cast<int32_t>(std::floor(dev_crop->left())) - roi_origin_x, 0, static_cast<int32_t>(w));
            cy0 = std::clamp(static_cast<int32_t>(std::floor(dev_crop->top())) - roi_origin_y, 0, static_cast<int32_t>(h));
            cx1 = std::clamp(static_cast<int32_t>(std::ceil(dev_crop->right())) - roi_origin_x, 0, static_cast<int32_t>(w));
            cy1 = std::clamp(static_cast<int32_t>(std::ceil(dev_crop->bottom())) - roi_origin_y, 0, static_cast<int32_t>(h));
        } else {
            return std::move(*out_opt);
        }
    }

    const FuncDef& main_fn = main_it->second;
    CpuEvalContext ctx{
        toks, uni_map, funcs, src_ref, roi_origin_x, roi_origin_y,
        layer_transform, spec.input_child_index, spec.has_input_child,
        {}, false, {}, 0
    };

    for (int32_t y = cy0; y < cy1; ++y) {
        auto* dst_row = out_mut.row(static_cast<size_t>(y));
        const float dev_y = static_cast<float>(roi_origin_y + y) + 0.5f;
        for (int32_t x = cx0; x < cx1; ++x) {
            Point lp = Point::from_xy(static_cast<float>(roi_origin_x + x) + 0.5f, dev_y);
            inv_ts.map_point(lp);

            ctx.scopes.clear();
            ctx.scopes.emplace_back();
            if (!main_fn.params.empty()) {
                ctx.scopes.back()[main_fn.params[0]] = Val::vec2(lp.x, lp.y);
            }
            if (main_fn.params.size() >= 2) {
                ctx.scopes.back()[main_fn.params[1]] = Val::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            }
            ctx.returned = false;
            ctx.return_val = Val::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            ctx.pos = main_fn.body_start;
            ctx.exec_block();

            const float a = std::clamp(ctx.return_val.get(3), 0.0f, 1.0f);
            const float r = std::clamp(ctx.return_val.get(0), 0.0f, a);
            const float g = std::clamp(ctx.return_val.get(1), 0.0f, a);
            const float b = std::clamp(ctx.return_val.get(2), 0.0f, a);
            dst_row[x] = PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>(r * 255.0f + 0.5f),
                static_cast<uint8_t>(g * 255.0f + 0.5f),
                static_cast<uint8_t>(b * 255.0f + 0.5f),
                static_cast<uint8_t>(a * 255.0f + 0.5f)
            );
        }
    }

    return std::move(*out_opt);
}

PremultipliedColorU8 eval_runtime_blender_cpu_pixel(
    const RuntimeFilterSpec& spec,
    PremultipliedColorU8 src,
    PremultipliedColorU8 dst
) {
    const std::vector<Tok> toks = tokenize(spec.transformed_glsl);
    const auto funcs = parse_functions(toks);
    auto main_it = funcs.find("user_main");
    if (main_it == funcs.end()) return src;

    std::unordered_map<std::string, Val> uni_map;
    for (const auto& u : spec.uniforms) {
        if (u.offset + u.size_bytes > spec.uniform_data.size()) continue;
        const uint8_t* ptr = spec.uniform_data.data() + u.offset;
        Val val;
        if (u.type == RuntimeUniformType::Int) {
            int32_t iv = 0; std::memcpy(&iv, ptr, 4); val = Val::scalar(static_cast<float>(iv));
        } else if (u.type == RuntimeUniformType::Float) {
            float fv = 0.0f; std::memcpy(&fv, ptr, 4); val = Val::scalar(fv);
        } else if (u.type == RuntimeUniformType::Float2) {
            float fv[2]{}; std::memcpy(fv, ptr, 8); val = Val::vec2(fv[0], fv[1]);
        } else if (u.type == RuntimeUniformType::Float3) {
            float fv[3]{}; std::memcpy(fv, ptr, 12); val = Val::vec3(fv[0], fv[1], fv[2]);
        } else if (u.type == RuntimeUniformType::Float4) {
            float fv[4]{}; std::memcpy(fv, ptr, 16); val = Val::vec4(fv[0], fv[1], fv[2], fv[3]);
        }
        uni_map[u.name] = val;
    }

    auto dummy_pm = Pixmap::allocate(1, 1, PixelFormat::RGBA8888);
    CpuEvalContext ctx{
        toks, uni_map, funcs, dummy_pm->as_ref(), 0, 0,
        Transform(), 0, false, {}, false, {}, 0
    };
    const FuncDef& main_fn = main_it->second;
    constexpr float inv = 1.0f / 255.0f;
    ctx.scopes.emplace_back();
    if (main_fn.params.size() >= 1) {
        ctx.scopes.back()[main_fn.params[0]] = Val::vec4(src.r * inv, src.g * inv, src.b * inv, src.a * inv);
    }
    if (main_fn.params.size() >= 2) {
        ctx.scopes.back()[main_fn.params[1]] = Val::vec4(dst.r * inv, dst.g * inv, dst.b * inv, dst.a * inv);
    }
    ctx.pos = main_fn.body_start;
    ctx.exec_block();
    const float a = std::clamp(ctx.return_val.get(3), 0.0f, 1.0f);
    const float r = std::clamp(ctx.return_val.get(0), 0.0f, a);
    const float g = std::clamp(ctx.return_val.get(1), 0.0f, a);
    const float b = std::clamp(ctx.return_val.get(2), 0.0f, a);
    return PremultipliedColorU8::from_rgba_unchecked(
        static_cast<uint8_t>(r * 255.0f + 0.5f),
        static_cast<uint8_t>(g * 255.0f + 0.5f),
        static_cast<uint8_t>(b * 255.0f + 0.5f),
        static_cast<uint8_t>(a * 255.0f + 0.5f)
    );
}

void eval_runtime_blender_cpu_region(
    const RuntimeFilterSpec& spec,
    PixmapMut& dst,
    const PixmapRef& src,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    float opacity,
    const uint8_t* clip_mask,
    uint32_t clip_stride
) {
    const std::vector<Tok> toks = tokenize(spec.transformed_glsl);
    const auto funcs = parse_functions(toks);
    auto main_it = funcs.find("user_main");
    if (main_it == funcs.end()) return;

    std::unordered_map<std::string, Val> uni_map;
    for (const auto& u : spec.uniforms) {
        if (u.offset + u.size_bytes > spec.uniform_data.size()) continue;
        const uint8_t* ptr = spec.uniform_data.data() + u.offset;
        Val val;
        if (u.type == RuntimeUniformType::Int) {
            int32_t iv = 0; std::memcpy(&iv, ptr, 4); val = Val::scalar(static_cast<float>(iv));
        } else if (u.type == RuntimeUniformType::Float) {
            float fv = 0.0f; std::memcpy(&fv, ptr, 4); val = Val::scalar(fv);
        } else if (u.type == RuntimeUniformType::Float2) {
            float fv[2]{}; std::memcpy(fv, ptr, 8); val = Val::vec2(fv[0], fv[1]);
        } else if (u.type == RuntimeUniformType::Float3) {
            float fv[3]{}; std::memcpy(fv, ptr, 12); val = Val::vec3(fv[0], fv[1], fv[2]);
        } else if (u.type == RuntimeUniformType::Float4) {
            float fv[4]{}; std::memcpy(fv, ptr, 16); val = Val::vec4(fv[0], fv[1], fv[2], fv[3]);
        }
        uni_map[u.name] = val;
    }

    const FuncDef& main_fn = main_it->second;
    CpuEvalContext ctx{
        toks, uni_map, funcs, src, 0, 0,
        Transform(), 0, false, {}, false, {}, 0
    };
    const bool dst_bgra = (dst.format() == PixelFormat::BGRA8888);
    const bool src_bgra = (src.format() == PixelFormat::BGRA8888);
    constexpr float inv = 1.0f / 255.0f;
    const float op = std::clamp(opacity, 0.0f, 1.0f);

    for (int32_t y = y0; y < y1; ++y) {
        const auto* srow = src.row(static_cast<size_t>(y));
        auto* drow = dst.row(static_cast<size_t>(y));
        const uint8_t* mrow = clip_mask ? (clip_mask + static_cast<size_t>(y) * clip_stride) : nullptr;
        for (int32_t x = x0; x < x1; ++x) {
            const uint8_t cov = mrow ? mrow[x] : 255u;
            if (cov == 0) continue;
            auto sp = srow[x];
            auto dp = drow[x];
            if (src_bgra) sp = PremultipliedColorU8::from_rgba_unchecked(sp.b, sp.g, sp.r, sp.a);
            if (dst_bgra) dp = PremultipliedColorU8::from_rgba_unchecked(dp.b, dp.g, dp.r, dp.a);

            ctx.scopes.clear();
            ctx.scopes.emplace_back();
            if (main_fn.params.size() >= 1) {
                ctx.scopes.back()[main_fn.params[0]] = Val::vec4(
                    sp.r * inv * op, sp.g * inv * op, sp.b * inv * op, sp.a * inv * op);
            }
            if (main_fn.params.size() >= 2) {
                ctx.scopes.back()[main_fn.params[1]] = Val::vec4(
                    dp.r * inv, dp.g * inv, dp.b * inv, dp.a * inv);
            }
            ctx.returned = false;
            ctx.return_val = Val::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            ctx.pos = main_fn.body_start;
            ctx.exec_block();

            const float a = std::clamp(ctx.return_val.get(3), 0.0f, 1.0f);
            const float r = std::clamp(ctx.return_val.get(0), 0.0f, a);
            const float g = std::clamp(ctx.return_val.get(1), 0.0f, a);
            const float b = std::clamp(ctx.return_val.get(2), 0.0f, a);
            auto out = PremultipliedColorU8::from_rgba_unchecked(
                static_cast<uint8_t>(r * 255.0f + 0.5f),
                static_cast<uint8_t>(g * 255.0f + 0.5f),
                static_cast<uint8_t>(b * 255.0f + 0.5f),
                static_cast<uint8_t>(a * 255.0f + 0.5f)
            );
            if (cov < 255u) {
                const uint32_t ic = 255u - cov;
                out = PremultipliedColorU8::from_rgba_unchecked(
                    static_cast<uint8_t>((dp.r * ic + out.r * cov + 128u) / 255u),
                    static_cast<uint8_t>((dp.g * ic + out.g * cov + 128u) / 255u),
                    static_cast<uint8_t>((dp.b * ic + out.b * cov + 128u) / 255u),
                    static_cast<uint8_t>((dp.a * ic + out.a * cov + 128u) / 255u)
                );
            }
            if (dst_bgra) out = PremultipliedColorU8::from_rgba_unchecked(out.b, out.g, out.r, out.a);
            drow[x] = out;
        }
    }
}

} // namespace nisaba::effects
