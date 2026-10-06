#include "imaging/lens_db.h"

#include "util/file.h"
#include "util/unicode.h"

#ifdef FOCAL_HAVE_LENSFUN
#include <algorithm>

#include "imaging/lens_db_impl.h"
#endif

namespace focal {

#ifdef FOCAL_HAVE_LENSFUN

namespace {

std::string str_or_empty(const char* s) { return s ? std::string(s) : std::string(); }

std::string lens_id(const lfLens* l) {
    return str_or_empty(lf_mlstr_get(l->Maker)) + "|" + str_or_empty(lf_mlstr_get(l->Model));
}

LensCandidate to_candidate(const lfDatabase* db, const lfLens* l) {
    LensCandidate c;
    c.maker = str_or_empty(lf_mlstr_get(l->Maker));
    c.model = str_or_empty(lf_mlstr_get(l->Model));
    c.id = c.maker + "|" + c.model;
    if (l->Mounts) {
        for (int i = 0; l->Mounts[i]; ++i) {
            if (i) c.mounts += ", ";
            const char* name = lf_db_mount_name(db, l->Mounts[i]);
            c.mounts += name ? name : l->Mounts[i];
        }
    }
    c.crop_factor = l->CropFactor;
    c.min_focal = l->MinFocal;
    c.max_focal = l->MaxFocal;
    c.has_distortion = l->CalibDistortion && l->CalibDistortion[0];
    c.has_tca = l->CalibTCA && l->CalibTCA[0];
    c.has_vignetting = l->CalibVignetting && l->CalibVignetting[0];
    return c;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string w;
    for (char ch : casefold_key(s)) {
        if (ch == ' ' || ch == '\t') {
            if (!w.empty()) {
                words.push_back(w);
                w.clear();
            }
        } else {
            w += ch;
        }
    }
    if (!w.empty()) words.push_back(w);
    return words;
}

} // namespace

LensDatabase::LensDatabase() : impl_(std::make_unique<Impl>()) { impl_->db = lf_db_new(); }

LensDatabase::~LensDatabase() {
    if (impl_ && impl_->db) lf_db_destroy(impl_->db);
}

bool LensDatabase::supported() { return true; }

bool LensDatabase::load_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    return lf_db_load_directory(impl_->db, path_to_utf8(dir).c_str()) != 0;
}

size_t LensDatabase::camera_count() const {
    size_t n = 0;
    const lfCamera* const* c = lf_db_get_cameras(impl_->db);
    while (c && c[n]) ++n;
    return n;
}

size_t LensDatabase::lens_count() const {
    size_t n = 0;
    const lfLens* const* l = lf_db_get_lenses(impl_->db);
    while (l && l[n]) ++n;
    return n;
}

std::optional<CameraMatch> LensDatabase::find_camera(const std::string& make, const std::string& model) const {
    if (model.empty()) return std::nullopt;
    const lfCamera** cams = lf_db_find_cameras(impl_->db, make.c_str(), model.c_str());
    if (!cams) return std::nullopt;
    std::optional<CameraMatch> r;
    if (cams[0]) {
        CameraMatch m;
        m.maker = str_or_empty(lf_mlstr_get(cams[0]->Maker));
        m.model = str_or_empty(lf_mlstr_get(cams[0]->Model));
        m.mount = str_or_empty(cams[0]->Mount);
        m.crop_factor = cams[0]->CropFactor;
        r = m;
    }
    lf_free(cams);
    return r;
}

std::vector<LensCandidate> LensDatabase::find_lenses(const std::string& camera_make, const std::string& camera_model,
                                                     const std::string& lens_name, size_t limit) const {
    std::vector<LensCandidate> out;
    if (lens_name.empty()) return out;
    const lfCamera* cam = nullptr;
    const lfCamera** cams = nullptr;
    if (!camera_model.empty()) {
        cams = lf_db_find_cameras(impl_->db, camera_make.c_str(), camera_model.c_str());
        if (cams && cams[0]) cam = cams[0];
    }
    const lfLens** lenses =
        lf_db_find_lenses_hd(impl_->db, cam, nullptr, lens_name.c_str(), LF_SEARCH_SORT_AND_UNIQUIFY);
    for (int i = 0; lenses && lenses[i] && out.size() < limit; ++i) out.push_back(to_candidate(impl_->db, lenses[i]));
    if (lenses) lf_free(lenses);
    if (cams) lf_free(cams);
    return out;
}

std::vector<LensCandidate> LensDatabase::search(const std::string& query, const std::string& mount,
                                                size_t limit) const {
    const auto words = split_words(query);
    const std::string mount_key = casefold_key(mount);
    std::vector<LensCandidate> out;
    const lfLens* const* all = lf_db_get_lenses(impl_->db);
    for (int i = 0; all && all[i]; ++i) {
        LensCandidate c = to_candidate(impl_->db, all[i]);
        if (!mount_key.empty() && casefold_key(c.mounts).find(mount_key) == std::string::npos) continue;
        const std::string hay = casefold_key(c.maker + " " + c.model);
        bool ok = true;
        for (const auto& w : words) {
            if (hay.find(w) == std::string::npos) {
                ok = false;
                break;
            }
        }
        if (ok) out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const LensCandidate& a, const LensCandidate& b) { return a.id < b.id; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const LensCandidate& a, const LensCandidate& b) { return a.id == b.id; }),
              out.end());
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::optional<LensCandidate> LensDatabase::find_by_id(const std::string& id) const {
    if (const lfLens* l = lens_by_id(*this, id)) return to_candidate(impl_->db, l);
    return std::nullopt;
}

const lfLens* lens_by_id(const LensDatabase& db, const std::string& id) {
    const lfLens* const* all = lf_db_get_lenses(db.impl()->db);
    for (int i = 0; all && all[i]; ++i) {
        if (lens_id(all[i]) == id) return all[i];
    }
    return nullptr;
}

const lfLens* lens_by_exif(const LensDatabase& db, const std::string& camera_make, const std::string& camera_model,
                           const std::string& lens_name) {
    const lfDatabase* d = db.impl()->db;
    const lfCamera** cams = camera_model.empty() ? nullptr : lf_db_find_cameras(d, camera_make.c_str(), camera_model.c_str());
    const lfCamera* cam = (cams && cams[0]) ? cams[0] : nullptr;
    // レンズ名がない（レンズ固定のカメラ）ときは、カメラのマウント（fixed lens）に合うレンズを探す
    if (lens_name.empty() && !(cam && cam->Mount && std::string(cam->Mount).rfind("fixed", 0) == 0)) {
        if (cams) lf_free(cams);
        return nullptr;
    }
    const lfLens** lenses =
        lf_db_find_lenses_hd(d, cam, nullptr, lens_name.empty() ? nullptr : lens_name.c_str(), LF_SEARCH_SORT_AND_UNIQUIFY);
    const lfLens* r = (lenses && lenses[0]) ? lenses[0] : nullptr;
    if (lenses) lf_free(lenses);
    if (cams) lf_free(cams);
    return r;
}

#else  // Lensfun なし

struct LensDatabase::Impl {};

LensDatabase::LensDatabase() : impl_(std::make_unique<Impl>()) {}
LensDatabase::~LensDatabase() = default;
bool LensDatabase::supported() { return false; }
bool LensDatabase::load_directory(const std::filesystem::path&) { return false; }
size_t LensDatabase::camera_count() const { return 0; }
size_t LensDatabase::lens_count() const { return 0; }
std::optional<CameraMatch> LensDatabase::find_camera(const std::string&, const std::string&) const {
    return std::nullopt;
}
std::vector<LensCandidate> LensDatabase::find_lenses(const std::string&, const std::string&, const std::string&,
                                                     size_t) const {
    return {};
}
std::vector<LensCandidate> LensDatabase::search(const std::string&, const std::string&, size_t) const { return {}; }
std::optional<LensCandidate> LensDatabase::find_by_id(const std::string&) const { return std::nullopt; }

#endif

} // namespace focal
