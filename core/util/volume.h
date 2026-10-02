#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace focal {

// マウントされているボリューム（v3.19）。外付けドライブはマウントポイント（ドライブレター）が変わるので、
// カタログにはボリューム ID と、ボリュームのルートからの相対パスで覚える（design.md 7.2 章）。
struct VolumeInfo {
    std::string id;                      // ボリューム固有の ID（macOS はボリューム UUID）。取れなければ空
    std::string name;                    // 表示名
    std::filesystem::path mount_point;   // 利用者から見えるマウントポイント
    bool removable = false;              // OS が取り外し可能と報告した（取れる OS だけ）
};

// いまマウントされていて、利用者が見るボリューム（システムの内部用ボリュームは除く）
std::vector<VolumeInfo> mounted_volumes();

// path があるボリューム。判別できなければ nullopt
std::optional<VolumeInfo> volume_for_path(const std::filesystem::path& path);

// path をボリュームのルートからの相対パス（NFC、区切りは '/'、ルート自身は ""）にする。
// path がボリュームの外なら nullopt
std::optional<std::string> volume_relative_path(const VolumeInfo& volume, const std::filesystem::path& path);

} // namespace focal
