#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/gpu/gpu_blur.hpp"
#include "nisaba/path/path_builder.hpp"
#include "gl_headers.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace nisaba::gpu {

namespace {

struct ScopedBlendMode {
    Context* ctx{nullptr};
    CompositeOperationState prev{};
    bool modified{false};

    ScopedBlendMode(Context* context, BlendMode mode) : ctx(context) {
        if (!ctx) return;
        prev = ctx->currentCompositeState();
        CompositeOperationState next = CompositeOperationState::fromBlendMode(mode);
        if (prev.blendMode != next.blendMode ||
            prev.isProgrammableBlend != next.isProgrammableBlend ||
            prev.srcRGB != next.srcRGB || prev.dstRGB != next.dstRGB ||
            prev.srcAlpha != next.srcAlpha || prev.dstAlpha != next.dstAlpha) {
            ctx->setCompositeState(next);
            modified = true;
        }
    }

    ~ScopedBlendMode() {
        if (ctx && modified) {
            ctx->setCompositeState(prev);
        }
    }
};

void draw_image_rect_internal(
    Context* ctx,
    int img,
    float tex_w,
    float tex_h,
    const nisaba::Rect& src_rect,
    const nisaba::Rect& dst_rect,
    const PixmapPaint& paint
) {
    if (!ctx || img <= 0 || tex_w <= 0.0f || tex_h <= 0.0f) return;
    if (src_rect.width() <= 0.0f || src_rect.height() <= 0.0f ||
        dst_rect.width() <= 0.0f || dst_rect.height() <= 0.0f) return;

    float u0 = src_rect.left() / tex_w;
    float v0 = src_rect.top() / tex_h;
    float u1 = src_rect.right() / tex_w;
    float v1 = src_rect.bottom() / tex_h;

    float x0 = dst_rect.left();
    float y0 = dst_rect.top();
    float x1 = dst_rect.right();
    float y1 = dst_rect.bottom();

    Paint p;
    p.image = img;
    Color tint = paint.tint;
    float alpha = std::clamp(paint.opacity, 0.0f, 1.0f);
    p.innerColor = tint.withAlpha(tint.a * alpha);
    p.outerColor = p.innerColor;

    constexpr uint32_t kWhite = 0xffffffffu;
    Vertex quad[6] = {
        Vertex(x0, y0, u0, v0, kWhite),
        Vertex(x0, y1, u0, v1, kWhite),
        Vertex(x1, y1, u1, v1, kWhite),
        Vertex(x0, y0, u0, v0, kWhite),
        Vertex(x1, y1, u1, v1, kWhite),
        Vertex(x1, y0, u1, v0, kWhite)
    };

    ScopedBlendMode scopedBlend(ctx, paint.blend_mode);
    ctx->drawTriangles(p, quad, 6, 0.0f, 3 /* ShaderImg */);
}

void draw_vertices_internal(
    Context* ctx,
    const nisaba::Vertices& vertices,
    int img,
    float tex_w,
    float tex_h,
    BlendMode blend_mode,
    float opacity
) {
    if (!ctx || !vertices.is_valid()) return;
    const size_t tri_count = vertices.triangle_count();
    if (tri_count == 0) return;

    const auto& pos = vertices.positions;
    const auto& cols = vertices.colors;
    const auto& uvs = vertices.tex_coords;
    const size_t npos = pos.size();
    const float alpha = std::clamp(opacity, 0.0f, 1.0f);
    const bool textured = (img > 0 && tex_w > 0.0f && tex_h > 0.0f);

    std::vector<Vertex> gpu_verts;
    gpu_verts.reserve(tri_count * 3);

    for (size_t t = 0; t < tri_count; ++t) {
        auto [i0, i1, i2] = vertices.get_triangle_indices(t);
        if (i0 >= npos || i1 >= npos || i2 >= npos) continue;

        const size_t idx[3] = {i0, i1, i2};
        for (size_t k = 0; k < 3; ++k) {
            size_t vi = idx[k];
            Color vc = (!cols.empty() && vi < cols.size())
                ? Color(cols[vi])
                : Color(1.0f, 1.0f, 1.0f, 1.0f);
            uint32_t packed = vc.withAlpha(vc.a * alpha).premultiplied().toRGBA8();

            float u = 0.5f;
            float v = 1.0f;
            if (textured) {
                if (!uvs.empty() && vi < uvs.size()) {
                    u = uvs[vi].x / tex_w;
                    v = uvs[vi].y / tex_h;
                } else {
                    v = 0.5f;
                }
            }
            gpu_verts.emplace_back(pos[vi].x, pos[vi].y, u, v, packed);
        }
    }

    if (gpu_verts.empty()) return;

    Paint p;
    p.innerColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
    p.outerColor = p.innerColor;
    int shaderType = 4; // ShaderSolid
    if (textured) {
        p.image = img;
        shaderType = 3; // ShaderImg
    }

    ScopedBlendMode scopedBlend(ctx, blend_mode);
    ctx->drawTriangles(p, gpu_verts.data(), static_cast<int>(gpu_verts.size()), 0.0f, shaderType);
}

template <typename TargetT>
void apply_color_effect_to(const nisaba::Paint& paint, TargetT& dst) noexcept {
    if (auto cf = paint.effective_color_effect()) {
        if (cf->has_matrix) {
            dst.hasColorMatrix = true;
            dst.colorMatrix = cf->matrix.m;
        }
        dst.colorTransfer = static_cast<uint8_t>(cf->transfer);
        if (cf->has_blend) {
            dst.hasColorBlend = true;
            dst.colorBlendColor = Color(cf->blend_color);
            dst.colorBlendMode = cf->blend_mode;
        }
    }
}

[[nodiscard]] inline float stroke_filter_margin(const nisaba::Stroke& stroke) noexcept {
    return std::max(stroke.width, stroke.width * std::max(1.0f, stroke.miter_limit) * 0.5f) + 2.0f;
}

[[nodiscard]] inline uint64_t mix_hash64(uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

[[nodiscard]] uint64_t hash_pixmap_content(const PixmapRef& src) noexcept {
    const uint8_t* base = static_cast<const uint8_t*>(src.data());
    const uint32_t w = src.width();
    const uint32_t h = src.height();
    const size_t stride = src.stride_bytes();
    const size_t bpp = bytes_per_pixel(src.format());
    const size_t row_bytes = static_cast<size_t>(w) * bpp;
    uint64_t hash = 0xcbf29ce484222325ULL;
    hash = mix_hash64(hash, (static_cast<uint64_t>(w) << 32) | h);
    hash = mix_hash64(hash, (static_cast<uint64_t>(stride) << 8) | static_cast<uint64_t>(src.format()));
    if (!base || row_bytes == 0 || h == 0) return hash;

    const size_t total_bytes = row_bytes * static_cast<size_t>(h);
    if (total_bytes <= 16384) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * stride;
            size_t x = 0;
            for (; x + 8 <= row_bytes; x += 8) {
                uint64_t word = 0;
                std::memcpy(&word, row + x, 8);
                hash = mix_hash64(hash, word);
            }
            if (x < row_bytes) {
                uint64_t tail = 0;
                std::memcpy(&tail, row + x, row_bytes - x);
                hash = mix_hash64(hash, tail);
            }
        }
        return hash;
    }

    auto sample_at = [&](uint32_t sx, uint32_t sy) noexcept {
        const uint8_t* row = base + static_cast<size_t>(sy) * stride;
        const size_t off = std::min(static_cast<size_t>(sx) * bpp, (row_bytes >= 8) ? (row_bytes - 8) : 0);
        const size_t n = std::min<size_t>(8, row_bytes - off);
        uint64_t word = 0;
        std::memcpy(&word, row + off, n);
        hash = mix_hash64(hash, word);
    };

    // 8x8 strided grid + corners + center (~68 samples, ~80ns)
    for (uint32_t gy = 0; gy < 8; ++gy) {
        const uint32_t sy = (gy * (h - 1)) / 7;
        for (uint32_t gx = 0; gx < 8; ++gx) {
            const uint32_t sx = (gx * (w - 1)) / 7;
            sample_at(sx, sy);
        }
    }
    sample_at(w / 2, h / 2);
    sample_at(0, 0);
    sample_at(w - 1, 0);
    sample_at(0, h - 1);
    sample_at(w - 1, h - 1);
    return hash;
}

[[nodiscard]] inline std::optional<nisaba::Color> try_fold_solid_color_filter(const nisaba::Paint& paint) noexcept {
    if (!paint.is_solid_color() || !paint.has_color_filter()) return std::nullopt;
    auto eff = paint.effective_color_effect();
    if (!eff.has_value()) return std::nullopt;
    const PremultipliedColor pm = eff->transform_premul(paint.shader.solid_color().premultiply());
    if (pm.alpha() > 1e-4f || (pm.red() == 0.0f && pm.green() == 0.0f && pm.blue() == 0.0f)) {
        return pm.demultiply();
    }
    return std::nullopt;
}

} // namespace

GpuCanvas::GpuCanvas(std::shared_ptr<GpuSurface> surface)
    : surface_(std::move(surface)),
      scissor_clip_(surface_ ? ScreenIntRect::from_xywh(0, 0, surface_->width(), surface_->height()) : std::nullopt) {
    if (surface_) {
        surface_->bind();
        context_ = surface_->create_context(CreateFlags::Antialias | CreateFlags::StencilStrokes);
        if (context_) {
            context_->beginFrame(static_cast<float>(surface_->width()), static_cast<float>(surface_->height()), 1.0f);
            in_frame_ = true;
        }
    }
}

GpuCanvas::~GpuCanvas() {
    flush();
    if (context_) {
        for (const auto& entry : grad_lut_cache_) {
            if (entry.img_id > 0) context_->deleteImage(entry.img_id);
        }
        for (const auto& entry : pixmap_tex_cache_) {
            if (entry.img_id > 0) context_->deleteImage(entry.img_id);
        }
    }
    grad_lut_cache_.clear();
    pixmap_tex_cache_.clear();
    pixmap_cache_bytes_ = 0;
    context_.reset();
}

int GpuCanvas::get_or_upload_pixmap(const PixmapRef& src, int flags) {
    if (!context_ || !src.data() || src.width() == 0 || src.height() == 0) return 0;

    const void* ptr = src.data();
    const uint32_t w = src.width();
    const uint32_t h = src.height();
    const uint32_t stride = static_cast<uint32_t>(src.stride_bytes());
    const PixelFormat fmt = src.format();
    const uint64_t hash = hash_pixmap_content(src);

    for (auto& entry : pixmap_tex_cache_) {
        if (entry.data_ptr == ptr && entry.width == w && entry.height == h &&
            entry.stride == stride && entry.format == fmt && entry.flags == flags) {
            if (entry.content_hash == hash && entry.img_id > 0) {
                entry.last_used_frame = frame_counter_;
                entry.last_use_seq = ++pixmap_use_seq_;
                return entry.img_id;
            }
            // Same address, mutated pixels: replace safely
            if (entry.img_id > 0) {
                if (in_frame_ && entry.last_used_frame == frame_counter_) {
                    temp_images_.push_back(entry.img_id);
                } else {
                    context_->deleteImage(entry.img_id);
                }
            }
            int new_img = context_->createImageWithFormat(
                static_cast<int>(w), static_cast<int>(h), static_cast<int>(stride),
                fmt, flags, static_cast<const unsigned char*>(ptr));
            entry.img_id = new_img;
            entry.content_hash = hash;
            entry.last_used_frame = frame_counter_;
            entry.last_use_seq = ++pixmap_use_seq_;
            return new_img;
        }
    }

    const size_t byte_size = static_cast<size_t>(w) * static_cast<size_t>(h) * bytes_per_pixel(fmt);
    constexpr size_t kMaxEntries = 128;
    constexpr size_t kMaxBytes = 64ULL * 1024ULL * 1024ULL;

    int img = context_->createImageWithFormat(
        static_cast<int>(w), static_cast<int>(h), static_cast<int>(stride),
        fmt, flags, static_cast<const unsigned char*>(ptr));
    if (img <= 0) return 0;

    // Do not cache huge single textures (> 16MB); let them behave as transient
    if (byte_size > 16ULL * 1024ULL * 1024ULL) {
        temp_images_.push_back(img);
        return img;
    }

    while (!pixmap_tex_cache_.empty() &&
           (pixmap_tex_cache_.size() >= kMaxEntries || pixmap_cache_bytes_ + byte_size > kMaxBytes)) {
        size_t victim = 0;
        for (size_t i = 1; i < pixmap_tex_cache_.size(); ++i) {
            if (pixmap_tex_cache_[i].last_use_seq < pixmap_tex_cache_[victim].last_use_seq) {
                victim = i;
            }
        }
        auto& v = pixmap_tex_cache_[victim];
        if (v.img_id > 0) {
            if (in_frame_ && v.last_used_frame == frame_counter_) {
                temp_images_.push_back(v.img_id);
            } else {
                context_->deleteImage(v.img_id);
            }
        }
        pixmap_cache_bytes_ = (pixmap_cache_bytes_ >= v.byte_size) ? (pixmap_cache_bytes_ - v.byte_size) : 0;
        pixmap_tex_cache_[victim] = pixmap_tex_cache_.back();
        pixmap_tex_cache_.pop_back();
    }

    pixmap_tex_cache_.push_back(PixmapTexCacheEntry{
        .data_ptr = ptr,
        .width = w,
        .height = h,
        .stride = stride,
        .format = fmt,
        .flags = flags,
        .content_hash = hash,
        .last_used_frame = frame_counter_,
        .last_use_seq = ++pixmap_use_seq_,
        .byte_size = byte_size,
        .img_id = img
    });
    pixmap_cache_bytes_ += byte_size;
    return img;
}

void GpuCanvas::ensure_frame() {
    if (!context_ || !surface_) return;
    if (!in_frame_) {
        surface_->bind();
        context_->beginFrame(static_cast<float>(surface_->width()), static_cast<float>(surface_->height()), 1.0f);
        in_frame_ = true;
        needs_surface_bind_ = false;
    } else if (needs_surface_bind_) {
        surface_->bind();
        needs_surface_bind_ = false;
    }
}

void GpuCanvas::clear(nisaba::Color color) {
    invalidate_backdrop_cache();
    if (context_ && in_frame_) {
        context_->cancelFrame();
        in_frame_ = false;
        for (int img : temp_images_) {
            context_->deleteImage(img);
        }
        temp_images_.clear();
        ++frame_counter_;
    }
    current_stencil_depth_ = 0;
    if (surface_) {
        surface_->bind();
        surface_->clear(color);
        needs_surface_bind_ = false;
    }
    ensure_frame();
}

bool GpuCanvas::resize(uint32_t width, uint32_t height) {
    if (!surface_ || !surface_->resize_screen(width, height)) {
        return false;
    }
    scissor_clip_ = ScreenIntRect::from_xywh(0, 0, width, height);
    invalidate_backdrop_cache();
    return true;
}

void GpuCanvas::save() {
    if (context_) context_->save();
    state_stack_.push_back(GpuCanvasState{
        .transform = current_transform_,
        .scissor_clip = scissor_clip_,
        .stencil_clip_depth = current_stencil_depth_,
        .is_layer = false
    });
}

void GpuCanvas::save_layer(const nisaba::Paint& paint) {
    push_layer_internal(std::nullopt, paint, false, 0.0f);
}

void GpuCanvas::save_layer_bounds(const nisaba::Rect& bounds, const nisaba::Paint& paint) {
    push_layer_internal(bounds, paint, false, 0.0f);
}

void GpuCanvas::save_layer_backdrop(std::optional<nisaba::Rect> bounds, const nisaba::Paint& paint, float blur_sigma) {
    push_layer_internal(bounds, paint, true, blur_sigma);
}

void GpuCanvas::push_layer_internal(
    std::optional<nisaba::Rect> bounds,
    const nisaba::Paint& paint,
    bool with_backdrop,
    float blur_sigma,
    std::optional<effects::MaskFilter> mask_filter,
    nisaba::Color mask_tint
) {
    if (!context_ || !surface_) return;
    ensure_frame();

    state_stack_.push_back(GpuCanvasState{
        .transform = current_transform_,
        .scissor_clip = scissor_clip_,
        .stencil_clip_depth = current_stencil_depth_,
        .is_layer = true
    });

    auto dev_bounds = nisaba::detail::clip_layer_bounds(
        bounds, current_transform_, width(), height(), scissor_clip_
    );

    LayerDesc desc;
    desc.hasBounds = bounds.has_value() || scissor_clip_.has_value();
    if (dev_bounds.has_value()) {
        desc.bounds[0] = static_cast<float>(dev_bounds->x());
        desc.bounds[1] = static_cast<float>(dev_bounds->y());
        desc.bounds[2] = static_cast<float>(dev_bounds->right());
        desc.bounds[3] = static_cast<float>(dev_bounds->bottom());
        scissor_clip_ = dev_bounds;
    } else {
        desc.hasBounds = true;
        desc.bounds[0] = 0.0f;
        desc.bounds[1] = 0.0f;
        desc.bounds[2] = 0.0f;
        desc.bounds[3] = 0.0f;
        scissor_clip_ = std::nullopt;
    }

    desc.copyBackdrop = with_backdrop;
    desc.backdropBlurSigma = with_backdrop ? std::max(0.0f, blur_sigma) : 0.0f;
    desc.opacity = paint.shader.is_solid_color()
        ? std::clamp(paint.shader.solid_color().alpha(), 0.0f, 1.0f)
        : 1.0f;
    desc.compOp = CompositeOperationState::fromBlendMode(paint.blend_mode);
    desc.layerTransform = Transform2D(current_transform_);
    if (mask_filter.has_value() && mask_filter->is_blur()) {
        desc.maskFilter = mask_filter;
        desc.maskTintColor = Color(mask_tint);
    }
    if (paint.has_custom_blender()) {
        desc.blender = paint.blender;
    }
    if (auto eff_cf = paint.effective_color_effect()) {
        if (eff_cf->is_extended()) {
            desc.extColorEffect = *eff_cf;
        } else {
            apply_color_effect_to(paint, desc);
        }
    }
    if (paint.has_image_filter()) {
        const auto& imgf = *paint.image_filter;
        desc.imageFilter = imgf;
        const bool simple_identity_xform =
            std::abs(current_transform_.sx - 1.0f) < 1e-4f &&
            std::abs(current_transform_.sy - 1.0f) < 1e-4f &&
            std::abs(current_transform_.kx) < 1e-4f &&
            std::abs(current_transform_.ky) < 1e-4f;
        if (imgf.is_simple_leaf() && simple_identity_xform) {
            desc.postBlurSigmaX = imgf.sigma_x();
            desc.postBlurSigmaY = imgf.sigma_y();
            if (imgf.kind() == effects::ImageFilterKind::DropShadow) {
                desc.isDropShadow = true;
                desc.shadowDx = imgf.dx();
                desc.shadowDy = imgf.dy();
                desc.shadowColor = Color(imgf.color());
                desc.shadowOnly = imgf.shadow_only();
            }
        }
    }

    current_stencil_depth_ = 0;
    context_->pushLayer(desc);

    if (bounds.has_value() && dev_bounds.has_value() &&
        current_transform_.has_skew()) {
        clip_rect(*bounds);
    }
}

void GpuCanvas::restore() {
    if (context_) context_->restore();
    if (!state_stack_.empty()) {
        current_transform_ = state_stack_.back().transform;
        scissor_clip_ = state_stack_.back().scissor_clip;
        current_stencil_depth_ = state_stack_.back().stencil_clip_depth;
        state_stack_.pop_back();
    }
}

void GpuCanvas::reset_state() noexcept {
    if (context_) {
        context_->resetState();
    }
    needs_surface_bind_ = true;
}

void GpuCanvas::reset_transform() noexcept {
    current_transform_ = nisaba::Transform();
    if (context_) context_->resetTransform();
}

void GpuCanvas::set_transform(const nisaba::Transform& ts) noexcept {
    current_transform_ = ts;
    if (context_) {
        context_->resetTransform();
        context_->transform(ts);
    }
}

void GpuCanvas::translate(float dx, float dy) noexcept {
    current_transform_ = current_transform_.pre_translate(dx, dy);
    if (context_) context_->translate(dx, dy);
}

void GpuCanvas::scale(float sx, float sy) noexcept {
    current_transform_ = current_transform_.pre_scale(sx, sy);
    if (context_) context_->scale(sx, sy);
}

void GpuCanvas::rotate(float degrees) noexcept {
    current_transform_ = current_transform_.pre_rotate(degrees);
    if (context_) context_->rotate(degrees / 180.0f * nisaba::gpu::PI);
}

void GpuCanvas::skew(float kx, float ky) noexcept {
    concat(nisaba::Transform::from_skew(kx, ky));
}

void GpuCanvas::concat(const nisaba::Transform& ts) noexcept {
    current_transform_ = current_transform_.pre_concat(ts);
    if (context_) context_->transform(ts);
}

void GpuCanvas::clip_rect(const nisaba::Rect& rect) {
    if (context_) {
        context_->scissor(rect.x(), rect.y(), rect.width(), rect.height());
    }
    float x0 = rect.left() * current_transform_.sx + current_transform_.tx;
    float x1 = rect.right() * current_transform_.sx + current_transform_.tx;
    float y0 = rect.top() * current_transform_.sy + current_transform_.ty;
    float y1 = rect.bottom() * current_transform_.sy + current_transform_.ty;
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    int32_t ix0 = std::clamp(static_cast<int32_t>(std::round(x0)), 0, static_cast<int32_t>(width()));
    int32_t iy0 = std::clamp(static_cast<int32_t>(std::round(y0)), 0, static_cast<int32_t>(height()));
    int32_t ix1 = std::clamp(static_cast<int32_t>(std::round(x1)), 0, static_cast<int32_t>(width()));
    int32_t iy1 = std::clamp(static_cast<int32_t>(std::round(y1)), 0, static_cast<int32_t>(height()));

    if (ix1 > ix0 && iy1 > iy0) {
        auto new_clip = ScreenIntRect::from_xywh(
            static_cast<uint32_t>(ix0), static_cast<uint32_t>(iy0),
            static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0)
        );
        if (new_clip) {
            scissor_clip_ = scissor_clip_.has_value() ? scissor_clip_->intersect(*new_clip) : new_clip;
            return;
        }
    }
    scissor_clip_ = std::nullopt;
}

void GpuCanvas::clip_rect(const nisaba::ScreenIntRect& rect) {
    if (context_) {
        context_->scissorScreen(static_cast<float>(rect.x()),
                                static_cast<float>(rect.y()),
                                static_cast<float>(rect.width()),
                                static_cast<float>(rect.height()));
    }
    if (scissor_clip_.has_value()) {
        scissor_clip_ = scissor_clip_->intersect(rect);
    } else {
        scissor_clip_ = rect;
    }
}

void GpuCanvas::clear_rect(const nisaba::Rect& rect, nisaba::Color color) {
    save();
    clip_rect(rect);
    fill_rect(rect, nisaba::Paint(color));
    restore();
}

void GpuCanvas::clear_rect(const nisaba::ScreenIntRect& rect, nisaba::Color color) {
    save();
    clip_rect(rect);
    fill_rect(*nisaba::Rect::from_xywh(rect.x(), rect.y(), rect.width(), rect.height()), nisaba::Paint(color));
    restore();
}

void GpuCanvas::clip_path(const nisaba::Path& path, nisaba::FillRule fill_rule) {
    if (!context_) return;
    ensure_frame();
    current_stencil_depth_++;
    context_->clipPath(path, fill_rule == FillRule::EvenOdd ? Winding::Clockwise : Winding::CounterClockwise);
}

void GpuCanvas::reset_clip() noexcept {
    if (context_) {
        context_->resetScissor();
        context_->resetClip();
    }
    current_stencil_depth_ = 0;
    if (surface_) {
        scissor_clip_ = ScreenIntRect::from_xywh(0, 0, surface_->width(), surface_->height());
    } else {
        scissor_clip_ = std::nullopt;
    }
}

static SpreadKind to_gpu_spread(nisaba::SpreadMode mode) noexcept {
    switch (mode) {
        case nisaba::SpreadMode::Pad:     return SpreadKind::Pad;
        case nisaba::SpreadMode::Repeat:  return SpreadKind::Repeat;
        case nisaba::SpreadMode::Reflect: return SpreadKind::Reflect;
    }
    return SpreadKind::Pad;
}

int GpuCanvas::get_or_create_grad_lut(const nisaba::Gradient& grad) {
    if (!context_) return 0;
    const auto& stops = grad.stops();
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(static_cast<uint64_t>(stops.size()));
    for (const auto& st : stops) {
        uint32_t pb = 0;
        float pos = st.position.get();
        std::memcpy(&pb, &pos, sizeof(float));
        mix(pb);
        const auto cu = st.color.to_color_u8();
        const uint32_t rgba = (static_cast<uint32_t>(cu.r) << 24) |
                              (static_cast<uint32_t>(cu.g) << 16) |
                              (static_cast<uint32_t>(cu.b) << 8) |
                              static_cast<uint32_t>(cu.a);
        mix(rgba);
    }
    for (const auto& entry : grad_lut_cache_) {
        if (entry.key == h) return entry.img_id;
    }
    int img = context_->createImageWithFormat(
        static_cast<int>(nisaba::Gradient::RAMP_SIZE),
        1,
        static_cast<int>(nisaba::Gradient::RAMP_SIZE * 4),
        PixelFormat::RGBA8888,
        ImageFlags::ImagePremultiplied | ImageFlags::ImageForceBilinear,
        reinterpret_cast<const unsigned char*>(grad.ramp().data())
    );
    if (img <= 0) return 0;
    if (grad_lut_cache_.size() >= 32) {
        if (grad_lut_cache_.front().img_id > 0) {
            context_->deleteImage(grad_lut_cache_.front().img_id);
        }
        grad_lut_cache_.erase(grad_lut_cache_.begin());
    }
    grad_lut_cache_.push_back(GradLutEntry{h, img});
    return img;
}

void GpuCanvas::populate_gradient_stops_and_xform(
    const nisaba::Gradient& grad,
    Paint& gp
) {
    gp.xform = Transform2D(grad.transform());
    gp.gradientSpread = to_gpu_spread(grad.spread_mode());
    gp.innerColor = Color::rgbaf(1.0f, 1.0f, 1.0f, 1.0f);
    gp.outerColor = gp.innerColor;

    const auto& stops = grad.stops();
    if (stops.size() <= 4) {
        gp.gradStopCount = static_cast<int>(stops.size());
        for (size_t i = 0; i < stops.size(); ++i) {
            gp.gradStops[i].position = stops[i].position.get();
            const auto& c = stops[i].color;
            gp.gradStops[i].color = Color::rgbaf(c.red(), c.green(), c.blue(), c.alpha());
        }
    } else {
        gp.gradStopCount = -1;
        int img = get_or_create_grad_lut(grad);
        if (img > 0) {
            gp.image = img;
        } else {
            gp.gradStopCount = 0;
        }
    }
}

static void populate_conical_params(
    nisaba::Point start_center,
    float start_radius,
    nisaba::Point end_center,
    float end_radius,
    Paint& gp
) {
    gp.gradientKind = GradientKind::TwoPointConical;
    gp.gradUnitXform = Transform2D::translate(-start_center.x, -start_center.y);
    const float dx = end_center.x - start_center.x;
    const float dy = end_center.y - start_center.y;
    const float dr = end_radius - start_radius;
    const float A = dx * dx + dy * dy - dr * dr;
    const bool is_concentric = (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f);
    gp.gradParams0 = {dx, dy, start_radius, dr};
    gp.gradParams1 = {A, is_concentric ? 1.0f : 0.0f, 0.0f, 0.0f};
}

Paint GpuCanvas::build_gpu_paint(const nisaba::Paint& paint) {
    Paint gp;
    const auto stype = paint.shader.type();

    if (stype == nisaba::Shader::Type::LinearGradient) {
        const auto& lg = paint.shader.linear_gradient();
        const nisaba::Point p0 = lg.start();
        const nisaba::Point p1 = lg.end();
        const float dx = p1.x - p0.x;
        const float dy = p1.y - p0.y;
        const float len2 = dx * dx + dy * dy;
        const bool is_simple =
            (lg.stops().size() == 2) &&
            (lg.spread_mode() == nisaba::SpreadMode::Pad) &&
            lg.transform().is_identity() &&
            std::abs(lg.stops().front().position.get() - 0.0f) < 1e-5f &&
            std::abs(lg.stops().back().position.get() - 1.0f) < 1e-5f &&
            (len2 >= 1.0f);
        if (is_simple) {
            gp = context_->linearGradient(p0.x, p0.y, p1.x, p1.y, lg.stops().front().color, lg.stops().back().color);
        } else {
            gp.gradientKind = GradientKind::Linear;
            if (len2 > 1e-12f) {
                const float inv_len2 = 1.0f / len2;
                const float tx = -(p0.x * dx + p0.y * dy) * inv_len2;
                const float ty = (p0.x * dy - p0.y * dx) * inv_len2;
                gp.gradUnitXform = Transform2D(dx * inv_len2, -dy * inv_len2, dy * inv_len2, dx * inv_len2, tx, ty);
            } else {
                gp.gradParams0[2] = 1.0f; // degenerate length -> constant t=1.0
            }
            populate_gradient_stops_and_xform(lg, gp);
        }
    } else if (stype == nisaba::Shader::Type::RadialGradient) {
        const auto& rg = paint.shader.radial_gradient();
        if (!rg.is_2point()) {
            const bool is_simple =
                (rg.stops().size() == 2) &&
                (rg.spread_mode() == nisaba::SpreadMode::Pad) &&
                rg.transform().is_identity() &&
                std::abs(rg.stops().front().position.get() - 0.0f) < 1e-5f &&
                std::abs(rg.stops().back().position.get() - 1.0f) < 1e-5f &&
                (rg.radius() >= 1.0f);
            if (is_simple) {
                const nisaba::Point center = rg.center();
                gp = context_->radialGradient(center.x, center.y, 0.0f, rg.radius(), rg.stops().front().color, rg.stops().back().color);
            } else {
                gp.gradientKind = GradientKind::Radial;
                const float inv_r = (rg.radius() > 1e-6f) ? (1.0f / rg.radius()) : 0.0f;
                const nisaba::Point center = rg.center();
                gp.gradUnitXform = Transform2D(inv_r, 0.0f, 0.0f, inv_r, -center.x * inv_r, -center.y * inv_r);
                populate_gradient_stops_and_xform(rg, gp);
            }
        } else {
            populate_conical_params(rg.start(), rg.start_radius(), rg.end(), rg.end_radius(), gp);
            populate_gradient_stops_and_xform(rg, gp);
        }
    } else if (stype == nisaba::Shader::Type::SweepGradient) {
        const auto& sg = paint.shader.sweep_gradient();
        gp.gradientKind = GradientKind::Sweep;
        gp.gradUnitXform = Transform2D::translate(-sg.center().x, -sg.center().y);
        gp.gradParams0 = {sg.start_angle_rad(), sg.inv_angle_range(), 0.0f, 0.0f};
        populate_gradient_stops_and_xform(sg, gp);
    } else if (stype == nisaba::Shader::Type::TwoPointConicalGradient) {
        const auto& cg = paint.shader.conical_gradient();
        populate_conical_params(cg.start_center(), cg.start_radius(), cg.end_center(), cg.end_radius(), gp);
        populate_gradient_stops_and_xform(cg, gp);
    } else if (stype == nisaba::Shader::Type::PerlinNoise) {
        const auto& pn = paint.shader.perlin_noise();
        gp.gradientKind = (pn.noise_type() == nisaba::PerlinNoiseType::Turbulence)
            ? GradientKind::PerlinTurbulence
            : GradientKind::PerlinFractal;
        gp.xform = Transform2D(pn.transform());
        gp.gradUnitXform = Transform2D::identity();
        gp.gradStopCount = pn.num_octaves();
        gp.gradParams0 = {pn.base_freq_x(), pn.base_freq_y(), pn.seed(), std::clamp(pn.opacity(), 0.0f, 1.0f)};
        const bool has_tile = pn.tile_size().has_value() && pn.tile_size()->width() > 0 && pn.tile_size()->height() > 0;
        const float tw = has_tile ? static_cast<float>(pn.tile_size()->width()) : 0.0f;
        const float th = has_tile ? static_cast<float>(pn.tile_size()->height()) : 0.0f;
        gp.gradParams1 = {
            has_tile ? std::max(1.0f, std::round(tw * pn.base_freq_x())) : 0.0f,
            has_tile ? std::max(1.0f, std::round(th * pn.base_freq_y())) : 0.0f,
            has_tile ? 1.0f : 0.0f,
            0.0f
        };
        gp.innerColor = Color::rgbaf(1.0f, 1.0f, 1.0f, 1.0f);
        gp.outerColor = gp.innerColor;
    } else if (stype == nisaba::Shader::Type::Pattern) {
        const auto& pat = paint.shader.pattern();
        const auto& px = pat.pixmap();
        if (pat.inv_transform().has_value() && px.width() > 0 && px.height() > 0 && px.data() != nullptr) {
            int flags = ImageFlags::ImagePremultiplied;
            if (pat.quality() == nisaba::FilterQuality::Nearest) {
                flags |= ImageFlags::ImageNearest | ImageFlags::ImageForceNearest;
            } else if (px.width() > 1 && px.height() > 1) {
                const auto [psx, psy] = pat.transform().get_scale();
                const auto [csx, csy] = current_transform_.get_scale();
                if (pat.quality() == nisaba::FilterQuality::Bicubic ||
                    (psx * csx < 0.75f || psy * csy < 0.75f)) {
                    flags |= ImageFlags::ImageGenerateMipmaps;
                }
            }
            if (pat.spread_mode() == nisaba::SpreadMode::Repeat) {
                flags |= ImageFlags::ImageRepeatX | ImageFlags::ImageRepeatY;
            } else if (pat.spread_mode() == nisaba::SpreadMode::Reflect) {
                flags |= ImageFlags::ImageMirrorX | ImageFlags::ImageMirrorY;
            }
            int img = get_or_upload_pixmap(px, flags);
            if (img > 0) {
                gp.image = img;
                gp.extent[0] = static_cast<float>(px.width());
                gp.extent[1] = static_cast<float>(px.height());
                gp.xform = Transform2D(pat.transform());
                const float op = std::clamp(pat.opacity(), 0.0f, 1.0f);
                gp.innerColor = Color::rgbaf(1.0f, 1.0f, 1.0f, op);
                gp.outerColor = gp.innerColor;
            } else {
                gp = Paint::color(Color::rgba(0, 0, 0, 0));
            }
        } else {
            gp = Paint::color(Color::rgba(0, 0, 0, 0));
        }
    } else {
        gp = Paint::color(paint.shader.solid_color());
    }

    apply_color_effect_to(paint, gp);
    return gp;
}

void GpuCanvas::apply_fill_paint(const nisaba::Paint& paint) {
    if (!context_) return;

    if (paint.custom_shader) {
        context_->fillShader(paint.custom_shader);
        return;
    }

    if (auto folded = try_fold_solid_color_filter(paint)) {
        context_->fillColor(*folded);
        return;
    }

    if (paint.shader.type() != nisaba::Shader::Type::SolidColor || paint.has_color_filter()) {
        context_->fillPaint(build_gpu_paint(paint));
        return;
    }

    context_->fillColor(paint.shader.solid_color());
}

void GpuCanvas::apply_stroke(const nisaba::Stroke& stroke, const nisaba::Paint& paint) {
    if (!context_) return;
    context_->strokeWidth(stroke.width);
    context_->miterLimit(stroke.miter_limit);
    context_->lineCap(stroke.line_cap);
    context_->lineJoin(stroke.line_join);

    if (paint.custom_shader) {
        context_->strokeShader(paint.custom_shader);
        return;
    }

    if (auto folded = try_fold_solid_color_filter(paint)) {
        context_->strokeColor(*folded);
        return;
    }

    if (paint.shader.type() != nisaba::Shader::Type::SolidColor || paint.has_color_filter()) {
        context_->strokePaint(build_gpu_paint(paint));
        return;
    }

    context_->strokeColor(paint.shader.solid_color());
}

template <typename DrawFn>
void GpuCanvas::draw_with_image_filter(
    const std::optional<nisaba::Rect>& local_bounds,
    const nisaba::Paint& paint,
    float extra_margin,
    DrawFn&& draw_fn
) {
    if (paint.has_custom_blender()) {
        const float mask_pad = (paint.has_mask_filter() && paint.mask_filter->is_blur())
            ? (std::ceil(paint.mask_filter->sigma * 3.0f) + 2.0f) : 0.0f;
        std::optional<nisaba::Rect> padded_bounds = std::nullopt;
        if (local_bounds.has_value()) {
            nisaba::Rect base = paint.has_image_filter() ? paint.image_filter->map_layer_bounds(*local_bounds) : *local_bounds;
            const float tot = extra_margin + mask_pad + 1.0f;
            padded_bounds = nisaba::Rect::from_xywh(
                base.x() - tot, base.y() - tot, base.width() + 2.0f * tot, base.height() + 2.0f * tot);
        }
        nisaba::Paint layer_paint(nisaba::Color::WHITE);
        layer_paint.blender = paint.blender;
        nisaba::Paint inner_paint = paint;
        inner_paint.blender = std::nullopt;
        inner_paint.blend_mode = BlendMode::SourceOver;
        push_layer_internal(padded_bounds, layer_paint, false, 0.0f);
        draw_fn(inner_paint);
        restore();
        return;
    }

    if (paint.has_image_filter()) {
        const auto& imgf = *paint.image_filter;
        const float mask_pad = (paint.has_mask_filter() && paint.mask_filter->is_blur())
            ? (std::ceil(paint.mask_filter->sigma * 3.0f) + 2.0f) : 0.0f;
        std::optional<nisaba::Rect> padded_bounds = std::nullopt;
        if (local_bounds.has_value()) {
            const nisaba::Rect mapped = imgf.map_layer_bounds(*local_bounds);
            const float tot = extra_margin + mask_pad;
            padded_bounds = nisaba::Rect::from_xywh(
                mapped.x() - tot,
                mapped.y() - tot,
                mapped.width() + 2.0f * tot,
                mapped.height() + 2.0f * tot
            );
        }
        nisaba::Paint layer_paint(nisaba::Color::WHITE);
        layer_paint.blend_mode = paint.blend_mode;
        layer_paint.image_filter = imgf;

        nisaba::Paint inner_paint = paint;
        inner_paint.blend_mode = BlendMode::SourceOver;
        inner_paint.image_filter = std::nullopt;

        push_layer_internal(padded_bounds, layer_paint, false, 0.0f);
        draw_fn(inner_paint);
        restore();
        return;
    }

    if (paint.has_extended_color_filter() && !paint.is_solid_color()) {
        std::optional<nisaba::Rect> padded_bounds = std::nullopt;
        if (local_bounds.has_value()) {
            const float tot = extra_margin + 1.0f;
            padded_bounds = nisaba::Rect::from_xywh(
                local_bounds->x() - tot, local_bounds->y() - tot,
                local_bounds->width() + 2.0f * tot, local_bounds->height() + 2.0f * tot);
        }
        nisaba::Paint layer_paint(nisaba::Color::WHITE);
        layer_paint.blend_mode = paint.blend_mode;
        layer_paint.color_effect = paint.color_effect;

        nisaba::Paint inner_paint = paint;
        inner_paint.blend_mode = BlendMode::SourceOver;
        inner_paint.clear_color_filter();

        push_layer_internal(padded_bounds, layer_paint, false, 0.0f);
        draw_fn(inner_paint);
        restore();
        return;
    }

    if (paint.has_mask_filter()) {
        const auto& mf = *paint.mask_filter;
        if (mf.is_shader()) {
            std::optional<nisaba::Rect> padded_bounds = std::nullopt;
            if (local_bounds.has_value()) {
                const float tot = extra_margin + 1.0f;
                padded_bounds = nisaba::Rect::from_xywh(
                    local_bounds->x() - tot, local_bounds->y() - tot,
                    local_bounds->width() + 2.0f * tot, local_bounds->height() + 2.0f * tot);
            }
            nisaba::Paint outer_paint(nisaba::Color::WHITE);
            outer_paint.blend_mode = paint.blend_mode;
            push_layer_internal(padded_bounds, outer_paint, false, 0.0f);

            nisaba::Paint inner_paint = paint;
            inner_paint.blend_mode = BlendMode::SourceOver;
            inner_paint.clear_mask_filter();
            draw_fn(inner_paint);

            nisaba::Paint mask_p(*mf.mask_shader);
            mask_p.blend_mode = BlendMode::DestinationIn;
            nisaba::Rect cover = padded_bounds.value_or(
                nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height())).value());
            fill_rect(cover, mask_p);
            restore();
            return;
        }
        float pad = std::ceil(mf.sigma * 3.0f) + 2.0f + extra_margin;
        if (!mf.respect_ctm) {
            const float min_scale = std::min(
                std::hypot(current_transform_.sx, current_transform_.ky),
                std::hypot(current_transform_.kx, current_transform_.sy)
            );
            if (min_scale > 1e-4f) {
                pad = std::ceil((mf.sigma / min_scale) * 3.0f) + 2.0f + extra_margin;
            }
        }
        std::optional<nisaba::Rect> padded_bounds = std::nullopt;
        if (local_bounds.has_value()) {
            padded_bounds = nisaba::Rect::from_xywh(
                local_bounds->x() - pad,
                local_bounds->y() - pad,
                local_bounds->width() + 2.0f * pad,
                local_bounds->height() + 2.0f * pad
            );
        }

        nisaba::Paint mask_draw_paint = paint;
        mask_draw_paint.set_color(nisaba::Color::WHITE);
        mask_draw_paint.clear_color_filter();
        mask_draw_paint.clear_image_filter();
        mask_draw_paint.clear_mask_filter();
        mask_draw_paint.blend_mode = BlendMode::SourceOver;

        if (paint.is_solid_color()) {
            nisaba::Color tint = paint.shader.solid_color();
            if (auto cf = paint.effective_color_effect()) {
                tint = cf->transform(tint);
            }
            nisaba::Paint layer_paint(nisaba::Color::WHITE);
            layer_paint.blend_mode = paint.blend_mode;
            push_layer_internal(padded_bounds, layer_paint, false, 0.0f, mf, tint);
            draw_fn(mask_draw_paint);
            restore();
        } else {
            nisaba::Paint outer_paint(nisaba::Color::WHITE);
            outer_paint.blend_mode = paint.blend_mode;
            push_layer_internal(padded_bounds, outer_paint, false, 0.0f);

            push_layer_internal(padded_bounds, nisaba::Paint(nisaba::Color::WHITE), false, 0.0f, mf, nisaba::Color::WHITE);
            draw_fn(mask_draw_paint);
            restore();

            nisaba::Paint shade_paint = paint;
            shade_paint.clear_image_filter();
            shade_paint.clear_mask_filter();
            shade_paint.blend_mode = BlendMode::SourceIn;
            nisaba::Rect cover = padded_bounds.value_or(
                nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height())).value());
            fill_rect(cover, shade_paint);
            restore();
        }
    }
}

void GpuCanvas::fill_rect(const nisaba::Rect& rect, const nisaba::Paint& paint) {
    if (!context_) return;
    if (paint.has_path_effect()) {
        fill_path(PathBuilder::from_rect(rect), paint, nisaba::FillRule::Winding);
        return;
    }
    if (paint.has_layer_filter()) {
        draw_with_image_filter(rect, paint, 0.0f, [&](const nisaba::Paint& inner) {
            fill_rect(rect, inner);
        });
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->rect(rect.x(), rect.y(), rect.width(), rect.height());
    apply_fill_paint(paint);
    context_->fill();
}

void GpuCanvas::stroke_rect(const nisaba::Rect& rect, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_) return;
    if (paint.has_path_effect()) {
        stroke_path(PathBuilder::from_rect(rect), paint, stroke);
        return;
    }
    if (paint.has_layer_filter()) {
        draw_with_image_filter(rect, paint, stroke_filter_margin(stroke), [&](const nisaba::Paint& inner) {
            stroke_rect(rect, inner, stroke);
        });
        return;
    }
    if (stroke.dash.has_value()) {
        stroke_path(PathBuilder::from_rect(rect), paint, stroke);
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->rect(rect.x(), rect.y(), rect.width(), rect.height());
    apply_stroke(stroke, paint);
    context_->stroke();
}

void GpuCanvas::fill_round_rect(const nisaba::Rect& rect, float rx, float ry, const nisaba::Paint& paint) {
    if (!context_) return;
    if (paint.has_layer_filter()) {
        draw_with_image_filter(rect, paint, 0.0f, [&](const nisaba::Paint& inner) {
            fill_round_rect(rect, rx, ry, inner);
        });
        return;
    }
    if (std::abs(rx - ry) > 1e-4f) {
        if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
            fill_path(*p, paint);
        }
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->roundedRect(rect.x(), rect.y(), rect.width(), rect.height(), rx);
    apply_fill_paint(paint);
    context_->fill();
}

void GpuCanvas::stroke_round_rect(const nisaba::Rect& rect, float rx, float ry, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_) return;
    if (paint.has_layer_filter()) {
        draw_with_image_filter(rect, paint, stroke_filter_margin(stroke), [&](const nisaba::Paint& inner) {
            stroke_round_rect(rect, rx, ry, inner, stroke);
        });
        return;
    }
    if (stroke.dash.has_value() || std::abs(rx - ry) > 1e-4f) {
        if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
            stroke_path(*p, paint, stroke);
        }
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->roundedRect(rect.x(), rect.y(), rect.width(), rect.height(), rx);
    apply_stroke(stroke, paint);
    context_->stroke();
}

void GpuCanvas::fill_rrect(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const nisaba::Paint& paint) {
    if (!context_) return;
    if (paint.has_layer_filter()) {
        draw_with_image_filter(rect, paint, 0.0f, [&](const nisaba::Paint& inner) {
            fill_rrect(rect, radii, inner);
        });
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    BorderRadius cr = radii.constrained_for_size(rect.width(), rect.height());
    context_->beginPath();
    context_->roundedRectVarying(rect.x(), rect.y(), rect.width(), rect.height(),
                                 cr.top_left, cr.top_right, cr.bottom_right, cr.bottom_left);
    apply_fill_paint(paint);
    context_->fill();
}

void GpuCanvas::stroke_rrect(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_) return;
    BorderRadius cr = radii.constrained_for_size(rect.width(), rect.height());
    if (cr.is_uniform() && !paint.has_layer_filter() && !stroke.dash.has_value()) {
        stroke_round_rect(rect, cr.top_left, cr.top_left, paint, stroke);
        return;
    }
    ensure_frame();
    auto path_opt = PathBuilder::from_rounded_rect(rect, radii);
    if (path_opt) {
        stroke_path(*path_opt, paint, stroke);
    }
}

void GpuCanvas::fill_rrect(const nisaba::RRect& rrect, const nisaba::Paint& paint) {
    fill_rrect(rrect.rect, rrect.radii, paint);
}

void GpuCanvas::stroke_rrect(const nisaba::RRect& rrect, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    stroke_rrect(rrect.rect, rrect.radii, paint, stroke);
}

void GpuCanvas::clip_rrect(const nisaba::RRect& rrect) {
    auto path_opt = PathBuilder::from_rrect(rrect);
    if (path_opt) {
        clip_path(*path_opt, FillRule::Winding);
    }
}

void GpuCanvas::clip_rrect(const nisaba::Rect& rect, const nisaba::BorderRadius& radii) {
    clip_rrect(RRect(rect, radii));
}

void GpuCanvas::fill_circle(float cx, float cy, float radius, const nisaba::Paint& paint) {
    if (!context_) return;
    if (paint.has_layer_filter()) {
        auto lb = nisaba::Rect::from_xywh(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        draw_with_image_filter(lb, paint, 0.0f, [&](const nisaba::Paint& inner) {
            fill_circle(cx, cy, radius, inner);
        });
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->circle(cx, cy, radius);
    apply_fill_paint(paint);
    context_->fill();
}

void GpuCanvas::stroke_circle(float cx, float cy, float radius, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_) return;
    if (paint.has_layer_filter()) {
        auto lb = nisaba::Rect::from_xywh(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        draw_with_image_filter(lb, paint, stroke_filter_margin(stroke), [&](const nisaba::Paint& inner) {
            stroke_circle(cx, cy, radius, inner, stroke);
        });
        return;
    }
    if (stroke.dash.has_value()) {
        if (auto p = PathBuilder::from_circle(cx, cy, radius)) {
            stroke_path(*p, paint, stroke);
        }
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->circle(cx, cy, radius);
    apply_stroke(stroke, paint);
    context_->stroke();
}

void GpuCanvas::stroke_line(nisaba::Point p0, nisaba::Point p1, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    stroke_line(p0.x, p0.y, p1.x, p1.y, paint, stroke);
}

void GpuCanvas::stroke_line(float x0, float y0, float x1, float y1, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_) return;
    if (paint.has_layer_filter()) {
        const float min_x = std::min(x0, x1);
        const float min_y = std::min(y0, y1);
        const float max_x = std::max(x0, x1);
        const float max_y = std::max(y0, y1);
        auto lb = nisaba::Rect::from_ltrb(min_x, min_y, std::max(max_x, min_x + 1.0f), std::max(max_y, min_y + 1.0f));
        draw_with_image_filter(lb, paint, stroke_filter_margin(stroke), [&](const nisaba::Paint& inner) {
            stroke_line(x0, y0, x1, y1, inner, stroke);
        });
        return;
    }
    if (stroke.dash.has_value()) {
        nisaba::PathBuilder pb;
        pb.move_to(x0, y0);
        pb.line_to(x1, y1);
        if (auto p = pb.finish()) {
            stroke_path(*p, paint, stroke);
        }
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->moveTo(x0, y0);
    context_->lineTo(x1, y1);
    apply_stroke(stroke, paint);
    context_->stroke();
}

void GpuCanvas::fill_oval(const nisaba::Rect& oval, const nisaba::Paint& paint) {
    if (!context_ || oval.is_empty()) return;
    if (paint.has_layer_filter()) {
        draw_with_image_filter(oval, paint, 0.0f, [&](const nisaba::Paint& inner) {
            fill_oval(oval, inner);
        });
        return;
    }
    const float rx = oval.width() * 0.5f;
    const float ry = oval.height() * 0.5f;
    const float cx = oval.left() + rx;
    const float cy = oval.top() + ry;
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->ellipse(cx, cy, rx, ry);
    apply_fill_paint(paint);
    context_->fill();
}

void GpuCanvas::stroke_oval(const nisaba::Rect& oval, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_ || oval.is_empty()) return;
    if (paint.has_layer_filter()) {
        draw_with_image_filter(oval, paint, stroke_filter_margin(stroke), [&](const nisaba::Paint& inner) {
            stroke_oval(oval, inner, stroke);
        });
        return;
    }
    if (stroke.dash.has_value()) {
        if (auto p = nisaba::PathBuilder::from_oval(oval)) {
            stroke_path(*p, paint, stroke);
        }
        return;
    }
    const float rx = oval.width() * 0.5f;
    const float ry = oval.height() * 0.5f;
    const float cx = oval.left() + rx;
    const float cy = oval.top() + ry;
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->ellipse(cx, cy, rx, ry);
    apply_stroke(stroke, paint);
    context_->stroke();
}

void GpuCanvas::clip_oval(const nisaba::Rect& oval) {
    if (auto p = nisaba::PathBuilder::from_oval(oval)) {
        clip_path(*p, nisaba::FillRule::Winding);
    }
}

void GpuCanvas::fill_drrect(const nisaba::RRect& outer, const nisaba::RRect& inner, const nisaba::Paint& paint) {
    if (!context_ || outer.is_empty()) return;
    if (inner.is_empty()) {
        fill_rrect(outer, paint);
        return;
    }

    const bool inner_box_inside =
        outer.left() <= inner.left() && outer.top() <= inner.top() &&
        outer.right() >= inner.right() && outer.bottom() >= inner.bottom() &&
        outer.contains(nisaba::Point::from_xy(inner.left(), inner.top())) &&
        outer.contains(nisaba::Point::from_xy(inner.right(), inner.top())) &&
        outer.contains(nisaba::Point::from_xy(inner.left(), inner.bottom())) &&
        outer.contains(nisaba::Point::from_xy(inner.right(), inner.bottom()));

    if (inner_box_inside) {
        if (context_->renderer() && context_->renderer()->supportsDRRectShader() &&
            paint.is_solid_color() && !paint.has_layer_filter() && !paint.has_custom_shader()) {
            const auto comp = CompositeOperationState::fromBlendMode(paint.blend_mode);
            nisaba::Color col = paint.shader.solid_color();
            bool color_ok = !paint.has_color_filter();
            if (!color_ok) {
                if (auto folded = try_fold_solid_color_filter(paint)) {
                    col = *folded;
                    color_ok = true;
                }
            }
            if (color_ok && !comp.isProgrammableBlend) {
                ensure_frame();
                const auto oc = outer.constrained();
                const auto ic = inner.constrained();
                const float ohw = oc.width() * 0.5f;
                const float ohh = oc.height() * 0.5f;
                const float ocx = oc.left() + ohw;
                const float ocy = oc.top() + ohh;
                const float ihw = ic.width() * 0.5f;
                const float ihh = ic.height() * 0.5f;
                const float icx = ic.left() + ihw;
                const float icy = ic.top() + ihh;

                const float pad = 1.5f;
                const float u0 = -ohw - pad, v0 = -ohh - pad;
                const float u1 =  ohw + pad, v1 =  ohh + pad;
                const uint32_t packed = Color(col).premultiplied().toRGBA8();

                Paint gp = Paint::color(Color(col));
                gp.extent[0] = ohw;
                gp.extent[1] = ohh;
                gp.radius = oc.radii.top_left;
                gp.feather = oc.radii.top_right;
                gp.xform[0] = oc.radii.bottom_right;
                gp.xform[1] = oc.radii.bottom_left;
                gp.gradParams0 = {icx - ocx, icy - ocy, ihw, ihh};
                gp.gradParams1 = {ic.radii.top_left, ic.radii.top_right, ic.radii.bottom_right, ic.radii.bottom_left};

                Vertex quad[6] = {
                    Vertex(ocx + u0, ocy + v0, u0, v0, packed),
                    Vertex(ocx + u0, ocy + v1, u0, v1, packed),
                    Vertex(ocx + u1, ocy + v1, u1, v1, packed),
                    Vertex(ocx + u0, ocy + v0, u0, v0, packed),
                    Vertex(ocx + u1, ocy + v1, u1, v1, packed),
                    Vertex(ocx + u1, ocy + v0, u1, v0, packed)
                };
                ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
                context_->drawTriangles(gp, quad, 6, 1.0f, 18 /* ShaderDRRect */);
                return;
            }
        }
        if (auto po = nisaba::PathBuilder::from_rrect(outer)) {
            if (auto pi = nisaba::PathBuilder::from_rrect(inner)) {
                ensure_frame();
                ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
                context_->beginPath();
                context_->path(*po);
                context_->pathWinding(Winding::CounterClockwise);
                context_->path(*pi);
                context_->pathWinding(Winding::Hole);
                apply_fill_paint(paint);
                context_->fill();
                return;
            }
        }
    }

    const nisaba::Path po_path = outer.to_path();
    const nisaba::Path pi_path = inner.to_path();
    if (auto diff = po_path.difference(pi_path)) {
        if (!diff->is_empty()) {
            fill_path(*diff, paint);
        }
    } else {
        fill_path(po_path, paint);
    }
}

void GpuCanvas::stroke_drrect(
    const nisaba::RRect& outer,
    const nisaba::RRect& inner,
    const nisaba::Paint& paint,
    const nisaba::Stroke& stroke
) {
    if (!context_) return;
    stroke_rrect(outer, paint, stroke);
    stroke_rrect(inner, paint, stroke);
}

void GpuCanvas::draw_points(
    nisaba::PointMode mode,
    std::span<const nisaba::Point> points,
    const nisaba::Paint& paint,
    const nisaba::Stroke& stroke
) {
    if (!context_ || points.empty()) return;
    nisaba::ICanvas::draw_points(mode, points, paint, stroke);
}

void GpuCanvas::fill_path(const nisaba::Path& path, const nisaba::Paint& paint, nisaba::FillRule fill_rule) {
    if (!context_) return;
    if (paint.has_path_effect()) {
        const auto& pe = *paint.path_effect;
        nisaba::Path mod = pe.apply(path);
        nisaba::Paint p2 = paint;
        p2.clear_path_effect();
        if (pe.needs_source_clip()) {
            save();
            clip_path(path, fill_rule);
            fill_path(mod, p2, fill_rule);
            restore();
        } else {
            fill_path(mod, p2, fill_rule);
        }
        return;
    }
    if (paint.has_layer_filter()) {
        draw_with_image_filter(path.bounds(), paint, 0.0f, [&](const nisaba::Paint& inner) {
            fill_path(path, inner, fill_rule);
        });
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    context_->beginPath();
    context_->path(path, fill_rule == nisaba::FillRule::EvenOdd ? Winding::EvenOdd : Winding::Preserve);
    apply_fill_paint(paint);
    context_->fill();
}

void GpuCanvas::stroke_path(const nisaba::Path& path, const nisaba::Paint& paint, const nisaba::Stroke& stroke) {
    if (!context_) return;
    if (paint.has_path_effect()) {
        const auto& pe = *paint.path_effect;
        nisaba::Path mod = pe.apply(path);
        nisaba::Paint p2 = paint;
        p2.clear_path_effect();
        if (pe.produces_fill_geometry()) {
            if (pe.needs_source_clip()) {
                save();
                clip_path(path, nisaba::FillRule::Winding);
                fill_path(mod, p2, nisaba::FillRule::Winding);
                restore();
            } else {
                fill_path(mod, p2, nisaba::FillRule::Winding);
            }
        } else {
            stroke_path(mod, p2, stroke);
        }
        return;
    }
    if (paint.has_layer_filter()) {
        draw_with_image_filter(path.bounds(), paint, stroke_filter_margin(stroke), [&](const nisaba::Paint& inner) {
            stroke_path(path, inner, stroke);
        });
        return;
    }
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    if (stroke.dash.has_value()) {
        float res_scale = PathStroker::compute_resolution_scale(current_transform_);
        const nisaba::Path* dashed = dash_path_fast(path, *stroke.dash, res_scale);
        if (!dashed || dashed->is_empty()) return;
        context_->beginPath();
        context_->path(*dashed);
        apply_stroke(stroke, paint);
        context_->stroke();
        return;
    }
    context_->beginPath();
    context_->path(path);
    apply_stroke(stroke, paint);
    context_->stroke();
}

void GpuCanvas::draw_pixmap(int32_t x, int32_t y, const PixmapRef& src, const PixmapPaint& paint) {
    if (!context_ || src.width() == 0 || src.height() == 0 || !src.data()) return;
    auto w = static_cast<float>(src.width());
    auto h = static_cast<float>(src.height());
    auto src_r = nisaba::Rect::from_xywh(0.0f, 0.0f, w, h);
    auto dst_r = nisaba::Rect::from_xywh(static_cast<float>(x), static_cast<float>(y), w, h);
    if (src_r && dst_r) {
        draw_pixmap_rect(src, *src_r, *dst_r, paint);
    }
}

void GpuCanvas::draw_pixmap_3d(
    const PixmapRef& src,
    const nisaba::Rect& src_rect,
    const Transform4x4& transform3d,
    float opacity,
    BlendMode blend_mode
) {
    if (!context_ || src.is_empty() || src.width() == 0 || src.height() == 0 || !src.data()) return;
    if (src_rect.width() <= 0.0f || src_rect.height() <= 0.0f || opacity <= 0.0f) return;
    ensure_frame();

    Transform cur_xform = transform();
    Transform4x4 final_t = transform3d;
    if (!cur_xform.is_identity()) {
        final_t = Transform4x4(cur_xform) * transform3d;
    }

    auto p0_opt = final_t.map_point_checked(Point(src_rect.left(), src_rect.top()));
    auto p1_opt = final_t.map_point_checked(Point(src_rect.right(), src_rect.top()));
    auto p2_opt = final_t.map_point_checked(Point(src_rect.right(), src_rect.bottom()));
    auto p3_opt = final_t.map_point_checked(Point(src_rect.left(), src_rect.bottom()));
    if (!p0_opt || !p1_opt || !p2_opt || !p3_opt) return;

    const float a = final_t.m[0],  b = final_t.m[1],  c = final_t.m[3];
    const float d = final_t.m[4],  e = final_t.m[5],  f = final_t.m[7];
    const float g = final_t.m[12], h = final_t.m[13], i = final_t.m[15];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12f) return;

    const float inv_det = 1.0f / det;
    const float inv_h[9] = {
        (e * i - f * h) * inv_det, (c * h - b * i) * inv_det, (b * f - c * e) * inv_det,
        (f * g - d * i) * inv_det, (a * i - c * g) * inv_det, (c * d - a * f) * inv_det,
        (d * h - e * g) * inv_det, (b * g - a * h) * inv_det, (a * e - b * d) * inv_det
    };

    int flags3d = ImageFlags::ImagePremultiplied;
    if (src.width() >= 128 && src.height() >= 128) {
        flags3d |= ImageFlags::ImageGenerateMipmaps;
    }
    int img = get_or_upload_pixmap(src, flags3d);
    if (img <= 0) return;

    const float tex_w = static_cast<float>(src.width());
    const float tex_h = static_cast<float>(src.height());
    const float alpha = std::clamp(opacity, 0.0f, 1.0f);

    Paint p;
    p.image = img;
    p.innerColor = Color(1.0f, 1.0f, 1.0f, alpha);
    p.outerColor = p.innerColor;
    p.extent[0] = tex_w;
    p.extent[1] = tex_h;
    p.gradParams0 = {inv_h[0], inv_h[1], inv_h[2], inv_h[3]};
    p.gradParams1 = {inv_h[4], inv_h[5], inv_h[6], inv_h[7]};
    p.radius = inv_h[8];
    p.gradStops[0].color = Color(
        src_rect.left() / tex_w,
        src_rect.top() / tex_h,
        src_rect.right() / tex_w,
        src_rect.bottom() / tex_h
    );

    constexpr uint32_t kWhite = 0xffffffffu;
    const Point p0 = *p0_opt, p1 = *p1_opt, p2 = *p2_opt, p3 = *p3_opt;
    Vertex quad[6] = {
        Vertex(p0.x, p0.y, 0.0f, 0.0f, kWhite),
        Vertex(p3.x, p3.y, 0.0f, 1.0f, kWhite),
        Vertex(p2.x, p2.y, 1.0f, 1.0f, kWhite),
        Vertex(p0.x, p0.y, 0.0f, 0.0f, kWhite),
        Vertex(p2.x, p2.y, 1.0f, 1.0f, kWhite),
        Vertex(p1.x, p1.y, 1.0f, 0.0f, kWhite)
    };

    context_->save();
    context_->resetTransform();
    {
        ScopedBlendMode scopedBlend(context_.get(), blend_mode);
        context_->drawTriangles(p, quad, 6, 0.0f, 17 /* ShaderPerspectiveImg */);
    }
    context_->restore();
}

void GpuCanvas::draw_pixmap_3d(
    const PixmapRef& src,
    const Transform4x4& transform3d,
    float opacity,
    BlendMode blend_mode
) {
    if (src.width() == 0 || src.height() == 0) return;
    if (auto r = nisaba::Rect::from_xywh(0.0f, 0.0f, static_cast<float>(src.width()), static_cast<float>(src.height()))) {
        draw_pixmap_3d(src, *r, transform3d, opacity, blend_mode);
    }
}

void GpuCanvas::draw_pixmap_rect(
    const PixmapRef& src,
    const nisaba::Rect& src_rect,
    const nisaba::Rect& dst_rect,
    const PixmapPaint& paint
) {
    if (!context_ || src.width() == 0 || src.height() == 0 || !src.data()) return;
    if (src_rect.width() <= 0.0f || src_rect.height() <= 0.0f ||
        dst_rect.width() <= 0.0f || dst_rect.height() <= 0.0f) return;
    ensure_frame();

    int flags = ImageFlags::ImagePremultiplied;
    if (paint.quality == FilterQuality::Nearest) {
        flags |= ImageFlags::ImageNearest;
    } else if (src.width() > 1 && src.height() > 1 &&
               src_rect.width() >= static_cast<float>(src.width()) - 1.0f &&
               src_rect.height() >= static_cast<float>(src.height()) - 1.0f) {
        const auto [csx, csy] = current_transform_.get_scale();
        const float eff_sx = (dst_rect.width() * csx) / src_rect.width();
        const float eff_sy = (dst_rect.height() * csy) / src_rect.height();
        if (paint.quality == FilterQuality::Bicubic || eff_sx < 0.75f || eff_sy < 0.75f) {
            flags |= ImageFlags::ImageGenerateMipmaps;
        }
    }
    int img = get_or_upload_pixmap(src, flags);
    if (img > 0) {
        draw_image_rect_internal(
            context_.get(), img,
            static_cast<float>(src.width()), static_cast<float>(src.height()),
            src_rect, dst_rect, paint
        );
    }
}

void GpuCanvas::draw_gpu_texture(int32_t x, int32_t y, const std::shared_ptr<GpuTexture>& texture, const PixmapPaint& paint) {
    if (!context_ || !texture || texture->width() == 0 || texture->height() == 0) return;
    auto dst_r = nisaba::Rect::from_xywh(
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(texture->width()), static_cast<float>(texture->height())
    );
    if (dst_r) {
        draw_gpu_texture_rect(texture, *dst_r, paint);
    }
}

void GpuCanvas::draw_gpu_texture_rect(
    const std::shared_ptr<GpuTexture>& texture,
    const nisaba::Rect& src_rect,
    const nisaba::Rect& dst_rect,
    const PixmapPaint& paint
) {
    if (!context_ || !texture || texture->width() == 0 || texture->height() == 0) return;
    if (src_rect.width() <= 0.0f || src_rect.height() <= 0.0f ||
        dst_rect.width() <= 0.0f || dst_rect.height() <= 0.0f) return;
    ensure_frame();

    int flags = ImageFlags::ImagePremultiplied |
                ((paint.quality == FilterQuality::Nearest)
                    ? ImageFlags::ImageForceNearest
                    : ImageFlags::ImageForceBilinear);
    int img = context_->renderer()->createTextureFromNativeHandle(
        texture->id(),
        static_cast<int>(texture->width()),
        static_cast<int>(texture->height()),
        flags
    );
    if (img > 0) {
        temp_images_.push_back(img);
        draw_image_rect_internal(
            context_.get(), img,
            static_cast<float>(texture->width()), static_cast<float>(texture->height()),
            src_rect, dst_rect, paint
        );
    }
}

void GpuCanvas::draw_gpu_texture_rect(
    const std::shared_ptr<GpuTexture>& texture,
    const nisaba::Rect& dst_rect,
    const PixmapPaint& paint
) {
    if (!texture || texture->width() == 0 || texture->height() == 0) return;
    auto src_r = nisaba::Rect::from_xywh(
        0.0f, 0.0f,
        static_cast<float>(texture->width()),
        static_cast<float>(texture->height())
    );
    if (src_r) {
        draw_gpu_texture_rect(texture, *src_r, dst_rect, paint);
    }
}

void GpuCanvas::draw_vertices(const nisaba::Vertices& vertices, BlendMode blend_mode, float opacity) {
    if (!context_ || !vertices.is_valid()) return;
    ensure_frame();
    draw_vertices_internal(context_.get(), vertices, 0, 0.0f, 0.0f, blend_mode, opacity);
}

void GpuCanvas::draw_textured_vertices(
    const nisaba::Vertices& vertices,
    const std::shared_ptr<GpuTexture>& texture,
    BlendMode blend_mode,
    float opacity
) {
    if (!context_ || !vertices.is_valid()) return;
    if (!texture || texture->width() == 0 || texture->height() == 0) {
        draw_vertices(vertices, blend_mode, opacity);
        return;
    }
    ensure_frame();
    int img = context_->renderer()->createTextureFromNativeHandle(
        texture->id(),
        static_cast<int>(texture->width()),
        static_cast<int>(texture->height()),
        ImageFlags::ImagePremultiplied | ImageFlags::ImageForceBilinear
    );
    if (img > 0) {
        temp_images_.push_back(img);
        draw_vertices_internal(
            context_.get(), vertices, img,
            static_cast<float>(texture->width()), static_cast<float>(texture->height()),
            blend_mode, opacity
        );
    }
}

void GpuCanvas::draw_textured_vertices(
    const nisaba::Vertices& vertices,
    const PixmapRef& texture,
    BlendMode blend_mode,
    float opacity
) {
    if (!context_ || !vertices.is_valid()) return;
    if (texture.width() == 0 || texture.height() == 0 || !texture.data()) {
        draw_vertices(vertices, blend_mode, opacity);
        return;
    }
    ensure_frame();
    int img = get_or_upload_pixmap(texture, ImageFlags::ImagePremultiplied);
    if (img > 0) {
        draw_vertices_internal(
            context_.get(), vertices, img,
            static_cast<float>(texture.width()), static_cast<float>(texture.height()),
            blend_mode, opacity
        );
    }
}

void GpuCanvas::draw_round_rect_shadow(const nisaba::Rect& rect, float rx, float ry, const effects::DropShadow& shadow) {
    if (!context_ || rect.width() <= 0.0f || rect.height() <= 0.0f || shadow.color.alpha() <= 0.0f) return;
    if (rx <= 0.0f || ry <= 0.0f) {
        draw_rrect_shadow(rect, BorderRadius::zero(), shadow);
        return;
    }
    if (std::abs(rx - ry) <= 0.1f) {
        draw_rrect_shadow(rect, BorderRadius::all(rx), shadow);
        return;
    }
    if (auto p = PathBuilder::from_rounded_rect(rect, rx, ry)) {
        draw_path_shadow(*p, shadow, nisaba::FillRule::Winding);
    }
}

void GpuCanvas::draw_rrect_shadow(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const effects::DropShadow& shadow) {
    if (!context_ || rect.width() <= 0.0f || rect.height() <= 0.0f || shadow.color.alpha() <= 0.0f) return;
    ensure_frame();
    BorderRadius cr = radii.constrained_for_size(rect.width(), rect.height());
    context_->rrectShadow(
        rect.x() + shadow.dx,
        rect.y() + shadow.dy,
        rect.width(),
        rect.height(),
        cr.top_left,
        cr.top_right,
        cr.bottom_right,
        cr.bottom_left,
        shadow.sigma,
        nisaba::gpu::Color(shadow.color)
    );
}

void GpuCanvas::draw_rrect_shadow(const nisaba::RRect& rrect, const effects::DropShadow& shadow) {
    draw_rrect_shadow(rrect.rect, rrect.radii, shadow);
}

void GpuCanvas::draw_rect_shadow(const nisaba::Rect& rect, const effects::DropShadow& shadow) {
    draw_rrect_shadow(rect, BorderRadius::zero(), shadow);
}

void GpuCanvas::draw_circle_shadow(float cx, float cy, float radius, const effects::DropShadow& shadow) {
    if (!context_ || radius <= 0.0f || shadow.color.alpha() <= 0.0f) return;
    ensure_frame();
    context_->rrectShadow(
        cx - radius + shadow.dx,
        cy - radius + shadow.dy,
        radius * 2.0f,
        radius * 2.0f,
        radius, radius, radius, radius,
        shadow.sigma,
        nisaba::gpu::Color(shadow.color)
    );
}

void GpuCanvas::draw_path_shadow(const nisaba::Path& path, const effects::DropShadow& shadow, nisaba::FillRule fill_rule) {
    if (!context_ || path.is_empty() || shadow.color.alpha() <= 0.0f) return;
    nisaba::Paint sp(nisaba::Color::BLACK);
    sp.set_image_filter(effects::ImageFilter::drop_shadow(shadow.dx, shadow.dy, shadow.sigma, shadow.color, true));
    fill_path(path, sp, fill_rule);
}

void GpuCanvas::draw_path_with_shadow(const nisaba::Path& path, const nisaba::Paint& paint, const effects::DropShadow& shadow, nisaba::FillRule fill_rule) {
    draw_path_shadow(path, shadow, fill_rule);
    fill_path(path, paint, fill_rule);
}

static bool is_opaque_occluding_paint(const nisaba::Paint& paint) noexcept {
    if (!paint.is_solid_color() || paint.has_image_filter() || paint.has_color_filter()) return false;
    return paint.blend_mode == BlendMode::SourceOver && paint.shader.solid_color().alpha() >= 0.999f;
}

void GpuCanvas::draw_rect_with_shadow(const nisaba::Rect& rect, const nisaba::Paint& paint, const effects::DropShadow& shadow) {
    if (context_ && is_opaque_occluding_paint(paint) && rect.width() > 4.0f && rect.height() > 4.0f && shadow.color.alpha() > 0.0f) {
        ensure_frame();
        const float occ[4] = {rect.left() + 1.5f, rect.top() + 1.5f, rect.right() - 1.5f, rect.bottom() - 1.5f};
        context_->rrectShadow(rect.x() + shadow.dx, rect.y() + shadow.dy, rect.width(), rect.height(),
                              0.0f, 0.0f, 0.0f, 0.0f, shadow.sigma, nisaba::gpu::Color(shadow.color), occ);
    } else {
        draw_rect_shadow(rect, shadow);
    }
    fill_rect(rect, paint);
}

void GpuCanvas::draw_round_rect_with_shadow(const nisaba::Rect& rect, float rx, float ry, const nisaba::Paint& paint, const effects::DropShadow& shadow) {
    if (context_ && is_opaque_occluding_paint(paint) && std::abs(rx - ry) <= 0.1f && shadow.color.alpha() > 0.0f) {
        draw_rrect_with_shadow(RRect::from_rect_and_radius(rect, rx, ry), paint, shadow);
        return;
    }
    draw_round_rect_shadow(rect, rx, ry, shadow);
    fill_round_rect(rect, rx, ry, paint);
}

void GpuCanvas::draw_rrect_with_shadow(const nisaba::RRect& rrect, const nisaba::Paint& paint, const effects::DropShadow& shadow) {
    if (context_ && is_opaque_occluding_paint(paint) && !rrect.is_empty() && shadow.color.alpha() > 0.0f) {
        ensure_frame();
        const BorderRadius cr = rrect.radii.constrained_for_size(rrect.width(), rrect.height());
        const float rTop = std::max(cr.top_left, cr.top_right);
        const float rBot = std::max(cr.bottom_left, cr.bottom_right);
        const float rLeft = std::max(cr.top_left, cr.bottom_left);
        const float rRight = std::max(cr.top_right, cr.bottom_right);
        // Pick whichever inscribed rectangle (horizontal middle band vs vertical middle band) has larger area
        const float hW = std::max(0.0f, rrect.width() - 3.0f);
        const float hH = std::max(0.0f, rrect.height() - rTop - rBot - 3.0f);
        const float vW = std::max(0.0f, rrect.width() - rLeft - rRight - 3.0f);
        const float vH = std::max(0.0f, rrect.height() - 3.0f);
        float occ[4]{0.0f, 0.0f, 0.0f, 0.0f};
        bool hasOcc = false;
        if (hW * hH >= vW * vH && hW > 2.0f && hH > 2.0f) {
            occ[0] = rrect.left() + 1.5f;
            occ[1] = rrect.top() + rTop + 1.5f;
            occ[2] = rrect.right() - 1.5f;
            occ[3] = rrect.bottom() - rBot - 1.5f;
            hasOcc = true;
        } else if (vW > 2.0f && vH > 2.0f) {
            occ[0] = rrect.left() + rLeft + 1.5f;
            occ[1] = rrect.top() + 1.5f;
            occ[2] = rrect.right() - rRight - 1.5f;
            occ[3] = rrect.bottom() - 1.5f;
            hasOcc = true;
        }
        context_->rrectShadow(
            rrect.x() + shadow.dx, rrect.y() + shadow.dy, rrect.width(), rrect.height(),
            cr.top_left, cr.top_right, cr.bottom_right, cr.bottom_left,
            shadow.sigma, nisaba::gpu::Color(shadow.color),
            hasOcc ? occ : nullptr
        );
    } else {
        draw_rrect_shadow(rrect, shadow);
    }
    fill_rrect(rrect, paint);
}

void GpuCanvas::draw_circle_with_shadow(float cx, float cy, float radius, const nisaba::Paint& paint, const effects::DropShadow& shadow) {
    draw_circle_shadow(cx, cy, radius, shadow);
    fill_circle(cx, cy, radius, paint);
}

void GpuCanvas::draw_glow(const nisaba::Rect& rect, float rx, float ry, nisaba::Color glow_color, float radius) {
    effects::DropShadow shadow(0.0f, 0.0f, radius, glow_color);
    draw_round_rect_shadow(rect, rx, ry, shadow);
}

void GpuCanvas::invalidate_backdrop_cache() noexcept {
    backdrop_cache_.valid = false;
    backdrop_cache_.texture_id = 0;
    backdrop_cache_.img_handle = 0;
    backdrop_cache_.sigma = 0.0f;
}

void GpuCanvas::draw_backdrop_blur(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, float sigma) {
    if (!context_ || !surface_ || sigma <= 0.0f || rect.width() <= 0.0f || rect.height() <= 0.0f) return;

    int blur_img = 0;
    if (backdrop_cache_.valid && backdrop_cache_.img_handle > 0) {
        // High-Performance Glass Optimization:
        // Reuse cached blurred backdrop texture for all sibling glass panels in the active frame
        blur_img = backdrop_cache_.img_handle;
    } else {
        // 1. Flush all pending background primitives to the active GPU framebuffer
        if (in_frame_) {
            context_->endFrame();
            in_frame_ = false;
        }

        // 2. Capture and blur active backdrop in a single step (direct texture when non-MSAA, direct MSAA blit otherwise)
        if (!blur_engine_) {
            blur_engine_ = std::make_unique<DualKawaseBlur>();
        }

        DualKawaseConfig cfg;
        cfg.passes = std::clamp(static_cast<int>(std::ceil(sigma / 6.0f)), 1, 5);
        cfg.radius = std::max(1.5f, sigma / 4.0f);

        const int surfW = static_cast<int>(surface_->width());
        const int surfH = static_cast<int>(surface_->height());
        uint32_t blurred_tex = 0;
        if (surface_->samples() <= 1 && surface_->texture() != nullptr) {
            if (blur_engine_->init(surfW, surfH, cfg.passes)) {
                blurred_tex = blur_engine_->blur_texture(surface_->texture()->id(), surfW, surfH, cfg);
            }
        } else {
            blurred_tex = blur_engine_->blur_framebuffer(surface_->fbo_id(), surfW, surfH, cfg);
        }

        if (blurred_tex != 0) {
            blur_img = context_->renderer()->createTextureFromNativeHandle(
                blurred_tex,
                static_cast<int>(surface_->width()),
                static_cast<int>(surface_->height()),
                ImageFlags::ImageFlipY
            );

            if (blur_img > 0) {
                temp_images_.push_back(blur_img);
                backdrop_cache_.valid = true;
                backdrop_cache_.texture_id = blurred_tex;
                backdrop_cache_.img_handle = blur_img;
                backdrop_cache_.sigma = sigma;
            }
        }
    }

    // 3. Resume frame for drawing the blurred glass panel
    ensure_frame();

    if (blur_img > 0) {
        auto pattern = context_->imagePattern(
            0.0f, 0.0f,
            static_cast<float>(surface_->width()),
            static_cast<float>(surface_->height()),
            0.0f, blur_img, 1.0f
        );
        if (!current_transform_.is_identity()) {
            if (auto inv = current_transform_.invert()) {
                pattern.xform = Transform2D(*inv);
            }
        }

        BorderRadius cr = radii.constrained_for_size(rect.width(), rect.height());
        context_->beginPath();
        context_->roundedRectVarying(
            rect.x(), rect.y(), rect.width(), rect.height(),
            cr.top_left, cr.top_right, cr.bottom_right, cr.bottom_left
        );
        context_->fillPaint(pattern);
        context_->fill();
    }
}

void GpuCanvas::draw_backdrop_blur(const nisaba::Rect& rect, float sigmaX, float sigmaY) {
    float sigma = (sigmaY > 0.0f) ? std::max(sigmaX, sigmaY) : sigmaX;
    draw_backdrop_blur(rect, BorderRadius::all(0.0f), sigma);
}

void GpuCanvas::draw_backdrop_blur(const nisaba::RRect& rrect, float sigma) {
    draw_backdrop_blur(rrect.rect, rrect.radii, sigma);
}

void GpuCanvas::draw_glass_panel(const nisaba::Rect& rect, const nisaba::BorderRadius& radii, const effects::GlassParams& params) {
    if (!context_) return;
    ensure_frame();

    BorderRadius cr = radii.constrained_for_size(rect.width(), rect.height());

    // 1. Drop shadow if present
    if (params.shadow.has_value()) {
        draw_rrect_shadow(rect, cr, *params.shadow);
    }

    // 2. Hardware GPU Backdrop Blur if blur_sigma > 0
    if (params.blur_sigma > 0.0f) {
        draw_backdrop_blur(rect, cr, params.blur_sigma);
    }

    // 3. Glass tinted background
    if (params.tint_color.alpha() > 0.0f) {
        context_->beginPath();
        context_->roundedRectVarying(rect.x(), rect.y(), rect.width(), rect.height(),
                                     cr.top_left, cr.top_right, cr.bottom_right, cr.bottom_left);
        context_->fillColor(params.tint_color);
        context_->fill();
    }

    // 4. Crisp specular border stroke
    if (params.border_width > 0.0f && params.border_color.alpha() > 0.0f) {
        if (cr.is_uniform()) {
            stroke_round_rect(rect, cr.top_left, cr.top_left, nisaba::Paint(params.border_color), Stroke(params.border_width));
        } else if (auto path = PathBuilder::from_rounded_rect(rect, cr)) {
            stroke_path(*path, nisaba::Paint(params.border_color), Stroke(params.border_width));
        }
    }
}

void GpuCanvas::draw_glass_panel(const nisaba::RRect& rrect, const effects::GlassParams& params) {
    draw_glass_panel(rrect.rect, rrect.radii, params);
}

void GpuCanvas::draw_glass_panel(const nisaba::Rect& rect, float rx, float ry, const effects::GlassParams& params) {
    draw_glass_panel(rect, BorderRadius::all(std::min(rx, ry)), params);
}

void GpuCanvas::draw_svg(const svg::SvgDocument& doc, const nisaba::Rect& dest_bounds) {
    doc.render_gpu(*this, &dest_bounds);
}

void GpuCanvas::draw_svg(const svg::SvgDocument& doc, const nisaba::Rect& dest_bounds, std::optional<nisaba::Color> tint) {
    doc.render_gpu(*this, &dest_bounds, tint);
}

void GpuCanvas::draw_svg(const svg::SvgDocument& doc, const nisaba::Rect& dest_bounds, const svg::SvgTheme& theme) {
    doc.render_gpu(*this, &dest_bounds, theme);
}

void GpuCanvas::flush_internal(const ScreenIntRect* damage_rect, const std::vector<ScreenIntRect>* damage_rects) {
    invalidate_backdrop_cache();
    if (context_ && in_frame_) {
        // Keep in_frame_ active across mid-frame flushes so Context state & stencil clip depth persist.
        context_->endFrame();
        needs_surface_bind_ = true;
    }
    if (surface_) {
        if (damage_rect) {
            surface_->resolve(*damage_rect);
        } else if (damage_rects) {
            surface_->resolve(*damage_rects);
        } else {
            surface_->resolve();
        }
    }
    if (context_) {
        for (int img : temp_images_) {
            context_->deleteImage(img);
        }
        temp_images_.clear();
    }
    ++frame_counter_;
}

void GpuCanvas::flush() {
    flush_internal(nullptr, nullptr);
}

void GpuCanvas::flush(const ScreenIntRect& damage_rect) {
    flush_internal(&damage_rect, nullptr);
}

void GpuCanvas::flush(const std::vector<ScreenIntRect>& damage_rects) {
    flush_internal(nullptr, &damage_rects);
}

void GpuCanvas::draw_text(std::string_view text, float x, float y, const text::Font& font, const nisaba::Paint& paint, float font_size) {
    if (!context_ || text.empty()) return;
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    apply_fill_paint(paint);
    context_->textWithFont(x, y, font, font_size, text.data(), text.data() + text.size());
}

void GpuCanvas::draw_text(float x, float y, std::string_view text, const nisaba::Paint& paint, float font_size, const char* font_name) {
    if (!context_ || text.empty()) return;
    ensure_frame();
    ScopedBlendMode scopedBlend(context_.get(), paint.blend_mode);
    apply_fill_paint(paint);
    context_->fontSize(font_size);
    if (font_name && font_name[0] != '\0') {
        context_->fontFace(font_name);
    }
    context_->text(x, y, text.data(), text.data() + text.size());
}

void GpuCanvas::draw_text_buffer(const text::Buffer& buffer, text::FontSystem& fonts, Point pos, nisaba::Color default_color) {
    if (!context_) return;
    ensure_frame();

    bool need_shaping = false;
    for (const auto& line : buffer.lines()) {
        if (!line.is_shaped()) {
            need_shaping = true;
            break;
        }
    }
    if (need_shaping) {
        const_cast<text::Buffer&>(buffer).shape_until_scroll(fonts);
    }

    auto runs = buffer.layout_runs();
    if (runs.empty()) return;

    bool has_rich_bg = false;
    bool has_rich_fg = false;
    for (const auto& run : runs) {
        for (const auto& g : run.glyphs) {
            if (g.background_paint_opt != nullptr) has_rich_bg = true;
            if (!g.is_placeholder && g.foreground_paint_opt != nullptr &&
                text::paint_requires_vector_path(*g.foreground_paint_opt)) {
                has_rich_fg = true;
            }
        }
    }

    if (!has_rich_bg && !has_rich_fg) {
        context_->drawTextBuffer(buffer, fonts, pos, default_color);
        return;
    }

    // Pass 1: Span-merged backgrounds (handles both rich Paint and solid TextColor)
    for (const auto& run : runs) {
        if (run.line_height <= 0.0f) continue;
        bool bg_active = false;
        float bg_x0 = 0.0f, bg_x1 = 0.0f;
        const nisaba::Paint* bg_paint_ptr = nullptr;
        bool bg_has_solid = false;
        text::TextColor bg_solid{};

        auto flush_bg = [&]() {
            if (!bg_active || bg_x1 <= bg_x0) return;
            if (auto r = nisaba::Rect::from_xywh(bg_x0, pos.y + run.line_top, bg_x1 - bg_x0, run.line_height)) {
                if (bg_paint_ptr != nullptr) {
                    fill_rect(*r, *bg_paint_ptr);
                } else if (bg_has_solid) {
                    fill_rect(*r, nisaba::Paint(bg_solid.to_color()));
                }
            }
            bg_active = false;
            bg_paint_ptr = nullptr;
            bg_has_solid = false;
        };

        for (const auto& glyph : run.glyphs) {
            if (glyph.is_mark && glyph.w <= 0.0f) continue;
            const bool has_bg = (glyph.background_paint_opt != nullptr || glyph.background_color_opt.has_value()) && glyph.w > 0.0f;
            if (!has_bg) {
                flush_bg();
                continue;
            }
            const float gx0 = pos.x + glyph.x;
            const float gx1 = pos.x + glyph.x + glyph.w;
            const nisaba::Paint* gp = glyph.background_paint_opt.get();
            const bool g_has_solid = (gp == nullptr && glyph.background_color_opt.has_value());
            const text::TextColor gs = g_has_solid ? *glyph.background_color_opt : text::TextColor{};
            if (!bg_active) {
                bg_active = true;
                bg_x0 = gx0;
                bg_x1 = gx1;
                bg_paint_ptr = gp;
                bg_has_solid = g_has_solid;
                bg_solid = gs;
            } else if (gx0 <= bg_x1 + 1.5f && gx1 >= bg_x0 - 1.5f &&
                       bg_paint_ptr == gp && bg_has_solid == g_has_solid && (!g_has_solid || bg_solid == gs)) {
                bg_x0 = std::min(bg_x0, gx0);
                bg_x1 = std::max(bg_x1, gx1);
            } else {
                flush_bg();
                bg_active = true;
                bg_x0 = gx0;
                bg_x1 = gx1;
                bg_paint_ptr = gp;
                bg_has_solid = g_has_solid;
                bg_solid = gs;
            }
        }
        flush_bg();
    }

    // Pass 1.5 + 2a: Shadows and solid-color glyphs via GlyphAtlas batch
    context_->drawTextBuffer(buffer, fonts, pos, default_color, 0x02u | 0x04u);

    // Pass 2b: Rich Paint foreground glyphs via GPU vector path pipeline
    if (has_rich_fg) {
        std::vector<text::ColorGlyphLayer> color_layers;
        for (const auto& run : runs) {
            const float baseline_offset = (run.max_ascent > 0.0f) ? run.max_ascent : buffer.metrics().font_size;
            const float line_baseline = pos.y + run.line_top + baseline_offset;
            for (const auto& glyph : run.glyphs) {
                if (glyph.is_placeholder || glyph.glyph_id == 0 || !glyph.foreground_paint_opt) continue;
                const nisaba::Paint& fg_paint = *glyph.foreground_paint_opt;
                if (!text::paint_requires_vector_path(fg_paint)) continue;
                const text::TtfFont* font = fonts.get_font(glyph.font_id);
                if (!font && fonts.default_font_id()) {
                    font = fonts.get_font(*fonts.default_font_id());
                }
                if (!font) continue;

                const float gx = pos.x + glyph.x + glyph.x_offset;
                const float gy = line_baseline + glyph.y + glyph.y_offset;
                const nisaba::Stroke stroke = glyph.foreground_stroke_opt
                    ? *glyph.foreground_stroke_opt
                    : nisaba::Stroke(fg_paint.get_stroke_width());
                const std::span<const text::FontVariation> vars = glyph.variation_coords_opt
                    ? std::span<const text::FontVariation>(*glyph.variation_coords_opt)
                    : std::span<const text::FontVariation>{};

                if (font->has_color_glyphs() && font->get_color_glyph_layers(glyph.glyph_id, color_layers)) {
                    for (const auto& layer : color_layers) {
                        nisaba::Path lpath;
                        const bool ok = (!vars.empty() && font->is_variable_font())
                            ? font->get_glyph_path_varied(layer.glyph_id, lpath, glyph.font_size, vars,
                                                          glyph.synthetic_bold, glyph.synthetic_italic)
                            : font->get_glyph_path_styled(layer.glyph_id, lpath, glyph.font_size,
                                                          glyph.synthetic_bold, glyph.synthetic_italic);
                        if (!ok) continue;
                        lpath.apply_transform(nisaba::Transform::from_translate(gx, gy));
                        if (layer.use_foreground) {
                            draw_path(lpath, fg_paint, stroke, nisaba::FillRule::Winding);
                        } else {
                            nisaba::Paint lp(layer.color.to_color());
                            lp.blend_mode = fg_paint.blend_mode;
                            fill_path(lpath, lp, nisaba::FillRule::Winding);
                        }
                    }
                    continue;
                }

                nisaba::Path gpath;
                const bool ok = (!vars.empty() && font->is_variable_font())
                    ? font->get_glyph_path_varied(glyph.glyph_id, gpath, glyph.font_size, vars,
                                                  glyph.synthetic_bold, glyph.synthetic_italic)
                    : font->get_glyph_path_styled(glyph.glyph_id, gpath, glyph.font_size,
                                                  glyph.synthetic_bold, glyph.synthetic_italic);
                if (!ok) continue;
                gpath.apply_transform(nisaba::Transform::from_translate(gx, gy));
                draw_path(gpath, fg_paint, stroke, nisaba::FillRule::Winding);
            }
        }
    }

    // Pass 3: Decorations on top (with DecorationMode::Gaps)
    context_->drawTextBuffer(buffer, fonts, pos, default_color, 0x08u);
}

void GpuCanvas::draw_text_buffer(const text::Buffer& buffer, text::FontSystem& fonts, Point pos, const nisaba::Paint& paint) {
    if (!context_) return;
    ensure_frame();

    if (!text::paint_requires_vector_path(paint)) {
        draw_text_buffer(buffer, fonts, pos, paint.shader.solid_color());
        return;
    }

    bool need_shaping = false;
    for (const auto& line : buffer.lines()) {
        if (!line.is_shaped()) {
            need_shaping = true;
            break;
        }
    }
    if (need_shaping) {
        const_cast<text::Buffer&>(buffer).shape_until_scroll(fonts);
    }

    auto runs = buffer.layout_runs();
    if (runs.empty()) return;

    const nisaba::Color fallback_color = paint.is_solid_color() ? paint.shader.solid_color() : nisaba::Color::WHITE;

    // Pass 1: Span-merged backgrounds
    for (const auto& run : runs) {
        if (run.line_height <= 0.0f) continue;
        bool bg_active = false;
        float bg_x0 = 0.0f, bg_x1 = 0.0f;
        const nisaba::Paint* bg_paint_ptr = nullptr;
        bool bg_has_solid = false;
        text::TextColor bg_solid{};
        auto flush_bg = [&]() {
            if (!bg_active || bg_x1 <= bg_x0) return;
            if (auto r = nisaba::Rect::from_xywh(bg_x0, pos.y + run.line_top, bg_x1 - bg_x0, run.line_height)) {
                if (bg_paint_ptr != nullptr) fill_rect(*r, *bg_paint_ptr);
                else if (bg_has_solid) fill_rect(*r, nisaba::Paint(bg_solid.to_color()));
            }
            bg_active = false;
            bg_paint_ptr = nullptr;
            bg_has_solid = false;
        };
        for (const auto& glyph : run.glyphs) {
            if (glyph.is_mark && glyph.w <= 0.0f) continue;
            const bool has_bg = (glyph.background_paint_opt != nullptr || glyph.background_color_opt.has_value()) && glyph.w > 0.0f;
            if (!has_bg) { flush_bg(); continue; }
            const float gx0 = pos.x + glyph.x;
            const float gx1 = pos.x + glyph.x + glyph.w;
            const nisaba::Paint* gp = glyph.background_paint_opt.get();
            const bool g_has_solid = (gp == nullptr && glyph.background_color_opt.has_value());
            const text::TextColor gs = g_has_solid ? *glyph.background_color_opt : text::TextColor{};
            if (!bg_active) {
                bg_active = true; bg_x0 = gx0; bg_x1 = gx1; bg_paint_ptr = gp; bg_has_solid = g_has_solid; bg_solid = gs;
            } else if (gx0 <= bg_x1 + 1.5f && gx1 >= bg_x0 - 1.5f &&
                       bg_paint_ptr == gp && bg_has_solid == g_has_solid && (!g_has_solid || bg_solid == gs)) {
                bg_x0 = std::min(bg_x0, gx0); bg_x1 = std::max(bg_x1, gx1);
            } else {
                flush_bg();
                bg_active = true; bg_x0 = gx0; bg_x1 = gx1; bg_paint_ptr = gp; bg_has_solid = g_has_solid; bg_solid = gs;
            }
        }
        flush_bg();
    }

    // Pass 1.5: Shadows
    context_->drawTextBuffer(buffer, fonts, pos, fallback_color, 0x02u);

    // Pass 2: Glyphs (span Paint overrides default Paint; span solid TextColor uses solid fill)
    std::vector<text::ColorGlyphLayer> color_layers;
    for (const auto& run : runs) {
        const float baseline_offset = (run.max_ascent > 0.0f) ? run.max_ascent : buffer.metrics().font_size;
        const float line_baseline = pos.y + run.line_top + baseline_offset;
        for (const auto& glyph : run.glyphs) {
            if (glyph.is_placeholder || glyph.glyph_id == 0) continue;
            const text::TtfFont* font = fonts.get_font(glyph.font_id);
            if (!font && fonts.default_font_id()) font = fonts.get_font(*fonts.default_font_id());
            if (!font) continue;

            const float gx = pos.x + glyph.x + glyph.x_offset;
            const float gy = line_baseline + glyph.y + glyph.y_offset;

            nisaba::Paint solid_span_paint;
            const nisaba::Paint* active_paint = nullptr;
            if (glyph.foreground_paint_opt) {
                active_paint = glyph.foreground_paint_opt.get();
            } else if (glyph.color_opt.has_value()) {
                solid_span_paint.set_color(glyph.color_opt->to_color());
                active_paint = &solid_span_paint;
            } else {
                active_paint = &paint;
            }

            const nisaba::Stroke stroke = glyph.foreground_stroke_opt
                ? *glyph.foreground_stroke_opt
                : nisaba::Stroke(active_paint->get_stroke_width());
            const std::span<const text::FontVariation> vars = glyph.variation_coords_opt
                ? std::span<const text::FontVariation>(*glyph.variation_coords_opt)
                : std::span<const text::FontVariation>{};

            nisaba::Path gpath;
            const bool ok = (!vars.empty() && font->is_variable_font())
                ? font->get_glyph_path_varied(glyph.glyph_id, gpath, glyph.font_size, vars,
                                              glyph.synthetic_bold, glyph.synthetic_italic)
                : font->get_glyph_path_styled(glyph.glyph_id, gpath, glyph.font_size,
                                              glyph.synthetic_bold, glyph.synthetic_italic);
            if (!ok) continue;
            gpath.apply_transform(nisaba::Transform::from_translate(gx, gy));
            draw_path(gpath, *active_paint, stroke, nisaba::FillRule::Winding);
        }
    }

    // Pass 3: Decorations
    context_->drawTextBuffer(buffer, fonts, pos, fallback_color, 0x08u);
}

int GpuCanvas::create_font(const char* name, const char* filename) {
    return context_ ? context_->createFont(name, filename) : -1;
}

int GpuCanvas::create_font_mem(const char* name, const uint8_t* data, size_t size) {
    return context_ ? context_->createFontMem(name, const_cast<unsigned char*>(data), static_cast<int>(size), 0) : -1;
}

} // namespace nisaba::gpu
