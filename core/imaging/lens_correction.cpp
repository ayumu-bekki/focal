#include "imaging/lens_correction.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "util/error.h"

#ifdef FOCAL_HAVE_LENSFUN
#include "imaging/lens_db_impl.h"
#endif

namespace focal {

void LensMaps::lookup(double px, double py, float q[6], float& gain) const {
    const double u = std::clamp(px / sensor_w * (nx - 1), 0.0, static_cast<double>(nx - 1));
    const double v = std::clamp(py / sensor_h * (ny - 1), 0.0, static_cast<double>(ny - 1));
    const int i0 = std::min(static_cast<int>(u), nx - 2), j0 = std::min(static_cast<int>(v), ny - 2);
    const float tx = static_cast<float>(u - i0), ty = static_cast<float>(v - j0);
    const float* n00 = data.data() + (static_cast<size_t>(j0) * nx + i0) * kStride;
    const float* n10 = n00 + kStride;
    const float* n01 = n00 + static_cast<size_t>(nx) * kStride;
    const float* n11 = n01 + kStride;
    float out[7];
    for (int k = 0; k < 7; ++k) {
        const float top = n00[k] + (n10[k] - n00[k]) * tx;
        const float bot = n01[k] + (n11[k] - n01[k]) * tx;
        out[k] = top + (bot - top) * ty;
    }
    std::copy_n(out, 6, q);
    gain = out[6];
}

namespace {

std::mutex g_db_mutex;
std::vector<std::filesystem::path> g_db_dirs;
std::shared_ptr<const LensDatabase> g_db;

} // namespace

void set_lens_database_dirs(std::vector<std::filesystem::path> dirs) {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    g_db_dirs = std::move(dirs);
    g_db.reset();
}

std::shared_ptr<const LensDatabase> shared_lens_database() {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_db) {
        auto db = std::make_shared<LensDatabase>();
        for (const auto& d : g_db_dirs) db->load_directory(d);
        g_db = std::move(db);
    }
    return g_db;
}

std::filesystem::path user_lens_db_dir() {
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / "jp.bekki.focal" / "Lensfun";
#endif
    return {};
}

#ifdef FOCAL_HAVE_LENSFUN

namespace {

lfLensType target_geometry(LensProjection p, lfLensType lens_type) {
    switch (p) {
    case LensProjection::Keep: return lens_type;
    case LensProjection::Rectilinear: return LF_RECTILINEAR;
    case LensProjection::Fisheye: return LF_FISHEYE;
    case LensProjection::Equisolid: return LF_FISHEYE_EQUISOLID;
    case LensProjection::Stereographic: return LF_FISHEYE_STEREOGRAPHIC;
    case LensProjection::Orthographic: return LF_FISHEYE_ORTHOGRAPHIC;
    case LensProjection::Panoramic: return LF_PANORAMIC;
    case LensProjection::Equirectangular: return LF_EQUIRECTANGULAR;
    }
    return lens_type;
}

const lfLens* pick_lens(const LensDatabase& db, const LensSettings& lens, const RawMetadata& meta) {
    if (!lens.id.empty()) return lens_by_id(db, lens.id);
    return lens_by_exif(db, meta.make, meta.model, meta.lens);
}

struct ModifierDeleter {
    void operator()(lfModifier* m) const { lf_modifier_destroy(m); }
};

} // namespace

std::optional<LensCandidate> detect_lens(const RawMetadata& meta) {
    const auto db = shared_lens_database();
    if (const lfLens* l = lens_by_exif(*db, meta.make, meta.model, meta.lens)) {
        const std::string id = std::string(lf_mlstr_get(l->Maker) ? lf_mlstr_get(l->Maker) : "") + "|" +
                               (lf_mlstr_get(l->Model) ? lf_mlstr_get(l->Model) : "");
        return db->find_by_id(id);
    }
    return std::nullopt;
}

std::shared_ptr<const LensMaps> build_lens_maps(const LensSettings& ls, const RawMetadata& meta, int sensor_w,
                                                int sensor_h, std::optional<LensCandidate>* resolved) {
    if (resolved) resolved->reset();
    if (!ls.enabled || sensor_w < 2 || sensor_h < 2) return nullptr;
    const double ad = ls.distortion / 100.0, at = ls.tca / 100.0, av = ls.vignetting / 100.0;
    if (ad <= 0 && at <= 0 && av <= 0) return nullptr;

    const auto db = shared_lens_database();
    const lfLens* lens = pick_lens(*db, ls, meta);
    if (!lens) return nullptr;
    if (resolved) {
        const std::string id = std::string(lf_mlstr_get(lens->Maker) ? lf_mlstr_get(lens->Maker) : "") + "|" +
                               (lf_mlstr_get(lens->Model) ? lf_mlstr_get(lens->Model) : "");
        *resolved = db->find_by_id(id);
    }

    // クロップ係数はカメラ優先（DB にあれば）。なければレンズの値
    float crop = lens->CropFactor > 0 ? lens->CropFactor : 1.0f;
    if (auto cam = db->find_camera(meta.make, meta.model); cam && cam->crop_factor > 0) crop = cam->crop_factor;
    const float focal = meta.focal_length > 0 ? meta.focal_length : std::max(lens->MinFocal, 1.0f);
    const float aperture = meta.aperture > 0 ? meta.aperture : 5.6f;

    int flags = 0;
    const bool geometry = ad > 0 && ls.projection != LensProjection::Keep;
    if (at > 0) flags |= LF_MODIFY_TCA;
    if (av > 0) flags |= LF_MODIFY_VIGNETTING;
    if (ad > 0) flags |= LF_MODIFY_DISTORTION | LF_MODIFY_SCALE;
    if (geometry) flags |= LF_MODIFY_GEOMETRY;

    std::unique_ptr<lfModifier, ModifierDeleter> mod(lf_modifier_new(lens, crop, sensor_w, sensor_h));
    if (!mod) return nullptr;
    // 被写体距離は不明なので 1000m（無限遠）。scale = 0 は、四隅に空白が出ない最小の拡大率
    const int applied = lf_modifier_initialize(mod.get(), lens, LF_PF_F32, focal, aperture, 1000.0f, 0.0f,
                                               target_geometry(ls.projection, lens->Type), flags, false);
    if (applied == 0) return nullptr;

    auto maps = std::make_shared<LensMaps>();
    maps->sensor_w = sensor_w;
    maps->sensor_h = sensor_h;
    // 格子の間隔は長辺の約 1/128（補間の誤差は 0.1 画素以下）
    const double step = std::max(sensor_w, sensor_h) / 128.0;
    maps->nx = std::max(2, static_cast<int>(std::ceil(sensor_w / step)) + 1);
    maps->ny = std::max(2, static_cast<int>(std::ceil(sensor_h / step)) + 1);
    maps->data.assign(static_cast<size_t>(maps->nx) * maps->ny * LensMaps::kStride, 0.0f);

    const bool do_dist = (applied & (LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE)) != 0;
    const bool do_tca = (applied & LF_MODIFY_TCA) != 0;
    const bool do_vig = (applied & LF_MODIFY_VIGNETTING) != 0;

    for (int j = 0; j < maps->ny; ++j) {
        for (int i = 0; i < maps->nx; ++i) {
            // 連続座標（画素中心 0.5）の格子点。Lensfun の座標は画素の中心が整数
            const double px = static_cast<double>(i) / (maps->nx - 1) * sensor_w;
            const double py = static_cast<double>(j) / (maps->ny - 1) * sensor_h;
            const float lx = static_cast<float>(px - 0.5), ly = static_cast<float>(py - 0.5);

            float g[2] = {lx, ly};
            if (do_dist) lf_modifier_apply_geometry_distortion(mod.get(), lx, ly, 1, 1, g);
            float c[6] = {g[0], g[1], g[0], g[1], g[0], g[1]};
            if (do_tca) {
                float t[6];
                lf_modifier_apply_subpixel_geometry_distortion(mod.get(), lx, ly, 1, 1, t);
                // 歪曲収差の量は g で決め、TCA は G との差だけを量に応じて足す
                for (int k = 0; k < 3; ++k) {
                    c[k * 2] = g[0] + static_cast<float>(at) * (t[k * 2] - t[2]);
                    c[k * 2 + 1] = g[1] + static_cast<float>(at) * (t[k * 2 + 1] - t[3]);
                }
            }
            // 歪曲収差の量: 入力そのまま（補正なし）と g の間を線形に混ぜる。TCA の差は c に含まれている
            const float d = do_dist ? static_cast<float>(ad) : 0.0f;
            for (int k = 0; k < 3; ++k) {
                c[k * 2] = lx + d * (g[0] - lx) + (c[k * 2] - g[0]);
                c[k * 2 + 1] = ly + d * (g[1] - ly) + (c[k * 2 + 1] - g[1]);
            }

            float gain = 1.0f;
            if (do_vig) {
                float px3[3] = {1.0f, 1.0f, 1.0f};
                if (lf_modifier_apply_color_modification(mod.get(), px3, c[2], c[3], 1, 1, LF_CR_3(RED, GREEN, BLUE),
                                                         static_cast<int>(sizeof px3)))
                    gain = 1.0f + static_cast<float>(av) * (px3[1] - 1.0f);
            }

            float* node = maps->data.data() + (static_cast<size_t>(j) * maps->nx + i) * LensMaps::kStride;
            for (int k = 0; k < 6; ++k) node[k] = c[k] + 0.5f;
            node[6] = gain;
            if (std::abs(c[2] - lx) > 1e-3f || std::abs(c[3] - ly) > 1e-3f) maps->has_geometry = true;
            if (std::abs(c[0] - c[2]) > 1e-3f || std::abs(c[1] - c[3]) > 1e-3f || std::abs(c[4] - c[2]) > 1e-3f ||
                std::abs(c[5] - c[3]) > 1e-3f)
                maps->has_tca = true;
            if (std::abs(gain - 1.0f) > 1e-4f) maps->has_gain = true;
        }
    }
    if (!maps->has_geometry && !maps->has_tca && !maps->has_gain) return nullptr;
    return maps;
}

#else  // Lensfun なし

std::optional<LensCandidate> detect_lens(const RawMetadata&) { return std::nullopt; }

std::shared_ptr<const LensMaps> build_lens_maps(const LensSettings&, const RawMetadata&, int, int,
                                                std::optional<LensCandidate>* resolved) {
    if (resolved) resolved->reset();
    return nullptr;
}

#endif

} // namespace focal
