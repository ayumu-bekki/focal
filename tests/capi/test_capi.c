/*
 * C API のテスト（12 章）。C だけで書き、C API だけを使う。AddressSanitizer でも実行する。
 * テスト用 RAW がなければ 77 を返して skip 扱いにする。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "focal/focal.h"

static int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

#define REQUIRE_OK(expr)                                                                           \
    do {                                                                                           \
        fc_status st_ = (expr);                                                                    \
        if (st_ != FC_OK) {                                                                        \
            fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #expr, st_, fc_last_error()); \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* ---- 完了を待つための小さな仕組み ---- */

typedef struct waiter {
    pthread_mutex_t m;
    pthread_cond_t c;
    int done;
    int calls;
} waiter;

static void waiter_init(waiter* w) {
    pthread_mutex_init(&w->m, NULL);
    pthread_cond_init(&w->c, NULL);
    w->done = 0;
    w->calls = 0;
}

static void waiter_signal(waiter* w) {
    pthread_mutex_lock(&w->m);
    w->calls++;
    w->done = 1;
    pthread_cond_broadcast(&w->c);
    pthread_mutex_unlock(&w->m);
}

static void waiter_wait_calls(waiter* w, int calls) {
    pthread_mutex_lock(&w->m);
    while (w->calls < calls) pthread_cond_wait(&w->c, &w->m);
    pthread_mutex_unlock(&w->m);
}

/* ---- スキャン ---- */

typedef struct scan_ctx {
    waiter w;
    fc_status status;
    fc_scan_stats stats;
    int progress_calls;
    char message[512];
} scan_ctx;

static void on_progress(void* user, int32_t done, int32_t total) {
    scan_ctx* ctx = (scan_ctx*)user;
    ctx->progress_calls++;
    (void)done;
    (void)total;
}

static void on_scan_done(void* user, fc_status status, const fc_scan_stats* stats, const char* message) {
    scan_ctx* ctx = (scan_ctx*)user;
    ctx->status = status;
    ctx->stats = *stats;
    snprintf(ctx->message, sizeof ctx->message, "%s", message ? message : "");
    waiter_signal(&ctx->w);
}

static scan_ctx run_scan(fc_catalog* cat, int64_t root, const char* cache) {
    scan_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    waiter_init(&ctx.w);
    fc_task* task = NULL;
    REQUIRE_OK(fc_catalog_scan_async(cat, root, cache, on_progress, on_scan_done, &ctx, &task));
    waiter_wait_calls(&ctx.w, 1);
    fc_task_release(task);
    return ctx;
}

/* ---- カードの取り込み ---- */

typedef struct card_ctx {
    waiter w;
    fc_status status;
    fc_card_import_result result;
    int copy_progress;
} card_ctx;

static void on_card_progress(void* user, int32_t phase, int32_t done, int32_t total, int64_t bytes_done,
                             int64_t bytes_total, const char* current) {
    card_ctx* ctx = (card_ctx*)user;
    (void)done;
    (void)total;
    (void)bytes_done;
    (void)bytes_total;
    (void)current;
    if (phase == FC_IMPORT_COPYING) ctx->copy_progress++;
}

static void on_card_done(void* user, fc_status status, const fc_card_import_result* result, const char* message) {
    card_ctx* ctx = (card_ctx*)user;
    (void)message;
    ctx->status = status;
    ctx->result = *result;
    waiter_signal(&ctx->w);
}

/* ---- 写真の削除 ---- */

static int trash_count = 0;

static int32_t trash_fail(void* user, const char* path) {
    (void)user;
    (void)path;
    return 1;
}

static int32_t trash_remove(void* user, const char* path) {
    (void)user;
    trash_count++;
    return remove(path) == 0 ? 0 : 1;
}

/* ---- 現像 ---- */

typedef struct session_ctx {
    waiter w;
    int preview;
    int ready;
    int failed;
} session_ctx;

static void on_session(void* user, int32_t event, const char* message) {
    session_ctx* s = (session_ctx*)user;
    (void)message;
    pthread_mutex_lock(&s->w.m);
    if (event == FC_EVENT_PREVIEW) s->preview++;
    if (event == FC_EVENT_READY) s->ready++;
    if (event == FC_EVENT_FAILED) s->failed++;
    s->w.calls++;
    pthread_cond_broadcast(&s->w.c);
    pthread_mutex_unlock(&s->w.m);
}

static void wait_ready(session_ctx* s) {
    pthread_mutex_lock(&s->w.m);
    while (!s->ready && !s->failed) pthread_cond_wait(&s->w.c, &s->w.m);
    pthread_mutex_unlock(&s->w.m);
}

typedef struct render_ctx {
    waiter w;
    fc_status status;
    fc_render_result result;
} render_ctx;

static void on_render(void* user, fc_status status, const fc_render_result* result) {
    render_ctx* r = (render_ctx*)user;
    pthread_mutex_lock(&r->w.m);
    r->status = status;
    if (result) r->result = *result;
    r->w.calls++;
    pthread_cond_broadcast(&r->w.c);
    pthread_mutex_unlock(&r->w.m);
}

/* ---- 書き出し ---- */

typedef struct export_ctx {
    waiter w;
    int ok;
    int failed;
    fc_status status;
    char last[1024];
} export_ctx;

static void on_export_item(void* user, int32_t done, int32_t total, int64_t id, int32_t ok, const char* text) {
    export_ctx* e = (export_ctx*)user;
    (void)done;
    (void)total;
    (void)id;
    if (ok) e->ok++; else e->failed++;
    snprintf(e->last, sizeof e->last, "%s", text ? text : "");
}

static void on_export_done(void* user, fc_status status) {
    export_ctx* e = (export_ctx*)user;
    e->status = status;
    waiter_signal(&e->w);
}

/* ---- サムネイル ---- */

typedef struct thumb_ctx {
    waiter w;
    int ok;
    int cancelled;
    int failed;
    int existing_files;
} thumb_ctx;

static void on_thumb(void* user, uint64_t request_id, fc_status status, const char* path) {
    thumb_ctx* t = (thumb_ctx*)user;
    (void)request_id;
    pthread_mutex_lock(&t->w.m);
    if (status == FC_OK) {
        t->ok++;
        struct stat st;
        if (path && stat(path, &st) == 0 && st.st_size > 0) t->existing_files++;
    } else if (status == FC_ERR_CANCELLED) {
        t->cancelled++;
        if (path) t->failed += 1000; /* キャンセル時は NULL のはず */
    } else {
        t->failed++;
    }
    t->w.calls++;
    pthread_cond_broadcast(&t->w.c);
    pthread_mutex_unlock(&t->w.m);
}

static void copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    FILE* out = fopen(to, "wb");
    if (!in || !out) {
        fprintf(stderr, "copy failed: %s -> %s\n", from, to);
        exit(1);
    }
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

int main(void) {
    const char* data = FOCAL_TEST_DATA_DIR;
    char sony[1024];
    snprintf(sony, sizeof sony, "%s/sony_ilce7m3.ARW", data);
    if (access(sony, R_OK) != 0) {
        fprintf(stderr, "skip: tests/data/fetch.sh でテスト用 RAW を取得する\n");
        return 77;
    }

    CHECK(fc_api_version() == FC_API_VERSION);

    char tmpl[] = "/tmp/focal-capi-XXXXXX";
    char* tmp = mkdtemp(tmpl);
    CHECK(tmp != NULL);

    /* 日本語（NFD）のフォルダ名とファイル名: 写真/がっこう.ARW */
    char lib[1024], sub[1024], photo[1024], cat_path[1024], cache[1024];
    snprintf(lib, sizeof lib, "%s/lib", tmp);
    snprintf(sub, sizeof sub, "%s/\xe5\x86\x99\xe7\x9c\x9f", lib);
    snprintf(photo, sizeof photo, "%s/\xe3\x81\x8b\xe3\x82\x99\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86.ARW", sub);
    snprintf(cat_path, sizeof cat_path, "%s/catalog.sqlite", tmp);
    snprintf(cache, sizeof cache, "%s/thumbs", tmp);
    mkdir(lib, 0755);
    mkdir(sub, 0755);
    copy_file(sony, photo);
    {
        char p[1024];
        snprintf(p, sizeof p, "%s/canon_eos_m50.CR3", data);
        char d[1024];
        snprintf(d, sizeof d, "%s/IMG_0001.CR3", lib);
        copy_file(p, d);
    }

    /* 引数の誤りはエラーコードとメッセージで返る（例外は出ない） */
    fc_catalog* bad = NULL;
    CHECK(fc_catalog_open(NULL, &bad) == FC_ERR_INVALID_ARGUMENT);
    CHECK(strlen(fc_last_error()) > 0);
    CHECK(fc_catalog_open(lib, &bad) == FC_ERR_DATABASE); /* ディレクトリはカタログとして開けない */
    CHECK(bad == NULL);

    fc_catalog* cat = NULL;
    REQUIRE_OK(fc_catalog_open(cat_path, &cat));
    CHECK(strlen(fc_last_error()) == 0);

    CHECK(fc_catalog_add_root(cat, "/nonexistent/dir", NULL) == FC_ERR_NOT_FOUND);
    int64_t root = 0;
    REQUIRE_OK(fc_catalog_add_root(cat, lib, &root));
    CHECK(root > 0);

    /* スキャン（サムネイル作成あり） */
    scan_ctx sc = run_scan(cat, root, cache);
    CHECK(sc.status == FC_OK);
    CHECK(sc.stats.added == 2);
    CHECK(sc.stats.thumbnails == 2);
    CHECK(sc.progress_calls >= 1);
    CHECK(sc.w.calls == 1);

    /* 存在しないルートのスキャンはエラーで完了する */
    scan_ctx bad_scan = run_scan(cat, 9999, NULL);
    CHECK(bad_scan.status == FC_ERR_NOT_FOUND);
    CHECK(strlen(bad_scan.message) > 0);

    fc_root_array* roots = NULL;
    REQUIRE_OK(fc_catalog_roots(cat, &roots));
    CHECK(roots->count == 1);
    fc_root_array_free(roots);

    fc_folder_array* folders = NULL;
    REQUIRE_OK(fc_catalog_folders(cat, root, &folders));
    CHECK(folders->count == 2);
    CHECK(strcmp(folders->items[0].rel_path, "") == 0);
    CHECK(folders->items[0].parent_id == 0);
    /* NFC で保存されている（写真 は正規化で変わらない） */
    CHECK(strcmp(folders->items[1].rel_path, "\xe5\x86\x99\xe7\x9c\x9f") == 0);
    fc_folder_array_free(folders);

    /* 写真の一覧 */
    fc_photo_filter f;
    fc_photo_filter_init(&f);
    int64_t n = 0;
    REQUIRE_OK(fc_catalog_count(cat, &f, &n));
    CHECK(n == 2);

    fc_photo_array* photos = NULL;
    REQUIRE_OK(fc_catalog_query(cat, &f, 0, -1, &photos));
    CHECK(photos->count == 2);
    /* 撮影日時順: Sony（2018-03）→ Canon（2018-07） */
    CHECK(strcmp(photos->items[0].file_name, "\xe3\x81\x8c\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86.ARW") == 0);
    CHECK(strcmp(photos->items[0].camera_make, "Sony") == 0);
    CHECK(photos->items[0].iso == 400);
    CHECK(strcmp(photos->items[0].capture_time, "2018-03-13T16:38:13") == 0);
    CHECK(photos->items[0].status == FC_PHOTO_OK);
    const int64_t sony_id = photos->items[0].id;
    const int64_t canon_id = photos->items[1].id;
    fc_photo_array_free(photos);

    fc_id_array* ids = NULL;
    REQUIRE_OK(fc_catalog_query_ids(cat, &f, &ids));
    CHECK(ids->count == 2);
    CHECK(ids->items[0] == sony_id);
    /* id 指定は指定した順で返る。存在しない id は飛ばす */
    int64_t want[3] = {canon_id, 12345, sony_id};
    REQUIRE_OK(fc_catalog_photos_by_ids(cat, want, 3, &photos));
    CHECK(photos->count == 2);
    CHECK(photos->items[0].id == canon_id);
    CHECK(photos->items[1].id == sony_id);
    fc_photo_array_free(photos);
    fc_id_array_free(ids);

    /* ★・フラグ・タグ */
    REQUIRE_OK(fc_catalog_set_rating(cat, &sony_id, 1, 4));
    CHECK(fc_catalog_set_rating(cat, &sony_id, 1, 9) == FC_ERR_INVALID_ARGUMENT);
    REQUIRE_OK(fc_catalog_set_flag(cat, &canon_id, 1, -1));
    f.min_rating = 3;
    REQUIRE_OK(fc_catalog_count(cat, &f, &n));
    CHECK(n == 1);
    fc_photo_filter_init(&f);
    f.flag = FC_FLAG_NOT_REJECTED;
    REQUIRE_OK(fc_catalog_count(cat, &f, &n));
    CHECK(n == 1);

    int64_t tag = 0;
    REQUIRE_OK(fc_catalog_ensure_tag(cat, "Places/\xe5\xad\xa6\xe6\xa0\xa1", &tag)); /* Places/学校 */
    int64_t both[2] = {sony_id, canon_id};
    REQUIRE_OK(fc_catalog_add_tag(cat, both, 2, tag));
    fc_tag_array* tags = NULL;
    REQUIRE_OK(fc_catalog_tags(cat, &tags));
    CHECK(tags->count == 2);
    CHECK(strcmp(tags->items[1].path, "Places/\xe5\xad\xa6\xe6\xa0\xa1") == 0);
    CHECK(tags->items[1].photo_count == 2);
    CHECK(tags->items[1].parent_id == tags->items[0].id);
    fc_tag_array_free(tags);
    REQUIRE_OK(fc_catalog_remove_tag(cat, &canon_id, 1, tag));
    REQUIRE_OK(fc_catalog_photo_tags(cat, canon_id, &tags));
    CHECK(tags->count == 0);
    fc_tag_array_free(tags);

    /* アルバム（v3.16） */
    {
        int64_t album = 0;
        REQUIRE_OK(fc_catalog_create_album(cat, "\xe6\x97\x85\xe8\xa1\x8c", 0, &album)); /* 旅行 */
        CHECK(fc_catalog_create_album(cat, "\xe6\x97\x85\xe8\xa1\x8c", 0, NULL) == FC_ERR_INVALID_ARGUMENT);
        REQUIRE_OK(fc_catalog_add_to_album(cat, album, both, 2));
        fc_photo_filter_init(&f);
        f.album_id = album;
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n == 2);
        REQUIRE_OK(fc_catalog_remove_from_album(cat, album, &canon_id, 1));
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n == 1);
        REQUIRE_OK(fc_catalog_rename_album(cat, album, "Trip"));
        fc_album_array* albums = NULL;
        REQUIRE_OK(fc_catalog_albums(cat, &albums));
        CHECK(albums->count == 1);
        CHECK(strcmp(albums->items[0].name, "Trip") == 0);
        CHECK(albums->items[0].photo_count == 1);
        fc_album_array_free(albums);
        /* 最近の取り込み: 最初の取り込みで足した写真 */
        fc_photo_filter_init(&f);
        f.recent_import = 1;
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n >= 2);
        REQUIRE_OK(fc_catalog_delete_album(cat, album));
        REQUIRE_OK(fc_catalog_albums(cat, &albums));
        CHECK(albums->count == 0);
        fc_album_array_free(albums);
    }

    /* アルバムのフォルダ・スマートアルバム（v3.19） */
    {
        int64_t folder = 0, inner = 0, smart = 0, flat = 0;
        REQUIRE_OK(fc_catalog_create_album_folder(cat, "Trips", 0, &folder));
        REQUIRE_OK(fc_catalog_create_album(cat, "Hokkaido", folder, &inner));
        REQUIRE_OK(fc_catalog_create_album(cat, "Hokkaido", 0, &flat)); /* 親が違えば同じ名前でよい */
        CHECK(fc_catalog_create_album(cat, "Hokkaido", folder, NULL) == FC_ERR_INVALID_ARGUMENT);
        CHECK(fc_catalog_create_album(cat, "X", flat, NULL) == FC_ERR_INVALID_ARGUMENT); /* 親にできるのはフォルダだけ */
        CHECK(fc_catalog_add_to_album(cat, folder, both, 2) == FC_ERR_INVALID_ARGUMENT);
        REQUIRE_OK(fc_catalog_add_to_album(cat, inner, both, 2));

        const char* query = "{\"match\":\"all\",\"rules\":[{\"field\":\"camera\",\"op\":\"contains\",\"value\":\"Canon\"},"
                            "{\"field\":\"album\",\"op\":\"in\",\"value\":%lld}]}";
        char json[512];
        snprintf(json, sizeof json, query, (long long)folder);
        CHECK(fc_catalog_create_smart_album(cat, "Canon in Trips", "{", 0, NULL) == FC_ERR_INVALID_ARGUMENT);
        REQUIRE_OK(fc_catalog_create_smart_album(cat, "Canon in Trips", json, 0, &smart));
        CHECK(fc_catalog_add_to_album(cat, smart, both, 2) == FC_ERR_INVALID_ARGUMENT); /* 読み取り専用 */
        fc_photo_filter_init(&f);
        f.smart_album_id = smart;
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n == 1); /* フォルダの中のアルバムも含み、Canon だけ */
        fc_string* q = NULL;
        REQUIRE_OK(fc_catalog_smart_query(cat, smart, &q));
        CHECK(strstr(q->value, "Canon") != NULL);
        fc_string_free(q);
        CHECK(fc_catalog_smart_query(cat, inner, &q) == FC_ERR_NOT_FOUND);
        REQUIRE_OK(fc_catalog_set_smart_query(cat, smart, "{\"rules\":[]}"));
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n == 2);

        fc_album_array* albums = NULL;
        REQUIRE_OK(fc_catalog_albums(cat, &albums));
        CHECK(albums->count == 4);
        CHECK(albums->items[0].kind == FC_ALBUM_FOLDER);
        CHECK(albums->items[1].id == inner && albums->items[1].parent_id == folder);
        CHECK(albums->items[1].kind == FC_ALBUM_ALBUM && albums->items[1].photo_count == 2);
        CHECK(albums->items[3].kind == FC_ALBUM_SMART && albums->items[3].photo_count == 2);
        fc_album_array_free(albums);

        CHECK(fc_catalog_move_album(cat, flat, folder) == FC_ERR_INVALID_ARGUMENT); /* 同じ名前がフォルダの中にある */
        CHECK(fc_catalog_move_album(cat, folder, inner) == FC_ERR_INVALID_ARGUMENT); /* 自分の中へは動かせない */
        REQUIRE_OK(fc_catalog_rename_album(cat, flat, "Elsewhere"));
        REQUIRE_OK(fc_catalog_move_album(cat, flat, folder));
        REQUIRE_OK(fc_catalog_move_album(cat, flat, 0));
        REQUIRE_OK(fc_catalog_delete_album(cat, folder)); /* 中のアルバムも消える */
        REQUIRE_OK(fc_catalog_albums(cat, &albums));
        CHECK(albums->count == 2); /* Elsewhere とスマートアルバム */
        fc_album_array_free(albums);
        REQUIRE_OK(fc_catalog_delete_album(cat, smart));
        REQUIRE_OK(fc_catalog_delete_album(cat, flat));
    }

    /* 写真の削除（v3.19）: ゴミ箱の代わりに、呼ばれたパスを数えてファイルを消す */
    {
        char del_lib[1024], del_photo[1024], del_jpg[1024];
        snprintf(del_lib, sizeof del_lib, "%s/dellib", tmp);
        snprintf(del_photo, sizeof del_photo, "%s/DEL_0001.CR3", del_lib);
        snprintf(del_jpg, sizeof del_jpg, "%s/DEL_0001.JPG", del_lib);
        char canon_src[1024];
        snprintf(canon_src, sizeof canon_src, "%s/canon_eos_m50.CR3", data);
        mkdir(del_lib, 0755);
        copy_file(canon_src, del_photo);
        copy_file(canon_src, del_jpg); /* 中身は何でもよい。JPEG として一緒に消える */
        int64_t del_root = 0;
        REQUIRE_OK(fc_catalog_add_root(cat, del_lib, &del_root));
        scan_ctx dctx = run_scan(cat, del_root, NULL);
        CHECK(dctx.status == FC_OK && dctx.stats.added == 1);
        fc_photo_filter_init(&f);
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        int64_t before = n;
        fc_id_array* del_ids = NULL;
        f.folder_id = 0;
        REQUIRE_OK(fc_catalog_query_ids(cat, &f, &del_ids));
        int64_t victim = 0;
        for (size_t i = 0; i < del_ids->count; ++i) {
            fc_photo_array* one = NULL;
            REQUIRE_OK(fc_catalog_photos_by_ids(cat, &del_ids->items[i], 1, &one));
            if (one->count == 1 && strcmp(one->items[0].file_name, "DEL_0001.CR3") == 0) victim = one->items[0].id;
            fc_photo_array_free(one);
        }
        fc_id_array_free(del_ids);
        CHECK(victim != 0);

        fc_delete_plan plan;
        REQUIRE_OK(fc_catalog_plan_delete(cat, &victim, 1, &plan));
        CHECK(plan.photos == 1 && plan.files == 2 && plan.network_photos == 0 && plan.missing_photos == 0);

        fc_delete_result res;
        fc_string* errors = NULL;
        /* ゴミ箱が使えない: RAW を消せないので、ファイルもカタログも残る */
        REQUIRE_OK(fc_catalog_delete_photos(cat, &victim, 1, trash_fail, NULL, &res, &errors));
        CHECK(res.photos_deleted == 0 && res.photos_failed == 1);
        CHECK(errors != NULL && strlen(errors->value) > 0);
        fc_string_free(errors);
        CHECK(access(del_photo, R_OK) == 0);
        REQUIRE_OK(fc_catalog_delete_photos(cat, &victim, 1, NULL, NULL, &res, NULL)); /* trash なしも安全側 */
        CHECK(res.photos_failed == 1 && access(del_photo, R_OK) == 0);

        trash_count = 0;
        REQUIRE_OK(fc_catalog_delete_photos(cat, &victim, 1, trash_remove, NULL, &res, &errors));
        CHECK(res.photos_deleted == 1 && res.files_trashed == 2 && res.files_failed == 0);
        CHECK(trash_count == 2);
        fc_string_free(errors);
        CHECK(access(del_photo, R_OK) != 0 && access(del_jpg, R_OK) != 0);
        fc_photo_filter_init(&f);
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n == before - 1);
    }

    /* カードの取り込み（v3.19） */
    {
        char card_dir[1024], dcim[1024], card_photo[1024], dest[1024], src[1024];
        snprintf(card_dir, sizeof card_dir, "%s/card", tmp);
        snprintf(dcim, sizeof dcim, "%s/DCIM", card_dir);
        char sub100[1024];
        snprintf(sub100, sizeof sub100, "%s/100CANON", dcim);
        snprintf(card_photo, sizeof card_photo, "%s/IMG_9999.CR3", sub100);
        snprintf(dest, sizeof dest, "%s/imported", tmp);
        snprintf(src, sizeof src, "%s/canon_eos_m50.CR3", data);
        mkdir(card_dir, 0755);
        mkdir(dcim, 0755);
        mkdir(sub100, 0755);
        copy_file(src, card_photo);

        fc_card_summary sum;
        REQUIRE_OK(fc_card_summarize(card_dir, &sum));
        CHECK(sum.shots == 1 && sum.files == 1 && sum.bytes > 0);
        CHECK(fc_card_summarize("/nonexistent/card", &sum) == FC_ERR_NOT_FOUND);

        fc_import_source_array* sources = NULL;
        REQUIRE_OK(fc_import_sources(&sources)); /* この環境に SD カードがあるかは分からない。解放できればよい */
        fc_import_source_array_free(sources);

        int64_t tag = 0, album = 0;
        REQUIRE_OK(fc_catalog_ensure_tag(cat, "Card/Day1", &tag));
        REQUIRE_OK(fc_catalog_create_album(cat, "FromCard", 0, &album));
        fc_card_import_options opt;
        memset(&opt, 0, sizeof opt);
        opt.source = card_dir;
        opt.dest_root = dest;
        opt.verify = 1;
        opt.album_id = album;
        opt.tag_ids = &tag;
        opt.tag_count = 1;
        opt.thumbnail_cache_dir = cache;

        card_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        waiter_init(&ctx.w);
        fc_task* task = NULL;
        REQUIRE_OK(fc_card_import_start(cat, &opt, on_card_progress, on_card_done, &ctx, &task));
        waiter_wait_calls(&ctx.w, 1);
        fc_task_release(task);
        CHECK(ctx.status == FC_OK);
        CHECK(ctx.result.shots == 1 && ctx.result.imported == 1 && ctx.result.failed == 0);
        CHECK(ctx.result.files_copied == 1 && ctx.result.bytes_copied > 0);
        CHECK(ctx.result.added == 1 && ctx.result.root_id > 0);
        CHECK(ctx.copy_progress > 0);
        char copied[1024];
        snprintf(copied, sizeof copied, "%s/2018/2018-07-01/IMG_9999.CR3", dest);
        CHECK(access(copied, R_OK) == 0);
        CHECK(access(card_photo, R_OK) == 0); /* カードのファイルは残る */

        fc_photo_filter_init(&f);
        f.album_id = album;
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n == 1);
        fc_photo_filter_init(&f);
        f.recent_import = 1;
        REQUIRE_OK(fc_catalog_count(cat, &f, &n));
        CHECK(n >= 1); /* 時刻は秒単位なので、直前のスキャンの写真も同じ秒なら入る */

        /* 2 回目は取り込み済み */
        memset(&ctx, 0, sizeof ctx);
        waiter_init(&ctx.w);
        REQUIRE_OK(fc_card_import_start(cat, &opt, on_card_progress, on_card_done, &ctx, &task));
        waiter_wait_calls(&ctx.w, 1);
        fc_task_release(task);
        CHECK(ctx.result.skipped_duplicates == 1 && ctx.result.imported == 0);

        /* ルートにはボリュームの情報が付き、オンライン */
        fc_root_array* roots2 = NULL;
        REQUIRE_OK(fc_catalog_roots(cat, &roots2));
        CHECK(roots2->count == 3); /* ライブラリ、削除のテスト、取り込み先 */
        for (size_t i = 0; i < roots2->count; ++i) CHECK(roots2->items[i].online == 1);
        {
            fc_root_details* det = NULL;
            REQUIRE_OK(fc_catalog_root_details(cat, roots2->items[0].id, &det));
            CHECK(det->id == roots2->items[0].id);
            CHECK(det->online == 1);
            CHECK(det->photos >= 0);
            CHECK(det->total_bytes > 0 && det->free_bytes >= 0);
            CHECK(strlen(det->path) > 0);
            fc_root_details_free(det);
            CHECK(fc_catalog_root_details(cat, 987654, &det) == FC_ERR_NOT_FOUND);
        }
        fc_root_array_free(roots2);
        {
            /* 重なり・場所の付け替え・フォルダの場所（v3.19） */
            int32_t nested = -1, merged = -1;
            REQUIRE_OK(fc_catalog_nested_root_count(cat, &nested));
            CHECK(nested == 0);
            REQUIRE_OK(fc_catalog_merge_nested_roots(cat, &merged));
            CHECK(merged == 0);
            int64_t root_id = 0, folder_id = 0;
            REQUIRE_OK(fc_catalog_folder_for_path(cat, lib, &root_id, &folder_id));
            CHECK(root_id > 0 && folder_id > 0);
            CHECK(fc_catalog_folder_for_path(cat, "/nonexistent/elsewhere", NULL, NULL) == FC_ERR_NOT_FOUND);
            /* 登録済みのルートの中のフォルダを追加しても、新しいルートはできない */
            int64_t again = 0;
            REQUIRE_OK(fc_catalog_add_root(cat, sub, &again));
            CHECK(again == root_id);
            /* 別のルートと重なる場所へは付け替えられない */
            CHECK(fc_catalog_relocate_root(cat, root_id, dest) == FC_ERR_INVALID_ARGUMENT);
        }
        int32_t changed = -1;
        REQUIRE_OK(fc_catalog_refresh_volumes(cat, &changed));
        CHECK(changed == 0);
        REQUIRE_OK(fc_catalog_delete_album(cat, album));
    }

    /* サムネイル: キャッシュ済みでも未作成でも、どの要求にもちょうど 1 回コールバックが来る */
    {
        char cache2[1024];
        snprintf(cache2, sizeof cache2, "%s/thumbs2", tmp);
        fc_thumbnailer* th = NULL;
        REQUIRE_OK(fc_thumbnailer_create(cat, cache2, 1, &th));
        thumb_ctx t;
        memset(&t, 0, sizeof t);
        waiter_init(&t.w);
        const int requests = 40;
        uint64_t rids[40];
        for (int i = 0; i < requests; ++i) rids[i] = fc_thumbnailer_request(th, (i % 2) ? sony_id : canon_id, on_thumb, &t);
        for (int i = 0; i < requests; ++i) CHECK(rids[i] != 0);
        /* 1 スレッドなので、後半の多くは開始前にキャンセルできる */
        for (int i = 0; i < requests - 2; ++i) fc_thumbnailer_cancel(th, rids[i]);
        uint64_t missing = fc_thumbnailer_request(th, 99999, on_thumb, &t); /* 存在しない写真 */
        CHECK(missing != 0);
        waiter_wait_calls(&t.w, requests + 1);
        CHECK(t.ok + t.cancelled + t.failed == requests + 1);
        CHECK(t.cancelled > 0);
        CHECK(t.ok >= 2);
        CHECK(t.existing_files == t.ok);
        CHECK(t.failed == 1);
        fc_thumbnailer_destroy(th);
        /* 破棄後も追加のコールバックは来ない */
        CHECK(t.w.calls == requests + 1);
    }

    /* 破棄時に開始前の要求はキャンセルとして返る */
    {
        char cache3[1024];
        snprintf(cache3, sizeof cache3, "%s/thumbs3", tmp);
        fc_thumbnailer* th = NULL;
        REQUIRE_OK(fc_thumbnailer_create(cat, cache3, 1, &th));
        thumb_ctx t;
        memset(&t, 0, sizeof t);
        waiter_init(&t.w);
        for (int i = 0; i < 20; ++i) fc_thumbnailer_request(th, sony_id, on_thumb, &t);
        fc_thumbnailer_destroy(th);
        CHECK(t.w.calls == 20);
        CHECK(t.ok + t.cancelled == 20);
    }

    /* キャンセルしたスキャンは FC_ERR_CANCELLED か FC_OK（間に合わなかった場合）で 1 回だけ完了する */
    {
        scan_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        waiter_init(&ctx.w);
        fc_task* task = NULL;
        REQUIRE_OK(fc_catalog_scan_async(cat, root, NULL, NULL, on_scan_done, &ctx, &task));
        fc_task_cancel(task);
        fc_task_release(task);
        CHECK(ctx.w.calls == 1);
        CHECK(ctx.status == FC_OK || ctx.status == FC_ERR_CANCELLED);
    }

    /* 現像: 開く → 現像可能 → レンダリング → 編集 → Undo → 保存して開き直す */
    {
        fc_editor* ed = NULL;
        REQUIRE_OK(fc_editor_create(cat, &ed));
        char previews[1024];
        snprintf(previews, sizeof previews, "%s/previews", tmp);
        REQUIRE_OK(fc_editor_set_preview_cache(ed, previews, (uint64_t)1 << 30));
        uint64_t usage = 1;
        REQUIRE_OK(fc_editor_preview_cache_usage(ed, &usage));
        CHECK(usage == 0);
        session_ctx sx;
        memset(&sx, 0, sizeof sx);
        waiter_init(&sx.w);
        fc_session* ses = NULL;
        REQUIRE_OK(fc_editor_open(ed, sony_id, 800, 1200, on_session, &sx, &ses));

        /* デコード前のレンダリングは NOT_READY（間に合ってしまった場合は OK） */
        enum { W = 1200, H = 900 };
        uint8_t* buf = (uint8_t*)malloc((size_t)W * H * 3);
        fc_render_request rq;
        memset(&rq, 0, sizeof rq);
        rq.mode = FC_RENDER_FIT;
        rq.max_width = W;
        rq.max_height = H;
        rq.buffer = buf;
        rq.stride = W * 3;
        rq.capacity = (size_t)W * H * 3;
        render_ctx rc;
        memset(&rc, 0, sizeof rc);
        waiter_init(&rc.w);
        CHECK(fc_session_render(ses, &rq, on_render, &rc) != 0);
        waiter_wait_calls(&rc.w, 1);
        CHECK(rc.status == FC_ERR_NOT_READY || rc.status == FC_OK);

        wait_ready(&sx);
        CHECK(sx.ready == 1);
        CHECK(sx.failed == 0);
        fc_session_info info;
        REQUIRE_OK(fc_session_get_info(ses, &info));
        CHECK(info.stage == FC_STAGE_READY);
        CHECK(info.output_width == 6024);
        CHECK(info.as_shot_temperature > 3000);
        CHECK(info.preview_display_p3 == 0);
        if (info.preview_width > 0) {
            size_t cap = (size_t)info.preview_width * info.preview_height * 3;
            uint8_t* pv = (uint8_t*)malloc(cap);
            REQUIRE_OK(fc_session_copy_preview(ses, pv, (size_t)info.preview_width * 3, cap));
            CHECK(fc_session_copy_preview(ses, pv, 3, cap) == FC_ERR_INVALID_ARGUMENT);
            free(pv);
        }

        CHECK(fc_session_render(ses, &rq, on_render, &rc) != 0);
        waiter_wait_calls(&rc.w, 2);
        CHECK(rc.status == FC_OK);
        CHECK(rc.result.width == 1200);
        CHECK(rc.result.height == 802);
        {
            uint32_t sum = 0;
            for (int i = 0; i < 256; ++i) sum += rc.result.histogram[1][i];
            CHECK(sum == 1200u * 802u);
        }

        fc_settings st;
        REQUIRE_OK(fc_session_get_settings(ses, &st));
        CHECK(st.exposure == 0.0);
        CHECK(st.wb_mode == FC_WB_AS_SHOT);
        CHECK(fc_session_can_undo(ses) == 0);
        fc_session_begin_change(ses);
        st.exposure = 0.3;
        REQUIRE_OK(fc_session_set_settings(ses, &st));
        st.exposure = 0.7;
        REQUIRE_OK(fc_session_set_settings(ses, &st));
        fc_session_end_change(ses);
        st.saturation = 25;
        st.vibrance = -40;
        st.whites = 10;
        st.clarity = 35;
        st.sharpness = 60;
        st.color_noise_reduction = 25;
        st.wb_mode = FC_WB_CUSTOM;
        st.temperature = 4000;
        st.tint = 10;
        REQUIRE_OK(fc_session_set_settings(ses, &st));
        CHECK(fc_session_undo(ses) == 1);
        REQUIRE_OK(fc_session_get_settings(ses, &st));
        CHECK(st.wb_mode == FC_WB_AS_SHOT);
        CHECK(st.exposure == 0.7);
        CHECK(fc_session_can_redo(ses) == 1);
        CHECK(fc_session_redo(ses) == 1);
        fc_session_close(ses);
        free(buf);
        fc_editor_destroy(ed);

        /* 閉じたときの現像結果が大きいプレビューのキャッシュに残る（破棄時に書き終える） */
        REQUIRE_OK(fc_editor_create(cat, &ed));
        REQUIRE_OK(fc_editor_set_preview_cache(ed, previews, (uint64_t)1 << 30));
        REQUIRE_OK(fc_editor_preview_cache_usage(ed, &usage));
        CHECK(usage > 0);
        REQUIRE_OK(fc_editor_set_preview_cache_limit(ed, 0));
        REQUIRE_OK(fc_editor_preview_cache_usage(ed, &usage));
        CHECK(usage == 0);
        REQUIRE_OK(fc_editor_clear_preview_cache(ed));
        CHECK(fc_editor_preview_cache_usage(NULL, &usage) == FC_ERR_INVALID_ARGUMENT);

        /* 開き直すと編集が復元される */
        REQUIRE_OK(fc_editor_open(ed, sony_id, 256, 512, NULL, NULL, &ses));
        REQUIRE_OK(fc_session_get_settings(ses, &st));
        CHECK(st.exposure == 0.7);
        CHECK(st.wb_mode == FC_WB_CUSTOM);
        CHECK(st.temperature == 4000);
        CHECK(st.saturation == 25);
        CHECK(st.vibrance == -40);
        CHECK(st.whites == 10);
        CHECK(st.blacks == 0);
        CHECK(st.clarity == 35);
        CHECK(st.sharpness == 60);
        CHECK(st.noise_reduction == 0);
        CHECK(st.color_noise_reduction == 25);
        CHECK(fc_session_can_undo(ses) == 0);
        fc_session_close(ses);
        fc_editor_destroy(ed);
        fc_session_close(NULL);
        fc_editor_destroy(NULL);
    }

    /* 書き出し: 1 枚目は成功、存在しない写真は失敗、完了は 1 回 */
    {
        char dest[1024];
        snprintf(dest, sizeof dest, "%s/export", tmp);
        mkdir(dest, 0755);
        export_ctx ex;
        memset(&ex, 0, sizeof ex);
        waiter_init(&ex.w);
        fc_export_options eo;
        memset(&eo, 0, sizeof eo);
        eo.format = FC_EXPORT_JPEG;
        eo.quality = 85;
        eo.long_edge = 640;
        eo.dest_dir = dest;
        int64_t ids2[2] = {canon_id, 424242};
        fc_task* task = NULL;
        REQUIRE_OK(fc_export_start(cat, ids2, 2, &eo, on_export_item, on_export_done, &ex, &task));
        waiter_wait_calls(&ex.w, 1);
        fc_task_release(task);
        CHECK(ex.status == FC_OK);
        CHECK(ex.ok == 1);
        CHECK(ex.failed == 1);
        char expect[1100];
        snprintf(expect, sizeof expect, "%s/IMG_0001.jpg", dest);
        CHECK(access(expect, R_OK) == 0);
        eo.dest_dir = NULL;
        CHECK(fc_export_start(cat, ids2, 2, &eo, NULL, on_export_done, &ex, &task) == FC_ERR_INVALID_ARGUMENT);
    }

    /* 現像のプリセット（6.3 章）: 保存・一覧・重ね・カタログへの適用。切り取りは含めない */
    {
        char pdir[1100], bdir[1100];
        snprintf(pdir, sizeof pdir, "%s/presets", tmp);
        snprintf(bdir, sizeof bdir, "%s/builtin-presets", tmp);
        fc_presets* presets = NULL;
        CHECK(fc_presets_open(NULL, NULL, &presets) == FC_ERR_INVALID_ARGUMENT);
        REQUIRE_OK(fc_presets_open(pdir, bdir, &presets));
        fc_preset_array* list = NULL;
        REQUIRE_OK(fc_presets_list(presets, &list));
        CHECK(list->count == 0);
        fc_preset_array_free(list);

        fc_settings ps;
        fc_settings_init(&ps);
        ps.exposure = 0.8;
        ps.clarity = 25;
        ps.rotate90 = 1;
        ps.crop_w = 0.5;
        fc_string* id = NULL;
        REQUIRE_OK(fc_presets_save(presets, "Landscape", &ps, &id));
        CHECK(strncmp(id->value, "user:", 5) == 0);
        REQUIRE_OK(fc_presets_list(presets, &list));
        CHECK(list->count == 1);
        CHECK(strcmp(list->items[0].name, "Landscape") == 0);
        CHECK(list->items[0].builtin == 0);
        fc_preset_array_free(list);

        fc_settings base, out;
        fc_settings_init(&base);
        base.rotate90 = 3;
        base.contrast = 40;
        REQUIRE_OK(fc_presets_apply(presets, id->value, &base, &out));
        CHECK(out.exposure == 0.8);
        CHECK(out.clarity == 25);
        CHECK(out.contrast == 0.0);   /* 調整はプリセットの値 */
        CHECK(out.rotate90 == 3);     /* 切り取りなどは base のまま */
        CHECK(out.crop_w == 1.0);
        CHECK(fc_presets_apply(presets, "user:none", &base, &out) == FC_ERR_NOT_FOUND);

        int64_t one[1] = {canon_id};
        REQUIRE_OK(fc_catalog_apply_preset(cat, presets, id->value, one, 1));
        REQUIRE_OK(fc_catalog_flush(cat));
        CHECK(fc_catalog_apply_preset(cat, presets, "user:none", one, 1) == FC_ERR_NOT_FOUND);

        CHECK(fc_presets_delete(presets, "builtin:x") == FC_ERR_INVALID_ARGUMENT);
        REQUIRE_OK(fc_presets_delete(presets, id->value));
        fc_string_free(id);
        fc_presets_close(presets);
        fc_presets_close(NULL);
    }

    fc_catalog_close(cat);
    fc_catalog_close(NULL);
    fc_root_array_free(NULL);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    (void)system(cmd);

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("C API tests passed\n");
    return failures ? 1 : 0;
}
