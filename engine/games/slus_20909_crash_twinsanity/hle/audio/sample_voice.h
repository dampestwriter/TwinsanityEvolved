#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/sound/vag_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/volume_ramp.h"

namespace title {

// Playback arithmetic over one immutable sample asset. There is no SPU RAM,
// register window, address mask, libsd command queue or guest pointer here.
class SampleVoice {
public:
    using Samples = std::shared_ptr<const std::vector<uint8_t>>;
    enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
    struct Parameters {
        uint16_t pitch = 0x1000;
        uint16_t left = 0x3fff, right = 0x3fff;
        uint8_t attack = 0, decay = 0, sustain = 15, release = 0;
    };
    struct State {
        Samples samples;
        Parameters parameters;
        Phase phase = Phase::Off;
        uint32_t block_offset = 0, loop_offset = 0, fraction = 0, sample_index = 0;
        VagDecoder::History history;
        std::array<int16_t, VagDecoder::kBlockSamples> block{};
        uint8_t flags = 0;
        int16_t previous = 0;
        float envelope = 0, left = 0, right = 0;
        VolumeRamp left_ramp, right_ramp;
        uint64_t ends = 0;
    };

    bool Start(Samples samples, Parameters parameters);
    void SetPitch(uint16_t pitch);
    void SetVolumes(uint16_t left, uint16_t right);
    void RampVolumes(uint16_t left, uint16_t right);
    void AdvanceControls();
    void SetRelease(uint8_t rate);
    void Release(uint8_t rate);
    void Stop();
    void Step(float& left, float& right);
    // Muted pause keeps the sample cursor fixed while release/attack still
    // follows the output-sample clock. Resume restores the requested gains.
    void StepPaused();
    bool Playing() const;
    uint64_t Ends() const;
    const State& Capture() const;
    bool Restore(const State& state);

private:
    static bool Valid(const State& state);
    static float Volume(uint16_t value);
    void Decode();
    void Envelope();
    State state_;
};

} // namespace title
