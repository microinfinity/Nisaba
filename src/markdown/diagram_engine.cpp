#include "nisaba/markdown/diagram_engine.hpp"
#include "nisaba/canvas/canvas.hpp"
#include "nisaba/pdf/pdf_canvas.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/math/rrect.hpp"
#include "nisaba/text/buffer.hpp"
#ifdef NISABA_GLEW
#include "nisaba/gpu/gpu_canvas.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace nisaba::markdown {

namespace {

inline std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == ';')) s.remove_suffix(1);
    return s;
}

inline std::string strip_quotes(std::string_view s) {
    s = trim_sv(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return std::string(s);
}

std::vector<std::string_view> split_lines_sv(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

/// Finalizes a PathBuilder into a Path (empty Path when degenerate).
inline Path fin(PathBuilder& pb) {
    return pb.finish().value_or(Path());
}

float measure_label(std::string_view text, float font_size, bool bold, text::FontSystem& font_system) {
    if (text.empty()) return 0.0f;
    text::Attrs attrs;
    attrs.set_family(text::Family::sans_serif());
    if (bold) attrs.set_weight(text::Weight::Bold);

    text::Buffer buf(text::Metrics(font_size, font_size * 1.25f));
    buf.set_wrap(text::Wrap::None);
    buf.set_text(text, attrs);
    buf.shape_until_scroll(font_system);
    float w = buf.max_line_width();
    if (w <= 0.0f) {
        w = static_cast<float>(text.size()) * font_size * 0.56f;
    }
    return w;
}

void draw_label(
    ICanvas& canvas,
    std::string_view text,
    float x,
    float y,
    float width,
    float font_size,
    bool bold,
    text::Align align,
    Color color,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    if (text.empty()) return;
    text::Attrs attrs;
    attrs.set_family(text::Family::sans_serif());
    if (bold) attrs.set_weight(text::Weight::Bold);

    text::Buffer buf(text::Metrics(font_size, font_size * 1.25f));
    buf.set_wrap(text::Wrap::None);
    buf.set_align(align);
    if (width > 1.0f) {
        buf.set_size(width, std::nullopt);
    }
    buf.set_text(text, attrs);
    buf.shape_until_scroll(font_system);

    if (canvas.canvas_kind() == CanvasKind::Software) {
        auto& cpu_canvas = static_cast<Canvas&>(canvas);
        buf.draw(cpu_canvas, glyph_cache, font_system, color, x, y);
        return;
    }

#ifdef NISABA_GLEW
    if (canvas.canvas_kind() == CanvasKind::Gpu) {
        auto& gpu_canvas = static_cast<gpu::GpuCanvas&>(canvas);
        gpu_canvas.draw_text_buffer(buf, font_system, Point::from_xy(x, y), color);
        return;
    }
#endif

    if (canvas.canvas_kind() == CanvasKind::Pdf) {
        auto& pdf_canvas = static_cast<pdf::PdfCanvas&>(canvas);
        float text_w = buf.max_line_width();
        float tx = x;
        if (align == text::Align::Center && width > text_w) {
            tx = x + (width - text_w) * 0.5f;
        } else if (align == text::Align::Right && width > text_w) {
            tx = x + (width - text_w);
        }
        pdf_canvas.draw_text(text, tx, y, font_size, color, bold ? "Helvetica-Bold" : "Helvetica");
    }
}

void draw_arrow_head(ICanvas& canvas, float tip_x, float tip_y, float dir_x, float dir_y, float size, Color color) {
    float len = std::hypot(dir_x, dir_y);
    if (len < 1e-4f) return;
    dir_x /= len;
    dir_y /= len;
    float perp_x = -dir_y;
    float perp_y = dir_x;

    float bx = tip_x - dir_x * size;
    float by = tip_y - dir_y * size;
    float w = size * 0.52f;

    PathBuilder pb;
    pb.move_to(tip_x, tip_y);
    pb.line_to(bx + perp_x * w, by + perp_y * w);
    pb.line_to(bx - perp_x * w, by - perp_y * w);
    pb.close();

    canvas.fill_path(fin(pb), Paint(color));
}

// Parse a Flowchart / State node specification like `A["My Label"]`, `B{Decision}`, `C((Circle))`, `[*]`
void parse_node_token(
    std::string_view token,
    std::string& out_id,
    std::string& out_label,
    DiagramNodeShape& out_shape,
    bool& has_explicit_shape
) {
    token = trim_sv(token);
    has_explicit_shape = false;
    out_shape = DiagramNodeShape::RoundRect;

    if (token == "[*]") {
        out_id = "[*]";
        out_label = "";
        out_shape = DiagramNodeShape::StartState;
        has_explicit_shape = true;
        return;
    }

    size_t i = 0;
    while (i < token.size() && (std::isalnum(static_cast<unsigned char>(token[i])) || token[i] == '_' || token[i] == '-')) {
        i++;
    }
    out_id = std::string(token.substr(0, i));
    if (out_id.empty()) {
        out_id = std::string(token);
        out_label = out_id;
        return;
    }

    auto rest = trim_sv(token.substr(i));
    if (rest.empty()) {
        out_label = out_id;
        return;
    }

    has_explicit_shape = true;
    if (rest.starts_with("([") && rest.ends_with("])") && rest.size() >= 4) {
        out_shape = DiagramNodeShape::Stadium;
        out_label = strip_quotes(rest.substr(2, rest.size() - 4));
    } else if (rest.starts_with("[[") && rest.ends_with("]]") && rest.size() >= 4) {
        out_shape = DiagramNodeShape::Subroutine;
        out_label = strip_quotes(rest.substr(2, rest.size() - 4));
    } else if (rest.starts_with("((") && rest.ends_with("))") && rest.size() >= 4) {
        out_shape = DiagramNodeShape::Circle;
        out_label = strip_quotes(rest.substr(2, rest.size() - 4));
    } else if (rest.starts_with("{{") && rest.ends_with("}}") && rest.size() >= 4) {
        out_shape = DiagramNodeShape::Hexagon;
        out_label = strip_quotes(rest.substr(2, rest.size() - 4));
    } else if (rest.front() == '[' && rest.back() == ']' && rest.size() >= 2) {
        out_shape = DiagramNodeShape::Rect;
        out_label = strip_quotes(rest.substr(1, rest.size() - 2));
    } else if (rest.front() == '(' && rest.back() == ')' && rest.size() >= 2) {
        out_shape = DiagramNodeShape::RoundRect;
        out_label = strip_quotes(rest.substr(1, rest.size() - 2));
    } else if (rest.front() == '{' && rest.back() == '}' && rest.size() >= 2) {
        out_shape = DiagramNodeShape::Diamond;
        out_label = strip_quotes(rest.substr(1, rest.size() - 2));
    } else {
        out_label = strip_quotes(rest);
    }
}

size_t get_or_add_node(
    DiagramModel& model,
    std::string_view token,
    bool is_target = false
) {
    std::string id, label;
    DiagramNodeShape shape;
    bool explicit_shape = false;
    parse_node_token(token, id, label, shape, explicit_shape);

    if (id == "[*]") {
        id = is_target ? "[*]_end" : "[*]_start";
        shape = is_target ? DiagramNodeShape::EndState : DiagramNodeShape::StartState;
    }

    for (size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].id == id) {
            if (explicit_shape) {
                if (!label.empty()) model.nodes[i].label = label;
                model.nodes[i].shape = shape;
            }
            return i;
        }
    }

    DiagramNode n;
    n.id = id;
    n.label = label;
    n.shape = shape;
    model.nodes.push_back(std::move(n));
    return model.nodes.size() - 1;
}

size_t get_or_add_actor(DiagramModel& model, std::string_view id, std::string_view label = {}) {
    std::string clean_id = strip_quotes(id);
    std::string clean_label = label.empty() ? clean_id : strip_quotes(label);
    for (size_t i = 0; i < model.actors.size(); ++i) {
        if (model.actors[i].id == clean_id) {
            if (!label.empty()) model.actors[i].label = clean_label;
            return i;
        }
    }
    DiagramNode actor;
    actor.id = clean_id;
    actor.label = clean_label;
    model.actors.push_back(std::move(actor));
    return model.actors.size() - 1;
}

std::vector<std::string> parse_bracket_list(std::string_view s) {
    std::vector<std::string> items;
    size_t l = s.find('[');
    size_t r = s.rfind(']');
    if (l == std::string_view::npos || r == std::string_view::npos || r <= l) return items;
    auto inner = s.substr(l + 1, r - l - 1);
    size_t start = 0;
    while (start < inner.size()) {
        size_t comma = inner.find(',', start);
        if (comma == std::string_view::npos) comma = inner.size();
        auto val = strip_quotes(inner.substr(start, comma - start));
        if (!val.empty()) items.push_back(std::move(val));
        start = comma + 1;
    }
    return items;
}

} // namespace

std::shared_ptr<DiagramModel> DiagramEngine::parse(std::string_view mermaid_code) {
    auto model = std::make_shared<DiagramModel>();
    auto lines = split_lines_sv(mermaid_code);

    bool header_parsed = false;

    for (auto raw_line : lines) {
        auto line = trim_sv(raw_line);
        if (line.empty() || line.starts_with("%%")) continue;

        if (!header_parsed) {
            header_parsed = true;
            if (line.starts_with("flowchart") || line.starts_with("graph")) {
                model->kind = DiagramKind::Flowchart;
                if (line.find("LR") != std::string_view::npos) model->direction = DiagramDirection::LeftRight;
                else if (line.find("RL") != std::string_view::npos) model->direction = DiagramDirection::RightLeft;
                else if (line.find("BT") != std::string_view::npos) model->direction = DiagramDirection::BottomTop;
                else model->direction = DiagramDirection::TopDown;
                continue;
            }
            if (line.starts_with("sequenceDiagram")) {
                model->kind = DiagramKind::Sequence;
                continue;
            }
            if (line.starts_with("stateDiagram")) {
                model->kind = DiagramKind::State;
                model->direction = DiagramDirection::TopDown;
                continue;
            }
            if (line.starts_with("xychart-beta") || line.starts_with("xychart")) {
                model->kind = DiagramKind::XYChart;
                continue;
            }
            if (line.starts_with("pie")) {
                model->kind = DiagramKind::PieChart;
                size_t tpos = line.find("title");
                if (tpos != std::string_view::npos) {
                    model->title = strip_quotes(line.substr(tpos + 5));
                }
                continue;
            }
        }

        if (line.starts_with("title ")) {
            model->title = strip_quotes(line.substr(6));
            continue;
        }
        if (line.starts_with("direction ")) {
            auto dir = trim_sv(line.substr(10));
            if (dir == "LR") model->direction = DiagramDirection::LeftRight;
            else if (dir == "RL") model->direction = DiagramDirection::RightLeft;
            else if (dir == "BT") model->direction = DiagramDirection::BottomTop;
            else model->direction = DiagramDirection::TopDown;
            continue;
        }
        if (line.starts_with("subgraph ") || line == "end") {
            continue;
        }

        // 1. Sequence Diagram Lines
        if (model->kind == DiagramKind::Sequence) {
            if (line.starts_with("participant ") || line.starts_with("actor ")) {
                auto rest = trim_sv(line.substr(line.starts_with("actor ") ? 6 : 12));
                size_t as_pos = rest.find(" as ");
                if (as_pos != std::string_view::npos) {
                    get_or_add_actor(*model, rest.substr(0, as_pos), rest.substr(as_pos + 4));
                } else {
                    get_or_add_actor(*model, rest);
                }
                continue;
            }
            if (line.starts_with("Note ") || line.starts_with("note ")) {
                size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    auto note_txt = strip_quotes(line.substr(colon + 1));
                    SequenceMessage msg;
                    msg.from_actor = model->actors.empty() ? 0 : model->actors.size() - 1;
                    msg.to_actor = msg.from_actor;
                    msg.text = note_txt;
                    msg.is_note = true;
                    model->messages.push_back(std::move(msg));
                }
                continue;
            }

            // Arrows: -->>, ->>, -->, ->, --x, -x
            struct SeqArrow { std::string_view token; DiagramEdgeStyle style; };
            static constexpr SeqArrow arrows[] = {
                {"-->>", DiagramEdgeStyle::DottedArrow},
                {"->>",  DiagramEdgeStyle::SolidArrow},
                {"-->",  DiagramEdgeStyle::DottedArrow},
                {"->",   DiagramEdgeStyle::OpenSolid},
                {"--x",  DiagramEdgeStyle::DottedArrow},
                {"-x",   DiagramEdgeStyle::SolidArrow}
            };

            for (const auto& ar : arrows) {
                size_t apos = line.find(ar.token);
                if (apos != std::string_view::npos) {
                    auto left = trim_sv(line.substr(0, apos));
                    auto right_full = trim_sv(line.substr(apos + ar.token.size()));
                    // Strip optional +/- activation modifiers
                    if (!right_full.empty() && (right_full.front() == '+' || right_full.front() == '-')) {
                        right_full.remove_prefix(1);
                    }
                    size_t colon = right_full.find(':');
                    std::string_view right_actor = (colon != std::string_view::npos) ? right_full.substr(0, colon) : right_full;
                    std::string_view msg_text = (colon != std::string_view::npos) ? right_full.substr(colon + 1) : "";

                    size_t from_i = get_or_add_actor(*model, left);
                    size_t to_i = get_or_add_actor(*model, right_actor);

                    SequenceMessage msg;
                    msg.from_actor = from_i;
                    msg.to_actor = to_i;
                    msg.text = strip_quotes(msg_text);
                    msg.style = ar.style;
                    model->messages.push_back(std::move(msg));
                    break;
                }
            }
            continue;
        }

        // 2. XYChart Lines
        if (model->kind == DiagramKind::XYChart) {
            if (line.starts_with("x-axis")) {
                model->x_categories = parse_bracket_list(line);
                continue;
            }
            if (line.starts_with("y-axis")) {
                auto rest = trim_sv(line.substr(6));
                size_t arrow = rest.find("-->");
                if (arrow != std::string_view::npos) {
                    auto after_arrow = trim_sv(rest.substr(arrow + 3));
                    auto before_arrow = trim_sv(rest.substr(0, arrow));
                    size_t last_sp = before_arrow.find_last_of(" \t\"");
                    std::string_view min_sv = (last_sp != std::string_view::npos) ? before_arrow.substr(last_sp + 1) : before_arrow;
                    if (last_sp != std::string_view::npos) {
                        model->y_axis_title = strip_quotes(before_arrow.substr(0, last_sp + 1));
                    }
                    model->y_min = std::strtof(std::string(min_sv).c_str(), nullptr);
                    model->y_max = std::strtof(std::string(after_arrow).c_str(), nullptr);
                    model->auto_y_range = false;
                } else {
                    model->y_axis_title = strip_quotes(rest);
                }
                continue;
            }
            if (line.starts_with("bar") || line.starts_with("line")) {
                ChartSeries s;
                s.is_bar = line.starts_with("bar");
                auto vals = parse_bracket_list(line);
                for (const auto& v : vals) {
                    s.values.push_back(std::strtof(v.c_str(), nullptr));
                }
                if (!s.values.empty()) {
                    model->series.push_back(std::move(s));
                }
                continue;
            }
            continue;
        }

        // 3. PieChart Lines
        if (model->kind == DiagramKind::PieChart) {
            size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                PieSlice sl;
                sl.label = strip_quotes(line.substr(0, colon));
                sl.value = std::max(0.0f, std::strtof(std::string(trim_sv(line.substr(colon + 1))).c_str(), nullptr));
                if (sl.value > 0.0f) {
                    model->pie_slices.push_back(std::move(sl));
                }
            }
            continue;
        }

        // 4. Flowchart & State Diagram Lines
        struct EdgeOp { std::string_view token; DiagramEdgeStyle style; };
        static constexpr EdgeOp edge_ops[] = {
            {"<-->", DiagramEdgeStyle::BiDirectional},
            {"-.->", DiagramEdgeStyle::DottedArrow},
            {"==>",  DiagramEdgeStyle::ThickArrow},
            {"-->",  DiagramEdgeStyle::SolidArrow},
            {"---",  DiagramEdgeStyle::OpenSolid}
        };

        bool matched_edge = false;
        for (const auto& op : edge_ops) {
            size_t epos = line.find(op.token);
            if (epos != std::string_view::npos) {
                matched_edge = true;
                auto lhs = trim_sv(line.substr(0, epos));
                auto rhs = trim_sv(line.substr(epos + op.token.size()));
                std::string edge_label;

                // Check for `A -- text --> B`
                if (op.token == "-->" && lhs.find(" -- ") != std::string_view::npos) {
                    size_t mid = lhs.find(" -- ");
                    edge_label = strip_quotes(lhs.substr(mid + 4));
                    lhs = trim_sv(lhs.substr(0, mid));
                }

                // Check for `-->|label| B`
                if (rhs.starts_with("|")) {
                    size_t end_pipe = rhs.find('|', 1);
                    if (end_pipe != std::string_view::npos) {
                        edge_label = strip_quotes(rhs.substr(1, end_pipe - 1));
                        rhs = trim_sv(rhs.substr(end_pipe + 1));
                    }
                }

                // Check for State diagram `S1 --> S2 : label`
                size_t colon = rhs.find(':');
                if (colon != std::string_view::npos && rhs.find('[') == std::string_view::npos) {
                    if (edge_label.empty()) {
                        edge_label = strip_quotes(rhs.substr(colon + 1));
                    }
                    rhs = trim_sv(rhs.substr(0, colon));
                }

                size_t u = get_or_add_node(*model, lhs, false);
                size_t v = get_or_add_node(*model, rhs, true);

                DiagramEdge edge;
                edge.from_idx = u;
                edge.to_idx = v;
                edge.label = edge_label;
                edge.style = op.style;
                model->edges.push_back(std::move(edge));
                break;
            }
        }

        if (!matched_edge) {
            // Standalone node declaration or state description `State1 : Description`
            size_t colon = line.find(':');
            if (colon != std::string_view::npos && line.find('[') == std::string_view::npos) {
                auto id_part = trim_sv(line.substr(0, colon));
                auto desc_part = strip_quotes(line.substr(colon + 1));
                size_t idx = get_or_add_node(*model, id_part);
                model->nodes[idx].label = desc_part;
            } else {
                get_or_add_node(*model, line);
            }
        }
    }

    return model;
}

float DiagramEngine::layout(
    DiagramModel& model,
    float max_width,
    const MarkdownStyle& style,
    text::FontSystem& font_system
) {
    (void)style;
    model.width = max_width;
    float title_h = model.title.empty() ? 0.0f : 32.0f;

    // 1. Sequence Diagram Layout
    if (model.kind == DiagramKind::Sequence) {
        if (model.actors.empty()) {
            model.height = 60.0f;
            return model.height;
        }
        size_t n_actors = model.actors.size();
        float pad_x = 28.0f;
        float usable_w = std::max(200.0f, max_width - 2.0f * pad_x);
        float spacing = usable_w / static_cast<float>(std::max<size_t>(1, n_actors));

        for (size_t i = 0; i < n_actors; ++i) {
            float lw = measure_label(model.actors[i].label, 13.5f, true, font_system);
            model.actors[i].width = std::clamp(lw + 28.0f, 88.0f, std::max(96.0f, spacing - 14.0f));
            model.actors[i].height = 36.0f;
            float cx = pad_x + (static_cast<float>(i) + 0.5f) * spacing;
            model.actors[i].x = cx - model.actors[i].width * 0.5f;
            model.actors[i].y = title_h + 16.0f;
        }

        float cur_y = title_h + 74.0f;
        for (auto& msg : model.messages) {
            msg.y = cur_y;
            cur_y += (msg.from_actor == msg.to_actor && !msg.is_note) ? 50.0f : 42.0f;
        }

        model.height = cur_y + 52.0f;
        return model.height;
    }

    // 2. XYChart Layout
    if (model.kind == DiagramKind::XYChart) {
        if (model.auto_y_range) {
            float min_v = 0.0f;
            float max_v = 10.0f;
            for (const auto& s : model.series) {
                for (float v : s.values) {
                    min_v = std::min(min_v, v);
                    max_v = std::max(max_v, v);
                }
            }
            model.y_min = min_v;
            model.y_max = (max_v <= min_v) ? min_v + 10.0f : max_v * 1.12f;
        }
        model.height = title_h + 230.0f;
        return model.height;
    }

    // 3. PieChart Layout
    if (model.kind == DiagramKind::PieChart) {
        model.height = title_h + 210.0f;
        return model.height;
    }

    // 4. Flowchart & State Diagram Layered Layout (Sugiyama Algorithm)
    if (model.nodes.empty()) {
        model.height = 60.0f;
        return model.height;
    }

    size_t n = model.nodes.size();
    for (auto& node : model.nodes) {
        if (node.shape == DiagramNodeShape::StartState || node.shape == DiagramNodeShape::EndState) {
            node.width = 24.0f;
            node.height = 24.0f;
        } else {
            float lw = measure_label(node.label, 13.5f, true, font_system);
            float pad_w = (node.shape == DiagramNodeShape::Diamond) ? 48.0f : 32.0f;
            node.width = std::clamp(lw + pad_w, 84.0f, 240.0f);
            node.height = (node.shape == DiagramNodeShape::Diamond) ? 54.0f :
                          (node.shape == DiagramNodeShape::Circle) ? std::max(54.0f, node.width * 0.75f) : 40.0f;
        }
        node.rank = 0;
    }

    // Cycle removal: DFS marks back edges, which are excluded from rank assignment.
    std::vector<bool> is_back_edge(model.edges.size(), false);
    {
        std::vector<uint8_t> state(n, 0); // 0 = unvisited, 1 = on stack, 2 = done
        std::vector<std::vector<size_t>> out_edges(n);
        for (size_t ei = 0; ei < model.edges.size(); ++ei) {
            const auto& e = model.edges[ei];
            if (e.from_idx < n && e.to_idx < n) out_edges[e.from_idx].push_back(ei);
        }
        struct Frame { size_t node; size_t next; };
        for (size_t root = 0; root < n; ++root) {
            if (state[root] != 0) continue;
            std::vector<Frame> stack{{root, 0}};
            state[root] = 1;
            while (!stack.empty()) {
                Frame& f = stack.back();
                if (f.next < out_edges[f.node].size()) {
                    size_t ei = out_edges[f.node][f.next++];
                    size_t to = model.edges[ei].to_idx;
                    if (state[to] == 1) {
                        is_back_edge[ei] = true;
                    } else if (state[to] == 0) {
                        state[to] = 1;
                        stack.push_back({to, 0});
                    }
                } else {
                    state[f.node] = 2;
                    stack.pop_back();
                }
            }
        }
    }

    // Longest-path rank assignment over the resulting DAG
    for (size_t iter = 0; iter < n; ++iter) {
        bool changed = false;
        for (size_t ei = 0; ei < model.edges.size(); ++ei) {
            const auto& e = model.edges[ei];
            if (is_back_edge[ei]) continue;
            if (e.from_idx < n && e.to_idx < n && e.from_idx != e.to_idx) {
                if (model.nodes[e.to_idx].rank < model.nodes[e.from_idx].rank + 1) {
                    model.nodes[e.to_idx].rank = model.nodes[e.from_idx].rank + 1;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }

    int max_rank = 0;
    for (const auto& node : model.nodes) max_rank = std::max(max_rank, node.rank);

    std::vector<std::vector<size_t>> layers(static_cast<size_t>(max_rank + 1));
    for (size_t i = 0; i < n; ++i) {
        layers[static_cast<size_t>(model.nodes[i].rank)].push_back(i);
    }

    bool is_horizontal = (model.direction == DiagramDirection::LeftRight || model.direction == DiagramDirection::RightLeft);

    if (!is_horizontal) {
        // Top-Down / Bottom-Top
        float cur_y = title_h + 24.0f;
        float rank_gap = 52.0f;
        float node_gap = 28.0f;

        for (size_t r = 0; r <= static_cast<size_t>(max_rank); ++r) {
            size_t layer_idx = (model.direction == DiagramDirection::BottomTop) ? (static_cast<size_t>(max_rank) - r) : r;
            const auto& layer = layers[layer_idx];
            if (layer.empty()) continue;

            float total_layer_w = 0.0f;
            float max_layer_h = 0.0f;
            for (size_t idx : layer) {
                total_layer_w += model.nodes[idx].width;
                max_layer_h = std::max(max_layer_h, model.nodes[idx].height);
            }
            total_layer_w += static_cast<float>(layer.size() - 1) * node_gap;

            float start_x = std::max(20.0f, (max_width - total_layer_w) * 0.5f);
            float cur_x = start_x;
            for (size_t idx : layer) {
                model.nodes[idx].x = cur_x;
                model.nodes[idx].y = cur_y + (max_layer_h - model.nodes[idx].height) * 0.5f;
                cur_x += model.nodes[idx].width + node_gap;
            }
            cur_y += max_layer_h + rank_gap;
        }
        model.height = cur_y - rank_gap + 24.0f;
    } else {
        // Left-Right / Right-Left
        float rank_gap = 54.0f;
        float v_gap = 24.0f;
        float total_ranks_w = 0.0f;
        std::vector<float> col_widths(static_cast<size_t>(max_rank + 1), 0.0f);
        float max_col_h = 0.0f;

        for (size_t r = 0; r <= static_cast<size_t>(max_rank); ++r) {
            float col_h = 0.0f;
            for (size_t idx : layers[r]) {
                col_widths[r] = std::max(col_widths[r], model.nodes[idx].width);
                col_h += model.nodes[idx].height;
            }
            if (!layers[r].empty()) {
                col_h += static_cast<float>(layers[r].size() - 1) * v_gap;
            }
            max_col_h = std::max(max_col_h, col_h);
            total_ranks_w += col_widths[r];
        }
        total_ranks_w += static_cast<float>(max_rank) * rank_gap;

        // If wider than max_width, compress rank_gap and node widths proportionally
        if (total_ranks_w > max_width - 40.0f && max_rank > 0) {
            float scale = (max_width - 40.0f) / total_ranks_w;
            rank_gap *= scale;
            for (float& cw : col_widths) cw *= scale;
            for (auto& nd : model.nodes) nd.width *= scale;
            total_ranks_w = max_width - 40.0f;
        }

        float cur_x = std::max(20.0f, (max_width - total_ranks_w) * 0.5f);
        for (size_t r = 0; r <= static_cast<size_t>(max_rank); ++r) {
            size_t layer_idx = (model.direction == DiagramDirection::RightLeft) ? (static_cast<size_t>(max_rank) - r) : r;
            const auto& layer = layers[layer_idx];
            float col_h = 0.0f;
            for (size_t idx : layer) col_h += model.nodes[idx].height;
            if (!layer.empty()) col_h += static_cast<float>(layer.size() - 1) * v_gap;

            float cur_y = title_h + 24.0f + (max_col_h - col_h) * 0.5f;
            for (size_t idx : layer) {
                model.nodes[idx].x = cur_x + (col_widths[layer_idx] - model.nodes[idx].width) * 0.5f;
                model.nodes[idx].y = cur_y;
                cur_y += model.nodes[idx].height + v_gap;
            }
            cur_x += col_widths[layer_idx] + rank_gap;
        }
        model.height = title_h + max_col_h + 48.0f;
    }

    return model.height;
}

void DiagramEngine::render(
    ICanvas& canvas,
    const DiagramModel& model,
    float x,
    float y,
    float max_width,
    const MarkdownStyle& style,
    text::FontSystem& font_system,
    text::GlyphCache& glyph_cache
) {
    // 1. Diagram Container Card
    if (auto card = Rect::from_xywh(x, y, max_width, model.height)) {
        canvas.fill_rrect(RRect::from_rect_and_radius(*card, 8.0f, 8.0f), Paint(style.code_block_bg));
        canvas.stroke_rrect(RRect::from_rect_and_radius(*card, 8.0f, 8.0f), Paint(style.code_block_border), Stroke(1.0f));
    }

    // Optional Title
    if (!model.title.empty()) {
        draw_label(
            canvas, model.title, x + 16.0f, y + 10.0f, max_width - 32.0f,
            15.0f, true, text::Align::Center, style.heading_color, font_system, glyph_cache
        );
    }

    // 2. Sequence Diagram Rendering
    if (model.kind == DiagramKind::Sequence) {
        size_t n_actors = model.actors.size();
        if (n_actors == 0) return;

        float lifelines_top = y + model.actors[0].y + model.actors[0].height;
        float lifelines_bot = y + model.height - 46.0f;

        // Draw vertical dashed lifelines
        Stroke lifeline_s(1.2f);
        lifeline_s.dash = StrokeDash::create({5.0f, 4.0f}, 0.0f);
        Paint lifeline_p(style.table_border_color);

        for (const auto& actor : model.actors) {
            float cx = x + actor.x + actor.width * 0.5f;
            PathBuilder pb;
            pb.move_to(cx, lifelines_top);
            pb.line_to(cx, lifelines_bot);
            canvas.stroke_path(fin(pb), lifeline_p, lifeline_s);
        }

        // Draw top & bottom actor boxes
        for (const auto& actor : model.actors) {
            for (float box_y : {y + actor.y, lifelines_bot}) {
                if (auto r = Rect::from_xywh(x + actor.x, box_y, actor.width, actor.height)) {
                    canvas.fill_rrect(RRect::from_rect_and_radius(*r, 6.0f, 6.0f), Paint(style.table_header_bg));
                    canvas.stroke_rrect(RRect::from_rect_and_radius(*r, 6.0f, 6.0f), Paint(style.quote_bar_color), Stroke(1.4f));
                    draw_label(
                        canvas, actor.label, x + actor.x + 4.0f, box_y + 9.0f, actor.width - 8.0f,
                        13.0f, true, text::Align::Center, style.heading_color, font_system, glyph_cache
                    );
                }
            }
        }

        // Draw sequence messages
        for (const auto& msg : model.messages) {
            if (msg.from_actor >= n_actors || msg.to_actor >= n_actors) continue;
            float x1 = x + model.actors[msg.from_actor].x + model.actors[msg.from_actor].width * 0.5f;
            float x2 = x + model.actors[msg.to_actor].x + model.actors[msg.to_actor].width * 0.5f;
            float my = y + msg.y;

            if (msg.is_note) {
                float nw = 150.0f;
                if (auto nr = Rect::from_xywh(x1 - nw * 0.5f, my - 12.0f, nw, 28.0f)) {
                    canvas.fill_rrect(RRect::from_rect_and_radius(*nr, 4.0f, 4.0f), Paint(style.alert_warning.bg_color));
                    canvas.stroke_rrect(RRect::from_rect_and_radius(*nr, 4.0f, 4.0f), Paint(style.alert_warning.bar_color), Stroke(1.0f));
                    draw_label(
                        canvas, msg.text, nr->left() + 6.0f, nr->top() + 6.0f, nw - 12.0f,
                        12.0f, false, text::Align::Center, style.text_color, font_system, glyph_cache
                    );
                }
                continue;
            }

            Stroke ms(1.5f);
            if (msg.style == DiagramEdgeStyle::DottedArrow) {
                ms.dash = StrokeDash::create({5.0f, 4.0f}, 0.0f);
            }
            Paint mp(style.link_color);

            if (msg.from_actor == msg.to_actor) {
                // Self-call loop
                PathBuilder pb;
                pb.move_to(x1, my);
                pb.line_to(x1 + 36.0f, my);
                pb.line_to(x1 + 36.0f, my + 20.0f);
                pb.line_to(x1, my + 20.0f);
                canvas.stroke_path(fin(pb), mp, ms);
                draw_arrow_head(canvas, x1, my + 20.0f, -1.0f, 0.0f, 8.0f, style.link_color);
                draw_label(
                    canvas, msg.text, x1 + 42.0f, my + 2.0f, 180.0f,
                    12.0f, false, text::Align::Left, style.text_color, font_system, glyph_cache
                );
            } else {
                PathBuilder pb;
                pb.move_to(x1, my);
                pb.line_to(x2, my);
                canvas.stroke_path(fin(pb), mp, ms);
                draw_arrow_head(canvas, x2, my, (x2 > x1) ? 1.0f : -1.0f, 0.0f, 8.5f, style.link_color);

                float left_x = std::min(x1, x2);
                float span_w = std::abs(x2 - x1);
                draw_label(
                    canvas, msg.text, left_x + 6.0f, my - 18.0f, span_w - 12.0f,
                    12.0f, false, text::Align::Center, style.text_color, font_system, glyph_cache
                );
            }
        }
        return;
    }

    // 3. XYChart Rendering
    if (model.kind == DiagramKind::XYChart) {
        float title_off = model.title.empty() ? 16.0f : 42.0f;
        float plot_x = x + 56.0f;
        float plot_y = y + title_off;
        float plot_w = std::max(120.0f, max_width - 84.0f);
        float plot_h = model.height - title_off - 42.0f;

        // Gridlines & Y-axis labels
        int ticks = 4;
        Stroke grid_s(1.0f);
        grid_s.dash = StrokeDash::create({3.0f, 3.0f}, 0.0f);
        for (int t = 0; t <= ticks; ++t) {
            float ratio = static_cast<float>(t) / static_cast<float>(ticks);
            float gy = plot_y + plot_h - ratio * plot_h;
            float val = model.y_min + ratio * (model.y_max - model.y_min);

            PathBuilder pb;
            pb.move_to(plot_x, gy);
            pb.line_to(plot_x + plot_w, gy);
            canvas.stroke_path(fin(pb), Paint(style.divider_color), (t == 0) ? Stroke(1.5f) : grid_s);

            std::string lbl = std::to_string(static_cast<int>(std::round(val)));
            draw_label(
                canvas, lbl, x + 6.0f, gy - 7.0f, 44.0f,
                11.0f, false, text::Align::Right, style.footnote_text_color, font_system, glyph_cache
            );
        }

        size_t n_cats = model.x_categories.size();
        for (const auto& s : model.series) n_cats = std::max(n_cats, s.values.size());
        if (n_cats == 0) return;

        float cat_w = plot_w / static_cast<float>(n_cats);
        for (size_t c = 0; c < model.x_categories.size(); ++c) {
            float cx = plot_x + static_cast<float>(c) * cat_w;
            draw_label(
                canvas, model.x_categories[c], cx, plot_y + plot_h + 8.0f, cat_w,
                11.5f, false, text::Align::Center, style.text_color, font_system, glyph_cache
            );
        }

        float y_span = std::max(1e-3f, model.y_max - model.y_min);
        static const Color palette[] = {
            Color::from_rgba8(31, 136, 255, 220),
            Color::from_rgba8(46, 204, 113, 220),
            Color::from_rgba8(163, 113, 247, 220),
            Color::from_rgba8(255, 159, 67, 220)
        };

        // Render Bar series
        size_t bar_count = 0;
        for (const auto& s : model.series) if (s.is_bar) bar_count++;
        size_t bar_idx = 0;

        for (size_t si = 0; si < model.series.size(); ++si) {
            const auto& s = model.series[si];
            Color col = palette[si % 4];
            if (s.is_bar) {
                float total_bar_group_w = cat_w * 0.62f;
                float single_bar_w = total_bar_group_w / static_cast<float>(std::max<size_t>(1, bar_count));
                for (size_t i = 0; i < s.values.size(); ++i) {
                    float norm = std::clamp((s.values[i] - model.y_min) / y_span, 0.0f, 1.0f);
                    float bh = norm * plot_h;
                    float bx = plot_x + (static_cast<float>(i) + 0.5f) * cat_w - total_bar_group_w * 0.5f + static_cast<float>(bar_idx) * single_bar_w;
                    float by = plot_y + plot_h - bh;
                    if (auto r = Rect::from_xywh(bx, by, std::max(4.0f, single_bar_w - 2.0f), bh)) {
                        canvas.fill_rrect(RRect::from_rect_and_radius(*r, 4.0f, 4.0f), Paint(col));
                    }
                }
                bar_idx++;
            }
        }

        // Render Line series
        for (size_t si = 0; si < model.series.size(); ++si) {
            const auto& s = model.series[si];
            if (s.is_bar || s.values.empty()) continue;
            Color col = palette[(si + 1) % 4];
            PathBuilder pb;
            for (size_t i = 0; i < s.values.size(); ++i) {
                float norm = std::clamp((s.values[i] - model.y_min) / y_span, 0.0f, 1.0f);
                float px = plot_x + (static_cast<float>(i) + 0.5f) * cat_w;
                float py = plot_y + plot_h - norm * plot_h;
                if (i == 0) pb.move_to(px, py);
                else pb.line_to(px, py);
            }
            Stroke ls(2.4f);
            ls.line_cap = LineCap::Round;
            ls.line_join = LineJoin::Round;
            canvas.stroke_path(fin(pb), Paint(col), ls);

            for (size_t i = 0; i < s.values.size(); ++i) {
                float norm = std::clamp((s.values[i] - model.y_min) / y_span, 0.0f, 1.0f);
                float px = plot_x + (static_cast<float>(i) + 0.5f) * cat_w;
                float py = plot_y + plot_h - norm * plot_h;
                if (auto mr = Rect::from_xywh(px - 3.5f, py - 3.5f, 7.0f, 7.0f)) {
                    canvas.fill_rrect(RRect::from_rect_and_radius(*mr, 3.5f, 3.5f), Paint(style.heading_color));
                }
            }
        }
        return;
    }

    // 4. PieChart Rendering
    if (model.kind == DiagramKind::PieChart) {
        float total = 0.0f;
        for (const auto& sl : model.pie_slices) total += sl.value;
        if (total <= 0.0f) return;

        float title_off = model.title.empty() ? 16.0f : 42.0f;
        float r = 74.0f;
        float cx = x + max_width * 0.34f;
        float cy = y + title_off + r + 14.0f;

        static const Color palette[] = {
            Color::from_rgba8(31, 111, 235, 255),
            Color::from_rgba8(46, 160, 67, 255),
            Color::from_rgba8(137, 87, 229, 255),
            Color::from_rgba8(210, 153, 34, 255),
            Color::from_rgba8(218, 54, 51, 255),
            Color::from_rgba8(0, 190, 210, 255)
        };

        float cur_angle = -static_cast<float>(M_PI) * 0.5f;
        for (size_t i = 0; i < model.pie_slices.size(); ++i) {
            float frac = model.pie_slices[i].value / total;
            float sweep = frac * 2.0f * static_cast<float>(M_PI);
            int steps = std::max(6, static_cast<int>(sweep * 18.0f));

            PathBuilder pb;
            pb.move_to(cx, cy);
            for (int s = 0; s <= steps; ++s) {
                float a = cur_angle + sweep * (static_cast<float>(s) / static_cast<float>(steps));
                pb.line_to(cx + r * std::cos(a), cy + r * std::sin(a));
            }
            pb.close();

            Color col = palette[i % 6];
            canvas.fill_path(fin(pb), Paint(col));
            canvas.stroke_path(fin(pb), Paint(style.code_block_bg), Stroke(1.5f));

            // Legend entry
            float leg_x = x + max_width * 0.58f;
            float leg_y = y + title_off + 16.0f + static_cast<float>(i) * 24.0f;
            if (auto box = Rect::from_xywh(leg_x, leg_y, 12.0f, 12.0f)) {
                canvas.fill_rrect(RRect::from_rect_and_radius(*box, 3.0f, 3.0f), Paint(col));
            }
            int pct = static_cast<int>(std::round(frac * 100.0f));
            std::string leg_lbl = model.pie_slices[i].label + " (" + std::to_string(pct) + "%)";
            draw_label(
                canvas, leg_lbl, leg_x + 18.0f, leg_y - 2.0f, max_width * 0.38f,
                12.5f, false, text::Align::Left, style.text_color, font_system, glyph_cache
            );

            cur_angle += sweep;
        }
        return;
    }

    // 5. Flowchart & State Diagram Rendering
    bool is_horiz = (model.direction == DiagramDirection::LeftRight || model.direction == DiagramDirection::RightLeft);

    // 5a. Render Edges first (underneath nodes)
    for (const auto& edge : model.edges) {
        if (edge.from_idx >= model.nodes.size() || edge.to_idx >= model.nodes.size()) continue;
        const auto& u = model.nodes[edge.from_idx];
        const auto& v = model.nodes[edge.to_idx];

        float x1 = x + u.x + u.width * 0.5f;
        float y1 = y + u.y + u.height * 0.5f;
        float x2 = x + v.x + v.width * 0.5f;
        float y2 = y + v.y + v.height * 0.5f;

        float tip_dx = 0.0f;
        float tip_dy = 1.0f;

        if (!is_horiz) {
            if (v.y >= u.y) {
                y1 = y + u.y + u.height;
                y2 = y + v.y;
                tip_dx = 0.0f;
                tip_dy = 1.0f;
            } else {
                y1 = y + u.y;
                y2 = y + v.y + v.height;
                tip_dx = 0.0f;
                tip_dy = -1.0f;
            }
        } else {
            if (v.x >= u.x) {
                x1 = x + u.x + u.width;
                x2 = x + v.x;
                tip_dx = 1.0f;
                tip_dy = 0.0f;
            } else {
                x1 = x + u.x;
                x2 = x + v.x + v.width;
                tip_dx = -1.0f;
                tip_dy = 0.0f;
            }
        }

        PathBuilder pb;
        pb.move_to(x1, y1);
        if (!is_horiz) {
            float mid_y = (y1 + y2) * 0.5f;
            pb.cubic_to(x1, mid_y, x2, mid_y, x2, y2);
        } else {
            float mid_x = (x1 + x2) * 0.5f;
            pb.cubic_to(mid_x, y1, mid_x, y2, x2, y2);
        }

        Stroke es(edge.style == DiagramEdgeStyle::ThickArrow ? 2.6f : 1.6f);
        if (edge.style == DiagramEdgeStyle::DottedArrow) {
            es.dash = StrokeDash::create({5.0f, 4.0f}, 0.0f);
        }
        Color edge_col = style.link_color;
        canvas.stroke_path(fin(pb), Paint(edge_col), es);

        if (edge.style != DiagramEdgeStyle::OpenSolid) {
            draw_arrow_head(canvas, x2, y2, tip_dx, tip_dy, 8.5f, edge_col);
        }
        if (edge.style == DiagramEdgeStyle::BiDirectional) {
            draw_arrow_head(canvas, x1, y1, -tip_dx, -tip_dy, 8.5f, edge_col);
        }

        if (!edge.label.empty()) {
            float mx = (x1 + x2) * 0.5f;
            float my = (y1 + y2) * 0.5f;
            float lw = measure_label(edge.label, 11.5f, false, font_system) + 12.0f;
            if (auto br = Rect::from_xywh(mx - lw * 0.5f, my - 10.0f, lw, 20.0f)) {
                canvas.fill_rrect(RRect::from_rect_and_radius(*br, 4.0f, 4.0f), Paint(style.code_block_bg));
                canvas.stroke_rrect(RRect::from_rect_and_radius(*br, 4.0f, 4.0f), Paint(style.table_border_color), Stroke(1.0f));
                draw_label(
                    canvas, edge.label, br->left(), br->top() + 3.0f, lw,
                    11.5f, false, text::Align::Center, style.text_color, font_system, glyph_cache
                );
            }
        }
    }

    // 5b. Render Nodes
    for (const auto& node : model.nodes) {
        float nx = x + node.x;
        float ny = y + node.y;
        float nw = node.width;
        float nh = node.height;

        if (node.shape == DiagramNodeShape::StartState) {
            if (auto r = Rect::from_xywh(nx, ny, nw, nh)) {
                canvas.fill_rrect(RRect::from_rect_and_radius(*r, nw * 0.5f, nh * 0.5f), Paint(style.quote_bar_color));
            }
            continue;
        }
        if (node.shape == DiagramNodeShape::EndState) {
            if (auto r = Rect::from_xywh(nx, ny, nw, nh)) {
                canvas.stroke_rrect(RRect::from_rect_and_radius(*r, nw * 0.5f, nh * 0.5f), Paint(style.quote_bar_color), Stroke(2.0f));
            }
            if (auto ir = Rect::from_xywh(nx + 5.0f, ny + 5.0f, nw - 10.0f, nh - 10.0f)) {
                canvas.fill_rrect(RRect::from_rect_and_radius(*ir, (nw - 10.0f) * 0.5f, (nh - 10.0f) * 0.5f), Paint(style.quote_bar_color));
            }
            continue;
        }

        if (node.shape == DiagramNodeShape::Diamond) {
            PathBuilder pb;
            pb.move_to(nx + nw * 0.5f, ny);
            pb.line_to(nx + nw, ny + nh * 0.5f);
            pb.line_to(nx + nw * 0.5f, ny + nh);
            pb.line_to(nx, ny + nh * 0.5f);
            pb.close();
            Path dp = fin(pb);
            canvas.fill_path(dp, Paint(style.alert_important.bg_color));
            canvas.stroke_path(dp, Paint(style.alert_important.bar_color), Stroke(1.6f));
        } else if (node.shape == DiagramNodeShape::Hexagon) {
            float cut = 12.0f;
            PathBuilder pb;
            pb.move_to(nx + cut, ny);
            pb.line_to(nx + nw - cut, ny);
            pb.line_to(nx + nw, ny + nh * 0.5f);
            pb.line_to(nx + nw - cut, ny + nh);
            pb.line_to(nx + cut, ny + nh);
            pb.line_to(nx, ny + nh * 0.5f);
            pb.close();
            Path hp = fin(pb);
            canvas.fill_path(hp, Paint(style.table_header_bg));
            canvas.stroke_path(hp, Paint(style.quote_bar_color), Stroke(1.5f));
        } else {
            float radius = 6.0f;
            if (node.shape == DiagramNodeShape::Rect || node.shape == DiagramNodeShape::Subroutine) radius = 2.0f;
            else if (node.shape == DiagramNodeShape::Stadium) radius = nh * 0.5f;
            else if (node.shape == DiagramNodeShape::Circle) radius = std::min(nw, nh) * 0.5f;

            if (auto r = Rect::from_xywh(nx, ny, nw, nh)) {
                canvas.fill_rrect(RRect::from_rect_and_radius(*r, radius, radius), Paint(style.table_header_bg));
                canvas.stroke_rrect(RRect::from_rect_and_radius(*r, radius, radius), Paint(style.quote_bar_color), Stroke(1.5f));

                if (node.shape == DiagramNodeShape::Subroutine) {
                    if (auto ir = Rect::from_xywh(nx + 6.0f, ny, nw - 12.0f, nh)) {
                        canvas.stroke_rrect(RRect::from_rect_and_radius(*ir, 1.0f, 1.0f), Paint(style.quote_bar_color), Stroke(1.2f));
                    }
                }
            }
        }

        draw_label(
            canvas, node.label, nx + 6.0f, ny + (nh - 16.0f) * 0.5f, nw - 12.0f,
            13.0f, true, text::Align::Center, style.heading_color, font_system, glyph_cache
        );
    }
}

} // namespace nisaba::markdown
