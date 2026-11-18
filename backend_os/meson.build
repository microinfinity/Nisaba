# Nisaba Native OS Backend Subsystem (backend_os)

add_languages('c', required : true, native : false)
cpp = meson.get_compiler('cpp')

backend_os_inc = include_directories('include')

is_windows = host_machine.system() == 'windows'
is_android = host_machine.system() == 'android'
is_wasm    = host_machine.system() == 'emscripten'

backend_os_deps = []
backend_os_args = []
backend_os_sources = files(
  'src/platform_native.cpp',
  'src/window_native.cpp',
  'src/input.cpp',
  'src/permissions.cpp',
  'src/app.cpp',
  'src/native_popup.cpp',
)

if is_android
  android_dep = cpp.find_library('android', required : true)
  log_dep     = cpp.find_library('log',     required : true)
  egl_dep     = cpp.find_library('EGL',     required : true)
  gles3_dep   = cpp.find_library('GLESv3',  required : true)
  thread_dep  = dependency('threads')

  backend_os_deps += [android_dep, log_dep, egl_dep, gles3_dep, thread_dep]
  backend_os_args += ['-DNISABA_BACKEND_OS_HAS_ANDROID=1', '-DNISABA_BACKEND_OS_PLATFORM_ANDROID=1']
  backend_os_sources += files(
    'src/android/android_platform.cpp',
    'src/android/android_app_glue.cpp',
    'src/android/android_surface.cpp',
  )

elif is_windows
  thread_dep = dependency('threads')
  opengl_dep = cpp.find_library('opengl32', required : true)
  gdi_dep    = cpp.find_library('gdi32', required : true)
  user32_dep = cpp.find_library('user32', required : true)
  dwmapi_dep = cpp.find_library('dwmapi', required : true)
  uxtheme_dep = cpp.find_library('uxtheme', required : true)
  ole32_dep  = cpp.find_library('ole32', required : true)
  shell32_dep = cpp.find_library('shell32', required : true)
  shcore_dep = cpp.find_library('shcore', required : false)

  backend_os_deps += [thread_dep, opengl_dep, gdi_dep, user32_dep, dwmapi_dep, uxtheme_dep, ole32_dep, shell32_dep]
  if shcore_dep.found()
    backend_os_deps += shcore_dep
  endif

  backend_os_sources += files(
    'src/windows/win32_platform.cpp',
    'src/windows/win32_window.cpp',
  )

elif is_wasm
  backend_os_args += '-DNISABA_BACKEND_OS_HAS_WASM=1'
  backend_os_sources += files(
    'src/wasm/wasm_platform.cpp',
    'src/wasm/wasm_window.cpp',
    'src/wasm/setjmp_compat.c',
  )

else
  # Native Linux Platform (X11 / Wayland / DRM / EGL / Linux Native)
  x11_dep    = dependency('x11',     required : false)
  xext_dep   = dependency('xext',    required : false)
  xrandr_dep = dependency('xrandr',  required : false)
  egl_dep    = dependency('egl',     required : true)
  gl_dep     = dependency('gl',      required : true)
  thread_dep = dependency('threads')
  dl_dep     = cpp.find_library('dl', required : false)
  m_dep      = cpp.find_library('m',  required : false)

  backend_os_deps += [egl_dep, gl_dep, thread_dep, dl_dep, m_dep]
  backend_os_sources += files('src/layer_surface.cpp')

  if x11_dep.found()
    backend_os_args += '-DNISABA_BACKEND_OS_HAS_X11=1'
    backend_os_deps += x11_dep
    backend_os_sources += files(
      'src/x11/x11_platform.cpp',
      'src/x11/x11_window.cpp',
      'src/x11/x11_layer_surface.cpp',
    )
    if xext_dep.found()
      backend_os_deps += xext_dep
      backend_os_args += '-DNISABA_BACKEND_OS_HAS_XSHAPE=1'
    endif
  endif

  if xrandr_dep.found()
    backend_os_deps += xrandr_dep
    backend_os_args += '-DNISABA_BACKEND_OS_HAS_XRANDR=1'
  endif

  # Wayland
  wayland_client_dep = dependency('wayland-client', required : false)
  wayland_egl_dep    = dependency('wayland-egl',    required : false)
  wayland_cursor_dep = dependency('wayland-cursor', required : false)
  xkbcommon_dep      = dependency('xkbcommon',      required : false)
  wayland_scanner    = find_program('wayland-scanner', required : false)

  if wayland_client_dep.found() and wayland_scanner.found()
    backend_os_args += '-DNISABA_BACKEND_OS_HAS_WAYLAND=1'
    backend_os_deps += [wayland_client_dep, wayland_egl_dep, wayland_cursor_dep, xkbcommon_dep]

    proto_c_sources = []
    proto_h_sources = []

    protocols = [
      'wlr-layer-shell-unstable-v1',
      'xdg-shell',
      'wlr-foreign-toplevel-management-unstable-v1',
      'xdg-output-unstable-v1',
      'xdg-decoration-unstable-v1'
    ]

    foreach proto : protocols
      xml = 'protocols' / proto + '.xml'

      hdr = custom_target(proto + '-client-header',
        input  : files(xml),
        output : proto + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
      )
      src = custom_target(proto + '-client-code',
        input  : files(xml),
        output : proto + '-protocol.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
      )

      proto_h_sources += hdr
      proto_c_sources += src
    endforeach

    wayland_proto_lib = static_library('nisaba_wayland_protocols',
      proto_c_sources,
      dependencies : [wayland_client_dep],
      override_options : ['warning_level=0'],
    )

    backend_os_proto_dep = declare_dependency(
      link_with : wayland_proto_lib,
      sources   : proto_h_sources,
    )
    backend_os_deps += backend_os_proto_dep

    backend_os_sources += files(
      'src/wayland/wayland_platform.cpp',
      'src/wayland/wayland_window.cpp',
      'src/wayland/wayland_surface.cpp',
    )
  endif

  # DRM/KMS (optional)
  libdrm_dep   = dependency('libdrm',   required : false)
  gbm_dep      = dependency('gbm',      required : false)
  libinput_dep = dependency('libinput', required : false)
  libudev_dep  = dependency('libudev',  required : false)

  if libdrm_dep.found() and gbm_dep.found() and libinput_dep.found() and libudev_dep.found()
    backend_os_args += '-DNISABA_BACKEND_OS_HAS_DRM=1'
    backend_os_deps += [libdrm_dep, gbm_dep, libinput_dep, libudev_dep]
    backend_os_sources += files(
      'src/drm/drm_platform.cpp',
      'src/drm/drm_window.cpp',
      'src/drm/drm_input.cpp',
    )
  endif
endif

nisaba_backend_os_lib = both_libraries('nisaba_backend_os',
  backend_os_sources,
  include_directories : backend_os_inc,
  dependencies : backend_os_deps,
  cpp_args : backend_os_args,
  install : true,
)

nisaba_backend_os_dep = declare_dependency(
  include_directories : backend_os_inc,
  link_with : nisaba_backend_os_lib,
  dependencies : backend_os_deps,
  compile_args : backend_os_args,
)
