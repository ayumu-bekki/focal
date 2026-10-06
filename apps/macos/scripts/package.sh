#!/usr/bin/env bash
# 配布物（DMG）を作る（13 章 M6、ADR-14）。
#
#   apps/macos/scripts/package.sh                 ローカル署名（ad-hoc）。この Mac で使う分にはこれで足りる
#   SIGN_IDENTITY="Developer ID Application: 名前 (TEAMID)" NOTARY_PROFILE=プロファイル名 \
#     apps/macos/scripts/package.sh               Developer ID で署名し、公証して staple する
#
# NOTARY_PROFILE は `xcrun notarytool store-credentials <名前>` で登録したキーチェーンのプロファイル。
# 出力: apps/macos/build/dist/Focal <版>.dmg
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
MAC="$ROOT/apps/macos"
DIST="$MAC/build/dist"
IDENTITY="${SIGN_IDENTITY:--}"
export PATH="/opt/local/bin:$PATH"

# 版は project.yml から。環境変数 MARKETING_VERSION / FOCAL_RELEASE_CHANNEL / CURRENT_PROJECT_VERSION があればそちらを使う
# （GitHub のリリースはタグから決める。FOCAL_RELEASE_CHANNEL は空の指定で正式版）
NUMBER="${MARKETING_VERSION:-$(awk -F'"' '/^ *MARKETING_VERSION:/{print $2; exit}' "$MAC/project.yml")}"
CHANNEL="${FOCAL_RELEASE_CHANNEL-$(awk -F'"' '/^ *FOCAL_RELEASE_CHANNEL:/{print $2; exit}' "$MAC/project.yml")}"  # 例: β
VERSION="$NUMBER"
[[ -n "$CHANNEL" ]] && VERSION="$NUMBER $CHANNEL"
echo "== Focal ${VERSION}（署名: ${IDENTITY}）"

"$MAC/scripts/build-core.sh"
cd "$MAC"
xcodegen -q
DERIVED="$MAC/build/DerivedData-release"
SIGN_ARGS=(CODE_SIGN_IDENTITY="$IDENTITY" MARKETING_VERSION="$NUMBER" FOCAL_RELEASE_CHANNEL="$CHANNEL")
[[ -n "${CURRENT_PROJECT_VERSION:-}" ]] && SIGN_ARGS+=(CURRENT_PROJECT_VERSION="$CURRENT_PROJECT_VERSION")
if [[ "$IDENTITY" == "-" ]]; then
  # ローカル署名（ad-hoc）には Team ID がないので、Hardened Runtime のライブラリ検証で
  # 同梱のフレームワークが読み込めない。Hardened Runtime は公証のためのもので、ローカルでは不要
  SIGN_ARGS+=(ENABLE_HARDENED_RUNTIME=NO)
else
  TEAM=$(sed -E 's/.*\(([A-Z0-9]+)\)$/\1/' <<<"$IDENTITY")
  # CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO: Xcode が付けるデバッグ用の権限（com.apple.security.get-task-allow）を入れない。
  # 付いていると、公証が「The executable requests the com.apple.security.get-task-allow entitlement」で却下する
  SIGN_ARGS+=(DEVELOPMENT_TEAM="$TEAM" OTHER_CODE_SIGN_FLAGS="--timestamp" CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO)
fi
xcodebuild build -project Focal.xcodeproj -scheme Focal -configuration Release -destination 'platform=macOS' \
  -derivedDataPath "$DERIVED" "${SIGN_ARGS[@]}" 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | sort -u
APP="$DERIVED/Build/Products/Release/Focal.app"
[[ -d "$APP" ]] || { echo "build failed"; exit 1; }

echo "== 依存の検査（開発環境への参照がないこと）"
bad=0
while IFS= read -r -d '' f; do
  file "$f" | grep -q "Mach-O" || continue
  # 参照しているライブラリ（1 行目は自分自身の名前）
  while read -r dep; do
    case "$dep" in
      @rpath/*|@executable_path/*|@loader_path/*|/usr/lib/*|/System/*) ;;
      *) echo "  NG: ${f#$APP/} -> $dep"; bad=1 ;;
    esac
  done < <(otool -L "$f" | tail -n +2 | awk '{print $1}')
  # rpath に開発環境の絶対パスがないこと（OS の Swift ランタイムとアプリ内の相対パスは可）
  while read -r rp; do
    case "$rp" in
      @executable_path|@executable_path/*|@loader_path|@loader_path/*|/usr/lib/*|/System/*) ;;
      *) echo "  NG: ${f#$APP/} の rpath ${rp}"; bad=1 ;;
    esac
  done < <(otool -l "$f" | awk '/LC_RPATH/{getline; getline; print $2}')
done < <(find "$APP" -type f -print0)
[[ $bad == 0 ]] || { echo "開発環境に依存しているため中止"; exit 1; }
echo "  OK"
ls "$APP/Contents/Resources/Licenses" >/dev/null || { echo "ライセンス文がない"; exit 1; }
for f in "Lensfun (LGPL-3.0).txt" "Lensfun database (CC BY-SA 3.0).txt" "GLib.txt"; do
  [[ -f "$APP/Contents/Resources/Licenses/$f" ]] || { echo "ライセンス文がない: $f"; exit 1; }
done
ls "$APP/Contents/Resources/LensfunDB"/*.xml >/dev/null || { echo "レンズ DB が同梱されていない"; exit 1; }

echo "== 署名の検証"
codesign --verify --deep --strict --verbose=1 "$APP" 2>&1 | tail -1
codesign -dv "$APP" 2>&1 | grep -E "Authority|flags=|TeamIdentifier" | head -4

echo "== DMG"
mkdir -p "$DIST"
STAGE="$MAC/build/dmg-stage"
rm -rf "$STAGE" && mkdir -p "$STAGE"
ditto "$APP" "$STAGE/Focal.app"
ln -s /Applications "$STAGE/Applications"
DMG="$DIST/Focal $VERSION.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "Focal $VERSION" -srcfolder "$STAGE" -format UDZO -fs HFS+ "$DMG"
[[ "$IDENTITY" != "-" ]] && codesign --force --sign "$IDENTITY" --timestamp "$DMG"

if [[ -n "${NOTARY_PROFILE:-}" ]]; then
  [[ "$IDENTITY" == "-" ]] && { echo "公証には Developer ID の署名が要る（SIGN_IDENTITY）"; exit 1; }
  echo "== 公証"
  # 却下（Invalid）でも submit は成功で返るので、結果を見て、Accepted でなければ Apple のログを出して止める
  RESULT=$(xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait 2>&1) || true
  echo "$RESULT"
  if ! grep -q "status: Accepted" <<<"$RESULT"; then
    SUBMISSION=$(awk '/^ *id:/{print $2; exit}' <<<"$RESULT")
    echo "== 公証が通らなかった。Apple のログ（${SUBMISSION}）"
    [[ -n "$SUBMISSION" ]] && xcrun notarytool log "$SUBMISSION" --keychain-profile "$NOTARY_PROFILE" || true
    exit 1
  fi
  xcrun stapler staple "$DMG"
  spctl --assess --type open --context context:primary-signature -v "$DMG"
fi
echo "== $DMG ($(du -h "$DMG" | cut -f1))"
