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
        REQUIRE_OK(fc_catalog_create_album(cat, "\xe6\x97\x85\xe8\xa1\x8c", &album)); /* 旅行 */
        CHECK(fc_catalog_create_album(cat, "\xe6\x97\x85\xe8\xa1\x8c", NULL) == FC_ERR_INVALID_ARGUMENT);
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
