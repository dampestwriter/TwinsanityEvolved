#include "games/slus_20909_crash_twinsanity/tests/title/effects/wave_oracle.h"

#include <cstring>
#include <stdexcept>

#include "engine/core/hash/hash.h"
#include "engine/gs/state/draw_flush.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "tests/lib/capture_sink.h"

namespace {
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
void Qw(VuUnit& unit, uint32_t address, std::array<uint32_t, 4> words) {
    unit.WriteDataMem(address * 16, reinterpret_cast<const uint8_t*>(words.data()), 16);
}
std::array<uint32_t, 4> Load(VuUnit& unit, uint32_t address) {
    std::array<uint32_t, 4> words;
    unit.ReadDataMem(address * 16, reinterpret_cast<uint8_t*>(words.data()), 16);
    return words;
}
}

WaveOracle::WaveOracle(std::vector<uint8_t> program) : program_(std::move(program)) {
    if (program_.size() != 78 * 8 ||
        ps2r::hash::Fnv1a64(program_.data(), program_.size()) != 0xf836c7ff99ff95eeull)
        throw std::invalid_argument("unverified NTSC type-1C program");
}
std::vector<ps2r::gs::DrawIr> WaveOracle::Run(
    const title::WaveGeometry::Grid& grid, const title::WaveGeometry::Waves& waves) const {
    ps2r::gs::CaptureSink sink;
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    flush.SetSink(&sink);
    registers.Write(0, 4 | 16 | 256);
    registers.Write(0x18, uint64_t((2048 - (grid.width >> 1)) * 16) |
                              uint64_t((2048 - (grid.height >> 1)) * 16) << 32);
    for (const auto& strip : grid.strips) {
        VuUnit unit(1);
        unit.WriteMicroMem(0, program_.data(), program_.size());
        unit.SetVifTop(256, 0);
        unit.regs().VI[14] = 96;
        // The unmodified JR at program end returns into a zero-filled exit.
        unit.regs().VI[15] = 0x3e0;
        Qw(unit, 1, {600, 750, 600, 750});
        Qw(unit, 98, {0, 0, 0, Bits(waves.amplitude)});
        for (size_t i = 0; i < waves.rows.size(); ++i)
            Qw(unit, 99 + uint32_t(i), {Bits(waves.rows[i][0]), Bits(waves.rows[i][1]),
                                       Bits(waves.rows[i][2]), 0});
        const uint32_t count = uint32_t(strip.vertices.size());
        Qw(unit, 256, {count | 0x8000u, 0x30000000, 0x513, 0});
        Qw(unit, 257, {count * 4, count | 0x8000u, 0, 0});
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            const auto& vertex = strip.vertices[i];
            Qw(unit, 259 + uint32_t(i) * 4, {Bits(vertex.position[0]), Bits(vertex.position[1]),
                                            vertex.depth, vertex.restart ? 0x8000u : 0u});
            Qw(unit, 260 + uint32_t(i) * 4, {vertex.texture[0], vertex.texture[1], 0, 0});
        }
        VuInterpreterTier interpreter(100000);
        interpreter.Run(unit, 0);
        if (interpreter.bundles() >= 100000)
            throw std::runtime_error("type-1C interpreter did not return");
        kick.OnPrimWritten();
        for (uint32_t i = 0; i < count; ++i) {
            const auto uv = Load(unit, 601 + 3 * i);
            const auto colour = Load(unit, 602 + 3 * i);
            const auto xyz = Load(unit, 603 + 3 * i);
            registers.PackedUv(uint64_t(uv[0]) | uint64_t(uv[1]) << 32);
            registers.PackedRgbaq(uint64_t(colour[0]) | uint64_t(colour[1]) << 32,
                                  uint64_t(colour[2]) | uint64_t(colour[3]) << 32);
            kick.PackedXyz(true, uint64_t(xyz[0]) | uint64_t(xyz[1]) << 32,
                          uint64_t(xyz[2]) | uint64_t(xyz[3]) << 32);
        }
        flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    }
    return sink.draws;
}
