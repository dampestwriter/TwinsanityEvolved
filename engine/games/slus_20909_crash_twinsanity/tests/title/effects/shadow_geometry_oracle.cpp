#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_geometry_oracle.h"

#include <stdexcept>

#include "engine/gs/state/draw_flush.h"
#include "engine/vu/vu_chop_round_guard.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/family_tristrip.h"
#include "tests/lib/capture_sink.h"

namespace {
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
void Parse(const Vu1HleOutput& output, const title::ShadowGeometry::View& view,
           std::vector<ps2r::gs::DrawIr>& draws) {
    ps2r::gs::GsRegisters registers;
    ps2r::gs::VertexKick kick(&registers);
    ps2r::gs::DrawFlush flush(&registers, &kick);
    ps2r::gs::CaptureSink sink;
    flush.SetSink(&sink);
    registers.Write(0x18, uint64_t(uint32_t(view.offset[0] * 16)) |
                              uint64_t(uint32_t(view.offset[1] * 16)) << 32);
    for (const auto& packet : output.kicks) {
        const auto word = [&](size_t offset) {
            if (offset + 8 > packet.size()) throw std::runtime_error("shadow oracle packet bound");
            uint64_t value;
            std::memcpy(&value, packet.data() + offset, 8);
            return value;
        };
        const uint64_t tag = word(0), descriptors = word(8);
        const uint32_t count = uint32_t(tag & 0x7fff), fields = uint32_t(tag >> 60);
        if (((tag >> 58) & 3) || fields != 3 || descriptors != 0x512 ||
            packet.size() != 16 + size_t(count) * 48)
            throw std::runtime_error("shadow oracle source layout");
        registers.Write(0, (tag >> 47) & 0x7ff);
        kick.OnPrimWritten();
        for (uint32_t i = 0; i < count; ++i) {
            const size_t at = 16 + size_t(i) * 48;
            registers.PackedSt(word(at), word(at + 8));
            registers.PackedRgbaq(word(at + 16), word(at + 24));
            kick.PackedXyz(true, word(at + 32), word(at + 40));
        }
        flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    }
    for (auto& draw : sink.draws) draws.push_back(std::move(draw));
}
void Instance(const title::ShadowGeometry::Strip& strip, const title::ShadowGeometry::View& view,
              bool winding, std::vector<ps2r::gs::DrawIr>& draws) {
    std::array<uint8_t, 16384> bytes{};
    QwPtr memory{bytes.data()};
    constexpr uint32_t node = 96, mesh = 120, top = 384;
    memory.SetW(1, 0, 600); memory.SetW(1, 1, 800);
    memory.SetW(1, 2, 600); memory.SetW(1, 3, 800);
    memory.SetW(2, 0, mesh);
    memory.SetW(4, 1, 0xb0 / 8);
    memory.SetW(5, 1, 0x440 / 8);
    memory.SetW(node, 1, 0x1ac0 / 8);
    memory.SetW(node + 2, 3, winding);
    for (unsigned lane = 0; lane < 4; ++lane)
        memory.SetW(node + 1, lane, lane == 3 ? 128 : winding ? 248 : 8);
    memory.SetW(mesh, 1, mesh + 10);
    memory.SetW(mesh, 2, 5);
    memory.SetW(mesh, 3, 10);
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned lane = 0; lane < 4; ++lane) {
            memory.SetW(mesh + 1 + row, lane, Bits(view.screen[row][lane]));
            memory.SetW(mesh + 5 + row, lane, Bits(view.clip[row][lane]));
        }
    for (unsigned lane = 0; lane < 4; ++lane)
        memory.SetW(mesh + 9, lane, Bits(view.guard[lane]));
    const auto tag = [&](uint32_t address, uint32_t primitive, uint32_t count) {
        const uint64_t first = uint64_t(count | 0x8000) | uint64_t(1) << 46 |
                               uint64_t(primitive) << 47 | uint64_t(3) << 60;
        memory.SetW(address, 0, uint32_t(first));
        memory.SetW(address, 1, uint32_t(first >> 32));
        memory.SetW(address, 2, 0x512);
    };
    tag(6, 5, 0);
    tag(top, 4, uint32_t(strip.vertices.size()));
    memory.SetW(top + 1, 0, uint32_t(strip.vertices.size() * 4));
    for (size_t i = 0; i < strip.vertices.size(); ++i) {
        const auto& vertex = strip.vertices[i];
        for (unsigned lane = 0; lane < 3; ++lane)
            memory.SetW(top + 3 + uint32_t(i) * 4, lane, Bits(vertex.position[lane]));
        memory.SetW(top + 4 + uint32_t(i) * 4, 3, vertex.restart ? 0x8000 : 0);
        memory.SetW(top + 5 + uint32_t(i) * 4, 0, vertex.reverse_fan ? 32 : 0);
    }
    ClipGate::GateRegs context{};
    context.valid = true;
    context.fan_pc = 0x1ac0;
    context.fan_kind = 2;
    Vu1HleOutput output;
    VuChopRoundGuard rounding;
    if (!FamilyTristrip::RunEmitterCall(memory, node, top, output, &context))
        throw std::runtime_error("NEW shadow family declined synthetic source input");
    Parse(output, view, draws);
}
}
std::vector<ps2r::gs::DrawIr> ShadowGeometryOracle::Run(
    const title::ShadowGeometry::Mesh& mesh, const std::vector<title::ShadowGeometry::View>& views) {
    std::vector<ps2r::gs::DrawIr> draws;
    for (const auto& strip : mesh.strips)
        for (bool winding : {false, true})
            for (const auto& view : views) Instance(strip, view, winding, draws);
    return draws;
}
