#include "nisaba/text/font_system.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace nisaba::text {

namespace {

bool str_case_contains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return std::tolower(static_cast<unsigned char>(ch1)) ==
                   std::tolower(static_cast<unsigned char>(ch2));
        }
    );
    return it != haystack.end();
}

bool is_supported_font_extension(std::string_view ext) noexcept {
    if (ext.size() != 4 || ext[0] != '.') return false;
    char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1])));
    char c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[2])));
    char c3 = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[3])));
    return (c1 == 't' && c2 == 't' && c3 == 'f') ||
           (c1 == 'o' && c2 == 't' && c3 == 'f') ||
           (c1 == 't' && c2 == 't' && c3 == 'c') ||
           (c1 == 'o' && c2 == 't' && c3 == 'c');
}

uint64_t compute_dirs_fingerprint(const std::vector<std::string>& dirs) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };

    for (const auto& d : dirs) {
        for (unsigned char c : d) {
            mix(c);
        }
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
        struct stat st{};
        if (::stat(d.c_str(), &st) == 0) {
            mix(static_cast<uint64_t>(st.st_mtime));
            mix(static_cast<uint64_t>(st.st_size));
        }
#endif
    }
    return hash;
}

std::string get_disk_cache_path() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            base = std::string(home) + "/.cache";
        }
    }
    if (base.empty()) return {};
    return base + "/nisaba/font_index_v1.bin";
}

constexpr uint32_t CACHE_MAGIC = 0x4E534631u; // 'NSF1'

} // namespace

FontSystem::FontSystem() {
    linux_config_.set_locale("");
}

FontSystem::~FontSystem() = default;

FontSystem::FontSystem(FontSystem&&) noexcept = default;
FontSystem& FontSystem::operator=(FontSystem&&) noexcept = default;

FontSystem FontSystem::with_system_fonts(bool use_disk_cache) {
    FontSystem fs;
    fs.scan_system_fonts(use_disk_cache);
    return fs;
}

void FontSystem::set_locale(std::string_view bcp47_locale) {
    linux_config_.set_locale(bcp47_locale);
    std::fill(std::begin(fallback_page_cache_), std::end(fallback_page_cache_), FallbackPageSlot{});
}

uint8_t FontSystem::codepoint_to_os2_range_bit(char32_t cp) noexcept {
    if (cp <= 0x007F) return 0;  // Basic Latin
    if (cp <= 0x00FF) return 1;  // Latin-1 Supplement
    if (cp <= 0x017F) return 2;  // Latin Extended-A
    if (cp <= 0x024F) return 3;  // Latin Extended-B
    if (cp <= 0x02AF) return 4;  // IPA Extensions
    if (cp <= 0x02FF) return 5;  // Spacing Modifier Letters
    if (cp <= 0x036F) return 6;  // Combining Diacritical Marks
    if (cp <= 0x03FF) return 7;  // Greek and Coptic
    if (cp >= 0x0400 && cp <= 0x052F) return 9;  // Cyrillic
    if (cp >= 0x0530 && cp <= 0x058F) return 10; // Armenian
    if (cp >= 0x0590 && cp <= 0x05FF) return 11; // Hebrew
    if (cp >= 0x0600 && cp <= 0x06FF) return 13; // Arabic
    if (cp >= 0x0750 && cp <= 0x077F) return 13; // Arabic Supplement
    if (cp >= 0x08A0 && cp <= 0x08FF) return 13; // Arabic Extended-A
    if (cp >= 0x0700 && cp <= 0x074F) return 71; // Syriac
    if (cp >= 0x0900 && cp <= 0x097F) return 15; // Devanagari
    if (cp >= 0x0980 && cp <= 0x09FF) return 16; // Bengali
    if (cp >= 0x0A00 && cp <= 0x0A7F) return 17; // Gurmukhi
    if (cp >= 0x0A80 && cp <= 0x0AFF) return 18; // Gujarati
    if (cp >= 0x0B80 && cp <= 0x0BFF) return 20; // Tamil
    if (cp >= 0x0E00 && cp <= 0x0E7F) return 24; // Thai
    if (cp >= 0x10A0 && cp <= 0x10FF) return 26; // Georgian
    if (cp >= 0x1100 && cp <= 0x11FF) return 28; // Hangul Jamo
    if (cp >= 0x1E00 && cp <= 0x1EFF) return 29; // Latin Extended Additional
    if (cp >= 0x1F00 && cp <= 0x1FFF) return 30; // Greek Extended
    if (cp >= 0x2000 && cp <= 0x206F) return 31; // General Punctuation
    if (cp >= 0x2070 && cp <= 0x209F) return 32; // Superscripts And Subscripts
    if (cp >= 0x20A0 && cp <= 0x20CF) return 33; // Currency Symbols
    if (cp >= 0x2100 && cp <= 0x214F) return 35; // Letterlike Symbols
    if (cp >= 0x2150 && cp <= 0x218F) return 36; // Number Forms
    if (cp >= 0x2190 && cp <= 0x21FF) return 37; // Arrows
    if (cp >= 0x2200 && cp <= 0x22FF) return 38; // Mathematical Operators
    if (cp >= 0x2300 && cp <= 0x23FF) return 39; // Miscellaneous Technical
    if (cp >= 0x2500 && cp <= 0x257F) return 42; // Box Drawing
    if (cp >= 0x2580 && cp <= 0x259F) return 43; // Block Elements
    if (cp >= 0x25A0 && cp <= 0x25FF) return 44; // Geometric Shapes
    if (cp >= 0x2600 && cp <= 0x26FF) return 45; // Miscellaneous Symbols
    if (cp >= 0x2700 && cp <= 0x27BF) return 46; // Dingbats
    if (cp >= 0x3000 && cp <= 0x303F) return 48; // CJK Symbols And Punctuation
    if (cp >= 0x3040 && cp <= 0x309F) return 49; // Hiragana
    if (cp >= 0x30A0 && cp <= 0x30FF) return 50; // Katakana
    if (cp >= 0x3100 && cp <= 0x312F) return 51; // Bopomofo
    if (cp >= 0x3130 && cp <= 0x318F) return 52; // Hangul Compatibility Jamo
    if (cp >= 0x3400 && cp <= 0x9FFF) return 59; // CJK Unified Ideographs
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 56; // Hangul Syllables
    if (cp >= 0xE000 && cp <= 0xF8FF) return 60; // Private Use Area (Nerd Fonts / Icons)
    if (cp >= 0xF900 && cp <= 0xFAFF) return 61; // CJK Compatibility Ideographs
    if (cp >= 0xFB50 && cp <= 0xFDFF) return 63; // Arabic Presentation Forms-A
    if (cp >= 0xFE70 && cp <= 0xFEFF) return 67; // Arabic Presentation Forms-B
    if (cp >= 0xFF00 && cp <= 0xFFEF) return 68; // Halfwidth And Fullwidth Forms
    if (cp >= 0x10000) return 57;                // Non-Plane 0 (SMP / Emoji / Historic / CJK Ext B+)
    return 0xFF;
}

uint32_t FontSystem::register_probed_face(std::string_view file_path, const FontFaceHeaderInfo& info) {
    uint32_t id = next_id_++;
    if (!default_font_id_.has_value()) {
        default_font_id_ = id;
    }

    FaceRecord rec;
    rec.id = id;
    rec.family_name = info.family_name;
    rec.family_key = LinuxFontConfig::normalize_key(info.family_name);
    rec.style_name = info.style_name;
    rec.postscript_name = info.postscript_name;
    rec.file_path = std::string(file_path);
    rec.face_index = info.face_index;
    rec.weight = info.weight;
    rec.style = info.style;
    rec.stretch = info.stretch;
    rec.is_monospace = info.is_monospace;
    rec.is_serif = info.is_serif;
    rec.is_color = info.is_color;
    rec.is_variable = info.is_variable;
    rec.unicode_ranges[0] = info.unicode_ranges[0];
    rec.unicode_ranges[1] = info.unicode_ranges[1];
    rec.unicode_ranges[2] = info.unicode_ranges[2];
    rec.unicode_ranges[3] = info.unicode_ranges[3];

    size_t idx = faces_.size();
    faces_.push_back(std::move(rec));
    family_to_faces_[faces_[idx].family_key].push_back(idx);
    return id;
}

void FontSystem::rebuild_lookup_tables() {
    family_to_faces_.clear();
    for (size_t i = 0; i < faces_.size(); ++i) {
        family_to_faces_[faces_[i].family_key].push_back(i);
    }
    std::fill(std::begin(fallback_page_cache_), std::end(fallback_page_cache_), FallbackPageSlot{});
}

bool FontSystem::try_load_disk_cache(const std::vector<std::string>& dirs) {
    std::string cache_path = get_disk_cache_path();
    if (cache_path.empty()) return false;

    std::ifstream file(cache_path, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic = 0;
    uint64_t saved_fp = 0;
    uint32_t count = 0;

    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != CACHE_MAGIC) return false;
    if (!file.read(reinterpret_cast<char*>(&saved_fp), sizeof(saved_fp))) return false;
    if (saved_fp != compute_dirs_fingerprint(dirs)) return false;
    if (!file.read(reinterpret_cast<char*>(&count), sizeof(count)) || count == 0 || count > 32768) return false;

    auto read_str = [&](std::string& out) -> bool {
        uint16_t len = 0;
        if (!file.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
        if (len > 4096) return false;
        out.resize(len);
        if (len > 0 && !file.read(out.data(), len)) return false;
        return true;
    };

    std::vector<std::pair<std::string, FontFaceHeaderInfo>> loaded_entries;
    loaded_entries.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        std::string fpath;
        FontFaceHeaderInfo info;
        if (!read_str(fpath)) return false;
        if (!read_str(info.family_name)) return false;
        if (!read_str(info.style_name)) return false;
        if (!read_str(info.postscript_name)) return false;
        if (!file.read(reinterpret_cast<char*>(&info.face_index), sizeof(info.face_index))) return false;

        uint16_t w = 400;
        uint8_t st = 0, str = 5, flags = 0;
        if (!file.read(reinterpret_cast<char*>(&w), sizeof(w))) return false;
        if (!file.read(reinterpret_cast<char*>(&st), sizeof(st))) return false;
        if (!file.read(reinterpret_cast<char*>(&str), sizeof(str))) return false;
        if (!file.read(reinterpret_cast<char*>(&flags), sizeof(flags))) return false;
        if (!file.read(reinterpret_cast<char*>(info.unicode_ranges), sizeof(info.unicode_ranges))) return false;

        info.weight = static_cast<Weight>(w);
        info.style = static_cast<Style>(st);
        info.stretch = static_cast<Stretch>(str);
        info.is_monospace = (flags & 1) != 0;
        info.is_serif = (flags & 2) != 0;
        info.is_color = (flags & 4) != 0;
        info.is_variable = (flags & 8) != 0;

        loaded_entries.emplace_back(std::move(fpath), std::move(info));
    }

    for (const auto& [fpath, info] : loaded_entries) {
        register_probed_face(fpath, info);
    }
    return true;
}

void FontSystem::save_disk_cache(const std::vector<std::string>& dirs) const {
    std::string cache_path = get_disk_cache_path();
    if (cache_path.empty() || faces_.empty()) return;

    std::error_code ec;
    fs::path cp(cache_path);
    fs::create_directories(cp.parent_path(), ec);

    std::ofstream file(cache_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;

    // Count disk-backed faces
    uint32_t disk_faces = 0;
    for (const auto& rec : faces_) {
        if (!rec.file_path.empty()) ++disk_faces;
    }
    if (disk_faces == 0) return;

    uint32_t magic = CACHE_MAGIC;
    uint64_t fp = compute_dirs_fingerprint(dirs);

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&fp), sizeof(fp));
    file.write(reinterpret_cast<const char*>(&disk_faces), sizeof(disk_faces));

    auto write_str = [&](std::string_view s) {
        uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 4096));
        file.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) file.write(s.data(), len);
    };

    for (const auto& rec : faces_) {
        if (rec.file_path.empty()) continue;
        write_str(rec.file_path);
        write_str(rec.family_name);
        write_str(rec.style_name);
        write_str(rec.postscript_name);
        file.write(reinterpret_cast<const char*>(&rec.face_index), sizeof(rec.face_index));

        uint16_t w = static_cast<uint16_t>(rec.weight);
        uint8_t st = static_cast<uint8_t>(rec.style);
        uint8_t str = static_cast<uint8_t>(rec.stretch);
        uint8_t flags = (rec.is_monospace ? 1 : 0) |
                        (rec.is_serif ? 2 : 0) |
                        (rec.is_color ? 4 : 0) |
                        (rec.is_variable ? 8 : 0);

        file.write(reinterpret_cast<const char*>(&w), sizeof(w));
        file.write(reinterpret_cast<const char*>(&st), sizeof(st));
        file.write(reinterpret_cast<const char*>(&str), sizeof(str));
        file.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
        file.write(reinterpret_cast<const char*>(rec.unicode_ranges), sizeof(rec.unicode_ranges));
    }
}

size_t FontSystem::scan_system_fonts(bool use_disk_cache) {
    auto_system_scan_attempted_ = true;
    if (!system_config_loaded_) {
        linux_config_.load_system_config(linux_config_.locale());
        system_config_loaded_ = true;
    }

    const auto& dirs = linux_config_.font_directories();
    if (dirs.empty()) return 0;

    size_t before_count = faces_.size();

    auto select_lazy_default_id = [&]() {
        auto candidates = linux_config_.resolve_family_candidates(Family::sans_serif());
        for (const auto& cand_name : candidates) {
            std::string key = LinuxFontConfig::normalize_key(cand_name);
            auto it = family_to_faces_.find(key);
            if (it != family_to_faces_.end() && !it->second.empty()) {
                if (const FaceRecord* matched = score_faces_in_family(it->second, Weight::Normal, Style::Normal, Stretch::Normal)) {
                    default_font_id_ = matched->id;
                    return;
                }
            }
        }
    };

    if (use_disk_cache && faces_.empty() && try_load_disk_cache(dirs)) {
        select_lazy_default_id();
        return faces_.size() - before_count;
    }

    for (const auto& dir : dirs) {
        scan_font_directory_internal(dir, true);
    }

    if (faces_.size() > before_count) {
        save_disk_cache(dirs);
    }

    select_lazy_default_id();

    return faces_.size() - before_count;
}

void FontSystem::ensure_default_system_fonts() const noexcept {
    if (!auto_system_fonts_enabled_ || has_custom_fonts_ || auto_system_scan_attempted_ || !faces_.empty()) [[likely]] {
        return;
    }
    auto_system_scan_attempted_ = true;
    try {
        const_cast<FontSystem*>(this)->scan_system_fonts(true);
    } catch (...) {
    }
}

std::optional<uint32_t> FontSystem::default_font_id() const noexcept {
    if (!default_font_id_.has_value()) {
        ensure_default_system_fonts();
    }
    return default_font_id_;
}

size_t FontSystem::scan_font_directory(std::string_view dir_path, bool recursive) {
    size_t added = scan_font_directory_internal(dir_path, recursive);
    if (added > 0) {
        has_custom_fonts_ = true;
    }
    return added;
}

size_t FontSystem::scan_font_directory_internal(std::string_view dir_path, bool recursive) {
    if (dir_path.empty()) return 0;
    std::error_code ec;
    fs::path root(dir_path);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return 0;

    // Track already registered file paths to avoid duplicates when directories overlap via symlinks
    std::unordered_set<std::string> existing_paths;
    existing_paths.reserve(faces_.size());
    for (const auto& f : faces_) {
        if (!f.file_path.empty()) existing_paths.insert(f.file_path);
    }

    std::vector<std::string> font_files;
    auto collect_entry = [&](const fs::directory_entry& entry) {
        std::error_code e;
        if (!entry.is_regular_file(e)) return;
        std::string path_str = entry.path().string();
        if (linux_config_.is_rejected_file(path_str)) return;
        std::string ext = entry.path().extension().string();
        if (!is_supported_font_extension(ext)) return;
        if (!existing_paths.contains(path_str)) {
            font_files.push_back(std::move(path_str));
        }
    };

    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            collect_entry(*it);
        }
    } else {
        for (auto it = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            collect_entry(*it);
        }
    }

    std::sort(font_files.begin(), font_files.end());

    size_t added = 0;
    for (const auto& fpath : font_files) {
        auto mapped = MappedFontData::open_file(fpath);
        if (!mapped || !mapped->is_valid()) continue;

        uint32_t num_faces = TtfFont::collection_face_count(mapped->span());
        // Cap collection face indexing at 32 per file to keep startup ultra-fast
        uint32_t max_faces = std::min<uint32_t>(num_faces, 32);
        for (uint32_t f_idx = 0; f_idx < max_faces; ++f_idx) {
            FontFaceHeaderInfo info;
            if (TtfFont::probe_face_header(mapped->span(), f_idx, info)) {
                register_probed_face(fpath, info);
                ++added;
            }
        }
    }

    return added;
}

std::optional<uint32_t> FontSystem::load_font_file(std::string_view path, uint32_t face_index) {
    auto mapped = MappedFontData::open_file(path);
    if (!mapped || !mapped->is_valid()) {
        return std::nullopt;
    }

    FontFaceHeaderInfo info;
    bool probed = TtfFont::probe_face_header(mapped->span(), face_index, info);

    auto font = TtfFont::from_mapped(mapped, face_index);
    if (!font || !font->is_valid()) {
        return std::nullopt;
    }

    if (!probed) {
        info.face_index = face_index;
        info.family_name = std::string(font->family_name());
        info.style_name = std::string(font->style_name());
        info.postscript_name = std::string(font->postscript_name());
        info.weight = font->weight();
        info.style = font->style();
        info.stretch = font->stretch();
        info.is_monospace = font->is_monospace();
        info.is_serif = font->is_serif();
        info.is_color = font->has_color_glyphs();
        info.is_variable = !font->variation_axes().empty();
        auto ur = font->unicode_ranges();
        for (int k = 0; k < 4; ++k) info.unicode_ranges[k] = ur[k];
    }

    std::string path_str(path);
    mmap_pool_[path_str] = mapped;

    has_custom_fonts_ = true;
    uint32_t id = register_probed_face(path_str, info);
    faces_.back().font = std::move(font);
    return id;
}

std::vector<uint32_t> FontSystem::load_font_collection(std::string_view path) {
    std::vector<uint32_t> ids;
    auto mapped = MappedFontData::open_file(path);
    if (!mapped || !mapped->is_valid()) return ids;

    uint32_t count = TtfFont::collection_face_count(mapped->span());
    if (count == 0) return ids;

    std::string path_str(path);
    mmap_pool_[path_str] = mapped;

    for (uint32_t i = 0; i < count; ++i) {
        FontFaceHeaderInfo info;
        if (TtfFont::probe_face_header(mapped->span(), i, info)) {
            has_custom_fonts_ = true;
            uint32_t id = register_probed_face(path_str, info);
            ids.push_back(id);
        }
    }
    return ids;
}

std::optional<uint32_t> FontSystem::load_font_data(std::span<const uint8_t> data, uint32_t face_index) {
    FontFaceHeaderInfo info;
    bool probed = TtfFont::probe_face_header(data, face_index, info);

    auto font = TtfFont::from_bytes(data, true, face_index);
    if (!font || !font->is_valid()) {
        return std::nullopt;
    }

    if (!probed) {
        info.face_index = face_index;
        info.family_name = std::string(font->family_name());
        info.style_name = std::string(font->style_name());
        info.postscript_name = std::string(font->postscript_name());
        info.weight = font->weight();
        info.style = font->style();
        info.stretch = font->stretch();
        info.is_monospace = font->is_monospace();
        info.is_serif = font->is_serif();
        info.is_color = font->has_color_glyphs();
        info.is_variable = !font->variation_axes().empty();
        auto ur = font->unicode_ranges();
        for (int k = 0; k < 4; ++k) info.unicode_ranges[k] = ur[k];
    }

    has_custom_fonts_ = true;
    uint32_t id = register_probed_face("", info);
    faces_.back().font = std::move(font);
    return id;
}

const FontSystem::FaceRecord* FontSystem::find_face_record(uint32_t font_id) const noexcept {
    if (font_id == 0 || faces_.empty()) return nullptr;
    // Fast O(1) index check since IDs are assigned monotonically starting at 1
    if (font_id <= faces_.size() && faces_[font_id - 1].id == font_id) [[likely]] {
        return &faces_[font_id - 1];
    }
    for (const auto& rec : faces_) {
        if (rec.id == font_id) return &rec;
    }
    return nullptr;
}

const TtfFont* FontSystem::ensure_face_loaded(const FaceRecord& rec) const noexcept {
    if (rec.font) [[likely]] {
        return rec.font.get();
    }
    if (rec.load_failed || rec.file_path.empty()) {
        return nullptr;
    }

    std::shared_ptr<MappedFontData> mapped;
    auto it = mmap_pool_.find(rec.file_path);
    if (it != mmap_pool_.end()) {
        mapped = it->second.lock();
    }
    if (!mapped) {
        mapped = MappedFontData::open_file(rec.file_path);
        if (mapped) {
            mmap_pool_[rec.file_path] = mapped;
        }
    }

    if (!mapped) {
        rec.load_failed = true;
        return nullptr;
    }

    rec.font = TtfFont::from_mapped(std::move(mapped), rec.face_index);
    if (!rec.font || !rec.font->is_valid()) {
        rec.font.reset();
        rec.load_failed = true;
        return nullptr;
    }
    return rec.font.get();
}

const TtfFont* FontSystem::get_font(uint32_t font_id) const noexcept {
    const FaceRecord* rec = find_face_record(font_id);
    if (!rec) return nullptr;
    return ensure_face_loaded(*rec);
}

const FontSystem::FaceRecord* FontSystem::score_faces_in_family(
    const std::vector<size_t>& face_indices,
    Weight target_weight,
    Style target_style,
    Stretch target_stretch
) const noexcept {
    const FaceRecord* best = nullptr;
    int best_penalty = 0x7FFFFFFF;

    int req_w = static_cast<int>(target_weight);
    int req_str = static_cast<int>(target_stretch);

    for (size_t idx : face_indices) {
        const FaceRecord& rec = faces_[idx];
        if (rec.load_failed) continue;

        int penalty = 0;

        // 1. Stretch distance (CSS Font Matching Step 1)
        int str_diff = std::abs(static_cast<int>(rec.stretch) - req_str);
        penalty += str_diff * 500;

        // 2. Style distance (CSS Font Matching Step 2)
        if (rec.style != target_style) {
            if (target_style == Style::Italic && rec.style == Style::Oblique) {
                penalty += 100;
            } else if (target_style == Style::Oblique && rec.style == Style::Italic) {
                penalty += 100;
            } else if (target_style != Style::Normal && rec.style == Style::Normal) {
                // Can synthesize italic from Normal
                penalty += 300;
            } else {
                // Requested Normal, but face is Italic/Oblique (avoid unless no Normal exists)
                penalty += 2000;
            }
        }

        // 3. Weight distance (CSS Font Matching Step 3)
        int rec_w = static_cast<int>(rec.weight);
        int w_diff = std::abs(rec_w - req_w);
        if (w_diff > 0) {
            penalty += w_diff;
            // CSS preference: if req_w >= 600, prefer heavier weights first; if req_w <= 400, prefer lighter or 400/500
            if (req_w >= 600 && rec_w < 400) {
                penalty += 250;
            } else if (req_w <= 400 && rec_w >= 600) {
                penalty += 400;
            }
        }

        if (penalty < best_penalty) {
            best_penalty = penalty;
            best = &rec;
        }
    }

    return best;
}

FontMatchResult FontSystem::match_font_styled(
    const Family& family,
    Weight weight,
    Style style,
    Stretch stretch
) const noexcept {
    ensure_default_system_fonts();
    if (faces_.empty()) {
        return {};
    }

    auto make_result = [&](const FaceRecord& rec) -> FontMatchResult {
        const TtfFont* f = ensure_face_loaded(rec);
        bool synth_bold = (static_cast<int>(weight) >= 600) && (static_cast<int>(rec.weight) < 600);
        bool synth_italic = (style != Style::Normal) && (rec.style == Style::Normal);
        return FontMatchResult{rec.id, f, synth_bold, synth_italic};
    };

    if (faces_.size() == 1) {
        return make_result(faces_[0]);
    }

    // 1. Resolve ordered candidate family list via LinuxFontConfig rules (handles concrete names, metric aliases & generics)
    auto candidates = linux_config_.resolve_family_candidates(family);
    for (const auto& cand_name : candidates) {
        std::string key = LinuxFontConfig::normalize_key(cand_name);
        auto it = family_to_faces_.find(key);
        if (it != family_to_faces_.end() && !it->second.empty()) {
            if (const FaceRecord* matched = score_faces_in_family(it->second, weight, style, stretch)) {
                if (ensure_face_loaded(*matched)) {
                    return make_result(*matched);
                }
            }
        }
    }

    // 2. Heuristic substring / PANOSE / fixed-pitch matching across loaded/indexed faces
    const FaceRecord* best_match = nullptr;
    int best_score = -100000;

    for (const auto& rec : faces_) {
        if (rec.load_failed) continue;
        int score = 0;

        if (family.kind == Family::Kind::Name) {
            if (str_case_contains(rec.family_name, family.name)) {
                score += 1000;
            } else {
                score -= 500;
            }
        } else if (family.kind == Family::Kind::Monospace) {
            if (rec.is_monospace ||
                str_case_contains(rec.family_name, "Mono") ||
                str_case_contains(rec.family_name, "Code") ||
                str_case_contains(rec.family_name, "Console") ||
                str_case_contains(rec.family_name, "Courier")) {
                score += 600;
            }
        } else if (family.kind == Family::Kind::Serif) {
            if ((rec.is_serif || str_case_contains(rec.family_name, "Serif")) &&
                !str_case_contains(rec.family_name, "Sans")) {
                score += 600;
            }
        } else if (family.kind == Family::Kind::Emoji) {
            if (rec.is_color || str_case_contains(rec.family_name, "Emoji")) {
                score += 800;
            }
        } else if (family.kind == Family::Kind::SansSerif || family.kind == Family::Kind::SystemUI) {
            if (!rec.is_monospace && !rec.is_serif && !rec.is_color) {
                score += 300;
            }
            if (str_case_contains(rec.family_name, "Sans") ||
                str_case_contains(rec.family_name, "Inter")) {
                score += 250;
            }
        }

        int w_diff = std::abs(static_cast<int>(rec.weight) - static_cast<int>(weight));
        score -= w_diff / 5;

        if (rec.style == style) {
            score += 80;
        } else if (style == Style::Normal && rec.style != Style::Normal) {
            score -= 150;
        }

        if (default_font_id_.has_value() && rec.id == *default_font_id_) {
            score += 50;
        }

        if (score > best_score) {
            best_score = score;
            best_match = &rec;
        }
    }

    if (best_match && ensure_face_loaded(*best_match)) {
        return make_result(*best_match);
    }

    return make_result(faces_[0]);
}

std::pair<uint32_t, const TtfFont*> FontSystem::match_font(
    const Family& family,
    Weight weight,
    Style style,
    Stretch stretch
) const noexcept {
    auto res = match_font_styled(family, weight, style, stretch);
    return {res.font_id, res.font};
}

std::pair<uint32_t, uint16_t> FontSystem::find_glyph_or_fallback(
    uint32_t primary_font_id,
    char32_t codepoint
) const noexcept {
    ensure_default_system_fonts();
    const FaceRecord* primary_rec = find_face_record(primary_font_id);
    if (primary_rec) {
        if (const TtfFont* primary = ensure_face_loaded(*primary_rec)) {
            uint16_t gid = primary->glyph_index(codepoint);
            if (gid != 0) {
                return {primary_font_id, gid};
            }
        }
    }

    if (faces_.size() <= 1) {
        return {primary_font_id, 0};
    }

    // 1. Check O(1) Codepoint Page Fallback Cache (256-codepoint page granularity)
    uint32_t page = static_cast<uint32_t>(codepoint) >> 8;
    size_t cache_idx = ((primary_font_id * 31u) ^ page) & (FALLBACK_CACHE_SIZE - 1);
    const auto& cached_slot = fallback_page_cache_[cache_idx];
    if (cached_slot.primary_id == primary_font_id &&
        cached_slot.page == page &&
        cached_slot.matched_font_id != 0 &&
        cached_slot.matched_font_id != primary_font_id) {
        if (const FaceRecord* cached_rec = find_face_record(cached_slot.matched_font_id)) {
            if (const TtfFont* cf = ensure_face_loaded(*cached_rec)) {
                uint16_t gid = cf->glyph_index(codepoint);
                if (gid != 0) {
                    return {cached_rec->id, gid};
                }
            }
        }
    }

    Weight req_weight = primary_rec ? primary_rec->weight : Weight::Normal;
    Style req_style = primary_rec ? primary_rec->style : Style::Normal;
    Stretch req_stretch = primary_rec ? primary_rec->stretch : Stretch::Normal;
    uint8_t os2_bit = codepoint_to_os2_range_bit(codepoint);

    auto update_cache_and_return = [&](uint32_t fid, uint16_t gid) -> std::pair<uint32_t, uint16_t> {
        fallback_page_cache_[cache_idx] = FallbackPageSlot{primary_font_id, page, fid};
        return {fid, gid};
    };

    // 2. Search system-configured fallback chain for the appropriate generic category first
    Family fallback_category = Family::sans_serif();
    if (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) {
        fallback_category = Family::emoji();
    } else if (primary_rec) {
        if (primary_rec->is_monospace) fallback_category = Family::monospace();
        else if (primary_rec->is_serif) fallback_category = Family::serif();
    }

    auto candidate_families = linux_config_.resolve_family_candidates(fallback_category);
    for (const auto& fam_name : candidate_families) {
        std::string key = LinuxFontConfig::normalize_key(fam_name);
        auto it = family_to_faces_.find(key);
        if (it == family_to_faces_.end() || it->second.empty()) continue;

        const FaceRecord* cand = score_faces_in_family(it->second, req_weight, req_style, req_stretch);
        if (!cand || cand->id == primary_font_id) continue;

        // If the candidate face declares OS/2 Unicode ranges, skip loading if it doesn't cover os2_bit
        if (os2_bit < 128 && cand->has_any_unicode_range() && !cand->has_unicode_bit(os2_bit)) {
            continue;
        }

        if (const TtfFont* f = ensure_face_loaded(*cand)) {
            uint16_t gid = f->glyph_index(codepoint);
            if (gid != 0) {
                return update_cache_and_return(cand->id, gid);
            }
        }
    }

    // 3. Pass A: Check already-instantiated faces in memory first (zero disk/mmap overhead)
    for (const auto& rec : faces_) {
        if (rec.id == primary_font_id || !rec.font) continue;
        if (os2_bit < 128 && rec.has_any_unicode_range() && !rec.has_unicode_bit(os2_bit)) {
            continue;
        }
        uint16_t gid = rec.font->glyph_index(codepoint);
        if (gid != 0) {
            return update_cache_and_return(rec.id, gid);
        }
    }

    // 4. Pass B: Check uninstantiated indexed faces matching the OS/2 Unicode Range bit
    if (os2_bit < 128) {
        for (const auto& rec : faces_) {
            if (rec.id == primary_font_id || rec.font || rec.load_failed) continue;
            if (rec.has_any_unicode_range() && !rec.has_unicode_bit(os2_bit)) continue;

            if (const TtfFont* f = ensure_face_loaded(rec)) {
                uint16_t gid = f->glyph_index(codepoint);
                if (gid != 0) {
                    return update_cache_and_return(rec.id, gid);
                }
            }
        }
    }

    // 5. Pass C: Final exhaustive fallback for faces with empty/unpopulated OS/2 range bits
    for (const auto& rec : faces_) {
        if (rec.id == primary_font_id || rec.font || rec.load_failed) continue;
        if (const TtfFont* f = ensure_face_loaded(rec)) {
            uint16_t gid = f->glyph_index(codepoint);
            if (gid != 0) {
                return update_cache_and_return(rec.id, gid);
            }
        }
    }

    return {primary_font_id, 0};
}

size_t FontSystem::loaded_font_count() const noexcept {
    size_t count = 0;
    for (const auto& rec : faces_) {
        if (rec.font != nullptr) ++count;
    }
    return count;
}

std::vector<std::string> FontSystem::list_families() const {
    ensure_default_system_fonts();
    std::vector<std::string> families;
    std::unordered_set<std::string> seen;
    for (const auto& rec : faces_) {
        if (!seen.contains(rec.family_key)) {
            seen.insert(rec.family_key);
            families.push_back(rec.family_name);
        }
    }
    return families;
}

std::vector<FontFaceInfo> FontSystem::list_faces_for_family(std::string_view family_name) const {
    ensure_default_system_fonts();
    std::vector<FontFaceInfo> result;
    std::string key = LinuxFontConfig::normalize_key(family_name);
    auto it = family_to_faces_.find(key);
    if (it == family_to_faces_.end()) return result;

    result.reserve(it->second.size());
    for (size_t idx : it->second) {
        const auto& rec = faces_[idx];
        result.push_back(FontFaceInfo{
            rec.id,
            rec.family_name,
            rec.style_name,
            rec.postscript_name,
            rec.file_path,
            rec.face_index,
            rec.weight,
            rec.style,
            rec.stretch,
            rec.is_monospace,
            rec.is_serif,
            rec.is_color,
            rec.is_variable,
            rec.font != nullptr
        });
    }
    return result;
}

std::optional<FontFaceInfo> FontSystem::get_face_info(uint32_t font_id) const noexcept {
    const FaceRecord* rec = find_face_record(font_id);
    if (!rec) return std::nullopt;
    return FontFaceInfo{
        rec->id,
        rec->family_name,
        rec->style_name,
        rec->postscript_name,
        rec->file_path,
        rec->face_index,
        rec->weight,
        rec->style,
        rec->stretch,
        rec->is_monospace,
        rec->is_serif,
        rec->is_color,
        rec->is_variable,
        rec->font != nullptr
    };
}

} // namespace nisaba::text
