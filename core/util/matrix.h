#pragma once

#include <array>
#include <cmath>

namespace focal {

struct Vec3 {
    double v[3] = {0, 0, 0};
    double& operator[](int i) { return v[i]; }
    double operator[](int i) const { return v[i]; }
};

struct Mat3 {
    double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

    static Mat3 identity() {
        Mat3 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
        return r;
    }

    static Mat3 diag(double a, double b, double c) {
        Mat3 r;
        r.m[0][0] = a;
        r.m[1][1] = b;
        r.m[2][2] = c;
        return r;
    }

    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }

    Vec3 operator*(const Vec3& v) const {
        Vec3 r;
        for (int i = 0; i < 3; ++i) r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
        return r;
    }

    double det() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }

    Mat3 inverse() const {
        const double d = det();
        Mat3 r;
        r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
        r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
        r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
        r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
        r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
        r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
        r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
        r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
        r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
        return r;
    }
};

// 原色の xy と白色点 xy から RGB→XYZ 行列を作る。
inline Mat3 rgb_to_xyz_matrix(const double prim[3][2], const double white[2]) {
    Mat3 p;
    for (int c = 0; c < 3; ++c) {
        const double x = prim[c][0], y = prim[c][1];
        p.m[0][c] = x / y;
        p.m[1][c] = 1.0;
        p.m[2][c] = (1.0 - x - y) / y;
    }
    const Vec3 w{{white[0] / white[1], 1.0, (1.0 - white[0] - white[1]) / white[1]}};
    const Vec3 s = p.inverse() * w;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) p.m[r][c] *= s[c];
    return p;
}

namespace primaries {
inline constexpr double kD65[2] = {0.3127, 0.3290};
inline constexpr double kSrgb[3][2] = {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}};
inline constexpr double kRec2020[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
inline constexpr double kDisplayP3[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
} // namespace primaries

} // namespace focal
