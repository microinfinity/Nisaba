#include "nisaba/pdf/pdf_canvas.hpp"
#include "nisaba/path/path_builder.hpp"
#include <cstdio>
#include <cmath>

namespace nisaba::pdf {

PdfCanvas::PdfCanvas(float page_width, float page_height)
    : width_(page_width),
      height_(page_height),
      clip_bounds_(ScreenIntRect::from_xywh(
          0, 0,
          static_cast<uint32_t>(std::max(1.0f, std::ceil(page_width))),
          static_cast<uint32_t>(std::max(1.0f, std::ceil(page_height))))) {
    // Establish Nisaba top-down coordinate space:
    // CTM = [1 0 0 -1 0 page_height]
    char buf[64];
    snprintf(buf, sizeof(buf), "1 0 0 -1 0 %.2f cm\n", height_);
    stream_ += buf;
}

int PdfCanvas::save() {
    const int prev = save_count();
    state_stack_.push_back(PdfState{current_transform_, current_transform_4x4_, clip_bounds_});
    stream_ += "q\n";
    return prev;
}

int PdfCanvas::save_count() const noexcept {
    return static_cast<int>(state_stack_.size()) + 1;
}

void PdfCanvas::restore_to_count(int count) {
    const int target = std::max(1, count);
    while (save_count() > target) {
        restore();
    }
}

void PdfCanvas::restore() {
    if (state_stack_.empty()) return;
    current_transform_ = state_stack_.back().transform;
    current_transform_4x4_ = state_stack_.back().transform_4x4;
    clip_bounds_ = state_stack_.back().clip_bounds;
    state_stack_.pop_back();
    stream_ += "Q\n";
}

void PdfCanvas::translate(float dx, float dy) {
    current_transform_4x4_ = current_transform_4x4_.pre_translate(dx, dy);
    current_transform_ = current_transform_4x4_.to_transform_2d();
    char buf[64];
    snprintf(buf, sizeof(buf), "1 0 0 1 %.2f %.2f cm\n", dx, dy);
    stream_ += buf;
}

void PdfCanvas::scale(float sx, float sy) {
    current_transform_4x4_ = current_transform_4x4_.pre_scale(sx, sy);
    current_transform_ = current_transform_4x4_.to_transform_2d();
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f 0 0 %.4f 0 0 cm\n", sx, sy);
    stream_ += buf;
}

void PdfCanvas::rotate(float degrees) {
    concat(Transform::from_rotate(degrees));
}

void PdfCanvas::concat(const Transform& ts) {
    current_transform_4x4_ = current_transform_4x4_ * Transform4x4(ts);
    current_transform_ = current_transform_4x4_.to_transform_2d();
    char buf[96];
    snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f %.2f %.2f cm\n",
             ts.sx, ts.ky, ts.kx, ts.sy, ts.tx, ts.ty);
    stream_ += buf;
}

void PdfCanvas::concat(const Transform4x4& ts) {
    current_transform_4x4_ = current_transform_4x4_ * ts;
    current_transform_ = current_transform_4x4_.to_transform_2d();
    const Transform t2 = ts.to_transform_2d();
    char buf[96];
    snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f %.2f %.2f cm\n",
             t2.sx, t2.ky, t2.kx, t2.sy, t2.tx, t2.ty);
    stream_ += buf;
}

void PdfCanvas::set_transform_4x4(const Transform4x4& ts) {
    if (auto inv = current_transform_.invert()) {
        concat(*inv);
    }
    current_transform_4x4_ = ts;
    current_transform_ = ts.to_transform_2d();
    char buf[96];
    snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f %.2f %.2f cm\n",
             current_transform_.sx, current_transform_.ky, current_transform_.kx,
             current_transform_.sy, current_transform_.tx, current_transform_.ty);
    stream_ += buf;
}

void PdfCanvas::clip_rect(const Rect& rect, ClipOp op) {
    if (op == ClipOp::Difference) {
        if (!clip_bounds_.has_value()) return;
        // Emit outer page contour + cutout rect using even-odd clip
        char buf[160];
        snprintf(buf, sizeof(buf), "0 0 %.2f %.2f re %.2f %.2f %.2f %.2f re W* n\n",
                 width_, height_, rect.left(), rect.top(), rect.width(), rect.height());
        stream_ += buf;
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f re W n\n",
             rect.left(), rect.top(), rect.width(), rect.height());
    stream_ += buf;

    if (!clip_bounds_.has_value()) return;
    auto tr = rect.transform(current_transform_);
    if (!tr.has_value() || tr->is_empty()) {
        clip_bounds_ = std::nullopt;
        return;
    }
    const int32_t pw = static_cast<int32_t>(std::max(1.0f, std::ceil(width_)));
    const int32_t ph = static_cast<int32_t>(std::max(1.0f, std::ceil(height_)));
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(tr->left())), 0, pw);
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(tr->top())), 0, ph);
    const int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(tr->right())), 0, pw);
    const int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(tr->bottom())), 0, ph);
    if (x1 > x0 && y1 > y0) {
        if (auto br = ScreenIntRect::from_xywh(
                static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0))) {
            clip_bounds_ = clip_bounds_->intersect(*br);
            return;
        }
    }
    clip_bounds_ = std::nullopt;
}

void PdfCanvas::clip_path(const Path& path, FillRule fill_rule, ClipOp op) {
    if (op == ClipOp::Difference) {
        if (path.is_empty() || !clip_bounds_.has_value()) return;
        char buf[64];
        snprintf(buf, sizeof(buf), "0 0 %.2f %.2f re\n", width_, height_);
        stream_ += buf;
        emit_path_commands(path);
        stream_ += "W* n\n";
        return;
    }
    if (path.is_empty()) {
        clip_bounds_ = std::nullopt;
        stream_ += "0 0 0 0 re W n\n";
        return;
    }
    emit_path_commands(path);
    stream_ += (fill_rule == FillRule::EvenOdd) ? "W* n\n" : "W n\n";

    if (!clip_bounds_.has_value()) return;
    Rect b = path.bounds();
    if (!current_transform_.is_identity()) {
        if (auto tr = path.transform(current_transform_)) b = tr->bounds();
    }
    const int32_t pw = static_cast<int32_t>(std::max(1.0f, std::ceil(width_)));
    const int32_t ph = static_cast<int32_t>(std::max(1.0f, std::ceil(height_)));
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(b.left())), 0, pw);
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(b.top())), 0, ph);
    const int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(b.right())), 0, pw);
    const int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(b.bottom())), 0, ph);
    if (x1 > x0 && y1 > y0) {
        if (auto br = ScreenIntRect::from_xywh(
                static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0))) {
            clip_bounds_ = clip_bounds_->intersect(*br);
            return;
        }
    }
    clip_bounds_ = std::nullopt;
}

void PdfCanvas::apply_fill_paint(const Paint& paint) {
    Color color = paint.shader.is_solid_color() ? paint.shader.solid_color() : paint.shader.sample(0.0f, 0.0f);
    float a = color.alpha();
    if (a < 0.999f) {
        std::string gs = get_or_add_extgstate(a);
        stream_ += "/" + gs + " gs\n";
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f %.4f %.4f rg\n",
             color.red(), color.green(), color.blue());
    stream_ += buf;
    current_fill_color_ = color;
}

void PdfCanvas::apply_stroke_paint(const Paint& paint, const Stroke& stroke) {
    Color color = paint.shader.is_solid_color() ? paint.shader.solid_color() : paint.shader.sample(0.0f, 0.0f);
    float a = color.alpha();
    if (a < 0.999f) {
        std::string gs = get_or_add_extgstate(a);
        stream_ += "/" + gs + " gs\n";
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f %.4f %.4f RG\n",
             color.red(), color.green(), color.blue());
    stream_ += buf;

    // Stroke width
    snprintf(buf, sizeof(buf), "%.2f w\n", stroke.width);
    stream_ += buf;

    // Line cap: Butt = 0, Round = 1, Square = 2
    int cap = 0;
    if (stroke.line_cap == LineCap::Round) cap = 1;
    else if (stroke.line_cap == LineCap::Square) cap = 2;
    snprintf(buf, sizeof(buf), "%d J\n", cap);
    stream_ += buf;

    // Line join: Miter = 0, Round = 1, Bevel = 2
    int join = 0;
    if (stroke.line_join == LineJoin::Round) join = 1;
    else if (stroke.line_join == LineJoin::Bevel) join = 2;
    snprintf(buf, sizeof(buf), "%d j\n", join);
    stream_ += buf;

    // Miter limit
    snprintf(buf, sizeof(buf), "%.2f M\n", stroke.miter_limit);
    stream_ += buf;

    // Dash pattern
    if (stroke.dash.has_value() && !stroke.dash->array().empty()) {
        std::string dash = "[";
        for (size_t i = 0; i < stroke.dash->array().size(); ++i) {
            if (i > 0) dash += ' ';
            char dbuf[32];
            snprintf(dbuf, sizeof(dbuf), "%.2f", stroke.dash->array()[i]);
            dash += dbuf;
        }
        char dbuf[32];
        snprintf(dbuf, sizeof(dbuf), "] %.2f d\n", stroke.dash->offset());
        dash += dbuf;
        stream_ += dash;
    } else {
        stream_ += "[] 0 d\n";
    }
}

void PdfCanvas::emit_path_commands(const Path& path) {
    char buf[128];
    Point last_pt(0, 0);

    auto iter = path.segments();
    while (auto seg_opt = iter.next()) {
        const auto& seg = *seg_opt;
        switch (seg.type) {
            case PathSegment::Type::MoveTo:
                snprintf(buf, sizeof(buf), "%.2f %.2f m\n", seg.p0.x, seg.p0.y);
                stream_ += buf;
                last_pt = seg.p0;
                break;
            case PathSegment::Type::LineTo:
                snprintf(buf, sizeof(buf), "%.2f %.2f l\n", seg.p0.x, seg.p0.y);
                stream_ += buf;
                last_pt = seg.p0;
                break;
            case PathSegment::Type::QuadTo: {
                constexpr float k = 2.0f / 3.0f;
                Point cp1(last_pt.x + (seg.p0.x - last_pt.x) * k, last_pt.y + (seg.p0.y - last_pt.y) * k);
                Point cp2(seg.p1.x + (seg.p0.x - seg.p1.x) * k, seg.p1.y + (seg.p0.y - seg.p1.y) * k);
                snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
                         cp1.x, cp1.y, cp2.x, cp2.y, seg.p1.x, seg.p1.y);
                stream_ += buf;
                last_pt = seg.p1;
                break;
            }
            case PathSegment::Type::CubicTo:
                snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f %.2f %.2f c\n",
                         seg.p0.x, seg.p0.y, seg.p1.x, seg.p1.y, seg.p2.x, seg.p2.y);
                stream_ += buf;
                last_pt = seg.p2;
                break;
            case PathSegment::Type::Close:
                stream_ += "h\n";
                break;
        }
    }
}

void PdfCanvas::fill_rect(const Rect& rect, const Paint& paint) {
    apply_fill_paint(paint);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f re f\n",
             rect.left(), rect.top(), rect.width(), rect.height());
    stream_ += buf;
}

void PdfCanvas::fill_path(const Path& path, const Paint& paint, FillRule fill_rule) {
    if (path.is_empty()) return;
    apply_fill_paint(paint);
    emit_path_commands(path);
    if (fill_rule == FillRule::EvenOdd) {
        stream_ += "f*\n";
    } else {
        stream_ += "f\n";
    }
}

void PdfCanvas::stroke_path(const Path& path, const Paint& paint, const Stroke& stroke) {
    if (path.is_empty()) return;
    apply_stroke_paint(paint, stroke);
    emit_path_commands(path);
    stream_ += "S\n";
}

void PdfCanvas::stroke_rect(const Rect& rect, const Paint& paint, const Stroke& stroke) {
    apply_stroke_paint(paint, stroke);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f re S\n",
             rect.left(), rect.top(), rect.width(), rect.height());
    stream_ += buf;
}

void PdfCanvas::draw_line(Point p1, Point p2, const Paint& paint, const Stroke& stroke) {
    apply_stroke_paint(paint, stroke);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %.2f m %.2f %.2f l S\n",
             p1.x, p1.y, p2.x, p2.y);
    stream_ += buf;
}

void PdfCanvas::draw_circle(Point center, float radius, const Paint& paint) {
    // Approximate circle with 4 cubic Béziers
    const float k = 0.5522847498f * radius;
    PathBuilder pb;
    pb.move_to(center.x, center.y - radius);
    pb.cubic_to(center.x + k, center.y - radius, center.x + radius, center.y - k, center.x + radius, center.y);
    pb.cubic_to(center.x + radius, center.y + k, center.x + k, center.y + radius, center.x, center.y + radius);
    pb.cubic_to(center.x - k, center.y + radius, center.x - radius, center.y + k, center.x - radius, center.y);
    pb.cubic_to(center.x - radius, center.y - k, center.x - k, center.y - radius, center.x, center.y - radius);
    pb.close();
    auto p = pb.finish();
    if (p) fill_path(*p, paint);
}

void PdfCanvas::stroke_circle(Point center, float radius, const Paint& paint, const Stroke& stroke) {
    const float k = 0.5522847498f * radius;
    PathBuilder pb;
    pb.move_to(center.x, center.y - radius);
    pb.cubic_to(center.x + k, center.y - radius, center.x + radius, center.y - k, center.x + radius, center.y);
    pb.cubic_to(center.x + radius, center.y + k, center.x + k, center.y + radius, center.x, center.y + radius);
    pb.cubic_to(center.x - k, center.y + radius, center.x - radius, center.y + k, center.x - radius, center.y);
    pb.cubic_to(center.x - radius, center.y - k, center.x - k, center.y - radius, center.x, center.y - radius);
    pb.close();
    auto p = pb.finish();
    if (p) stroke_path(*p, paint, stroke);
}

std::string PdfCanvas::get_or_add_font(std::string_view font_name) {
    for (const auto& f : fonts_) {
        if (f.base_font == font_name) {
            return f.resource_name;
        }
    }
    std::string res_name = "F" + std::to_string(fonts_.size() + 1);
    fonts_.push_back(PdfFontResource{
        .resource_name = res_name,
        .base_font = std::string(font_name)
    });
    return res_name;
}

std::string PdfCanvas::get_or_add_extgstate(float alpha) {
    int key = static_cast<int>(std::round(alpha * 1000.0f));
    auto it = extgstates_.find(key);
    if (it != extgstates_.end()) {
        return it->second;
    }
    std::string name = "GS" + std::to_string(extgstates_.size() + 1);
    extgstates_[key] = name;
    return name;
}

void PdfCanvas::draw_text(
    std::string_view text,
    float x,
    float y,
    float font_size,
    Color color,
    std::string_view font_name
) {
    if (text.empty()) return;
    std::string font_res = get_or_add_font(font_name);
    apply_fill_paint(Paint(color));

    std::string escaped = escape_pdf_string(text);
    // In our flipped CTM ([1 0 0 -1 0 H]), we use Tm [1 0 0 -1 x baseline]
    // to invert Y locally for the glyphs so they render upright!
    char buf[256];
    float baseline = y + font_size * 0.8f;
    snprintf(buf, sizeof(buf), "BT\n/%s %.2f Tf\n1 0 0 -1 %.2f %.2f Tm\n(%s) Tj\nET\n",
             font_res.c_str(), font_size, x, baseline, escaped.c_str());
    stream_ += buf;
}

void PdfCanvas::draw_pixmap(const Pixmap& pixmap, const Rect& dest_rect) {
    if (pixmap.width() == 0 || pixmap.height() == 0) return;

    std::string res_name = "Im" + std::to_string(images_.size() + 1);
    images_.push_back(PdfImageResource{
        .name = res_name,
        .width = pixmap.width(),
        .height = pixmap.height(),
        .data = std::vector<uint8_t>(pixmap.data(), pixmap.data() + pixmap.data_len())
    });

    // In PDF, an image XObject fills the unit square [0,0]x[1,1] with origin at bottom-left.
    // In our top-down CTM, we map [0,1] to [dest.top, dest.bottom]:
    // Matrix [dest.w, 0, 0, -dest.h, dest.x, dest.bottom]
    char buf[128];
    snprintf(buf, sizeof(buf), "q\n%.2f 0 0 -%.2f %.2f %.2f cm\n/%s Do\nQ\n",
             dest_rect.width(), dest_rect.height(),
             dest_rect.left(), dest_rect.bottom(),
             res_name.c_str());
    stream_ += buf;
}

void PdfCanvas::draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint) {
    if (src.width() == 0 || src.height() == 0 || !src.data()) return;
    auto sr = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(src.width()), static_cast<float>(src.height()));
    auto dr = Rect::from_xywh(static_cast<float>(x), static_cast<float>(y),
                              static_cast<float>(src.width()), static_cast<float>(src.height()));
    if (sr && dr) {
        draw_pixmap_rect(src, *sr, *dr, paint);
    }
}

void PdfCanvas::draw_pixmap_rect(
    const PixmapRef& src,
    const Rect& src_rect,
    const Rect& dst_rect,
    const PixmapPaint& paint
) {
    if (src.width() == 0 || src.height() == 0 || !src.data() ||
        src_rect.is_empty() || dst_rect.is_empty()) {
        return;
    }

    const int32_t sw = static_cast<int32_t>(src.width());
    const int32_t sh = static_cast<int32_t>(src.height());
    const int32_t ix0 = std::clamp(static_cast<int32_t>(std::floor(src_rect.left())), 0, sw - 1);
    const int32_t iy0 = std::clamp(static_cast<int32_t>(std::floor(src_rect.top())), 0, sh - 1);
    const int32_t ix1 = std::clamp(static_cast<int32_t>(std::ceil(src_rect.right())), ix0 + 1, sw);
    const int32_t iy1 = std::clamp(static_cast<int32_t>(std::ceil(src_rect.bottom())), iy0 + 1, sh);
    const uint32_t cw = static_cast<uint32_t>(ix1 - ix0);
    const uint32_t ch = static_cast<uint32_t>(iy1 - iy0);

    auto cropped = Pixmap::allocate(cw, ch, PixelFormat::RGBA8888);
    if (!cropped) return;

    const float op = std::clamp(paint.opacity, 0.0f, 1.0f);
    const float tr = paint.tint.red();
    const float tg = paint.tint.green();
    const float tb = paint.tint.blue();
    const float ta = paint.tint.alpha() * op;

    auto dst_mut = cropped->as_mut();
    for (uint32_t y = 0; y < ch; ++y) {
        for (uint32_t x = 0; x < cw; ++x) {
            auto px_opt = src.pixel(static_cast<uint32_t>(ix0) + x, static_cast<uint32_t>(iy0) + y);
            if (!px_opt) {
                dst_mut.set_pixel(x, y, PremultipliedColorU8::TRANSPARENT);
                continue;
            }
            const ColorU8 straight = px_opt->demultiply();
            const uint8_t r = static_cast<uint8_t>(std::clamp(static_cast<float>(straight.red()) * tr, 0.0f, 255.0f) + 0.5f);
            const uint8_t g = static_cast<uint8_t>(std::clamp(static_cast<float>(straight.green()) * tg, 0.0f, 255.0f) + 0.5f);
            const uint8_t b = static_cast<uint8_t>(std::clamp(static_cast<float>(straight.blue()) * tb, 0.0f, 255.0f) + 0.5f);
            const uint8_t a = static_cast<uint8_t>(std::clamp(static_cast<float>(straight.alpha()) * ta, 0.0f, 255.0f) + 0.5f);
            dst_mut.set_pixel(x, y, PremultipliedColorU8::from_rgba_unchecked(r, g, b, a));
        }
    }

    draw_pixmap(*cropped, dst_rect);
}

void PdfCanvas::draw_vertices(const Vertices& vertices, BlendMode blend_mode, float opacity) {
    (void)blend_mode;
    if (!vertices.is_valid()) return;
    const size_t tri_count = vertices.triangle_count();
    if (tri_count == 0) return;

    const auto& pos = vertices.positions;
    const auto& cols = vertices.colors;
    const size_t npos = pos.size();
    const float alpha = std::clamp(opacity, 0.0f, 1.0f);

    Stroke seam_stroke(0.35f);
    seam_stroke.line_join = LineJoin::Round;
    char buf[160];

    for (size_t t = 0; t < tri_count; ++t) {
        auto [i0, i1, i2] = vertices.get_triangle_indices(t);
        if (i0 >= npos || i1 >= npos || i2 >= npos) continue;

        Color c = Color::WHITE;
        if (!cols.empty() && i0 < cols.size() && i1 < cols.size() && i2 < cols.size()) {
            c = Color::from_rgba_unchecked(
                (cols[i0].red() + cols[i1].red() + cols[i2].red()) / 3.0f,
                (cols[i0].green() + cols[i1].green() + cols[i2].green()) / 3.0f,
                (cols[i0].blue() + cols[i1].blue() + cols[i2].blue()) / 3.0f,
                std::clamp(((cols[i0].alpha() + cols[i1].alpha() + cols[i2].alpha()) / 3.0f) * alpha, 0.0f, 1.0f)
            );
        } else {
            c = Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, alpha);
        }

        Paint p(c);
        apply_fill_paint(p);
        apply_stroke_paint(p, seam_stroke);
        snprintf(buf, sizeof(buf), "%.2f %.2f m %.2f %.2f l %.2f %.2f l h B\n",
                 pos[i0].x, pos[i0].y, pos[i1].x, pos[i1].y, pos[i2].x, pos[i2].y);
        stream_ += buf;
    }
}

void PdfCanvas::draw_annotation(
    const Rect& rect,
    std::string_view key,
    std::span<const uint8_t> data
) {
    if (key.empty()) return;
    Rect page_rect = rect;
    if (current_transform_4x4_.is_planar_perspective()) {
        if (auto mapped = current_transform_4x4_.map_rect(rect)) {
            page_rect = *mapped;
        }
    } else if (auto mapped = rect.transform(current_transform_)) {
        page_rect = *mapped;
    }
    annotations_.push_back(PdfAnnotation{
        .rect = page_rect,
        .key = std::string(key),
        .data = std::vector<uint8_t>(data.begin(), data.end())
    });
}

} // namespace nisaba::pdf
