#include "imaging/crop_tool.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace focal {

namespace {

constexpr double kMinSizePx = 16;  // 枠の最小の大きさ（キャンバスの画素）

// 傾き補正後の座標 → 補正前（キャンバス内）の座標。geometry.cpp と同じ対応
PointD to_rotated(PointD p, double w, double h, double deg) {
    const double rad = deg * std::numbers::pi / 180.0;
    const double cs = std::cos(rad), sn = std::sin(rad);
    const double x = p.x - w / 2, y = p.y - h / 2;
    return {cs * x + sn * y + w / 2, -sn * x + cs * y + h / 2};
}

bool point_inside(PointD p, double w, double h, double deg) {
    const PointD q = to_rotated(p, w, h, deg);
    constexpr double eps = 1e-6;
    return q.x >= -eps && q.y >= -eps && q.x <= w + eps && q.y <= h + eps;
}

CropRect lerp(const CropRect& a, const CropRect& b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

bool inside_canvas(const CropRect& c) {
    constexpr double eps = 1e-9;
    return c.x >= -eps && c.y >= -eps && c.x + c.w <= 1 + eps && c.y + c.h <= 1 + eps && c.w > 0 && c.h > 0;
}

bool valid(const CropRect& c, double deg, double cw, double ch) {
    return inside_canvas(c) && crop_inside_image(c, deg, cw, ch);
}

// start（有効）から proposed に向かって、有効な範囲でいちばん遠くまで動かす
CropRect toward(const CropRect& start, const CropRect& proposed, double deg, double cw, double ch) {
    if (valid(proposed, deg, cw, ch)) return proposed;
    if (!valid(start, deg, cw, ch)) return start;
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2;
        (valid(lerp(start, proposed, mid), deg, cw, ch) ? lo : hi) = mid;
    }
    return lerp(start, proposed, lo);
}

} // namespace

bool crop_inside_image(const CropRect& c, double deg, double cw, double ch) {
    const double x0 = c.x * cw, y0 = c.y * ch, x1 = (c.x + c.w) * cw, y1 = (c.y + c.h) * ch;
    return point_inside({x0, y0}, cw, ch, deg) && point_inside({x1, y0}, cw, ch, deg) &&
           point_inside({x0, y1}, cw, ch, deg) && point_inside({x1, y1}, cw, ch, deg);
}

double crop_aspect(AspectMode mode, double cw, double ch) { return aspect_ratio(mode, cw, ch); }

CropRect max_crop(double aspect, double deg, double cw, double ch, PointD center) {
    if (aspect <= 0) aspect = cw / ch;
    // 縦横比 aspect の最大の枠（キャンバス画素）
    double wpx = std::min(cw, ch * aspect);
    double hpx = wpx / aspect;
    auto rect_at = [&](PointD c, double s) {
        CropRect r;
        r.w = wpx * s / cw;
        r.h = hpx * s / ch;
        r.x = std::clamp(c.x - r.w / 2, 0.0, 1.0 - r.w);
        r.y = std::clamp(c.y - r.h / 2, 0.0, 1.0 - r.h);
        return r;
    };
    auto best_scale = [&](PointD c) {
        double lo = 0, hi = 1;
        if (valid(rect_at(c, 1), deg, cw, ch)) return 1.0;
        for (int i = 0; i < 50; ++i) {
            const double mid = (lo + hi) / 2;
            (valid(rect_at(c, mid), deg, cw, ch) ? lo : hi) = mid;
        }
        return lo;
    };
    const double s_here = best_scale(center);
    const double s_mid = best_scale({0.5, 0.5});
    // 端に寄せた中心のせいで大きく縮むなら、中央に寄せる
    return s_here >= s_mid * 0.98 ? rect_at(center, s_here) : rect_at({0.5, 0.5}, s_mid);
}

CropRect drag_crop(const CropRect& start, CropHandle handle, double dx, double dy, double aspect, double deg,
                   double cw, double ch) {
    if (handle == CropHandle::Move) {
        CropRect moved = start;
        moved.x = std::clamp(start.x + dx, 0.0, 1.0 - start.w);
        moved.y = std::clamp(start.y + dy, 0.0, 1.0 - start.h);
        return toward(start, moved, deg, cw, ch);
    }

    double x0 = start.x, y0 = start.y, x1 = start.x + start.w, y1 = start.y + start.h;
    const bool left = handle == CropHandle::Left || handle == CropHandle::TopLeft || handle == CropHandle::BottomLeft;
    const bool right =
        handle == CropHandle::Right || handle == CropHandle::TopRight || handle == CropHandle::BottomRight;
    const bool top = handle == CropHandle::Top || handle == CropHandle::TopLeft || handle == CropHandle::TopRight;
    const bool bottom =
        handle == CropHandle::Bottom || handle == CropHandle::BottomLeft || handle == CropHandle::BottomRight;
    if (left) x0 += dx;
    if (right) x1 += dx;
    if (top) y0 += dy;
    if (bottom) y1 += dy;

    const double min_w = kMinSizePx / cw, min_h = kMinSizePx / ch;
    // 反対側を越えて裏返さない
    if (left) x0 = std::min(x0, x1 - min_w);
    if (right) x1 = std::max(x1, x0 + min_w);
    if (top) y0 = std::min(y0, y1 - min_h);
    if (bottom) y1 = std::max(y1, y0 + min_h);

    if (aspect > 0) {
        // 正規化した幅と高さの比: (w * cw) / (h * ch) = aspect
        double w = x1 - x0, h = y1 - y0;
        const bool corner = (left || right) && (top || bottom);
        if (corner) {
            // 動かした量の大きい方に合わせる
            if (w * cw / aspect >= h * ch) h = w * cw / aspect / ch; else w = h * ch * aspect / cw;
        } else if (left || right) {
            h = w * cw / aspect / ch;
        } else {
            w = h * ch * aspect / cw;
        }
        // 固定する側: 角なら反対の角、辺なら反対の辺の中央
        if (left) x0 = x1 - w; else if (right) x1 = x0 + w;
        else { const double cx = (start.x + start.x + start.w) / 2; x0 = cx - w / 2; x1 = cx + w / 2; }
        if (top) y0 = y1 - h; else if (bottom) y1 = y0 + h;
        else { const double cy = (start.y + start.y + start.h) / 2; y0 = cy - h / 2; y1 = cy + h / 2; }
    }
    return toward(start, CropRect{x0, y0, x1 - x0, y1 - y0}, deg, cw, ch);
}

CropRect rotate_crop(const CropRect& c, int steps) {
    CropRect r = c;
    const int k = ((steps % 4) + 4) % 4;
    for (int i = 0; i < k; ++i) {
        // 時計回り: 正規化座標 (x, y) → (1 - y, x)
        r = CropRect{1.0 - (r.y + r.h), r.x, r.h, r.w};
    }
    return r;
}

double straighten_from_line(PointD a, PointD b, double current) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    if (std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9) return current;
    double deg = std::atan2(dy, dx) * 180.0 / std::numbers::pi;  // y は下向き。正なら右下がり
    // 向きを揃えて -90..90 に、縦に近い線なら垂直を基準にする
    if (deg > 90) deg -= 180;
    if (deg < -90) deg += 180;
    if (deg > 45) deg -= 90;
    if (deg < -45) deg += 90;
    // 右下がりの線（画像が時計回りに傾いて見える）を水平にするには反時計回りに回す
    return std::clamp(current - deg, -45.0, 45.0);
}

} // namespace focal
