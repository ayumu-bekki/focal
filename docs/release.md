# リリースの手順（GitHub Actions、タグ駆動）

`.github/workflows/release.yml` が、**`v` で始まるタグを push したときだけ**、署名・公証した DMG を GitHub Releases に添付します。
`main` にマージしただけではリリースしません（意図しないリリースを防ぐため）。版はタグで決めます。

## 流れ

1. `develop` で開発する。
2. リリースノートを `docs/release-notes/<タグ>.md`（例: `docs/release-notes/v26.0.2-beta.md`）に書いてコミットする（下の「リリースノート」）。
3. `main` にマージする。
4. `main` の上でタグを打って push する。

```sh
git checkout main && git pull
git tag v26.0.0            # β なら v26.0.0-beta（プレリリースになる）
git push origin v26.0.0
```

5. Actions の「Release」が次を行う（macos-latest）。
   1. タグの形と、`main` の履歴にあることを確認（違えば中止）
   2. 署名・公証の Secrets がそろっていることを確認（足りなければ、署名なしでは出さずに中止）
   3. core のビルド、Swift ラッパーのテスト（FocalCoreTests）
   4. Developer ID の証明書を一時キーチェーンに入れる
   5. `apps/macos/scripts/package.sh`: ビルド → 署名 → DMG → 公証（notarytool）→ staple
   6. GitHub Releases に DMG と SHA-256 を添付。本文は 注意書き + `docs/release-notes/<タグ>.md` + 自動の変更一覧

## リリースノート

- `docs/release-notes/<タグ>.md` に、利用者向けの変更点（大切な変更・新機能・不具合の修正）を Markdown で書く。冒頭の「ご利用にあたって」の注意書きはワークフローが付けるので、書かない。
- ファイル名はタグと同じにする（`v26.0.2-beta` なら `v26.0.2-beta.md`）。**タグを打つコミットに入っていること**（ワークフローはタグのコミットを読む）。
- ファイルがなければ止めずに、注意書きと自動の変更一覧だけで公開する（Actions のログに警告が出る）。あとから足すときは `gh release edit <タグ> --notes-file <ファイル>`（本文を丸ごと置き換えるので、注意書きも含めたファイルを渡す）。
- 公開後に直すときも `gh release edit`。リポジトリのファイルも同じように直しておく。

## タグと版

| タグ | アプリの版 | 表示 | リリース |
|---|---|---|---|
| `v26.0.0` | 26.0.0 | 26.0.0 | 正式版 |
| `v26.0.0-beta` / `-beta.2` | 26.0.0 | 26.0.0 β | プレリリース |
| `v26.0.0-rc.1` | 26.0.0 | 26.0.0 rc.1 | プレリリース |

- ビルド番号（`CFBundleVersion`）は Actions の実行番号です。
- タグの版は `MARKETING_VERSION`、種類は `FOCAL_RELEASE_CHANNEL` として `package.sh` に渡されます（`project.yml` の値より優先）。`project.yml` の値は手元の開発用です。
- DMG は `Focal-26.0.0-beta.dmg` のように、空白や β を避けた名前で添付されます。

## Secrets（リポジトリの Settings ▸ Secrets and variables ▸ Actions）

| 名前 | 内容 |
|---|---|
| `MACOS_CERTIFICATE_P12` | Developer ID Application 証明書（秘密鍵つき）を書き出した `.p12` を base64 にしたもの（`base64 -i cert.p12 \| pbcopy`） |
| `MACOS_CERTIFICATE_PASSWORD` | その `.p12` のパスワード |
| `APPLE_ID` | 公証に使う Apple ID |
| `APPLE_APP_PASSWORD` | その Apple ID の App 用パスワード（appleid.apple.com で作る） |
| `APPLE_TEAM_ID` | Team ID（10 桁） |

## 失敗したとき・やり直し

- 中止（タグが `main` にない・Secrets 不足など）は、直してからタグを打ち直す: `git tag -d v26.0.0 && git push origin :refs/tags/v26.0.0` で消し、同じコミットかマージ後のコミットに打つ。
- 公証が通らないときは、Actions のログの `notarytool submit` の出力（`xcrun notarytool log <id>`）を見る。
- 手元で同じ DMG を作る: `SIGN_IDENTITY="Developer ID Application: … (TEAMID)" NOTARY_PROFILE=… MARKETING_VERSION=26.0.0 FOCAL_RELEASE_CHANNEL=β apps/macos/scripts/package.sh`（環境変数を付けなければ `project.yml` の値）。

## 確認済み

- `v26.0.0-test` で、署名 → 公証 → staple → GitHub Releases への添付まで通ることを確認した（ダウンロードした DMG が `stapler validate`・`spctl` で Notarized Developer ID）。
- 公証が却下される主な原因は、Xcode が付けるデバッグ用の権限 `com.apple.security.get-task-allow`（`package.sh` が `CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO` で防ぐ）。`package.sh` は、公証が Accepted でなければ Apple のログを出して止まる。
- テスト用のリリース（`v26.0.0-test`）は、不要になったら GitHub の Releases から削除し、タグも消す: `gh release delete v26.0.0-test --cleanup-tag`。
