/// @file animation.cpp
/// @brief Implementation of root Lottie & dotLottie (.lottie ZIP) document, builder, dirty tracking, and dynamic overrides.

#include "nisaba/lottie/animation.hpp"
#include "nisaba/json/json.hpp"
#include "nisaba/image/deflate.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nisaba::lottie {

// Internal bridge for constructing O(1) property handles with private constructors
struct PropertyObserverBridge {
    static std::unique_ptr<ColorHandle> make_color_handle(
        std::weak_ptr<void> owner,
        Property<Color>* prop,
        std::shared_ptr<std::atomic<uint64_t>> gen
    ) {
        return std::unique_ptr<ColorHandle>(new ColorHandle(std::move(owner), prop, std::move(gen)));
    }

    static std::unique_ptr<ColorHandle> make_solid_color_handle(
        std::weak_ptr<void> owner,
        SolidLayer* solid,
        std::shared_ptr<std::atomic<uint64_t>> gen
    ) {
        return std::unique_ptr<ColorHandle>(new ColorHandle(std::move(owner), solid, std::move(gen)));
    }

    static std::unique_ptr<OpacityHandle> make_opacity_handle(
        std::weak_ptr<void> owner,
        Property<float>* prop,
        std::shared_ptr<std::atomic<uint64_t>> gen
    ) {
        return std::unique_ptr<OpacityHandle>(new OpacityHandle(std::move(owner), prop, std::move(gen)));
    }

    static std::unique_ptr<TransformHandle> make_transform_handle(
        std::weak_ptr<void> owner,
        LottieTransform* tr,
        std::shared_ptr<std::atomic<uint64_t>> gen
    ) {
        return std::unique_ptr<TransformHandle>(new TransformHandle(std::move(owner), tr, std::move(gen)));
    }

    static std::unique_ptr<TextHandle> make_text_handle(
        std::weak_ptr<void> owner,
        TextLayer* tl,
        std::shared_ptr<std::atomic<uint64_t>> gen
    ) {
        return std::unique_ptr<TextHandle>(new TextHandle(std::move(owner), tl, std::move(gen)));
    }
};

namespace {

// ── Sovereign ZIP / dotLottie (.lottie) Archive Reader ────────────────────

uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct ZipArchive {
    std::unordered_map<std::string, std::vector<uint8_t>> files;

    static std::optional<ZipArchive> parse(std::span<const uint8_t> data) {
        if (data.size() < 22) return std::nullopt;
        if (data[0] != 'P' || data[1] != 'K') return std::nullopt;

        ZipArchive archive;

        // Locate End of Central Directory (EOCD) signature 0x06054b50 from the end
        size_t eocd_pos = std::string_view::npos;
        size_t min_search = (data.size() > 65557) ? (data.size() - 65557) : 0;
        for (size_t i = data.size() - 22; ; --i) {
            if (read_u32_le(data.data() + i) == 0x06054b50u) {
                eocd_pos = i;
                break;
            }
            if (i <= min_search) break;
        }

        if (eocd_pos != std::string_view::npos) {
            uint16_t num_entries = read_u16_le(data.data() + eocd_pos + 10);
            uint32_t cd_offset = read_u32_le(data.data() + eocd_pos + 16);

            size_t pos = cd_offset;
            for (uint16_t e = 0; e < num_entries && pos + 46 <= data.size(); ++e) {
                if (read_u32_le(data.data() + pos) != 0x02014b50u) break;
                uint16_t method = read_u16_le(data.data() + pos + 10);
                uint32_t comp_size = read_u32_le(data.data() + pos + 20);
                uint32_t uncomp_size = read_u32_le(data.data() + pos + 24);
                uint16_t name_len = read_u16_le(data.data() + pos + 28);
                uint16_t extra_len = read_u16_le(data.data() + pos + 30);
                uint16_t comment_len = read_u16_le(data.data() + pos + 32);
                uint32_t lfh_offset = read_u32_le(data.data() + pos + 42);

                if (pos + 46 + name_len > data.size()) break;
                std::string filename(reinterpret_cast<const char*>(data.data() + pos + 46), name_len);
                std::replace(filename.begin(), filename.end(), '\\', '/');
                if (filename.starts_with("./")) filename.erase(0, 2);

                pos += 46 + name_len + extra_len + comment_len;

                if (lfh_offset + 30 <= data.size() && read_u32_le(data.data() + lfh_offset) == 0x04034b50u) {
                    uint16_t lfh_name_len = read_u16_le(data.data() + lfh_offset + 26);
                    uint16_t lfh_extra_len = read_u16_le(data.data() + lfh_offset + 28);
                    size_t data_offset = lfh_offset + 30 + lfh_name_len + lfh_extra_len;
                    if (data_offset + comp_size <= data.size()) {
                        std::span<const uint8_t> payload(data.data() + data_offset, comp_size);
                        if (method == 0) {
                            archive.files[filename] = std::vector<uint8_t>(payload.begin(), payload.end());
                        } else if (method == 8) {
                            auto dec = image::deflate_decompress(payload, uncomp_size);
                            if (dec.has_value()) {
                                archive.files[filename] = std::move(*dec);
                            }
                        }
                    }
                }
            }
        } else {
            // Sequential Local File Header scan fallback
            size_t pos = 0;
            while (pos + 30 <= data.size() && read_u32_le(data.data() + pos) == 0x04034b50u) {
                uint16_t method = read_u16_le(data.data() + pos + 8);
                uint32_t comp_size = read_u32_le(data.data() + pos + 18);
                uint32_t uncomp_size = read_u32_le(data.data() + pos + 22);
                uint16_t name_len = read_u16_le(data.data() + pos + 26);
                uint16_t extra_len = read_u16_le(data.data() + pos + 28);
                size_t data_offset = pos + 30 + name_len + extra_len;
                if (data_offset + comp_size > data.size()) break;

                std::string filename(reinterpret_cast<const char*>(data.data() + pos + 30), name_len);
                std::replace(filename.begin(), filename.end(), '\\', '/');
                std::span<const uint8_t> payload(data.data() + data_offset, comp_size);
                if (method == 0) {
                    archive.files[filename] = std::vector<uint8_t>(payload.begin(), payload.end());
                } else if (method == 8) {
                    auto dec = image::deflate_decompress(payload, uncomp_size);
                    if (dec.has_value()) {
                        archive.files[filename] = std::move(*dec);
                    }
                }
                pos = data_offset + comp_size;
            }
        }

        if (archive.files.empty()) return std::nullopt;
        return archive;
    }

    const std::vector<uint8_t>* find_file(std::string_view path) const {
        std::string norm(path);
        std::replace(norm.begin(), norm.end(), '\\', '/');
        if (norm.starts_with("./")) norm.erase(0, 2);
        if (norm.starts_with("/")) norm.erase(0, 1);

        auto it = files.find(norm);
        if (it != files.end()) return &it->second;

        auto slash = norm.find_last_of('/');
        std::string_view base = (slash != std::string::npos) ? std::string_view(norm).substr(slash + 1) : std::string_view(norm);
        for (const auto& [k, v] : files) {
            if (k == base || (k.size() > base.size() && k[k.size() - base.size() - 1] == '/' &&
                              std::string_view(k).ends_with(base))) {
                return &v;
            }
        }
        return nullptr;
    }
};

/// ResourceProvider backed by an extracted dotLottie ZIP archive with optional fallback.
class ArchiveResourceProvider final : public ResourceProvider {
public:
    ArchiveResourceProvider(
        ZipArchive archive,
        std::shared_ptr<ResourceProvider> fallback,
        bool defer_decode
    ) : archive_(std::move(archive)), fallback_(std::move(fallback)) {
        defer_image_decoding_ = defer_decode;
    }

    std::vector<uint8_t> load(std::string_view resource_path, std::string_view resource_name) override {
        std::string combined = std::string(resource_path) + std::string(resource_name);
        if (const auto* f = archive_.find_file(combined)) return *f;
        if (!resource_name.empty()) {
            if (const auto* f = archive_.find_file("images/" + std::string(resource_name))) return *f;
            if (const auto* f = archive_.find_file(resource_name)) return *f;
        }
        return fallback_ ? fallback_->load(resource_path, resource_name) : std::vector<uint8_t>{};
    }

    std::shared_ptr<ImageAsset> load_image_asset(
        std::string_view resource_path,
        std::string_view resource_name,
        std::string_view resource_id
    ) override {
        auto bytes = load(resource_path, resource_name);
        if (!bytes.empty()) {
            return DecodedImageAsset::from_encoded_bytes(std::move(bytes), defer_image_decoding_);
        }
        return fallback_ ? fallback_->load_image_asset(resource_path, resource_name, resource_id) : nullptr;
    }

    std::vector<uint8_t> load_font(std::string_view font_name, std::string_view font_url) override {
        if (!font_url.empty()) {
            if (const auto* f = archive_.find_file(font_url)) return *f;
        }
        if (!font_name.empty()) {
            if (const auto* f = archive_.find_file(font_name)) return *f;
        }
        return fallback_ ? fallback_->load_font(font_name, font_url) : std::vector<uint8_t>{};
    }

private:
    ZipArchive archive_;
    std::shared_ptr<ResourceProvider> fallback_;
};

// ── Diagnostics Helpers ───────────────────────────────────────────────────

void log_warn(const std::shared_ptr<Logger>& logger, std::string_view msg, std::string_view ctx = {}) {
    if (logger) logger->log(Logger::Level::Warning, msg, ctx);
}

void log_error(const std::shared_ptr<Logger>& logger, std::string_view msg, std::string_view ctx = {}) {
    if (logger) logger->log(Logger::Level::Error, msg, ctx);
}

void inspect_layer_diagnostics(
    const json::JsonValue& l_val,
    const std::shared_ptr<Logger>& logger
) {
    if (!logger || !l_val.is_object()) return;
    const int ty = l_val.get_int("ty", 3);
    const std::string_view nm = l_val.get_string("nm", "");
    switch (ty) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6:
            break;
        default:
            log_warn(logger, "Unsupported layer type (falling back to NullLayer): ty=" + std::to_string(ty), nm);
            break;
    }
    if (const auto* ef_val = l_val.get("ef"); ef_val && ef_val->is_array()) {
        for (const auto& e_val : ef_val->as_array()) {
            if (!e_val.is_object()) continue;
            if (e_val.get_int("en", 1) == 0) continue;
            if (!LayerEffect::is_supported_effect_json(e_val)) {
                const int ety = e_val.get_int("ty", -1);
                log_warn(
                    logger,
                    "Unsupported layer effect type: ty=" + std::to_string(ety),
                    e_val.get_string("nm", nm)
                );
            }
        }
    }
}

// ── PropertyObserver Traversal ────────────────────────────────────────────

std::string join_keypath(const std::string& prefix, std::string_view name) {
    if (prefix.empty()) return std::string(name);
    if (name.empty()) return prefix;
    return prefix + "." + std::string(name);
}

void notify_shape_item_observer(
    const std::shared_ptr<LottieShapeItem>& item,
    const std::string& parent_path,
    PropertyObserver& observer,
    const std::shared_ptr<Animation>& anim_owner,
    const std::shared_ptr<std::atomic<uint64_t>>& mutation_gen
) {
    if (!item) return;
    const std::string kp = join_keypath(parent_path, item->name);
    observer.on_node_enter(item->name);

    const std::string_view tn = item->type_name();
    std::weak_ptr<void> weak_owner(anim_owner);

    if (tn == "gr") {
        auto grp = std::static_pointer_cast<ShapeGroup>(item);
        observer.on_transform_property(kp, [weak_owner, tr = &grp->transform, mutation_gen]() {
            return PropertyObserverBridge::make_transform_handle(weak_owner, tr, mutation_gen);
        });
        observer.on_opacity_property(kp, [weak_owner, op = &grp->transform.opacity, mutation_gen]() {
            return PropertyObserverBridge::make_opacity_handle(weak_owner, op, mutation_gen);
        });
        for (const auto& child : grp->items) {
            notify_shape_item_observer(child, kp, observer, anim_owner, mutation_gen);
        }
    } else if (tn == "fl") {
        auto fl = std::static_pointer_cast<FillShape>(item);
        observer.on_color_property(kp, [weak_owner, cp = &fl->color, mutation_gen]() {
            return PropertyObserverBridge::make_color_handle(weak_owner, cp, mutation_gen);
        });
        observer.on_opacity_property(kp, [weak_owner, op = &fl->opacity, mutation_gen]() {
            return PropertyObserverBridge::make_opacity_handle(weak_owner, op, mutation_gen);
        });
    } else if (tn == "st") {
        auto st = std::static_pointer_cast<StrokeShape>(item);
        observer.on_color_property(kp, [weak_owner, cp = &st->color, mutation_gen]() {
            return PropertyObserverBridge::make_color_handle(weak_owner, cp, mutation_gen);
        });
        observer.on_opacity_property(kp, [weak_owner, op = &st->opacity, mutation_gen]() {
            return PropertyObserverBridge::make_opacity_handle(weak_owner, op, mutation_gen);
        });
    } else if (tn == "gf") {
        auto gf = std::static_pointer_cast<GradientFillShape>(item);
        observer.on_opacity_property(kp, [weak_owner, op = &gf->opacity, mutation_gen]() {
            return PropertyObserverBridge::make_opacity_handle(weak_owner, op, mutation_gen);
        });
    } else if (tn == "gs") {
        auto gs = std::static_pointer_cast<GradientStrokeShape>(item);
        observer.on_opacity_property(kp, [weak_owner, op = &gs->opacity, mutation_gen]() {
            return PropertyObserverBridge::make_opacity_handle(weak_owner, op, mutation_gen);
        });
    }

    observer.on_node_leave(item->name);
}

void notify_layers_observer(
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    const std::string& parent_path,
    PropertyObserver& observer,
    const std::shared_ptr<Animation>& anim_owner,
    const std::shared_ptr<std::atomic<uint64_t>>& mutation_gen
) {
    std::weak_ptr<void> weak_owner(anim_owner);
    for (const auto& layer : layers) {
        if (!layer) continue;
        const std::string kp = join_keypath(parent_path, layer->name);
        observer.on_node_enter(layer->name);

        observer.on_transform_property(kp, [weak_owner, tr = &layer->transform, mutation_gen]() {
            return PropertyObserverBridge::make_transform_handle(weak_owner, tr, mutation_gen);
        });
        observer.on_opacity_property(kp, [weak_owner, op = &layer->transform.opacity, mutation_gen]() {
            return PropertyObserverBridge::make_opacity_handle(weak_owner, op, mutation_gen);
        });

        if (layer->type == LayerType::Solid) {
            auto sl = std::static_pointer_cast<SolidLayer>(layer);
            observer.on_color_property(kp, [weak_owner, solid = sl.get(), mutation_gen]() {
                return PropertyObserverBridge::make_solid_color_handle(weak_owner, solid, mutation_gen);
            });
        } else if (layer->type == LayerType::Text) {
            auto tl = std::static_pointer_cast<TextLayer>(layer);
            observer.on_text_property(kp, [weak_owner, text_l = tl.get(), mutation_gen]() {
                return PropertyObserverBridge::make_text_handle(weak_owner, text_l, mutation_gen);
            });
        } else if (layer->type == LayerType::Shape) {
            auto shl = std::static_pointer_cast<ShapeLayer>(layer);
            for (const auto& item : shl->shapes) {
                notify_shape_item_observer(item, kp, observer, anim_owner, mutation_gen);
            }
        }

        observer.on_node_leave(layer->name);
    }
}

// ── Keypath Matching & Property Override Traversal ────────────────────────

void split_keypath(std::string_view kp, std::vector<std::string_view>& out_parts) {
    out_parts.clear();
    size_t pos = 0;
    while (pos <= kp.size()) {
        size_t next = kp.find('.', pos);
        if (next == std::string_view::npos) {
            if (pos < kp.size()) out_parts.push_back(kp.substr(pos));
            break;
        }
        out_parts.push_back(kp.substr(pos, next - pos));
        pos = next + 1;
    }
}

bool match_segment(std::string_view pattern, std::string_view name) noexcept {
    if (pattern == "*" || pattern == "**") return true;
    return pattern == name;
}

bool match_keypath_prefix(const std::vector<std::string_view>& parts, size_t idx, std::string_view item_name) noexcept {
    if (parts.empty()) return true;
    if (parts.size() == 1 && parts[0] == "*") return true;
    if (idx < parts.size()) {
        return match_segment(parts[idx], item_name);
    }
    return parts.back() == "*" || parts.back() == "**" || parts.back() == item_name;
}

bool apply_color_to_shape_item(LottieShapeItem& item, const std::vector<std::string_view>& parts, size_t depth, Color color) {
    bool matched = false;
    bool seg_ok = (depth >= parts.size()) || match_keypath_prefix(parts, depth, item.name) || (parts.size() == 1 && match_segment(parts[0], item.name));

    if (item.type_name() == "gr") {
        auto& grp = static_cast<ShapeGroup&>(item);
        size_t next_depth = seg_ok ? (depth + 1) : depth;
        for (auto& child : grp.items) {
            if (child && apply_color_to_shape_item(*child, parts, next_depth, color)) {
                matched = true;
            }
        }
    } else if (item.type_name() == "fl" && seg_ok) {
        static_cast<FillShape&>(item).color.set_override(color);
        matched = true;
    } else if (item.type_name() == "st" && seg_ok) {
        static_cast<StrokeShape&>(item).color.set_override(color);
        matched = true;
    }
    return matched;
}

bool apply_scalar_to_shape_item(LottieShapeItem& item, const std::vector<std::string_view>& parts, size_t depth, float value) {
    bool matched = false;
    bool seg_ok = (depth >= parts.size()) || match_keypath_prefix(parts, depth, item.name) || (parts.size() == 1 && match_segment(parts[0], item.name));

    if (item.type_name() == "gr") {
        auto& grp = static_cast<ShapeGroup&>(item);
        if (seg_ok && depth + 1 >= parts.size()) {
            grp.transform.opacity.set_override(value);
            matched = true;
        }
        size_t next_depth = seg_ok ? (depth + 1) : depth;
        for (auto& child : grp.items) {
            if (child && apply_scalar_to_shape_item(*child, parts, next_depth, value)) {
                matched = true;
            }
        }
    } else if (item.type_name() == "fl" && seg_ok) {
        static_cast<FillShape&>(item).opacity.set_override(value);
        matched = true;
    } else if (item.type_name() == "st" && seg_ok) {
        static_cast<StrokeShape&>(item).width.set_override(value);
        matched = true;
    } else if (item.type_name() == "gs" && seg_ok) {
        static_cast<GradientStrokeShape&>(item).width.set_override(value);
        matched = true;
    } else if (item.type_name() == "rd" && seg_ok) {
        static_cast<RoundCornersShape&>(item).radius.set_override(value);
        matched = true;
    }
    return matched;
}

bool apply_point_to_shape_item(LottieShapeItem& item, const std::vector<std::string_view>& parts, size_t depth, Point value) {
    bool matched = false;
    bool seg_ok = (depth >= parts.size()) || match_keypath_prefix(parts, depth, item.name) || (parts.size() == 1 && match_segment(parts[0], item.name));

    if (item.type_name() == "gr") {
        auto& grp = static_cast<ShapeGroup&>(item);
        if (seg_ok && depth + 1 >= parts.size()) {
            grp.transform.position.set_override(value);
            matched = true;
        }
        size_t next_depth = seg_ok ? (depth + 1) : depth;
        for (auto& child : grp.items) {
            if (child && apply_point_to_shape_item(*child, parts, next_depth, value)) {
                matched = true;
            }
        }
    } else if (item.type_name() == "rc" && seg_ok) {
        auto& rc = static_cast<RectShape&>(item);
        rc.size.set_override(value);
        rc.has_cached_static = false;
        matched = true;
    } else if (item.type_name() == "el" && seg_ok) {
        auto& el = static_cast<EllipseShape&>(item);
        el.size.set_override(value);
        el.has_cached_static = false;
        matched = true;
    }
    return matched;
}

bool apply_slot_color_to_shape_item(LottieShapeItem& item, std::string_view slot_id, Color color) {
    bool matched = false;
    if (item.type_name() == "gr") {
        auto& grp = static_cast<ShapeGroup&>(item);
        for (auto& child : grp.items) {
            if (child && apply_slot_color_to_shape_item(*child, slot_id, color)) {
                matched = true;
            }
        }
    } else if (item.type_name() == "fl") {
        auto& fl = static_cast<FillShape&>(item);
        if (fl.color.slot_id() == slot_id) {
            fl.color.set_override(color);
            matched = true;
        }
    } else if (item.type_name() == "st") {
        auto& st = static_cast<StrokeShape&>(item);
        if (st.color.slot_id() == slot_id) {
            st.color.set_override(color);
            matched = true;
        }
    }
    return matched;
}

void clear_shape_item_overrides(LottieShapeItem& item) {
    if (item.type_name() == "gr") {
        auto& grp = static_cast<ShapeGroup&>(item);
        grp.transform.anchor.clear_override();
        grp.transform.position.clear_override();
        if (grp.transform.x_pos) grp.transform.x_pos->clear_override();
        if (grp.transform.y_pos) grp.transform.y_pos->clear_override();
        grp.transform.scale.clear_override();
        grp.transform.rotation.clear_override();
        grp.transform.opacity.clear_override();
        grp.transform.skew.clear_override();
        grp.transform.skew_axis.clear_override();
        for (auto& child : grp.items) {
            if (child) clear_shape_item_overrides(*child);
        }
    } else if (item.type_name() == "fl") {
        auto& fl = static_cast<FillShape&>(item);
        fl.color.clear_override();
        fl.opacity.clear_override();
    } else if (item.type_name() == "st") {
        auto& st = static_cast<StrokeShape&>(item);
        st.color.clear_override();
        st.opacity.clear_override();
        st.width.clear_override();
    } else if (item.type_name() == "gs") {
        auto& gs = static_cast<GradientStrokeShape&>(item);
        gs.opacity.clear_override();
        gs.width.clear_override();
    } else if (item.type_name() == "rc") {
        auto& rc = static_cast<RectShape&>(item);
        rc.position.clear_override();
        rc.size.clear_override();
        rc.roundness.clear_override();
    } else if (item.type_name() == "el") {
        auto& el = static_cast<EllipseShape&>(item);
        el.position.clear_override();
        el.size.clear_override();
    } else if (item.type_name() == "rd") {
        static_cast<RoundCornersShape&>(item).radius.clear_override();
    }
}

} // anonymous namespace

// ── Animation::Builder ────────────────────────────────────────────────────

std::shared_ptr<Animation> Animation::Builder::make_from_bytes(
    std::span<const uint8_t> bytes,
    std::string_view base_dir
) const {
    if (bytes.empty()) {
        log_error(logger_, "Empty animation input buffer");
        return nullptr;
    }

    // Detect dotLottie (.lottie) ZIP container ("PK")
    if (bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K') {
        auto zip = ZipArchive::parse(bytes);
        if (!zip) {
            log_error(logger_, "Failed to parse .lottie ZIP archive");
            return nullptr;
        }

        std::string target_json_path;
        if (const auto* manifest_bytes = zip->find_file("manifest.json")) {
            std::string_view manifest_sv(reinterpret_cast<const char*>(manifest_bytes->data()), manifest_bytes->size());
            if (auto m_json = json::JsonValue::parse(manifest_sv); m_json && m_json->is_object()) {
                if (const auto* anims = m_json->get("animations"); anims && anims->is_array() && !anims->as_array().empty()) {
                    std::string id = std::string(anims->as_array()[0].get_string("id", ""));
                    if (!id.empty()) {
                        target_json_path = "animations/" + id + ".json";
                    }
                }
            } else {
                log_warn(logger_, "Failed to parse manifest.json inside .lottie archive");
            }
        }

        const std::vector<uint8_t>* anim_json_bytes = nullptr;
        if (!target_json_path.empty()) {
            anim_json_bytes = zip->find_file(target_json_path);
        }
        if (!anim_json_bytes) {
            for (const auto& [k, v] : zip->files) {
                if (k != "manifest.json" && k.ends_with(".json")) {
                    anim_json_bytes = &v;
                    break;
                }
            }
        }
        if (!anim_json_bytes) {
            log_error(logger_, "No animation JSON payload found inside .lottie archive");
            return nullptr;
        }

        // Copy JSON payload before moving `zip` into ArchiveResourceProvider
        std::string json_copy(reinterpret_cast<const char*>(anim_json_bytes->data()), anim_json_bytes->size());
        const bool defer_img = (flags_ & kDeferImageLoading) != 0;

        std::shared_ptr<ResourceProvider> fallback = resource_provider_;
        if (!fallback && !base_dir.empty()) {
            fallback = std::make_shared<FileResourceProvider>(std::string(base_dir), defer_img);
        }
        auto archive_rp = std::make_shared<ArchiveResourceProvider>(std::move(*zip), std::move(fallback), defer_img);

        Builder sub_builder = *this;
        sub_builder.set_resource_provider(std::move(archive_rp));
        return sub_builder.make(json_copy, base_dir);
    }

    std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return make(sv, base_dir);
}

std::shared_ptr<Animation> Animation::Builder::make(
    std::string_view content,
    std::string_view base_dir
) const {
    if (content.empty()) {
        log_error(logger_, "Empty animation content");
        return nullptr;
    }

    if (content.size() >= 4 && content[0] == 'P' && content[1] == 'K') {
        std::span<const uint8_t> raw(reinterpret_cast<const uint8_t*>(content.data()), content.size());
        return make_from_bytes(raw, base_dir);
    }

    Stats local_stats{};
    local_stats.json_size_bytes = content.size();

    using Clock = std::chrono::steady_clock;
    const auto t_json_start = Clock::now();
    auto opt_root = json::JsonValue::parse(content);
    const auto t_json_end = Clock::now();
    local_stats.json_parse_ms = std::chrono::duration<double, std::milli>(t_json_end - t_json_start).count();

    if (!opt_root || !opt_root->is_object()) {
        log_error(logger_, "Invalid Lottie JSON document (parse failure or root is not an object)");
        if (out_stats_) *out_stats_ = local_stats;
        return nullptr;
    }

    const auto t_scene_start = Clock::now();
    const auto& root = *opt_root;

    auto anim = std::make_shared<Animation>();
    anim->load_flags_ = flags_;
    anim->version_ = std::string(root.get_string("v", ""));
    anim->name_ = std::string(root.get_string("nm", ""));
    anim->width_ = root.get_float("w", 0.0f);
    anim->height_ = root.get_float("h", 0.0f);
    anim->in_point_ = root.get_float("ip", 0.0f);
    anim->out_point_ = root.get_float("op", 0.0f);
    anim->frame_rate_ = root.get_float("fr", 60.0f);
    if (anim->frame_rate_ <= 0.0f) anim->frame_rate_ = 60.0f;
    anim->current_sought_frame_ = anim->in_point_;

    anim->assets_.comp_width = (anim->width_ > 0.0f) ? anim->width_ : 512.0f;
    anim->assets_.comp_height = (anim->height_ > 0.0f) ? anim->height_ : 512.0f;
    anim->assets_.frame_rate = anim->frame_rate_;
    anim->assets_.font_system = font_system_ ? font_system_ : std::make_shared<text::FontSystem>();

    const bool defer_img = (flags_ & kDeferImageLoading) != 0;

    // Build effective resource provider chain: DataURI proxy -> user provider (or default FileResourceProvider)
    std::shared_ptr<ResourceProvider> base_rp = resource_provider_;
    if (base_rp) {
        if (defer_img) base_rp->set_defer_image_decoding(true);
    } else {
        base_rp = std::make_shared<FileResourceProvider>(std::string(base_dir), defer_img);
    }
    auto rp = std::make_shared<DataURIResourceProviderProxy>(std::move(base_rp), defer_img);

    // Parse fonts ("fonts": { "list": [ ... ] })
    if (const auto* fonts_val = root.get("fonts"); fonts_val && fonts_val->is_object()) {
        if (const auto* list_val = fonts_val->get("list"); list_val && list_val->is_array()) {
            for (const auto& f_val : list_val->as_array()) {
                if (!f_val.is_object()) continue;
                LottieFontDef fdef;
                fdef.name = std::string(f_val.get_string("fName", ""));
                fdef.family = std::string(f_val.get_string("fFamily", ""));
                fdef.style = std::string(f_val.get_string("fStyle", ""));
                fdef.path = std::string(f_val.get_string("fPath", ""));
                fdef.ascent = f_val.get_float("ascent", 75.0f);

                if (anim->assets_.font_system && (!fdef.path.empty() || resource_provider_ != nullptr)) {
                    auto font_bytes = rp->load_font(fdef.name, fdef.path);
                    if (!font_bytes.empty()) {
                        fdef.loaded_font_id = anim->assets_.font_system->load_font_data(font_bytes);
                        if (!fdef.loaded_font_id.has_value()) {
                            log_warn(logger_, "Failed to decode font stream from ResourceProvider", fdef.name);
                        }
                    } else if (!fdef.path.empty()) {
                        log_warn(logger_, "Failed to load external font resource: " + fdef.path, fdef.name);
                    }
                }
                if (!fdef.name.empty()) {
                    anim->assets_.fonts[fdef.name] = std::move(fdef);
                }
            }
        }
    }

    // Parse pre-baked glyph characters ("chars": [ ... ])
    if (const auto* chars_val = root.get("chars"); chars_val && chars_val->is_array()) {
        for (const auto& c_val : chars_val->as_array()) {
            if (!c_val.is_object()) continue;
            LottieGlyphChar gc;
            gc.ch = std::string(c_val.get_string("ch", ""));
            gc.style = std::string(c_val.get_string("style", ""));
            gc.family = std::string(c_val.get_string("fFamily", ""));
            gc.size = c_val.get_float("size", 100.0f);
            gc.width = c_val.get_float("w", 0.0f);
            if (const auto* data_obj = c_val.get("data"); data_obj && data_obj->is_object()) {
                if (const auto* shapes_arr = data_obj->get("shapes"); shapes_arr && shapes_arr->is_array()) {
                    for (const auto& s_val : shapes_arr->as_array()) {
                        if (auto sg = ShapeGroup::parse(s_val)) {
                            gc.shapes.push_back(std::move(sg));
                        }
                    }
                }
            }
            anim->assets_.chars.push_back(std::move(gc));
        }
    }

    // Parse precomp, image, and audio assets
    if (const auto* assets_val = root.get("assets"); assets_val && assets_val->is_array()) {
        for (const auto& asset : assets_val->as_array()) {
            if (!asset.is_object()) continue;
            std::string id = std::string(asset.get_string("id", ""));
            if (id.empty()) continue;

            if (const auto* layers_val = asset.get("layers"); layers_val && layers_val->is_array()) {
                std::vector<std::shared_ptr<LottieLayer>> sub_layers;
                for (const auto& l_val : layers_val->as_array()) {
                    inspect_layer_diagnostics(l_val, logger_);
                    if (auto l = LottieLayer::parse(l_val)) {
                        local_stats.layer_count++;
                        local_stats.animator_count += l->count_animators();
                        sub_layers.push_back(std::move(l));
                    }
                }
                link_layer_hierarchy(sub_layers);
                anim->assets_.precomps[std::move(id)] = std::move(sub_layers);
                local_stats.precomp_count++;
            } else if (asset.has("p")) {
                const std::string dir = std::string(asset.get_string("u", ""));
                const std::string fname = std::string(asset.get_string("p", ""));
                const bool is_audio = fname.ends_with(".mp3") || fname.ends_with(".wav") ||
                                      fname.ends_with(".ogg") || fname.starts_with("data:audio/");

                if (is_audio) {
                    if (auto audio_asset = rp->load_audio_asset(dir, fname, id)) {
                        anim->assets_.audio_assets[id] = std::move(audio_asset);
                    } else {
                        log_warn(logger_, "Failed to load audio asset: " + fname, id);
                    }
                } else {
                    LottieImageAsset img;
                    img.id = id;
                    img.directory = dir;
                    img.filename = fname;
                    img.width = asset.get_float("w", 0.0f);
                    img.height = asset.get_float("h", 0.0f);
                    img.embedded = (asset.get_int("e", 0) == 1) || img.filename.starts_with("data:");

                    img.asset = rp->load_image_asset(img.directory, img.filename, img.id);
                    if (!img.asset) {
                        log_warn(logger_, "Failed to resolve image asset: " + img.filename, img.id);
                    } else if (!defer_img && !img.asset->is_multi_frame()) {
                        // Also populate legacy `pixmap` field for backward-compatible direct readers
                        if (auto ref = img.asset->get_frame(0.0f)) {
                            img.pixmap = ref->to_owned();
                        }
                    }
                    anim->assets_.images[std::move(id)] = std::move(img);
                    local_stats.image_asset_count++;
                }
            }
        }
    }

    // Parse root layers
    if (const auto* layers_val = root.get("layers"); layers_val && layers_val->is_array()) {
        for (const auto& l_val : layers_val->as_array()) {
            inspect_layer_diagnostics(l_val, logger_);
            if (auto l = LottieLayer::parse(l_val)) {
                local_stats.layer_count++;
                local_stats.animator_count += l->count_animators();
                anim->layers_.push_back(std::move(l));
            }
        }
    }
    link_layer_hierarchy(anim->layers_);

    // Apply PrecompInterceptor across root and nested precomp layers
    if (precomp_interceptor_) {
        auto intercept_list = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
            for (auto& l : list) {
                if (!l || l->type != LayerType::Precomp) continue;
                auto& pl = static_cast<PrecompLayer&>(*l);
                const float pw = (pl.width > 0.0f) ? pl.width : anim->assets_.comp_width;
                const float ph = (pl.height > 0.0f) ? pl.height : anim->assets_.comp_height;
                if (auto sz = Size::from_wh(std::max(1.0f, pw), std::max(1.0f, ph))) {
                    if (auto ext = precomp_interceptor_->on_load_precomp(pl.ref_id, pl.name, *sz)) {
                        pl.external_layer = std::move(ext);
                    }
                }
            }
        };
        intercept_list(anim->layers_);
        for (auto& [pid, sub] : anim->assets_.precomps) {
            (void)pid;
            intercept_list(sub);
        }
    }

    // Validate missing asset references via Logger
    if (logger_) {
        auto validate_refs = [&](const std::vector<std::shared_ptr<LottieLayer>>& list) {
            for (const auto& l : list) {
                if (!l) continue;
                if (l->type == LayerType::Precomp) {
                    const auto& pl = static_cast<const PrecompLayer&>(*l);
                    if (!pl.external_layer && !pl.ref_id.empty() &&
                        anim->assets_.precomps.find(pl.ref_id) == anim->assets_.precomps.end()) {
                        log_warn(logger_, "PrecompLayer references missing asset id: " + pl.ref_id, pl.name);
                    }
                } else if (l->type == LayerType::Image) {
                    const auto& il = static_cast<const ImageLayer&>(*l);
                    if (!il.ref_id.empty() && anim->assets_.images.find(il.ref_id) == anim->assets_.images.end()) {
                        log_warn(logger_, "ImageLayer references missing image asset id: " + il.ref_id, il.name);
                    }
                }
            }
        };
        validate_refs(anim->layers_);
        for (const auto& [pid, sub] : anim->assets_.precomps) {
            (void)pid;
            validate_refs(sub);
        }
    }

    // Parse markers
    if (const auto* markers_val = root.get("markers"); markers_val && markers_val->is_array()) {
        for (const auto& m_val : markers_val->as_array()) {
            if (!m_val.is_object()) continue;
            Marker m;
            m.name = std::string(m_val.get_string("cm", ""));
            m.time = m_val.get_float("tm", 0.0f);
            m.duration = m_val.get_float("dr", 0.0f);
            anim->markers_.push_back(std::move(m));
        }
    }

    // Parse slots ("slots": { "slot_id": { "p": { ... } } })
    if (const auto* slots_val = root.get("slots"); slots_val && slots_val->is_object()) {
        for (const auto& [sid, s_obj] : slots_val->as_object()) {
            if (!s_obj.is_object()) continue;
            if (const auto* p_val = s_obj.get("p")) {
                ColorProperty cp = parse_color_property(*p_val, Color::WHITE);
                anim->set_slot_color(sid, cp.evaluate(anim->in_point_));
            }
        }
    }

    // Notify PropertyObserver so host code can bind O(1) typed handles
    if (property_observer_) {
        notify_layers_observer(anim->layers_, "", *property_observer_, anim, anim->mutation_gen_);
        for (const auto& [pid, sub] : anim->assets_.precomps) {
            notify_layers_observer(sub, pid, *property_observer_, anim, anim->mutation_gen_);
        }
    }

    // Bind pluggable ExpressionManager to all properties across root & precomp layers
    if (expression_manager_) {
        size_t total_anim = 0;
        for (auto& l : anim->layers_) {
            if (l) {
                l->bind_expression_manager(*expression_manager_);
                total_anim += l->count_animators();
            }
        }
        for (auto& [pid, sub] : anim->assets_.precomps) {
            (void)pid;
            for (auto& l : sub) {
                if (l) {
                    l->bind_expression_manager(*expression_manager_);
                    total_anim += l->count_animators();
                }
            }
        }
        local_stats.animator_count = total_anim;
    }

    const auto t_scene_end = Clock::now();
    local_stats.scene_build_ms = std::chrono::duration<double, std::milli>(t_scene_end - t_scene_start).count();
    anim->stats_ = local_stats;
    if (out_stats_) {
        *out_stats_ = local_stats;
    }

    return anim;
}

std::shared_ptr<Animation> Animation::Builder::make_from_file(const std::string& filepath) const {
    std::ifstream file(filepath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        log_error(logger_, "Failed to open animation file: " + filepath);
        return nullptr;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    std::string base_dir;
    std::filesystem::path p(filepath);
    if (p.has_parent_path()) {
        base_dir = p.parent_path().string();
    }
    return make(content, base_dir);
}

// ── Static Legacy Loaders (Forward to Builder) ────────────────────────────

std::shared_ptr<Animation> Animation::load_from_bytes(std::span<const uint8_t> bytes, std::string_view base_dir) {
    return Builder().make_from_bytes(bytes, base_dir);
}

std::shared_ptr<Animation> Animation::load_from_data(std::string_view content, std::string_view base_dir) {
    return Builder().make(content, base_dir);
}

std::shared_ptr<Animation> Animation::load_from_file(const std::string& filepath) {
    return Builder().make_from_file(filepath);
}

// ── Frame Seeking, Deferred Image Warming & Dirty-Region Tracking ─────────

void Animation::warm_visible_deferred_assets(
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    float frame
) const {
    if ((load_flags_ & kDeferImageLoading) == 0) return;

    const float fps = (assets_.frame_rate > 0.0f) ? assets_.frame_rate : 60.0f;
    for (const auto& layer : layers) {
        if (!layer || !layer->is_active(frame)) continue;
        if (layer->type == LayerType::Image) {
            const auto& il = static_cast<const ImageLayer&>(*layer);
            auto it = assets_.images.find(il.ref_id);
            if (it != assets_.images.end() && it->second.asset && !it->second.asset->is_decoded()) {
                const float t_sec = std::max(0.0f, ((frame - il.start_time) / il.time_stretch) / fps);
                (void)it->second.asset->get_frame(t_sec);
            }
        } else if (layer->type == LayerType::Precomp) {
            const auto& pl = static_cast<const PrecompLayer&>(*layer);
            auto it = assets_.precomps.find(pl.ref_id);
            if (it != assets_.precomps.end()) {
                const float sub_frame = pl.has_time_remap
                    ? pl.time_remap.evaluate(frame)
                    : (frame - pl.start_time) / pl.time_stretch;
                warm_visible_deferred_assets(it->second, sub_frame);
            }
        }
    }
}

void Animation::reset_invalidation_state() noexcept {
    has_sought_once_ = false;
    layer_damage_.clear();
}

void Animation::seek_frame(float frame, InvalidationController* ic) {
    current_sought_frame_ = frame;

    // Decode any deferred images that are now visible at `frame`
    warm_visible_deferred_assets(layers_, frame);

    if (!ic) {
        // Still keep damage tracking synchronized if caller switches between ic=null and ic!=null
        if (has_sought_once_) {
            reset_invalidation_state();
        }
        return;
    }

    const uint64_t cur_mut_gen = mutation_gen_ ? mutation_gen_->load(std::memory_order_relaxed) : 0u;
    if (layer_damage_.size() != layers_.size()) {
        layer_damage_.assign(layers_.size(), LayerDamageEntry{});
        has_sought_once_ = false;
    }

    for (size_t i = 0; i < layers_.size(); ++i) {
        const auto& layer = layers_[i];
        auto& state = layer_damage_[i];
        if (!layer || layer->is_matte_target) {
            continue;
        }

        const bool now_active = layer->is_active(frame);

        if (!has_sought_once_) {
            state.was_active = now_active;
            state.prev_mutation_gen = cur_mut_gen;
            if (now_active) {
                state.prev_bounds = layer->compute_complete_bounds(frame, assets_);
                state.prev_fingerprint = layer->visual_fingerprint(frame, assets_);
                if (state.prev_bounds.has_value()) {
                    ic->inval(*state.prev_bounds);
                }
            } else {
                state.prev_bounds.reset();
                state.prev_fingerprint = 0u;
            }
            continue;
        }

        const bool mutated = (state.prev_mutation_gen != cur_mut_gen);
        const bool animated = layer->is_layer_animated(assets_);

        // Static layer whose active state and runtime overrides did not change -> 0 damage!
        if (!mutated && !animated && state.was_active == now_active) {
            continue;
        }

        std::optional<Rect> cur_bounds;
        uint64_t cur_fp = 0u;
        if (now_active) {
            cur_bounds = layer->compute_complete_bounds(frame, assets_);
            cur_fp = layer->visual_fingerprint(frame, assets_);
        }

        const bool changed = (state.was_active != now_active) ||
                             (state.prev_bounds != cur_bounds) ||
                             (state.prev_fingerprint != cur_fp);

        if (changed) {
            if (state.was_active && state.prev_bounds.has_value()) {
                ic->inval(*state.prev_bounds);
            }
            if (now_active && cur_bounds.has_value()) {
                ic->inval(*cur_bounds);
            }
        }

        state.was_active = now_active;
        state.prev_bounds = cur_bounds;
        state.prev_fingerprint = cur_fp;
        state.prev_mutation_gen = cur_mut_gen;
    }

    has_sought_once_ = true;

    if ((load_flags_ & kDisableTopLevelClipping) == 0 && width_ > 0.0f && height_ > 0.0f) {
        if (auto comp_rect = Rect::from_xywh(0.0f, 0.0f, width_, height_)) {
            ic->clamp_to(*comp_rect);
        }
    }
}

bool Animation::seek_frame(float frame, Rect* out_dirty_rect) {
    InvalidationController ic;
    seek_frame(frame, out_dirty_rect ? &ic : nullptr);
    if (out_dirty_rect) {
        if (auto b = ic.bounds()) {
            *out_dirty_rect = *b;
            return true;
        }
        *out_dirty_rect = Rect();
        return false;
    }
    return false;
}

void Animation::set_font_system(std::shared_ptr<text::FontSystem> fs) {
    assets_.font_system = std::move(fs);
    bump_mutation_gen();
}

// ── Dynamic Property Overrides ────────────────────────────────────────────

bool Animation::set_color_override(std::string_view keypath, Color color) {
    std::vector<std::string_view> parts;
    split_keypath(keypath, parts);
    if (parts.empty()) return false;

    bool matched = false;
    auto apply_to_layers = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
        for (auto& layer : list) {
            if (!layer) continue;
            bool layer_ok = match_segment(parts[0], layer->name) || parts.size() == 1;
            if (!layer_ok) continue;

            if (layer->type == LayerType::Solid) {
                if (match_segment(parts[0], layer->name)) {
                    static_cast<SolidLayer&>(*layer).color_override = color;
                    matched = true;
                }
            } else if (layer->type == LayerType::Shape) {
                auto& sl = static_cast<ShapeLayer&>(*layer);
                size_t next_depth = match_segment(parts[0], layer->name) ? 1 : 0;
                for (auto& item : sl.shapes) {
                    if (item && apply_color_to_shape_item(*item, parts, next_depth, color)) {
                        matched = true;
                    }
                }
            }
        }
    };

    apply_to_layers(layers_);
    for (auto& [id, sub] : assets_.precomps) {
        apply_to_layers(sub);
    }
    if (matched) bump_mutation_gen();
    return matched;
}

bool Animation::set_scalar_override(std::string_view keypath, float value) {
    std::vector<std::string_view> parts;
    split_keypath(keypath, parts);
    if (parts.empty()) return false;

    bool matched = false;
    auto apply_to_layers = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
        for (auto& layer : list) {
            if (!layer) continue;
            bool layer_ok = match_segment(parts[0], layer->name);
            if (layer_ok && parts.size() == 1) {
                layer->transform.opacity.set_override(value);
                matched = true;
            }
            if (layer->type == LayerType::Shape && (layer_ok || parts.size() == 1)) {
                auto& sl = static_cast<ShapeLayer&>(*layer);
                size_t next_depth = layer_ok ? 1 : 0;
                for (auto& item : sl.shapes) {
                    if (item && apply_scalar_to_shape_item(*item, parts, next_depth, value)) {
                        matched = true;
                    }
                }
            }
        }
    };

    apply_to_layers(layers_);
    for (auto& [id, sub] : assets_.precomps) {
        apply_to_layers(sub);
    }
    if (matched) bump_mutation_gen();
    return matched;
}

bool Animation::set_point_override(std::string_view keypath, Point value) {
    std::vector<std::string_view> parts;
    split_keypath(keypath, parts);
    if (parts.empty()) return false;

    bool matched = false;
    auto apply_to_layers = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
        for (auto& layer : list) {
            if (!layer) continue;
            bool layer_ok = match_segment(parts[0], layer->name);
            if (layer_ok && parts.size() == 1) {
                layer->transform.position.set_override(value);
                matched = true;
            }
            if (layer->type == LayerType::Shape && (layer_ok || parts.size() == 1)) {
                auto& sl = static_cast<ShapeLayer&>(*layer);
                size_t next_depth = layer_ok ? 1 : 0;
                for (auto& item : sl.shapes) {
                    if (item && apply_point_to_shape_item(*item, parts, next_depth, value)) {
                        matched = true;
                    }
                }
            }
        }
    };

    apply_to_layers(layers_);
    for (auto& [id, sub] : assets_.precomps) {
        apply_to_layers(sub);
    }
    if (matched) bump_mutation_gen();
    return matched;
}

bool Animation::set_text_override(std::string_view layer_name, std::string text) {
    bool matched = false;
    auto apply_to_layers = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
        for (auto& layer : list) {
            if (!layer || layer->type != LayerType::Text) continue;
            if (layer_name == "*" || layer->name == layer_name) {
                auto& tl = static_cast<TextLayer&>(*layer);
                if (tl.doc_override.has_value()) {
                    tl.doc_override->text = text;
                } else {
                    tl.text_override = text;
                }
                matched = true;
            }
        }
    };

    apply_to_layers(layers_);
    for (auto& [id, sub] : assets_.precomps) {
        apply_to_layers(sub);
    }
    if (matched) bump_mutation_gen();
    return matched;
}

bool Animation::set_slot_color(std::string_view slot_id, Color color) {
    if (slot_id.empty()) return false;
    bool matched = false;
    auto apply_to_layers = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
        for (auto& layer : list) {
            if (!layer || layer->type != LayerType::Shape) continue;
            auto& sl = static_cast<ShapeLayer&>(*layer);
            for (auto& item : sl.shapes) {
                if (item && apply_slot_color_to_shape_item(*item, slot_id, color)) {
                    matched = true;
                }
            }
        }
    };

    apply_to_layers(layers_);
    for (auto& [id, sub] : assets_.precomps) {
        apply_to_layers(sub);
    }
    if (matched) bump_mutation_gen();
    return matched;
}

void Animation::clear_overrides() {
    auto clear_layers = [&](std::vector<std::shared_ptr<LottieLayer>>& list) {
        for (auto& layer : list) {
            if (!layer) continue;
            layer->transform.anchor.clear_override();
            layer->transform.position.clear_override();
            if (layer->transform.x_pos) layer->transform.x_pos->clear_override();
            if (layer->transform.y_pos) layer->transform.y_pos->clear_override();
            layer->transform.scale.clear_override();
            layer->transform.rotation.clear_override();
            layer->transform.opacity.clear_override();
            layer->transform.skew.clear_override();
            layer->transform.skew_axis.clear_override();
            if (layer->type == LayerType::Solid) {
                static_cast<SolidLayer&>(*layer).color_override.reset();
            } else if (layer->type == LayerType::Text) {
                auto& tl = static_cast<TextLayer&>(*layer);
                tl.text_override.reset();
                tl.doc_override.reset();
            } else if (layer->type == LayerType::Shape) {
                for (auto& item : static_cast<ShapeLayer&>(*layer).shapes) {
                    if (item) clear_shape_item_overrides(*item);
                }
            }
        }
    };

    clear_layers(layers_);
    for (auto& [id, sub] : assets_.precomps) {
        clear_layers(sub);
    }
    bump_mutation_gen();
}

std::optional<std::string> Animation::hit_test(float x, float y, float frame) const {
    for (const auto& layer : layers_) {
        if (!layer || !layer->is_active(frame) || layer->is_matte_target) continue;
        std::string hit_name;
        if (layer->hit_test(x, y, frame, assets_, hit_name)) {
            return hit_name.empty() ? layer->name : hit_name;
        }
    }
    return std::nullopt;
}

const Marker* Animation::find_marker(std::string_view marker_name) const noexcept {
    for (const auto& m : markers_) {
        if (m.name == marker_name) return &m;
    }
    return nullptr;
}

void Animation::render(ICanvas& canvas, float frame) const {
    render_layer_tree_with_assets(canvas, layers_, frame, assets_, 1.0f);
}

void Animation::render(ICanvas& canvas, float frame, const Rect& dest_rect, bool preserve_aspect_ratio) const {
    if (width_ <= 0.0f || height_ <= 0.0f || dest_rect.width() <= 0.0f || dest_rect.height() <= 0.0f) {
        return;
    }

    canvas.save();
    float sx = dest_rect.width() / width_;
    float sy = dest_rect.height() / height_;

    if (preserve_aspect_ratio) {
        float s = std::min(sx, sy);
        float ox = dest_rect.x() + (dest_rect.width() - width_ * s) * 0.5f;
        float oy = dest_rect.y() + (dest_rect.height() - height_ * s) * 0.5f;
        canvas.translate(ox, oy);
        canvas.scale(s, s);
    } else {
        canvas.translate(dest_rect.x(), dest_rect.y());
        canvas.scale(sx, sy);
    }

    render(canvas, frame);
    canvas.restore();
}

namespace {

void dispatch_audio_layers(
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    float frame,
    const LottieAssetRegistry& assets,
    AudioTrack& track,
    bool is_playing,
    std::unordered_set<const AudioLayer*>& active_audio
) {
    const float fps = (assets.frame_rate > 0.0f) ? assets.frame_rate : 60.0f;
    for (const auto& layer : layers) {
        if (!layer) continue;
        if (layer->type == LayerType::Audio) {
            const auto& al = static_cast<const AudioLayer&>(*layer);
            if (!al.is_active(frame)) {
                if (active_audio.erase(&al) > 0) {
                    track.on_audio_stop(al.ref_id);
                }
                continue;
            }
            active_audio.insert(&al);
            const float local_frame = (frame - al.start_time) / al.time_stretch;
            const float local_sec = std::max(0.0f, local_frame / fps);
            const Point lv = al.level.evaluate(frame, fps);
            const float gain_l = audio_db_to_linear_gain(lv.x);
            const float gain_r = audio_db_to_linear_gain(lv.y);
            const float gain = 0.5f * (gain_l + gain_r);

            std::shared_ptr<AudioAsset> audio_asset;
            if (auto it = assets.audio_assets.find(al.ref_id); it != assets.audio_assets.end()) {
                audio_asset = it->second;
                if (audio_asset) {
                    audio_asset->seek(local_sec);
                    audio_asset->set_volume(gain);
                }
            }
            track.on_audio_frame(al.ref_id, local_sec, lv.x, lv.y, gain, is_playing, audio_asset);
        } else if (layer->type == LayerType::Precomp) {
            const auto& pl = static_cast<const PrecompLayer&>(*layer);
            if (auto it = assets.precomps.find(pl.ref_id); it != assets.precomps.end()) {
                if (!layer->is_active(frame)) {
                    for (const auto& sub_l : it->second) {
                        if (sub_l && sub_l->type == LayerType::Audio) {
                            const auto* al = static_cast<const AudioLayer*>(sub_l.get());
                            if (active_audio.erase(al) > 0) {
                                track.on_audio_stop(al->ref_id);
                            }
                        }
                    }
                    continue;
                }
                const float sub_frame = pl.has_time_remap
                    ? pl.time_remap.evaluate(frame, fps)
                    : (frame - pl.start_time) / pl.time_stretch;
                dispatch_audio_layers(it->second, sub_frame, assets, track, is_playing, active_audio);
            }
        }
    }
}

void stop_audio_layers(
    const std::vector<std::shared_ptr<LottieLayer>>& layers,
    const LottieAssetRegistry& assets,
    AudioTrack& track,
    std::unordered_set<const AudioLayer*>& active_audio
) {
    for (const auto& layer : layers) {
        if (!layer) continue;
        if (layer->type == LayerType::Audio) {
            const auto* al = static_cast<const AudioLayer*>(layer.get());
            if (active_audio.erase(al) > 0) {
                track.on_audio_stop(al->ref_id);
            }
        } else if (layer->type == LayerType::Precomp) {
            const auto& pl = static_cast<const PrecompLayer&>(*layer);
            if (auto it = assets.precomps.find(pl.ref_id); it != assets.precomps.end()) {
                stop_audio_layers(it->second, assets, track, active_audio);
            }
        }
    }
}

} // anonymous namespace

void Animation::dispatch_audio(float frame, AudioTrack& track, bool is_playing) const {
    dispatch_audio_layers(layers_, frame, assets_, track, is_playing, active_audio_layers_);
}

void Animation::stop_audio(AudioTrack& track) const {
    stop_audio_layers(layers_, assets_, track, active_audio_layers_);
    active_audio_layers_.clear();
}

} // namespace nisaba::lottie
