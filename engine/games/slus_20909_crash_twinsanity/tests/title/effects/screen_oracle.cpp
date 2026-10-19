#include "games/slus_20909_crash_twinsanity/tests/title/effects/screen_oracle.h"

#include "engine/gs/state/draw_flush.h"
#include "tests/lib/capture_sink.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t address, CpuContext& c) {
    switch (address) {
    // Program/shader transport cannot affect the already emitted copy and
    // complete screen setup. WaveGeometry has its own unchanged-program oracle.
    case 0x1da0f0: c.cpuRegs.GPR.r[2].UL[0] = c.cpuRegs.GPR.r[4].UL[0]; return;
    case 0x1da980: c.cpuRegs.GPR.r[2].UL[0] = c.cpuRegs.GPR.r[5].UL[0]; return;
    case 0x1816c8: c.cpuRegs.GPR.r[2].UL[0] = 0x170000; return;
    case 0x205528: c.cpuRegs.GPR.r[2].UL[0] = 0x190000; return;
    default:
        std::fprintf(stderr, "unexpected screen oracle callee %08x\n", address);
        std::abort();
    }
}
}
namespace {
namespace ram = strip_oracle::memory;
constexpr uint32_t kStart = 0x180000, kCursor = 0x323310, kBucket = 0x323740;
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "ScreenOracle: %s\n", message); std::abort(); }
}
void Reset(uint32_t width, uint32_t height) {
    ram::bytes.fill(0);
    ram::write<uint32_t>(0x309744, width);
    ram::write<uint32_t>(0x309748, height);
    ram::write<uint32_t>(0x30a77c, 0x38);
    ram::write<uint32_t>(0x30a780, 0xa8);
    ram::write<uint32_t>(kCursor, kStart);
    ram::write<uint32_t>(kBucket + 8, 0x170000);
    ram::write<uint8_t>(0x3114f0 - 0x7c5f, 1); // FilterColours enabled
}
uint32_t Run(uint32_t entry) {
    CpuContext c{};
    c.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    c.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    c.vuRegs.VF[0].w = 1;
    const auto start = ram::read<uint32_t>(kCursor);
    strip_oracle::RunGenerated(entry, c);
    Require(ram::read<uint32_t>(kCursor) > start, "source producer published its packet");
    return start;
}
uint32_t Pairs(uint32_t direct, ps2r::gs::GsRegisters& registers,
               ps2r::gs::VertexKick& kick, ps2r::gs::DrawFlush& flush) {
    const uint32_t packet = direct + 16;
    const uint64_t tag = ram::read<uint64_t>(packet);
    const auto count = uint32_t(tag & 0x7fff);
    Require((tag >> 60) == 1 && ram::read<uint64_t>(packet + 8) == 14 && count < 2048,
            "source-bounded screen A+D packet");
    for (uint32_t i = 0; i < count; ++i) {
        const auto value = ram::read<uint64_t>(packet + 16 + i * 16);
        const auto reg = ram::read<uint64_t>(packet + 24 + i * 16);
        Require(reg <= 0x7f, "source screen register bound");
        if (reg == 0) { registers.Write(0, value); kick.OnPrimWritten(); }
        else if (reg == 5) kick.WriteXyz(true, value);
        else registers.Write(uint8_t(reg), value);
    }
    flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    return packet + (count + 1) * 16;
}
std::vector<ps2r::gs::DrawIr> Execute(uint32_t width, uint32_t height, unsigned kind, bool blur) {
    Reset(width, height);
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    ps2r::gs::CaptureSink sink;
    flush.SetSink(&sink);
    const auto changed = [&] { flush.Flush(ps2r::gs::DrawFlush::FlushKind::StateChange); };
    registers.has_pending = [&] { return kick.HasPending(); };
    registers.flush_pending = changed;
    kick.SetFlushHook(changed);
    registers.Write(0x46, 1); // source frame owns COLCLAMP
    const auto depth = Run(0x1af788);
    Pairs(Pairs(depth, registers, kick, flush), registers, kick, flush);
    if (kind == 3) return sink.draws; // the two CopyDepthBuffer halves alone
    if (blur) Pairs(Run(0x1afca8), registers, kick, flush);
    sink.draws.clear();
    if (kind == 0) Pairs(Run(0x1afca8), registers, kick, flush);
    if (kind == 1) {
        const auto setup = Pairs(Run(0x1aef48), registers, kick, flush);
        Pairs(setup, registers, kick, flush);
        // Probe only the source material state after complete setup. Geometry
        // equivalence is separately pinned against the original type-1C image.
        registers.Write(0, 4);
        kick.OnPrimWritten();
        kick.WriteXyz(true, 0);
        kick.WriteXyz(true, 16);
        kick.WriteXyz(true, uint64_t(16) << 16);
        flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    }
    if (kind == 2) Pairs(Run(0x1ae698) + 16, registers, kick, flush);
    return sink.draws;
}
}
std::vector<ps2r::gs::DrawIr> ScreenOracle::Blur(uint32_t height) {
    return Execute(512, height, 0, false);
}
std::vector<ps2r::gs::DrawIr> ScreenOracle::Wave(uint32_t width, uint32_t height) {
    return Execute(width, height, 1, false);
}
std::vector<ps2r::gs::DrawIr> ScreenOracle::DepthCopy(uint32_t height) {
    auto draws = Execute(512, height, 3, false);
    Require(draws.size() == 2, "two depth copy halves");
    return draws;
}
ps2r::gs::DrawIr ScreenOracle::Filter(uint32_t height, bool after_blur) {
    auto draws = Execute(512, height, 2, after_blur);
    Require(draws.size() == 1, "one filter draw");
    return draws.front();
}
uint64_t ScreenOracle::Calls() { return strip_oracle::calls; }
