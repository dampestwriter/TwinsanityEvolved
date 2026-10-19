#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_material_oracle.h"

#include <stdexcept>

#include "engine/gs/state/draw_flush.h"
#include "tests/lib/capture_sink.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t address, CpuContext& c) {
    if (address != 0x1815c0 || c.cpuRegs.GPR.r[4].UL[0] != 0xa0)
        throw std::runtime_error("unexpected distortion source callee/allocation");
    c.cpuRegs.GPR.r[2].UL[0] = 0x190000;
}
}
namespace {
namespace ram = strip_oracle::memory;
void Require(bool value) { if (!value) throw std::runtime_error("distortion material source fixture"); }
CpuContext Context(uint32_t argument = 0) {
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    context.cpuRegs.GPR.r[4].UL[0] = argument;
    context.vuRegs.VF[0].w = 1;
    return context;
}
void Run(uint32_t entry, uint32_t argument = 0) {
    auto context = Context(argument);
    strip_oracle::RunGenerated(entry, context);
}
void Pairs(uint32_t at, ps2r::gs::GsRegisters& registers, ps2r::gs::VertexKick& kick,
           ps2r::gs::DrawFlush& flush) {
    const auto tag = ram::read<uint64_t>(at);
    const auto count = uint32_t(tag & 0x7fff);
    Require(count <= 80 && (tag >> 60) == 1 && ram::read<uint64_t>(at + 8) == 14);
    for (uint32_t i = 0; i < count; ++i) {
        const auto value = ram::read<uint64_t>(at + 16 + i * 16);
        const auto reg = ram::read<uint64_t>(at + 24 + i * 16);
        Require(reg <= 0x7f);
        if (reg == 0) { registers.Write(0, value); kick.OnPrimWritten(); }
        else if (reg == 5) kick.WriteXyz(true, value);
        else registers.Write(uint8_t(reg), value);
    }
    flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
}
}
void DistortionMaterialOracle::Reset(title::DistortionMaterial::Settings settings,
                                     uint32_t height, unsigned variant) {
    ram::bytes.fill(0);
    using S = title::MaterialState;
    frame = {512, height, 2, 168, {bool(variant % 2), bool(variant % 3), uint8_t(variant % 4),
        uint8_t(variant * 3), 128, 255, uint8_t(variant % 2), 0x6071243571603524}};
    target = {0, 7, 9, 497, height - 19, 0, 512, height};
    ram::write(kShader, settings.attributes);
    ram::write(kShader + 0x18, settings.lod_k); ram::write(kShader + 0x1a, settings.lod_l);
    ram::write<uint32_t>(kShader + 0x64, 0x18);
    ram::write<uint32_t>(kShader + 0x6c, title::DistortionMaterial::kVtable);
    ram::write<uint32_t>(title::DistortionMaterial::kVtable + 0x2c, title::DistortionMaterial::kSettings);
    ram::write(S::kDisplayWidth, frame.display_width); ram::write(S::kDisplayHeight, height);
    ram::write(S::kDisplayPixelBytes, frame.display_pixel_bytes);
    ram::write(S::kDepthBufferPage, frame.depth_buffer_page);
    ram::write(S::kRenderTarget, kTarget); ram::write(kTarget, target);
    ram::write<uint32_t>(kTarget + 0x68, 0x15000);
    ram::write<uint32_t>(0x309744, 512); ram::write<uint32_t>(0x309748, height);
    ram::write<uint32_t>(0x30a77c, frame.display_width * height * 2 / 8192);
}
std::vector<ps2r::gs::DrawIr> DistortionMaterialOracle::Original(bool late_fields) {
    ram::write(kTarget, target);
    Run(0x1da2d8); Run(0x19ef90, kTarget); Run(0x1d5f90, kShader);
    if (late_fields) {
        ram::write<uint64_t>(kShader, ram::read<uint64_t>(kShader) | uint64_t{1} << 55);
        ram::write<int16_t>(kShader + 0x18, -200); ram::write<int16_t>(kShader + 0x1a, 1);
    }
    auto copy_context = Context(kShader);
    copy_context.cpuRegs.GPR.r[5].UL[0] = kCopy;
    copy_context.cpuRegs.GPR.r[6].UL[0] = 0x12000;
    copy_context.cpuRegs.GPR.r[7].UL[0] = 1;
    strip_oracle::RunGenerated(0x1d6318, copy_context);
    Require(copy_context.cpuRegs.GPR.r[2].UL[0] == kCopy + 0x520);
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    ps2r::gs::CaptureSink sink;
    flush.SetSink(&sink);
    const auto changed = [&] { flush.Flush(ps2r::gs::DrawFlush::FlushKind::StateChange); };
    registers.has_pending = [&] { return kick.HasPending(); };
    registers.flush_pending = changed; kick.SetFlushHook(changed);
    const auto& environment = frame.environment;
    registers.Write(0x46, environment.colour_clamp); registers.Write(0x49, environment.per_pixel_alpha);
    registers.Write(0x22, environment.scan_mask); registers.Write(0x44, environment.dither_matrix);
    registers.Write(0x45, environment.dither);
    registers.Write(0x3d, environment.fog_r | uint64_t(environment.fog_g) << 8 | uint64_t(environment.fog_b) << 16);
    Pairs(ram::read<uint32_t>(kTarget + 0x68) + 16, registers, kick, flush);
    Pairs(kCopy + 16, registers, kick, flush);
    Pairs(ram::read<uint32_t>(kShader + 0x58), registers, kick, flush);
    registers.Write(0, 4);
    kick.OnPrimWritten();
    // The original type18 program writes ST before its vertices, replacing the
    // preceding copy's UV attribute. Full expansion has its independent oracle.
    registers.PackedSt(0, 0x3f800000);
    kick.WriteXyz(true, 0); kick.WriteXyz(true, 16); kick.WriteXyz(true, uint64_t(16) << 16);
    flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    Require(sink.draws.size() == 2);
    return sink.draws;
}
title::MaterialState::Read DistortionMaterialOracle::Reader(uint32_t deny) const {
    return [deny](uint32_t address, void* out, size_t size) {
        const auto at = address & 0x1fffffff;
        if (at >= ram::bytes.size() || size > ram::bytes.size() - at ||
            (deny && at <= deny && uint64_t(at) + size > deny)) return false;
        std::memcpy(out, ram::bytes.data() + at, size);
        return true;
    };
}
uint64_t DistortionMaterialOracle::Calls() { return strip_oracle::calls; }
