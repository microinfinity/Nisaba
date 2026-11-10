# Nisaba Text Engine (`nisaba::text`) — Technical Reference

## 1. Architecture Overview

`nisaba::text` is the typography, font parsing, text shaping, layout, caching, and rendering subsystem of the Nisaba engine. It operates without external font or shaping dependencies and outputs directly to both `nisaba::Canvas` (CPU scanline/mask rasterization) and `nisaba::gpu::Context` (OpenGL ES 3.2 / Vulkan hardware-accelerated glyph atlas rendering).

### Subsystem Structure

| Header / Source | Responsibility |
| :--- | :--- |
| `include/nisaba/text/glyph.hpp` | Subpixel quantization (`SubpixelBin`, `compute_subpixel_bin`), `CacheKey` hashing, and `CachedGlyph` alpha mask representation. |
| `include/nisaba/text/attrs.hpp`<br>`src/text/attrs.cpp` | Typographic attributes (`TextColor`, `Family`, `Weight`, `Style`, `Stretch`, `Metrics`, `Attrs`), inline span ranges (`AttrsSpan`, `AttrsList`), and `Shaping` tiers. |
| `include/nisaba/text/cff_font.hpp`<br>`src/text/cff_font.cpp` | PostScript `CFF ` (Compact Font Format v1) and `CFF2` parser and Type 2 Charstring stack virtual machine. |
| `include/nisaba/text/opentype_tables.hpp`<br>`src/text/opentype_tables.cpp` | OpenType layout tables: `OpenTypeGdef` (glyph class definitions & mark attachment), `OpenTypeGsub` (single, multiple, alternate, and ligature substitutions), and `OpenTypeGpos` (pair kerning, mark-to-base, and mark-to-mark positioning). |
| `include/nisaba/text/ttf_font.hpp`<br>`src/text/ttf_font.cpp` | SFNT container & TTC/OTC collection parser (`TTF` / `OTF` / `TTC`), POSIX `mmap` zero-copy file mapping (`MappedFontData`), fast header-only probing (`probe_face_header`), `cmap` Format 4/12/14 character mapping, `hmtx`/`hhea`/`post`/`OS/2` metrics & 128-bit `ulUnicodeRange` bitmasks, `glyf`/`loca` quadratic outlines, `COLR`/`CPAL` layered color glyphs, `fvar` variable axis descriptors, `kern` binary search + direct-mapped kerning cache, and synthetic bold/italic outline transformations. |
| `include/nisaba/text/linux_font_config.hpp`<br>`src/text/linux_font_config.cpp` | Sovereign zero-dependency Linux Fontconfig XML & environment parser (`/etc/fonts/fonts.conf`, `conf.d/*.conf`, `local.conf`, XDG font directories, metric-compatible family aliases, and locale-specific `<match>` overrides). |
| `include/nisaba/text/font_system.hpp`<br>`src/text/font_system.cpp` | System-integrated multi-font registry (`FontSystem`), automatic Linux font discovery (`scan_system_fonts`, `with_system_fonts`), persistent binary index caching (`~/.cache/nisaba/font_index_v1.bin`), lazy `mmap` instantiation, CSS Fonts Level 4 family/weight/style/stretch matching (`match_font`, `match_font_styled`), and OS/2 Unicode range & script-aware multilingual fallback (`find_glyph_or_fallback`). |
| `include/nisaba/text/bidi.hpp`<br>`src/text/bidi.cpp` | Bidirectional run analysis (`Bidi::segment_runs`), Arabic/Persian/Urdu/Kurdish contextual joining (`Bidi::shape_text`), cluster-preserving RTL reordering, bracket mirroring (`Bidi::mirror_char`), and UAX #29 grapheme cluster / word boundary segmentation. |
| `include/nisaba/text/layout.hpp`<br>`src/text/layout.cpp` | Layout data structures (`Wrap`, `Align`, `Overflow`, `LayoutGlyph`, `LayoutLine`, `LayoutRun`). |
| `include/nisaba/text/glyph_cache.hpp`<br>`src/text/glyph_cache.cpp` | Subpixel-aware glyph alpha mask cache (`GlyphCache`) with $O(1)$ LRU eviction. |
| `include/nisaba/text/buffer.hpp`<br>`src/text/buffer.cpp` | Paragraph management (`BufferLine`, `Buffer`), multi-stage shaping & line-wrapping pipeline (`shape_and_layout`), justification, ellipsis truncation, and multi-pass rendering (`Buffer::draw`). |
| `include/nisaba/text/cursor.hpp`<br>`src/text/cursor.cpp` | Interactive caret and selection controller (`Cursor`, `Selection`, `Motion`, `CursorController::hit_test`, `move`, `cursor_rect`, `selection_rects`). |
| `include/nisaba/text/baked_text.hpp`<br>`src/text/baked_text.cpp` | Pre-composited RGBA8888 text surface (`BakedText`) with direct SIMD scanline blitting (`draw_fast`). |
| `include/nisaba/text/text_cache.hpp`<br>`src/text/text_cache.cpp` | Hash-indexed LRU cache (`TextCache`) of `BakedText` entries keyed by `TextCacheKey`. |

---

## 2. Font Binary Ingestion & Outline Extraction

### 2.1 SFNT Table Directory (`TtfFont`)
`TtfFont::from_bytes` and `TtfFont::from_file` validate SFNT headers (`0x00010000`, `true`, `typ1`, and `OTTO` for OpenType/CFF) and populate a fixed-capacity stack array `TableRecord tables_[48]` without heap allocations for table directory indexing.

Supported SFNT tables:
- **Required Metrics & Header Tables:** `head`, `maxp`, `hhea`, `hmtx`, `cmap`, `name`, `OS/2`
- **Outline Tables:** `loca` + `glyf` (TrueType quadratic curves), `CFF ` + `CFF2` (PostScript cubic curves)
- **Advanced Layout Tables:** `GDEF`, `GSUB`, `GPOS`, `kern`
- **Color & Variation Tables:** `COLR`, `CPAL`, `fvar`

### 2.2 Character-to-Glyph Mapping (`cmap`)
`TtfFont::glyph_index(char32_t codepoint)` resolves Unicode codepoints through three tiers:
1. **Direct ASCII Lookup Table ($O(1)$):** Codepoints in $[0, 127]$ index directly into `uint16_t ascii_gid_lut_[128]`, populated during initialization.
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

### 2.6 Variable Font Axis Metadata (`fvar`)
`TtfFont::variation_axes()` parses `fvar` `VariationAxisRecord` entries, exposing:
- `uint32_t tag` (4-character OpenType axis tag, e.g., `'wght'`, `'wdth'`, `'slnt'`, `'opsz'`)
- `float min_value`, `float default_value`, `float max_value` (decoded from 16.16 fixed-point values)

### 2.7 Synthetic Bold & Synthetic Italic (`get_glyph_path_styled`)
When a requested weight ($\ge 600$) or style (`Style::Italic` / `Style::Oblique`) is not natively present in the matched font face:
- **Synthetic Bold:** Computes stroke width $w = \max(0.75, 0.045 \times \text{font\_size})$ with `LineJoin::Round` and `LineCap::Round`, converts the stroke envelope via `Path::stroke_to_fill`, and appends it to the base glyph outline.
- **Synthetic Italic:** Applies a horizontal shear matrix `Transform::from_skew(-0.2125f, 0.0f)` ($\tan 12^\circ \approx 0.2125$) in screen space.

---

## 3. OpenType Layout Tables (`GDEF`, `GSUB`, `GPOS`)

### 3.1 Glyph Definition Table (`OpenTypeGdef`)
- Parses `GlyphClassDef` (Format 1 array and Format 2 binary-searched `ClassRangeRecord` intervals), classifying glyphs into:
  - `GlyphClass::Unclassified` (`0`)
  - `GlyphClass::Base` (`1`)
  - `GlyphClass::Ligature` (`2`)
  - `GlyphClass::Mark` (`3`)
  - `GlyphClass::Component` (`4`)
- Evaluates `LookupFlag` bitmasks (`IgnoreBaseGlyphs = 0x0002`, `IgnoreLigatures = 0x0004`, `IgnoreMarks = 0x0008`) via `OpenTypeGdef::should_skip_glyph(uint16_t glyph_id, uint16_t lookup_flag)`.

### 3.2 Glyph Substitution Table (`OpenTypeGsub`)
- Traverses `FeatureList` and `LookupList` for any requested sequence of feature tags (`apply_features`, `apply_single_glyph_feature`, `apply_ligatures`).
- Supported lookup types:
  - **LookupType 1 (Single Substitution):** Format 1 (`deltaGlyphID` modulo 65536) and Format 2 (`substituteGlyphIDArray` indexed by Coverage index).
  - **LookupType 2 (Multiple Substitution):** Format 1 (`Sequence` table expanding 1 glyph into $N$ glyphs).
  - **LookupType 3 (Alternate Substitution):** Format 1 (`AlternateSet` selecting alternate glyph 0).
  - **LookupType 4 (Ligature Substitution):** Format 1 (`LigatureSet` $\to$ `Ligature` table). Uses `should_skip_glyph` with `lookup_flag` (and `GDEF` mark classification) to match base glyph sequences even when combining marks intervene, placing the substituted ligature at the first component position while preserving intervening marks after the ligature.
  - **LookupType 7 (Extension Substitution):** 32-bit `extensionOffset` unwrapping to underlying subtables.
- Built-in feature tags (`feature_tag` namespace): `liga`, `clig`, `rlig`, `calt`, `ccmp`, `locl`, `isol`, `init`, `medi`, `fina`.

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

### 4.1 Bidirectional Run Segmentation
- `Bidi::detect_base_direction(std::string_view text)` scans for the first strong directional character (`CharType::LTR` vs. `CharType::RTL` / `CharType::ArabicLetter`).
- `Bidi::segment_runs(std::string_view text)` classifies UTF-8 codepoints into `LTR`, `RTL`, `ArabicLetter`, `EuropeanNumber`, `ArabicNumber`, `Whitespace`, and `Neutral`, resolving embedding levels (`level = 0` for LTR, `level = 1` for RTL, `level = 2` for numbers embedded within RTL) and emitting contiguous `BidiRun` slices.

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
- **CSS Fonts Level 4 Matching (`match_font` / `match_font_styled`):**
  - Resolves ordered family candidates through `LinuxFontConfig::resolve_family_candidates`, then scores faces within the matched family by `Stretch` distance, `Style` precedence (`Italic` $\leftrightarrow$ `Oblique` $\to$ `Normal`), and `Weight` distance (`100`–`900`).
  - Sets `synthetic_bold = true` when `attrs.weight >= 600` and the matched face has weight $< 600$, and `synthetic_italic = true` when italic/oblique is requested on a roman face.
- **Unicode Range & Script-Aware Fallback (`find_glyph_or_fallback`):**
  - Queries the primary `FontId` first, then checks a 256-entry direct-mapped `fallback_page_cache_` (`codepoint >> 8`) for $O(1)$ repeat lookups.
  - Maps the missing `codepoint` to its OpenType `OS/2 ulUnicodeRange` bit (`0..127` via `codepoint_to_os2_range_bit`), testing candidate fallback families and indexed system faces whose 128-bit coverage mask includes the target script bit before lazily loading the face.

---

## 6. Typography Attributes, Layout Pipeline & Rendering

### 6.1 Span Attribute Model (`Attrs` & `AttrsList`)
`Attrs` defines per-span styling properties:
- **Font Selection:** `Family family`, `Weight weight`, `Style style`, `Stretch stretch`
- **Sizing & Spacing:** `std::optional<float> font_size_opt`, `std::optional<float> line_height_opt`, `float letter_spacing`, `float word_spacing`, `float baseline_shift`
- **Decorations & Colors:** `bool underline`, `bool strikethrough`, `std::optional<TextColor> color_opt`, `std::optional<TextColor> background_color_opt`

`AttrsList` stores a default `Attrs` and a vector of non-overlapping byte-range spans (`AttrsSpan{start, end, attrs}`), queried via `AttrsList::get(size_t byte_index)`.

### 6.2 Shaping & Line Layout Pipeline (`BufferLine::shape_and_layout`)
`BufferLine::shape_and_layout` selects between three execution paths based on line content and attributes:
1. **Fast Pure-ASCII Path:** Triggered when all bytes are in $[0x00, 0x7F]$, `attrs_list_.spans().empty()`, and `!enable_ligatures`. Uses direct `ascii_gid()` and `ascii_advance()` LUTs with `get_kerning()`.
2. **Fast LTR Path:** Triggered when `detect_base_direction` is LTR, no Arabic/RTL characters or custom spans are present, and `!enable_ligatures`. Decodes UTF-8 codepoints inline and resolves glyphs via `find_glyph_or_fallback`.
3. **Full Rich / BiDi / OpenType Path:**
   - Segments bidirectional runs (`Bidi::segment_runs`) and shapes characters (`Bidi::shape_text`).
   - Resolves per-span `Attrs`, font size, baseline shift, and synthetic bold/italic flags via `match_font_styled`.
   - Applies Arabic OpenType `GSUB` contextual features (`isol`, `init`, `medi`, `fina`) when the font lacks Unicode Presentation Forms (`FB50`–`FEFF`), and applies `GSUB` ligature features (`rlig`, `liga`, `clig`, `calt`) when `enable_ligatures` is active or RTL shaping requires `rlig`.
   - Applies `GPOS` pair positioning (`get_pair_adjustment` / `get_kerning`) between adjacent base glyphs, `GPOS` Mark-to-Base (`get_mark_to_base_offset`) from the preceding base glyph to combining marks (`is_mark`), and `GPOS` Mark-to-Mark (`get_mark_to_mark_offset`) for stacked marks, setting mark advance width `w = 0.0f`.

### 6.3 Line Wrapping, Alignment & Overflow (`Buffer`)
- **Wrapping Modes (`Wrap`):**
  - `Wrap::None`: Single visual line per paragraph line.
  - `Wrap::Glyph`: Breaks at any base glyph boundary exceeding `wrap_width` (keeping trailing `is_mark` glyphs attached to their base glyph).
  - `Wrap::Word` / `Wrap::WordOrGlyph`: Breaks at whitespace or CJK ideographic break opportunities (`0x3000`–`0x9FFF`, `0xAC00`–`0xD7AF`, `0xF900`–`0xFAFF`, `0xFF00`–`0xFFEF`), falling back to glyph-level splitting if a single word exceeds `wrap_width`.
- **Horizontal Alignment (`Align`):**
  - `Align::Left`, `Align::Right`, `Align::Center`, `Align::End` (right-aligned for RTL, left-aligned for LTR).
  - `Align::Justified`: Distributes remaining horizontal slack `wrap_width - line.w` evenly across inter-word whitespace glyphs on all wrapped non-final lines of a paragraph.
- **Overflow & Line Limits (`Overflow` & `max_lines`):**
  - Enforces `max_lines` and vertical `height()` constraints.
  - When `Overflow::Ellipsis` is active and a line exceeds `width()` or is truncated by `max_lines` / `height()`, `Buffer::apply_ellipsis_truncation` pops trailing glyphs until the ellipsis glyph (`U+2026` `…`, or three `.` glyphs as fallback) fits within `max_width`.

### 6.4 CPU & GPU Rendering
- **CPU Rendering (`Buffer::draw`):**
  1. **Pass 1 (Background Highlights):** Fills contiguous `background_color_opt` spans across the line height $[y_{\text{top}}, y_{\text{top}} + h]$.
  2. **Pass 2 (Glyphs & Color Layers):**
     - If `font->has_color_glyphs()` and `get_color_glyph_layers` returns `COLR`/`CPAL` layers, extracts and fills each layer's vector `Path` with its palette `TextColor` (or foreground color).
     - Otherwise, fetches the subpixel-positioned alpha mask from `GlyphCache::get_or_render` (passing `synthetic_bold` and `synthetic_italic`) and composites via `Canvas::draw_mask`.
  3. **Pass 3 (Text Decorations):** Renders `underline` (at $y_{\text{baseline}} + 0.12 \times \text{font\_size}$) and `strikethrough` (at $y_{\text{baseline}} - 0.30 \times \text{font\_size}$) with thickness $\max(1.0, 0.065 \times \text{font\_size})$.
- **GPU Rendering (`gpu::Context::drawTextBuffer`):**
  - Packs `CachedGlyph` alpha masks into a dynamic shelf-packed GPU texture (`GlyphAtlas`, auto-expanding up to $2048 \times 2048$) keyed by `CacheKey` (including subpixel bins and `style_flags`), emitting transformed textured triangle quads in batches.

---

## 7. Caching Subsystems (`GlyphCache`, `BakedText`, `TextCache`)

### 7.1 Subpixel Glyph Cache (`GlyphCache`)
- Quantizes fractional screen coordinates $(x, y)$ into 4 horizontal subpixel bins (`SubpixelBin::Zero` $[0, 0.125) \cup [0.875, 1.0)$, `Quarter` $[0.125, 0.375)$, `Half` $[0.375, 0.625)$, `ThreeQuarters` $[0.625, 0.875)$) via `compute_subpixel_bin`.
- Stores rasterized `Mask::Format::A8` bitmaps in an `std::unordered_map<CacheKey, Entry, CacheKeyHash>` paired with a doubly-linked ` std::list<CacheKey> lru_list_`.
- Automatically evicts the least-recently-used entry when `cache_.size() >= max_entries_` (default `4096`, configurable via `set_max_entries`).

### 7.2 Pre-Baked SIMD Surface Cache (`BakedText` & `TextCache`)
- `BakedText::from_buffer` shapes and renders an entire `Buffer` once into a tight `Pixmap` (`RGBA8888` premultiplied alpha).
- `BakedText::draw_fast(Canvas& canvas, float x, float y)` bypasses shaping and path/mask rasterization when the canvas transform is pure integer translation and `BlendMode::SourceOver`, executing `pipeline::composite_scanline_src_over` (AVX2 / NEON SIMD) directly per row.
- `TextCache` manages up to `max_entries` (default `512`) `BakedText` surfaces indexed by `TextCacheKey` (`text`, `font_size_bits`, `line_height_bits`, `wrap_width_bits`, `color_rgba`, `weight`, `style`, `align`, `wrap`), tracking hit/miss statistics and frame-tick LRU eviction.

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
