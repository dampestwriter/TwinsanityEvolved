#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_material.h"

#include <cstring>

namespace title {
namespace {
template<class T> T Field(const uint8_t* bytes, size_t offset) {
    T value; std::memcpy(&value, bytes + offset, sizeof(T)); return value;
}
bool Extent(uint32_t address, size_t size) {
    const auto segment = address & 0xe0000000, at = address & 0x1fffffff;
    return (segment == 0 || segment == 0x80000000 || segment == 0xa0000000) &&
        at && !(address & 3) && at < 0x2000000 && size <= 0x2000000 - at;
}
bool Attributes(uint64_t settings) {
    // The proved procedural family is first-context Gouraud without fog.
    return (settings & (uint64_t{1} << 26)) && !(settings & (uint64_t{3} << 29));
}
}
bool DistortionMaterial::Supported(const Settings& value) { return Attributes(value.attributes); }
DistortionMaterial::Result DistortionMaterial::CaptureSettings(
    const MaterialState::Read& read, uint32_t shader, Settings& out) {
    std::array<uint8_t, 0x70> object{};
    std::array<uint32_t, 2> slot{};
    try {
        if (!read || !Extent(shader, object.size()) || !read(shader, object.data(), object.size()) ||
            !read(kVtable + 0x28, slot.data(), sizeof(slot))) return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    Settings value{Field<uint64_t>(object.data(), 0), Field<int16_t>(object.data(), 0x18),
                    Field<int16_t>(object.data(), 0x1a)};
    if (Field<uint32_t>(object.data(), 0x64) != 0x18 || Field<uint32_t>(object.data(), 0x6c) != kVtable ||
        slot[0] || slot[1] != kSettings || !Attributes(value.attributes)) return Result::UnsupportedSettings;
    out = value;
    return Result::Ok;
}
DistortionMaterial::Result DistortionMaterial::Build(const Settings& value, const MaterialState::Frame& frame,
    const std::array<uint32_t, 8>& target, uint32_t source_frame, ps2r::gs::DrawIr& out) {
    if (!Attributes(value.attributes)) return Result::UnsupportedSettings;
    if (frame.display_width < 64 || frame.display_width > 2048 || !frame.display_height ||
        frame.display_height > 2048 || frame.display_pixel_bytes != 2 || frame.depth_buffer_page >= 512 ||
        target[1] > 2047 || target[2] > 2047 || !target[3] || !target[4] ||
        uint64_t(target[1]) + target[3] > 2048 || uint64_t(target[2]) + target[4] > 2048)
        return Result::UnsupportedTarget;
    const auto bit = [&](unsigned n) { return bool(value.attributes & (uint64_t{1} << n)); };
    const auto bits = [&](unsigned n, unsigned mask) { return uint8_t((value.attributes >> n) & mask); };
    ps2r::gs::DrawIr draw;
    draw.src_frame = source_frame;
    draw.prim_type = 4; draw.gouraud = draw.tex_enabled = true; draw.alpha_blend = bit(0);
    draw.off_x = float(2048 - (frame.display_width >> 1));
    draw.off_y = float(2048 - (frame.display_height >> 1));
    const uint64_t pixels = uint64_t(frame.display_width) * frame.display_height;
    draw.raster.frame = {uint32_t(pixels * frame.display_pixel_bytes / 8192) * 8192,
                        (frame.display_width >> 6) * 64, 0, 0};
    draw.raster.scissor = {uint16_t(target[1]), uint16_t(target[2]),
        uint16_t(target[1] + target[3] - 1), uint16_t(target[2] + target[4] - 1)};
    draw.raster.zbuf = {frame.depth_buffer_page * 8192, 0, bit(59)};
    draw.raster.fba = !bit(56);
    draw.raster.test = {bit(5), bits(6, 7), bits(9, 255), bits(17, 3), bit(19), bit(20), true, bits(21, 3)};
    if (bit(38)) {
        draw.raster.alpha = {bits(39, 3), bits(41, 3), bits(43, 3), bits(45, 3), 128};
        if (draw.raster.alpha.a == 3 || draw.raster.alpha.b == 3 ||
            draw.raster.alpha.c == 3 || draw.raster.alpha.d == 3) return Result::UnsupportedSettings;
    } else {
        constexpr std::array<std::array<uint8_t, 4>, 16> presets{{
            {0, 1, 0, 1}, {0, 2, 0, 1}, {2, 0, 0, 1},
            {1, 2, 0, 1}, {2, 1, 0, 1}, {1, 2, 0, 2}}};
        const auto& preset = presets[bits(1, 15)];
        draw.raster.alpha = {preset[0], preset[1], preset[2], preset[3], 0};
    }
    const auto& environment = frame.environment;
    draw.raster.colclamp = environment.colour_clamp; draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.dimx = environment.dither_matrix;
    draw.raster.fogcol_r = environment.fog_r; draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b; draw.scanmsk = environment.scan_mask;
    draw.raster.dthe = false; // The preceding screen-copy operation disables it.
    draw.tex.tbp = 0x280000; // Source annotation only; renderer supplies the owned copy lease.
    draw.tex.psm = 1; draw.tex.tbw = 256; draw.tex.tw = draw.tex.th = 8;
    draw.tex.wms = draw.tex.wmt = 1;
    draw.tex.texa_ta1 = 128;
    draw.tex.mmin = bit(55);
    draw.tex.mmag = draw.tex.mmin;
    // The original shifts signed L/K into the combined filter fields (BUG084).
    const uint64_t filters = uint64_t(int64_t(value.lod_k)) << 32 | uint64_t(int64_t(value.lod_l)) << 19;
    draw.tex.l = (filters >> 19) & 3;
    const int32_t k = int32_t((filters >> 32) & 4095);
    draw.tex.k = int16_t(k & 2048 ? k - 4096 : k);
    out = std::move(draw);
    return Result::Ok;
}
}  // namespace title
