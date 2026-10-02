#include "import/card_import.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>

#include "imaging/raw_decoder.h"
#include "util/error.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/thread_pool.h"
#include "util/unicode.h"

namespace focal {

namespace fs = std::filesystem;

namespace {

constexpr size_t kCopyChunk = 1 << 20;

std::string lower_ext(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = name.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string stem_of(const std::string& name) {
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

enum class FileClass { Ignored, Raw, Image, Video, Sidecar };

FileClass classify(const std::string& name) {
    static const std::set<std::string> images = {"jpg", "jpeg", "jpe", "heic", "heif", "hif", "tif", "tiff", "png"};
    static const std::set<std::string> videos = {"mp4", "mov", "m4v", "avi", "mts", "m2ts", "mxf", "3gp", "lrv"};
    static const std::set<std::string> sidecars = {"xmp", "xml", "thm", "dop", "pp3", "acr", "cos", "wav", "dat"};
    if (is_raw_file_name(name)) return FileClass::Raw;
    const std::string e = lower_ext(name);
    if (images.count(e)) return FileClass::Image;
    if (videos.count(e)) return FileClass::Video;
    if (sidecars.count(e)) return FileClass::Sidecar;
    return FileClass::Ignored;
}

// 1 枚（同じフォルダで同じ名前の幹を持つファイルの集まり）
struct Shot {
    fs::path dir;
    std::string stem;  // 元の綴り（最初に見つけたファイル）
    struct File {
        fs::path path;
        std::string name;
        FileClass cls;
        int64_t size = 0;
        fs::file_time_type mtime;
        int64_t mtime_unix = 0;
    };
    std::vector<File> files;

    std::string capture_time;  // 'YYYY-MM-DDTHH:MM:SS'
    bool estimated = false;
    bool duplicate = false;
    fs::path dest_dir;
    std::string dest_stem;
    std::vector<bool> already_copied;  // files と同じ順。コピー先に同じファイルがすでにある
    int64_t bytes() const {
        int64_t n = 0;
        for (const auto& f : files) n += f.size;
        return n;
    }
    const File* primary() const {
        for (const auto& f : files)
            if (f.cls == FileClass::Raw) return &f;
        return nullptr;
    }
};

std::optional<fs::path> find_dcim(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = path_to_utf8(it->path().filename());
        for (auto& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n == "DCIM" && it->is_directory(ec)) return it->path();
    }
    return std::nullopt;
}

fs::path resolve_source(const fs::path& source) {
    std::error_code ec;
    if (!fs::is_directory(source, ec)) throw Error(Error::Code::NotFound, "not a directory: " + path_to_utf8(source));
    if (auto d = find_dcim(source)) return *d;
    return source;
}

std::vector<Shot> list_shots(const fs::path& source) {
    const fs::path root = resolve_source(source);
    std::map<std::pair<std::string, std::string>, Shot> shots;  // (フォルダ, 幹の case folding) → 1 枚
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) throw Error(Error::Code::Io, "cannot read " + path_to_utf8(root) + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string name = to_nfc(path_to_utf8(it->path().filename()));
        if (!name.empty() && name[0] == '.') {  // ._ で始まる macOS のメタデータなど
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const FileClass cls = classify(name);
        if (cls == FileClass::Ignored) continue;
        const auto st = stat_file(it->path());
        if (!st) continue;
        const auto key = std::make_pair(path_to_utf8(it->path().parent_path()), casefold_key(stem_of(name)));
        Shot& shot = shots[key];
        if (shot.files.empty()) {
            shot.dir = it->path().parent_path();
            shot.stem = stem_of(name);
        }
        Shot::File f;
        f.path = it->path();
        f.name = name;
        f.cls = cls;
        f.size = st->size;
        f.mtime_unix = st->mtime;
        f.mtime = fs::last_write_time(it->path(), ec);
        shot.files.push_back(std::move(f));
    }
    std::vector<Shot> out;
    for (auto& [key, shot] : shots) {
        const bool has_media = std::any_of(shot.files.begin(), shot.files.end(), [](const Shot::File& f) {
            return f.cls == FileClass::Raw || f.cls == FileClass::Image || f.cls == FileClass::Video;
        });
        if (!has_media) continue;  // サイドカーだけ
        std::sort(shot.files.begin(), shot.files.end(),
                  [](const Shot::File& a, const Shot::File& b) { return a.name < b.name; });
        out.push_back(std::move(shot));
    }
    std::sort(out.begin(), out.end(), [](const Shot& a, const Shot& b) {
        return std::make_pair(a.dir.native(), a.stem) < std::make_pair(b.dir.native(), b.stem);
    });
    return out;
}

// 同じ名前・大きさ・更新日時のファイルがコピー先にある（以前の取り込みで同じカードからコピーしたもの）
enum class DestState { Absent, Identical, Conflict };

DestState dest_state(const fs::path& dest, const Shot::File& f) {
    const auto st = stat_file(dest);
    std::error_code ec;
    if (!st) return fs::exists(dest, ec) ? DestState::Conflict : DestState::Absent;
    return st->size == f.size && st->mtime == f.mtime_unix ? DestState::Identical : DestState::Conflict;
}

std::string dest_name(const Shot::File& f, const std::string& stem) {
    const auto dot = f.name.find_last_of('.');
    return stem + (dot == std::string::npos ? std::string() : f.name.substr(dot));
}

// 撮影日時（capture_time、estimated は決まっているものとする）からコピー先を決め、取り込み済みかを調べる
void plan_destination(Catalog& catalog, Shot& s, const fs::path& dest_root) {
    const std::string year = s.capture_time.substr(0, 4);
    const std::string day = s.capture_time.substr(0, 10);
    s.dest_dir = dest_root / utf8_to_path(year) / utf8_to_path(day);

    // カタログに同じ（名前・大きさ・撮影日時）の写真がある = 取り込み済み（あとで別の場所へ移していても分かる）
    if (!s.estimated)
        if (const auto* raw = s.primary())
            if (catalog.find_photo_by_identity(raw->name, raw->size, s.capture_time)) {
                s.duplicate = true;
                return;
            }

    // コピー先の名前: 同名の別ファイルがあれば _1, _2 … を付ける（ペアの全ファイルで同じ幹にそろえる）
    for (int n = 0;; ++n) {
        const std::string stem = n == 0 ? s.stem : s.stem + "_" + std::to_string(n);
        bool conflict = false;
        std::vector<bool> copied;
        for (const auto& f : s.files) {
            const DestState st = dest_state(s.dest_dir / utf8_to_path(dest_name(f, stem)), f);
            if (st == DestState::Conflict) {
                conflict = true;
                break;
            }
            copied.push_back(st == DestState::Identical);
        }
        if (conflict) continue;
        s.dest_stem = stem;
        s.already_copied = std::move(copied);
        s.duplicate = std::all_of(s.already_copied.begin(), s.already_copied.end(), [](bool b) { return b; });
        return;
    }
}

// ルート（正規化した絶対パス）の下にある dir の、ルートからの相対パス
std::string relative_under(const std::string& root, const std::string& dir) {
    if (dir == root) return {};
    const size_t skip = root == "/" ? 1 : root.size() + 1;
    return dir.size() > skip ? dir.substr(skip) : std::string();
}

} // namespace

std::vector<ImportSource> detect_import_sources() {
    std::vector<ImportSource> out;
    for (auto& v : mounted_volumes()) {
        if (v.mount_point == fs::path("/")) continue;  // 起動ボリュームは対象外
        if (auto d = find_dcim(v.mount_point)) out.push_back({v, *d});
    }
    return out;
}

CardSummary summarize_card(const fs::path& source) {
    CardSummary s;
    for (const auto& shot : list_shots(source)) {
        ++s.shots;
        s.files += static_cast<int>(shot.files.size());
        s.bytes += shot.bytes();
    }
    return s;
}

void copy_file_verified(const fs::path& src, const fs::path& dest, bool verify, const std::atomic<bool>* cancel,
                        int64_t* bytes_done) {
    std::error_code ec;
    if (fs::exists(dest, ec)) throw Error(Error::Code::Io, "destination exists: " + path_to_utf8(dest));
    const fs::path tmp = dest.parent_path() / utf8_to_path("." + path_to_utf8(dest.filename()) + ".focal-part");
    struct Closer {
        void operator()(FILE* f) const {
            if (f) std::fclose(f);
        }
    };
    auto cleanup = [&] { fs::remove(tmp, ec); };
    try {
        std::unique_ptr<FILE, Closer> in(open_file(src, "rb"));
        if (!in) throw Error(Error::Code::Io, "cannot open: " + path_to_utf8(src));
        std::unique_ptr<FILE, Closer> out(open_file(tmp, "wb"));
        if (!out) throw Error(Error::Code::Io, "cannot create: " + path_to_utf8(tmp));
        Blake3Stream hash;
        std::vector<char> buf(kCopyChunk);
        int64_t total = 0;
        for (;;) {
            if (cancel && cancel->load()) throw Error(Error::Code::Cancelled, "cancelled");
            const size_t got = std::fread(buf.data(), 1, buf.size(), in.get());
            if (got > 0) {
                hash.update(buf.data(), got);
                if (std::fwrite(buf.data(), 1, got, out.get()) != got)
                    throw Error(Error::Code::Io, "write error: " + path_to_utf8(tmp));
                total += static_cast<int64_t>(got);
                if (bytes_done) *bytes_done += static_cast<int64_t>(got);
            }
            if (got < buf.size()) {
                if (std::ferror(in.get())) throw Error(Error::Code::Io, "read error: " + path_to_utf8(src));
                break;
            }
        }
        if (std::fflush(out.get()) != 0 || std::fclose(out.release()) != 0)
            throw Error(Error::Code::Io, "write error: " + path_to_utf8(tmp));
        in.reset();

        const auto written = stat_file(tmp);
        const auto original = stat_file(src);
        if (!written || written->size != total || (original && original->size != total))
            throw Error(Error::Code::Io, "size mismatch: " + path_to_utf8(src));
        if (verify && blake3_file_hex(tmp, cancel) != hash.finish_hex())
            throw Error(Error::Code::Io, "verification failed: " + path_to_utf8(src));

        fs::last_write_time(tmp, fs::last_write_time(src, ec), ec);  // 更新日時を引き継ぐ
        if (fs::exists(dest, ec)) throw Error(Error::Code::Io, "destination exists: " + path_to_utf8(dest));
        fs::rename(tmp, dest, ec);
        if (ec) throw Error(Error::Code::Io, "cannot rename to " + path_to_utf8(dest) + ": " + ec.message());
    } catch (...) {
        cleanup();
        throw;
    }
}

CardImportResult import_from_card(Catalog& catalog, const CardImportOptions& opt) {
    CardImportResult result;
    auto report = [&](CardImportProgress::Phase phase, int done, int total, int64_t bytes_done, int64_t bytes_total,
                      const std::string& current) {
        if (opt.progress) opt.progress({phase, done, total, bytes_done, bytes_total, current});
    };
    auto cancelled = [&] { return opt.cancel && opt.cancel->load(); };

    std::error_code ec;
    if (!opt.dry_run) fs::create_directories(opt.dest_root, ec);
    if (!fs::is_directory(opt.dest_root, ec) && !opt.dry_run)
        throw Error(Error::Code::Io, "cannot create destination: " + path_to_utf8(opt.dest_root));

    // ---- 計画: 撮影日時（RAW のメタデータ）、コピー先、取り込み済みか
    std::vector<Shot> shots = list_shots(opt.source);
    result.shots = static_cast<int>(shots.size());
    {
        ThreadPool pool(std::max(1u, std::min(4u, std::thread::hardware_concurrency())));
        std::atomic<int> done{0};
        // メタデータの読み取りは並列に、コピー先の決定とカタログの照合は順に行う（同じ名前を取り合わないため）
        std::vector<std::string> times(shots.size());
        pool.parallel_for(static_cast<int>(shots.size()), 1, [&](int b, int e) {
            for (int i = b; i < e; ++i) {
                if (cancelled()) return;
                if (const auto* raw = shots[i].primary()) {
                    try {
                        if (auto t = capture_time_string(read_raw_metadata(raw->path).timestamp)) times[i] = *t;
                    } catch (const Error&) {
                    }
                }
                report(CardImportProgress::Phase::Reading, ++done, result.shots, 0, 0, shots[i].stem);
            }
        });
        if (cancelled()) {
            result.cancelled = true;
            return result;
        }
        for (size_t i = 0; i < shots.size(); ++i) {
            Shot& s = shots[i];
            s.capture_time = times[i];
            if (s.capture_time.empty()) {
                // LibRaw から撮影日時が取れない（JPEG だけ・動画など）。ファイルの更新日時で代用して「推定」と示す
                s.estimated = true;
                int64_t earliest = s.files.front().mtime_unix;
                for (const auto& f : s.files) earliest = std::min(earliest, f.mtime_unix);
                s.capture_time =
                    capture_time_string(static_cast<std::time_t>(earliest)).value_or("1970-01-01T00:00:00");
            }
            plan_destination(catalog, s, opt.dest_root);
        }
    }

    int to_copy = 0;
    int64_t bytes_total = 0;
    for (const auto& s : shots) {
        if (s.duplicate) {
            ++result.skipped_duplicates;
            continue;
        }
        ++to_copy;
        if (s.estimated) ++result.estimated_dates;
        for (size_t i = 0; i < s.files.size(); ++i)
            if (!s.already_copied[i]) bytes_total += s.files[i].size;
    }
    if (opt.dry_run) {
        result.imported = to_copy;
        return result;
    }

    // ---- コピー（1 枚ずつ。ペアのファイルは同じ幹の名前で同じフォルダへ）
    struct Copied {
        fs::path dir;
        std::string name;
    };
    std::vector<Copied> raws;  // コピーした RAW（カタログの id を引くため）
    int done = 0;
    int64_t bytes_done = 0;
    for (const auto& s : shots) {
        if (s.duplicate) continue;
        if (cancelled()) {
            result.cancelled = true;
            break;
        }
        try {
            fs::create_directories(s.dest_dir, ec);
            for (size_t i = 0; i < s.files.size(); ++i) {
                if (s.already_copied[i]) continue;
                const auto& f = s.files[i];
                const std::string name = dest_name(f, s.dest_stem);
                report(CardImportProgress::Phase::Copying, done, to_copy, bytes_done, bytes_total, name);
                copy_file_verified(f.path, s.dest_dir / utf8_to_path(name), opt.verify, opt.cancel, &bytes_done);
                ++result.files_copied;
                if (f.cls == FileClass::Raw) raws.push_back({s.dest_dir, to_nfc(name)});
            }
            for (size_t i = 0; i < s.files.size(); ++i)  // すでにコピー済みの RAW も登録の対象（途中で止まった取り込みの続き）
                if (s.already_copied[i] && s.files[i].cls == FileClass::Raw)
                    raws.push_back({s.dest_dir, to_nfc(dest_name(s.files[i], s.dest_stem))});
            ++result.imported;
        } catch (const Error& e) {
            if (e.code() == Error::Code::Cancelled) {
                result.cancelled = true;
                break;
            }
            ++result.failed;
            result.errors.push_back(s.stem + ": " + e.what());
        }
        ++done;
    }
    result.bytes_copied = bytes_done;
    report(CardImportProgress::Phase::Copying, done, to_copy, bytes_done, bytes_total, {});

    // ---- カタログへの登録（キャンセルしても、コピーした分は登録する）
    if (raws.empty() && result.files_copied == 0) return result;
    auto root = catalog.root_containing(opt.dest_root);
    if (!root) {
        catalog.add_root(opt.dest_root);
        root = catalog.root_containing(opt.dest_root);
    }
    if (!root) throw Error(Error::Code::Internal, "cannot register destination as a library root");
    result.root_id = root->id;

    ScanOptions so;
    so.thumbnails = opt.thumbnails;
    so.progress = [&](int d, int t) { report(CardImportProgress::Phase::Cataloging, d, t, 0, 0, {}); };
    result.scan = catalog.scan_root(root->id, so);

    const std::string root_path = root->path;
    for (const auto& r : raws) {
        const std::string dir = normalized_path_string(r.dir);
        const std::string rel = relative_under(root_path, dir);
        if (const auto id = catalog.find_photo(root->id, rel, r.name)) result.photo_ids.push_back(*id);
    }
    if (!result.photo_ids.empty()) {
        if (opt.album_id) catalog.add_to_album(*opt.album_id, result.photo_ids);
        for (int64_t tag : opt.tag_ids) catalog.add_tag(result.photo_ids, tag);
    }
    return result;
}

} // namespace focal
