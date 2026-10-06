# Focal

軽量でシンプルな macOS 用の RAW 写真管理・現像アプリ。
Photomator の代わりとして使えるものを目指しています。

- 元の RAW ファイルには書き込みません。編集内容はカタログに保存します
- 登録したフォルダの写真はコピーせず、そのまま参照します
- SD カードからの取り込みは、日付フォルダへコピーします
- 使い方は [使い方ガイド](https://ayumu-bekki.github.io/focal/manual/)（[Markdown 版](docs/user-guide.md)）を見てください。紹介ページ: <https://ayumu-bekki.github.io/focal/>
- 画像処理とカタログは C++ の core にまとめ、macOS の画面（SwiftUI）とは C API でつないでいます。

## ご利用にあたって

Focal は開発中のソフトウェア（ベータ版）です。現像やカタログの内容が失われたり、写真のファイルが変更・削除されたりする不具合が含まれている可能性があります。**大切な写真は、必ずバックアップを取ったうえでお使いください。** 本ソフトウェアの使用によって生じたいかなる損害についても、開発者は責任を負いません（Apache License 2.0 のとおり、現状のまま提供し、いかなる保証もしません）。不具合は [GitHub の Issue](https://github.com/ayumu-bekki/focal/issues) で教えてください。

## できること

| 分類 | 機能 |
|---|---|
| ライブラリ | フォルダの登録と再スキャン、外付けドライブ・NAS の追従とオフライン表示、グリッド表示、★0〜5、ピック / リジェクト、階層タグ、アルバム（フォルダで入れ子）、スマートアルバム、絞り込み（フォルダ・★・フラグ・タグ・撮影日） |
| 取り込み | SD カードなどから `年/日付` のフォルダへコピー（検証・取り込み済みのスキップ・空き容量の確認） |
| 現像 | 露出、コントラスト、ホワイトバランス（撮影時の設定 / カスタム）、ハイライト、シャドウ、白レベル、黒レベル、明るさ、彩度、自然な彩度、明瞭度、シャープネス、ノイズ低減（輝度・カラー）。調整だけを保存・適用できるプリセット（取り込み時にも適用可能） |
| RAW 以外の写真 | JPEG・TIFF・PNG（macOS では HEIF も）の管理に対応。RAW と同名の JPEG は同一写真の付属ファイルとして統合管理（現像は RAW のみ） |
| カタログ | 複数のカタログの切り替え、終了時の自動バックアップ（そのまま開けるカタログとして保存）、カタログ情報の表示と最適化 |
| ジオメトリ | 切り取り（縦横比の固定）、90° 回転、傾き補正（±45°）、水平線ツール |
| レンズ補正 | 歪曲収差・倍率色収差・周辺減光の補正、魚眼 ↔ 通常などの射影の変換（[Lensfun](https://lensfun.github.io/) のレンズデータを使用。macOS）。レンズは写真から自動で選び、手動でも選べる。利用者がレンズデータを足せる |
| 表示 | フィット表示、100% 表示、ヒストグラム、フィルムストリップ。表示は GPU（Metal）で描きます |
| 編集 | 取り消し / やり直し |
| 書き出し | sRGB の JPEG、16 ビットの TIFF（ICC プロファイル付き）、長辺の指定、複数枚の一括書き出し |
| 削除 | ⌥⌃⇧ Delete で確認のうえディスクから削除（ローカルはゴミ箱、NAS は完全削除） |

対応する RAW の形式は [LibRaw](https://www.libraw.org/) によります（CR3・NEF・ARW・RAF・DNG で確認しています）。

## 動作環境

- macOS 14 以降
- Apple シリコン（arm64）

## ビルド

- Xcode(Swift 6)、CMake、Ninja
- [vcpkg](https://github.com/microsoft/vcpkg)(依存ライブラリは初回のビルドで vcpkg が用意します)
- [XcodeGen](https://github.com/yonaskolb/XcodeGen)(Xcode のプロジェクトを作るのに使います)

```sh
# vcpkg を用意して VCPKG_ROOT を設定する
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg

# core と CLI をビルドしてテストする
cmake --preset release
cmake --build --preset release
ctest --preset release -j4

# macOS アプリ
cd apps/macos && xcodegen && open Focal.xcodeproj
```

アプリのビルドでは、ビルド前のスクリプト(`apps/macos/scripts/build-core.sh`)が core を XCFramework にまとめます。
配布用の DMG は `apps/macos/scripts/package.sh` で作れます。

### テスト用の RAW

テスト用RAW画像に、[raw.pixls.us](https://raw.pixls.us/) の RAW(CC0)を使います。
リポジトリには含めていないので、次のスクリプトで取得してください。RAW がないときは、RAW画像のテストを飛ばします。

```sh
tests/data/fetch.sh
```

## コマンドライン(CLI)

画像処理の確認やカタログの操作に使える `focal` コマンドがあります。

```sh
./build/release/cli/focal info in.NEF
./build/release/cli/focal render in.NEF out.tif --ev 1 --temp 5000 --contrast 20
./build/release/cli/focal import ~/Pictures/RAW
./build/release/cli/focal ls --rating 3 --flag pick
./build/release/cli/focal export 12 34 --dest ~/Desktop/out --long-edge 2048
```

## 構成

| ディレクトリ | 内容 |
|---|---|
| `core/` | 画像処理、カタログ(SQLite)、サムネイル、現像、書き出し。OS と UI に依存しません |
| `gpu/` | 表示用の GPU レンダラー(Metal) |
| `capi/` | C API(`include/focal/focal.h`) |
| `cli/` | コマンドラインツール |
| `apps/macos/` | macOS アプリ(SwiftUI) |
| `tests/` | ユニットテスト(Catch2)とゴールデン画像 |
| `ports/`、`triplets/` | vcpkg のオーバーレイポートとトリプレット |

## ライセンス

[Apache License 2.0](LICENSE)

依存ライブラリのライセンス文は配布物に含めています。

### レンズ補正のライセンス表示

- レンズ補正には [Lensfun](https://lensfun.github.io/) のライブラリ（LGPL-3.0）と、その依存の GLib・libintl（LGPL）を、**動的ライブラリとして**使っています。アプリの `Contents/Frameworks` にあり、差し替えられます（差し替えた後は再署名が必要です）。
- 同梱するレンズのデータベースは Lensfun プロジェクトのデータで、[CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/) です。**未改変のまま**アプリの `Contents/Resources/LensfunDB` に入れています（取得: `tools/fetch-lensfun-db.sh`）。データの日付とライセンス文は、アプリの「ライセンス」に載せています。
- 利用者が自分でレンズのデータを足すときは、Lensfun 形式の XML を `~/Library/Application Support/jp.bekki.focal/Lensfun/` に置きます。
