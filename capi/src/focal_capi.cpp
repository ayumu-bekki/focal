// C API の実装（design.md 4.3 章）。すべての関数で例外を捕まえてステータスコードに変換する。
#include "focal/focal.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/photo_delete.h"
#include "edit/editor.h"
#include "edit/preset.h"
#include "export/exporter.h"
#include "import/card_import.h"
#include "imaging/crop_tool.h"
#include "thumbs/thumbnail.h"
#include "thumbs/thumbnail_service.h"
#include "util/error.h"
#include "util/file.h"
#ifdef FOCAL_HAVE_APPLE_IMAGE_READER
#include "platform/apple_image_reader.h"
#endif

#ifdef FOCAL_HAVE_GPU
#include "gpu/metal_renderer.h"
#endif

using namespace focal;

struct fc_catalog {
    std::unique_ptr<Catalog> catalog;
};

struct fc_presets {
    PresetStore store;
    fc_presets(std::filesystem::path user, std::optional<std::filesystem::path> builtin)
        : store(std::move(user), std::move(builtin)) {}
};

struct fc_task {
    std::thread thread;
    std::atomic<bool> cancel{false};
};

struct fc_thumbnailer {
    std::unique_ptr<ThumbnailService> service;
};

struct fc_editor {
    std::unique_ptr<Editor> editor;
    std::string gpu_name;  // fc_editor_gpu_name が返す（editor と同じだけ生きる）
};

struct fc_session {
    Editor* editor;
    std::shared_ptr<EditSession> session;
};

namespace {

thread_local std::string g_last_error;

fc_status to_status(Error::Code code) {
    switch (code) {
    case Error::Code::InvalidArgument: return FC_ERR_INVALID_ARGUMENT;
    case Error::Code::Io: return FC_ERR_IO;
    case Error::Code::Unsupported: return FC_ERR_UNSUPPORTED;
    case Error::Code::Decode: return FC_ERR_DECODE;
    case Error::Code::Internal: return FC_ERR_INTERNAL;
    case Error::Code::Cancelled: return FC_ERR_CANCELLED;
    case Error::Code::Database: return FC_ERR_DATABASE;
    case Error::Code::NotFound: return FC_ERR_NOT_FOUND;
    }
    return FC_ERR_INTERNAL;
}

// 例外を境界の外に出さない
template <class F>
fc_status guard(F&& fn) noexcept {
    g_last_error.clear();
    try {
        fn();
        return FC_OK;
    } catch (const Error& e) {
        g_last_error = e.what();
        return to_status(e.code());
    } catch (const std::exception& e) {
        g_last_error = e.what();
        return FC_ERR_INTERNAL;
    } catch (...) {
        g_last_error = "unknown error";
        return FC_ERR_INTERNAL;
    }
}

void require(bool cond, const char* what) {
    if (!cond) throw Error(Error::Code::InvalidArgument, what);
}

std::span<const int64_t> id_span(const int64_t* ids, size_t count) {
    require(ids != nullptr || count == 0, "photo_ids is NULL");
    return {ids, count};
}

PhotoFilter to_filter(const fc_photo_filter* f) {
    PhotoFilter out;
    if (!f) return out;
    if (f->folder_id > 0) out.folder_id = f->folder_id;
    out.include_subfolders = f->include_subfolders != 0;
    out.min_rating = f->min_rating;
    switch (f->flag) {
    case FC_FLAG_PICKED: out.flag = FlagFilter::Picked; break;
    case FC_FLAG_REJECTED: out.flag = FlagFilter::Rejected; break;
    case FC_FLAG_UNFLAGGED: out.flag = FlagFilter::Unflagged; break;
    case FC_FLAG_NOT_REJECTED: out.flag = FlagFilter::NotRejected; break;
    default: out.flag = FlagFilter::Any; break;
    }
    if (f->tag_id > 0) out.tag_id = f->tag_id;
    if (f->date_from) out.date_from = f->date_from;
    if (f->date_to) out.date_to = f->date_to;
    out.include_unavailable = f->include_unavailable != 0;
    if (f->album_id > 0) out.album_id = f->album_id;
    out.recent_import = f->recent_import != 0;
    if (f->smart_album_id > 0) out.smart_album_id = f->smart_album_id;
    return out;
}

// 配列は C の構造体を基底に持ち、中身の文字列を自分で持つ
struct RootArray : fc_root_array {
    std::vector<RootInfo> src;
    std::vector<fc_root> v;
};

struct FolderArray : fc_folder_array {
    std::vector<FolderInfo> src;
    std::vector<fc_folder> v;
};

struct TagArray : fc_tag_array {
    std::vector<TagInfo> src;
    std::vector<fc_tag> v;
};

struct RootDetailsBox : fc_root_details {
    RootDetails src;
};

struct StringBox : fc_string {
    std::string text;
};

struct BackupArray : fc_backup_array {
    std::vector<BackupInfo> src;
    std::vector<std::string> paths;
    std::vector<fc_backup_item> v;
};

struct PresetArray : fc_preset_array {
    std::vector<PresetInfo> src;
    std::vector<fc_preset_info> v;
};

struct ImportSourceArray : fc_import_source_array {
    std::vector<ImportSource> src;
    std::vector<std::string> mount, dcim;
    std::vector<fc_import_source> v;
};

std::optional<int64_t> optional_id(int64_t id) {
    return id > 0 ? std::optional<int64_t>(id) : std::nullopt;
}

struct AlbumArray : fc_album_array {
    std::vector<AlbumInfo> src;
    std::vector<fc_album> v;
};

struct PhotoArray : fc_photo_array {
    std::vector<PhotoRecord> src;
    std::vector<std::string> companions;  // fc_photo.companions の文字列（ファイル名を '/' で区切る）
    std::vector<fc_photo> v;
};

struct IdArray : fc_id_array {
    std::vector<int64_t> v;
};

fc_tag_array* make_tags(std::vector<TagInfo> tags) {
    auto a = std::make_unique<TagArray>();
    a->src = std::move(tags);
    for (const auto& t : a->src)
        a->v.push_back({t.id, t.parent_id.value_or(0), t.name.c_str(), t.path.c_str(), t.photo_count});
    a->count = a->v.size();
    a->items = a->v.data();
    return a.release();
}

fc_photo_array* make_photos(std::vector<PhotoRecord> photos) {
    auto a = std::make_unique<PhotoArray>();
    a->src = std::move(photos);
    a->v.reserve(a->src.size());
    a->companions.reserve(a->src.size());
    for (const auto& p : a->src) {
        fc_photo c{};
        std::string joined;
        for (const auto& name : p.companions) joined += (joined.empty() ? "" : "/") + name;
        a->companions.push_back(std::move(joined));
        c.companions = a->companions.back().c_str();
        c.kind = static_cast<int32_t>(p.kind);
        c.id = p.id;
        c.folder_id = p.folder_id;
        c.file_name = p.file_name.c_str();
        c.path = p.path.c_str();
        c.status = static_cast<int32_t>(p.status);
        c.capture_time = p.capture_time ? p.capture_time->c_str() : nullptr;
        c.camera_make = p.camera_make.c_str();
        c.camera_model = p.camera_model.c_str();
        c.lens_model = p.lens_model.c_str();
        c.iso = p.iso.value_or(0);
        c.exposure_time = p.exposure_time.value_or(0);
        c.f_number = p.f_number.value_or(0);
        c.focal_length = p.focal_length.value_or(0);
        c.width = p.width;
        c.height = p.height;
        c.orientation = p.orientation;
        c.rating = p.rating;
        c.flag = p.flag;
        c.file_size = p.file_size;
        c.file_mtime = p.file_mtime;
        a->v.push_back(c);
    }
    a->count = a->v.size();
    a->items = a->v.data();
    return a.release();
}

Settings to_settings(const fc_settings& c) {
    Settings s;
    s.process_version = c.process_version;
    s.wb.mode = c.wb_mode == FC_WB_CUSTOM ? WhiteBalanceSettings::Mode::Custom : WhiteBalanceSettings::Mode::AsShot;
    s.wb.temperature = c.temperature;
    s.wb.tint = c.tint;
    s.exposure = c.exposure;
    s.contrast = c.contrast;
    s.highlights = c.highlights;
    s.shadows = c.shadows;
    s.whites = c.whites;
    s.blacks = c.blacks;
    s.brightness = c.brightness;
    s.saturation = c.saturation;
    s.vibrance = c.vibrance;
    s.clarity = c.clarity;
    s.sharpness = c.sharpness;
    s.noise_reduction = c.noise_reduction;
    s.color_noise_reduction = c.color_noise_reduction;
    s.geometry.rotate90 = c.rotate90;
    s.geometry.straighten = c.straighten;
    s.geometry.crop = {c.crop_x, c.crop_y, c.crop_w, c.crop_h};
    s.geometry.aspect = static_cast<AspectMode>(std::clamp<int32_t>(c.aspect, 0, 6));
    return s;
}

fc_settings to_c(const Settings& s) {
    fc_settings c{};
    c.process_version = s.process_version;
    c.wb_mode = s.wb.mode == WhiteBalanceSettings::Mode::Custom ? FC_WB_CUSTOM : FC_WB_AS_SHOT;
    c.temperature = s.wb.temperature;
    c.tint = s.wb.tint;
    c.exposure = s.exposure;
    c.contrast = s.contrast;
    c.highlights = s.highlights;
    c.shadows = s.shadows;
    c.whites = s.whites;
    c.blacks = s.blacks;
    c.brightness = s.brightness;
    c.saturation = s.saturation;
    c.vibrance = s.vibrance;
    c.clarity = s.clarity;
    c.sharpness = s.sharpness;
    c.noise_reduction = s.noise_reduction;
    c.color_noise_reduction = s.color_noise_reduction;
    c.rotate90 = s.geometry.rotate90;
    c.straighten = s.geometry.straighten;
    c.crop_x = s.geometry.crop.x;
    c.crop_y = s.geometry.crop.y;
    c.crop_w = s.geometry.crop.w;
    c.crop_h = s.geometry.crop.h;
    c.aspect = static_cast<int32_t>(s.geometry.aspect);
    return c;
}

} // namespace

extern "C" {

int32_t fc_api_version(void) { return FC_API_VERSION; }

const char* fc_last_error(void) { return g_last_error.c_str(); }

// ---- カタログ ----------------------------------------------------------------

fc_status fc_catalog_open(const char* path, fc_catalog** out) {
    return guard([&] {
#ifdef FOCAL_HAVE_APPLE_IMAGE_READER
        focal::platform::register_apple_image_reader();  // HEIF を ImageIO で読む（何度呼んでもよい）
#endif
        require(path && out, "path and out must not be NULL");
        *out = nullptr;
        auto c = std::make_unique<fc_catalog>();
        c->catalog = Catalog::open(utf8_to_path(path));
        *out = c.release();
    });
}

void fc_catalog_close(fc_catalog* catalog) { delete catalog; }

fc_status fc_catalog_flush(fc_catalog* catalog) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->flush();
    });
}

fc_status fc_catalog_add_root(fc_catalog* catalog, const char* dir, int64_t* out_root_id) {
    return guard([&] {
        require(catalog && dir, "catalog and dir must not be NULL");
        const int64_t id = catalog->catalog->add_root(utf8_to_path(dir));
        if (out_root_id) *out_root_id = id;
    });
}

fc_status fc_catalog_roots(fc_catalog* catalog, fc_root_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        auto a = std::make_unique<RootArray>();
        a->src = catalog->catalog->roots();
        for (const auto& r : a->src)
            a->v.push_back({r.id, r.path.c_str(), r.label.c_str(), r.volume_id.c_str(), r.volume_name.c_str(),
                            r.volume_rel_path.c_str(), r.online ? 1 : 0});
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

void fc_root_array_free(fc_root_array* array) { delete static_cast<RootArray*>(array); }

fc_status fc_catalog_folder_for_path(fc_catalog* catalog, const char* dir, int64_t* out_root_id, int64_t* out_folder_id) {
    return guard([&] {
        require(catalog && dir, "catalog and dir must not be NULL");
        const auto loc = catalog->catalog->folder_for_path(utf8_to_path(dir));
        if (!loc) throw Error(Error::Code::NotFound, "not inside a folder of the catalog");
        if (out_root_id) *out_root_id = loc->root_id;
        if (out_folder_id) *out_folder_id = loc->folder_id;
    });
}

fc_status fc_catalog_relocate_root(fc_catalog* catalog, int64_t root_id, const char* new_dir) {
    return guard([&] {
        require(catalog && new_dir, "catalog and new_dir must not be NULL");
        catalog->catalog->relocate_root(root_id, utf8_to_path(new_dir));
    });
}

fc_status fc_catalog_nested_root_count(fc_catalog* catalog, int32_t* out_count) {
    return guard([&] {
        require(catalog && out_count, "catalog and out_count must not be NULL");
        *out_count = static_cast<int32_t>(catalog->catalog->nested_roots().size());
    });
}

fc_status fc_catalog_merge_nested_roots(fc_catalog* catalog, int32_t* out_merged) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        const int n = catalog->catalog->merge_nested_roots();
        if (out_merged) *out_merged = n;
    });
}

fc_status fc_catalog_detached_summary(fc_catalog* catalog, fc_detached_summary* out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        const auto d = catalog->catalog->detached_summary();
        *out = {d.items, d.edits};
    });
}

fc_status fc_catalog_get_info(fc_catalog* catalog, fc_catalog_info* out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        const auto i = catalog->catalog->info();
        *out = {i.file_bytes,     i.schema_version,  i.roots,          i.folders,       i.photos,
                i.photos_raw,     i.photos_missing,  i.edited_photos,  i.albums,        i.tags,
                i.detached_items, i.detached_edits,  i.backup_files,   i.backup_bytes};
    });
}

fc_status fc_catalog_optimize(fc_catalog* catalog, fc_optimize_result* out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        const auto r = catalog->catalog->optimize();
        *out = {r.removed_items, r.bytes_before, r.bytes_after};
    });
}

fc_status fc_catalog_root_details(fc_catalog* catalog, int64_t root_id, fc_root_details** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = nullptr;
        auto b = std::make_unique<RootDetailsBox>();
        b->src = catalog->catalog->root_details(root_id);
        const RootDetails& d = b->src;
        static_cast<fc_root_details&>(*b) = {
            d.root.id, d.root.path.c_str(), d.root.label.c_str(), d.root.volume_id.c_str(),
            d.root.volume_name.c_str(), d.mount_point.c_str(), d.fs_type.c_str(), d.root.online ? 1 : 0, d.kind,
            d.total_bytes, d.free_bytes, d.photos, d.folders, d.missing, d.edited_photos, d.total_file_bytes,
            d.capture_from.c_str(), d.capture_to.c_str()};
        *out = b.release();
    });
}

void fc_root_details_free(fc_root_details* details) { delete static_cast<RootDetailsBox*>(details); }

fc_status fc_catalog_refresh_volumes(fc_catalog* catalog, int32_t* out_changed) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        const int n = catalog->catalog->refresh_volumes();
        if (out_changed) *out_changed = n;
    });
}

fc_status fc_catalog_remove_root(fc_catalog* catalog, int64_t root_id) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->remove_root(root_id);
    });
}

fc_status fc_catalog_set_root_label(fc_catalog* catalog, int64_t root_id, const char* label) {
    return guard([&] {
        require(catalog && label, "catalog and label must not be NULL");
        catalog->catalog->set_root_label(root_id, label);
    });
}

fc_status fc_catalog_folders(fc_catalog* catalog, int64_t root_id, fc_folder_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        auto a = std::make_unique<FolderArray>();
        a->src = catalog->catalog->folders(root_id);
        for (const auto& f : a->src)
            a->v.push_back({f.id, f.root_id, f.parent_id.value_or(0), f.rel_path.c_str(), f.photo_count});
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

void fc_folder_array_free(fc_folder_array* array) { delete static_cast<FolderArray*>(array); }

fc_status fc_catalog_tags(fc_catalog* catalog, fc_tag_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = make_tags(catalog->catalog->tags());
    });
}

fc_status fc_catalog_photo_tags(fc_catalog* catalog, int64_t photo_id, fc_tag_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = make_tags(catalog->catalog->photo_tags(photo_id));
    });
}

void fc_tag_array_free(fc_tag_array* array) { delete static_cast<TagArray*>(array); }

fc_status fc_catalog_ensure_tag(fc_catalog* catalog, const char* path, int64_t* out_tag_id) {
    return guard([&] {
        require(catalog && path, "catalog and path must not be NULL");
        const int64_t id = catalog->catalog->ensure_tag(path);
        if (out_tag_id) *out_tag_id = id;
    });
}

fc_status fc_catalog_add_tag(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int64_t tag_id) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->add_tag(id_span(photo_ids, count), tag_id);
    });
}

fc_status fc_catalog_remove_tag(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int64_t tag_id) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->remove_tag(id_span(photo_ids, count), tag_id);
    });
}

// ---- アルバム（v3.16）--------------------------------------------------------------

fc_status fc_catalog_albums(fc_catalog* catalog, fc_album_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        auto a = std::make_unique<AlbumArray>();
        a->src = catalog->catalog->albums();
        for (const auto& s : a->src)
            a->v.push_back({s.id, s.name.c_str(), s.photo_count, s.parent_id.value_or(0), static_cast<int32_t>(s.kind),
                            s.cover_photo_id.value_or(0)});
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

void fc_album_array_free(fc_album_array* array) { delete static_cast<AlbumArray*>(array); }

fc_status fc_catalog_create_album(fc_catalog* catalog, const char* name, int64_t parent_id, int64_t* out_album_id) {
    return guard([&] {
        require(catalog && name, "catalog and name must not be NULL");
        const int64_t id = catalog->catalog->create_album(name, optional_id(parent_id));
        if (out_album_id) *out_album_id = id;
    });
}

fc_status fc_catalog_create_album_folder(fc_catalog* catalog, const char* name, int64_t parent_id,
                                         int64_t* out_album_id) {
    return guard([&] {
        require(catalog && name, "catalog and name must not be NULL");
        const int64_t id = catalog->catalog->create_album_folder(name, optional_id(parent_id));
        if (out_album_id) *out_album_id = id;
    });
}

fc_status fc_catalog_create_smart_album(fc_catalog* catalog, const char* name, const char* query_json,
                                        int64_t parent_id, int64_t* out_album_id) {
    return guard([&] {
        require(catalog && name && query_json, "catalog, name and query_json must not be NULL");
        const int64_t id = catalog->catalog->create_smart_album(name, query_json, optional_id(parent_id));
        if (out_album_id) *out_album_id = id;
    });
}

fc_status fc_catalog_set_smart_query(fc_catalog* catalog, int64_t album_id, const char* query_json) {
    return guard([&] {
        require(catalog && query_json, "catalog and query_json must not be NULL");
        catalog->catalog->set_smart_query(album_id, query_json);
    });
}

fc_status fc_catalog_smart_query(fc_catalog* catalog, int64_t album_id, fc_string** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = nullptr;
        const auto q = catalog->catalog->smart_query(album_id);
        if (!q) throw Error(Error::Code::NotFound, "not a smart album");
        auto b = std::make_unique<StringBox>();
        b->text = *q;
        b->value = b->text.c_str();
        *out = b.release();
    });
}

fc_status fc_presets_open(const char* user_dir, const char* builtin_dir, fc_presets** out) {
    return guard([&] {
        require(user_dir && out, "user_dir and out must not be NULL");
        *out = nullptr;
        std::optional<std::filesystem::path> builtin;
        if (builtin_dir && *builtin_dir) builtin = utf8_to_path(builtin_dir);
        *out = new fc_presets(utf8_to_path(user_dir), std::move(builtin));
    });
}

void fc_presets_close(fc_presets* presets) { delete presets; }

fc_status fc_presets_list(fc_presets* presets, fc_preset_array** out) {
    return guard([&] {
        require(presets && out, "presets and out must not be NULL");
        *out = nullptr;
        auto a = std::make_unique<PresetArray>();
        a->src = presets->store.list();
        for (const auto& p : a->src) a->v.push_back({p.id.c_str(), p.name.c_str(), p.builtin ? 1 : 0});
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

void fc_preset_array_free(fc_preset_array* array) { delete static_cast<PresetArray*>(array); }

fc_status fc_presets_save(fc_presets* presets, const char* name, const fc_settings* settings, fc_string** out_id) {
    return guard([&] {
        require(presets && name && settings && out_id, "invalid arguments");
        *out_id = nullptr;
        auto b = std::make_unique<StringBox>();
        b->text = presets->store.save(name, to_settings(*settings));
        b->value = b->text.c_str();
        *out_id = b.release();
    });
}

fc_status fc_presets_delete(fc_presets* presets, const char* id) {
    return guard([&] {
        require(presets && id, "presets and id must not be NULL");
        presets->store.remove(id);
    });
}

fc_status fc_presets_rename(fc_presets* presets, const char* id, const char* name) {
    return guard([&] {
        require(presets && id && name, "invalid arguments");
        presets->store.rename(id, name);
    });
}

fc_status fc_presets_match(fc_presets* presets, const fc_settings* settings, fc_string** out_id) {
    return guard([&] {
        require(presets && settings && out_id, "invalid arguments");
        *out_id = nullptr;
        if (const auto id = presets->store.find_match(to_settings(*settings))) {
            auto b = std::make_unique<StringBox>();
            b->text = *id;
            b->value = b->text.c_str();
            *out_id = b.release();
        }
    });
}

fc_status fc_presets_apply(fc_presets* presets, const char* id, const fc_settings* base, fc_settings* out) {
    return guard([&] {
        require(presets && id && base && out, "invalid arguments");
        *out = to_c(apply_preset(to_settings(*base), presets->store.load(id)));
    });
}

fc_status fc_catalog_apply_preset(fc_catalog* catalog, fc_presets* presets, const char* id, const int64_t* photo_ids,
                                  size_t count) {
    return guard([&] {
        require(catalog && presets && id && (photo_ids || count == 0), "invalid arguments");
        catalog->catalog->apply_preset({photo_ids, count}, presets->store.load(id));
    });
}

void fc_string_free(fc_string* string) { delete static_cast<StringBox*>(string); }

fc_status fc_catalog_move_album(fc_catalog* catalog, int64_t album_id, int64_t parent_id) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->move_album(album_id, optional_id(parent_id));
    });
}

fc_status fc_catalog_move_album_order(fc_catalog* catalog, int64_t album_id, int32_t delta) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->move_album_order(album_id, delta);
    });
}

fc_status fc_catalog_set_album_cover(fc_catalog* catalog, int64_t album_id, int64_t photo_id) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->set_album_cover(album_id, optional_id(photo_id));
    });
}

fc_status fc_catalog_rename_album(fc_catalog* catalog, int64_t album_id, const char* name) {
    return guard([&] {
        require(catalog && name, "catalog and name must not be NULL");
        catalog->catalog->rename_album(album_id, name);
    });
}

fc_status fc_catalog_delete_album(fc_catalog* catalog, int64_t album_id) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->delete_album(album_id);
    });
}

fc_status fc_catalog_add_to_album(fc_catalog* catalog, int64_t album_id, const int64_t* photo_ids, size_t count) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->add_to_album(album_id, id_span(photo_ids, count));
    });
}

fc_status fc_catalog_remove_from_album(fc_catalog* catalog, int64_t album_id, const int64_t* photo_ids, size_t count) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->remove_from_album(album_id, id_span(photo_ids, count));
    });
}

// ---- 写真 ----------------------------------------------------------------------

void fc_photo_filter_init(fc_photo_filter* filter) {
    if (!filter) return;
    *filter = fc_photo_filter{};
    filter->include_subfolders = 1;
    filter->include_unavailable = 1;
}

fc_status fc_catalog_count(fc_catalog* catalog, const fc_photo_filter* filter, int64_t* out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = catalog->catalog->count(to_filter(filter));
    });
}

fc_status fc_catalog_query(fc_catalog* catalog, const fc_photo_filter* filter, int64_t offset, int64_t limit,
                           fc_photo_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = make_photos(catalog->catalog->query(to_filter(filter), offset, limit));
    });
}

fc_status fc_catalog_query_ids(fc_catalog* catalog, const fc_photo_filter* filter, fc_id_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        auto a = std::make_unique<IdArray>();
        a->v = catalog->catalog->query_ids(to_filter(filter));
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

fc_status fc_catalog_photos_by_ids(fc_catalog* catalog, const int64_t* ids, size_t count, fc_photo_array** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = make_photos(catalog->catalog->photos_by_ids(id_span(ids, count)));
    });
}

void fc_photo_array_free(fc_photo_array* array) { delete static_cast<PhotoArray*>(array); }
void fc_id_array_free(fc_id_array* array) { delete static_cast<IdArray*>(array); }

fc_status fc_catalog_set_rating(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int32_t rating) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->set_rating(id_span(photo_ids, count), rating);
    });
}

fc_status fc_catalog_set_flag(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int32_t flag) {
    return guard([&] {
        require(catalog, "catalog must not be NULL");
        catalog->catalog->set_flag(id_span(photo_ids, count), flag);
    });
}

// ---- スキャン ------------------------------------------------------------------

fc_status fc_catalog_scan_async(fc_catalog* catalog, int64_t root_id, const char* thumbnail_cache_dir,
                                fc_scan_progress_fn progress, fc_scan_done_fn done, void* user, fc_task** out_task) {
    return guard([&] {
        require(catalog && done && out_task, "catalog, done and out_task must not be NULL");
        *out_task = nullptr;
        auto task = std::make_unique<fc_task>();
        std::optional<std::string> cache_dir;
        if (thumbnail_cache_dir) cache_dir = thumbnail_cache_dir;
        fc_task* t = task.get();
        Catalog* c = catalog->catalog.get();
        task->thread = std::thread([t, c, root_id, cache_dir, progress, done, user] {
            fc_scan_stats stats{};
            fc_status status = FC_OK;
            std::string message;
            try {
                std::optional<ThumbnailCache> cache;
                if (cache_dir) cache.emplace(utf8_to_path(*cache_dir));
                ScanOptions opt;
                opt.thumbnails = cache ? &*cache : nullptr;
                opt.cancel = &t->cancel;
                if (progress) opt.progress = [progress, user](int d, int n) { progress(user, d, n); };
                const ScanStats s = c->scan_root(root_id, opt);
                stats = {s.added,         s.updated,    s.unchanged,    s.missing,   s.restored,
                         s.renamed,       s.unsupported, s.relinked,    s.inherited, s.restored_data, s.folders_added, s.thumbnails,
                         s.thumbnail_failures};
            } catch (const Error& e) {
                status = to_status(e.code());
                message = e.what();
            } catch (const std::exception& e) {
                status = FC_ERR_INTERNAL;
                message = e.what();
            }
            done(user, status, &stats, message.c_str());
        });
        *out_task = task.release();
    });
}

// ---- 定期バックアップ（v3.24）------------------------------------------------------

fc_status fc_catalog_backup_start(fc_catalog* catalog, const fc_backup_options* options, fc_backup_progress_fn progress,
                                  fc_backup_done_fn done, void* user, fc_task** out_task) {
    return guard([&] {
        require(catalog && options && options->dest_dir && done && out_task,
                "catalog, options, done and out_task must not be NULL");
        *out_task = nullptr;
        auto task = std::make_unique<fc_task>();
        fc_task* t = task.get();
        Catalog* c = catalog->catalog.get();
        const std::string dest = options->dest_dir;
        const bool check = options->check_integrity != 0, allow = options->allow_damaged != 0;
        task->thread = std::thread([t, c, dest, check, allow, progress, done, user] {
            fc_backup_result out{};
            fc_status status = FC_OK;
            std::string message, path;
            try {
                BackupOptions opt;
                opt.check_integrity = check;
                opt.allow_damaged = allow;
                opt.cancel = &t->cancel;
                if (progress) opt.progress = [progress, user](double f) { progress(user, f); };
                const BackupResult r = c->backup_to(utf8_to_path(dest), opt);
                out.skipped_damaged = r.skipped_damaged ? 1 : 0;
                out.bytes = r.bytes;
                path = r.path.empty() ? std::string() : path_to_utf8(r.path);
                message = r.integrity_message;
            } catch (const Error& e) {
                status = to_status(e.code());
                message = e.what();
            } catch (const std::exception& e) {
                status = FC_ERR_INTERNAL;
                message = e.what();
            }
            done(user, status, &out, path.c_str(), message.c_str());
        });
        *out_task = task.release();
    });
}

fc_status fc_catalog_name(fc_catalog* catalog, fc_string** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        auto b = std::make_unique<StringBox>();
        b->text = catalog->catalog->name();
        b->value = b->text.c_str();
        *out = b.release();
    });
}

fc_status fc_catalog_last_backup_at(fc_catalog* catalog, fc_string** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = nullptr;
        if (const auto last = catalog->catalog->last_backup_at()) {
            auto b = std::make_unique<StringBox>();
            b->text = *last;
            b->value = b->text.c_str();
            *out = b.release();
        }
    });
}

fc_status fc_catalog_backup_due(fc_catalog* catalog, int32_t interval_days, int32_t* out_due) {
    return guard([&] {
        require(catalog && out_due, "catalog and out_due must not be NULL");
        *out_due = catalog->catalog->backup_due(interval_days) ? 1 : 0;
    });
}

fc_status fc_catalog_get_preference(fc_catalog* catalog, const char* name, fc_string** out) {
    return guard([&] {
        require(catalog && name && out, "catalog, name and out must not be NULL");
        *out = nullptr;
        if (const auto v = catalog->catalog->preference(name)) {
            auto b = std::make_unique<StringBox>();
            b->text = *v;
            b->value = b->text.c_str();
            *out = b.release();
        }
    });
}

fc_status fc_catalog_set_preference(fc_catalog* catalog, const char* name, const char* value) {
    return guard([&] {
        require(catalog && name, "catalog and name must not be NULL");
        catalog->catalog->set_preference(name, value ? std::optional<std::string>(value) : std::nullopt);
    });
}

fc_status fc_backups_list(const char* dir, const char* catalog_name, fc_backup_array** out) {
    return guard([&] {
        require(dir && catalog_name && out, "dir, catalog_name and out must not be NULL");
        *out = nullptr;
        auto a = std::make_unique<BackupArray>();
        a->src = list_backups(utf8_to_path(dir), catalog_name);
        for (const auto& b : a->src) a->paths.push_back(path_to_utf8(b.path));
        for (size_t i = 0; i < a->src.size(); ++i)
            a->v.push_back({a->paths[i].c_str(), a->src[i].created.c_str(), a->src[i].bytes});
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

void fc_backup_array_free(fc_backup_array* array) { delete static_cast<BackupArray*>(array); }

fc_status fc_backups_prune(const char* dir, const char* catalog_name, int32_t keep, int32_t* out_removed) {
    return guard([&] {
        require(dir && catalog_name, "dir and catalog_name must not be NULL");
        const int n = prune_backups(utf8_to_path(dir), catalog_name, keep);
        if (out_removed) *out_removed = n;
    });
}

// ---- 写真の削除（v3.19）-----------------------------------------------------------

fc_status fc_catalog_plan_delete(fc_catalog* catalog, const int64_t* photo_ids, size_t count, fc_delete_plan* out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        const DeletePlan p = plan_delete(*catalog->catalog, id_span(photo_ids, count));
        *out = {static_cast<int32_t>(p.items.size()), p.files, p.network_photos, p.missing_photos};
    });
}

fc_status fc_catalog_delete_photos(fc_catalog* catalog, const int64_t* photo_ids, size_t count, fc_trash_fn trash,
                                   void* user, fc_delete_result* out, fc_string** errors) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        if (errors) *errors = nullptr;
        const DeletePlan plan = plan_delete(*catalog->catalog, id_span(photo_ids, count));
        TrashFn fn;
        if (trash) fn = [trash, user](const std::filesystem::path& p) { return trash(user, path_to_utf8(p).c_str()) == 0; };
        const DeleteResult r = delete_photos(*catalog->catalog, plan, fn);
        *out = {r.photos_deleted, r.photos_failed, r.files_trashed, r.files_removed, r.files_failed};
        if (errors) {
            auto b = std::make_unique<StringBox>();
            for (const auto& e : r.errors) b->text += (b->text.empty() ? "" : "\n") + e;
            b->value = b->text.c_str();
            *errors = b.release();
        }
    });
}

// ---- カードの取り込み（v3.19）---------------------------------------------------

fc_status fc_import_sources(fc_import_source_array** out) {
    return guard([&] {
        require(out, "out must not be NULL");
        *out = nullptr;
        auto a = std::make_unique<ImportSourceArray>();
        a->src = detect_import_sources();
        for (const auto& s : a->src) {
            a->mount.push_back(path_to_utf8(s.volume.mount_point));
            a->dcim.push_back(path_to_utf8(s.dcim));
        }
        for (size_t i = 0; i < a->src.size(); ++i)
            a->v.push_back({a->src[i].volume.id.c_str(), a->src[i].volume.name.c_str(), a->mount[i].c_str(),
                            a->dcim[i].c_str(), a->src[i].volume.removable ? 1 : 0});
        a->count = a->v.size();
        a->items = a->v.data();
        *out = a.release();
    });
}

void fc_import_source_array_free(fc_import_source_array* array) { delete static_cast<ImportSourceArray*>(array); }

int64_t fc_free_space(const char* path) {
    int64_t n = -1;
    guard([&] {
        require(path, "path must not be NULL");
        n = free_space_bytes(utf8_to_path(path));
    });
    return n;
}

fc_status fc_card_summarize(const char* source, fc_card_summary* out) {
    return guard([&] {
        require(source && out, "source and out must not be NULL");
        const CardSummary s = summarize_card(utf8_to_path(source));
        *out = {s.shots, s.files, s.bytes};
    });
}

fc_status fc_card_import_start(fc_catalog* catalog, const fc_card_import_options* options,
                               fc_card_progress_fn progress, fc_card_done_fn done, void* user, fc_task** out_task) {
    return guard([&] {
        require(catalog && options && options->source && options->dest_root && done && out_task,
                "catalog, options, done and out_task must not be NULL");
        require(options->tag_ids != nullptr || options->tag_count == 0, "tag_ids is NULL");
        *out_task = nullptr;
        CardImportOptions opt;
        opt.source = utf8_to_path(options->source);
        opt.dest_root = utf8_to_path(options->dest_root);
        opt.verify = options->verify != 0;
        opt.dry_run = options->dry_run != 0;
        opt.album_id = optional_id(options->album_id);
        opt.tag_ids.assign(options->tag_ids, options->tag_ids + options->tag_count);
        if (options->preset_id && *options->preset_id) {
            require(options->presets, "presets must not be NULL when preset_id is given");
            opt.preset = options->presets->store.load(options->preset_id);  // 見つからなければ、取り込みを始める前に失敗する
        }
        std::optional<std::string> cache_dir;
        if (options->thumbnail_cache_dir) cache_dir = options->thumbnail_cache_dir;

        auto task = std::make_unique<fc_task>();
        fc_task* t = task.get();
        Catalog* c = catalog->catalog.get();
        task->thread = std::thread([t, c, opt = std::move(opt), cache_dir, progress, done, user]() mutable {
            fc_card_import_result out{};
            fc_status status = FC_OK;
            std::string message;
            try {
                std::optional<ThumbnailCache> cache;
                if (cache_dir) cache.emplace(utf8_to_path(*cache_dir));
                opt.thumbnails = cache ? &*cache : nullptr;
                opt.cancel = &t->cancel;
                if (progress)
                    opt.progress = [progress, user](const CardImportProgress& p) {
                        progress(user, static_cast<int32_t>(p.phase), p.done, p.total, p.bytes_done, p.bytes_total,
                                 p.current.c_str());
                    };
                const CardImportResult r = import_from_card(*c, opt);
                out = {r.shots,           r.imported,           r.skipped_duplicates, r.failed,
                       r.estimated_dates, r.files_copied,       r.bytes_copied,       r.cancelled ? 1 : 0,
                       r.root_id.value_or(0), r.scan.added, r.bytes_needed, r.space_available};
                for (const auto& e : r.errors) message += (message.empty() ? "" : "\n") + e;
                if (r.cancelled) status = FC_ERR_CANCELLED;
            } catch (const Error& e) {
                status = to_status(e.code());
                message = e.what();
            } catch (const std::exception& e) {
                status = FC_ERR_INTERNAL;
                message = e.what();
            }
            done(user, status, &out, message.c_str());
        });
        *out_task = task.release();
    });
}

fc_status fc_export_start(fc_catalog* catalog, const int64_t* photo_ids, size_t count, const fc_export_options* options,
                          fc_export_progress_fn progress, fc_export_done_fn done, void* user, fc_task** out_task) {
    return guard([&] {
        require(catalog && options && options->dest_dir && done && out_task, "invalid arguments");
        *out_task = nullptr;
        ExportOptions opt;
        opt.format = options->format == FC_EXPORT_TIFF16 ? ExportOptions::Format::Tiff16 : ExportOptions::Format::Jpeg;
        opt.quality = options->quality > 0 ? options->quality : 92;
        opt.long_edge = std::max(0, options->long_edge);
        opt.dest_dir = utf8_to_path(options->dest_dir);
        std::vector<int64_t> ids(id_span(photo_ids, count).begin(), id_span(photo_ids, count).end());

        auto task = std::make_unique<fc_task>();
        fc_task* t = task.get();
        Catalog* c = catalog->catalog.get();
        task->thread = std::thread([t, c, ids = std::move(ids), opt, progress, done, user] {
            bool completed = false;
            try {
                completed = export_photos(
                    *c, ids, opt,
                    [&](int d, int n, const ExportItemResult& r) {
                        if (!progress) return;
                        const std::string text = r.ok ? path_to_utf8(r.output) : r.error;
                        progress(user, d, n, r.photo_id, r.ok ? 1 : 0, text.c_str());
                    },
                    &t->cancel);
            } catch (const std::exception&) {
            }
            done(user, completed ? FC_OK : FC_ERR_CANCELLED);
        });
        *out_task = task.release();
    });
}

void fc_task_cancel(fc_task* task) {
    if (task) task->cancel = true;
}

void fc_task_release(fc_task* task) {
    if (!task) return;
    if (task->thread.joinable()) task->thread.join();
    delete task;
}

// ---- サムネイル ----------------------------------------------------------------

fc_status fc_thumbnailer_create(fc_catalog* catalog, const char* cache_dir, int32_t threads, fc_thumbnailer** out) {
    return guard([&] {
        require(catalog && cache_dir && out, "catalog, cache_dir and out must not be NULL");
        *out = nullptr;
        auto t = std::make_unique<fc_thumbnailer>();
        t->service = std::make_unique<ThumbnailService>(*catalog->catalog, utf8_to_path(cache_dir),
                                                        threads > 0 ? static_cast<unsigned>(threads) : 0u);
        *out = t.release();
    });
}

void fc_thumbnailer_destroy(fc_thumbnailer* thumbnailer) { delete thumbnailer; }

uint64_t fc_thumbnailer_request(fc_thumbnailer* thumbnailer, int64_t photo_id, fc_thumbnail_fn callback, void* user) {
    uint64_t id = 0;
    const fc_status st = guard([&] {
        require(thumbnailer && callback, "thumbnailer and callback must not be NULL");
        id = thumbnailer->service->request(
            photo_id, [callback, user](uint64_t rid, ThumbnailService::Result r, const std::string& path) {
                switch (r) {
                case ThumbnailService::Result::Ok: callback(user, rid, FC_OK, path.c_str()); break;
                case ThumbnailService::Result::Cancelled: callback(user, rid, FC_ERR_CANCELLED, nullptr); break;
                case ThumbnailService::Result::Failed: callback(user, rid, FC_ERR_DECODE, nullptr); break;
                }
            });
    });
    return st == FC_OK ? id : 0;
}

void fc_thumbnailer_cancel(fc_thumbnailer* thumbnailer, uint64_t request_id) {
    if (!thumbnailer) return;
    guard([&] { thumbnailer->service->cancel(request_id); });
}

// ---- 現像 ------------------------------------------------------------------------

void fc_settings_init(fc_settings* settings) {
    if (settings) *settings = to_c(Settings{});
}

fc_status fc_editor_create(fc_catalog* catalog, fc_editor** out) {
    return guard([&] {
        require(catalog && out, "catalog and out must not be NULL");
        *out = nullptr;
        auto e = std::make_unique<fc_editor>();
        e->editor = std::make_unique<Editor>(*catalog->catalog);
#ifdef FOCAL_HAVE_GPU
        // 表示は GPU で描く（v3.14）。使える GPU がない、または FOCAL_GPU=0 なら CPU
        e->editor->set_gpu_renderer(focal::gpu::create_metal_renderer());
#endif
        e->gpu_name = e->editor->gpu_name();
        *out = e.release();
    });
}

void fc_editor_destroy(fc_editor* editor) { delete editor; }

const char* fc_editor_gpu_name(fc_editor* editor) { return editor ? editor->gpu_name.c_str() : ""; }

fc_status fc_editor_open(fc_editor* editor, int64_t photo_id, int32_t preview_long_edge, int32_t proxy_long_edge,
                         fc_session_fn callback, void* user, fc_session** out) {
    return guard([&] {
        require(editor && out, "editor and out must not be NULL");
        *out = nullptr;
        EditSession::EventCallback cb;
        if (callback) {
            cb = [callback, user](EditSession::Event ev, const std::string& msg) {
                const int32_t code = ev == EditSession::Event::Preview ? FC_EVENT_PREVIEW
                                     : ev == EditSession::Event::Ready ? FC_EVENT_READY
                                                                       : FC_EVENT_FAILED;
                callback(user, code, msg.c_str());
            };
        }
        auto s = std::make_unique<fc_session>();
        s->editor = editor->editor.get();
        s->session = editor->editor->open(photo_id, preview_long_edge, proxy_long_edge, std::move(cb));
        *out = s.release();
    });
}

fc_status fc_editor_set_thumbnail_cache(fc_editor* editor, const char* cache_dir, fc_thumbnail_updated_fn callback,
                                        void* user) {
    return guard([&] {
        require(editor && cache_dir, "editor and cache_dir must not be NULL");
        std::function<void(int64_t)> cb;
        if (callback) cb = [callback, user](int64_t id) { callback(user, id); };
        editor->editor->set_thumbnail_cache(utf8_to_path(cache_dir), std::move(cb));
    });
}

fc_status fc_editor_set_preview_cache(fc_editor* editor, const char* cache_dir, uint64_t limit_bytes) {
    return guard([&] {
        require(editor && cache_dir, "editor and cache_dir must not be NULL");
        editor->editor->set_preview_cache(utf8_to_path(cache_dir), limit_bytes);
    });
}

fc_status fc_editor_set_preview_cache_limit(fc_editor* editor, uint64_t limit_bytes) {
    return guard([&] {
        require(editor, "editor must not be NULL");
        editor->editor->set_preview_cache_limit(limit_bytes);
    });
}

fc_status fc_editor_preview_cache_usage(fc_editor* editor, uint64_t* out_bytes) {
    return guard([&] {
        require(editor && out_bytes, "editor and out_bytes must not be NULL");
        *out_bytes = editor->editor->preview_cache_usage();
    });
}

fc_status fc_editor_clear_preview_cache(fc_editor* editor) {
    return guard([&] {
        require(editor, "editor must not be NULL");
        editor->editor->clear_preview_cache();
    });
}

void fc_editor_prefetch(fc_editor* editor, int64_t photo_id) {
    if (!editor) return;
    guard([&] { editor->editor->prefetch(photo_id); });
}

void fc_session_close(fc_session* session) {
    if (!session) return;
    guard([&] { session->editor->close(session->session); });
    delete session;
}

fc_status fc_session_get_info(fc_session* session, fc_session_info* out) {
    return guard([&] {
        require(session && out, "session and out must not be NULL");
        const SessionInfo i = session->session->info();
        *out = {};
        out->stage = static_cast<int32_t>(i.stage);
        out->oriented_width = i.oriented_width;
        out->oriented_height = i.oriented_height;
        out->output_width = i.output_width;
        out->output_height = i.output_height;
        out->canvas_width = i.canvas_width;
        out->canvas_height = i.canvas_height;
        out->as_shot_temperature = i.as_shot_temperature;
        out->as_shot_tint = i.as_shot_tint;
        out->preview_width = i.preview_width;
        out->preview_height = i.preview_height;
        out->preview_display_p3 = i.preview_display_p3 ? 1 : 0;
    });
}

fc_status fc_session_copy_preview(fc_session* session, uint8_t* dst, size_t stride, size_t capacity) {
    return guard([&] {
        require(session && dst, "session and dst must not be NULL");
        if (!session->session->copy_preview(dst, stride, capacity))
            throw Error(Error::Code::InvalidArgument, "no preview or buffer too small");
    });
}

fc_status fc_session_get_settings(fc_session* session, fc_settings* out) {
    return guard([&] {
        require(session && out, "session and out must not be NULL");
        *out = to_c(session->session->settings());
    });
}

fc_status fc_session_save(fc_session* session) {
    return guard([&] {
        require(session, "session must not be NULL");
        session->editor->save_now(session->session);
    });
}

fc_status fc_session_set_settings(fc_session* session, const fc_settings* settings) {
    return guard([&] {
        require(session && settings, "session and settings must not be NULL");
        session->session->set_settings(to_settings(*settings));
    });
}

void fc_session_begin_change(fc_session* session) {
    if (session) guard([&] { session->session->begin_change(); });
}

void fc_session_end_change(fc_session* session) {
    if (session) guard([&] { session->session->end_change(); });
}

int32_t fc_session_undo(fc_session* session) {
    bool r = false;
    if (session) guard([&] { r = session->session->undo(); });
    return r ? 1 : 0;
}

int32_t fc_session_redo(fc_session* session) {
    bool r = false;
    if (session) guard([&] { r = session->session->redo(); });
    return r ? 1 : 0;
}

int32_t fc_session_can_undo(fc_session* session) {
    bool r = false;
    if (session) guard([&] { r = session->session->can_undo(); });
    return r ? 1 : 0;
}

int32_t fc_session_can_redo(fc_session* session) {
    bool r = false;
    if (session) guard([&] { r = session->session->can_redo(); });
    return r ? 1 : 0;
}

void fc_session_set_proxy_long_edge(fc_session* session, int32_t long_edge) {
    if (session) guard([&] { session->session->set_proxy_long_edge(long_edge); });
}

uint64_t fc_session_render(fc_session* session, const fc_render_request* request, fc_render_fn callback, void* user) {
    uint64_t gen = 0;
    guard([&] {
        require(session && request && callback, "session, request and callback must not be NULL");
        RenderRequest rq;
        rq.mode = request->mode == FC_RENDER_REGION ? RenderRequest::Mode::Region : RenderRequest::Mode::Fit;
        rq.max_width = request->max_width;
        rq.max_height = request->max_height;
        rq.region_x = request->region_x;
        rq.region_y = request->region_y;
        rq.region_width = request->region_width;
        rq.region_height = request->region_height;
        rq.ignore_crop = request->ignore_crop != 0;
        rq.layout = request->pixel_format == FC_PIXEL_BGRX8 ? PixelLayout::Bgrx8 : PixelLayout::Rgb8;
        rq.buffer = request->buffer;
        rq.stride = request->stride;
        rq.capacity = request->capacity;
        gen = session->editor->render(session->session, rq, [callback, user](RenderStatus st, const RenderResult& r) {
            switch (st) {
            case RenderStatus::Ok: {
                fc_render_result c{};
                c.width = r.width;
                c.height = r.height;
                c.scale = r.scale;
                c.region_x = r.region_x;
                c.region_y = r.region_y;
                for (int ch = 0; ch < 3; ++ch)
                    for (int i = 0; i < 256; ++i) c.histogram[ch][i] = r.histogram[ch][i];
                callback(user, FC_OK, &c);
                break;
            }
            case RenderStatus::Cancelled: callback(user, FC_ERR_CANCELLED, nullptr); break;
            case RenderStatus::NotReady: callback(user, FC_ERR_NOT_READY, nullptr); break;
            case RenderStatus::Failed: callback(user, FC_ERR_INTERNAL, nullptr); break;
            }
        });
    });
    return gen;
}

// ---- クロップモードの操作 ------------------------------------------------------

fc_status fc_crop_drag(const fc_settings* start, int32_t canvas_width, int32_t canvas_height, int32_t handle,
                       double dx, double dy, fc_settings* out) {
    return guard([&] {
        require(start && out && canvas_width > 0 && canvas_height > 0, "invalid arguments");
        require(handle >= FC_HANDLE_MOVE && handle <= FC_HANDLE_BOTTOM_RIGHT, "invalid handle");
        const Settings s = to_settings(*start);
        const double aspect = crop_aspect(s.geometry.aspect, canvas_width, canvas_height);
        const CropRect r = drag_crop(s.geometry.crop, static_cast<CropHandle>(handle), dx, dy, aspect,
                                     s.geometry.straighten, canvas_width, canvas_height);
        *out = *start;
        out->crop_x = r.x;
        out->crop_y = r.y;
        out->crop_w = r.w;
        out->crop_h = r.h;
    });
}

fc_status fc_crop_fit(fc_settings* settings, int32_t canvas_width, int32_t canvas_height) {
    return guard([&] {
        require(settings && canvas_width > 0 && canvas_height > 0, "invalid arguments");
        const Settings s = to_settings(*settings);
        const CropRect& c = s.geometry.crop;
        double aspect = crop_aspect(s.geometry.aspect, canvas_width, canvas_height);
        if (aspect <= 0) aspect = c.w * canvas_width / (c.h * canvas_height);  // 自由比率は今の枠の比
        const CropRect r = max_crop(aspect, s.geometry.straighten, canvas_width, canvas_height,
                                    {c.x + c.w / 2, c.y + c.h / 2});
        settings->crop_x = r.x;
        settings->crop_y = r.y;
        settings->crop_w = r.w;
        settings->crop_h = r.h;
    });
}

fc_status fc_crop_rotate(fc_settings* settings, int32_t steps) {
    return guard([&] {
        require(settings, "settings must not be NULL");
        const CropRect r =
            rotate_crop({settings->crop_x, settings->crop_y, settings->crop_w, settings->crop_h}, steps);
        settings->rotate90 = (((settings->rotate90 + steps) % 4) + 4) % 4;
        settings->crop_x = r.x;
        settings->crop_y = r.y;
        settings->crop_w = r.w;
        settings->crop_h = r.h;
    });
}

double fc_crop_straighten_from_line(double x0, double y0, double x1, double y1, double current) {
    return straighten_from_line({x0, y0}, {x1, y1}, current);
}

} // extern "C"
