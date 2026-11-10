/// @file wayland_platform.cpp
/// @brief Native Wayland Client Platform backend implementation with wlr-layer-shell & EGL.

#include "nisaba/backend_os/wayland/wayland_platform.hpp"
#include "nisaba/backend_os/wayland/wayland_surface.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
#include <cstring>
#include <sstream>

namespace nisaba::backend_os::wayland {

// ── Wayland Registry Listeners ───────────────────────────────────

static void registry_global_handler(void* data, [[maybe_unused]] wl_registry* registry, uint32_t name,
                                   const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleGlobal(name, interface, version);
}

static void registry_global_remove_handler(void* data, [[maybe_unused]] wl_registry* registry, uint32_t name) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleGlobalRemove(name);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global_handler,
    .global_remove = registry_global_remove_handler,
};

// ── Wayland Pointer Listeners ────────────────────────────────────

static void pointer_enter_handler(void* data, [[maybe_unused]] wl_pointer* pointer, uint32_t serial,
                                  wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handlePointerEnter(serial, surface, sx, sy);
}

static void pointer_leave_handler(void* data, [[maybe_unused]] wl_pointer* pointer, [[maybe_unused]] uint32_t serial,
                                 wl_surface* surface) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handlePointerLeave(surface);
}

static void pointer_motion_handler(void* data, [[maybe_unused]] wl_pointer* pointer, uint32_t time,
                                  wl_fixed_t sx, wl_fixed_t sy) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handlePointerMotion(time, sx, sy);
}

static void pointer_button_handler(void* data, [[maybe_unused]] wl_pointer* pointer, uint32_t serial,
                                  uint32_t time, uint32_t button, uint32_t state) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handlePointerButton(serial, time, button, state);
}

static void pointer_axis_handler(void* data, [[maybe_unused]] wl_pointer* pointer, uint32_t time,
                                uint32_t axis, wl_fixed_t value) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handlePointerAxis(time, axis, value);
}

static void pointer_frame_handler([[maybe_unused]] void* data, [[maybe_unused]] wl_pointer* pointer) {}
static void pointer_axis_source_handler([[maybe_unused]] void* data, [[maybe_unused]] wl_pointer* pointer, [[maybe_unused]] uint32_t axis_source) {}
static void pointer_axis_stop_handler([[maybe_unused]] void* data, [[maybe_unused]] wl_pointer* pointer, [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis) {}
static void pointer_axis_discrete_handler([[maybe_unused]] void* data, [[maybe_unused]] wl_pointer* pointer, [[maybe_unused]] uint32_t axis, [[maybe_unused]] int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter_handler,
    .leave = pointer_leave_handler,
    .motion = pointer_motion_handler,
    .button = pointer_button_handler,
    .axis = pointer_axis_handler,
    .frame = pointer_frame_handler,
    .axis_source = pointer_axis_source_handler,
    .axis_stop = pointer_axis_stop_handler,
    .axis_discrete = pointer_axis_discrete_handler,
    .axis_value120 = nullptr,
    .axis_relative_direction = nullptr,
};

// ── Wayland Keyboard Listeners ───────────────────────────────────

static void keyboard_keymap_handler(void* data, [[maybe_unused]] wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleKeymap(format, fd, size);
    close(fd);
}

static void keyboard_enter_handler(void* data, [[maybe_unused]] wl_keyboard* keyboard, uint32_t serial,
                                  wl_surface* surface, [[maybe_unused]] wl_array* keys) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    if (self) {
        self->handleKeyboardEnter(serial, surface);
    }
}
static void keyboard_leave_handler([[maybe_unused]] void* data, [[maybe_unused]] wl_keyboard* keyboard, [[maybe_unused]] uint32_t serial,
                                  [[maybe_unused]] wl_surface* surface) {}

static void keyboard_key_handler(void* data, [[maybe_unused]] wl_keyboard* keyboard, uint32_t serial,
                                uint32_t time, uint32_t key, uint32_t state) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleKey(serial, time, key, state);
}

static void keyboard_modifiers_handler(void* data, [[maybe_unused]] wl_keyboard* keyboard, [[maybe_unused]] uint32_t serial,
                                      uint32_t mods_depressed, uint32_t mods_latched,
                                      uint32_t mods_locked, uint32_t group) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleModifiers(mods_depressed, mods_latched, mods_locked, group);
}

static void keyboard_repeat_info_handler(void* data, [[maybe_unused]] wl_keyboard* keyboard, int32_t rate, int32_t delay) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    if (self) {
        self->setKeyRepeatInfo(rate, delay);
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap_handler,
    .enter = keyboard_enter_handler,
    .leave = keyboard_leave_handler,
    .key = keyboard_key_handler,
    .modifiers = keyboard_modifiers_handler,
    .repeat_info = keyboard_repeat_info_handler,
};

// ── Wayland Seat Listeners ───────────────────────────────────────

static void seat_capabilities_handler(void* data, wl_seat* seat, uint32_t caps) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleSeatCapabilities(seat, caps);
}

static void seat_name_handler([[maybe_unused]] void* data, [[maybe_unused]] wl_seat* seat, [[maybe_unused]] const char* name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities_handler,
    .name = seat_name_handler,
};

// ── Wayland Data Offer Wrapper (implements DataOffer) ─────────────

class WaylandDataOfferWrapper : public DataOffer {
public:
    WaylandDataOfferWrapper(wl_display* display, wl_data_offer* offer, std::vector<std::string> mimes)
        : display_(display), offer_(offer), mimes_(std::move(mimes)) {}

    [[nodiscard]] bool hasFormat(std::string_view mime_type) const override {
        for (const auto& m : mimes_) {
            if (m == mime_type) return true;
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> formats() const override {
        return mimes_;
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
        if (!offer_ || !display_) return {};
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) return {};

        wl_data_offer_receive(offer_, std::string(mime_type).c_str(), fds[1]);
        close(fds[1]);
        wl_display_flush(display_);

        std::vector<uint8_t> result;
        char buffer[4096];
        ssize_t bytes_read = 0;
        while ((bytes_read = read(fds[0], buffer, sizeof(buffer))) > 0) {
            result.insert(result.end(), buffer, buffer + bytes_read);
        }
        close(fds[0]);
        return result;
    }

private:
    wl_display*    display_ = nullptr;
    wl_data_offer* offer_   = nullptr;
    std::vector<std::string> mimes_;
};

// ── Wayland Data Offer Listeners ──────────────────────────────────

static void data_offer_offer_handler(void* data, wl_data_offer* /*offer*/, const char* mime_type) {
    auto* ctx = static_cast<WaylandPlatformBackend::WaylandOfferContext*>(data);
    if (ctx && mime_type) {
        ctx->mime_types.emplace_back(mime_type);
    }
}

static void data_offer_source_actions_handler(void* data, wl_data_offer* /*offer*/, uint32_t source_actions) {
    auto* ctx = static_cast<WaylandPlatformBackend::WaylandOfferContext*>(data);
    if (ctx) ctx->source_actions = source_actions;
}

static void data_offer_action_handler(void* data, wl_data_offer* /*offer*/, uint32_t dnd_action) {
    auto* ctx = static_cast<WaylandPlatformBackend::WaylandOfferContext*>(data);
    if (ctx) ctx->dnd_action = dnd_action;
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = data_offer_offer_handler,
    .source_actions = data_offer_source_actions_handler,
    .action = data_offer_action_handler,
};

// ── Wayland Data Source Listeners ─────────────────────────────────

static void data_source_target_handler(void* /*data*/, wl_data_source* /*source*/, const char* /*mime_type*/) {}

static void data_source_send_handler(void* data, wl_data_source* /*source*/, const char* mime_type, int32_t fd) {
    auto* active_src = static_cast<WaylandPlatformBackend::ActiveDataSource*>(data);
    if (active_src && mime_type) {
        auto raw = active_src->data.getRaw(mime_type);
        if (raw.empty() && active_src->data.hasText()) {
            std::string t = active_src->data.getText();
            raw.assign(t.begin(), t.end());
        }
        if (!raw.empty()) {
            size_t total = 0;
            while (total < raw.size()) {
                ssize_t written = write(fd, raw.data() + total, raw.size() - total);
                if (written <= 0) break;
                total += written;
            }
        }
    }
    close(fd);
}

static void data_source_cancelled_handler(void* /*data*/, wl_data_source* source) {
    wl_data_source_destroy(source);
}

static void data_source_dnd_drop_performed_handler(void* /*data*/, wl_data_source* /*source*/) {}
static void data_source_dnd_finished_handler(void* /*data*/, wl_data_source* /*source*/) {}
static void data_source_action_handler(void* /*data*/, wl_data_source* /*source*/, uint32_t /*dnd_action*/) {}

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target_handler,
    .send = data_source_send_handler,
    .cancelled = data_source_cancelled_handler,
    .dnd_drop_performed = data_source_dnd_drop_performed_handler,
    .dnd_finished = data_source_dnd_finished_handler,
    .action = data_source_action_handler,
};

// ── Wayland Data Device Listeners ─────────────────────────────────

static void data_device_data_offer_handler(void* data, wl_data_device* /*data_device*/, wl_data_offer* offer) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleDataOffer(offer);
}

static void data_device_enter_handler(void* data, wl_data_device* /*data_device*/, uint32_t serial,
                                      wl_surface* surface, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleDataDeviceEnter(serial, surface, x, y, offer);
}

static void data_device_leave_handler(void* data, wl_data_device* /*data_device*/) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleDataDeviceLeave();
}

static void data_device_motion_handler(void* data, wl_data_device* /*data_device*/, uint32_t time,
                                       wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleDataDeviceMotion(time, x, y);
}

static void data_device_drop_handler(void* data, wl_data_device* /*data_device*/) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleDataDeviceDrop();
}

static void data_device_selection_handler(void* data, wl_data_device* /*data_device*/, wl_data_offer* offer) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleDataDeviceSelection(offer);
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer_handler,
    .enter = data_device_enter_handler,
    .leave = data_device_leave_handler,
    .motion = data_device_motion_handler,
    .drop = data_device_drop_handler,
    .selection = data_device_selection_handler,
};

// ── Wayland Toplevel Implementation ───────────────────────────────

class WaylandPlatformBackend::WaylandToplevel : public ToplevelWindow {
public:
    WaylandToplevel(zwlr_foreign_toplevel_handle_v1* handle, wl_seat* seat)
        : handle_(handle), seat_(seat) {}

    ~WaylandToplevel() override {
        if (handle_) {
            zwlr_foreign_toplevel_handle_v1_destroy(handle_);
            handle_ = nullptr;
        }
    }

    [[nodiscard]] uint64_t id() const override { return reinterpret_cast<uint64_t>(handle_); }
    [[nodiscard]] std::string title() const override { return title_; }
    [[nodiscard]] std::string appId() const override { return app_id_; }
    [[nodiscard]] WindowState state() const override { return state_; }

    void setTitle(std::string t) { title_ = std::move(t); }
    void setAppId(std::string a) { app_id_ = std::move(a); }
    void setState(WindowState s) { state_ = s; }

    void activate() override {
        if (handle_ && seat_) {
            zwlr_foreign_toplevel_handle_v1_activate(handle_, seat_);
        }
    }

    void setMinimized(bool min) override {
        if (!handle_) return;
        if (min) {
            zwlr_foreign_toplevel_handle_v1_set_minimized(handle_);
        } else {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(handle_);
        }
    }

    void setMaximized(bool max) override {
        if (!handle_) return;
        if (max) {
            zwlr_foreign_toplevel_handle_v1_set_maximized(handle_);
        } else {
            zwlr_foreign_toplevel_handle_v1_unset_maximized(handle_);
        }
    }

    void setFullscreen(bool full) override {
        if (!handle_) return;
        if (full) {
            zwlr_foreign_toplevel_handle_v1_set_fullscreen(handle_, nullptr);
        } else {
            zwlr_foreign_toplevel_handle_v1_unset_fullscreen(handle_);
        }
    }

    void close() override {
        if (handle_) {
            zwlr_foreign_toplevel_handle_v1_close(handle_);
        }
    }

    [[nodiscard]] zwlr_foreign_toplevel_handle_v1* getHandle() const { return handle_; }
    void detachHandle() { handle_ = nullptr; }

private:
    zwlr_foreign_toplevel_handle_v1* handle_ = nullptr;
    wl_seat* seat_ = nullptr;
    std::string title_;
    std::string app_id_;
    WindowState state_ = WindowState::Normal;
};

// ── Foreign Toplevel Listeners ────────────────────────────────────

static void toplevel_handle_title(void* data, zwlr_foreign_toplevel_handle_v1* handle, const char* title) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleToplevelTitle(handle, title);
}

static void toplevel_handle_app_id(void* data, zwlr_foreign_toplevel_handle_v1* handle, const char* app_id) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleToplevelAppId(handle, app_id);
}

static void toplevel_handle_output_enter(void* /*data*/, zwlr_foreign_toplevel_handle_v1* /*handle*/, wl_output* /*output*/) {}
static void toplevel_handle_output_leave(void* /*data*/, zwlr_foreign_toplevel_handle_v1* /*handle*/, wl_output* /*output*/) {}

static void toplevel_handle_state(void* data, zwlr_foreign_toplevel_handle_v1* handle, wl_array* state) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleToplevelState(handle, state);
}

static void toplevel_handle_done(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleToplevelDone(handle);
}

static void toplevel_handle_closed(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleToplevelClosed(handle);
}

static void toplevel_handle_parent(void* /*data*/, zwlr_foreign_toplevel_handle_v1* /*handle*/, zwlr_foreign_toplevel_handle_v1* /*parent*/) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    .title = toplevel_handle_title,
    .app_id = toplevel_handle_app_id,
    .output_enter = toplevel_handle_output_enter,
    .output_leave = toplevel_handle_output_leave,
    .state = toplevel_handle_state,
    .done = toplevel_handle_done,
    .closed = toplevel_handle_closed,
    .parent = toplevel_handle_parent,
};

static void toplevel_manager_toplevel(void* data, zwlr_foreign_toplevel_manager_v1* /*manager*/, zwlr_foreign_toplevel_handle_v1* handle) {
    auto* self = static_cast<WaylandPlatformBackend*>(data);
    self->handleToplevelHandle(handle);
}

static void toplevel_manager_finished(void* /*data*/, zwlr_foreign_toplevel_manager_v1* /*manager*/) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
    .toplevel = toplevel_manager_toplevel,
    .finished = toplevel_manager_finished,
};

void WaylandPlatformBackend::handleSeatCapabilities(wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
        if (!pointer_) {
            pointer_ = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(pointer_, &pointer_listener, this);
        }
    } else if (pointer_) {
        wl_pointer_destroy(pointer_);
        pointer_ = nullptr;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        if (!keyboard_) {
            keyboard_ = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(keyboard_, &keyboard_listener, this);
        }
    } else if (keyboard_) {
        wl_keyboard_destroy(keyboard_);
        keyboard_ = nullptr;
    }

    if (data_device_manager_ && seat_ && !data_device_) {
        data_device_ = wl_data_device_manager_get_data_device(data_device_manager_, seat_);
        wl_data_device_add_listener(data_device_, &data_device_listener, this);
    }
}

// ════════════════════════════════════════════════════════════════
// WaylandPlatformBackend Implementation
// ════════════════════════════════════════════════════════════════

WaylandPlatformBackend::WaylandPlatformBackend(Platform* owner)
    : owner_(owner) {
    xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

WaylandPlatformBackend::~WaylandPlatformBackend() {
    shutdown();
    if (xkb_state_)   xkb_state_unref(xkb_state_);
    if (xkb_keymap_)  xkb_keymap_unref(xkb_keymap_);
    if (xkb_context_) xkb_context_unref(xkb_context_);
}

bool WaylandPlatformBackend::init() {
    // 1. Connect to Wayland display socket
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::cerr << "[Nisaba Wayland] Could not connect to Wayland display server\n";
        return false;
    }

    // 2. Fetch registry globals
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);

    // Initial roundtrip to populate registry globals
    wl_display_roundtrip(display_);

    if (!compositor_) {
        std::cerr << "[Nisaba Wayland] Missing required wl_compositor interface\n";
        return false;
    }

    if (shm_) {
        const char* theme_name = std::getenv("XCURSOR_THEME");
        int size = 24;
        if (const char* size_env = std::getenv("XCURSOR_SIZE")) {
            int s = std::atoi(size_env);
            if (s > 0) size = s;
        }
        cursor_theme_ = wl_cursor_theme_load(theme_name, size, shm_);
    }

    if (compositor_) {
        cursor_surface_ = wl_compositor_create_surface(compositor_);
    }

    if (!layer_shell_) {
        std::cerr << "[Nisaba Wayland] zwlr_layer_shell_v1 not available on this compositor\n";
    }

    // 3. Initialize EGL for Wayland
    egl_display_ = eglGetDisplay((EGLNativeDisplayType)display_);
    if (egl_display_ == EGL_NO_DISPLAY) {
        std::cerr << "[Nisaba Wayland] Failed to get EGL display for Wayland\n";
        return false;
    }

    if (!eglInitialize(egl_display_, &egl_major_, &egl_minor_)) {
        std::cerr << "[Nisaba Wayland] Failed to initialize EGL on Wayland\n";
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        eglBindAPI(EGL_OPENGL_ES_API);
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_STENCIL_SIZE,    8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLint num_configs = 0;
    if (!eglChooseConfig(egl_display_, config_attribs, &egl_config_, 1, &num_configs) || num_configs == 0) {
        const EGLint fallback_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE,     8,
            EGL_GREEN_SIZE,   8,
            EGL_BLUE_SIZE,    8,
            EGL_ALPHA_SIZE,   8,
            EGL_NONE
        };
        eglChooseConfig(egl_display_, fallback_attribs, &egl_config_, 1, &num_configs);
    }

    // Shared master EGL context for Wayland surfaces
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

    return true;
}

void WaylandPlatformBackend::shutdown() {
    if (outgoing_selection_source_) {
        wl_data_source_destroy(outgoing_selection_source_->source);
        outgoing_selection_source_.reset();
    }
    if (outgoing_dnd_source_) {
        wl_data_source_destroy(outgoing_dnd_source_->source);
        outgoing_dnd_source_.reset();
    }
    if (data_device_) {
        wl_data_device_destroy(data_device_);
        data_device_ = nullptr;
    }
    if (data_device_manager_) {
        wl_data_device_manager_destroy(data_device_manager_);
        data_device_manager_ = nullptr;
    }

    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(egl_display_, egl_context_);
            egl_context_ = EGL_NO_CONTEXT;
        }
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }

    if (cursor_surface_) {
        wl_surface_destroy(cursor_surface_);
        cursor_surface_ = nullptr;
    }
    if (cursor_theme_) {
        wl_cursor_theme_destroy(cursor_theme_);
        cursor_theme_ = nullptr;
    }
    if (pointer_) {
        wl_pointer_destroy(pointer_);
        pointer_ = nullptr;
    }
    if (keyboard_) {
        wl_keyboard_destroy(keyboard_);
        keyboard_ = nullptr;
    }

    if (layer_shell_) {
        zwlr_layer_shell_v1_destroy(layer_shell_);
        layer_shell_ = nullptr;
    }
    if (toplevel_manager_) {
        zwlr_foreign_toplevel_manager_v1_stop(toplevel_manager_);
        zwlr_foreign_toplevel_manager_v1_destroy(toplevel_manager_);
        toplevel_manager_ = nullptr;
    }
    toplevels_.clear();
    toplevel_map_.clear();
    active_toplevel_.reset();

    if (xdg_output_manager_) {
        zxdg_output_manager_v1_destroy(xdg_output_manager_);
        xdg_output_manager_ = nullptr;
    }
    if (decoration_manager_) {
        zxdg_decoration_manager_v1_destroy(decoration_manager_);
        decoration_manager_ = nullptr;
    }
    output_map_.clear();
    outputs_.clear();

    if (xdg_wm_base_) {
        xdg_wm_base_destroy(xdg_wm_base_);
        xdg_wm_base_ = nullptr;
    }
    if (compositor_) {
        wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
    }
    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

// ── Wayland Output Implementation & Listeners ────────────────────

class WaylandOutput : public Output {
public:
    WaylandPlatformBackend* backend_ = nullptr;
    uint32_t                id_      = 0;
    wl_output*              output_  = nullptr;
    zxdg_output_v1*         xdg_output_ = nullptr;

    std::string name_;
    std::string make_;
    std::string model_;
    std::string description_;

    int32_t phys_x_ = 0;
    int32_t phys_y_ = 0;
    int32_t phys_w_mm_ = 0;
    int32_t phys_h_mm_ = 0;

    int32_t logic_x_ = 0;
    int32_t logic_y_ = 0;
    int32_t logic_w_ = 0;
    int32_t logic_h_ = 0;

    int32_t scale_ = 1;
    OutputTransform transform_ = OutputTransform::Normal;
    OutputSubpixel subpixel_ = OutputSubpixel::Unknown;

    std::vector<OutputMode> modes_;
    OutputMode current_mode_;
    bool is_primary_ = false;
    bool announced_ = false;

    WaylandOutput(WaylandPlatformBackend* backend, uint32_t id, wl_output* output)
        : backend_(backend), id_(id), output_(output) {}

    ~WaylandOutput() override {
        if (xdg_output_) {
            zxdg_output_v1_destroy(xdg_output_);
            xdg_output_ = nullptr;
        }
        if (output_) {
            wl_output_destroy(output_);
            output_ = nullptr;
        }
    }

    [[nodiscard]] uint32_t id() const noexcept override { return id_; }
    [[nodiscard]] const std::string& name() const noexcept override { return name_; }
    [[nodiscard]] const std::string& make() const noexcept override { return make_; }
    [[nodiscard]] const std::string& model() const noexcept override { return model_; }
    [[nodiscard]] const std::string& description() const noexcept override { return description_; }

    [[nodiscard]] Rect geometry() const noexcept override {
        return Rect{static_cast<float>(phys_x_), static_cast<float>(phys_y_),
                    static_cast<float>(current_mode_.width), static_cast<float>(current_mode_.height)};
    }

    [[nodiscard]] Rect logicalGeometry() const noexcept override {
        if (logic_w_ > 0 && logic_h_ > 0) {
            return Rect{static_cast<float>(logic_x_), static_cast<float>(logic_y_),
                        static_cast<float>(logic_w_), static_cast<float>(logic_h_)};
        }
        float w = current_mode_.width > 0 ? static_cast<float>(current_mode_.width) / (scale_ > 0 ? scale_ : 1) : 0.0f;
        float h = current_mode_.height > 0 ? static_cast<float>(current_mode_.height) / (scale_ > 0 ? scale_ : 1) : 0.0f;
        return Rect{static_cast<float>(phys_x_), static_cast<float>(phys_y_), w, h};
    }

    [[nodiscard]] int32_t physicalWidthMm() const noexcept override { return phys_w_mm_; }
    [[nodiscard]] int32_t physicalHeightMm() const noexcept override { return phys_h_mm_; }
    [[nodiscard]] int32_t scaleFactor() const noexcept override { return scale_; }
    [[nodiscard]] double fractionalScale() const noexcept override {
        if (logic_w_ > 0 && current_mode_.width > 0) {
            return static_cast<double>(current_mode_.width) / logic_w_;
        }
        return static_cast<double>(scale_);
    }
    [[nodiscard]] OutputTransform transform() const noexcept override { return transform_; }
    [[nodiscard]] OutputSubpixel subpixel() const noexcept override { return subpixel_; }
    [[nodiscard]] const std::vector<OutputMode>& modes() const noexcept override { return modes_; }
    [[nodiscard]] const OutputMode& currentMode() const noexcept override { return current_mode_; }
    [[nodiscard]] bool isPrimary() const noexcept override { return is_primary_; }
    [[nodiscard]] void* nativeHandle() const noexcept override { return output_; }
};

static void output_geometry_handler(void* data, struct wl_output* /*wl_output*/,
                                    int32_t x, int32_t y,
                                    int32_t physical_width, int32_t physical_height,
                                    int32_t subpixel,
                                    const char* make, const char* model,
                                    int32_t transform) {
    auto* out = static_cast<WaylandOutput*>(data);
    out->phys_x_ = x;
    out->phys_y_ = y;
    out->phys_w_mm_ = physical_width;
    out->phys_h_mm_ = physical_height;
    out->subpixel_ = static_cast<OutputSubpixel>(subpixel);
    if (make) out->make_ = make;
    if (model) out->model_ = model;
    out->transform_ = static_cast<OutputTransform>(transform);
}

static void output_mode_handler(void* data, struct wl_output* /*wl_output*/,
                                uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    auto* out = static_cast<WaylandOutput*>(data);
    OutputMode mode;
    mode.width = width;
    mode.height = height;
    mode.refresh_rate_mHz = refresh;
    mode.is_current = (flags & WL_OUTPUT_MODE_CURRENT) != 0;
    mode.is_preferred = (flags & WL_OUTPUT_MODE_PREFERRED) != 0;

    bool found = false;
    for (auto& m : out->modes_) {
        if (m.width == width && m.height == height && m.refresh_rate_mHz == refresh) {
            m.is_current = mode.is_current;
            m.is_preferred = mode.is_preferred;
            found = true;
            break;
        }
    }
    if (!found) {
        out->modes_.push_back(mode);
    }
    if (mode.is_current) {
        out->current_mode_ = mode;
    }
}

static void output_done_handler(void* data, struct wl_output* /*wl_output*/) {
    auto* out = static_cast<WaylandOutput*>(data);
    if (out->description_.empty()) {
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
    }
    if (!out->announced_) {
        out->announced_ = true;
        if (out->backend_ && out->backend_->getOwner()) {
            auto ptr = out->backend_->findOutputByPtr(out);
            if (ptr) {
                out->backend_->getOwner()->onOutputAdded().emit(ptr);
            }
        }
    } else {
        out->onGeometryChanged().emit();
        out->onModeChanged().emit();
        if (out->backend_ && out->backend_->getOwner()) {
            auto ptr = out->backend_->findOutputByPtr(out);
            if (ptr) {
                out->backend_->getOwner()->onOutputChanged().emit(ptr);
            }
        }
    }
}

static void output_scale_handler(void* data, struct wl_output* /*wl_output*/, int32_t factor) {
    auto* out = static_cast<WaylandOutput*>(data);
    out->scale_ = factor;
    out->onScaleChanged().emit();
}

static void output_name_handler(void* data, struct wl_output* /*wl_output*/, const char* name) {
    auto* out = static_cast<WaylandOutput*>(data);
    if (name) out->name_ = name;
}

static void output_description_handler(void* data, struct wl_output* /*wl_output*/, const char* description) {
    auto* out = static_cast<WaylandOutput*>(data);
    if (description) out->description_ = description;
}

static const struct wl_output_listener wayland_output_listener = {
    .geometry = output_geometry_handler,
    .mode = output_mode_handler,
    .done = output_done_handler,
    .scale = output_scale_handler,
    .name = output_name_handler,
    .description = output_description_handler,
};

static void xdg_output_logical_position_handler(void* data, struct zxdg_output_v1* /*zxdg_output_v1*/, int32_t x, int32_t y) {
    auto* out = static_cast<WaylandOutput*>(data);
    out->logic_x_ = x;
    out->logic_y_ = y;
}

static void xdg_output_logical_size_handler(void* data, struct zxdg_output_v1* /*zxdg_output_v1*/, int32_t width, int32_t height) {
    auto* out = static_cast<WaylandOutput*>(data);
    out->logic_w_ = width;
    out->logic_h_ = height;
}

static void xdg_output_done_handler(void* data, struct zxdg_output_v1* /*zxdg_output_v1*/) {
    auto* out = static_cast<WaylandOutput*>(data);
    out->onGeometryChanged().emit();
}

static void xdg_output_name_handler(void* data, struct zxdg_output_v1* /*zxdg_output_v1*/, const char* name) {
    auto* out = static_cast<WaylandOutput*>(data);
    if (name && out->name_.empty()) out->name_ = name;
}

static void xdg_output_description_handler(void* data, struct zxdg_output_v1* /*zxdg_output_v1*/, const char* description) {
    auto* out = static_cast<WaylandOutput*>(data);
    if (description && out->description_.empty()) out->description_ = description;
}

static const struct zxdg_output_v1_listener wayland_xdg_output_listener = {
    .logical_position = xdg_output_logical_position_handler,
    .logical_size = xdg_output_logical_size_handler,
    .done = xdg_output_done_handler,
    .name = xdg_output_name_handler,
    .description = xdg_output_description_handler,
};

// ── XDG Shell Base Listener (Ping/Pong) ───────────────────────────

static void xdg_wm_base_ping_handler(void* /*data*/, xdg_wm_base* wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping_handler,
};

void WaylandPlatformBackend::handleGlobal(uint32_t name, const char* interface, uint32_t version) {
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry_, name, &wl_compositor_interface, std::min<uint32_t>(version, 4)));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
        subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry_, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        shm_ = static_cast<wl_shm*>(
            wl_registry_bind(registry_, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        seat_ = static_cast<wl_seat*>(
            wl_registry_bind(registry_, name, &wl_seat_interface, std::min<uint32_t>(version, 5)));
        wl_seat_add_listener(seat_, &seat_listener, this);
        if (data_device_manager_ && !data_device_) {
            data_device_ = wl_data_device_manager_get_data_device(data_device_manager_, seat_);
            wl_data_device_add_listener(data_device_, &data_device_listener, this);
        }
    } else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        data_device_manager_ = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry_, name, &wl_data_device_manager_interface, std::min<uint32_t>(version, 3)));
        if (seat_ && !data_device_) {
            data_device_ = wl_data_device_manager_get_data_device(data_device_manager_, seat_);
            wl_data_device_add_listener(data_device_, &data_device_listener, this);
        }
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry_, name, &zwlr_layer_shell_v1_interface, std::min<uint32_t>(version, 4)));
    } else if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        toplevel_manager_ = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(registry_, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min<uint32_t>(version, 3)));
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager_, &toplevel_manager_listener, this);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        xdg_wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry_, name, &xdg_wm_base_interface, std::min<uint32_t>(version, 3)));
        xdg_wm_base_add_listener(xdg_wm_base_, &wm_base_listener, this);
    } else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        xdg_output_manager_ = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry_, name, &zxdg_output_manager_v1_interface, std::min<uint32_t>(version, 3)));
        for (auto& out : outputs_) {
            if (out->output_ && !out->xdg_output_) {
                out->xdg_output_ = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager_, out->output_);
                zxdg_output_v1_add_listener(out->xdg_output_, &wayland_xdg_output_listener, out.get());
            }
        }
    } else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        decoration_manager_ = static_cast<zxdg_decoration_manager_v1*>(
            wl_registry_bind(registry_, name, &zxdg_decoration_manager_v1_interface, 1));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto* output = static_cast<wl_output*>(
            wl_registry_bind(registry_, name, &wl_output_interface, std::min<uint32_t>(version, 4)));
        auto out = std::make_shared<WaylandOutput>(this, name, output);
        wl_output_add_listener(output, &wayland_output_listener, out.get());
        if (xdg_output_manager_) {
            out->xdg_output_ = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager_, output);
            zxdg_output_v1_add_listener(out->xdg_output_, &wayland_xdg_output_listener, out.get());
        }
        outputs_.push_back(out);
        output_map_[output] = out;
    }
}

void WaylandPlatformBackend::handleGlobalRemove(uint32_t name) {
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if ((*it)->id() == name) {
            auto out = *it;
            out->onRemoved().emit();
            if (owner_) {
                owner_->onOutputRemoved().emit(out);
            }
            output_map_.erase(out->output_);
            outputs_.erase(it);
            break;
        }
    }
}

std::vector<std::shared_ptr<Output>> WaylandPlatformBackend::getOutputs() const {
    std::vector<std::shared_ptr<Output>> res;
    res.reserve(outputs_.size());
    for (const auto& o : outputs_) {
        res.push_back(o);
    }
    return res;
}

std::shared_ptr<Output> WaylandPlatformBackend::getOutputByName(std::string_view name) const {
    for (const auto& o : outputs_) {
        if (o->name() == name) return o;
    }
    return nullptr;
}

std::shared_ptr<Output> WaylandPlatformBackend::getPrimaryOutput() const {
    if (outputs_.empty()) return nullptr;
    return outputs_.front();
}

std::shared_ptr<WaylandOutput> WaylandPlatformBackend::findOutputByPtr(const WaylandOutput* ptr) const {
    for (const auto& o : outputs_) {
        if (o.get() == ptr) return o;
    }
    return nullptr;
}

bool WaylandPlatformBackend::pollEvents() {
    if (!display_) return false;

    // 1. Process pending incoming events
    while (wl_display_prepare_read(display_) != 0) {
        wl_display_dispatch_pending(display_);
    }

    wl_display_flush(display_);

    // 2. Poll socket without blocking
    struct pollfd pfd;
    pfd.fd     = wl_display_get_fd(display_);
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 0); // Non-blocking
    if (ret > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(display_);
    } else {
        wl_display_cancel_read(display_);
    }

    wl_display_dispatch_pending(display_);
    
    // 3. Process Key Repeat
    if (repeating_key_ != 0 && repeat_rate_ > 0 && xkb_state_ && owner_) {
        double current_time = owner_->getTime();
        if (current_time >= repeat_next_fire_time_) {
            xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, repeating_keycode_);
            owner_->onKeyDown().emit(static_cast<int>(sym), active_modifiers_);
            
            char utf8_buf[64];
            int len = xkb_state_key_get_utf8(xkb_state_, repeating_keycode_, utf8_buf, sizeof(utf8_buf));
            if (len > 0) {
                owner_->onTextInput().emit(std::string(utf8_buf, len));
            }
            
            repeat_next_fire_time_ = current_time + (1.0 / repeat_rate_);
        }
    }

    return true;
}

void WaylandPlatformBackend::registerSurface(LayerSurface* surface) {
    if (surface) surfaces_.insert(surface);
}

void WaylandPlatformBackend::unregisterSurface(LayerSurface* surface) {
    if (surface) surfaces_.erase(surface);
}

void WaylandPlatformBackend::setCursor(SystemCursor cursor) {
    if (current_cursor_ == cursor) return;
    current_cursor_ = cursor;
    updateWaylandCursor();
}

void WaylandPlatformBackend::updateWaylandCursor() {
    if (!pointer_ || !cursor_surface_ || !cursor_theme_ || !pointer_focus_surface_ || last_pointer_serial_ == 0) {
        return;
    }

    const char* names[4] = { "default", "left_ptr", nullptr, nullptr };
    switch (current_cursor_) {
        case SystemCursor::Pointer:
            names[0] = "pointer";
            names[1] = "hand2";
            names[2] = "hand";
            break;
        case SystemCursor::Text:
            names[0] = "text";
            names[1] = "xterm";
            names[2] = "ibeam";
            break;
        case SystemCursor::Crosshair:
            names[0] = "crosshair";
            break;
        case SystemCursor::Move:
            names[0] = "move";
            names[1] = "grab";
            break;
        case SystemCursor::NotAllowed:
            names[0] = "not-allowed";
            names[1] = "forbidden";
            break;
        case SystemCursor::ResizeHorizontal:
            names[0] = "ew-resize";
            names[1] = "h_double_arrow";
            break;
        case SystemCursor::ResizeVertical:
            names[0] = "ns-resize";
            names[1] = "v_double_arrow";
            break;
        case SystemCursor::ResizeTopLeft:
        case SystemCursor::ResizeBottomRight:
            names[0] = "nwse-resize";
            names[1] = "nw-resize";
            names[2] = "se-resize";
            break;
        case SystemCursor::ResizeTopRight:
        case SystemCursor::ResizeBottomLeft:
            names[0] = "nesw-resize";
            names[1] = "ne-resize";
            names[2] = "sw-resize";
            break;
        case SystemCursor::Wait:
            names[0] = "wait";
            names[1] = "watch";
            break;
        case SystemCursor::Default:
        case SystemCursor::Arrow:
        default:
            names[0] = "default";
            names[1] = "left_ptr";
            names[2] = "arrow";
            break;
    }

    wl_cursor* cur = nullptr;
    for (int i = 0; i < 3 && names[i]; ++i) {
        cur = wl_cursor_theme_get_cursor(cursor_theme_, names[i]);
        if (cur) break;
    }
    if (!cur) {
        cur = wl_cursor_theme_get_cursor(cursor_theme_, "left_ptr");
    }
    if (!cur) {
        cur = wl_cursor_theme_get_cursor(cursor_theme_, "default");
    }
    if (!cur || cur->image_count == 0) return;

    wl_cursor_image* img = cur->images[0];
    wl_buffer* buf = wl_cursor_image_get_buffer(img);
    if (!buf) return;

    wl_pointer_set_cursor(pointer_, last_pointer_serial_, cursor_surface_, img->hotspot_x, img->hotspot_y);
    wl_surface_attach(cursor_surface_, buf, 0, 0);
    wl_surface_damage(cursor_surface_, 0, 0, img->width, img->height);
    wl_surface_commit(cursor_surface_);
}

// ── Event Forwarding to Nisaba Platform Signals ──────────────────

void WaylandPlatformBackend::handlePointerEnter(uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy) {
    pointer_focus_surface_ = surface;
    last_pointer_serial_ = serial;
    last_px_ = wl_fixed_to_double(sx);
    last_py_ = wl_fixed_to_double(sy);

    updateWaylandCursor();

    if (owner_) {
        owner_->onMouseMove().emit(last_px_, last_py_);
        owner_->onTargetedMouseMove().emit((void*)pointer_focus_surface_, last_px_, last_py_);
    }
}

void WaylandPlatformBackend::handlePointerLeave(wl_surface* surface) {
    if (pointer_focus_surface_ == surface) {
        pointer_focus_surface_ = nullptr;
    }
    if (owner_) {
        owner_->onMouseMove().emit(-10000.0f, -10000.0f);
        owner_->onTargetedMouseMove().emit((void*)surface, -10000.0f, -10000.0f);
    }
}

void WaylandPlatformBackend::handlePointerMotion([[maybe_unused]] uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    last_px_ = wl_fixed_to_double(sx);
    last_py_ = wl_fixed_to_double(sy);
    if (owner_) {
        owner_->onMouseMove().emit(last_px_, last_py_);
        owner_->onTargetedMouseMove().emit((void*)pointer_focus_surface_, last_px_, last_py_);
    }
}

void WaylandPlatformBackend::handlePointerButton(uint32_t serial, [[maybe_unused]] uint32_t time, uint32_t button, uint32_t state) {
    if (!owner_) return;

    last_pointer_serial_ = serial;

    // Linux button codes: BTN_LEFT (0x110/272)=1, BTN_RIGHT (0x111/273)=3, BTN_MIDDLE (0x112/274)=2
    int btn_code = 1;
    if (button == 273) btn_code = 3;       // Right
    else if (button == 274) btn_code = 2;  // Middle

    void* win_handle = (void*)pointer_focus_surface_;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        owner_->onMouseDown().emit(last_px_, last_py_, btn_code);
        owner_->onTargetedMouseDown().emit(win_handle, last_px_, last_py_, btn_code);
    } else {
        owner_->onMouseUp().emit(last_px_, last_py_, btn_code);
        owner_->onTargetedMouseUp().emit(win_handle, last_px_, last_py_, btn_code);
    }
}

void WaylandPlatformBackend::handlePointerAxis([[maybe_unused]] uint32_t time, uint32_t axis, wl_fixed_t value) {
    if (!owner_) return;
    double v = wl_fixed_to_double(value);
    void* win_handle = (void*)pointer_focus_surface_;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        float dy = v < 0 ? 1.0f : -1.0f;
        owner_->onScroll().emit(0.0f, dy);
        owner_->onTargetedScroll().emit(win_handle, 0.0f, dy);
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        float dx = v < 0 ? -1.0f : 1.0f;
        owner_->onScroll().emit(dx, 0.0f);
        owner_->onTargetedScroll().emit(win_handle, dx, 0.0f);
    }
}

void WaylandPlatformBackend::handleKeymap(uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) return;

    char* map_shm = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
    if (map_shm == MAP_FAILED) return;

    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    xkb_keymap_ = xkb_keymap_new_from_string(xkb_context_, map_shm,
                                            XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size);

    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (xkb_keymap_) {
        xkb_state_ = xkb_state_new(xkb_keymap_);
    }
}

void WaylandPlatformBackend::handleKeyboardEnter(uint32_t serial, wl_surface* /*surface*/) {
    last_keyboard_serial_ = serial;
}

void WaylandPlatformBackend::setKeyRepeatInfo(int32_t rate, int32_t delay) {
    repeat_rate_ = rate;
    repeat_delay_ = delay;
}

void WaylandPlatformBackend::handleKey(uint32_t serial, [[maybe_unused]] uint32_t time, uint32_t key, uint32_t state) {
    if (!xkb_state_ || !owner_) return;

    last_keyboard_serial_ = serial;

    uint32_t keycode = key + 8; // evdev -> XKB offset
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, keycode);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        owner_->onKeyDown().emit(static_cast<int>(sym), active_modifiers_);

        char utf8_buf[64];
        int len = xkb_state_key_get_utf8(xkb_state_, keycode, utf8_buf, sizeof(utf8_buf));
        if (len > 0) {
            owner_->onTextInput().emit(std::string(utf8_buf, len));
        }
        
        // Start repeat
        if (repeat_rate_ > 0 && xkb_keymap_key_repeats(xkb_keymap_, keycode)) {
            repeating_key_ = key;
            repeating_keycode_ = keycode;
            repeat_next_fire_time_ = owner_->getTime() + (repeat_delay_ / 1000.0);
        }
    } else {
        owner_->onKeyUp().emit(static_cast<int>(sym), active_modifiers_);
        if (repeating_key_ == key) {
            repeating_key_ = 0;
            repeating_keycode_ = 0;
        }
    }
}

void WaylandPlatformBackend::handleModifiers(uint32_t mods_depressed, uint32_t mods_latched,
                                            uint32_t mods_locked, uint32_t group) {
    if (!xkb_state_) return;
    xkb_state_update_mask(xkb_state_, mods_depressed, mods_latched, mods_locked, 0, 0, group);

    active_modifiers_ = 0;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) active_modifiers_ |= 1;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_CTRL,  XKB_STATE_MODS_EFFECTIVE)) active_modifiers_ |= 2;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_ALT,   XKB_STATE_MODS_EFFECTIVE)) active_modifiers_ |= 4;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_LOGO,  XKB_STATE_MODS_EFFECTIVE)) active_modifiers_ |= 8;
}

// ── Wayland Data Device, Clipboard & DnD Subsystem ───────────────

void WaylandPlatformBackend::handleDataOffer(wl_data_offer* offer) {
    if (!offer) return;
    auto ctx = std::make_shared<WaylandOfferContext>();
    ctx->offer = offer;
    active_offers_[offer] = ctx;
    wl_data_offer_add_listener(offer, &data_offer_listener, ctx.get());
}

void WaylandPlatformBackend::handleDataDeviceEnter(uint32_t serial, wl_surface* /*surface*/, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer) {
    last_dnd_serial_ = serial;
    if (offer && active_offers_.find(offer) != active_offers_.end()) {
        current_dnd_offer_ = active_offers_[offer];
        wl_data_offer_set_actions(offer,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        for (const auto& mime : current_dnd_offer_->mime_types) {
            wl_data_offer_accept(offer, serial, mime.c_str());
            break;
        }
    }

    if (owner_) {
        DragEnterEvent ev;
        ev.position = Point{static_cast<float>(wl_fixed_to_double(x)), static_cast<float>(wl_fixed_to_double(y))};
        if (current_dnd_offer_) {
            ev.mime_types = current_dnd_offer_->mime_types;
        }
        ev.suggested_action = DragAction::Copy;
        owner_->onDragEnter().emit(ev);
    }
}

void WaylandPlatformBackend::handleDataDeviceLeave() {
    current_dnd_offer_.reset();
    if (owner_) {
        DragLeaveEvent ev;
        owner_->onDragLeave().emit(ev);
    }
}

void WaylandPlatformBackend::handleDataDeviceMotion(uint32_t /*time*/, wl_fixed_t x, wl_fixed_t y) {
    if (owner_) {
        DragMotionEvent ev;
        ev.position = Point{static_cast<float>(wl_fixed_to_double(x)), static_cast<float>(wl_fixed_to_double(y))};
        ev.suggested_action = DragAction::Copy;
        owner_->onDragMotion().emit(ev);
    }
}

void WaylandPlatformBackend::handleDataDeviceDrop() {
    if (owner_ && current_dnd_offer_) {
        DropEvent ev;
        ev.position = Point{last_px_, last_py_};
        ev.action = DragAction::Copy;
        ev.data = std::make_shared<WaylandDataOfferWrapper>(display_, current_dnd_offer_->offer, current_dnd_offer_->mime_types);
        owner_->onDrop().emit(ev);

        if (current_dnd_offer_->offer) {
            wl_data_offer_finish(current_dnd_offer_->offer);
        }
    }
    current_dnd_offer_.reset();
}

void WaylandPlatformBackend::handleDataDeviceSelection(wl_data_offer* offer) {
    if (offer && active_offers_.find(offer) != active_offers_.end()) {
        current_selection_offer_ = active_offers_[offer];
    } else {
        current_selection_offer_.reset();
    }
    if (owner_) {
        owner_->onClipboardChanged().emit(ClipboardType::Clipboard);
    }
}

void WaylandPlatformBackend::setClipboardData(const ClipboardData& data, ClipboardType type) {
    if (type == ClipboardType::Primary) {
        local_primary_ = data;
        return;
    }

    local_clipboard_ = data;

    if (!data_device_manager_ || !data_device_) return;

    if (outgoing_selection_source_) {
        wl_data_source_destroy(outgoing_selection_source_->source);
        outgoing_selection_source_.reset();
    }

    auto src_obj = wl_data_device_manager_create_data_source(data_device_manager_);
    if (!src_obj) return;

    outgoing_selection_source_ = std::make_unique<ActiveDataSource>();
    outgoing_selection_source_->source = src_obj;
    outgoing_selection_source_->data = data;

    wl_data_source_add_listener(src_obj, &data_source_listener, outgoing_selection_source_.get());

    for (const auto& mime : data.formats()) {
        wl_data_source_offer(src_obj, mime.c_str());
    }
    if (data.hasText()) {
        wl_data_source_offer(src_obj, std::string(mime::TextPlainUtf8).c_str());
        wl_data_source_offer(src_obj, std::string(mime::TextPlain).c_str());
        wl_data_source_offer(src_obj, std::string(mime::TextUtf8).c_str());
        wl_data_source_offer(src_obj, std::string(mime::TextString).c_str());
    }

    uint32_t serial = last_keyboard_serial_ >= last_pointer_serial_ ? last_keyboard_serial_ : last_pointer_serial_;
    if (serial == 0) serial = last_pointer_serial_ ? last_pointer_serial_ : last_keyboard_serial_;
    wl_data_device_set_selection(data_device_, src_obj, serial);
    wl_display_flush(display_);
}

ClipboardData WaylandPlatformBackend::getClipboardData(ClipboardType type) const {
    if (type == ClipboardType::Primary) {
        return local_primary_;
    }

    if (current_selection_offer_ && display_) {
        ClipboardData result;
        for (const auto& mime : current_selection_offer_->mime_types) {
            auto raw = const_cast<WaylandPlatformBackend*>(this)->getClipboardDataForMime(mime, type);
            if (!raw.empty()) {
                result.setRaw(mime, raw);
            }
        }
        if (!result.empty()) return result;
    }
    return local_clipboard_;
}

std::vector<uint8_t> WaylandPlatformBackend::getClipboardDataForMime(std::string_view mime_type, ClipboardType type) const {
    if (type == ClipboardType::Primary) {
        return local_primary_.getRaw(mime_type);
    }

    if (current_selection_offer_ && current_selection_offer_->offer && display_) {
        bool format_supported = false;
        for (const auto& m : current_selection_offer_->mime_types) {
            if (m == mime_type) {
                format_supported = true;
                break;
            }
        }
        if (format_supported) {
            int fds[2];
            if (pipe2(fds, O_CLOEXEC) == 0) {
                wl_data_offer_receive(current_selection_offer_->offer, std::string(mime_type).c_str(), fds[1]);
                close(fds[1]);
                wl_display_flush(display_);
                wl_display_roundtrip(display_);

                std::vector<uint8_t> result;
                char buffer[4096];
                ssize_t bytes_read = 0;
                while ((bytes_read = read(fds[0], buffer, sizeof(buffer))) > 0) {
                    result.insert(result.end(), buffer, buffer + bytes_read);
                }
                close(fds[0]);
                if (!result.empty()) return result;
            }
        }
    }
    return local_clipboard_.getRaw(mime_type);
}

std::vector<std::string> WaylandPlatformBackend::getClipboardFormats(ClipboardType type) const {
    if (type == ClipboardType::Primary) {
        return local_primary_.formats();
    }
    if (current_selection_offer_ && !current_selection_offer_->mime_types.empty()) {
        return current_selection_offer_->mime_types;
    }
    return local_clipboard_.formats();
}

bool WaylandPlatformBackend::hasClipboardFormat(std::string_view mime_type, ClipboardType type) const {
    if (type == ClipboardType::Primary) {
        return local_primary_.hasFormat(mime_type);
    }
    if (current_selection_offer_) {
        for (const auto& m : current_selection_offer_->mime_types) {
            if (m == mime_type) return true;
        }
    }
    return local_clipboard_.hasFormat(mime_type);
}

bool WaylandPlatformBackend::startDrag(const DragData& data, DragAction actions) {
    if (!data_device_manager_ || !data_device_ || !pointer_focus_surface_) {
        return false;
    }

    if (outgoing_dnd_source_) {
        wl_data_source_destroy(outgoing_dnd_source_->source);
        outgoing_dnd_source_.reset();
    }

    auto src_obj = wl_data_device_manager_create_data_source(data_device_manager_);
    if (!src_obj) return false;

    outgoing_dnd_source_ = std::make_unique<ActiveDataSource>();
    outgoing_dnd_source_->source = src_obj;
    outgoing_dnd_source_->data = data.payload;

    wl_data_source_add_listener(src_obj, &data_source_listener, outgoing_dnd_source_.get());

    for (const auto& mime : data.payload.formats()) {
        wl_data_source_offer(src_obj, mime.c_str());
    }

    uint32_t wl_actions = 0;
    if (hasDragAction(actions, DragAction::Copy)) wl_actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    if (hasDragAction(actions, DragAction::Move)) wl_actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    wl_data_source_set_actions(src_obj, wl_actions);

    uint32_t serial = last_pointer_serial_ ? last_pointer_serial_ : last_keyboard_serial_;
    wl_data_device_start_drag(data_device_, src_obj, pointer_focus_surface_, nullptr /* icon */, serial);
    wl_display_flush(display_);
    return true;
}

// ── Foreign Toplevel Subsystem Implementation ─────────────────────

std::vector<std::shared_ptr<ToplevelWindow>> WaylandPlatformBackend::getToplevels() const {
    std::vector<std::shared_ptr<ToplevelWindow>> result;
    result.reserve(toplevels_.size());
    for (const auto& tl : toplevels_) {
        result.push_back(tl);
    }
    return result;
}

std::shared_ptr<ToplevelWindow> WaylandPlatformBackend::getActiveToplevel() const {
    return active_toplevel_;
}

void WaylandPlatformBackend::handleToplevelHandle(zwlr_foreign_toplevel_handle_v1* handle) {
    if (!handle) return;
    auto tl = std::make_shared<WaylandToplevel>(handle, seat_);
    toplevel_map_[handle] = tl;
    toplevels_.push_back(tl);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle_listener, this);
}

void WaylandPlatformBackend::handleToplevelTitle(zwlr_foreign_toplevel_handle_v1* handle, const char* title) {
    auto it = toplevel_map_.find(handle);
    if (it != toplevel_map_.end()) {
        it->second->setTitle(title ? title : "");
        if (owner_) {
            owner_->onToplevelTitleChanged().emit(it->second, it->second->title());
        }
    }
}

void WaylandPlatformBackend::handleToplevelAppId(zwlr_foreign_toplevel_handle_v1* handle, const char* app_id) {
    auto it = toplevel_map_.find(handle);
    if (it != toplevel_map_.end()) {
        it->second->setAppId(app_id ? app_id : "");
        if (owner_) {
            owner_->onToplevelAppIdChanged().emit(it->second, it->second->appId());
        }
    }
}

void WaylandPlatformBackend::handleToplevelState(zwlr_foreign_toplevel_handle_v1* handle, wl_array* state) {
    auto it = toplevel_map_.find(handle);
    if (it != toplevel_map_.end()) {
        WindowState s = WindowState::Normal;
        if (state && state->data && state->size >= sizeof(uint32_t)) {
            const auto* entries = static_cast<const uint32_t*>(state->data);
            size_t count = state->size / sizeof(uint32_t);
            for (size_t i = 0; i < count; ++i) {
                uint32_t entry = entries[i];
                if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) {
                    s |= WindowState::Maximized;
                } else if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
                    s |= WindowState::Minimized;
                } else if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
                    s |= WindowState::Activated;
                } else if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) {
                    s |= WindowState::Fullscreen;
                }
            }
        }
        it->second->setState(s);
        if (hasWindowState(s, WindowState::Activated)) {
            active_toplevel_ = it->second;
            if (owner_) {
                owner_->onActiveToplevelChanged().emit(it->second);
            }
        } else if (active_toplevel_ == it->second) {
            active_toplevel_.reset();
        }
        if (owner_) {
            owner_->onToplevelStateChanged().emit(it->second, s);
        }
    }
}

void WaylandPlatformBackend::handleToplevelDone(zwlr_foreign_toplevel_handle_v1* handle) {
    auto it = toplevel_map_.find(handle);
    if (it != toplevel_map_.end()) {
        if (owner_) {
            owner_->onToplevelCreated().emit(it->second);
        }
    }
}

void WaylandPlatformBackend::handleToplevelClosed(zwlr_foreign_toplevel_handle_v1* handle) {
    auto it = toplevel_map_.find(handle);
    if (it != toplevel_map_.end()) {
        auto tl = it->second;
        tl->detachHandle();
        if (active_toplevel_ == tl) {
            active_toplevel_.reset();
            if (owner_) {
                owner_->onActiveToplevelChanged().emit(nullptr);
            }
        }
        for (auto v_it = toplevels_.begin(); v_it != toplevels_.end(); ++v_it) {
            if (*v_it == tl) {
                toplevels_.erase(v_it);
                break;
            }
        }
        toplevel_map_.erase(it);
        if (owner_) {
            owner_->onToplevelClosed().emit(tl);
        }
    }
}

} // namespace nisaba::backend_os::wayland
