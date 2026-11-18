#pragma once
/// @file x11_platform.hpp
/// @brief X11 + EGL native platform backend.
/// Internal backend used by Platform when WAYLAND_DISPLAY is not available.

#include "nisaba/backend_os/platform.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unordered_set>
#include <string>

namespace nisaba::backend_os {
class Window;
class LayerSurface;
}

namespace nisaba::backend_os::x11 {

class X11PlatformBackend {
public:
    explicit X11PlatformBackend(Platform* owner);
    ~X11PlatformBackend();

    bool init();
    void shutdown();
    bool pollEvents();

    // ── Display / EGL accessors ──────────────────────────────────
    [[nodiscard]] ::Display*  getDisplay()    const { return display_; }
    [[nodiscard]] EGLDisplay  getEGLDisplay() const { return egl_display_; }
    [[nodiscard]] EGLConfig   getEGLConfig()  const { return egl_config_; }
    [[nodiscard]] EGLContext  getEGLContext() const { return egl_context_; }
    [[nodiscard]] int         getDefaultScreen() const { return default_screen_; }

    // ── Atoms ────────────────────────────────────────────────────
    [[nodiscard]] Atom getAtomWmProtocols()    const { return atom_wm_protocols_; }
    [[nodiscard]] Atom getAtomWmDeleteWindow() const { return atom_wm_delete_window_; }
    [[nodiscard]] Atom getAtomUtf8String()     const { return atom_utf8_string_; }
    [[nodiscard]] Atom getAtomClipboard()      const { return atom_clipboard_; }

    void registerWindow(Window* w);
    void unregisterWindow(Window* w);
    void registerLayerSurface(LayerSurface* s);
    void unregisterLayerSurface(LayerSurface* s);
    [[nodiscard]] bool isOwnWindow(::Window xid) const;
    [[nodiscard]] const std::unordered_set<Window*>& windows() const { return windows_; }

    // ── Clipboard Subsystem ──────────────────────────────────────
    void setClipboardData(const ClipboardData& data, ClipboardType type);
    void setClipboardText(std::string_view text, ClipboardType type = ClipboardType::Clipboard);
    [[nodiscard]] std::string getClipboardText(ClipboardType type = ClipboardType::Clipboard) const;
    [[nodiscard]] ClipboardData getClipboardData(ClipboardType type = ClipboardType::Clipboard) const;
    [[nodiscard]] std::vector<uint8_t> getClipboardDataForMime(std::string_view mime_type, ClipboardType type = ClipboardType::Clipboard) const;
    [[nodiscard]] std::vector<std::string> getClipboardFormats(ClipboardType type = ClipboardType::Clipboard) const;
    [[nodiscard]] bool hasClipboardFormat(std::string_view mime_type, ClipboardType type = ClipboardType::Clipboard) const;

    // ── Drag and Drop Subsystem ──────────────────────────────────
    bool startDrag(const DragData& data, DragAction actions);

    // ── Foreign Toplevel / EWMH Subsystem ───────────────────────
    [[nodiscard]] std::vector<std::shared_ptr<ToplevelWindow>> getToplevels() const;
    [[nodiscard]] std::shared_ptr<ToplevelWindow> getActiveToplevel() const;
    void refreshClientList();
    void refreshActiveWindow();

    // Atoms getters for EWMH
    [[nodiscard]] Atom getAtomNetActiveWindow() const { return atom_net_active_window_; }
    [[nodiscard]] Atom getAtomNetCloseWindow()  const { return atom_net_close_window_; }
    [[nodiscard]] Atom getAtomNetWmState()      const { return atom_net_wm_state_; }
    [[nodiscard]] Atom getAtomNetWmStateMaxVert() const { return atom_net_wm_state_max_vert_; }
    [[nodiscard]] Atom getAtomNetWmStateMaxHorz() const { return atom_net_wm_state_max_horz_; }
    [[nodiscard]] Atom getAtomNetWmStateHidden()  const { return atom_net_wm_state_hidden_; }
    [[nodiscard]] Atom getAtomNetWmStateFullscreen() const { return atom_net_wm_state_fullscreen_; }
    [[nodiscard]] Atom getAtomNetWmMoveresize() const { return atom_net_wm_moveresize_; }
    [[nodiscard]] Atom getAtomGtkFrameExtents() const { return atom_gtk_frame_extents_; }
    [[nodiscard]] Atom getAtomNetWmName() const { return atom_net_wm_name_; }
    [[nodiscard]] Atom getAtomWmClass() const { return atom_wm_class_; }

    void setCursor(SystemCursor cursor);

    // ── Output / Monitor Subsystem ──────────────────────────────
    [[nodiscard]] std::vector<std::shared_ptr<Output>> getOutputs() const;
    [[nodiscard]] std::shared_ptr<Output> getOutputByName(std::string_view name) const;
    [[nodiscard]] std::shared_ptr<Output> getPrimaryOutput() const;
    void updateOutputs();
    [[nodiscard]] Platform* getOwner() const { return owner_; }

    // Internal XDnD & EWMH handlers
    void handleSelectionRequest(const XSelectionRequestEvent& req);
    void handleSelectionClear(const XSelectionClearEvent& clr);
    void handleXdndEnter(const XClientMessageEvent& cme);
    void handleXdndPosition(const XClientMessageEvent& cme);
    void handleXdndLeave(const XClientMessageEvent& cme);
    void handleXdndDrop(const XClientMessageEvent& cme);
    void handlePropertyNotify(const XPropertyEvent& prop);

private:
    Platform*   owner_          = nullptr;
    ::Display*  display_        = nullptr;
    int         default_screen_ = 0;
    EGLDisplay egl_display_    = EGL_NO_DISPLAY;
    EGLConfig  egl_config_     = nullptr;
    EGLContext egl_context_    = EGL_NO_CONTEXT;
    int        egl_major_      = 0;
    int        egl_minor_      = 0;

    // Standard Atoms
    Atom atom_wm_protocols_     = None;
    Atom atom_wm_delete_window_ = None;
    Atom atom_utf8_string_      = None;
    Atom atom_string_           = None;
    Atom atom_text_             = None;
    Atom atom_targets_          = None;
    Atom atom_clipboard_        = None;
    Atom atom_primary_          = None;
    Atom atom_nisaba_sel_prop_    = None;
    Atom atom_wm_class_         = None;

    // EWMH Atoms
    Atom atom_net_client_list_        = None;
    Atom atom_net_active_window_      = None;
    Atom atom_net_close_window_       = None;
    Atom atom_net_wm_name_            = None;
    Atom atom_net_wm_state_           = None;
    Atom atom_net_wm_state_max_vert_  = None;
    Atom atom_net_wm_state_max_horz_  = None;
    Atom atom_net_wm_state_hidden_    = None;
    Atom atom_net_wm_state_fullscreen_= None;
    Atom atom_net_wm_state_focused_   = None;
    Atom atom_net_wm_moveresize_      = None;
    Atom atom_gtk_frame_extents_      = None;

    // XDnD Atoms
    Atom atom_xdnd_aware_       = None;
    Atom atom_xdnd_enter_       = None;
    Atom atom_xdnd_position_    = None;
    Atom atom_xdnd_status_      = None;
    Atom atom_xdnd_leave_       = None;
    Atom atom_xdnd_drop_        = None;
    Atom atom_xdnd_finished_    = None;
    Atom atom_xdnd_selection_   = None;
    Atom atom_xdnd_type_list_   = None;
    Atom atom_xdnd_action_copy_ = None;
    Atom atom_xdnd_action_move_ = None;
    Atom atom_xdnd_action_link_ = None;
    Atom atom_xdnd_action_ask_  = None;
    Atom atom_xdnd_action_priv_ = None;

    std::unordered_set<Window*>       windows_;
    std::unordered_set<LayerSurface*> layer_surfaces_;

    // Local Clipboard buffers
    ClipboardData local_clipboard_;
    ClipboardData local_primary_;

    // XDnD state
    ::Window                 xdnd_source_window_ = None;
    std::vector<std::string> xdnd_source_types_;
    DragAction               xdnd_accepted_action_ = DragAction::NoAction;
    Point                    xdnd_last_pos_;

    // Outgoing Drag state
    DragData outgoing_drag_data_;

    // Foreign Toplevel State
    class X11Toplevel;
    std::vector<std::shared_ptr<X11Toplevel>> toplevels_;
    std::unordered_map<::Window, std::shared_ptr<X11Toplevel>> toplevel_map_;
    std::shared_ptr<X11Toplevel> active_toplevel_;

    // Outputs State
    class X11Output;
    std::vector<std::shared_ptr<X11Output>> outputs_;
    int  xrandr_event_base_ = 0;
    int  xrandr_error_base_ = 0;
    bool has_xrandr_        = false;
};

} // namespace nisaba::backend_os::x11
