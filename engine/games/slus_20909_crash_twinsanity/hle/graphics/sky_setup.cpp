#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_setup.h"

#include <array>
#include <cmath>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_intent.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
template<class T> bool Read(const MaterialState::Read& read, uint32_t address, T& value) {
    return address && uint64_t(address) + sizeof(T) <= uint64_t{1} << 32 &&
           read(address, &value, sizeof(T));
}
}  // namespace

SkySetup::Result SkySetup::Capture(const MaterialState::Read& read,
    const MaterialState::Environment& environment, Output& out) {
    if (!read) return Result::InvalidMemory;
    Output result;
    uint32_t target = 0, view = 0;
    std::array<uint32_t, 8> description{};
    SkyGeometry::Matrix clip{};
    try {
        if (!Read(read, MaterialState::kDisplayWidth, result.width) ||
            !Read(read, MaterialState::kDisplayHeight, result.height) ||
            !Read(read, MaterialState::kRenderTarget, target) ||
            !Read(read, kRenderView, view) || view > 0xffffff7f ||
            !Read(read, target, description) || !Read(read, view + 0x80, clip))
            return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    SkyIntent::Output finish;
    if (SkyIntent::Build(result.width, result.height, environment, finish) != SkyIntent::Result::Ok ||
        description[1] > 2047 || description[2] > 2047 ||
        description[3] < 2 || description[3] > 2048 ||
        description[4] < 2 || description[4] > 2048 ||
        !description[6] || description[6] > 4096 || !description[7] || description[7] > 4096)
        return Result::UnsupportedFrame;
    for (const auto& row : clip)
        for (float value : row) if (!std::isfinite(value)) return Result::InvalidMatrix;
    RoundingGuard rounding;
    auto projection = MathLibrary::EvaluateIdentity();
    const float half_width = float(description[3] >> 1), half_height = float(description[4] >> 1);
    const float x = (2048.0f - float(description[6] >> 2)) + float(description[1]);
    const float y = (2048.0f - float(description[7] >> 2)) + float(description[2]);
    projection[0][0] = half_width * 8;
    projection[1][1] = -half_height * 8;
    projection[2][2] = -8388607.0f;
    projection[3][0] = (x + half_width * 0.5f) * 16;
    projection[3][1] = (y + half_height * 0.5f) * 16;
    projection[3][2] = 8388607.0f;
    result.to_screen = MathLibrary::EvaluateMatrixProduct(clip, projection).output;
    const uint32_t pixels = result.width * result.height;
    auto& draw = result.state;
    draw.off_x = float(2048 - (result.width >> 2));
    draw.off_y = float(2048 - (result.height >> 2));
    draw.raster.frame = {(pixels * 10 >> 13) * 8192, (result.width >> 7) * 64, 0, 0};
    draw.raster.zbuf = {(pixels >> 12) * 8192, 0, true};
    draw.raster.scissor = {0, 0, uint16_t((result.width >> 1) - 1),
                                   uint16_t((result.height >> 1) - 1)};
    draw.raster.colclamp = environment.colour_clamp;
    draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.dimx = environment.dither_matrix;
    draw.raster.fogcol_r = environment.fog_r;
    draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b;
    draw.scanmsk = environment.scan_mask;
    out = std::move(result);
    return Result::Ok;
}

}  // namespace title
