#pragma once

#include "engine/sound/voice_source.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

// Both oracle arms use the production mixer. This provider only selects the
// owner of a channel's next sample; summation, gain and clamp stay in Mixer.
class SampleOutput final : public ps2r::sound::VoiceSource {
public:
    explicit SampleOutput(title::Audio& owner) : audio_(owner) {}
    void BeginMixTick() override { audio_.BeginMixTick(); }
    bool StepVoice(uint32_t channel, float& left, float& right) override {
        return audio_.StepVoice(channel, left, right);
    }

private:
    title::Audio& audio_;
};
