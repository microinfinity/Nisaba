/// @file wayland_surface.cpp
/// @brief Wayland Layer Surface implementation with wlr-layer-shell protocol and EGL.

#include "nisaba/backend_os/wayland/wayland_surface.hpp"
#include <iostream>

namespace nisaba::backend_os::wayland {

// ── Protocol Listeners ───────────────────────────────────────────

static void layer_surface_configure_handler(void* data, [[maybe_unused]] zwlr_layer_surface_v1* surface,
                                            uint32_t serial, uint32_t width, uint32_t height) {
    auto* self = static_cast<WaylandLayerSurface*>(data);
    self->handleConfigure(serial, width, height);
}

static void layer_surface_closed_handler(void* data, [[maybe_unused]] zwlr_layer_surface_v1* surface) {
    auto* self = static_cast<WaylandLayerSurface*>(data);
    self->handleClosed();
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure_handler,
    .closed    = layer_surface_closed_handler,
};

// ════════════════════════════════════════════════════════════════
// WaylandLayerSurface Implementation
// ════════════════════════════════════════════════════════════════

WaylandLayerSurface::WaylandLayerSurface(WaylandPlatformBackend& backend, LayerSurfaceConfig config)
    : backend_(backend), config_(config) {
    current_width_  = config_.width > 0 ? config_.width : 1920;
    current_height_ = config_.height > 0 ? config_.height : 34;
}

WaylandLayerSurface::~WaylandLayerSurface() {
    backend_.unregisterSurface(this);

    if (backend_.getEGLDisplay() != EGL_NO_DISPLAY) {
        if (egl_surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(backend_.getEGLDisplay(), egl_surface_);
            egl_surface_ = EGL_NO_SURFACE;
        }
    }

    if (egl_window_) {
        wl_egl_window_destroy(egl_window_);
        egl_window_ = nullptr;
    }
    if (layer_surface_) {
        zwlr_layer_surface_v1_destroy(layer_surface_);
        layer_surface_ = nullptr;
    }
    if (wl_surface_) {
        wl_surface_destroy(wl_surface_);
        wl_surface_ = nullptr;
    }
}

bool WaylandLayerSurface::init() {
    auto* compositor = backend_.getCompositor();
    auto* layer_shell = backend_.getLayerShell();
    egl_display_ = backend_.getEGLDisplay();
    auto egl_cfg = backend_.getEGLConfig();

    if (!compositor || !layer_shell || egl_display_ == EGL_NO_DISPLAY || !egl_cfg) {
        std::cerr << "[Nisaba Wayland] Missing compositor or layer-shell interface\n";
        return false;
    }

    // 1. Create base Wayland surface
    wl_surface_ = wl_compositor_create_surface(compositor);
    if (!wl_surface_) {
        std::cerr << "[Nisaba Wayland] Failed to create wl_surface\n";
        return false;
    }

    // 2. Create Layer Shell surface
    uint32_t layer_val = static_cast<uint32_t>(config_.layer);
    wl_output* target_wl_output = config_.target_output ? static_cast<wl_output*>(config_.target_output->nativeHandle()) : nullptr;
    layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell,
        wl_surface_,
        target_wl_output,
        layer_val,
        config_.namespace_id.c_str()
    );

    if (!layer_surface_) {
        std::cerr << "[Nisaba Wayland] Failed to create zwlr_layer_surface_v1\n";
        return false;
    }

    zwlr_layer_surface_v1_add_listener(layer_surface_, &layer_surface_listener, this);

    // Apply configuration
    zwlr_layer_surface_v1_set_size(layer_surface_, config_.width, config_.height);
    zwlr_layer_surface_v1_set_anchor(layer_surface_, static_cast<uint32_t>(config_.anchor));
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, config_.exclusive_zone);
    zwlr_layer_surface_v1_set_margin(layer_surface_,
                                     config_.margin.top,
                                     config_.margin.right,
                                     config_.margin.bottom,
                                     config_.margin.left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface_,
                                                    static_cast<uint32_t>(config_.keyboard_mode));

    // Commit initial state & wait for compositor's first configure event
    wl_surface_commit(wl_surface_);
    wl_display_roundtrip(backend_.getDisplay());

    // 3. Create Wayland EGL Window & EGL Surface
    egl_window_ = wl_egl_window_create(wl_surface_, current_width_, current_height_);
    if (!egl_window_) {
        std::cerr << "[Nisaba Wayland] Failed to create wl_egl_window\n";
        return false;
    }

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_cfg, (EGLNativeWindowType)egl_window_, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        std::cerr << "[Nisaba Wayland] Failed to create EGL surface for Wayland\n";
        return false;
    }

    // 4. Use shared EGL Context from backend
    egl_context_ = backend_.getEGLContext();
    if (egl_context_ == EGL_NO_CONTEXT) {
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        egl_context_ = eglCreateContext(egl_display_, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (egl_context_ == EGL_NO_CONTEXT) {
            const EGLint gles_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            egl_context_ = eglCreateContext(egl_display_, egl_cfg, EGL_NO_CONTEXT, gles_attribs);
        }
        if (egl_context_ == EGL_NO_CONTEXT) {
            egl_context_ = eglCreateContext(egl_display_, egl_cfg, EGL_NO_CONTEXT, nullptr);
        }
    }

    if (egl_context_ == EGL_NO_CONTEXT) {
        std::cerr << "[Nisaba Wayland] Failed to create EGL context\n";
        return false;
    }

    // Make current & set VSync
    makeCurrent();
    eglSwapInterval(egl_display_, config_.vsync ? 1 : 0);

    backend_.registerSurface(this);
    return true;
}

void WaylandLayerSurface::handleConfigure(uint32_t serial, uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(layer_surface_, serial);

    if (width > 0 && height > 0) {
        if (current_width_ != static_cast<int32_t>(width) || current_height_ != static_cast<int32_t>(height)) {
            current_width_  = width;
            current_height_ = height;
            if (egl_window_) {
                wl_egl_window_resize(egl_window_, current_width_, current_height_, 0, 0);
            }
            on_resize_.emit(current_width_, current_height_);
        }
    }
    configured_ = true;
}

void WaylandLayerSurface::handleClosed() {
    on_close_.emit();
}

void WaylandLayerSurface::setSize(int32_t width, int32_t height) {
    config_.width = width;
    config_.height = height;
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_size(layer_surface_, width, height);
        wl_surface_commit(wl_surface_);
    }
}

void WaylandLayerSurface::setLayer(ShellLayer layer) {
    config_.layer = layer;
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_layer(layer_surface_, static_cast<uint32_t>(layer));
        wl_surface_commit(wl_surface_);
    }
}

void WaylandLayerSurface::setAnchor(ShellAnchor anchor) {
    config_.anchor = anchor;
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_anchor(layer_surface_, static_cast<uint32_t>(anchor));
        wl_surface_commit(wl_surface_);
    }
}

void WaylandLayerSurface::setExclusiveZone(int32_t zone) {
    config_.exclusive_zone = zone;
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, zone);
        wl_surface_commit(wl_surface_);
    }
}

void WaylandLayerSurface::setMargin(const SurfaceMargin& margin) {
    config_.margin = margin;
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_margin(layer_surface_, margin.top, margin.right, margin.bottom, margin.left);
        wl_surface_commit(wl_surface_);
    }
}

void WaylandLayerSurface::setKeyboardMode(KeyboardMode mode) {
    config_.keyboard_mode = mode;
    if (layer_surface_) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface_, static_cast<uint32_t>(mode));
        wl_surface_commit(wl_surface_);
    }
}

void WaylandLayerSurface::setInputRegion(const std::vector<RegionRect>& rects) {
    auto* compositor = backend_.getCompositor();
    if (!compositor || !wl_surface_) return;

    struct wl_region* region = wl_compositor_create_region(compositor);
    if (!region) return;

    for (const auto& r : rects) {
        if (r.width > 0 && r.height > 0) {
            wl_region_add(region, r.x, r.y, r.width, r.height);
        }
    }

    wl_surface_set_input_region(wl_surface_, region);
    wl_region_destroy(region);
    wl_surface_commit(wl_surface_);
}

void WaylandLayerSurface::resetInputRegion() {
    if (!wl_surface_) return;
    wl_surface_set_input_region(wl_surface_, nullptr);
    wl_surface_commit(wl_surface_);
}

Size WaylandLayerSurface::getSize() const {
    return Size{static_cast<float>(current_width_), static_cast<float>(current_height_)};
}

Size WaylandLayerSurface::getDrawableSize() const {
    return Size{static_cast<float>(current_width_), static_cast<float>(current_height_)};
}

float WaylandLayerSurface::getDpiScale() const {
    return scale_factor_;
}

void WaylandLayerSurface::makeCurrent() {
    if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE && egl_context_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
    }
}

void WaylandLayerSurface::swapBuffers() {
    if (!configured_) {
        return;
    }
    if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(egl_display_, egl_surface_);
    }
}

} // namespace nisaba::backend_os::wayland
