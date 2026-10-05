# focal — 軽量 RAW 現像・管理アプリ

設計書は `design.md`（v3.20）。ADR は `docs/adr/`。利用者向けの使い方は `docs/user-guide.md`（機能や操作を変えたら、ここも直す）。

## 守ること

- **design.md 3 章（ADR）は確定事項。変更が必要なら実装前に人間に確認すること。** 14 章の未決事項は仮の既定値から外れない。
- core は OS・UI フレームワークに依存しない（標準ライブラリと vcpkg の依存だけ）。GPU（Metal）は `gpu/` に置き、core には枠組み（`GpuRenderer`）だけを置く。
- UI 層（`apps/macos`）は C API（`capi`）だけを使う。core の C++ ヘッダを直接 include しない。
- C++ 例外を C API の外に出さない。
- Swift 側に画像処理・カタログのロジックを書かない。
- LibRaw のインスタンスをスレッド間で共有しない（`libraw::raw_r` をリンク）。
- 元の RAW ファイルには一切書き込まない。消すのは、利用者が ⌥⌃⇧ Delete（確認ダイアログつき）で明示的に削除したときだけ（design.md 5.11 章、ADR-12）。カードからの取り込みもカードには書き込まない（ADR-12。コピー先に新規作成するだけで、既存のファイルは上書きしない）。
- 画像処理の正しさは GUI ではなく CLI とゴールデン画像で確認する。
- ゴールデン画像を更新するのは process_version を変えるときだけ。

## セットアップ

```sh
# vcpkg（公式の方法）。VCPKG_ROOT を設定しておく
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg

# テスト用 RAW（raw.pixls.us、CC0、チェックサム確認付き。tests/data/ は gitignore 対象）
tests/data/fetch.sh
```

## ビルド・テスト・ベンチ

```sh
cmake --preset release            # 初回は vcpkg が依存をビルドする（バイナリキャッシュ後は数秒）
cmake --build --preset release
ctest --preset release -j4        # ユニット + ゴールデン画像（RAW がなければゴールデンは skip）
ctest --preset release -E golden  # ユニットテストのみ

cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # ASan + UBSan
cmake --preset tsan && cmake --build --preset tsan --target focal_tests && ./build/tsan/tests/focal_tests "[editor]"   # TSan

./build/release/cli/focal bench tests/data/nikon_z7.NEF          # 11.1 章の性能計測（--clarity / --sharpness / --nr / --color-nr で (5a) ありも）
./build/release/cli/focal render in.NEF out.tif --ev 1 --temp 5000 --contrast 20
./build/release/cli/focal info in.NEF

# カタログ（--catalog / --cache、または FOCAL_CATALOG / FOCAL_CACHE。既定は ~/Library/.../jp.bekki.focal）
./build/release/cli/focal import ~/Pictures/RAW          # 登録 + 取り込み。再実行で再スキャン
./build/release/cli/focal ls --rating 3 --flag pick
./build/release/cli/focal export 12 34 --dest ~/Desktop/out --long-edge 2048   # 編集を反映して書き出す
./build/release/cli/focal preset ls ; focal preset save 風景 --from 12 ; focal preset apply user:風景 34   # 現像プリセット（--presets / FOCAL_PRESETS、--builtin-presets / FOCAL_BUILTIN_PRESETS）。import-card に --preset ID
./build/release/cli/focal sources                        # DCIM のあるボリューム（SD カード）を探す
./build/release/cli/focal import-card /Volumes/EOS --dest ~/Pictures/Photos --album 3 --tags "旅行/北海道"   # <dest>/YYYY/YYYY-MM-DD/ へコピーして登録（--dry-run で数えるだけ）
./build/release/cli/focal album create 旅行 ; focal album folder 2026 ; focal album smart "星4以上" --query '{"rules":[{"field":"rating","op":">=","value":4}]}'   # アルバム・フォルダ・スマートアルバム
bench/import_bench.sh 1000                               # 1,000 枚の取り込み計測（APFS のクローンを使う）
```

- ゴールデン画像の更新: `cmake --preset release -DFOCAL_UPDATE_GOLDEN=ON && ctest --preset release -R golden`、終わったら `-DFOCAL_UPDATE_GOLDEN=OFF` で再構成する。
- プリセット: macOS は `release` / `debug` / `asan` / `tsan`、Linux は `linux-release`（既定 x64、arm64 は `-DVCPKG_TARGET_TRIPLET=arm64-linux-focal`）、Windows は `windows-release`。C の API テストは POSIX を使うので Windows では作らない。
- リリースは `.github/workflows/release.yml`（`v*` のタグを push したときだけ。署名・公証した DMG を GitHub Releases に添付。手順と Secrets は `docs/release.md`。ブランチは main。版はタグで決め、`package.sh` に環境変数 `MARKETING_VERSION` / `FOCAL_RELEASE_CHANNEL` で渡す。Actions では未実行）。
- CI は `.github/workflows/ci.yml`（core を 3 OS、macOS アプリのビルドと XCTest、DMG）。まだ一度も実行していない（リポジトリ未作成）。Linux / Windows のビルドは未確認。

## macOS アプリ（apps/macos、表示名 Focal、バンドル ID jp.bekki.focal）

```sh
cd apps/macos && xcodegen          # project.yml から Focal.xcodeproj を生成（.xcodeproj はコミットしない）
open Focal.xcodeproj            # ビルド前スクリプトが scripts/build-core.sh で core を XCFramework にする
xcodebuild test -project Focal.xcodeproj -scheme Focal -destination 'platform=macOS' \
  -derivedDataPath build/DerivedData -only-testing:FocalCoreTests     # Swift ラッパーのテスト
apps/macos/scripts/ui-test.sh      # GUI スモークテスト（テスト用カタログを CLI で作り、スクリーンショットを build/ui-test に書き出す）
apps/macos/scripts/scroll-bench.sh # 10 万件のスクロール計測（初回はダミーライブラリを APFS のクローンで作る）
apps/macos/scripts/package.sh      # 配布物（build/dist/Focal <版>.dmg）。既定はローカル署名
# Developer ID で署名・公証: SIGN_IDENTITY="Developer ID Application: … (TEAMID)" NOTARY_PROFILE=… apps/macos/scripts/package.sh
```

- `scripts/build-core.sh`: core + capi + 静的リンクの依存を `build/FocalCore.xcframework`（module `CFocal`）にまとめ、LibRaw と libomp の dylib を `build/Frameworks` に置く。アプリのビルド時に `Contents/Frameworks` へコピーして署名する。
- `ui-test.sh` は、書き出しのテスト用フォルダと、色のチェックの基準（ニコンの写真を CLI で書き出して `focal colorgrid` でマスごとの平均色にしたもの）も用意する。外部ディスプレイで色のチェックをするときは `FOCAL_WINDOW_SCREEN=1 apps/macos/scripts/ui-test.sh … -only-testing:FocalUITests/FocalUITests/testDisplayColor`。
- 環境変数: `FOCAL_CATALOG`（このカタログを開く。.sqlite か .focalcatalog）/ `FOCAL_CACHE`（サムネイルのキャッシュ。大きいプレビューは隣の `〜-previews`）、`FOCAL_CATALOG_HOME`（新規カタログの既定の場所。前回のカタログと古い場所を見ず、記録もしない。初回起動のテスト用）、`FOCAL_EXPORT_DIR`（書き出し先の初期値）、`FOCAL_PRESETS` / `FOCAL_BUILTIN_PRESETS`（現像プリセットの利用者・同梱のフォルダ。UI テスト用）、`FOCAL_IMPORT_SOURCE` / `FOCAL_IMPORT_DEST`（カード取り込みシートの取り込み元と読み込み先。UI テスト用。指定すると設定を書き換えない）、`FOCAL_WINDOW_SCREEN=番号`（ウィンドウを置くディスプレイ）、`FOCAL_GRID=lazy`（比較用の LazyVGrid）、`FOCAL_FRAME_STATS=1`（ヒッチと現像の計測値を表示）、`FOCAL_SCROLL_BENCH=1`（自動スクロール）、`FOCAL_WINDOW_SIZE=幅x高さ`（ウィンドウの大きさ。UI テストは毎回 1440x900 を指定する。指定しないと前回の大きさが使われる。指定したときは、保存されたサイドバーとインスペクタの区分の開閉も消し、保存された外観も使わない）、`FOCAL_APPEARANCE=system|light|dark`（UI テストの外観）、`FOCAL_GPU=0`（表示を CPU で描く。`FOCAL_GPU=0 apps/macos/scripts/ui-test.sh` で UI テストにも渡る）、`FOCAL_GPU_TIMING=1`（GPU の実行時間を標準エラーに出す）。
- 文言は `Focal/Localizable.xcstrings`（英語がキー、日本語訳）。SwiftUI の `Text("…")` などは自動で引かれる。`String` を渡す API（`Button(cond ? "a" : "b")` など）は `String(localized:)` を使う。UI テストは表示言語に依らないよう、文言ではなくアクセシビリティ識別子で要素を探す。
- XCUITest のランナーはサンドボックス内で動くので、スクリーンショットは xcresult から書き出す。UI テストでスクロール性能を測らない（スクロール命令のたびにアクセシビリティの木を取得して 70ms ほど止まる）。
- アイコンは `Focal/AppIcon.icon`（Icon Composer の形式。ライト・ダークの背景と図形を持つ）。元の絵は `Icon/focal_icon.svg`。`scripts/make-icon.py` で作り直す（MacPorts の `rsvg-convert` が要る）。SVG のレイヤーは放射グラデーションが使えないので PNG にしている。icon.json で `fill` や `image-name` を `〜-specializations` と一緒に書くと、ダークの指定が無視される。見た目は `ictool` で確かめる（スクリプトの先頭のコメントを参照）。
- 版は `apps/macos/project.yml` の `MARKETING_VERSION`（数字だけ。現在 26.0.0）と `FOCAL_RELEASE_CHANNEL`（β など。空なら正式版）。後者は Info.plist の `FocalReleaseChannel` に入り、「Focal について」の版表示（`AppInfo.displayVersion`）と `package.sh` の DMG の名前（`Focal 26.0.0 β.dmg`）に使う。`CFBundleShortVersionString` に β は入れられない（数字の並びだけ）。
- アプリは arm64 専用（`ARCHS: arm64`）。Release のローカル署名では Hardened Runtime を無効にする（ad-hoc には Team ID がなく、ライブラリ検証で同梱のフレームワークが読み込めないため）。
- SwiftUI の性能（v3.15、Time Profiler で確認）: NSViewRepresentable は `sizeThatFits` で大きさを返し、フォームの行に置くときは `.baselineAtCenter()` を付ける（AppKit にベースラインを問い合わせると外観の解決で重い）。頻繁に更新される重いビュー（インスペクタ）は `IsolatedHost` に入れる。ウィンドウの最小サイズは `.frame(minWidth:)` にしない。`@Observable` のプロパティは値が変わったときだけ代入する。
- 遅さを調べるとき: `apps/macos/scripts/ui-test.sh … -only-testing:FocalUITests/FocalUITests/testDevelopTimings` を動かしながら `xcrun xctrace record --template 'Time Profiler' --attach <pid> --time-limit 40s --output x.trace` で取り、`xcrun xctrace export --input x.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'` の XML をメインスレッドで集計する。計測値の `wait_p50_ms`（core に頼むまで）と `core_p50_ms`（core の描画）で、アプリ側か core 側かを切り分けられる。
- Swift は 6 モード。C のハンドルを持つクラスは `@unchecked Sendable`（C API がスレッド安全なため）。

## 依存ライブラリの構成（M0 で確認済み）

- vcpkg のオーバーレイトリプレット `triplets/arm64-osx-focal.cmake`: **LibRaw と libomp を動的リンク**（`libraw_r.dylib`、`libomp.dylib`）、他は静的リンク（ADR-01 / ADR-11）。
- LibRaw 0.22.2、feature `dng-lossy` と **`openmp` を有効**。スレッド安全版 `raw_r` を使う。
- オーバーレイポート（`ports/`、プリセットで `VCPKG_OVERLAY_PORTS` に設定済み）:
  - `llvm-openmp`: LLVM 23.1.2 の libomp。llvm-project の tarball から必要なディレクトリだけ展開し、`runtimes/` 経由でビルドする（LLVM 23 で OpenMP の standalone ビルドは廃止）。`share/openmp/vcpkg-cmake-wrapper.cmake` で `find_package(OpenMP)` がこの libomp を使うようにしている（Apple clang は `-Xclang -fopenmp` が必要）。install name は `@rpath/libomp.dylib`。
  - `libraw`: vcpkg 本体のポートの複製。`openmp` feature が macOS で `llvm-openmp` に依存するようにした点だけが違う。vcpkg のベースラインを上げるときは本体の変更を取り込むこと。
- OpenMP の効果（M0 実測、M4 10 コア）: Z7 45MP 1.17s → 0.63s、X-Trans 14.5s → 2.3s、CR3 0.55s → 0.18s。
- LibRaw の `imgdata.image` は `dcraw_process()` 後も向き未適用で、flip は `dcraw_make_mem_image()` 等の出力時に `flip_index()` で適用される（`tests/test_raw.cpp` で確認）。自前の向き補正は `flip_index` と同じ対応にしてある。
- `dcraw_process()` 後の `imgdata.color.pre_mul` が実際に適用された As Shot 係数。昼光係数は `dcraw_process()` の前に取る。

## 既知の問題

- Xcode は、ビルド前スクリプト（`build-core.sh`）が XCFramework を作り直す前に、その中身（ヘッダと .a）を `BUILT_PRODUCTS_DIR` にコピーする。そのままだと C API のヘッダを変えた直後のビルドが古いヘッダで Swift をコンパイルする（「API version mismatch」やメンバーがないエラー）。`build-core.sh` が Xcode から呼ばれたときにそのコピーと CFocal のモジュールキャッシュを更新して防いでいる。それでも起きたら `apps/macos/build/DerivedData/Build/Intermediates.noindex/Focal.build/Debug/FocalCore.build` と `SwiftExplicitPrecompiledModules` を消す。

- グリッド ⇄ ビューアを切り替えると、SwiftUI がインスペクタ（現像パネル）を作り直すレイアウトでメインスレッドが止まる（初回 約 190ms、以降 約 60ms、M4 で Time Profiler により確認）。スライダー操作やスクロールには影響しない。後で、パネルを作り直さない構成にして詰める。
- **OpenMP 有効時、LibRaw の X-Trans 処理は実行ごとに結果がわずかに変わる**（design.md 5.1 章）。フル解像度では約 0.01% の画素が最大 5/255、`half_size` では行単位で最大 72/255 違う。Bayer 機はビット単位で同じ。受け入れ済み。X-Trans のゴールデン画像テストを `--half` で作らないこと。
- libomp のワーカーはパラレル領域の後に既定で 200ms スピン待ちする（`KMP_BLOCKTIME`）。M0 のベンチでは影響はなかったが、M3 でデコードとレンダリングを並行させるときに確認する。
- M1 のサムネイルプールでは、各ワーカーで `omp_set_num_threads(1)` を呼び、OpenMP スレッドが掛け算で増えないようにする（design.md 5.1 章）。

## コード構成

- `core/imaging/` — `raw_decoder`（LibRaw）、`resample`（プロキシ・Lanczos3）、`white_balance`（Robertson 法の K/tint ⇔ 係数）、`tone_curve`（5.5 章 + ベースカーブ + 白・黒・明るさ、4096 要素 LUT）、`color_pipeline`（5.4 章 (2)〜(5b)。(5b) は彩度・自然な彩度。`process_tone` と `process_color` に分けられる）、`local_contrast`（(5a) 明瞭度。ガイデッドフィルタ、縮小して計算）、`detail`（(5a) ノイズ低減・シャープネス。半径はフル解像度の画素）、`geometry`（5.6 章の逆写像・自動クロップ）、`output_transform`（lcms2、(6)(7)）、`renderer`（(1)〜(7) の組み立て）、`image_io`（TIFF / JPEG）
- `core/edit/settings` — 6.1 章の JSON（未知キー保持）
- `core/edit/preset` — 現像のプリセット（v3.21、design.md 6.3 章）。調整だけ（`geometry` は含めない）、`apply_preset`（調整は丸ごと置き換え、geometry・processVersion・未知キーは写真のまま）、`PresetStore`（1 プリセット 1 JSON ファイル `.focalpreset`、利用者のフォルダ + 同梱の読み取り専用フォルダ、id は `user:` / `builtin:`）。`Catalog::apply_preset`、`CardImportOptions::preset`。Focal 標準は `apps/macos/Presets/`（README.md に登録の手順）。`PresetStore` は一覧・照合の結果をキャッシュ（`find_match` はスライダー操作のたびに呼ぶ。save / remove / rename / list で読み直す）。右ペインの「プリセット」区分は `DevelopPanel.presetSection`、状態は `PresetModel`（`appliedID`・`revision`）。プリセットの項目を足すときは `settings` と同じ項目を `apply_preset` が丸ごと置き換えることを確認する
- `core/catalog/` — `sqlite`（RAII ラッパー）、`schema`（7.2 章とマイグレーション。移行前に `VACUUM INTO` でバックアップ）、`db_writer`（書き込み専用スレッド。最大 500 件を 1 トランザクションにまとめ、ジョブごとに SAVEPOINT）、`catalog`（ルート登録・スキャン・照合・絞り込み・★/フラグ/タグ・アルバム・最近の取り込み）
- `core/imaging/image_io` — 書き出しの JPEG・TIFF。`ExifInfo` でカタログの撮影情報を書く（JPEG は APP1 に最小限の EXIF、TIFF は標準タグ。design.md 5.8 章）。EXIF の読み戻しはテスト用（自分が書く項目だけ）
- `core/thumbs/thumbnail` — 埋め込みプレビューの選択と抽出、向き補正、フォールバック（half_size + 既定パイプライン、同時実行はコア数の半分、OpenMP 1 スレッド）、ディスクキャッシュ、`rendered_key`（現像結果のキー）、`PreviewCache`（大きいプレビュー、上限付き LRU）
- `core/util/volume` — ボリュームの ID・名前・マウントポイント（macOS は `getattrlist` の UUID、Windows はボリューム GUID、Linux は `/dev/disk/by-uuid`）。macOS は Data ボリュームを "/" として扱う。Linux / Windows の実装は未確認（macOS 以外ではビルドしていない）
- `Localizable.xcstrings` に文言を足したら `apps/macos/scripts/sort-strings.py` を実行する（Xcode と同じ並びにそろえ、Xcode で開いたときの差分を減らす）。
- ルートの接続確認は `directories_reachable`（別スレッド + 時間切れ。応答しない NAS で止まらない）。SMB・NFS のボリューム ID は共有の場所から作る（`network_volume_id`）。実際の NAS では未確認（ユニットテストと macOS のローカルボリュームのみ）。
- `core/import/card_import` — SD カードなどの取り込み（v3.19、design.md 5.10 章）。1 枚の単位（RAW + JPEG + サイドカー）、日付フォルダ、重複判定、一時ファイル + BLAKE3 検証のコピー、コピーしたファイルだけの登録（`Catalog::register_files`。ルートは走査しない）と登録の確認、アルバム・タグ付け
- `core/catalog/photo_delete` — 写真の削除（v3.19、5.11 章）。RAW + 同じ名前の幹の JPEG・サイドカー、ネットワークボリュームは完全削除、ローカルはゴミ箱（呼び出し側の `TrashFn`）。RAW を消せなければファイルもカタログも残す
- `core/catalog/smart_query` — スマートアルバムの条件（JSON → SQL の断片。検証も）
- `core/util/` — スレッドプール（latest-wins 用の `CancelToken`）、行列、画像バッファ、例外、`unicode`（utf8proc で NFC と case folding）、`hash`（BLAKE3、quick_hash）、`file`（UTF-8 ⇔ path、NFC のパスから実ファイルを解決）、`omp_threads`
- `gpu/` — 表示用の GPU レンダラー（Metal、metal-cpp、macOS のみ）。`src/shaders.metal` は CPU 版と同じ式で書き、実行時にコンパイルする（CMake が C++ の文字列にする）。capi が Editor に渡す。`tests/test_gpu.cpp` で CPU 版との一致を確かめる
- `capi/` — C API（`include/focal/focal.h`）。全関数で例外を捕まえてステータスに変換。配列は core が確保し `fc_*_array_free` で解放。テストは C だけで書いた `tests/capi/test_capi.c`
- `core/thumbs/thumbnail_service` — グリッド用のサムネイル要求キュー（新しい要求を優先、開始前ならキャンセル可、コールバックは必ず 1 回）
- `apps/macos/FocalCore/` — Swift ラッパー（`Catalog`、`Thumbnailer`、スキャンは `AsyncThrowingStream`）
- `core/imaging/crop_tool` — クロップモードの計算（ドラッグ、最大の枠、90° 回転、水平線ツール）。M4
- `core/export/exporter` — 書き出し（M5）。名前の確保、1 枚ずつ書き出し、キャンセル
- `core/edit/editor` — 現像ビューア（M3）。編集の保存時と写真を閉じるときに現像結果のサムネイルを、閉じるときに大きいプレビュー（`set_preview_cache`）も作る。開くときは大きいプレビューのキャッシュを埋め込みプレビューより先に使う（`SessionInfo::preview_display_p3`）。`Editor` がプレビュー・デコード（先読み 1 枚、打ち切り可）・レンダリング（latest-wins）・保存（500ms デバウンス）のスレッドを持つ。`EditSession` が写真 1 枚の Settings と Undo（`undo_stack`、直近 20 枚分）
- `apps/macos/Focal/` — SwiftUI アプリ（`LibraryModel` が状態、`PhotoCollectionView` がグリッド、`KeyMonitor` が単キー操作、`DevelopModel` / `DevelopView`（NSView + CALayer、クロップ枠の重ね描き）/ `DevelopPanel` / `CropToolbar` が現像。`AppState`（`FocalApp.swift`）がカタログを開く・作る・切り替える・記録する、`AppPaths` が保存先、`WelcomeView` が初回起動、`SettingsView` が設定ウィンドウ。v3.16: `SidebarView` がライブラリとアルバム、`FilterBar` が絞り込みバー、`PhotoCollectionView(style: .filmstrip)` が現像画面のフィルムストリップ、`DevelopTools` がインスペクタ上部の現像の操作、`IsolatedHost` がインスペクタを別の NSHostingView に入れる。v3.19: `SidebarView` がストレージ（ボリューム ▸ ルート ▸ フォルダ）とアルバムのツリー、`SmartAlbumSheet` / `SmartQuery.swift` がスマートアルバムの編集（条件の行 ⇔ JSON の変換だけ。意味づけは core）、`ImportSheet` / `CardImportModel` がカードの取り込み、`FolderWatcher` が表示中のルートの FSEvents 監視）
- `cli/` — `render` / `info` / `compare` / `bench`（実装は `bench/cmd_bench.cpp`）/ `thumb` / `import` / `roots` / `ls` / `rate` / `flag` / `tag` / `export` / `colorgrid` / `sources` / `import-card` / `album`（後の 3 つは `cmd_library.cpp`）
- `tests/` — Catch2 のユニットテスト、`tests/golden/` のゴールデン画像（CLI でレンダリングして比較）

## 実装上の注意

- 周辺画素を使う処理（ノイズ低減 → 明瞭度 → シャープネス）は `renderer.cpp` の `render_neighborhood` が、描く範囲の外側に余白を足して (5) まで描き、(5a) をかけてから切り出す。ぼかしの大きさはセンサーの長辺に比例させ、縮小の区切りは出力画像の座標に合わせる（どの縮尺・どの範囲で描いても同じ見た目にするため）。
- `ThreadPool::parallel_for` のラムダはワーカースレッドで動く。`thread_local` の変数をラムダの中で直接使うと、ワーカーの別のインスタンス（空）を参照して壊れる。呼び出し元で参照を取ってから使う。
- 表示の計算を変えるとき（パイプライン・(5a) の式や定数）は、CPU 版（core）と GPU 版（`gpu/src/shaders.metal`）の両方を変える。パラメータの導き方は共有の関数（`plan_neighborhood`、`plan_local_contrast`、`detail::*`）を使う。シェーダーのコンパイルに失敗すると黙って CPU で描くことになるので、`tests/test_gpu.cpp`（デバイスがあるのに作れなければ失敗）を必ず通す。
- 現像パラメータを足すときは、既定値で何もしない形にする（既存の写真の見た目もゴールデン画像も変えない）。6.1 章の JSON・`Settings`（`operator==`、`clamp`、`settings_need_no_row` の既知キー）・`fc_settings`（`FC_API_VERSION` を上げる）・Swift の `DevelopSettings`・CLI の `render` の各オプションをそろえて変える。

- 出力変換の U8 経路は、lcms2 から取り出した行列と TRC 表で直接計算する（float 入力の `cmsDoTransform` は 1 スレッド 80ns/画素で 16ms 予算に収まらないため）。両プロファイルが matrix-shaper なので結果は同じで、`apply_lcms()` との差 ±1 以内をテストしている。U16（書き出し）は `cmsDoTransform` を使う。
- 画像処理ホットパスは `-O3`。再現性のため `-ffast-math` は使わない。
- カタログに保存するパス・ファイル名・タグ名はすべて NFC。ファイルを開くときは `Catalog::photo_disk_path()`（`resolve_nfc_path`）を使い、保存した文字列をそのまま開かない。
- スキャンの照合はフォルダごとに「完全一致 → case folding で一致」の 2 段階。大文字小文字だけのリネームは同じ写真として扱い、★・フラグ・タグを引き継ぐ。別フォルダへの移動は、ファイル名・サイズ・撮影日時が同じ「ファイルなし」の写真がちょうど 1 枚あればその写真につなぎ直す（v3.19。★・フラグ・タグ・アルバム・編集を引き継ぐ。候補が複数なら新規）。
- ルートは入れ子にしない。`add_root` は既存ルートの中なら既存の id を返し、既存ルートを含むなら `merge_root_into` で統合（`merge_photo_rows` が編集・★・タグ等を保つ）。破壊的な操作（`remove_root`、統合）の前に `Catalog::backup()` が `catalog.sqlite.before-<理由>-<日時>-NN.bak` を作る（理由ごとに 5 つ残す）。`relocate_root`・`nested_roots`・`merge_nested_roots`（CLI は `relocate` / `merge-roots`）。
- スキャンで新規の写真が既存の写真と quick_hash + サイズ一致なら `inherit_from_copies` が編集・★・フラグ・タグを複製（元ファイルがディスクになければ行をつなぎ直す）。ディスクの存在確認は DB スレッドの外。
- ルートフォルダにアクセスできない（外付けドライブを外した等）ときは、写真をファイルなしにせず Error を投げる。
- `DbWriter::call()` はコミット後に戻るので、直後に読み取り接続から結果が見える。
- 現像のコールバック（セッションのイベント、レンダリング結果）は core のスレッドから来る。Swift 側では `Task { @MainActor … }` で戻す。セッションのコールバックの中で `close` しない（close は実行中のコールバックの終了を待つ）。
- `Editor` は開いている `EditSession` より長く生きている必要がある（Swift の `Session` が `Editor` を参照で持つ）。
- アプリの終了時とカタログの切り替え時は `LibraryModel.prepareForTermination()` でセッションを閉じ、`Catalog.flush()` で保存の完了を待つ。
- アプリのカタログの既定は `~/Pictures/Focal/〜.focalcatalog` だが、CLI の既定は今も `~/Library/Application Support/jp.bekki.focal/catalog.sqlite`（CLI はパッケージを開くとき `--catalog 〜.focalcatalog/catalog.sqlite` と指定する）。
- アルバムは `albums.kind`（0 アルバム / 1 フォルダ / 2 スマート）と `parent_id`。写真を足せるのは kind 0 だけ（core が Error にする）。スマートアルバムの条件を足すときは `smart_query.cpp`（検証と SQL）・design.md 7.2 章の表・`SmartQuery.swift`（編集の行）をそろえる。名前の一意は「同じ親の下」で、v2 の `albums` は v3 のマイグレーションで作り直している（`album_photos` を先に消してから `albums` を消す）。
- ルートの `path` は「最後に見たマウントポイントからの絶対パス」。`Catalog::open` と、ボリュームの接続・取り外しの通知（アプリ）で `refresh_volumes()` が合わせ直す。すでに同じ場所を指しているなら書き換えない（サムネイルのキャッシュのキーがパスを含むため）。
- アプリのスキャンは同時に 1 つだけ（`LibraryModel.scan`。同じルートを 2 つのスキャンが書くと重複するため、待ち行列に積む）。起動時の再スキャンは `AppPaths.isolatedFromDefaults`（UI テスト）のときは行わない。
- 既知の未確認: GUI のカード取り込みは、実際の SD カードではなくフォルダ（`FOCAL_IMPORT_SOURCE`）でテストしている。カードの検出（`detect_import_sources`）は macOS で DCIM を持つボリュームを探すだけで、実機のカードでは確認していない。
