#include <cstdio>
#include <cstring>
#include <exception>

#include "commands.h"
#ifdef FOCAL_HAVE_APPLE_IMAGE_READER
#include "platform/apple_image_reader.h"
#endif

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: focal <command> [args]\n"
                 "\n"
                 "commands:\n"
                 "  render  <in.RAW> <out.tif|out.jpg> [options]  RAW を現像して書き出す\n"
                 "  lens    info | search <text> [--mount M] | detect <in.RAW>   レンズ補正の DB（--lens-db DIR / FOCAL_LENSFUN_DB）\n"
                 "  info    <in.RAW>                              メタデータと色情報を表示する\n"
                 "  compare <a.tif> <b.tif> [--max-diff N]        2 枚の画像の最大差を比較する\n"
                 "  bench   <in.RAW> [--proxy 2560] [--iters 30]   レンダリング時間を計測する\n"
                 "  thumb   <in.RAW> <out.jpg> [--render]          サムネイルを作る\n"
                 "\n"
                 "catalog commands（--catalog file / --cache dir、または FOCAL_CATALOG / FOCAL_CACHE）:\n"
                 "  import <dir> [--no-thumbs] [--threads N]       フォルダを登録して取り込む（再実行で再スキャン）\n"
                 "  roots                                         登録したフォルダの一覧（ボリューム名・オフライン表示つき）\n"
                 "  relocate <root id> <dir>                      ルートの場所を付け替える（現像・★・タグはそのまま）\n"
                 "  merge-roots [--dry-run]                       重なったルートを統合する（先にバックアップを作る）\n"
                 "  unroot <root id>                              ルートをカタログから外す（ファイルは消さない。現像・★・タグは保管される）\n"
                 "  catalog-info                                  カタログの情報（大きさ・写真の数・保管した情報・バックアップ）\n"
                 "  backup <dest dir> [--no-check] [--allow-damaged] [--keep N]  カタログのバックアップ（開けるパッケージ）を取る\n"
                 "  backups <dest dir>                            バックアップの一覧と、最後のバックアップの日時\n"
                 "  optimize [--dry-run]                          カタログの最適化: 保管した情報（外したルートの現像・★・タグ）を消してファイルを詰める\n"
                 "  sources [--path dir]                          DCIM があるボリューム（SD カード）を探す。--path ならその中身の概算\n"
                 "  import-card <card|DCIM> --dest dir [--album ID] [--tags a/b,c] [--no-verify] [--dry-run] [--no-thumbs]\n"
                 "                  [--preset ID]                 取り込んだ写真に現像のプリセットを重ねる（preset ls の ID）\n"
                 "                                                カードから <dest>/YYYY/YYYY-MM-DD/ へコピーして登録する\n"
                 "  album list | create <name> | folder <name> | smart <name> --query JSON | query <id> [--query JSON]\n"
                 "        | rename <id> <name> | move <id> | delete <id> | add <album> <photo>... | remove <album> <photo>...\n"
                 "        （create / folder / smart / move は --parent ID でフォルダの中へ）\n"
                 "  preset ls | save <name> --from <photo id> | apply <preset id> <photo id>... | delete <preset id>\n"
                 "        （--presets dir / --builtin-presets dir、または FOCAL_PRESETS / FOCAL_BUILTIN_PRESETS。調整だけを保存し、切り取りは含めない）\n"
                 "  ls [--rating N] [--flag pick|reject|none|not-rejected] [--folder ID] [--flat] [--album ID] [--smart ID]\n"
                 "     [--tag a/b] [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--available] [--limit N] [--offset N]\n"
                 "  rate <0-5> <id>...    flag <pick|reject|none> <id>...\n"
                 "  tag [--remove] <a/b> <id>...    tag --list\n"
                 "  export <id>... --dest dir [--format jpg|tif] [--quality 92] [--long-edge N]   編集を反映して書き出す\n"
                 "\n"
                 "render options:\n"
                 "  --settings file.json   編集パラメータ（6.1 章の JSON）。以下の個別指定で上書きできる\n"
                 "  --ev N  --contrast N  --highlights N  --shadows N  --whites N  --blacks N\n"
                 "  --brightness N  --saturation N  --vibrance N  --clarity N\n"
                 "  --sharpness N  --nr N  --color-nr N   シャープネス（0..150）、ノイズ低減（輝度・カラー、0..100）\n"
                 "  --temp K [--tint T]    カスタム WB\n"
                 "  --rotate N             90° 回転（0..3、時計回り）\n"
                 "  --straighten DEG       傾き補正（クロップは自動で縮める）\n"
                 "  --crop x,y,w,h         正規化クロップ矩形\n"
                 "  --lens-correction      レンズ補正を有効にする（写真のレンズ名から自動で選ぶ。DB は --lens-db DIR / FOCAL_LENSFUN_DB）\n"
                 "  --lens \"メーカー|モデル\"  レンズを指定する（`focal lens search` で探す）\n"
                 "  --lens-distortion N  --lens-tca N  --lens-vignetting N   補正の量（0..100、既定 100）\n"
                 "  --projection keep|rectilinear|fisheye|equisolid|stereographic|orthographic|panoramic|equirectangular\n"
                 "  --long-edge N          長辺を N px に縮小（Lanczos3）\n"
                 "  --proxy N              長辺 N px のプロキシからプレビュー経路で描く（バイリニア）\n"
                 "  --half                 half_size でデコードする\n"
                 "  --quality Q            JPEG 品質（既定 92）\n"
                 "  --bits 8|16            TIFF のビット深度（既定 16。--proxy 時は 8）\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const char* cmd = argv[1];
#ifdef FOCAL_HAVE_APPLE_IMAGE_READER
    focal::platform::register_apple_image_reader();  // macOS では HEIF も読む（v3.22）
#endif
    try {
        if (!std::strcmp(cmd, "render")) return focal::cli::cmd_render(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "lens")) return focal::cli::cmd_lens(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "info")) return focal::cli::cmd_info(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "compare")) return focal::cli::cmd_compare(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "bench")) return focal::cli::cmd_bench(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "thumb")) return focal::cli::cmd_thumb(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "export")) return focal::cli::cmd_export(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "colorgrid")) return focal::cli::cmd_colorgrid(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "import")) return focal::cli::cmd_import(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "sources")) return focal::cli::cmd_sources(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "import-card")) return focal::cli::cmd_import_card(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "album")) return focal::cli::cmd_album(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "preset")) return focal::cli::cmd_preset(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "relocate")) return focal::cli::cmd_relocate(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "merge-roots")) return focal::cli::cmd_merge_roots(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "catalog-info")) return focal::cli::cmd_catalog_info(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "backup")) return focal::cli::cmd_backup(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "backups")) return focal::cli::cmd_backups(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "optimize")) return focal::cli::cmd_optimize(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "unroot")) return focal::cli::cmd_unroot(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "roots")) return focal::cli::cmd_roots(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "ls")) return focal::cli::cmd_ls(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "rate")) return focal::cli::cmd_rate(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "flag")) return focal::cli::cmd_flag(argc - 2, argv + 2);
        if (!std::strcmp(cmd, "tag")) return focal::cli::cmd_tag(argc - 2, argv + 2);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
