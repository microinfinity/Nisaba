<div align="center">

![Nisaba — Full-Stack 2D Graphics Engine](assets/Nisaba-background-banner.jpeg)

# Nisaba — Full-Stack 2D Graphics Engine
### Sovereign C++20 2D Vector Graphics, Typography, UI Layout & Hardware GPU Engine

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Build System](https://img.shields.io/badge/Build-Meson%20%2B%20Ninja-green.svg)](https://mesonbuild.com)
[![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20(Sovereign)-brightgreen.svg)](#technological-sovereignty)
[![Platforms](https://img.shields.io/badge/Platforms-Linux%20%7C%20Windows%20%7C%20Android%20%7C%20WASM%20%7C%20DRM%2FKMS-orange.svg)](#cross-platform)

</div>

**Nisaba** (`nisaba`) is an embedded-first, zero-dependency, ultra-lightweight graphics rendering, UI layout, and simulation engine written from scratch in **Modern C++ (C++20)**, featuring multi-backend sovereign rendering (CPU software rasterizer, native **Vulkan** GPU pipeline, and **OpenGL ES 3.2** GPU pipeline), a sovereign 3D Real-Time PBR Rendering Pipeline (`nisaba::render3d`) with 64-bit front-to-back/back-to-front draw sorting and glTF 2.0 ingestion, a sovereign Wayland hardware GPU compositor subsystem (`nisaba::compositor`), a sovereign 3D rigid body physics pipeline (`nisaba::physics`) with SIMD acceleration and 0-heap allocation, exact geometric path boolean operations (`nisaba::path_ops`), stroke-to-fill path outlining (`nisaba::stroker`), multi-format embedded framebuffers (RGB565, Alpha8, Gray8, BGRA8888, RGBA8888), physical linear-space sRGB color blending, a deterministic 7-stage Flexible Box and UI layout engine (`nisaba-layout`), complete sovereign image codec subsystems, a sovereign Lottie vector animation engine (`nisaba::lottie`), an optional sovereign native OS platform and windowing subsystem (`backend_os`), a sovereign Markdown document layout and multi-page rendering engine (`nisaba::markdown`), and a full-specification sovereign PDF document engine (`nisaba::pdf`). Purpose-built as the native rendering foundation for the **`vaxp`** cross-platform organization.

Nisaba delivers high-end vector graphics, real-time 3D PBR rendering with Cook-Torrance GGX microfacet BRDF and Cascaded Shadow Maps (CSM), direct DMA-BUF zero-copy Wayland window composition, a high-performance 3D rigid body simulation engine (4-way SIMD BVH broadphase, GJK/EPA narrowphase, sequential impulse solver, simulation islands, and sleep deactivation), geometric path boolean algebra (Union, Difference, Intersect, Xor), high-precision 256-level subpixel anti-aliasing, hardware 4x MSAA, analytical SDF Gaussian shadows, screen-space dithering, two-point conical & compose multi-shaders, procedural GLSL runtime shaders, 4x5 affine color matrix filtering, Porter-Duff compositing, full W3C SVG vector graphics with gradients and masks, real-time Bodymovin/Lottie vector animations, a sovereign multilingual typography subsystem with native TrueType and PostScript CFF/OpenType layout, zero-dependency PNG/JPEG/WebP/QOI image codecs, a sovereign CommonMark/GFM Markdown engine, a full-specification ISO 32000 PDF document engine, an adaptive flexbox and stack layout solver, and a sovereign cross-platform OS windowing layer with **absolute technological sovereignty** (zero external third-party dependencies, 100% free of GLFW/SDL).


---

## Visual Showcases

### Sovereign Image Codecs Showcase — PNG, JPEG, WebP & QOI (VAXP Logo Integration)
![Nisaba Image Codecs Showcase](showcase/showcase_vaxp.png)


### Vector & Shader Rendering Showcase
![Nisaba Vector Showcase](showcase/embedded_hmi_showcase.png)

### Multilingual Typography & Text Layout Showcase
![Nisaba Text Showcase](showcase/nisaba_text_showcase.png)

### Modern Glassmorphism & Visual Effects Showcase
![Nisaba Effects Showcase](showcase/nisaba_effects_showcase.png)

### Sovereign SVG Icons & Vector Graphics Showcase
![Nisaba SVG Showcase](showcase/nisaba_svg_showcase.png)

### High-Performance SVG Caching & SIMD Pre-Baking Subsystem Showcase (`SvgCache` & `BakedSvg`)
![Nisaba SVG Cache Showcase](showcase/showcase_svg_cache.png)

### 3D Perspective Transformations & Spatial Warp Showcase
![Nisaba 3D Perspective Showcase](showcase/nisaba_perspective_showcase.png)

### 2D Vertex Mesh & Freeform Gradient Meshes Showcase
![Nisaba 2D Mesh Showcase](showcase/nisaba_mesh_showcase.png)

### Hardware-Accelerated GPU Rendering Showcase (Vulkan & OpenGL ES 3.2 with 4x MSAA)
![Nisaba GPU Showcase](showcase/example_calculator.png)

### Hardware-Accelerated GPU Backdrop Blur & Frosted Glass Showcase (`DualKawaseBlur`)
![Nisaba GPU Blur Showcase](showcase/glass_gpu_blur_verified.png)

### Sovereign Procedural Runtime Shaders & Dynamic Container Injection Showcase (`nisaba::gpu::RuntimeEffect` & `RuntimeShader`)
![Nisaba Runtime Shaders Showcase](showcase/runtime_shaders_visual_showcase.png)

### Sovereign Lottie Vector Animation Engine Showcase (`nisaba::lottie` & Hardware GPU Acceleration)
![Nisaba Lottie GPU Showcase](showcase/nisaba_lottie_gpu_showcase.png)

### Sovereign Adaptive UI Layout Engine Showcase (`nisaba-layout`)
Nisaba's native C++20 Flexible Box and UI Layout Engine powering reactive desktop, embedded, and dashboard layouts with deterministic subpixel layout resolution.

| Responsive Multi-Row Complex Dashboard | Stack & Absolute Positioning (Z-Layers, Badges & Insets) |
| :---: | :---: |
| ![Complex Dashboard Showcase](showcase/layout_complex_dashboard.png) | ![Stack & Absolute Positioning Showcase](showcase/layout_absolute_stack.png) |

| Flex Direction (Row, Column, RowReverse, ColumnReverse) | Justify Content (Distribution Along Main Axis) |
| :---: | :---: |
| ![Flex Direction](showcase/layout_flex_direction.png) | ![Justify Content](showcase/layout_justify_content.png) |

| Cross-Axis Alignment & Stretch (`align-items` / `align-self`) | Flex Wrap & Dimensional Gaps (`gap`, `row-gap`, `column-gap`) |
| :---: | :---: |
| ![Align Items](showcase/layout_align_items.png) | ![Flex Wrap & Gap](showcase/layout_flex_wrap_gap.png) |

| Space Distribution (`flex-grow`, `flex-shrink`, `flex-basis`) | Full CSS Box Model (`margin: auto`, padding, border, box-sizing) |
| :---: | :---: |
| ![Flex Grow & Shrink](showcase/layout_flex_grow_shrink.png) | ![Box Model & Margin Auto](showcase/layout_box_model.png) |

### Sovereign Markdown Rendering & Document Layout Showcase (`nisaba::markdown`)
![Nisaba Markdown Showcase](showcase/nisaba_markdown_showcase.png)

### Sovereign Full-Specification PDF Studio & GPU Viewer Showcase (`nisaba::pdf` & `nisaba::gpu`)
![Nisaba PDF Viewer Showcase](showcase/nisaba_pdf_viewer_showcase.png)

### VSPR Ahead-Of-Time (AOT) UI Scene Compilation Showcase (`nisaba::vspr::AotSceneCompiler`)
![VSPR AOT UI Compilation Showcase](showcase/aot_ui_comparison_gpu.png)

### VSPR Hardware Primitive Instancing Showcase (`nisaba::vspr::PrimitiveInstancer`)
![VSPR Primitive Instancing Showcase](showcase/instancing_comparison_gpu.png)

### Sovereign Wayland GPU Compositor & Vector Pipeline Fusion Showcase (`nisaba::compositor` & `nisaba::gpu`)
![Nisaba Sovereign GPU Compositor Showcase](showcase/compositor_desktop_showcase.png)

High-performance composite of a Full HD 1080p desktop session running at ultra-high real-time framerates and sub-millisecond composition latency with zero dynamic heap allocations in steady-state loop. Demonstrates zero-copy DMA-BUF window surface scanout, analytical SDF window drop shadows, hardware-accelerated frosted glass dock with Dual-Kawase backdrop blur, live telemetry activity monitors, and procedural GLSL runtime shader integration.

---

## Key Architectural Pillars

### 1. Absolute Technological Sovereignty (Zero External Dependencies)
- Implemented strictly using standard ISO C++20 and standard compiler toolchains.
- Completely free of external graphics, rasterization, or mathematical libraries.
- Features native, self-contained BMP and PPM decoders and encoders directly inside the core.

### 2. True Zero-Copy Framebuffer Rendering with Stride
- Supports direct rendering into arbitrary raw memory pointers (`uint8_t*`) with hardware row stride / pitch alignment.
- Ready for immediate integration with native display scanout buffers:
  - Linux DRM/KMS Dumb Buffers (`DRM_FORMAT_XRGB8888` / `ARGB8888`)
  - Linux Framebuffer (`/dev/fb0`)
  - X11/Wayland Shared Memory (`wl_shm`)
  - Windows DIB Sections
  - Android `ANativeWindow_Buffer`
  - WebAssembly `ImageData`

### 3. Hardware Acceleration via SIMD
- High-performance vector paths processing 4 pixels concurrently utilizing SSE2 / AVX2 (x86_64) and ARM NEON (ARM/AArch64).
- 100% portable scalar generic C++20 fallback for microcontrollers and platforms lacking SIMD extensions.

### 4. Arbitrary Vector Clip Paths
- Clip any drawing operation within arbitrary, complex vector paths (`Canvas::clip_path`, `Canvas::clip_rect`).
- Complete hierarchical state stack to preserve and restore clip regions and transformation matrices (`Canvas::save()`, `Canvas::restore()`).

### 5. Sovereign Multilingual Typography & OpenType Subsystem (`nisaba::text`)
- **Native Binary TTF/OTF Font Parser**: Directly parses TrueType and OpenType vector outlines and emits them as native `nisaba::Path` curves without external font libraries.
- **PostScript CFF Type 2 Charstrings**: Native bytecode parsing of `.otf` font outlines supporting stack evaluation, local and global subroutines (`callsubr`, `callgsubr`), stem hints, transient storage, and exact cubic Bézier curve extraction (`rrcurveto`, `rcurveline`, `hhcurveto`, `vvcurveto`) without conversion approximations.
- **OpenType Advanced Typography Engine (`GSUB` & `GPOS`)**:
  - **`GSUB` (Glyph Substitution)**: Supports Format 1 & 2 single substitutions, standard typographic ligatures (`fi`, `fl`, `ffi`, `ffl`), and contextual chaining substitutions.
  - **`GPOS` (Glyph Positioning)**: Direct pair kerning evaluation across Coverage-based and Class-based pair adjustment tables.
- **Bidirectional Text Layout (BiDi)**: Implements Unicode Bidirectional Algorithm rules (UAX #9) and 4-form contextual Arabic shaping (Isolated, Initial, Medial, Final).
- **East Asian (CJK) & Global Scripts Support**: Specialized Kinsoku Shori line-breaking rules and spaceless boundary classification for Japanese, Chinese, and Korean alongside Cyrillic and Latin.
- **Multi-Line Text Buffer**: Dynamic paragraph layout with configurable wrapping (`Wrap::Word`, `Wrap::Glyph`) and alignment (`Align::Left`, `Right`, `Center`, `Justify`).
- **Subpixel Anti-Aliased Glyph Cache**: High-precision 4-bin subpixel horizontal positioning cache utilizing Nisaba's native scanline rasterizer.
- **Interactive Cursor & Selection**: Text editing primitives including caret positioning, selection ranges, and spatial hit-testing (`Cursor`, `Selection`, `HitResult`).
- **Built-in Monospace 8x16 Font**: Embedded in-memory bitmap font for zero-dependency boot consoles, debug logging, and realtime FPS telemetry on headless or embedded screens.

### 6. Sovereign Visual Effects, Shadows & Color Matrix Filters (`nisaba::effects`)
- **Fast Separable Gaussian Blur**: 3-pass sliding accumulator box blur operating in $O(1)$ per-pixel time regardless of radius size on both 32-bit RGBA surfaces and 8-bit alpha masks.
- **Elevation Drop Shadows & Glowing Halos**: Padded subpixel vector mask convolutions with customizable offsets, blur sigma, spread, and ambient neon glow halos (`DropShadow`).
- **Modern Glassmorphism Panels**: Single-line frosted glass card rendering with real backdrop blur refraction, translucent frost overlay tint, and specular edge reflection borders (`Canvas::draw_glass_panel`).
- **4x5 Affine Color Transformation Matrix (`ColorMatrix`)**:
  - Full $4 \times 5$ affine color transformation matrix $[M_{4 \times 4} \mid T_{4 \times 1}]$ evaluating:
    $$\begin{pmatrix} R' \\ G' \\ B' \\ A' \end{pmatrix} = M \cdot \begin{pmatrix} R \\ G \\ B \\ A \end{pmatrix} + T$$
  - Provides instant color space transforms: saturation scaling, hue rotation, sepia, luminance projection, and brightness/contrast adjustments in a single pass.
  - Applied directly in-place across `Pixmap` surfaces or dynamically during rasterization via `Paint::set_color_filter()`.
- **In-Place Color Filters**: Instant adjustments for brightness, contrast, grayscale luminance, sepia tones, and color inversion.

### 7. Sovereign W3C SVG Vector Graphics, Icon Subsystem & High-Performance Caching (`nisaba::svg`)
- **Complete W3C SVG Path Lexer**: Full support for standard SVG path commands: `M/m`, `L/l`, `H/h`, `V/v`, `C/c`, `S/s`, `Q/q`, `T/t`, `A/a`, `Z/z`.
- **Analytical Elliptical Arc Decomposition**: Converts SVG `A/a` arc parametrization into exact cubic Bézier curves with $C^1$ continuity.
- **Zero-Dependency SVG/XML Document Parser**: Parses `<path>`, `<rect>`, `<circle>`, `<ellipse>`, `<line>`, `<polyline>`, and `<polygon>` elements, handling fills, strokes, line caps, line joins, opacities, transformations, and `viewBox` scaling.
- **Complete Paint Servers & Gradients**: Full support for `<linearGradient>` and `<radialGradient>` with arbitrary color stops, `gradientUnits` (`userSpaceOnUse`, `objectBoundingBox`), `gradientTransform`, and spread modes (`pad`, `reflect`, `repeat`).
- **Resource Re-use & Definitions**: Resolves `<defs>` dictionaries and dynamic `<use>` instancing with cascading transforms and inherited style attributes.
- **Vector Clipping & Masking**: Evaluates `<clipPath>` arbitrary vector clipping masks across child groups and supports luminosity and alpha `<mask>` elements.
- **Embedded Raster Images**: Decodes and renders `<image>` elements from filesystem paths and inline RFC 2397 base64 data URIs (`data:image/png;base64,...`).
- **Affine Skew Transformations**: Exact support for `skewX(angle)` and `skewY(angle)` within SVG transformation chains.
- **Direct Canvas Integration**: Render arbitrary SVG icons and vector illustrations into any target rectangle preserving aspect ratio (`Canvas::draw_svg`).
- **High-Performance SVG Caching (`SvgCache`)**:
  - Eliminates repeated runtime Bézier parsing, curve tessellation, and scanline rasterization by caching pre-rasterized vector assets in an intelligent LRU (Least Recently Used) cache.
  - Generates robust 64-bit FNV-1a composite keys combining source path/XML hash, target dimensions, and dynamic tint values.
  - Achieves massive multi-fold throughput gains over raw uncached vector rasterization by reducing repeated curve parsing and tessellation overhead to near zero per draw call.
- **Pre-Baked Surfaces (`BakedSvg`)**:
  - Pre-bakes SVG documents once to high-quality raster surfaces (`Pixmap`), turning subsequent draws into instant $O(1)$ SIMD row blits via `simd::blend_source_over_span`.
  - Direct integration into `Canvas::draw_baked_svg(baked_svg, x, y, opacity)`.
- **Dynamic Runtime Tint Modulation (Tinting)**:
  - Enables dynamic color modulation for monochrome SVG icons (e.g. UI status icons, battery, settings, search) at arbitrary resolutions and colors without modifying the source XML or duplicating asset files.
- **GPU Pattern Blitting**:
  - Seamless drawing onto hardware-accelerated `GpuCanvas` textures with pattern rendering, maintaining fluid high-refresh-rate frame rates even under dense multi-icon loads.

### 8. Sovereign 3D Perspective & Spatial Warping Engine (`nisaba::math` & `nisaba::raster`)
- **4x4 Projective Transformation Matrix (`Transform4x4`)**: Full 3D affine and projective mathematics including 3D Euler rotations (pitch, yaw, roll), scale, translation, and perspective camera projection with focal distance $d$ ($W = 1 - z/d$).
- **Paul Heckbert 8-DOF Quad Homography Solver**: Direct analytical computation of the 3x3 projective homography mapping any source rectangle to an arbitrary non-planar quadrilateral without diagonal distortion or affine tearing.
- **Perspective-Correct Scanline Rasterizer**: High-performance linear scanline interpolation in homogeneous space $(u/w, v/w, 1/w)$ with per-pixel reciprocal division, bilinear texture filtering, and subpixel edge antialiasing.
- **Seamless Canvas Integration**: Draw warped quads and 3D rotated surfaces with single-line API calls (`Canvas::draw_pixmap_perspective`, `Canvas::draw_pixmap_3d`).

### 9. Sovereign 2D Vertex Mesh & Freeform Gradient Meshes (`nisaba::mesh` & `nisaba::raster`)
- **Flexible Vertex Modes (`VertexMode`)**: Support for `Triangles`, `TriangleStrip`, and `TriangleFan` with optional 32-bit indexed buffers and automated grid tessellation (`Vertices::create_grid`).
- **Differential Barycentric Triangle Rasterizer**: Analytical scanline edge walking with constant horizontal derivatives $(dR, dG, dB, dA)$ and $(dU, dV)$, achieving fast $O(1)$ per-pixel attribute interpolation.
- **Per-Vertex Gouraud Shading**: Rich, multi-point color interpolation for low-poly artwork, vector primitives, and specular lighting.
- **Freeform 2D Mesh Deformation & Bilinear UV Mapping**: Wrap and deform source textures across organic waving, stretching, or wrinkled 2D meshes without shearing or pixelation (`Canvas::draw_textured_vertices`).
- **Photorealistic Bicubic Coons Patches (`CoonsPatch` & `GradientMesh`)**: Analytical blending of 4 cubic Bézier boundary curves and 4 corner colors, matching vector illustration standards (`Canvas::draw_gradient_mesh`).

### 10. Sovereign Hardware-Accelerated GPU Pipeline (`nisaba::gpu`)
- **Multi-Backend Sovereign Architecture (`GpuBackendType`)**:
  - Unified hardware abstraction layer supporting both native **Vulkan** (`GpuBackendType::Vulkan`) and **OpenGL ES 3.2 / Headless EGL** (`GpuBackendType::OpenGL`).
  - Automatic intelligent runtime arbitration (`GpuBackendType::Auto`) prioritizing modern Vulkan compute/graphics with graceful fallback to OpenGL ES.
  - Environment variable dynamic override (`NISABA_GPU_BACKEND=vulkan` or `NISABA_GPU_BACKEND=opengl`) for testing and hardware diagnostics without recompilation.
- **Native Vulkan 1.0+ GPU Pipeline (`VulkanDevice`, `VulkanSurface`, `VulkanRenderer`)**:
  - Direct hardware binding to the native Vulkan loader with zero third-party framework dependencies.
  - Self-contained GPU memory management (`VkDeviceMemory`), staging host-visible buffers for zero-stall host-to-device transfers, and linear blit resolves.
  - Custom compiled SPIR-V bytecode shaders embedded directly into engine binaries (`vulkan_shaders.hpp`).
  - Dedicated multi-attachment `VkRenderPass` architecture: hardware 4x MSAA color buffers, combined depth/stencil attachments (`VK_FORMAT_D24_UNORM_S8_UINT` / `VK_FORMAT_D32_SFLOAT_S8_UINT`), and automated single-sample resolve transitions.
  - Dynamic viewport and scissor states (`VkPipelineDynamicStateCreateInfo`) matching Nisaba's vector clipping hierarchy.
- **Headless EGL & OpenGL ES 3.2 Driver Interface (`GLDevice`, `GLSurface`, `GL3Renderer`)**:
  - Direct, zero-dependency binding to GPU hardware drivers via headless EGL, enabling high-performance hardware rendering and headless offscreen compute across Linux, Android, DRM/KMS, and desktop displays.
- **4x Hardware Multi-Sample Anti-Aliasing (MSAA)**: Native multisampled color and depth/stencil renderbuffers/attachments with automated blit resolve to single-sample surfaces during frame readback, guaranteeing smooth antialiased vector geometry.
- **Hardware GPU Backdrop Blur & Dual-Kawase Hierarchical Blur Engine (`GpuCanvas::draw_backdrop_blur`, `nisaba::gpu::DualKawaseBlur`)**:
  - Real-time sovereign Dual-Kawase hierarchical FBO pyramid blur engine with zero dynamic heap allocation per frame.
  - 5-tap downsampling pre-filter with hardware bilinear texture filtering:
    $$\text{FragColor} = 0.125 \times (4 \times T(uv) + T(uv - d) + T(uv + d) + T(uv + (d_x, -d_y)) + T(uv + (-d_x, d_y)))$$
  - 8-tap upsampling tent reconstruction filter eliminating boxiness and high-frequency edge sparkling:
    $$\text{FragColor} = \frac{1}{12} \times \left(\sum \text{corners} + 2 \times \sum \text{edges}\right)$$
  - Core Profile VAO + Fullscreen Triangle VBO rasterization.
  - Automated vector framebuffer synchronization ensuring all background vector primitives are rasterized prior to texture capture (`glCopyTexSubImage2D`).
  - High-level primitives: `GpuCanvas::draw_backdrop_blur(Rect, BorderRadius, sigma)` and `GpuCanvas::draw_backdrop_blur(RRect, sigma)` powering frosted glass cards, blurred dialogs, and translucent panels at fluid real-time framerates.
- **GPU Vector Path & Shape Clipping via Dual-Nibble Stencil Buffer (`GpuCanvas::clip_path`, `GpuCanvas::clip_rrect`)**:
  - Dedicated `GL_DEPTH24_STENCIL8` renderbuffer attachment on both resolve and 4x MSAA framebuffers.
  - Dual-nibble stencil state encoding:
    - **Bits 4..7 (Upper Nibble)**: Encodes active clip depth level supporting up to 15 nested clipping scopes ($C_1 \cap C_2 \dots$).
    - **Bits 0..3 (Lower Nibble)**: Transient scratch space for winding-number (`GL_INCR_WRAP`/`GL_DECR_WRAP`) and even-odd (`GL_INVERT`) polygon accumulation.
  - Two-pass stencil tessellation & cover quad pipeline (`GL3Renderer::renderClip` / `restoreClip`):
    - Pass 1: Disables color writes and evaluates path geometry into scratch bits 0..3 matching previous depth.
    - Pass 2: Screen/bounding-box cover quad testing `(stencil & 0x0f) != 0` replacing passing fragments with `targetDepth << 4` (clearing scratch bits back to 0) via `GL_REPLACE`.
  - Full state stack preservation across `GpuCanvas::save()` / `restore()` with automatic `stencil_clip_depth` unwinding.
  - Mask-safe buffer clearing with `GpuDevice::clear` resetting write masks (`glColorMask`, `glDepthMask`, `glStencilMask(0xff)`).
- **Hardware-Accelerated Asymmetric 4-Corner Rounded Rectangles (`BorderRadius` & `RRect` Fast-Path)**:
  - Direct zero-tessellation analytical GPU quad fast-path via `ShaderRRect` and `roundedRectVarying` shader pipeline.
  - Complete W3C CSS Backgrounds & Borders Level 3 proportional constraint scaling ($f = \min(1.0, \dots)$) guaranteeing zero overlapping corner distortion on arbitrary dimensions.
  - Native GPU primitives: `GpuCanvas::fill_rrect(const RRect&, const Paint&)`, `stroke_rrect(const RRect&, const Paint&)`, and `clip_rrect(const RRect&)`.
  - Analytical GPU drop shadows `GpuCanvas::draw_rrect_shadow(const RRect&, const effects::DropShadow&)` and translucent frosted glass cards `GpuCanvas::draw_glass_panel(const RRect&, const effects::GlassPanelParams&)`.
- **Triangular Screen-Space Dithering (Anti-Banding)**: High-precision temporal-stable triangular probability density function (TPDF) dither embedded directly into linear, radial, and angular sweep gradient shaders (both GLSL and SPIR-V), eradicating color banding artifacts.
- **Analytical Signed Distance Field (SDF) Gaussian Shadows & Neon Halos**: Closed-form mathematical evaluation of the 2D Gaussian convolution integral via the error function ($\text{erf}$), generating soft ambient drop shadows and vibrant glowing halos in a single GPU draw call.
- **Real-Time Glassmorphism & Translucent Panels (`GpuCanvas::draw_glass_panel`)**: Modern frosted glass UI surfaces incorporating analytical elevation drop shadows, tinted translucent bodies, specular edge reflection borders, and hardware backdrop blur.
- **Direct W3C SVG Vector Graphics on GPU (`GpuCanvas::draw_svg`)**: Native execution and GPU rasterization of arbitrary W3C SVG paths and icons with hardware MSAA and zero external dependencies.
- **Hardware GPU Text Rendering & Dynamic Glyph Atlas (`GpuCanvas::draw_text`, `GpuCanvas::draw_text_buffer`, `GpuGlyphAtlas`)**:
  - Full-specification hardware typography on GPU via dynamic Skyline 2D bin-packed alpha atlases ($1024 \times 1024$ dynamically expandable on-demand up to $2048 \times 2048$).
  - 1-pixel boundary padding eliminating texture bleeding, 4-bin subpixel horizontal positioning, and minimal dirty-region partial GPU texture flushes (`flushToGpu`).
  - Unified single-batch draw calls (`GpuCanvas::draw_text_buffer`) batching full shaped paragraphs and multi-line text layouts into a single GPU draw call with per-vertex color tinting (`Vertex::color`) for multicolored text spans and syntax highlighting.
  - Substantially accelerates CPU command submission and overall frame throughput by batching glyph geometry and eliminating redundant driver state transitions.
- **Ultra-Low CPU Command Overhead & Sustained High Frame Rates**:
  - Highly optimized GPU command recording and state caching delivering near-zero CPU submission overhead and sustained high-refresh-rate throughput across complex vector scenes.
- **Continuous Ribbon Stroking & High-Density Tessellation**: Adaptive curved geometry tessellation with averaged vertex miter normals for continuous, break-free ribbons and strokes at arbitrary angles.
- **Procedural Runtime Shaders & Dynamic Shader Injection Pipeline (`RuntimeEffect`, `RuntimeShaderBuilder`, `Shader`)**:
  - Runtime compilation and execution of procedural GLSL fragment programs on the OpenGL hardware pipeline.
  - Automatic synthesis and compilation into GLSL 150 Core with automated uniform reflection (`float`, `vec2`, `vec3`, `vec4`, `mat3`, `mat4`, `int`).
  - High-precision local coordinate transformation matrix (`u_localMatrix`) with inverted affine transformation for resolution-independent local `fragCoord` evaluation.
  - Direct hardware quad fast-path for axis-aligned rectangles with injected shaders (`Context::fill` and `GpuCanvas::fill_rect`), eliminating stencil passes and tessellation overhead.
  - Direct integration into vector fills (`Context::fillShader`, `Paint::set_shader`) and vector strokes (`Context::strokeShader`).
  - Strict paint state isolation with zero uniform or state leaks across consecutive draw calls.
- **Seamless Dual CPU/GPU Coexistence**: Complete architectural isolation ensuring 0 regressions on CPU rasterization while sharing mathematical types (`Rect`, `Point`, `Color`, `Path`, `Transform`, `RRect`, `BorderRadius`) and textures (`Pixmap`).


### 11. Sovereign Image Codecs Subsystem — PNG, JPEG, WebP & QOI (`nisaba::image`)
- **Zero-Dependency Deflate Compression Subsystem (`nisaba::image::deflate`)**:
  - Full RFC 1950 (Zlib wrapper) and RFC 1951 (Deflate stream) compliance.
  - Compile-time `constexpr` 256-entry CRC-32 (ISO 3309) and loop-unrolled Adler-32 accumulators.
  - Direct $O(1)$ primary Huffman table (9-bit lookup) for immediate single-cycle symbol resolution.
  - Sovereign LZ77 compressor with a 32 KB circular window (128 KB total memory footprint) that stays resident within CPU L2 cache, eliminating dynamic heap allocations.
  - Multi-level compression: Fast UI Compositor Mode (Level 1) for real-time window compositing and rapid screen captures, and Production Storage Mode (Level 6) for maximum compression.
- **Sovereign PNG Subsystem (`nisaba::image::png`)**:
  - Full standard chunk parser and generator: `IHDR`, `PLTE`, `tRNS`, `IDAT`, `IEND` with multi-chunk streaming assembly.
  - Comprehensive color models: RGBA8, RGB8, Grayscale (1, 2, 4, 8, 16-bit), Grayscale+Alpha, and Indexed / 8-bit Colormap (Palette) with full alpha channel transparency.
  - All 5 scanline unfilters: `None`, `Sub`, `Up`, `Average`, and `Paeth` with overflow-safe arithmetic.
  - Full Adam7 7-pass interlaced image decoding.
  - Adaptive scanline filter selection using Sum of Absolute Differences (SAD).
  - Direct row-write optimization for RGBA8 and RGB8, bypassing per-pixel function call and branching overhead.
- **Sovereign JPEG Subsystem (`nisaba::image::jpeg`)**:
  - Complete JFIF parser (`SOI`, `SOF0`, `DQT`, `DHT`, `SOS`, `DRI`, `RST0-7`, `APP0`, `EOI`) with automatic byte-stuffing handling (`0xFF 0x00`).
  - Chrominance subsampling support for 4:4:4, 4:2:2, 4:2:0, and Grayscale.
  - Exact Separable 2D Forward and Inverse DCT ($D = T \cdot in \cdot T^T$ and $X = T^T \cdot D \cdot T$) achieving studio master reference fidelity (**PSNR = 44.95 dB**).
  - High-speed 9-bit direct Huffman lookup combined with ISO 10918-1 bounded canonical prefix search.
  - 16-bit fixed-point ITU-R BT.601 integer color transformation ($Y'CbCr \leftrightarrow RGB$).
  - Reciprocal quantization multiplier tables eliminating run-time integer divisions.
  - Single-instruction bit-width category evaluation via C++20 `std::bit_width` (compiles to hardware `BSR`/`LZCNT`).
  - Baseline sequential encoder with configurable quality ($1 \dots 100$).
- **OS-Grade Security & Fuzz-Proofing**:
  - Dimension limits ($W, H \le 32768$) preventing integer overflow exploits during buffer allocations.
  - Component count ($1 \dots 4$) and sampling factor ($H, V \le 4$) validation preventing infinite loops.
  - Zero-division protection against empty or malformed quantization tables.
  - Safe progressive JPEG (`SOF2`) handshake returning clean error codes without process crashes.
- **Sovereign WebP Subsystem (`nisaba::image::webp`)**:
  - Full standard RIFF/WEBP container chunk parser and generator supporting `VP8L`, `VP8X`, `ANIM`, `ANMF`, `ALPH`, `ICCP`, `EXIF`, and `XMP` chunks with zero third-party dependencies (100% free of `libwebp`).
  - **Sovereign Lossless WebP (VP8L)**:
    - Pure Modern C++20 VP8L decoder and encoder.
    - Full decoding of spatial prediction transforms, color transforms, subtract green transforms, and color indexing (palette) transforms.
    - Bitwise canonical Huffman stream decoder and high-efficiency lossless bitstream compressor.
  - **Sovereign Animated WebP Engine (`WebPAnimation`, `WebPAnimationFrame`)**:
    - Multi-frame timeline parser evaluating sequence timing (`duration_ms`), sub-rectangle offsets $(x, y)$, and dimensions $(w, h)$ across sequential `ANMF` chunks.
    - Full canvas frame compositing engine supporting alpha blending (`WebPBlendMethod::AlphaBlend`) and overwrite (`WebPBlendMethod::Overwrite`).
    - Frame disposal handling: background clearing (`WebPDisposeMethod::Background`) and canvas preservation (`WebPDisposeMethod::None`).
    - Dedicated APIs: `decode_webp_animation`, `decode_webp_animation_file`, `is_animated_webp`, with automatic fallback to the first composited frame in still image decoders (`decode_webp`).
  - **Non-Destructive Header Probing**:
    - Direct $O(1)$ header inspection (`probe_webp`, `probe_image_file`) querying canvas width, height, alpha channel presence, and bit depth without decompressing image pixel payloads.
  - **Direct `Pixmap` Integration**:
    - Seamless loading and saving via `Pixmap::load_webp(path)`, `pixmap.save_webp(path)`, and automatic sniffing of `RIFF....WEBP` magic bytes in `Pixmap::load_file()`.
- **Sovereign QOI Subsystem (`nisaba::image::qoi`)**:
  - Full Quite OK Image Format specification implementation in pure C++20.
  - All 6 core operations supported: `QOI_OP_RUN`, `QOI_OP_INDEX` (64-entry running color cache), `QOI_OP_DIFF`, `QOI_OP_LUMA`, `QOI_OP_RGB`, `QOI_OP_RGBA`.
  - Ultra-fast lossless performance: encodes and decodes in a fraction of the time required by traditional compression codecs on HD surfaces.
  - Direct integration into `Pixmap::load_qoi()`, `pixmap.save_qoi()`, and auto-detection via `qoif` magic bytes.
- **Unified I/O & `Pixmap` Auto-Sniffing (`nisaba::image::image_io`)**:
  - `Pixmap::load_file(path)`: automatic magic-byte sniffing (PNG, JPEG, WebP, QOI, BMP, PPM).
  - `Pixmap::load_from_memory(data)`: in-memory decoding.
  - `pixmap.save_png(path, level)`, `pixmap.save_jpeg(path, quality)`, `pixmap.save_webp(path)`, and `pixmap.save_qoi(path)`.
  - `pixmap.save_image(path)`: auto-format detection by file extension (.png, .jpg, .jpeg, .webp, .qoi, .bmp, .ppm).

### 12. Sovereign Adaptive Flexbox & UI Layout Engine (`nisaba-layout` / `nisaba::layout`)
- **Deterministic 7-Stage Layout Pipeline (`LayoutPipeline`)**:
  - **Stage 1 (Geometric Context & Direction Resolution)**: Resolves bidirectional writing mode (`Direction::LTR` / `RTL`), available container content bounds, and physical padding/border insets.
  - **Stage 2 (Intrinsic Item Measurement & Hypothetical Main Sizes)**: Calculates hypothetical item lengths across main axis with support for leaf measurement callbacks (`MeasureCallback`), fixed/percent units, and aspect ratio preservation.
  - **Stage 3 (Line Segmentation)**: Partitions items into flex lines for multi-line configurations (`Wrap::Wrap`, `Wrap::WrapReverse`) incorporating discrete cross and main axis gaps.
  - **Stage 4 (Flexible Free-Space Distribution)**: Exact W3C-compliant resolution distributing positive free space via `flex-grow` and resolving negative overflow via `flex-shrink` * flex-basis product weighting, constrained by min/max dimensions.
  - **Stage 5 (Cross-Axis Sizing & Stretch)**: Resolves line heights/widths, stretches unconstrained children (`Align::Stretch`), and computes baseline offsets (`Align::Baseline`).
  - **Stage 6 (Alignment & Positioning)**: Distributes items along main axis (`Justify::FlexStart`, `Center`, `FlexEnd`, `SpaceBetween`, `SpaceAround`, `SpaceEvenly`), applies cross-axis alignment (`align-items`, `align-self`, `align-content`), and executes automatic margin centering (`margin: auto`).
  - **Stage 7 (Absolute Positioning & Z-Stacking)**: Positions out-of-flow children (`PositionType::Absolute`) based on 4-corner insets (`top`, `left`, `right`, `bottom`), percentages, explicit dimensions, and layered Z-order stacks without affecting sibling flow.
- **Cache-Friendly Flat Geometry Representation**:
  - Memory-aligned flat C++20 structures (`StyleLength`, `StyleSizeLength`, 8 bytes each) eliminating pointer indirection and heap churn.
  - Bidirectional parent-child tree linking (`Node`) with safe, zero-leak stack and heap lifetime management.
- **Complete W3C CSS Box Model Compliance**:
  - Full support for `BoxSizing::BorderBox` and `BoxSizing::ContentBox`.
  - Directional margins, borders, paddings, and discrete gap control (`Gutter::Row`, `Gutter::Column`, `Gutter::All`).
  - Dimensional constraints: min/max dimensions, auto dimensions, percentage scaling, and explicit aspect ratios.
- **Subpixel Quantization & Integral Alignment (`quantizeLayoutMetrics`)**:
  - Quantizes physical positions and dimensions to device pixels via rounded offsets while tracking fractional subpixels, preventing visual blurring or raster seam artifacts on high-DPI displays.
- **Direct Canvas Integration**:
  - Resolves layouts and maps computed bounds (`layout.position()`, `layout.dimension()`) directly into `nisaba::Canvas` draw calls in a single deterministic pass.

### 13. Sovereign Native Platform & Windowing Subsystem (`nisaba::backend_os`)
- **Strictly Optional & Non-Intrusive Architecture**:
  - Located in the dedicated top-level directory `backend_os/` and compiled only when explicitly enabled via `-Denable_backend_os=true` (disabled by default).
  - Preserves Nisaba's core identity as a lightweight, independent 2D graphics and layout engine—**not** an opinionated, monolithic application framework. It is leveraged strictly when native desktop or embedded display windows and hardware event loops are required.
- **Pure Native Cross-Platform OS Backends (100% Zero GLFW / SDL Dependencies)**:
  - **Linux Wayland**: Pure XDG Shell toplevels (`xdg_toplevel`), desktop layer surfaces (`zwlr_layer_shell_v1`), popups (`xdg_popup`), and decoration negotiation (`zxdg_decoration_manager_v1`).
  - **Linux X11**: Pure Xlib/XCB + EGL/GLX with Client-Side Decorations (CSD) and window manager state synchronization (`_NET_WM_*`).
  - **Linux DRM/KMS**: Bare-metal direct scanout via `libdrm`, `gbm`, and `libinput` for automotive infotainment, smart displays, and embedded robotics without an X11 or Wayland server.
  - **Windows Win32**: Win32 API with dark mode titlebars, high-DPI awareness, DWM blur-behind, and seamless message loop.
  - **Android**: Direct `ANativeActivity` and `ANativeWindow` glue with touch event translation and lifecycle management.
  - **WebAssembly**: HTML5 Canvas and Emscripten event pipeline with WebGL context binding.
- **Unified Application Lifecycle & Event Loop (`nisaba::backend_os::App`)**:
  - Encapsulates OS event pumps, dispatch loops, and multi-window orchestration behind an intuitive, RAII-governed `App` interface.
  - Real-time frame statistics and profiling (`FrameStats`): smoothed instantaneous FPS, exact frame duration in milliseconds, and 95th-percentile (P95) latency tracking over the last 120 frames.
  - Dual execution modes: continuous automated render loop (`App::run()`) or discrete manual frame stepping (`App::stepFrame()`) for embedded controllers.
  - Automatic VSync synchronization and configurable target frame rates (`target_fps`).
- **Native Windowing & Glassmorphism (`backend_os::Window`)**:
  - Full support for 32-bit ARGB compositing with true alpha transparency (`WindowConfig::transparent`).
  - Native compositor blur-behind hints (`WindowConfig::blur` and `Window::setBlurBehind`).
  - Client-Side Decorations (CSD) with native interactive dragging (`Window::beginMove`), edge resizing (`Window::beginResize`), maximize, minimize, and fullscreen toggles.
- **Sovereign Native Popups & Overlays (`backend_os::NativePopup` & `App::addPopup`)**:
  - Lightweight secondary surfaces for context menus, dropdowns, tooltips, and floating palettes.
  - Built-in automatic dismissal on outside clicks, anchor coordinate tracking, and desktop layer shell placement.
- **Zero-Allocation Type-Safe Signals (`backend_os::Signal`)**:
  - Decoupled, header-only signal-slot dispatching for mouse movement, clicks, scrolls, keyboard inputs, window resizes, and close requests without heavy callback dependencies.
- **Comprehensive Desktop Subsystems**:
  - **Clipboard**: Full UTF-8 text and multi-format MIME data exchange (`getClipboardText`, `setClipboardText`, `getClipboardDataForMime`).
  - **Drag and Drop**: Native OS drag initiation and payload handling (`startDrag`, `cancelDrag`).
  - **Display Outputs & High-DPI**: Monitor enumeration, physical geometry, refresh rates, and subpixel scale factor queries (`backend_os::Output`).
  - **Safe Area Insets**: Query display notch, status bar, and gesture navigation bar boundaries across mobile and desktop screens.
  - **Cross-Platform Permissions**: Dynamic permission requests for camera, audio, and storage on embedded and mobile targets (`backend_os::Permissions`).
  - **Radical Independence**: 100% free of GLFW, SDL, or external framework bloat.

### 14. Sovereign VAXP Super Performance Render (VSPR) Subsystem (`nisaba::vspr`)
- **Radical Sub-Element & Pixel-Span Precision**:
  - Blends high-performance 2D micro-tiles with contiguous 1D horizontal scanline spans (`Span`), achieving fine-grained dirty tracking at sub-component and pixel granularity.
  - Zero heap churn during steady-state invalidation passes: utilizes dense flat 64-bit bitmasks for $O(1)$ spatial tile marking and queries.
- **Micro-Tile Grid (`MicroTileGrid<TileSize = 16>`)**:
  - Decomposes the framebuffer into uniform $16 \times 16$ pixel micro-tiles.
  - Fast bitwise arithmetic: translates arbitrary bounding boxes into grid tile coordinates using bit shifts and 64-bit bitmask operations.
- **Horizontal Span Coalescing (`TiledSpanTracker<16>`)**:
  - Scans dirty micro-tiles to generate contiguous horizontal scanline spans (`Span { y, x0, x1 }`) and coalesced rectangles (`DamageRegion` / `std::vector<ScreenIntRect>`).
  - Completely eliminates rasterizer traversal, shading, and blending passes across unchanged static UI areas.
- **Multi-Buffer History & Page-Flipping (`BufferAgeTracker`)**:
  - Solves the multi-buffering damage problem across double-buffered and triple-buffered display pipelines (Wayland, DRM/KMS, X11, EGL/Vulkan).
  - Tracks per-buffer damage history using `buffer_age`: automatically unions historical damage from previous frames so that newly swapped backbuffers receive complete, artifact-free updates with zero ghosting or tearing.
- **Direct Canvas & Native Wayland Compositor Integration**:
  - Restricts software rendering passes directly via `Canvas::clip_rect(const ScreenIntRect&)`.
  - Propagates damage rectangles to the display server via `backend_os::Window::setDamage()` and Wayland `wl_surface_damage_buffer()`, slashing compositor blit overhead and GPU memory bus bandwidth.
- **Empirical Benchmark Validation (Full Repaint vs. Damage Tracking)**:
  - Validated across interactive glassmorphic dashboard scenarios (`benchmarks/benchmark_damage_tracking.cpp`):
    - **Micro-Pulse (Blinking cursor)**: Massive multi-order speedup, slashing frame invalidation latency and saving over 99% of rasterization overhead.
    - **Sub-Component (Dynamic widgets & gauges)**: Drastic reduction in rasterization passes by restricting work exclusively to localized dirty regions.
    - **Multi-Zone (Concurrent micro-updates)**: Orders of magnitude faster than full-frame repaints, keeping CPU/GPU utilization minimal.
- **Ahead-Of-Time (AOT) UI Scene Compilation (`AotSceneCompiler` & `CompiledUiSurface`)**:
  - Compiles complex visual layouts, static chromes, and interactive component state representations ahead of time into unified, hardware-ready surfaces.
  - Dynamically routes live application data through isolated punch-through slots, eliminating continuous full-frame vector rasterization cycles.
  - Dramatically improves rendering performance, slashes frame processing overhead to near zero, and ensures instantaneous UI responsiveness across complex viewports.
- **Hardware-Accelerated Primitive Instancing (`PrimitiveInstancer`)**:
  - Consolidates and renders vast collections of repetitive geometric forms, controls, and vector primitives in batched operations.
  - Substantially eliminates driver call overhead and CPU-to-GPU dispatch bottlenecks, unlocking massive performance gains in element-dense interfaces.

### 15. Sovereign Lottie Vector Animation Engine (`nisaba::lottie` & `nisaba::json`)
- **100% Technological Sovereignty & Zero Dependencies**:
  - Sovereign C++20 Bodymovin/Lottie JSON parser and real-time vector animation playback engine.
- **Embedded Sovereign High-Speed JSON Parser (`nisaba::json`)**:
  - Zero-allocation recursive-descent DOM parser with full standard UTF-8 parsing, IEEE-754 numeric conversion, and strict validation (`JsonValue`).
- **Complete Lottie Vector Primitives & Layer Tree**:
  - **Layers**: Precomp, Solid, Image, Null, Shape, and Text layers with opacity, nesting, hierarchical parenting, and in/out frame bounds.
  - **Vector Shapes**: Groups (`gr`), Bezier Paths (`sh`), Ellipses (`el`), Rounded Rectangles (`rc`), Solid Fills (`fl`), Vector Strokes (`st` with line caps, joins, and miter limits), and Transforms (`tr`).
  - **Dynamic Modifiers**: Trim Paths (`tm`) supporting simultaneous and individual modes with animated start, end, and offset properties for elastic vector spinners and line reveals.
- **Parametric Properties & Multidimensional Easing Curves (`Property<T>`)**:
  - Multi-keyframe animation tracks with cubic Bezier easing curves (`in_tangent`, `out_tangent`) for elastic, bounce, and deceleration dynamics.
  - Continuous parametric interpolation across scalar floats, 2D vectors (`Point`), and 4D colors (`Color`).
- **Polymorphic CPU & Hardware GPU Vector Canvas (`ICanvas`)**:
  - Unified drawing abstraction enabling identical vector animation code to execute across software rasterizers (`Canvas`) and direct hardware GPU pipelines (`GpuCanvas`).
  - Hardware GPU vector rendering processes complex vector animations with negligible frame times, achieving smooth, fluid real-time framerates.
- **Interactive Animation Player Lifecycle (`Player`)**:
  - Real-time playback controller supporting `play()`, `pause()`, `seek_frame()`, `seek_progress()`, speed scaling (`0.25x` to `4.0x`), and continuous looping.

### 16. Sovereign Markdown Document Layout & Rendering Subsystem (`nisaba::markdown`)
- **100% Sovereign Architecture & Zero Dependencies**:
  - Native C++20 document parsing, layout solving, and visual presentation engine built strictly according to CommonMark and GitHub Flavored Markdown (GFM) standards.
  - Zero external markup libraries, zero third-party bindings, and zero runtime overhead.
- **Abstract Syntax Tree (AST) Document Representation (`MarkdownNode`)**:
  - Full structural node hierarchy: `Document`, `Heading` (levels 1 through 6), `Paragraph`, `Blockquote`, `List` (ordered and unordered with bullet style resolution), `ListItem`, `CodeBlock` (indented and fenced with language identifiers), `ThematicBreak` (horizontal rules), and `Table`.
  - Rich inline formatting spans: `Text`, `Emphasis` (italic), `Strong` (bold), `Strikethrough`, `Code` (inline monospace), `Link` (interactive URI targets), `Image` (embedded graphics), and `AutoLink`.
  - Comprehensive GFM Tables: header identification, per-column text alignment (`Left`, `Center`, `Right`), and multi-row data cell grid layout.
- **Extensible Theming & Design System (`MarkdownTheme`)**:
  - Fully configurable typography hierarchy: heading font sizes, line heights, paragraph spacing, and text margins.
  - Granular color styling for dark and light surfaces: background colors, body text, link highlights, code syntax background, and quote borders.
  - Specialized callout and blockquote styling: customizable border thickness, left padding, and tinted backdrop surfaces.
  - Table styling: cell padding, header fill, alternating row striping, and grid border strokes.
- **Multi-Page Layout & Multilingual Typography Integration (`MarkdownRenderer`)**:
  - Direct integration with Nisaba's sovereign typography engine (`FontSystem`, `Buffer`) for complex bidirectional Arabic shaping, CJK character support, and subpixel glyph positioning.
  - Automated page breaking and vertical pagination with configurable margins (`top`, `bottom`, `left`, `right`).
  - Running headers, footers, page numbering, and visual separators.
- **Sovereign Vector PDF Export Pipeline (`MarkdownPdfExporter`)**:
  - Compiles parsed Markdown AST directly into standard vector PDF documents.
  - Preserves true vector outlines for typography, vector geometric primitives for table borders, rules, and callout containers, and embeds high-resolution raster images without fidelity loss.

### 17. Sovereign Full-Specification PDF Document Engine (`nisaba::pdf`)
- **Full ISO 32000 Standard Architecture & Zero Dependencies**:
  - Sovereign C++20 PDF reading, parsing, rendering, and vector generation engine built from the ground up without external libraries.
  - Strict memory safety, bounded allocations, and resilience against malformed streams.
- **Lexer & Recursive Object Model (`PdfParser` & `PdfValue`)**:
  - Recursive-descent parser supporting all PDF primitive and composite types: Booleans, Integers, Reals, Literal Strings with escape decoding, Hexadecimal Strings, Names (`/Name`), Arrays, Dictionaries, Indirect Object References (`R`), and binary Streams.
  - Deferred and cached object resolution (`PdfParser::resolve`) with cycle detection preventing circular reference exploits.
- **Comprehensive Typography & Font Subsystem (`PdfCMap`, `CIDFontType2`, `FontSystem`)**:
  - **`ToUnicode` CMap Stream Parser**: Decodes embedded character maps (`beginbfchar`/`endbfchar`, `beginbfrange`/`endbfrange`), resolving character codes directly to Unicode codepoints including multi-byte surrogate pairs.
  - **Type 0 & Composite Fonts**: Handles `/CIDFontType2` and `Identity-H` two-byte character sequences with exact glyph advance calculation and font metrics.
  - **Embedded Font Extraction**: Automatically extracts `/FontFile2` (TrueType) and `/FontFile3` streams from `/FontDescriptor` dictionaries and injects them directly into Nisaba's native vector `FontSystem`.
  - **Full Text Operator Support**: State matrix operations (`BT`, `ET`, `Tf`, `Tm`, `Td`, `TD`, `T*`) and string display operators (`Tj`, `TJ` with inter-character kerning arrays, `'`, `"`).
- **Modern Object Architecture & Cross-Reference Streams**:
  - **Cross-Reference Streams (`/Type /XRef`)**: Reads compressed binary cross-reference streams alongside traditional text xref tables, resolving Type 0 (free), Type 1 (uncompressed), and Type 2 (compressed within object streams) entries.
  - **Object Streams (`/Type /ObjStm`)**: Parses consolidated object stream containers (`/N` object count, `/First` byte offset) with offset indexing for instant object lookups.
  - **Predictor Decompression**: Implements all standard predictor functions (PNG Predictors 10-15: Sub, Up, Average, Paeth; and TIFF Predictor 2 horizontal difference) across multi-channel streams.
  - **Incremental Update Chains**: Traverses document revision histories via `/Prev` xref pointers with cycle detection.
- **Advanced Vector Graphics, Shadings & Patterns**:
  - **Vector Graphics Pipeline**: Full execution of path construction (`m`, `l`, `c`, `v`, `y`, `h`, `re`), clipping (`W`, `W*`), and painting operators (`S`, `s`, `f`, `F`, `f*`, `B`, `B*`, `b`, `b*`, `n`).
  - **Gradients & Shadings (Types 2 & 3)**: Axial (linear) and Radial shadings with domain extension (`/Extend [true true]`) and multi-stage stitching and exponential transition functions (Types 0, 2, 3).
  - **Freeform & Coons Mesh Shadings (Types 4, 5, 6, 7)**: Gouraud-shaded triangle meshes and bicubic Coons patch meshes utilizing Nisaba's sovereign `Mesh` and `CoonsPatch` engines.
  - **Tiling & Shading Patterns (Types 1 & 2)**: Re-entrant stream cell rendering with bilinear tiling repetition (`SpreadMode::Repeat`) and pattern coordinate transformation matrices.
- **Transparency, Blend Modes & Color Spaces**:
  - **All 16 Blend Modes**: Direct hardware and software compositing for `Normal`, `Multiply`, `Screen`, `Overlay`, `Darken`, `Lighten`, `ColorDodge`, `ColorBurn`, `HardLight`, `SoftLight`, `Difference`, `Exclusion`, `Hue`, `Saturation`, `Color`, `Luminosity`.
  - **Transparency Groups (`/Group`)**: Isolated and non-isolated offscreen compositing surfaces with alpha blending and opacity factors (`ca`, `CA`).
  - **Soft Masks (`/SMask`)**: Luminosity masks ($Y = 0.2126 R + 0.7152 G + 0.0722 B$) and direct alpha masks.
  - **Advanced Color Spaces**: Full support for DeviceRGB, DeviceCMYK, DeviceGray, CalRGB, CalGray, Lab, ICCBased, Indexed (1, 2, 4, 8 bits with lookup palette decoding), Separation, and DeviceN with tint transform functions.
  - **Image Stencils (`ImageMask`)**: 1-bit monochrome stencil masking rendered in current fill paint.
- **Sovereign Codecs & Media Decoders (`nisaba::pdf::codec`)**:
  - `FlateDecode` (Zlib/Deflate with automatic header validation).
  - `CCITTFaxDecode` (Group 3 and Group 4 2D facsimile decompression with 2D Pass, Horizontal, and Vertical modes for archival and scanned documents).
  - `ASCII85Decode` (standard 5-tuple base-85 with 'z' run handling), `ASCIIHexDecode`, and `RunLengthDecode`.
- **Sovereign Standard Security Handler & Cryptography (`nisaba::pdf::crypto`)**:
  - Native standard password authentication (ISO 32000-1 Algorithms 2, 4, and 7) for user and owner encryption keys.
  - Pure C++20 cryptographic primitives: MD5 (RFC 1321) and SHA-256 (FIPS 180-4).
  - Stream and string ciphers: RC4 (40-bit and 128-bit) with per-object key derivation (Algorithm 1), AES-128 (CBC mode), and AES-256 (CBC mode) with PKCS#7 unpadding.
  - Automatic transparent decryption of strings and streams during object resolution.
- **Hardware-Accelerated GPU Rendering & Navigation (`nisaba::pdf::navigation`)**:
  - Direct vector rendering onto hardware GPU pipelines (`PdfReader::render_page_gpu`) via `GpuCanvas` and Vulkan/OpenGL ES, achieving smooth, fluid real-time continuous viewport scrolling.
  - Hierarchical bookmarks and table of contents (`/Outlines`) with multi-byte text decoding (PDFDocEncoding, UTF-16BE, UTF-8 BOM), styles (bold/italic), and RGB colors.
  - Interactive hyperlink annotations (`/Annots` with `/Link`) supporting internal page jumps (`/GoTo`), external web links (`/URI`), external files (`/GoToR`), and named destinations with automatic page resolution (`resolve_destination_page`).
- **Sovereign Vector PDF Generation (`PdfWriter`)**:
  - Programmatic creation of compact, compliant vector PDF files with embedded TrueType fonts, vector paths, images, and cross-reference tables.

### 18. Sovereign Geometric Path Boolean Operations (`nisaba::path_ops`)
- **Planar Graph & Sweep-Line Decomposition**:
  - Exact planar graph representation decomposing intersecting vector paths into a non-overlapping planar segment arrangement.
  - Sweep-line intersection solver identifying all line-line, line-curve, and curve-curve intersection vertices with exact parameter splitting.
- **Topological Winding Classification**:
  - Evaluates segment midpoints against input paths utilizing non-zero and even-odd winding accumulation to determine topological containment ($in_A, in_B$).
- **Complete Boolean Algebra (`PathOp`)**:
  - `PathOp::Union`: Computes the geometric union ($A \cup B$).
  - `PathOp::Difference`: Computes the geometric difference ($A \setminus B$).
  - `PathOp::Intersect`: Computes the mutual intersection ($A \cap B$).
  - `PathOp::Xor`: Computes the symmetric difference ($A \oplus B$).
- **Cycle Extraction & Directed Contours**:
  - Extracts closed clockwise and counter-clockwise loop cycles using angle-ordered edge incidence tables, outputting valid, wound `nisaba::Path` objects.
  - Seamlessly accessible through functional APIs (`nisaba::path_ops::compute_op`) and object convenience methods (`Path::unite`, `Path::difference`, `Path::intersect`, `Path::xor_op`).

### 19. Sovereign Path Outlining & Clean Stroke-to-Fill Geometry (`nisaba::stroker`)
- **Exact Vector Contour Expansion (`Path::stroke_to_fill`)**:
  - Expands stroked vector geometries into closed fillable polygon boundaries based on configurable `StrokeStyle` parameters.
  - Full support for stroke caps (`LineCap::Butt`, `Round`, `Square`), joins (`LineJoin::Miter`, `Round`, `Bevel`), and miter cut-off limits.
  - Generates parallel offset curves and exact circular arc caps/joins for both open paths and closed subpaths.
- **Topological Self-Intersection Resolution (`Path::stroke_to_fill_clean`)**:
  - Automatically pipes generated stroke boundary polygons through the planar `PathOps` Union solver.
  - Resolves overlapping ribbon folds, self-intersecting loops, and tight-corner miter self-intersections into single, continuous, non-overlapping vector boundaries with positive winding.

### 20. Multi-Format Framebuffer & Embedded Color Architectures (`PixelFormat`)
- **Native Color Format Hierarchy**:
  - `PixelFormat::RGBA8888`: Standard 32-bit linear/sRGB 4-channel color representation.
  - `PixelFormat::BGRA8888`: Native 32-bit hardware scanout format for display servers and compositors.
  - `PixelFormat::RGB565`: High-performance 16-bit packed format (5 bits Red, 6 bits Green, 5 bits Blue) reducing memory bandwidth and buffer size by 50% for microcontrollers and memory-restricted LCD displays.
  - `PixelFormat::Alpha8` & `PixelFormat::Gray8`: Single-channel 8-bit surfaces for alpha masks, font glyph caches, and monochrome displays (75% memory reduction).
- **Zero-Allocation Dedicated Span Blitters**:
  - Direct bitwise assembly and span operations: `blit_row_rgb565`, `blit_row_alpha8`, `blit_row_gray8`, and `blit_row_bgra8888`.
  - Fast bit-shifting channel unpack and repack arithmetic ($R_5 \leftrightarrow R_8$, $G_6 \leftrightarrow G_8$, $B_5 \leftrightarrow B_8$) with exact rounding and saturation.
  - Unified memory lifecycle: allocate formatted surfaces via `Pixmap::create(w, h, format)` or wrap external display hardware buffers via `PixmapMut::from_raw_parts(ptr, w, h, pitch, format)`.

### 21. Linear sRGB Blending & Photometric Alpha Compositing (`nisaba::color`)
- **Physical Light Linearity (IEC 61966-2-1)**:
  - Eliminates gamma distortion, dark boundary fringes, and optical edge dimming inherent in non-linear color blending by performing alpha compositing in physical linear light space.
- **Ultra-Compact Dual 12-Bit Lookup Tables (`ColorSpaceLUT`)**:
  - `srgb_to_linear_u12`: Maps 8-bit non-linear sRGB values to 12-bit linear photometric intensities.
  - `linear_u12_to_srgb`: Maps 12-bit linear values back to 8-bit non-linear sRGB with rounded quantization.
  - Total memory footprint is strictly **4.5 KB**, guaranteeing permanent residency inside the CPU L1 data cache without eviction.
- **Selective Runtime Control**:
  - Toggled per-draw via `Paint::set_linear_blending(bool)` / `paint.linear_blending`.
  - Integrated directly across solid colors, gradients, and custom shaders within the scanline blitter pipeline.

### 22. Advanced Shaders & Multi-Shader Compositing (`nisaba::shaders`)
- **Two-Point Conical Gradients (`TwoPointConicalGradient`)**:
  - Analytical evaluation of conical light cones and spotlight effects by solving the quadratic equation $A t^2 + 2 B t + C = 0$ across arbitrary focal centers and radii $(c_1, r_1)$ and $(c_2, r_2)$.
  - Supports extended focal radii, fully inverted conical cones, arbitrary color stops, and spread modes (`Pad`, `Reflect`, `Repeat`).
- **Compose Shaders (`ComposeShader` / `BlendShader`)**:
  - Dynamic compositing of two independent source shaders ($A$ and $B$) using any of Nisaba's 29 Porter-Duff or advanced blend modes.
  - Enables complex procedural materials, textured gradient masks, lighting modulation, and multi-pass shader composition in a single rasterization pass.

### 23. Sovereign Wayland GPU Compositor & Vector Pipeline Fusion (`nisaba::compositor`)
- **Dual-Path Zero-Copy Hardware Architecture**:
  - Direct hardware DMA-BUF and EGLImage surface binding (`GpuTexture::from_native_handle` / `wl_buffer` import) delivering near-instantaneous zero-copy blit latency for client window contents without CPU staging memory copies.
  - Full bidirectional vector engine unification: wraps native compositor render target FBOs directly as sovereign `GpuSurface` instances (`GpuSurface::from_render_target`), granting compositor windows, titlebars, docks, and overlays direct access to `GpuCanvas`.
- **Strict RAII GPU State Isolation (`GlStateGuard`)**:
  - Full-spectrum state capture and restoration preserving active GL program, Vertex Array Object (VAO), Array/Element Array buffers (VBO/EBO), framebuffer bindings (FBO read/draw), blend equations and functions, scissor box, viewport dimensions, depth/stencil tests, and texture unit bindings.
  - Guarantees zero graphical artifacts, state corruption, or pipeline stalls when transitioning between raw Wayland surface blitting and complex vector UI drawing passes.
- **High-Level Compositor Decorator Primitives (`CompositorCanvas`)**:
  - Native window frame decoration with rounded corners, active/inactive accent borders, and subpixel window title typography (`drawWindowDecoration`).
  - Modern desktop panels and floating dock bars with real-time Dual-Kawase frosted glass backdrop blur and specular edge borders (`drawDesktopPanel`).
  - Procedural runtime shader injection across arbitrary compositor windows (`applyWindowShader`).
  - Direct vector icon and glyph rendering (`drawSvg`, `drawText`) using built-in typography and glyph caching.
- **O(1) Damage-Aware Spatial Culling**:
  - Integrates with Nisaba's VSPR damage tracker and Wayland `wl_surface_damage_buffer`: static windows and occluded decorations are culled on CPU before submitting draw commands to the GPU.
- **Zero Steady-State Heap Allocations**:
  - Reusable static scratch vertex buffers and persistent descriptor tables sustain ultra-high real-time framerate throughput with zero dynamic heap allocations in steady-state loop.

### 24. Sovereign 3D Rigid Body Physics Pipeline Subsystem (`nisaba::physics`)
- **Independent Sovereign C++20 Architecture**:
  - Embedded-first, zero-dependency 3D rigid body dynamics subsystem built entirely from first principles for high-performance simulation.
  - 100% self-contained standard ISO C++20 with native 128-bit/256-bit SIMD intrinsics (SSE2/AVX2/AVX-512 and ARM NEON).
  - Strict **0-byte steady-state dynamic heap allocation** during simulation ticks via pre-allocated scratch arenas and contiguous body slots.
- **Cache-Optimized Rigid Body & Kinematics Representation**:
  - **`Body`**: Exactly 128 bytes (2 cache lines, 16-byte aligned). Splits immovable static geometry from dynamic bodies to conserve 80% memory footprint.
  - **`MotionProperties`**: Exactly 128 bytes (2 cache lines). Houses linear/angular velocities, inverted mass, gyroscopic momentum, and sleep tracking metrics.
  - 16-byte aligned mathematical foundation: `Vec3`, `Quat`, `Mat44`, `AABB`, `Transform3D`, and analytical inertia tensors.
- **SIMD 4-Way Dynamic AABB Tree (`BroadPhase`)**:
  - 4-way Bounding Volume Hierarchy (`QuadTree` BVH) testing 4 child AABBs simultaneously via single 128-bit SIMD instructions.
  - Speculative bounding box fattening based on velocity displacement, collision layer filtering, and instantaneous raycast pruning.
- **Narrow-Phase Geometric Collision Dispatcher (`NarrowPhase`)**:
  - Analytical fast paths: `Sphere vs Sphere`, `Sphere vs Box`, `Sphere vs Plane`, `Box vs Box` (SAT).
  - General convex collision: 3D GJK (Gilbert-Johnson-Keerthi) Voronoi simplex solver and robust Expanding Polytope Algorithm (EPA) for general polyhedra (`CylinderShape`, `ConvexHullShape`).
  - Contact Manifold Quad-Point coplanar reduction: caps any contact patch to at most 4 maximal-area points, eliminating exponential solver complexity while maintaining physical stability.
- **Sequential Impulse Velocity & Position Solver (`ImpulseSolver`)**:
  - Projected Gauss-Seidel iterative velocity solver with Coulomb cone friction clamping ($|\lambda_t| \le \mu \lambda_n$) and non-penetration normal impulses ($\lambda_n \ge 0$).
  - Non-linear Baumgarte position error stabilization and restitution bouncing.
  - Persistent warm-starting transferring accumulated impulses across consecutive frames, guaranteeing instantaneous resting equilibrium without jittering or stacking collapse.
- **Simulation Islands & Energy Deactivation Sleeping (`IslandBuilder`)**:
  - Disjoint-Set Union (DSU) with path compression and rank optimization clustering interacting dynamic bodies into disjoint simulation graphs.
  - Prevents false merging across shared static boundaries (e.g. infinite floors).
  - Quiescent sleep deactivation based on kinetic energy thresholds, driving active island CPU utilization to zero for resting stacks while maintaining zero floating-point drift.
  - Automatic collision wake-up propagation and external impulse triggers.
- **Spatial Raycasting & Scene Queries**:
  - Full-scene raycasting traversing the 4-way SIMD BVH with exact analytical ray-shape intersection across Spheres, Boxes, Capsules, Planes, Cylinders, and Convex Hulls.
- **556 Passing Automated Unit & Integration Assertions**:
  - 100% verified across 8 dedicated test suites (`test_physics_math`, `test_physics_body`, `test_physics_contact`, `test_physics_solver`, `test_physics_broadphase`, `test_physics_gjk_epa`, `test_physics_island`, `test_physics_scene_integration`).

### 25. Sovereign 3D Real-Time PBR Rendering Pipeline Subsystem (`nisaba::render3d`)
- **Independent Sovereign C++20 Architecture**:
  - High-performance, data-oriented 3D PBR rendering engine written from scratch in modern C++20 with zero external dependencies.
  - Multi-platform matrix math with 16-byte SIMD acceleration, column-vector conventions, and Vulkan $[0, 1]$ depth normalization.
- **Camera & Spatial Frustum Culling (`nisaba::render3d::Camera3D`, `Frustum3D`, `OrbitCameraController`)**:
  - Perspective and Orthographic projection with Gribb-Hartmann plane normalization and zero-jitter lazy matrix caching.
  - Ultra-fast SIMD P-vertex / N-vertex AABB, bounding sphere, and point frustum culling.
  - Analytical unprojection from screen coordinates $(x, y)$ to world-space picking `Ray`.
  - Spherical orbit camera controller with continuous yaw, pitch clamping $[-\frac{\pi}{2} + \epsilon, \frac{\pi}{2} - \epsilon]$, distance zoom, and screen-space pan.
- **Cache-Aligned Geometry & Mesh Engine (`Vertex3D`, `VertexLayout`, `VertexBuffer3D`, `Mesh3D`, `MeshPrimitives`)**:
  - Exactly 64-byte CPU cache-line aligned `Vertex3D` (Pos, Norm, Tan4, UV0, UV1) with complete stride and offset reflections.
  - Dual host/GPU `VertexBuffer3D` and `IndexBuffer3D` supporting 16-bit and 32-bit index slicing.
  - Automated area-weighted smooth normal generation and Gram-Schmidt Lengyel tangent space orthogonalization.
  - Procedural primitive generators producing manifold geometry: Cube, UV Sphere, Subdivided Plane, Cylinder, and Torus.
- **PBR Shading, BRDF & Material System (`PbrMaterial`, `CookTorranceBRDF`, `Texture2D`)**:
  - Physically Based Rendering (PBR) metallic-roughness workflow matching the standard radiometric microfacet formulation.
  - Analytical Cook-Torrance BRDF evaluation: Trowbridge-Reitz GGX normal distribution ($D$), Smith-Schlick geometric shadowing ($G$), and Schlick Fresnel approximation ($F$).
  - Exactly 64-byte std140-compatible `PbrMaterialUniforms` for zero-overhead GPU uniform buffer uploading.
  - Sovereign `Texture2D` bilinear filtering, repeat wrapping, and direct integration with Nisaba's native image codecs.
- **Multi-Source Lighting & Cascaded Shadow Mapping (`Light3D`, `ShadowMap3D`)**:
  - Directional, Point, and Conical Spot lights with smooth windowed inverse-square distance attenuation and angular cone smoothstepping.
  - Analytical depth buffer with 4-tap Percentage-Closer Filtering (PCF) and normal-offset depth bias preventing shadow acne.
  - Stable Cascaded Shadow Mapping (CSM) projection solver with texel snapping to eliminate subpixel shadow shimmering under camera motion.
- **Data-Oriented Scene Graph & 64-Bit Sorted Renderer (`Scene3D`, `View3D`, `Renderable3D`, `DrawSortKey64`, `Renderer3D`)**:
  - 64-bit multi-field draw sort key `DrawSortKey64` [Pass 4b | Material 16b | Depth 24b | Primitive 20b].
  - Early-Z front-to-back sorting for opaque geometry to eliminate overdraw, and strict back-to-front sorting for transparent geometry.
  - SIMD frustum culling reducing draw calls and vertex submissions to visible geometry only.
  - Comprehensive per-frame telemetry: total objects, visible/culled objects, draw call count, and triangle metrics.
- **glTF 2.0 Ingestion & Physics 1:1 Synchronization Bridge (`GltfLoader`, `PhysicsRenderBridge`)**:
  - Native sovereign glTF 2.0 and binary GLB asset loader: parses RFC 8259 JSON, base64 data URIs, binary chunks, accessors, buffer views, PBR materials, meshes, and hierarchical node graphs.
  - Real-time `PhysicsRenderBridge` binding rigid bodies (`BodyID`) to visual renderables (`Renderable3D`) with automated primitive mesh synthesis matching physical shapes.
  - Sub-frame temporal interpolation (lerp/slerp) providing ultra-smooth rendering decoupled from fixed physics simulation timesteps.
- **555 Passing Automated Unit & Integration Assertions Across 6 Test Suites**:
  - 100% verified across `test_render3d_camera`, `test_render3d_geometry`, `test_render3d_material`, `test_render3d_lighting`, `test_render3d_renderer`, and `test_render3d_gltf_physics` (57/57 total engine suites passing).

### 26. Sovereign Hardware GPU 3D Rendering Pipeline Subsystem (`nisaba::render3d::gpu`)
- **Direct Sovereign GPU Layer Integration (`nisaba::gpu::GpuDevice` / OpenGL ES 3.2 & Desktop GL)**:
  - High-performance hardware rendering pipeline executing real-time 3D PBR and physical simulations directly on the GPU hardware.
  - Absolute zero external third-party graphics engine dependencies.
- **Hardware Shader Program & RAII Pipeline Isolation (`GlShaderProgram`, `GlStateGuard3D`)**:
  - Embedded modern GLSL PBR and shadow depth shader compilers with introspection, active uniform caching, and UBO block binding indices.
  - Zero-leak RAII pipeline isolation (`GlStateGuard3D`) capturing and restoring depth testing, cull face modes, blend equations, scissor rects, viewports, bound textures, and active framebuffers across frames.
- **Hardware Resource Cache & std140 UBO Blocks (`GpuResourceCache`)**:
  - Automated lifetime tracking and cache management for 64-byte `Vertex3D` geometry buffers (VAO, VBO, EBO), GPU PBR textures with automated mipmapping, and uniform buffer blocks.
  - Exactly 208-byte std140 `CameraUBO` (view, projection, viewProjection, cameraPosition) and 64-byte `MaterialUBO` for fast uniform streaming.
- **Hardware Cascaded Shadow Pass (`GpuShadowPass`)**:
  - Dedicated depth framebuffer rendering light-space shadow maps with hardware polygon offset (`glPolygonOffset`) and front-face culling to eliminate Peter-Panning and shadow acne.
- **64-Bit Sorted Forward PBR Hardware Backend (`GpuBackend3D`)**:
  - Multi-pass execution executing the Cascaded Shadow Depth pass followed by 64-bit sorted forward PBR rendering with Early-Z rejection for opaque geometry and back-to-front alpha blending.
- **2D/3D Zero-Copy Fusion Surface (`RenderSurface3D`)**:
  - Offscreen rendering surface managing color textures and depth/stencil renderbuffers with zero-copy texture sharing and readback to `nisaba::Pixmap` / `GpuCanvas` for 2D UI overlay.
- **Automated Verification Across 58 Engine Test Suites**:
  - 100% verified across dedicated GPU backend integration tests (`test_render3d_gpu_backend`) and full engine suites (58/58 test suites passing).

```
+-----------------------------------------------------------------------------------------------------------------------------+
|                               NISABA SOVEREIGN 3D REAL-TIME PBR & PHYSICS GPU SHOWCASE                                      |
+-----------------------------------------------------------------------------------------------------------------------------+
| [Hardware PBR] 1080p Real-time PBR scene with Cook-Torrance GGX, CSM hardware shadows, and 3D rigid body dynamics          |
+-----------------------------------------------------------------------------------------------------------------------------+
```
<p align="center">
  <img src="showcase/showcase_physics_world.png" alt="Nisaba Sovereign 3D PBR & Physics GPU Showcase" width="950">
</p>
<p align="center"><i>Real-time 1080p GPU hardware rendering of colliding 3D physical rigid bodies (cubes, golden sphere, emerald cylinders) with Cook-Torrance PBR lighting and cascaded shadow maps.</i></p>

---

## Core Engine Capabilities

- **Vector Geometry & Boolean Algebra**:
  - Quadratic, Cubic, and Conic Bézier curves; *de Casteljau* subdivision, root solving, extrema, cusps, and tight bounding boxes.
  - **Arc-Length Parameterization & Measurement (`nisaba::PathMeasure`)**: Sub-millimeter contour length calculation, analytical tangent and position evaluation, and de Casteljau subsegment carving for animated spinners, progress rings, and vector morphing.
  - **Geometric Path Operations (`nisaba::path_ops`)**: Exact planar sweep-line solver (`Union`, `Difference`, `Intersect`, `Xor`).
  - **Stroke-to-Fill Outlining (`nisaba::stroker`)**: Closed polygon contour generation with clean planar self-intersection union resolution.
- **Path Stroking & Dashing**: Flexible line caps (`Butt`, `Round`, `Square`), line joins (`Miter`, `MiterClip`, `Round`, `Bevel`), arbitrary dash patterns with phase offset.
- **Anti-Aliased Scanline Rasterization**: High-precision 256-level subpixel coverage anti-aliasing with non-zero (`Winding`) and `EvenOdd` fill rules.
- **Comprehensive Pixel Formats**: Native 32-bit `RGBA8888`, native display 32-bit `BGRA8888`, 16-bit packed `RGB565`, and 8-bit `Alpha8` / `Gray8` single-channel buffers with zero-allocation bitwise row blitters.
- **Photometric Color Pipeline**:
  - **Linear sRGB Blending**: IEC 61966-2-1 optical light linearity via compact 4.5 KB L1 cache lookup tables.
  - **4x5 Affine ColorMatrix**: 20-element color filtering for saturation, hue rotation, sepia, and channel mixing.
- **Shaders & Gradients**:
  - `LinearGradient` (multi-stop arbitrary direction)
  - `RadialGradient` (1-point circular and 2-point focal)
  - `TwoPointConicalGradient` (analytical quadratic conical spotlight solver)
  - `SweepGradient` (360-degree angular color wheel)
  - `ComposeShader` / `BlendShader` (multi-shader composition combining two shaders via any blend mode)
  - `RuntimeEffect` / `RuntimeShaderBuilder` (procedural GLSL 150 Core runtime shaders with uniform reflection and affine local coordinate mapping)
  - `Pattern` with high-performance **Bilinear Filtering**
  - Spread modes: `Pad`, `Reflect`, `Repeat`
- **29 Blend Modes**: All 12 Porter-Duff modes (`Clear`, `Source`, `Dest`, `SourceOver`, `DestOver`, `SourceIn`, `DestIn`, `SourceOut`, `DestOut`, `SourceAtop`, `DestAtop`, `Xor`) plus 17 advanced blend modes (`Plus`, `Modulate`, `Screen`, `Overlay`, `Darken`, `Lighten`, `ColorDodge`, `ColorBurn`, `HardLight`, `SoftLight`, `Difference`, `Exclusion`, `Multiply`, `Hue`, `Saturation`, `Color`, `Luminosity`).
- **Complete W3C SVG Support**: `<path>`, `<rect>`, `<circle>`, `<ellipse>`, `<line>`, `<polyline>`, `<polygon>`, `<defs>`, `<use>`, `<clipPath>`, `<mask>`, `<linearGradient>`, `<radialGradient>`, `<image>` (including RFC 2397 base64 inline data URIs), and affine `skewX`/`skewY` transforms.
- **Multilingual Typography & OpenType**: TrueType outlines (.ttf), PostScript CFF Type 2 Charstrings (.otf), GSUB ligatures and contextual substitutions, GPOS pair kerning, Unicode BiDi, and Arabic shaping.
- **VSPR Super Performance Acceleration**:
  - Ahead-of-Time (AOT) UI scene compilation with dynamic slot routing and interactive state caching.
  - Hardware-accelerated primitive instancing and batching for high-density geometric layouts.
  - Fine-grained micro-tile and scanline span damage tracking with multi-buffer history.
- **Sovereign Wayland GPU Compositor Subsystem (`nisaba::compositor`)**:
  - Zero-copy DMA-BUF/EGLImage window composition with near-instantaneous zero-copy hardware scanout latency.
  - Strict RAII OpenGL ES state protection (`GlStateGuard`) eliminating state leaks between compositor passes and vector UI drawing.
  - Full-pipeline vector fusion (`CompositorCanvas`) providing analytical SDF drop shadows, frosted glass panels with Dual-Kawase blur, procedural GLSL runtime shaders, and subpixel typography.
  - Damage-aware spatial culling ($O(1)$) with zero steady-state heap allocations per frame and ultra-high framerate throughput.
- **Sovereign 3D Rigid Body Physics Engine (`nisaba::physics`)**:
  - High-performance 3D rigid body dynamics pipeline built entirely from first principles with 100% technological sovereignty.
  - Exactly 128-byte cache-aligned `Body` (2 cache lines, 16-byte aligned) and `MotionProperties` with static/dynamic separation.
  - 4-way SIMD Dynamic AABB Tree (`QuadTree` BVH) evaluating 4 child nodes simultaneously in a single SIMD instruction.
  - Analytical narrowphase dispatchers and general convex GJK/EPA solvers for arbitrary polyhedra and shapes.
  - Gauss-Seidel sequential impulse solver with Coulomb friction clamping, Baumgarte stabilization, and persistent warm-starting.
  - Disjoint-Set Union (DSU) simulation islands with quiescent energy threshold sleep deactivation and collision wake-up propagation.
  - 556 passing unit & integration assertions (100% green across all 51 engine test suites).
- **Sovereign 3D Real-Time PBR Rendering Engine (`nisaba::render3d`)**:
  - High-performance, data-oriented 3D PBR rendering pipeline with 100% technological sovereignty.
  - 64-bit multi-field draw command sorting (`DrawSortKey64`) providing optimal Early-Z rejection for opaque geometry and back-to-front transparency ordering.
  - Analytical Cook-Torrance GGX microfacet BRDF with embedded GLSL 450 shaders and 64-byte std140 uniform blocks.
  - Cascaded Shadow Mapping (CSM) with 4-tap PCF filtering and subpixel texel snapping.
  - Native glTF 2.0 / GLB asset ingestion supporting embedded data URIs, binary buffers, and node hierarchies.
  - Real-time `PhysicsRenderBridge` synchronizing physical bodies to visual renderables with sub-frame temporal interpolation.
  - Hardware GPU execution backend (`nisaba::render3d::gpu`) integrating directly with `nisaba::gpu::GpuDevice` (OpenGL ES 3.2 / Desktop GL) with zero-leak RAII state isolation (`GlStateGuard3D`), std140 UBO uploads, and hardware shadow depth passes.
  - 100% verified across 7 dedicated 3D test suites (58/58 total engine test suites passing).

---
## License & Sovereignty

The **Nisaba** engine is developed exclusively for the **`MICRO INFINITY`** organization. Engineered to adhere to the highest standards of architectural purity, speed, and reliability, providing an uncompromising sovereign foundation for modern software engineering, robotics, and embedded systems.

