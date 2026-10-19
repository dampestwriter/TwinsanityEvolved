#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_material.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace title {
namespace {
template<class T> T Field(const uint8_t* bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}
bool Read(const MaterialState::Read& read, uint32_t address, void* data, size_t size) {
    return read && address && uint64_t(address) + size <= uint64_t{1} << 32 && read(address, data, size);
}
}  // namespace

SkyMaterial::Result SkyMaterial::Capture(const MaterialState::Read& read, uint32_t material,
    const SkySetup::Output& setup, const SkyAssets& assets, Output& out) {
    std::array<uint8_t, 0x24> object{};
    std::array<uint8_t, 0x70> shader{};
    uint32_t depth_page = 0;
    try {
        if (!Read(read, material, object.data(), object.size())) return Result::InvalidMemory;
        if (Field<uint32_t>(object.data(), 0x10) != 1 || Field<uint32_t>(object.data(), 0x20) != 0 ||
            Field<uint64_t>(object.data(), 0x18) != 0x10)
            return Result::UnsupportedMaterial;
        if (!Read(read, Field<uint32_t>(object.data(), 0), shader.data(), shader.size()) ||
            !Read(read, MaterialState::kDepthBufferPage, &depth_page, sizeof(depth_page)))
            return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    const uint64_t settings = Field<uint64_t>(shader.data(), 0);
    const auto bit = [&](unsigned at) { return bool((settings >> at) & 1); };
    const auto bits = [&](unsigned at, uint64_t mask) { return uint8_t((settings >> at) & mask); };
    // Context-0, Gouraud, textured STQ, no fog, and ALWAYS depth are the
    // statically enumerated family. Other states need a separate source gate.
    if (Field<uint32_t>(shader.data(), 0x64) != 10 ||
        Field<uint32_t>(shader.data(), 0x6c) != kVtable || !bit(26) || !bit(27) || !bit(28) ||
        bit(29) || bit(30) || bits(21, 3) != 1 || depth_page > 511 ||
        (!bit(38) && bits(1, 15) > 5))
        return Result::UnsupportedMaterial;
    Output result;
    result.texture = assets.FindTexture(Field<uint32_t>(shader.data(), 0x14));
    if (!result.texture) return Result::MissingTexture;
    result.scroll = Field<std::array<float, 2>>(shader.data(), 0x50);
    if (!std::isfinite(result.scroll[0]) || !std::isfinite(result.scroll[1]))
        return Result::UnsupportedMaterial;
    result.state = setup.state;
    auto& draw = result.state;
    draw.prim_type = 4;
    draw.gouraud = draw.tex_enabled = true;
    draw.alpha_blend = bit(0);
    draw.raster.zbuf = {depth_page * 8192, 0, bit(59)};
    draw.raster.fba = !bit(56);
    draw.raster.test = {bit(5), bits(6, 7), bits(9, 255), bits(17, 3),
                        bit(19), bit(20), true, 1};
    if (bit(38)) {
        draw.raster.alpha = {bits(39, 3), bits(41, 3), bits(43, 3), bits(45, 3), bits(47, 255)};
    } else {
        constexpr std::array<std::array<uint8_t, 4>, 6> presets{{
            {0, 1, 0, 1}, {0, 2, 0, 1}, {2, 0, 0, 1},
            {1, 2, 0, 1}, {2, 1, 0, 1}, {1, 2, 0, 2}}};
        const auto& preset = presets[bits(1, 15)];
        draw.raster.alpha = {preset[0], preset[1], preset[2], preset[3], 0};
    }
    const auto& image = *result.texture;
    auto& texture = draw.tex;
    texture.psm = 19;
    texture.tbw = image.buffer_width;
    for (uint32_t value = image.width; value > 1; value >>= 1) ++texture.tw;
    for (uint32_t value = image.height; value > 1; value >>= 1) ++texture.th;
    texture.tcc = image.alpha;
    texture.tfx = image.function;
    texture.cld = 1;
    texture.mmag = bit(55);
    texture.mmin = bit(55); // type A never minifies through authored mips
    texture.mxl = image.levels - 1;
    // Source TextureFiltering sign-extends L and K before shifting. Preserve
    // its field overlap and the signed 12-bit K; do not reinterpret as +LOD.
    const uint64_t filters = uint64_t(int64_t(Field<int16_t>(shader.data(), 0x18))) << 32 |
                             uint64_t(int64_t(Field<int16_t>(shader.data(), 0x1a))) << 19;
    texture.l = (filters >> 19) & 3;
    const int32_t k = int32_t((filters >> 32) & 4095);
    texture.k = int16_t(k & 2048 ? k - 4096 : k);
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
