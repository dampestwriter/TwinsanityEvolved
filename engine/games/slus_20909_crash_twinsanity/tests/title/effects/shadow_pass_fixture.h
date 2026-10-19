#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_pass.h"

class ShadowPassFixture {
public:
    using Pass = title::ShadowPass;
    using Source = title::ShadowSource;
    using Matrix = title::ShadowGeometry::Matrix;
    explicit ShadowPassFixture(bool enabled = true, bool install_pass = true);
    void Run(uint32_t boundary);
    // stack: the matrix arguments are DrawShadowEntry's locals on the title's
    // scratchpad stack (0x7000xxxx), as the source passes them.
    void Mesh(uint32_t object, unsigned tag, bool stack = false);
    void Store(uint32_t address, uint32_t value);
    void StoreScratchpad(uint32_t offset, const void* bytes, size_t size);
    Matrix ViewScreen() const;
    Pass::Hooks Hooks(bool enabled);
    Pass::Body Capture(uint32_t);
    bool Install(uint32_t, Pass::Body);
    title::MaterialState::Read Reader() const;
    const std::vector<uint8_t>& Memory() const { return memory_; }
    title::ShadowAssets assets;
    std::unique_ptr<Pass> pass;
    std::vector<std::shared_ptr<const Pass::Batch>> batches;
    std::map<uint32_t, unsigned> original_calls;
    // Every refusal the pass attributed through Hooks::refuse, in order.
    struct Refusal { title::GraphicsRefusals::Reason reason; uint32_t producer, detail; };
    std::vector<Refusal> refusals;
    bool frame_ok = true, reserve_ok = true, write_ok = true;
    Pass::Frame frame{0x70000, 0x150000, 512, 448, 7, 8};
private:
    std::vector<uint8_t> memory_, scratchpad_;
    std::map<uint32_t, Pass::Body> dispatch_;
    CpuContext cpu_{};
    bool Read(uint32_t, void*, size_t) const;
    bool Write(const std::vector<Source::Write>&);
};
