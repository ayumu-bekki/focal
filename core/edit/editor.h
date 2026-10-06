#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "edit/settings.h"
#include "edit/undo_stack.h"
#include "imaging/gpu_renderer.h"
#include "imaging/output_transform.h"
#include "imaging/raw_decoder.h"
#include "imaging/renderer.h"
#include "thumbs/thumbnail.h"
#include "util/image.h"

namespace focal {

class Catalog;
class Editor;

// 現像ビューの要求（5.3 章・5.7 章）。出力は Display P3 の 8-bit RGB（ADR-06）で、呼び出し側のバッファに書く。
struct RenderRequest {
    enum class Mode {
        Fit,     // 出力画像全体を max_width × max_height に収める（プロキシから）
        Region,  // 100% 表示: 出力画像（フル解像度）の一部を 1 画素 = 1 画素で（フル解像度から）
    };
    Mode mode = Mode::Fit;
    int max_width = 0;
    int max_height = 0;
    double region_x = 0;  // Region: 出力画像の座標での左上
    double region_y = 0;
    int region_width = 0;
    int region_height = 0;
    PixelLayout layout = PixelLayout::Rgb8;
    bool ignore_crop = false;  // クロップモード: (d) を適用せず全体を描く（5.6 章）。座標はキャンバス
    uint8_t* buffer = nullptr;  // 呼び出し側が確保。少なくとも (幅 × 画素のバイト数) × 高さ（stride ずつ）
    size_t stride = 0;
    size_t capacity = 0;        // buffer のバイト数
};

struct RenderResult {
    int width = 0;   // 書き込んだ大きさ
    int height = 0;
    double scale = 0;  // 出力画像（フル解像度）に対する縮尺。Region なら 1
    double region_x = 0;  // 実際に描いた範囲（出力画像の座標。端で切り詰めた後）
    double region_y = 0;
    std::array<std::array<uint32_t, 256>, 3> histogram{};  // 表示用出力（Display P3）の R/G/B
};

enum class RenderStatus { Ok, Cancelled, NotReady, Failed };

struct SessionInfo {
    enum class Stage { Opening, Preview, Ready, Failed };
    Stage stage = Stage::Opening;
    int oriented_width = 0;   // 向き補正後（ジオメトリ前）
    int oriented_height = 0;
    int output_width = 0;     // ジオメトリ（回転・クロップ）適用後のフル解像度
    int output_height = 0;
    int canvas_width = 0;     // 回転・傾き補正後、クロップ前（クロップ枠の座標の基準）
    int canvas_height = 0;
    double as_shot_temperature = 0;  // Ready 以降のみ（それまでは 0）
    double as_shot_tint = 0;
    int preview_width = 0;
    int preview_height = 0;
    // プレビューが Focal の現像結果のキャッシュ（Display P3、ジオメトリ適用済み）なら true。
    // false ならカメラの埋め込みプレビュー（sRGB、ジオメトリ前）
    bool preview_display_p3 = false;
    std::string error;
};

// 写真 1 枚の編集セッション。メソッドはどのスレッドから呼んでもよい。
class EditSession {
public:
    using Stage = SessionInfo::Stage;
    enum class Event { Preview, Ready, Failed };
    using EventCallback = std::function<void(Event, const std::string& message)>;

    int64_t photo_id() const { return photo_id_; }
    SessionInfo info() const;
    // 写真のメタデータ（カメラ・レンズ・焦点距離など）。メタデータの読み込み前なら false
    bool metadata(RawMetadata& out) const;
    // 埋め込みプレビュー（sRGB、向き補正済み）を呼び出し側のバッファにコピーする（大きさは info() のとおり）
    bool copy_preview(uint8_t* dst, size_t stride, size_t capacity) const;

    Settings settings() const;
    // スライダーのドラッグなど、まとめて 1 回の Undo にしたい変更の開始と終了（9.3 章）
    void begin_change();
    void end_change();
    // begin_change と end_change の間なら途中経過、そうでなければそれ自体が 1 回の変更になる
    void set_settings(Settings s);
    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;

    // プロキシの長辺（ビューの長辺 × backingScaleFactor、上限 3840）。次の Fit レンダリングで作り直す
    void set_proxy_long_edge(int long_edge);

private:
    friend class Editor;
    EditSession(Editor& editor, int64_t photo_id, Settings settings, EventCallback cb);

    void emit(Event ev, const std::string& message);
    void mark_dirty_locked();
    UndoStack& undo_stack() const;

    Editor& editor_;
    const int64_t photo_id_;

    mutable std::mutex mutex_;
    Stage stage_ = Stage::Opening;
    std::string error_;
    Settings settings_;
    std::optional<Settings> change_before_;
    RawMetadata meta_;
    bool have_meta_ = false;
    ImageU8 preview_;
    bool preview_display_p3_ = false;
    std::shared_ptr<const DecodedRaw> raw_;
    std::shared_ptr<const ImageF> proxy_;
    int proxy_long_edge_ = 2560;
    double as_shot_temperature_ = 0, as_shot_tint_ = 0;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_change_;

    // コールバックは closed_ を見てから呼ぶ。close() はこのロックを取るので、実行中のコールバックの終了を待つ
    std::mutex callback_mutex_;
    EventCallback callback_;
    bool closed_ = false;
};

// 現像ビューア全体（デコード・プレビュー・レンダリング・保存のスレッドを持つ）。
class Editor {
public:
    using RenderCallback = std::function<void(RenderStatus, const RenderResult&)>;

    static constexpr int kMaxProxyLongEdge = 3840;
    static constexpr auto kSaveDelay = std::chrono::milliseconds(500);  // 7.4 章

    explicit Editor(Catalog& catalog);
    // 未保存の編集と、閉じた写真の現像結果のキャッシュを書き込み、スレッドを止める
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    // 写真を開く（5.3 章）。すぐ戻り、プレビュー → 現像可能の順にコールバックする。
    // 保存済みの編集を読み込む。先読み済みならすぐ現像可能になる
    std::shared_ptr<EditSession> open(int64_t photo_id, int preview_long_edge, int proxy_long_edge,
                                      EditSession::EventCallback callback);
    // 次の写真を 1 枚だけ先読みする（11.2 章: 表示中 1 枚 + 先読み 1 枚まで）
    void prefetch(int64_t photo_id);
    // 編集をすぐ保存し、以降のコールバックを止める。実行中のコールバックがあれば終了を待つ
    void close(const std::shared_ptr<EditSession>& session);

    // latest-wins（4.2 章）: 新しい要求が来たら、古い要求は行ブロックの切れ目で打ち切って Cancelled を返す。
    // コールバックはレンダースレッドから必ず 1 回呼ばれる。戻り値は要求の世代番号
    uint64_t render(const std::shared_ptr<EditSession>& session, const RenderRequest& request, RenderCallback cb);

    // テスト用: 保存待ちの編集をすべてすぐ書き込む
    void flush_saves();

    // このセッションの編集をすぐ保存し、書き込みが終わるまで待つ（書き出しの直前など）
    void save_now(const std::shared_ptr<EditSession>& session);

    // 編集を保存するときと写真を閉じるとき、デコード済みのデータから現像結果のサムネイルを作ってキャッシュに入れる
    // （10 章）。作り終えたら on_updated(photo_id) を保存スレッドから呼ぶ（編集がなくなった場合も呼ぶ）
    void set_thumbnail_cache(std::filesystem::path dir, std::function<void(int64_t)> on_updated);

    // 表示用の大きいプレビューのキャッシュ。写真を閉じるとき現像結果を入れ、次に開いたとき埋め込みプレビューより先に出す
    void set_preview_cache(std::filesystem::path dir, uint64_t limit_bytes);
    void set_preview_cache_limit(uint64_t limit_bytes);
    uint64_t preview_cache_usage();
    void clear_preview_cache();

    // 表示用の GPU レンダラー（v3.14）。渡さなければ CPU で描く。GPU で描けない設定やエラーのときも CPU で描く
    void set_gpu_renderer(std::unique_ptr<GpuRenderer> gpu);
    // 使っている GPU の名前。CPU なら空
    std::string gpu_name() const;

private:
    friend class EditSession;

    struct DecodeJob {
        std::weak_ptr<EditSession> session;  // 開いた写真（空なら先読み）
        int64_t photo_id = 0;
    };
    struct RenderJob {
        std::shared_ptr<EditSession> session;
        RenderRequest request;
        RenderCallback cb;
        uint64_t generation = 0;
    };
    struct Prefetched {
        int64_t photo_id = 0;
        std::shared_ptr<const DecodedRaw> raw;
    };

    void preview_loop();
    void decode_loop();
    void render_loop();
    void save_loop();

    void run_preview(const std::shared_ptr<EditSession>& s, int long_edge);
    void finish_decode(const std::shared_ptr<EditSession>& s, std::shared_ptr<const DecodedRaw> raw);
    void run_render(RenderJob& job);
    // GPU（あれば）か CPU で描き、ヒストグラムも作る。打ち切られたら false
    std::shared_ptr<const LensMaps> lens_maps_for(const LensSettings& ls, const std::shared_ptr<const DecodedRaw>& raw);
    bool draw(const GpuSource& src, const GeometryPlan& plan, double scale, PointD origin, const ColorPipeline& pipeline,
              const RenderRequest& rq, RenderResult& result, const CancelToken& cancel);
    void save(EditSession& s);
    struct CacheJob {
        std::shared_ptr<EditSession> session;
        Settings settings;
        bool closing = false;  // 閉じるとき: 大きいプレビューも作る。編集の行は書き込まない
    };
    void update_caches(const CacheJob& job);
    void enqueue_cache_job(const std::shared_ptr<EditSession>& s, Settings settings, bool closing);
    std::shared_ptr<PreviewCache> preview_cache();

    Catalog& catalog_;
    const OutputTransform display_;  // 作業色空間 → Display P3（8.1 章）
    UndoHistory history_;
    std::mutex history_mutex_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::deque<std::pair<std::weak_ptr<EditSession>, int>> preview_queue_;
    std::optional<DecodeJob> open_job_;      // 開いた写真のデコード（最新の 1 件だけ）
    std::optional<int64_t> prefetch_job_;    // 先読み（最新の 1 件だけ）
    std::optional<Prefetched> prefetched_;
    int64_t decoding_photo_ = 0;             // デコード中の写真（先読みも含む）
    std::atomic<bool> decode_cancel_{false};
    std::optional<RenderJob> render_job_;
    std::atomic<uint64_t> render_generation_{0};
    // 直近のレンズ補正マップ（描画スレッドだけが触る）
    LensSettings lens_cache_key_;
    std::weak_ptr<const DecodedRaw> lens_cache_raw_;
    std::shared_ptr<const LensMaps> lens_cache_;
    std::vector<std::weak_ptr<EditSession>> sessions_;
    // 編集後のサムネイル（保存スレッドで作る）
    std::optional<ThumbnailCache> thumbnail_cache_;
    std::function<void(int64_t)> on_thumbnail_;
    std::deque<CacheJob> cache_jobs_;
    std::unique_ptr<GpuRenderer> gpu_;  // レンダースレッドだけが使う（設定は最初に 1 回）
    std::shared_ptr<PreviewCache> preview_cache_;

    std::thread preview_thread_, decode_thread_, render_thread_, save_thread_;
};

} // namespace focal
