#pragma once

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
  #include <GLES3/gl3.h>
  #ifndef NISABA_TARGET_GLES
    #define NISABA_TARGET_GLES 1
  #endif
#else
  #if defined(NISABA_GLEW)
    #include <GL/glew.h>
  #endif
  #if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <windows.h>
    #include <GL/gl.h>
  #else
    #include <GL/gl.h>
  #endif
#endif

#include <cstring>

namespace nisaba::gpu::detail {

inline void ensure_gl_functions_loaded() noexcept {
#if defined(NISABA_GLEW) && !defined(NISABA_TARGET_GLES)
    static bool s_glew_inited = false;
    if (!s_glew_inited) {
        glewExperimental = GL_TRUE;
        (void)glewInit();
        while (glGetError() != GL_NO_ERROR) {}
        s_glew_inited = true;
    }
#endif
}

inline bool is_gles_or_webgl() noexcept {
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (ver != nullptr) {
        if (std::strstr(ver, "OpenGL ES") != nullptr || std::strstr(ver, "WebGL") != nullptr) {
            return true;
        }
    }
#if defined(NISABA_TARGET_GLES)
    return true;
#else
    return false;
#endif
}

} // namespace nisaba::gpu::detail
