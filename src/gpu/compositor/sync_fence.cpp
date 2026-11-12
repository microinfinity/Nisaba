#include "nisaba/compositor/sync_fence.hpp"
#include "nisaba/compositor/log.hpp"

#include <GLES2/gl2.h>
#include <poll.h>
#include <unistd.h>

namespace nisaba::compositor {

namespace {

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif

#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#endif

#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif

} // namespace

bool SyncFence::isSupported(const EglContext& egl) noexcept {
    return egl.hasExtension("EGL_ANDROID_native_fence_sync") &&
           egl.hasExtension("EGL_KHR_fence_sync");
}

std::unique_ptr<SyncFence> SyncFence::exportFromCurrentContext(const EglContext& egl) {
    if (!isSupported(egl)) {
        return nullptr;
    }

    auto createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        egl.getProcAddress("eglCreateSyncKHR"));
    auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        egl.getProcAddress("eglDestroySyncKHR"));
    auto dupFenceFd = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
        egl.getProcAddress("eglDupNativeFenceFDANDROID"));

    if (!createSync || !destroySync || !dupFenceFd) {
        return nullptr;
    }

    EGLSyncKHR sync = createSync(egl.display(), EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        logError("SyncFence::exportFromCurrentContext: eglCreateSyncKHR failed");
        return nullptr;
    }

    // Flush the command buffer so the kernel fence object receives a valid seqno
    glFlush();

    int rawFd = dupFenceFd(egl.display(), sync);
    if (rawFd == EGL_NO_NATIVE_FENCE_FD_ANDROID || rawFd < 0) {
        destroySync(egl.display(), sync);
        return nullptr;
    }

    return std::make_unique<SyncFence>(&egl, sync, UniqueFd(rawFd));
}

std::unique_ptr<SyncFence> SyncFence::fromNativeFd(UniqueFd fd) noexcept {
    if (!fd.isValid()) return nullptr;
    return std::make_unique<SyncFence>(nullptr, EGL_NO_SYNC_KHR, std::move(fd));
}

bool SyncFence::waitOnGpu(const EglContext& egl, int fenceFd) noexcept {
    if (fenceFd < 0) return false;

    if (!isSupported(egl)) {
        // Fallback to brief CPU poll if GPU-side native fence import is unavailable
        struct pollfd pfd{};
        pfd.fd = fenceFd;
        pfd.events = POLLIN;
        return poll(&pfd, 1, 100) >= 0;
    }

    auto createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        egl.getProcAddress("eglCreateSyncKHR"));
    auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        egl.getProcAddress("eglDestroySyncKHR"));
    auto waitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(
        egl.getProcAddress("eglWaitSyncKHR"));

    if (!createSync || !destroySync || !waitSync) {
        return false;
    }

    int dupFd = dup(fenceFd);
    if (dupFd < 0) return false;

    const EGLint attribs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, dupFd,
        EGL_NONE
    };

    EGLSyncKHR sync = createSync(egl.display(), EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        close(dupFd);
        return false;
    }

    EGLint ok = waitSync(egl.display(), sync, 0);
    destroySync(egl.display(), sync);
    return ok == EGL_TRUE;
}

SyncFence::SyncFence(const EglContext* egl, EGLSyncKHR sync, UniqueFd fd) noexcept
    : m_egl(egl), m_sync(sync), m_fd(std::move(fd)) {}

SyncFence::~SyncFence() noexcept {
    if (m_sync != EGL_NO_SYNC_KHR && m_egl && m_egl->display() != EGL_NO_DISPLAY) {
        auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
            m_egl->getProcAddress("eglDestroySyncKHR"));
        if (destroySync) {
            destroySync(m_egl->display(), m_sync);
        }
        m_sync = EGL_NO_SYNC_KHR;
    }
}

SyncFence::SyncFence(SyncFence&& other) noexcept
    : m_egl(other.m_egl), m_sync(other.m_sync), m_fd(std::move(other.m_fd)) {
    other.m_egl = nullptr;
    other.m_sync = EGL_NO_SYNC_KHR;
}

SyncFence& SyncFence::operator=(SyncFence&& other) noexcept {
    if (this != &other) {
        if (m_sync != EGL_NO_SYNC_KHR && m_egl && m_egl->display() != EGL_NO_DISPLAY) {
            auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
                m_egl->getProcAddress("eglDestroySyncKHR"));
            if (destroySync) {
                destroySync(m_egl->display(), m_sync);
            }
        }
        m_egl = other.m_egl;
        m_sync = other.m_sync;
        m_fd = std::move(other.m_fd);
        other.m_egl = nullptr;
        other.m_sync = EGL_NO_SYNC_KHR;
    }
    return *this;
}

UniqueFd SyncFence::duplicateFd() const noexcept {
    if (!m_fd.isValid()) return UniqueFd{};
    int d = dup(m_fd.get());
    return UniqueFd(d);
}

bool SyncFence::clientWait(uint64_t timeoutNs) const noexcept {
    if (m_fd.isValid()) {
        struct pollfd pfd{};
        pfd.fd = m_fd.get();
        pfd.events = POLLIN;
        int timeoutMs = static_cast<int>(timeoutNs / 1'000'000ULL);
        int ret = poll(&pfd, 1, timeoutMs);
        return ret > 0 && (pfd.revents & POLLIN);
    }

    if (m_sync != EGL_NO_SYNC_KHR && m_egl) {
        auto clientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(
            m_egl->getProcAddress("eglClientWaitSyncKHR"));
        if (clientWaitSync) {
            EGLint res = clientWaitSync(
                m_egl->display(),
                m_sync,
                EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                static_cast<EGLTimeKHR>(timeoutNs));
            return res == EGL_CONDITION_SATISFIED_KHR;
        }
    }
    return false;
}

} // namespace nisaba::compositor

