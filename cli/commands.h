#pragma once

#include <filesystem>

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

// --catalog / --cache、または環境変数 FOCAL_CATALOG / FOCAL_CACHE。なければ既定の場所
std::filesystem::path catalog_path(const Args& args);
std::filesystem::path cache_path(const Args& args);

} // namespace focal::cli
