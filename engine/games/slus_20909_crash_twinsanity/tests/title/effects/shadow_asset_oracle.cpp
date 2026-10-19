#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_asset_oracle.h"

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
void Require(bool value) { if (!value) throw std::runtime_error("shadow original fixture contract"); }
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
    throw std::runtime_error("shadow original ELF range absent");
}
}
ShadowAssetOracle::ShadowAssetOracle(const std::vector<uint8_t>& elf) {
    // Independently verifies every original image used, including common
    // dispatch/clip bodies. Addresses are checked NTSC, not TR's PAL labels.
    constexpr std::array<uint64_t, 5> hashes{
        0x5867e657eb012cd1ull, 0xbd62bf1c5a21ea90ull, 0xe881406e5b395512ull,
        0x8d359cfb80d4dc34ull, 0x96040343da406cd4ull};
    for (const auto& record : std::array<std::array<uint32_t, 3>, 5>{{
             {0x2dad60, 18, 0}, {0x2d9ce0, 4, 0x90}, {0x2e40b0, 114, 0xb0},
             {0x2e4440, 222, 0x440}, {0x2e1710, 236, 0x1720}}}) {
        auto bytes = Range(elf, record[0], record[1] * 8);
        Require(ps2r::hash::Fnv1a64(bytes.data(), bytes.size()) == hashes[programs_.size()]);
        programs_.push_back({record[2], std::move(bytes)});
    }
}
std::vector<ps2r::gs::DrawIr> ShadowAssetOracle::Run(
    const std::vector<uint8_t>& asset, const std::vector<title::ShadowGeometry::View>& views, bool eye) const {
    Require(!views.empty() && views.size() <= 20);
    auto state = std::make_unique<ps2r::gs::GsState>();
    ps2r::gs::CaptureSink sink;
    state->SetSink(&sink);
    state->WriteRegister(0x18, uint64_t(uint32_t(views[0].offset[0] * 16)) |
                                  uint64_t(uint32_t(views[0].offset[1] * 16)) << 32);
    GsStateSink gs_sink(*state);
    GifRegs registers;
    GifParser parser(gs_sink, registers);
    GifArbiter arbiter(parser);
    VuUnit vu0(0), vu1(1, &arbiter);
    Vif vif(vu0, vu1, arbiter, registers);
    vu1.dispatch().settings().vu1_mode = VuTierMode::Interp;
    for (const auto& program : programs_)
        vu1.WriteMicroMem(program.address, program.bytes.data(), program.bytes.size());
    const auto word = [&](uint32_t address, std::array<uint32_t, 4> values) {
        vu1.WriteDataMem(address * 16, reinterpret_cast<const uint8_t*>(values.data()), 16);
    };
    word(1, {600, 800, 600, 800});
    word(2, {120, 0, 0, 0});
    word(3, {96, 0, 0, 0});
    word(4, {0, 0xb0 / 8, 0, 0});
    word(5, {0, 0x440 / 8, 0, 0});
    word(6, {0x8000, 0x3002c000, 0x512, 0});
    // Complete source two-node type-0B material, including prelude flags.
    word(96, {0x1720 / 8, 0x1ac0 / 8, 99, 1});
    word(97, {8, 8, 8, 128});
    word(98, {0, 0, 0, 0});
    word(99, {0x1720 / 8, 0x1ac0 / 8, 102, 1});
    word(100, {248, 248, 248, 128});
    word(101, {0, 0, 0, 1});
    word(102, {0x90 / 8, 0, 102, 0});
    uint32_t at = 120;
    const uint32_t stride = eye ? 11 : 10;
    for (const auto& view : views) {
        Require(view.offset == views[0].offset);
        word(at, {0x8000, uint32_t(120 + views.size() * stride), 5, stride});
        vu1.WriteDataMem((at + 1) * 16, reinterpret_cast<const uint8_t*>(view.screen.data()), 64);
        vu1.WriteDataMem((at + 5) * 16, reinterpret_cast<const uint8_t*>(view.clip.data()), 64);
        vu1.WriteDataMem((at + 9) * 16, reinterpret_cast<const uint8_t*>(view.guard.data()), 16);
        // The eye quadword (TR PlacedInstance::eye): poison proves the type-0B
        // program never reads it.
        if (eye) word(at + 10, {0x7fc00000, 0xff7fffff, 0x7f7fffff, 0xdeadbeef});
        at += stride;
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
