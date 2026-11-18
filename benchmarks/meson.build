benchmark_image_decoders_exe = executable('benchmark_image_decoders',
  'benchmark_image_decoders.cpp',
  dependencies : nisaba_dep
)

benchmark_text_engines_exe = executable('benchmark_text_engines',
  'benchmark_text_engines.cpp',
  dependencies : nisaba_dep
)

benchmark_damage_tracking_exe = executable('benchmark_damage_tracking',
  'benchmark_damage_tracking.cpp',
  dependencies : nisaba_dep
)

benchmark_render3d_pbr_exe = executable('benchmark_render3d_pbr',
  'benchmark_render3d_pbr.cpp',
  dependencies : [nisaba_dep, extra_deps]
)


cairo_dep = dependency('cairo', required : false)
if cairo_dep.found()
  cpp_comp = meson.get_compiler('cpp')
  blend2d_lib = cpp_comp.find_library('blend2d', dirs : [meson.project_source_root() / 'third_party/build_blend2d'], required : false)
  threads_dep = dependency('threads', required : false)
  rt_dep = cpp_comp.find_library('rt', required : false)

  profiler_deps = [nisaba_dep, cairo_dep]
  profiler_c_args = []
  profiler_inc = []


  skia_lib = cpp_comp.find_library('skia', dirs : [meson.project_source_root() / 'third_party/skia/out/Release-x64'], required : false)
  fontconfig_dep = dependency('fontconfig', required : false)
  gl_dep = dependency('gl', required : false)
  zlib_dep = dependency('zlib', required : false)

  skia_inc = [
    include_directories('../third_party/skia/include/core', is_system : true),
    include_directories('../third_party/skia/include/config', is_system : true),
    include_directories('../third_party/skia/include/effects', is_system : true),
    include_directories('../third_party/skia', is_system : true),
  ]

  if skia_lib.found()
    profiler_deps += [skia_lib, fontconfig_dep, gl_dep, zlib_dep]
    profiler_c_args += ['-DHAVE_SKIA=1']
    profiler_inc += skia_inc
  endif

  micro_profiler_exe = executable('micro_profiler',
    'micro_profiler.cpp',
    dependencies : profiler_deps,
    cpp_args : profiler_c_args,
    include_directories : profiler_inc
  )

  nisaba_vs_cairo_exe = executable('nisaba_vs_cairo',
    'nisaba_vs_cairo.cpp',
    dependencies : [nisaba_dep, cairo_dep]
  )

  if skia_lib.found()
    nisaba_vs_skia_exe = executable('nisaba_vs_skia',
      'nisaba_vs_skia.cpp',
      dependencies : [nisaba_dep, skia_lib, fontconfig_dep, gl_dep, zlib_dep],
      include_directories : skia_inc
    )
  endif
  if skia_lib.found()
    skia_dir = meson.project_source_root() / 'third_party/skia/out/Release-x64'
    skottie_lib = cpp_comp.find_library('skottie', dirs : [skia_dir], required : false)
    sksg_lib = cpp_comp.find_library('sksg', dirs : [skia_dir], required : false)
    skresources_lib = cpp_comp.find_library('skresources', dirs : [skia_dir], required : false)
    skshaper_lib = cpp_comp.find_library('skshaper', dirs : [skia_dir], required : false)
    skunicode_lib = cpp_comp.find_library('skunicode', dirs : [skia_dir], required : false)
    sk_harfbuzz_lib = cpp_comp.find_library('harfbuzz', dirs : [skia_dir], required : false)
    sk_icu_lib = cpp_comp.find_library('icu', dirs : [skia_dir], required : false)
    gpu_prof_deps = [nisaba_dep]
    gpu_prof_cpp_args = []
    if skottie_lib.found() and sksg_lib.found() and skresources_lib.found() and skshaper_lib.found() and skunicode_lib.found()
      gpu_prof_deps += [skottie_lib, sksg_lib, skresources_lib, skshaper_lib, skunicode_lib]
      if sk_harfbuzz_lib.found()
        gpu_prof_deps += [sk_harfbuzz_lib]
      endif
      if sk_icu_lib.found()
        gpu_prof_deps += [sk_icu_lib]
      endif
      gpu_prof_cpp_args += ['-DHAVE_SKOTTIE=1']
    endif
    gpu_prof_deps += [skia_lib, fontconfig_dep, gl_dep, zlib_dep, nisaba_backend_os_dep, egl_dep, dl_dep]
    gpu_micro_profiler_exe = executable('gpu_micro_profiler',
      'gpu_micro_profiler.cpp',
      dependencies : gpu_prof_deps,
      cpp_args : gpu_prof_cpp_args,
      include_directories : skia_inc
    )
    benchmark_runtime_shaders_exe = executable('benchmark_runtime_shaders',
      'benchmark_runtime_shaders.cpp',
      dependencies : [nisaba_dep, skia_lib, fontconfig_dep, gl_dep, zlib_dep, egl_dep, dl_dep],
      include_directories : skia_inc
    )
  endif
endif
