#pragma once

#include <memory>
#include <span>
#include <vector>
#include <optional>
#include "nisaba/gpu/types.hpp"
#include "nisaba/gpu/context.hpp"
#include "nisaba/gpu/gpu_types.hpp"
#include "nisaba/gpu/gpu_surface.hpp"
#include "nisaba/canvas/paint.hpp"
#include "nisaba/path/path.hpp"
#include "nisaba/path/stroker.hpp"
#include "nisaba/mesh/mesh.hpp"
#include "nisaba/effects/glass.hpp"
#include "nisaba/effects/drop_shadow.hpp"
#include "nisaba/svg/svg_document.hpp"
#include "nisaba/canvas/canvas_interface.hpp"
#include "nisaba/text/font_system.hpp"
#include "nisaba/text/buffer.hpp"
#include "nisaba/text/ttf_font.hpp"

namespace nisaba::gpu {

struct GpuCanvasState {
    nisaba::Transform transform{};
    std::optional<ScreenIntRect> scissor_clip{};
    int stencil_clip_depth{0};
    bool is_layer{false};
};

/// High-level, hardware-accelerated GPU Canvas bridging Nisaba 2D to the sovereign C++20 GPU engine.
class GpuCanvas : public ICanvas {
public:
    explicit GpuCanvas(std::shared_ptr<GpuSurface> surface);
    ~GpuCanvas();

    [[nodiscard]] CanvasKind canvas_kind() const noexcept override { return CanvasKind::Gpu; }
    [[nodiscard]] uint32_t width() const noexcept { return surface_ ? surface_->width() : 0; }
    [[nodiscard]] uint32_t height() const noexcept { return surface_ ? surface_->height() : 0; }
    [[nodiscard]] uint32_t canvas_width() const noexcept override { return surface_ ? surface_->width() : 0; }
    [[nodiscard]] uint32_t canvas_height() const noexcept override { return surface_ ? surface_->height() : 0; }
    [[nodiscard]] std::shared_ptr<GpuSurface> surface() const noexcept { return surface_; }
    [[nodiscard]] Context* context() const noexcept { return context_.get(); }

    // Surface operations
    void clear(nisaba::Color color = nisaba::Color::TRANSPARENT);
    bool resize(uint32_t width, uint32_t height);

    // State stack
    void save() override;
    void save_layer(const nisaba::Paint& paint = nisaba::Paint(nisaba::Color::WHITE)) override;
    void save_layer_bounds(const nisaba::Rect& bounds, const nisaba::Paint& paint = nisaba::Paint(nisaba::Color::WHITE)) override;
    void save_layer_backdrop(
        std::optional<nisaba::Rect> bounds = std::nullopt,
        const nisaba::Paint& paint = nisaba::Paint(nisaba::Color::WHITE),
        float blur_sigma = 0.0f
    ) override;
    void restore() override;
    /// Invalidates cached GPU/GL state when sharing the underlying context with an external embedder.
    void reset_state() noexcept;
    void reset_transform() noexcept;
    void set_transform(const nisaba::Transform& ts) noexcept;
    [[nodiscard]] const nisaba::Transform& transform() const noexcept { return current_transform_; }
    [[nodiscard]] nisaba::Transform current_transform() const noexcept override { return current_transform_; }

    void translate(float dx, float dy) noexcept override;
    void scale(float sx, float sy) noexcept override;
    void rotate(float degrees) noexcept override;
    void skew(float kx, float ky) noexcept override;
    void concat(const nisaba::Transform& ts) noexcept override;

    // Clipping
    void clip_rect(const nisaba::Rect& rect) override;
    void clip_rect(const nisaba::ScreenIntRect& rect);
    void clip_path(const nisaba::Path& path, nisaba::FillRule fill_rule = nisaba::FillRule::Winding) override;
    void reset_clip() noexcept;
    [[nodiscard]] const std::optional<ScreenIntRect>& scissor_clip() const noexcept { return scissor_clip_; }
    [[nodiscard]] int stencil_clip_depth() const noexcept { return current_stencil_depth_; }

    // Fast localized clears for damage tracking
    void clear_rect(const nisaba::Rect& rect, nisaba::Color color);
    void clear_rect(const nisaba::ScreenIntRect& rect, nisaba::Color color);

    // Drawing primitives
    void fill_rect(const nisaba::Rect& rect, const nisaba::Paint& paint) override;
    void stroke_rect(const nisaba::Rect& rect, const nisaba::Paint& paint, const nisaba::Stroke& stroke) override;

    void fill_round_rect(const nisaba::Rect& rect, float rx, float ry, const nisaba::Paint& paint) override;
    void stroke_round_rect(const nisaba::Rect& rect, float rx, float ry, const nisaba::Paint& paint, const nisaba::Stroke& stroke) override;

    void fill_rrect(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const nisaba::Paint& paint);
    void stroke_rrect(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const nisaba::Paint& paint, const nisaba::Stroke& stroke);
    void fill_rrect(const nisaba::RRect& rrect, const nisaba::Paint& paint) override;
    void stroke_rrect(const nisaba::RRect& rrect, const nisaba::Paint& paint, const nisaba::Stroke& stroke) override;
    void clip_rrect(const nisaba::RRect& rrect) override;
    void clip_rrect(const nisaba::Rect& rect, const nisaba::BorderRadius& radii);

    void fill_oval(const nisaba::Rect& oval, const nisaba::Paint& paint) override;
    void stroke_oval(const nisaba::Rect& oval, const nisaba::Paint& paint, const nisaba::Stroke& stroke) override;
    void clip_oval(const nisaba::Rect& oval) override;

    void fill_drrect(const nisaba::RRect& outer, const nisaba::RRect& inner, const nisaba::Paint& paint) override;
    void stroke_drrect(const nisaba::RRect& outer, const nisaba::RRect& inner, const nisaba::Paint& paint, const nisaba::Stroke& stroke) override;

    void draw_points(
        nisaba::PointMode mode,
        std::span<const nisaba::Point> points,
        const nisaba::Paint& paint,
        const nisaba::Stroke& stroke = nisaba::Stroke{}
    ) override;

    void fill_circle(float cx, float cy, float radius, const nisaba::Paint& paint);
    void stroke_circle(float cx, float cy, float radius, const nisaba::Paint& paint, const nisaba::Stroke& stroke);

    void stroke_line(nisaba::Point p0, nisaba::Point p1, const nisaba::Paint& paint, const nisaba::Stroke& stroke);
    void stroke_line(float x0, float y0, float x1, float y1, const nisaba::Paint& paint, const nisaba::Stroke& stroke);

    void fill_path(const nisaba::Path& path, const nisaba::Paint& paint, nisaba::FillRule fill_rule = nisaba::FillRule::Winding) override;
    void stroke_path(const nisaba::Path& path, const nisaba::Paint& paint, const nisaba::Stroke& stroke) override;

    void draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint = PixmapPaint()) override;
    void draw_pixmap_rect(const PixmapRef& src, const nisaba::Rect& src_rect, const nisaba::Rect& dst_rect, const PixmapPaint& paint = PixmapPaint()) override;
    using ICanvas::draw_pixmap_rect;
    void draw_pixmap_3d(
        const PixmapRef& src,
        const nisaba::Rect& src_rect,
        const Transform4x4& transform3d,
        float opacity = 1.0f,
        BlendMode blend_mode = BlendMode::SourceOver
    );
    void draw_pixmap_3d(
        const PixmapRef& src,
        const Transform4x4& transform3d,
        float opacity = 1.0f,
        BlendMode blend_mode = BlendMode::SourceOver
    ) override;
    void draw_gpu_texture(int32_t x, int32_t y, const std::shared_ptr<GpuTexture>& texture, const PixmapPaint& paint = PixmapPaint());
    void draw_gpu_texture_rect(const std::shared_ptr<GpuTexture>& texture, const nisaba::Rect& src_rect, const nisaba::Rect& dst_rect, const PixmapPaint& paint = PixmapPaint());
    void draw_gpu_texture_rect(const std::shared_ptr<GpuTexture>& texture, const nisaba::Rect& dst_rect, const PixmapPaint& paint = PixmapPaint());

    void draw_vertices(const nisaba::Vertices& vertices, BlendMode blend_mode = BlendMode::SourceOver, float opacity = 1.0f);
    void draw_textured_vertices(const nisaba::Vertices& vertices, const std::shared_ptr<GpuTexture>& texture, BlendMode blend_mode = BlendMode::SourceOver, float opacity = 1.0f);
    void draw_textured_vertices(const nisaba::Vertices& vertices, const PixmapRef& texture, BlendMode blend_mode = BlendMode::SourceOver, float opacity = 1.0f);

    // --- Effects & Glassmorphism ---
    void draw_round_rect_shadow(const nisaba::Rect& rect, float rx, float ry, const effects::DropShadow& shadow);
    void draw_rrect_shadow(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const effects::DropShadow& shadow);
    void draw_rrect_shadow(const nisaba::RRect& rrect, const effects::DropShadow& shadow);
    void draw_rect_shadow(const nisaba::Rect& rect, const effects::DropShadow& shadow);
    void draw_circle_shadow(float cx, float cy, float radius, const effects::DropShadow& shadow);
    void draw_path_shadow(const nisaba::Path& path, const effects::DropShadow& shadow, nisaba::FillRule fill_rule = nisaba::FillRule::Winding);
    void draw_path_with_shadow(const nisaba::Path& path, const nisaba::Paint& paint, const effects::DropShadow& shadow, nisaba::FillRule fill_rule = nisaba::FillRule::Winding) override;
    void draw_rect_with_shadow(const nisaba::Rect& rect, const nisaba::Paint& paint, const effects::DropShadow& shadow) override;
    void draw_round_rect_with_shadow(const nisaba::Rect& rect, float rx, float ry, const nisaba::Paint& paint, const effects::DropShadow& shadow) override;
    void draw_rrect_with_shadow(const nisaba::RRect& rrect, const nisaba::Paint& paint, const effects::DropShadow& shadow) override;
    void draw_circle_with_shadow(float cx, float cy, float radius, const nisaba::Paint& paint, const effects::DropShadow& shadow) override;
    void draw_glow(const nisaba::Rect& rect, float rx, float ry, nisaba::Color glow_color, float radius);
    void draw_glass_panel(const nisaba::Rect& rect, float rx, float ry, const effects::GlassParams& params);
    void draw_glass_panel(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const effects::GlassParams& params);
    void draw_glass_panel(const nisaba::RRect& rrect, const effects::GlassParams& params);

    // --- Hardware GPU Backdrop Blur ---
    void draw_backdrop_blur(const nisaba::Rect& rect, float sigmaX, float sigmaY = 0.0f);
    void draw_backdrop_blur(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, float sigma);
    void draw_backdrop_blur(const nisaba::RRect& rrect, float sigma);
    void invalidate_backdrop_cache() noexcept;

    // --- SVG Vector Graphics ---
    void draw_svg(const svg::SvgDocument& doc, const nisaba::Rect& dest_bounds);
    void draw_svg(const svg::SvgDocument& doc, const nisaba::Rect& dest_bounds, std::optional<nisaba::Color> tint);
    void draw_svg(const svg::SvgDocument& doc, const nisaba::Rect& dest_bounds, const svg::SvgTheme& theme);

    // --- Typography & Hardware Text Rendering ---
    void draw_text(std::string_view text, float x, float y, const text::Font& font, const nisaba::Paint& paint, float font_size = 16.0f);
    void draw_text(float x, float y, std::string_view text, const nisaba::Paint& paint, float font_size = 16.0f, const char* font_name = nullptr);
    void draw_text_buffer(const text::Buffer& buffer, text::FontSystem& fonts, Point pos = Point{0.0f, 0.0f}, nisaba::Color default_color = nisaba::Color::BLACK);
    void draw_text_buffer(const text::Buffer& buffer, text::FontSystem& fonts, Point pos, const nisaba::Paint& paint);

    int create_font(const char* name, const char* filename);
    int create_font_mem(const char* name, const uint8_t* data, size_t size);
    [[nodiscard]] text::FontSystem* font_system() noexcept { return context_ ? &context_->fontSystem() : nullptr; }
    [[nodiscard]] const text::FontSystem* font_system() const noexcept { return context_ ? &context_->fontSystem() : nullptr; }

    /// Flushes all pending draw operations to the GPU.
    void flush();
    void flush(const ScreenIntRect& damage_rect);
    void flush(const std::vector<ScreenIntRect>& damage_rects);

private:
    std::shared_ptr<GpuSurface> surface_;
    std::unique_ptr<Context> context_;
    bool in_frame_{false};
    bool needs_surface_bind_{false};

    nisaba::Transform current_transform_{};
    std::optional<ScreenIntRect> scissor_clip_{};
    int current_stencil_depth_{0};
    std::vector<GpuCanvasState> state_stack_{};
    std::vector<int> temp_images_{};
    std::unique_ptr<class DualKawaseBlur> blur_engine_{nullptr};

    struct BackdropCache {
        uint32_t texture_id{0};
        int img_handle{0};
        float sigma{0.0f};
        bool valid{false};
    } backdrop_cache_{};

    struct GradLutEntry {
        uint64_t key{0};
        int img_id{0};
    };
    std::vector<GradLutEntry> grad_lut_cache_{};

    struct PixmapTexCacheEntry {
        const void* data_ptr{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
        PixelFormat format{PixelFormat::RGBA8888};
        int flags{0};
        uint64_t content_hash{0};
        uint64_t last_used_frame{0};
        uint64_t last_use_seq{0};
        size_t byte_size{0};
        int img_id{0};
    };
    std::vector<PixmapTexCacheEntry> pixmap_tex_cache_{};
    size_t pixmap_cache_bytes_{0};
    uint64_t frame_counter_{1};
    uint64_t pixmap_use_seq_{0};

    [[nodiscard]] int get_or_upload_pixmap(const PixmapRef& src, int flags);
    [[nodiscard]] int get_or_create_grad_lut(const nisaba::Gradient& grad);
    void populate_gradient_stops_and_xform(const nisaba::Gradient& grad, Paint& gp);

    [[nodiscard]] Paint build_gpu_paint(const nisaba::Paint& paint);
    void apply_fill_paint(const nisaba::Paint& paint);
    void apply_stroke(const nisaba::Stroke& stroke, const nisaba::Paint& paint);
    void push_layer_internal(
        std::optional<nisaba::Rect> bounds,
        const nisaba::Paint& paint,
        bool with_backdrop,
        float blur_sigma,
        std::optional<effects::MaskFilter> mask_filter = std::nullopt,
        nisaba::Color mask_tint = nisaba::Color::WHITE
    );
    template <typename DrawFn>
    void draw_with_image_filter(
        const std::optional<nisaba::Rect>& local_bounds,
        const nisaba::Paint& paint,
        float extra_margin,
        DrawFn&& draw_fn
    );
    void ensure_frame();
    void flush_internal(const ScreenIntRect* damage_rect, const std::vector<ScreenIntRect>* damage_rects);
};

} // namespace nisaba::gpu
