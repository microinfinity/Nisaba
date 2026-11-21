# Nisaba Showcase and Demo Examples
# examples/meson.build

# Showcase example demo
showcase_exe = executable('showcase',
  'showcase.cpp',
  dependencies : nisaba_dep
)

showcase_text_exe = executable('showcase_text',
  'showcase_text.cpp',
  dependencies : nisaba_dep
)

showcase_text_adv_deps = [nisaba_dep]
showcase_text_adv_args = []
if get_option('enable_gpu')
  showcase_text_adv_deps += [extra_deps]
  showcase_text_adv_args += ['-DNISABA_HAS_GPU=1']
endif

showcase_text_advanced_exe = executable('showcase_text_advanced',
  'showcase_text_advanced.cpp',
  cpp_args : showcase_text_adv_args,
  dependencies : showcase_text_adv_deps
)

showcase_effects_exe = executable('showcase_effects',
  'showcase_effects.cpp',
  dependencies : nisaba_dep
)

showcase_effects_adv_deps = [nisaba_dep]
showcase_effects_adv_args = []
if get_option('enable_gpu')
  showcase_effects_adv_deps += [extra_deps]
  showcase_effects_adv_args += ['-DNISABA_HAS_GPU=1']
endif

showcase_effects_advanced_exe = executable('showcase_effects_advanced',
  'showcase_effects_advanced.cpp',
  cpp_args : showcase_effects_adv_args,
  dependencies : showcase_effects_adv_deps
)

showcase_svg_exe = executable('showcase_svg',
  'showcase_svg.cpp',
  dependencies : nisaba_dep
)

showcase_svg_cache_exe = executable('showcase_svg_cache',
  'showcase_svg_cache.cpp',
  dependencies : nisaba_dep
)

showcase_perspective_exe = executable('showcase_perspective',
  'showcase_perspective.cpp',
  dependencies : nisaba_dep
)

showcase_mesh_exe = executable('showcase_mesh',
  'showcase_mesh.cpp',
  dependencies : nisaba_dep
)

showcase_cpu_compare_exe = executable('showcase_cpu_compare',
  'showcase_cpu_compare.cpp',
  dependencies : nisaba_dep
)

image_codec_demo_exe = executable('image_codec_demo',
  'image_codec_demo.cpp',
  dependencies : nisaba_dep
)

if get_option('enable_gpu')
  showcase_gpu_exe = executable('showcase_gpu',
    'showcase_gpu.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )

  showcase_gpu_3d_physics_exe = executable('showcase_gpu_3d_physics',
    'showcase_gpu_3d_physics.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )

  showcase_gltf_model_exe = executable('showcase_gltf_model',
    'showcase_gltf_model.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )

  showcase_advanced_pbr_exe = executable('showcase_advanced_pbr',
    'showcase_advanced_pbr.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )

  if get_option('enable_backend_os')
    example_ecommerce_exe = executable('example_ecommerce',
      'example_ecommerce.cpp',
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_calculator_exe = executable('example_calculator',
      'example_calculator.cpp',
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    showcase_enki_stages_exe = executable('showcase_enki_stages',
      'showcase_enki_stages.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    showcase_gpu_blur_exe = executable('showcase_gpu_blur',
      'showcase_gpu_blur.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_damage_comparison_exe = executable('example_damage_comparison',
      'example_damage_comparison.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_damage_comparison_software_exe = executable('example_damage_comparison_software',
      'example_damage_comparison_software.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_damage_comparison_gpu_exe = executable('example_damage_comparison_gpu',
      'example_damage_comparison_gpu.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_instancing_comparison_gpu_exe = executable('example_instancing_comparison_gpu',
      'example_instancing_comparison_gpu.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_aot_comparison_gpu_exe = executable('example_aot_comparison_gpu',
      'example_aot_comparison_gpu.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_glass_benchmark_exe = executable('example_glass_benchmark',
      'example_glass_benchmark.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    example_embedded_hmi_exe = executable('example_embedded_hmi',
      'example_embedded_hmi.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    showcase_runtime_shaders_exe = executable('showcase_runtime_shaders',
      'showcase_runtime_shaders.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )

    showcase_3d_walkthrough_exe = executable('showcase_3d_walkthrough',
      'showcase_3d_walkthrough.cpp',
      cpp_args : ['-DNISABA_HAS_BACKEND_OS=1'],
      dependencies : [nisaba_dep, extra_deps, nisaba_backend_os_dep]
    )
  endif
endif



if get_option('enable_layout')
  layout_examples = [
    'example_layout_flex_direction',
    'example_layout_justify_content',
    'example_layout_align_items',
    'example_layout_flex_wrap_gap',
    'example_layout_flex_grow_shrink',
    'example_layout_box_model',
    'example_layout_complex_dashboard',
    'example_layout_absolute_stack',
  ]

  foreach ex : layout_examples
    executable(ex,
      ex + '.cpp',
      dependencies : nisaba_dep
    )
  endforeach
endif

if get_option('enable_animation')
  showcase_animation_exe = executable('showcase_animation',
    'showcase_animation.cpp',
    dependencies : nisaba_dep
  )
endif

if get_option('enable_lottie')
  showcase_lottie_deps = [nisaba_dep]
  showcase_lottie_args = []
  if get_option('enable_backend_os') and get_option('enable_gpu')
    showcase_lottie_deps += [extra_deps, nisaba_backend_os_dep]
    showcase_lottie_args += ['-DNISABA_HAS_BACKEND_OS=1']
  endif

  showcase_lottie_exe = executable('showcase_lottie',
    'showcase_lottie.cpp',
    cpp_args : showcase_lottie_args,
    dependencies : showcase_lottie_deps
  )

  if get_option('enable_gpu')
    showcase_lottie_gpu_deps = [nisaba_dep, extra_deps]
    showcase_lottie_gpu_args = []
    if get_option('enable_backend_os')
      showcase_lottie_gpu_deps += [nisaba_backend_os_dep]
      showcase_lottie_gpu_args += ['-DNISABA_HAS_BACKEND_OS=1']
    endif

    showcase_lottie_gpu_exe = executable('showcase_lottie_gpu',
      'showcase_lottie_gpu.cpp',
      cpp_args : showcase_lottie_gpu_args,
      dependencies : showcase_lottie_gpu_deps
    )
  endif
endif

if get_option('enable_markdown')
  showcase_md_deps = [nisaba_dep]
  showcase_md_args = []
  if get_option('enable_backend_os') and get_option('enable_gpu')
    showcase_md_deps += [extra_deps, nisaba_backend_os_dep]
    showcase_md_args += ['-DNISABA_HAS_BACKEND_OS=1']
  endif

  showcase_markdown_exe = executable('showcase_markdown',
    'showcase_markdown.cpp',
    cpp_args : showcase_md_args,
    dependencies : showcase_md_deps
  )

  showcase_markdown_parity_exe = executable('showcase_markdown_parity',
    'showcase_markdown_parity.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )
endif

if get_option('enable_pdf')
  showcase_pdf_deps = [nisaba_dep]
  showcase_pdf_args = []
  if get_option('enable_gpu')
    showcase_pdf_deps += [extra_deps]
    showcase_pdf_args += ['-DNISABA_HAS_GPU=1']
  endif
  if get_option('enable_backend_os') and get_option('enable_gpu')
    showcase_pdf_deps += [nisaba_backend_os_dep]
    showcase_pdf_args += ['-DNISABA_HAS_BACKEND_OS=1']
  endif

  showcase_pdf_exe = executable('showcase_pdf',
    'showcase_pdf.cpp',
    cpp_args : showcase_pdf_args,
    dependencies : showcase_pdf_deps
  )
endif

if get_option('enable_compositor')
  example_compositor_fusion_exe = executable('example_compositor_fusion',
    'example_compositor_fusion.cpp',
    dependencies : nisaba_dep
  )
endif
