#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "args.h"
#include "commands.h"
#include "imaging/lens_correction.h"
#include "imaging/lens_db.h"
#include "imaging/raw_decoder.h"
#include "util/error.h"

namespace focal::cli {

void init_lens_db(const Args& args) {
    std::vector<std::filesystem::path> dirs;
    if (auto d = args.get("lens-db")) {
        dirs.emplace_back(*d);
    } else if (const char* env = std::getenv("FOCAL_LENSFUN_DB"); env && *env) {
        dirs.emplace_back(env);
    } else {
        dirs.emplace_back("build/lensfun/db");  // tools/fetch-lensfun-db.sh の出力先
    }
    if (auto u = user_lens_db_dir(); !u.empty()) dirs.push_back(u);
    set_lens_database_dirs(std::move(dirs));
}

namespace {

void print_candidate(const LensCandidate& c) {
    std::printf("%s  [%s]  %.4g-%.4g mm  crop %.3g  %s%s%s\n", c.id.c_str(), c.mounts.c_str(), c.min_focal,
                c.max_focal, c.crop_factor, c.has_distortion ? "D" : "-", c.has_tca ? "T" : "-",
                c.has_vignetting ? "V" : "-");
}

} // namespace

int cmd_lens(int argc, char** argv) {
    Args args(argc, argv);
    const auto& pos = args.positional();
    if (pos.empty()) {
        std::fprintf(stderr,
                     "usage: focal lens info | search <text> [--mount M] | detect <in.RAW>   [--lens-db DIR]\n");
        return 2;
    }
    if (!LensDatabase::supported()) {
        std::fprintf(stderr, "このビルドはレンズ補正（Lensfun）に対応していません\n");
        return 1;
    }
    init_lens_db(args);
    const auto db = shared_lens_database();
    if (pos[0] == "info") {
        std::printf("cameras: %zu\nlenses : %zu\n", db->camera_count(), db->lens_count());
        return db->lens_count() ? 0 : 1;
    }
    if (pos[0] == "search" && pos.size() >= 2) {
        std::string q;
        for (size_t i = 1; i < pos.size(); ++i) q += (i > 1 ? " " : "") + pos[i];
        for (const auto& c : db->search(q, args.get("mount").value_or(""), static_cast<size_t>(args.get_int("limit", 50))))
            print_candidate(c);
        return 0;
    }
    if (pos[0] == "detect" && pos.size() == 2) {
        const RawMetadata m = read_raw_metadata(pos[1]);
        std::printf("camera: %s %s\nlens  : %s  (%.4g mm, f/%.3g)\n", m.make.c_str(), m.model.c_str(), m.lens.c_str(),
                    m.focal_length, m.aperture);
        if (auto c = detect_lens(m)) {
            std::printf("match : ");
            print_candidate(*c);
            return 0;
        }
        std::printf("match : (なし)\n");
        return 1;
    }
    std::fprintf(stderr, "unknown lens subcommand\n");
    return 2;
}

} // namespace focal::cli
