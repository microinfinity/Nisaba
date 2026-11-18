/// @file x11_layer_surface.cpp
/// @brief X11 Layer Surface implementation with EWMH desktop/dock/strut hints,
///        32-bit ARGB EGL transparency, and XShape ShapeInput click-through regions.

#include "nisaba/backend_os/x11/x11_layer_surface.hpp"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef NISABA_BACKEND_OS_HAS_XSHAPE
#include <X11/extensions/shape.h>
#endif

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace nisaba::backend_os::x11 {

X11LayerSurface::X11LayerSurface(X11PlatformBackend& backend, LayerSurfaceConfig config)
    : backend_(backend), config_(std::move(config)) {
    current_width_  = config_.width > 0 ? config_.width : 1920;
    current_height_ = config_.height > 0 ? config_.height : 34;
    if (config_.target_output) {
        scale_factor_ = config_.target_output->scaleFactor();
    } else if (auto primary = backend_.getPrimaryOutput()) {
        scale_factor_ = primary->scaleFactor();
    }
    if (scale_factor_ < 1.0f) scale_factor_ = 1.0f;
}

X11LayerSurface::~X11LayerSurface() {
    if (grabbed_exclusive_ && display_) {
        XUngrabKeyboard(display_, CurrentTime);
        XUngrabPointer(display_, CurrentTime);
        grabbed_exclusive_ = false;
    }

    backend_.unregisterLayerSurface(this);

    if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE) {
        if (eglGetCurrentSurface(EGL_DRAW) == egl_surface_) {
            eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }

    if (display_) {
        if (x11_window_) {
            XDestroyWindow(display_, x11_window_);
            x11_window_ = 0;
        }
        if (colormap_) {
            XFreeColormap(display_, colormap_);
            colormap_ = 0;
        }
        XFlush(display_);
    }
}

void X11LayerSurface::resolveMonitorRect(int& mx, int& my, int& mw, int& mh) const {
    if (config_.target_output) {
        auto g = config_.target_output->logicalGeometry();
        if (g.width > 0 && g.height > 0) {
            mx = g.x;
            my = g.y;
            mw = g.width;
            mh = g.height;
            return;
        }
    }
    if (auto primary = backend_.getPrimaryOutput()) {
        auto g = primary->logicalGeometry();
        if (g.width > 0 && g.height > 0) {
            mx = g.x;
            my = g.y;
            mw = g.width;
            mh = g.height;
            return;
        }
    }
    int screen = backend_.getDefaultScreen();
    mx = 0;
    my = 0;
    mw = DisplayWidth(display_, screen);
    mh = DisplayHeight(display_, screen);
}

void X11LayerSurface::computeTargetGeometry(int& out_x, int& out_y, int& out_w, int& out_h) const {
    int mx = 0, my = 0, mw = 1920, mh = 1080;
    resolveMonitorRect(mx, my, mw, mh);

    const bool left   = hasAnchor(config_.anchor, ShellAnchor::Left);
    const bool right  = hasAnchor(config_.anchor, ShellAnchor::Right);
    const bool top    = hasAnchor(config_.anchor, ShellAnchor::Top);
    const bool bottom = hasAnchor(config_.anchor, ShellAnchor::Bottom);

    if (config_.width > 0) {
        out_w = config_.width;
    } else if (left && right) {
        out_w = std::max(1, mw - config_.margin.left - config_.margin.right);
    } else {
        out_w = mw;
    }

    if (config_.height > 0) {
        out_h = config_.height;
    } else if (top && bottom) {
        out_h = std::max(1, mh - config_.margin.top - config_.margin.bottom);
    } else {
        out_h = mh;
    }

    if (left && right) {
        out_x = mx + config_.margin.left;
    } else if (left) {
        out_x = mx + config_.margin.left;
    } else if (right) {
        out_x = mx + mw - config_.margin.right - out_w;
    } else {
        out_x = mx + (mw - out_w) / 2;
    }

    if (top && bottom) {
        out_y = my + config_.margin.top;
    } else if (top) {
        out_y = my + config_.margin.top;
    } else if (bottom) {
        out_y = my + mh - config_.margin.bottom - out_h;
    } else {
        out_y = my + (mh - out_h) / 2;
    }
}

bool X11LayerSurface::init() {
    display_     = backend_.getDisplay();
    egl_display_ = backend_.getEGLDisplay();
    egl_context_ = backend_.getEGLContext();
    EGLConfig egl_cfg = backend_.getEGLConfig();

    if (!display_ || egl_display_ == EGL_NO_DISPLAY || !egl_cfg || egl_context_ == EGL_NO_CONTEXT) {
        std::cerr << "[Nisaba X11LayerSurface] Missing X11 display or EGL context\n";
        return false;
    }

#ifdef NISABA_BACKEND_OS_HAS_XSHAPE
    int shape_ev = 0, shape_err = 0;
    shape_supported_ = (XShapeQueryExtension(display_, &shape_ev, &shape_err) != 0);
#endif

    int screen = backend_.getDefaultScreen();
    ::Window root = RootWindow(display_, screen);

    computeTargetGeometry(current_x_, current_y_, current_width_, current_height_);

    EGLint visual_id = 0;
    eglGetConfigAttrib(egl_display_, egl_cfg, EGL_NATIVE_VISUAL_ID, &visual_id);

    Visual* visual = nullptr;
    int depth = 0;
    if (visual_id != 0) {
        XVisualInfo tmpl{};
        tmpl.visualid = static_cast<VisualID>(visual_id);
        int n = 0;
        XVisualInfo* list = XGetVisualInfo(display_, VisualIDMask, &tmpl, &n);
        if (list && n > 0) {
            visual = list[0].visual;
            depth  = list[0].depth;
        }
        if (list) XFree(list);
    }
    if (!visual) {
        visual = DefaultVisual(display_, screen);
        depth  = DefaultDepth(display_, screen);
    }

    colormap_ = XCreateColormap(display_, root, visual, AllocNone);

    XSetWindowAttributes swa{};
    swa.colormap          = colormap_;
    swa.border_pixel      = 0;
    swa.background_pixmap = 0L;
    swa.bit_gravity       = NorthWestGravity;
    swa.event_mask        = ExposureMask | StructureNotifyMask |
                            ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                            EnterWindowMask | LeaveWindowMask |
                            KeyPressMask | KeyReleaseMask |
                            FocusChangeMask | PropertyChangeMask;

    unsigned long valuemask = CWColormap | CWBorderPixel | CWBackPixmap | CWBitGravity | CWEventMask;
    x11_window_ = XCreateWindow(display_, root,
                                current_x_, current_y_,
                                static_cast<unsigned int>(current_width_),
                                static_cast<unsigned int>(current_height_),
                                0, depth, InputOutput, visual,
                                valuemask, &swa);
    if (!x11_window_) {
        std::cerr << "[Nisaba X11LayerSurface] XCreateWindow failed\n";
        return false;
    }

    XSetWindowBackgroundPixmap(display_, x11_window_, 0L);

    if (!config_.namespace_id.empty()) {
        XStoreName(display_, x11_window_, config_.namespace_id.c_str());
        XClassHint class_hint{};
        std::string res_name = config_.namespace_id;
        std::string res_class = "obsidian-shell";
        class_hint.res_name  = res_name.data();
        class_hint.res_class = res_class.data();
        XSetClassHint(display_, x11_window_, &class_hint);
    }

    // All desktops (0xFFFFFFFF)
    Atom net_wm_desktop = XInternAtom(display_, "_NET_WM_DESKTOP", False);
    unsigned long all_desktops = 0xFFFFFFFFUL;
    XChangeProperty(display_, x11_window_, net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<unsigned char*>(&all_desktops), 1);

    applyWindowTypeAndLayer();
    applyStruts();
    applyKeyboardHints();

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_cfg,
                                          static_cast<EGLNativeWindowType>(x11_window_), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        std::cerr << "[Nisaba X11LayerSurface] eglCreateWindowSurface failed\n";
        return false;
    }

    backend_.registerLayerSurface(this);

    XMapWindow(display_, x11_window_);
    mapped_ = true;
    XMoveResizeWindow(display_, x11_window_, current_x_, current_y_,
                      static_cast<unsigned int>(current_width_),
                      static_cast<unsigned int>(current_height_));
    if (config_.layer == ShellLayer::Background) {
        XLowerWindow(display_, x11_window_);
    } else if (config_.layer == ShellLayer::Top || config_.layer == ShellLayer::Overlay) {
        XRaiseWindow(display_, x11_window_);
    }
    XFlush(display_);

    makeCurrent();
    eglSwapInterval(egl_display_, config_.vsync ? 1 : 0);
    return true;
}

void X11LayerSurface::sendNetWmState(bool add, Atom prop1, Atom prop2) {
    if (!display_ || !x11_window_) return;
    Atom wm_state = XInternAtom(display_, "_NET_WM_STATE", False);
    XEvent ev{};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = x11_window_;
    ev.xclient.message_type = wm_state;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = add ? 1 : 0; // _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE
    ev.xclient.data.l[1]    = static_cast<long>(prop1);
    ev.xclient.data.l[2]    = static_cast<long>(prop2);
    ev.xclient.data.l[3]    = 1; // Normal source indication
    XSendEvent(display_, RootWindow(display_, backend_.getDefaultScreen()), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

void X11LayerSurface::applyWindowTypeAndLayer() {
    if (!display_ || !x11_window_) return;

    Atom wm_type = XInternAtom(display_, "_NET_WM_WINDOW_TYPE", False);
    Atom type_desktop = XInternAtom(display_, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom type_dock    = XInternAtom(display_, "_NET_WM_WINDOW_TYPE_DOCK", False);

    Atom chosen_type = (config_.layer == ShellLayer::Background || config_.layer == ShellLayer::Bottom)
        ? type_desktop
        : type_dock;
    XChangeProperty(display_, x11_window_, wm_type, XA_ATOM, 32,
                    PropModeReplace, reinterpret_cast<unsigned char*>(&chosen_type), 1);

    Atom wm_state      = XInternAtom(display_, "_NET_WM_STATE", False);
    Atom st_sticky     = XInternAtom(display_, "_NET_WM_STATE_STICKY", False);
    Atom st_skip_tb    = XInternAtom(display_, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom st_skip_pager = XInternAtom(display_, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom st_above      = XInternAtom(display_, "_NET_WM_STATE_ABOVE", False);
    Atom st_below      = XInternAtom(display_, "_NET_WM_STATE_BELOW", False);

    std::vector<Atom> states = {st_sticky, st_skip_tb, st_skip_pager};
    if (config_.layer == ShellLayer::Top || config_.layer == ShellLayer::Overlay) {
        states.push_back(st_above);
    } else if (config_.layer == ShellLayer::Bottom || config_.layer == ShellLayer::Background) {
        states.push_back(st_below);
    }

    XChangeProperty(display_, x11_window_, wm_state, XA_ATOM, 32,
                    PropModeReplace, reinterpret_cast<unsigned char*>(states.data()),
                    static_cast<int>(states.size()));

    if (mapped_) {
        if (config_.layer == ShellLayer::Top || config_.layer == ShellLayer::Overlay) {
            sendNetWmState(false, st_below);
            sendNetWmState(true, st_above);
            XRaiseWindow(display_, x11_window_);
        } else {
            sendNetWmState(false, st_above);
            sendNetWmState(true, st_below);
            XLowerWindow(display_, x11_window_);
        }
        XFlush(display_);
    }
}

void X11LayerSurface::applyStruts() {
    if (!display_ || !x11_window_) return;
    Atom net_strut         = XInternAtom(display_, "_NET_WM_STRUT", False);
    Atom net_strut_partial = XInternAtom(display_, "_NET_WM_STRUT_PARTIAL", False);

    if (config_.exclusive_zone <= 0) {
        XDeleteProperty(display_, x11_window_, net_strut);
        XDeleteProperty(display_, x11_window_, net_strut_partial);
        return;
    }

    int mx = 0, my = 0, mw = 1920, mh = 1080;
    resolveMonitorRect(mx, my, mw, mh);

    unsigned long struts[12] = {};
    const unsigned long ez = static_cast<unsigned long>(config_.exclusive_zone);

    const bool top    = hasAnchor(config_.anchor, ShellAnchor::Top);
    const bool bottom = hasAnchor(config_.anchor, ShellAnchor::Bottom);
    const bool left   = hasAnchor(config_.anchor, ShellAnchor::Left);
    const bool right  = hasAnchor(config_.anchor, ShellAnchor::Right);

    if (top && !bottom) {
        struts[2] = static_cast<unsigned long>(std::max(0, my)) + ez; // top
        struts[8] = static_cast<unsigned long>(std::max(0, current_x_));
        struts[9] = static_cast<unsigned long>(std::max(0, current_x_ + current_width_ - 1));
    } else if (bottom && !top) {
        struts[3]  = ez; // bottom
        struts[10] = static_cast<unsigned long>(std::max(0, current_x_));
        struts[11] = static_cast<unsigned long>(std::max(0, current_x_ + current_width_ - 1));
    } else if (left && !right) {
        struts[0] = static_cast<unsigned long>(std::max(0, mx)) + ez; // left
        struts[4] = static_cast<unsigned long>(std::max(0, current_y_));
        struts[5] = static_cast<unsigned long>(std::max(0, current_y_ + current_height_ - 1));
    } else if (right && !left) {
        struts[1] = ez; // right
        struts[6] = static_cast<unsigned long>(std::max(0, current_y_));
        struts[7] = static_cast<unsigned long>(std::max(0, current_y_ + current_height_ - 1));
    }

    XChangeProperty(display_, x11_window_, net_strut, XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<unsigned char*>(struts), 4);
    XChangeProperty(display_, x11_window_, net_strut_partial, XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<unsigned char*>(struts), 12);
}

void X11LayerSurface::applyKeyboardHints() {
    if (!display_ || !x11_window_) return;

    XWMHints* hints = XAllocWMHints();
    if (hints) {
        hints->flags = InputHint;
        hints->input = (config_.keyboard_mode == KeyboardMode::None) ? False : True;
        XSetWMHints(display_, x11_window_, hints);
        XFree(hints);
    }

    if (config_.keyboard_mode == KeyboardMode::Exclusive) {
        if (!grabbed_exclusive_ && mapped_) {
            XRaiseWindow(display_, x11_window_);
            XSetInputFocus(display_, x11_window_, RevertToParent, CurrentTime);
            XGrabKeyboard(display_, x11_window_, True, GrabModeAsync, GrabModeAsync, CurrentTime);
            XGrabPointer(display_, x11_window_, True,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, 0L, 0L, CurrentTime);
            grabbed_exclusive_ = true;
        }
    } else if (grabbed_exclusive_) {
        XUngrabKeyboard(display_, CurrentTime);
        XUngrabPointer(display_, CurrentTime);
        grabbed_exclusive_ = false;
    }

    XFlush(display_);
}

void X11LayerSurface::updateGeometry() {
    if (!display_ || !x11_window_) return;
    int nx = 0, ny = 0, nw = 1, nh = 1;
    computeTargetGeometry(nx, ny, nw, nh);
    current_x_      = nx;
    current_y_      = ny;
    current_width_  = nw;
    current_height_ = nh;
    XMoveResizeWindow(display_, x11_window_, nx, ny,
                      static_cast<unsigned int>(nw), static_cast<unsigned int>(nh));
    applyStruts();
    XFlush(display_);
}

void X11LayerSurface::setSize(int32_t width, int32_t height) {
    config_.width  = width;
    config_.height = height;
    updateGeometry();
}

void X11LayerSurface::setLayer(ShellLayer layer) {
    if (config_.layer == layer) return;
    config_.layer = layer;
    applyWindowTypeAndLayer();
}

void X11LayerSurface::setAnchor(ShellAnchor anchor) {
    config_.anchor = anchor;
    updateGeometry();
}

void X11LayerSurface::setExclusiveZone(int32_t zone) {
    config_.exclusive_zone = zone;
    applyStruts();
    if (display_) XFlush(display_);
}

void X11LayerSurface::setMargin(const SurfaceMargin& margin) {
    config_.margin = margin;
    updateGeometry();
}

void X11LayerSurface::setKeyboardMode(KeyboardMode mode) {
    config_.keyboard_mode = mode;
    applyKeyboardHints();
}

void X11LayerSurface::setInputRegion(const std::vector<RegionRect>& rects) {
#ifdef NISABA_BACKEND_OS_HAS_XSHAPE
    if (!display_ || !x11_window_ || !shape_supported_) return;

    std::vector<XRectangle> xrects;
    xrects.reserve(rects.size());
    for (const auto& r : rects) {
        if (r.width > 0 && r.height > 0) {
            XRectangle xr{};
            xr.x      = static_cast<short>(r.x);
            xr.y      = static_cast<short>(r.y);
            xr.width  = static_cast<unsigned short>(r.width);
            xr.height = static_cast<unsigned short>(r.height);
            xrects.push_back(xr);
        }
    }

    if (xrects.empty()) {
        // 0 rectangles => entire transparent window becomes click-through
        XShapeCombineRectangles(display_, x11_window_, ShapeInput, 0, 0,
                                nullptr, 0, ShapeSet, Unsorted);
    } else {
        XShapeCombineRectangles(display_, x11_window_, ShapeInput, 0, 0,
                                xrects.data(), static_cast<int>(xrects.size()),
                                ShapeSet, Unsorted);
    }
    XFlush(display_);
#else
    (void)rects;
#endif
}

void X11LayerSurface::resetInputRegion() {
#ifdef NISABA_BACKEND_OS_HAS_XSHAPE
    if (!display_ || !x11_window_ || !shape_supported_) return;
    XShapeCombineMask(display_, x11_window_, ShapeInput, 0, 0, 0L, ShapeSet);
    XFlush(display_);
#endif
}

Size X11LayerSurface::getSize() const {
    return Size{static_cast<float>(current_width_), static_cast<float>(current_height_)};
}

Size X11LayerSurface::getDrawableSize() const {
    return Size{static_cast<float>(current_width_), static_cast<float>(current_height_)};
}

float X11LayerSurface::getDpiScale() const {
    return scale_factor_;
}

void X11LayerSurface::makeCurrent() {
    if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
    }
}

void X11LayerSurface::swapBuffers() {
    if (egl_display_ != EGL_NO_DISPLAY && egl_surface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(egl_display_, egl_surface_);
    }
}

void X11LayerSurface::handleConfigure(int width, int height) {
    if (width > 0 && height > 0 && (width != current_width_ || height != current_height_)) {
        current_width_  = width;
        current_height_ = height;
        on_resize_.emit(width, height);
    }
}

void X11LayerSurface::handleButtonFocus() {
    if (!display_ || !x11_window_) return;
    if (config_.keyboard_mode == KeyboardMode::OnDemand ||
        config_.keyboard_mode == KeyboardMode::Exclusive) {
        XSetInputFocus(display_, x11_window_, RevertToParent, CurrentTime);
        XFlush(display_);
    }
}

} // namespace nisaba::backend_os::x11
