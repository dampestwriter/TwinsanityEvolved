#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_material.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace title {
namespace {
template<class T> T Field(const uint8_t* bytes, size_t at) {
    T result;
    std::memcpy(&result, bytes + at, sizeof(T));
    return result;
}
bool Extent(uint32_t address, size_t size) {
    const uint32_t segment = address & 0xe0000000, at = address & 0x1fffffff;
    return (segment == 0 || segment == 0x80000000 || segment == 0xa0000000) &&
        at && !(address & 3) && at < 0x2000000 && size <= 0x2000000 - at;
}
template<class T> bool Read(const MaterialState::Read& read, uint32_t at, T& out) {
    return read && Extent(at, sizeof(T)) && read(at, &out, sizeof(T));
}
bool Texture(const MeshMaterial::Texture& image) {
    return image.lease && !image.lease->description.renderable &&
        image.lease->description.format == ps2r::render::SurfaceFormat::Rgba8 &&
        image.lease->description.scale == 1 &&
        image.lease->description.width == image.width && image.lease->description.height == image.height &&
        image.width && image.width <= 1024 && !(image.width & (image.width - 1)) &&
        image.height && image.height <= 1024 && !(image.height & (image.height - 1)) &&
        image.levels && image.levels <= 7 && (image.source_format == 0 || image.source_format == 19) &&
        image.function <= 3 && image.buffer_width && image.buffer_width <= 4032 &&
        image.buffer_width % 64 == 0;
}
bool Template(const uint8_t* shader, const MaterialState::Frame& frame,
              const std::array<uint32_t, 8>& rectangle, MeshMaterial::Output& out) {
    const uint64_t settings = Field<uint64_t>(shader, 0);
    const auto bit = [&](unsigned n) { return bool(settings & (uint64_t{1} << n)); };
    const auto bits = [&](unsigned n, unsigned mask) { return uint8_t((settings >> n) & mask); };
    if (!bit(26) || !bit(27) || !bit(28) || bit(29) || bit(30)) return false;
    out.scroll = Field<std::array<float, 2>>(shader, 0x50);
    for (float value : out.scroll) if (!std::isfinite(value)) return false;
    auto& draw = out.state;
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
        if (draw.raster.alpha.a == 3 || draw.raster.alpha.b == 3 ||
            draw.raster.alpha.c == 3 || draw.raster.alpha.d == 3) return false;
    } else {
        // Source InitAlphaPresets populates six entries; the other ten are zero.
        constexpr std::array<std::array<uint8_t, 4>, 16> presets{{
            {0, 1, 0, 1}, {0, 2, 0, 1}, {2, 0, 0, 1},
            {1, 2, 0, 1}, {2, 1, 0, 1}, {1, 2, 0, 2}}};
        const auto& p = presets[bits(1, 15)];
        draw.raster.alpha = {p[0], p[1], p[2], p[3], 0};
    }
    const auto& env = frame.environment;
    draw.raster.colclamp = env.colour_clamp; draw.raster.pabe = env.per_pixel_alpha;
    draw.raster.dimx = env.dither_matrix; draw.raster.dthe = env.dither;
    draw.raster.fogcol_r = env.fog_r; draw.raster.fogcol_g = env.fog_g; draw.raster.fogcol_b = env.fog_b;
    draw.scanmsk = env.scan_mask;
    const auto& image = out.texture;
    auto& texture = draw.tex;
    texture.psm = image.source_format;
    texture.tbw = image.buffer_width;
    for (uint32_t n = image.width; n > 1; n >>= 1) ++texture.tw;
    for (uint32_t n = image.height; n > 1; n >>= 1) ++texture.th;
    texture.tcc = image.alpha; texture.tfx = image.function; texture.cld = 1;
    texture.mmag = bit(55); texture.mmin = image.levels > 1 ? 4 : bit(55);
    texture.mxl = image.levels - 1;
    // OLD/local BUG-084: signed L spills into K before its 12-bit extraction.
    const uint64_t filters = uint64_t(int64_t(Field<int16_t>(shader, 0x18))) << 32 |
                             uint64_t(int64_t(Field<int16_t>(shader, 0x1a))) << 19;
    texture.l = (filters >> 19) & 3;
    const int32_t k = int32_t((filters >> 32) & 4095);
    texture.k = int16_t(k & 2048 ? k - 4096 : k);
    return true;
}
}
ClothMaterial::Result ClothMaterial::Capture(const MaterialState::Read& read, uint32_t material,
    const MaterialState::Environment& environment, const MeshMaterial::Lookup& lookup, Output& out) {
    Output result;
    try {
        std::array<uint8_t, 0x24> object{};
        std::array<uint8_t, 0x70> shader{};
        uint32_t call = 0, target = 0;
        if (!Extent(material, 0x64) || !Read(read, material, object) ||
            !Read(read, material + 0x60, call)) return Result::InvalidMemory;
        result.material.bucket = Field<uint32_t>(object.data(), 0x20);
        if (Field<uint32_t>(object.data(), 0x10) != 1 || call != 1 ||
            result.material.bucket >= Graphics::kBucketCount) return Result::UnsupportedMaterial;
        const uint32_t address = Field<uint32_t>(object.data(), 0);
        if (!Extent(address, 0x280) || !Read(read, address, shader)) return Result::InvalidMemory;
        const uint32_t kind = Field<uint32_t>(shader.data(), 0x64);
        const bool three = kind == 0x1a;
        if ((kind != 0x17 && !three) ||
            Field<uint64_t>(object.data(), 0x18) != (three ? 0x80000u : 0x10000u) ||
            Field<uint32_t>(shader.data(), 0x6c) != (three ? kThreeVtable : kSingleVtable))
            return Result::UnsupportedMaterial;
        std::array<uint32_t, 2> settings_slot{};
        if (!Read(read, (three ? kThreeVtable : kSingleVtable) + 0x28, settings_slot))
            return Result::InvalidMemory;
        if (settings_slot[0] || settings_slot[1] != (three ? 0x1d0d20u : 0x1d18c8u))
            return Result::UnsupportedMaterial;
        MaterialState::Frame frame;
        std::array<uint32_t, 8> rectangle{};
        const auto frame_result = MaterialState::ReadFrame(read, environment, frame);
        if (frame_result != MaterialState::Result::Ok)
            return frame_result == MaterialState::Result::InvalidMemory ? Result::InvalidMemory : Result::UnsupportedTarget;
        if (!Read(read, MaterialState::kRenderTarget, target) || !Read(read, target, rectangle))
            return Result::InvalidMemory;
        if (rectangle[1] > 2047 || rectangle[2] > 2047 || !rectangle[3] || !rectangle[4] ||
            uint64_t(rectangle[1]) + rectangle[3] > 2048 ||
            uint64_t(rectangle[2]) + rectangle[4] > 2048) return Result::UnsupportedTarget;
        if (!lookup || !lookup(Field<uint32_t>(shader.data(), 0x14), result.material.texture) ||
            !Texture(result.material.texture)) return Result::MissingTexture;
        if (!Template(shader.data(), frame, rectangle, result.material)) return Result::UnsupportedMaterial;
        uint32_t waves = 0;
        std::array<std::array<float, 4>, 16> rows{};
        result.waves.kind = ClothGeometry::Kind(kind);
        if (!Read(read, address + 0x270, waves) || (waves & 15) || !Read(read, waves, rows) ||
            !Read(read, address + 0x27c, result.waves.amplitude[0])) return Result::InvalidMemory;
        if (three && !Read(read, address + 0x27c, result.waves.amplitude)) return Result::InvalidMemory;
        for (float value : result.waves.amplitude)
            if (!std::isfinite(value)) return Result::UnsupportedMaterial;
        for (size_t i = 0; i < rows.size(); ++i)
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(rows[i][axis])) return Result::UnsupportedMaterial;
                result.waves.rows[i][axis] = rows[i][axis];
            }
    } catch (...) { return Result::InvalidMemory; }
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
