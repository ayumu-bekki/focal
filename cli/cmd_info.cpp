#include <cstdio>
#include <ctime>

#include "args.h"
#include "commands.h"
#include "imaging/raw_decoder.h"
#include "imaging/white_balance.h"

namespace focal::cli {

int cmd_info(int argc, char** argv) {
    Args args(argc, argv, {"full"});
    if (args.positional().size() != 1) {
        std::fprintf(stderr, "usage: focal info <in.RAW>\n");
        return 2;
    }
    const auto& path = args.positional()[0];
    const RawMetadata m = read_raw_metadata(path);

    char ts[32] = "-";
    if (m.timestamp) {
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &m.timestamp);
#else
        localtime_r(&m.timestamp, &tm);
#endif
        std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    }
    std::printf("camera     : %s %s (%s)\n", m.make.c_str(), m.model.c_str(), m.normalized_make.c_str());
    std::printf("lens       : %s\n", m.lens.empty() ? "-" : m.lens.c_str());
    std::printf("exposure   : ISO %.0f, %.6gs, f/%.1f, %.1fmm\n", m.iso, m.shutter, m.aperture, m.focal_length);
    std::printf("captured   : %s\n", ts);
    std::printf("size       : %d x %d (flip %d)\n", m.width, m.height, m.flip);

    // 色情報は half_size デコードで取る
    const DecodedRaw raw = decode_raw(path, {.half_size = true});
    const auto& c = raw.color;
    double k = 0, tint = 0;
    temp_tint_from_wb(c.as_shot_wb, c, k, tint);
    std::printf("as shot wb : %.4f %.4f %.4f  (%.0fK, tint %+.1f)\n", c.as_shot_wb[0], c.as_shot_wb[1],
                c.as_shot_wb[2], k, tint);
    temp_tint_from_wb(c.daylight_wb, c, k, tint);
    std::printf("daylight wb: %.4f %.4f %.4f  (%.0fK, tint %+.1f)\n", c.daylight_wb[0], c.daylight_wb[1],
                c.daylight_wb[2], k, tint);
    std::printf("rgb_cam    :");
    for (int r = 0; r < 3; ++r)
        std::printf(" [%.4f %.4f %.4f]", c.rgb_cam.m[r][0], c.rgb_cam.m[r][1], c.rgb_cam.m[r][2]);
    std::printf("\nhalf image : %d x %d\n", raw.image.width, raw.image.height);
    return 0;
}

} // namespace focal::cli
