/// @file layer_surface.cpp
/// @brief Platform-agnostic LayerSurface::create factory (Wayland wlr-layer-shell & X11 EWMH+XShape).

#include "nisaba/backend_os/layer_surface.hpp"

#ifdef NISABA_BACKEND_OS_HAS_WAYLAND
#include "nisaba/backend_os/wayland/wayland_platform.hpp"
#include "nisaba/backend_os/wayland/wayland_surface.hpp"
#endif

#ifdef NISABA_BACKEND_OS_HAS_X11
#include "nisaba/backend_os/x11/x11_platform.hpp"
#include "nisaba/backend_os/x11/x11_layer_surface.hpp"
#endif

namespace nisaba::backend_os {

Result<std::unique_ptr<LayerSurface>> LayerSurface::create(Platform& platform, LayerSurfaceConfig config) {
#ifdef NISABA_BACKEND_OS_HAS_WAYLAND
    if (auto* wayland_backend = static_cast<wayland::WaylandPlatformBackend*>(platform.getWaylandBackend())) {
        auto surface = std::make_unique<wayland::WaylandLayerSurface>(*wayland_backend, config);
        if (!surface->init()) {
            return Result<std::unique_ptr<LayerSurface>>::err(
                ErrorCode::WindowError,
                "Failed to initialize Wayland Layer Surface"
            );
        }
        return Result<std::unique_ptr<LayerSurface>>::ok(std::move(surface));
    }
#endif

#ifdef NISABA_BACKEND_OS_HAS_X11
    if (auto* x11_backend = static_cast<x11::X11PlatformBackend*>(platform.getX11Backend())) {
        auto surface = std::make_unique<x11::X11LayerSurface>(*x11_backend, config);
        if (!surface->init()) {
            return Result<std::unique_ptr<LayerSurface>>::err(
                ErrorCode::WindowError,
                "Failed to initialize X11 Layer Surface"
            );
        }
        return Result<std::unique_ptr<LayerSurface>>::ok(std::move(surface));
    }
#endif

    return Result<std::unique_ptr<LayerSurface>>::err(
        ErrorCode::PlatformError,
        "LayerSurface requires Wayland (zwlr_layer_shell_v1) or X11"
    );
}

} // namespace nisaba::backend_os
