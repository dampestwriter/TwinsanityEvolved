#pragma once

#include <string>

#include "engine/sound/voice_source.h"
#include "engine/sound/audio_mixer.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"

namespace ps2r::verify { class RunMeta; }
namespace ps2r::sound { class Mixer; class PcmSource; class SpuCore; }

namespace ps2r::app {

// Banks, sample controls and mixing are one selection. Construct before game
// execution; destroy only after every game worker and delivery has stopped.
class SoundOutput final : public sound::VoiceSource {
public:
    SoundOutput(bool samples, bool music, bool output = false);
    ~SoundOutput();
    bool Install(title::SoundControls::RemainingVolume remaining_volume);
    bool AdvanceBoundary(uint64_t tick);
    void BindMixer(sound::Mixer& mixer);
    // The Movie PCM sink seam (one consumer). The Movie owner attaches its
    // PcmSource (gain + FIFO, persisted in MOVI) and detaches with nullptr;
    // the selected output pops it inside AudioMixer, otherwise the bound
    // common Mixer does. No Movie gain or PCM is stored here. BindMixer first.
    void BindMovie(sound::PcmSource* source);
    uint64_t MovieFrames(const sound::SpuCore& comparison) const;
    uint64_t AudibleMovieFrames(const sound::SpuCore& comparison) const;
    title::Audio* StateOwner() { return selected_ && installed_ ? &audio_ : nullptr; }
    sound::AudioMixer* OutputOwner() { return mix_ && installed_ ? &output_ : nullptr; }
    void Record(verify::RunMeta& meta, const sound::SpuCore& comparison) const;
    void BeginMixTick() override;
    bool StepVoice(uint32_t channel, float& left, float& right) override;

private:
    title::Audio audio_;
    sound::AudioMixer output_{*this};
    bool selected_ = false, music_ = false, attached_ = false, installed_ = false;
    bool mix_ = false;
    sound::Mixer* mixer_ = nullptr;
    uint64_t mix_ticks_ = 0, owned_samples_ = 0;
    // Owned channel samples split by owner (sample voice / music voice) and
    // whether the stepped PCM was non-zero: a keyed-but-silent sample play is
    // readable from run.meta (`sound_output`), not inferred from the mix hash.
    uint64_t sample_samples_ = 0, sample_nonzero_ = 0, music_samples_ = 0, music_nonzero_ = 0;
};

}  // namespace ps2r::app
