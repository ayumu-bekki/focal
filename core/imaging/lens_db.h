#pragma once
// レンズ補正のデータベース（Lensfun。LGPL-3.0 のライブラリを動的リンクし、レンズ DB は別ファイルで読む。v3.27、design.md 5.13 章）。
// Lensfun のない環境（macOS 以外）では supported() が false で、すべて空を返す。
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace focal {

struct CameraMatch {
    std::string maker;
    std::string model;
    std::string mount;
    float crop_factor = 1.0f;
};

struct LensCandidate {
    std::string id;  // "メーカー|モデル"（settings の lens.id に保存する）
    std::string maker;
    std::string model;
    std::string mounts;  // "Nikon Z, Nikon F" のように「, 」区切り
    float crop_factor = 1.0f;
    float min_focal = 0;
    float max_focal = 0;
    bool has_distortion = false;
    bool has_tca = false;
    bool has_vignetting = false;
};

class LensDatabase {
public:
    LensDatabase();
    ~LensDatabase();
    LensDatabase(const LensDatabase&) = delete;
    LensDatabase& operator=(const LensDatabase&) = delete;

    // Lensfun を使えるビルドか
    static bool supported();
    // ディレクトリの XML をすべて読む。ディレクトリがなければ false
    bool load_directory(const std::filesystem::path& dir);

    size_t camera_count() const;
    size_t lens_count() const;

    std::optional<CameraMatch> find_camera(const std::string& make, const std::string& model) const;
    // 写真の EXIF から: カメラ（あれば）のマウントで絞り、レンズ名で近い順に返す（最大 limit 件）
    std::vector<LensCandidate> find_lenses(const std::string& camera_make, const std::string& camera_model,
                                           const std::string& lens_name, size_t limit = 10) const;
    // 手動選択用: 文字列（スペース区切りの AND、大文字小文字を区別しない）で全レンズから探す。mount が空なら全マウント
    std::vector<LensCandidate> search(const std::string& query, const std::string& mount = {},
                                      size_t limit = 50) const;
    std::optional<LensCandidate> find_by_id(const std::string& id) const;

    struct Impl;
    const Impl* impl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace focal
