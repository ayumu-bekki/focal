#include "imaging/output_transform.h"

#include <lcms2.h>

#include <algorithm>
#include <cmath>

#include "util/error.h"
#include "util/matrix.h"

namespace focal {

namespace {

cmsHPROFILE create_rgb_profile(const double prim[3][2], cmsToneCurve* curve, const wchar_t* description) {
    cmsCIExyY white{primaries::kD65[0], primaries::kD65[1], 1.0};
    cmsCIExyYTRIPLE p{{prim[0][0], prim[0][1], 1.0}, {prim[1][0], prim[1][1], 1.0}, {prim[2][0], prim[2][1], 1.0}};
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE h = cmsCreateRGBProfile(&white, &p, curves);
    if (h && description) {
        cmsMLU* mlu = cmsMLUalloc(nullptr, 1);
        cmsMLUsetWide(mlu, "en", "US", description);
        cmsWriteTag(h, cmsSigProfileDescriptionTag, mlu);
        cmsMLUfree(mlu);
    }
    return h;
}

cmsHPROFILE create_linear_rec2020() {
    cmsToneCurve* linear = cmsBuildGamma(nullptr, 1.0);
    cmsHPROFILE h = create_rgb_profile(primaries::kRec2020, linear, L"Linear Rec.2020");
    cmsFreeToneCurve(linear);
    return h;
}

cmsHPROFILE create_linear(const double prim[3][2]) {
    cmsToneCurve* linear = cmsBuildGamma(nullptr, 1.0);
    cmsHPROFILE h = create_rgb_profile(prim, linear, nullptr);
    cmsFreeToneCurve(linear);
    return h;
}

cmsHPROFILE create_display_p3() {
    // sRGB と同じ区分関数の TRC（IEC 61966-2-1）
    const cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    cmsToneCurve* trc = cmsBuildParametricToneCurve(nullptr, 4, params);
    cmsHPROFILE h = create_rgb_profile(primaries::kDisplayP3, trc, L"Display P3");
    cmsFreeToneCurve(trc);
    return h;
}

} // namespace

OutputTransform::OutputTransform(OutputSpace space, OutputDepth depth) : space_(space), depth_(depth) {
    cmsHPROFILE in = create_linear_rec2020();
    cmsHPROFILE out = (space == OutputSpace::Srgb) ? cmsCreate_sRGBProfile() : create_display_p3();
    if (!in || !out) throw Error(Error::Code::Internal, "failed to create ICC profiles");

    const cmsUInt32Number out_fmt = (depth == OutputDepth::U8) ? TYPE_RGB_8 : TYPE_RGB_16;
    transform_ = cmsCreateTransform(in, TYPE_RGB_FLT, out, out_fmt, INTENT_RELATIVE_COLORIMETRIC,
                                    cmsFLAGS_NOCACHE);
    out_profile_ = out;
    if (!transform_) {
        cmsCloseProfile(in);
        throw Error(Error::Code::Internal, "failed to create color transform");
    }

    if (depth == OutputDepth::U8) {
        // 行列: 作業色空間 → 出力原色（リニア）を lcms2 に計算させ、単位ベクトルの像から取り出す
        cmsHPROFILE lin = create_linear(space == OutputSpace::Srgb ? primaries::kSrgb : primaries::kDisplayP3);
        cmsHTRANSFORM m = cmsCreateTransform(in, TYPE_RGB_DBL, lin, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                             cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
        const double unit[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        double cols[9];
        cmsDoTransform(m, unit, cols, 3);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) matrix_[r * 3 + c] = static_cast<float>(cols[c * 3 + r]);
        cmsDeleteTransform(m);
        cmsCloseProfile(lin);

        // TRC: 出力プロファイルのトーンカーブ（符号値 → リニア）の逆関数を lcms2 で評価して表にする
        const auto* trc = static_cast<const cmsToneCurve*>(cmsReadTag(out, cmsSigRedTRCTag));
        cmsToneCurve* encode = trc ? cmsReverseToneCurve(trc) : nullptr;
        if (!encode) {
            cmsCloseProfile(in);
            throw Error(Error::Code::Internal, "output profile has no invertible TRC");
        }
        trc_u8_.resize(65536);
        for (int i = 0; i < 65536; ++i) {
            const float v = cmsEvalToneCurveFloat(encode, static_cast<float>(i / 65535.0));
            trc_u8_[i] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
        cmsFreeToneCurve(encode);
    }
    cmsCloseProfile(in);
}

OutputTransform::~OutputTransform() {
    if (transform_) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(transform_));
    if (out_profile_) cmsCloseProfile(static_cast<cmsHPROFILE>(out_profile_));
}

void OutputTransform::apply(const float* rgb, void* out, size_t pixels) const {
    if (depth_ != OutputDepth::U8) {
        apply_lcms(rgb, out, pixels);
        return;
    }
    const float* m = matrix_;
    const uint8_t* lut = trc_u8_.data();
    auto* o = static_cast<uint8_t*>(out);
    auto index = [](float v) {
        // NaN も 0 に落とす
        v = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
        return static_cast<int>(v * 65535.0f + 0.5f);
    };
    for (size_t i = 0; i < pixels; ++i, rgb += 3, o += 3) {
        const float r = rgb[0], g = rgb[1], b = rgb[2];
        o[0] = lut[index(m[0] * r + m[1] * g + m[2] * b)];
        o[1] = lut[index(m[3] * r + m[4] * g + m[5] * b)];
        o[2] = lut[index(m[6] * r + m[7] * g + m[8] * b)];
    }
}

void OutputTransform::apply_bgrx(const float* rgb, uint8_t* o, size_t pixels) const {
    if (depth_ != OutputDepth::U8) throw Error(Error::Code::InvalidArgument, "apply_bgrx needs U8");
    const float* m = matrix_;
    const uint8_t* lut = trc_u8_.data();
    auto index = [](float v) {
        v = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
        return static_cast<int>(v * 65535.0f + 0.5f);
    };
    for (size_t i = 0; i < pixels; ++i, rgb += 3, o += 4) {
        const float r = rgb[0], g = rgb[1], b = rgb[2];
        o[2] = lut[index(m[0] * r + m[1] * g + m[2] * b)];
        o[1] = lut[index(m[3] * r + m[4] * g + m[5] * b)];
        o[0] = lut[index(m[6] * r + m[7] * g + m[8] * b)];
        o[3] = 255;
    }
}

void OutputTransform::apply_lcms(const float* rgb, void* out, size_t pixels) const {
    cmsDoTransform(static_cast<cmsHTRANSFORM>(transform_), rgb, out, static_cast<cmsUInt32Number>(pixels));
}

std::vector<uint8_t> OutputTransform::icc_profile() const {
    cmsUInt32Number size = 0;
    auto* h = static_cast<cmsHPROFILE>(out_profile_);
    if (!cmsSaveProfileToMem(h, nullptr, &size)) return {};
    std::vector<uint8_t> bytes(size);
    cmsSaveProfileToMem(h, bytes.data(), &size);
    return bytes;
}

} // namespace focal
