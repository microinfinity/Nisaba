#include "nisaba/markdown/math_engine.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/pdf/pdf_canvas.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/text/buffer.hpp"
#ifdef NISABA_GLEW
#include "nisaba/gpu/gpu_canvas.hpp"
#endif
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace nisaba::markdown {

namespace {

// Comprehensive LaTeX symbol & command lookup table to UTF-8 Unicode
static const std::unordered_map<std::string_view, std::string_view> latex_symbols = {
    // Lowercase Greek
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"},
    {"epsilon", "ϵ"}, {"varepsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"},
    {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"},
    {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"},
    {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"}, {"varrho", "ϱ"},
    {"sigma", "σ"}, {"varsigma", "ς"}, {"tau", "τ"}, {"upsilon", "υ"},
    {"phi", "ϕ"}, {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"},
    {"omega", "ω"},
    // Uppercase Greek
    {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"},
    {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Upsilon", "Υ"},
    {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
    // Big Operators
    {"sum", "∑"}, {"prod", "∏"}, {"coprod", "∐"},
    {"int", "∫"}, {"iint", "∬"}, {"iiint", "∭"}, {"oint", "∮"},
    {"bigcup", "⋃"}, {"bigcap", "⋂"}, {"bigoplus", "⨁"}, {"bigotimes", "⨂"},
    // Binary Operators & Relations
    {"pm", "±"}, {"mp", "∓"}, {"times", "×"}, {"div", "÷"},
    {"cdot", "·"}, {"ast", "∗"}, {"star", "⋆"}, {"circ", "∘"},
    {"bullet", "∙"}, {"oplus", "⊕"}, {"ominus", "⊖"}, {"otimes", "⊗"},
    {"oslash", "⊘"}, {"odot", "⊙"}, {"leq", "≤"}, {"le", "≤"},
    {"geq", "≥"}, {"ge", "≥"}, {"neq", "≠"}, {"ne", "≠"},
    {"approx", "≈"}, {"equiv", "≡"}, {"cong", "≅"}, {"sim", "∼"},
    {"simeq", "≃"}, {"propto", "∝"}, {"ll", "≪"}, {"gg", "≫"},
    {"subset", "⊂"}, {"supset", "⊃"}, {"subseteq", "⊆"}, {"supseteq", "⊇"},
    {"in", "∈"}, {"notin", "∉"}, {"ni", "∋"}, {"cup", "∪"},
    {"cap", "∩"}, {"setminus", "∖"}, {"emptyset", "∅"}, {"varnothing", "∅"},
    // Logic & Quantifiers
    {"forall", "∀"}, {"exists", "∃"}, {"nexists", "∄"}, {"neg", "¬"},
    {"lnot", "¬"}, {"land", "∧"}, {"wedge", "∧"}, {"lor", "∨"},
    {"vee", "∨"}, {"vdash", "⊢"}, {"models", "⊨"}, {"top", "⊤"},
    {"bot", "⊥"}, {"perp", "⊥"}, {"parallel", "∥"},
    // Arrows
    {"to", "→"}, {"rightarrow", "→"}, {"leftarrow", "←"}, {"gets", "←"},
    {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"},
    {"Leftrightarrow", "⇔"}, {"iff", "⇔"}, {"implies", "⇒"},
    {"mapsto", "↦"}, {"longmapsto", "⟼"}, {"longrightarrow", "⟶"},
    {"longleftarrow", "⟵"}, {"uparrow", "↑"}, {"downarrow", "↓"},
    {"updownarrow", "↕"}, {"nearrow", "↗"}, {"searrow", "↘"},
    // Calculus, Infinity & Misc Symbols
    {"infty", "∞"}, {"partial", "∂"}, {"nabla", "∇"}, {"hbar", "ℏ"},
    {"ell", "ℓ"}, {"Re", "ℜ"}, {"Im", "ℑ"}, {"wp", "℘"},
    {"aleph", "ℵ"}, {"angle", "∠"}, {"triangle", "△"}, {"square", "□"},
    {"diamond", "⋄"}, {"checkmark", "✓"}, {"dagger", "†"}, {"ddagger", "‡"},
    {"dots", "…"}, {"ldots", "…"}, {"cdots", "⋯"}, {"vdots", "⋮"},
    {"ddots", "⋱"}, {"prime", "′"}, {"degree", "°"},
    // Delimiters
    {"langle", "⟨"}, {"rangle", "⟩"}, {"lceil", "⌈"}, {"rceil", "⌉"},
    {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"vert", "|"}, {"Vert", "‖"},
    {"{", "{"}, {"}", "}"}, {"%", "%"}, {"$", "$"}, {"&", "&"}, {"_", "_"}, {"#", "#"}
};

static const std::unordered_set<std::string_view> big_op_commands = {
    "sum", "prod", "coprod", "int", "iint", "iiint", "oint",
    "bigcup", "bigcap", "bigoplus", "bigotimes",
    "lim", "limsup", "liminf", "max", "min", "sup", "inf", "det", "Pr", "gcd"
};

static const std::unordered_set<std::string_view> roman_functions = {
    "sin", "cos", "tan", "cot", "sec", "csc",
    "arcsin", "arccos", "arctan",
    "sinh", "cosh", "tanh", "coth",
    "log", "ln", "lg", "exp",
    "lim", "limsup", "liminf", "max", "min", "sup", "inf",
    "det", "dim", "ker", "hom", "arg", "deg", "gcd", "Pr", "erf", "mod", "sgn", "tr"
};

class LatexParser {
public:
    LatexParser(std::string_view src, bool display_mode)
        : src_(src), pos_(0), display_mode_(display_mode) {}

    std::shared_ptr<MathNode> parse_row(std::string_view stop_token = {}) {
        auto row = std::make_shared<MathNode>();
        row->type = MathNodeType::Row;

        while (pos_ < src_.size()) {
            skip_spaces();
            if (pos_ >= src_.size()) break;

            if (!stop_token.empty() && src_.substr(pos_).starts_with(stop_token)) {
                break;
            }
            if (src_[pos_] == '}' || src_[pos_] == '&') {
                break;
            }
            if (src_.substr(pos_).starts_with("\\\\")) {
                break;
            }
            if (src_.substr(pos_).starts_with("\\end{") || src_.substr(pos_).starts_with("\\right")) {
                break;
            }

            auto atom = parse_scripted_atom();
            if (atom) {
                row->children.push_back(std::move(atom));
            } else {
                pos_++;
            }
        }
        return row;
    }

private:
    std::string_view src_;
    size_t pos_;
    bool display_mode_;

    void skip_spaces() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
            pos_++;
        }
    }

    std::string_view read_command_name() {
        size_t start = pos_;
        if (pos_ < src_.size() && !std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
            pos_++;
            return src_.substr(start, 1);
        }
        while (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
            pos_++;
        }
        return src_.substr(start, pos_ - start);
    }

    std::string read_braced_text() {
        skip_spaces();
        if (pos_ >= src_.size() || src_[pos_] != '{') return "";
        pos_++; // skip '{'
        int depth = 1;
        size_t start = pos_;
        while (pos_ < src_.size() && depth > 0) {
            if (src_[pos_] == '{') depth++;
            else if (src_[pos_] == '}') {
                depth--;
                if (depth == 0) {
                    std::string res(src_.substr(start, pos_ - start));
                    pos_++;
                    return res;
                }
            }
            pos_++;
        }
        return std::string(src_.substr(start));
    }

    std::shared_ptr<MathNode> parse_group_or_atom() {
        skip_spaces();
        if (pos_ >= src_.size()) return std::make_shared<MathNode>();
        if (src_[pos_] == '{') {
            pos_++; // skip '{'
            auto node = parse_row();
            if (pos_ < src_.size() && src_[pos_] == '}') pos_++;
            return node;
        }
        return parse_atom();
    }

    std::shared_ptr<MathNode> parse_scripted_atom() {
        auto base = parse_group_or_atom();
        if (!base) return nullptr;

        std::shared_ptr<MathNode> sub = nullptr;
        std::shared_ptr<MathNode> sup = nullptr;

        while (pos_ < src_.size()) {
            skip_spaces();
            if (pos_ >= src_.size()) break;

            if (src_[pos_] == '_') {
                pos_++;
                sub = parse_group_or_atom();
            } else if (src_[pos_] == '^') {
                pos_++;
                sup = parse_group_or_atom();
            } else if (src_[pos_] == '\'') {
                // Prime notation f'(x) or f''(x)
                std::string primes;
                while (pos_ < src_.size() && src_[pos_] == '\'') {
                    primes += "′";
                    pos_++;
                }
                auto pnode = std::make_shared<MathNode>();
                pnode->type = MathNodeType::Symbol;
                pnode->text = primes;
                sup = pnode;
            } else {
                break;
            }
        }

        if (sub || sup) {
            auto script = std::make_shared<MathNode>();
            script->type = MathNodeType::Script;
            script->is_big_operator = base->is_big_operator;
            script->limits_above_below = base->limits_above_below;
            script->numerator = std::move(base);
            script->denominator = std::move(sub);
            script->superscript = std::move(sup);
            return script;
        }
        return base;
    }

    std::shared_ptr<MathNode> parse_atom() {
        skip_spaces();
        if (pos_ >= src_.size()) return nullptr;

        char c = src_[pos_];

        // 1. LaTeX Command \...
        if (c == '\\') {
            pos_++;
            if (pos_ >= src_.size()) return nullptr;

            // Spacing commands \, \; \: \!
            if (src_[pos_] == ',' || src_[pos_] == ';' || src_[pos_] == ':') {
                char sp = src_[pos_++];
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Space;
                node->space_em = (sp == ';') ? 0.28f : 0.17f;
                return node;
            }
            if (src_[pos_] == '!') {
                pos_++;
                return std::make_shared<MathNode>();
            }
            if (src_[pos_] == ' ') {
                pos_++;
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Space;
                node->space_em = 0.25f;
                return node;
            }

            auto cmd = read_command_name();

            if (cmd == "quad" || cmd == "qquad") {
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Space;
                node->space_em = (cmd == "qquad") ? 2.0f : 1.0f;
                return node;
            }

            // Fractions: \frac{a}{b}, \dfrac{a}{b}, \tfrac{a}{b}
            if (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac" || cmd == "cfrac") {
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Fraction;
                node->numerator = parse_group_or_atom();
                node->denominator = parse_group_or_atom();
                return node;
            }

            //binom: \binom{n}{k}
            if (cmd == "binom") {
                auto frac = std::make_shared<MathNode>();
                frac->type = MathNodeType::Fraction;
                frac->text = "nobar";
                frac->numerator = parse_group_or_atom();
                frac->denominator = parse_group_or_atom();

                auto delim = std::make_shared<MathNode>();
                delim->type = MathNodeType::Delimited;
                delim->left_delim = "(";
                delim->right_delim = ")";
                delim->numerator = std::move(frac);
                return delim;
            }

            // Roots: \sqrt[n]{x} or \sqrt{x}
            if (cmd == "sqrt") {
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Sqrt;
                skip_spaces();
                if (pos_ < src_.size() && src_[pos_] == '[') {
                    pos_++; // skip '['
                    node->denominator = parse_row("]");
                    if (pos_ < src_.size() && src_[pos_] == ']') pos_++;
                }
                node->numerator = parse_group_or_atom();
                return node;
            }

            // Delimiters: \left( ... \right)
            if (cmd == "left") {
                skip_spaces();
                std::string l_delim = "(";
                if (pos_ < src_.size()) {
                    if (src_[pos_] == '\\') {
                        pos_++;
                        auto dcmd = read_command_name();
                        auto it = latex_symbols.find(dcmd);
                        l_delim = (it != latex_symbols.end()) ? std::string(it->second) : std::string(dcmd);
                    } else {
                        if (src_[pos_] != '.') l_delim = std::string(1, src_[pos_]);
                        else l_delim = "";
                        pos_++;
                    }
                }
                auto inner = parse_row();
                std::string r_delim = ")";
                skip_spaces();
                if (src_.substr(pos_).starts_with("\\right")) {
                    pos_ += 6;
                    skip_spaces();
                    if (pos_ < src_.size()) {
                        if (src_[pos_] == '\\') {
                            pos_++;
                            auto dcmd = read_command_name();
                            auto it = latex_symbols.find(dcmd);
                            r_delim = (it != latex_symbols.end()) ? std::string(it->second) : std::string(dcmd);
                        } else {
                            if (src_[pos_] != '.') r_delim = std::string(1, src_[pos_]);
                            else r_delim = "";
                            pos_++;
                        }
                    }
                }
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Delimited;
                node->left_delim = l_delim;
                node->right_delim = r_delim;
                node->numerator = std::move(inner);
                return node;
            }

            // Environments: \begin{pmatrix} ... \end{pmatrix}
            if (cmd == "begin") {
                std::string env = read_braced_text();
                auto mat = std::make_shared<MathNode>();
                mat->type = MathNodeType::Matrix;
                if (env == "pmatrix") mat->matrix_kind = MathMatrixKind::Paren;
                else if (env == "bmatrix") mat->matrix_kind = MathMatrixKind::Bracket;
                else if (env == "Bmatrix") mat->matrix_kind = MathMatrixKind::Brace;
                else if (env == "vmatrix") mat->matrix_kind = MathMatrixKind::Vert;
                else if (env == "Vmatrix") mat->matrix_kind = MathMatrixKind::DoubleVert;
                else if (env == "cases") mat->matrix_kind = MathMatrixKind::Cases;
                else mat->matrix_kind = MathMatrixKind::Plain;

                std::vector<std::shared_ptr<MathNode>> current_row;
                while (pos_ < src_.size()) {
                    skip_spaces();
                    if (src_.substr(pos_).starts_with("\\end{")) {
                        pos_ += 4;
                        read_braced_text();
                        break;
                    }
                    auto cell = parse_row();
                    current_row.push_back(std::move(cell));

                    skip_spaces();
                    if (pos_ < src_.size() && src_[pos_] == '&') {
                        pos_++;
                    } else if (src_.substr(pos_).starts_with("\\\\")) {
                        pos_ += 2;
                        mat->matrix_rows.push_back(std::move(current_row));
                        current_row.clear();
                    } else if (src_.substr(pos_).starts_with("\\end{")) {
                        // handled on next loop iteration
                    } else {
                        break;
                    }
                }
                if (!current_row.empty()) {
                    mat->matrix_rows.push_back(std::move(current_row));
                }
                return mat;
            }

            // Accents: \hat, \bar, \vec, \tilde, \dot, \ddot, \overline, \underline
            if (cmd == "hat" || cmd == "widehat" || cmd == "bar" || cmd == "overline" ||
                cmd == "vec" || cmd == "overrightarrow" || cmd == "tilde" || cmd == "widetilde" ||
                cmd == "dot" || cmd == "ddot" || cmd == "underline") {
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Accent;
                if (cmd == "hat" || cmd == "widehat") node->accent_kind = MathAccentKind::Hat;
                else if (cmd == "bar") node->accent_kind = MathAccentKind::Bar;
                else if (cmd == "overline") node->accent_kind = MathAccentKind::Overline;
                else if (cmd == "underline") node->accent_kind = MathAccentKind::Underline;
                else if (cmd == "vec" || cmd == "overrightarrow") node->accent_kind = MathAccentKind::Vec;
                else if (cmd == "tilde" || cmd == "widetilde") node->accent_kind = MathAccentKind::Tilde;
                else if (cmd == "dot") node->accent_kind = MathAccentKind::Dot;
                else if (cmd == "ddot") node->accent_kind = MathAccentKind::DDot;
                node->numerator = parse_group_or_atom();
                return node;
            }

            // Text & Font styles: \text{}, \mathrm{}, \mathbf{}, \mathbb{}, \mathcal{}, \mathtt{}
            if (cmd == "text" || cmd == "textrm" || cmd == "mathrm" || cmd == "operatorname") {
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Text;
                node->font_style = MathFontStyle::Roman;
                node->text = read_braced_text();
                return node;
            }
            if (cmd == "mathbf" || cmd == "textbf" || cmd == "boldsymbol" || cmd == "bm") {
                auto node = parse_group_or_atom();
                apply_font_style_recursive(*node, MathFontStyle::Bold);
                return node;
            }
            if (cmd == "mathbb") {
                std::string content = read_braced_text();
                auto node = std::make_shared<MathNode>();
                node->type = MathNodeType::Symbol;
                node->font_style = MathFontStyle::Blackboard;
                // Map common Blackboard Bold letters (R, N, Z, Q, C, E, P)
                if (content == "R") node->text = "ℝ";
                else if (content == "N") node->text = "ℕ";
                else if (content == "Z") node->text = "ℤ";
                else if (content == "Q") node->text = "ℚ";
                else if (content == "C") node->text = "ℂ";
                else if (content == "E") node->text = "𝔼";
                else if (content == "P") node->text = "ℙ";
                else node->text = content;
                return node;
            }
            if (cmd == "mathcal") {
                auto node = parse_group_or_atom();
                apply_font_style_recursive(*node, MathFontStyle::Calligraphic);
                return node;
            }
            if (cmd == "mathtt" || cmd == "texttt") {
                auto node = parse_group_or_atom();
                apply_font_style_recursive(*node, MathFontStyle::Monospace);
                return node;
            }

            // Big operators & Roman functions
            bool is_big = big_op_commands.count(cmd) > 0;
            bool is_func = roman_functions.count(cmd) > 0;

            auto node = std::make_shared<MathNode>();
            node->type = MathNodeType::Symbol;
            node->is_big_operator = is_big;
            node->limits_above_below = is_big && (display_mode_ || cmd == "lim" || cmd == "max" || cmd == "min" || cmd == "sup" || cmd == "inf");
            if (is_func) {
                node->font_style = MathFontStyle::Roman;
            }

            auto it = latex_symbols.find(cmd);
            if (it != latex_symbols.end()) {
                node->text = std::string(it->second);
            } else {
                node->text = std::string(cmd);
            }
            return node;
        }

        // 2. Contiguous Numbers (e.g. 3.14, 42)
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = pos_++;
            while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.')) {
                pos_++;
            }
            auto node = std::make_shared<MathNode>();
            node->type = MathNodeType::Symbol;
            node->font_style = MathFontStyle::Roman;
            node->text = std::string(src_.substr(start, pos_ - start));
            return node;
        }

        // 3. Single Latin variable (italic by default in math mode)
        if (std::isalpha(static_cast<unsigned char>(c))) {
            pos_++;
            auto node = std::make_shared<MathNode>();
            node->type = MathNodeType::Symbol;
            node->font_style = MathFontStyle::Normal;
            node->text = std::string(1, c);
            return node;
        }

        // 4. Operators & Punctuation (+, -, =, <, >, (, ), [, ], |, /, *, !, etc.)
        pos_++;
        auto node = std::make_shared<MathNode>();
        node->type = MathNodeType::Symbol;
        node->font_style = MathFontStyle::Roman;
        if (c == '-') {
            node->text = " − "; // True mathematical minus sign with spacing
        } else if (c == '+' || c == '=') {
            node->text = std::string(" ") + c + " ";
        } else {
            node->text = std::string(1, c);
        }
        return node;
    }

    static void apply_font_style_recursive(MathNode& n, MathFontStyle style) {
        n.font_style = style;
        for (auto& ch : n.children) {
            if (ch) apply_font_style_recursive(*ch, style);
        }
        if (n.numerator) apply_font_style_recursive(*n.numerator, style);
        if (n.denominator) apply_font_style_recursive(*n.denominator, style);
        if (n.superscript) apply_font_style_recursive(*n.superscript, style);
    }
};

/// Finalizes a PathBuilder into a Path (empty Path when degenerate).
inline Path fin(PathBuilder& pb) {
    return pb.finish().value_or(Path());
}

float measure_math_string(
    std::string_view text,
    float font_size,
    MathFontStyle style,
    text::FontSystem& font_system
) {
    if (text.empty()) return 0.0f;

    text::Attrs attrs;
    if (style == MathFontStyle::Monospace) {
        attrs.set_family(text::Family::monospace());
    } else {
        attrs.set_family(text::Family::sans_serif());
    }
    if (style == MathFontStyle::Bold || style == MathFontStyle::Blackboard) {
        attrs.set_weight(text::Weight::Bold);
    }
    if (style == MathFontStyle::Normal && text.size() == 1 && std::isalpha(static_cast<unsigned char>(text[0]))) {
        attrs.set_style(text::Style::Italic);
    }

    text::Buffer buf(text::Metrics(font_size, font_size * 1.2f));
    buf.set_wrap(text::Wrap::None);
    buf.set_text(text, attrs);
    buf.shape_until_scroll(font_system);
    float w = buf.max_line_width();
    if (w <= 0.0f) {
        w = static_cast<float>(text.size()) * font_size * 0.55f;
    }
    return w;
}

void draw_math_string(
    ICanvas& canvas,
    std::string_view text,
    float x,
    float baseline_y,
    float font_size,
    MathFontStyle style,
    Color color,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    if (text.empty()) return;

    text::Attrs attrs;
    if (style == MathFontStyle::Monospace) {
        attrs.set_family(text::Family::monospace());
    } else {
        attrs.set_family(text::Family::sans_serif());
    }
    if (style == MathFontStyle::Bold || style == MathFontStyle::Blackboard) {
        attrs.set_weight(text::Weight::Bold);
    }
    if (style == MathFontStyle::Normal && text.size() == 1 && std::isalpha(static_cast<unsigned char>(text[0]))) {
        attrs.set_style(text::Style::Italic);
    }

    float top_y = baseline_y - font_size * 0.82f;

    if (canvas.canvas_kind() == CanvasKind::Software) {
        auto& cpu_canvas = static_cast<Canvas&>(canvas);
        text::Buffer buf(text::Metrics(font_size, font_size * 1.2f));
        buf.set_wrap(text::Wrap::None);
        buf.set_text(text, attrs);
        buf.shape_until_scroll(font_system);
        buf.draw(cpu_canvas, glyph_cache, font_system, color, x, top_y);
        return;
    }

#ifdef NISABA_GLEW
    if (canvas.canvas_kind() == CanvasKind::Gpu) {
        auto& gpu_canvas = static_cast<gpu::GpuCanvas&>(canvas);
        text::Buffer buf(text::Metrics(font_size, font_size * 1.2f));
        buf.set_wrap(text::Wrap::None);
        buf.set_text(text, attrs);
        buf.shape_until_scroll(font_system);
        gpu_canvas.draw_text_buffer(buf, font_system, Point::from_xy(x, top_y), color);
        return;
    }
#endif

    if (canvas.canvas_kind() == CanvasKind::Pdf) {
        auto& pdf_canvas = static_cast<pdf::PdfCanvas&>(canvas);
        std::string_view pdf_font = "Helvetica";
        if (style == MathFontStyle::Bold) pdf_font = "Helvetica-Bold";
        else if (style == MathFontStyle::Normal && text.size() == 1 && std::isalpha(static_cast<unsigned char>(text[0]))) {
            pdf_font = "Helvetica-Oblique";
        } else if (style == MathFontStyle::Monospace) {
            pdf_font = "Courier";
        }
        pdf_canvas.draw_text(text, x, top_y, font_size, color, pdf_font);
    }
}

void draw_vector_delimiter(
    ICanvas& canvas,
    std::string_view delim,
    float x,
    float top_y,
    float height,
    float width,
    Color color
) {
    if (delim.empty() || height <= 2.0f) return;
    Paint p(color);
    Stroke s(std::max(1.3f, width * 0.18f));
    s.line_cap = LineCap::Round;
    s.line_join = LineJoin::Round;

    float bot_y = top_y + height;
    float mid_y = top_y + height * 0.5f;

    PathBuilder pb;
    if (delim == "(") {
        pb.move_to(x + width * 0.85f, top_y + 1.0f);
        pb.quad_to(x + width * 0.10f, mid_y, x + width * 0.85f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == ")") {
        pb.move_to(x + width * 0.15f, top_y + 1.0f);
        pb.quad_to(x + width * 0.90f, mid_y, x + width * 0.15f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == "[") {
        pb.move_to(x + width * 0.85f, top_y + 1.0f);
        pb.line_to(x + width * 0.25f, top_y + 1.0f);
        pb.line_to(x + width * 0.25f, bot_y - 1.0f);
        pb.line_to(x + width * 0.85f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == "]") {
        pb.move_to(x + width * 0.15f, top_y + 1.0f);
        pb.line_to(x + width * 0.75f, top_y + 1.0f);
        pb.line_to(x + width * 0.75f, bot_y - 1.0f);
        pb.line_to(x + width * 0.15f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == "{") {
        float cx = x + width * 0.5f;
        pb.move_to(x + width * 0.85f, top_y + 1.0f);
        pb.quad_to(cx, top_y + 1.0f, cx, top_y + height * 0.25f);
        pb.line_to(cx, mid_y - 3.0f);
        pb.line_to(x + width * 0.15f, mid_y);
        pb.line_to(cx, mid_y + 3.0f);
        pb.line_to(cx, bot_y - height * 0.25f);
        pb.quad_to(cx, bot_y - 1.0f, x + width * 0.85f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == "}") {
        float cx = x + width * 0.5f;
        pb.move_to(x + width * 0.15f, top_y + 1.0f);
        pb.quad_to(cx, top_y + 1.0f, cx, top_y + height * 0.25f);
        pb.line_to(cx, mid_y - 3.0f);
        pb.line_to(x + width * 0.85f, mid_y);
        pb.line_to(cx, mid_y + 3.0f);
        pb.line_to(cx, bot_y - height * 0.25f);
        pb.quad_to(cx, bot_y - 1.0f, x + width * 0.15f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == "|") {
        pb.move_to(x + width * 0.5f, top_y + 1.0f);
        pb.line_to(x + width * 0.5f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    } else if (delim == "‖") {
        pb.move_to(x + width * 0.32f, top_y + 1.0f);
        pb.line_to(x + width * 0.32f, bot_y - 1.0f);
        pb.move_to(x + width * 0.68f, top_y + 1.0f);
        pb.line_to(x + width * 0.68f, bot_y - 1.0f);
        canvas.stroke_path(fin(pb), p, s);
    }
}

void measure_node(
    MathNode& node,
    float font_size,
    bool display_mode,
    text::FontSystem& font_system
) {
    node.computed_font_size = font_size;

    switch (node.type) {
        case MathNodeType::Row: {
            float w = 0.0f;
            float max_asc = font_size * 0.75f;
            float max_desc = font_size * 0.22f;
            for (auto& ch : node.children) {
                if (!ch) continue;
                measure_node(*ch, font_size, display_mode, font_system);
                w += ch->width;
                max_asc = std::max(max_asc, ch->ascent);
                max_desc = std::max(max_desc, ch->descent);
            }
            node.width = w;
            node.ascent = max_asc;
            node.descent = max_desc;
            break;
        }

        case MathNodeType::Space: {
            node.width = font_size * node.space_em;
            node.ascent = font_size * 0.5f;
            node.descent = 0.0f;
            break;
        }

        case MathNodeType::Symbol:
        case MathNodeType::Text: {
            float eff_size = node.is_big_operator ? (display_mode ? font_size * 1.45f : font_size * 1.15f) : font_size;
            node.computed_font_size = eff_size;
            node.width = measure_math_string(node.text, eff_size, node.font_style, font_system);
            node.ascent = eff_size * 0.80f;
            node.descent = eff_size * 0.24f;
            break;
        }

        case MathNodeType::Fraction: {
            float sub_size = display_mode ? std::max(10.0f, font_size * 0.85f) : std::max(9.0f, font_size * 0.75f);
            if (node.numerator) measure_node(*node.numerator, sub_size, false, font_system);
            if (node.denominator) measure_node(*node.denominator, sub_size, false, font_system);

            float num_w = node.numerator ? node.numerator->width : 0.0f;
            float den_w = node.denominator ? node.denominator->width : 0.0f;
            float num_h = node.numerator ? node.numerator->height() : sub_size;
            float den_h = node.denominator ? node.denominator->height() : sub_size;

            float pad_x = font_size * 0.28f;
            float gap_v = font_size * 0.18f;
            float axis_h = font_size * 0.30f; // Mathematical axis height above baseline

            node.width = std::max(num_w, den_w) + 2.0f * pad_x;
            node.ascent = axis_h + gap_v + num_h;
            node.descent = std::max(0.0f, (den_h + gap_v) - axis_h);
            break;
        }

        case MathNodeType::Sqrt: {
            if (node.numerator) measure_node(*node.numerator, font_size, display_mode, font_system);
            float deg_size = std::max(8.5f, font_size * 0.60f);
            if (node.denominator) measure_node(*node.denominator, deg_size, false, font_system);

            float rad_w = node.numerator ? node.numerator->width : font_size * 0.5f;
            float rad_asc = node.numerator ? node.numerator->ascent : font_size * 0.75f;
            float rad_desc = node.numerator ? node.numerator->descent : font_size * 0.20f;

            float hook_w = font_size * 0.62f;
            if (node.denominator) {
                hook_w = std::max(hook_w, node.denominator->width + font_size * 0.25f);
            }
            float pad_top = font_size * 0.20f;
            float pad_right = font_size * 0.15f;

            node.width = hook_w + rad_w + pad_right;
            node.ascent = rad_asc + pad_top;
            node.descent = rad_desc + 2.0f;
            break;
        }

        case MathNodeType::Script: {
            if (node.numerator) measure_node(*node.numerator, font_size, display_mode, font_system);
            float script_size = std::max(8.5f, font_size * 0.68f);
            if (node.denominator) measure_node(*node.denominator, script_size, false, font_system);
            if (node.superscript) measure_node(*node.superscript, script_size, false, font_system);

            float base_w = node.numerator ? node.numerator->width : 0.0f;
            float base_asc = node.numerator ? node.numerator->ascent : font_size * 0.75f;
            float base_desc = node.numerator ? node.numerator->descent : font_size * 0.20f;

            if (node.limits_above_below) {
                float sup_w = node.superscript ? node.superscript->width : 0.0f;
                float sub_w = node.denominator ? node.denominator->width : 0.0f;
                float sup_h = node.superscript ? node.superscript->height() + 2.0f : 0.0f;
                float sub_h = node.denominator ? node.denominator->height() + 2.0f : 0.0f;

                node.width = std::max({base_w, sup_w, sub_w}) + 4.0f;
                node.ascent = base_asc + sup_h;
                node.descent = base_desc + sub_h;
            } else {
                float sup_w = node.superscript ? node.superscript->width : 0.0f;
                float sub_w = node.denominator ? node.denominator->width : 0.0f;
                node.width = base_w + std::max(sup_w, sub_w) + 2.0f;
                node.ascent = base_asc + (node.superscript ? node.superscript->ascent * 0.55f : 0.0f);
                node.descent = base_desc + (node.denominator ? node.denominator->height() * 0.45f : 0.0f);
            }
            break;
        }

        case MathNodeType::Delimited: {
            if (node.numerator) measure_node(*node.numerator, font_size, display_mode, font_system);
            float inner_w = node.numerator ? node.numerator->width : 0.0f;
            float inner_asc = node.numerator ? node.numerator->ascent : font_size * 0.75f;
            float inner_desc = node.numerator ? node.numerator->descent : font_size * 0.20f;

            float delim_w = font_size * 0.42f;
            float left_w = node.left_delim.empty() ? 0.0f : delim_w;
            float right_w = node.right_delim.empty() ? 0.0f : delim_w;

            node.width = left_w + inner_w + right_w + 4.0f;
            node.ascent = inner_asc + 3.0f;
            node.descent = inner_desc + 3.0f;
            break;
        }

        case MathNodeType::Matrix: {
            float cell_size = std::max(10.0f, font_size * 0.88f);
            size_t num_rows = node.matrix_rows.size();
            size_t num_cols = 0;
            for (const auto& r : node.matrix_rows) num_cols = std::max(num_cols, r.size());

            if (num_rows == 0 || num_cols == 0) {
                node.width = font_size;
                node.ascent = font_size * 0.8f;
                node.descent = font_size * 0.2f;
                break;
            }

            std::vector<float> col_widths(num_cols, 0.0f);
            std::vector<float> row_heights(num_rows, cell_size * 1.2f);

            for (size_t r = 0; r < num_rows; ++r) {
                for (size_t c = 0; c < node.matrix_rows[r].size(); ++c) {
                    auto& cell = node.matrix_rows[r][c];
                    if (cell) {
                        measure_node(*cell, cell_size, false, font_system);
                        col_widths[c] = std::max(col_widths[c], cell->width);
                        row_heights[r] = std::max(row_heights[r], cell->height());
                    }
                }
            }

            float col_gap = font_size * 0.65f;
            float row_gap = font_size * 0.28f;
            float total_w = 0.0f;
            for (float cw : col_widths) total_w += cw;
            total_w += static_cast<float>(num_cols - 1) * col_gap;

            float total_h = 0.0f;
            for (float rh : row_heights) total_h += rh;
            total_h += static_cast<float>(num_rows - 1) * row_gap + 6.0f;

            float delim_w = (node.matrix_kind == MathMatrixKind::Plain) ? 0.0f : font_size * 0.48f;
            float right_delim_w = (node.matrix_kind == MathMatrixKind::Plain || node.matrix_kind == MathMatrixKind::Cases) ? 0.0f : delim_w;

            float axis_h = font_size * 0.30f;
            node.width = delim_w + right_delim_w + total_w + 8.0f;
            node.ascent = total_h * 0.5f + axis_h;
            node.descent = std::max(0.0f, total_h * 0.5f - axis_h);
            break;
        }

        case MathNodeType::Accent: {
            if (node.numerator) measure_node(*node.numerator, font_size, display_mode, font_system);
            float base_w = node.numerator ? node.numerator->width : font_size * 0.5f;
            float base_asc = node.numerator ? node.numerator->ascent : font_size * 0.75f;
            float base_desc = node.numerator ? node.numerator->descent : font_size * 0.20f;

            node.width = base_w;
            node.ascent = base_asc + (node.accent_kind == MathAccentKind::Underline ? 0.0f : font_size * 0.22f);
            node.descent = base_desc + (node.accent_kind == MathAccentKind::Underline ? font_size * 0.18f : 0.0f);
            break;
        }
    }
}

void position_node(MathNode& node, float x, float baseline_y) {
    node.x = x;
    node.y = baseline_y;
    float font_size = node.computed_font_size;

    switch (node.type) {
        case MathNodeType::Row: {
            float cur_x = x;
            for (auto& ch : node.children) {
                if (!ch) continue;
                position_node(*ch, cur_x, baseline_y);
                cur_x += ch->width;
            }
            break;
        }

        case MathNodeType::Fraction: {
            float axis_y = baseline_y - font_size * 0.30f;
            float gap_v = font_size * 0.18f;
            if (node.numerator) {
                float nx = x + (node.width - node.numerator->width) * 0.5f;
                float ny = axis_y - gap_v - node.numerator->descent;
                position_node(*node.numerator, nx, ny);
            }
            if (node.denominator) {
                float dx = x + (node.width - node.denominator->width) * 0.5f;
                float dy = axis_y + gap_v + node.denominator->ascent;
                position_node(*node.denominator, dx, dy);
            }
            break;
        }

        case MathNodeType::Sqrt: {
            float hook_w = font_size * 0.62f;
            if (node.denominator) {
                hook_w = std::max(hook_w, node.denominator->width + font_size * 0.25f);
                position_node(*node.denominator, x + 2.0f, baseline_y - font_size * 0.42f);
            }
            if (node.numerator) {
                position_node(*node.numerator, x + hook_w, baseline_y);
            }
            break;
        }

        case MathNodeType::Script: {
            if (node.limits_above_below) {
                if (node.numerator) {
                    float bx = x + (node.width - node.numerator->width) * 0.5f;
                    position_node(*node.numerator, bx, baseline_y);
                }
                float base_asc = node.numerator ? node.numerator->ascent : font_size * 0.75f;
                float base_desc = node.numerator ? node.numerator->descent : font_size * 0.20f;
                if (node.superscript) {
                    float sx = x + (node.width - node.superscript->width) * 0.5f;
                    float sy = baseline_y - base_asc - node.superscript->descent - 2.0f;
                    position_node(*node.superscript, sx, sy);
                }
                if (node.denominator) {
                    float dx = x + (node.width - node.denominator->width) * 0.5f;
                    float dy = baseline_y + base_desc + node.denominator->ascent + 2.0f;
                    position_node(*node.denominator, dx, dy);
                }
            } else {
                float base_w = 0.0f;
                if (node.numerator) {
                    position_node(*node.numerator, x, baseline_y);
                    base_w = node.numerator->width;
                }
                if (node.superscript) {
                    position_node(*node.superscript, x + base_w + 1.0f, baseline_y - font_size * 0.38f);
                }
                if (node.denominator) {
                    position_node(*node.denominator, x + base_w + 1.0f, baseline_y + font_size * 0.24f);
                }
            }
            break;
        }

        case MathNodeType::Delimited: {
            float delim_w = node.left_delim.empty() ? 0.0f : font_size * 0.42f;
            if (node.numerator) {
                position_node(*node.numerator, x + delim_w + 2.0f, baseline_y);
            }
            break;
        }

        case MathNodeType::Matrix: {
            size_t num_rows = node.matrix_rows.size();
            size_t num_cols = 0;
            for (const auto& r : node.matrix_rows) num_cols = std::max(num_cols, r.size());
            if (num_rows == 0 || num_cols == 0) break;

            std::vector<float> col_widths(num_cols, 0.0f);
            std::vector<float> row_ascs(num_rows, font_size * 0.65f);
            std::vector<float> row_descs(num_rows, font_size * 0.20f);

            for (size_t r = 0; r < num_rows; ++r) {
                for (size_t c = 0; c < node.matrix_rows[r].size(); ++c) {
                    const auto& cell = node.matrix_rows[r][c];
                    if (cell) {
                        col_widths[c] = std::max(col_widths[c], cell->width);
                        row_ascs[r] = std::max(row_ascs[r], cell->ascent);
                        row_descs[r] = std::max(row_descs[r], cell->descent);
                    }
                }
            }

            float col_gap = font_size * 0.65f;
            float row_gap = font_size * 0.28f;
            float delim_w = (node.matrix_kind == MathMatrixKind::Plain) ? 0.0f : font_size * 0.48f;

            float cur_y = baseline_y - node.ascent + 3.0f;
            for (size_t r = 0; r < num_rows; ++r) {
                float row_base_y = cur_y + row_ascs[r];
                float cur_x = x + delim_w + 4.0f;
                for (size_t c = 0; c < node.matrix_rows[r].size(); ++c) {
                    auto& cell = node.matrix_rows[r][c];
                    if (cell) {
                        float cell_x = (node.matrix_kind == MathMatrixKind::Cases)
                            ? cur_x
                            : cur_x + (col_widths[c] - cell->width) * 0.5f;
                        position_node(*cell, cell_x, row_base_y);
                    }
                    cur_x += col_widths[c] + col_gap;
                }
                cur_y += row_ascs[r] + row_descs[r] + row_gap;
            }
            break;
        }

        case MathNodeType::Accent: {
            if (node.numerator) {
                position_node(*node.numerator, x, baseline_y);
            }
            break;
        }

        case MathNodeType::Symbol:
        case MathNodeType::Text:
        case MathNodeType::Space:
            break;
    }
}

void render_node_recursive(
    ICanvas& canvas,
    const MathNode& node,
    float offset_x,
    float offset_y,
    Color color,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    float x = offset_x + node.x;
    float base_y = offset_y + node.y;
    float fsize = node.computed_font_size;
    Paint p(color);

    switch (node.type) {
        case MathNodeType::Row: {
            for (const auto& ch : node.children) {
                if (ch) render_node_recursive(canvas, *ch, offset_x, offset_y, color, font_system, glyph_cache);
            }
            break;
        }

        case MathNodeType::Symbol:
        case MathNodeType::Text: {
            draw_math_string(canvas, node.text, x, base_y, fsize, node.font_style, color, font_system, glyph_cache);
            break;
        }

        case MathNodeType::Fraction: {
            if (node.numerator) {
                render_node_recursive(canvas, *node.numerator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            if (node.denominator) {
                render_node_recursive(canvas, *node.denominator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            if (node.text != "nobar") {
                float axis_y = base_y - fsize * 0.30f;
                float bar_pad = fsize * 0.12f;
                float thickness = std::max(1.1f, fsize * 0.08f);
                if (auto r = Rect::from_xywh(x + bar_pad, axis_y - thickness * 0.5f, node.width - 2.0f * bar_pad, thickness)) {
                    canvas.fill_rect(*r, p);
                }
            }
            break;
        }

        case MathNodeType::Sqrt: {
            if (node.denominator) {
                render_node_recursive(canvas, *node.denominator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            if (node.numerator) {
                render_node_recursive(canvas, *node.numerator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            float hook_w = fsize * 0.62f;
            if (node.denominator) {
                hook_w = std::max(hook_w, node.denominator->width + fsize * 0.25f);
            }
            float top_y = base_y - node.ascent + 2.0f;
            float bot_y = base_y + node.descent - 1.0f;
            float mid_y = top_y + (bot_y - top_y) * 0.58f;

            PathBuilder pb;
            pb.move_to(x + hook_w * 0.15f, mid_y);
            pb.line_to(x + hook_w * 0.35f, mid_y - 2.0f);
            pb.line_to(x + hook_w * 0.62f, bot_y);
            pb.line_to(x + hook_w, top_y);
            pb.line_to(x + node.width - 2.0f, top_y);

            Stroke s(std::max(1.2f, fsize * 0.085f));
            s.line_cap = LineCap::Round;
            s.line_join = LineJoin::Round;
            canvas.stroke_path(fin(pb), p, s);
            break;
        }

        case MathNodeType::Script: {
            if (node.numerator) {
                render_node_recursive(canvas, *node.numerator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            if (node.denominator) {
                render_node_recursive(canvas, *node.denominator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            if (node.superscript) {
                render_node_recursive(canvas, *node.superscript, offset_x, offset_y, color, font_system, glyph_cache);
            }
            break;
        }

        case MathNodeType::Delimited: {
            float top_y = base_y - node.ascent;
            float h = node.height();
            float dw = fsize * 0.42f;
            if (!node.left_delim.empty()) {
                draw_vector_delimiter(canvas, node.left_delim, x, top_y, h, dw, color);
            }
            if (node.numerator) {
                render_node_recursive(canvas, *node.numerator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            if (!node.right_delim.empty()) {
                draw_vector_delimiter(canvas, node.right_delim, x + node.width - dw, top_y, h, dw, color);
            }
            break;
        }

        case MathNodeType::Matrix: {
            float top_y = base_y - node.ascent;
            float h = node.height();
            float dw = fsize * 0.48f;

            if (node.matrix_kind == MathMatrixKind::Paren) {
                draw_vector_delimiter(canvas, "(", x, top_y, h, dw, color);
                draw_vector_delimiter(canvas, ")", x + node.width - dw, top_y, h, dw, color);
            } else if (node.matrix_kind == MathMatrixKind::Bracket) {
                draw_vector_delimiter(canvas, "[", x, top_y, h, dw, color);
                draw_vector_delimiter(canvas, "]", x + node.width - dw, top_y, h, dw, color);
            } else if (node.matrix_kind == MathMatrixKind::Brace) {
                draw_vector_delimiter(canvas, "{", x, top_y, h, dw, color);
                draw_vector_delimiter(canvas, "}", x + node.width - dw, top_y, h, dw, color);
            } else if (node.matrix_kind == MathMatrixKind::Vert) {
                draw_vector_delimiter(canvas, "|", x, top_y, h, dw, color);
                draw_vector_delimiter(canvas, "|", x + node.width - dw, top_y, h, dw, color);
            } else if (node.matrix_kind == MathMatrixKind::DoubleVert) {
                draw_vector_delimiter(canvas, "‖", x, top_y, h, dw, color);
                draw_vector_delimiter(canvas, "‖", x + node.width - dw, top_y, h, dw, color);
            } else if (node.matrix_kind == MathMatrixKind::Cases) {
                draw_vector_delimiter(canvas, "{", x, top_y, h, dw, color);
            }

            for (const auto& row : node.matrix_rows) {
                for (const auto& cell : row) {
                    if (cell) {
                        render_node_recursive(canvas, *cell, offset_x, offset_y, color, font_system, glyph_cache);
                    }
                }
            }
            break;
        }

        case MathNodeType::Accent: {
            if (node.numerator) {
                render_node_recursive(canvas, *node.numerator, offset_x, offset_y, color, font_system, glyph_cache);
            }
            float bw = node.width;
            float acc_y = base_y - (node.numerator ? node.numerator->ascent : fsize * 0.72f) - 2.0f;
            Stroke s(std::max(1.1f, fsize * 0.08f));
            s.line_cap = LineCap::Round;
            s.line_join = LineJoin::Round;
            PathBuilder pb;

            if (node.accent_kind == MathAccentKind::Bar || node.accent_kind == MathAccentKind::Overline) {
                pb.move_to(x + 1.0f, acc_y);
                pb.line_to(x + bw - 1.0f, acc_y);
                canvas.stroke_path(fin(pb), p, s);
            } else if (node.accent_kind == MathAccentKind::Underline) {
                float uy = base_y + (node.numerator ? node.numerator->descent : fsize * 0.18f) + 2.0f;
                pb.move_to(x + 1.0f, uy);
                pb.line_to(x + bw - 1.0f, uy);
                canvas.stroke_path(fin(pb), p, s);
            } else if (node.accent_kind == MathAccentKind::Hat) {
                pb.move_to(x + bw * 0.15f, acc_y + 3.0f);
                pb.line_to(x + bw * 0.50f, acc_y - 1.5f);
                pb.line_to(x + bw * 0.85f, acc_y + 3.0f);
                canvas.stroke_path(fin(pb), p, s);
            } else if (node.accent_kind == MathAccentKind::Vec) {
                pb.move_to(x + 1.0f, acc_y);
                pb.line_to(x + bw - 1.0f, acc_y);
                pb.move_to(x + bw - 4.5f, acc_y - 2.5f);
                pb.line_to(x + bw - 1.0f, acc_y);
                pb.line_to(x + bw - 4.5f, acc_y + 2.5f);
                canvas.stroke_path(fin(pb), p, s);
            } else if (node.accent_kind == MathAccentKind::Tilde) {
                pb.move_to(x + bw * 0.15f, acc_y + 1.5f);
                pb.quad_to(x + bw * 0.35f, acc_y - 2.0f, x + bw * 0.50f, acc_y + 0.5f);
                pb.quad_to(x + bw * 0.65f, acc_y + 3.0f, x + bw * 0.85f, acc_y - 1.0f);
                canvas.stroke_path(fin(pb), p, s);
            } else if (node.accent_kind == MathAccentKind::Dot) {
                if (auto r = Rect::from_xywh(x + bw * 0.5f - 1.2f, acc_y - 1.2f, 2.4f, 2.4f)) {
                    canvas.fill_rect(*r, p);
                }
            } else if (node.accent_kind == MathAccentKind::DDot) {
                if (auto r1 = Rect::from_xywh(x + bw * 0.35f - 1.2f, acc_y - 1.2f, 2.4f, 2.4f)) {
                    canvas.fill_rect(*r1, p);
                }
                if (auto r2 = Rect::from_xywh(x + bw * 0.65f - 1.2f, acc_y - 1.2f, 2.4f, 2.4f)) {
                    canvas.fill_rect(*r2, p);
                }
            }
            break;
        }

        case MathNodeType::Space:
            break;
    }
}

} // namespace

std::shared_ptr<MathNode> MathEngine::parse(std::string_view latex, bool display_mode) {
    LatexParser parser(latex, display_mode);
    return parser.parse_row();
}

void MathEngine::layout(
    MathNode& root,
    float base_font_size,
    bool display_mode,
    text::FontSystem& font_system
) {
    measure_node(root, base_font_size, display_mode, font_system);
    position_node(root, 0.0f, root.ascent);
}

void MathEngine::render(
    ICanvas& canvas,
    const MathNode& root,
    float x,
    float y,
    Color color,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    render_node_recursive(canvas, root, x, y, color, font_system, glyph_cache);
}

std::string MathEngine::to_plain_text(const MathNode& node) {
    std::string out;
    switch (node.type) {
        case MathNodeType::Row:
            for (const auto& ch : node.children) {
                if (ch) out += to_plain_text(*ch);
            }
            break;
        case MathNodeType::Symbol:
        case MathNodeType::Text:
            out += node.text;
            break;
        case MathNodeType::Fraction:
            out += "(";
            if (node.numerator) out += to_plain_text(*node.numerator);
            out += ")/(";
            if (node.denominator) out += to_plain_text(*node.denominator);
            out += ")";
            break;
        case MathNodeType::Sqrt:
            out += "√(";
            if (node.numerator) out += to_plain_text(*node.numerator);
            out += ")";
            break;
        case MathNodeType::Script:
            if (node.numerator) out += to_plain_text(*node.numerator);
            if (node.denominator) out += "_" + to_plain_text(*node.denominator);
            if (node.superscript) out += "^" + to_plain_text(*node.superscript);
            break;
        case MathNodeType::Delimited:
            out += node.left_delim;
            if (node.numerator) out += to_plain_text(*node.numerator);
            out += node.right_delim;
            break;
        case MathNodeType::Matrix:
            out += "[";
            for (size_t r = 0; r < node.matrix_rows.size(); ++r) {
                if (r > 0) out += "; ";
                for (size_t c = 0; c < node.matrix_rows[r].size(); ++c) {
                    if (c > 0) out += ", ";
                    if (node.matrix_rows[r][c]) out += to_plain_text(*node.matrix_rows[r][c]);
                }
            }
            out += "]";
            break;
        case MathNodeType::Accent:
            if (node.numerator) out += to_plain_text(*node.numerator);
            break;
        case MathNodeType::Space:
            out += " ";
            break;
    }
    return out;
}

} // namespace nisaba::markdown
