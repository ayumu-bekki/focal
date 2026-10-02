#include "thumbs/thumbnail.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <semaphore>
#include <system_error>
#include <thread>

#include "edit/settings.h"
#include "imaging/image_io.h"
#include "imaging/libraw_util.h"
#include "imaging/output_transform.h"
#include "imaging/renderer.h"
#include "imaging/resample.h"
#include "util/error.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/omp_threads.h"
#include <cstdio>

namespace focal {

namespace fs = std::filesystem;

namespace {

constexpr int kMinEmbeddedLongEdge = 256;
constexpr int kJpegQuality = 85;

// half_size デコードによるフォールバックの同時実行数はコア数の半分まで（11.2 章）
std::counting_semaphore<1024>& fallback_slots() {
    static std::counting_semaphore<1024> slots(
        static_cast<std::ptrdiff_t>(std::max(1u, std::thread::hardware_concurrency() / 2)));
    return slots;
}

Thumbnail render_thumbnail(const fs::path& raw_path, int long_edge, const Settings& settings) {
    fallback_slots().acquire();
    struct Release {
        ~Release() { fallback_slots().release(); }
    } release;
    // サムネイルプールの中で動くので、LibRaw の OpenMP は 1 スレッドにする
    ScopedOmpThreads omp(1);

    const DecodedRaw raw = decode_raw(raw_path, {.half_size = true});
    const ImageF proxy = make_proxy(raw.image, long_edge);
    const OutputTransform srgb(OutputSpace::Srgb, OutputDepth::U8);
    // ジオメトリ（回転・クロップ）でプロキシより小さくなることがあるので、描いてから長辺を揃える
    return {downscale_u8(render_preview(raw, proxy, settings, srgb), long_edge), ThumbnailSource::Rendered};
}

// 使う埋め込みプレビューを選ぶ。優先順:
//   1. 長辺が long_edge 以上の JPEG のうち最小のもの（縮小デコードが速い）
//   2. 大きさ不明（0x0）の JPEG（多くは原寸のプレビュー）
//   3. long_edge 未満の JPEG のうち最大のもの
int choose_preview(const libraw_thumbnail_list_t& list, int long_edge) {
    int big = -1, big_edge = 0, under = -1, under_edge = 0, unknown = -1;
    for (int i = 0; i < list.thumbcount && i < LIBRAW_THUMBNAIL_MAXCOUNT; ++i) {
        const auto& t = list.thumblist[i];
        if (t.tformat != LIBRAW_INTERNAL_THUMBNAIL_JPEG) continue;
        const int edge = std::max<int>(t.twidth, t.theight);
        if (edge == 0) {
            if (unknown < 0) unknown = i;
        } else if (edge >= long_edge) {
            if (big < 0 || edge < big_edge) big = i, big_edge = edge;
        } else if (under < 0 || edge > under_edge) {
            under = i, under_edge = edge;
        }
    }
    if (big >= 0) return big;
    if (unknown >= 0) return unknown;
    return under;
}

} // namespace

std::string thumbnail_key(std::string_view nfc_path, int64_t file_size, int64_t file_mtime,
                          std::string_view settings_hash) {
    std::string s(nfc_path);
    s += '\0';
    s += std::to_string(file_size);
    s += '\0';
    s += std::to_string(file_mtime);
    if (!settings_hash.empty()) {
        s += '\0';
        s += settings_hash;
    }
    return blake3_hex(s);
}

std::string rendered_key(std::string_view nfc_path, int64_t file_size, int64_t file_mtime, const Settings& settings) {
    // 未知のキー（preserved_json）は描画に影響しないので、既知のパラメータだけでハッシュを作る
    Settings known = settings;
    known.preserved_json.clear();
    return thumbnail_key(nfc_path, file_size, file_mtime, blake3_hex(settings_to_json(known)));
}

// ---- PreviewCache ----

PreviewCache::PreviewCache(fs::path dir, uint64_t limit_bytes) : dir_(std::move(dir)), limit_(limit_bytes) {
    fs::create_directories(dir_);
}

fs::path PreviewCache::path_for(const std::string& key) const { return dir_ / key.substr(0, 2) / (key + ".jpg"); }

bool PreviewCache::contains(const std::string& key) const {
    std::error_code ec;
    return fs::exists(path_for(key), ec);
}

bool PreviewCache::load(const std::string& key, ImageU8& out) const {
    const fs::path p = path_for(key);
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    std::vector<uint8_t> bytes;
    {
        std::unique_ptr<FILE, int (*)(FILE*)> f(open_file(p, "rb"), &std::fclose);
        if (!f) return false;
        std::fseek(f.get(), 0, SEEK_END);
        bytes.resize(static_cast<size_t>(std::ftell(f.get())));
        std::fseek(f.get(), 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), f.get()) != bytes.size()) return false;
    }
    try {
        out = decode_jpeg(bytes.data(), bytes.size());
    } catch (const Error&) {
        return false;
    }
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);  // 最後に使った日時（古いものから消すため）
    return true;
}

void PreviewCache::store(const std::string& key, const ImageU8& display_p3) {
    static const std::vector<uint8_t> icc = OutputTransform(OutputSpace::DisplayP3, OutputDepth::U8).icc_profile();
    static std::atomic<uint64_t> counter{0};
    const fs::path final_path = path_for(key);
    fs::create_directories(final_path.parent_path());
    fs::path tmp = final_path;
    tmp += ".tmp" + std::to_string(counter.fetch_add(1));
    write_jpeg(tmp, display_p3, 90, icc);
    std::error_code ec;
    const auto size = fs::file_size(tmp, ec);
    fs::rename(tmp, final_path);
    // 毎回フォルダを数えると重いので、64MB 書くごとに上限を確かめる
    if (written_since_check_.fetch_add(ec ? 0 : size) + size > (64u << 20)) enforce_limit();
}

void PreviewCache::set_limit(uint64_t bytes) {
    limit_ = bytes;
    enforce_limit();
}

uint64_t PreviewCache::usage() const {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

void PreviewCache::clear() {
    std::lock_guard lock(enforce_mutex_);
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) fs::remove_all(it->path(), ec);
    written_since_check_ = 0;
}

void PreviewCache::enforce_limit() {
    std::lock_guard lock(enforce_mutex_);
    written_since_check_ = 0;
    struct Entry {
        fs::path path;
        fs::file_time_type time;
        uint64_t size;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".jpg") continue;
        const uint64_t size = it->file_size(ec);
        entries.push_back({it->path(), it->last_write_time(ec), size});
        total += size;
    }
    if (total <= limit_) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const auto& e : entries) {
        if (total <= limit_) break;
        if (fs::remove(e.path, ec)) total -= e.size;
    }
}

ThumbnailCache::ThumbnailCache(fs::path dir) : dir_(std::move(dir)) { fs::create_directories(dir_); }

fs::path ThumbnailCache::path_for(const std::string& key) const {
    return dir_ / key.substr(0, 2) / (key + ".jpg");
}

bool ThumbnailCache::contains(const std::string& key) const {
    std::error_code ec;
    return fs::exists(path_for(key), ec);
}

void ThumbnailCache::store(const std::string& key, const ImageU8& image) const {
    static std::atomic<uint64_t> counter{0};
    const fs::path final_path = path_for(key);
    fs::create_directories(final_path.parent_path());
    fs::path tmp = final_path;
    tmp += ".tmp" + std::to_string(counter.fetch_add(1));
    // サムネイルは sRGB とみなす約束なので ICC は埋め込まない（8.1 章）
    write_jpeg(tmp, image, kJpegQuality, {});
    fs::rename(tmp, final_path);
}

ImageU8 apply_orientation(const ImageU8& src, int flip) {
    if (flip == 0) return src;
    const int ow = (flip & 4) ? src.height : src.width;
    const int oh = (flip & 4) ? src.width : src.height;
    ImageU8 out(ow, oh);
    for (int y = 0; y < oh; ++y) {
        uint8_t* d = out.row(y);
        for (int x = 0; x < ow; ++x) {
            int r = y, c = x;
            if (flip & 4) std::swap(r, c);
            if (flip & 2) r = src.height - 1 - r;
            if (flip & 1) c = src.width - 1 - c;
            const uint8_t* s = src.row(r) + c * 3;
            d[x * 3 + 0] = s[0];
            d[x * 3 + 1] = s[1];
            d[x * 3 + 2] = s[2];
        }
    }
    return out;
}

ImageU8 downscale_u8(const ImageU8& src, int long_edge) {
    int ow, oh;
    fit_long_edge(src.width, src.height, long_edge, ow, oh);
    if (ow == src.width && oh == src.height) return src;
    ImageU8 out(ow, oh);
    const double sx = static_cast<double>(src.width) / ow, sy = static_cast<double>(src.height) / oh;
    for (int y = 0; y < oh; ++y) {
        const double y0 = y * sy, y1 = (y + 1) * sy;
        for (int x = 0; x < ow; ++x) {
            const double x0 = x * sx, x1 = (x + 1) * sx;
            double acc[3] = {0, 0, 0}, wsum = 0;
            for (int j = static_cast<int>(y0); j < std::min(src.height, static_cast<int>(std::ceil(y1))); ++j) {
                const double wy = std::min<double>(y1, j + 1) - std::max<double>(y0, j);
                const uint8_t* row = src.row(j);
                for (int i = static_cast<int>(x0); i < std::min(src.width, static_cast<int>(std::ceil(x1))); ++i) {
                    const double w = wy * (std::min<double>(x1, i + 1) - std::max<double>(x0, i));
                    for (int c = 0; c < 3; ++c) acc[c] += w * row[i * 3 + c];
                    wsum += w;
                }
            }
            for (int c = 0; c < 3; ++c) out.row(y)[x * 3 + c] = static_cast<uint8_t>(std::lround(acc[c] / wsum));
        }
    }
    return out;
}

std::optional<ImageU8> extract_embedded_preview(LibRaw& raw, int long_edge) {
    const int idx = choose_preview(raw.imgdata.thumbs_list, long_edge);
    if (idx < 0) return std::nullopt;
    if (raw.unpack_thumb_ex(idx) != LIBRAW_SUCCESS) return std::nullopt;
    const auto& t = raw.imgdata.thumbnail;
    if (t.tformat != LIBRAW_THUMBNAIL_JPEG || !t.thumb || t.tlength == 0) return std::nullopt;

    ImageU8 img;
    try {
        img = decode_jpeg(reinterpret_cast<const uint8_t*>(t.thumb), t.tlength, long_edge);
    } catch (const Error&) {
        return std::nullopt;
    }
    if (std::max(img.width, img.height) < kMinEmbeddedLongEdge) return std::nullopt;
    // 埋め込み JPEG はセンサーの向きのまま。RAW 本体と同じ flip を適用する（10 章）
    return apply_orientation(downscale_u8(img, long_edge), raw.imgdata.sizes.flip);
}

Thumbnail make_thumbnail(LibRaw& raw, const fs::path& raw_path, const ThumbnailOptions& options) {
    if (options.settings) return render_thumbnail(raw_path, options.long_edge, *options.settings);
    if (options.allow_embedded) {
        if (auto img = extract_embedded_preview(raw, options.long_edge))
            return {std::move(*img), ThumbnailSource::Embedded};
    }
    return render_thumbnail(raw_path, options.long_edge, Settings{});
}

Thumbnail make_thumbnail(const fs::path& raw_path, const ThumbnailOptions& options) {
    auto raw = std::make_unique<LibRaw>();
    open_libraw(*raw, raw_path);
    return make_thumbnail(*raw, raw_path, options);
}

} // namespace focal
