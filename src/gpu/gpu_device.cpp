#include "nisaba/gpu/gpu_device.hpp"
#include "nisaba/gpu/gl3_renderer.hpp"
#include "nisaba/gpu/context.hpp"
#include "nisaba/gpu/vulkan_device.hpp"
#include "gl_headers.hpp"
#include <iostream>
#include <cstring>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <dlfcn.h>

#if defined(__has_include)
  #if __has_include(<EGL/egl.h>)
    #include <EGL/egl.h>
    #include <EGL/eglext.h>
    #define NISABA_HAS_EGL 1
  #endif
#endif
#endif

namespace nisaba::gpu {

namespace {
#if defined(NISABA_HAS_EGL)
void* open_egl_library() noexcept {
#if defined(__ANDROID__)
    static void* s_lib_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
#else
    static void* s_lib_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
#endif
    return s_lib_egl;
}
#endif
} // namespace

GpuDevice::GpuDevice() = default;

void GpuDevice::init_glew() {
    detail::ensure_gl_functions_loaded();

    const char* rend = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (rend) renderer_name_ = rend;
    const char* vers = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (vers) version_name_ = vers;
    is_valid_ = true;

    // Check MSAA support
#if defined(NISABA_TARGET_GLES)
    has_msaa_ = true;
#else
    has_msaa_ = (glRenderbufferStorageMultisample != nullptr && glBlitFramebuffer != nullptr);
#endif
}

GpuDevice::~GpuDevice() {
#if defined(NISABA_HAS_EGL)
    if (egl_display_ && egl_context_) {
        void* lib_egl = open_egl_library();
        if (lib_egl) {
            using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
            using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
            using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
            auto eglMakeCurrent_fn = reinterpret_cast<PFN_eglMakeCurrent>(dlsym(lib_egl, "eglMakeCurrent"));
            auto eglDestroySurface_fn = reinterpret_cast<PFN_eglDestroySurface>(dlsym(lib_egl, "eglDestroySurface"));
            auto eglDestroyContext_fn = reinterpret_cast<PFN_eglDestroyContext>(dlsym(lib_egl, "eglDestroyContext"));
            if (eglMakeCurrent_fn) {
                eglMakeCurrent_fn(static_cast<EGLDisplay>(egl_display_),
                                  (EGLSurface)0, (EGLSurface)0, (EGLContext)0);
            }
            if (eglDestroySurface_fn && egl_surface_) {
                eglDestroySurface_fn(static_cast<EGLDisplay>(egl_display_),
                                     static_cast<EGLSurface>(egl_surface_));
            }
            if (eglDestroyContext_fn && egl_context_) {
                eglDestroyContext_fn(static_cast<EGLDisplay>(egl_display_),
                                     static_cast<EGLContext>(egl_context_));
            }
        }
    }
#endif
}

void GpuDevice::make_current() {
#if defined(NISABA_HAS_EGL)
    if (egl_display_ && egl_context_) {
        void* lib_egl = open_egl_library();
        if (lib_egl) {
            using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
            auto eglMakeCurrent_fn = reinterpret_cast<PFN_eglMakeCurrent>(dlsym(lib_egl, "eglMakeCurrent"));
            if (eglMakeCurrent_fn) {
                eglMakeCurrent_fn(static_cast<EGLDisplay>(egl_display_),
                                  static_cast<EGLSurface>(egl_surface_),
                                  static_cast<EGLSurface>(egl_surface_),
                                  static_cast<EGLContext>(egl_context_));
            }
        }
    }
#endif
}

std::unique_ptr<Context> GpuDevice::create_context(int flags) {
    if (flags == 0) {
        flags = CreateFlags::Antialias | CreateFlags::StencilStrokes;
    }
    return createContextGL3(flags);
}

std::shared_ptr<GpuDevice> GpuDevice::create(GpuBackendType type) {
    if (type == GpuBackendType::Auto) {
        const char* env_backend = std::getenv("NISABA_GPU_BACKEND");
        if (!env_backend) env_backend = std::getenv("NISABA_BACKEND");
        if (env_backend) {
            std::string_view s(env_backend);
            if (s == "vulkan" || s == "Vulkan" || s == "VULKAN" || s == "vk" || s == "VK") {
                type = GpuBackendType::Vulkan;
            } else if (s == "opengl" || s == "OpenGL" || s == "OPENGL" || s == "gl" || s == "GL") {
                type = GpuBackendType::OpenGL;
            }
        }
    }
    if (type == GpuBackendType::Vulkan) {
        return VulkanDevice::create();
    }
    // 1. If an OpenGL context is ALREADY active on this thread, adopt it
    const char* rend = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (rend != nullptr && rend[0] != '\0' && (type == GpuBackendType::Auto || type == GpuBackendType::OpenGL)) {
        struct EnableMakeShared : public GpuDevice {};
        auto dev = std::make_shared<EnableMakeShared>();
        dev->backend_type_ = GpuBackendType::OpenGL;
        dev->init_glew();
        return dev;
    }
    // 2. Otherwise create a dedicated headless hardware context
    return create_headless(800, 600, type);
}

std::shared_ptr<GpuDevice> GpuDevice::create_headless(uint32_t width, uint32_t height, GpuBackendType type) {
    if (type == GpuBackendType::Auto) {
        const char* env_backend = std::getenv("NISABA_GPU_BACKEND");
        if (!env_backend) env_backend = std::getenv("NISABA_BACKEND");
        if (env_backend) {
            std::string_view s(env_backend);
            if (s == "vulkan" || s == "Vulkan" || s == "VULKAN" || s == "vk" || s == "VK") {
                type = GpuBackendType::Vulkan;
            } else if (s == "opengl" || s == "OpenGL" || s == "OPENGL" || s == "gl" || s == "GL") {
                type = GpuBackendType::OpenGL;
            }
        }
    }
    if (type == GpuBackendType::Vulkan) {
        return VulkanDevice::create_headless(width, height);
    }
    struct EnableMakeShared : public GpuDevice {};
    auto dev = std::make_shared<EnableMakeShared>();
    dev->backend_type_ = GpuBackendType::OpenGL;

#if defined(NISABA_HAS_EGL)
    void* lib_egl = open_egl_library();
#if !defined(__ANDROID__)
    static void* lib_gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    (void)lib_gl;
#endif

    if (lib_egl) {
        using PFN_eglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType);
        using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
        using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
        using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
        using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
        using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*);
        using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);

        auto eglGetDisplay_fn = reinterpret_cast<PFN_eglGetDisplay>(dlsym(lib_egl, "eglGetDisplay"));
        auto eglInitialize_fn = reinterpret_cast<PFN_eglInitialize>(dlsym(lib_egl, "eglInitialize"));
        auto eglBindAPI_fn = reinterpret_cast<PFN_eglBindAPI>(dlsym(lib_egl, "eglBindAPI"));
        auto eglChooseConfig_fn = reinterpret_cast<PFN_eglChooseConfig>(dlsym(lib_egl, "eglChooseConfig"));
        auto eglCreateContext_fn = reinterpret_cast<PFN_eglCreateContext>(dlsym(lib_egl, "eglCreateContext"));
        auto eglCreatePbufferSurface_fn = reinterpret_cast<PFN_eglCreatePbufferSurface>(dlsym(lib_egl, "eglCreatePbufferSurface"));
        auto eglMakeCurrent_fn = reinterpret_cast<PFN_eglMakeCurrent>(dlsym(lib_egl, "eglMakeCurrent"));

        if (eglGetDisplay_fn && eglInitialize_fn && eglBindAPI_fn && eglChooseConfig_fn &&
            eglCreateContext_fn && eglCreatePbufferSurface_fn && eglMakeCurrent_fn) {

            EGLDisplay dpy = eglGetDisplay_fn((EGLNativeDisplayType)0);
            if (dpy != (EGLDisplay)0) {
                EGLint major = 0, minor = 0;
                if (eglInitialize_fn(dpy, &major, &minor)) {
#if defined(__ANDROID__)
                    constexpr EGLenum kEglApi = 0x30A0; // EGL_OPENGL_ES_API
                    constexpr EGLint  kRenderableBit = 0x0040; // EGL_OPENGL_ES3_BIT
#else
                    constexpr EGLenum kEglApi = 0x30A2; // EGL_OPENGL_API
                    constexpr EGLint  kRenderableBit = 0x0008; // EGL_OPENGL_BIT
#endif
                    eglBindAPI_fn(kEglApi);

                    const EGLint config_attribs[] = {
                        0x3040, kRenderableBit,
                        0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,
                        0x3026, 8, // Stencil 8
                        0x3033, 0x0001 /* EGL_PBUFFER_BIT */,
                        0x3038
                    };

                    EGLConfig config = nullptr;
                    EGLint num_configs = 0;
                    bool has_pbuffer_cfg = eglChooseConfig_fn(dpy, config_attribs, &config, 1, &num_configs) && num_configs > 0;
                    if (!has_pbuffer_cfg) {
                        const EGLint surfaceless_attribs[] = {
                            0x3040, kRenderableBit,
                            0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,
                            0x3026, 8,
                            0x3038
                        };
                        eglChooseConfig_fn(dpy, surfaceless_attribs, &config, 1, &num_configs);
                    }
                    if (num_configs > 0) {
                        const EGLint ctx_attribs[] = {
                            0x3098, 3, // EGL_CONTEXT_MAJOR_VERSION = 3
                            0x3038
                        };

                        EGLContext ctx = eglCreateContext_fn(dpy, config, (EGLContext)0, ctx_attribs);
                        if (ctx != (EGLContext)0) {
                            EGLSurface pbuf = (EGLSurface)0;
                            if (has_pbuffer_cfg) {
                                const EGLint pbuf_attribs[] = {
                                    0x3057, static_cast<EGLint>(width),
                                    0x3056, static_cast<EGLint>(height),
                                    0x3038
                                };
                                pbuf = eglCreatePbufferSurface_fn(dpy, config, pbuf_attribs);
                            }
                            if (eglMakeCurrent_fn(dpy, pbuf, pbuf, ctx)) {
                                dev->egl_display_ = dpy;
                                dev->egl_context_ = ctx;
                                dev->egl_surface_ = pbuf;
                                dev->init_glew();
                                return dev;
                            }
                        }
                    }
                }
            }
        }
    }
#endif

    // Fallback: adopt thread context if available
    dev->init_glew();
    return dev;
}

void GpuDevice::set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void GpuDevice::set_scissor(const std::optional<ScreenIntRect>& scissor) {
    if (scissor.has_value()) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(scissor->x()), static_cast<GLint>(scissor->y()),
                  static_cast<GLsizei>(scissor->width()), static_cast<GLsizei>(scissor->height()));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void GpuDevice::set_blend_mode(BlendMode blend_mode) {
    glEnable(GL_BLEND);
    switch (blend_mode) {
        case BlendMode::SourceOver:
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        case BlendMode::Plus:
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        case BlendMode::Multiply:
            glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        case BlendMode::Screen:
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        case BlendMode::Clear:
            glBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        case BlendMode::Source:
            glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        case BlendMode::Destination:
            glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
        default:
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            break;
    }
}

void GpuDevice::clear(nisaba::Color color) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xffffffff);
    glClearColor(color.red(), color.green(), color.blue(), color.alpha());
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

uint32_t GpuDevice::create_buffer() {
    uint32_t id = 0;
    glGenBuffers(1, &id);
    return id;
}

void GpuDevice::delete_buffer(uint32_t buffer_id) {
    if (buffer_id != 0) glDeleteBuffers(1, &buffer_id);
}

void GpuDevice::bind_vertex_buffer(uint32_t buffer_id) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer_id);
}

void GpuDevice::bind_index_buffer(uint32_t buffer_id) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer_id);
}

void GpuDevice::upload_buffer_data(uint32_t target, const void* data, size_t size, bool dynamic) {
    glBufferData(target, static_cast<GLsizeiptr>(size), data, dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
}

void GpuDevice::upload_buffer_subdata(uint32_t target, size_t offset, const void* data, size_t size) {
    glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
}

uint32_t GpuDevice::create_vao() {
    uint32_t id = 0;
    glGenVertexArrays(1, &id);
    return id;
}

void GpuDevice::delete_vao(uint32_t vao_id) {
    if (vao_id != 0) glDeleteVertexArrays(1, &vao_id);
}

void GpuDevice::bind_vao(uint32_t vao_id) {
    glBindVertexArray(vao_id);
}

void GpuDevice::enable_vertex_attrib_array(uint32_t index) {
    glEnableVertexAttribArray(index);
}

void GpuDevice::vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type, bool normalized, int32_t stride, const void* pointer) {
    glVertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE, stride, pointer);
}

uint32_t GpuDevice::create_texture() {
    uint32_t id = 0;
    glGenTextures(1, &id);
    return id;
}

void GpuDevice::delete_texture(uint32_t texture_id) {
    if (texture_id != 0) glDeleteTextures(1, &texture_id);
}

void GpuDevice::bind_texture(uint32_t unit, uint32_t texture_id) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture_id);
}

void GpuDevice::upload_texture_image_2d(
    uint32_t width, uint32_t height,
    const void* pixels,
    GpuTextureFormat format,
    GpuTextureFilter filter,
    GpuTextureWrap wrap
) {
    GLenum gl_fmt = (format == GpuTextureFormat::Alpha8) ? GL_RED : GL_RGBA;
    GLint gl_int_fmt = (format == GpuTextureFormat::Alpha8) ? GL_R8 : GL_RGBA8;

    glTexImage2D(GL_TEXTURE_2D, 0, gl_int_fmt, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, gl_fmt, GL_UNSIGNED_BYTE, pixels);

    GLint min_f = (filter == GpuTextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR;
    GLint mag_f = (filter == GpuTextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_f);

    GLint wrap_mode = GL_CLAMP_TO_EDGE;
    if (wrap == GpuTextureWrap::Repeat) wrap_mode = GL_REPEAT;
    else if (wrap == GpuTextureWrap::MirroredRepeat) wrap_mode = GL_MIRRORED_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode);
}

void GpuDevice::update_texture_sub_image_2d(
    uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    const void* pixels,
    GpuTextureFormat format
) {
    GLenum gl_fmt = (format == GpuTextureFormat::Alpha8) ? GL_RED : GL_RGBA;
    glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(y),
                    static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                    gl_fmt, GL_UNSIGNED_BYTE, pixels);
}

uint32_t GpuDevice::create_framebuffer() {
    uint32_t id = 0;
    glGenFramebuffers(1, &id);
    return id;
}

void GpuDevice::delete_framebuffer(uint32_t fbo_id) {
    if (fbo_id != 0) glDeleteFramebuffers(1, &fbo_id);
}

void GpuDevice::bind_framebuffer(uint32_t fbo_id) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
}

bool GpuDevice::check_framebuffer_complete() {
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void GpuDevice::attach_texture_to_framebuffer(uint32_t texture_id) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);
}

uint32_t GpuDevice::create_renderbuffer() {
    uint32_t id = 0;
    glGenRenderbuffers(1, &id);
    return id;
}

void GpuDevice::delete_renderbuffer(uint32_t rb_id) {
    if (rb_id != 0) glDeleteRenderbuffers(1, &rb_id);
}

void GpuDevice::bind_renderbuffer(uint32_t rb_id) {
    glBindRenderbuffer(GL_RENDERBUFFER, rb_id);
}

void GpuDevice::renderbuffer_storage_multisample(uint32_t samples, uint32_t format, uint32_t width, uint32_t height) {
#if defined(NISABA_TARGET_GLES)
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, static_cast<GLsizei>(samples),
                                     format, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
#else
    if (glRenderbufferStorageMultisample) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, static_cast<GLsizei>(samples),
                                         format, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, format, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    }
#endif
}

void GpuDevice::attach_renderbuffer_to_framebuffer(uint32_t attachment, uint32_t rb_id) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb_id);
}

void GpuDevice::blit_framebuffer(uint32_t src_fbo, uint32_t dst_fbo, uint32_t width, uint32_t height) {
#if !defined(NISABA_TARGET_GLES)
    if (!glBlitFramebuffer) return;
#endif
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glBlitFramebuffer(0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                      0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
}

void GpuDevice::blit_framebuffer(uint32_t src_fbo, uint32_t dst_fbo,
                                 int32_t src_x0, int32_t src_y0, int32_t src_x1, int32_t src_y1,
                                 int32_t dst_x0, int32_t dst_y0, int32_t dst_x1, int32_t dst_y1) {
#if !defined(NISABA_TARGET_GLES)
    if (!glBlitFramebuffer) return;
#endif
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1,
                      dst_x0, dst_y0, dst_x1, dst_y1,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
}

void GpuDevice::read_pixels(int32_t x, int32_t y, uint32_t width, uint32_t height, void* out_rgba) {
    glReadPixels(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
}

} // namespace nisaba::gpu
