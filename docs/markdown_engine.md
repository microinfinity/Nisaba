# Nisaba Markdown Engine (`nisaba::markdown`) — Technical Reference

## 1. Architecture Overview

`nisaba::markdown` is the CommonMark 0.30 and GitHub Flavored Markdown (GFM) parsing, streaming, sanitization, extensibility, layout, multi-format exporting, mathematical typesetting, diagramming, and multi-backend rendering subsystem of the Nisaba engine.

It is written in pure ISO C++20 with zero external parsing, layout, HTML, math, or diagram dependencies, compiles cleanly with `-fno-rtti`, and renders uniformly across CPU `Canvas`, hardware `GpuCanvas`, and native ISO 32000-1 `PdfCanvas` through the RTTI-free `ICanvas` / `CanvasKind` interface.

### Subsystem Structure

| Header / Source | Responsibility |
| :--- | :--- |
| `include/nisaba/markdown/markdown.hpp` | Umbrella header exposing the entire `nisaba::markdown` public API. |
| `include/nisaba/markdown/types.hpp` | Core AST types (`BlockNode`, `InlineSpan`, `TableRow`, `TableCell`), `SourcePos`, `MarkdownParseOptions`, `AlertType`, `TableAlign`, `HitTestResult`. |
| `include/nisaba/markdown/entities.hpp`<br>`src/markdown/entities.cpp` | Complete 2,125-entry HTML5 named character reference table, decimal/hex numeric entity decoding, 32 ASCII punctuation backslash escapes, SWAR 8-byte UTF-8 validation/sanitization (`U+FFFD`), and Unicode full case-folding. |
| `include/nisaba/markdown/html_parser.hpp`<br>`src/markdown/html_parser.cpp` | Zero-dependency HTML tag/attribute/style/color parser, safe-tag whitelist, and GFM `Tagfilter` neutralization for disallowed raw HTML tags. |
| `include/nisaba/markdown/extension.hpp` | Pluggable, RTTI-free syntax extension interface (`MarkdownExtension`, `MarkdownExtensionRegistry`, `BlockExtensionContext`, `InlineExtensionContext`). |
| `include/nisaba/markdown/parser.hpp`<br>`src/markdown/parser.cpp` | Stateful incremental streaming parser (`feed`, `snapshot`, `finish`, `reset`) and one-shot CommonMark 0.30 + GFM block and inline delimiter-stack parser. |
| `include/nisaba/markdown/exporter.hpp`<br>`src/markdown/exporter.cpp` | Multi-format AST exporters: HTML5 (`to_html`), unformatted UTF-8 plaintext (`to_plaintext`), and normalized CommonMark/GFM round-trip serializer (`to_markdown`). |
| `include/nisaba/markdown/document.hpp`<br>`src/markdown/document.cpp` | Document lifecycle (`from_string`, `from_file`), responsive block/table/list layout engine, Table of Contents extraction, spatial hit-testing, cursor hints, and interactive state toggles. |
| `include/nisaba/markdown/style.hpp`<br>`src/markdown/style.cpp` | Theme configuration (`MarkdownStyle::dark_theme()`, `MarkdownStyle::light_theme()`), typography scale, spacing metrics, alert themes, and syntax highlighting palette. |
| `include/nisaba/markdown/syntax_highlighter.hpp`<br>`src/markdown/syntax_highlighter.cpp` | Multi-language code tokenizer for fenced code blocks (keywords, types, strings, numbers, comments, preprocessor directives, operators, punctuation). |
| `include/nisaba/markdown/math_engine.hpp`<br>`src/markdown/math_engine.cpp` | LaTeX mathematical expression parser, 2D typographic box layout solver, Unicode plaintext converter, and polymorphic `ICanvas` vector renderer. |
| `include/nisaba/markdown/diagram_engine.hpp`<br>`src/markdown/diagram_engine.cpp` | Flowchart, Sequence, State, XYChart, and PieChart parser, graph/chart layout solver, and polymorphic `ICanvas` vector renderer. |
| `include/nisaba/markdown/renderer.hpp`<br>`src/markdown/renderer.cpp` | Viewport-culled `MarkdownRenderer` supporting `Canvas`, `GpuCanvas`, and `PdfCanvas` via `ICanvas` with recursive text attribute inheritance and TrueType `text::Buffer` rendering. |
| `include/nisaba/markdown/pdf_exporter.hpp`<br>`src/markdown/pdf_exporter.cpp` | Multi-page ISO 32000-1 vector PDF exporter (`MarkdownPdfExporter`) with automatic pagination, headers, footers, page numbers, and document metadata. |

---

## 2. AST Data Model (`types.hpp`)

### 2.1 Source Positions (`SourcePos`)
Every `BlockNode` and `InlineSpan` carries a 1-based source range referencing its exact origin in the input Markdown stream:

```cpp
struct SourcePos {
    uint32_t start_line{0}; // 1-based line number (0 = unspecified)
    uint32_t start_col{0};  // 1-based byte column within start_line
    uint32_t end_line{0};
    uint32_t end_col{0};

    [[nodiscard]] bool is_valid() const noexcept;
};
```

`SourcePos` enables synchronized split-pane editor scrolling, click-to-source navigation, AST diagnostics, and `data-sourcepos="sl:sc-el:ec"` HTML export attributes.

### 2.2 Block & Inline Node Taxonomy

- **`BlockType` (16 variants):**
  `Document`, `Heading`, `Paragraph`, `BlockQuote`, `CodeBlock`, `List`, `ListItem`, `Table`, `ThematicBreak`, `Alert`, `Details`, `HtmlBlock`, `FootnoteDef`, `MathBlock`, `Diagram`, `Custom`.
- **`InlineType` (20 variants):**
  `Text`, `Bold`, `Italic`, `BoldItalic`, `Strikethrough`, `CodeSpan`, `Link`, `Image`, `Kbd`, `Subscript`, `Superscript`, `Underline`, `Highlight`, `Emoji`, `FootnoteRef`, `LineBreak`, `SoftBreak`, `Math`, `HtmlInline`, `Custom`.
- **`AlertType` (6 variants):**
  `None`, `Note`, `Tip`, `Important`, `Warning`, `Caution`.
- **`TableAlign` (4 variants):**
  `None`, `Left`, `Center`, `Right`.

### 2.3 Parser Configuration (`MarkdownParseOptions`)

```cpp
struct MarkdownParseOptions {
    bool hard_breaks{false};        // Treat soft line breaks as hard LineBreak
    bool no_breaks{false};          // Emit soft line breaks as plain spaces
    bool unsafe_html{false};        // Allow raw HTML blocks & raw inline HTML tags
    bool validate_utf8{false};      // Replace invalid UTF-8 sequences & NUL with U+FFFD
    bool smart_punctuation{false};  // Convert --, ---, ..., and straight quotes to typographic forms
    bool tagfilter{true};           // Neutralize GFM disallowed raw HTML tags
    bool autolink{true};            // GFM autolink extension (www., http(s)://, email)
    bool strikethrough{true};       // GFM ~~strikethrough~~
    bool tables{true};              // GFM pipe tables
    bool task_lists{true};          // GFM task list checkboxes [ ] / [x]
    bool math{true};                // LaTeX math ($...$ and $$...$$)
    bool diagrams{true};            // Diagram fenced code blocks
    bool emojis{true};              // :shortcode: emoji expansion
};
```

---

## 3. CommonMark 0.30 & GFM Parser (`parser.hpp`)

### 3.1 Incremental / Streaming Architecture (`feed` + `snapshot` + `finish`)
`MarkdownParser` is a stateful streaming block parser designed for live token-by-token streaming and large documents without re-parsing previously committed blocks from byte 0:

```cpp
MarkdownParseOptions opts;
opts.smart_punctuation = true;
opts.validate_utf8 = true;

MarkdownParser parser(opts);
while (auto chunk = receive_next_chunk()) {
    parser.feed(chunk);
    // Optional live preview AST reflecting finalized blocks + active trailing block
    auto live_ast = parser.snapshot();
}
auto final_ast = parser.finish();
```

Key streaming mechanics:
- **Zero-Copy Line Slicing:** When a complete line lies contiguously inside a `feed()` chunk, it is dispatched directly as a `std::string_view` without copying into an intermediate buffer.
- **Chunk-Boundary Safety:** Multi-byte UTF-8 sequences split across chunks are held in an internal 1–3 byte carry buffer until complete (or replaced with `U+FFFD` on `finish()`). Split `\r\n` sequences are merged cleanly across chunk boundaries.
- **Deferred Forward Reference Resolution:** Blocks referencing link labels defined later in the stream (`[label][later]`) are selectively updated when new reference definitions arrive, ensuring identical AST output between arbitrary chunk sizes (down to 1-byte chunks) and one-shot `MarkdownParser::parse()`.
- **Reusable Instance:** `parser.reset()` clears streaming buffers and AST state for reuse.

### 3.2 Block-Level Grammar Support
- **ATX Headings (`#`–`######`) & Setext Headings (`===` / `---`):** Computes GitHub-compatible kebab-case `anchor_id` slugs via `MarkdownParser::slugify()` and marks `BlockNode::is_setext_heading`.
- **Fenced & Indented Code Blocks:**
  - Fenced blocks with 3+ backticks (`` ``` ``) or tildes (`~~~`), opening indentation stripping, and unescaped language info string extraction.
  - 4-space / tab-stop indented code blocks (`BlockNode::is_indented_code`) preserving internal blank lines and stripping trailing blank lines; cannot interrupt open paragraphs.
- **BlockQuotes & GitHub Callout Alerts:**
  - Supports nested sub-documents and CommonMark lazy continuation lines.
  - Recognizes `> [!NOTE]`, `> [!TIP]`, `> [!IMPORTANT]`, `> [!WARNING]`, and `> [!CAUTION]` with optional custom title overrides.
- **Tight vs. Loose Lists & GFM Task Lists:**
  - Unordered (`-`, `*`, `+`) and ordered (`1.`, `1)`) lists with nesting levels, indented multi-block continuations, lazy paragraph continuations, and GFM task items (`[ ]`, `[x]`, `[X]`).
  - Accurately sets both `BlockNode::is_tight_list` and `BlockNode::is_loose_list` based on blank-line separation between items or between sub-blocks inside an item.
- **GFM Pipe Tables:** Column alignment parsing (`:---`, `:---:`, `---:`), escaped pipe handling (`\|`), inline code pipe protection, and per-cell inline AST parsing.
- **Complete Link Reference Definitions:**
  - Supports `[label]: destination "title"` with bare URLs (balanced parentheses), angle-bracketed URLs (`<https://...>`), single-quoted / double-quoted / parenthesized titles spanning multiple lines, Unicode case-folded label matching, and first-definition-wins semantics.
  - Definitions inside fenced or indented code blocks are ignored.
- **Structured Containers & Specialized Blocks:**
  - Collapsible `<details>` / `<summary>` blocks (`is_open`, `summary_text`).
  - Block alignment containers (`<div align="center|right">`, `<p align="...">`).
  - Raw HTML blocks (CommonMark types 1–7) gated by `options.unsafe_html`.
  - Footnote definitions (`[^label]: ...`) with automatic back-reference links.
  - Display LaTeX math (`$$ ... $$`, ` ```math `, ` ```latex `, ` ```tex `) and Diagram blocks (` ```mermaid `).

### 3.3 Inline Delimiter Stack & GFM Inline Extensions
Inline parsing executes an in-place CommonMark 0.30 delimiter-stack algorithm (zero `std::vector::erase` or `std::vector::insert` shifting) with a 256-byte fast-skip table and a zero-overhead fast path for plain text runs:
- **Emphasis & Strikethrough:** Full `left_flanking` / `right_flanking` Unicode punctuation and whitespace rules, intraword underscore preservation (`foo_bar_baz`), intraword asterisk emphasis (`foo*bar*baz`), Rule of 3 delimiter-length matching, nested emphasis (`**bold *and italic* bold**`, `***bold italic***`), and GFM `~~strikethrough~~` / `~strikethrough~`.
- **Links & Images:** Balanced nested brackets (`[outer [inner] text](url)`), inline links with destinations and titles, full (`[text][label]`), collapsed (`[text][]`), and shortcut (`[text]`) reference links and images, and strict non-nesting of links inside links.
- **Multi-Backtick Code Spans:** Exact backtick run matching with newline-to-space normalization and CommonMark single-space stripping (`` `` ` `` `` → `` ` ``).
- **Line Breaks:** Backslash hard breaks (`\` + newline), two-or-more trailing space hard breaks, and configurable soft breaks (`SoftBreak`, `hard_breaks`, or `no_breaks`).
- **Strict GFM Autolinks & Angle Autolinks:**
  - CommonMark `<https://...>` and `<user@domain.tld>`.
  - GFM bare autolinks for `http://`, `https://`, `www.` (prepends `http://`), and email addresses (`mailto:`), enforcing domain label rules (no `_` in last two segments) and trailing punctuation stripping (`?`, `!`, `.`, `,`, `:`, `*`, `_`, `~`, trailing `&entity;`, and unmatched `)` while preserving balanced parentheses such as `https://en.wikipedia.org/wiki/C_(programming_language)`).
- **Smart Punctuation (`smart_punctuation = true`):** Converts `---` → `—`, `--` → `–`, `...` → `…`, and straight `"` / `'` to directional curly quotes (`“ ”`, `‘ ’`) and apostrophes (`’`) outside code spans, URLs, HTML tags, and escaped characters.
- **Inline Math, Footnotes & Emojis:** Currency-safe `$...$` / `$$...$$` inline LaTeX math, `[^label]` footnote references, and `:shortcode:` UTF-8 emoji expansion.

---

## 4. HTML5 Entities, Escapes & Unicode (`entities.hpp`)

`Entities` provides zero-dependency text decoding and validation primitives:
- **Complete HTML5 Named Entities:** Compile-time sorted table of all **2,125** standard semicolon-terminated HTML5 named character references (including multi-codepoint entities), resolved in $O(\log N)$ time via `std::lower_bound` with zero heap allocations.
- **Numeric & Hexadecimal References:** Decodes `&#123;` (up to 7 decimal digits) and `&#x1F600;` / `&#X41;` (up to 6 hex digits), mapping `0`, UTF-16 surrogates (`U+D800`–`U+DFFF`), and codepoints `> U+10FFFF` to `U+FFFD`.
- **Backslash Escapes:** Strictly handles the 32 CommonMark ASCII punctuation characters (``!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~``) while preserving non-punctuation backslashes literally (`\a` → `\a`).
- **UTF-8 Validation (`is_valid_utf8` / `sanitize_utf8`):** SWAR 8-byte fast-path ASCII validator that avoids any buffer copy on valid UTF-8 streams; invalid byte sequences and embedded `NUL` bytes are replaced with `U+FFFD`.
- **Unicode Label Normalization (`normalize_reference_label`):** Trims leading/trailing whitespace, collapses internal whitespace runs, and applies Unicode case folding (ASCII, Latin-1/Extended-A, German `ß`/`ẞ` → `ss`, Greek including final sigma `ς` → `σ`, Cyrillic, Fullwidth Latin).

---

## 5. HTML Parsing & GFM Security (`html_parser.hpp`)

- **Structured Safe HTML Elements:** Parses `<kbd>`, `<sub>`, `<sup>`, `<mark>`, `<u>`/`<ins>`, `<b>`/`<strong>`, `<i>`/`<em>`, `<s>`/`<del>`/`<strike>`, `<code>`, `<span style="...">`, `<font color="...">`, `<a href="...">`, `<img src="..." alt="...">`, `<br>`, `<details>`, `<summary>`, and `<div align="...">` into typed AST nodes and styling overrides.
- **CSS Inline Style & Color Parser:** `HtmlParser::parse_style()` and `HtmlParser::parse_color()` support named colors, `#RGB`, `#RGBA`, `#RRGGBB`, `#RRGGBBAA`, `rgb(...)`, and `rgba(...)`.
- **GFM `Tagfilter` Extension:**
  - `HtmlParser::is_tagfilter_disallowed()` and `HtmlParser::apply_tagfilter()` neutralize the 9 disallowed raw HTML tags (case-insensitive, both opening and closing forms):
    `<title>`, `<textarea>`, `<style>`, `<xmp>`, `<iframe>`, `<noembed>`, `<noframes>`, `<script>`, and `<plaintext>`.
  - Neutralization replaces the leading `<` with `&lt;` while preserving safe raw HTML tags when `unsafe_html = true`.

---

## 6. Pluggable Syntax Extensions (`extension.hpp`)

Host applications can introduce custom block and inline directives (such as `@mentions`, `#issue-links`, or custom callout banners) without modifying core parser or exporter code:

```cpp
class MentionExtension final : public nisaba::markdown::MarkdownExtension {
public:
    std::string_view name() const noexcept override { return "mentions"; }

    bool try_parse_inline(
        const InlineExtensionContext& ctx,
        InlineSpan& out_span,
        size_t& out_bytes_consumed
    ) const override {
        auto rem = ctx.remaining();
        if (rem.size() >= 2 && rem[0] == '@') {
            size_t end = 1;
            while (end < rem.size() && std::isalnum(static_cast<unsigned char>(rem[end]))) end++;
            if (end > 1) {
                std::string handle(rem.substr(0, end));
                out_span = InlineSpan::make_custom("mention", handle, "/users/" + handle.substr(1));
                out_bytes_consumed = end;
                return true;
            }
        }
        return false;
    }

    bool render_html_inline(const InlineSpan& span, std::string& out_html) const override {
        if (span.custom_kind == "mention") {
            out_html += "<a class=\"mention\" href=\"" + span.target + "\">" + span.text + "</a>";
            return true;
        }
        return false;
    }
};
```

Extensions are registered on a `MarkdownExtensionRegistry` (for one-shot `MarkdownParser::parse`, `MarkdownDocument::from_string`, and `MarkdownExporter::to_html`) or directly on a streaming `MarkdownParser` instance via `register_extension()`.

---

## 7. Multi-Format AST Exporters (`exporter.hpp`)

`MarkdownExporter` provides three zero-dependency AST serializers (also exposed directly on `MarkdownDocument`):

1. **HTML5 Exporter (`to_html`):**
   - Emits clean GFM-compatible HTML5 (`<h1>`–`<h6>`, `<p>`, `<pre><code class="language-...">`, `<blockquote>`, `<ul>`/`<ol>` with tight `<li>text</li>` vs. loose `<li><p>text</p></li>`, task `<input type="checkbox" disabled="" />`, aligned `<table>`/`<thead>`/`<tbody>`, `<del>`, `<kbd>`, `<mark>`, `<details>`, callout `<div>` alerts, math, diagrams, and custom extension hooks).
   - Configured via `MarkdownHtmlExportOptions`: `unsafe_html` (emits `<!-- raw HTML omitted -->` when `false`), `tagfilter`, `hard_breaks`, `no_breaks`, `sourcepos` (`data-sourcepos="..."`), and `extensions`.
2. **Plaintext Extractor (`to_plaintext`):**
   - Strips formatting markers and raw HTML, converts inline and display LaTeX math into readable Unicode via `MathEngine::to_plain_text()`, preserves paragraph/list/tab-delimited table readability for clipboard copy, full-text search indexing, and screen readers.
3. **CommonMark / GFM Round-Trip Formatter (`to_markdown`):**
   - Serializes any AST (including programmatically edited trees) back into clean normalized CommonMark + GFM source with automatic code-fence length escaping, aligned pipe tables, task markers, and configurable `MarkdownFormatOptions` (`bullet_char`, `emphasis_char`, `prefer_atx_headings`).

---

## 8. Document Layout, Interactivity & Theming (`document.hpp`, `style.hpp`, `syntax_highlighter.hpp`)

- **`MarkdownDocument`:**
  - Loads documents via `from_string()` and `from_file()` (with optional `MarkdownParseOptions` and `MarkdownExtensionRegistry`).
  - `layout(max_width, style, font_system, cache)` computes bounding boxes and total vertical height using Nisaba's native `text::Buffer` shaping, proportional column-width solving for multi-line GFM tables, tight vs. loose list item spacing, and lazy AST/layout resolution for `MathBlock` and `Diagram` nodes.
  - **Interactive Hit-Testing:** `hit_test(x, y)`, `handle_click(x, y, on_link_click)`, `get_cursor_at(x, y)` (`Default`, `Pointer`, `Text`), `toggle_details()`, `toggle_task_item()`, and `table_of_contents()`.
- **Theming & Syntax Highlighting:**
  - `MarkdownStyle::dark_theme()` and `MarkdownStyle::light_theme()` configure typography sizes, line heights, margins, colors, 5 GitHub alert themes, code/kbd/mark/details/footnote visuals, and syntax token colors.
  - `SyntaxHighlighter::tokenize_line()` tokenizes multi-line comments, strings, numbers, keywords, types, preprocessor directives, operators, and punctuation across C/C++, Python, Rust, CMake, HTML/XML, JSON, Shell, and JavaScript/TypeScript.

---

## 9. Mathematical Typesetting & Diagrams (`math_engine.hpp`, `diagram_engine.hpp`)

- **`MathEngine`:**
  - Parses inline (`$...$`) and display (`$$...$$`, ` ```math `) LaTeX expressions into a `MathNode` tree supporting fractions (`\frac`), roots (`\sqrt`), subscripts/superscripts and big-operator limits (`\sum`, `\prod`, `\int`, `\lim`), extensible delimiters (`\left(...\right)`), matrix/cases environments (`pmatrix`, `bmatrix`, `Bmatrix`, `vmatrix`, `Vmatrix`, `matrix`, `cases`), accents (`\hat`, `\bar`, `\vec`, `\tilde`, `\dot`, `\overline`, `\underline`), font styles (`\mathrm`, `\mathbf`, `\mathbb`, `\mathcal`, `\mathtt`), and Greek/mathematical symbols.
  - Solves exact 2D mathematical baseline ascent/descent metrics and renders directly onto any `ICanvas`.
- **`DiagramEngine`:**
  - Parses and lays out `flowchart` / `graph` (TD, TB, LR, BT, RL with 9 node shapes and 5 edge arrow styles), `sequenceDiagram` (actors, lifelines, messages, notes), `stateDiagram` / `stateDiagram-v2`, `xychart-beta` (bar and line series), and `pie` charts, rendering directly onto any `ICanvas`.

---

## 10. Multi-Backend Rendering & Vector PDF Export (`renderer.hpp`, `pdf_exporter.hpp`)

### 10.1 Unified RTTI-Free `ICanvas` Rendering (`MarkdownRenderer`)
`MarkdownRenderer::render()` targets CPU `Canvas`, GPU `GpuCanvas`, and `PdfCanvas` through a single `ICanvas` pipeline without `dynamic_cast`:
- Uses `ICanvas::canvas_kind()` (`CanvasKind::Software`, `CanvasKind::Gpu`, `CanvasKind::Pdf`) for RTTI-free text buffer dispatch (`text::Buffer::draw`, `GpuCanvas::draw_text_buffer`, `PdfCanvas::draw_text`).
- Performs vertical viewport culling (`scroll_y` + `canvas_height()`) so large documents only rasterize visible blocks.
- Implements recursive `text::Attrs` inheritance so nested styles (`**bold *and italic* bold**`, `[Outer [Inner] **Link**]`, `~~strikethrough~~`, `<u>underline</u>`, colored custom spans, `<kbd>`, `<mark>`, `<sub>`, `<sup>`) combine accurately without overwriting inner styles.
- Renders code blocks, syntax-highlighted tokens, alert titles, details summaries, ordered list numbers, and footnote prefixes with TrueType `text::Buffer` shaping.

### 10.2 Multi-Page Vector PDF Export (`MarkdownPdfExporter`)
`MarkdownPdfExporter::export_to_bytes()` and `export_to_file()` translate the full `MarkdownDocument` AST (including headings, paragraphs, alerts, details, syntax-formatted code blocks, tables, lists/task checkboxes, footnotes, vector math formulas, and vector diagrams) into multi-page ISO 32000-1 PDF files with configurable `MarkdownPdfExportOptions` (page size, orientation, margins, headers, footers, page numbers, title/author/subject metadata).

---

## 11. Build Configuration, Verification & Measured Throughput

### Build Options (`meson_options.txt`)
- `-Denable_markdown=true` — Builds `nisaba::markdown` into `libnisaba`.
- `-Denable_pdf=true` — Enables `MarkdownPdfExporter` (`src/markdown/pdf_exporter.cpp`).
- `-fno-rtti` — Fully supported across all headers, implementations, tests, benchmarks, and examples.

### Automated Verification & Showcase
- **Unit Test Suite:** `tests/test_markdown.cpp` (`meson test -C build markdown_test`) validates **31 test suites** covering CommonMark 0.30 blocks/inlines, streaming chunk splits (1B / 7B / 33B), `SourcePos`, all `MarkdownParseOptions`, Delimiter Stack edge cases, HTML5 entities & 32 backslash escapes, Setext/Indented/Lazy/Tight-Loose constructs, Reference Links, GFM `Tagfilter`, strict GFM `Autolink`, `MarkdownExtension`, `to_html` / `to_plaintext` / `to_markdown` round-trip parity, Math, Diagrams, PDF export, and RTTI-free `ICanvas` rendering parity.
- **Visual Parity Showcase:** `examples/showcase_markdown_parity.cpp` generates dark/light PNGs, multi-page vector PDF, `data-sourcepos` HTML5, plaintext, and normalized round-trip Markdown outputs into `showcase/`.

### Measured Throughput (`benchmarks/benchmark_markdown.cpp`)
Measured on a composite 153.5 KB (1,600-block) CommonMark + GFM stress-test document with `smart_punctuation`, `validate_utf8`, `tagfilter`, `autolink`, and full `SourcePos` enabled (`-O3 -march=native`):

| Workload | Latency | Throughput |
| :--- | :---: | :---: |
| One-shot parse (1,600 blocks) | 14.5 ms | 10.4 MB/s |
| Streaming parse (`chunk = 16 B`) | 14.0 ms | 10.7 MB/s |
| Streaming parse (`chunk = 128 B`) | 12.9 ms | 11.6 MB/s |
| Streaming parse (`chunk = 4096 B`) | 14.8 ms | 10.1 MB/s |
| Exporter `to_html` | 2.69 ms | 55.8 MB/s |
| Exporter `to_plaintext` | 0.71 ms | 210.2 MB/s |
| Exporter `to_markdown` (round-trip) | 1.95 ms | 77.0 MB/s |

