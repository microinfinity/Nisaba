# Nisaba Automated Unit Tests
# tests/meson.build

# Math unit tests
test_math_exe = executable('test_math',
  'test_math.cpp',
  dependencies : nisaba_dep
)
test('math_test', test_math_exe)

# Path unit tests
test_path_exe = executable('test_path',
  'test_path.cpp',
  dependencies : nisaba_dep
)
test('path_test', test_path_exe)

# PathMeasure arc-length & parameterization unit tests
test_path_measure_exe = executable('test_path_measure',
  'test_path_measure.cpp',
  dependencies : nisaba_dep
)
test('path_measure_test', test_path_measure_exe)

# PathOps boolean operations unit tests
test_path_ops_exe = executable('test_path_ops',
  'test_path_ops.cpp',
  dependencies : nisaba_dep
)
test('path_ops_test', test_path_ops_exe)

# Stroke & Dash unit tests
test_stroke_exe = executable('test_stroke',
  'test_stroke.cpp',
  dependencies : nisaba_dep
)
test('stroke_test', test_stroke_exe)

# Stroke to Fill (Path Outlining) unit tests
test_stroke_to_fill_exe = executable('test_stroke_to_fill',
  'test_stroke_to_fill.cpp',
  dependencies : nisaba_dep
)
test('stroke_to_fill_test', test_stroke_to_fill_exe)

# Color & Fixed-point unit tests
test_color_exe = executable('test_color',
  'test_color.cpp',
  dependencies : nisaba_dep
)
test('color_test', test_color_exe)

# Pixmap unit tests
test_pixmap_exe = executable('test_pixmap',
  'test_pixmap.cpp',
  dependencies : nisaba_dep
)
test('pixmap_test', test_pixmap_exe)

# Diverse Pixel Formats (RGBA8888, BGRA8888, RGB565, Alpha8) unit tests
test_pixmap_formats_exe = executable('test_pixmap_formats',
  'test_pixmap_formats.cpp',
  dependencies : nisaba_dep
)
test('pixmap_formats_test', test_pixmap_formats_exe)

# Scanline rasterizer unit tests
test_raster_exe = executable('test_raster',
  'test_raster.cpp',
  dependencies : nisaba_dep
)
test('raster_test', test_raster_exe)

# Canvas, Shaders & Pipeline unit tests
test_canvas_exe = executable('test_canvas',
  'test_canvas.cpp',
  dependencies : nisaba_dep
)
test('canvas_test', test_canvas_exe)

# Advanced features tests (Zero-copy stride, SIMD accuracy, Clip path, Text/Glyphs)
test_advanced_exe = executable('test_advanced',
  'test_advanced.cpp',
  dependencies : nisaba_dep
)
test('advanced_test', test_advanced_exe)

# Sovereign Text Engine tests
test_text_exe = executable('test_text',
  'test_text.cpp',
  dependencies : nisaba_dep
)
test('text_test', test_text_exe)

# Sovereign Linux System Font Discovery & Fontconfig Integration tests
test_linux_font_system_exe = executable('test_linux_font_system',
  'test_linux_font_system.cpp',
  dependencies : nisaba_dep
)
test('linux_font_system_test', test_linux_font_system_exe)

# PostScript / CFF & Advanced OpenType (GSUB/GPOS) tests
test_cff_opentype_exe = executable('test_cff_opentype',
  'test_cff_opentype.cpp',
  dependencies : nisaba_dep
)
test('cff_opentype_test', test_cff_opentype_exe)

# Linear sRGB & Gamma-Correct Blending tests
test_linear_blending_exe = executable('test_linear_blending',
  'test_linear_blending.cpp',
  dependencies : nisaba_dep
)
test('linear_blending_test', test_linear_blending_exe)

# Advanced Shaders (Compose/Blend, TwoPointConicalGradient) & ColorMatrix Filters tests
test_shaders_advanced_exe = executable('test_shaders_advanced',
  'test_shaders_advanced.cpp',
  dependencies : nisaba_dep
)
test('shaders_advanced_test', test_shaders_advanced_exe)

# Visual Effects, Blur & Shadows tests
test_effects_exe = executable('test_effects',
  'test_effects.cpp',
  dependencies : nisaba_dep
)
test('effects_test', test_effects_exe)

# Sovereign SVG Renderer tests
test_svg_exe = executable('test_svg',
  'test_svg.cpp',
  dependencies : nisaba_dep
)
test('svg_test', test_svg_exe)

# SVG Completeness tests (Gradients, ClipPath, Mask, Defs, Use, Image)
test_svg_completeness_exe = executable('test_svg_completeness',
  'test_svg_completeness.cpp',
  dependencies : nisaba_dep
)
test('svg_completeness_test', test_svg_completeness_exe)

# 3D Perspective & Spatial Transform tests
test_perspective_exe = executable('test_perspective',
  'test_perspective.cpp',
  dependencies : nisaba_dep
)
test('perspective_test', test_perspective_exe)

# 2D Vertex Mesh & Freeform Gradient Mesh tests
test_mesh_exe = executable('test_mesh',
  'test_mesh.cpp',
  dependencies : nisaba_dep
)
test('mesh_test', test_mesh_exe)

# Sovereign Deflate/Inflate & Checksum tests
test_deflate_exe = executable('test_deflate',
  'test_deflate.cpp',
  dependencies : nisaba_dep
)
test('deflate_test', test_deflate_exe)

# Sovereign PNG Codec tests
test_png_exe = executable('test_png',
  'test_png.cpp',
  dependencies : nisaba_dep
)
test('png_test', test_png_exe)

# Sovereign JPEG Codec tests
test_jpeg_exe = executable('test_jpeg',
  'test_jpeg.cpp',
  dependencies : nisaba_dep
)
test('jpeg_test', test_jpeg_exe)

# Sovereign Image I/O & Auto-detection tests
test_image_io_exe = executable('test_image_io',
  'test_image_io.cpp',
  dependencies : nisaba_dep
)
test('image_io_test', test_image_io_exe)

# Sovereign QOI Codec tests
test_qoi_exe = executable('test_qoi',
  'test_qoi.cpp',
  dependencies : nisaba_dep
)
test('qoi_test', test_qoi_exe)

# Sovereign WebP Codec tests
test_webp_exe = executable('test_webp',
  'test_webp.cpp',
  dependencies : nisaba_dep
)
test('webp_test', test_webp_exe)

# Sovereign Extended Codecs (BMP, PPM, HDR) tests
test_codecs_extended_exe = executable('test_codecs_extended',
  'test_codecs_extended.cpp',
  dependencies : nisaba_dep
)
test('codecs_extended_test', test_codecs_extended_exe)

if get_option('enable_gpu')
  test_gpu_exe = executable('test_gpu',
    'test_gpu.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )
  test('gpu_test', test_gpu_exe)

  test_gpu_damage_exe = executable('test_gpu_damage',
    'test_gpu_damage.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )
  test('gpu_damage_test', test_gpu_damage_exe)

  test_runtime_effect_exe = executable('test_runtime_effect',
    'test_runtime_effect.cpp',
    dependencies : [nisaba_dep, extra_deps]
  )
  test('runtime_effect_test', test_runtime_effect_exe)

  if get_option('enable_vulkan') and vulkan_dep.found()
    test_vulkan_exe = executable('test_vulkan',
      'test_vulkan.cpp',
      dependencies : [nisaba_dep, extra_deps]
    )
    test('vulkan_test', test_vulkan_exe)
  endif
endif


if get_option('enable_backend_os')
  test_backend_os_exe = executable('test_backend_os',
    'test_backend_os.cpp',
    dependencies : [nisaba_dep, nisaba_backend_os_dep]
  )
  test('backend_os_test', test_backend_os_exe)
endif

# Damage tracking subsystem unit tests
test_damage_tracker_exe = executable('test_damage_tracker',
  'test_damage_tracker.cpp',
  dependencies : nisaba_dep
)
test('damage_tracker_test', test_damage_tracker_exe)

# VSPR Primitive & Shape Instancer unit tests
test_primitive_instancer_exe = executable('test_primitive_instancer',
  'test_primitive_instancer.cpp',
  dependencies : nisaba_dep
)
test('primitive_instancer_test', test_primitive_instancer_exe)

# VSPR AOT UI Compilation unit tests
test_aot_ui_exe = executable('test_aot_ui',
  'test_aot_ui.cpp',
  dependencies : nisaba_dep
)
test('aot_ui_test', test_aot_ui_exe)

# Animation subsystem unit tests
if get_option('enable_animation')
  test_animation_exe = executable('test_animation',
    'test_animation.cpp',
    dependencies : nisaba_dep
  )
  test('animation_test', test_animation_exe)
endif

# Text Caching subsystem unit tests
test_text_cache_exe = executable('test_text_cache',
  'test_text_cache.cpp',
  dependencies : nisaba_dep
)
test('text_cache_test', test_text_cache_exe)

# SVG Caching subsystem unit tests
test_svg_cache_exe = executable('test_svg_cache',
  'test_svg_cache.cpp',
  dependencies : nisaba_dep
)
test('svg_cache_test', test_svg_cache_exe)

# Sovereign Lottie vector animation engine unit tests
if get_option('enable_lottie')
  test_lottie_exe = executable('test_lottie',
    'test_lottie.cpp',
    dependencies : nisaba_dep
  )
  test('lottie_test', test_lottie_exe)
endif

if get_option('enable_markdown')
  test_markdown_exe = executable('test_markdown',
    'test_markdown.cpp',
    dependencies : nisaba_dep
  )
  test('markdown_test', test_markdown_exe)
endif

if get_option('enable_pdf')
  test_pdf_exe = executable('test_pdf',
    'test_pdf.cpp',
    dependencies : nisaba_dep
  )
  test('pdf_test', test_pdf_exe)
endif

# RRect & BorderRadius 4-corner asymmetric geometry unit tests
test_rrect_exe = executable('test_rrect',
  'test_rrect.cpp',
  dependencies : nisaba_dep
)
test('rrect_test', test_rrect_exe)

if get_option('enable_compositor')
  test_compositor_exe = executable('test_compositor',
    'test_compositor.cpp',
    dependencies : nisaba_dep
  )
  test('compositor_test', test_compositor_exe)
endif

if get_option('enable_orion')
  test_physics_math_exe = executable('test_physics_math',
    'test_physics_math.cpp',
    dependencies : nisaba_dep
  )
  test('physics_math_test', test_physics_math_exe)

  test_physics_body_exe = executable('test_physics_body',
    'test_physics_body.cpp',
    dependencies : nisaba_dep
  )
  test('physics_body_test', test_physics_body_exe)

  test_physics_contact_exe = executable('test_physics_contact',
    'test_physics_contact.cpp',
    dependencies : nisaba_dep
  )
  test('physics_contact_test', test_physics_contact_exe)

  test_physics_solver_exe = executable('test_physics_solver',
    'test_physics_solver.cpp',
    dependencies : nisaba_dep
  )
  test('physics_solver_test', test_physics_solver_exe)

  test_physics_broadphase_exe = executable('test_physics_broadphase',
    'test_physics_broadphase.cpp',
    dependencies : nisaba_dep
  )
  test('physics_broadphase_test', test_physics_broadphase_exe)

  test_physics_gjk_epa_exe = executable('test_physics_gjk_epa',
    'test_physics_gjk_epa.cpp',
    dependencies : nisaba_dep
  )
  test('physics_gjk_epa_test', test_physics_gjk_epa_exe)

  test_physics_island_exe = executable('test_physics_island',
    'test_physics_island.cpp',
    dependencies : nisaba_dep
  )
  test('physics_island_test', test_physics_island_exe)

  test_physics_scene_integration_exe = executable('test_physics_scene_integration',
    'test_physics_scene_integration.cpp',
    dependencies : nisaba_dep
  )
  test('physics_scene_integration_test', test_physics_scene_integration_exe)

  test_render3d_camera_exe = executable('test_render3d_camera',
    'test_render3d_camera.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_camera_test', test_render3d_camera_exe)

  test_render3d_geometry_exe = executable('test_render3d_geometry',
    'test_render3d_geometry.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_geometry_test', test_render3d_geometry_exe)

  test_render3d_material_exe = executable('test_render3d_material',
    'test_render3d_material.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_material_test', test_render3d_material_exe)

  test_render3d_lighting_exe = executable('test_render3d_lighting',
    'test_render3d_lighting.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_lighting_test', test_render3d_lighting_exe)

  test_render3d_lighting_clustered_exe = executable('test_render3d_lighting_clustered',
    'test_render3d_lighting_clustered.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_lighting_clustered_test', test_render3d_lighting_clustered_exe)

  test_render3d_ibl_exe = executable('test_render3d_ibl',
    'test_render3d_ibl.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_ibl_test', test_render3d_ibl_exe)

  test_render3d_shadows_exe = executable('test_render3d_shadows',
    'test_render3d_shadows.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_shadows_test', test_render3d_shadows_exe)

  test_render3d_renderer_exe = executable('test_render3d_renderer',
    'test_render3d_renderer.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_renderer_test', test_render3d_renderer_exe)

  test_render3d_gltf_physics_exe = executable('test_render3d_gltf_physics',
    'test_render3d_gltf_physics.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_gltf_physics_test', test_render3d_gltf_physics_exe)

  test_render3d_gltf_extensions_exe = executable('test_render3d_gltf_extensions',
    'test_render3d_gltf_extensions.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_gltf_extensions_test', test_render3d_gltf_extensions_exe)

  test_render3d_animation_exe = executable('test_render3d_animation',
    'test_render3d_animation.cpp',
    dependencies : nisaba_dep
  )
  test('render3d_animation_test', test_render3d_animation_exe)

  if get_option('enable_gpu')
    test_render3d_gpu_backend_exe = executable('test_render3d_gpu_backend',
      'test_render3d_gpu_backend.cpp',
      dependencies : [nisaba_dep, extra_deps]
    )
    test('render3d_gpu_backend_test', test_render3d_gpu_backend_exe)

    test_render3d_postprocess_exe = executable('test_render3d_postprocess',
      'test_render3d_postprocess.cpp',
      dependencies : [nisaba_dep, extra_deps]
    )
    test('render3d_postprocess_test', test_render3d_postprocess_exe)
  endif
endif




