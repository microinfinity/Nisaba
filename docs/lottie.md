# Nisaba Lottie Subsystem (`nisaba::lottie`) — Technical Reference

## 1. Architecture Overview

`nisaba::lottie` is the sovereign C++20 vector animation subsystem of the Nisaba engine (`-fno-rtti` compatible, zero third-party animation dependencies). It parses, evaluates, tracks damage for, and renders Lottie JSON (`.json`) and dotLottie (`.lottie` ZIP archive) documents directly onto any `nisaba::ICanvas` target (`nisaba::Canvas` for CPU software rasterization and `nisaba::gpu::GpuCanvas` for OpenGL 4.6 / GLES 3.2 / Vulkan hardware acceleration) with 1:1 CPU/GPU visual parity.

### Subsystem Structure

| Header / Source | Responsibility |
| :--- | :--- |
| `include/nisaba/lottie/lottie.hpp` | Umbrella public header pulling in the complete `nisaba::lottie` API. |
| `include/nisaba/json/json.hpp`<br>`src/json/json.cpp` | Recursive-descent JSON parser (`nisaba::json::JsonValue`) using cache-local contiguous `std::vector<std::pair<std::string, JsonValue>>` storage, `std::from_chars` numeric parsing, and UTF-16 surrogate pair decoding. |
| `include/nisaba/lottie/lottie_types.hpp` | Enumerations (`LayerType`, `MatteType`, `MaskMode`, `FillRule`, `LineCap`, `LineJoin`, `TrimType`, `GradientType`, `MergeMode`, `CompositeOrder`, `LoopMode`, `EffectType`, `TextJustify`) and blend mode mapping (`map_lottie_blend_mode`). |
| `include/nisaba/lottie/invalidation.hpp`<br>`src/lottie/invalidation.cpp` | Incremental dirty-region tracker (`InvalidationController`) computing minimal changed screen-space bounding boxes (`ScreenIntRect`) across consecutive frames for partial redraws and `GpuCanvas::flush(damage_rect)`. |
| `include/nisaba/lottie/resources.hpp`<br>`src/lottie/resources.cpp` | Pluggable `ResourceProvider` hierarchy (`FileResourceProvider`, `MemoryResourceProvider`, `DataURIResourceProviderProxy`), `LoadFlags::DeferImageLoading` (`kDeferImageLoading`), multi-frame `ImageAsset` / `DecodedImageAsset` (static formats, animated WebP, native GIF87a/89a LZW decoder), and `AudioAsset` / `MemoryAudioAsset`. |
| `include/nisaba/lottie/diagnostics.hpp`<br>`src/lottie/diagnostics.cpp` | Diagnostic `Logger` / `CallbackLogger`, build performance `Stats`, and `PropertyObserver` binding memory-safe O(1) runtime handles (`ColorHandle`, `OpacityHandle`, `TransformHandle`, `TextHandle`). |
| `include/nisaba/lottie/external_layer.hpp` | Host-rendered `ExternalLayer` / `CallbackExternalLayer` and `PrecompInterceptor` / `CallbackPrecompInterceptor` for embedding live widgets or video inside Lottie layer hierarchies. |
| `include/nisaba/lottie/expression.hpp` | Pluggable `ExpressionManager` / `CallbackExpressionManager` and typed `ExpressionEvaluator<T>` interfaces (`float`, `std::string`, `std::vector<float>`) alongside built-in fast expressions. |
| `include/nisaba/lottie/audio.hpp` | Synchronized `AudioTrack` / `CallbackAudioTrack` playback hook and decibel-to-linear gain conversion (`audio_db_to_linear_gain`) for `LayerType::Audio`. |
| `include/nisaba/lottie/property.hpp`<br>`src/lottie/property.cpp` | Generic animatable `Property<T>`, temporal cubic Bezier easing solver, spatial cubic Bezier arc-length evaluator, built-in + pluggable expression evaluation, `BezierData` morphing, and 2D/3D `LottieTransform`. |
| `include/nisaba/lottie/effects.hpp`<br>`src/lottie/effects.cpp` | Layer effects parser and unified CPU/GPU `ImageFilter` & `ColorEffect` pipeline (Color/Tone, Transitions/Tiling, Distortion/Blur/Procedural) with compiled C++20 CPU kernels and GLSL GPU shaders. |
| `include/nisaba/lottie/shape.hpp`<br>`src/lottie/shape.cpp` | Vector geometry generators (`sh`, `rc`, `el`, `sr`), painting styles (`fl`, `st`, `gf`, `gs`), path modifiers (`tm`, `mm`, `rd`, `op`, `pb`, `zz`, `rp`), root/group shape rendering, bounds calculation, and `RenderContext`. |
| `include/nisaba/lottie/layer.hpp`<br>`src/lottie/layer.cpp` | Layer hierarchy (`PrecompLayer`, `SolidLayer`, `ImageLayer`, `NullLayer`, `ShapeLayer`, `TextLayer`, `AudioLayer`), vector/feathered `Mask`, `Track Mattes`, `TextDocument` / multi-selector `TextAnimator` / cubic Bezier `TextPathOptions`, per-layer bounds & fingerprinting, and 3D perspective compositing. |
| `include/nisaba/lottie/animation.hpp`<br>`src/lottie/animation.cpp` | Root `Animation` document, fluent `Animation::Builder` (`AnimationBuilder`), `.lottie` ZIP extraction, `LottieAssetRegistry`, deferred asset warming, incremental damage tracking, dynamic overrides/slots, audio dispatch, and hit-testing. |
| `include/nisaba/lottie/player.hpp`<br>`src/lottie/player.cpp` | Interactive playback state machine (`Player`), damage-tracked `seek_frame` / `advance`, marker segments, loop modes (`Once`, `Loop`, `Bounce`), synchronized `AudioTrack` hook, and lifecycle callbacks. |

---

## 2. Document Ingestion, Builder & Resource Loading

### 2.1 Fluent `Animation::Builder` (`AnimationBuilder`)
Documents can be loaded either through the legacy one-line helpers (`Animation::load_from_file`, `Animation::load_from_data`, `Animation::load_from_bytes`) or through the fluent `AnimationBuilder`:
- `set_flags(uint32_t flags)`: Configure loader behavior (e.g., `kDeferImageLoading`).
- `set_resource_provider(std::shared_ptr<ResourceProvider>)`: Route external image, font, and audio asset resolution through custom storage (memory, VFS, network, or encrypted bundles).
- `set_logger(std::shared_ptr<Logger>)`: Receive parse errors and unsupported feature diagnostics during load.
- `set_stats(Stats*)`: Capture `json_parse_ms`, `scene_build_ms`, `animator_count`, `layer_count`, `json_size_bytes`, `precomp_count`, and `image_asset_count`.
- `set_property_observer(std::shared_ptr<PropertyObserver>)`: Bind direct O(1) runtime handles to colors, opacities, transforms, and text documents during scene construction.
- `set_precomp_interceptor(std::shared_ptr<PrecompInterceptor>)`: Intercept precomp layers by ID or name to mount live `ExternalLayer` renderers.
- `set_expression_manager(std::shared_ptr<ExpressionManager>)`: Attach custom runtime evaluators for arbitrary `"x"` expression scripts.

### 2.2 JSON (`.json`) & dotLottie (`.lottie`) Containers
- **JSON (`.json`):** Parsed via `nisaba::json::JsonValue::parse(std::string_view)`. Supports RFC 8259 JSON, `//` and `/* ... */` comments, and `\uXXXX` escape sequences (including UTF-16 surrogate pairs).
- **dotLottie (`.lottie`):** Automatically detected via ZIP headers (`PK\x03\x04` / EOCD `0x06054b50`), supporting Store (`0`) and DEFLATE (`8`) compression via `nisaba::image::deflate_decompress`. Resolves `manifest.json` and exposes packaged assets directly through an internal archive resource provider without touching disk.

### 2.3 `ResourceProvider`, Deferred Decoding & Multi-Frame `ImageAsset`
- **`ResourceProvider` Implementations:**
  - `FileResourceProvider`: Loads assets relative to a base directory.
  - `MemoryResourceProvider`: Serves pre-registered byte buffers, `ImageAsset`, and `AudioAsset` instances entirely from RAM.
  - `DataURIResourceProviderProxy`: Wraps any provider to transparently decode inline RFC 2397 `data:...;base64,...` image and font URIs.
- **Deferred Image Loading (`kDeferImageLoading`):** Defers pixel decoding during `AnimationBuilder::make(...)`. `ImageAsset` payloads remain undecoded until their owning `ImageLayer` becomes active during `seek_frame(...)`, minimizing startup latency and memory footprint for long multi-scene animations.
- **Multi-Frame `ImageAsset` (`DecodedImageAsset`):**
  - Decodes static images (`PNG`, `JPEG`, `WebP`, `QOI`, `BMP`, `PPM`, `HDR`), animated `WebP` (`nisaba::image::decode_webp_animation`), and animated `GIF87a` / `GIF89a` (sovereign LZW + disposal + transparency decoder).
  - Supports custom procedural or video frame sequences via `DecodedImageAsset::from_frames(...)`.
  - Exposes `get_frame(float t_seconds)` and `frame_token(float t_seconds)` so multi-frame image changes automatically invalidate dirty regions in `InvalidationController`.

---

## 3. Property System, Interpolation & Expressions

### 3.1 `Property<T>` Evaluation
`Property<T>` supports `FloatProperty`, `PointProperty`, `ColorProperty`, `ShapeProperty` (`BezierData`), and `FloatArrayProperty`. Sequential playback resolves keyframes in amortized O(1) time via a mutable cursor hint (`last_kf_hint_`), falling back to `std::upper_bound` on random seeks.

### 3.2 Temporal, Spatial & Shape Interpolation
- **Temporal Cubic Bezier Easing:** `solve_bezier_easing(x, out_h, in_h)` solves $B_x(t) = x$ with Newton-Raphson refinement and bisection fallback, supporting hold keyframes (`"h": 1`).
- **Spatial Cubic Bezier Motion Paths:** Keyframes with `"to"` and `"ti"` tangents compute a 16-segment arc-length LUT along $\mathbf{P}(u)$ for constant-speed motion and analytical auto-orient tangents $\mathbf{P}'(u)$.
- **Shape Morphing (`BezierData`):** Interpolates vertices (`"v"`), incoming tangents (`"i"`), and outgoing tangents (`"o"`), with winding reversal when `"d": 3`.

### 3.3 Built-in Fast Expressions & Pluggable `ExpressionManager`
Every animatable property preserves its raw `"x"` script and evaluates in strict priority order:
1. **Live runtime overrides / O(1) handles** (highest priority)
2. **Pluggable `ExpressionManager` evaluator** (when bound via `AnimationBuilder::set_expression_manager`):
   - `create_number_expression_evaluator(expr)` $\rightarrow$ `ExpressionEvaluator<float>`
   - `create_array_expression_evaluator(expr)` $\rightarrow$ `ExpressionEvaluator<std::vector<float>>` (2D/3D points, scale, RGBA colors, float arrays)
   - `create_string_expression_evaluator(expr)` $\rightarrow$ `ExpressionEvaluator<std::string>` (dynamic `TextLayer` `"t.d.x"` scripts)
   - Returning `nullptr` falls back cleanly to built-in fast expressions and keyframes.
3. **Built-in zero-overhead fast expressions (`LottieExpression`):**
   - `loopOut("cycle")`, `loopOut("pingpong")`, `loopOut("offset")`
   - `loopIn("cycle")`, `loopIn("pingpong")`
   - `wiggle(freq, amp)`
   - Inertial bounce damped harmonic oscillation (`amp`, `freq`, `decay`)

---

## 4. Transforms (2D & 3D)

- **2D Affine (`LottieTransform::matrix`):** Evaluates anchor $T(-a_x, -a_y)$, scale $S(s_x/100, s_y/100)$, skew (`"sk"`, `"sa"`), rotation (`"r"` + optional `auto_orient`), and position (`"p"` or split `"px"`/`"py"`).
- **3D Perspective (`LottieTransform::matrix_3d`):** When `"ddd": 1` is active on a layer or ancestor, evaluates 3D anchor, scale, orientation (`"or"`), and Euler rotations (`"rx"`, `"ry"`, `"rz"`) into a `nisaba::Transform4x4` with centered perspective projection composited via `ICanvas::draw_pixmap_3d`.

---

## 5. Vector Shapes, Styles & Path Modifiers

All shape items inherit from `LottieShapeItem` with visibility (`"hd"`), winding (`"d"`), per-item `BlendMode` (`"bm"`), and static path caching (`cached_static_path`). Both grouped (`"gr"`) and direct root-level shape arrays inside `"shapes": [...]` are supported.

- **Geometry Primitives:** `PathShape` (`"sh"`), `RectShape` (`"rc"`), `EllipseShape` (`"el"`), `StarShape` (`"sr"`, polystar & polygon with inner/outer roundness).
- **Painting Styles:** `FillShape` (`"fl"`), `StrokeShape` (`"st"` with animated dashes `"d"`), `GradientFillShape` (`"gf"`), and `GradientStrokeShape` (`"gs"`) supporting linear, radial, and two-point conical focal gradients with cross-interpolated RGB + alpha stops.
- **Path Modifiers:**
  - `TrimPathShape` (`"tm"`, `Simultaneously` & `Individually`)
  - `MergePathsShape` (`"mm"`, Merge / Union / Difference / Intersect / Xor)
  - `RoundCornersShape` (`"rd"`)
  - `OffsetPathShape` (`"op"`)
  - `PuckerBloatShape` (`"pb"`)
  - `ZigZagShape` (`"zz"`, corner & smooth cubic wave)
  - `RepeaterShape` (`"rp"`, cumulative transforms, start/end opacity, Above/Below compositing)

---

## 6. Layer Hierarchy, Compositing, Effects, Text & Audio

### 6.1 Layer Types
| `"ty"` | Class | Description |
| :---: | :--- | :--- |
| `0` | `PrecompLayer` | Nested composition (`"refId"`, clip `"w"`/`"h"`, `"sr"`, `"st"`, `"tm"`) or host-intercepted `ExternalLayer`. |
| `1` | `SolidLayer` | Rectangular solid fill (`"sw"`, `"sh"`, `"sc"`). |
| `2` | `ImageLayer` | Static or multi-frame `LottieImageAsset` rendered via `ICanvas::draw_pixmap_rect` with bilinear filtering. |
| `3` | `NullLayer` | Non-rendering transform parent controller. |
| `4` | `ShapeLayer` | Evaluates and renders root-level and grouped `LottieShapeItem` trees. |
| `5` | `TextLayer` | Evaluates `TextDocument` keyframes/expressions, BiDi/Arabic shaping, multi-selector `TextAnimator`, and cubic Bezier `TextPathOptions`. |
| `6` | `AudioLayer` | Synchronized audio timeline layer (`"refId"`, animated stereo `"au.lv"` dB keyframes) driving `AudioTrack`. |

### 6.2 Pre-Linked Hierarchy, Masks & Track Mattes
- `link_layer_hierarchy` resolves `parent_ptr` and `matte_layer_ptr` once at load time for O(1) zero-allocation traversal.
- **Masks (`"masksProperties"`):** `Add`, `Subtract`, `Intersect`, `Difference`, `None`, inverted masks (`"inv"`), expansion (`"x"`), and 2D Gaussian feathering (`"f"`). Pure vector masks use direct `ICanvas::clip_path`.
- **Track Mattes (`"tt"` / `"td"` / `"tp"`):** `Alpha`, `InvertedAlpha`, `Luma` (BT.709), and `InvertedLuma`.

### 6.3 External Precomp Layers (`ExternalLayer` & `PrecompInterceptor`)
Host applications can intercept any Precomp layer by asset ID or layer name via `AnimationBuilder::set_precomp_interceptor(...)` and return an `ExternalLayer` (or `CallbackExternalLayer`). The external renderer draws directly onto the active `ICanvas` (CPU or GPU) while inheriting the Precomp's full transform chain, clipping bounds, opacity, blend mode, masks, track mattes, layer effects, and dirty-region tracking.

### 6.4 Layer Effects (`"ef"`) — Unified CPU & GPU Pipeline
Layer effects are built into composable `nisaba::effects::ImageFilter` / `ColorEffect` DAGs in `src/lottie/effects.cpp` and routed natively through `ICanvas::save_layer_bounds` with tight per-layer bounds (and zero CPU readback on GPU). All shader-based effects pair a cached GLSL GPU shader with a compiled native C++20 CPU kernel (`CpuRuntimeFilterFn`) for 1:1 CPU/GPU pixel parity at real-time frame rates:

- **Color & Tone Effects:**
  - `Fill`, `Tint`, `Tritone`, `CCToner` (Duotone / Tritone / Pentone)
  - `BrightnessContrast`, `HueSaturation` (Master & Colorize HSLA matrix)
  - `BlackAndWhite` (6-band color channel weighting + optional tint)
  - `Invert` (RGB, single channel, Alpha, and HLS channel inversion + blend with original)
  - `Threshold`, `ShiftChannels` (channel remapping & BT.709 luminance)
  - `EasyLevels` & `ProLevels` (per-channel input/output black/white & gamma LUT)
  - `GradientRamp` (linear & radial ramp; includes a zero-FBO direct hardware gradient fast path on `SolidLayer`) & `FourColorGradient`
- **Transition & Tiling Effects:**
  - `LinearWipe` (completion, angle, feather)
  - `VenetianBlinds` (completion, direction, width, feather)
  - `MotionTile` (tile center, tile/output dimensions, mirror edges, horizontal/vertical phase shift)
  - `TransformEffect` (effect-level anchor, position, scale, rotation, skew, skew axis, opacity)
- **Distortion, Blur & Procedural Effects:**
  - `GaussianBlur`, `DropShadow`
  - `CornerPin` (projective 4-point homography via `Transform4x4::from_rect_to_quad`)
  - `Bulge` & `Sphere` (radial lens magnification & spherical refraction)
  - `DirectionalBlur` (oriented multi-tap motion blur) & `Sharpen` (convolution kernel)
  - `FractalNoise` (multi-octave deterministic procedural noise with contrast, brightness, complexity, sub-influence/scaling, evolution, and invert)

### 6.5 Advanced Typography, Multi-Selector `TextAnimator` & Text-on-Path
- **Cached Layout & Path Geometry:** `TextLayer` caches resolved fonts, BiDi runs, shaped glyph clusters, extracted glyph vector `Path`s, and static cubic Bezier `PathMeasure` tables across frames, eliminating redundant shaping and curve flattening during animations.
- **BiDi & Arabic Shaping:** Directional segmentation (`Bidi::segment_runs`) and contextual Arabic glyph joining including Lam-Alif ligatures (`Bidi::shape_text`).
- **Multi-Selector `TextAnimator` (`"t.a"`):**
  - Supports all four Lottie selection domains (`"b"`): `1` Characters, `2` Characters excluding spaces, `3` Words, and `4` Lines.
  - Supports percentage (`"r": 1`) and index (`"r": 2`) units, `start` (`"s"`), `end` (`"e"`), `offset` (`"o"`), `smoothness` (`"sm"`), `max_amount` (`"a"`), `max_ease` (`"xe"`), `min_ease` (`"ne"`), and shapes (`Square`, `Ramp Up`, `Ramp Down`, `Triangle`, `Round`, `Smooth`).
  - Supports multiple selectors per animator combined via `"m"` (`1` Add, `2` Subtract, `3` Intersect, `4` Min, `5` Max, `6` Difference).
  - Animates per-glyph position, scale, rotation, opacity, fill color, stroke color, stroke width, and tracking.
- **Text-on-Path (`TextPathOptions`, `"t.p"`):** Lays text out along any referenced cubic Bezier mask path (`"m"`, using `MaskMode::None`) via `nisaba::PathMeasure::get_pos_tan`, supporting animated `first_margin` (`"f"`), `last_margin` (`"l"`), `perpendicular_to_path` (`"p"`), `reverse_path` (`"r"`), and `force_alignment` (`"a"`).

### 6.6 Synchronized Audio (`AudioLayer` & `AudioTrack`)
- `AudioLayer` (`"ty": 6`) parses `"refId"` and animated stereo decibel levels (`"au.lv"`), converting dB to linear amplitude via `audio_db_to_linear_gain(db)` (`0 dB` $\rightarrow 1.0$, $\le -96\text{ dB} \rightarrow 0.0$).
- Attaching an `AudioTrack` (or `CallbackAudioTrack`) via `Player::set_audio_track(...)` delivers synchronized `on_audio_frame(asset_id, local_time_seconds, left_db, right_db, linear_gain, is_playing, audio_asset)` callbacks during `advance()`, `seek_frame()`, and `seek_progress()`.
- Active audio layer state is tracked so `on_audio_stop(asset_id)` fires cleanly on active-to-inactive transitions, playback completion, or `Player::stop()`.

---

## 7. Damage Tracking, Diagnostics, O(1) Handles & Hit-Testing

### 7.1 Dirty-Region Tracking (`InvalidationController`)
- Calling `Animation::seek_frame(frame, &ic)` or `Player::advance(dt, &ic)` compares each layer's active state, complete visual bounds (including 3D projection, effect halos, and AA margin), content fingerprint, and property-handle mutation generation against the previous frame.
- Only changed layers union their previous and current bounds into `InvalidationController`.
- `ic.to_screen_int_rect(w, h)` and `ic.to_screen_int_rect_fitted(w, h, comp_w, comp_h, dest_rect, preserve_aspect)` produce exact integer damage boxes ready to pass directly into `GpuCanvas::flush(damage_rect)` or compositor partial redraws.

### 7.2 O(1) Property Handles (`PropertyObserver`) & Keypath/Slot Overrides
- **`PropertyObserver` Handles:** During load, `on_color_property`, `on_opacity_property`, `on_transform_property`, and `on_text_property` yield typed `ColorHandle`, `OpacityHandle`, `TransformHandle`, and `TextHandle` objects backed by `std::weak_ptr` lifetime guards and atomic mutation counters (automatically dirtying `InvalidationController` on mutation with zero string lookups).
- **Dynamic Keypath & Slot Overrides:** `set_color_override`, `set_scalar_override`, `set_point_override`, `set_text_override`, `set_slot_color`, and `clear_overrides` remain available for wildcard keypath and Lottie Slot (`"sid"`) theming.

### 7.3 Hit-Testing
`Animation::hit_test(x, y, frame)` and `Player::hit_test(x, y)` traverse active layers top-to-bottom in local coordinates against vector paths, solids, images, text bounds, and intercepted external precomps.

### 7.4 End-to-End C++20 Usage Example

```cpp
#include "nisaba/lottie/lottie.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"

using namespace nisaba;
using namespace nisaba::lottie;

struct MyObserver final : public PropertyObserver {
    std::unique_ptr<ColorHandle> accent_fill;
    void on_color_property(std::string_view keypath, const std::function<std::unique_ptr<ColorHandle>()>& make_handle) override {
        if (keypath == "Badge.Grp.Fill") accent_fill = make_handle();
    }
};

void run_lottie_example(gpu::GpuCanvas& gpu_canvas, uint32_t w, uint32_t h) {
    auto resources = std::make_shared<MemoryResourceProvider>(/*defer_decode=*/true);
    auto observer  = std::make_shared<MyObserver>();
    auto logger    = std::make_shared<CallbackLogger>([](Logger::Level, std::string_view msg, std::string_view) {
        // Handle load warnings or diagnostics
        (void)msg;
    });
    Stats stats{};

    auto interceptor = std::make_shared<CallbackPrecompInterceptor>(
        [](std::string_view id, std::string_view, Size sz) -> std::shared_ptr<ExternalLayer> {
            if (id != "live_widget") return nullptr;
            return std::make_shared<CallbackExternalLayer>([sz](ICanvas& c, float t_sec) {
                (void)t_sec;
                if (auto r = Rect::from_xywh(0.0f, 0.0f, sz.width(), sz.height())) {
                    c.fill_round_rect(*r, 12.0f, 12.0f, Paint(Color::from_rgba8(0, 220, 190, 255)));
                }
            });
        }
    );

    auto anim = AnimationBuilder()
        .set_flags(kDeferImageLoading)
        .set_resource_provider(resources)
        .set_property_observer(observer)
        .set_precomp_interceptor(interceptor)
        .set_logger(logger)
        .set_stats(&stats)
        .make_from_file("assets/scene.lottie");
    if (!anim) return;

    // O(1) runtime color update without keypath string lookups
    if (observer->accent_fill && observer->accent_fill->is_valid()) {
        observer->accent_fill->set(Color::from_rgba8(255, 195, 35, 255));
    }

    Player player(anim);
    player.set_loop_mode(LoopMode::Loop);
    player.set_audio_track(std::make_shared<CallbackAudioTrack>(
        [](std::string_view id, float local_sec, float l_db, float r_db, float gain, bool playing, const std::shared_ptr<AudioAsset>&) {
            // Feed synchronized audio parameters to host mixer
            (void)id; (void)local_sec; (void)l_db; (void)r_db; (void)gain; (void)playing;
        }
    ));

    // Incremental damage-tracked frame advance & GPU partial flush
    InvalidationController ic;
    player.advance(1.0f / 60.0f, &ic);
    if (auto dmg = ic.to_screen_int_rect(w, h)) {
        gpu_canvas.save();
        gpu_canvas.clip_rect(*dmg);
        gpu_canvas.clear_rect(*dmg, Color::TRANSPARENT);
        player.render(gpu_canvas);
        gpu_canvas.restore();
        gpu_canvas.flush(*dmg);
    }
}
```

---

## 8. Interactive Showcases & Verification

Two synchronized 6-card interactive showcases (`examples/showcase_lottie_gpu.cpp` for direct hardware GPU rendering and `examples/showcase_lottie.cpp` for CPU software reference rendering, sharing `examples/lottie_showcase_scenes.hpp`) exercise all subsystem capabilities live:
- Card 1: `InvalidationController` live dirty-rect overlay, `PropertyObserver` O(1) handle mutation, and multi-frame `DecodedImageAsset` with `kDeferImageLoading`.
- Card 2: `ExternalLayer` & `PrecompInterceptor` rendering a live animated widget inside a Lottie layer hierarchy.
- Card 3: Color & Tone effect pipeline (`GradientRamp`, `HueSaturation`, `FourColorGradient`, `CCToner`, `EasyLevels`, `BlackAndWhite`, `Invert`).
- Card 4: Transitions & Tiling (`LinearWipe`, `VenetianBlinds`, `MotionTile`, `TransformEffect`).
- Card 5: Distortion, Blur & Procedural (`Bulge`, `Sphere`, `CornerPin`, `DirectionalBlur`, `FractalNoise`).
- Card 6: `ExpressionManager`, multi-selector `TextAnimator`, cubic Bezier Text-on-Path, and live `AudioLayer` stereo VU telemetry.
