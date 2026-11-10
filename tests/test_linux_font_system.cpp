#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "nisaba/nisaba.hpp"

using namespace nisaba;
using namespace nisaba::text;

namespace {

std::string resolve_font(std::string_view filename) {
    std::vector<std::string> candidates = {
        std::string("fonts/") + std::string(filename),
        std::string("../fonts/") + std::string(filename),
        std::string("../../fonts/") + std::string(filename)
    };
    for (const auto& path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return candidates[0];
}

// Synthesizes a valid in-memory TrueType Collection (.ttc) wrapping two SFNT fonts
std::vector<uint8_t> build_synthetic_ttc(
    std::span<const uint8_t> font0,
    std::span<const uint8_t> font1
) {
    // TTC Header v1.0:
    // 'ttcf' (4 bytes)
    // major=1, minor=0 (4 bytes: 0x00010000)
    // numFonts=2 (4 bytes: 0x00000002)
    // offset0 (4 bytes)
    // offset1 (4 bytes)
    // Total header = 20 bytes
    std::vector<uint8_t> ttc;
    size_t header_size = 20;
    size_t font0_base = header_size;
    size_t font1_base = font0_base + ((font0.size() + 3) & ~size_t(3));
    ttc.resize(font1_base + font1.size(), 0);

    auto write_u32_be = [&](size_t pos, uint32_t v) {
        ttc[pos + 0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        ttc[pos + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        ttc[pos + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        ttc[pos + 3] = static_cast<uint8_t>(v & 0xFF);
    };

    write_u32_be(0, 0x74746366u); // 'ttcf'
    write_u32_be(4, 0x00010000u); // v1.0
    write_u32_be(8, 2u);          // 2 faces
    write_u32_be(12, static_cast<uint32_t>(font0_base));
    write_u32_be(16, static_cast<uint32_t>(font1_base));

    std::memcpy(ttc.data() + font0_base, font0.data(), font0.size());
    std::memcpy(ttc.data() + font1_base, font1.data(), font1.size());

    // Patch table directory offsets inside font0 and font1 so they are absolute from the start of the TTC file
    auto patch_sfnt_offsets = [&](size_t base_offset) {
        uint16_t num_tables = static_cast<uint16_t>((ttc[base_offset + 4] << 8) | ttc[base_offset + 5]);
        for (uint16_t i = 0; i < num_tables; ++i) {
            size_t rec_pos = base_offset + 12 + static_cast<size_t>(i) * 16;
            uint32_t old_off = (static_cast<uint32_t>(ttc[rec_pos + 8]) << 24) |
                               (static_cast<uint32_t>(ttc[rec_pos + 9]) << 16) |
                               (static_cast<uint32_t>(ttc[rec_pos + 10]) << 8) |
                                static_cast<uint32_t>(ttc[rec_pos + 11]);
            write_u32_be(rec_pos + 8, old_off + static_cast<uint32_t>(base_offset));
        }
    };

    patch_sfnt_offsets(font0_base);
    patch_sfnt_offsets(font1_base);
    return ttc;
}

} // namespace

void test_linux_fontconfig_xml_parser() {
    std::cout << "[Test 1] Sovereign Linux Fontconfig XML & Alias Parser..." << std::endl;

    LinuxFontConfig cfg;
    bool loaded = cfg.load_system_config("en-US");
    assert(loaded);
    assert(!cfg.font_directories().empty());

    std::cout << "  Discovered " << cfg.font_directories().size() << " active Linux font directories:" << std::endl;
    for (const auto& d : cfg.font_directories()) {
        std::cout << "    - " << d << std::endl;
    }

    // Verify generic family resolution
    auto sans_cands = cfg.resolve_family_candidates(Family::sans_serif());
    auto serif_cands = cfg.resolve_family_candidates(Family::serif());
    auto mono_cands = cfg.resolve_family_candidates(Family::monospace());
    auto sysui_cands = cfg.resolve_family_candidates(Family::system_ui());
    auto emoji_cands = cfg.resolve_family_candidates(Family::emoji());

    assert(!sans_cands.empty());
    assert(!serif_cands.empty());
    assert(!mono_cands.empty());
    assert(!sysui_cands.empty());
    assert(!emoji_cands.empty());

    // Verify metric-compatible alias resolution (Arial -> Liberation Sans / Arimo / Noto Sans)
    auto arial_cands = cfg.resolve_family_candidates(Family("Arial"));
    assert(arial_cands.size() >= 2);
    assert(arial_cands[0] == "Arial");

    // Verify custom XML parsing with <match>, <alias>, and language override
    std::string custom_xml = R"(
        <?xml version="1.0"?>
        <fontconfig>
            <alias>
                <family>VaxpCustomUI</family>
                <prefer>
                    <family>Inter</family>
                    <family>Noto Sans</family>
                </prefer>
            </alias>
            <match target="pattern">
                <test name="lang" compare="contains">
                    <string>ar</string>
                </test>
                <test name="family">
                    <string>sans-serif</string>
                </test>
                <edit name="family" mode="prepend">
                    <string>Noto Sans Arabic</string>
                </edit>
            </match>
            <selectfont>
                <rejectfont>
                    <glob>*.dpkg-tmp</glob>
                </rejectfont>
            </selectfont>
        </fontconfig>
    )";
    cfg.set_locale("ar-IQ");
    assert(cfg.parse_xml_content(custom_xml));
    assert(cfg.is_rejected_file("/usr/share/fonts/foo.ttf.dpkg-tmp"));
    assert(!cfg.is_rejected_file("/usr/share/fonts/NotoSans-Regular.ttf"));

    auto vaxp_cands = cfg.resolve_family_name("VaxpCustomUI");
    assert(vaxp_cands.size() >= 3);
    assert(vaxp_cands[1] == "Inter");

    auto ar_sans = cfg.resolve_family_candidates(Family::sans_serif());
    assert(!ar_sans.empty());
    assert(ar_sans[0] == "Noto Sans Arabic");

    std::cout << "  -> PASS" << std::endl;
}

void test_mmap_and_header_probe() {
    std::cout << "[Test 2] POSIX mmap Zero-Copy & Fast Header Probing (OS/2 Unicode Ranges)..." << std::endl;

    auto mapped_inter = MappedFontData::open_file(resolve_font("Inter-Regular.ttf"));
    auto mapped_arabic = MappedFontData::open_file(resolve_font("NotoSansArabic.ttf"));
    assert(mapped_inter && mapped_inter->is_valid());
    assert(mapped_arabic && mapped_arabic->is_valid());

    FontFaceHeaderInfo info_inter, info_arabic;
    assert(TtfFont::probe_face_header(mapped_inter->span(), 0, info_inter));
    assert(TtfFont::probe_face_header(mapped_arabic->span(), 0, info_arabic));

    std::cout << "  Probed Inter: family='" << info_inter.family_name
              << "', style='" << info_inter.style_name
              << "', weight=" << static_cast<int>(info_inter.weight) << std::endl;
    std::cout << "  Probed Arabic: family='" << info_arabic.family_name
              << "', style='" << info_arabic.style_name
              << "', weight=" << static_cast<int>(info_arabic.weight) << std::endl;

    // Verify OS/2 Unicode Range bit detection
    uint8_t latin_bit = FontSystem::codepoint_to_os2_range_bit(U'A');
    uint8_t arabic_bit = FontSystem::codepoint_to_os2_range_bit(0x0628); // Arabic Ba 'ب'
    assert(latin_bit == 0);
    assert(arabic_bit == 13);

    // Check that NotoSansArabic sets Arabic bit (13) in its OS/2 ulUnicodeRange
    bool arabic_supports_bit13 = (info_arabic.unicode_ranges[arabic_bit >> 5] & (1u << (arabic_bit & 31))) != 0;
    assert(arabic_supports_bit13);

    std::cout << "  -> PASS" << std::endl;
}

void test_ttc_font_collections() {
    std::cout << "[Test 3] TrueType/OpenType Collection (.ttc / .otc) Multi-Face Parsing..." << std::endl;

    auto mapped0 = MappedFontData::open_file(resolve_font("Inter-Regular.ttf"));
    auto mapped1 = MappedFontData::open_file(resolve_font("NotoSansArabic.ttf"));
    assert(mapped0 && mapped1);

    std::vector<uint8_t> ttc_bytes = build_synthetic_ttc(mapped0->span(), mapped1->span());
    uint32_t face_count = TtfFont::collection_face_count(ttc_bytes);
    assert(face_count == 2);

    auto f0 = TtfFont::from_bytes(ttc_bytes, false, 0);
    auto f1 = TtfFont::from_bytes(ttc_bytes, false, 1);
    assert(f0 && f0->is_valid());
    assert(f1 && f1->is_valid());

    assert(f0->family_name() != f1->family_name());
    assert(f0->glyph_index(U'A') != 0);
    assert(f1->glyph_index(0x0628) != 0); // 'ب'

    // Load both into FontSystem via load_font_data with face_index
    FontSystem fs;
    auto id0 = fs.load_font_data(ttc_bytes, 0);
    auto id1 = fs.load_font_data(ttc_bytes, 1);
    assert(id0.has_value() && id1.has_value());
    assert(fs.font_count() == 2);

    auto [fb_id, fb_gid] = fs.find_glyph_or_fallback(*id0, 0x0628);
    assert(fb_id == *id1);
    assert(fb_gid != 0);

    std::cout << "  Successfully parsed 2 distinct faces from TTC container: '"
              << f0->family_name() << "' and '" << f1->family_name() << "'." << std::endl;
    std::cout << "  -> PASS" << std::endl;
}

void test_system_font_discovery_and_lazy_loading() {
    std::cout << "[Test 4] Linux System Font Discovery, Lazy Loading & Binary Index Cache..." << std::endl;

    auto t0 = std::chrono::high_resolution_clock::now();
    FontSystem fs;
    size_t indexed_count = fs.scan_system_fonts(false); // Fresh scan
    auto t1 = std::chrono::high_resolution_clock::now();
    double scan_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Also scan local project fonts/ directory so test is guaranteed to have Arabic & CJK even in minimal containers
    fs.scan_font_directory("fonts", true);
    fs.scan_font_directory("../fonts", true);

    assert(fs.font_count() > 0);
    auto families = fs.list_families();
    assert(!families.empty());

    // Verify lazy loading: only the default font (at most 1 face) should be instantiated in memory right after scan!
    size_t loaded_initially = fs.loaded_font_count();
    std::cout << "  Indexed " << fs.font_count() << " total font faces across "
              << families.size() << " families in " << scan_ms << " ms." << std::endl;
    std::cout << "  Instantiated in RAM after scan: " << loaded_initially << " / " << fs.font_count()
              << " faces (Lazy Loading verified!)." << std::endl;
    assert(loaded_initially <= 2);

    // Test binary cache reload speed
    auto t2 = std::chrono::high_resolution_clock::now();
    FontSystem cached_fs = FontSystem::with_system_fonts(true);
    auto t3 = std::chrono::high_resolution_clock::now();
    double cached_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::cout << "  Binary Index Cache reload time: " << cached_ms << " ms ("
              << cached_fs.font_count() << " system faces)." << std::endl;
    if (indexed_count > 0) {
        assert(cached_fs.font_count() == indexed_count);
    }

    // Verify generic family matching works automatically without manual load_font_file!
    auto [sans_id, sans_font] = fs.match_font(Family::sans_serif());
    auto [serif_id, serif_font] = fs.match_font(Family::serif());
    auto [mono_id, mono_font] = fs.match_font(Family::monospace());
    auto [sys_id, sys_font] = fs.match_font(Family::system_ui());

    assert(sans_font != nullptr);
    assert(serif_font != nullptr);
    assert(mono_font != nullptr);
    assert(sys_font != nullptr);

    std::cout << "  Matched System Generics:" << std::endl;
    std::cout << "    - sans-serif -> " << sans_font->family_name() << " (" << sans_font->style_name() << ")" << std::endl;
    std::cout << "    - serif      -> " << serif_font->family_name() << " (" << serif_font->style_name() << ")" << std::endl;
    std::cout << "    - monospace  -> " << mono_font->family_name() << " (" << mono_font->style_name() << ")" << std::endl;
    std::cout << "    - system-ui  -> " << sys_font->family_name() << " (" << sys_font->style_name() << ")" << std::endl;

    // Test automatic multilingual rendering (English + Arabic + CJK) using discovered fonts
    auto pixmap = Pixmap::allocate(480, 160);
    assert(pixmap.has_value());
    Canvas canvas(*pixmap);
    canvas.clear(Color::BLACK);

    GlyphCache cache;
    Buffer buffer(Metrics(18.0f, 24.0f));
    buffer.set_text(
        "System Sans-Serif: Linux Font Integration\n"
        "العربية: تكامل عميق مع خطوط النظام\n"
        "日本語 / 中文: システムフォント統合"
    );
    buffer.draw(canvas, cache, fs, Color::WHITE, 16.0f, 16.0f);

    assert(cache.size() >= 10);
    std::cout << "  Rendered multilingual paragraph using lazy-loaded system fonts (cached glyphs: "
              << cache.size() << ", loaded faces in RAM: " << fs.loaded_font_count() << " / " << fs.font_count() << ")." << std::endl;
    std::cout << "  -> PASS" << std::endl;
}

void test_zero_config_auto_vs_explicit_custom_fonts() {
    std::cout << "[Test 5] Zero-Config Automatic System Fonts vs. Explicit Custom Fonts..." << std::endl;

    // Case A: Default-constructed FontSystem with NO custom fonts added -> automatically loads system fonts on demand!
    {
        FontSystem auto_fs;
        assert(!auto_fs.has_custom_fonts());

        auto pixmap = Pixmap::allocate(320, 80);
        assert(pixmap.has_value());
        Canvas canvas(*pixmap);
        canvas.clear(Color::BLACK);

        GlyphCache cache;
        Buffer buf(Metrics(16.0f, 22.0f));
        buf.set_text("Zero-Config System Font Rendering!");
        buf.draw(canvas, cache, auto_fs, Color::WHITE, 10.0f, 10.0f);

        assert(auto_fs.font_count() > 0);
        assert(cache.size() > 5);
        std::cout << "  Case A (Zero-Config): Automatically discovered " << auto_fs.font_count()
                  << " system faces and rendered text with 0 manual setup calls." << std::endl;
    }

    // Case B: Developer explicitly loads a custom font -> system fonts are NOT auto-loaded unless requested!
    {
        FontSystem custom_fs;
        auto id = custom_fs.load_font_file(resolve_font("Inter-Regular.ttf"));
        assert(id.has_value());
        assert(custom_fs.has_custom_fonts());
        assert(custom_fs.font_count() == 1);

        auto [matched_id, matched_font] = custom_fs.match_font(Family::sans_serif());
        assert(matched_id == *id);
        assert(matched_font != nullptr);
        assert(custom_fs.font_count() == 1); // Still strictly 1 custom font!
        std::cout << "  Case B (Explicit Custom Font): Strictly isolated to the 1 developer-provided font ('"
                  << matched_font->family_name() << "')." << std::endl;
    }

    std::cout << "  -> PASS" << std::endl;
}

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << "  Nisaba Sovereign Linux Font System Integration Suite" << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_linux_fontconfig_xml_parser();
    test_mmap_and_header_probe();
    test_ttc_font_collections();
    test_system_font_discovery_and_lazy_loading();
    test_zero_config_auto_vs_explicit_custom_fonts();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  All Linux Font System tests passed!" << std::endl;
    std::cout << "==========================================================" << std::endl;
    return 0;
}
