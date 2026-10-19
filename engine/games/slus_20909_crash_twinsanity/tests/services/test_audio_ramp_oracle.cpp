#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "engine/iop/iop_memory.h"
#include "engine/iop/iop_module_table.h"
#include "engine/sound/spu_core.h"
#include "engine/platform/host/platform.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"

using namespace ps2r::iop;
using namespace ps2r::title::stream_irx;

int main(int argc, char** argv) {
    if (argc != 2) return 77;
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) return 77;
    const std::vector<uint8_t> irx((std::istreambuf_iterator<char>(in)), {});
    ps2r::platform::host::RegisterSelf();
    IopModuleTable table(IopClock::Policy::Frame);
    IopMemory memory;
    ps2r::sound::SpuCore spu;
    StreamIrxPlugin plugin(table, memory, spu, nullptr, nullptr);
    if (!plugin.Boot(irx.data(), irx.size()).entry_ran) return 1;
    auto call = [](uint32_t address, uint32_t a, uint32_t b, uint32_t c) {
        auto ctx = IopContext::ForEntry(0x180000, ModuleLayout::kGp);
        ctx.gpr[4] = a; ctx.gpr[5] = b; ctx.gpr[6] = c;
        StreamIrxPlugin::Resolve(address)(ctx);
    };
    spu.ApplyQueued();
    spu.SetMasterVolume(1);
    for (int i = 0; i < 2; ++i) { spu.SetMvolL(i, 1); spu.SetMvolR(i, 1); }
    // Two cores, distinct predictors/pitches. Loop start is decoded at key-on.
    std::array<title::SampleVoice, 2> owned;
    constexpr std::array<uint32_t, 2> channels{0, 25};
    for (size_t i = 0; i < channels.size(); ++i) {
        std::vector<uint8_t> bytes(64, 0x71);
        for (size_t block = 0; block < 4; ++block) {
            bytes[block * 16] = static_cast<uint8_t>(0x20 + block);
            bytes[block * 16 + 1] = block == 0 ? 4 : block == 3 ? 3 : 0;
        }
        const uint32_t at = 0x5000 + static_cast<uint32_t>(i) * 64;
        std::memcpy(spu.SpuRam() + at, bytes.data(), bytes.size());
        auto& model = spu.VoiceAt(channels[i]);
        const auto pitch = static_cast<uint16_t>(i ? 0xaaa : 0x1000);
        model.SetPitch(pitch); model.SetAdsr1(15); model.SetAdsr2(0x1fc0);
        model.SetVolL(0); model.SetVolR(0x3fff);
        model.SetSsa(at); model.SetLsax(at); model.KeyOn(spu.SpuRam());
        // The fixture keys directly, so publish the same module channel
        // state that its play command normally sets for the outer ramp loop.
        memory.Write<uint8_t>(0x12f60 + channels[i], 1);
        title::SampleVoice::Parameters params;
        params.pitch = pitch; params.left = 0; params.right = 0x3fff;
        if (!owned[i].Start(std::make_shared<const std::vector<uint8_t>>(bytes), params)) return 1;
    }
    spu.RepublishReadbacks();
    uint64_t frames = 0, different = 0;
    // Ramp bands, downward ramps, interrupted/reversed ramps, exact snap,
    // and idle pulses all use the actual NEW IOP target/control bodies.
    constexpr std::array<uint16_t, 12> targets{
        0x3fff, 0, 0x1200, 0x0800, 0x0a00, 0x0a01, 0x0a02, 0x3fff, 0x0123, 0, 0x2000, 0x3fff};
    for (size_t change = 0; change < targets.size(); ++change) {
        for (size_t i = 0; i < owned.size(); ++i) {
            const uint16_t left = targets[change], right = targets[targets.size() - change - 1];
            call(0x10b7c, channels[i], left, right);
            owned[i].RampVolumes(left, right);
        }
        const unsigned pulses = change % 3 == 0 ? 2 : 40;
        for (unsigned pulse = 0; pulse < pulses; ++pulse) {
            call(0x10ab8, 0, 0, 0);
            for (auto& voice : owned) voice.AdvanceControls();
            std::array<float, 1600> model{};
            spu.MixInto(model.data(), 800);
            for (size_t frame = 0; frame < 800; ++frame) {
                float left = 0, right = 0;
                for (auto& voice : owned) {
                    float l = 0, r = 0;
                    voice.Step(l, r);
                    left += l; right += r;
                }
                left = std::clamp(left, -1.0f, 1.0f);
                right = std::clamp(right, -1.0f, 1.0f);
                if (left != model[frame * 2] || right != model[frame * 2 + 1]) {
                    if (different++ < 3)
                        std::printf("ramp mismatch change=%zu pulse=%u frame=%zu model=%a,%a owned=%a,%a\n",
                                    change, pulse, frame, model[frame*2], model[frame*2+1], left, right);
                }
                ++frames;
            }
        }
    }
    std::printf("AudioRampSelfOracle: %llu stereo frames, %llu mismatches (actual IOP 10b7c/10ab8/10d28)\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(different));
    return different ? 1 : 0;
}
