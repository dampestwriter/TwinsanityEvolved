#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_oracle.h"

#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
#include "engine/gs/state/draw_flush.h"
#include "tests/lib/capture_sink.h"

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t address, CpuContext& context) {
    switch (address) {
    // The source claims the first texture slot for its identity-invert palette.
    // The palette's bytes are a separate OLD pin; the returned ADDRESS is not
    // an input to the replacement and is varied independently of its resources.
    case 0x1c0850: context.cpuRegs.GPR.r[2].UL[0] = 0x100; return;
    default:
        std::fprintf(stderr, "unexpected shadow oracle callee %08x\n", address);
        std::abort();
    }
}
}
namespace {
namespace ram = strip_oracle::memory;
constexpr uint32_t kCursor = 0x323310, kBucket = 0x323af8, kStart = 0x180000;
void Require(bool condition, const char* text) {
    if (!condition) { std::fprintf(stderr, "ShadowOracle: %s\n", text); std::abort(); }
}
void Run(uint32_t address) {
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    context.vuRegs.VF[0].w = 1;
    strip_oracle::RunGenerated(address, context);
}
void Seed(uint32_t width, uint32_t height) {
    ram::bytes.fill(0);
    ram::write<uint32_t>(0x30a764, width);
    ram::write<uint32_t>(0x30a768, height);
    ram::write<uint32_t>(0x309840, 0x160000);
    ram::write<uint32_t>(0x30983c, 0x164000);
    ram::write<uint32_t>(kCursor, kStart);
    ram::write<uint32_t>(kBucket + 8, 0x170000);
    ram::write<uint32_t>(kBucket + 16, 0);
    ram::write<std::array<int32_t, 8>>(0x164000,
        {0, 0, 0, int32_t(width), int32_t(height), 0, int32_t(width), int32_t(height)});
}
void Pairs(uint32_t packet, ps2r::gs::GsRegisters& registers, ps2r::gs::VertexKick& kick,
           ps2r::gs::DrawFlush& flush) {
    const uint64_t tag = ram::read<uint64_t>(packet);
    Require((tag >> 60) == 1 && ram::read<uint64_t>(packet + 8) == 14, "bounded A+D source packet");
    const uint32_t count = uint32_t(tag & 0x7fff);
    Require(count < 2048, "pair limit");
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t value = ram::read<uint64_t>(packet + 16 + i * 16);
        const uint64_t reg = ram::read<uint64_t>(packet + 24 + i * 16);
        Require(reg <= 0x7f, "register address");
        if (reg == 0) { registers.Write(0, value); kick.OnPrimWritten(); }
        else if (reg == 4) kick.WriteXyzf(true, value);
        else if (reg == 5) kick.WriteXyz(true, value);
        else registers.Write(uint8_t(reg), value);
    }
    flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
}
}

std::vector<ps2r::gs::DrawIr> ShadowOracle::Passes(uint32_t width, uint32_t height) {
    Seed(width, height);
    Run(0x1c9048);
    const uint32_t next = ram::read<uint32_t>(kCursor);
    Require(next > kStart + 32 && next < kStart + 65536, "StartShadows ring publication");
    Run(0x1c9ac0);
    Require(ram::read<uint32_t>(kCursor) > next, "ApplyShadows ring publication");
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    ps2r::gs::CaptureSink sink;
    flush.SetSink(&sink);
    const auto changed = [&] { flush.Flush(ps2r::gs::DrawFlush::FlushKind::StateChange); };
    registers.has_pending = [&] { return kick.HasPending(); };
    registers.flush_pending = changed;
    kick.SetFlushHook(changed);
    Pairs(kStart + 32, registers, kick, flush);
    Pairs(next + 16, registers, kick, flush);
    Require(sink.draws.size() == 6, "six source screen operations");
    return sink.draws;
}
uint64_t ShadowOracle::Calls() { return strip_oracle::calls; }
ShadowOracle::CpuEffects ShadowOracle::Matrices(uint32_t width, uint32_t height,
    const std::array<int32_t, 8>& target, const Matrix& camera, const Matrix& screen) {
    Seed(width, height);
    ram::write(0x164000, target);
    ram::write(0x160080, camera);
    ram::write(0x1600c0, screen);
    Run(0x1c9048);
    CpuEffects effects{{ram::read<Matrix>(0x3bd840), ram::read<Matrix>(0x1600c0),
                        ram::read<Matrix>(0x3b03f0), ram::read<Matrix>(0x3b0430)}, {}};
    Run(0x1c9ac0);
    effects.end = ram::read<Matrix>(0x1600c0);
    return effects;
}
