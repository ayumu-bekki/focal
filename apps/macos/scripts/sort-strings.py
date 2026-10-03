#!/usr/bin/env python3
"""Localizable.xcstrings のキーを、Xcode と同じ並び（記号 < 数字（数値順）< 英字（大文字小文字を区別しない））にそろえる。
手で・スクリプトで文言を足したあとに実行すると、Xcode で開いたときに差分が大きく出ない。
使い方: apps/macos/scripts/sort-strings.py"""
import json, re, pathlib

P = "_-,;:!?.'\"“”‘’()[]{}@*/\\&#%…"
S = "`^+<=>≥≤|~$"


def key(k):
    out = []
    for m in re.finditer(r'\d+|.', k, flags=re.S):
        t = m.group(0)
        if t.isdigit(): out.append((3, int(t), ''))
        elif t.isspace(): out.append((0, 0, ''))
        elif t in P: out.append((1, P.index(t), ''))
        elif t in S: out.append((2, S.index(t), ''))
        elif t.isalpha() and ord(t) < 128: out.append((4, 0, t.casefold()))
        else: out.append((5, ord(t), ''))
    return out


def dump(d):
    out = json.dumps(d, indent=2, ensure_ascii=False, separators=(',', ' : '))
    # Xcode は空の辞書を「{」「空行」「}」で書く
    return re.sub(r'^(\s*)("(?:[^"\\]|\\.)*" : )\{\}', lambda m: f'{m.group(1)}{m.group(2)}{{\n\n{m.group(1)}}}', out, flags=re.M)


path = pathlib.Path(__file__).resolve().parent.parent / 'Focal' / 'Localizable.xcstrings'
d = json.loads(path.read_text(encoding='utf-8'))
d['strings'] = dict(sorted(d['strings'].items(), key=lambda kv: key(kv[0])))
path.write_text(dump(d), encoding='utf-8')
