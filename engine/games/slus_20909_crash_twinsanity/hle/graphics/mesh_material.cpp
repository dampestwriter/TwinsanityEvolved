#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_material.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace title {
namespace {
template<class T> T Field(const uint8_t* bytes, size_t offset) {
    T result;
    std::memcpy(&result, bytes + offset, sizeof(T));
    return result;
}
template<class T> bool Read(const MaterialState::Read& read, uint32_t at, T& out) {
    return read && at && uint64_t(at) + sizeof(T) <= uint64_t{1} << 32 &&
           read(at, &out, sizeof(T));
}
}  // namespace
MeshMaterial::Result MeshMaterial::Capture(const MaterialState::Read& read, uint32_t material,
    const MaterialState::Environment& environment, const Lookup& lookup, Output& out) {
    std::array<uint8_t, 0x24> object{};
    std::array<uint8_t, 0x70> shader{};
    MaterialState::Frame frame;
    uint32_t target = 0, call = 0;
    std::array<uint32_t, 8> rectangle{};
    Output result;
    try {
        if (material > 0xffffff9f || !Read(read, material, object) ||
            !Read(read, material + 0x60, call)) return Result::InvalidMemory;
        result.bucket = Field<uint32_t>(object.data(), 0x20);
        if (Field<uint32_t>(object.data(), 0x10) != 1 ||
            Field<uint64_t>(object.data(), 0x18) != 8 || call != 1 ||
            result.bucket >= Graphics::kBucketCount)
            return Result::UnsupportedMaterial;
        if (!Read(read, Field<uint32_t>(object.data(), 0), shader) ||
            MaterialState::ReadFrame(read, environment, frame) != MaterialState::Result::Ok ||
            !Read(read, MaterialState::kRenderTarget, target) || !Read(read, target, rectangle))
            return Result::InvalidMemory;
        if (!lookup || !lookup(Field<uint32_t>(shader.data(), 0x14), result.texture))
            return Result::MissingTexture;
    } catch (...) { return Result::InvalidMemory; }
    const uint64_t settings = Field<uint64_t>(shader.data(), 0);
    const auto bit = [&](unsigned n) { return bool(settings & (uint64_t{1} << n)); };
    const auto bits = [&](unsigned n, unsigned mask) { return uint8_t((settings >> n) & mask); };
    if (Field<uint32_t>(shader.data(), 0x64) != 1 ||
        Field<uint32_t>(shader.data(), 0x6c) != kVtable ||
        !bit(26) || !bit(27) || !bit(28) || bit(29) || bit(30) ||
        (bit(0) && !bit(38) && bits(1, 15) > 5))
        return Result::UnsupportedMaterial;
    if (rectangle[1] > 2047 || rectangle[2] > 2047 || !rectangle[3] || !rectangle[4] ||
        uint64_t(rectangle[1]) + rectangle[3] > 2048 ||
        uint64_t(rectangle[2]) + rectangle[4] > 2048)
        return Result::UnsupportedTarget;
    const auto& image = result.texture;
    if (!image.lease || image.lease->description.renderable ||
        image.lease->description.format != ps2r::render::SurfaceFormat::Rgba8 ||
        image.lease->description.scale != 1 ||
        image.lease->description.width != image.width || image.lease->description.height != image.height ||
        !image.width || image.width > 1024 || !image.height || image.height > 1024 ||
        (image.width & (image.width - 1)) || (image.height & (image.height - 1)) ||
        !image.levels || image.levels > 7 ||
        (image.source_format != 0 && image.source_format != 19) || image.function > 3 ||
        !image.buffer_width || image.buffer_width > 4032 || image.buffer_width % 64)
        return Result::MissingTexture;
    result.scroll = Field<std::array<float, 2>>(shader.data(), 0x50);
    for (float lane : result.scroll) if (!std::isfinite(lane)) return Result::UnsupportedMaterial;
    auto& draw = result.state;
    draw.prim_type = 4;
    draw.gouraud = draw.tex_enabled = true;
    draw.alpha_blend = bit(0);
    draw.off_x = float(2048 - (frame.display_width >> 1));
    draw.off_y = float(2048 - (frame.display_height >> 1));
    const uint64_t pixels = uint64_t(frame.display_width) * frame.display_height;
    draw.raster.frame = {uint32_t(pixels * frame.display_pixel_bytes / 8192) * 8192,
                         (frame.display_width >> 6) * 64, 0, 0};
    draw.raster.scissor = {uint16_t(rectangle[1]), uint16_t(rectangle[2]),
        uint16_t(rectangle[1] + rectangle[3] - 1), uint16_t(rectangle[2] + rectangle[4] - 1)};
    draw.raster.zbuf = {frame.depth_buffer_page * 8192, 0, bit(59)};
    draw.raster.fba = !bit(56);
    draw.raster.test = {bit(5), bits(6, 7), bits(9, 255), bits(17, 3), bit(19), bit(20), true, bits(21, 3)};
    if (bit(38)) {
        draw.raster.alpha = {bits(39, 3), bits(41, 3), bits(43, 3), bits(45, 3), bits(47, 255)};
    } else if (bits(1, 15) <= 5) {
        constexpr std::array<std::array<uint8_t, 4>, 6> presets{{
            {0, 1, 0, 1}, {0, 2, 0, 1}, {2, 0, 0, 1},
            {1, 2, 0, 1}, {2, 1, 0, 1}, {1, 2, 0, 2}}};
        const auto& blend = presets[bits(1, 15)];
        draw.raster.alpha = {blend[0], blend[1], blend[2], blend[3], 0};
    } // Disabled blend selectors outside the preset table are inactive.
    draw.raster.colclamp = environment.colour_clamp;
    draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.dimx = environment.dither_matrix;
    draw.raster.dthe = environment.dither;
    draw.raster.fogcol_r = environment.fog_r;
    draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b;
    draw.scanmsk = environment.scan_mask;
    auto& texture = draw.tex;
    texture.psm = image.source_format;
    texture.tbw = image.buffer_width;
    for (uint32_t value = image.width; value > 1; value >>= 1) ++texture.tw;
    for (uint32_t value = image.height; value > 1; value >>= 1) ++texture.th;
    texture.tcc = image.alpha;
    texture.tfx = image.function;
    texture.cld = 1;
    texture.mmag = bit(55);
    texture.mmin = image.levels > 1 ? 4 : bit(55);
    texture.mxl = image.levels - 1;
    // BUG-084: L and K are signed before shifting, including overlapping bits.
    const uint64_t filters = uint64_t(int64_t(Field<int16_t>(shader.data(), 0x18))) << 32 |
                             uint64_t(int64_t(Field<int16_t>(shader.data(), 0x1a))) << 19;
    texture.l = (filters >> 19) & 3;
    const int32_t k = int32_t((filters >> 32) & 4095);
    texture.k = int16_t(k & 2048 ? k - 4096 : k);
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
