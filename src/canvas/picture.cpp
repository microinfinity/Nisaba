#include "nisaba/canvas/picture.hpp"
#include <algorithm>
#include <cstring>
#include <variant>

namespace nisaba {

namespace {

[[nodiscard]] Pixmap clone_pixmap_ref(const PixmapRef& src) {
    if (src.width() == 0 || src.height() == 0 || !src.data()) return Pixmap{};
    auto pm = Pixmap::allocate(src.width(), src.height(), src.format());
    if (!pm) return Pixmap{};
    const size_t row_bytes = static_cast<size_t>(src.width()) * bytes_per_pixel(src.format());
    const uint8_t* src_base = static_cast<const uint8_t*>(src.data());
    auto dst_mut = pm->as_mut();
    for (uint32_t y = 0; y < src.height(); ++y) {
        std::memcpy(dst_mut.row_raw(y), src_base + static_cast<size_t>(y) * src.stride_bytes(), row_bytes);
    }
    return std::move(*pm);
}

struct CmdSave {};
struct CmdSaveLayer { Paint paint; };
struct CmdSaveLayerBounds { Rect bounds; Paint paint; };
struct CmdSaveLayerBackdrop { std::optional<Rect> bounds; Paint paint; float blur_sigma; };
struct CmdRestore {};
struct CmdTranslate { float dx; float dy; };
struct CmdScale { float sx; float sy; };
struct CmdRotate { float degrees; };
struct CmdSkew { float kx; float ky; };
struct CmdConcat { Transform ts; };
struct CmdConcat4x4 { Transform4x4 ts; };
struct CmdSetTransform4x4 { Transform4x4 ts; };

struct CmdClipRect { Rect rect; ClipOp op; };
struct CmdClipRRect { RRect rrect; ClipOp op; };
struct CmdClipOval { Rect oval; ClipOp op; };
struct CmdClipPath { Path path; FillRule rule; ClipOp op; };
struct CmdClipShader { Shader shader; ClipOp op; };
struct CmdClipRegion { Region region; ClipOp op; };

struct CmdDrawPaint { Paint paint; };
struct CmdDrawColor { Color color; BlendMode mode; };
struct CmdFillRect { Rect rect; Paint paint; };
struct CmdStrokeRect { Rect rect; Paint paint; Stroke stroke; };
struct CmdFillRoundRect { Rect rect; float rx; float ry; Paint paint; };
struct CmdStrokeRoundRect { Rect rect; float rx; float ry; Paint paint; Stroke stroke; };
struct CmdFillRRect { RRect rrect; Paint paint; };
struct CmdStrokeRRect { RRect rrect; Paint paint; Stroke stroke; };
struct CmdFillOval { Rect oval; Paint paint; };
struct CmdStrokeOval { Rect oval; Paint paint; Stroke stroke; };
struct CmdFillArc { Rect oval; float start_deg; float sweep_deg; bool use_center; Paint paint; };
struct CmdStrokeArc { Rect oval; float start_deg; float sweep_deg; bool use_center; Paint paint; Stroke stroke; };
struct CmdFillDRRect { RRect outer; RRect inner; Paint paint; };
struct CmdStrokeDRRect { RRect outer; RRect inner; Paint paint; Stroke stroke; };
struct CmdFillPath { Path path; Paint paint; FillRule rule; };
struct CmdStrokePath { Path path; Paint paint; Stroke stroke; };
struct CmdDrawPoints { PointMode mode; std::vector<Point> points; Paint paint; Stroke stroke; };
struct CmdDrawRegion { Region region; Paint paint; Stroke stroke; };

struct CmdDrawPixmap { int32_t x; int32_t y; Pixmap pixmap; PixmapPaint paint; };
struct CmdDrawPixmapRect { Pixmap pixmap; Rect src_rect; Rect dst_rect; PixmapPaint paint; };
struct CmdDrawPixmap3D { Pixmap pixmap; Transform4x4 transform3d; float opacity; BlendMode blend_mode; };
struct CmdDrawImageLattice {
    Pixmap pixmap;
    std::vector<int32_t> x_divs;
    std::vector<int32_t> y_divs;
    std::vector<Lattice::RectType> rect_types;
    std::vector<Color> colors;
    std::optional<IntRect> bounds;
    Rect dst_rect;
    PixmapPaint paint;
};
struct CmdDrawImageNine { Pixmap pixmap; IntRect center; Rect dst_rect; PixmapPaint paint; };
struct CmdDrawAtlas {
    Pixmap pixmap;
    std::vector<RSXform> xforms;
    std::vector<Rect> tex_rects;
    std::vector<Color> colors;
    BlendMode mode;
    PixmapPaint paint;
};

struct CmdDrawVertices { Vertices vertices; BlendMode blend_mode; float opacity; };
struct CmdDrawTexturedVertices { Vertices vertices; Pixmap texture; BlendMode blend_mode; float opacity; };
struct CmdDrawCoonsPatch { CoonsPatch patch; BlendMode blend_mode; float opacity; uint32_t su; uint32_t sv; };
struct CmdDrawGradientMesh { GradientMesh mesh; BlendMode blend_mode; float opacity; uint32_t subdivision; };

struct CmdDrawPathShadow { Path path; Paint paint; effects::DropShadow shadow; FillRule rule; };
struct CmdDrawRectShadow { Rect rect; Paint paint; effects::DropShadow shadow; };
struct CmdDrawRoundRectShadow { Rect rect; float rx; float ry; Paint paint; effects::DropShadow shadow; };
struct CmdDrawRRectShadow { RRect rrect; Paint paint; effects::DropShadow shadow; };
struct CmdDrawCircleShadow { float cx; float cy; float radius; Paint paint; effects::DropShadow shadow; };

struct CmdDrawPicture {
    std::shared_ptr<Picture> picture;
    std::optional<Transform> transform;
    std::optional<Paint> paint;
};
struct CmdDrawDrawable {
    std::shared_ptr<Drawable> drawable;
    std::optional<Transform> transform;
};
struct CmdDrawGlyphsPos {
    const text::TtfFont* font{nullptr};
    float font_size{0.0f};
    std::vector<uint16_t> glyph_ids{};
    std::vector<Point> positions{};
    Paint paint{};
};
struct CmdDrawGlyphsRSXform {
    const text::TtfFont* font{nullptr};
    float font_size{0.0f};
    std::vector<uint16_t> glyph_ids{};
    std::vector<RSXform> xforms{};
    Paint paint{};
};
struct CmdDrawTextBlob {
    TextBlob blob{};
    float x{0.0f};
    float y{0.0f};
    Paint paint{};
};
struct CmdDrawAnnotation {
    Rect rect{};
    std::string key{};
    std::vector<uint8_t> data{};
};
struct CmdDiscard {};

using PictureCmd = std::variant<
    CmdSave,
    CmdSaveLayer,
    CmdSaveLayerBounds,
    CmdSaveLayerBackdrop,
    CmdRestore,
    CmdTranslate,
    CmdScale,
    CmdRotate,
    CmdSkew,
    CmdConcat,
    CmdConcat4x4,
    CmdSetTransform4x4,
    CmdClipRect,
    CmdClipRRect,
    CmdClipOval,
    CmdClipPath,
    CmdClipShader,
    CmdClipRegion,
    CmdDrawPaint,
    CmdDrawColor,
    CmdFillRect,
    CmdStrokeRect,
    CmdFillRoundRect,
    CmdStrokeRoundRect,
    CmdFillRRect,
    CmdStrokeRRect,
    CmdFillOval,
    CmdStrokeOval,
    CmdFillArc,
    CmdStrokeArc,
    CmdFillDRRect,
    CmdStrokeDRRect,
    CmdFillPath,
    CmdStrokePath,
    CmdDrawPoints,
    CmdDrawRegion,
    CmdDrawPixmap,
    CmdDrawPixmapRect,
    CmdDrawPixmap3D,
    CmdDrawImageLattice,
    CmdDrawImageNine,
    CmdDrawAtlas,
    CmdDrawVertices,
    CmdDrawTexturedVertices,
    CmdDrawCoonsPatch,
    CmdDrawGradientMesh,
    CmdDrawPathShadow,
    CmdDrawRectShadow,
    CmdDrawRoundRectShadow,
    CmdDrawRRectShadow,
    CmdDrawCircleShadow,
    CmdDrawPicture,
    CmdDrawDrawable,
    CmdDrawGlyphsPos,
    CmdDrawGlyphsRSXform,
    CmdDrawTextBlob,
    CmdDrawAnnotation,
    CmdDiscard
>;

void replay_cmd(ICanvas& c, const PictureCmd& cmd) {
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CmdSave>) {
            c.save();
        } else if constexpr (std::is_same_v<T, CmdSaveLayer>) {
            c.save_layer(op.paint);
        } else if constexpr (std::is_same_v<T, CmdSaveLayerBounds>) {
            c.save_layer_bounds(op.bounds, op.paint);
        } else if constexpr (std::is_same_v<T, CmdSaveLayerBackdrop>) {
            c.save_layer_backdrop(op.bounds, op.paint, op.blur_sigma);
        } else if constexpr (std::is_same_v<T, CmdRestore>) {
            c.restore();
        } else if constexpr (std::is_same_v<T, CmdTranslate>) {
            c.translate(op.dx, op.dy);
        } else if constexpr (std::is_same_v<T, CmdScale>) {
            c.scale(op.sx, op.sy);
        } else if constexpr (std::is_same_v<T, CmdRotate>) {
            c.rotate(op.degrees);
        } else if constexpr (std::is_same_v<T, CmdSkew>) {
            c.skew(op.kx, op.ky);
        } else if constexpr (std::is_same_v<T, CmdConcat>) {
            c.concat(op.ts);
        } else if constexpr (std::is_same_v<T, CmdConcat4x4>) {
            c.concat(op.ts);
        } else if constexpr (std::is_same_v<T, CmdSetTransform4x4>) {
            c.set_transform_4x4(op.ts);
        } else if constexpr (std::is_same_v<T, CmdClipRect>) {
            c.clip_rect(op.rect, op.op);
        } else if constexpr (std::is_same_v<T, CmdClipRRect>) {
            c.clip_rrect(op.rrect, op.op);
        } else if constexpr (std::is_same_v<T, CmdClipOval>) {
            c.clip_oval(op.oval, op.op);
        } else if constexpr (std::is_same_v<T, CmdClipPath>) {
            c.clip_path(op.path, op.rule, op.op);
        } else if constexpr (std::is_same_v<T, CmdClipShader>) {
            c.clip_shader(op.shader, op.op);
        } else if constexpr (std::is_same_v<T, CmdClipRegion>) {
            c.clip_region(op.region, op.op);
        } else if constexpr (std::is_same_v<T, CmdDrawPaint>) {
            c.draw_paint(op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawColor>) {
            c.draw_color(op.color, op.mode);
        } else if constexpr (std::is_same_v<T, CmdFillRect>) {
            c.fill_rect(op.rect, op.paint);
        } else if constexpr (std::is_same_v<T, CmdStrokeRect>) {
            c.stroke_rect(op.rect, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdFillRoundRect>) {
            c.fill_round_rect(op.rect, op.rx, op.ry, op.paint);
        } else if constexpr (std::is_same_v<T, CmdStrokeRoundRect>) {
            c.stroke_round_rect(op.rect, op.rx, op.ry, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdFillRRect>) {
            c.fill_rrect(op.rrect, op.paint);
        } else if constexpr (std::is_same_v<T, CmdStrokeRRect>) {
            c.stroke_rrect(op.rrect, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdFillOval>) {
            c.fill_oval(op.oval, op.paint);
        } else if constexpr (std::is_same_v<T, CmdStrokeOval>) {
            c.stroke_oval(op.oval, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdFillArc>) {
            c.fill_arc(op.oval, op.start_deg, op.sweep_deg, op.use_center, op.paint);
        } else if constexpr (std::is_same_v<T, CmdStrokeArc>) {
            c.stroke_arc(op.oval, op.start_deg, op.sweep_deg, op.use_center, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdFillDRRect>) {
            c.fill_drrect(op.outer, op.inner, op.paint);
        } else if constexpr (std::is_same_v<T, CmdStrokeDRRect>) {
            c.stroke_drrect(op.outer, op.inner, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdFillPath>) {
            c.fill_path(op.path, op.paint, op.rule);
        } else if constexpr (std::is_same_v<T, CmdStrokePath>) {
            c.stroke_path(op.path, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdDrawPoints>) {
            c.draw_points(op.mode, op.points, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdDrawRegion>) {
            c.draw_region(op.region, op.paint, op.stroke);
        } else if constexpr (std::is_same_v<T, CmdDrawPixmap>) {
            c.draw_pixmap(op.x, op.y, op.pixmap.as_ref(), op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawPixmapRect>) {
            c.draw_pixmap_rect(op.pixmap.as_ref(), op.src_rect, op.dst_rect, op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawPixmap3D>) {
            c.draw_pixmap_3d(op.pixmap.as_ref(), op.transform3d, op.opacity, op.blend_mode);
        } else if constexpr (std::is_same_v<T, CmdDrawImageLattice>) {
            Lattice lat;
            lat.x_divs = op.x_divs;
            lat.y_divs = op.y_divs;
            lat.rect_types = op.rect_types;
            lat.colors = op.colors;
            lat.bounds = op.bounds;
            c.draw_image_lattice(op.pixmap.as_ref(), lat, op.dst_rect, op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawImageNine>) {
            c.draw_image_nine(op.pixmap.as_ref(), op.center, op.dst_rect, op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawAtlas>) {
            c.draw_atlas(op.pixmap.as_ref(), op.xforms, op.tex_rects, op.colors, op.mode, op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawVertices>) {
            c.draw_vertices(op.vertices, op.blend_mode, op.opacity);
        } else if constexpr (std::is_same_v<T, CmdDrawTexturedVertices>) {
            c.draw_textured_vertices(op.vertices, op.texture.as_ref(), op.blend_mode, op.opacity);
        } else if constexpr (std::is_same_v<T, CmdDrawCoonsPatch>) {
            c.draw_coons_patch(op.patch, op.blend_mode, op.opacity, op.su, op.sv);
        } else if constexpr (std::is_same_v<T, CmdDrawGradientMesh>) {
            c.draw_gradient_mesh(op.mesh, op.blend_mode, op.opacity, op.subdivision);
        } else if constexpr (std::is_same_v<T, CmdDrawPathShadow>) {
            c.draw_path_with_shadow(op.path, op.paint, op.shadow, op.rule);
        } else if constexpr (std::is_same_v<T, CmdDrawRectShadow>) {
            c.draw_rect_with_shadow(op.rect, op.paint, op.shadow);
        } else if constexpr (std::is_same_v<T, CmdDrawRoundRectShadow>) {
            c.draw_round_rect_with_shadow(op.rect, op.rx, op.ry, op.paint, op.shadow);
        } else if constexpr (std::is_same_v<T, CmdDrawRRectShadow>) {
            c.draw_rrect_with_shadow(op.rrect, op.paint, op.shadow);
        } else if constexpr (std::is_same_v<T, CmdDrawCircleShadow>) {
            c.draw_circle_with_shadow(op.cx, op.cy, op.radius, op.paint, op.shadow);
        } else if constexpr (std::is_same_v<T, CmdDrawPicture>) {
            if (op.picture) {
                c.draw_picture(
                    *op.picture,
                    op.transform ? &(*op.transform) : nullptr,
                    op.paint ? &(*op.paint) : nullptr
                );
            }
        } else if constexpr (std::is_same_v<T, CmdDrawDrawable>) {
            if (op.drawable) {
                c.draw_drawable(*op.drawable, op.transform ? &(*op.transform) : nullptr);
            }
        } else if constexpr (std::is_same_v<T, CmdDrawGlyphsPos>) {
            if (op.font) {
                c.draw_glyphs(op.glyph_ids, op.positions, *op.font, op.font_size, op.paint);
            }
        } else if constexpr (std::is_same_v<T, CmdDrawGlyphsRSXform>) {
            if (op.font) {
                c.draw_glyphs(op.glyph_ids, op.xforms, *op.font, op.font_size, op.paint);
            }
        } else if constexpr (std::is_same_v<T, CmdDrawTextBlob>) {
            c.draw_text_blob(op.blob, op.x, op.y, op.paint);
        } else if constexpr (std::is_same_v<T, CmdDrawAnnotation>) {
            c.draw_annotation(op.rect, op.key, op.data);
        } else if constexpr (std::is_same_v<T, CmdDiscard>) {
            c.discard();
        }
    }, cmd);
}

} // namespace

struct Picture::Impl {
    Rect cull_rect{};
    std::vector<PictureCmd> commands{};
    size_t extra_bytes{0};
};

Picture::Picture(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Picture::~Picture() = default;

void Picture::playback(ICanvas& canvas) const {
    if (!impl_) return;
    for (const auto& cmd : impl_->commands) {
        replay_cmd(canvas, cmd);
    }
}

Rect Picture::cull_rect() const noexcept {
    return impl_ ? impl_->cull_rect : Rect();
}

size_t Picture::approximate_op_count() const noexcept {
    return impl_ ? impl_->commands.size() : 0;
}

size_t Picture::approximate_bytes_used() const noexcept {
    if (!impl_) return 0;
    return sizeof(Impl) + impl_->commands.size() * sizeof(PictureCmd) + impl_->extra_bytes;
}

// --- ICanvas default implementations for Picture & Drawable ---

void ICanvas::draw_picture(const Picture& picture, const Transform* transform, const Paint* paint) {
    if (picture.is_empty()) return;
    const Rect cull = picture.cull_rect();
    if (!cull.is_empty()) {
        if (transform) {
            if (auto tc = cull.transform(*transform); tc && quick_reject(*tc)) return;
        } else if (quick_reject(cull)) {
            return;
        }
    }
    const int saved = save();
    if (transform) {
        concat(*transform);
    }
    if (paint) {
        if (!cull.is_empty()) {
            save_layer_bounds(cull, *paint);
        } else {
            save_layer(*paint);
        }
    }
    picture.playback(*this);
    restore_to_count(saved);
}

void ICanvas::draw_drawable(Drawable& drawable, const Transform* transform) {
    drawable.draw(*this, transform);
}

// --- Drawable ---

void Drawable::draw(ICanvas& canvas, const Transform* transform) {
    const int saved = canvas.save();
    if (transform) {
        canvas.concat(*transform);
    }
    on_draw(canvas);
    canvas.restore_to_count(saved);
}

Rect Drawable::bounds() const {
    return on_get_bounds();
}

std::shared_ptr<Picture> Drawable::make_picture_snapshot() {
    PictureRecorder rec;
    ICanvas& c = rec.begin_recording(on_get_bounds());
    on_draw(c);
    return rec.finish_recording();
}

// --- PictureRecorder::RecordingCanvas ---

struct PictureRecorder::RecordingCanvas final : public ICanvas {
    struct RecState {
        Transform4x4 transform{};
        std::optional<ScreenIntRect> clip{};
    };

    std::shared_ptr<Picture::Impl> impl{};
    Transform4x4 current_m44{};
    std::optional<ScreenIntRect> clip_bounds{};
    std::vector<RecState> stack{};
    uint32_t rec_w{0};
    uint32_t rec_h{0};

    explicit RecordingCanvas(const Rect& bounds) {
        impl = std::make_shared<Picture::Impl>();
        impl->cull_rect = bounds;
        rec_w = static_cast<uint32_t>(std::max(0.0f, std::ceil(bounds.right())));
        rec_h = static_cast<uint32_t>(std::max(0.0f, std::ceil(bounds.bottom())));
        if (rec_w > 0 && rec_h > 0) {
            clip_bounds = ScreenIntRect::from_xywh(0, 0, rec_w, rec_h);
        }
    }

    [[nodiscard]] CanvasKind canvas_kind() const noexcept override { return CanvasKind::Picture; }
    [[nodiscard]] uint32_t canvas_width() const noexcept override { return rec_w; }
    [[nodiscard]] uint32_t canvas_height() const noexcept override { return rec_h; }

    int save() override {
        const int prev = save_count();
        stack.push_back(RecState{current_m44, clip_bounds});
        impl->commands.emplace_back(CmdSave{});
        return prev;
    }

    [[nodiscard]] int save_count() const noexcept override {
        return static_cast<int>(stack.size()) + 1;
    }

    int save_layer(const Paint& paint) override {
        const int prev = save_count();
        stack.push_back(RecState{current_m44, clip_bounds});
        impl->commands.emplace_back(CmdSaveLayer{paint});
        return prev;
    }

    int save_layer_bounds(const Rect& bounds, const Paint& paint) override {
        const int prev = save_count();
        stack.push_back(RecState{current_m44, clip_bounds});
        impl->commands.emplace_back(CmdSaveLayerBounds{bounds, paint});
        return prev;
    }

    int save_layer_backdrop(std::optional<Rect> bounds, const Paint& paint, float blur_sigma) override {
        const int prev = save_count();
        stack.push_back(RecState{current_m44, clip_bounds});
        impl->commands.emplace_back(CmdSaveLayerBackdrop{bounds, paint, blur_sigma});
        return prev;
    }

    void restore() override {
        if (stack.empty()) return;
        current_m44 = stack.back().transform;
        clip_bounds = stack.back().clip;
        stack.pop_back();
        impl->commands.emplace_back(CmdRestore{});
    }

    void translate(float dx, float dy) override {
        current_m44 = current_m44.pre_translate(dx, dy);
        impl->commands.emplace_back(CmdTranslate{dx, dy});
    }

    void scale(float sx, float sy) override {
        current_m44 = current_m44.pre_scale(sx, sy);
        impl->commands.emplace_back(CmdScale{sx, sy});
    }

    void rotate(float degrees) override {
        current_m44 = current_m44.pre_rotate_z(degrees);
        impl->commands.emplace_back(CmdRotate{degrees});
    }
    using ICanvas::rotate;

    void skew(float kx, float ky) override {
        current_m44 = current_m44 * Transform4x4(Transform::from_skew(kx, ky));
        impl->commands.emplace_back(CmdSkew{kx, ky});
    }

    void concat(const Transform& ts) override {
        current_m44 = current_m44 * Transform4x4(ts);
        impl->commands.emplace_back(CmdConcat{ts});
    }

    void concat(const Transform4x4& ts) override {
        current_m44 = current_m44 * ts;
        impl->commands.emplace_back(CmdConcat4x4{ts});
    }

    void set_transform_4x4(const Transform4x4& ts) override {
        current_m44 = ts;
        impl->commands.emplace_back(CmdSetTransform4x4{ts});
    }

    [[nodiscard]] Transform current_transform() const noexcept override {
        return current_m44.to_transform_2d();
    }

    [[nodiscard]] Transform4x4 current_transform_4x4() const noexcept override {
        return current_m44;
    }

    [[nodiscard]] std::optional<ScreenIntRect> get_device_clip_bounds() const noexcept override {
        return clip_bounds;
    }

    void clip_rect(const Rect& rect, ClipOp op) override {
        impl->commands.emplace_back(CmdClipRect{rect, op});
        if (op == ClipOp::Intersect && clip_bounds.has_value()) {
            if (auto tr = current_m44.map_rect(rect)) {
                const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(tr->left())), 0, static_cast<int32_t>(rec_w));
                const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(tr->top())), 0, static_cast<int32_t>(rec_h));
                const int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(tr->right())), 0, static_cast<int32_t>(rec_w));
                const int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(tr->bottom())), 0, static_cast<int32_t>(rec_h));
                if (x1 > x0 && y1 > y0) {
                    if (auto br = ScreenIntRect::from_xywh(
                            static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                            static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0))) {
                        clip_bounds = clip_bounds->intersect(*br);
                        return;
                    }
                }
            }
            clip_bounds = std::nullopt;
        }
    }

    void clip_rrect(const RRect& rrect, ClipOp op) override {
        impl->commands.emplace_back(CmdClipRRect{rrect, op});
    }

    void clip_oval(const Rect& oval, ClipOp op) override {
        impl->commands.emplace_back(CmdClipOval{oval, op});
    }

    void clip_path(const Path& path, FillRule fill_rule, ClipOp op) override {
        impl->commands.emplace_back(CmdClipPath{path, fill_rule, op});
    }

    void clip_shader(const Shader& shader, ClipOp op) override {
        impl->commands.emplace_back(CmdClipShader{shader, op});
    }

    void clip_region(const Region& region, ClipOp op) override {
        impl->commands.emplace_back(CmdClipRegion{region, op});
    }

    void draw_paint(const Paint& paint) override {
        impl->commands.emplace_back(CmdDrawPaint{paint});
    }

    void draw_color(Color color, BlendMode mode) override {
        impl->commands.emplace_back(CmdDrawColor{color, mode});
    }

    void fill_rect(const Rect& rect, const Paint& paint) override {
        impl->commands.emplace_back(CmdFillRect{rect, paint});
    }

    void stroke_rect(const Rect& rect, const Paint& paint, const Stroke& stroke) override {
        impl->commands.emplace_back(CmdStrokeRect{rect, paint, stroke});
    }

    void fill_round_rect(const Rect& rect, float rx, float ry, const Paint& paint) override {
        impl->commands.emplace_back(CmdFillRoundRect{rect, rx, ry, paint});
    }

    void stroke_round_rect(const Rect& rect, float rx, float ry, const Paint& paint, const Stroke& stroke) override {
        impl->commands.emplace_back(CmdStrokeRoundRect{rect, rx, ry, paint, stroke});
    }

    void fill_rrect(const RRect& rrect, const Paint& paint) override {
        impl->commands.emplace_back(CmdFillRRect{rrect, paint});
    }

    void stroke_rrect(const RRect& rrect, const Paint& paint, const Stroke& stroke) override {
        impl->commands.emplace_back(CmdStrokeRRect{rrect, paint, stroke});
    }

    void fill_oval(const Rect& oval, const Paint& paint) override {
        impl->commands.emplace_back(CmdFillOval{oval, paint});
    }

    void stroke_oval(const Rect& oval, const Paint& paint, const Stroke& stroke) override {
        impl->commands.emplace_back(CmdStrokeOval{oval, paint, stroke});
    }

    void fill_arc(const Rect& oval, float start_deg, float sweep_deg, bool use_center, const Paint& paint) override {
        impl->commands.emplace_back(CmdFillArc{oval, start_deg, sweep_deg, use_center, paint});
    }

    void stroke_arc(const Rect& oval, float start_deg, float sweep_deg, bool use_center, const Paint& paint, const Stroke& stroke) override {
        impl->commands.emplace_back(CmdStrokeArc{oval, start_deg, sweep_deg, use_center, paint, stroke});
    }

    void fill_drrect(const RRect& outer, const RRect& inner, const Paint& paint) override {
        impl->commands.emplace_back(CmdFillDRRect{outer, inner, paint});
    }

    void stroke_drrect(const RRect& outer, const RRect& inner, const Paint& paint, const Stroke& stroke) override {
        impl->commands.emplace_back(CmdStrokeDRRect{outer, inner, paint, stroke});
    }

    void fill_path(const Path& path, const Paint& paint, FillRule fill_rule) override {
        impl->extra_bytes += path.points().size() * sizeof(Point) + path.verbs().size();
        impl->commands.emplace_back(CmdFillPath{path, paint, fill_rule});
    }

    void stroke_path(const Path& path, const Paint& paint, const Stroke& stroke) override {
        impl->extra_bytes += path.points().size() * sizeof(Point) + path.verbs().size();
        impl->commands.emplace_back(CmdStrokePath{path, paint, stroke});
    }

    void draw_points(PointMode mode, std::span<const Point> points, const Paint& paint, const Stroke& stroke) override {
        impl->extra_bytes += points.size() * sizeof(Point);
        impl->commands.emplace_back(CmdDrawPoints{mode, std::vector<Point>(points.begin(), points.end()), paint, stroke});
    }

    void draw_region(const Region& region, const Paint& paint, const Stroke& stroke) override {
        impl->extra_bytes += region.rect_count() * sizeof(IntRect);
        impl->commands.emplace_back(CmdDrawRegion{region, paint, stroke});
    }

    void draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint) override {
        Pixmap copy = clone_pixmap_ref(src);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height();
        impl->commands.emplace_back(CmdDrawPixmap{x, y, std::move(copy), paint});
    }

    void draw_pixmap_rect(const PixmapRef& src, const Rect& src_rect, const Rect& dst_rect, const PixmapPaint& paint) override {
        Pixmap copy = clone_pixmap_ref(src);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height();
        impl->commands.emplace_back(CmdDrawPixmapRect{std::move(copy), src_rect, dst_rect, paint});
    }

    void draw_pixmap_3d(const PixmapRef& src, const Transform4x4& transform3d, float opacity, BlendMode blend_mode) override {
        Pixmap copy = clone_pixmap_ref(src);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height();
        impl->commands.emplace_back(CmdDrawPixmap3D{std::move(copy), transform3d, opacity, blend_mode});
    }

    void draw_image_lattice(const PixmapRef& src, const Lattice& lattice, const Rect& dst_rect, const PixmapPaint& paint) override {
        Pixmap copy = clone_pixmap_ref(src);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height();
        impl->commands.emplace_back(CmdDrawImageLattice{
            std::move(copy),
            std::vector<int32_t>(lattice.x_divs.begin(), lattice.x_divs.end()),
            std::vector<int32_t>(lattice.y_divs.begin(), lattice.y_divs.end()),
            std::vector<Lattice::RectType>(lattice.rect_types.begin(), lattice.rect_types.end()),
            std::vector<Color>(lattice.colors.begin(), lattice.colors.end()),
            lattice.bounds,
            dst_rect,
            paint
        });
    }

    void draw_image_nine(const PixmapRef& src, const IntRect& center, const Rect& dst_rect, const PixmapPaint& paint) override {
        Pixmap copy = clone_pixmap_ref(src);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height();
        impl->commands.emplace_back(CmdDrawImageNine{std::move(copy), center, dst_rect, paint});
    }

    void draw_atlas(
        const PixmapRef& src,
        std::span<const RSXform> xforms,
        std::span<const Rect> tex_rects,
        std::span<const Color> colors,
        BlendMode mode,
        const PixmapPaint& paint
    ) override {
        Pixmap copy = clone_pixmap_ref(src);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height() + xforms.size() * sizeof(RSXform) + tex_rects.size() * sizeof(Rect);
        impl->commands.emplace_back(CmdDrawAtlas{
            std::move(copy),
            std::vector<RSXform>(xforms.begin(), xforms.end()),
            std::vector<Rect>(tex_rects.begin(), tex_rects.end()),
            std::vector<Color>(colors.begin(), colors.end()),
            mode,
            paint
        });
    }

    void draw_vertices(const Vertices& vertices, BlendMode blend_mode, float opacity) override {
        impl->extra_bytes += vertices.positions.size() * sizeof(Point) + vertices.indices.size() * sizeof(uint32_t);
        impl->commands.emplace_back(CmdDrawVertices{vertices, blend_mode, opacity});
    }

    void draw_textured_vertices(const Vertices& vertices, const PixmapRef& texture, BlendMode blend_mode, float opacity) override {
        Pixmap copy = clone_pixmap_ref(texture);
        impl->extra_bytes += static_cast<size_t>(copy.stride_bytes()) * copy.height() + vertices.positions.size() * sizeof(Point);
        impl->commands.emplace_back(CmdDrawTexturedVertices{vertices, std::move(copy), blend_mode, opacity});
    }

    void draw_coons_patch(const CoonsPatch& patch, BlendMode blend_mode, float opacity, uint32_t subdiv_u, uint32_t subdiv_v) override {
        impl->commands.emplace_back(CmdDrawCoonsPatch{patch, blend_mode, opacity, subdiv_u, subdiv_v});
    }

    void draw_gradient_mesh(const GradientMesh& mesh, BlendMode blend_mode, float opacity, uint32_t subdivision) override {
        impl->extra_bytes += mesh.patches.size() * sizeof(CoonsPatch);
        impl->commands.emplace_back(CmdDrawGradientMesh{mesh, blend_mode, opacity, subdivision});
    }

    void draw_path_with_shadow(const Path& path, const Paint& paint, const effects::DropShadow& shadow, FillRule fill_rule) override {
        impl->commands.emplace_back(CmdDrawPathShadow{path, paint, shadow, fill_rule});
    }

    void draw_rect_with_shadow(const Rect& rect, const Paint& paint, const effects::DropShadow& shadow) override {
        impl->commands.emplace_back(CmdDrawRectShadow{rect, paint, shadow});
    }

    void draw_round_rect_with_shadow(const Rect& rect, float rx, float ry, const Paint& paint, const effects::DropShadow& shadow) override {
        impl->commands.emplace_back(CmdDrawRoundRectShadow{rect, rx, ry, paint, shadow});
    }

    void draw_rrect_with_shadow(const RRect& rrect, const Paint& paint, const effects::DropShadow& shadow) override {
        impl->commands.emplace_back(CmdDrawRRectShadow{rrect, paint, shadow});
    }

    void draw_circle_with_shadow(float cx, float cy, float radius, const Paint& paint, const effects::DropShadow& shadow) override {
        impl->commands.emplace_back(CmdDrawCircleShadow{cx, cy, radius, paint, shadow});
    }

    void draw_picture(const Picture& picture, const Transform* transform, const Paint* paint) override {
        impl->commands.emplace_back(CmdDrawPicture{
            std::make_shared<Picture>(picture),
            transform ? std::optional<Transform>(*transform) : std::nullopt,
            paint ? std::optional<Paint>(*paint) : std::nullopt
        });
    }

    void draw_drawable(Drawable& drawable, const Transform* transform) override {
        // Snapshot raw non-owning Drawable immediately so the Picture holds no dangling pointer
        drawable.draw(*this, transform);
    }

    void draw_drawable(const std::shared_ptr<Drawable>& drawable, const Transform* transform) override {
        if (!drawable) return;
        impl->commands.emplace_back(CmdDrawDrawable{
            drawable,
            transform ? std::optional<Transform>(*transform) : std::nullopt
        });
    }

    void draw_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const Point> positions,
        const text::TtfFont& font,
        float font_size,
        const Paint& paint
    ) override {
        const size_t n = std::min(glyph_ids.size(), positions.size());
        if (n == 0) return;
        impl->extra_bytes += n * (sizeof(uint16_t) + sizeof(Point));
        impl->commands.emplace_back(CmdDrawGlyphsPos{
            &font,
            font_size,
            std::vector<uint16_t>(glyph_ids.begin(), glyph_ids.begin() + static_cast<std::ptrdiff_t>(n)),
            std::vector<Point>(positions.begin(), positions.begin() + static_cast<std::ptrdiff_t>(n)),
            paint
        });
    }

    void draw_glyphs(
        std::span<const uint16_t> glyph_ids,
        std::span<const RSXform> xforms,
        const text::TtfFont& font,
        float font_size,
        const Paint& paint
    ) override {
        const size_t n = std::min(glyph_ids.size(), xforms.size());
        if (n == 0) return;
        impl->extra_bytes += n * (sizeof(uint16_t) + sizeof(RSXform));
        impl->commands.emplace_back(CmdDrawGlyphsRSXform{
            &font,
            font_size,
            std::vector<uint16_t>(glyph_ids.begin(), glyph_ids.begin() + static_cast<std::ptrdiff_t>(n)),
            std::vector<RSXform>(xforms.begin(), xforms.begin() + static_cast<std::ptrdiff_t>(n)),
            paint
        });
    }

    void draw_text_blob(
        const TextBlob& blob,
        float x,
        float y,
        const Paint& paint
    ) override {
        if (blob.is_empty()) return;
        impl->extra_bytes += blob.glyph_count() * (sizeof(uint16_t) + sizeof(Point));
        impl->commands.emplace_back(CmdDrawTextBlob{blob, x, y, paint});
    }

    void draw_annotation(
        const Rect& rect,
        std::string_view key,
        std::span<const uint8_t> data
    ) override {
        impl->extra_bytes += key.size() + data.size();
        impl->commands.emplace_back(CmdDrawAnnotation{
            rect,
            std::string(key),
            std::vector<uint8_t>(data.begin(), data.end())
        });
    }

    void discard() override {
        impl->commands.emplace_back(CmdDiscard{});
    }
};

PictureRecorder::PictureRecorder() = default;
PictureRecorder::~PictureRecorder() = default;
PictureRecorder::PictureRecorder(PictureRecorder&&) noexcept = default;
PictureRecorder& PictureRecorder::operator=(PictureRecorder&&) noexcept = default;

ICanvas& PictureRecorder::begin_recording(const Rect& bounds) {
    recorder_ = std::make_unique<RecordingCanvas>(bounds);
    return *recorder_;
}

ICanvas* PictureRecorder::recording_canvas() noexcept {
    return recorder_.get();
}

std::shared_ptr<Picture> PictureRecorder::finish_recording() {
    if (!recorder_) {
        return std::make_shared<Picture>(std::make_shared<Picture::Impl>());
    }
    auto impl = std::move(recorder_->impl);
    recorder_.reset();
    return std::make_shared<Picture>(std::move(impl));
}

} // namespace nisaba

