project('nisaba', 'cpp',
  version : '0.1.0',
  default_options : [
    'warning_level=3',
    'cpp_std=c++20',
    'optimization=2',
    'default_library=both',
  ]
)

add_project_arguments('-D_USE_MATH_DEFINES', language: ['c', 'cpp'])

cpp_comp = meson.get_compiler('cpp')
 if cpp_comp.has_argument('-march=native')
  add_project_arguments('-march=native', language: ['c', 'cpp'])
 endif

nisaba_inc = [include_directories('include')]

nisaba_sources = files(
  'src/path/dash.cpp',
  'src/path/stroker.cpp',
  'src/path/path_ops.cpp',
  'src/path/path_measure.cpp',
  'src/canvas/pixmap.cpp',
  'src/raster/line_clipper.cpp',
  'src/raster/edge.cpp',
  'src/raster/edge_builder.cpp',
  'src/raster/edge_clipper.cpp',
  'src/raster/scan.cpp',
  'src/canvas/mask.cpp',
  'src/shaders/shader.cpp',
  'src/color/color_space_lut.cpp',
  'src/pipeline/blitter.cpp',
  'src/canvas/painter.cpp',
  'src/text/attrs.cpp',
  'src/text/ttf_font.cpp',
  'src/text/cff_font.cpp',
  'src/text/opentype_tables.cpp',
  'src/text/bidi.cpp',
  'src/text/layout.cpp',
  'src/text/glyph_cache.cpp',
  'src/text/linux_font_config.cpp',
  'src/text/font_system.cpp',
  'src/text/buffer.cpp',
  'src/text/cursor.cpp',
  'src/text/baked_text.cpp',
  'src/text/text_cache.cpp',
  'src/effects/blur.cpp',
  'src/effects/color_filter.cpp',
  'src/effects/color_matrix.cpp',
  'src/effects/drop_shadow.cpp',
  'src/effects/glass.cpp',
  'src/svg/path_parser.cpp',
  'src/svg/svg_document.cpp',
  'src/svg/baked_svg.cpp',
  'src/svg/svg_cache.cpp',
  'src/math/transform4x4.cpp',
  'src/math/rrect.cpp',
  'src/raster/perspective_raster.cpp',
  'src/mesh/mesh.cpp',
  'src/raster/mesh_raster.cpp',
  'src/image/deflate.cpp',
  'src/image/png.cpp',
  'src/image/jpeg.cpp',
  'src/image/qoi.cpp',
  'src/image/webp.cpp',
  'src/image/bmp.cpp',
  'src/image/ppm.cpp',
  'src/image/hdr.cpp',
  'src/image/image_io.cpp',
)

gl_dep = dependency('gl', required : false)
egl_dep = dependency('egl', required : false)
dl_dep = dependency('dl', required : false)
glew_dep = dependency('glew', required : get_option('enable_gpu'))
vulkan_dep = dependency('vulkan', required : get_option('enable_vulkan'))
gbm_dep = dependency('gbm', required : get_option('enable_compositor'))
drm_dep = dependency('libdrm', required : get_option('enable_compositor'))
wayland_server_dep = dependency('wayland-server', required : get_option('enable_compositor'))
glesv2_dep = dependency('glesv2', required : get_option('enable_compositor'))

extra_deps = []
if dl_dep.found()
  extra_deps += dl_dep
endif
if gl_dep.found()
  extra_deps += gl_dep
endif
if egl_dep.found()
  extra_deps += egl_dep
endif
if glew_dep.found()
  extra_deps += glew_dep
endif
if vulkan_dep.found()
  extra_deps += vulkan_dep
endif
if gbm_dep.found()
  extra_deps += gbm_dep
endif
if drm_dep.found()
  extra_deps += drm_dep
endif
if wayland_server_dep.found()
  extra_deps += wayland_server_dep
endif
if glesv2_dep.found()
  extra_deps += glesv2_dep
endif

if get_option('enable_gpu')
  nisaba_inc += include_directories('src/gpu')
  add_project_arguments('-DNISABA_GLEW', language: ['c', 'cpp'])
  nisaba_sources += files(
    'src/gpu/types.cpp',
    'src/gpu/tessellator.cpp',
    'src/gpu/glyph_atlas.cpp',
    'src/gpu/context.cpp',
    'src/gpu/gl3_renderer.cpp',
    'src/gpu/gpu_device.cpp',
    'src/gpu/gpu_buffer.cpp',
    'src/gpu/gpu_texture.cpp',
    'src/gpu/gpu_surface.cpp',
    'src/gpu/gpu_tessellator.cpp',
    'src/gpu/gpu_canvas.cpp',
    'src/gpu/gpu_blur.cpp',
    'src/gpu/runtime_effect.cpp',
  )
  if get_option('enable_vulkan') and vulkan_dep.found()
    add_project_arguments('-DNISABA_HAS_VULKAN=1', language: ['c', 'cpp'])
    nisaba_sources += files(
      'src/gpu/vulkan/vulkan_device.cpp',
      'src/gpu/vulkan/vulkan_surface.cpp',
      'src/gpu/vulkan/vulkan_renderer.cpp',
    )
  endif
endif

if get_option('enable_compositor')
  add_project_arguments('-DNISABA_HAS_COMPOSITOR=1', language: ['c', 'cpp'])
  nisaba_sources += files(
    'src/gpu/compositor/gbm_device.cpp',
    'src/gpu/compositor/gbm_allocator.cpp',
    'src/gpu/compositor/gbm_buffer.cpp',
    'src/gpu/compositor/egl_context.cpp',
    'src/gpu/compositor/egl_image.cpp',
    'src/gpu/compositor/texture.cpp',
    'src/gpu/compositor/dmabuf_texture.cpp',
    'src/gpu/compositor/shm_texture.cpp',
    'src/gpu/compositor/solid_texture.cpp',
    'src/gpu/compositor/damage_ring.cpp',
    'src/gpu/compositor/dual_kawase.cpp',
    'src/gpu/compositor/render_target.cpp',
    'src/gpu/compositor/shader.cpp',
    'src/gpu/compositor/render_pass.cpp',
    'src/gpu/compositor/renderer.cpp',
    'src/gpu/compositor/scene_node.cpp',
    'src/gpu/compositor/scene_tree.cpp',
    'src/gpu/compositor/swapchain.cpp',
    'src/gpu/compositor/frame_clock.cpp',
    'src/gpu/compositor/compositor_canvas.cpp',
  )
endif


if get_option('enable_layout')
  nisaba_inc += include_directories('nisaba-layout/include')
  nisaba_sources += files(
    'nisaba-layout/src/core/node.cpp',
    'nisaba-layout/src/core/config.cpp',
    'nisaba-layout/src/core/layout_results.cpp',
    'nisaba-layout/src/solver/flex_solver.cpp',
    'nisaba-layout/src/solver/pipeline.cpp',
    'nisaba-layout/src/solver/baseline.cpp',
    'nisaba-layout/src/solver/pixel_grid.cpp',
    'nisaba-layout/src/debug/assert.cpp',
    'nisaba-layout/src/debug/log.cpp',
    'nisaba-layout/src/event/event.cpp',
  )
endif

if get_option('enable_animation')
  nisaba_sources += files(
    'src/animation/curves.cpp',
    'src/animation/spring_simulation.cpp',
    'src/animation/spring_controller.cpp',
    'src/animation/timeline.cpp',
    'src/animation/particle_system.cpp',
  )
endif

if get_option('enable_lottie')
  nisaba_sources += files(
    'src/json/json.cpp',
    'src/lottie/property.cpp',
    'src/lottie/shape.cpp',
    'src/lottie/layer.cpp',
    'src/lottie/animation.cpp',
    'src/lottie/player.cpp',
  )
endif

if get_option('enable_markdown')
  nisaba_sources += files(
    'src/markdown/html_parser.cpp',
    'src/markdown/syntax_highlighter.cpp',
    'src/markdown/parser.cpp',
    'src/markdown/document.cpp',
    'src/markdown/style.cpp',
    'src/markdown/renderer.cpp',
    'src/markdown/math_engine.cpp',
    'src/markdown/diagram_engine.cpp',
  )
endif

if get_option('enable_pdf')
  nisaba_sources += files(
    'src/pdf/pdf_types.cpp',
    'src/pdf/pdf_writer.cpp',
    'src/pdf/pdf_canvas.cpp',
    'src/pdf/pdf_document.cpp',
    'src/pdf/pdf_parser.cpp',
    'src/pdf/pdf_cmap.cpp',
    'src/pdf/pdf_interpreter.cpp',
    'src/pdf/pdf_reader.cpp',
    'src/pdf/pdf_codec.cpp',
    'src/pdf/pdf_crypto.cpp',
    'src/pdf/pdf_navigation.cpp',
  )
endif

if get_option('enable_markdown') and get_option('enable_pdf')
  nisaba_sources += files(
    'src/markdown/pdf_exporter.cpp',
  )
endif

if get_option('enable_orion')
  add_project_arguments('-DNISABA_HAS_ORION=1', '-DNISABA_HAS_PHYSICS=1', '-DNISABA_HAS_RENDER3D=1', language: ['c', 'cpp'])
  nisaba_inc += include_directories('orion/include')
  nisaba_sources += files(
    'orion/src/math/math.cpp',
    'orion/src/physics/body/body.cpp',
    'orion/src/physics/collision/shape.cpp',
    'orion/src/physics/collision/gjk.cpp',
    'orion/src/physics/collision/epa.cpp',
    'orion/src/physics/collision/narrow_phase.cpp',
    'orion/src/physics/solver/impulse_solver.cpp',
    'orion/src/physics/scene/physics_scene.cpp',
    'orion/src/physics/scene/island.cpp',
    'orion/src/physics/broadphase/broad_phase.cpp',
    'orion/src/camera/frustum.cpp',
    'orion/src/camera/camera.cpp',
    'orion/src/camera/orbit_controller.cpp',
    'orion/src/geometry/mesh.cpp',
    'orion/src/geometry/primitives.cpp',
    'orion/src/material/texture.cpp',
    'orion/src/material/pbr_material.cpp',
    'orion/src/material/brdf.cpp',
    'orion/src/lighting/light.cpp',
    'orion/src/lighting/cluster_grid.cpp',
    'orion/src/lighting/lighting_manager.cpp',
    'orion/src/lighting/shadow_map.cpp',
    'orion/src/ibl/spherical_harmonics.cpp',
    'orion/src/ibl/dfg_lut.cpp',
    'orion/src/ibl/environment_map.cpp',
    'orion/src/scene/renderable.cpp',
    'orion/src/scene/scene.cpp',
    'orion/src/scene/view.cpp',
    'orion/src/scene/renderer.cpp',
    'orion/src/bridge/physics_bridge.cpp',
    'orion/src/animation/skeleton.cpp',
    'orion/src/animation/animation_clip.cpp',
    'orion/src/animation/animator.cpp',
    'orion/src/animation/skinned_mesh.cpp',
    'orion/src/gltf/gltf_loader.cpp',
    'orion/src/gpu/gl_shader_program.cpp',
    'orion/src/gpu/gl_state_guard.cpp',
    'orion/src/gpu/gpu_resource_cache.cpp',
    'orion/src/gpu/gpu_shadow_pass.cpp',
    'orion/src/gpu/gpu_scene_depth_pass.cpp',
    'orion/src/postprocess/bloom_pass.cpp',
    'orion/src/postprocess/gtao_pass.cpp',
    'orion/src/postprocess/postprocess_pipeline.cpp',
    'orion/src/gpu/gpu_backend3d.cpp',
    'orion/src/gpu/render_surface3d.cpp',
    'orion/src/orionjs/orionjs.cpp',
    'orion/src/orionimg/deflate.cpp',
    'orion/src/orionimg/png.cpp',
    'orion/src/orionimg/jpeg.cpp',
    'orion/src/orionimg/qoi.cpp',
    'orion/src/orionimg/webp.cpp',
    'orion/src/orionimg/bmp.cpp',
    'orion/src/orionimg/ppm.cpp',
    'orion/src/orionimg/hdr.cpp',
    'orion/src/orionimg/orionimg.cpp',
  )
endif

nisaba_lib = both_libraries('nisaba',
  nisaba_sources,
  include_directories : nisaba_inc,
  dependencies : extra_deps,
  install : true
)

nisaba_dep = declare_dependency(
  include_directories : nisaba_inc,
  link_with : nisaba_lib.get_static_lib(),
  dependencies : extra_deps
)

# Headers installation
install_subdir('include/nisaba', install_dir : get_option('includedir'))

# Pkg-config generation
pkg = import('pkgconfig')
pkg.generate(
  nisaba_lib,
  name : 'nisaba',
  description : 'Nisaba 2D Software CPU Rendering Engine for vaxp',
  version : meson.project_version(),
  subdirs : 'nisaba'
)

if get_option('enable_backend_os')
  subdir('backend_os')
endif

if get_option('build_tests')
  subdir('tests')
endif

if get_option('build_examples')
  subdir('examples')
endif

if get_option('build_benchmarks')
  subdir('benchmarks')
endif

