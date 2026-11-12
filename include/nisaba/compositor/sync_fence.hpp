#pragma once

#include "nisaba/compositor/egl_context.hpp"
#include "nisaba/compositor/fd.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <memory>

namespace nisaba::compositor {

/**
 * @brief Sovereign GPU Explicit Synchronization Fence (linux-drm-syncobj-v1 / sync_file).
 *
 * Wraps EGL_KHR_fence_sync, EGL_ANDROID_native_fence_sync, and EGL_KHR_wait_sync
 * to provide zero-stall GPU-to-KMS and Client-to-GPU explicit synchronization.
 */
class SyncFence {
public:
    /**
     * @brief Checks whether the active EGL display supports native Android/Linux fence fds.
     */
    [[nodiscard]] static bool isSupported(const EglContext& egl) noexcept;

    /**
     * @brief Inserts a fence command into the active GL command stream and exports a Linux sync_file fd.
     */
    [[nodiscard]] static std::unique_ptr<SyncFence> exportFromCurrentContext(const EglContext& egl);

    /**
     * @brief Creates a SyncFence wrapping an already-exported native fence file descriptor.
     */
    [[nodiscard]] static std::unique_ptr<SyncFence> fromNativeFd(UniqueFd fd) noexcept;

    /**
     * @brief Instructs the GPU command processor to wait on an external client fence fd without blocking the CPU.
     */
    static bool waitOnGpu(const EglContext& egl, int fenceFd) noexcept;

    SyncFence(const EglContext* egl, EGLSyncKHR sync, UniqueFd fd) noexcept;
    ~SyncFence() noexcept;

    SyncFence(const SyncFence&) = delete;
    SyncFence& operator=(const SyncFence&) = delete;
    SyncFence(SyncFence&& other) noexcept;
    SyncFence& operator=(SyncFence&& other) noexcept;

    [[nodiscard]] bool isValid() const noexcept { return m_fd.isValid() || m_sync != EGL_NO_SYNC_KHR; }
    [[nodiscard]] int fd() const noexcept { return m_fd.get(); }
    [[nodiscard]] UniqueFd takeFd() noexcept { return std::move(m_fd); }
    [[nodiscard]] UniqueFd duplicateFd() const noexcept;

    /**
     * @brief Performs a CPU-side wait on the fence up to timeoutNs nanoseconds.
     */
    bool clientWait(uint64_t timeoutNs = 1'000'000'000ULL) const noexcept;

private:
    const EglContext* m_egl{nullptr};
    EGLSyncKHR m_sync{EGL_NO_SYNC_KHR};
    UniqueFd m_fd;
};

} // namespace nisaba::compositor

