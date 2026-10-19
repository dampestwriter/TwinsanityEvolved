#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_pass_fixture.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
std::vector<uint8_t> ram(0x400000);
constexpr uint64_t Settings = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 | uint64_t{1} << 56 | uint64_t{1} << 59;
}

uint8_t* DistortionPassFixture::Ram() { return ram.data(); }
bool DistortionPassFixture::Reader(uint32_t at, void* out, size_t size) {
    if (at > ram.size() || size > ram.size() - at) return false;
    std::memcpy(out, ram.data() + at, size); return true;
}
DistortionPassFixture::DistortionPassFixture(bool enabled) : blocks(Reader) {
    std::fill(ram.begin(), ram.end(), 0);
    Write<uint32_t>(P::kMaterial, Shader);
    Write<uint32_t>(Shader + 0x6c, title::DistortionMaterial::kVtable);
    Write<uint32_t>(Shader + 0x64, 0x18);
    Write<uint32_t>(title::MaterialState::kRenderTarget, Target);
    const std::array<uint32_t, 8> target{0, 0, 0, 512, 448, 0, 512, 448};
    Write(Target, target);
    Write<uint32_t>(title::MaterialState::kDisplayWidth, 512);
    Write<uint32_t>(title::MaterialState::kDisplayHeight, 448);
    Write<uint32_t>(title::MaterialState::kDisplayPixelBytes, 2);
    Write<uint32_t>(title::MaterialState::kDepthBufferPage, 168);
    Write<float>(Table + 0x14, source.look.gravity);
    for (unsigned corner = 0; corner < 6; ++corner) {
        Write<float>(Table + P::kTableCorners + corner * 8, source.look.corners[corner][0]);
        Write<float>(Table + P::kTableCorners + corner * 8 + 4, source.look.corners[corner][1]);
    }
    for (unsigned step = 0; step < 64; ++step)
        for (unsigned value = 0; value < 6; ++value)
            Write<float>(Table + P::kTableSteps + step * 32 + value * 4, source.look.shapes[step][value]);
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            Write<float>(Matrix + (row * 4 + column) * 4, source.view.screen[row][column]);
    // Three owned blocks: a two-block chain and a detached single block.
    title::DistortionBlocks::Snapshot snapshot; snapshot.generation = 1; snapshot.pool = 0x210000;
    snapshot.next = {{Block0, Block1}, {Block1, 0}, {Block2, 0}};
    if (!blocks.Restore(snapshot)) throw std::runtime_error("distortion pass fixture blocks");
    for (uint32_t block : {Block0, Block1, Block2}) Write(block + 0x20, source.blocks[0]);
    hooks.enabled = enabled; hooks.read = Reader;
    hooks.settings = [this](uint32_t shader, title::DistortionMaterial::Settings& out) {
        if (shader != Shader || unversioned_settings) return false;
        out = {Settings, 0, 0}; return true; };
    hooks.environment = [this](title::MaterialState::Environment& out) {
        ++environments; out = source.environment; return true; };
    hooks.frame = [this](P::Frame& out) {
        ++frames; if (refuse_frame) return false;
        out = P::Frame{0x3300, 168, 512, 448, 7, 11}; return true; };
    hooks.reserve = [this](std::shared_ptr<const P::Batch> batch) {
        if (refuse_reserve) return false;
        reserved.push_back(std::move(batch)); return true; };
}
DistortionPassFixture::P DistortionPassFixture::Make() { return P(blocks, hooks); }
bool DistortionPassFixture::Install(P& pass) {
    return pass.Install([this](uint32_t) { return P::Body([this](CpuContext&, uint32_t) { ++originals; }); },
                        [](uint32_t, P::Body) { return true; });
}
CpuContext DistortionPassFixture::Context(uint32_t block, float dx, float dy, uint32_t material) {
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = block;
    context.cpuRegs.GPR.r[5].UL[0] = Table;
    context.cpuRegs.GPR.r[6].UL[0] = Matrix;
    context.cpuRegs.GPR.r[7].UL[0] = material;
    context.fpuRegs.fpr[12].f = 2;
    context.fpuRegs.fpr[13].f = dx;
    context.fpuRegs.fpr[14].f = dy;
    return context;
}
