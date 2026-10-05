#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cmath>
#include <future>
#include <numeric>

#include "catalog/catalog.h"
#include "edit/editor.h"
#include "edit/undo_stack.h"
#include "imaging/image_io.h"
#include "thumbs/thumbnail.h"
#include "thumbs/thumbnail_service.h"
#include "test_util.h"
#include "util/file.h"

using namespace focal;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

Settings with_exposure(double ev) {
    Settings s;
    s.exposure = ev;
    return s;
}

// セッションのイベントを待つ
struct Events {
    std::mutex m;
    std::condition_variable cv;
    std::vector<EditSession::Event> events;

    EditSession::EventCallback callback() {
        return [this](EditSession::Event e, const std::string&) {
            std::lock_guard lock(m);
            events.push_back(e);
            cv.notify_all();
        };
    }

    bool wait_for(EditSession::Event e, std::chrono::milliseconds timeout = 30s) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return std::find(events.begin(), events.end(), e) != events.end(); });
    }
};

// レンダリングの結果を待つ
struct RenderWait {
    std::mutex m;
    std::condition_variable cv;
    int calls = 0;
    std::vector<RenderStatus> statuses;
    RenderResult last;

    Editor::RenderCallback callback() {
        return [this](RenderStatus st, const RenderResult& r) {
            std::lock_guard lock(m);
            ++calls;
            statuses.push_back(st);
            if (st == RenderStatus::Ok) last = r;
            cv.notify_all();
        };
    }

    void wait_calls(int n) {
        std::unique_lock lock(m);
        cv.wait_for(lock, 30s, [&] { return calls >= n; });
    }
};

struct Fixture {
    TempDir dir{"focal-editor"};
    std::unique_ptr<Catalog> catalog;
    int64_t sony = 0, canon = 0;

    Fixture() {
        const fs::path data(FOCAL_TEST_DATA_DIR);
        fs::create_directories(dir / "lib");
        fs::copy_file(data / "sony_ilce7m3.ARW", dir / "lib" / "sony.ARW");
        fs::copy_file(data / "canon_eos_m50.CR3", dir / "lib" / "canon.CR3");
        catalog = Catalog::open(dir / "c.sqlite");
        catalog->scan_root(catalog->add_root(dir / "lib"));
        for (const auto& p : catalog->query({})) (p.file_name == "sony.ARW" ? sony : canon) = p.id;
    }
};

bool have_data() { return fs::exists(fs::path(FOCAL_TEST_DATA_DIR) / "sony_ilce7m3.ARW"); }

double mean(const std::vector<uint8_t>& buf, int w, int h, size_t stride) {
    double sum = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w * 3; ++x) sum += buf[y * stride + x];
    return sum / (w * h * 3.0);
}

} // namespace

TEST_CASE("Undo スタック: 変更前と変更後の組", "[undo]") {
    UndoStack st;
    CHECK_FALSE(st.can_undo());
    st.push(with_exposure(0), with_exposure(1));
    st.push(with_exposure(1), with_exposure(2));
    CHECK(st.undo()->exposure == 1);
    CHECK(st.undo()->exposure == 0);
    CHECK_FALSE(st.undo());
    CHECK(st.redo()->exposure == 1);
    // 途中で新しい変更をすると redo できる分は消える
    st.push(with_exposure(1), with_exposure(5));
    CHECK_FALSE(st.can_redo());
    CHECK(st.undo()->exposure == 1);
}

TEST_CASE("Undo 履歴: 直近 20 枚分だけ持つ", "[undo]") {
    UndoHistory h;
    for (int64_t id = 1; id <= 25; ++id) h.stack(id).push(with_exposure(0), with_exposure(1));
    CHECK(h.stack(25).can_undo());
    CHECK(h.stack(6).can_undo());
    CHECK_FALSE(h.stack(1).can_undo());  // 捨てられている
}

TEST_CASE("DB に行が要らない編集の判定", "[settings]") {
    CHECK(settings_need_no_row(Settings{}));
    CHECK_FALSE(settings_need_no_row(with_exposure(1)));
    Settings s = settings_from_json(R"({"exposure": 0, "wb": {"mode": "asShot"}, "geometry": {"crop": {"x": 0}}})");
    CHECK(settings_need_no_row(s));
    Settings u = settings_from_json(R"({"exposure": 0, "futureKey": 1})");
    CHECK_FALSE(settings_need_no_row(u));  // 未知のキーは保持する
}

TEST_CASE("Editor: 開く → プレビュー → 現像可能、Fit と 100% のレンダリング", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    Editor editor(*f.catalog);
    Events ev;
    auto s = editor.open(f.sony, 1200, 1600, ev.callback());

    // 現像可能になる前のレンダリングは NotReady（プレビューは出ていてもよい）
    REQUIRE(ev.wait_for(EditSession::Event::Ready));
    {
        std::lock_guard lock(ev.m);
        // プレビューが出るなら現像可能より前
        if (ev.events.size() == 2) CHECK(ev.events[0] == EditSession::Event::Preview);
    }
    const SessionInfo info = s->info();
    CHECK(info.stage == SessionInfo::Stage::Ready);
    CHECK(info.output_width == 6024);
    CHECK(info.output_height == 4024);
    CHECK(info.as_shot_temperature > 3000);
    CHECK(info.as_shot_temperature < 7000);
    CHECK(info.preview_width == 1200);

    std::vector<uint8_t> pv(static_cast<size_t>(info.preview_width) * info.preview_height * 3);
    CHECK(s->copy_preview(pv.data(), info.preview_width * 3, pv.size()));
    CHECK_FALSE(s->copy_preview(pv.data(), 10, pv.size()));

    // Fit
    const int W = 1000, H = 800;
    std::vector<uint8_t> buf(static_cast<size_t>(W) * H * 3);
    RenderRequest rq;
    rq.max_width = W;
    rq.max_height = H;
    rq.buffer = buf.data();
    rq.stride = W * 3;
    rq.capacity = buf.size();
    RenderWait rw;
    editor.render(s, rq, rw.callback());
    rw.wait_calls(1);
    REQUIRE(rw.statuses[0] == RenderStatus::Ok);
    CHECK(rw.last.width == 1000);
    CHECK(rw.last.height == 668);
    CHECK(std::accumulate(rw.last.histogram[1].begin(), rw.last.histogram[1].end(), 0u) == 1000u * 668u);
    const double base = mean(buf, rw.last.width, rw.last.height, rq.stride);

    // 露出を上げると明るくなる
    s->set_settings(with_exposure(1.0));
    editor.render(s, rq, rw.callback());
    rw.wait_calls(2);
    CHECK(mean(buf, rw.last.width, rw.last.height, rq.stride) > base + 10);

    // 100%（Region）: 出力画像の端で切り詰める
    RenderRequest region = rq;
    region.mode = RenderRequest::Mode::Region;
    region.region_x = 6024 - 400;
    region.region_y = 100;
    region.region_width = W;
    region.region_height = H;
    editor.render(s, region, rw.callback());
    rw.wait_calls(3);
    REQUIRE(rw.statuses[2] == RenderStatus::Ok);
    CHECK(rw.last.width == 400);
    CHECK(rw.last.height == 800);
    CHECK(rw.last.scale == 1.0);
    CHECK(rw.last.region_x == 6024 - 400);

    // バッファが小さすぎれば失敗
    RenderRequest small = rq;
    small.capacity = 100;
    editor.render(s, small, rw.callback());
    rw.wait_calls(4);
    CHECK(rw.statuses[3] == RenderStatus::Failed);
    editor.close(s);
}

TEST_CASE("Editor: latest-wins で古い要求は打ち切られ、どの要求にも 1 回だけ返る", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    Editor editor(*f.catalog);
    Events ev;
    auto s = editor.open(f.sony, 512, 2560, ev.callback());
    REQUIRE(ev.wait_for(EditSession::Event::Ready));

    const int W = 2560, H = 1440;
    std::vector<std::vector<uint8_t>> bufs(20, std::vector<uint8_t>(static_cast<size_t>(W) * H * 3));
    RenderWait rw;
    for (int i = 0; i < 20; ++i) {
        RenderRequest rq;
        rq.max_width = W;
        rq.max_height = H;
        rq.buffer = bufs[i].data();
        rq.stride = W * 3;
        rq.capacity = bufs[i].size();
        s->set_settings(with_exposure(i * 0.1));
        editor.render(s, rq, rw.callback());
    }
    rw.wait_calls(20);
    std::this_thread::sleep_for(100ms);
    CHECK(rw.calls == 20);
    CHECK(rw.statuses.back() == RenderStatus::Ok);
    CHECK(std::count(rw.statuses.begin(), rw.statuses.end(), RenderStatus::Cancelled) >= 10);
    editor.close(s);
}

TEST_CASE("Editor: ドラッグは 1 回の Undo、保存と再読み込み", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    {
        Editor editor(*f.catalog);
        auto s = editor.open(f.canon, 512, 1024, nullptr);
        CHECK_FALSE(s->can_undo());

        // スライダーのドラッグ: 途中経過は Undo に積まない
        s->begin_change();
        for (double ev : {0.2, 0.5, 0.8}) s->set_settings(with_exposure(ev));
        CHECK(s->can_undo());
        s->end_change();
        Settings c = s->settings();
        c.contrast = 30;
        s->set_settings(c);  // ボタンなどは 1 回で 1 コマンド

        CHECK(s->undo());
        CHECK(s->settings().contrast == 0);
        CHECK(s->settings().exposure == 0.8);
        CHECK(s->undo());
        CHECK(s->settings().exposure == 0.0);
        CHECK_FALSE(s->undo());
        CHECK(s->redo());
        CHECK(s->settings().exposure == 0.8);

        // 500ms のデバウンスの後に保存される（7.4 章）。遅い環境（CI の Windows）でも間に合うよう、最大 8 秒まで待つ
        CHECK_FALSE(f.catalog->edit_json(f.canon));
        for (int i = 0; i < 80 && !f.catalog->edit_json(f.canon); ++i) {
            std::this_thread::sleep_for(100ms);
            f.catalog->flush();
        }
        REQUIRE(f.catalog->edit_json(f.canon));
        editor.close(s);
    }
    {
        // 「再起動」後も編集が復元される
        Editor editor(*f.catalog);
        auto s = editor.open(f.canon, 512, 1024, nullptr);
        CHECK(s->settings().exposure == 0.8);
        CHECK_FALSE(s->can_undo());  // Undo はセッション内のみ（永続化しない）
        // 既定値に戻すと行が消える
        s->set_settings(Settings{});
        editor.close(s);
        f.catalog->flush();
        CHECK_FALSE(f.catalog->edit_json(f.canon));
    }
}

TEST_CASE("Editor: 先読みした写真はすぐ現像可能になる", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    Editor editor(*f.catalog);
    auto time_to_ready = [&] {
        Events ev;
        const auto t0 = std::chrono::steady_clock::now();
        auto s = editor.open(f.canon, 512, 1024, ev.callback());
        REQUIRE(ev.wait_for(EditSession::Event::Ready));
        const auto ms = std::chrono::steady_clock::now() - t0;
        editor.close(s);
        return std::chrono::duration_cast<std::chrono::milliseconds>(ms);
    };
    // 先読みなし（デコードから）と先読みありを同じビルドで比べる（サニタイザ付きでは全体が遅いため）
    const auto cold = time_to_ready();
    editor.prefetch(f.canon);
    std::this_thread::sleep_for(cold * 3 + 1000ms);  // 先読みが終わるのを待つ（遅い CI でも足りるよう、余裕をとる）
    const auto warm = time_to_ready();
    INFO("cold " << cold.count() << " ms, warm " << warm.count() << " ms");
    CHECK(warm < cold / 2);

    // 閉じたセッションにはコールバックが来ない
    Events ev2;
    auto s2 = editor.open(f.sony, 512, 1024, ev2.callback());
    editor.close(s2);
    // close が戻るまでに届いたイベントは正当（遅い環境では open の間にプレビューが届く）。戻ったあとは増えない
    size_t after_close;
    {
        std::lock_guard lock(ev2.m);
        after_close = ev2.events.size();
    }
    std::this_thread::sleep_for(1500ms);
    std::lock_guard lock(ev2.m);
    CHECK(ev2.events.size() == after_close);
}

TEST_CASE("Editor: 回転・傾き補正・クロップ後の 100% 表示は全体を等倍で描いた画像と同じ位置を表示する", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    Editor editor(*f.catalog);
    Events ev;
    auto s = editor.open(f.sony, 512, 1024, ev.callback());
    REQUIRE(ev.wait_for(EditSession::Event::Ready));
    Settings st;
    st.geometry.rotate90 = 1;
    st.geometry.straighten = 4;
    st.geometry.crop = {0.15, 0.1, 0.6, 0.7};
    s->set_settings(st);
    const SessionInfo info = s->info();
    REQUIRE(info.output_width == static_cast<int>(std::lround(0.6 * 4024)));  // 回転後のキャンバスは 4024 × 6024
    REQUIRE(info.output_height == static_cast<int>(std::lround(0.7 * 6024)));
    CHECK(info.canvas_width == 4024);
    CHECK(info.canvas_height == 6024);

    auto render = [&](RenderRequest rq, std::vector<uint8_t>& buf) {
        rq.layout = PixelLayout::Bgrx8;
        buf.assign(rq.stride * (rq.mode == RenderRequest::Mode::Fit ? rq.max_height : rq.region_height), 0);
        rq.buffer = buf.data();
        rq.capacity = buf.size();
        RenderWait rw;
        editor.render(s, rq, rw.callback());
        rw.wait_calls(1);
        REQUIRE(rw.statuses[0] == RenderStatus::Ok);
        return rw.last;
    };

    // 出力画像全体を等倍で
    const int ow = info.output_width, oh = info.output_height;
    std::vector<uint8_t> whole;
    RenderRequest all;
    all.mode = RenderRequest::Mode::Region;
    all.region_width = ow;
    all.region_height = oh;
    all.stride = static_cast<size_t>(ow) * 4;
    const RenderResult wr = render(all, whole);
    REQUIRE(wr.width == ow);

    // その一部を 100% 表示として描くと、同じ位置の画素と一致する
    std::vector<uint8_t> part;
    RenderRequest region;
    region.mode = RenderRequest::Mode::Region;
    region.region_x = 700;
    region.region_y = 1900;
    region.region_width = 640;
    region.region_height = 480;
    region.stride = 640 * 4;
    const RenderResult pr = render(region, part);
    REQUIRE(pr.width == 640);
    int max_diff = 0;
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 640 * 4; ++x) {
            const int a = part[y * region.stride + x];
            const int b = whole[(static_cast<size_t>(y) + 1900) * all.stride + 700 * 4 + x];
            max_diff = std::max(max_diff, std::abs(a - b));
        }
    CHECK(max_diff <= 1);

    // クロップモード: クロップを適用せずキャンバス全体を描く
    std::vector<uint8_t> canvas;
    RenderRequest fit;
    fit.max_width = 800;
    fit.max_height = 1200;
    fit.stride = 800 * 4;
    fit.ignore_crop = true;
    const RenderResult cr = render(fit, canvas);
    // 4024 × 6024 を 800 × 1200 に収めると幅が効く
    CHECK(cr.width == 800);
    CHECK(cr.height == static_cast<int>(std::lround(6024 * 800.0 / 4024)));
    editor.close(s);
}

TEST_CASE("Editor: 編集を保存すると編集後のサムネイルを作り、サムネイルの要求はそれを返す", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    const ThumbnailCache cache(f.dir / "thumbs");
    std::mutex m;
    std::condition_variable cv;
    std::vector<int64_t> updated;
    {
        Editor editor(*f.catalog);
        editor.set_thumbnail_cache(f.dir / "thumbs", [&](int64_t id) {
            std::lock_guard lock(m);
            updated.push_back(id);
            cv.notify_all();
        });
        Events ev;
        auto s = editor.open(f.canon, 512, 1024, ev.callback());
        REQUIRE(ev.wait_for(EditSession::Event::Ready));
        Settings st;
        st.exposure = 1.5;
        st.geometry.rotate90 = 1;
        s->set_settings(st);
        editor.close(s);
        std::unique_lock lock(m);
        REQUIRE(cv.wait_for(lock, 10s, [&] { return !updated.empty(); }));
        CHECK(updated[0] == f.canon);
    }
    const PhotoRecord p = *f.catalog->photo(f.canon);
    Settings st;
    st.exposure = 1.5;
    st.geometry.rotate90 = 1;
    const std::string key = rendered_key(p.path, p.file_size, p.file_mtime, st);
    CHECK(key != thumbnail_key(p.path, p.file_size, p.file_mtime));
    REQUIRE(cache.contains(key));
    const LoadedImage img = read_jpeg(cache.path_for(key));
    CHECK(img.height == 512);  // 90° 回転して縦長
    CHECK(img.width < 512);

    // サムネイルの要求は編集後のサムネイルを返す
    ThumbnailService service(*f.catalog, f.dir / "thumbs", 1);
    std::promise<std::string> got;
    service.request(f.canon, [&](uint64_t, ThumbnailService::Result r, const std::string& path) {
        got.set_value(r == ThumbnailService::Result::Ok ? path : "");
    });
    CHECK(got.get_future().get() == path_to_utf8(cache.path_for(key)));
}

TEST_CASE("Editor: 閉じると現像結果のサムネイルと大きいプレビューを残し、次に開くとそれを先に出す", "[editor][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    const ThumbnailCache cache(f.dir / "thumbs");
    std::mutex m;
    std::condition_variable cv;
    std::vector<int64_t> updated;
    Editor editor(*f.catalog);
    editor.set_thumbnail_cache(f.dir / "thumbs", [&](int64_t id) {
        std::lock_guard lock(m);
        updated.push_back(id);
        cv.notify_all();
    });
    editor.set_preview_cache(f.dir / "previews", 1ull << 30);
    CHECK(editor.preview_cache_usage() == 0);

    // 編集せずに開いて閉じる
    {
        Events ev;
        auto s = editor.open(f.canon, 512, 1024, ev.callback());
        REQUIRE(ev.wait_for(EditSession::Event::Ready));
        CHECK_FALSE(s->info().preview_display_p3);  // 初回は埋め込みプレビュー
        editor.close(s);
        std::unique_lock lock(m);
        REQUIRE(cv.wait_for(lock, 10s, [&] { return !updated.empty(); }));
    }
    const PhotoRecord p = *f.catalog->photo(f.canon);
    const std::string key = rendered_key(p.path, p.file_size, p.file_mtime, Settings{});
    CHECK(key != thumbnail_key(p.path, p.file_size, p.file_mtime));
    CHECK(cache.contains(key));
    CHECK_FALSE(f.catalog->edit_json(f.canon));  // 編集の行は作らない
    CHECK(editor.preview_cache_usage() > 0);

    // サムネイルの要求は現像結果を返す
    {
        ThumbnailService service(*f.catalog, f.dir / "thumbs", 1);
        std::promise<std::string> got;
        service.request(f.canon, [&](uint64_t, ThumbnailService::Result r, const std::string& path) {
            got.set_value(r == ThumbnailService::Result::Ok ? path : "");
        });
        CHECK(got.get_future().get() == path_to_utf8(cache.path_for(key)));
    }

    // 開き直すと、大きいプレビューのキャッシュ（P3、プロキシの大きさ）が出る
    {
        Events ev;
        auto s = editor.open(f.canon, 512, 1024, ev.callback());
        REQUIRE(ev.wait_for(EditSession::Event::Preview));
        const SessionInfo i = s->info();
        CHECK(i.preview_display_p3);
        CHECK(std::max(i.preview_width, i.preview_height) == 1024);
        editor.close(s);
    }

    editor.clear_preview_cache();
    CHECK(editor.preview_cache_usage() == 0);
}
