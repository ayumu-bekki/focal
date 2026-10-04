#include "export/exporter.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <system_error>

#include "catalog/catalog.h"
#include "edit/settings.h"
#include "imaging/image_io.h"
#include "imaging/output_transform.h"
#include "imaging/raw_decoder.h"
#include "imaging/renderer.h"
#include "util/error.h"
#include "util/file.h"

namespace focal {

namespace fs = std::filesystem;

namespace {

std::string stem_of(const std::string& file_name) {
    const auto dot = file_name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? file_name : file_name.substr(0, dot);
}

Settings saved_settings(Catalog& catalog, int64_t id) {
    if (auto json = catalog.edit_json(id)) {
        try {
            return settings_from_json(*json);
        } catch (const Error&) {
        }
    }
    return {};
}

} // namespace

fs::path reserve_output_path(const fs::path& dir, const std::string& stem, const std::string& ext) {
    for (int n = 0; n < 100000; ++n) {
        const std::string name = stem + (n == 0 ? "" : "_" + std::to_string(n)) + "." + ext;
        const fs::path p = dir / utf8_to_path(name);
        // "x": 既にあれば失敗する（C11）。これで名前を原子的に確保する
        if (FILE* f = open_file(p, "wbx")) {
            std::fclose(f);
            return p;
        }
        if (errno != EEXIST) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
                throw Error(Error::Code::Io, "export folder is not available: " + path_to_utf8(dir));
            if (!fs::exists(p, ec)) throw Error(Error::Code::Io, "cannot create " + path_to_utf8(p));
        }
    }
    throw Error(Error::Code::Io, "too many files named " + stem);
}

ExportItemResult export_photo(Catalog& catalog, int64_t photo_id, const ExportOptions& opt,
                              const std::atomic<bool>* cancel) {
    ExportItemResult r;
    r.photo_id = photo_id;
    fs::path reserved;
    try {
        const auto photo = catalog.photo(photo_id);
        if (!photo) throw Error(Error::Code::NotFound, "unknown photo " + std::to_string(photo_id));
        const auto disk = catalog.photo_disk_path(photo_id);
        if (!disk) throw Error(Error::Code::NotFound, "file not found: " + photo->path);
        const Settings settings = saved_settings(catalog, photo_id);

        const DecodedRaw raw = decode_raw(*disk, {.cancel = cancel});
        if (cancel && cancel->load()) throw Error(Error::Code::Cancelled, "export cancelled");
        const ImageF linear = render_for_export(raw, settings, opt.long_edge);

        // 撮影情報（カタログにある値）を書き出しファイルに入れる。撮影日時などで並べられるように（v3.19）
        ExifInfo exif;
        exif.capture_time = photo->capture_time.value_or("");
        exif.make = photo->camera_make;
        exif.model = photo->camera_model;
        exif.lens = photo->lens_model;
        if (photo->iso) exif.iso = static_cast<int>(*photo->iso);
        exif.exposure_time = photo->exposure_time;
        exif.f_number = photo->f_number;
        exif.focal_length = photo->focal_length;

        const bool jpeg = opt.format == ExportOptions::Format::Jpeg;
        reserved = reserve_output_path(opt.dest_dir, stem_of(photo->file_name), jpeg ? "jpg" : "tif");
        if (jpeg) {
            const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
            write_jpeg(reserved, encode_u8(linear, xf), std::clamp(opt.quality, 1, 100), xf.icc_profile(), &exif);
        } else {
            const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U16);
            write_tiff(reserved, encode_u16(linear, xf), xf.icc_profile(), TiffCompression::Deflate, &exif);
        }
        r.ok = true;
        r.output = reserved;
    } catch (const std::exception& e) {
        r.error = e.what();
        if (!reserved.empty()) {
            std::error_code ec;
            fs::remove(reserved, ec);  // 書きかけのファイルを残さない
        }
        if (const auto* err = dynamic_cast<const Error*>(&e); err && err->code() == Error::Code::Cancelled) throw;
    }
    return r;
}

bool export_photos(Catalog& catalog, const std::vector<int64_t>& ids, const ExportOptions& opt,
                   const std::function<void(int, int, const ExportItemResult&)>& progress,
                   const std::atomic<bool>* cancel) {
    const int total = static_cast<int>(ids.size());
    for (int i = 0; i < total; ++i) {
        if (cancel && cancel->load()) return false;
        ExportItemResult r;
        try {
            r = export_photo(catalog, ids[i], opt, cancel);
        } catch (const Error&) {
            return false;  // キャンセル
        }
        if (progress) progress(i + 1, total, r);
    }
    return true;
}

} // namespace focal
