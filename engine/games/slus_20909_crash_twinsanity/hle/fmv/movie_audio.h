#pragma once

#include <string>

#include "engine/sound/fmv_pcm_input.h"
#include "engine/sound/pcm_source.h"

namespace title {
// The source player's stereo gain and existing decoder PCM queue. No second
// FIFO, block-transfer timer, register shadow or device clock is introduced.
class MovieAudio final : public ps2r::sound::PcmSource {
public:
    explicit MovieAudio(ps2r::sound::FmvPcmInput& input) : input_(input) {}
    bool SetGain(uint16_t gain);
    uint16_t Gain() const { return gain_; }
    std::array<float, 2> Gains() const override;
    int Pop(int16_t* stereo, int frames) override;
    bool CanCheckpoint(std::string* error = nullptr) const;
    bool RestoreClosed(uint16_t gain, std::string* error = nullptr);
    struct Counters {
        uint64_t gain_changes = 0, nonzero_changes = 0, pops = 0, frames = 0, audible_frames = 0;
        uint16_t peak = 0;
    };
    const Counters& Count() const { return count_; }

private:
    ps2r::sound::FmvPcmInput& input_;
    uint16_t gain_ = 0;
    Counters count_;
};
}  // namespace title
