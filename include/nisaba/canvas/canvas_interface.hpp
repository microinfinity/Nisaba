#pragma once

/// @file canvas_interface.hpp
/// @brief Sovereign 2D vector drawing interface shared by CPU Canvas and GPU GpuCanvas.

#include "nisaba/math/transform.hpp"
#include "nisaba/math/transform4x4.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/math/region.hpp"
#include "nisaba/math/border_radius.hpp"
#include "nisaba/math/rrect.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/path_builder.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/mesh/mesh.hpp"
#include "nisaba/text/text_blob.hpp"
#include "nisaba/effects/drop_shadow.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace nisaba {

/// Boolean clip combination operator.
enum class ClipOp : uint8_t {
    Intersect = 0,  ///< Keep only the region inside both the existing clip and the new shape.
    Difference = 1, ///< Subtract the new shape from the existing clip region.
};

/// Describes a grid partitioning of a source image for 9-slice and general lattice scaling.
///
/// Even column/row indices (0, 2, 4, ...) are fixed-size slices that preserve their source pixel
/// dimensions unless the destination is smaller than the sum of fixed slices.
/// Odd column/row indices (1, 3, 5, ...) are scalable slices that stretch proportionally to fill
/// the remaining destination space.
struct Lattice {
    enum class RectType : uint8_t {
        Default = 0,     ///< Draws the corresponding source image tile.
        Transparent = 1, ///< Skips drawing this tile, leaving the destination untouched.
        FixedColor = 2   ///< Fills this tile with the corresponding entry in `colors`.
    };

    std::span<const int32_t> x_divs{};
    std::span<const int32_t> y_divs{};
    std::span<const RectType> rect_types{};
    std::span<const Color> colors{};
    std::optional<IntRect> bounds{};
};

namespace detail {

struct LatticeCell {
    Rect src_rect{};
    Rect dst_rect{};
    Lattice::RectType type{Lattice::RectType::Default};
    Color color{Color::TRANSPARENT};
    bool scalable_x{false};
    bool scalable_y{false};
};

[[nodiscard]] inline bool compute_lattice_axis(
    int32_t bound_min,
    int32_t bound_max,
    std::span<const int32_t> divs,
    float dst_min,
    float dst_max,
    int32_t* out_src,
    float* out_dst
) {
    if (bound_max <= bound_min || dst_max <= dst_min) return false;

    const size_t n_slices = divs.size() + 1;
    out_src[0] = bound_min;
    int32_t prev = bound_min;
    for (size_t i = 0; i < divs.size(); ++i) {
        int32_t d = std::clamp(divs[i], prev, bound_max);
        out_src[i + 1] = d;
        prev = d;
    }
    out_src[n_slices] = bound_max;

    double fixed_total = 0.0;
    double scalable_total = 0.0;
    for (size_t i = 0; i < n_slices; ++i) {
        const double w = static_cast<double>(out_src[i + 1] - out_src[i]);
        if ((i & 1u) == 0u) fixed_total += w;
        else scalable_total += w;
    }

    const double dst_span = static_cast<double>(dst_max) - static_cast<double>(dst_min);
    if (dst_span <= 0.0) return false;

    out_dst[0] = dst_min;

    double fixed_scale = 1.0;
    double scalable_scale = 0.0;
    if (scalable_total <= 0.0) {
        fixed_scale = (fixed_total > 0.0) ? (dst_span / fixed_total) : 0.0;
        scalable_scale = 0.0;
    } else if (fixed_total >= dst_span) {
        fixed_scale = (fixed_total > 0.0) ? (dst_span / fixed_total) : 0.0;
        scalable_scale = 0.0;
    } else {
        fixed_scale = 1.0;
        scalable_scale = (dst_span - fixed_total) / scalable_total;
    }

    double cursor = static_cast<double>(dst_min);
    for (size_t i = 0; i < n_slices; ++i) {
        const double sw = static_cast<double>(out_src[i + 1] - out_src[i]);
        const double dw = ((i & 1u) == 0u) ? (sw * fixed_scale) : (sw * scalable_scale);
        cursor += dw;
        out_dst[i + 1] = static_cast<float>(cursor);
    }
    out_dst[n_slices] = dst_max;
    return true;
}

template <typename EmitFn>
inline void for_each_lattice_cell(
    uint32_t src_w,
    uint32_t src_h,
    const Lattice& lattice,
    const Rect& dst_rect,
    EmitFn&& emit
) {
    if (src_w == 0 || src_h == 0 || dst_rect.is_empty()) return;

    int32_t bx0 = 0;
    int32_t by0 = 0;
    int32_t bx1 = static_cast<int32_t>(src_w);
    int32_t by1 = static_cast<int32_t>(src_h);
    if (lattice.bounds.has_value()) {
        bx0 = std::clamp(lattice.bounds->left(), 0, static_cast<int32_t>(src_w));
        by0 = std::clamp(lattice.bounds->top(), 0, static_cast<int32_t>(src_h));
        bx1 = std::clamp(lattice.bounds->right(), 0, static_cast<int32_t>(src_w));
        by1 = std::clamp(lattice.bounds->bottom(), 0, static_cast<int32_t>(src_h));
    }
    if (bx1 <= bx0 || by1 <= by0) return;

    const size_t nx_len = lattice.x_divs.size() + 2;
    const size_t ny_len = lattice.y_divs.size() + 2;
    constexpr size_t kStackCap = 16;

    int32_t sx_stack[kStackCap];
    int32_t sy_stack[kStackCap];
    float dx_stack[kStackCap];
    float dy_stack[kStackCap];

    std::vector<int32_t> sx_heap, sy_heap;
    std::vector<float> dx_heap, dy_heap;

    int32_t* sx = sx_stack;
    float* dx = dx_stack;
    if (nx_len > kStackCap) {
        sx_heap.resize(nx_len);
        dx_heap.resize(nx_len);
        sx = sx_heap.data();
        dx = dx_heap.data();
    }

    int32_t* sy = sy_stack;
    float* dy = dy_stack;
    if (ny_len > kStackCap) {
        sy_heap.resize(ny_len);
        dy_heap.resize(ny_len);
        sy = sy_heap.data();
        dy = dy_heap.data();
    }

    if (!compute_lattice_axis(bx0, bx1, lattice.x_divs, dst_rect.left(), dst_rect.right(), sx, dx)) return;
    if (!compute_lattice_axis(by0, by1, lattice.y_divs, dst_rect.top(), dst_rect.bottom(), sy, dy)) return;

    const size_t n_cols = nx_len - 1;
    const size_t n_rows = ny_len - 1;

    for (size_t row = 0; row < n_rows; ++row) {
        const float dy0 = dy[row];
        const float dy1 = dy[row + 1];
        if (dy1 <= dy0) continue;
        const int32_t sy0 = sy[row];
        const int32_t sy1 = sy[row + 1];

        for (size_t col = 0; col < n_cols; ++col) {
            const float dx0 = dx[col];
            const float dx1 = dx[col + 1];
            if (dx1 <= dx0) continue;

            const size_t idx = row * n_cols + col;
            Lattice::RectType rtype = Lattice::RectType::Default;
            if (!lattice.rect_types.empty() && idx < lattice.rect_types.size()) {
                rtype = lattice.rect_types[idx];
            }
            if (rtype == Lattice::RectType::Transparent) continue;

            auto d_opt = Rect::from_ltrb(dx0, dy0, dx1, dy1);
            if (!d_opt || d_opt->is_empty()) continue;

            LatticeCell cell;
            cell.dst_rect = *d_opt;
            cell.type = rtype;
            cell.scalable_x = ((col & 1u) != 0u);
            cell.scalable_y = ((row & 1u) != 0u);

            if (rtype == Lattice::RectType::FixedColor) {
                cell.color = (idx < lattice.colors.size()) ? lattice.colors[idx] : Color::TRANSPARENT;
                emit(cell);
            } else {
                const int32_t sx0 = sx[col];
                const int32_t sx1 = sx[col + 1];
                if (sx1 <= sx0 || sy1 <= sy0) continue;
                auto s_opt = Rect::from_ltrb(
                    static_cast<float>(sx0), static_cast<float>(sy0),
                    static_cast<float>(sx1), static_cast<float>(sy1)
                );
                if (!s_opt || s_opt->is_empty()) continue;
                cell.src_rect = *s_opt;
                emit(cell);
            }
        }
    }
}

[[nodiscard]] inline Paint project_paint_4x4(const Paint& paint, const Transform4x4& m44) {
    if (paint.is_solid_color()) return paint;
    Paint out = paint;
    if (paint.shader.type() == Shader::Type::LinearGradient) {
        const auto& lg = paint.shader.linear_gradient();
        const Point p0 = m44.map_point(lg.start());
        const Point p1 = m44.map_point(lg.end());
        if (auto nlg = LinearGradient::create(p0, p1, lg.stops(), lg.spread_mode())) {
            out.shader = Shader(std::move(*nlg));
        }
    } else if (paint.shader.type() == Shader::Type::RadialGradient) {
        const auto& rg = paint.shader.radial_gradient();
        if (rg.is_simple_radial()) {
            const Point c = m44.map_point(rg.center());
            const Point e = m44.map_point(Point::from_xy(rg.center().x + rg.radius(), rg.center().y));
            const float r = std::max(0.001f, std::hypot(e.x - c.x, e.y - c.y));
            if (auto nrg = RadialGradient::create(c, r, rg.stops(), rg.spread_mode())) {
                out.shader = Shader(std::move(*nrg));
            }
        }
    } else {
        out.shader.transform(m44.to_transform_2d());
    }
    return out;
}

[[nodiscard]] inline std::optional<ScreenIntRect> clip_layer_bounds(
    const std::optional<Rect>& local_bounds,
    const Transform& ts,
    uint32_t canvas_w,
    uint32_t canvas_h,
    const std::optional<ScreenIntRect>& scissor_clip
) noexcept {
    int32_t cw = static_cast<int32_t>(canvas_w);
    int32_t ch = static_cast<int32_t>(canvas_h);
    if (cw <= 0 || ch <= 0) return std::nullopt;

    std::optional<ScreenIntRect> result = scissor_clip;
    if (!result.has_value()) {
        result = ScreenIntRect::from_xywh(0, 0, canvas_w, canvas_h);
    }
    if (!result.has_value()) return std::nullopt;

    if (local_bounds.has_value()) {
        auto tr = local_bounds->transform(ts);
        if (!tr.has_value()) return std::nullopt;
        int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(tr->left())), 0, cw);
        int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(tr->top())), 0, ch);
        int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(tr->right())), 0, cw);
        int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(tr->bottom())), 0, ch);
        if (x1 <= x0 || y1 <= y0) return std::nullopt;
        auto dev = ScreenIntRect::from_xywh(
            static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
            static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)
        );
        if (!dev.has_value()) return std::nullopt;
        result = result->intersect(*dev);
    }
    return result;
}

} // namespace detail

/// Runtime-free canvas backend discriminator for RTTI-free dispatch.
enum class CanvasKind : uint8_t {
    Unknown = 0,
    Software,
    Gpu,
    Pdf,
    Picture
};

class Picture;
class Drawable;

/// @class ICanvas
/// @brief Abstract 2D vector drawing interface shared by software (Canvas) and hardware (GpuCanvas).
class ICanvas {
public:
    virtual ~ICanvas() = default;

    [[nodiscard]] virtual CanvasKind canvas_kind() const noexcept { return CanvasKind::Unknown; }
    [[nodiscard]] virtual uint32_t canvas_width() const noexcept { return 0; }
    [[nodiscard]] virtual uint32_t canvas_height() const noexcept { return 0; }

    virtual int save() = 0;
    [[nodiscard]] virtual int save_count() const noexcept { return 1; }
    virtual void restore_to_count(int count) {
        const int target = std::max(1, count);
        while (save_count() > target) {
            restore();
        }
    }
    virtual int save_layer(const Paint& paint = Paint(Color::WHITE)) {
        (void)paint;
        return save();
    }
    virtual int save_layer_bounds(const Rect& bounds, const Paint& paint = Paint(Color::WHITE)) {
        const int prev = save_layer(paint);
        clip_rect(bounds);
        return prev;
    }
    virtual int save_layer_backdrop(
        std::optional<Rect> bounds = std::nullopt,
        const Paint& paint = Paint(Color::WHITE),
        float blur_sigma = 0.0f
    ) {
        (void)blur_sigma;
        if (bounds.has_value()) {
            return save_layer_bounds(*bounds, paint);
        }
        return save_layer(paint);
    }
    int save_layer_alpha(float alpha) {
        Paint p(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)));
        return save_layer(p);
    }
    int save_layer_alpha(const Rect& bounds, float alpha) {
        Paint p(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)));
        return save_layer_bounds(bounds, p);
    }
    int save_layer_alpha(std::optional<Rect> bounds, float alpha) {
        Paint p(Color::from_rgba_unchecked(1.0f, 1.0f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)));
        if (bounds.has_value()) {
            return save_layer_bounds(*bounds, p);
        }
        return save_layer(p);
    }
    virtual void restore() = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotate(float degrees) {
        concat(Transform::from_rotate(degrees));
    }
    void rotate(float degrees, float px, float py) {
        concat(Transform::from_rotate_at(degrees, px, py));
    }
    virtual void skew(float kx, float ky) {
        concat(Transform::from_skew(kx, ky));
    }
    virtual void concat(const Transform& ts) = 0;
    virtual void concat(const Transform4x4& ts) {
        concat(ts.to_transform_2d());
    }
    virtual void set_transform_4x4(const Transform4x4& ts) {
        (void)ts;
    }
    [[nodiscard]] virtual Transform current_transform() const noexcept { return Transform::identity(); }
    [[nodiscard]] virtual Transform4x4 current_transform_4x4() const noexcept {
        return Transform4x4(current_transform());
    }

    // --- Clip Queries & Frustum Culling ---
    [[nodiscard]] virtual std::optional<ScreenIntRect> get_device_clip_bounds() const noexcept {
        return ScreenIntRect::from_xywh(0, 0, canvas_width(), canvas_height());
    }

    [[nodiscard]] virtual std::optional<Rect> get_local_clip_bounds() const noexcept {
        const auto dev = get_device_clip_bounds();
        if (!dev.has_value()) return std::nullopt;
        const auto inv = current_transform().invert();
        if (!inv.has_value()) return std::nullopt;
        return dev->to_rect().transform(*inv);
    }

    [[nodiscard]] virtual bool quick_reject(const Rect& rect) const noexcept {
        const auto dev_clip_opt = get_device_clip_bounds();
        if (!dev_clip_opt.has_value()) return true;
        const Transform ctm = current_transform();
        if (!ctm.is_valid()) return true;
        const auto dev_rect = rect.transform(ctm);
        if (!dev_rect.has_value()) return true;
        const Rect dev_clip = dev_clip_opt->to_rect();
        return dev_rect->right() <= dev_clip.left() ||
               dev_rect->left() >= dev_clip.right() ||
               dev_rect->bottom() <= dev_clip.top() ||
               dev_rect->top() >= dev_clip.bottom();
    }

    [[nodiscard]] virtual bool quick_reject(const Path& path) const noexcept {
        if (path.is_empty()) return true;
        return quick_reject(path.bounds());
    }

    // --- Full-Clip Fill Operations ---
    virtual void draw_paint(const Paint& paint) {
        const auto dev_clip = get_device_clip_bounds();
        if (!dev_clip.has_value()) return;
        const auto inv = current_transform().invert();
        if (!inv.has_value()) return;
        const Rect dev_r = dev_clip->to_rect();
        auto outset = Rect::from_ltrb(dev_r.left() - 1.0f, dev_r.top() - 1.0f,
                                      dev_r.right() + 1.0f, dev_r.bottom() + 1.0f);
        if (!outset.has_value()) return;
        auto local = outset->transform(*inv);
        if (!local.has_value()) return;
        Paint p = paint;
        p.clear_path_effect();
        fill_rect(*local, p);
    }

    virtual void draw_color(Color color, BlendMode mode = BlendMode::SourceOver) {
        Paint p(color);
        p.blend_mode = mode;
        draw_paint(p);
    }

    virtual void fill_rect(const Rect& rect, const Paint& paint) = 0;
    virtual void stroke_rect(const Rect& rect, const Paint& paint, const Stroke& stroke) {
        auto p = PathBuilder::from_rect(rect);
        if (!p.is_empty()) stroke_path(p, paint, stroke);
    }
    virtual void fill_path(const Path& path, const Paint& paint, FillRule fill_rule = FillRule::Winding) = 0;
    virtual void stroke_path(const Path& path, const Paint& paint, const Stroke& stroke) = 0;

    virtual void clip_path(
        const Path& path,
        FillRule fill_rule = FillRule::Winding,
        ClipOp op = ClipOp::Intersect
    ) {
        (void)path; (void)fill_rule; (void)op;
    }
    virtual void clip_rect(const Rect& rect, ClipOp op = ClipOp::Intersect) {
        if (auto p = PathBuilder::from_rect(rect); !p.is_empty()) {
            clip_path(p, FillRule::Winding, op);
        }
    }
    virtual void clip_shader(const Shader& shader, ClipOp op = ClipOp::Intersect) {
        (void)shader; (void)op;
    }
    virtual void clip_region(const Region& region, ClipOp op = ClipOp::Intersect) {
        if (region.is_empty()) {
            if (op == ClipOp::Intersect) {
                clip_path(Path(), FillRule::Winding, ClipOp::Intersect);
            }
            return;
        }
        const Transform4x4 saved_m = current_transform_4x4();
        set_transform_4x4(Transform4x4::identity());
        if (region.is_rect()) {
            clip_rect(region.bounds()->to_rect(), op);
        } else {
            clip_path(region.to_path(), FillRule::Winding, op);
        }
        set_transform_4x4(saved_m);
    }

    virtual void draw_region(
        const Region& region,
        const Paint& paint,
        const Stroke& stroke = Stroke{}
    ) {
        if (region.is_empty()) return;
        if (paint.style == PaintStyle::Fill && !paint.has_layer_filter() && !paint.has_path_effect()) {
            if (region.is_rect()) {
                fill_rect(region.bounds()->to_rect(), paint);
            } else {
                for (const auto& r : region.rects()) {
                    fill_rect(r.to_rect(), paint);
                }
            }
            return;
        }
        const Path p = region.to_path();
        if (p.is_empty()) return;
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_path(p, paint, FillRule::Winding);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_path(p, paint, stroke);
        }
    }

    // --- Retained Display Lists & Drawables ---
    virtual void draw_picture(
        const Picture& picture,
        const Transform* transform = nullptr,
        const Paint* paint = nullptr
    );
    void draw_picture(
        const std::shared_ptr<Picture>& picture,
        const Transform* transform = nullptr,
        const Paint* paint = nullptr
    ) {
        if (picture) draw_picture(*picture, transform, paint);
    }

    virtual void draw_drawable(
        Drawable& drawable,
        const Transform* transform = nullptr
    );
    virtual void draw_drawable(
        const std::shared_ptr<Drawable>& drawable,
        const Transform* transform = nullptr
    ) {
        if (drawable) draw_drawable(*drawable, transform);
    }

    // --- Positioned & Transformed Glyph Runs ---
    virtual void draw_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        const text::TtfFont& font,
        float font_size,
        const Paint& paint
    );
    virtual void draw_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        const text::TtfFont& font,
        float font_size,
        const Paint& paint
    );
    virtual void draw_text_blob(
        const TextBlob& blob,
        float x,
        float y,
        const Paint& paint
    );
    void draw_text_blob(
        const std::shared_ptr<TextBlob>& blob,
        float x,
        float y,
        const Paint& paint
    ) {
        if (blob) draw_text_blob(*blob, x, y, paint);
    }

    // --- Document Annotations & Framebuffer Discard Hints ---
    virtual void draw_annotation(
        const Rect& rect,
        std::string_view key,
        std::span<const uint8_t> data
    ) {
        (void)rect; (void)key; (void)data;
    }
    void draw_annotation(
        const Rect& rect,
        std::string_view key,
        std::string_view value
    ) {
        draw_annotation(
            rect,
            key,
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size())
        );
    }

    /// Hints that the current framebuffer contents may be discarded without store/load overhead
    /// on tile-based and offscreen GPUs.
    virtual void discard() {}

    virtual void draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint = PixmapPaint()) {
        (void)x; (void)y; (void)src; (void)paint;
    }

    virtual void draw_pixmap_rect(
        const PixmapRef& src,
        const Rect& src_rect,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        (void)src; (void)src_rect; (void)dst_rect; (void)paint;
    }

    void draw_pixmap_rect(
        const PixmapRef& src,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        if (auto sr = Rect::from_xywh(0.0f, 0.0f, static_cast<float>(src.width()), static_cast<float>(src.height()))) {
            draw_pixmap_rect(src, *sr, dst_rect, paint);
        }
    }

    virtual void draw_image_lattice(
        const PixmapRef& src,
        const Lattice& lattice,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        if (src.width() == 0 || src.height() == 0 || !src.data() || dst_rect.is_empty()) return;
        if (quick_reject(dst_rect)) return;
        const float op = std::clamp(paint.opacity, 0.0f, 1.0f);
        detail::for_each_lattice_cell(src.width(), src.height(), lattice, dst_rect,
            [&](const detail::LatticeCell& cell) {
                if (cell.type == Lattice::RectType::FixedColor) {
                    const Color c = Color::from_rgba_unchecked(
                        cell.color.red(), cell.color.green(), cell.color.blue(),
                        std::clamp(cell.color.alpha() * op, 0.0f, 1.0f)
                    );
                    Paint fp(c);
                    fp.blend_mode = paint.blend_mode;
                    fill_rect(cell.dst_rect, fp);
                } else {
                    draw_pixmap_rect(src, cell.src_rect, cell.dst_rect, paint);
                }
            });
    }

    virtual void draw_image_nine(
        const PixmapRef& src,
        const IntRect& center,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        const std::array<int32_t, 2> x_divs{center.left(), center.right()};
        const std::array<int32_t, 2> y_divs{center.top(), center.bottom()};
        Lattice lattice;
        lattice.x_divs = x_divs;
        lattice.y_divs = y_divs;
        draw_image_lattice(src, lattice, dst_rect, paint);
    }

    virtual void draw_atlas(
        const PixmapRef& src,
        std::span<const RSXform> xforms,
        std::span<const Rect> tex_rects,
        std::span<const Color> colors = {},
        BlendMode mode = BlendMode::Modulate,
        const PixmapPaint& paint = PixmapPaint()
    ) {
        if (src.width() == 0 || src.height() == 0 || !src.data()) return;
        const size_t count = std::min(xforms.size(), tex_rects.size());
        if (count == 0) return;
        const bool has_colors = colors.size() >= count;
        for (size_t i = 0; i < count; ++i) {
            const Rect& sr = tex_rects[i];
            if (sr.is_empty()) continue;
            auto dr = Rect::from_xywh(0.0f, 0.0f, sr.width(), sr.height());
            if (!dr) continue;
            save();
            concat(xforms[i].to_transform());
            if (has_colors && mode == BlendMode::Source) {
                const float a = std::clamp(colors[i].alpha() * paint.opacity, 0.0f, 1.0f);
                Paint sp(Color::from_rgba_unchecked(colors[i].red(), colors[i].green(), colors[i].blue(), a));
                sp.blend_mode = paint.blend_mode;
                fill_rect(*dr, sp);
            } else {
                PixmapPaint sp = paint;
                if (has_colors && mode == BlendMode::Modulate) {
                    sp.tint = Color::from_rgba_unchecked(
                        paint.tint.red() * colors[i].red(),
                        paint.tint.green() * colors[i].green(),
                        paint.tint.blue() * colors[i].blue(),
                        paint.tint.alpha() * colors[i].alpha()
                    );
                }
                draw_pixmap_rect(src, sr, *dr, sp);
            }
            restore();
        }
    }

    virtual void draw_vertices(
        const Vertices& vertices,
        BlendMode blend_mode = BlendMode::SourceOver,
        float opacity = 1.0f
    ) {
        (void)vertices; (void)blend_mode; (void)opacity;
    }

    virtual void draw_textured_vertices(
        const Vertices& vertices,
        const PixmapRef& texture,
        BlendMode blend_mode = BlendMode::SourceOver,
        float opacity = 1.0f
    ) {
        (void)vertices; (void)texture; (void)blend_mode; (void)opacity;
    }

    virtual void draw_coons_patch(
        const CoonsPatch& patch,
        BlendMode blend_mode = BlendMode::SourceOver,
        float opacity = 1.0f,
        uint32_t subdiv_u = 0,
        uint32_t subdiv_v = 0
    ) {
        if (subdiv_u == 0 || subdiv_v == 0) {
            const auto [au, av] = patch.compute_adaptive_subdivision(current_transform());
            if (subdiv_u == 0) subdiv_u = au;
            if (subdiv_v == 0) subdiv_v = av;
        }
        draw_vertices(patch.to_vertices(subdiv_u, subdiv_v), blend_mode, opacity);
    }

    virtual void draw_gradient_mesh(
        const GradientMesh& mesh,
        BlendMode blend_mode = BlendMode::SourceOver,
        float opacity = 1.0f,
        uint32_t subdivision = 0
    ) {
        if (subdivision == 0) {
            draw_vertices(mesh.to_vertices_adaptive(current_transform()), blend_mode, opacity);
        } else {
            draw_vertices(mesh.to_vertices(subdivision), blend_mode, opacity);
        }
    }

    virtual void draw_pixmap_3d(
        const PixmapRef& src,
        const Transform4x4& transform3d,
        float opacity = 1.0f,
        BlendMode blend_mode = BlendMode::SourceOver
    ) {
        (void)src; (void)transform3d; (void)opacity; (void)blend_mode;
    }

    virtual void fill_round_rect(const Rect& rect, float rx, float ry, const Paint& paint) {
        if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
            fill_path(*p, paint);
        }
    }
    virtual void stroke_round_rect(const Rect& rect, float rx, float ry, const Paint& paint, const Stroke& stroke) {
        if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
            stroke_path(*p, paint, stroke);
        }
    }

    virtual void fill_rrect(const RRect& rrect, const Paint& paint) {
        fill_path(rrect.to_path(), paint);
    }
    virtual void stroke_rrect(const RRect& rrect, const Paint& paint, const Stroke& stroke) {
        stroke_path(rrect.to_path(), paint, stroke);
    }
    virtual void clip_rrect(const RRect& rrect, ClipOp op = ClipOp::Intersect) {
        clip_path(rrect.to_path(), FillRule::Winding, op);
    }

    virtual void fill_oval(const Rect& oval, const Paint& paint) {
        if (auto p = PathBuilder::from_oval(oval)) {
            fill_path(*p, paint);
        }
    }
    virtual void stroke_oval(const Rect& oval, const Paint& paint, const Stroke& stroke) {
        if (auto p = PathBuilder::from_oval(oval)) {
            stroke_path(*p, paint, stroke);
        }
    }
    virtual void clip_oval(const Rect& oval, ClipOp op = ClipOp::Intersect) {
        if (auto p = PathBuilder::from_oval(oval)) {
            clip_path(*p, FillRule::Winding, op);
        }
    }

    virtual void fill_arc(const Rect& oval, float start_deg, float sweep_deg, bool use_center, const Paint& paint) {
        PathBuilder pb;
        pb.push_arc(oval, start_deg, sweep_deg, use_center, true);
        if (auto p = pb.finish(); p && !p->is_empty()) {
            fill_path(*p, paint);
        }
    }
    virtual void stroke_arc(
        const Rect& oval, float start_deg, float sweep_deg, bool use_center, const Paint& paint, const Stroke& stroke
    ) {
        PathBuilder pb;
        pb.push_arc(oval, start_deg, sweep_deg, use_center, true);
        if (auto p = pb.finish(); p && !p->is_empty()) {
            stroke_path(*p, paint, stroke);
        }
    }

    virtual void fill_drrect(const RRect& outer, const RRect& inner, const Paint& paint) {
        const auto po = outer.to_path();
        const auto pi = inner.to_path();
        if (auto diff = po.difference(pi)) {
            if (!diff->is_empty()) {
                fill_path(*diff, paint);
            }
        } else {
            fill_path(po, paint);
        }
    }
    virtual void stroke_drrect(const RRect& outer, const RRect& inner, const Paint& paint, const Stroke& stroke) {
        stroke_rrect(outer, paint, stroke);
        stroke_rrect(inner, paint, stroke);
    }

    virtual void draw_points(
        PointMode mode,
        std::span<const Point> points,
        const Paint& paint,
        const Stroke& stroke = Stroke{}
    ) {
        if (points.empty()) return;
        PathBuilder pb;
        switch (mode) {
            case PointMode::Points: {
                const float w = std::max(1.0f, stroke.width);
                const float r = w * 0.5f;
                const bool round = (stroke.line_cap == LineCap::Round);
                for (const auto& pt : points) {
                    if (auto rect = Rect::from_xywh(pt.x - r, pt.y - r, w, w)) {
                        if (round) pb.push_oval(*rect);
                        else pb.push_rect(*rect);
                    }
                }
                if (auto p = pb.finish(); p && !p->is_empty()) {
                    fill_path(*p, paint);
                }
                break;
            }
            case PointMode::Lines: {
                for (size_t i = 0; i + 1 < points.size(); i += 2) {
                    pb.move_to(points[i]);
                    pb.line_to(points[i + 1]);
                }
                if (auto p = pb.finish(); p && !p->is_empty()) {
                    stroke_path(*p, paint, stroke);
                }
                break;
            }
            case PointMode::Polygon: {
                if (points.size() < 2) return;
                pb.move_to(points[0]);
                for (size_t i = 1; i < points.size(); ++i) {
                    pb.line_to(points[i]);
                }
                if (auto p = pb.finish(); p && !p->is_empty()) {
                    stroke_path(*p, paint, stroke);
                }
                break;
            }
        }
    }

    // --- Paint::style Dispatchers ---
    void draw_path(
        const Path& path,
        const Paint& paint,
        const Stroke& stroke = Stroke{},
        FillRule fill_rule = FillRule::Winding
    ) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_path(path, paint, fill_rule);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_path(path, paint, stroke);
        }
    }

    void draw_rect(const Rect& rect, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_rect(rect, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_rect(rect, paint, stroke);
        }
    }

    void draw_round_rect(const Rect& rect, float rx, float ry, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_round_rect(rect, rx, ry, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_round_rect(rect, rx, ry, paint, stroke);
        }
    }

    void draw_oval(const Rect& oval, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_oval(oval, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_oval(oval, paint, stroke);
        }
    }

    void draw_arc(
        const Rect& oval,
        float start_deg,
        float sweep_deg,
        bool use_center,
        const Paint& paint,
        const Stroke& stroke = Stroke{}
    ) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_arc(oval, start_deg, sweep_deg, use_center, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_arc(oval, start_deg, sweep_deg, use_center, paint, stroke);
        }
    }

    void draw_rrect(const RRect& rrect, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_rrect(rrect, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_rrect(rrect, paint, stroke);
        }
    }

    void draw_drrect(const RRect& outer, const RRect& inner, const Paint& paint, const Stroke& stroke = Stroke{}) {
        if (paint.style == PaintStyle::Fill || paint.style == PaintStyle::StrokeAndFill) {
            fill_drrect(outer, inner, paint);
        }
        if (paint.style == PaintStyle::Stroke || paint.style == PaintStyle::StrokeAndFill) {
            stroke_drrect(outer, inner, paint, stroke);
        }
    }

    // --- Shadows & Effects ---
    virtual void draw_path_with_shadow(
        const Path& path,
        const Paint& paint,
        const effects::DropShadow& shadow,
        FillRule fill_rule = FillRule::Winding
    ) {
        (void)shadow;
        fill_path(path, paint, fill_rule);
    }

    virtual void draw_rect_with_shadow(
        const Rect& rect,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        fill_rect(rect, paint);
    }

    virtual void draw_round_rect_with_shadow(
        const Rect& rect,
        float rx,
        float ry,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        fill_round_rect(rect, rx, ry, paint);
    }

    virtual void draw_rrect_with_shadow(
        const RRect& rrect,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        fill_rrect(rrect, paint);
    }

    virtual void draw_circle_with_shadow(
        float cx,
        float cy,
        float radius,
        const Paint& paint,
        const effects::DropShadow& shadow
    ) {
        (void)shadow;
        if (auto p = PathBuilder::from_circle(cx, cy, radius)) {
            fill_path(*p, paint);
        }
    }
};

} // namespace nisaba
