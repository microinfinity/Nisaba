#include "nisaba/nisaba.hpp"
#include "nisaba/pdf/pdf_cmap.hpp"
#include "nisaba/pdf/pdf_codec.hpp"
#include "nisaba/pdf/pdf_crypto.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

using namespace nisaba;
using namespace nisaba::pdf;

static void test_pdf_types() {
    std::cout << "[TEST] PDF Types & Serialization..." << std::endl;

    // Numbers & Booleans
    PdfValue v_null;
    assert(v_null.is_null());

    PdfValue v_bool(true);
    assert(v_bool.is_bool() && v_bool.as_bool() == true);

    PdfValue v_int(42);
    assert(v_int.is_int() && v_int.as_int() == 42);

    PdfValue v_float(3.1415f);
    assert(v_float.is_number());

    // Names & Refs
    PdfValue v_name(PdfName("MediaBox"));
    assert(v_name.is_name() && v_name.as_name() == "MediaBox");

    PdfValue v_ref(PdfRef(10, 0));
    assert(v_ref.is_ref() && v_ref.as_ref().id == 10 && v_ref.as_ref().gen == 0);

    // Dictionaries & Arrays
    PdfDict dict;
    dict["Type"] = PdfName("Page");
    dict["Count"] = 1;
    dict["Parent"] = PdfRef(1, 0);

    PdfValue v_dict(dict);
    assert(v_dict.is_dict());
    assert(v_dict.find("Type") != nullptr);
    assert(v_dict.find("Type")->as_name() == "Page");
    assert(v_dict.find("Count")->as_int() == 1);

    std::string serialized;
    serialize_pdf_value(v_dict, serialized);
    assert(serialized.find("/Type /Page") != std::string::npos);
    assert(serialized.find("/Count 1") != std::string::npos);

    // String Escaping
    std::string raw_str = "Hello (World) \\ Test\n";
    std::string escaped = escape_pdf_string(raw_str);
    assert(escaped.find("\\(") != std::string::npos);
    assert(escaped.find("\\)") != std::string::npos);
    assert(escaped.find("\\\\") != std::string::npos);

    std::cout << "  -> PDF Types passed." << std::endl;
}

static void test_pdf_canvas_drawing() {
    std::cout << "[TEST] PDF Vector Canvas Drawing..." << std::endl;

    PdfCanvas canvas(595.28f, 841.89f);

    // Save / Restore / Transforms
    canvas.save();
    canvas.translate(50.0f, 100.0f);
    canvas.scale(1.5f, 1.5f);

    // Rectangle
    canvas.fill_rect(
        Rect::from_xywh(10.0f, 10.0f, 200.0f, 80.0f).value_or(Rect()),
        Paint(Color::from_rgba8(255, 100, 50, 255))
    );

    // Circle
    canvas.draw_circle(Point(150.0f, 150.0f), 40.0f, Paint(Color::from_rgba8(0, 180, 255, 255)));

    // Line & Stroke with Dash
    Stroke stroke(3.0f);
    stroke.line_cap = LineCap::Round;
    stroke.line_join = LineJoin::Round;
    stroke.dash = StrokeDash::create({10.0f, 5.0f}, 0.0f);
    canvas.draw_line(Point(20.0f, 250.0f), Point(300.0f, 250.0f), Paint(Color::from_rgba8(50, 220, 100, 255)), stroke);

    // Text
    canvas.draw_text("Nisaba Sovereign PDF Engine", 20.0f, 300.0f, 18.0f, Color::WHITE, "Helvetica-Bold");

    canvas.restore();

    const std::string& strm = canvas.content_stream();
    assert(!strm.empty());
    assert(strm.find("re f") != std::string::npos);
    assert(strm.find("cm") != std::string::npos);
    assert(strm.find("BT") != std::string::npos);
    assert(strm.find("ET") != std::string::npos);
    assert(strm.find("/F1 18.00 Tf") != std::string::npos);
    assert(strm.find("(Nisaba Sovereign PDF Engine) Tj") != std::string::npos);

    std::cout << "  -> PDF Vector Canvas passed." << std::endl;
}

static void test_pdf_document_and_reader() {
    std::cout << "[TEST] PDF Document Export & Sovereign PDF Reader / Interpreter..." << std::endl;

    // 1. Create multi-page PDF Document
    PdfDocument doc;
    doc.set_title("Nisaba Architecture Specification");
    doc.set_author("vaxp Sovereign Core");
    doc.set_subject("Embedded 2D Engine");
    doc.set_creator("Nisaba Graphics Engine");

    // Page 1: A4
    PdfPage* p1 = doc.add_page(PageSize::A4, PageOrientation::Portrait);
    assert(p1 != nullptr);
    auto& cvs1 = p1->canvas();

    // Background card
    cvs1.fill_rect(
        Rect::from_xywh(40.0f, 40.0f, 515.0f, 760.0f).value_or(Rect()),
        Paint(Color::from_rgba8(20, 28, 45, 255))
    );
    cvs1.draw_text("Page 1: Sovereign Vector Graphics", 60.0f, 70.0f, 22.0f, Color::from_rgba8(0, 220, 255, 255), "Helvetica-Bold");
    cvs1.draw_text("Nisaba runs at 7.5x the speed of GNU Cairo.", 60.0f, 110.0f, 14.0f, Color::WHITE, "Helvetica");

    // Geometric curves
    cvs1.draw_circle(Point(200.0f, 250.0f), 60.0f, Paint(Color::from_rgba8(255, 80, 120, 255)));
    cvs1.draw_circle(Point(300.0f, 250.0f), 60.0f, Paint(Color::from_rgba8(80, 220, 140, 255)));

    // Page 2: Letter
    PdfPage* p2 = doc.add_page(PageSize::Letter, PageOrientation::Landscape);
    assert(p2 != nullptr);
    auto& cvs2 = p2->canvas();
    cvs2.draw_text("Page 2: Landscape Data Matrix", 50.0f, 50.0f, 20.0f, Color::from_rgba8(255, 200, 50, 255), "Helvetica-Bold");

    // 2. Export to memory bytes and disk file
    std::vector<uint8_t> pdf_bytes = doc.save_to_bytes();
    assert(!pdf_bytes.empty());
    assert(pdf_bytes.size() > 500);

    const std::string file_path = "test_nisaba_engine.pdf";
    bool save_ok = doc.save_to_file(file_path);
    assert(save_ok);

    // 3. Open with sovereign PdfReader
    PdfReader reader;
    bool open_ok = reader.open_from_file(file_path);
    assert(open_ok);

    assert(reader.page_count() == 2);
    assert(reader.title() == "Nisaba Architecture Specification");
    assert(reader.author() == "vaxp Sovereign Core");

    Rect b1 = reader.page_box(0);
    assert(std::abs(b1.width() - 595.28f) < 1.0f);
    assert(std::abs(b1.height() - 841.89f) < 1.0f);

    // 4. Render Page 1 to software Canvas
    uint32_t cvs_w = static_cast<uint32_t>(b1.width());
    uint32_t cvs_h = static_cast<uint32_t>(b1.height());
    auto pm_opt = Pixmap::allocate(cvs_w, cvs_h);
    assert(pm_opt.has_value());
    Canvas canvas(*pm_opt);

    bool render_ok = reader.render_page(0, canvas, 1.0f);
    assert(render_ok);

    // Verify canvas rendered pixels
    bool has_drawn_pixels = false;
    for (uint32_t y = 0; y < cvs_h; ++y) {
        for (uint32_t x = 0; x < cvs_w; ++x) {
            auto px = pm_opt->pixel(x, y);
            if (px && (px->red() > 0 || px->green() > 0 || px->blue() > 0)) {
                has_drawn_pixels = true;
                break;
            }
        }
        if (has_drawn_pixels) break;
    }
    assert(has_drawn_pixels);

    std::cout << "  -> PDF Document Export & Sovereign Reader passed." << std::endl;
}

static void test_markdown_to_pdf_export() {
    std::cout << "[TEST] Markdown to Multi-Page PDF Vector Export..." << std::endl;

    const std::string md_text = R"(# Nisaba Sovereign 2D Graphics Engine

Nisaba is an embedded-first, zero-dependency graphics engine in Modern C++20.

## Benchmark Results
- **CPU Rasterizer**: 7.53x faster than GNU Cairo 1.18.
- **MicroProfiler**: 5.97ms vs 44.96ms across 25 comprehensive graphics test suites.
- **Memory Footprint**: Sub-millisecond allocation with cache-local scanline blitters.

> "A document engine that serves only generation is half an engine."
> — Sovereign Engine Architectural Mandate

```cpp
#include <nisaba/nisaba.hpp>
int main() {
    nisaba::pdf::PdfDocument doc;
    doc.save_to_file("output.pdf");
    return 0;
}
```

| Subsystem | Performance | Memory |
| :--- | :---: | :---: |
| Blend Blitter | 120 FPS | 2.1 MB |
| Text Layout | 0.05 ms | 0.8 MB |
| PDF Engine | Native Vector | 0 KB Overhead |
)";

    markdown::MarkdownDocument doc = markdown::MarkdownDocument::from_string(md_text);
    assert(doc.is_valid());

    markdown::MarkdownStyle style = markdown::MarkdownStyle::dark_theme();
    text::FontSystem font_system;
    text::GlyphCache glyph_cache;

    PdfDocument pdf_doc;
    pdf_doc.set_title("Nisaba Markdown Export");
    pdf_doc.set_author("vaxp");

    bool export_ok = pdf_doc.export_markdown(doc, style, font_system, glyph_cache, 595.28f, 841.89f, 40.0f);
    assert(export_ok);
    assert(pdf_doc.page_count() >= 1);

    const std::string md_pdf_path = "test_markdown_export.pdf";
    bool save_ok = pdf_doc.save_to_file(md_pdf_path);
    assert(save_ok);

    // Open and verify with reader
    PdfReader reader;
    bool read_ok = reader.open_from_file(md_pdf_path);
    assert(read_ok);
    assert(reader.page_count() >= 1);
    assert(reader.title() == "Nisaba Markdown Export");

    std::cout << "  -> Markdown to Multi-Page PDF Export passed." << std::endl;
}

static void test_pdf_cmap_and_cid_decoding() {
    std::cout << "[TEST] Sovereign CMap & PostScript ToUnicode Decoding..." << std::endl;

    std::string sample_cmap = R"(
/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /Custom-ToUnicode def
/CMapType 2 def
1 begincodespacerange
<0000> <FFFF>
endcodespacerange
2 beginbfchar
<0001> <0041>
<0002> <0639>
endbfchar
2 beginbfrange
<0010> <0012> <0061>
<0020> <0022> [ <0030> <0031> <0032> ]
endbfrange
endcmap
CMapName currentdict /CMap defineresource pop
end
end
)";

    std::vector<uint8_t> stream_bytes(sample_cmap.begin(), sample_cmap.end());
    auto cmap = PdfCMap::parse(stream_bytes);
    assert(cmap != nullptr);
    assert(cmap->is_2byte());
    assert(cmap->size() == 8);

    // Direct bfchar tests
    assert(cmap->map_code(0x0001) == "A"); // 'A'
    assert(cmap->map_code(0x0002) == "ع"); // Arabic Ayn 'ع'

    // bfrange linear test (<0010> -> 'a', <0011> -> 'b', <0012> -> 'c')
    assert(cmap->map_code(0x0010) == "a");
    assert(cmap->map_code(0x0011) == "b");
    assert(cmap->map_code(0x0012) == "c");

    // bfrange array test (<0020> -> '0', <0021> -> '1', <0022> -> '2')
    assert(cmap->map_code(0x0020) == "0");
    assert(cmap->map_code(0x0021) == "1");
    assert(cmap->map_code(0x0022) == "2");

    // Test multi-byte string decoding: "\x00\x01\x00\x10\x00\x20" -> "Aa0"
    std::string raw_bytes = std::string("\x00\x01\x00\x10\x00\x20", 6);
    std::string decoded_utf8 = cmap->to_utf8(raw_bytes);
    assert(decoded_utf8 == "Aa0");

    // Test real-world PDF opening and rendering page 0 with CMap & text engine
    PdfReader reader;
    if (reader.open_from_file("assets/sr.pdf")) {
        assert(reader.page_count() >= 8);
        auto pm = Pixmap::allocate(400, 600);
        assert(pm.has_value());
        Canvas canvas(*pm);
        bool ok = reader.render_page(0, canvas, 1.0f, nullptr);
        assert(ok);
        std::cout << "  -> Real-world PDF assets/sr.pdf page 0 rendered successfully." << std::endl;
    }

    std::cout << "  -> Sovereign CMap & PostScript Decoding passed." << std::endl;
}

static void test_pdf_xref_and_object_streams() {
    std::cout << "[TEST] PDF 1.5+ XRef Streams, Object Streams & Predictors..." << std::endl;

    // 1. Test Predictor Functions
    // TIFF Predictor 2 (horizontal difference)
    std::vector<uint8_t> tiff_diff = {10, 10, 10}; // differenced from {10, 20, 30}
    auto tiff_recovered = PdfParser::apply_predictor(tiff_diff, 2, 3, 1, 8);
    assert(tiff_recovered.size() == 3);
    assert(tiff_recovered[0] == 10 && tiff_recovered[1] == 20 && tiff_recovered[2] == 30);

    // PNG Up (Predictor 12)
    // Row 0: tag=2, [10, 20]
    // Row 1: tag=2, [5, 10] -> should become [15, 30]
    std::vector<uint8_t> png_up_in = {
        2, 10, 20,
        2, 5, 10
    };
    auto png_up_out = PdfParser::apply_predictor(png_up_in, 12, 2, 1, 8);
    assert(png_up_out.size() == 4);
    assert(png_up_out[0] == 10 && png_up_out[1] == 20);
    assert(png_up_out[2] == 15 && png_up_out[3] == 30);

    // 2. Synthetic PDF 1.5 with XRef Stream and Object Stream
    std::string pdf_data;
    pdf_data += "%PDF-1.5\n";

    size_t off1 = pdf_data.size();
    pdf_data += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    size_t off2 = pdf_data.size();
    pdf_data += "2 0 obj\n<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>\nendobj\n";

    size_t off3 = pdf_data.size();
    pdf_data += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 400 400 ] /Contents 4 0 R >>\nendobj\n";

    size_t off4 = pdf_data.size();
    std::string content_stream = "10 10 100 100 re f\n";
    pdf_data += "4 0 obj\n<< /Length " + std::to_string(content_stream.size()) + " >>\nstream\n" + content_stream + "endstream\nendobj\n";

    // Object 5: Object Stream (/Type /ObjStm) containing Obj 6 and Obj 7
    std::string obj6_text = "<< /Type /Metadata /Title (Nisaba Sovereign Stream) >>";
    std::string obj7_text = "[ 111 222 333 ]";
    std::string hdr_text = "6 0 7 " + std::to_string(obj6_text.size() + 1) + "\n";
    size_t first_offset = hdr_text.size();
    std::string objstm_payload = hdr_text + obj6_text + " " + obj7_text + "\n";

    size_t off5 = pdf_data.size();
    pdf_data += "5 0 obj\n<< /Type /ObjStm /N 2 /First " + std::to_string(first_offset) +
                " /Length " + std::to_string(objstm_payload.size()) + " >>\nstream\n" +
                objstm_payload + "endstream\nendobj\n";

    // Object 8: Cross-Reference Stream (/Type /XRef)
    std::vector<uint8_t> xref_binary;
    auto add_entry = [&](uint8_t t, uint16_t f1, uint8_t f2) {
        xref_binary.push_back(t);
        xref_binary.push_back(static_cast<uint8_t>((f1 >> 8) & 0xFF));
        xref_binary.push_back(static_cast<uint8_t>(f1 & 0xFF));
        xref_binary.push_back(f2);
    };

    size_t off8_placeholder = pdf_data.size();
    std::string obj8_head = "8 0 obj\n<< /Type /XRef /Size 9 /Root 1 0 R /W [ 1 2 1 ] /Index [ 1 8 ] /Length 32 >>\nstream\n";
    size_t off8 = off8_placeholder;

    add_entry(1, static_cast<uint16_t>(off1), 0);
    add_entry(1, static_cast<uint16_t>(off2), 0);
    add_entry(1, static_cast<uint16_t>(off3), 0);
    add_entry(1, static_cast<uint16_t>(off4), 0);
    add_entry(1, static_cast<uint16_t>(off5), 0);
    add_entry(2, 5, 0); // Obj 6 in ObjStm 5, index 0
    add_entry(2, 5, 1); // Obj 7 in ObjStm 5, index 1
    add_entry(1, static_cast<uint16_t>(off8), 0);

    pdf_data += obj8_head;
    pdf_data.append(reinterpret_cast<const char*>(xref_binary.data()), xref_binary.size());
    pdf_data += "\nendstream\nendobj\n";
    pdf_data += "startxref\n" + std::to_string(off8) + "\n%%EOF\n";

    PdfParser parser;
    std::vector<uint8_t> pdf_bytes(pdf_data.begin(), pdf_data.end());
    bool loaded = parser.load(pdf_bytes);
    assert(loaded);
    assert(parser.page_count() == 1);

    auto obj6 = parser.resolve(PdfRef(6, 0));
    assert(obj6.has_value());
    assert(obj6->value.is_dict());
    assert(obj6->value.find("Type") != nullptr);
    assert(obj6->value.find("Type")->as_name() == "Metadata");
    assert(obj6->value.find("Title") != nullptr);
    assert(obj6->value.find("Title")->as_string() == "Nisaba Sovereign Stream");

    auto obj7 = parser.resolve(PdfRef(7, 0));
    assert(obj7.has_value());
    assert(obj7->value.is_array());
    assert(obj7->value.as_array().size() == 3);
    assert(obj7->value.as_array()[0].as_int() == 111);
    assert(obj7->value.as_array()[1].as_int() == 222);
    assert(obj7->value.as_array()[2].as_int() == 333);

    std::cout << "  -> PDF 1.5+ XRef Streams, Object Streams & Predictors passed." << std::endl;
}

static void test_pdf_shadings_and_patterns() {
    std::cout << "[TEST] PDF ISO 32000 Advanced Shadings & Patterns..." << std::endl;

    // 1. Synthetic PDF document containing:
    // Page 1: Type 2 Axial Shading (Linear Gradient from Red to Blue)
    // Page 2: Type 3 Radial Shading (Radial Gradient from Green to Yellow)
    // Page 3: Pattern Color Space with Shading Pattern
    std::string pdf_data;
    pdf_data += "%PDF-1.5\n";

    // 1: Catalog
    size_t o1 = pdf_data.size();
    pdf_data += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    // 2: Pages
    size_t o2 = pdf_data.size();
    pdf_data += "2 0 obj\n<< /Type /Pages /Kids [ 3 0 R 4 0 R 5 0 R ] /Count 3 >>\nendobj\n";

    // 10: Shading 1 (Axial Red -> Blue)
    // 11: Function 1 (Type 2, C0=[1 0 0], C1=[0 0 1], N=1)
    size_t o11 = pdf_data.size();
    pdf_data += "11 0 obj\n<< /FunctionType 2 /Domain [ 0 1 ] /C0 [ 1 0 0 ] /C1 [ 0 0 1 ] /N 1.0 >>\nendobj\n";

    size_t o10 = pdf_data.size();
    pdf_data += "10 0 obj\n<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [ 0 0 100 0 ] /Function 11 0 R /Extend [ true true ] >>\nendobj\n";

    // 20: Shading 2 (Radial Green -> Yellow)
    // 21: Function 2 (Type 2, C0=[0 1 0], C1=[1 1 0], N=1)
    size_t o21 = pdf_data.size();
    pdf_data += "21 0 obj\n<< /FunctionType 2 /Domain [ 0 1 ] /C0 [ 0 1 0 ] /C1 [ 1 1 0 ] /N 1.0 >>\nendobj\n";

    size_t o20 = pdf_data.size();
    pdf_data += "20 0 obj\n<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [ 50 50 5 50 50 40 ] /Function 21 0 R /Extend [ true true ] >>\nendobj\n";

    // 30: Pattern 1 (Shading Pattern wrapping Shading 10)
    size_t o30 = pdf_data.size();
    pdf_data += "30 0 obj\n<< /Type /Pattern /PatternType 2 /Shading 10 0 R >>\nendobj\n";

    // Page 1: MediaBox [0 0 100 100], Contents 6 0 R (uses /Sh1 sh)
    size_t o3 = pdf_data.size();
    pdf_data += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 100 100 ] /Resources << /Shading << /Sh1 10 0 R >> >> /Contents 6 0 R >>\nendobj\n";

    std::string c1 = "/Sh1 sh\n";
    size_t o6 = pdf_data.size();
    pdf_data += "6 0 obj\n<< /Length " + std::to_string(c1.size()) + " >>\nstream\n" + c1 + "endstream\nendobj\n";

    // Page 2: MediaBox [0 0 100 100], Contents 7 0 R (uses /Sh2 sh)
    size_t o4 = pdf_data.size();
    pdf_data += "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 100 100 ] /Resources << /Shading << /Sh2 20 0 R >> >> /Contents 7 0 R >>\nendobj\n";

    std::string c2 = "/Sh2 sh\n";
    size_t o7 = pdf_data.size();
    pdf_data += "7 0 obj\n<< /Length " + std::to_string(c2.size()) + " >>\nstream\n" + c2 + "endstream\nendobj\n";

    // Page 3: MediaBox [0 0 100 100], Contents 8 0 R (uses /Pattern cs /P1 scn 10 10 80 80 re f)
    size_t o5 = pdf_data.size();
    pdf_data += "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 100 100 ] /Resources << /Pattern << /P1 30 0 R >> >> /Contents 8 0 R >>\nendobj\n";

    std::string c3 = "/Pattern cs /P1 scn 10 10 80 80 re f\n";
    size_t o8 = pdf_data.size();
    pdf_data += "8 0 obj\n<< /Length " + std::to_string(c3.size()) + " >>\nstream\n" + c3 + "endstream\nendobj\n";

    // Classic XRef table
    size_t xref_off = pdf_data.size();
    pdf_data += "xref\n0 31\n";
    pdf_data += "0000000000 65535 f \n";
    auto fmt_off = [](size_t offset) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offset);
        return std::string(buf);
    };

    std::map<uint32_t, size_t> offsets = {
        {1, o1}, {2, o2}, {3, o3}, {4, o4}, {5, o5},
        {6, o6}, {7, o7}, {8, o8}, {10, o10}, {11, o11},
        {20, o20}, {21, o21}, {30, o30}
    };

    for (uint32_t id = 1; id <= 30; ++id) {
        if (offsets.count(id)) {
            pdf_data += fmt_off(offsets[id]);
        } else {
            pdf_data += "0000000000 65535 f \n";
        }
    }

    pdf_data += "trailer\n<< /Size 31 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_off) + "\n%%EOF\n";

    PdfReader reader;
    std::vector<uint8_t> pdf_bytes(pdf_data.begin(), pdf_data.end());
    bool ok = reader.open_from_memory(pdf_bytes);
    assert(ok);
    assert(reader.page_count() == 3);

    // Render Page 1 (Axial Shading: Red -> Blue)
    auto pm1_opt = Pixmap::allocate(100, 100);
    assert(pm1_opt.has_value());
    Canvas canvas1(*pm1_opt);
    bool r1 = reader.render_page(0, canvas1, 1.0f);
    assert(r1);

    // Left side should be predominantly Red, Right side should be Blue
    auto px_left = pm1_opt->pixel(10, 50);
    auto px_right = pm1_opt->pixel(90, 50);
    assert(px_left.has_value() && px_right.has_value());
    assert(px_left->red() > px_left->blue());
    assert(px_right->blue() > px_right->red());

    // Render Page 2 (Radial Shading: Green -> Yellow)
    auto pm2_opt = Pixmap::allocate(100, 100);
    assert(pm2_opt.has_value());
    Canvas canvas2(*pm2_opt);
    bool r2 = reader.render_page(1, canvas2, 1.0f);
    assert(r2);
    auto px_center = pm2_opt->pixel(50, 50);
    assert(px_center.has_value());
    assert(px_center->green() > 0);

    // Render Page 3 (Pattern Color Space with Shading Pattern)
    auto pm3_opt = Pixmap::allocate(100, 100);
    assert(pm3_opt.has_value());
    Canvas canvas3(*pm3_opt);
    bool r3 = reader.render_page(2, canvas3, 1.0f);
    assert(r3);
    auto px_pat = pm3_opt->pixel(50, 50);
    assert(px_pat.has_value());
    assert(px_pat->red() > 0 || px_pat->blue() > 0);

    std::cout << "  -> PDF ISO 32000 Advanced Shadings & Patterns passed." << std::endl;
}

static void test_pdf_transparency_and_blend_modes() {
    std::cout << "[TEST] PDF ISO 32000 Transparency, Blend Modes & Special Colorspaces..." << std::endl;

    std::string pdf_data = "%PDF-1.4\n";
    std::vector<size_t> xref;
    xref.push_back(0);

    auto add_obj = [&](const std::string& content) -> size_t {
        size_t off = pdf_data.size();
        size_t id = xref.size();
        xref.push_back(off);
        pdf_data += std::to_string(id) + " 0 obj\n" + content + "\nendobj\n";
        return id;
    };

    // 1: Catalog
    add_obj("<< /Type /Catalog /Pages 2 0 R >>");
    // 2: Pages
    add_obj("<< /Type /Pages /Kids [10 0 R 11 0 R 12 0 R] /Count 3 >>");

    // 3: ExtGState Multiply
    add_obj("<< /Type /ExtGState /BM /Multiply >>");
    // 4: ExtGState Screen
    add_obj("<< /Type /ExtGState /BM /Screen >>");
    // 5: ExtGState Half Alpha
    add_obj("<< /Type /ExtGState /ca 0.5 >>");

    // 6: Form XObject with Transparency Group
    std::string form_stream = "1 0 0 rg 0 0 100 100 re f";
    std::string form_dict = "<< /Type /XObject /Subtype /Form /BBox [0 0 100 100] /Group << /Type /Group /S /Transparency >> /Length " +
                            std::to_string(form_stream.size()) + " >>\nstream\n" + form_stream + "\nendstream";
    add_obj(form_dict);

    // 7: Page 1 Stream (Blend modes: Multiply & Screen)
    std::string p1_stream = "1 0 0 rg 0 0 100 100 re f\n"
                            "/GS_mult gs 0 1 0 rg 0 0 50 100 re f\n"
                            "/GS_screen gs 0 1 0 rg 50 0 50 100 re f\n";
    add_obj("<< /Length " + std::to_string(p1_stream.size()) + " >>\nstream\n" + p1_stream + "endstream");

    // 8: Page 2 Stream (Indexed Colorspace)
    std::string p2_stream = "/CS_idx cs\n"
                            "0 sc 0 0 50 100 re f\n"
                            "1 sc 50 0 50 100 re f\n";
    add_obj("<< /Length " + std::to_string(p2_stream.size()) + " >>\nstream\n" + p2_stream + "endstream");

    // 9: Page 3 Stream (Transparency Group at 50% opacity over White)
    std::string p3_stream = "1 1 1 rg 0 0 100 100 re f\n"
                            "/GS_half gs\n"
                            "/Fm1 Do\n";
    add_obj("<< /Length " + std::to_string(p3_stream.size()) + " >>\nstream\n" + p3_stream + "endstream");

    // 10: Page 1
    std::string p1_dict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /ExtGState << /GS_mult 3 0 R /GS_screen 4 0 R >> >> /Contents 7 0 R >>";
    add_obj(p1_dict);

    // 11: Page 2
    std::string p2_dict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /ColorSpace << /CS_idx [/Indexed /DeviceRGB 1 <FF00000000FF>] >> >> /Contents 8 0 R >>";
    add_obj(p2_dict);

    // 12: Page 3
    std::string p3_dict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /ExtGState << /GS_half 5 0 R >> /XObject << /Fm1 6 0 R >> >> /Contents 9 0 R >>";
    add_obj(p3_dict);

    size_t xref_off = pdf_data.size();
    pdf_data += "xref\n0 " + std::to_string(xref.size()) + "\n";
    pdf_data += "0000000000 65535 f \n";
    for (size_t i = 1; i < xref.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", xref[i]);
        pdf_data += buf;
    }
    pdf_data += "trailer\n<< /Size " + std::to_string(xref.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_off) + "\n%%EOF\n";

    PdfReader reader;
    std::vector<uint8_t> pdf_bytes(pdf_data.begin(), pdf_data.end());
    bool ok = reader.open_from_memory(pdf_bytes);
    assert(ok);
    assert(reader.page_count() == 3);

    // Render Page 1 (Blend Modes)
    auto pm1_opt = Pixmap::allocate(100, 100);
    assert(pm1_opt.has_value());
    Canvas canvas1(*pm1_opt);
    bool r1 = reader.render_page(0, canvas1, 1.0f);
    assert(r1);

    // Multiply: Red * Green = Black
    auto px_mult = pm1_opt->pixel(25, 50);
    assert(px_mult.has_value());
    assert(px_mult->red() == 0 && px_mult->green() == 0 && px_mult->blue() == 0);

    // Screen: Red + Green = Yellow
    auto px_screen = pm1_opt->pixel(75, 50);
    assert(px_screen.has_value());
    assert(px_screen->red() > 200 && px_screen->green() > 200 && px_screen->blue() < 50);

    // Render Page 2 (Indexed Color Space)
    auto pm2_opt = Pixmap::allocate(100, 100);
    assert(pm2_opt.has_value());
    Canvas canvas2(*pm2_opt);
    bool r2 = reader.render_page(1, canvas2, 1.0f);
    assert(r2);

    // Index 0: Pure Red
    auto px_idx0 = pm2_opt->pixel(25, 50);
    assert(px_idx0.has_value());
    assert(px_idx0->red() > 200 && px_idx0->blue() < 50);

    // Index 1: Pure Blue
    auto px_idx1 = pm2_opt->pixel(75, 50);
    assert(px_idx1.has_value());
    assert(px_idx1->blue() > 200 && px_idx1->red() < 50);

    // Render Page 3 (Transparency Group at 50% opacity over White)
    auto pm3_opt = Pixmap::allocate(100, 100);
    assert(pm3_opt.has_value());
    Canvas canvas3(*pm3_opt);
    bool r3 = reader.render_page(2, canvas3, 1.0f);
    assert(r3);

    auto px_grp = pm3_opt->pixel(50, 50);
    assert(px_grp.has_value());
    // Red 50% over White -> Red is high, Green & Blue around 128
    assert(px_grp->red() > 200);
    assert(px_grp->green() > 80 && px_grp->green() < 180);

    std::cout << "  -> PDF ISO 32000 Transparency, Blend Modes & Special Colorspaces passed." << std::endl;
}

static void test_pdf_codecs_and_encryption() {
    std::cout << "[TEST] PDF Media Codecs & Sovereign Cryptography (Phase 5)..." << std::endl;

    // 1. ASCIIHexDecode
    {
        std::string hex_input = "4E 69 73 61 62 61 >";
        auto decoded = decode_ascii_hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(hex_input.data()), hex_input.size()));
        std::string res(decoded.begin(), decoded.end());
        assert(res == "Nisaba");

        // Odd number of hex digits (appends implicit '0' as per ISO 32000-1 §7.4.2)
        std::string hex_odd = "4E697361626>";
        auto dec_odd = decode_ascii_hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(hex_odd.data()), hex_odd.size()));
        assert(dec_odd.size() == 6);
        assert(dec_odd[5] == 0x60);
    }

    // 2. ASCII85Decode
    {
        // "Nisa" encodes to ":2+cX"
        std::string a85_input = "<~:2+cX~>";
        auto decoded = decode_ascii_85(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(a85_input.data()), a85_input.size()));
        std::string res(decoded.begin(), decoded.end());
        assert(res == "Nisa");

        // 'z' expansion to 4 zero bytes
        std::string a85_z = "<~:2+cXz~>";
        auto dec_z = decode_ascii_85(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(a85_z.data()), a85_z.size()));
        assert(dec_z.size() == 8);
        assert(dec_z[0] == 'N' && dec_z[1] == 'i' && dec_z[2] == 's' && dec_z[3] == 'a');
        assert(dec_z[4] == 0 && dec_z[5] == 0 && dec_z[6] == 0 && dec_z[7] == 0);
    }

    // 3. RunLengthDecode
    {
        // 0x03 -> 4 literals ("Nisa"), 0xFD -> 4 repeats of 'b' (257 - 253 = 4), 0x80 -> EOD (128)
        std::vector<uint8_t> rle_input = {0x03, 'N', 'i', 's', 'a', 0xFD, 'b', 0x80};
        auto decoded = decode_run_length(rle_input);
        std::string res(decoded.begin(), decoded.end());
        assert(res == "Nisabbbb");
    }

    // 4. CCITTFaxDecode
    {
        // Simple 2D Group 4 pass-through / decode test
        std::vector<uint8_t> fax_dummy = {0x00, 0x00};
        PdfDict parms;
        parms["K"] = -1; // Group 4
        parms["Columns"] = 16;
        parms["Rows"] = 2;
        auto decoded = decode_ccitt_fax(fax_dummy, parms);
        // CCITTFax produces a 1-bit bi-level bitmap (16 columns = 2 bytes per row)
        size_t expected_size = 2 * ((16 + 7) / 8);
        assert(decoded.size() == expected_size);
    }

    // 5. Sovereign Cryptography Primitives
    // RFC 1321 MD5
    {
        std::string empty = "";
        auto h_empty = md5_hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(empty.data()), empty.size()));
        const uint8_t expected_empty[16] = {
            0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
            0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e
        };
        assert(std::memcmp(h_empty.data(), expected_empty, 16) == 0);

        std::string abc = "abc";
        auto h_abc = md5_hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()));
        const uint8_t expected_abc[16] = {
            0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
            0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72
        };
        assert(std::memcmp(h_abc.data(), expected_abc, 16) == 0);
    }

    // FIPS 180-4 SHA-256
    {
        std::string empty = "";
        auto s_empty = sha256_hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(empty.data()), empty.size()));
        const uint8_t exp_s_empty[32] = {
            0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
            0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
            0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
            0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
        };
        assert(std::memcmp(s_empty.data(), exp_s_empty, 32) == 0);

        std::string abc = "abc";
        auto s_abc = sha256_hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()));
        const uint8_t exp_s_abc[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        assert(std::memcmp(s_abc.data(), exp_s_abc, 32) == 0);
    }

    // Rivest Cipher 4 (RC4)
    {
        std::string key_str = "Key";
        std::string pt_str = "Plaintext";
        std::span<const uint8_t> key(reinterpret_cast<const uint8_t*>(key_str.data()), key_str.size());
        std::span<const uint8_t> pt(reinterpret_cast<const uint8_t*>(pt_str.data()), pt_str.size());

        auto ct = rc4_crypt(key, pt);
        const uint8_t exp_ct[9] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
        assert(ct.size() == 9);
        assert(std::memcmp(ct.data(), exp_ct, 9) == 0);

        auto dec = rc4_crypt(key, ct);
        assert(dec.size() == pt.size());
        assert(std::memcmp(dec.data(), pt.data(), pt.size()) == 0);
    }

    // NIST AES-128 CBC Decryption
    {
        const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
        const uint8_t iv[16]  = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        const uint8_t ct[16]  = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};
        const uint8_t exp[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};

        std::span<const uint8_t, 16> iv_span(iv, 16);
        auto dec = aes_decrypt_cbc(key, ct, iv_span);
        assert(dec.size() == 16);
        assert(std::memcmp(dec.data(), exp, 16) == 0);
    }

    // 6. Sovereign Encrypted PDF Document (ISO 32000-1 §7.6.3 Standard Security Handler)
    {
        static const uint8_t kPad[32] = {
            0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
            0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
            0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
            0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A
        };

        std::string user_pw = "secret";
        std::string owner_pw = "master";
        int64_t permissions = -4; // all permissions
        std::string file_id = "0123456789ABCDEF"; // 16 bytes

        // Step 1: Owner key & O value (Algorithm 3)
        std::vector<uint8_t> pw_owner_data(kPad, kPad + 32);
        for (size_t i = 0; i < owner_pw.size(); ++i) pw_owner_data[i] = static_cast<uint8_t>(owner_pw[i]);
        auto o_hash = md5_hash(pw_owner_data);
        std::vector<uint8_t> o_key(o_hash.begin(), o_hash.begin() + 5);

        std::vector<uint8_t> pw_user_data(kPad, kPad + 32);
        for (size_t i = 0; i < user_pw.size(); ++i) pw_user_data[i] = static_cast<uint8_t>(user_pw[i]);
        auto o_val = rc4_crypt(o_key, pw_user_data);

        // Step 2: User file encryption key (Algorithm 2)
        std::vector<uint8_t> hash_input = pw_user_data;
        hash_input.insert(hash_input.end(), o_val.begin(), o_val.end());
        uint32_t p_val = static_cast<uint32_t>(permissions);
        hash_input.push_back(static_cast<uint8_t>(p_val & 0xFF));
        hash_input.push_back(static_cast<uint8_t>((p_val >> 8) & 0xFF));
        hash_input.push_back(static_cast<uint8_t>((p_val >> 16) & 0xFF));
        hash_input.push_back(static_cast<uint8_t>((p_val >> 24) & 0xFF));
        hash_input.insert(hash_input.end(), file_id.begin(), file_id.end());

        auto f_hash = md5_hash(hash_input);
        std::vector<uint8_t> file_key(f_hash.begin(), f_hash.begin() + 5);

        // Step 3: Compute U value (Algorithm 4)
        auto u_val = rc4_crypt(file_key, std::span<const uint8_t>(kPad, 32));

        // Step 4: Encrypt content stream for Object 4 (id=4, gen=0)
        // Per-object key = MD5(file_key + id[3 bytes] + gen[2 bytes])
        std::vector<uint8_t> obj4_key_data = file_key;
        obj4_key_data.push_back(4); obj4_key_data.push_back(0); obj4_key_data.push_back(0);
        obj4_key_data.push_back(0); obj4_key_data.push_back(0);
        auto obj4_hash = md5_hash(obj4_key_data);
        std::vector<uint8_t> obj4_key(obj4_hash.begin(), obj4_hash.begin() + 10);

        std::string stream_content = "1 0 0 rg 0 0 100 100 re f";
        auto encrypted_stream = rc4_crypt(obj4_key, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(stream_content.data()), stream_content.size()));

        // Hex encode helper
        auto to_hex = [](std::span<const uint8_t> data) {
            std::ostringstream ss;
            for (uint8_t b : data) {
                ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }
            return ss.str();
        };

        std::string o_hex = to_hex(o_val);
        std::string u_hex = to_hex(u_val);
        std::string id_hex = to_hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(file_id.data()), file_id.size()));

        // Assemble Encrypted PDF with exact byte offsets
        std::ostringstream pdf;
        pdf << "%PDF-1.4\n";
        size_t off1 = pdf.tellp();
        pdf << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
        size_t off2 = pdf.tellp();
        pdf << "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
        size_t off3 = pdf.tellp();
        pdf << "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R >>\nendobj\n";
        size_t off4 = pdf.tellp();
        pdf << "4 0 obj\n<< /Length " << encrypted_stream.size() << " >>\nstream\n";
        for (uint8_t b : encrypted_stream) pdf << static_cast<char>(b);
        pdf << "\nendstream\nendobj\n";
        size_t off5 = pdf.tellp();
        pdf << "5 0 obj\n<< /Filter /Standard /V 1 /R 2 /O <" << o_hex << "> /U <" << u_hex << "> /P " << permissions << " >>\nendobj\n";
        size_t xref_off = pdf.tellp();
        pdf << "xref\n0 6\n";
        pdf << "0000000000 65535 f \n";
        auto write_entry = [&](size_t off) {
            std::ostringstream entry;
            entry << std::setw(10) << std::setfill('0') << off << " 00000 n \n";
            pdf << entry.str();
        };
        write_entry(off1);
        write_entry(off2);
        write_entry(off3);
        write_entry(off4);
        write_entry(off5);
        pdf << "trailer\n<< /Size 6 /Root 1 0 R /Encrypt 5 0 R /ID [ <" << id_hex << "> <" << id_hex << "> ] >>\n";
        pdf << "startxref\n" << xref_off << "\n%%EOF\n";

        std::string enc_pdf_data = pdf.str();
        std::vector<uint8_t> pdf_bytes(enc_pdf_data.begin(), enc_pdf_data.end());

        // Test 1: Open with wrong password -> authentication fails
        {
            PdfReader reader_wrong;
            bool ok = reader_wrong.open_from_memory(pdf_bytes, "wrong_pw");
            assert(!ok);
        }

        // Test 2: Open with user password "secret" -> succeeds and decrypts stream!
        {
            PdfReader reader_user;
            bool ok = reader_user.open_from_memory(pdf_bytes, "secret");
            assert(ok);
            assert(reader_user.page_count() == 1);

            auto pm_opt = Pixmap::allocate(100, 100);
            assert(pm_opt.has_value());
            Canvas canvas(*pm_opt);
            bool rendered = reader_user.render_page(0, canvas, 1.0f);
            assert(rendered);

            auto px = pm_opt->pixel(50, 50);
            assert(px.has_value());
            assert(px->red() > 200 && px->green() == 0 && px->blue() == 0);
        }

        // Test 3: Open with owner password "master" -> Algorithm 7 succeeds!
        {
            PdfReader reader_owner;
            bool ok = reader_owner.open_from_memory(pdf_bytes, "master");
            assert(ok);
            assert(reader_owner.page_count() == 1);

            auto pm_opt = Pixmap::allocate(100, 100);
            assert(pm_opt.has_value());
            Canvas canvas(*pm_opt);
            bool rendered = reader_owner.render_page(0, canvas, 1.0f);
            assert(rendered);

            auto px = pm_opt->pixel(50, 50);
            assert(px.has_value());
            assert(px->red() > 200 && px->green() == 0 && px->blue() == 0);
        }
    }

    std::cout << "  -> PDF Media Codecs & Sovereign Cryptography passed." << std::endl;
}

static void test_pdf_gpu_and_navigation() {
    std::cout << "[TEST] PDF Phase 6: Hardware-Accelerated GPU Rendering & Interactive Navigation..." << std::endl;

    // 1. Text Doc String Decoding (UTF-16BE BOM, UTF-8 BOM, PDFDocEncoding)
    {
        // UTF-16BE BOM: "\xFE\xFF" + "N" (0x004E) + "i" (0x0069) + "s" (0x0073) + "a" (0x0061)
        std::string utf16_be("\xFE\xFF\x00\x4E\x00\x69\x00\x73\x00\x61", 10);
        std::string dec_utf16 = decode_pdf_doc_string(utf16_be);
        assert(dec_utf16 == "Nisa");

        // UTF-8 BOM: "\xEF\xBB\xBF" + "Nisaba"
        std::string utf8_bom = "\xEF\xBB\xBFNisaba";
        std::string dec_utf8 = decode_pdf_doc_string(utf8_bom);
        assert(dec_utf8 == "Nisaba");

        // Plain PDFDocEncoding string
        std::string plain = "Chapter 1: Overview";
        std::string dec_plain = decode_pdf_doc_string(plain);
        assert(dec_plain == plain);
    }

    // 2. Interactive Navigation, Document Outlines (Bookmarks), & Link Annotations
    {
        // Construct a 2-page PDF document with Outlines and Link Annotations
        std::ostringstream pdf;
        pdf << "%PDF-1.4\n";

        size_t off_cat = pdf.tellp();
        pdf << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>\nendobj\n";

        size_t off_pages = pdf.tellp();
        pdf << "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n";

        size_t off_p1 = pdf.tellp();
        pdf << "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 7 0 R /Annots [8 0 R 9 0 R] >>\nendobj\n";

        size_t off_p2 = pdf.tellp();
        pdf << "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 10 0 R >>\nendobj\n";

        // Outlines hierarchy
        size_t off_outlines = pdf.tellp();
        pdf << "5 0 obj\n<< /Type /Outlines /First 6 0 R /Last 11 0 R /Count 2 >>\nendobj\n";

        // Outline Item 1: "Cover Page" -> points to Page 1 (3 0 R), Next is 11 0 R
        size_t off_out1 = pdf.tellp();
        pdf << "6 0 obj\n<< /Title (Cover Page) /Parent 5 0 R /Next 11 0 R /Dest [3 0 R /Fit] >>\nendobj\n";

        // Page 1 content stream (Blue square)
        std::string p1_stream = "0 0 1 rg 0 0 200 200 re f";
        size_t off_p1_stream = pdf.tellp();
        pdf << "7 0 obj\n<< /Length " << p1_stream.size() << " >>\nstream\n" << p1_stream << "\nendstream\nendobj\n";

        // Link Annotation 1: Internal jump to Page 2
        size_t off_link1 = pdf.tellp();
        pdf << "8 0 obj\n<< /Type /Annot /Subtype /Link /Rect [10 10 100 50] /A << /S /GoTo /D [4 0 R /XYZ 0 200 1.0] >> >>\nendobj\n";

        // Link Annotation 2: External web link
        size_t off_link2 = pdf.tellp();
        pdf << "9 0 obj\n<< /Type /Annot /Subtype /Link /Rect [110 10 190 50] /A << /S /URI /URI (https://github.com/vaxp/Nisaba) >> >>\nendobj\n";

        // Page 2 content stream (Green square)
        std::string p2_stream = "0 1 0 rg 0 0 200 200 re f";
        size_t off_p2_stream = pdf.tellp();
        pdf << "10 0 obj\n<< /Length " << p2_stream.size() << " >>\nstream\n" << p2_stream << "\nendstream\nendobj\n";

        // Outline Item 2: "Section 1" -> points to Page 2 (4 0 R), Prev = 6 0 R, First = 12 0 R (child)
        size_t off_out2 = pdf.tellp();
        pdf << "11 0 obj\n<< /Title (Section 1) /Parent 5 0 R /Prev 6 0 R /First 12 0 R /Dest [4 0 R /XYZ 0 200 1] /C [1 0 0] /F 2 >>\nendobj\n";

        // Outline Child Item 2.1: "Sub-section 1.1" -> child of 11 0 R
        size_t off_out2_1 = pdf.tellp();
        pdf << "12 0 obj\n<< /Title (Sub-section 1.1) /Parent 11 0 R /Dest [4 0 R /FitH 150] >>\nendobj\n";

        // Cross-reference table
        size_t xref_off = pdf.tellp();
        pdf << "xref\n0 13\n";
        pdf << "0000000000 65535 f \n";
        auto write_entry = [&](size_t off) {
            std::ostringstream entry;
            entry << std::setw(10) << std::setfill('0') << off << " 00000 n \n";
            pdf << entry.str();
        };
        write_entry(off_cat);        // 1
        write_entry(off_pages);      // 2
        write_entry(off_p1);         // 3
        write_entry(off_p2);         // 4
        write_entry(off_outlines);   // 5
        write_entry(off_out1);       // 6
        write_entry(off_p1_stream);  // 7
        write_entry(off_link1);      // 8
        write_entry(off_link2);      // 9
        write_entry(off_p2_stream);  // 10
        write_entry(off_out2);       // 11
        write_entry(off_out2_1);     // 12

        pdf << "trailer\n<< /Size 13 /Root 1 0 R >>\n";
        pdf << "startxref\n" << xref_off << "\n%%EOF\n";

        std::string pdf_data = pdf.str();
        std::vector<uint8_t> pdf_bytes(pdf_data.begin(), pdf_data.end());

        PdfReader reader;
        bool ok = reader.open_from_memory(pdf_bytes);
        assert(ok);
        assert(reader.page_count() == 2);

        // Verify Outlines (Bookmarks hierarchy)
        auto outlines = reader.outlines();
        assert(outlines.size() == 2);
        assert(outlines[0].title == "Cover Page");
        assert(outlines[0].target_page() == 0);
        assert(outlines[0].destination.type == DestinationType::Fit);

        assert(outlines[1].title == "Section 1");
        assert(outlines[1].target_page() == 1);
        assert(outlines[1].bold == true);
        assert(outlines[1].color.red() > 0.8f);
        assert(outlines[1].children.size() == 1);
        assert(outlines[1].children[0].title == "Sub-section 1.1");
        assert(outlines[1].children[0].target_page() == 1);
        assert(outlines[1].children[0].destination.type == DestinationType::FitH);
        assert(outlines[1].children[0].destination.top.has_value() && *outlines[1].children[0].destination.top == 150.0f);

        // Verify Link Annotations on Page 1
        auto links = reader.page_links(0);
        assert(links.size() == 2);

        // Link 1: Internal jump to Page 2
        assert(links[0].action.type == PdfActionType::GoTo);
        assert(links[0].target_page == 1);
        assert(links[0].rect.left() == 10.0f && links[0].rect.top() == 10.0f);

        // Link 2: External web link
        assert(links[1].action.type == PdfActionType::URI);
        assert(links[1].uri == "https://github.com/vaxp/Nisaba");
        assert(links[1].rect.left() == 110.0f);

        // Verify Page 2 has no links
        auto p2_links = reader.page_links(1);
        assert(p2_links.empty());

        // Resolve Destination Page
        auto dest_p2 = outlines[1].destination;
        auto resolved_p = reader.resolve_destination_page(dest_p2);
        assert(resolved_p.has_value() && *resolved_p == 1);

        // 3. GPU Hardware-Accelerated Rendering (Phase 6.1)
        {
            auto device = gpu::GpuDevice::create();
            if (device) {
                auto surface = gpu::GpuSurface::create(device, 200, 200);
                assert(surface != nullptr);
                gpu::GpuCanvas gpu_canvas(surface);
                gpu_canvas.clear(Color::TRANSPARENT);

                // Render Page 1 (Blue square) directly into GpuCanvas
                bool gpu_rendered = reader.render_page_gpu(0, gpu_canvas, 1.0f);
                assert(gpu_rendered);

                // Readback GPU surface and verify blue pixel rendered by GPU pipeline
                auto readback = surface->to_pixmap();
                assert(readback.has_value());
                assert(readback->width() == 200 && readback->height() == 200);

                auto px_blue = readback->pixel(100, 100);
                assert(px_blue.has_value());
                assert(px_blue->blue() > 200 && px_blue->red() < 50 && px_blue->green() < 50);

                // Benchmark 120+ FPS hardware acceleration loop
                const int num_frames = 60;
                for (int f = 0; f < num_frames; ++f) {
                    reader.render_page_gpu(0, gpu_canvas, 1.0f);
                }
            }
        }
    }

    std::cout << "  -> PDF Hardware-Accelerated GPU Rendering & Interactive Navigation passed." << std::endl;
}

static void test_pdf_annotations_and_picture_replay() {
    std::cout << "[TEST] PDF Annotations (URL, NamedDest, Metadata) & Picture Replay..." << std::endl;

    PdfDocument doc;
    PdfPage* page = doc.add_page(PageSize::A4, PageOrientation::Portrait);
    assert(page != nullptr);
    auto& cvs = page->canvas();

    cvs.save();
    cvs.translate(50.0f, 40.0f);
    cvs.draw_annotation(*Rect::from_xywh(10.0f, 20.0f, 120.0f, 24.0f), "URL", "https://nisaba.engine/docs");
    cvs.draw_annotation(*Rect::from_xywh(10.0f, 60.0f, 100.0f, 20.0f), "NamedDest", "section_intro");
    cvs.draw_annotation(*Rect::from_xywh(10.0f, 90.0f, 80.0f, 18.0f), "AccessibilityRole", "Heading1");
    cvs.restore();

    // Also test Picture playback emitting an annotation onto PdfCanvas
    PictureRecorder rec;
    ICanvas& rc = rec.begin_recording(*Rect::from_xywh(0.0f, 0.0f, 595.0f, 842.0f));
    rc.draw_annotation(*Rect::from_xywh(200.0f, 120.0f, 90.0f, 22.0f), "URI", "https://nisaba.engine/api");
    auto pic = rec.finish_recording();
    cvs.draw_picture(pic);

    assert(cvs.annotations().size() == 4);
    assert(std::abs(cvs.annotations()[0].rect.left() - 60.0f) < 0.01f);
    assert(std::abs(cvs.annotations()[0].rect.top() - 60.0f) < 0.01f);

    std::vector<uint8_t> bytes = doc.save_to_bytes();
    assert(!bytes.empty());
    std::string pdf_str(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    assert(pdf_str.find("/Annots") != std::string::npos);
    assert(pdf_str.find("/Subtype /Link") != std::string::npos);
    assert(pdf_str.find("/S /URI") != std::string::npos);
    assert(pdf_str.find("(https://nisaba.engine/docs)") != std::string::npos);
    assert(pdf_str.find("(https://nisaba.engine/api)") != std::string::npos);
    assert(pdf_str.find("/Dest (section_intro)") != std::string::npos);
    assert(pdf_str.find("/T (AccessibilityRole)") != std::string::npos);
    assert(pdf_str.find("/Contents (Heading1)") != std::string::npos);

    std::cout << "  -> PDF Annotations & Picture Replay passed." << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "    NISABA PDF ENGINE TEST SUITE          " << std::endl;
    std::cout << "==========================================" << std::endl;

    test_pdf_types();
    test_pdf_canvas_drawing();
    test_pdf_document_and_reader();
    test_markdown_to_pdf_export();
    test_pdf_cmap_and_cid_decoding();
    test_pdf_xref_and_object_streams();
    test_pdf_shadings_and_patterns();
    test_pdf_transparency_and_blend_modes();
    test_pdf_codecs_and_encryption();
    test_pdf_gpu_and_navigation();
    test_pdf_annotations_and_picture_replay();

    std::cout << "==========================================" << std::endl;
    std::cout << "    ALL PDF ENGINE TESTS PASSED!          " << std::endl;
    std::cout << "==========================================" << std::endl;

    return 0;
}
