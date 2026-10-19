#include "games/slus_20909_crash_twinsanity/tests/title/effects/cloth_material_oracle.h"

#include <cstring>
#include <stdexcept>

#include "engine/gs/state/draw_flush.h"
#include "tests/lib/capture_sink.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
uint32_t allocation = 0x180000;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t address, CpuContext& c) {
    const uint32_t bytes = c.cpuRegs.GPR.r[4].UL[0];
    if (address != 0x1815c0 || bytes > 4096 || allocation + bytes > 0x190000)
        throw std::runtime_error("cloth material unexpected source call");
    c.cpuRegs.GPR.r[2].UL[0] = allocation;
    allocation += (bytes + 15) & ~15u;
}
}
namespace {
namespace ram = strip_oracle::memory;
void Require(bool value) { if (!value) throw std::runtime_error("cloth material source fixture"); }
template<class T> T Field(const std::vector<uint8_t>& bytes, size_t at) {
    Require(at <= bytes.size() && sizeof(T) <= bytes.size() - at);
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}
void Run(uint32_t entry, uint32_t object = 0) {
    CpuContext c{};
    c.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    c.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    c.cpuRegs.GPR.r[4].UL[0] = object;
    c.vuRegs.VF[0].w = 1;
    strip_oracle::RunGenerated(entry, c);
}
}
ClothMaterialOracle::ClothMaterialOracle(const std::vector<uint8_t>& elf) : elf_(elf) {}
void ClothMaterialOracle::Reset(title::ClothGeometry::Kind kind, uint64_t settings,
                               int16_t k, int16_t l, unsigned levels, unsigned variant) {
    ram::bytes.fill(0);
    const size_t table = Field<uint32_t>(elf_, 28);
    for (size_t i = 0; i < Field<uint16_t>(elf_, 44); ++i) {
        const size_t at = table + i * Field<uint16_t>(elf_, 42);
        if (Field<uint32_t>(elf_, at) != 1) continue;
        const auto offset = Field<uint32_t>(elf_, at + 4), address = Field<uint32_t>(elf_, at + 8);
        const auto size = Field<uint32_t>(elf_, at + 16);
        Require(offset <= elf_.size() && size <= elf_.size() - offset);
        std::memcpy(ram::bytes.data() + ram::Address(address, size), elf_.data() + offset, size);
    }
    const bool three = kind == title::ClothGeometry::Kind::Three;
    ram::write(kMaterial, kShader);
    ram::write<uint32_t>(kMaterial + 0x10, 1);
    ram::write<uint64_t>(kMaterial + 0x18, three ? 0x80000 : 0x10000);
    ram::write<uint32_t>(kMaterial + 0x20, variant % 2 ? 9 : 2);
    ram::write<uint32_t>(kMaterial + 0x60, 1);
    ram::write(kShader, settings);
    ram::write(kShader + 0x14, kTexture);
    ram::write(kShader + 0x18, k); ram::write(kShader + 0x1a, l);
    ram::write(kShader + 0x50, std::array<float, 2>{.125f, -.375f});
    ram::write<uint32_t>(kShader + 0x64, uint32_t(kind));
    ram::write(kShader + 0x6c, three ? title::ClothMaterial::kThreeVtable : title::ClothMaterial::kSingleVtable);
    ram::write(kShader + 0x270, kWaves);
    ram::write(kShader + 0x27c, std::array<float, 3>{.125f, -.0625f, .1875f});
    for (unsigned row = 0; row < 16; ++row)
        ram::write(kWaves + row * 16, std::array<float, 4>{float(row) * .1f - .75f, .5f, -.8f, 12345.f});
    const uint32_t width = variant % 3 ? 512 : 640, height = variant % 2 ? 448 : 480;
    using S = title::MaterialState;
    ram::write(S::kDisplayWidth, width); ram::write(S::kDisplayHeight, height);
    ram::write<uint32_t>(S::kDisplayPixelBytes, 2);
    ram::write<uint32_t>(S::kDepthBufferPage, variant % 2 ? 168 : 180);
    ram::write(S::kRenderTarget, kTarget);
    ram::write(kTarget, std::array<uint32_t, 8>{0, 7, 9, width - 15, height - 19, 0, width, height});
    ram::write<uint32_t>(kTarget + 0x68, 0x15000);
    auto resource = std::make_shared<title::Graphics::Resource>();
    resource->description.width = 256; resource->description.height = 128;
    resource->description.format = ps2r::render::SurfaceFormat::Rgba8;
    resource->description.scale = 1;
    texture = {};
    texture.lease = std::move(resource);
    texture.width = 256; texture.height = 128; texture.buffer_width = 256;
    texture.levels = uint8_t(levels); texture.source_format = variant % 2 ? 19 : 0;
    texture.function = uint8_t(variant % 4); texture.alpha = variant % 3;
    ram::write<uint8_t>(kTexture + 20, uint8_t(levels));
    environment = {bool(variant % 2), bool(variant % 3), uint8_t(variant % 4),
                   uint8_t(variant * 3), 128, 255, uint8_t(variant % 2), 0x6071243571603524};
    strip_oracle::allocation = 0x180000;
}
ps2r::gs::DrawIr ClothMaterialOracle::Original() {
    const auto kind = ram::read<uint32_t>(kShader + 0x64);
    Run(0x1da2d8);
    Run(0x19ef90, kTarget);
    Run(kind == 0x17 ? 0x1d18c8 : 0x1d0d20, kShader);
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    ps2r::gs::CaptureSink sink;
    flush.SetSink(&sink);
    const auto pairs = [&](uint32_t at) {
        const auto tag = ram::read<uint64_t>(at);
        const auto count = uint32_t(tag & 0x7fff);
        Require(count < 16 && (tag >> 60) == 1 && ram::read<uint64_t>(at + 8) == 14);
        for (uint32_t i = 0; i < count; ++i)
            registers.Write(uint8_t(ram::read<uint64_t>(at + 24 + i * 16)),
                            ram::read<uint64_t>(at + 16 + i * 16));
    };
    pairs(ram::read<uint32_t>(kShader + 0x58));
    pairs(ram::read<uint32_t>(kTarget + 0x68) + 16);
    registers.Write(0x46, environment.colour_clamp);
    registers.Write(0x49, environment.per_pixel_alpha);
    registers.Write(0x22, environment.scan_mask);
    registers.Write(0x3d, environment.fog_r | uint64_t(environment.fog_g) << 8 | uint64_t(environment.fog_b) << 16);
    registers.Write(0x44, environment.dither_matrix);
    registers.Write(0x45, environment.dither);
    // The original load owner supplies TEX0 independently of shader settings.
    registers.Write(6, uint64_t(4) << 14 | uint64_t(texture.source_format) << 20 |
        uint64_t(8) << 26 | uint64_t(7) << 30 | uint64_t(texture.alpha) << 34 |
        uint64_t(texture.function) << 35 | uint64_t(1) << 61);
    registers.Write(0, 4);
    kick.OnPrimWritten();
    kick.WriteXyz(true, 0);
    kick.WriteXyz(true, 16);
    kick.WriteXyz(true, uint64_t(16) << 16);
    flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    Require(sink.draws.size() == 1);
    auto draw = sink.draws.front();
    draw.verts.clear(); draw.vertex_count = 0; draw.seq = 0; draw.snapshot = {};
    for (float& value : draw.bbox) value = 0;
    return draw;
}
title::MaterialState::Read ClothMaterialOracle::Reader(uint32_t deny) const {
    return [deny](uint32_t address, void* out, size_t size) {
        const uint32_t at = address & 0x1fffffff;
        if (at >= ram::bytes.size() || size > ram::bytes.size() - at ||
            (deny && at <= deny && uint64_t(at) + size > deny)) return false;
        std::memcpy(out, ram::bytes.data() + at, size);
        return true;
    };
}
uint64_t ClothMaterialOracle::Calls() const { return strip_oracle::calls; }
