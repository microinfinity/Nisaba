#include "nisaba/text/text_blob.hpp"
#include "nisaba/canvas/canvas_interface.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nisaba {

namespace {

std::atomic<uint32_t> g_next_text_blob_id{1};

bool decode_utf8_next(std::string_view text, size_t& i, char32_t& out_cp) noexcept {
    if (i >= text.size()) return false;
    const uint8_t c0 = static_cast<uint8_t>(text[i]);
    if (c0 < 0x80u) {
        out_cp = static_cast<char32_t>(c0);
        ++i;
        return true;
    }
    if ((c0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
        const uint8_t c1 = static_cast<uint8_t>(text[i + 1]);
        if ((c1 & 0xC0u) == 0x80u) {
            out_cp = static_cast<char32_t>(((c0 & 0x1Fu) << 6) | (c1 & 0x3Fu));
            i += 2;
            return true;
        }
    } else if ((c0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
        const uint8_t c1 = static_cast<uint8_t>(text[i + 1]);
        const uint8_t c2 = static_cast<uint8_t>(text[i + 2]);
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u) {
            out_cp = static_cast<char32_t>(((c0 & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu));
            i += 3;
            return true;
        }
    } else if ((c0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
        const uint8_t c1 = static_cast<uint8_t>(text[i + 1]);
        const uint8_t c2 = static_cast<uint8_t>(text[i + 2]);
        const uint8_t c3 = static_cast<uint8_t>(text[i + 3]);
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u) {
            out_cp = static_cast<char32_t>(((c0 & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu));
            i += 4;
            return true;
        }
    }
    out_cp = 0xFFFDu;
    ++i;
    return true;
}

Rect compute_pos_run_bounds(
    const text::TtfFont& font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions
) noexcept {
    const size_t n = std::min(glyph_ids.size(), positions.size());
    if (n == 0 || font_size <= 0.0f) return Rect();

    const float ascent = std::max(0.0f, font.ascent(font_size));
    const float descent = std::max(0.0f, -font.descent(font_size));
    const float pad_x = std::max(1.0f, font_size * 0.25f);
    const float pad_y = std::max(1.0f, font_size * 0.15f);

    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();

    for (size_t i = 0; i < n; ++i) {
        const float adv = std::max(font_size * 0.5f, font.glyph_advance(glyph_ids[i], font_size));
        const Point& p = positions[i];
        min_x = std::min(min_x, p.x - pad_x);
        max_x = std::max(max_x, p.x + adv + pad_x);
        min_y = std::min(min_y, p.y - ascent - pad_y);
        max_y = std::max(max_y, p.y + descent + pad_y);
    }

    if (!std::isfinite(min_x) || !std::isfinite(max_x) || max_x <= min_x || max_y <= min_y) {
        return Rect();
    }
    return Rect::from_ltrb(min_x, min_y, max_x, max_y).value_or(Rect());
}

Rect compute_rsxform_run_bounds(
    const text::TtfFont& font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms
) noexcept {
    const size_t n = std::min(glyph_ids.size(), xforms.size());
    if (n == 0 || font_size <= 0.0f) return Rect();

    const float ascent = std::max(0.0f, font.ascent(font_size));
    const float descent = std::max(0.0f, -font.descent(font_size));
    const float pad_x = std::max(1.0f, font_size * 0.25f);
    const float pad_y = std::max(1.0f, font_size * 0.15f);

    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();

    for (size_t i = 0; i < n; ++i) {
        const float adv = std::max(font_size * 0.5f, font.glyph_advance(glyph_ids[i], font_size));
        const Transform t = xforms[i].to_transform();
        Point corners[4] = {
            Point(-pad_x, -ascent - pad_y),
            Point(adv + pad_x, -ascent - pad_y),
            Point(adv + pad_x, descent + pad_y),
            Point(-pad_x, descent + pad_y)
        };
        for (Point& c : corners) {
            t.map_point(c);
            min_x = std::min(min_x, c.x);
            min_y = std::min(min_y, c.y);
            max_x = std::max(max_x, c.x);
            max_y = std::max(max_y, c.y);
        }
    }

    if (!std::isfinite(min_x) || !std::isfinite(max_x) || max_x <= min_x || max_y <= min_y) {
        return Rect();
    }
    return Rect::from_ltrb(min_x, min_y, max_x, max_y).value_or(Rect());
}

void draw_glyph_path_with_paint(ICanvas& canvas, const Path& path, const Paint& paint, const Stroke& stroke) {
    if (path.is_empty()) return;
    if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
        canvas.fill_path(path, paint, FillRule::Winding);
    }
    if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
        canvas.stroke_path(path, paint, stroke);
    }
}

void shape_utf8_to_pos_run(
    std::string_view utf8_text,
    const text::TtfFont& font,
    float font_size,
    float origin_x,
    float origin_y,
    std::vector<uint16_t>& out_glyphs,
    std::vector<Point>& out_positions
) {
    out_glyphs.clear();
    out_positions.clear();
    if (utf8_text.empty() || !font.is_valid() || font_size <= 0.0f) return;

    const float scale = font.scale_for_size(font_size);
    float pen_x = origin_x;
    uint16_t prev_gid = 0;

    size_t i = 0;
    char32_t cp = 0;
    while (decode_utf8_next(utf8_text, i, cp)) {
        const uint16_t gid = font.glyph_index(cp);
        if (prev_gid != 0 && gid != 0) {
            pen_x += static_cast<float>(font.get_kerning(prev_gid, gid)) * scale;
        }
        out_glyphs.push_back(gid);
        out_positions.emplace_back(pen_x, origin_y);
        pen_x += font.glyph_advance_scaled(gid, scale);
        prev_gid = gid;
    }
}

} // namespace

uint32_t TextBlob::next_unique_id() noexcept {
    uint32_t id = g_next_text_blob_id.fetch_add(1u, std::memory_order_relaxed);
    if (id == 0u) {
        id = g_next_text_blob_id.fetch_add(1u, std::memory_order_relaxed);
    }
    return id;
}

TextBlob::TextBlob(std::vector<Run> runs, Rect bounds) noexcept
    : runs_(std::move(runs)), bounds_(bounds), unique_id_(next_unique_id()) {}

size_t TextBlob::glyph_count() const noexcept {
    size_t total = 0;
    for (const auto& r : runs_) {
        total += r.glyph_ids.size();
    }
    return total;
}

std::shared_ptr<TextBlob> TextBlob::from_text(
    std::string_view utf8_text,
    const text::TtfFont& font,
    float font_size,
    float origin_x,
    float origin_y
) {
    if (utf8_text.empty() || !font.is_valid() || font_size <= 0.0f) {
        return std::make_shared<TextBlob>();
    }
    std::vector<uint16_t> glyphs;
    std::vector<Point> positions;
    shape_utf8_to_pos_run(utf8_text, font, font_size, origin_x, origin_y, glyphs, positions);
    return from_pos_glyphs(glyphs, positions, font, font_size);
}

std::shared_ptr<TextBlob> TextBlob::from_text(
    std::string_view utf8_text,
    std::shared_ptr<const text::TtfFont> font,
    float font_size,
    float origin_x,
    float origin_y
) {
    if (!font || utf8_text.empty() || !font->is_valid() || font_size <= 0.0f) {
        return std::make_shared<TextBlob>();
    }
    std::vector<uint16_t> glyphs;
    std::vector<Point> positions;
    shape_utf8_to_pos_run(utf8_text, *font, font_size, origin_x, origin_y, glyphs, positions);
    return from_pos_glyphs(glyphs, positions, std::move(font), font_size);
}

std::shared_ptr<TextBlob> TextBlob::from_pos_glyphs(
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions,
    const text::TtfFont& font,
    float font_size
) {
    TextBlobBuilder builder;
    builder.add_run_pos(font, font_size, glyph_ids, positions);
    return builder.make();
}

std::shared_ptr<TextBlob> TextBlob::from_pos_glyphs(
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions,
    std::shared_ptr<const text::TtfFont> font,
    float font_size
) {
    TextBlobBuilder builder;
    builder.add_run_pos(std::move(font), font_size, glyph_ids, positions);
    return builder.make();
}

std::shared_ptr<TextBlob> TextBlob::from_rsxform_glyphs(
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms,
    const text::TtfFont& font,
    float font_size
) {
    TextBlobBuilder builder;
    builder.add_run_rsxform(font, font_size, glyph_ids, xforms);
    return builder.make();
}

std::shared_ptr<TextBlob> TextBlob::from_rsxform_glyphs(
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms,
    std::shared_ptr<const text::TtfFont> font,
    float font_size
) {
    TextBlobBuilder builder;
    builder.add_run_rsxform(std::move(font), font_size, glyph_ids, xforms);
    return builder.make();
}

void TextBlobBuilder::add_run_pos_impl(
    std::shared_ptr<const text::TtfFont> owned_font,
    const text::TtfFont* font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions,
    std::optional<Rect> custom_bounds
) {
    const size_t n = std::min(glyph_ids.size(), positions.size());
    if (!font || !font->is_valid() || font_size <= 0.0f || n == 0) return;

    TextBlob::Run run;
    run.owned_font = std::move(owned_font);
    run.font = font;
    run.font_size = font_size;
    run.positioning = TextBlob::RunPositioning::Positioned;
    run.glyph_ids.assign(glyph_ids.begin(), glyph_ids.begin() + static_cast<std::ptrdiff_t>(n));
    run.positions.assign(positions.begin(), positions.begin() + static_cast<std::ptrdiff_t>(n));
    run.bounds = custom_bounds.value_or(compute_pos_run_bounds(*font, font_size, run.glyph_ids, run.positions));
    runs_.push_back(std::move(run));
}

void TextBlobBuilder::add_run_rsxform_impl(
    std::shared_ptr<const text::TtfFont> owned_font,
    const text::TtfFont* font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms,
    std::optional<Rect> custom_bounds
) {
    const size_t n = std::min(glyph_ids.size(), xforms.size());
    if (!font || !font->is_valid() || font_size <= 0.0f || n == 0) return;

    TextBlob::Run run;
    run.owned_font = std::move(owned_font);
    run.font = font;
    run.font_size = font_size;
    run.positioning = TextBlob::RunPositioning::RSXform;
    run.glyph_ids.assign(glyph_ids.begin(), glyph_ids.begin() + static_cast<std::ptrdiff_t>(n));
    run.xforms.assign(xforms.begin(), xforms.begin() + static_cast<std::ptrdiff_t>(n));
    run.bounds = custom_bounds.value_or(compute_rsxform_run_bounds(*font, font_size, run.glyph_ids, run.xforms));
    runs_.push_back(std::move(run));
}

void TextBlobBuilder::add_run_pos(
    const text::TtfFont& font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions,
    std::optional<Rect> custom_bounds
) {
    add_run_pos_impl(nullptr, &font, font_size, glyph_ids, positions, custom_bounds);
}

void TextBlobBuilder::add_run_pos(
    std::shared_ptr<const text::TtfFont> font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions,
    std::optional<Rect> custom_bounds
) {
    const text::TtfFont* ptr = font.get();
    add_run_pos_impl(std::move(font), ptr, font_size, glyph_ids, positions, custom_bounds);
}

void TextBlobBuilder::add_run_pos_h(
    const text::TtfFont& font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const float> x_positions,
    float const_y,
    std::optional<Rect> custom_bounds
) {
    const size_t n = std::min(glyph_ids.size(), x_positions.size());
    if (n == 0) return;
    std::vector<Point> pts;
    pts.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        pts.emplace_back(x_positions[i], const_y);
    }
    add_run_pos_impl(nullptr, &font, font_size, glyph_ids.subspan(0, n), pts, custom_bounds);
}

void TextBlobBuilder::add_run_pos_h(
    std::shared_ptr<const text::TtfFont> font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const float> x_positions,
    float const_y,
    std::optional<Rect> custom_bounds
) {
    const size_t n = std::min(glyph_ids.size(), x_positions.size());
    if (!font || n == 0) return;
    std::vector<Point> pts;
    pts.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        pts.emplace_back(x_positions[i], const_y);
    }
    const text::TtfFont* ptr = font.get();
    add_run_pos_impl(std::move(font), ptr, font_size, glyph_ids.subspan(0, n), pts, custom_bounds);
}

void TextBlobBuilder::add_run_rsxform(
    const text::TtfFont& font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms,
    std::optional<Rect> custom_bounds
) {
    add_run_rsxform_impl(nullptr, &font, font_size, glyph_ids, xforms, custom_bounds);
}

void TextBlobBuilder::add_run_rsxform(
    std::shared_ptr<const text::TtfFont> font,
    float font_size,
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms,
    std::optional<Rect> custom_bounds
) {
    const text::TtfFont* ptr = font.get();
    add_run_rsxform_impl(std::move(font), ptr, font_size, glyph_ids, xforms, custom_bounds);
}

std::shared_ptr<TextBlob> TextBlobBuilder::make() {
    if (runs_.empty()) {
        return std::make_shared<TextBlob>();
    }
    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();
    for (const auto& r : runs_) {
        if (!r.bounds.is_empty()) {
            min_x = std::min(min_x, r.bounds.left());
            min_y = std::min(min_y, r.bounds.top());
            max_x = std::max(max_x, r.bounds.right());
            max_y = std::max(max_y, r.bounds.bottom());
        }
    }
    Rect total_bounds;
    if (std::isfinite(min_x) && std::isfinite(max_x) && max_x > min_x && max_y > min_y) {
        total_bounds = Rect::from_ltrb(min_x, min_y, max_x, max_y).value_or(Rect());
    }
    auto blob = std::make_shared<TextBlob>(std::move(runs_), total_bounds);
    runs_.clear();
    return blob;
}

// =========================================================================
// ICanvas default positioned / RSXform glyph & TextBlob implementations
// =========================================================================

void ICanvas::draw_glyphs(
    std::span<const uint16_t> glyph_ids,
    std::span<const Point> positions,
    const text::TtfFont& font,
    float font_size,
    const Paint& paint
) {
    const size_t n = std::min(glyph_ids.size(), positions.size());
    if (n == 0 || !font.is_valid() || font_size <= 0.0f) return;

    const bool isolate_filter = paint.has_layer_filter();
    Paint draw_p = paint;
    if (isolate_filter) {
        Paint layer_p(Color::WHITE);
        layer_p.blend_mode = paint.blend_mode;
        layer_p.image_filter = paint.image_filter;
        layer_p.mask_filter = paint.mask_filter;
        save_layer(layer_p);
        draw_p.blend_mode = BlendMode::SourceOver;
        draw_p.clear_image_filter();
        draw_p.clear_mask_filter();
    }

    const Stroke stroke(draw_p.get_stroke_width());
    std::vector<text::ColorGlyphLayer> color_layers;

    for (size_t i = 0; i < n; ++i) {
        const uint16_t gid = glyph_ids[i];
        if (gid == 0) continue;
        const Transform t = Transform::from_translate(positions[i].x, positions[i].y);

        if (font.has_color_glyphs() && font.get_color_glyph_layers(gid, color_layers)) {
            for (const auto& layer : color_layers) {
                Path lpath;
                if (!font.get_glyph_path(layer.glyph_id, lpath, font_size) || lpath.is_empty()) continue;
                lpath.apply_transform(t);
                if (layer.use_foreground) {
                    draw_glyph_path_with_paint(*this, lpath, draw_p, stroke);
                } else {
                    Paint lp(layer.color.to_color());
                    lp.blend_mode = draw_p.blend_mode;
                    fill_path(lpath, lp, FillRule::Winding);
                }
            }
            continue;
        }

        Path gpath;
        if (!font.get_glyph_path(gid, gpath, font_size) || gpath.is_empty()) continue;
        gpath.apply_transform(t);
        draw_glyph_path_with_paint(*this, gpath, draw_p, stroke);
    }

    if (isolate_filter) {
        restore();
    }
}

void ICanvas::draw_glyphs(
    std::span<const uint16_t> glyph_ids,
    std::span<const RSXform> xforms,
    const text::TtfFont& font,
    float font_size,
    const Paint& paint
) {
    const size_t n = std::min(glyph_ids.size(), xforms.size());
    if (n == 0 || !font.is_valid() || font_size <= 0.0f) return;

    const bool isolate_filter = paint.has_layer_filter();
    Paint draw_p = paint;
    if (isolate_filter) {
        Paint layer_p(Color::WHITE);
        layer_p.blend_mode = paint.blend_mode;
        layer_p.image_filter = paint.image_filter;
        layer_p.mask_filter = paint.mask_filter;
        save_layer(layer_p);
        draw_p.blend_mode = BlendMode::SourceOver;
        draw_p.clear_image_filter();
        draw_p.clear_mask_filter();
    }

    const Stroke stroke(draw_p.get_stroke_width());
    std::vector<text::ColorGlyphLayer> color_layers;

    for (size_t i = 0; i < n; ++i) {
        const uint16_t gid = glyph_ids[i];
        if (gid == 0) continue;
        const Transform t = xforms[i].to_transform();
        if (!t.is_valid()) continue;

        if (font.has_color_glyphs() && font.get_color_glyph_layers(gid, color_layers)) {
            for (const auto& layer : color_layers) {
                Path lpath;
                if (!font.get_glyph_path(layer.glyph_id, lpath, font_size) || lpath.is_empty()) continue;
                lpath.apply_transform(t);
                if (layer.use_foreground) {
                    draw_glyph_path_with_paint(*this, lpath, draw_p, stroke);
                } else {
                    Paint lp(layer.color.to_color());
                    lp.blend_mode = draw_p.blend_mode;
                    fill_path(lpath, lp, FillRule::Winding);
                }
            }
            continue;
        }

        Path gpath;
        if (!font.get_glyph_path(gid, gpath, font_size) || gpath.is_empty()) continue;
        gpath.apply_transform(t);
        draw_glyph_path_with_paint(*this, gpath, draw_p, stroke);
    }

    if (isolate_filter) {
        restore();
    }
}

void ICanvas::draw_text_blob(
    const TextBlob& blob,
    float x,
    float y,
    const Paint& paint
) {
    if (blob.is_empty()) return;

    if (!paint.has_layer_filter() && !blob.bounds().is_empty()) {
        const Rect& b = blob.bounds();
        if (auto moved = Rect::from_ltrb(b.left() + x, b.top() + y, b.right() + x, b.bottom() + y)) {
            if (quick_reject(*moved)) return;
        }
    }

    const bool has_offset = (x != 0.0f || y != 0.0f);
    const bool isolate_filter = paint.has_layer_filter();

    Paint run_paint = paint;
    if (isolate_filter) {
        Paint layer_p(Color::WHITE);
        layer_p.blend_mode = paint.blend_mode;
        layer_p.image_filter = paint.image_filter;
        layer_p.mask_filter = paint.mask_filter;
        save_layer(layer_p);
        run_paint.blend_mode = BlendMode::SourceOver;
        run_paint.clear_image_filter();
        run_paint.clear_mask_filter();
    } else if (has_offset) {
        save();
    }

    if (has_offset) {
        translate(x, y);
    }

    for (const auto& run : blob.runs()) {
        if (!run.font || run.glyph_ids.empty()) continue;
        if (run.positioning == TextBlob::RunPositioning::Positioned) {
            draw_glyphs(run.glyph_ids, run.positions, *run.font, run.font_size, run_paint);
        } else {
            draw_glyphs(run.glyph_ids, run.xforms, *run.font, run.font_size, run_paint);
        }
    }

    if (isolate_filter || has_offset) {
        restore();
    }
}

} // namespace nisaba

