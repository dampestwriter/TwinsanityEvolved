#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_pass_fixture.h"

#include <cstring>
#include <stdexcept>

namespace {
using A = title::ShadowAssets;
std::vector<uint8_t> Part() {
    const std::array<uint32_t, 52> words{
        0x6000000c, 0, 0x070000fa, 0, 0x6c018000, 0x8003, 0x30024000, 0x512, 0,
        0x01000101, 0x64018001, 12, 0x8003, 0x01000104, 0x68038003,
        0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0,
        0x6c038004, 0, 0, 0, 0x8000, 0, 0, 0, 0x8000, 0, 0, 0, 0,
        0x68038005, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0x14000000, 0x01000101, 0, 0, 0};
    std::vector<uint8_t> bytes(sizeof(words));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}
}
bool ShadowPassFixture::Read(uint32_t at, void* out, size_t size) const {
    // The title's main-thread stack: the 16 KB scratchpad window, served like
    // the root's AddressMap serves it (the matrix arguments live there).
    if ((at & 0x70000000u) == 0x70000000u) {
        const uint32_t offset = at & 0x3fff;
        if (offset > scratchpad_.size() || size > scratchpad_.size() - offset) return false;
        std::memcpy(out, scratchpad_.data() + offset, size);
        return true;
    }
    if (at > memory_.size() || size > memory_.size() - at) return false;
    // No source disk allocation or authored packet is present in this fixture.
    if (at >= 0x31b138 && at < 0x321000) throw std::runtime_error("draw consulted disk owner");
    std::memcpy(out, memory_.data() + at, size);
    return true;
}
bool ShadowPassFixture::Write(const std::vector<Source::Write>& writes) {
    if (!write_ok) return false;
    for (const auto& write : writes)
        if (write.address > memory_.size() || write.bytes.size() > memory_.size() - write.address)
            return false;
    for (const auto& write : writes)
        std::memcpy(memory_.data() + write.address, write.bytes.data(), write.bytes.size());
    return true;
}
void ShadowPassFixture::Store(uint32_t at, uint32_t value) {
    if (at > memory_.size() || 4 > memory_.size() - at) throw std::runtime_error("fixture bounds");
    std::memcpy(memory_.data() + at, &value, 4);
}
void ShadowPassFixture::StoreScratchpad(uint32_t offset, const void* bytes, size_t size) {
    if (offset > scratchpad_.size() || size > scratchpad_.size() - offset) throw std::runtime_error("fixture scratchpad");
    std::memcpy(scratchpad_.data() + offset, bytes, size);
}
ShadowPassFixture::ShadowPassFixture(bool enabled, bool install_pass) : memory_(0x400000), scratchpad_(0x4000) {
    A::Snapshot snapshot;
    snapshot.next_generation = 2;
    snapshot.assets.push_back({{0x1000, 1}, 0xd8d37a1c, 3, Part()});
    if (assets.Restore(snapshot) != A::Result::Ok) throw std::runtime_error("fixture asset");
    Store(0x1004, 0xd8d37a1c); Store(0x1008, 1);
    Store(0x100c, 0x3000); Store(0x1010, 0x3010);
    Store(0x3000, 3); Store(0x3010, uint32_t(snapshot.assets[0].bytes.size()));
    for (uint32_t object : {0x1100u, 0x1120u}) {
        Store(object + 8, 0x2100); Store(object + 12, 0x1000);
        // InstanceBlockOwner: no block, needsEye 1 (the type-0B shader's answer:
        // eye-sized source instances), billboard 0.
        Store(object + 16, 0); Store(object + 20, 1);
        Store(object + 24, 1); Store(object + 28, 0xd8d37a1c);
    }
    Store(0x2100, 0x2200); Store(0x2208, 0x6000);
    Store(0x6000, 0x7000); Store(0x6010, 1); Store(0x6018, 32); Store(0x6020, 21);
    Store(0x7064, 11); Store(0x706c, Source::kShaderVtable);
    Store(A::kMeshes, 0x1100); Store(A::kMeshes + 4, 0x1120);
    Store(Source::kView, 0x8000); Store(Source::kTarget, 0xa000);
    Store(0xa00c, frame.width); Store(0xa010, frame.height);
    Store(0xa018, frame.width); Store(0xa01c, frame.height);
    const Matrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    std::memcpy(memory_.data() + 0x8080, identity.data(), sizeof(identity));
    std::memcpy(memory_.data() + 0x80c0, identity.data(), sizeof(identity));
    const std::array<float, 4> guard{1, 1, 1, 1};
    std::memcpy(memory_.data() + 0x8140, guard.data(), sizeof(guard));
    for (uint32_t boundary : {Source::kBegin, Source::kEnd, Source::kMesh, Pass::kClose,
                              Pass::kForget, A::kRead, A::kDestroy})
        dispatch_[boundary] = [this, boundary](CpuContext&, uint32_t) { ++original_calls[boundary]; };
    if (!install_pass) return;
    pass = std::make_unique<Pass>(assets, Hooks(enabled));
    if (!pass->Install([this](uint32_t at) { return Capture(at); },
                      [this](uint32_t at, Pass::Body body) { return Install(at, std::move(body)); }))
        throw std::runtime_error("fixture source install");
}
title::MaterialState::Read ShadowPassFixture::Reader() const {
    return [this](uint32_t at, void* out, size_t size) { return Read(at, out, size); };
}
ShadowPassFixture::Pass::Body ShadowPassFixture::Capture(uint32_t at) { return dispatch_.at(at); }
bool ShadowPassFixture::Install(uint32_t at, Pass::Body body) {
    dispatch_[at] = std::move(body);
    return true;
}
ShadowPassFixture::Pass::Hooks ShadowPassFixture::Hooks(bool enabled) {
    Pass::Hooks hooks;
    hooks.enabled = enabled;
    hooks.read = [this](uint32_t at, void* out, size_t size) { return Read(at, out, size); };
    hooks.write = [this](const auto& writes) { return Write(writes); };
    hooks.frame = [this](Pass::Frame& out) {
        if (!frame_ok) return false;
        out = frame; return true;
    };
    hooks.reserve = [this](auto batch) {
        if (!reserve_ok) return false;
        batches.push_back(std::move(batch)); return true;
    };
    hooks.refuse = [this](title::GraphicsRefusals::Reason reason, uint32_t producer, uint32_t detail) {
        refusals.push_back({reason, producer, detail});
    };
    return hooks;
}
void ShadowPassFixture::Run(uint32_t boundary) { dispatch_.at(boundary)(cpu_, 0); }
void ShadowPassFixture::Mesh(uint32_t object, unsigned tag, bool stack) {
    Matrix screen, camera;
    if (!Read(Source::kShadowScreen, &screen, sizeof(screen)) ||
        !Read(Source::kShadowCamera, &camera, sizeof(camera))) throw std::runtime_error("fixture screen");
    screen[3][0] += float(tag * 16);
    cpu_.cpuRegs.GPR.r[4].UL[0] = object;
    if (stack) {
        // DrawShadowEntry's locals on the title's scratchpad stack.
        StoreScratchpad(0x3e00, screen.data(), sizeof(screen));
        StoreScratchpad(0x3e40, camera.data(), sizeof(camera));
        cpu_.cpuRegs.GPR.r[5].UL[0] = 0x70003e00;
        cpu_.cpuRegs.GPR.r[6].UL[0] = 0x70003e40;
    } else {
        std::memcpy(memory_.data() + 0x9000, screen.data(), sizeof(screen));
        cpu_.cpuRegs.GPR.r[5].UL[0] = 0x9000;
        cpu_.cpuRegs.GPR.r[6].UL[0] = Source::kShadowCamera;
    }
    Run(Source::kMesh);
}
ShadowPassFixture::Matrix ShadowPassFixture::ViewScreen() const {
    Matrix matrix;
    if (!Read(0x80c0, &matrix, sizeof(matrix))) throw std::runtime_error("fixture saved screen");
    return matrix;
}
