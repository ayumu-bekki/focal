// カタログ関連のコマンド（M1）: import / ls / roots / rate / flag / tag / thumb
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "args.h"
#include "catalog/catalog.h"
#include "commands.h"
#include "export/exporter.h"
#include "imaging/image_io.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"
#include "util/file.h"

namespace focal::cli {

namespace fs = std::filesystem;

namespace {

// 既定の保存先。macOS はアプリと同じ場所（バンドル ID jp.bekki.focal）。
// --catalog / --cache、または環境変数 FOCAL_CATALOG / FOCAL_CACHE で変えられる。
fs::path env_path(const char* name, const fs::path& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? utf8_to_path(v) : fallback;
}

fs::path data_dir() {
#if defined(__APPLE__)
    return env_path("HOME", fs::current_path()) / "Library/Application Support/jp.bekki.focal";
#elif defined(_WIN32)
    return env_path("LOCALAPPDATA", fs::current_path()) / "jp.bekki.focal";
#else
    return env_path("XDG_DATA_HOME", env_path("HOME", fs::current_path()) / ".local/share") / "jp.bekki.focal";
#endif
}

fs::path cache_dir() {
#if defined(__APPLE__)
    return env_path("HOME", fs::current_path()) / "Library/Caches/jp.bekki.focal";
#elif defined(_WIN32)
    return env_path("LOCALAPPDATA", fs::current_path()) / "jp.bekki.focal" / "cache";
#else
    return env_path("XDG_CACHE_HOME", env_path("HOME", fs::current_path()) / ".cache") / "jp.bekki.focal";
#endif
}

fs::path catalog_path(const Args& args) {
    if (auto p = args.get("catalog")) return utf8_to_path(*p);
    return env_path("FOCAL_CATALOG", data_dir() / "catalog.sqlite");
}

fs::path cache_path(const Args& args) {
    if (auto p = args.get("cache")) return utf8_to_path(*p);
    return env_path("FOCAL_CACHE", cache_dir() / "thumbs");
}

std::vector<int64_t> parse_ids(const std::vector<std::string>& v, size_t from, size_t to) {
    std::vector<int64_t> ids;
    for (size_t i = from; i < to; ++i) ids.push_back(std::stoll(v[i]));
    return ids;
}

std::string format_shutter(const std::optional<double>& s) {
    if (!s) return "-";
    char buf[32];
    if (*s >= 1.0 || *s <= 0)
        std::snprintf(buf, sizeof buf, "%.1fs", *s);
    else
        std::snprintf(buf, sizeof buf, "1/%.0f", 1.0 / *s);
    return buf;
}

const char* flag_mark(int flag) { return flag > 0 ? "P" : flag < 0 ? "X" : "-"; }

} // namespace

int cmd_import(int argc, char** argv) {
    Args args(argc, argv, {"no-thumbs"});
    if (args.positional().size() != 1) {
        std::fprintf(stderr, "usage: focal import <dir> [--catalog file] [--cache dir] [--no-thumbs] [--threads N]\n");
        return 2;
    }
    const fs::path dir = utf8_to_path(args.positional()[0]);
    auto catalog = Catalog::open(catalog_path(args));
    if (!catalog->migration_backup().empty())
        std::fprintf(stderr, "catalog migrated (backup: %s)\n", path_to_utf8(catalog->migration_backup()).c_str());

    std::optional<ThumbnailCache> thumbs;
    if (!args.has("no-thumbs")) thumbs.emplace(cache_path(args));

    const int64_t root = catalog->add_root(dir);
    ScanOptions opt;
    opt.thumbnails = thumbs ? &*thumbs : nullptr;
    opt.threads = static_cast<unsigned>(args.get_int("threads", 0));
    opt.progress = [](int done, int total) { std::fprintf(stderr, "\r  %d / %d", done, total); };

    const auto t0 = std::chrono::steady_clock::now();
    const ScanStats s = catalog->scan_root(root, opt);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "\n");
    std::printf("added %d, updated %d, unchanged %d, missing %d, restored %d, renamed %d, unsupported %d\n",
                s.added, s.updated, s.unchanged, s.missing, s.restored, s.renamed, s.unsupported);
    std::printf("folders added %d, thumbnails %d (failed %d), %.2f s\n", s.folders_added, s.thumbnails,
                s.thumbnail_failures, sec);
    return 0;
}

int cmd_roots(int argc, char** argv) {
    Args args(argc, argv);
    auto catalog = Catalog::open(catalog_path(args));
    for (const auto& r : catalog->roots()) {
        std::printf("%lld\t%s\n", static_cast<long long>(r.id), r.path.c_str());
        for (const auto& f : catalog->folders(r.id))
            std::printf("  %lld\t%s/\t(%lld)\n", static_cast<long long>(f.id), f.rel_path.c_str(),
                        static_cast<long long>(f.photo_count));
    }
    return 0;
}

int cmd_ls(int argc, char** argv) {
    Args args(argc, argv, {"available", "flat"});
    auto catalog = Catalog::open(catalog_path(args));
    PhotoFilter f;
    f.min_rating = args.get_int("rating", 0);
    if (auto fl = args.get("flag")) {
        if (*fl == "pick") f.flag = FlagFilter::Picked;
        else if (*fl == "reject") f.flag = FlagFilter::Rejected;
        else if (*fl == "none") f.flag = FlagFilter::Unflagged;
        else if (*fl == "not-rejected") f.flag = FlagFilter::NotRejected;
        else throw Error(Error::Code::InvalidArgument, "--flag expects pick|reject|none|not-rejected");
    }
    if (auto folder = args.get("folder")) f.folder_id = std::stoll(*folder);
    f.include_subfolders = !args.has("flat");
    if (auto tag = args.get("tag")) {
        f.tag_id = catalog->find_tag(*tag);
        if (!f.tag_id) throw Error(Error::Code::NotFound, "no such tag: " + *tag);
    }
    f.date_from = args.get("from").value_or("");
    f.date_to = args.get("to").value_or("");
    f.include_unavailable = !args.has("available");

    const auto rows = catalog->query(f, args.get_int("offset", 0), args.get_int("limit", -1));
    for (const auto& p : rows) {
        const std::string stars(static_cast<size_t>(p.rating), '*');
        std::printf("%6lld %-5s %s %-19s %-24s ISO%-6s %-8s %s%s\n", static_cast<long long>(p.id), stars.c_str(),
                    flag_mark(p.flag), p.capture_time.value_or("-").c_str(),
                    (p.camera_make + " " + p.camera_model).c_str(),
                    p.iso ? std::to_string(*p.iso).c_str() : "-", format_shutter(p.exposure_time).c_str(),
                    p.path.c_str(),
                    p.status == PhotoStatus::Missing       ? "  [missing]"
                    : p.status == PhotoStatus::Unsupported ? "  [unsupported]"
                                                           : "");
    }
    std::fprintf(stderr, "%zu / %lld photos\n", rows.size(), static_cast<long long>(catalog->count(f)));
    return 0;
}

int cmd_rate(int argc, char** argv) {
    Args args(argc, argv);
    const auto& p = args.positional();
    if (p.size() < 2) {
        std::fprintf(stderr, "usage: focal rate <rating 0-5> <photo id>...\n");
        return 2;
    }
    auto catalog = Catalog::open(catalog_path(args));
    catalog->set_rating(parse_ids(p, 1, p.size()), std::stoi(p[0]));
    return 0;
}

int cmd_flag(int argc, char** argv) {
    Args args(argc, argv);
    const auto& p = args.positional();
    if (p.size() < 2) {
        std::fprintf(stderr, "usage: focal flag <pick|reject|none> <photo id>...\n");
        return 2;
    }
    const int flag = p[0] == "pick" ? 1 : p[0] == "reject" ? -1 : p[0] == "none" ? 0 : 2;
    if (flag == 2) throw Error(Error::Code::InvalidArgument, "flag must be pick, reject or none");
    auto catalog = Catalog::open(catalog_path(args));
    catalog->set_flag(parse_ids(p, 1, p.size()), flag);
    return 0;
}

int cmd_tag(int argc, char** argv) {
    Args args(argc, argv, {"remove", "list"});
    auto catalog = Catalog::open(catalog_path(args));
    const auto& p = args.positional();
    if (args.has("list")) {
        for (const auto& t : catalog->tags())
            std::printf("%lld\t%s\t(%lld)\n", static_cast<long long>(t.id), t.path.c_str(),
                        static_cast<long long>(t.photo_count));
        return 0;
    }
    if (p.size() < 2) {
        std::fprintf(stderr, "usage: focal tag [--remove] <tag/path> <photo id>... | focal tag --list\n");
        return 2;
    }
    const auto ids = parse_ids(p, 1, p.size());
    if (args.has("remove")) {
        if (auto tag = catalog->find_tag(p[0])) catalog->remove_tag(ids, *tag);
    } else {
        catalog->add_tag(ids, catalog->ensure_tag(p[0]));
    }
    return 0;
}

int cmd_export(int argc, char** argv) {
    Args args(argc, argv);
    const auto& p = args.positional();
    if (p.empty() || !args.has("dest")) {
        std::fprintf(stderr, "usage: focal export <photo id>... --dest dir [--format jpg|tif] [--quality 92] [--long-edge N]\n");
        return 2;
    }
    auto catalog = Catalog::open(catalog_path(args));
    ExportOptions opt;
    opt.dest_dir = utf8_to_path(*args.get("dest"));
    opt.format = args.get("format").value_or("jpg") == "tif" ? ExportOptions::Format::Tiff16 : ExportOptions::Format::Jpeg;
    opt.quality = args.get_int("quality", 92);
    opt.long_edge = args.get_int("long-edge", 0);
    int failed = 0;
    export_photos(*catalog, parse_ids(p, 0, p.size()), opt, [&](int done, int total, const ExportItemResult& r) {
        if (r.ok)
            std::printf("[%d/%d] %lld -> %s\n", done, total, static_cast<long long>(r.photo_id), path_to_utf8(r.output).c_str());
        else {
            ++failed;
            std::printf("[%d/%d] %lld failed: %s\n", done, total, static_cast<long long>(r.photo_id), r.error.c_str());
        }
    });
    return failed ? 1 : 0;
}

int cmd_thumb(int argc, char** argv) {
    Args args(argc, argv, {"render"});
    if (args.positional().size() != 2) {
        std::fprintf(stderr, "usage: focal thumb <in.RAW> <out.jpg> [--render]\n");
        return 2;
    }
    ThumbnailOptions opt;
    opt.allow_embedded = !args.has("render");
    const auto t0 = std::chrono::steady_clock::now();
    const Thumbnail t = make_thumbnail(utf8_to_path(args.positional()[0]), opt);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    write_jpeg(utf8_to_path(args.positional()[1]), t.image, 85, {});
    std::printf("%s %d x %d, %.1f ms\n", t.source == ThumbnailSource::Embedded ? "embedded" : "rendered",
                t.image.width, t.image.height, ms);
    return 0;
}

} // namespace focal::cli
