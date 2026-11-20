# Nisaba Text Engine (`nisaba::text`) — Technical Reference

## 1. Architecture Overview

`nisaba::text` is the sovereign typography, font parsing, OpenType shaping, variable font interpolation, bidirectional paragraph layout, incremental update, caching, and rendering subsystem of the Nisaba engine. It operates with zero external font or shaping dependencies, compiles cleanly under `-fno-rtti`, and maintains 1:1 visual parity across both `nisaba::Canvas` (CPU scanline/mask & vector rasterization) and `nisaba::gpu::GpuCanvas` / `nisaba::gpu::Context` (OpenGL ES 3.2 / Vulkan hardware-accelerated glyph atlas and vector path rendering).

### Subsystem Structure

| Header / Source | Responsibility |
| :--- | :--- |
| `include/nisaba/text/glyph.hpp` | Subpixel quantization (`SubpixelBin`, `compute_subpixel_bin`), `CacheKey` hashing (including `style_flags` and `variation_hash`), and `CachedGlyph` alpha mask representation. |
| `include/nisaba/text/attrs.hpp`<br>`src/text/attrs.cpp` | Typographic attributes (`TextColor`, `Family`, `Weight`, `Style`, `Stretch`, `Metrics`, `DecorationStyle`, `DecorationMode`, `TextShadow`, `FontFeature`, `FontVariation`, `hash_font_variations`, `Attrs`), rich `Paint` / `Stroke` span references, and range-compacting `AttrsList` (`AttrsSpan`, `apply_range_modifier`). |
| `include/nisaba/text/cff_font.hpp`<br>`src/text/cff_font.cpp` | PostScript `CFF ` (Compact Font Format v1) and `CFF2` parser and Type 2 Charstring stack virtual machine. |
| `include/nisaba/text/opentype_tables.hpp`<br>`src/text/opentype_tables.cpp` | OpenType layout tables: `OpenTypeGdef` (glyph class definitions & mark attachment), `OpenTypeGsub` (`ScriptList`/`LangSys` resolution, BCP-47 to OpenType language tag mapping, single, multiple, alternate with 1-based index selection, ligature, contextual, and extension substitutions), and `OpenTypeGpos` (pair kerning, mark-to-base, and mark-to-mark positioning). |
| `include/nisaba/text/ttf_font.hpp`<br>`src/text/ttf_font.cpp` | SFNT container & TTC/OTC collection parser (`TTF` / `OTF` / `TTC`), POSIX `mmap` zero-copy file mapping (`MappedFontData`), fast header-only probing (`probe_face_header`), `cmap` Format 4/12/14 character mapping, `hmtx`/`hhea`/`post`/`OS/2` metrics & 128-bit `ulUnicodeRange` bitmasks, `glyf`/`loca` quadratic outlines, `COLR`/`CPAL` layered color glyphs, full Variable Font interpolation (`fvar`, `avar`, `gvar` with TrueType IUP, `HVAR`), `kern` binary search + direct-mapped kerning cache, and synthetic bold/italic outline transformations. |
| `include/nisaba/text/linux_font_config.hpp`<br>`src/text/linux_font_config.cpp` | Sovereign zero-dependency Linux Fontconfig XML & environment parser (`/etc/fonts/fonts.conf`, `conf.d/*.conf`, `local.conf`, XDG font directories, metric-compatible family aliases, and locale-specific `<match>` overrides). |
| `include/nisaba/text/font_system.hpp`<br>`src/text/font_system.cpp` | System-integrated multi-font registry (`FontSystem`), automatic Linux font discovery (`scan_system_fonts`, `with_system_fonts`), persistent binary index caching (`~/.cache/nisaba/font_index_v1.bin`), lazy `mmap` instantiation, CSS Fonts Level 4 family/weight/style/stretch matching (`match_font`, `match_font_styled`, `match_font_chain_styled`), and ordered family chain + BCP-47 locale/Han unification & OS/2 script-aware fallback (`find_glyph_or_fallback`, `find_glyph_in_chain_or_fallback`). |
| `include/nisaba/text/bidi.hpp`<br>`src/text/bidi.cpp` | Bidirectional run analysis (`Bidi::segment_runs`), UAX #9 Rule L2 visual run reordering, Arabic/Persian/Urdu/Kurdish contextual joining (`Bidi::shape_text`), cluster-preserving RTL reordering, bracket mirroring (`Bidi::mirror_char`), `U+FFFC` neutral object handling, and UAX #29 grapheme cluster / word boundary segmentation. |
| `include/nisaba/text/layout.hpp`<br>`src/text/layout.cpp` | Layout data structures (`Wrap`, `Align`, `Overflow`, `TextDirection`, `PlaceholderAlignment`, `TextBaseline`, `PlaceholderStyle`, `PlaceholderRect`, `StrutStyle`, `TextHeightBehavior`, `LineMetrics`, `LayoutGlyph`, `LayoutLine`, `LayoutRun`, `GlyphRunInfo`). |
| `include/nisaba/text/glyph_cache.hpp`<br>`src/text/glyph_cache.cpp` | Subpixel- and variable-axis-aware glyph alpha mask cache (`GlyphCache`) with $O(1)$ LRU eviction. |
| `include/nisaba/text/buffer.hpp`<br>`src/text/buffer.cpp` | Paragraph engine (`BufferLine`, `Buffer`), multi-stage shaping & line-wrapping pipeline (`shape_and_layout`), intrinsic/line metrics, custom UTF-8 ellipsis truncation, inline placeholders, `StrutStyle`/`HalfLeading`/`TextHeightBehavior`, shared CPU/GPU decoration geometry (`build_run_decorations` with `DecorationMode::Gaps`), incremental zero-reshape in-place updates (`update_text_align`, `update_foreground_paint`, `update_background_paint`, `update_font_size`), glyph run visitor (`visit`), and multi-pass CPU rendering (`Buffer::draw`). |
| `include/nisaba/text/paragraph_cache.hpp`<br>`src/text/paragraph_cache.cpp` | LRU paragraph layout cache (`ParagraphCache` / `ParagraphLayoutCache`) keyed by `(text, attrs, spans, placeholders, metrics, width, ...)` via `get_or_layout` and `lookup_or_shape`. |
| `include/nisaba/text/cursor.hpp`<br>`src/text/cursor.cpp` | Interactive caret and selection controller (`Cursor`, `Selection`, `Motion`, `CursorController::hit_test`, `move`, `cursor_rect`, `selection_rects`). |
| `include/nisaba/text/baked_text.hpp`<br>`src/text/baked_text.cpp` | Pre-composited RGBA8888 text surface (`BakedText`) with direct SIMD scanline blitting (`draw_fast`). |
| `include/nisaba/text/text_cache.hpp`<br>`src/text/text_cache.cpp` | Hash-indexed LRU cache (`TextCache`) of `BakedText` entries keyed by `TextCacheKey`. |

---

## 2. Font Binary Ingestion, Variable Fonts & Outline Extraction

### 2.1 SFNT Table Directory (`TtfFont`)
`TtfFont::from_bytes` and `TtfFont::from_file` validate SFNT headers (`0x00010000`, `true`, `typ1`, and `OTTO` for OpenType/CFF) and populate a fixed-capacity stack array `TableRecord tables_[48]` without heap allocations for table directory indexing.

Supported SFNT tables:
- **Required Metrics & Header Tables:** `head`, `maxp`, `hhea`, `hmtx`, `cmap`, `name`, `OS/2`
- **Outline Tables:** `loca` + `glyf` (TrueType quadratic curves), `CFF ` + `CFF2` (PostScript cubic curves)
- **Advanced Layout Tables:** `GDEF`, `GSUB`, `GPOS`, `kern`
- **Color & Variable Font Tables:** `COLR`, `CPAL`, `fvar`, `avar`, `gvar`, `HVAR`

### 2.2 Character-to-Glyph Mapping (`cmap`)
`TtfFont::glyph_index(char32_t codepoint)` resolves Unicode codepoints through four tiers:
1. **Direct ASCII Lookup Table ($O(1)$):** Codepoints in $[0, 127]$ index directly into `uint16_t ascii_lut_[128]`, populated during initialization alongside `ascii_adv_lut_[128]`.
2. **Format 4 (BMP Segmented Mapping):** Binary search over `endCount` / `startCount` segments for codepoints in $[0x0080, 0xFFFF]$, supporting both `idDelta` arithmetic and `idRangeOffset` `glyphIdArray` indexing.
3. **Format 12 (Segmented Coverage):** Binary search over 32-bit `SequentialMapGroup` records (`startCharCode`, `endCharCode`, `startGlyphID`) for Supplementary Multilingual Plane (SMP / Emoji / CJK Extension) codepoints up to `0x10FFFF`.
4. **Format 14 (Unicode Variation Sequences):** `TtfFont::glyph_index_variation(char32_t codepoint, char32_t variation_selector)` binary-searches `VarSelectorRecord` entries (`varSelector` 24-bit uint24):
   - **Default UVS Table:** Validates `UnicodeValueRanges` (`startUnicodeValue` + `additionalCount`) and delegates to `glyph_index(codepoint)`.
   - **Non-Default UVS Table:** Binary-searches `UVSMapping` records (`unicodeValue` 24-bit $\to$ `glyphID` 16-bit) to return specialized variant glyphs.

### 2.3 TrueType Quadratic Outlines (`glyf` / `loca`)
- Supports both short (`indexToLocFormat == 0`, 16-bit word offsets) and long (`indexToLocFormat == 1`, 32-bit byte offsets) `loca` tables.
- **Simple Glyphs (`numContours >= 0`):** Decodes run-length encoded point flags (`ON_CURVE_POINT`, `X_SHORT_VECTOR`, `Y_SHORT_VECTOR`, `REPEAT_FLAG`, `X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR`, `Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR`) using stack buffers (`STACK_POINTS = 512`) for common glyphs. Consecutive off-curve quadratic control points automatically synthesize their implicit on-curve midpoint $\mathbf{m} = \frac{1}{2}(\mathbf{p}_i + \mathbf{p}_{i+1})$ and emit `PathVerb::Quad` segments.
- **Composite Glyphs (`numContours < 0`):** Recursively resolves component sub-glyphs (up to depth 16), applying component offsets (`ARGS_ARE_XY_VALUES`) and $2 \times 2$ linear transformations (`WE_HAVE_A_SCALE`, `WE_HAVE_AN_X_AND_Y_SCALE`, `WE_HAVE_A_TWO_BY_TWO`).
- Unscaled glyph paths (at scale $1.0$) are cached lazily per glyph ID in `glyph_path_cache_` with thread-safe `std::atomic<bool>` acquire/release flags.

### 2.4 PostScript CFF / CFF2 Outlines (`CffFont`)
When the SFNT container has tag `OTTO` or contains `CFF ` / `CFF2` tables:
- Parses `INDEX` structures (1- to 4-byte offset element sizes), `Top DICT`, `Private DICT`, `Global Subrs`, `Local Subrs`, and CID-keyed `FDSelect` (Formats 0, 3, 4) / `FDArray` font dictionaries.
- Evaluates Type 2 Charstring bytecode on an internal operand stack (max depth 513) and call stack (max recursion depth 10):
  - **Path Construction Operators:** `rmoveto` (`21`), `hmoveto` (`22`), `vmoveto` (`4`), `rlineto` (`5`), `hlineto` (`6`), `vlineto` (`7`), `rrcurveto` (`8`), `hhcurveto` (`27`), `vvcurveto` (`26`), `hvcurveto` (`31`), `vhcurveto` (`30`), `rcurveline` (`24`), `rlinecurve` (`25`), and `flex` / `hflex` / `hflex1` / `flex1` (`12 34`–`12 37`).
  - **Hinting & Stem Width Clearing:** `hstem`, `vstem`, `hstemhm`, `vstemhm`, `hintmask`, `cntrmask` (tracks cumulative stem count to compute exact bitmask byte lengths).
  - **Subroutine Operators:** `callsubr` (`10`) and `callgsubr` (`29`) with bias calculation ($107$ for $<1240$ subrs, $1131$ for $<33900$ subrs, $32768$ otherwise).
- Emits true cubic Bézier curves (`PathVerb::Cubic`) into `nisaba::Path`.

### 2.5 Color Glyphs (`COLR` / `CPAL`)
`TtfFont::get_color_glyph_layers(uint16_t glyph_id, std::vector<ColorGlyphLayer>& out_layers, uint16_t palette_index)` parses `COLR` v0 and `CPAL` tables:
1. Binary-searches `BaseGlyphRecord` array (`glyphID`, `firstLayerIndex`, `numLayers`).
2. Iterates `LayerRecord` entries (`layerGlyphID`, `paletteIndex`).
3. Maps `paletteIndex` through `CPAL` `colorRecordIndices[palette_index]` to `ColorRecord` (`BGRA` byte order $\to$ `TextColor(r, g, b, a)`), or sets `use_foreground = true` when `paletteIndex == 0xFFFF`.

### 2.6 Variable Font Interpolation (`fvar`, `avar`, `gvar`, `HVAR`)
`TtfFont` provides native OpenType Variable Font support for arbitrary design axes (such as `wght`, `wdth`, `slnt`, `opsz`) across both Latin and Arabic/multilingual variable fonts:
- **Axis Metadata (`fvar`):** `TtfFont::variation_axes()` exposes `VariationAxis{tag, min_value, default_value, max_value}` decoded from 16.16 fixed-point values, and `is_variable_font()` reports axis availability.
- **Coordinate Normalization (`avar`):** `TtfFont::normalize_variation_coords(std::span<const FontVariation>)` clamps user-space axis values to `[min_value, max_value]`, normalizes linearly into `[-1.0, 1.0]`, and applies non-linear `avar` piecewise `AxisValueMap` segment remapping.
- **Horizontal Advance Variation (`HVAR` / `gvar` phantom points):** `TtfFont::glyph_advance_varied(glyph_id, font_size, variations)` evaluates `HVAR` `ItemVariationStore` (`DeltaSetIndexMap` + `VariationRegionList` region scalars + `ItemVariationData` 8/16-bit deltas) to compute exact varied horizontal advances, falling back to `gvar` phantom point deltas when `HVAR` is absent.
- **Varied Outline Extraction (`gvar` + TrueType IUP):** `TtfFont::get_glyph_path_varied(glyph_id, out_path, font_size, variations, synthetic_bold, synthetic_italic)` decodes shared and private tuple variation headers, packed point numbers, packed delta runs, and region scalars, then performs full per-contour TrueType **Interpolation of Untouched Points (IUP)** for simple glyphs and recursive varied component positioning for composite glyphs.
- When a `wght` axis coordinate is explicitly supplied on a variable font, synthetic bold expansion is automatically suppressed in favor of true variable outline weight interpolation.

### 2.7 Synthetic Bold & Synthetic Italic (`get_glyph_path_styled`)
When a requested weight ($\ge 600$, without an active variable `wght` axis) or style (`Style::Italic` / `Style::Oblique`) is not natively present in the matched font face:
- **Synthetic Bold:** Computes stroke width $w = \max(0.75, 0.045 \times \text{font\_size})$ with `LineJoin::Round` and `LineCap::Round`, converts the stroke envelope via `Path::stroke_to_fill`, and appends it to the base glyph outline.
- **Synthetic Italic:** Applies a horizontal shear matrix `Transform::from_skew(-0.2125f, 0.0f)` ($\tan 12^\circ \approx 0.2125$) in screen space.

---

## 3. OpenType Layout Tables (`GDEF`, `GSUB`, `GPOS`)

### 3.1 Glyph Definition Table (`OpenTypeGdef`)
- Parses `GlyphClassDef` (Format 1 array and Format 2 binary-searched `ClassRangeRecord` intervals), classifying glyphs into:
  - `GlyphClass::Unclassified` (`0`), `Base` (`1`), `Ligature` (`2`), `Mark` (`3`), `Component` (`4`)
- Evaluates `LookupFlag` bitmasks (`IgnoreBaseGlyphs = 0x0002`, `IgnoreLigatures = 0x0004`, `IgnoreMarks = 0x0008`) via `OpenTypeGdef::should_skip_glyph(uint16_t glyph_id, uint16_t lookup_flag)`.

### 3.2 Glyph Substitution Table (`OpenTypeGsub`)
- Parses `ScriptList` / `LangSys`, `FeatureList`, and `LookupList`:
  - **`OpenTypeGsub::bcp47_to_ot_lang_tag(std::string_view)`:** Maps BCP-47 locale tags (e.g., `"ar"`, `"ur"`, `"fa"`, `"ro"`, `"ca"`, `"ja"`, `"zh-Hant"`, `"zh-Hans"`, `"ko"`, `"tr"`, `"sr"`, `"bg"`) to 4-byte OpenType `LangSys` tags (`'ARA '`, `'URD '`, `'FAR '`, `'ROM '`, `'CAT '`, `'JAN '`, `'ZHT '`, `'ZHS '`, `'KOR '`, etc.).
  - **`OpenTypeGsub::apply_features_for_script(glyphs, features, script_tag, lang_tag)`:** Filters lookups through the matching `ScriptRecord` (`'latn'`, `'arab'`, `'hebr'`, `'cyrl'`, `'grek'`, `'hani'`, `'kana'`, `'hang'`, `'DFLT'`) and `LangSys` (`reqFeatureIndex` + `featureIndices`), respecting explicit feature values (`0` = disabled, `>= 1` = enabled / 1-based alternate index).
  - Also provides `apply_features`, `apply_single_glyph_feature(gid, feature_tag, script_tag, lang_tag, feature_value)`, and `apply_ligatures`.
- Supported lookup types:
  - **LookupType 1 (Single Substitution):** Format 1 (`deltaGlyphID` modulo 65536) and Format 2 (`substituteGlyphIDArray` indexed by Coverage index).
  - **LookupType 2 (Multiple Substitution):** Format 1 (`Sequence` table expanding 1 glyph into $N$ glyphs).
  - **LookupType 3 (Alternate Substitution):** Format 1 (`AlternateSet` selecting alternate `(feature_value - 1)` clamped to available alternates).
  - **LookupType 4 (Ligature Substitution):** Format 1 (`LigatureSet` $\to$ `Ligature` table). Uses `should_skip_glyph` with `lookup_flag` (and `GDEF` mark classification) to match base glyph sequences even when combining marks intervene, placing the substituted ligature at the first component position while preserving intervening marks after the ligature.
  - **LookupType 7 (Extension Substitution):** 32-bit `extensionOffset` unwrapping to underlying subtables.
- Built-in and user-selectable OpenType features: `locl` (automatic when `locale` is set), `isol`, `init`, `medi`, `fina`, `rlig`, `liga`, `clig`, `calt`, `ccmp`, `dlig`, `tnum`, `pnum`, `zero`, `smcp`, `c2sc`, `frac`, `sups`, `subs`, `ss01`–`ss20`, etc.

### 3.3 Glyph Positioning Table (`OpenTypeGpos`)
`OpenTypeGpos` lazily indexes lookup subtables into flat descriptors (`PairSubtable`, `MarkSubtable`) on first query:
- **LookupType 1 (Single Adjustment):** Formats 1 and 2 (`ValueRecord` adjustments).
- **LookupType 2 (Pair Adjustment — `kern`):**
  - **Format 1 (Glyph Pair Adjustments):** Binary search on Coverage (Format 1 or 2) followed by binary search on `PairValueRecord` array using `stride = 2 + len1 + len2`.
  - **Format 2 (Class Pair Adjustments):** Binary search on `ClassDef1` and `ClassDef2` followed by direct $O(1)$ offset indexing into `Class1Record[c1].Class2Record[c2]`.
- **LookupType 4 (Mark-to-Base Attachment — `mark`):**
  - Resolves `markCoverage` index $\to$ `MarkRecord` (`markClass`, `markAnchorOffset`) and `baseCoverage` index $\to$ `BaseRecord` (`baseAnchorOffsets[markClass]`).
  - Computes displacement $(\Delta x, \Delta y) = (X_{\text{base}} - X_{\text{mark}}, Y_{\text{base}} - Y_{\text{mark}})$ in font design units.
- **LookupType 6 (Mark-to-Mark Attachment — `mkmk`):**
  - Resolves `mark1Coverage` (combining mark) and `mark2Coverage` (base/preceding mark) anchors, computing stacked diacritic displacement $(\Delta x, \Delta y) = (X_{\text{mark2}} - X_{\text{mark1}}, Y_{\text{mark2}} - Y_{\text{mark1}})$.
- **LookupType 9 (Extension Positioning):** 32-bit offset unwrapping for large `GPOS` tables.

### 3.4 Two-Tier Kerning Cache (`TtfFont::get_kerning`)
`TtfFont::get_kerning(uint16_t left_glyph, uint16_t right_glyph)` executes in $O(1)$ on repeated pairs:
1. Forms 32-bit key `key = (uint32_t(left_glyph) << 16) | uint32_t(right_glyph)`.
2. Hashes into a 2048-entry direct-mapped cache `kern_fast_cache_` via Knuth multiplicative hashing `((key * 2654435761u) >> 21) & 2047`.
3. On cache hit (`slot.key == key`), returns `slot.value` inline from `ttf_font.hpp`.
4. On cache miss (`compute_kerning_slow`), queries `OpenTypeGpos::get_kerning` first, falls back to binary search over sorted legacy `kern` Format 0 pairs (`kerning_pairs_`), updates the cache slot, and returns the design-unit adjustment.

---

## 4. Bidirectional Analysis, Script Shaping & Segmentation (`Bidi`)

### 4.1 Bidirectional Run Segmentation & UAX #9 Visual Reordering
- `Bidi::detect_base_direction(std::string_view text)` scans for the first strong directional character (`CharType::LTR` vs. `CharType::RTL` / `CharType::ArabicLetter`).
- `Buffer::set_text_direction(TextDirection)` supports explicit paragraph base direction (`TextDirection::Auto`, `LTR`, `RTL`) independent of horizontal `Align`.
- `Bidi::segment_runs(std::string_view text, ...)` classifies UTF-8 codepoints into `LTR`, `RTL`, `ArabicLetter`, `EuropeanNumber`, `ArabicNumber`, `Whitespace`, and `Neutral` (including `U+FFFC` inline placeholder anchors so embedded boxes inherit surrounding LTR/RTL direction), resolving embedding levels (`level = 0` LTR, `level = 1` RTL, `level = 2` LTR/numbers embedded within RTL).
- `Bidi::shape_text` applies **UAX #9 Rule L2** visual run reordering across runs, and wrapped RTL paragraphs also apply per-line L2 word reordering so visual left-to-right ordering remains accurate across line wraps.

### 4.2 Arabic, Persian, Urdu & Kurdish Contextual Shaping
`Bidi::shape_text` determines contextual `ArabicForm` (`Isolated`, `Final`, `Initial`, `Medial`) for each character based on neighbor `JoiningType` (`NonJoining`, `RightJoining`, `DualJoining`, `Transparent`):
- **Transparent Marks:** Combining marks (`0x0610`–`0x061A`, `0x064B`–`0x065F`, `0x0670`, `0x06D6`–`0x06ED`) are skipped when inspecting preceding and succeeding joining neighbors so vocalization (`Tashkeel` / `Harakat`) does not break cursive letter joining.
- **Extended Character Table (`ARABIC_SHAPES`):** Covers standard Arabic (`0x0621`–`0x064A`) plus Persian, Urdu, and Kurdish letters (`پ 0x067E`, `ٹ 0x0679`, `چ 0x0686`, `ڈ 0x0688`, `ڑ 0x0691`, `ژ 0x0698`, `ک 0x06A9`, `گ 0x06AF`, `ں 0x06BA`, `ھ 0x06BE`, `ہ 0x06C1`, `ۀ 0x06C2`, `ی 0x06CC`, `ے 0x06D2`) and Kashida/Tatweel (`ـ 0x0640`).
- **Lam-Alif Mandatory Ligatures:** Combines `0x0644` (`ل`) followed by `0x0622` (`آ`), `0x0623` (`أ`), `0x0625` (`إ`), or `0x0627` (`ا`) into `0xFEF5`–`0xFEFC` (isolated or final form depending on preceding dual-joining state), even when combining marks sit between Lam and Alif.
- **Cluster-Preserving RTL Reordering:** Groups each base character with its trailing combining marks into a `Cluster` before reversing the RTL run into visual left-to-right order, ensuring marks remain immediately after their base glyph in the visual stream.
- **RTL Bracket & Symbol Mirroring (`Bidi::mirror_char`):** Automatically mirrors paired delimiters (`()`, `[]`, `{}`, `<>`, `«»`, `‹›`, `⁅⁆`, `⌈⌉`, `⌊⌋`, `⟨⟩`) inside RTL runs.

### 4.3 Grapheme Cluster & Word Boundary Segmentation (UAX #29)
- `Bidi::next_grapheme_cluster` / `Bidi::prev_grapheme_cluster`: Steps across extended grapheme clusters, keeping `CRLF` (`\r\n`), base + combining mark chains (`is_combining_mark`), Variation Selectors (`FE00`–`FE0F`, `E0100`–`E01EF`), Zero-Width Joiner sequences (`U+200D`), and Regional Indicator pairs (`1F1E6`–`1F1FF`) intact as atomic cursor units.
- `Bidi::next_word_boundary` / `Bidi::prev_word_boundary`: Steps across word and punctuation runs separated by whitespace.

---

## 5. System Font Integration, Discovery & Multilingual Fallback (`FontSystem` & `LinuxFontConfig`)

- **Sovereign Linux Fontconfig Integration (`LinuxFontConfig`):**
  - Parses `/etc/fonts/fonts.conf`, `/etc/fonts/conf.d/*.conf`, `/etc/fonts/local.conf`, and `$XDG_CONFIG_HOME/fontconfig/fonts.conf` using a zero-dependency streaming XML lexer without linking `libfontconfig` or `libexpat`.
  - Discovers system and user font directories (`/usr/share/fonts`, `/usr/local/share/fonts`, `~/.local/share/fonts`, `~/.fonts`, `$XDG_DATA_DIRS/fonts`) and respects `<rejectfont><glob>` filters.
  - Builds a directed alias & substitution graph for generic families (`sans-serif`, `serif`, `monospace`, `system-ui`, `emoji`, `math`), metric-compatible families (`Arial`, `Helvetica`, `Times New Roman`, `Courier New`), and locale-dependent overrides (`$LC_ALL`, `$LC_CTYPE`, `$LANG`).
- **Zero-Config Automatic System Font Discovery vs. Explicit Custom Fonts:**
  - By default (`auto_system_fonts_enabled() == true`), if a developer constructs `FontSystem fs;` and does **not** explicitly load custom fonts (`load_font_file`, `load_font_data`, `load_font_collection`, `scan_font_directory`), `FontSystem` automatically invokes `scan_system_fonts(true)` on the first font query or text layout/draw operation (`ensure_default_system_fonts`).
  - Conversely, if the developer explicitly registers one or more custom fonts (`has_custom_fonts() == true`), `FontSystem` remains strictly isolated to those custom fonts unless `scan_system_fonts()` is explicitly called.
- **Fast Header Probing, Lazy `mmap` Loading & Persistent Binary Cache (`FontSystem::scan_system_fonts`):**
  - `TtfFont::probe_face_header` inspects only the SFNT header and lightweight metadata tables (`head`, `name`, `OS/2`, `post`, `fvar`, `COLR`) across single fonts and TrueType Collections (`.ttc` / `.otc` via `face_index`) without allocating or parsing `glyf` / `CFF` / `GSUB` / `GPOS` tables.
  - Serializes discovered face descriptors and 128-bit `OS/2 ulUnicodeRange` bitmasks to `$XDG_CACHE_HOME/nisaba/font_index_v1.bin` validated by directory `mtime` fingerprints, enabling sub-millisecond startup across hundreds of system fonts.
  - Full `TtfFont` instances are instantiated lazily on-demand via `MappedFontData` (`POSIX mmap` zero-copy shared pages) only when matched for shaping or rendering.
- **CSS Fonts Level 4 Matching & Ordered Family Fallback Chains:**
  - `match_font` / `match_font_styled`: Resolves family candidates through `LinuxFontConfig::resolve_family_candidates`, scoring faces by `Stretch` distance, `Style` precedence (`Italic` $\leftrightarrow$ `Oblique` $\to$ `Normal`), and `Weight` distance (`100`–`900`).
  - `match_font_chain_styled(families, fallback_family, weight, style, stretch, locale)`: Evaluates an ordered per-span family chain (e.g., `["Inter", "Noto Sans Arabic", "Noto Color Emoji"]`), picking the first available family matching the requested style.
- **Ordered Chain, Locale/Han Unification & OS/2 Script-Aware Fallback (`find_glyph_or_fallback`, `find_glyph_in_chain_or_fallback`):**
  - Queries the primary `FontId` first, then walks remaining families in the span's `font_families` chain in priority order.
  - Applies BCP-47 locale preferences for Han unification and regional scripts (`ja`, `zh-Hant`, `zh-Hans`, `ko`, `ar`/`fa`/`ur`, `he`) before general fallback.
  - Uses a 256-entry direct-mapped `fallback_page_cache_` (`codepoint >> 8`) and maps missing codepoints to their OpenType `OS/2 ulUnicodeRange` bit (`0..127` via `codepoint_to_os2_range_bit`), testing only indexed faces covering the target script bit before lazily loading them.

---

## 6. Typography Attributes, Layout Pipeline & Rendering

### 6.1 Span Attribute Model (`Attrs` & `AttrsList`)
`Attrs` defines per-span styling properties with a fluent builder API:
- **Font Selection & Locale:** `Family family`, `std::vector<Family> font_families` (`set_font_families`, `add_font_family`), `std::string locale` (`set_locale`), `Weight weight`, `Style style`, `Stretch stretch`
- **OpenType Features & Variable Axes:** `std::vector<FontFeature> font_features` (`add_font_feature`, `set_font_features`), `std::vector<FontVariation> font_variations` (`add_font_variation`, `set_font_variations`)
- **Sizing & Spacing:** `std::optional<float> font_size_opt`, `float letter_spacing`, `float word_spacing`, `float baseline_shift`
- **Solid Colors & Full `Paint` Styling:** `std::optional<TextColor> color_opt`, `std::optional<TextColor> background_color_opt`, plus lightweight shared `std::shared_ptr<const Paint> foreground_paint_opt`, `background_paint_opt`, and `std::shared_ptr<const Stroke> foreground_stroke_opt` (`set_foreground_paint`, `set_background_paint`) enabling linear/radial/sweep/conical gradients, stroked/outlined glyphs, custom shaders, blend modes, and color/image filters per span with only 16 bytes overhead per `LayoutGlyph`.
- **Decorations & Shadows:** `underline`, `strikethrough`, `overline`, `DecorationStyle` (`Solid`, `Double`, `Dotted`, `Dashed`, `Wavy`), `DecorationMode` (`Through`, `Gaps`), `decoration_color_opt`, `decoration_thickness`, and `std::vector<TextShadow> shadows` (supporting both sharp offset shadows and Gaussian-blurred `blur_sigma` shadows).

`AttrsList` stores a default `Attrs` and byte-range spans (`AttrsSpan{start, end, attrs}`), queried via `AttrsList::get(size_t byte_index)`. `AttrsList::apply_range_modifier(start, end, fn)` modifies orthogonal properties across arbitrary sub-ranges while automatically splitting and compacting superseded/overlapping spans to keep span counts bounded during repeated interactive edits.

### 6.2 Shaping & Line Layout Pipeline (`BufferLine::shape_and_layout`)
`BufferLine::shape_and_layout` selects between three execution paths based on line content and attributes:
1. **Fast Pure-ASCII Path:** Triggered when all bytes are in $[0x00, 0x7F]$, there are no spans, placeholders, custom metrics/decorations, RTL direction, or ligatures. Uses direct `ascii_glyph_index()` and `ascii_advance()` LUTs with `get_kerning()`.
2. **Fast LTR Path:** Triggered when there are no RTL characters, combining marks, custom spans/attributes, or ligatures. Decodes UTF-8 codepoints inline and resolves glyphs via `find_glyph_or_fallback`.
3. **Full Rich / BiDi / OpenType / Variable Font Path:**
   - Segments bidirectional runs (`Bidi::segment_runs`) and shapes characters (`Bidi::shape_text`) honoring explicit `TextDirection`.
   - Emits inline placeholders anchored at `U+FFFC` with exact box width/height and surrounding BiDi level.
   - Resolves per-span `Attrs` via `match_font_chain_styled` and `find_glyph_in_chain_or_fallback` with BCP-47 locale support.
   - Applies Arabic OpenType `GSUB` contextual features (`isol`, `init`, `medi`, `fina`), automatic `locl` substitutions when `locale` is set, single-glyph user OpenType features (`tnum`, `zero`, `smcp`, `ss01`, etc.), and multi-glyph ligature/feature passes (`rlig`, `calt`, `liga`, `clig`, plus user-enabled/disabled features with `value == 0` overrides).
   - Evaluates varied glyph advances via `glyph_advance_varied` when variable font coordinates are present.
   - Applies `GPOS` pair positioning (`get_kerning`), `GPOS` Mark-to-Base (`get_mark_to_base_offset`), and `GPOS` Mark-to-Mark (`get_mark_to_mark_offset`), setting combining mark advance width `w = 0.0f`.

### 6.3 Line Wrapping, Alignment, Custom Ellipsis & Overflow (`Buffer`)
- **Wrapping Modes (`Wrap`):**
  - `Wrap::None`: Single visual line per paragraph line.
  - `Wrap::Glyph`: Breaks at any base glyph boundary exceeding `wrap_width` (keeping trailing `is_mark` glyphs attached to their base glyph).
  - `Wrap::Word` / `Wrap::WordOrGlyph`: Breaks at whitespace, atomic inline placeholder boundaries, or CJK ideographic break opportunities (respecting no-break-before/after punctuation rules), falling back to glyph-level splitting if a single word exceeds `wrap_width`.
- **Horizontal Alignment (`Align`):**
  - `Align::Left`, `Align::Right`, `Align::Center`, `Align::Start` (left for LTR, right for RTL), `Align::End` (right for LTR, left for RTL).
  - `Align::Justified`: Distributes remaining horizontal slack `wrap_width - line.w` evenly across inter-word whitespace glyphs on wrapped non-final lines, recording `LayoutGlyph::justified_extra_w` so alignment can be losslessly changed later without re-shaping.
- **Custom UTF-8 Ellipsis & Overflow (`Overflow`, `max_lines`, `set_ellipsis`):**
  - Supports `Overflow::Visible`, `Overflow::Clip`, and `Overflow::Ellipsis`, constrained by `width()`, `height()`, and `max_lines()`.
  - `Buffer::set_ellipsis(std::string_view)` shapes arbitrary UTF-8 truncation strings (e.g., `"…"`, `"... more"`, or Arabic `"… المزيد"`) with full RTL visual-left truncation and re-alignment; `clear_ellipsis()` restores the default `"…"` (`U+2026`, or `"..."` fallback).

### 6.4 Extended Paragraph Metrics & Layout Queries (`Buffer`)
- **Intrinsic & Line Bounds:** `max_line_width()` / `longest_line_width()`, `total_height()`, `max_intrinsic_width(FontSystem&)` (unwrapped full width even under `Wrap::None + Overflow::Ellipsis`), and `min_intrinsic_width(FontSystem&)` (widest unbreakable word, cluster, or inline placeholder).
- **Detailed Per-Line Metrics (`line_metrics()`):** Returns `std::vector<LineMetrics>` containing `start_index`, `end_index`, `ascent`, `descent`, `unscaled_ascent`, `height`, `width`, `left`, `baseline`, `hard_break`, and `line_number` in flattened buffer coordinates.
- **Baselines & Diagnostics:** `alphabetic_baseline()`, `ideographic_baseline()`, `did_exceed_max_lines()`, `unresolved_glyphs()` (missing/tofu glyph count excluding placeholders and ignorable control characters), `shape_count()`, and `is_layout_dirty()`.
- **Range & Hit Queries:** `range_rects(start, end, origin_x, origin_y, RectHeightStyle::Max | Tight)`, `glyph_position_at(x, y, origin_x, origin_y)` returning `GlyphPosition{position, upstream}`, and `word_range_at(byte_pos)` returning `TextRange{start, end}`.

### 6.5 Inline Placeholders (`add_text`, `add_placeholder`, `placeholder_rects`)
- `Buffer::add_text(std::string_view, const Attrs&)` appends multi-line rich text segments without clearing prior content.
- `Buffer::add_placeholder(width, height, PlaceholderAlignment, TextBaseline, baseline_offset)` (or `add_placeholder(const PlaceholderStyle&)`) embeds an atomic inline box anchored by `U+FFFC` that participates in BiDi direction resolution, word wrapping, line height expansion, and intrinsic width calculation.
- Supports all 6 vertical alignment modes (`Baseline` with `Alphabetic`/`Ideographic` reference + `baseline_offset`, `AboveBaseline`, `BelowBaseline`, `Top`, `Bottom`, `Middle`).
- `Buffer::placeholder_rects(origin_x, origin_y)` returns laid-out `PlaceholderRect{rect, direction, placeholder_index, line_number}` sorted by `placeholder_index`, ready for rendering inline UI widgets, icons, badges, chips, or bitmap emojis on either CPU or GPU.

### 6.6 Uniform Vertical Rhythm (`StrutStyle`, `HalfLeading`, `TextHeightBehavior`)
- **`StrutStyle` (`set_strut_style`):** Defines a minimum or forced (`force_strut_height`) line box rhythm (`font_families`, `weight`, `style`, `font_size`, `height_multiplier`, `leading`, `half_leading`) for uniform baseline-to-baseline spacing across mixed scripts (e.g., Arabic + Latin + Emoji).
- **`HalfLeading` (`set_half_leading`):** Splits excess line height evenly above the ascent and below the descent for optical vertical centering.
- **`TextHeightBehavior` (`set_text_height_behavior`):** Independently controls `apply_height_to_first_ascent` and `apply_height_to_last_descent`, trimming leading above the first line and/or below the last line for exact vertical centering inside buttons and single-line inputs.

### 6.7 Incremental In-Place Updates (Zero Re-Shape)
For interactive text editors, hover highlights, and animated typography, `Buffer` supports zero-reshaping in-place mutations (`shape_count()` remains unchanged while `AttrsList` stays synchronized for any future re-wrap):
- **`update_text_align(Align)`:** Re-applies alignment across all laid-out visual lines (including lossless restoration of `justified_extra_w` when switching away from `Align::Justified`) without re-running BiDi or shaping.
- **`update_foreground_paint(start, end, Paint [, Stroke])`:** Updates `foreground_paint_opt` (and optional `Stroke`) across flattened UTF-8 byte range `[start, end)` in-place.
- **`update_background_paint(start, end, Paint)`:** Updates `background_paint_opt` across `[start, end)` in-place.
- **`update_font_size(start, end, float)`:** Proportionally scales glyph `font_size`, advances, mark offsets, and glyph ascent/descent across `[start, end)`, then recomputes per-line `max_ascent`/`max_descent`/`line_height_opt`, placeholder expansion, vertical line metrics (`StrutStyle`/`HalfLeading`/`TextHeightBehavior`), and horizontal alignment in-place without re-shaping.

### 6.8 Glyph Run Visitor (`Buffer::visit`)
`Buffer::visit(visitor, &font_system, origin_x, origin_y)` (and convenience overload `visit(font_system, visitor, origin_x, origin_y)`) walks all laid-out visual lines in order, grouping contiguous same-font/same-style glyphs (and individual placeholders) into `GlyphRunInfo` records using zero-allocation scratch buffers:
- Exposes `line_number`, `paragraph_line`, `font_id`, `const TtfFont* font`, `font_size`, `font_weight`, `synthetic_bold`, `synthetic_italic`, `is_rtl`, `is_placeholder`, `placeholder_index`, baseline `origin`, `line_top`, `line_left`, `line_height`, `line_width`, `baseline`, `ascent`, `descent`, `advance_width`, `utf8_text`, `text_byte_offset`.
- Provides contiguous spans `glyph_ids`, `positions` (relative to `origin`), `advances`, flattened UTF-8 `clusters`, and underlying `glyphs` for custom renderers, PDF export, and accessibility trees.

### 6.9 CPU & GPU Rendering (1:1 Parity)
Both `Buffer::draw` (CPU `Canvas`, accepting either `Color` or `const Paint&`) and `GpuCanvas::draw_text_buffer` / `gpu::Context::drawTextBuffer` (GPU) execute the exact same multi-pass pipeline:
1. **Pass 1 (Span-Merged Background Highlights):** Merges adjacent glyphs sharing identical `background_paint_opt` or `background_color_opt` into continuous rectangles filled across $[y_{\text{top}}, y_{\text{top}} + h]$.
2. **Pass 1.5 (Sharp & Blurred Text Shadows):** Renders sharp offset shadows directly and groups Gaussian-blurred shadows (`blur_sigma > 0.1f`) per sigma inside bounded offscreen blur layers (`save_layer_bounds` + `ImageFilter::blur` on CPU; offscreen blurred layer on GPU).
3. **Pass 2 (Glyphs — Fast Atlas/Mask Path + Rich Vector Path):**
   - Solid-color glyphs (and `COLR`/`CPAL` color layers) use the subpixel/variable-aware alpha mask pipeline (`GlyphCache` + `Canvas::draw_glyph` on CPU; shelf-packed `GlyphAtlas` up to $2048 \times 2048$ in `gpu::Context::drawTextBuffer` on GPU).
   - Glyphs whose active foreground `Paint` requires vector rendering (`paint_requires_vector_path`: gradients, `PaintStyle::Stroke`/`StrokeAndFill`, custom shaders, non-`SourceOver` blend modes, color/image filters) automatically route through `get_glyph_path_varied` / `get_glyph_path_styled` and `draw_path` on both CPU and GPU.
4. **Pass 3 (Shared Decorations + Smart Underline Gap Skipping):**
   - Both CPU and GPU call the shared `build_run_decorations(run, origin_x, origin_y, default_font_size, default_color, &font_system)` geometry generator supporting `underline`, `strikethrough`, `overline` across all 5 `DecorationStyle`s (`Solid`, `Double`, `Dotted`, `Dashed`, `Wavy`).
   - When `DecorationMode::Gaps` is active, exact vector glyph outlines (including varied outlines) are intersected with the underline vertical ink band using scanline fill-interior pairing to carve clean visual gaps around Latin and Arabic descenders (`g`, `y`, `p`, `q`, `ج`, `ي`, `ر`, etc.).

---

## 7. Caching Subsystems (`GlyphCache`, `ParagraphCache`, `BakedText`, `TextCache`)

### 7.1 Subpixel & Variable-Axis Glyph Cache (`GlyphCache`)
- Quantizes fractional screen coordinates $(x, y)$ into 4 horizontal subpixel bins (`SubpixelBin::Zero` $[0, 0.125) \cup [0.875, 1.0)$, `Quarter` $[0.125, 0.375)$, `Half` $[0.375, 0.625)$, `ThreeQuarters` $[0.625, 0.875)$) via `compute_subpixel_bin`.
- Keys entries by `(font_id, glyph_id, font_size_fixed, subpixel_x, style_flags, variation_hash)` so multiple Variable Font axis instances of the same font face coexist cleanly in both CPU `GlyphCache` and GPU `GlyphAtlas`.
- Stores rasterized `Mask::Format::A8` bitmaps in an `std::unordered_map<CacheKey, Entry, CacheKeyHash>` paired with a doubly-linked `std::list<CacheKey> lru_list_`, evicting the least-recently-used entry when `cache_.size() >= max_entries_` (default `4096`, configurable via `set_max_entries`).

### 7.2 Paragraph Layout Cache (`ParagraphCache` / `ParagraphLayoutCache`)
- Caches fully shaped `Buffer` instances in an $O(1)$ LRU map keyed by `ParagraphCacheKey` (`text`, value-hashed `attrs` including solid `Paint`/`Stroke`, extra `spans`, `placeholders`, `metrics`, `width`, `height`, `wrap`, `align`, `shaping`, `overflow`, `text_direction`, `strut_style`, `half_leading`, `text_height_behavior`, `ellipsis`, `max_lines`, `enable_kerning`, `enable_ligatures`, and active `FontSystem::font_count()`).
- **`get_or_layout(font_system, text, attrs, metrics, width, ...)`** (and explicit `ParagraphCacheKey` overload): Returns a shared pre-shaped `std::shared_ptr<Buffer>` directly usable on both CPU `Canvas` and GPU `GpuCanvas`.
- **`lookup_or_shape(font_system, buffer)`:** Extracts a key from an arbitrary configured `Buffer` and hydrates its shaped layout via `copy_shaped_layout_from` on hit (zero shaping passes) or shapes and caches a snapshot on miss.
- Exposes LRU capacity control (`set_max_entries`, default `256`) and telemetry (`size()`, `hits()`, `misses()`, `hit_ratio()`, `reset_stats()`, `clear()`).

### 7.3 Pre-Baked SIMD Surface Cache (`BakedText` & `TextCache`)
- `BakedText::from_buffer` shapes and renders an entire `Buffer` once into a tight `Pixmap` (`RGBA8888` premultiplied alpha).
- `BakedText::draw_fast(Canvas& canvas, float x, float y)` bypasses shaping and path/mask rasterization when the canvas transform is pure integer translation and `BlendMode::SourceOver`, executing `pipeline::composite_scanline_src_over` (AVX2 / NEON SIMD) directly per row.
- `TextCache` manages up to `max_entries` (default `512`) `BakedText` surfaces indexed by `TextCacheKey`, tracking hit/miss statistics and LRU eviction.

---

## 8. Interactive Caret & Selection Geometry (`CursorController`)

- **`CursorController::hit_test(const Buffer&, float x, float y, float origin_x, float origin_y)`:** Locates the vertical `LayoutRun` containing `y - origin_y`, skips zero-advance `is_mark` glyphs, and resolves the byte index of the closest glyph edge (`glyph.start` if in the left half of `glyph.w`, `glyph.end` if in the right half).
- **`CursorController::move(const Buffer&, Cursor current, Motion motion)`:**
  - `Motion::Left` / `Motion::Right`: Moves by one extended grapheme cluster (`Bidi::prev_grapheme_cluster` / `Bidi::next_grapheme_cluster`), wrapping across paragraph lines.
  - `Motion::WordLeft` / `Motion::WordRight`: Moves to the previous or next word boundary (`Bidi::prev_word_boundary` / `Bidi::next_word_boundary`).
  - `Motion::Up` / `Motion::Down`: Navigates across wrapped visual `LayoutRun` lines while preserving the visual $X$ coordinate via `hit_test`, or steps to the adjacent paragraph line.
  - `Motion::Home` / `Motion::End`: Moves to byte index `0` or `line.text().size()`.
  - `Motion::DocumentStart` / `Motion::DocumentEnd`: Moves to the start of the first line or the end of the last line.
- **`CursorController::cursor_rect(const Buffer&, Cursor, float origin_x, float origin_y, float width)`:** Computes the visual bounding `Rect` of the caret at `Cursor`, accounting for RTL glyph direction (`level & 1`) and proportional ligature byte interpolation.
- **`CursorController::selection_rects(const Buffer&, Selection, float origin_x, float origin_y)`:** Normalizes `Selection(start, end)` and merges horizontally adjacent selected glyph boxes per `LayoutRun`, emitting one or more `Rect` boxes per visual line (supporting discontiguous visual selection spans in bidirectional text).
