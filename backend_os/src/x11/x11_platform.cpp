/// @file x11_platform.cpp
/// @brief X11 + EGL native platform backend implementation with multi-MIME Clipboard and XDnD.

#include "nisaba/backend_os/x11/x11_platform.hpp"
#include "nisaba/backend_os/x11/x11_window.hpp"
#include "nisaba/backend_os/x11/x11_layer_surface.hpp"
#ifndef None
#define None 0L
#endif
#include "nisaba/backend_os/window.hpp"

#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xutil.h>
#if defined(NISABA_BACKEND_OS_HAS_XRANDR)
#include <X11/extensions/Xrandr.h>
#endif
#include <GL/gl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <vector>

namespace nisaba::backend_os::x11 {

// ── X11 Data Offer Wrapper (implements DataOffer for XDnD and Clipboard) ─

class X11DataOffer : public DataOffer {
public:
    X11DataOffer(::Display* display, ::Window target_window, ::Window source_window,
                 Atom sel_atom, Atom prop_atom, std::vector<std::string> formats)
        : display_(display), target_window_(target_window), source_window_(source_window),
          sel_atom_(sel_atom), prop_atom_(prop_atom), formats_(std::move(formats)) {}

    [[nodiscard]] bool hasFormat(std::string_view mime_type) const override {
        for (const auto& f : formats_) {
            if (f == mime_type) return true;
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> formats() const override {
        return formats_;
    }

    [[nodiscard]] std::string readText() override {
        for (const auto& m : {mime::TextPlainUtf8, mime::TextPlain, mime::TextUtf8, mime::TextString}) {
            if (hasFormat(m)) {
                auto data = readData(m);
                return std::string(data.begin(), data.end());
            }
        }
        return {};
    }

    [[nodiscard]] std::vector<std::string> readUris() override {
        auto raw = readData(mime::TextUriList);
        if (raw.empty()) return {};
        std::string content(raw.begin(), raw.end());
        std::istringstream stream(content);
        std::string line;
        std::vector<std::string> uris;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line.front() != '#') {
                uris.push_back(line);
            }
        }
        return uris;
    }

    [[nodiscard]] std::vector<uint8_t> readData(std::string_view mime_type) override {
        if (!display_ || target_window_ == None) return {};
        Atom target_atom = XInternAtom(display_, std::string(mime_type).c_str(), False);
        if (target_atom == None) return {};

        XConvertSelection(display_, sel_atom_, target_atom, prop_atom_, target_window_, CurrentTime);
        XFlush(display_);

        // Wait for SelectionNotify with timeout
        XEvent ev;
        bool received = false;
        for (int i = 0; i < 50; ++i) {
            if (XCheckTypedWindowEvent(display_, target_window_, SelectionNotify, &ev)) {
                received = true;
                break;
            }
            usleep(2000); // 2ms
        }

        if (!received || ev.xselection.property == None) return {};

        Atom actual_type;
        int actual_format;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char* prop_data = nullptr;

        int status = XGetWindowProperty(display_, target_window_, prop_atom_,
                                        0, 1024 * 1024 / 4, True, AnyPropertyType,
                                        &actual_type, &actual_format, &nitems, &bytes_after,
                                        &prop_data);

        std::vector<uint8_t> result;
        if (status == Success && prop_data && nitems > 0) {
            size_t num_bytes = nitems * (actual_format / 8);
            result.assign(prop_data, prop_data + num_bytes);
            XFree(prop_data);
        }
        return result;
    }

private:
    ::Display* display_ = nullptr;
    ::Window target_window_ = None;
    ::Window source_window_ = None;
    Atom sel_atom_ = None;
    Atom prop_atom_ = None;
    std::vector<std::string> formats_;
};

// ════════════════════════════════════════════════════════════════
// X11PlatformBackend Implementation
// ════════════════════════════════════════════════════════════════

X11PlatformBackend::X11PlatformBackend(Platform* owner)
    : owner_(owner) {}

X11PlatformBackend::~X11PlatformBackend() {
    shutdown();
}

bool X11PlatformBackend::init() {
    static auto x11_error_handler = [](::Display* dpy, XErrorEvent* ev) -> int {
        if (!ev) return 0;
        if (ev->error_code == BadWindow || ev->error_code == BadDrawable ||
            ev->error_code == BadMatch || ev->error_code == BadAccess) {
            return 0;
        }
        char msg[256]{};
        if (dpy) XGetErrorText(dpy, ev->error_code, msg, sizeof(msg));
        std::cerr << "[Nisaba X11] Non-fatal XError: " << msg
                  << " (req=" << static_cast<int>(ev->request_code)
                  << " minor=" << static_cast<int>(ev->minor_code) << ")\n";
        return 0;
    };
    XSetErrorHandler(x11_error_handler);

    // 1. Connect to X server
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        std::cerr << "[Nisaba X11] Failed to open X11 display\n";
        return false;
    }
    default_screen_ = DefaultScreen(display_);
    std::cout << "[Nisaba Platform] X11 backend active\n";

    // 2. Intern atoms
    atom_wm_protocols_     = XInternAtom(display_, "WM_PROTOCOLS",     False);
    atom_wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    atom_utf8_string_      = XInternAtom(display_, "UTF8_STRING",      False);
    atom_string_           = XInternAtom(display_, "STRING",           False);
    atom_text_             = XInternAtom(display_, "TEXT",             False);
    atom_targets_          = XInternAtom(display_, "TARGETS",          False);
    atom_clipboard_        = XInternAtom(display_, "CLIPBOARD",        False);
    atom_primary_          = XInternAtom(display_, "PRIMARY",          False);
    atom_nisaba_sel_prop_    = XInternAtom(display_, "Nisaba_SELECTION",   False);
    atom_wm_class_         = XInternAtom(display_, "WM_CLASS",         False);

    // EWMH atoms
    atom_net_client_list_        = XInternAtom(display_, "_NET_CLIENT_LIST",         False);
    atom_net_active_window_      = XInternAtom(display_, "_NET_ACTIVE_WINDOW",       False);
    atom_net_close_window_       = XInternAtom(display_, "_NET_CLOSE_WINDOW",        False);
    atom_net_wm_name_            = XInternAtom(display_, "_NET_WM_NAME",             False);
    atom_net_wm_state_           = XInternAtom(display_, "_NET_WM_STATE",            False);
    atom_net_wm_state_max_vert_  = XInternAtom(display_, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    atom_net_wm_state_max_horz_  = XInternAtom(display_, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    atom_net_wm_state_hidden_    = XInternAtom(display_, "_NET_WM_STATE_HIDDEN",     False);
    atom_net_wm_state_fullscreen_= XInternAtom(display_, "_NET_WM_STATE_FULLSCREEN", False);
    atom_net_wm_state_focused_   = XInternAtom(display_, "_NET_WM_STATE_FOCUSED",    False);
    atom_net_wm_moveresize_      = XInternAtom(display_, "_NET_WM_MOVERESIZE",       False);
    atom_gtk_frame_extents_      = XInternAtom(display_, "_GTK_FRAME_EXTENTS",       False);

    // XDnD atoms
    atom_xdnd_aware_       = XInternAtom(display_, "XdndAware",        False);
    atom_xdnd_enter_       = XInternAtom(display_, "XdndEnter",        False);
    atom_xdnd_position_    = XInternAtom(display_, "XdndPosition",     False);
    atom_xdnd_status_      = XInternAtom(display_, "XdndStatus",       False);
    atom_xdnd_leave_       = XInternAtom(display_, "XdndLeave",        False);
    atom_xdnd_drop_        = XInternAtom(display_, "XdndDrop",         False);
    atom_xdnd_finished_    = XInternAtom(display_, "XdndFinished",     False);
    atom_xdnd_selection_   = XInternAtom(display_, "XdndSelection",    False);
    atom_xdnd_type_list_   = XInternAtom(display_, "XdndTypeList",     False);
    atom_xdnd_action_copy_ = XInternAtom(display_, "XdndActionCopy",   False);
    atom_xdnd_action_move_ = XInternAtom(display_, "XdndActionMove",   False);
    atom_xdnd_action_link_ = XInternAtom(display_, "XdndActionLink",   False);
    atom_xdnd_action_ask_  = XInternAtom(display_, "XdndActionAsk",    False);
    atom_xdnd_action_priv_ = XInternAtom(display_, "XdndActionPrivate",False);

    // Monitor Root window for EWMH updates
    ::Window root = RootWindow(display_, default_screen_);
    XSelectInput(display_, root, PropertyChangeMask | SubstructureNotifyMask);

    // 3. Init EGL
    egl_display_ = eglGetDisplay((EGLNativeDisplayType)display_);
    if (egl_display_ == EGL_NO_DISPLAY) {
        std::cerr << "[Nisaba X11] eglGetDisplay failed\n";
        return false;
    }
    if (!eglInitialize(egl_display_, &egl_major_, &egl_minor_)) {
        std::cerr << "[Nisaba X11] eglInitialize failed\n";
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        eglBindAPI(EGL_OPENGL_ES_API);
    }

    // 4. Choose EGL config (RGBA8 + stencil8) with preference for 32-bit TrueColor visual (ARGB for transparency)
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    auto select_best_config = [&](const EGLint* attrib_list) -> EGLConfig {
        EGLint num_configs = 0;
        if (!eglChooseConfig(egl_display_, attrib_list, nullptr, 0, &num_configs) || num_configs <= 0) {
            return nullptr;
        }
        std::vector<EGLConfig> configs(num_configs);
        if (!eglChooseConfig(egl_display_, attrib_list, configs.data(), num_configs, &num_configs) || num_configs <= 0) {
            return nullptr;
        }

        // Search for a TrueColor visual with depth 32 (ARGB) for compositing / transparency support
        for (EGLint i = 0; i < num_configs; ++i) {
            EGLint visual_id = 0;
            if (eglGetConfigAttrib(egl_display_, configs[i], EGL_NATIVE_VISUAL_ID, &visual_id) && visual_id != 0) {
                XVisualInfo tmpl{};
                tmpl.visualid = visual_id;
                int n_vis = 0;
                XVisualInfo* list = XGetVisualInfo(display_, VisualIDMask, &tmpl, &n_vis);
                if (list && n_vis > 0) {
                    bool is_32bit = (list[0].depth == 32 && list[0].c_class == TrueColor);
                    XFree(list);
                    if (is_32bit) {
                        return configs[i];
                    }
                }
            }
        }
        // Fallback to first matching config if no 32-bit visual is found
        return configs[0];
    };

    egl_config_ = select_best_config(attrs);
    if (!egl_config_) {
        const EGLint fallback[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        egl_config_ = select_best_config(fallback);
        if (!egl_config_) {
            std::cerr << "[Nisaba X11] No suitable EGL config\n";
            return false;
        }
    }

    // Create shared master EGL context
    const EGLint core_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    const EGLint gles_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, core_attrs);
    if (egl_context_ == EGL_NO_CONTEXT)
        egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, gles_attrs);
    if (egl_context_ == EGL_NO_CONTEXT)
        egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, nullptr);

    // Initial EWMH scan
    refreshClientList();
    refreshActiveWindow();

#if defined(NISABA_BACKEND_OS_HAS_XRANDR)
    if (XRRQueryExtension(display_, &xrandr_event_base_, &xrandr_error_base_)) {
        has_xrandr_ = true;
        ::Window root = RootWindow(display_, default_screen_);
        XRRSelectInput(display_, root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);
    }
#endif
    updateOutputs();

    return true;
}

void X11PlatformBackend::shutdown() {
    // Clear outputs without calling virtual methods — X11Output defined later in TU
    outputs_.clear();

    toplevels_.clear();
    toplevel_map_.clear();
    active_toplevel_.reset();

    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(egl_display_, egl_context_);
            egl_context_ = EGL_NO_CONTEXT;
        }
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

bool X11PlatformBackend::pollEvents() {
    if (!display_) return false;

    while (XPending(display_) > 0) {
        XEvent xev;
        XNextEvent(display_, &xev);

#if defined(NISABA_BACKEND_OS_HAS_XRANDR)
        if (has_xrandr_) {
            if (xev.type == xrandr_event_base_ + RRScreenChangeNotify ||
                xev.type == xrandr_event_base_ + RRNotify) {
                XRRUpdateConfiguration(&xev);
                updateOutputs();
                continue;
            }
        }
#endif

        switch (xev.type) {

        case ClientMessage:
            if (xev.xclient.message_type == atom_wm_protocols_ &&
                (Atom)xev.xclient.data.l[0] == atom_wm_delete_window_) {
                owner_->onQuit().emit();
                return false;
            } else if (xev.xclient.message_type == atom_xdnd_enter_) {
                handleXdndEnter(xev.xclient);
            } else if (xev.xclient.message_type == atom_xdnd_position_) {
                handleXdndPosition(xev.xclient);
            } else if (xev.xclient.message_type == atom_xdnd_leave_) {
                handleXdndLeave(xev.xclient);
            } else if (xev.xclient.message_type == atom_xdnd_drop_) {
                handleXdndDrop(xev.xclient);
            }
            break;

        case SelectionRequest:
            handleSelectionRequest(xev.xselectionrequest);
            break;

        case SelectionClear:
            handleSelectionClear(xev.xselectionclear);
            break;

        case PropertyNotify:
            handlePropertyNotify(xev.xproperty);
            break;

        case DestroyNotify:
            refreshClientList();
            break;

        case MotionNotify: {
            // Motion event compression: discard intermediate motion events for the same window
            XEvent next_ev;
            while (XPending(display_)) {
                XPeekEvent(display_, &next_ev);
                if (next_ev.type == MotionNotify && next_ev.xmotion.window == xev.xmotion.window) {
                    XNextEvent(display_, &xev);
                } else {
                    break;
                }
            }

            void* win_handle = (void*)(uintptr_t)xev.xmotion.window;
            float x = (float)xev.xmotion.x;
            float y = (float)xev.xmotion.y;

            for (Window* w : windows_) {
                if (w && w->getNativeHandle() == win_handle) {
                    auto* x11_win = static_cast<X11Window*>(w->getBackendWindow());
                    if (x11_win) {
                        x11_win->handleDragMotion(xev.xmotion.x_root, xev.xmotion.y_root);
                    }
                    break;
                }
            }

            owner_->onTargetedMouseMove().emit(win_handle, x, y);
            owner_->onMouseMove().emit(x, y);
            break;
        }

        case ButtonPress: {
            void* win_handle = (void*)(uintptr_t)xev.xbutton.window;
            for (LayerSurface* ls : layer_surfaces_) {
                if (ls && ls->getNativeHandle() == win_handle) {
                    if (auto* x11_ls = static_cast<X11LayerSurface*>(ls->getBackendLayer())) {
                        x11_ls->handleButtonFocus();
                    }
                    break;
                }
            }
            float x = (float)xev.xbutton.x, y = (float)xev.xbutton.y;
            int   b = xev.xbutton.button;
            if (b == 4) {
                owner_->onTargetedScroll().emit(win_handle, 0.0f, 1.0f);
                owner_->onScroll().emit(0.0f,  1.0f);
            } else if (b == 5) {
                owner_->onTargetedScroll().emit(win_handle, 0.0f, -1.0f);
                owner_->onScroll().emit(0.0f, -1.0f);
            } else {
                int btn_code = (b == 1 ? 1 : (b == 3 ? 3 : 2));
                owner_->onTargetedMouseDown().emit(win_handle, x, y, btn_code);
                owner_->onMouseDown().emit(x, y, btn_code);
            }
            break;
        }

        case ButtonRelease: {
            void* win_handle = (void*)(uintptr_t)xev.xbutton.window;
            float x = (float)xev.xbutton.x, y = (float)xev.xbutton.y;
            int   b = xev.xbutton.button;

            for (Window* w : windows_) {
                if (w) {
                    auto* x11_win = static_cast<X11Window*>(w->getBackendWindow());
                    if (x11_win) x11_win->endDrag();
                }
            }

            if (b != 4 && b != 5) {
                int btn_code = (b == 1 ? 1 : (b == 3 ? 3 : 2));
                owner_->onTargetedMouseUp().emit(win_handle, x, y, btn_code);
                owner_->onMouseUp().emit(x, y, btn_code);
            }
            break;
        }

        case KeyPress: {
            KeySym sym = XLookupKeysym(&xev.xkey, 0);
            int mods = 0;
            if (xev.xkey.state & ShiftMask)   mods |= 1;
            if (xev.xkey.state & ControlMask) mods |= 2;
            if (xev.xkey.state & Mod1Mask)    mods |= 4;
            owner_->onKeyDown().emit((int)sym, mods);
            char buf[32];
            int len = XLookupString(&xev.xkey, buf, 31, nullptr, nullptr);
            if (len > 0) { buf[len] = '\0'; owner_->onTextInput().emit(std::string_view(buf, len)); }
            break;
        }

        case KeyRelease: {
            KeySym sym = XLookupKeysym(&xev.xkey, 0);
            int mods = 0;
            if (xev.xkey.state & ShiftMask)   mods |= 1;
            if (xev.xkey.state & ControlMask) mods |= 2;
            if (xev.xkey.state & Mod1Mask)    mods |= 4;
            owner_->onKeyUp().emit((int)sym, mods);
            break;
        }

        case ConfigureNotify: {
            for (Window* w : windows_) {
                if ((void*)(uintptr_t)xev.xconfigure.window == w->getNativeHandle()) {
                    auto* x11_win = static_cast<X11Window*>(w->getBackendWindow());
                    if (x11_win) {
                        x11_win->handleConfigure(xev.xconfigure.width, xev.xconfigure.height);
                    }
                    w->onResize().emit(xev.xconfigure.width, xev.xconfigure.height);
                    break;
                }
            }
            for (LayerSurface* ls : layer_surfaces_) {
                if (ls && (void*)(uintptr_t)xev.xconfigure.window == ls->getNativeHandle()) {
                    if (auto* x11_ls = static_cast<X11LayerSurface*>(ls->getBackendLayer())) {
                        x11_ls->handleConfigure(xev.xconfigure.width, xev.xconfigure.height);
                    }
                    break;
                }
            }
            break;
        }

        case LeaveNotify: {
            if (xev.xcrossing.detail != NotifyInferior) {
                void* win_handle = (void*)(uintptr_t)xev.xcrossing.window;
                owner_->onTargetedMouseMove().emit(win_handle, -10000.0f, -10000.0f);
            }
            break;
        }

        case FocusIn: {
            for (Window* w : windows_) {
                if ((void*)(uintptr_t)xev.xfocus.window == w->getNativeHandle()) {
                    auto* x11_win = static_cast<X11Window*>(w->getBackendWindow());
                    if (x11_win) x11_win->handleFocus(true);
                    w->onFocus().emit(true);
                    break;
                }
            }
            break;
        }

        case FocusOut: {
            for (Window* w : windows_) {
                if ((void*)(uintptr_t)xev.xfocus.window == w->getNativeHandle()) {
                    auto* x11_win = static_cast<X11Window*>(w->getBackendWindow());
                    if (x11_win) x11_win->handleFocus(false);
                    w->onFocus().emit(false);
                    break;
                }
            }
            break;
        }

        default: break;
        }
    }
    return true;
}

void X11PlatformBackend::registerWindow(Window* w) {
    if (!w) return;
    windows_.insert(w);
    if (display_) {
        ::Window xwin = (::Window)(uintptr_t)w->getNativeHandle();
        Atom xdnd_version = 5;
        XChangeProperty(display_, xwin, atom_xdnd_aware_, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&xdnd_version, 1);
        XFlush(display_);
    }
}

void X11PlatformBackend::unregisterWindow(Window* w) {
    if (w) windows_.erase(w);
}

void X11PlatformBackend::registerLayerSurface(LayerSurface* s) {
    if (s) layer_surfaces_.insert(s);
}

void X11PlatformBackend::unregisterLayerSurface(LayerSurface* s) {
    if (s) layer_surfaces_.erase(s);
}

bool X11PlatformBackend::isOwnWindow(::Window xid) const {
    for (const Window* w : windows_) {
        if ((::Window)(uintptr_t)w->getNativeHandle() == xid) return true;
    }
    for (const LayerSurface* ls : layer_surfaces_) {
        if (ls && (::Window)(uintptr_t)ls->getNativeHandle() == xid) return true;
    }
    return false;
}

// ── Clipboard Subsystem ──────────────────────────────────────────

void X11PlatformBackend::setClipboardData(const ClipboardData& data, ClipboardType type) {
    if (type == ClipboardType::Primary) {
        local_primary_ = data;
    } else {
        local_clipboard_ = data;
    }

    if (windows_.empty() || !display_) return;

    ::Window xwin = (::Window)(uintptr_t)(*windows_.begin())->getNativeHandle();
    Atom sel = (type == ClipboardType::Primary) ? atom_primary_ : atom_clipboard_;
    XSetSelectionOwner(display_, sel, xwin, CurrentTime);
    XFlush(display_);
}

void X11PlatformBackend::setClipboardText(std::string_view text, ClipboardType type) {
    ClipboardData data;
    data.setText(text);
    setClipboardData(data, type);
}

std::string X11PlatformBackend::getClipboardText(ClipboardType type) const {
    const ClipboardData& local = (type == ClipboardType::Primary) ? local_primary_ : local_clipboard_;
    if (local.hasText()) {
        return local.getText();
    }

    auto bytes = getClipboardDataForMime(mime::TextPlainUtf8, type);
    if (bytes.empty()) {
        bytes = getClipboardDataForMime(mime::TextPlain, type);
    }
    if (bytes.empty()) {
        bytes = getClipboardDataForMime(mime::TextString, type);
    }
    if (!bytes.empty()) {
        return std::string(bytes.begin(), bytes.end());
    }
    return {};
}

ClipboardData X11PlatformBackend::getClipboardData(ClipboardType type) const {
    const ClipboardData& local = (type == ClipboardType::Primary) ? local_primary_ : local_clipboard_;
    if (!local.empty()) return local;

    ClipboardData result;
    auto formats = getClipboardFormats(type);
    for (const auto& fmt : formats) {
        auto bytes = getClipboardDataForMime(fmt, type);
        if (!bytes.empty()) {
            result.setRaw(fmt, bytes);
        }
    }
    return result;
}

std::vector<uint8_t> X11PlatformBackend::getClipboardDataForMime(std::string_view mime_type, ClipboardType type) const {
    const ClipboardData& local = (type == ClipboardType::Primary) ? local_primary_ : local_clipboard_;
    if (local.hasFormat(mime_type)) {
        return local.getRaw(mime_type);
    }

    if (windows_.empty() || !display_) return {};
    ::Window xwin = (::Window)(uintptr_t)(*windows_.begin())->getNativeHandle();
    Atom sel = (type == ClipboardType::Primary) ? atom_primary_ : atom_clipboard_;
    Atom target = XInternAtom(display_, std::string(mime_type).c_str(), False);
    if (target == None) return {};

    XConvertSelection(display_, sel, target, atom_nisaba_sel_prop_, xwin, CurrentTime);
    XFlush(display_);

    XEvent ev;
    bool received = false;
    for (int i = 0; i < 50; ++i) {
        if (XCheckTypedWindowEvent(display_, xwin, SelectionNotify, &ev)) {
            received = true;
            break;
        }
        usleep(2000); // 2ms
    }

    if (!received || ev.xselection.property == None) return {};

    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* prop_data = nullptr;

    int status = XGetWindowProperty(display_, xwin, atom_nisaba_sel_prop_,
                                    0, 1024 * 1024 / 4, True, AnyPropertyType,
                                    &actual_type, &actual_format, &nitems, &bytes_after,
                                    &prop_data);

    std::vector<uint8_t> result;
    if (status == Success && prop_data && nitems > 0) {
        size_t num_bytes = nitems * (actual_format / 8);
        result.assign(prop_data, prop_data + num_bytes);
        XFree(prop_data);
    }
    return result;
}

std::vector<std::string> X11PlatformBackend::getClipboardFormats(ClipboardType type) const {
    const ClipboardData& local = (type == ClipboardType::Primary) ? local_primary_ : local_clipboard_;
    if (!local.empty()) {
        return local.formats();
    }

    if (windows_.empty() || !display_) return {};
    ::Window xwin = (::Window)(uintptr_t)(*windows_.begin())->getNativeHandle();
    Atom sel = (type == ClipboardType::Primary) ? atom_primary_ : atom_clipboard_;

    XConvertSelection(display_, sel, atom_targets_, atom_nisaba_sel_prop_, xwin, CurrentTime);
    XFlush(display_);

    XEvent ev;
    bool received = false;
    for (int i = 0; i < 50; ++i) {
        if (XCheckTypedWindowEvent(display_, xwin, SelectionNotify, &ev)) {
            received = true;
            break;
        }
        usleep(2000);
    }

    if (!received || ev.xselection.property == None) return {};

    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* prop_data = nullptr;

    int status = XGetWindowProperty(display_, xwin, atom_nisaba_sel_prop_,
                                    0, 4096, True, XA_ATOM,
                                    &actual_type, &actual_format, &nitems, &bytes_after,
                                    &prop_data);

    std::vector<std::string> formats;
    if (status == Success && prop_data && nitems > 0) {
        auto* atoms = reinterpret_cast<Atom*>(prop_data);
        for (unsigned long i = 0; i < nitems; ++i) {
            char* name = XGetAtomName(display_, atoms[i]);
            if (name) {
                formats.emplace_back(name);
                XFree(name);
            }
        }
        XFree(prop_data);
    }
    return formats;
}

bool X11PlatformBackend::hasClipboardFormat(std::string_view mime_type, ClipboardType type) const {
    const ClipboardData& local = (type == ClipboardType::Primary) ? local_primary_ : local_clipboard_;
    if (local.hasFormat(mime_type)) return true;

    auto formats = getClipboardFormats(type);
    for (const auto& fmt : formats) {
        if (fmt == mime_type) return true;
    }
    return false;
}

void X11PlatformBackend::handleSelectionRequest(const XSelectionRequestEvent& req) {
    if (!display_) return;

    XSelectionEvent reply;
    reply.type = SelectionNotify;
    reply.display = req.display;
    reply.requestor = req.requestor;
    reply.selection = req.selection;
    reply.target = req.target;
    reply.property = None;
    reply.time = req.time;

    const ClipboardData& data = (req.selection == atom_primary_) ? local_primary_ :
                                (req.selection == atom_xdnd_selection_) ? outgoing_drag_data_.payload :
                                local_clipboard_;

    if (req.target == atom_targets_) {
        // Return supported target atoms
        std::vector<Atom> targets = { atom_targets_ };
        for (const auto& fmt : data.formats()) {
            Atom a = XInternAtom(display_, fmt.c_str(), False);
            if (a != None) targets.push_back(a);
        }
        if (data.hasText()) {
            targets.push_back(atom_utf8_string_);
            targets.push_back(atom_string_);
            targets.push_back(atom_text_);
        }

        XChangeProperty(display_, req.requestor, req.property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(targets.data()), static_cast<int>(targets.size()));
        reply.property = req.property;
    } else if (req.target == atom_utf8_string_ || req.target == atom_string_ || req.target == atom_text_) {
        std::string text = data.getText();
        XChangeProperty(display_, req.requestor, req.property, req.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
        reply.property = req.property;
    } else {
        char* target_name = XGetAtomName(display_, req.target);
        if (target_name) {
            std::string mime(target_name);
            XFree(target_name);
            if (data.hasFormat(mime)) {
                auto raw = data.getRaw(mime);
                XChangeProperty(display_, req.requestor, req.property, req.target, 8, PropModeReplace,
                                raw.data(), static_cast<int>(raw.size()));
                reply.property = req.property;
            }
        }
    }

    XSendEvent(display_, req.requestor, False, 0, reinterpret_cast<XEvent*>(&reply));
    XFlush(display_);
}

void X11PlatformBackend::handleSelectionClear(const XSelectionClearEvent& clr) {
    if (owner_) {
        ClipboardType type = (clr.selection == atom_primary_) ? ClipboardType::Primary : ClipboardType::Clipboard;
        owner_->onClipboardChanged().emit(type);
    }
}

// ── XDnD Handling ────────────────────────────────────────────────

void X11PlatformBackend::handleXdndEnter(const XClientMessageEvent& cme) {
    xdnd_source_window_ = static_cast<::Window>(cme.data.l[0]);
    bool more_than_3 = (cme.data.l[1] & 1) != 0;

    xdnd_source_types_.clear();
    if (!more_than_3) {
        for (int i = 2; i <= 4; ++i) {
            Atom a = static_cast<Atom>(cme.data.l[i]);
            if (a != None) {
                char* name = XGetAtomName(display_, a);
                if (name) {
                    xdnd_source_types_.emplace_back(name);
                    XFree(name);
                }
            }
        }
    } else {
        Atom actual_type;
        int actual_format;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char* prop_data = nullptr;

        int status = XGetWindowProperty(display_, xdnd_source_window_, atom_xdnd_type_list_,
                                        0, 1024, False, XA_ATOM,
                                        &actual_type, &actual_format, &nitems, &bytes_after,
                                        &prop_data);
        if (status == Success && prop_data && nitems > 0) {
            auto* atoms = reinterpret_cast<Atom*>(prop_data);
            for (unsigned long i = 0; i < nitems; ++i) {
                char* name = XGetAtomName(display_, atoms[i]);
                if (name) {
                    xdnd_source_types_.emplace_back(name);
                    XFree(name);
                }
            }
            XFree(prop_data);
        }
    }

    if (owner_) {
        DragEnterEvent ev;
        ev.position = xdnd_last_pos_;
        ev.mime_types = xdnd_source_types_;
        ev.suggested_action = DragAction::Copy;
        owner_->onDragEnter().emit(ev);
        xdnd_accepted_action_ = ev.accepted_action;
    }
}

void X11PlatformBackend::handleXdndPosition(const XClientMessageEvent& cme) {
    xdnd_source_window_ = static_cast<::Window>(cme.data.l[0]);
    float x = static_cast<float>((cme.data.l[2] >> 16) & 0xFFFF);
    float y = static_cast<float>(cme.data.l[2] & 0xFFFF);
    xdnd_last_pos_ = Point{x, y};

    if (owner_) {
        DragMotionEvent ev;
        ev.position = xdnd_last_pos_;
        ev.suggested_action = DragAction::Copy;
        owner_->onDragMotion().emit(ev);
        xdnd_accepted_action_ = ev.accepted_action;
    }

    // Send XdndStatus back to source window
    XClientMessageEvent status_ev;
    std::memset(&status_ev, 0, sizeof(status_ev));
    status_ev.type = ClientMessage;
    status_ev.display = display_;
    status_ev.window = xdnd_source_window_;
    status_ev.message_type = atom_xdnd_status_;
    status_ev.format = 32;
    status_ev.data.l[0] = cme.window; // Target window
    status_ev.data.l[1] = (xdnd_accepted_action_ != DragAction::NoAction) ? (1 | 2) : 0; // bit 0: accept, bit 1: want position
    status_ev.data.l[2] = 0; // Empty rectangle
    status_ev.data.l[3] = 0;
    status_ev.data.l[4] = (xdnd_accepted_action_ == DragAction::Move) ? atom_xdnd_action_move_ : atom_xdnd_action_copy_;

    XSendEvent(display_, xdnd_source_window_, False, NoEventMask, reinterpret_cast<XEvent*>(&status_ev));
    XFlush(display_);
}

void X11PlatformBackend::handleXdndLeave(const XClientMessageEvent& /*cme*/) {
    xdnd_source_window_ = None;
    xdnd_source_types_.clear();
    xdnd_accepted_action_ = DragAction::NoAction;
    if (owner_) {
        DragLeaveEvent ev;
        owner_->onDragLeave().emit(ev);
    }
}

void X11PlatformBackend::handleXdndDrop(const XClientMessageEvent& cme) {
    xdnd_source_window_ = static_cast<::Window>(cme.data.l[0]);
    bool handled = false;

    if (owner_) {
        DropEvent ev;
        ev.position = xdnd_last_pos_;
        ev.action = xdnd_accepted_action_;
        ev.data = std::make_shared<X11DataOffer>(display_, cme.window, xdnd_source_window_,
                                                atom_xdnd_selection_, atom_nisaba_sel_prop_, xdnd_source_types_);
        owner_->onDrop().emit(ev);
        handled = ev.handled;
    }

    // Send XdndFinished back to source window
    XClientMessageEvent finish_ev;
    std::memset(&finish_ev, 0, sizeof(finish_ev));
    finish_ev.type = ClientMessage;
    finish_ev.display = display_;
    finish_ev.window = xdnd_source_window_;
    finish_ev.message_type = atom_xdnd_finished_;
    finish_ev.format = 32;
    finish_ev.data.l[0] = cme.window;
    finish_ev.data.l[1] = handled ? 1 : 0;
    finish_ev.data.l[2] = (xdnd_accepted_action_ == DragAction::Move) ? atom_xdnd_action_move_ : atom_xdnd_action_copy_;

    XSendEvent(display_, xdnd_source_window_, False, NoEventMask, reinterpret_cast<XEvent*>(&finish_ev));
    XFlush(display_);

    xdnd_source_window_ = None;
    xdnd_source_types_.clear();
    xdnd_accepted_action_ = DragAction::NoAction;
}

bool X11PlatformBackend::startDrag(const DragData& data, DragAction /*actions*/) {
    if (windows_.empty() || !display_) return false;

    outgoing_drag_data_ = data;
    ::Window drag_win = (::Window)(uintptr_t)(*windows_.begin())->getNativeHandle();
    XSetSelectionOwner(display_, atom_xdnd_selection_, drag_win, CurrentTime);
    XFlush(display_);

    // Set XdndTypeList property on drag window
    std::vector<Atom> type_atoms;
    for (const auto& fmt : data.payload.formats()) {
        Atom a = XInternAtom(display_, fmt.c_str(), False);
        if (a != None) type_atoms.push_back(a);
    }
    if (data.payload.hasText()) {
        type_atoms.push_back(atom_utf8_string_);
    }

    XChangeProperty(display_, drag_win, atom_xdnd_type_list_, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(type_atoms.data()), static_cast<int>(type_atoms.size()));
    XFlush(display_);
    return true;
}

void X11PlatformBackend::setCursor(SystemCursor cursor) {
    if (!display_ || windows_.empty()) return;

    unsigned int shape = XC_left_ptr;
    switch (cursor) {
        case SystemCursor::Pointer: shape = XC_hand2; break;
        case SystemCursor::Text: shape = XC_xterm; break;
        case SystemCursor::Crosshair: shape = XC_crosshair; break;
        case SystemCursor::Move: shape = XC_fleur; break;
        case SystemCursor::NotAllowed: shape = XC_circle; break;
        case SystemCursor::ResizeHorizontal: shape = XC_sb_h_double_arrow; break;
        case SystemCursor::ResizeVertical: shape = XC_sb_v_double_arrow; break;
        case SystemCursor::ResizeTopLeft: shape = XC_top_left_corner; break;
        case SystemCursor::ResizeTopRight: shape = XC_top_right_corner; break;
        case SystemCursor::ResizeBottomLeft: shape = XC_bottom_left_corner; break;
        case SystemCursor::ResizeBottomRight: shape = XC_bottom_right_corner; break;
        case SystemCursor::Wait: shape = XC_watch; break;
        default: shape = XC_left_ptr; break;
    }

    ::Cursor xcursor = XCreateFontCursor(display_, shape);
    for (Window* w : windows_) {
        ::Window xwin = (::Window)(uintptr_t)w->getNativeHandle();
        XDefineCursor(display_, xwin, xcursor);
    }
    XFreeCursor(display_, xcursor);
    XFlush(display_);
}

// ════════════════════════════════════════════════════════════════
// Foreign Toplevel / EWMH Subsystem Implementation
// ════════════════════════════════════════════════════════════════

class X11PlatformBackend::X11Toplevel : public ToplevelWindow {
public:
    X11Toplevel(::Display* display, ::Window xid, X11PlatformBackend* backend, Platform* owner)
        : display_(display), xid_(xid), backend_(backend), owner_(owner) {
        if (display_ && xid_ != None) {
            // CRITICAL: NEVER call XSelectInput on our own application windows!
            // Doing so would overwrite the window's event_mask and wipe out ButtonPressMask/PointerMotionMask!
            if (!backend_ || !backend_->isOwnWindow(xid_)) {
                XSelectInput(display_, xid_, PropertyChangeMask | StructureNotifyMask);
            }
        }
        updateProperties();
    }

    ~X11Toplevel() override = default;

    [[nodiscard]] uint64_t id() const override { return static_cast<uint64_t>(xid_); }
    [[nodiscard]] std::string title() const override { return title_; }
    [[nodiscard]] std::string appId() const override { return app_id_; }
    [[nodiscard]] WindowState state() const override { return state_; }

    void setTitle(std::string t) { title_ = std::move(t); }
    void setAppId(std::string a) { app_id_ = std::move(a); }
    void setState(WindowState s) { state_ = s; }

    void activate() override {
        if (!display_ || xid_ == None) return;
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = xid_;
        ev.xclient.message_type = backend_->getAtomNetActiveWindow();
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 1; // 1 = application
        ev.xclient.data.l[1] = CurrentTime;
        ev.xclient.data.l[2] = 0;
        ::Window root = RootWindow(display_, DefaultScreen(display_));
        XSendEvent(display_, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(display_);
    }

    void setMinimized(bool min) override {
        if (!display_ || xid_ == None) return;
        if (min) {
            XIconifyWindow(display_, xid_, DefaultScreen(display_));
        } else {
            XMapWindow(display_, xid_);
            activate();
        }
        XFlush(display_);
    }

    void setMaximized(bool max) override {
        if (!display_ || xid_ == None) return;
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = xid_;
        ev.xclient.message_type = backend_->getAtomNetWmState();
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = max ? 1 : 0; // 1 = _NET_WM_STATE_ADD, 0 = _NET_WM_STATE_REMOVE
        ev.xclient.data.l[1] = (long)backend_->getAtomNetWmStateMaxVert();
        ev.xclient.data.l[2] = (long)backend_->getAtomNetWmStateMaxHorz();
        ev.xclient.data.l[3] = 1;
        ::Window root = RootWindow(display_, DefaultScreen(display_));
        XSendEvent(display_, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(display_);
    }

    void setFullscreen(bool full) override {
        if (!display_ || xid_ == None) return;
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = xid_;
        ev.xclient.message_type = backend_->getAtomNetWmState();
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = full ? 1 : 0;
        ev.xclient.data.l[1] = (long)backend_->getAtomNetWmStateFullscreen();
        ev.xclient.data.l[2] = 0;
        ev.xclient.data.l[3] = 1;
        ::Window root = RootWindow(display_, DefaultScreen(display_));
        XSendEvent(display_, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(display_);
    }

    void close() override {
        if (!display_ || xid_ == None) return;
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = xid_;
        ev.xclient.message_type = backend_->getAtomNetCloseWindow();
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = CurrentTime;
        ev.xclient.data.l[1] = 1;
        ::Window root = RootWindow(display_, DefaultScreen(display_));
        XSendEvent(display_, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(display_);
    }

    [[nodiscard]] ::Window getXid() const { return xid_; }

    void updateProperties() {
        if (!display_ || xid_ == None) return;

        // 1. Title
        std::string new_title;
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char* prop_data = nullptr;

        if (XGetWindowProperty(display_, xid_, backend_->getAtomNetWmName(),
                               0, 1024, False, backend_->getAtomUtf8String(),
                               &actual_type, &actual_format, &nitems, &bytes_after,
                               &prop_data) == Success && prop_data && nitems > 0) {
            new_title.assign(reinterpret_cast<char*>(prop_data), nitems);
            XFree(prop_data);
            prop_data = nullptr;
        } else {
            char* wm_name = nullptr;
            if (XFetchName(display_, xid_, &wm_name) > 0 && wm_name) {
                new_title = wm_name;
                XFree(wm_name);
            }
        }

        auto self = weak_from_this().lock();

        if (new_title != title_) {
            title_ = new_title;
            if (self && owner_) owner_->onToplevelTitleChanged().emit(self, title_);
        }

        // 2. App ID / WM_CLASS
        XClassHint class_hint;
        if (XGetClassHint(display_, xid_, &class_hint) != 0) {
            std::string new_app_id;
            if (class_hint.res_class) {
                new_app_id = class_hint.res_class;
                XFree(class_hint.res_class);
            }
            if (class_hint.res_name) {
                if (new_app_id.empty()) new_app_id = class_hint.res_name;
                XFree(class_hint.res_name);
            }
            if (new_app_id != app_id_) {
                app_id_ = new_app_id;
                if (self && owner_) owner_->onToplevelAppIdChanged().emit(self, app_id_);
            }
        }

        // 3. States (_NET_WM_STATE)
        WindowState new_state = WindowState::Normal;
        if (XGetWindowProperty(display_, xid_, backend_->getAtomNetWmState(),
                               0, 64, False, XA_ATOM,
                               &actual_type, &actual_format, &nitems, &bytes_after,
                               &prop_data) == Success && prop_data && nitems > 0) {
            auto* atoms = reinterpret_cast<Atom*>(prop_data);
            for (unsigned long i = 0; i < nitems; ++i) {
                if (atoms[i] == backend_->getAtomNetWmStateMaxVert() || atoms[i] == backend_->getAtomNetWmStateMaxHorz()) {
                    new_state |= WindowState::Maximized;
                } else if (atoms[i] == backend_->getAtomNetWmStateHidden()) {
                    new_state |= WindowState::Minimized;
                } else if (atoms[i] == backend_->getAtomNetWmStateFullscreen()) {
                    new_state |= WindowState::Fullscreen;
                }
            }
            XFree(prop_data);
        }

        if (backend_->getActiveToplevel() && backend_->getActiveToplevel()->id() == static_cast<uint64_t>(xid_)) {
            new_state |= WindowState::Activated;
        }

        if (new_state != state_) {
            state_ = new_state;
            if (self && owner_) owner_->onToplevelStateChanged().emit(self, state_);
        }
    }

private:
    ::Display* display_ = nullptr;
    ::Window xid_ = None;
    X11PlatformBackend* backend_ = nullptr;
    Platform* owner_ = nullptr;
    std::string title_;
    std::string app_id_;
    WindowState state_ = WindowState::Normal;
};

std::vector<std::shared_ptr<ToplevelWindow>> X11PlatformBackend::getToplevels() const {
    std::vector<std::shared_ptr<ToplevelWindow>> result;
    result.reserve(toplevels_.size());
    for (const auto& tl : toplevels_) {
        result.push_back(tl);
    }
    return result;
}

std::shared_ptr<ToplevelWindow> X11PlatformBackend::getActiveToplevel() const {
    return active_toplevel_;
}

void X11PlatformBackend::refreshClientList() {
    if (!display_) return;

    ::Window root = RootWindow(display_, default_screen_);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* prop_data = nullptr;

    if (XGetWindowProperty(display_, root, atom_net_client_list_,
                           0, 1024, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &prop_data) != Success || !prop_data) {
        return;
    }

    auto* xids = reinterpret_cast<::Window*>(prop_data);
    std::unordered_set<::Window> current_set(xids, xids + nitems);
    std::vector<std::shared_ptr<X11Toplevel>> new_list;

    // Detect removed windows
    for (auto it = toplevel_map_.begin(); it != toplevel_map_.end();) {
        if (current_set.find(it->first) == current_set.end()) {
            auto removed = it->second;
            if (active_toplevel_ == removed) {
                active_toplevel_.reset();
                if (owner_) owner_->onActiveToplevelChanged().emit(nullptr);
            }
            it = toplevel_map_.erase(it);
            if (owner_) owner_->onToplevelClosed().emit(removed);
        } else {
            ++it;
        }
    }

    // Detect new and existing windows
    for (unsigned long i = 0; i < nitems; ++i) {
        ::Window xid = xids[i];
        auto it = toplevel_map_.find(xid);
        if (it != toplevel_map_.end()) {
            new_list.push_back(it->second);
        } else {
            auto tl = std::make_shared<X11Toplevel>(display_, xid, this, owner_);
            toplevel_map_[xid] = tl;
            new_list.push_back(tl);
            if (owner_) owner_->onToplevelCreated().emit(tl);
        }
    }

    toplevels_ = std::move(new_list);
    XFree(prop_data);
}

void X11PlatformBackend::refreshActiveWindow() {
    if (!display_) return;

    ::Window root = RootWindow(display_, default_screen_);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* prop_data = nullptr;

    if (XGetWindowProperty(display_, root, atom_net_active_window_,
                           0, 1, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &prop_data) != Success || !prop_data) {
        return;
    }

    ::Window active_xid = None;
    if (nitems > 0) {
        active_xid = *reinterpret_cast<::Window*>(prop_data);
    }
    XFree(prop_data);

    std::shared_ptr<X11Toplevel> new_active;
    auto it = toplevel_map_.find(active_xid);
    if (it != toplevel_map_.end()) {
        new_active = it->second;
    }

    if (new_active != active_toplevel_) {
        if (active_toplevel_) {
            WindowState s = active_toplevel_->state();
            s = static_cast<WindowState>(static_cast<uint32_t>(s) & ~static_cast<uint32_t>(WindowState::Activated));
            active_toplevel_->setState(s);
            if (owner_) owner_->onToplevelStateChanged().emit(active_toplevel_, s);
        }

        active_toplevel_ = new_active;

        if (active_toplevel_) {
            WindowState s = active_toplevel_->state() | WindowState::Activated;
            active_toplevel_->setState(s);
            if (owner_) owner_->onToplevelStateChanged().emit(active_toplevel_, s);
        }

        if (owner_) {
            owner_->onActiveToplevelChanged().emit(active_toplevel_);
        }
    }
}

void X11PlatformBackend::handlePropertyNotify(const XPropertyEvent& prop) {
    if (!display_) return;

    ::Window root = RootWindow(display_, default_screen_);
    if (prop.window == root) {
        if (prop.atom == atom_net_client_list_) {
            refreshClientList();
        } else if (prop.atom == atom_net_active_window_) {
            refreshActiveWindow();
        }
    } else {
        auto it = toplevel_map_.find(prop.window);
        if (it != toplevel_map_.end()) {
            it->second->updateProperties();
        }
        if (prop.atom == atom_net_wm_state_) {
            for (Window* w : windows_) {
                if ((::Window)(uintptr_t)w->getNativeHandle() == prop.window) {
                    auto* x11_win = static_cast<X11Window*>(w->getBackendWindow());
                    if (x11_win) x11_win->updateState();
                    break;
                }
            }
        }
    }
}

// ── X11 Output Implementation & EDID Parser ──────────────────────

class X11PlatformBackend::X11Output : public Output {
public:
    X11PlatformBackend* backend_ = nullptr;
    uint32_t id_ = 0;
#if defined(NISABA_BACKEND_OS_HAS_XRANDR)
    RROutput rr_output_ = 0;
#else
    unsigned long rr_output_ = 0;
#endif

    std::string name_;
    std::string make_;
    std::string model_;
    std::string description_;

    Rect geometry_{0, 0, 0, 0};
    int32_t phys_w_mm_ = 0;
    int32_t phys_h_mm_ = 0;
    int32_t scale_ = 1;
    OutputTransform transform_ = OutputTransform::Normal;
    OutputSubpixel subpixel_ = OutputSubpixel::Unknown;

    std::vector<OutputMode> modes_;
    OutputMode current_mode_;
    bool is_primary_ = false;

    X11Output(X11PlatformBackend* backend, uint32_t id, unsigned long rr_output)
        : backend_(backend), id_(id), rr_output_(rr_output) {}

    [[nodiscard]] uint32_t id() const noexcept override { return id_; }
    [[nodiscard]] const std::string& name() const noexcept override { return name_; }
    [[nodiscard]] const std::string& make() const noexcept override { return make_; }
    [[nodiscard]] const std::string& model() const noexcept override { return model_; }
    [[nodiscard]] const std::string& description() const noexcept override { return description_; }

    [[nodiscard]] Rect geometry() const noexcept override { return geometry_; }
    [[nodiscard]] Rect logicalGeometry() const noexcept override { return geometry_; }
    [[nodiscard]] int32_t physicalWidthMm() const noexcept override { return phys_w_mm_; }
    [[nodiscard]] int32_t physicalHeightMm() const noexcept override { return phys_h_mm_; }
    [[nodiscard]] int32_t scaleFactor() const noexcept override { return scale_; }
    [[nodiscard]] double fractionalScale() const noexcept override { return static_cast<double>(scale_); }
    [[nodiscard]] OutputTransform transform() const noexcept override { return transform_; }
    [[nodiscard]] OutputSubpixel subpixel() const noexcept override { return subpixel_; }
    [[nodiscard]] const std::vector<OutputMode>& modes() const noexcept override { return modes_; }
    [[nodiscard]] const OutputMode& currentMode() const noexcept override { return current_mode_; }
    [[nodiscard]] bool isPrimary() const noexcept override { return is_primary_; }
    [[nodiscard]] void* nativeHandle() const noexcept override { return reinterpret_cast<void*>(static_cast<uintptr_t>(rr_output_)); }
};

#if defined(NISABA_BACKEND_OS_HAS_XRANDR)
static void parseEDID(const uint8_t* edid, size_t length, std::string& make, std::string& model) {
    if (!edid || length < 128) return;

    // Bytes 8-9: Manufacturer ID (3 letters, 5 bits each)
    uint16_t mfg_id = (static_cast<uint16_t>(edid[8]) << 8) | edid[9];
    char c1 = '@' + ((mfg_id >> 10) & 0x1F);
    char c2 = '@' + ((mfg_id >> 5) & 0x1F);
    char c3 = '@' + (mfg_id & 0x1F);
    if (c1 >= 'A' && c1 <= 'Z' && c2 >= 'A' && c2 <= 'Z' && c3 >= 'A' && c3 <= 'Z') {
        make = {c1, c2, c3};
    }

    // Four 18-byte detailed timing / descriptor blocks at offsets 54, 72, 90, 108
    for (int block = 0; block < 4; ++block) {
        const uint8_t* desc = edid + 54 + block * 18;
        if (desc[0] == 0 && desc[1] == 0 && desc[2] == 0) {
            uint8_t type = desc[3];
            if (type == 0xFC) { // Monitor Name
                std::string name_str;
                for (int i = 5; i < 18; ++i) {
                    if (desc[i] == 0x0A || desc[i] == 0x00) break;
                    if (desc[i] >= 32 && desc[i] <= 126) {
                        name_str.push_back(static_cast<char>(desc[i]));
                    }
                }
                while (!name_str.empty() && name_str.back() == ' ') name_str.pop_back();
                if (!name_str.empty()) {
                    model = name_str;
                }
            }
        }
    }
}
#endif

void X11PlatformBackend::updateOutputs() {
#if defined(NISABA_BACKEND_OS_HAS_XRANDR)
    if (!has_xrandr_ || !display_) return;

    ::Window root = RootWindow(display_, default_screen_);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(display_, root);
    if (!res) {
        res = XRRGetScreenResources(display_, root);
    }
    if (!res) return;

    RROutput primary_rr = XRRGetOutputPrimary(display_, root);
    Atom edid_atom = XInternAtom(display_, "EDID", True);

    std::vector<std::shared_ptr<X11Output>> current_active_outputs;

    for (int i = 0; i < res->noutput; ++i) {
        RROutput rr_out = res->outputs[i];
        XRROutputInfo* info = XRRGetOutputInfo(display_, res, rr_out);
        if (!info) continue;

        if (info->connection == RR_Connected && info->crtc != None) {
            XRRCrtcInfo* crtc = XRRGetCrtcInfo(display_, res, info->crtc);
            if (crtc) {
                std::shared_ptr<X11Output> out = nullptr;
                for (auto& existing : outputs_) {
                    if (existing->rr_output_ == rr_out) {
                        out = existing;
                        break;
                    }
                }
                bool is_new = false;
                if (!out) {
                    out = std::make_shared<X11Output>(this, static_cast<uint32_t>(rr_out), rr_out);
                    is_new = true;
                }

                out->name_ = info->name ? info->name : ("Screen-" + std::to_string(i));
                out->phys_w_mm_ = static_cast<int32_t>(info->mm_width);
                out->phys_h_mm_ = static_cast<int32_t>(info->mm_height);
                // XRROutputInfo does not expose subpixel order — leave as Unknown
                out->is_primary_ = (rr_out == primary_rr) || (info->crtc == res->crtcs[0] && primary_rr == None);

                out->geometry_ = Rect{static_cast<float>(crtc->x), static_cast<float>(crtc->y),
                                      static_cast<float>(crtc->width), static_cast<float>(crtc->height)};

                switch (crtc->rotation) {
                    case RR_Rotate_90:  out->transform_ = OutputTransform::Rotated90; break;
                    case RR_Rotate_180: out->transform_ = OutputTransform::Rotated180; break;
                    case RR_Rotate_270: out->transform_ = OutputTransform::Rotated270; break;
                    case RR_Reflect_X:  out->transform_ = OutputTransform::Flipped; break;
                    case RR_Reflect_Y:  out->transform_ = OutputTransform::Flipped180; break;
                    default:            out->transform_ = OutputTransform::Normal; break;
                }

                out->modes_.clear();
                for (int m = 0; m < info->nmode; ++m) {
                    RRMode mode_id = info->modes[m];
                    for (int rm = 0; rm < res->nmode; ++rm) {
                        if (res->modes[rm].id == mode_id) {
                            const auto& xmode = res->modes[rm];
                            OutputMode mode;
                            mode.width = static_cast<int32_t>(xmode.width);
                            mode.height = static_cast<int32_t>(xmode.height);
                            if (xmode.hTotal && xmode.vTotal) {
                                double hz = static_cast<double>(xmode.dotClock) / (static_cast<double>(xmode.hTotal) * static_cast<double>(xmode.vTotal));
                                mode.refresh_rate_mHz = static_cast<int32_t>(hz * 1000.0 + 0.5);
                            }
                            mode.is_current = (mode_id == crtc->mode);
                            mode.is_preferred = (m == 0 || (m < info->npreferred));
                            out->modes_.push_back(mode);
                            if (mode.is_current) {
                                out->current_mode_ = mode;
                            }
                            break;
                        }
                    }
                }

                if (edid_atom != None) {
                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems = 0, bytes_after = 0;
                    unsigned char* prop = nullptr;
                    if (XRRGetOutputProperty(display_, rr_out, edid_atom, 0, 128, False, False,
                                             AnyPropertyType, &actual_type, &actual_format,
                                             &nitems, &bytes_after, &prop) == Success && prop) {
                        parseEDID(prop, nitems, out->make_, out->model_);
                        XFree(prop);
                    }
                }

                std::string desc = out->make_;
                if (!out->model_.empty()) {
                    if (!desc.empty()) desc += " ";
                    desc += out->model_;
                }
                if (!out->name_.empty()) {
                    if (!desc.empty()) desc += " (" + out->name_ + ")";
                    else desc = out->name_;
                }
                out->description_ = desc;

                current_active_outputs.push_back(out);

                if (is_new) {
                    if (owner_) owner_->onOutputAdded().emit(out);
                } else {
                    out->onGeometryChanged().emit();
                    out->onModeChanged().emit();
                    if (owner_) owner_->onOutputChanged().emit(out);
                }

                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeOutputInfo(info);
    }

    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        bool still_present = false;
        for (const auto& act : current_active_outputs) {
            if (act->rr_output_ == (*it)->rr_output_) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            auto out = *it;
            out->onRemoved().emit();
            if (owner_) owner_->onOutputRemoved().emit(out);
        }
    }

    outputs_ = std::move(current_active_outputs);
    XRRFreeScreenResources(res);
#else
    if (outputs_.empty() && display_) {
        auto out = std::make_shared<X11Output>(this, 0, 0);
        out->name_ = "Default";
        out->description_ = "X11 Display";
        int w = DisplayWidth(display_, default_screen_);
        int h = DisplayHeight(display_, default_screen_);
        out->geometry_ = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
        out->current_mode_ = OutputMode{w, h, 60000, true, true};
        out->modes_.push_back(out->current_mode_);
        out->is_primary_ = true;
        outputs_.push_back(out);
        if (owner_) owner_->onOutputAdded().emit(out);
    }
#endif
}

std::vector<std::shared_ptr<Output>> X11PlatformBackend::getOutputs() const {
    std::vector<std::shared_ptr<Output>> res;
    res.reserve(outputs_.size());
    for (const auto& o : outputs_) {
        res.push_back(o);
    }
    return res;
}

std::shared_ptr<Output> X11PlatformBackend::getOutputByName(std::string_view name) const {
    for (const auto& o : outputs_) {
        if (o->name() == name) return o;
    }
    return nullptr;
}

std::shared_ptr<Output> X11PlatformBackend::getPrimaryOutput() const {
    for (const auto& o : outputs_) {
        if (o->isPrimary()) return o;
    }
    if (!outputs_.empty()) return outputs_.front();
    return nullptr;
}

} // namespace nisaba::backend_os::x11
