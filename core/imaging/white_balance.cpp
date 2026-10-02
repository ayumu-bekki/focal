#include "imaging/white_balance.h"

#include <algorithm>
#include <cmath>

namespace focal {

namespace {

struct RobertsonEntry {
    double r, u, v, t;  // 逆色温度（mired）、u、v、等色温度線の傾き
};

constexpr RobertsonEntry kTable[] = {
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},  {20, 0.18133, 0.26846, -0.26876},
    {30, 0.18208, 0.27119, -0.28539},  {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},
    {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},  {80, 0.18740, 0.28668, -0.40955},
    {90, 0.18880, 0.28997, -0.44278},  {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901}, {200, 0.21142, 0.32312, -1.0182},
    {225, 0.21807, 0.32909, -1.2168},  {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},
    {300, 0.24010, 0.34308, -2.0637},  {325, 0.24792, 0.34655, -2.4681},  {350, 0.25591, 0.34951, -2.9641},
    {375, 0.26400, 0.35200, -3.5814},  {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},  {500, 0.30505, 0.35907, -11.324},
    {525, 0.31320, 0.35968, -15.628},  {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},
    {600, 0.33724, 0.36051, -116.45},
};
constexpr int kTableSize = sizeof(kTable) / sizeof(kTable[0]);
constexpr double kTintScale = -3000.0;

} // namespace

ChromaXy temp_tint_to_xy(double temperature, double tint) {
    const double r = 1.0e6 / temperature;
    const double offset = tint / kTintScale;
    for (int i = 0; i < kTableSize - 1; ++i) {
        if (r < kTable[i + 1].r || i == kTableSize - 2) {
            const double f = (kTable[i + 1].r - r) / (kTable[i + 1].r - kTable[i].r);
            double u = kTable[i].u * f + kTable[i + 1].u * (1.0 - f);
            double v = kTable[i].v * f + kTable[i + 1].v * (1.0 - f);

            double uu1 = 1.0, vv1 = kTable[i].t;
            double uu2 = 1.0, vv2 = kTable[i + 1].t;
            const double len1 = std::sqrt(1.0 + vv1 * vv1);
            const double len2 = std::sqrt(1.0 + vv2 * vv2);
            uu1 /= len1, vv1 /= len1, uu2 /= len2, vv2 /= len2;
            double uu3 = uu1 * f + uu2 * (1.0 - f);
            double vv3 = vv1 * f + vv2 * (1.0 - f);
            const double len3 = std::sqrt(uu3 * uu3 + vv3 * vv3);
            uu3 /= len3, vv3 /= len3;

            u += uu3 * offset;
            v += vv3 * offset;
            const double d = u - 4.0 * v + 2.0;
            return {1.5 * u / d, v / d};
        }
    }
    return {0.3127, 0.3290};
}

void xy_to_temp_tint(ChromaXy xy, double& temperature, double& tint) {
    const double d = 1.5 - xy.x + 6.0 * xy.y;
    const double u = 2.0 * xy.x / d;
    const double v = 3.0 * xy.y / d;

    double last_dt = 0, last_du = 0, last_dv = 0;
    for (int i = 1; i < kTableSize; ++i) {
        double du = 1.0, dv = kTable[i].t;
        const double len = std::sqrt(1.0 + dv * dv);
        du /= len, dv /= len;
        double uu = u - kTable[i].u, vv = v - kTable[i].v;
        double dt = -uu * dv + vv * du;
        if (dt <= 0 || i == kTableSize - 1) {
            if (dt > 0) dt = 0;
            dt = -dt;
            const double f = (i == 1) ? 0.0 : dt / (last_dt + dt);
            temperature = 1.0e6 / (kTable[i - 1].r * f + kTable[i].r * (1.0 - f));
            uu = u - (kTable[i - 1].u * f + kTable[i].u * (1.0 - f));
            vv = v - (kTable[i - 1].v * f + kTable[i].v * (1.0 - f));
            du = du * (1.0 - f) + last_du * f;
            dv = dv * (1.0 - f) + last_dv * f;
            const double len2 = std::sqrt(du * du + dv * dv);
            du /= len2, dv /= len2;
            tint = (uu * du + vv * dv) * kTintScale;
            return;
        }
        last_dt = dt, last_du = du, last_dv = dv;
    }
}

std::array<double, 3> wb_from_temp_tint(double temperature, double tint, const ColorInfo& color) {
    const ChromaXy xy = temp_tint_to_xy(temperature, tint);
    const Vec3 xyz{{xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y}};
    const Vec3 cam = color.cam_xyz * xyz;  // この光源下の白に対するカメラの応答
    // 係数は応答の逆数を G = 1 で正規化したもの
    return {cam[1] / cam[0], 1.0, cam[1] / cam[2]};
}

void temp_tint_from_wb(const std::array<double, 3>& wb, const ColorInfo& color, double& temperature,
                       double& tint) {
    // 白に対するカメラの応答は係数の逆数
    const Vec3 neutral{{1.0 / wb[0], 1.0 / wb[1], 1.0 / wb[2]}};
    const Vec3 xyz = color.cam_xyz.inverse() * neutral;
    const double sum = xyz[0] + xyz[1] + xyz[2];
    xy_to_temp_tint({xyz[0] / sum, xyz[1] / sum}, temperature, tint);
    temperature = std::clamp(temperature, 1000.0, 100000.0);
}

} // namespace focal
