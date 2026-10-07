// v3.19: SD カードの取り込み（sources / import-card）とアルバム（album）
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "args.h"
#include "catalog/catalog.h"
#include "catalog/smart_query.h"
#include "commands.h"
#include "edit/preset.h"
#include "import/card_import.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"
#include "util/file.h"

namespace focal::cli {

namespace fs = std::filesystem;

namespace {

std::vector<int64_t> ids_from(const std::vector<std::string>& v, size_t from) {
    std::vector<int64_t> ids;
    for (size_t i = from; i < v.size(); ++i) ids.push_back(std::stoll(v[i]));
    return ids;
}

std::optional<int64_t> parent_arg(const Args& args) {
    if (auto p = args.get("parent")) return std::stoll(*p);
    return std::nullopt;
}

std::string human_bytes(int64_t n) {
    char buf[32];
    if (n >= (int64_t(1) << 30))
        std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(n) / (1 << 30));
    else
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(n) / (1 << 20));
    return buf;
}

std::string query_text(const Args& args) {
    if (auto q = args.get("query")) return *q;
    if (auto f = args.get("query-file")) {
        std::ifstream in(utf8_to_path(*f), std::ios::binary);
        if (!in) throw Error(Error::Code::Io, "cannot read " + *f);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    throw Error(Error::Code::InvalidArgument, "--query JSON か --query-file が必要です");
}

const char* kind_name(AlbumKind k) {
    return k == AlbumKind::Folder ? "folder" : k == AlbumKind::Smart ? "smart" : "album";
}

} // namespace

int cmd_sources(int argc, char** argv) {
    Args args(argc, argv);
    if (auto path = args.get("path")) {
        const CardSummary s = summarize_card(utf8_to_path(*path));
        std::printf("%d shots, %d files, %s\n", s.shots, s.files, human_bytes(s.bytes).c_str());
        return 0;
    }
    const auto sources = detect_import_sources();
    for (const auto& src : sources) {
        const CardSummary s = summarize_card(src.dcim);
        std::printf("%s\t%s\t%d shots, %s\n", path_to_utf8(src.volume.mount_point).c_str(), src.volume.name.c_str(),
                    s.shots, human_bytes(s.bytes).c_str());
    }
    if (sources.empty()) std::fprintf(stderr, "DCIM のあるボリュームはありません\n");
    return 0;
}

int cmd_import_card(int argc, char** argv) {
    Args args(argc, argv, {"no-verify", "dry-run", "no-thumbs", "list"});
    if (args.positional().size() != 1 || !args.has("dest")) {
        std::fprintf(stderr,
                     "usage: focal import-card <card|DCIM dir> --dest dir [--album ID] [--tags a/b,c] [--no-verify]"
                     " [--dry-run] [--no-thumbs] [--list] [--only name,name] [--catalog file] [--cache dir]\n");
        return 2;
    }
    auto catalog = Catalog::open(catalog_path(args));
    // --list: 取り込む写真を選ぶ一覧（撮影日時順。取り込み済みは「*」）。--only: 名前（RAW のファイル名）で選んだ写真だけ取り込む
    if (args.has("list")) {
        const auto shots = list_card_shots(*catalog, utf8_to_path(args.positional()[0]), utf8_to_path(*args.get("dest")));
        for (const auto& c : shots)
            std::printf("%c %s  %-24s %2d files  %s%s\n", c.imported ? '*' : ' ', c.capture_time.c_str(), c.name.c_str(),
                        c.files, human_bytes(c.bytes).c_str(), c.estimated ? "  (date estimated)" : "");
        return 0;
    }
    std::optional<ThumbnailCache> thumbs;
    if (!args.has("no-thumbs") && !args.has("dry-run")) thumbs.emplace(cache_path(args));

    CardImportOptions opt;
    opt.source = utf8_to_path(args.positional()[0]);
    opt.dest_root = utf8_to_path(*args.get("dest"));
    opt.verify = !args.has("no-verify");
    opt.dry_run = args.has("dry-run");
    opt.thumbnails = thumbs ? &*thumbs : nullptr;
    if (auto a = args.get("album")) opt.album_id = std::stoll(*a);
    if (auto t = args.get("tags")) {
        std::stringstream ss(*t);
        std::string tag;
        while (std::getline(ss, tag, ','))
            if (!tag.empty()) opt.tag_ids.push_back(catalog->ensure_tag(tag));
    }
    if (auto id = args.get("preset"))
        opt.preset = PresetStore(presets_path(args), builtin_presets_path(args)).load(*id);
    if (auto only = args.get("only")) {
        std::set<std::string> names;
        std::stringstream ss(*only);
        std::string name;
        while (std::getline(ss, name, ','))
            if (!name.empty()) names.insert(name);
        opt.only = std::vector<std::string>{};
        for (const auto& c : list_card_shots(*catalog, opt.source, opt.dest_root))
            if (names.count(c.name)) opt.only->push_back(c.key);
    }
    opt.progress = [](const CardImportProgress& p) {
        const char* phase = p.phase == CardImportProgress::Phase::Reading   ? "reading"
                            : p.phase == CardImportProgress::Phase::Copying ? "copying"
                                                                            : "cataloging";
        std::fprintf(stderr, "\r  %-10s %d / %d  %s\x1b[K", phase, p.done, p.total, p.current.c_str());
    };
    const CardImportResult r = import_from_card(*catalog, opt);
    std::fprintf(stderr, "\n");
    std::printf("%d shots: %s %d, skipped %d (already imported), failed %d", r.shots,
                opt.dry_run ? "would import" : "imported", r.imported, r.skipped_duplicates, r.failed);
    if (r.skipped_unselected) std::printf(", %d not selected", r.skipped_unselected);
    if (r.estimated_dates) std::printf(", %d dated by file time (estimated)", r.estimated_dates);
    std::printf("\n");
    if (!opt.dry_run)
        std::printf("%d files, %s copied; catalog: added %d, relinked %d\n", r.files_copied,
                    human_bytes(r.bytes_copied).c_str(), r.scan.added, r.scan.relinked);
    for (const auto& e : r.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
    if (r.cancelled) std::fprintf(stderr, "cancelled\n");
    return r.failed ? 1 : 0;
}

int cmd_preset(int argc, char** argv) {
    Args args(argc, argv, {});
    const auto& p = args.positional();
    if (p.empty()) {
        std::fprintf(stderr, "usage: focal preset ls | save <name> --from <photo id> | apply <preset id> <photo id>... | delete <preset id>\n");
        return 2;
    }
    PresetStore store(presets_path(args), builtin_presets_path(args));
    const std::string& sub = p[0];
    if (sub == "ls") {
        for (const auto& info : store.list())
            std::printf("%-40s %s%s\n", info.id.c_str(), info.name.c_str(), info.builtin ? "  (Focal)" : "");
        return 0;
    }
    if (sub == "save") {
        if (p.size() != 2 || !args.has("from"))
            throw Error(Error::Code::InvalidArgument, "preset save <name> --from <photo id>");
        auto catalog = Catalog::open(catalog_path(args));
        const auto json = catalog->edit_json(std::stoll(*args.get("from")));
        const Settings s = json ? settings_from_json(*json) : Settings{};
        std::printf("%s\n", store.save(p[1], s).c_str());  // 切り取りなど（geometry）は保存されない
        return 0;
    }
    if (sub == "apply") {
        if (p.size() < 3) throw Error(Error::Code::InvalidArgument, "preset apply <preset id> <photo id>...");
        auto catalog = Catalog::open(catalog_path(args));
        std::vector<int64_t> ids;
        for (size_t i = 2; i < p.size(); ++i) ids.push_back(std::stoll(p[i]));
        catalog->apply_preset(ids, store.load(p[1]));
        catalog->flush();
        std::printf("applied to %zu photos\n", ids.size());
        return 0;
    }
    if (sub == "delete") {
        if (p.size() != 2) throw Error(Error::Code::InvalidArgument, "preset delete <preset id>");
        store.remove(p[1]);
        return 0;
    }
    std::fprintf(stderr, "unknown preset command: %s\n", sub.c_str());
    return 2;
}

int cmd_album(int argc, char** argv) {
    Args args(argc, argv, {});
    const auto& p = args.positional();
    if (p.empty()) {
        std::fprintf(stderr, "usage: focal album list|create|folder|smart|query|rename|move|delete|add|remove ...\n");
        return 2;
    }
    auto catalog = Catalog::open(catalog_path(args));
    const std::string& sub = p[0];
    auto need = [&](size_t n) {
        if (p.size() < n) throw Error(Error::Code::InvalidArgument, "album " + sub + ": 引数が足りません");
    };

    if (sub == "list") {
        const auto albums = catalog->albums();
        for (const auto& a : albums) {
            int depth = 0;
            for (auto parent = a.parent_id; parent && depth < 16; ++depth) {
                std::optional<int64_t> up;
                for (const auto& q : albums)
                    if (q.id == *parent) up = q.parent_id;
                parent = up;
            }
            std::printf("%lld\t%-6s\t%*s%s\t(%lld)\n", static_cast<long long>(a.id), kind_name(a.kind), depth * 2, "",
                        a.name.c_str(), static_cast<long long>(a.photo_count));
        }
    } else if (sub == "create") {
        need(2);
        std::printf("%lld\n", static_cast<long long>(catalog->create_album(p[1], parent_arg(args))));
    } else if (sub == "folder") {
        need(2);
        std::printf("%lld\n", static_cast<long long>(catalog->create_album_folder(p[1], parent_arg(args))));
    } else if (sub == "smart") {
        need(2);
        std::printf("%lld\n",
                    static_cast<long long>(catalog->create_smart_album(p[1], query_text(args), parent_arg(args))));
    } else if (sub == "query") {
        need(2);
        const int64_t id = std::stoll(p[1]);
        if (args.has("query") || args.has("query-file")) {
            catalog->set_smart_query(id, query_text(args));
        } else if (auto q = catalog->smart_query(id)) {
            std::printf("%s\n", q->c_str());
        } else {
            throw Error(Error::Code::NotFound, "not a smart album: " + p[1]);
        }
    } else if (sub == "rename") {
        need(3);
        catalog->rename_album(std::stoll(p[1]), p[2]);
    } else if (sub == "move") {
        need(2);
        catalog->move_album(std::stoll(p[1]), parent_arg(args));
    } else if (sub == "delete") {
        need(2);
        catalog->delete_album(std::stoll(p[1]));
    } else if (sub == "add") {
        need(3);
        catalog->add_to_album(std::stoll(p[1]), ids_from(p, 2));
    } else if (sub == "remove") {
        need(3);
        catalog->remove_from_album(std::stoll(p[1]), ids_from(p, 2));
    } else {
        throw Error(Error::Code::InvalidArgument, "unknown album command: " + sub);
    }
    return 0;
}

} // namespace focal::cli
