#!/usr/bin/env python3
"""利用者向けガイド（docs/user-guide.md）のスクリーンショットを docs/images/ に作る。
使い方:
  1. ui-test.sh の出力フォルダを用意する（例: apps/macos/scripts/ui-test.sh /tmp/shots -only-testing:FocalUITests/FocalUITests/testDocScreenshots ...）
     撮るテスト: testDocScreenshots（現像・切り取り・一覧）、testLibraryOrganization（アルバム・スマートアルバム）、
     testFilterBar、testExport、testCardImport、testDeleteRequiresChord、testFirstRunCreatesCatalog、testAlbumOrderAndCover
  2. apps/macos/scripts/make-doc-images.py /tmp/shots [/tmp/shots2 …]   （後ろのフォルダの画像が優先）
写真が多い画像は JPEG（容量を抑える）、画面だけの画像は PNG にして、長辺 1500px に縮める。"""
import json, pathlib, subprocess, sys

# 画像の名前（UI テストの saveScreenshot の name）→ (docs/images のファイル名, 形式)
IMAGES = {
    'first-run-welcome': ('welcome.png', 'png'),
    'doc-grid': ('grid.jpg', 'jpeg'),
    'doc-develop': ('develop.jpg', 'jpeg'),
    'doc-develop-edited': ('develop-edited.jpg', 'jpeg'),
    'doc-crop': ('crop.jpg', 'jpeg'),
    'filter-bar': ('filter-bar.png', 'png'),
    'smart-album': ('sidebar-albums.jpg', 'jpeg'),
    'smart-album-sheet': ('smart-album-sheet.png', 'png'),
    'album-cover': ('album-cover.jpg', 'jpeg'),
    'import-sheet': ('import-sheet.png', 'png'),
    'import-finished': ('import-finished.png', 'png'),
    'export-settings': ('export-settings.png', 'png'),
    'delete-confirm': ('delete-confirm.png', 'png'),
    'delete-menu': ('delete-menu.png', 'png'),
}

out_dir = pathlib.Path(__file__).resolve().parents[3] / 'docs' / 'images'
out_dir.mkdir(parents=True, exist_ok=True)
found = {}
for d in map(pathlib.Path, sys.argv[1:]):
    for test in json.loads((d / 'manifest.json').read_text()):
        for a in test['attachments']:
            name = a['suggestedHumanReadableName'].split('_0_')[0]
            if name in IMAGES and a['exportedFileName'].endswith('.png'):
                found[name] = d / a['exportedFileName']
for name, (file, fmt) in IMAGES.items():
    if name not in found:
        print('なし:', name)
        continue
    cmd = ['sips', '-Z', '1500', str(found[name]), '--out', str(out_dir / file)]
    if fmt == 'jpeg':
        cmd[1:1] = ['-s', 'format', 'jpeg', '-s', 'formatOptions', '82']
    subprocess.run(cmd, check=True, capture_output=True)
    print(file, (out_dir / file).stat().st_size // 1024, 'KB')
