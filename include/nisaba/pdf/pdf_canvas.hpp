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
    void save() override;
    void restore() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void concat(const Transform& ts) override;

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

    // --- Resource & Stream Accessors ---
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return height_; }
    [[nodiscard]] const std::string& content_stream() const noexcept { return stream_; }

    const std::vector<PdfFontResource>& font_resources() const noexcept { return fonts_; }
    const std::vector<PdfImageResource>& image_resources() const noexcept { return images_; }
    const std::map<int, std::string>& extgstate_resources() const noexcept { return extgstates_; }

private:
    void apply_fill_paint(const Paint& paint);
    void apply_stroke_paint(const Paint& paint, const Stroke& stroke);
    void emit_path_commands(const Path& path);
    std::string get_or_add_font(std::string_view font_name);
    std::string get_or_add_extgstate(float alpha);

    float width_;
    float height_;
    std::string stream_{};

    std::vector<PdfFontResource> fonts_{};
    std::vector<PdfImageResource> images_{};
    std::map<int, std::string> extgstates_{}; // alpha (0-1000) -> resource name

    Color current_fill_color_{Color::BLACK};
    Color current_stroke_color_{Color::BLACK};
    float current_stroke_width_{1.0f};
};

} // namespace nisaba::pdf
