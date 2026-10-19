#include "games/slus_20909_crash_twinsanity/tests/title/effects/cloth_oracle.h"

#include <cstring>
#include <memory>
#include <stdexcept>

#include "engine/core/hash/hash.h"
#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "engine/vif/vif.h"
#include "engine/vu/tiers/vu_tier_dispatch.h"
#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_unit.h"
#include "tests/lib/capture_sink.h"

namespace {
void Require(bool value) { if (!value) throw std::runtime_error("cloth original fixture contract"); }
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
template<class T> T Load(const std::vector<uint8_t>& bytes, size_t at) {
    Require(at <= bytes.size() && sizeof(T) <= bytes.size() - at);
    T result;
    std::memcpy(&result, bytes.data() + at, sizeof(T));
    return result;
}
std::vector<uint8_t> Range(const std::vector<uint8_t>& elf, uint32_t address, size_t size) {
    const auto start = Load<uint32_t>(elf, 28);
    const auto count = Load<uint16_t>(elf, 44), stride = Load<uint16_t>(elf, 42);
    for (size_t i = 0; i < count; ++i) {
        const size_t at = start + i * stride;
        if (Load<uint32_t>(elf, at) != 1) continue;
        const auto base = Load<uint32_t>(elf, at + 8), length = Load<uint32_t>(elf, at + 16);
        if (address < base || size > length || address - base > length - size) continue;
        const size_t offset = Load<uint32_t>(elf, at + 4) + address - base;
        Require(offset <= elf.size() && size <= elf.size() - offset);
        return {elf.begin() + offset, elf.begin() + offset + size};
    }
    throw std::runtime_error("cloth original ELF range absent");
}
}
ClothOracle::ClothOracle(const std::vector<uint8_t>& elf) {
    constexpr std::array<uint64_t, 4> hashes{
        0x5867e657eb012cd1ull, 0xbd62bf1c5a21ea90ull, 0xe881406e5b395512ull, 0x8d359cfb80d4dc34ull};
    for (const auto& record : std::array<std::array<uint32_t, 3>, 4>{{
             {0x2dad60, 18, 0}, {0x2d9ce0, 4, 0x90}, {0x2e40b0, 114, 0xb0}, {0x2e4440, 222, 0x440}}}) {
        auto bytes = Range(elf, record[0], record[1] * 8);
        Require(ps2r::hash::Fnv1a64(bytes.data(), bytes.size()) == hashes[common_.size()]);
        common_.push_back({record[2], std::move(bytes)});
    }
    cloth_[0] = Range(elf, 0x2dd400, 248 * 8);
    cloth_[1] = Range(elf, 0x2dcc30, 250 * 8);
    Require(ps2r::hash::Fnv1a64(cloth_[0].data(), cloth_[0].size()) == 0x3d98d1e8522c4a7bull);
    Require(ps2r::hash::Fnv1a64(cloth_[1].data(), cloth_[1].size()) == 0x70c451b3ea9b5b26ull);
}
std::vector<ps2r::gs::DrawIr> ClothOracle::Run(const std::vector<uint8_t>& asset,
    const title::ClothGeometry::Waves& waves, const std::vector<title::MeshGeometry::View>& views,
    const ps2r::gs::DrawIr& material) const {
    Require(!views.empty() && views.size() <= 20);
    const bool three = waves.kind == title::ClothGeometry::Kind::Three;
    auto state = std::make_unique<ps2r::gs::GsState>();
    ps2r::gs::CaptureSink sink;
    state->SetSink(&sink);
    state->WriteRegister(0x18, uint64_t(uint32_t(material.off_x * 16)) |
                                  uint64_t(uint32_t(material.off_y * 16)) << 32);
    state->WriteRegister(0x1a, 0);
    state->WriteRegister(0x1b, 16);
    GsStateSink gs_sink(*state);
    GifRegs registers;
    GifParser parser(gs_sink, registers);
    GifArbiter arbiter(parser);
    VuUnit vu0(0), vu1(1, &arbiter);
    Vif vif(vu0, vu1, arbiter, registers);
    vu1.dispatch().settings().vu1_mode = VuTierMode::Interp;
    for (const auto& program : common_)
        vu1.WriteMicroMem(program.address, program.bytes.data(), program.bytes.size());
    vu1.WriteMicroMem(0x1720, cloth_[three].data(), cloth_[three].size());
    const auto word = [&](uint32_t at, std::array<uint32_t, 4> value) {
        vu1.WriteDataMem(at * 16, reinterpret_cast<const uint8_t*>(value.data()), 16);
    };
    word(1, {600, 800, 600, 800});
    word(2, {120, 0, 0, 0});
    word(3, {96, 0, 0, 0});
    word(4, {0, 0xb0 / 8, 0, 0});
    word(5, {0, 0x440 / 8, 0, 0});
    word(6, {0x8000, 0x3002c000, 0x512, 0});
    const uint32_t next = three ? 116 : 115;
    word(96, {0x1720 / 8, 0x1720 / 8 + (three ? 154u : 153u), next, 0});
    word(97, {128, 128, 128, 128});
    word(98, {Bits(views[0].scroll[0]), Bits(views[0].scroll[1]), 0, Bits(three ? 0 : waves.amplitude[0])});
    if (three) word(99, {Bits(waves.amplitude[0]), Bits(waves.amplitude[1]), Bits(waves.amplitude[2]), 0});
    for (unsigned i = 0; i < 16; ++i)
        word((three ? 100 : 99) + i, {Bits(waves.rows[i][0]), Bits(waves.rows[i][1]), Bits(waves.rows[i][2]), 0});
    word(next, {0x90 / 8, 0, next, 0});
    uint32_t at = 120;
    for (const auto& view : views) {
        Require(view.scroll == views[0].scroll);
        word(at, {view.clipped ? 0x8000u : 0u, uint32_t(120 + views.size() * 10), 5, 10});
        vu1.WriteDataMem((at + 1) * 16, reinterpret_cast<const uint8_t*>(view.screen.data()), 64);
        vu1.WriteDataMem((at + 5) * 16, reinterpret_cast<const uint8_t*>(view.clip.data()), 64);
        vu1.WriteDataMem((at + 9) * 16, reinterpret_cast<const uint8_t*>(view.guard.data()), 16);
        at += 10;
    }
    vif.regs(1).base = vif.regs(1).tops = 384;
    vif.regs(1).ofst = 0;
    const size_t length = size_t(Load<uint16_t>(asset, 0)) * 16;
    Require(asset.size() >= 16 && length <= asset.size() - 16);
    VuChopRoundGuard rounding;
    vif.ProcessData(1, asset.data() + 16, length);
    Require(vif.IsIdle(1));
    arbiter.drain();
    state->KickBoundary();
    return sink.draws;
}
