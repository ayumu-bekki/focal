# リリースの手順（GitHub Actions、タグ駆動）

`.github/workflows/release.yml` が、**`v` で始まるタグを push したときだけ**、署名・公証した DMG を GitHub Releases に添付します。
`main` にマージしただけではリリースしません（意図しないリリースを防ぐため）。版はタグで決めます。

## 流れ

1. `develop` で開発する。
2. `main` にマージする。
3. `main` の上でタグを打って push する。

```sh
git checkout main && git pull
git tag v26.0.0            # β なら v26.0.0-beta（プレリリースになる）
git push origin v26.0.0
```

4. Actions の「Release」が次を行う（macos-latest）。
   1. タグの形と、`main` の履歴にあることを確認（違えば中止）
   2. 署名・公証の Secrets がそろっていることを確認（足りなければ、署名なしでは出さずに中止）
   3. core のビルド、Swift ラッパーのテスト（FocalCoreTests）
   4. Developer ID の証明書を一時キーチェーンに入れる
   5. `apps/macos/scripts/package.sh`: ビルド → 署名 → DMG → 公証（notarytool）→ staple
   6. GitHub Releases に DMG と SHA-256 を添付（リリースノートは自動生成）

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

## 未確認

- ワークフロー全体は、まだ一度も Actions で実行していない（Secrets がなく、リポジトリでの実行は未確認）。`package.sh` の署名なし（ad-hoc）の経路は手元で確認済み。署名・公証の経路は、実際の証明書では未確認。
