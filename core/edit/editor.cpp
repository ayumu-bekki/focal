#include "edit/editor.h"

#include <algorithm>
#include <cmath>

#include "catalog/catalog.h"
#include "imaging/color_pipeline.h"
#include "imaging/geometry.h"
#include "imaging/libraw_util.h"
#include "imaging/renderer.h"
#include "imaging/resample.h"
#include "imaging/white_balance.h"
#include "thumbs/thumbnail.h"
#include "util/file.h"
#include "util/error.h"

namespace focal {

namespace {

Settings load_settings(Catalog& catalog, int64_t photo_id) {
    if (auto json = catalog.edit_json(photo_id)) {
        try {
            return settings_from_json(*json);
        } catch (const Error&) {
            // 壊れた JSON は既定値として扱う（元の行は保存するまで残る）
        }
    }
    return {};
}

std::optional<std::string> to_row(const Settings& s) {
    if (settings_need_no_row(s)) return std::nullopt;
    return settings_to_json(s);
}

int bytes_per_pixel(PixelLayout l) { return l == PixelLayout::Bgrx8 ? 4 : 3; }

void fill_histogram(const uint8_t* buf, size_t stride, int w, int h, PixelLayout layout, RenderResult& r) {
    for (auto& ch : r.histogram) ch.fill(0);
    const int bpp = bytes_per_pixel(layout);
    const int ri = layout == PixelLayout::Bgrx8 ? 2 : 0, bi = layout == PixelLayout::Bgrx8 ? 0 : 2;
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = buf + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x, p += bpp) {
            ++r.histogram[0][p[ri]];
            ++r.histogram[1][p[1]];
            ++r.histogram[2][p[bi]];
        }
    }
}

bool fits(const RenderRequest& rq, int w, int h) {
    const size_t row = static_cast<size_t>(w) * bytes_per_pixel(rq.layout);
    return rq.buffer && rq.stride >= row && rq.capacity >= rq.stride * static_cast<size_t>(h - 1) + row;
}

} // namespace

// ---- EditSession ---------------------------------------------------------------

EditSession::EditSession(Editor& editor, int64_t photo_id, Settings settings, EventCallback cb)
    : editor_(editor), photo_id_(photo_id), settings_(std::move(settings)), callback_(std::move(cb)) {}

SessionInfo EditSession::info() const {
    std::lock_guard lock(mutex_);
    SessionInfo i;
    i.stage = stage_;
    i.error = error_;
    i.as_shot_temperature = as_shot_temperature_;
    i.as_shot_tint = as_shot_tint_;
    i.preview_width = preview_.width;
    i.preview_height = preview_.height;
    i.preview_display_p3 = preview_display_p3_;

    int sw = 0, sh = 0, flip = 0;
    if (raw_) {
        sw = raw_->image.width;
        sh = raw_->image.height;
        flip = raw_->flip;
    } else if (have_meta_) {
        flip = meta_.flip;
        sw = (flip & 4) ? meta_.height : meta_.width;
        sh = (flip & 4) ? meta_.width : meta_.height;
    }
    if (sw > 0 && sh > 0) {
        i.oriented_width = (flip & 4) ? sh : sw;
        i.oriented_height = (flip & 4) ? sw : sh;
        const GeometryPlan plan(sw, sh, flip, settings_.geometry);
        i.output_width = static_cast<int>(std::lround(plan.output_width()));
        i.output_height = static_cast<int>(std::lround(plan.output_height()));
        i.canvas_width = static_cast<int>(std::lround(plan.canvas_width()));
        i.canvas_height = static_cast<int>(std::lround(plan.canvas_height()));
    }
    return i;
}

bool EditSession::copy_preview(uint8_t* dst, size_t stride, size_t capacity) const {
    std::lock_guard lock(mutex_);
    const int w = preview_.width, h = preview_.height;
    if (w == 0 || !dst || stride < static_cast<size_t>(w) * 3 ||
        capacity < stride * static_cast<size_t>(h - 1) + static_cast<size_t>(w) * 3)
        return false;
    for (int y = 0; y < h; ++y) std::copy_n(preview_.row(y), static_cast<size_t>(w) * 3, dst + y * stride);
    return true;
}

Settings EditSession::settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

UndoStack& EditSession::undo_stack() const { return editor_.history_.stack(photo_id_); }

void EditSession::mark_dirty_locked() {
    dirty_ = true;
    last_change_ = std::chrono::steady_clock::now();
}

void EditSession::begin_change() {
    std::lock_guard lock(mutex_);
    if (!change_before_) change_before_ = settings_;
}

void EditSession::end_change() {
    std::lock_guard lock(mutex_);
    if (!change_before_) return;
    if (*change_before_ != settings_) {
        std::lock_guard h(editor_.history_mutex_);
        undo_stack().push(*change_before_, settings_);
    }
    change_before_.reset();
}

void EditSession::set_settings(Settings s) {
    s.clamp();
    std::lock_guard lock(mutex_);
    s.preserved_json = settings_.preserved_json;  // 未知のキーは core が持ち続ける
    if (s == settings_) return;
    Settings before = std::exchange(settings_, std::move(s));
    mark_dirty_locked();
    if (!change_before_) {
        std::lock_guard h(editor_.history_mutex_);
        undo_stack().push(std::move(before), settings_);
    }
}

bool EditSession::undo() {
    end_change();
    std::lock_guard lock(mutex_);
    std::optional<Settings> s;
    {
        std::lock_guard h(editor_.history_mutex_);
        s = undo_stack().undo();
    }
    if (!s) return false;
    s->preserved_json = settings_.preserved_json;
    settings_ = std::move(*s);
    mark_dirty_locked();
    return true;
}

bool EditSession::redo() {
    end_change();
    std::lock_guard lock(mutex_);
    std::optional<Settings> s;
    {
        std::lock_guard h(editor_.history_mutex_);
        s = undo_stack().redo();
    }
    if (!s) return false;
    s->preserved_json = settings_.preserved_json;
    settings_ = std::move(*s);
    mark_dirty_locked();
    return true;
}

bool EditSession::can_undo() const {
    std::lock_guard lock(mutex_);
    std::lock_guard h(editor_.history_mutex_);
    return undo_stack().can_undo() || (change_before_ && *change_before_ != settings_);
}

bool EditSession::can_redo() const {
    std::lock_guard lock(mutex_);
    std::lock_guard h(editor_.history_mutex_);
    return undo_stack().can_redo();
}

void EditSession::set_proxy_long_edge(int long_edge) {
    std::lock_guard lock(mutex_);
    proxy_long_edge_ = std::clamp(long_edge, 256, Editor::kMaxProxyLongEdge);
}

void EditSession::emit(Event ev, const std::string& message) {
    std::lock_guard lock(callback_mutex_);
    if (!closed_ && callback_) callback_(ev, message);
}

// ---- Editor --------------------------------------------------------------------

Editor::Editor(Catalog& catalog)
    : catalog_(catalog), display_(OutputSpace::DisplayP3, OutputDepth::U8) {
    preview_thread_ = std::thread([this] { preview_loop(); });
    decode_thread_ = std::thread([this] { decode_loop(); });
    render_thread_ = std::thread([this] { render_loop(); });
    save_thread_ = std::thread([this] { save_loop(); });
}

Editor::~Editor() {
    std::optional<RenderJob> pending;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending = std::move(render_job_);
        render_job_.reset();
    }
    decode_cancel_ = true;
    ++render_generation_;
    cv_.notify_all();
    for (auto* t : {&preview_thread_, &decode_thread_, &render_thread_, &save_thread_}) t->join();
    if (pending) pending->cb(RenderStatus::Cancelled, {});
    flush_saves();
    // 閉じた写真の現像結果は書き終えてから終わる（次に開いたときすぐ出せるように）
    while (!cache_jobs_.empty()) {
        CacheJob job = std::move(cache_jobs_.front());
        cache_jobs_.pop_front();
        update_caches(job);
    }
}

std::shared_ptr<EditSession> Editor::open(int64_t photo_id, int preview_long_edge, int proxy_long_edge,
                                          EditSession::EventCallback callback) {
    std::shared_ptr<EditSession> s(new EditSession(*this, photo_id, load_settings(catalog_, photo_id),
                                                   std::move(callback)));
    s->set_proxy_long_edge(proxy_long_edge);
    {
        std::lock_guard lock(mutex_);
        sessions_.push_back(s);
        preview_queue_.emplace_back(s, preview_long_edge);
        open_job_ = DecodeJob{s, photo_id};
        // 別の写真をデコード中なら打ち切る（同じ写真の先読み中ならそのまま使う）
        if (decoding_photo_ != 0 && decoding_photo_ != photo_id) decode_cancel_ = true;
    }
    cv_.notify_all();
    return s;
}

void Editor::prefetch(int64_t photo_id) {
    {
        std::lock_guard lock(mutex_);
        if ((prefetched_ && prefetched_->photo_id == photo_id) || decoding_photo_ == photo_id) return;
        prefetch_job_ = photo_id;
    }
    cv_.notify_all();
}

void Editor::close(const std::shared_ptr<EditSession>& s) {
    if (!s) return;
    {
        std::lock_guard lock(s->callback_mutex_);  // 実行中のコールバックがあれば終わるまで待つ
        s->closed_ = true;
    }
    s->end_change();
    save(*s);  // sessions_ にまだ入っているので、サムネイルの作成も積まれる
    enqueue_cache_job(s, s->settings(), true);
    std::lock_guard lock(mutex_);
    std::erase_if(sessions_, [&](const std::weak_ptr<EditSession>& w) {
        auto p = w.lock();
        return !p || p == s;
    });
    if (open_job_ && open_job_->session.lock() == s) open_job_.reset();
    // GPU に写した描画元（フル解像度は 45MP で約 270MB）を解放する。次に開いた写真は描くときに写し直す
    if (gpu_) gpu_->release_sources();
}

void Editor::save(EditSession& s) {
    Settings settings;
    {
        std::lock_guard lock(s.mutex_);
        if (!s.dirty_) return;
        s.dirty_ = false;
        settings = s.settings_;
    }
    catalog_.save_edit(s.photo_id_, settings.process_version, to_row(settings));
    std::shared_ptr<EditSession> keep;
    {
        std::lock_guard lock(mutex_);
        for (auto& w : sessions_)
            if (auto p = w.lock(); p.get() == &s) keep = p;
    }
    if (keep) enqueue_cache_job(keep, std::move(settings), false);
}

void Editor::enqueue_cache_job(const std::shared_ptr<EditSession>& s, Settings settings, bool closing) {
    std::lock_guard lock(mutex_);
    if (!thumbnail_cache_ && !(closing && preview_cache_)) return;
    if (closing) {
        std::lock_guard sl(s->mutex_);
        if (!s->raw_) return;  // デコード前に閉じた: 作る材料がない
    }
    cache_jobs_.push_back({s, std::move(settings), closing});
    cv_.notify_all();
}

void Editor::set_thumbnail_cache(std::filesystem::path dir, std::function<void(int64_t)> on_updated) {
    std::lock_guard lock(mutex_);
    thumbnail_cache_.emplace(std::move(dir));
    on_thumbnail_ = std::move(on_updated);
}

void Editor::set_preview_cache(std::filesystem::path dir, uint64_t limit_bytes) {
    auto cache = std::make_shared<PreviewCache>(std::move(dir), limit_bytes);
    std::lock_guard lock(mutex_);
    preview_cache_ = std::move(cache);
}

void Editor::set_gpu_renderer(std::unique_ptr<GpuRenderer> gpu) {
    std::lock_guard lock(mutex_);
    gpu_ = std::move(gpu);
}

std::string Editor::gpu_name() const {
    std::lock_guard lock(mutex_);
    return gpu_ ? gpu_->name() : std::string();
}

std::shared_ptr<PreviewCache> Editor::preview_cache() {
    std::lock_guard lock(mutex_);
    return preview_cache_;
}

void Editor::set_preview_cache_limit(uint64_t limit_bytes) {
    if (auto c = preview_cache()) c->set_limit(limit_bytes);
}

uint64_t Editor::preview_cache_usage() {
    auto c = preview_cache();
    return c ? c->usage() : 0;
}

void Editor::clear_preview_cache() {
    if (auto c = preview_cache()) c->clear();
}

void Editor::update_caches(const CacheJob& job) {
    const auto& s = job.session;
    try {
        std::shared_ptr<const DecodedRaw> raw;
        std::shared_ptr<const ImageF> proxy;
        {
            std::lock_guard lock(s->mutex_);
            raw = s->raw_;
            proxy = s->proxy_;
        }
        const auto photo = catalog_.photo(s->photo_id());
        bool thumb_changed = false;
        if (photo && raw && proxy) {
            const std::string key = rendered_key(photo->path, photo->file_size, photo->file_mtime, job.settings);
            const auto previews = preview_cache();
            const bool need_thumb = thumbnail_cache_ && !(job.closing && thumbnail_cache_->contains(key));
            const bool need_preview = job.closing && previews && !previews->contains(key);
            if (need_thumb) {
                const OutputTransform srgb(OutputSpace::Srgb, OutputDepth::U8);
                thumbnail_cache_->store(
                    key, downscale_u8(render_preview(*raw, *proxy, job.settings, srgb), kThumbnailLongEdge));
                thumb_changed = true;
            }
            if (need_preview) previews->store(key, render_preview(*raw, *proxy, job.settings, display_));
        }
        if (thumb_changed || !job.closing) {
            // 編集の行が書き込まれてから知らせる（グリッドが新しいキーで読み直す）
            catalog_.flush();
            if (on_thumbnail_) on_thumbnail_(s->photo_id());
        }
    } catch (const std::exception&) {
        // キャッシュは表示時に作り直せる
    }
}

void Editor::save_now(const std::shared_ptr<EditSession>& s) {
    if (!s) return;
    save(*s);
    catalog_.flush();
}

void Editor::flush_saves() {
    std::vector<std::shared_ptr<EditSession>> live;
    {
        std::lock_guard lock(mutex_);
        for (auto& w : sessions_)
            if (auto p = w.lock()) live.push_back(p);
    }
    for (auto& s : live) save(*s);
    catalog_.flush();
}

// ---- スレッド ------------------------------------------------------------------

void Editor::preview_loop() {
    for (;;) {
        std::shared_ptr<EditSession> s;
        int long_edge = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !preview_queue_.empty(); });
            if (stopping_) return;
            // 古い要求は捨てて、最後に開いた写真だけを処理する
            auto job = preview_queue_.back();
            preview_queue_.clear();
            s = job.first.lock();
            long_edge = job.second;
        }
        if (s) run_preview(s, long_edge);
    }
}

void Editor::run_preview(const std::shared_ptr<EditSession>& s, int long_edge) {
    try {
        const auto path = catalog_.photo_disk_path(s->photo_id());
        if (!path) throw Error(Error::Code::NotFound, "file not found");
        auto raw = std::make_unique<LibRaw>();
        open_libraw(*raw, *path);
        const RawMetadata meta = metadata_from_libraw(*raw);
        // 前に表示したときの現像結果があればそれを、なければカメラの埋め込みプレビュー
        std::optional<ImageU8> preview;
        bool display_p3 = false;
        if (auto cache = preview_cache()) {
            const auto photo = catalog_.photo(s->photo_id());
            ImageU8 img;
            if (photo && cache->load(rendered_key(photo->path, photo->file_size, photo->file_mtime, s->settings()), img)) {
                preview = std::move(img);
                display_p3 = true;
            }
        }
        if (!preview) preview = extract_embedded_preview(*raw, std::max(256, long_edge));
        bool emit_preview = false;
        {
            std::lock_guard lock(s->mutex_);
            s->meta_ = meta;
            s->have_meta_ = true;
            if (preview && s->stage_ == EditSession::Stage::Opening) {
                s->preview_ = std::move(*preview);
                s->preview_display_p3_ = display_p3;
                s->stage_ = EditSession::Stage::Preview;
                emit_preview = true;
            }
        }
        if (emit_preview) s->emit(EditSession::Event::Preview, {});
    } catch (const std::exception&) {
        // プレビューが出せなくても、デコードが終われば表示できる
    }
}

void Editor::decode_loop() {
    for (;;) {
        std::shared_ptr<EditSession> session;
        int64_t photo = 0;
        bool is_prefetch = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || open_job_ || prefetch_job_; });
            if (stopping_) return;
            if (open_job_) {
                session = open_job_->session.lock();
                photo = open_job_->photo_id;
                open_job_.reset();
                if (!session) continue;
                if (prefetched_ && prefetched_->photo_id == photo) {
                    auto raw = std::move(prefetched_->raw);
                    prefetched_.reset();
                    lock.unlock();
                    finish_decode(session, std::move(raw));
                    continue;
                }
            } else {
                photo = *prefetch_job_;
                prefetch_job_.reset();
                is_prefetch = true;
                if (prefetched_ && prefetched_->photo_id == photo) continue;
                prefetched_.reset();  // 先読みは 1 枚分だけ持つ
            }
            decoding_photo_ = photo;
            decode_cancel_ = false;
        }

        std::shared_ptr<const DecodedRaw> raw;
        std::string error;
        bool cancelled = false;
        try {
            const auto path = catalog_.photo_disk_path(photo);
            if (!path) throw Error(Error::Code::NotFound, "file not found");
            raw = std::make_shared<DecodedRaw>(decode_raw(*path, {.cancel = &decode_cancel_}));
        } catch (const Error& e) {
            cancelled = e.code() == Error::Code::Cancelled;
            error = e.what();
        } catch (const std::exception& e) {
            error = e.what();
        }

        {
            std::lock_guard lock(mutex_);
            decoding_photo_ = 0;
            if (raw && is_prefetch) {
                // 先読み中に同じ写真が開かれていれば、すぐ使う
                if (open_job_ && open_job_->photo_id == photo) {
                    session = open_job_->session.lock();
                    open_job_.reset();
                } else {
                    prefetched_ = Prefetched{photo, raw};
                }
            }
        }
        if (!session) continue;
        if (raw) {
            finish_decode(session, std::move(raw));
        } else if (!cancelled) {
            {
                std::lock_guard lock(session->mutex_);
                session->stage_ = EditSession::Stage::Failed;
                session->error_ = error;
            }
            session->emit(EditSession::Event::Failed, error);
        }
    }
}

void Editor::finish_decode(const std::shared_ptr<EditSession>& s, std::shared_ptr<const DecodedRaw> raw) {
    int long_edge;
    {
        std::lock_guard lock(s->mutex_);
        long_edge = s->proxy_long_edge_;
    }
    auto proxy = std::make_shared<const ImageF>(make_proxy(raw->image, long_edge));
    double k = 0, tint = 0;
    temp_tint_from_wb(raw->color.as_shot_wb, raw->color, k, tint);
    {
        std::lock_guard lock(s->mutex_);
        s->raw_ = std::move(raw);
        s->proxy_ = std::move(proxy);
        s->as_shot_temperature_ = k;
        s->as_shot_tint_ = tint;
        s->stage_ = EditSession::Stage::Ready;
    }
    s->emit(EditSession::Event::Ready, {});
}

uint64_t Editor::render(const std::shared_ptr<EditSession>& session, const RenderRequest& request, RenderCallback cb) {
    const uint64_t gen = ++render_generation_;  // 実行中の古い要求はこれで打ち切られる
    std::optional<RenderJob> old;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            old = RenderJob{session, request, std::move(cb), gen};
        } else {
            old = std::move(render_job_);
            render_job_ = RenderJob{session, request, std::move(cb), gen};
        }
    }
    if (old) old->cb(RenderStatus::Cancelled, {});
    cv_.notify_all();
    return gen;
}

void Editor::render_loop() {
    for (;;) {
        RenderJob job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || render_job_; });
            if (stopping_) return;
            job = std::move(*render_job_);
            render_job_.reset();
        }
        run_render(job);
    }
}

bool Editor::draw(const GpuSource& src, const GeometryPlan& plan, double scale, PointD origin,
                  const ColorPipeline& pipeline, const RenderRequest& rq, RenderResult& result, const CancelToken& cancel) {
    if (gpu_) {
        switch (gpu_->render(src, plan, scale, origin, result.width, result.height, pipeline, display_, rq.buffer,
                             rq.stride, rq.layout, result.histogram, cancel)) {
        case GpuStatus::Ok:
            return true;
        case GpuStatus::Cancelled:
            return false;
        case GpuStatus::Unsupported:
        case GpuStatus::Failed:
            break;  // CPU で描く
        }
    }
    if (!render_display(src.view(), plan, scale, origin, result.width, result.height, pipeline, display_,
                        Interpolation::Bilinear, rq.buffer, rq.stride, cancel, rq.layout))
        return false;
    fill_histogram(rq.buffer, rq.stride, result.width, result.height, rq.layout, result);
    return true;
}

void Editor::run_render(RenderJob& job) {
    const CancelToken cancel(&render_generation_, job.generation);
    RenderResult result;
    try {
        EditSession& s = *job.session;
        Settings settings;
        std::shared_ptr<const DecodedRaw> raw;
        std::shared_ptr<const ImageF> proxy;
        int proxy_edge;
        {
            std::lock_guard lock(s.mutex_);
            settings = s.settings_;
            raw = s.raw_;
            proxy = s.proxy_;
            proxy_edge = s.proxy_long_edge_;
        }
        if (!raw) {
            job.cb(RenderStatus::NotReady, result);
            return;
        }

        const auto& rq = job.request;
        const GeometryPlan plan(raw->image.width, raw->image.height, raw->flip, settings.geometry, !rq.ignore_crop);
        const double ow = plan.output_width(), oh = plan.output_height();
        const ColorPipeline pipeline(settings, raw->color);

        if (rq.mode == RenderRequest::Mode::Fit) {
            // プロキシの大きさが変わっていれば作り直す（5.2 章: ビューの大きさに合わせる）
            const int want = std::min(proxy_edge, std::max(raw->image.width, raw->image.height));
            if (!proxy || std::max(proxy->width, proxy->height) != want) {
                proxy = std::make_shared<const ImageF>(make_proxy(raw->image, want));
                std::lock_guard lock(s.mutex_);
                if (s.raw_ == raw) s.proxy_ = proxy;
            }
            if (cancel.cancelled()) {
                job.cb(RenderStatus::Cancelled, result);
                return;
            }
            const double scale = std::min(rq.max_width / ow, rq.max_height / oh);
            result.width = std::clamp(static_cast<int>(std::lround(ow * scale)), 1, rq.max_width);
            result.height = std::clamp(static_cast<int>(std::lround(oh * scale)), 1, rq.max_height);
            result.scale = scale;
            if (!fits(rq, result.width, result.height)) throw Error(Error::Code::InvalidArgument, "buffer too small");
            const GpuSource gsrc{nullptr, proxy, raw->image.width, raw->image.height};
            if (!draw(gsrc, plan, scale, {}, pipeline, rq, result, cancel)) {
                job.cb(RenderStatus::Cancelled, result);
                return;
            }
        } else {
            // 100% 表示（5.7 章）: 出力画像の範囲内に切り詰めてフル解像度から描く
            const double x0 = std::clamp(std::floor(rq.region_x), 0.0, std::max(0.0, ow - 1));
            const double y0 = std::clamp(std::floor(rq.region_y), 0.0, std::max(0.0, oh - 1));
            result.width = std::max(1, std::min(rq.region_width, static_cast<int>(std::floor(ow - x0))));
            result.height = std::max(1, std::min(rq.region_height, static_cast<int>(std::floor(oh - y0))));
            result.scale = 1.0;
            result.region_x = x0;
            result.region_y = y0;
            if (!fits(rq, result.width, result.height)) throw Error(Error::Code::InvalidArgument, "buffer too small");
            // フル解像度は DecodedRaw の一部なので、所有者を共有する shared_ptr にする（GPU 側の使い回しの判定用）
            const GpuSource gsrc{std::shared_ptr<const ImageU16>(raw, &raw->image), nullptr, raw->image.width,
                                 raw->image.height};
            if (!draw(gsrc, plan, 1.0, {x0, y0}, pipeline, rq, result, cancel)) {
                job.cb(RenderStatus::Cancelled, result);
                return;
            }
        }
        job.cb(RenderStatus::Ok, result);
    } catch (const std::exception&) {
        job.cb(RenderStatus::Failed, result);
    }
}

void Editor::save_loop() {
    for (;;) {
        std::optional<CacheJob> cache_job;
        {
            std::unique_lock lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::milliseconds(100),
                             [this] { return stopping_ || !cache_jobs_.empty(); }) &&
                stopping_)
                return;
            if (!cache_jobs_.empty()) {
                cache_job = std::move(cache_jobs_.front());
                cache_jobs_.pop_front();
            }
        }
        if (cache_job) update_caches(*cache_job);
        std::vector<std::shared_ptr<EditSession>> due;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex_);
            std::erase_if(sessions_, [](const std::weak_ptr<EditSession>& w) { return w.expired(); });
            for (auto& w : sessions_) {
                auto s = w.lock();
                if (!s) continue;
                std::lock_guard sl(s->mutex_);
                // ドラッグ中（begin_change 中）は保存を待つ必要はないが、最後の変更から 500ms 待つ（7.4 章）
                if (s->dirty_ && now - s->last_change_ >= kSaveDelay) due.push_back(s);
            }
        }
        for (auto& s : due) save(*s);
    }
}

} // namespace focal
