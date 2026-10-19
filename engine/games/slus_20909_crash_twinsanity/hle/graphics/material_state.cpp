#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

#include <array>
#include <utility>

namespace title {
namespace {
using Result = MaterialState::Result;

uint32_t Word(const uint8_t* data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

uint64_t Long(const uint8_t* data) {
    return Word(data) | (uint64_t(Word(data + 4)) << 32);
}

bool ReadBytes(const MaterialState::Read& read, uint32_t address,
               void* data, size_t size) {
    if (!read || !address || uint64_t(address) + size > uint64_t{1} << 32)
        return false;
    return read(address, data, size);
}

bool ValidFrame(const MaterialState::Frame& frame) {
    // The shared target uses CT32 drawing behind the 16-bit display buffer.
    // Limit this replacement to representable, non-wrapping buffer placement.
    if (frame.display_width < 64 || frame.display_width > 4032 ||
        frame.display_height == 0 || frame.display_height > 2048 ||
        frame.display_pixel_bytes != 2) return false;
    const uint64_t pixels = uint64_t(frame.display_width) * frame.display_height;
    const uint64_t base = (pixels * frame.display_pixel_bytes / 8192) * 8192;
    return base + pixels * 4 <= 4 * 1024 * 1024 &&
           frame.depth_buffer_page <= 511 && frame.environment.scan_mask <= 3 &&
           frame.environment.dither <= 1;
}

bool FlatSettings(uint64_t settings) {
    // ShaderConstruct + MakeFlatMaterial/SetFlatSettings. Check meaning, not an
    // allocator-dependent 64-bit literal. Disabled test parameters are retained
    // below; texture/animation-only fields do not affect this Draw2D operation.
    constexpr uint64_t required = uint64_t{1} | (uint64_t{2} << 21) |
                                  (uint64_t{1} << 26) | (uint64_t{1} << 59);
    constexpr uint64_t mask = 0x3f | (uint64_t{1} << 19) | (uint64_t{3} << 21) |
        (uint64_t{3} << 26) | (uint64_t{3} << 29) | (uint64_t{1} << 38) |
        (uint64_t{1} << 56) | (uint64_t{1} << 59);
    return (settings & mask) == required;
}

Result ReadShader(const MaterialState::Read& read, uint32_t material,
                  uint64_t& settings) {
    // Material: first shader +0, count +0x10, program key +0x18, bucket +0x20.
    // NTSC 0x16cfc8..0x16cfe8 writes these exact flat-material fields.
    if (material != MaterialState::kFlatMaterial) return Result::UnsupportedMaterial;
    std::array<uint8_t, 0x24> object{};
    if (!ReadBytes(read, material, object.data(), object.size())) return Result::InvalidMemory;
    if (Word(object.data() + 0x10) != 1 || Long(object.data() + 0x18) != 2 ||
        Word(object.data() + 0x20) != 24) return Result::UnsupportedMaterial;
    std::array<uint8_t, 0x70> shader{};
    if (!ReadBytes(read, Word(object.data()), shader.data(), shader.size())) return Result::InvalidMemory;
    // Do not follow the legacy register pointer at +0x58 or require a packet.
    // Type/set-up and the concrete vtable identify the settings implementation.
    if (Word(shader.data() + 0x64) != 14 ||
        Word(shader.data() + 0x6c) != MaterialState::kFlatShaderVtable)
        return Result::UnsupportedShader;
    settings = Long(shader.data());
    return FlatSettings(settings) ? Result::Ok : Result::UnsupportedShader;
}

Result ReadTarget(const MaterialState::Read& read, uint32_t target,
                  MaterialState::Output& output) {
    // RenderTargetDescription: offset +4/+8, extent +c/+10, display +18/+1c.
    // NTSC SetUpRenderTarget 0x19f15c..0x19f19c uses the same scissor fields.
    std::array<uint8_t, 0x20> object{};
    if (!ReadBytes(read, target, object.data(), object.size())) return Result::InvalidMemory;
    const uint32_t x = Word(object.data() + 4), y = Word(object.data() + 8);
    const uint32_t width = Word(object.data() + 0xc), height = Word(object.data() + 0x10);
    const uint32_t display_width = Word(object.data() + 0x18);
    const uint32_t display_height = Word(object.data() + 0x1c);
    if (x > 2047 || y > 2047 || !width || !height ||
        uint64_t(x) + width > 2048 || uint64_t(y) + height > 2048 ||
        !display_width || display_width > 4096 || !display_height || display_height > 4096)
        return Result::UnsupportedTarget;
    output.coordinates = {static_cast<int32_t>(display_width), static_cast<int32_t>(display_height),
                          static_cast<int32_t>(x), static_cast<int32_t>(y)};
    output.draw.raster.scissor = {static_cast<uint16_t>(x), static_cast<uint16_t>(y),
        static_cast<uint16_t>(x + width - 1), static_cast<uint16_t>(y + height - 1)};
    return Result::Ok;
}

void FillDraw(uint64_t settings, const MaterialState::Frame& frame,
              MaterialState::Output& output) {
    auto& draw = output.draw;
    const auto& environment = frame.environment;
    output.bucket = 24;
    draw.prim_type = 4;
    draw.gouraud = true;
    draw.alpha_blend = true;
    draw.fog = false;
    draw.tex_enabled = false;
    draw.use_fst = (settings & (uint64_t{1} << 28)) == 0;
    draw.ctx = 0;
    draw.off_x = static_cast<float>(2048 - (frame.display_width >> 1));
    draw.off_y = static_cast<float>(2048 - (frame.display_height >> 1));
    // No texture is consumed; retain FST's meaning in both IR views. All other
    // texture fields are the canonical inactive view, independent of legacy GS.
    draw.tex = {};
    draw.tex.fst = draw.use_fst;
    const uint64_t pixels = uint64_t(frame.display_width) * frame.display_height;
    draw.raster.frame = {static_cast<uint32_t>(pixels * frame.display_pixel_bytes / 8192) * 8192,
                        (frame.display_width >> 6) * 64, 0, 0};
    // Type E emits Z32 (the IR stores the low four-bit format, zero).
    draw.raster.zbuf = {frame.depth_buffer_page * 8192, 0, true};
    draw.raster.test = {false, static_cast<uint8_t>((settings >> 6) & 7),
        static_cast<uint8_t>((settings >> 9) & 255), static_cast<uint8_t>((settings >> 17) & 3),
        false, (settings & (uint64_t{1} << 20)) != 0, true, 2};
    // Retail mix: (source - destination) * source_alpha + destination.
    draw.raster.alpha = {0, 1, 0, 1, 0};
    draw.raster.fba = true;
    draw.raster.colclamp = environment.colour_clamp;
    draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.fogcol_r = environment.fog_r;
    draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b;
    draw.raster.dthe = environment.dither;
    draw.raster.dimx = environment.dither_matrix;
    draw.scanmsk = environment.scan_mask;
}
}  // namespace

MaterialState::Result MaterialState::ReadFrame(const Read& read, const Environment& environment,
                                               Frame& out) {
    constexpr std::array<uint32_t, 4> addresses{
        kDisplayWidth, kDisplayHeight, kDisplayPixelBytes, kDepthBufferPage};
    std::array<uint32_t, 4> values{};
    for (size_t i = 0; i < addresses.size(); ++i) {
        std::array<uint8_t, 4> bytes{};
        if (!ReadBytes(read, addresses[i], bytes.data(), bytes.size())) return Result::InvalidMemory;
        values[i] = Word(bytes.data());
    }
    Frame candidate{values[0], values[1], values[2], values[3], environment};
    if (!ValidFrame(candidate)) return Result::UnsupportedFrame;
    out = candidate;
    return Result::Ok;
}

MaterialState::Result MaterialState::Build(const Read& read, uint32_t material, uint32_t target,
                                          const Frame& frame, Output& out) {
    if (!ValidFrame(frame)) return Result::UnsupportedFrame;
    uint64_t settings = 0;
    const Result shader = ReadShader(read, material, settings);
    if (shader != Result::Ok) return shader;
    Output candidate;
    const Result rectangle = ReadTarget(read, target, candidate);
    if (rectangle != Result::Ok) return rectangle;
    FillDraw(settings, frame, candidate);
    out = std::move(candidate);
    return Result::Ok;
}

}  // namespace title
