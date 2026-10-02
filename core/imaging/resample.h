#pragma once

#include "util/image.h"

namespace focal {

// 長辺が max_long_edge 以下になる縮小後のサイズ。縮小不要なら元のサイズを返す。
void fit_long_edge(int w, int h, int max_long_edge, int& out_w, int& out_h);

// フル解像度（uint16）から、リニア値のまま面積平均で縮小したプロキシ（float, 0..1）を作る（5.2 章）。
ImageF make_proxy(const ImageU16& full, int max_long_edge);

// float 画像を Lanczos3 で縮小する（書き出し用、5.8 章）。
ImageF resize_lanczos3(const ImageF& src, int out_w, int out_h);

} // namespace focal
