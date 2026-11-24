#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "nisaba/canvas/canvas_interface.hpp"
#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/pdf/pdf_types.hpp"

namespace nisaba::pdf {

class PdfWriter;

struct PdfImageResource {
    std::string name;
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> data{};
};

struct PdfFontResource {
    std::string resource_name; // e.g. /F1
    std::string base_font;     // e.g. Helvetica, Helvetica-Bold
};

struct PdfAnnotation {
    Rect rect{};
    std::string key{};
    std::vector<uint8_t> data{};
};

/// Vector graphics canvas that translates all 2D drawing operations directly
/// into native ISO 32000-1 PDF content streams and resources.
/// Implements Nisaba's sovereign ICanvas interface.
class PdfCanvas : public ICanvas {
public:
    PdfCanvas(float page_width, float page_height);
    ~PdfCanvas() override = default;

    // --- ICanvas Interface ---
    [[nodiscard]] CanvasKind canvas_kind() const noexcept override { return CanvasKind::Pdf; }
    [[nodiscard]] uint32_t canvas_width() const noexcept override { return static_cast<uint32_t>(width_); }
    [[nodiscard]] uint32_t canvas_height() const noexcept override { return static_cast<uint32_t>(height_); }
    int save() override;
    [[nodiscard]] int save_count() const noexcept override;
    void restore_to_count(int count) override;
    void restore() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float degrees) override;
    using ICanvas::rotate;
    void concat(const Transform& ts) override;
    void concat(const Transform4x4& ts) override;
    void set_transform_4x4(const Transform4x4& ts) override;
    [[nodiscard]] Transform current_transform() const noexcept override { return current_transform_; }
    [[nodiscard]] Transform4x4 current_transform_4x4() const noexcept override { return current_transform_4x4_; }
    [[nodiscard]] std::optional<ScreenIntRect> get_device_clip_bounds() const noexcept override { return clip_bounds_; }
    void clip_rect(const Rect& rect, ClipOp op = ClipOp::Intersect) override;
    void clip_path(const Path& path, FillRule fill_rule = FillRule::Winding, ClipOp op = ClipOp::Intersect) override;

    void fill_rect(const Rect& rect, const Paint& paint) override;
    void fill_path(const Path& path, const Paint& paint, FillRule fill_rule = FillRule::Winding) override;
    void stroke_path(const Path& path, const Paint& paint, const Stroke& stroke) override;

    // --- Extended Vector PDF Operations ---
    void stroke_rect(const Rect& rect, const Paint& paint, const Stroke& stroke) override;
    void draw_line(Point p1, Point p2, const Paint& paint, const Stroke& stroke);
    void draw_circle(Point center, float radius, const Paint& paint);
    void stroke_circle(Point center, float radius, const Paint& paint, const Stroke& stroke);

    /// Vector text rendering using standard Type 1 PDF fonts.
    /// Default standard font is "Helvetica".
    void draw_text(
        std::string_view text,
        float x,
        float y,
        float font_size,
        Color color,
        std::string_view font_name = "Helvetica"
    );

    /// Embeds a raster Pixmap as an XObject and renders it at the destination rectangle.
    void draw_pixmap(const Pixmap& pixmap, const Rect& dest_rect);
    void draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint = PixmapPaint()) override;
    void draw_pixmap_rect(
        const PixmapRef& src,
        const Rect& src_rect,
        const Rect& dst_rect,
        const PixmapPaint& paint = PixmapPaint()
    ) override;
    using ICanvas::draw_pixmap_rect;
    void draw_vertices(
        const Vertices& vertices,
        BlendMode blend_mode = BlendMode::SourceOver,
        float opacity = 1.0f
    ) override;

    /// Records a PDF link, named destination, or metadata annotation in page coordinates.
    void draw_annotation(
        const Rect& rect,
        std::string_view key,
        std::span<const uint8_t> data
    ) override;
    using ICanvas::draw_annotation;

    // --- Resource & Stream Accessors ---
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return height_; }
    [[nodiscard]] const std::string& content_stream() const noexcept { return stream_; }

    const std::vector<PdfFontResource>& font_resources() const noexcept { return fonts_; }
    const std::vector<PdfImageResource>& image_resources() const noexcept { return images_; }
    const std::map<int, std::string>& extgstate_resources() const noexcept { return extgstates_; }
    const std::vector<PdfAnnotation>& annotations() const noexcept { return annotations_; }

private:
    void apply_fill_paint(const Paint& paint);
    void apply_stroke_paint(const Paint& paint, const Stroke& stroke);
    void emit_path_commands(const Path& path);
    std::string get_or_add_font(std::string_view font_name);
    std::string get_or_add_extgstate(float alpha);

    struct PdfState {
        Transform transform{};
        Transform4x4 transform_4x4{};
        std::optional<ScreenIntRect> clip_bounds{};
    };

    float width_;
    float height_;
    std::string stream_{};
    Transform current_transform_{};
    Transform4x4 current_transform_4x4_{};
    std::optional<ScreenIntRect> clip_bounds_{};
    std::vector<PdfState> state_stack_{};

    std::vector<PdfFontResource> fonts_{};
    std::vector<PdfImageResource> images_{};
    std::map<int, std::string> extgstates_{}; // alpha (0-1000) -> resource name
    std::vector<PdfAnnotation> annotations_{};

    Color current_fill_color_{Color::BLACK};
    Color current_stroke_color_{Color::BLACK};
    float current_stroke_width_{1.0f};
};

} // namespace nisaba::pdf
