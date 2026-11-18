#pragma once
/// @file x11_layer_surface.hpp
/// @brief X11 + EGL + EWMH + XShape Layer Surface implementation.
/// Provides desktop/dock/overlay layers, _NET_WM_STRUT_PARTIAL exclusive zones,
/// and ShapeInput click-through regions for X11 window managers.

#include "nisaba/backend_os/x11/x11_platform.hpp"
#include "nisaba/backend_os/layer_surface.hpp"

#include <X11/Xlib.h>
#include <EGL/egl.h>

namespace nisaba::backend_os::x11 {

class X11LayerSurface final : public LayerSurface {
public:
    X11LayerSurface(X11PlatformBackend& backend, LayerSurfaceConfig config);
    ~X11LayerSurface() override;

    bool init();

    void setSize(int32_t width, int32_t height) override;
    void setLayer(ShellLayer layer) override;
    void setAnchor(ShellAnchor anchor) override;
    void setExclusiveZone(int32_t zone) override;
    void setMargin(const SurfaceMargin& margin) override;
    void setKeyboardMode(KeyboardMode mode) override;

    void setInputRegion(const std::vector<RegionRect>& rects) override;
    void resetInputRegion() override;

    [[nodiscard]] Size  getSize() const override;
    [[nodiscard]] Size  getDrawableSize() const override;
    [[nodiscard]] float getDpiScale() const override;

    void makeCurrent() override;
    void swapBuffers() override;

    [[nodiscard]] void* getNativeHandle() const override { return (void*)(uintptr_t)x11_window_; }
    [[nodiscard]] void* getEGLSurface()   const override { return (void*)egl_surface_; }
    [[nodiscard]] void* getEGLContext()   const override { return (void*)egl_context_; }
    [[nodiscard]] void* getBackendLayer() const override { return const_cast<X11LayerSurface*>(this); }

    void handleConfigure(int width, int height);
    void handleButtonFocus();

private:
    void resolveMonitorRect(int& mx, int& my, int& mw, int& mh) const;
    void computeTargetGeometry(int& out_x, int& out_y, int& out_w, int& out_h) const;
    void updateGeometry();
    void applyWindowTypeAndLayer();
    void applyStruts();
    void applyKeyboardHints();
    void sendNetWmState(bool add, Atom prop1, Atom prop2 = 0L);

    X11PlatformBackend& backend_;
    LayerSurfaceConfig  config_;

    ::Display* display_     = nullptr;
    ::Window   x11_window_  = 0;
    Colormap   colormap_    = 0;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLContext egl_context_ = EGL_NO_CONTEXT;

    int   current_x_      = 0;
    int   current_y_      = 0;
    int   current_width_  = 0;
    int   current_height_ = 0;
    float scale_factor_   = 1.0f;

    bool shape_supported_  = false;
    bool grabbed_exclusive_ = false;
    bool mapped_           = false;
};

} // namespace nisaba::backend_os::x11
