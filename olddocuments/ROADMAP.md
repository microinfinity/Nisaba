# Nisaba — Technical Roadmap

## 1. GPU Hardware Instancing

* **Primary Objective:** Implement **GPU Hardware Instancing** to batch and render repetitive 2D graphic primitives and UI elements (such as buttons, rounded rectangles, and shadows) in a single hardware draw call, eliminating redundant CPU-side geometry generation and per-item draw overhead.

### Technical Implementation Plan:
1. **Base Unit Geometry (Base Unit Quads):**
   * Pre-allocate standardized normalized unit quads (`[0,0] -> [1,1]`) directly in GPU VRAM to bypass per-element CPU vertex recreation.

2. **Per-Instance Dynamic Descriptor Buffer:**
   * Stream a compact per-instance data buffer containing layout and styling parameters (position, dimensions, tint color, corner radii, and stroke metrics).

3. **Instanced Shader Pipelines:**
   * Enable hardware instancing across both supported GPU backends:
     * **OpenGL / EGL:** Issue `glDrawArraysInstanced` / `glDrawElementsInstanced` addressing `gl_InstanceID`.
     * **Vulkan:** Issue `vkCmdDraw` / `vkCmdDrawIndexed` configuring dynamic `instanceCount`.

4. **Target UI & Rendering Primitives:**
   * Rounded rectangles, cards, and interactive buttons.
   * Box drop shadows and ambient glow effects.
   * Repeated vector icons, texture quads, and badge indicators.
   * Text glyphs and font run quads.

---

## 2. Nisaba Markdown Document Subsystem Roadmap

The architecture of the native Markdown document engine (`nisaba::markdown`) has been audited across all core components: the AST parser (`parser.cpp`), layout engine (`document.cpp`), vector renderer (`renderer.cpp`), PDF exporter (`pdf_exporter.cpp`), and syntax highlighter (`syntax_highlighter.cpp`).

Based on this audit, the technical roadmap outlines eight strategic axes to elevate Nisaba Markdown into an enterprise-grade document engine:

### Axis 1: True Rich Media & Image Decoding
* **Current Limitation:** Images within Markdown paragraphs are currently rendered as fallback plain text placeholders (`[Image: alt]`) without actual decoding or graphic display on the canvas.
* **Development Plan:**
  1. **Raster Image Decoding:** Integrate the Markdown parser with Nisaba's native image codec pipeline (`image_io`, `image_png`, `image_jpeg`, `image_qoi`, `image_webp`) to decode local and memory buffers directly.
  2. **Aspect Ratio Layout:** Calculate natural image dimensions during document layout, reserving precise vertical and horizontal flow space with alignment support (left, right, center).
  3. **Native SVG & Lottie Media Support:**
     * Support inline vector SVG images (`![icon](file.svg)`) rendered natively with infinite scaling via `nisaba::svg`.
     * Support embedded vector Lottie animations (`nisaba::lottie`) for dynamic, interactive living documents.

---

### Axis 2: Mathematical Typesetting (LaTeX / KaTeX Equations)
* **Objective:** Deliver first-class support for scientific, mathematical, and academic formula layout.
* **Development Plan:**
  1. **Inline Math:** Parse and layout inline formulas delimited by `$ ... $` (e.g. `$E = mc^2$`).
  2. **Display Math Blocks:** Support standalone centered formula blocks enclosed in `$$ ... $$`.
  3. **Sovereign Math Layout Engine:** Implement a lightweight TeX math parser that translates fractions (`\frac{a}{b}`), radicals and exponents (`\sqrt{x}`, `x^2`), summation, integrals, and matrices into precision vector glyph paths using OpenType Math tables.

---

### Axis 3: Vector Diagrams & Charts (Mermaid.js Integration)
* **Objective:** Render software architecture diagrams, flowcharts, and sequence graphs automatically within documents without external tools.
* **Development Plan:**
  1. **Mermaid Fenced Code Blocks:** Recognize ` ```mermaid ` code blocks in the AST lexer and route them to a dedicated diagram generator rather than a generic code block.
  2. **Flowcharts:** Support core graph syntax (`graph TD`, `graph LR`), rendering nodes, connectors, arrowheads, and inline labels using `Path` and `Stroker`.
  3. **Sequence Diagrams:** Render actor lifelines, activation boxes, and synchronous/asynchronous interaction arrows.

---

### Axis 4: Advanced GFM Tables
* **Objective:** Expand basic Markdown table formatting into a full publishing-ready tabular layout system.
* **Development Plan:**
  1. **Cell Merging:** Support horizontal spans (`colspan`) and vertical spans (`rowspan`) via extended GFM or HTML attributes.
  2. **Multi-line Cell Content:** Allow hard line breaks and bullet lists within table cells with automatic row height recomputation.
  3. **Sticky & Repeating Table Headers:**
     * Pin table header rows during interactive scrolling in viewer widgets.
     * Automatically repeat table headers across page breaks during PDF document export.
  4. **Dynamic Column Auto-Sizing:** Distribute column widths proportionally based on text density and content intrinsic width rather than rigid uniform sizing.

---

### Axis 5: Syntax Highlighter Expansion
* **Objective:** Broaden code syntax highlighting across modern languages, systems programming, and graphics technologies.
* **Development Plan:**
  1. **Shader Languages:** Implement highlighters for GLSL, WGSL, HLSL, and SPIR-V (essential for a graphics engine ecosystem).
  2. **Configuration & Data Formats:** Support JSON, YAML, TOML, and XML.
  3. **Modern Systems & App Languages:** Support Go, Java, C#, SQL, Kotlin, Swift, and Zig.
  4. **Targeted Line Highlighting:** Support line highlighting syntax in code fences (e.g. ` ```cpp {3-5,8} `).
  5. **Interactive Copy Button:** Render an interactive clipboard copy button in the top corner of code blocks.
  6. **Git Diff Syntax:** Highlight unified diff blocks (`diff`), rendering additions in green and deletions in red.

---

### Axis 6: Interactive UX & Document Selection
* **Objective:** Transform static Markdown views into an interactive document viewer and editor canvas.
* **Development Plan:**
  1. **Text Selection & Clipboard Copying:**
     * Enable click-and-drag cursor selection across paragraphs, table cells, and code blocks.
     * Render selection highlight boxes.
     * Bind system clipboard hotkeys (`Ctrl+C` / `Cmd+C`) to copy selected text to the OS clipboard.
  2. **In-Document Search & Highlight:**
     * Provide text matching across the document AST with total match counters.
     * Draw visual highlight overlays over matching bounding boxes and provide next/previous match navigation.
  3. **Kinetic Physics Scrolling:** Implement momentum, inertial decay, and friction for touchscreens and precision trackpads.
  4. **Smooth Collapsible Details:** Animate opening and closing transitions for `<details>` disclosure elements (Accordion animation).

---

### Axis 7: Enhanced PDF Generation & Print Publishing
* **Objective:** Generate publication-ready PDF documents suitable for books, technical manuals, and academic publishing.
* **Development Plan:**
  1. **PDF Document Outline / Bookmarks:** Automatically generate an interactive hierarchical PDF bookmark tree based on heading levels (`H1` to `H6`).
  2. **Table of Contents (TOC) Page:** Auto-generate a dedicated TOC page with dotted leader lines (`......`), page numbers, and clickable internal hyper-references.
  3. **Dynamic Header & Footer Macros:** Support page header and footer templates with variable interpolation (`{{page}}`, `{{total_pages}}`, `{{chapter_title}}`).
  4. **Multi-Column Layout:** Support two-column and three-column balanced page flows for periodicals and scientific papers.

---

### Axis 8: Smart Typography & Document Frontmatter
* **Objective:** Enhance text aesthetics and provide metadata management for publishing workflows.
* **Development Plan:**
  1. **Metadata Frontmatter (YAML / TOML):** Extract document metadata (title, author, date, categories, version) at the head of Markdown files.
  2. **Smart Typography (SmartyPants):**
     * Convert straight ASCII quotes (`" "`, `' '`) into curved typographical curly quotes (`“ ”`, `‘ ’`).
     * Convert double hyphens (`--`) into En-dashes (`–`) and triple hyphens (`---`) into Em-dashes (`—`).
     * Convert consecutive periods (`...`) into standard Unicode ellipsis symbols (`…`).

---

## 3. Nisaba SVG Vector Graphics Subsystem Roadmap

The native SVG vector engine (`nisaba::svg`) has been audited across all core components:
* Document model & parser: [`svg_document.cpp`](file:///home/x/Desktop/nisaba/src/svg/svg_document.cpp)
* W3C path interpreter: [`path_parser.cpp`](file:///home/x/Desktop/nisaba/src/svg/path_parser.cpp)
* Raster baking & caching: [`baked_svg.cpp`](file:///home/x/Desktop/nisaba/src/svg/baked_svg.cpp) and [`svg_cache.cpp`](file:///home/x/Desktop/nisaba/src/svg/svg_cache.cpp)

The engine currently provides robust support for geometric primitives (`path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`), grouping (`g`, `use`, `defs`), linear and radial gradients, clipping and masking (`clipPath`, `mask`), and affine transformation matrices.

To achieve complete compliance with **W3C SVG 1.1 Full and SVG 2.0**, the following development axes are established:

### Axis 1: Text Elements & Vector Typography (`<text>`, `<tspan>`, `<textPath>`)
* **Current Limitation:** The SVG engine currently lacks `<text>` element support, causing text labels, branded logos, and diagrams within SVG assets to be omitted.
* **Development Plan:**
  1. **Inline & Sub-string Text Elements (`<text>`, `<tspan>`):** Parse absolute and relative coordinates (`x, y, dx, dy`) and CSS font styling (`font-family`, `font-size`, `font-weight`, `font-style`).
  2. **Text Alignment & Anchoring:** Implement `text-anchor` (`start`, `middle`, `end`) and `dominant-baseline` for alignment within icons and badges.
  3. **Text Following Curved Paths (`<textPath>`):** Render text conforming to arbitrary vector curves using path length parametrization.
  4. **Integration with Sovereign Font Engine:** Feed SVG glyph runs directly to Nisaba's `FontSystem` and `GlyphCache` for complex script shaping (Arabic, Persian, Indic) with subpixel anti-aliasing.

---

### Axis 2: Dashed & Dotted Stroke Effects (`stroke-dasharray`, `stroke-dashoffset`)
* **Current Limitation:** While Nisaba has a high-performance `DashPathEffect` in its 2D core, the SVG parser does not parse or apply `stroke-dasharray` or `stroke-dashoffset` attributes.
* **Development Plan:**
  1. **Dash Array Parser:** Parse comma- and whitespace-separated length sequences into alternating on/off interval vectors.
  2. **Dash Phase Offset:** Support phase offsets (`stroke-dashoffset`) to enable animated marching-ants selections.
  3. **Core Stroker Integration:** Pass interval sequences directly to `DashPathEffect` prior to path stroke tessellation.

---

### Axis 3: Advanced Graphical Filter Effects (`<filter>` & Filter Graph)
* **Objective:** Enable drop shadows, glows, blurs, and color adjustments standard in modern design assets.
* **Development Plan:**
  1. **Filter Graph Pipeline:** Build an acyclic execution graph connecting filter node inputs and outputs (`in`, `result`).
  2. **Gaussian Blur (`<feGaussianBlur>`):** Implement separable dual-pass Gaussian blur leveraging Nisaba's SIMD convolution primitives.
  3. **Drop Shadow (`<feDropShadow>`):** Generate direct drop shadows with offset (`dx, dy`), blur radius, and color tinting.
  4. **Color Transformation (`<feColorMatrix>`):** Connect 4x5 color matrices using Nisaba's `ColorMatrix` pipeline.
  5. **Compositing Operators (`<feBlend>`, `<feOffset>`, `<feComposite>`):** Support blend modes including Multiply, Screen, Overlay, and arithmetic compositing.

---

### Axis 4: Repeating Patterns (`<pattern>`)
* **Objective:** Support tiled pattern fills (`fill="url(#patternId)"`) alongside solid colors and gradients.
* **Development Plan:**
  1. **Pattern Definition Container:** Parse pattern bounding dimensions and coordinate systems (`patternUnits`, `patternContentUnits`).
  2. **Tile Rasterization & Shader:** Render pattern contents once to a cached tile pixmap and expose it as a wrapped repeat/reflect texture shader.

---

### Axis 5: Markers & Arrowheads (`<marker>`)
* **Objective:** Automate rendering of arrowheads, connector endpoints, and geometric nodes in technical diagrams.
* **Development Plan:**
  1. **Marker Container:** Support marker dimensions, reference anchor coordinates (`refX, refY`), and automatic orientation (`orient="auto"` / `orient="auto-start-reverse"`).
  2. **Vertex Attachment:** Bind markers to path endpoints (`marker-start`, `marker-end`) and intermediate vertices (`marker-mid`).

---

### Axis 6: CSS Style Blocks & Class Selectors (`<style>`)
* **Current Limitation:** SVG files exported from tools such as Adobe Illustrator, Inkscape, and Figma rely heavily on `<style>` CSS rule blocks (e.g. `.st0 { fill: #f00; }`), which are ignored when styling is not inlined as direct XML attributes.
* **Development Plan:**
  1. **Lightweight CSS Lexer:** Extract selector rules from `<style>` blocks and store them in a class-to-property lookup table.
  2. **Class Matching & Cascade:** Match `class="..."` attributes on SVG elements and apply inherited color, stroke, and opacity rules.

---

### Axis 7: Symbol Reusability & ViewBox Scaling (`<symbol>`, `preserveAspectRatio`)
* **Objective:** Ensure complete compatibility with modern icon sprite sheets (FontAwesome, Material Symbols, Bootstrap Icons).
* **Development Plan:**
  1. **Symbol Element Support:** Implement `<symbol>` containers with independent coordinate `viewBox` roots instantiable via `<use href="#symbol-id">`.
  2. **Aspect Ratio Preservation (`preserveAspectRatio`):**
     * Implement `none` (non-uniform stretch to target bounds).
     * Implement `xMidYMid meet` (uniform scale preserving aspect ratio with containment — default).
     * Implement `xMidYMid slice` (uniform scale filling target bounds with edge cropping).

---

### Axis 8: Direct Hardware GPU SVG Rendering
* **Current Limitation:** The engine currently rasterizes SVG trees via the software `Canvas` before caching results into `BakedSvg`.
* **Development Plan:**
  1. **Direct GPU Rendering on `GpuCanvas`:** Stream SVG geometry directly to the hardware tessellation pipeline (`gpu_tessellator`) for on-the-fly triangle generation.
  2. **Hardware Gradient Shaders:** Compile SVG gradients directly to Vulkan / OpenGL fragment shaders.
  3. **GPU Vector Icon Atlas:** Cache rendered vector icons into a unified GPU texture atlas to sustain 120+ FPS in complex UI scenes.

---

## 4. Nisaba Lottie Vector Animation Subsystem Roadmap

The sovereign **Lottie** vector animation engine (`nisaba::lottie`) has been audited across all core source files:
* Layer hierarchy & composition: [`layer.cpp`](file:///home/x/Desktop/nisaba/src/lottie/layer.cpp) and [`layer.hpp`](file:///home/x/Desktop/nisaba/include/nisaba/lottie/layer.hpp)
* Shapes & geometric modifiers: [`shape.cpp`](file:///home/x/Desktop/nisaba/src/lottie/shape.cpp) and [`shape.hpp`](file:///home/x/Desktop/nisaba/include/nisaba/lottie/shape.hpp)
* Keyframe interpolation: [`property.cpp`](file:///home/x/Desktop/nisaba/src/lottie/property.cpp) and [`property.hpp`](file:///home/x/Desktop/nisaba/include/nisaba/lottie/property.hpp)
* Document container & player: [`animation.cpp`](file:///home/x/Desktop/nisaba/src/lottie/animation.cpp) and [`player.cpp`](file:///home/x/Desktop/nisaba/src/lottie/player.cpp)

The engine currently supports basic vector shapes (free paths `sh`, rectangles `rc`, ellipses `el`, polystars `sr`), solid fill `fl`, solid stroke `st`, gradient fill `gf`, trim paths `tm`, and nested precompositions (`PrecompLayer`).

To achieve complete parity with the **Airbnb Lottie / Bodymovin Full Specification**, the following eight axes are planned:

### Axis 1: Animated Gradient Strokes (`gs`)
* **Current Limitation:** While gradient fills (`gf`) are supported, gradient strokes (`gs`) are currently unhandled, preventing playback of neon lines and dynamic chromatic strokes common in modern motion design.
* **Development Plan:**
  1. **Gradient Stroke Parser:** Parse gradient type (linear or radial), start/end coordinates, opacity, stroke width, line caps, and line joins.
  2. **Animated Color Stop Interpolation:** Interpolate dynamic gradient color stops over the animation timeline and render using Nisaba's gradient stroke pipeline.

---

### Axis 2: Advanced Shape Modifiers
* **Objective:** Enable complex procedural animations exported from Adobe After Effects without manual vertex keyframing.
* **Development Plan:**
  1. **Repeater Modifier (`rp`):**
     * Parse copy counts, offsets, and incremental transform matrices (cumulative scaling, positioning, rotation, and alpha falloff).
     * Render procedural arrays (fireworks, pulsing rings, mechanical gears).
  2. **Boolean Path Merging (`mm` - Merge Paths):**
     * Connect `mm` operators directly to Nisaba's native boolean path engine (`nisaba::path_ops`).
     * Support all four boolean modes: Union, Subtract, Intersect, and Exclude / XOR.
  3. **Rounded Corners Modifier (`rd`):**
     * Apply procedural corner rounding using Bézier arc insertion on sharp polystar and path vertices.
  4. **Procedural Path Deformers (`tw` Twist, `zz` ZigZag, `op` Offset Path):**
     * Add mathematical path distortion filters for organic fluid motion.

---

### Axis 3: Raster Image Layers (`ty: 2`) & Embedded Assets
* **Current Limitation:** `ImageLayer::render` is currently a placeholder stub, and raster image items within the JSON `assets` array are ignored.
* **Development Plan:**
  1. **Asset Image Loader:** Extract Base64-encoded embedded data (`data:image/png;base64,...`) or external asset files and decode them via Nisaba's image subsystem (`image_png`, `image_jpeg`, `image_webp`).
  2. **Image Layer Compositing:** Bind decoded pixel buffers to layers, applying 3D transformation matrices, opacity, and layer masks (`Masks` and `Mattes`).

---

### Axis 4: Animated Text Layers (`ty: 5`) & Glyphs
* **Current Limitation:** Text layers are currently unsupported, preventing playback of kinetic typography, subtitles, and animated counters.
* **Development Plan:**
  1. **Text Layer Model:** Parse text content, font family, font size, tracking/kerning, line height, and alignment.
  2. **Per-Character Animators:** Animate individual glyphs sequentially (cascade fade-in, character bounces, letter rotations).
  3. **Font Engine Integration:** Render vector glyphs with complex shaping via Nisaba's native `FontSystem`.

---

### Axis 5: Spatial Bézier Interpolation (`ti` / `to` Tangents)
* **Current Limitation:** Spatial position animation is currently evaluated using straight linear interpolation (`lerp`), whereas After Effects animations frequently follow curved spatial trajectories.
* **Development Plan:**
  1. **Spatial Tangent Parser:** Parse incoming (`ti`) and outgoing (`to`) spatial tangents on position keyframes.
  2. **Cubic Spatial Bézier Splines:** Evaluate 3D spatial cubic curves over time to ensure smooth, natural curvilinear motion matching After Effects output.

---

### Axis 6: Layer Blend Modes & Layer Effects
* **Development Plan:**
  1. **Blend Modes (`bm`):** Parse layer blend modes and bind them to GPU compositing pipelines (Multiply, Screen, Overlay, Darken, Lighten, Color Dodge, Color Burn).
  2. **Layer Effects (`ef`):** Parse and apply layer-level visual effects, focusing on Gaussian Blur and Drop Shadows.

---

### Axis 7: Compressed dotLottie Format Support (`.lottie`)
* **Objective:** Support the modern industry-standard compressed animation format (up to 80% smaller than raw JSON).
* **Development Plan:**
  1. **Native dotLottie Archive Reader:** Implement a container parser for `.lottie` ZIP archives containing `manifest.json`, animations, images, and fonts, using Nisaba's native `nisaba::image::deflate` decompressor.
  2. **Multi-Animation Playback:** Support seamless switching between multiple animation scenes packaged within a single archive.

---

### Axis 8: Math Expressions & Direct GPU Pipeline
* **Development Plan:**
  1. **Lightweight Expressions Engine:** Support common After Effects JavaScript expressions such as `loopOut("cycle")`, `loopOut("pingpong")`, and `wiggle(frequency, amplitude)`.
  2. **Direct Hardware GPU Pipeline:**
     * Stream dynamic animated paths directly to `gpu_tessellator`.
     * Leverage **GPU Hardware Instancing** to render complex particle fields and vector scenes at 120 FPS on Vulkan and OpenGL ES.

---

## 5. Nisaba PDF Document Subsystem Roadmap

### Overview
The `nisaba::pdf` subsystem is a sovereign, zero-dependency PDF engine providing bidirectional capabilities: generating vector PDF documents ([`PdfCanvas`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_canvas.hpp), [`PdfWriter`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_writer.hpp)) and reading, parsing, and rendering existing documents ([`PdfReader`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_reader.hpp), [`PdfParser`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_parser.hpp), [`PdfInterpreter`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_interpreter.hpp), [`PdfCodec`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_codec.hpp), [`PdfCrypto`](file:///home/x/Desktop/nisaba/include/nisaba/pdf/pdf_crypto.hpp)).

The engine currently supports stream filters (`FlateDecode`, `ASCIIHexDecode`, `ASCII85Decode`, `RunLengthDecode`, `DCTDecode`), password protection (`Standard Security Handler` via RC4 and AES-128), page catalog parsing, graphics operator interpretation, standard Type 1 fonts, and `ToUnicode CMap` encoding.

This roadmap outlines eight technical axes to elevate Nisaba PDF into a competitive engine compliant with **ISO 32000-1 and ISO 32000-2**:

---

### Axis 1: Vector Font Embedding & Complex Scripts (TrueType/OpenType CIDFontType2 & Subsetting)
* **Current Limitation:** `PdfCanvas` currently supports only the 14 standard Latin Type 1 fonts (Helvetica, Times, Courier). It cannot embed external TrueType/OpenType fonts, preventing PDF generation in complex scripts (Arabic, Persian, Hebrew) or wide-character sets (CJK).
* **Development Plan:**
  1. **TrueType / OpenType Embedding:** Generate standard composite font dictionaries (`/CIDFontType2`, `/FontDescriptor`, `/FontFile2`) and embed font binary streams into the PDF structure.
  2. **Intelligent Font Subsetting:** Package only glyphs referenced in the document with unique subset tags (e.g. `ABCDEF+Amiri`) to minimize PDF file size and protect font licensing.
  3. **Automatic `ToUnicode CMap` Generation:** Embed character mapping tables so global PDF viewers can extract and copy Unicode text accurately without corruption.

---

### Axis 2: Structured Text Extraction & In-PDF Search
* **Current Limitation:** `PdfInterpreter` draws glyphs directly to the canvas but lacks a structured text extraction API, preventing client applications from performing text selection, clipboard copying, or keyword searching.
* **Development Plan:**
  1. **Structured Text Extractor (`PdfTextExtractor`):**
     * Parse text operators (`BT`, `ET`, `Tf`, `Tj`, `TJ`, `Tm`, `Td`) and compute precise bounding boxes for every character, word, and line run.
     * Apply Unicode Bidirectional (BiDi) reordering for accurate visual and logical reading order.
  2. **In-Document Search Engine (`PdfSearchEngine`):**
     * Provide substring and regular expression search across pages.
     * Return quadrilateral coordinates (`QuadPoints`) for all search hits to enable real-time UI highlighting.

---

### Axis 3: Interactive AcroForms & Form Flattening
* **Current Limitation:** The engine currently renders static page content only and does not process interactive form dictionaries (`/AcroForm`), a requirement for enterprise and legal documents.
* **Development Plan:**
  1. **Interactive Form Parser (`PdfAcroForm`):** Parse `/Fields` collections across standard field types:
     * Text input fields (`/Tx`): single-line, multi-line, formatted, and password fields.
     * Buttons and checkboxes (`/Btn`): push buttons, check boxes, and radio buttons.
     * Choice fields (`/Ch`): combo boxes and list boxes.
  2. **Appearance Stream Generation (`/AP`):** Render dynamic appearance streams when users edit or interact with form fields.
  3. **Form Flattening:** Provide a built-in feature to bake filled interactive fields into static page vector geometry prior to final export or printing.

---

### Axis 4: PDF Annotations Pipeline
* **Current Limitation:** Currently, only hyperlink annotations (`PdfLinkAnnotation`) are handled, ignoring other annotation types defined in ISO 32000.
* **Development Plan:**
  1. **Expanded Annotation Model (`PdfAnnotation`):** Support reading, authoring, and updating common annotation types:
     * Sticky notes and popups (`/Text`).
     * Text markups (`/Highlight`, `/Underline`, `/StrikeOut`, `/Squiggly`).
     * Freehand ink and digital stylus signatures (`/Ink`).
     * Rubber stamps (`/Stamp`: "Approved", "Confidential", "Draft", or custom vector stamps).
     * Geometric callouts (`/Square`, `/Circle`, `/Line`).
  2. **Annotation Rendering & Export:** Save annotations as standards-compliant `/Annots` objects or flatten them directly into page content streams.

---

### Axis 5: Archival Compliance (PDF/A Standards & XMP Metadata)
* **Objective:** Enable Nisaba to produce legally and medically certified archival documents compliant with ISO 19005 (PDF/A).
* **Development Plan:**
  1. **PDF/A Compliance Validation (PDF/A-1b, PDF/A-2b, PDF/A-3b):**
     * Enforce strict PDF/A constraints: ban executable JavaScript, mandate full font embedding, forbid device-dependent colors without intent profiles.
     * Generate `/OutputIntents` dictionaries with embedded standard ICC color profiles.
  2. **Structured XMP Metadata Packets:**
     * Generate XML/RDF metadata conforming to Dublin Core and Adobe PDF schemas.
     * Record creation timestamps, modification history, and standard compliance tags verifiable by enterprise tools like VeraPDF.

---

### Axis 6: Digital Signatures & Advanced Cryptography (PAdES / PKCS#7 & Cryptography)
* **Objective:** Provide a sovereign infrastructure to verify and sign digital documents to guarantee authenticity and prevent tampering.
* **Development Plan:**
  1. **Digital Signature Verification:**
     * Parse signature dictionaries (`/Sig`) and extract byte-range arrays (`/ByteRange`).
     * Compute document message digests (SHA-256 / SHA-512) and verify PKCS#7 / CMS signature packages natively.
     * Validate X.509 certificate chains and detect post-signing document alterations.
  2. **Visual Digital Signature Generation:**
     * Allow developers to sign PDF documents programmatically with custom visual appearance layouts (signer name, timestamp, crest logo, or handwritten signature).
  3. **AES-256 Encryption (PDF 2.0 / ExtensionLevel 3):**
     * Upgrade document security to AES-256 CBC with standard ISO 32000-2 key derivation.

---

### Axis 7: Advanced Archival Decoders (JBIG2Decode & JPXDecode / JPEG 2000)
* **Current Limitation:** The engine currently supports standard image filters (`Flate`, `DCT/JPEG`, `RunLength`), but lacks decoders for specialized scanned archival books and geospatial imagery.
* **Development Plan:**
  1. **Sovereign JBIG2 Decoder (`JBIG2Decode`):**
     * Implement a native decompressor for bi-level (1-bit black & white) images used extensively in document scanners, libraries, and digital book archives (e.g. Google Books, Internet Archive) for compression up to 10x smaller than TIFF/PNG.
  2. **JPEG 2000 Decoder (`JPXDecode`):**
     * Support wavelet-based JPEG 2000 streams prevalent in medical, engineering, and satellite PDF documents.

---

### Axis 8: PDF Page Manipulation & Document Geometry
* **Objective:** Provide page-level document lifecycle and editorial management tools.
* **Development Plan:**
  1. **Merge & Split API:**
     * Merge multiple PDF files into a single document with indirect object ID remapping and cross-reference table (`XRef`) conflict resolution.
     * Extract specific page subsets into standalone PDF files without re-encoding page streams (Lossless Page Extraction).
  2. **Page Geometry & Transformation:**
     * Programmatically adjust page orientations (`/Rotate 90, 180, 270`).
     * Modify page boundary boxes: MediaBox, CropBox, BleedBox, and TrimBox.
  3. **Watermarking & Bates Numbering:**
     * Provide an API to apply vector and text watermarks across document pages.
     * Implement sequential Bates numbering for legal and judicial document tracking.

---

## 6. Nisaba Native OS Backend Subsystem Roadmap

### Overview
The `backend_os` subsystem serves as Nisaba's sovereign operating system abstraction layer (Native OS Windowing & Platform Layer), operating without dependencies on SDL, GLFW, or Qt.

Supported platforms currently include:
* **Linux (Wayland, X11, DRM/KMS):** Protocols including `xdg-shell`, `wlr-layer-shell`, drag-and-drop (DnD), multi-MIME clipboard, foreign window management (`wlr-foreign-toplevel` and EWMH), and multi-monitor output geometry via `xdg-output`.
* **Windows (Win32):** Native window creation, Win32 message processing, OpenGL/EGL contexts, clipboard, and drag-and-drop.
* **Android:** Native `android_app_glue` integration, `ANativeWindow`, EGL contexts, and runtime permissions (API 33+).
* **WebAssembly (Wasm):** Direct Emscripten event loops, canvas scaling, and web input bindings.

The following eight axes address identified technical gaps to establish a unified, production-grade desktop and mobile OS abstraction:

---

### Axis 1: Apple macOS Native Backend (Cocoa / NSWindow & Metal Backend)
* **Current Limitation:** macOS (Darwin) is currently unsupported; there is no Cocoa integration in `backend_os`, preventing Nisaba from running natively on Apple hardware.
* **Development Plan:**
  1. **Native Cocoa Platform Layer (`CocoaPlatformBackend` & `CocoaWindow`):**
     * Implement Objective-C++ (`.mm`) compilation units managing `NSApplication` and `NSWindow` lifecycles.
     * Integrate with the macOS RunLoop using non-blocking event polling (`nextEventMatchingMask`).
  2. **Accelerated Surface Layers (CAMetalLayer & NSOpenGLView):**
     * Provide a `CAMetalLayer` surface to feed the GPU pipeline directly via Metal or MoltenVK.
  3. **macOS Native UX Integration:**
     * Support Retina displays and track resolution scale factor changes (`backingScaleFactor`).
     * Construct standard macOS application menu bars (`NSMenu`).
     * Support Dock icons and notification badges.

---

### Axis 2: Direct Vulkan Native Surfaces & Swapchains
* **Current Limitation:** The `Window` class currently relies exclusively on EGL and OpenGL contexts. While Nisaba features a Vulkan pipeline (`enable_vulkan: true`), it cannot present directly to OS surfaces without intermediary layers.
* **Development Plan:**
  1. **Native Vulkan Surface Creation (`VkSurfaceKHR`):**
     * Implement direct platform surface factory functions:
       * `vkCreateWaylandSurfaceKHR` for Wayland.
       * `vkCreateXlibSurfaceKHR` / `vkCreateXcbSurfaceKHR` for X11.
       * `vkCreateWin32SurfaceKHR` for Windows.
       * `vkCreateAndroidSurfaceKHR` for Android.
       * `vkCreateMetalSurfaceEXT` for macOS.
  2. **Vulkan Swapchain Pipeline:**
     * Expose swapchain configuration interfaces for surface color formats (`VkFormat`), color spaces, and presentation modes (`VK_PRESENT_MODE_FIFO_KHR`, `VK_PRESENT_MODE_MAILBOX_KHR`).
     * Handle swapchain recreation during window resize events (`VK_ERROR_OUT_OF_DATE_KHR`).

---

### Axis 3: Input Method Editor (IME) & Virtual Keyboards
* **Current Limitation:** Text input is currently restricted to single character key events, preventing composition of complex Asian scripts (Chinese, Japanese, Korean CJK) and virtual keyboard interaction.
* **Development Plan:**
  1. **Wayland Text Input Protocol (`zwp_text_input_v3`):**
     * Handle pre-edit composition text strings and committed text signals.
     * Report cursor bounding rectangles (`setInputArea(Rect)`) so the compositor can position candidate selection windows next to the cursor.
  2. **Desktop OS Integration:**
     * Support Windows Imm32 and Text Services Framework (TSF).
     * Support X11 XIM and IBus protocols.
  3. **Virtual Keyboard Controls for Touch Devices:**
     * Provide APIs to show/hide on-screen keyboards (`showVirtualKeyboard()`, `hideVirtualKeyboard()`).
     * Adjust viewport offsets automatically to prevent the virtual keyboard from obscuring active input fields.

---

### Axis 4: Multi-Touch, Stylus/Pen Tablets & Native Gesture Recognizers
* **Current Limitation:** Touch events across Android, Wasm, and DRM are currently flattened into single-pointer mouse clicks, without multi-touch tracking or stylus pressure support.
* **Development Plan:**
  1. **Multi-Touch Architecture (`TouchEvent`):**
     * Support concurrent touch points tracked by persistent identifiers (`touch_id`) and touch phases (`TouchDown`, `TouchMove`, `TouchUp`, `TouchCancel`).
  2. **Stylus & Graphic Tablet Support:**
     * Support Wacom, Apple Pencil, S-Pen, and Surface Pen devices.
     * Read continuous pressure levels (`pressure: 0.0 - 1.0`).
     * Read tilt angles (`tilt_x`, `tilt_y`) and pen rotation.
     * Distinguish between pen tip, eraser tip, and barrel buttons.
  3. **Native Gesture Recognizers:**
     * Implement recognizers for pinch-to-zoom, two-finger rotation, and inertial flick/pan.

---

### Axis 5: Gamepad & Joystick Subsystem
* **Objective:** Enable interactive 3D applications, games, and media centers to interface with game controllers natively.
* **Development Plan:**
  1. **Device Enumeration & Hotplugging:**
     * Auto-detect game controller connections and disconnections.
  2. **Analog Input & State Polling:**
     * Poll digital inputs (DPad, A/B/X/Y buttons, shoulder bumpers).
     * Read analog thumbsticks with configurable deadzone filtering.
     * Read variable analog triggers (L2/R2).
  3. **Haptic Feedback & Rumble:**
     * Support dual-motor force feedback and vibration pulses.
  4. **Platform Integrations:**
     * Linux: `libevdev` and `/dev/input/event*`.
     * Windows: `XInput` and `Windows.Gaming.Input`.
     * WebAssembly: HTML5 Gamepad API.

---

### Axis 6: Native File Dialogs & Message Boxes
* **Current Limitation:** The engine lacks native file dialog abstractions, requiring applications to build custom file browsers or invoke third-party tools.
* **Development Plan:**
  1. **Native File Dialogs (`NativeFileDialog`):**
     * Single and multi-file open dialogs (`showOpenFileDialog(filters, allow_multiple)`).
     * File save dialogs with default extension filters (`showSaveFileDialog()`).
     * Folder selection dialogs (`showFolderDialog()`).
  2. **Cross-Platform Integration:**
     * Linux: Utilize the standard **XDG Desktop Portal** over D-Bus (compatible with GNOME, KDE, Flatpak, and Snap).
     * Windows: Use the `IFileDialog` COM interface.
     * macOS: Use `NSOpenPanel` and `NSSavePanel`.
  3. **Native Message Boxes (`NativeMessageBox`):**
     * Display standard alert, confirmation, and warning dialogs with native action buttons (OK, Cancel, Yes, No).

---

### Axis 7: System Tray & Desktop Notifications
* **Objective:** Enable background service applications, media players, and desktop utilities to dock in the system tray and post alerts.
* **Development Plan:**
  1. **System Tray Integration (System Tray / AppIndicator):**
     * Support `StatusNotifierItem` and `AppIndicator` protocols over D-Bus for Linux desktops.
     * Support Windows `Shell_NotifyIcon`.
     * Support macOS `NSStatusItem` in the system menu bar.
     * Provide context menus bound to tray icon clicks.
  2. **Desktop Push Notifications:**
     * Post native system notifications via `org.freedesktop.Notifications` on Linux, Windows Action Center (Toast), and macOS Notification Center.

---

### Axis 8: Modern Wayland Protocols & Display HDR
* **Development Plan:**
  1. **Fractional Scaling Without Blurring (`fractional-scale-v1` & `viewporter`):**
     * Support native fractional scaling (e.g. 125%, 150%, 175%) without downscaled raster blurs.
  2. **Pointer Locking & Relative Motion (`pointer-constraints` & `relative-pointer`):**
     * Lock the mouse cursor to surfaces and stream unconstrained delta coordinates (essential for 3D navigation and CAD tools).
  3. **Idle Inhibit (`idle-inhibit-unstable-v1`):**
     * Prevent screen dimming or lockouts during video playback and presentation modes.
  4. **Display HDR & Wide Color Gamuts:**
     * Negotiate extended color gamuts (`DCI-P3` and `BT.2020`).
     * Manage HDR luminance metadata for high-dynamic-range displays.

---

## 7. Nisaba Image & Raster Codecs Subsystem Roadmap

### Overview
The `nisaba::image` subsystem is Nisaba's native, zero-dependency raster image codec engine. It operates without external libraries such as `libpng`, `libjpeg-turbo`, or `stb_image` to ensure memory safety, deterministic execution, and platform sovereignty.

The engine currently supports:
* **PNG** encoding and decoding via a native DEFLATE and zlib compression pipeline ([`nisaba::image::deflate`](file:///home/x/Desktop/nisaba/include/nisaba/image/deflate.hpp)).
* **JPEG** sequential decoding and encoding (Baseline DCT SOF0) supporting 4:4:4, 4:2:2, 4:2:0, and Grayscale.
* **QOI** (Quite OK Image) fast lossless encoding and decoding.
* Basic uncompressed export for **BMP** and **PPM**.

This roadmap establishes eight strategic axes focusing on codec diversity, next-generation image compression, animated formats, and metadata handling:

---

### Axis 1: Modern WebP Format (Lossy, Lossless & Animated WebP)
* **Current Limitation:** WebP is currently unsupported despite being standard across web ecosystems, mobile applications, and modern asset pipelines.
* **Development Plan:**
  1. **Lossless WebP Decoder (Lossless VP8L):**
     * Parse RIFF/WebP containers (`WEBPVP8L`).
     * Decode inverse spatial transforms (Color Transform, Predictor Transform, Color Indexing Transform).
     * Process canonical Huffman bitstreams and prefix codes for color and alpha channels.
  2. **Lossy WebP Decoder (Lossy VP8):**
     * Parse VP8 intra-frames, macroblock luma/chroma prediction modes (16x16 and 4x4), and inverse discrete cosine transforms (IDCT).
  3. **Extended Containers & Animations (`VP8X` & Animated WebP):**
     * Extract dedicated transparency planes (Alpha Chunks).
     * Parse animation frame sequences (`ANMF` chunks) with per-frame delay timing.

---

### Axis 2: Animated GIF & LZW Decompressor
* **Current Limitation:** GIF format support is absent, preventing the import and export of animated graphics.
* **Development Plan:**
  1. **Native LZW Decompressor for GIF:**
     * Implement an LZW decompressor handling variable code lengths (3-bit to 12-bit) with table clear code reset handling.
  2. **Color Table & Transparency Management:**
     * Process global and local color tables (up to 256 indexed colors).
     * Support transparent color indices.
  3. **Multi-Frame Animation Engine (GIF89a):**
     * Parse Graphic Control Extensions and application looping blocks (`NETSCAPE2.0`).
     * Evaluate frame timelines (`frame_delay_ms`) and apply frame disposal methods (Restore to background, Restore to previous, Do not dispose).

---

### Axis 3: Next-Gen High-Efficiency Formats (AVIF & HEIF / ISOBMFF)
* **Objective:** Support modern high-efficiency compression formats offering superior image fidelity at reduced file sizes with HDR color depth.
* **Development Plan:**
  1. **ISOBMFF Container Parser:**
     * Parse standard ISO base media file structures (`ftyp`, `meta`, `iinf`, `iloc`, `iprp`, `mdat`).
  2. **AV1 Still Image Decoding (`.avif`):**
     * Decode AV1 intra-frames supporting 10-bit and 12-bit deep color channels.
     * Support wide-color-gamut and HDR transfer functions (Rec.2100 PQ and HLG).
  3. **Grid Images & Auxiliary Alpha:**
     * Support mosaic grid image reconstruction (`grid` item type).
     * Extract and composite auxiliary alpha channels.

---

### Axis 4: Professional Archival & Publishing TIFF Format
* **Objective:** Enable Nisaba to import and export TIFF files common in document archiving, commercial printing, pre-press workflows, and medical imaging.
* **Development Plan:**
  1. **TIFF Tag Parser (IFD):**
     * Parse little-endian (`II`) and big-endian (`MM`) headers.
     * Read Image File Directories (IFD) and support BigTIFF for 64-bit files exceeding 4 GB.
  2. **Multi-Compression Scheme Support:**
     * Uncompressed raw pixel data.
     * PackBits run-length encoding.
     * LZW compression for TIFF.
     * Deflate / Zlib and JPEG-in-TIFF.
     * CCITT Group 3 and Group 4 bi-level document fax compression.
  3. **Color Models & Multi-Page Support:**
     * Support bilevel, grayscale, indexed palette, full RGB, and CMYK color models with RGB color conversions.
     * Support multi-page TIFF document structures.

---

### Axis 5: Universal BMP & Netpbm Engine (Full BMP Decoder & PBM/PGM/PPM)
* **Current Limitation:** The engine currently exports uncompressed 32-bit BMP files only and cannot decode existing BMP files; Netpbm formats cannot be read.
* **Development Plan:**
  1. **Universal BMP Decoder:**
     * Support header generations: OS/2 `BITMAPCOREHEADER`, standard `BITMAPINFOHEADER`, and modern `BITMAPV4HEADER` and `BITMAPV5HEADER`.
     * Support all bit depths: 1-bit monochrome, 4-bit and 8-bit paletted, 16-bit, 24-bit, and 32-bit.
     * Implement RLE4 and RLE8 run-length decoders.
     * Support custom channel masks (Bitfields RGB555, RGB565, RGBA8888).
     * Handle scanline orientation (bottom-up vs top-down).
  2. **Netpbm Format Suite:**
     * Read and write binary and ASCII variants: PBM (1-bit monochrome P1/P4), PGM (grayscale P2/P5), and PPM (RGB color P3/P6).

---

### Axis 6: Gaming & OS Icon Formats (TGA & Windows ICO/CUR)
* **Objective:** Provide native decoders for game texture assets and operating system desktop icons.
* **Development Plan:**
  1. **Truevision TGA Engine (`.tga`):**
     * Read and write uncompressed and RLE-compressed game textures.
     * Support 8-bit grayscale, 15/16-bit, and 24/32-bit RGBA color formats.
     * Handle coordinate origins (bottom-left vs top-left).
  2. **Windows Icon & Cursor Engine (`.ico`, `.cur`):**
     * Extract multiple embedded icon resolutions (16x16, 24x24, 32x32, 48x48, 64x64, 128x128, 256x256).
     * Decode classic DIB bitmaps with 1-bit transparency masks, as well as embedded PNG compressed icons.
     * Parse cursor hotspot coordinates (X/Y) from CUR headers.

---

### Axis 7: High Dynamic Range & 3D Lighting (Radiance HDR RGBE & OpenEXR)
* **Objective:** Equip Nisaba to import Image-Based Lighting (IBL) environment maps and 3D panoramic skyboxes at true physical radiance ranges.
* **Development Plan:**
  1. **Radiance HDR Decoder (`.hdr` / RGBE):**
     * Parse Radiance file headers and resolution strings.
     * Decode adaptive run-length encoded scanlines.
     * Convert 32-bit packed RGBE pixels (Red, Green, Blue, Exponent) into continuous 32-bit floating-point radiance values (`float32 RGB`).
  2. **Industrial Light & Magic OpenEXR Decoder (`.exr`):**
     * Support 16-bit half-float and 32-bit single-precision float channel streams.
     * Support standard compression formats (ZIP, RLE, PIZ) for cinematic HDRI assets.

---

### Axis 8: Modern Upgrades for PNG & JPEG (APNG, Progressive JPEG & EXIF Pipeline)
* **Objective:** Expand existing native PNG and JPEG implementations to match modern web and camera capture standards.
* **Development Plan:**
  1. **Animated PNG (APNG):**
     * Parse and author animation chunks (`acTL` frame counts, `fcTL` frame control, `fdAT` frame data).
     * Support 8-bit alpha transparency for animated stickers and UI micro-interactions.
  2. **Progressive JPEG Decoding (`SOF2`):**
     * Decode multi-scan progressive DCT JPEG streams, enabling progressive rendering from coarse previews to sharp images.
  3. **EXIF Metadata & Embedded ICC Color Profiles:**
     * Extract EXIF headers across JPEG, PNG, and WebP.
     * Read camera orientation tags (values 1 through 8) and rotate pixel matrices automatically to prevent mobile photos from appearing rotated.
     * Extract embedded ICC profiles (`iCCP` in PNG, `APP2 ICC_PROFILE` in JPEG) to apply color space transformations (Display-P3, AdobeRGB) to sRGB accurately.