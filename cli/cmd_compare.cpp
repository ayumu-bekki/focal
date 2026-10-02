#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "args.h"
#include "commands.h"
#include "imaging/image_io.h"

namespace focal::cli {

// 2 枚の TIFF を 8-bit 換算で比較する。最大差が --max-diff 以下なら 0 を返す（12 章のゴールデン比較）。
int cmd_compare(int argc, char** argv) {
    Args args(argc, argv);
    if (args.positional().size() != 2) {
        std::fprintf(stderr, "usage: focal compare <a.tif> <b.tif> [--max-diff 2]\n");
        return 2;
    }
    const double max_allowed = args.get_double("max-diff", 2.0);
    const LoadedImage a = read_tiff(args.positional()[0]);
    const LoadedImage b = read_tiff(args.positional()[1]);
    if (a.width != b.width || a.height != b.height) {
        std::fprintf(stderr, "size mismatch: %dx%d vs %dx%d\n", a.width, a.height, b.width, b.height);
        return 1;
    }
    auto to8 = [](const LoadedImage& img, size_t i) {
        return img.bits == 8 ? static_cast<double>(img.data[i]) : img.data[i] / 257.0;
    };
    double max_diff = 0, sum = 0;
    size_t over = 0;
    for (size_t i = 0; i < a.data.size(); ++i) {
        const double d = std::abs(to8(a, i) - to8(b, i));
        max_diff = std::max(max_diff, d);
        sum += d;
        if (d > max_allowed) ++over;
    }
    std::printf("max diff %.2f / mean diff %.4f / over %zu samples (limit %.2f)\n", max_diff,
                sum / a.data.size(), over, max_allowed);
    return max_diff <= max_allowed ? 0 : 1;
}

// 画像を cols × rows のマスに分け、各マスの平均（8-bit、ファイルの色空間のまま）を "r,g,b;r,g,b;…" で出す。
// アプリの表示のスクリーンショットと書き出した sRGB の画像を比べる UI テスト（8.2 章）の基準値に使う
int cmd_colorgrid(int argc, char** argv) {
    Args args(argc, argv);
    if (args.positional().size() != 1) {
        std::fprintf(stderr, "usage: focal colorgrid <image.jpg|tif> [--cols 24] [--rows 16] [--inset 0.01]\n");
        return 2;
    }
    const std::string path = args.positional()[0];
    const bool tif = path.size() > 4 && (path.ends_with(".tif") || path.ends_with(".tiff"));
    const LoadedImage img = tif ? read_tiff(path) : read_jpeg(path);
    const int cols = args.get_int("cols", 24), rows = args.get_int("rows", 16);
    const double scale = img.bits == 16 ? 1.0 / 257.0 : 1.0;
    // 端を除く割合（比べる相手の端の画素がずれやすいため）
    const double inset = args.get_double("inset", 0.0);
    const int ix0 = static_cast<int>(img.width * inset), iy0 = static_cast<int>(img.height * inset);
    const int iw = img.width - 2 * ix0, ih = img.height - 2 * iy0;
    std::string out;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int x0 = ix0 + c * iw / cols, x1 = ix0 + (c + 1) * iw / cols;
            const int y0 = iy0 + r * ih / rows, y1 = iy0 + (r + 1) * ih / rows;
            double sum[3] = {0, 0, 0};
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    for (int k = 0; k < 3; ++k) sum[k] += img.data[(static_cast<size_t>(y) * img.width + x) * 3 + k];
            const double n = static_cast<double>(x1 - x0) * (y1 - y0);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s%.1f,%.1f,%.1f", out.empty() ? "" : ";", sum[0] / n * scale,
                          sum[1] / n * scale, sum[2] / n * scale);
            out += buf;
        }
    std::printf("%s\n", out.c_str());
    return 0;
}

} // namespace focal::cli
