#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "edit/settings.h"

namespace focal {

// 現像のプリセット（v3.20、design.md 6.3 章）。現像の「調整」だけを持つ（白バランス・露出・トーン・色・明瞭度・
// ノイズ低減・シャープネス）。切り取り・回転・傾き補正（geometry）は含めない。

// プリセットにする設定。geometry・process_version・未知のキーを落とす
Settings preset_adjustments(const Settings& settings);

// base にプリセットの調整を重ねる。geometry・process_version・未知のキーは base のまま
Settings apply_preset(const Settings& base, const Settings& preset);

// 調整が同じか（白バランスは、As Shot なら色温度・色かぶりを見ない）。切り取りなどは見ない
bool adjustments_equal(const Settings& a, const Settings& b);

struct PresetInfo {
    std::string id;    // "builtin:<ファイル名の幹>" または "user:<ファイル名の幹>"
    std::string name;  // 表示名（NFC）
    bool builtin = false;
};

// プリセットのフォルダ。user_dir は利用者が作ったもの（読み書き）、builtin_dir はアプリに同梱したもの（読み取り専用。なくてもよい）。
// 1 つのプリセットは 1 つの JSON ファイル（.focalpreset）: {"focalPreset": 1, "name": "…", "settings": {6.1 章の JSON}}
class PresetStore {
public:
    PresetStore(std::filesystem::path user_dir, std::optional<std::filesystem::path> builtin_dir = std::nullopt);

    // 同梱のものを先に、それぞれ名前順。読めないファイルは飛ばす
    std::vector<PresetInfo> list() const;
    // 見つからなければ Error(NotFound)。返す Settings は geometry が既定値
    Settings load(const std::string& id) const;
    // 同じ名前（大文字小文字を区別しない）の利用者のプリセットがあれば上書き、なければ新しく作る。id を返す。
    // 名前が空なら Error(InvalidArgument)。書くのは調整だけ（geometry は落とす）
    std::string save(const std::string& name, const Settings& settings);
    // 利用者のプリセットだけ消せる（同梱は Error(InvalidArgument)）
    void remove(const std::string& id);
    // 利用者のプリセットの表示名を変える（ファイル名はそのまま）。同じ名前の別のプリセットがあれば Error(InvalidArgument)
    void rename(const std::string& id, const std::string& new_name);
    // settings の調整と同じ調整のプリセット（同梱が先）。なければ nullopt。読み込み済みの内容と比べるので、スライダーの操作のたびに
    // 呼んでよい（list・save・remove・rename で読み直す）。切り取りなどは見ない
    std::optional<std::string> find_match(const Settings& settings) const;

private:
    struct Cached {
        PresetInfo info;
        Settings settings;
    };
    std::vector<Cached> read_all() const;  // フォルダを読み直す
    std::filesystem::path user_dir_;
    std::optional<std::filesystem::path> builtin_dir_;
    mutable std::mutex mutex_;
    mutable std::optional<std::vector<Cached>> cache_;
};

} // namespace focal
