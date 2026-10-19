#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/primitive_material.h"

#include <array>
#include <cstring>
#include <utility>

namespace title {
namespace {
bool ReadBytes(const PrimitiveMaterial::Read& read, uint32_t address, void* out, size_t size) {
    const uint32_t physical = address & 0x1fffffff, segment = address & 0xe0000000;
    return read && physical && (segment == 0 || segment == 0x80000000 || segment == 0xa0000000) &&
        physical < 0x2000000 && size <= 0x2000000 - physical && read(address, out, size);
}
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}
PrimitiveMaterial::Result PrimitiveMaterial::Capture(
    const Read& read, const TextureBindings& textures, uint32_t material,
    const MaterialState::Environment& environment, Output& out, Kind kind) {
    std::array<uint8_t, 0x24> m{};
    std::array<uint8_t, 0x70> s{};
    if ((material & 3) || !ReadBytes(read, material, m.data(), m.size())) return Result::InvalidMemory;
    const uint32_t shader = Word(m.data()), bucket = Word(m.data() + 0x20);
    if (!Word(m.data() + 0x10) || Word(m.data() + 0x10) > 4 || bucket >= Graphics::kBucketCount)
        return Result::UnsupportedMaterial;
    if ((shader & 3) || !ReadBytes(read, shader, s.data(), s.size())) return Result::InvalidMemory;
    Output result;
    const auto texture = textures.CaptureMaterial(shader, result.sampling);
    if (texture == TextureBindings::Result::NotOwned) return Result::MissingTexture;
    if (texture != TextureBindings::Result::Ok ||
        result.sampling.source == TextureBindings::Source::SceneCopy) return Result::UnsupportedMaterial;
    const uint64_t settings = Word(s.data()) | uint64_t(Word(s.data() + 4)) << 32;
    const auto bit = [&](unsigned n) { return bool((settings >> n) & 1); };
    const auto bits = [&](unsigned n, unsigned mask) { return uint8_t((settings >> n) & mask); };
    // These source producers write colour/coordinates without a fog value.
    // Their named source targets establish context 0 only.
    const bool text = kind == Kind::Text;
    if (bit(29) || bit(30) || (!text && bit(27) && !result.sampling.stq) ||
        (result.sampling.type == 0 && bit(58)) || result.sampling.type == 3 ||
        result.sampling.type == 0x0a)
        return Result::UnsupportedMaterial;
    uint32_t call = 1;
    if (text) {
        std::array<uint8_t, 4> value{};
        if (!ReadBytes(read, material + 0x60, value.data(), value.size())) return Result::InvalidMemory;
        call = Word(value.data());
        if (Word(m.data() + 0x10) != 1 || Word(m.data() + 0x18) != 0x10000000 ||
            Word(m.data() + 0x1c) != 0 || result.sampling.type != 13 || !bit(27) ||
            result.sampling.stq || (call != 1 && call != 2)) return Result::UnsupportedMaterial;
    }
    MaterialState::Frame frame;
    if (MaterialState::ReadFrame(read, environment, frame) != MaterialState::Result::Ok)
        return Result::UnsupportedTarget;
    uint32_t x = 0, y = 0, width = frame.display_width, height = frame.display_height;
    uint32_t dw = width, dh = height;
    if (call == 1) {
        std::array<uint8_t, 4> pointer{};
        std::array<uint8_t, 0x20> target{};
        if (!ReadBytes(read, MaterialState::kRenderTarget, pointer.data(), pointer.size()) ||
            !ReadBytes(read, Word(pointer.data()), target.data(), target.size())) return Result::InvalidMemory;
        x = Word(target.data() + 4); y = Word(target.data() + 8);
        width = Word(target.data() + 0xc); height = Word(target.data() + 0x10);
        dw = Word(target.data() + 0x18); dh = Word(target.data() + 0x1c);
    }
    if (x > 2047 || y > 2047 || !width || !height || uint64_t(x) + width > 2048 ||
        uint64_t(y) + height > 2048 || !dw || dw > 4096 || !dh || dh > 4096)
        return Result::UnsupportedTarget;
    result.target.coordinates = {int32_t(dw), int32_t(dh), int32_t(x), int32_t(y)};
    if (!text) {
        std::array<uint8_t, 8> screen{};
        if (!ReadBytes(read, kScreenWidth, screen.data(), screen.size())) return Result::InvalidMemory;
        if (!Word(screen.data()) || Word(screen.data()) > 4096 ||
            !Word(screen.data() + 4) || Word(screen.data() + 4) > 4096) return Result::UnsupportedTarget;
        result.target.screen_width = int32_t(Word(screen.data()));
        result.target.screen_height = int32_t(Word(screen.data() + 4));
    }
    result.shader = shader;
    result.bucket = bucket;
    auto& d = result.draw;
    d.prim_type = text ? 6 : 4;
    d.gouraud = bit(26);
    d.alpha_blend = bit(0);
    d.tex_enabled = bit(27);
    d.use_fst = !result.sampling.stq;
    d.off_x = float(2048 - (frame.display_width >> 1));
    d.off_y = float(2048 - (frame.display_height >> 1));
    const uint64_t pixels = uint64_t(frame.display_width) * frame.display_height;
    d.raster.frame = {uint32_t(pixels * frame.display_pixel_bytes / 8192) * 8192,
                      (frame.display_width >> 6) * 64, 0, call == 2 ? 0xff000000u : 0};
    d.raster.scissor = {uint16_t(x), uint16_t(y), uint16_t(x + width - 1), uint16_t(y + height - 1)};
    d.raster.zbuf = {frame.depth_buffer_page * 8192, 0, bit(59)};
    d.raster.test = {bit(5), bits(6, 7), bits(9, 255), bits(17, 3), bit(19), bit(20), true, bits(21, 3)};
    d.raster.fba = !bit(56);
    if (bit(38)) {
        d.raster.alpha = {bits(39, 3), bits(41, 3), bits(43, 3), bits(45, 3), bits(47, 255)};
        if (d.raster.alpha.a == 3 || d.raster.alpha.b == 3 || d.raster.alpha.c == 3 || d.raster.alpha.d == 3)
            return Result::UnsupportedMaterial;
    } else {
        // InitAlphaPresets' six meanings; the remaining table entries are zero.
        constexpr std::array<std::array<uint8_t, 4>, 16> presets{{
            {0, 1, 0, 1}, {0, 2, 0, 1}, {2, 0, 0, 1},
            {1, 2, 0, 1}, {2, 1, 0, 1}, {1, 2, 0, 2}}};
        const auto& p = presets[bits(1, 15)];
        d.raster.alpha = {p[0], p[1], p[2], p[3], 0};
    }
    d.raster.colclamp = environment.colour_clamp;
    d.raster.pabe = environment.per_pixel_alpha;
    d.raster.dthe = environment.dither;
    d.raster.dimx = environment.dither_matrix;
    d.raster.fogcol_r = environment.fog_r; d.raster.fogcol_g = environment.fog_g; d.raster.fogcol_b = environment.fog_b;
    d.scanmsk = environment.scan_mask;
    d.tex.fst = d.use_fst;
    if (result.sampling.source == TextureBindings::Source::Asset) {
        const auto& sampling = result.sampling;
        const auto& image = *sampling.asset.image;
        // Format and filter metadata describe the source draw. Storage/CLUT
        // addresses remain zero; the renderer MUST use the explicit lease.
        d.tex.psm = image.format == TextureCodec::Format::Indexed8 ? 19 : 0;
        d.tex.tbw = image.levels[0].width;
        for (uint32_t n = image.levels[0].width; n > 1; n >>= 1) ++d.tex.tw;
        for (uint32_t n = image.levels[0].height; n > 1; n >>= 1) ++d.tex.th;
        d.tex.tcc = sampling.alpha; d.tex.tfx = sampling.function;
        d.tex.mmag = sampling.magnify == TextureBindings::Filter::Linear;
        d.tex.mmin = sampling.minify == TextureBindings::Filter::LinearMipNearest ? 4 :
                     sampling.minify == TextureBindings::Filter::Linear ? 1 : 0;
        d.tex.mxl = sampling.max_level; d.tex.k = sampling.lod_bias; d.tex.l = sampling.lod_shift;
        d.tex.wms = d.tex.wmt = sampling.wrap == TextureBindings::Wrap::Clamp ? 1 : 0;
    }
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
