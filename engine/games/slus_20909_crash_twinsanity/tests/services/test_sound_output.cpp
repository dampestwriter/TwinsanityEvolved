#include <array>
#include <cstdio>
#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/composition/sound_output.h"
#include "engine/sound/fmv_pcm_input.h"
#include "engine/sound/mixer.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"

// Root Movie PCM composition, three arms on identical inputs:
//   retained  — the control: SpuCore pops the FIFO at its BVOLL/BVOLR shadows
//               (what MovieSound does with hle.movie_lifecycle=off);
//   common    — Movie's owner (title::MovieAudio) attached through
//               SoundOutput::BindMovie on an unselected output: the common Mixer
//               seats it and SpuCore::MixInto reads the owner's gain;
//   owned     — the same owner attached through BindMovie on the selected
//               output (hle.audio_mix): AudioMixer pops it, no core executes.
// Exactly one consumer holds the FIFO in each arm; the gain lives in the
// owner (MOVI), never in SoundOutput or AudioMixer (SVCS).
int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool condition, const char* label) {
        ++checks;
        if (!condition) { ++failures; std::printf("FAIL SoundOutput %s\n", label); }
    };
    ps2r::sound::SpuCore retained, common_core, unused;
    ps2r::app::SoundOutput common_output(false, false), owned_output(true, true, true);
    ps2r::sound::Mixer retained_mixer(retained), common_mixer(common_core), owned_mixer(unused);
    ps2r::sound::FmvPcmInput retained_pcm(4096), common_pcm(4096), owned_pcm(4096);
    title::MovieAudio common_owner(common_pcm), owned_owner(owned_pcm);
    bool unbound_refused = false;
    try { owned_output.BindMovie(&owned_owner); } catch (const std::logic_error&) { unbound_refused = true; }
    check(unbound_refused, "Movie sink requires the bound Mixer first");
    common_output.BindMixer(common_mixer); owned_output.BindMixer(owned_mixer);
    retained.SetFmvInput(&retained_pcm);
    common_output.BindMovie(&common_owner); owned_output.BindMovie(&owned_owner);
    check(common_core.FmvInput() == nullptr && unused.FmvInput() == nullptr && retained.FmvInput() == &retained_pcm,
          "owner arms never attach the FIFO to a core; exactly one consumer per arm");
    check(owned_output.OutputOwner() == nullptr, "output owner is published only after Install");
    std::array<int16_t, 1600> samples{};
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(int(i * 73 % 65536) - 32768);
    uint64_t owned_frames = 0;
    for (uint16_t gain : {uint16_t{0x7fff}, uint16_t{0}, uint16_t{0x3fff}}) {
        retained.SetParam(0xf80, gain); retained.SetParam(0x1080, gain);
        check(common_owner.SetGain(gain) && owned_owner.SetGain(gain), "owner takes the fixed gain");
        retained_pcm.Push(samples.data(), 800); common_pcm.Push(samples.data(), 800); owned_pcm.Push(samples.data(), 800);
        retained_pcm.GateOpen(); common_pcm.GateOpen(); owned_pcm.GateOpen();
        for (const int n : {127, 129, 544}) {
            retained_mixer.Pump(n); common_mixer.Pump(n); owned_mixer.Pump(n);
            for (int sample = 0; sample < n * 2; ++sample) {
                check(common_mixer.Output()[sample] == retained_mixer.Output()[sample], "common arm == retained arm");
                check(owned_mixer.Output()[sample] == retained_mixer.Output()[sample], "owned arm == retained arm");
            }
            check(retained_pcm.Consumed() == common_pcm.Consumed() && retained_pcm.Consumed() == owned_pcm.Consumed(),
                  "same PCM clock in all three arms, muted included");
        }
        owned_frames += 800;
        check(owned_output.MovieFrames(unused) == owned_frames && owned_owner.Count().frames == owned_frames,
              "root metadata counts the frames the owner popped");
        check(owned_output.AudibleMovieFrames(unused) == owned_owner.Count().audible_frames,
              "audible frames follow the owner's gain");
    }
    check(owned_owner.Count().audible_frames == 1600 && owned_output.AudibleMovieFrames(unused) == 1600,
          "two audible gains of three");
    check(unused.MixCalls() == 0 && unused.BvolSets() == 0 && common_core.BvolSets() == 0 && retained.MixCalls() == 9 &&
          common_core.MixCalls() == 9 && common_core.FmvModelPops() == 0,
          "selected arms perform no Movie register write; the owned arm never enters a core");
    check(owned_pcm.Consumed() == 2400 && owned_pcm.Queued() == 0, "muted owned output consumes real PCM");
    check(!owned_owner.SetGain(0x8000), "the owner refuses a non-fixed gain; the sink has no gain to refuse");
    // Detach: the owner leaves the seat, the backlog stays with the owner.
    owned_pcm.Push(samples.data(), 100); owned_pcm.GateOpen();
    owned_output.BindMovie(nullptr); common_output.BindMovie(nullptr);
    owned_mixer.Pump(128); common_mixer.Pump(128);
    check(owned_pcm.Queued() == 100 && owned_output.MovieFrames(unused) == 2400, "detached owner is not popped");
    std::printf("SoundOutput %d checks %d failures; Movie frames=%llu model_mix_calls=%llu\n",
        checks, failures, static_cast<unsigned long long>(owned_pcm.Consumed()),
        static_cast<unsigned long long>(unused.MixCalls()));
    return failures ? 1 : 0;
}
