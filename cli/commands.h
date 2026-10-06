#pragma once

#include <filesystem>
#include <optional>

#include "args.h"

namespace focal::cli {

int cmd_render(int argc, char** argv);
int cmd_info(int argc, char** argv);
int cmd_compare(int argc, char** argv);
int cmd_bench(int argc, char** argv);
int cmd_import(int argc, char** argv);
int cmd_roots(int argc, char** argv);
int cmd_unroot(int argc, char** argv);
int cmd_relocate(int argc, char** argv);
int cmd_optimize(int argc, char** argv);
int cmd_catalog_info(int argc, char** argv);
int cmd_backup(int argc, char** argv);
int cmd_backups(int argc, char** argv);
int cmd_merge_roots(int argc, char** argv);
int cmd_ls(int argc, char** argv);
int cmd_rate(int argc, char** argv);
int cmd_flag(int argc, char** argv);
int cmd_tag(int argc, char** argv);
int cmd_thumb(int argc, char** argv);
int cmd_export(int argc, char** argv);
int cmd_colorgrid(int argc, char** argv);
int cmd_sources(int argc, char** argv);
int cmd_import_card(int argc, char** argv);
int cmd_album(int argc, char** argv);
int cmd_preset(int argc, char** argv);
int cmd_lens(int argc, char** argv);

// レンズ DB を開く場所を決める（--lens-db / FOCAL_LENSFUN_DB / build/lensfun/db、と利用者のフォルダ）
void init_lens_db(const Args& args);
// --catalog / --cache、または環境変数 FOCAL_CATALOG / FOCAL_CACHE。なければ既定の場所
std::filesystem::path catalog_path(const Args& args);
std::filesystem::path cache_path(const Args& args);
// --presets / FOCAL_PRESETS（利用者のプリセット）、--builtin-presets / FOCAL_BUILTIN_PRESETS（同梱。なければなし）
std::filesystem::path presets_path(const Args& args);
std::optional<std::filesystem::path> builtin_presets_path(const Args& args);

} // namespace focal::cli
