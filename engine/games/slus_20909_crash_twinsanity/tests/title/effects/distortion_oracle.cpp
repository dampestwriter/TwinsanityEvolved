#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_oracle.h"

#include <cstring>
#include <stdexcept>

#include "engine/core/hash/hash.h"
#include "engine/gs/state/draw_flush.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_unit.h"
#include "tests/lib/capture_sink.h"

namespace {
using Geometry = title::DistortionGeometry;
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
void Word(VuUnit& unit, uint32_t at, std::array<uint32_t, 4> value) {
    unit.WriteDataMem(at * 16, reinterpret_cast<const uint8_t*>(value.data()), 16);
}
std::array<uint32_t, 4> Load(VuUnit& unit, uint32_t at) {
    std::array<uint32_t, 4> value;
    unit.ReadDataMem(at * 16, reinterpret_cast<uint8_t*>(value.data()), 16);
    return value;
}
void SetInputs(VuUnit& unit, const Geometry::Look& look, const Geometry::View& view) {
    unit.regs().VI[14] = 220;
    unit.WriteDataMem(79 * 16, reinterpret_cast<const uint8_t*>(view.screen.data()), 64);
    Word(unit, 83, {Bits(.5f), Bits(look.gravity), 0, Bits(view.time)});
    Word(unit, 84, {Bits(view.distortion[0] * 0x1p-14f), Bits(view.distortion[1] * -0x1p-12f),
                    Bits(40), 0});
    for (unsigned i = 0; i < 3; ++i)
        Word(unit, 85 + i, {Bits(look.corners[2*i][0]), Bits(look.corners[2*i][1]),
                            Bits(look.corners[2*i+1][0]), Bits(look.corners[2*i+1][1])});
    for (unsigned i = 0; i < 64; ++i) {
        const auto& shape = look.shapes[i];
        Word(unit, 88 + 2*i, {Bits(shape[0]), Bits(shape[1]), Bits(shape[2]), Bits(shape[3])});
        Word(unit, 89 + 2*i, {Bits(shape[4]), Bits(shape[5]), 0, 0});
    }
    uint32_t width = 1, height = 1;
    while (width < view.width) width *= 2;
    while (height < view.height) height *= 2;
    Word(unit, 225, {Bits((float(view.width) * 8 - 32768) / (float(width) * 16)),
                    Bits((float(view.height) * 8 - 32768) / (float(height) * 16)), Bits(0x1p-17f), 0});
    Word(unit, 226, {Bits(1.f / (float(width) * 16)), Bits(1.f / (float(height) * 16)),
                    Bits((float(view.width) - 1) / float(width)),
                    Bits((float(view.height) - 1) / float(height))});
    for (unsigned i = 0; i < 4; ++i) Word(unit, 221 + i, {400 + i * 100, 0, 0, 0});
}
void Assemble(VuUnit& unit, uint32_t at, ps2r::gs::GsRegisters& registers,
              ps2r::gs::VertexKick& kick, ps2r::gs::DrawFlush& flush) {
    const auto header = Load(unit, at);
    const uint32_t loops = header[0] & 0x7fff, count = header[1] >> 28;
    if (loops != 6 || count != 13) throw std::runtime_error("original hexagon output header changed");
    const uint64_t descriptors = uint64_t(header[2]) | uint64_t(header[3]) << 32;
    kick.OnPrimWritten();
    for (unsigned loop = 0; loop < loops; ++loop) for (unsigned reg = 0; reg < count; ++reg) {
        const auto words = Load(unit, ++at);
        const uint64_t low = uint64_t(words[0]) | uint64_t(words[1]) << 32;
        const uint64_t high = uint64_t(words[2]) | uint64_t(words[3]) << 32;
        switch (descriptors >> (4 * reg) & 15) {
        case 1: registers.PackedRgbaq(low, high); break;
        case 2: registers.PackedSt(low, high); break;
        case 5: kick.PackedXyz(true, low, high); break;
        default: throw std::runtime_error("unexpected original hexagon descriptor");
        }
    }
    flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
}
}
DistortionOracle::DistortionOracle(std::vector<uint8_t> program) : program_(std::move(program)) {
    if (program_.size() != 212 * 8 ||
        ps2r::hash::Fnv1a64(program_.data(), program_.size()) != 0x12ec15362067dbdbull)
        throw std::invalid_argument("unverified original NTSC type18 program");
}
std::vector<ps2r::gs::DrawIr> DistortionOracle::Run(const std::vector<Geometry::Block>& blocks,
    const Geometry::Look& look, const Geometry::View& view, const ps2r::gs::DrawIr& material) const {
    VuChopRoundGuard rounding;
    ps2r::gs::CaptureSink sink;
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    flush.SetSink(&sink);
    registers.Write(0, 4 | 16 | 8);
    registers.Write(0x18, uint64_t(uint32_t(material.off_x * 16)) |
                              uint64_t(uint32_t(material.off_y * 16)) << 32);
    for (const auto& block : blocks) {
        VuUnit unit(1);
        unit.WriteMicroMem(0, program_.data(), program_.size());
        unit.SetVifTop(256, 0);
        SetInputs(unit, look, view);
        // Source InitParticleBlock's complete hexagon tag and 12 records.
        constexpr std::array<unsigned, 13> descriptors{2,1,5,2,5,1,2,5,1,2,5,2,5};
        uint64_t packed = 0;
        for (unsigned i = 0; i < descriptors.size(); ++i) packed |= uint64_t(descriptors[i]) << (4*i);
        Word(unit, 256, {0x8006, 0xd0024000, uint32_t(packed), uint32_t(packed >> 32)});
        for (unsigned i = 0; i < block.size(); ++i) {
            const auto& particle = block[i];
            Word(unit, 257 + i*2, {Bits(particle.start[0]), Bits(particle.start[1]), Bits(particle.start[2]),
                                   Bits(particle.spawn_time)});
            Word(unit, 258 + i*2, {Bits(particle.velocity[0]), Bits(particle.velocity[1]),
                                   Bits(particle.velocity[2]), Bits(particle.life_scale)});
        }
        VuInterpreterTier interpreter(100000);
        interpreter.Run(unit, 0);
        if (interpreter.bundles() >= 100000 || unit.regs().TPC != 0x140)
            throw std::runtime_error("original distortion setup did not reach its source continuation");
        if (unit.regs().VF[24].x == 0)
            throw std::runtime_error("original distortion setup lost its source texture scale");
        interpreter.Run(unit, 0x140);
        if (interpreter.bundles() >= 100000 || unit.regs().TPC != 0x668)
            throw std::runtime_error("original distortion block did not complete");
        for (unsigned group = 0; group < 4; ++group)
            Assemble(unit, 700 - group * 100, registers, kick, flush);
    }
    return sink.draws;
}
