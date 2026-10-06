#pragma once
// レンズ補正の補正マップ（v3.27、design.md 5.13 章）。
// Lensfun で、センサー座標（ジオメトリの (a)〜(d) を経た後、レンズ補正前の画像の座標）の格子点ごとに、
// 「その位置に写すべき画素の、元の（歪んだ）画像での座標」を R・G・B 別に求めて持つ。
// CPU の描画も Metal のシェーダーも同じ格子を双一次補間で引くので、両者の見た目が一致する。
#include <memory>
#include <string>
#include <vector>

#include "edit/settings.h"
#include "imaging/lens_db.h"
#include "imaging/raw_decoder.h"

namespace focal {

struct LensMaps {
    int sensor_w = 0, sensor_h = 0;
    int nx = 0, ny = 0;  // 格子点の数
    // 格子点ごとに 8 要素: Rx, Ry, Gx, Gy, Bx, By, 周辺減光の補正ゲイン, 予約。座標はセンサー画素の連続座標
    static constexpr int kStride = 8;
    std::vector<float> data;
    bool has_tca = false;       // R・G・B で座標が違う
    bool has_gain = false;      // ゲインが 1 でない点がある
    bool has_geometry = false;  // G の座標が入力と違う点がある

    // 入力座標 (px, py) を引く。q[6] = R, G, B の座標、gain = 補正ゲイン
    void lookup(double px, double py, float q[6], float& gain) const;
};

// 設定と写真のメタデータから補正マップを作る。補正が要らない（無効・レンズが見つからない・補正データなし）なら null。
// resolved が非 null なら、使ったレンズを入れる
std::shared_ptr<const LensMaps> build_lens_maps(const LensSettings& lens, const RawMetadata& meta, int sensor_w,
                                                int sensor_h, std::optional<LensCandidate>* resolved = nullptr);

// 写真のレンズ名から自動で選ばれるレンズ（設定の id が空のとき）。見つからなければ nullopt
std::optional<LensCandidate> detect_lens(const RawMetadata& meta);

// 共有のレンズ DB（アプリ・CLI が起動時に dirs を設定する。同梱の DB → 利用者の DB の順に読む）。
// set_lens_database_dirs は最初の使用より前に呼ぶ。後から呼ぶと読み直す
void set_lens_database_dirs(std::vector<std::filesystem::path> dirs);
std::shared_ptr<const LensDatabase> shared_lens_database();
// 利用者が Lensfun の XML を足せるフォルダ（macOS: ~/Library/Application Support/jp.bekki.focal/Lensfun）。他の OS では空
std::filesystem::path user_lens_db_dir();

} // namespace focal
