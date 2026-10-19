// OLD SOUND_P5: first-picture gate, capacity/drop-newest, muted consumption,
// skip flush and voice -> Movie -> master/clamp order. Both arms use the actual
// production common mixer; the control programs/reads retained BVOL and input.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/movie_sound.h"
#include "engine/sound/audio_mixer.h"
#include "engine/sound/mixer.h"
#include "engine/sound/spu_core.h"
#include "tests/sound/mixer_voice_source.h"

namespace {
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
struct Trace {
    std::vector<float> samples;
    std::vector<uint64_t> cursor;
    uint64_t model_pops = 0, model_gain_sets = 0, owned_pops = 0, owned_frames = 0;
};
Trace Run(bool selected) {
    using namespace ps2r::sound;
    FmvPcmInput pcm(1024);
    SpuCore core;
    core.SdInit();
    core.SetMvolL(0, 0.5f); core.SetMvolR(0, 0.75f);
    core.SetMvolL(1, 0.25f); core.SetMvolR(1, 0.5f);
    MixerVoiceSource voices; // fixed voice inputs: ordering test, no voice-model claim
    Mixer mixer(core);
    mixer.SetVoiceSource(&voices);
    Trace trace;
    {
        ps2r::app::MovieSound sound(selected, pcm, core, mixer);
        std::vector<int16_t> input(1600 * 2);
        for (size_t i = 0; i < input.size(); ++i) input[i] = int16_t(int(i * 97 % 65536) - 32768);
        const auto pump = [&](int frames) {
            mixer.Pump(frames);
            trace.samples.insert(trace.samples.end(), mixer.Output(), mixer.Output() + mixer.OutputFrames() * 2);
            trace.cursor.insert(trace.cursor.end(), {pcm.Consumed(), pcm.Queued(), pcm.Dropped(), pcm.Flushes()});
        };
        for (const uint16_t gain : {uint16_t{0}, uint16_t{8191}, uint16_t{16383}, uint16_t{32767}})
            for (const float master : {0.0f, 0.5f, 2.0f}) {
                pcm.Flush();
                sound.SetGain(gain);
                core.SetMasterVolume(master);
                pcm.Push(input.data(), 1600); // overflow deliberately exceeds the real queue
                const auto before = pcm.Consumed();
                pump(129);
                Check(pcm.Consumed() == before && pcm.Queued() == 1024, "priming cannot consume before first picture");
                pcm.GateOpen();
                for (int frames : {0, 1, 127, 128, 129, 800}) pump(frames);
                Check(pcm.Consumed() == before + 1024 && !pcm.Queued(),
                      "partial chunks and mute consume the accepted FIFO exactly once");
                pcm.Push(input.data(), 128);
                pcm.Flush(); // abort, then another movie
                pump(128);
                Check(pcm.Consumed() == before + 1024, "skip cannot leak queued PCM into subsequent fields");
            }
        if (auto* owned = sound.StateOwner()) {
            trace.owned_pops = owned->Count().pops;
            trace.owned_frames = owned->Count().frames;
            Check(owned->CanCheckpoint(), "source flush closes the software PCM owner");
            const auto before = owned->Count().gain_changes;
            Check(owned->RestoreClosed(12345) && owned->Gain() == 12345 &&
                  owned->Count().gain_changes == before, "state reconstruction is not a newly executed gain intent");
            Check(!owned->SetGain(32768) && !owned->RestoreClosed(32768) && owned->Gain() == 12345,
                  "invalid gain cannot replace a valid software intent");
            bool failed = false;
            try { sound.SetGain(65535); } catch (const std::runtime_error&) { failed = true; }
            Check(failed && owned->Gain() == 12345, "root rejects invalid source gain without a fallback write");
        }
        trace.model_pops = core.FmvModelPops();
        trace.model_gain_sets = core.BvolSets();
        voices.silent = true;
        pcm.Push(input.data(), 1);
        pcm.GateOpen();
    }
    const auto consumed = pcm.Consumed();
    mixer.Pump(1);
    Check(pcm.Consumed() == consumed && !core.FmvInput() && mixer.Output()[0] == 0 && mixer.Output()[1] == 0,
          "root input detaches before its Movie owner dies");
    return trace;
}

// The Movie owner must compose with whichever common sink the parent merges
// (the current Mixer or a later Audio output owner). Exactly one consumer at a
// time, identical popped frames, detach before the owner dies, and no selected
// owner without a sink.
struct RecordingSink {
    ps2r::sound::PcmSource* attached = nullptr;
    unsigned binds = 0, unbinds = 0;
    std::vector<int16_t> popped;
    std::vector<float> gains;
    void Pull(int frames) {
        std::vector<int16_t> pcm(size_t(frames) * 2);
        const int n = attached ? attached->Pop(pcm.data(), frames) : 0;
        popped.insert(popped.end(), pcm.begin(), pcm.begin() + size_t(n) * 2);
        if (attached) { const auto g = attached->Gains(); gains.insert(gains.end(), g.begin(), g.end()); }
    }
};
unsigned SinkSeam() {
    using namespace ps2r::sound;
    std::vector<int16_t> input(600 * 2);
    for (size_t i = 0; i < input.size(); ++i) input[i] = int16_t(int(i * 131 % 65536) - 32768);
    auto run = [&](auto make_sink_owner, RecordingSink& sink) {
        FmvPcmInput pcm(1024);
        SpuCore core;
        auto owner = make_sink_owner(pcm, core, sink);
        Check(sink.binds == 1 && sink.attached && !core.FmvInput(), "selected owner attaches to exactly one sink");
        owner->SetGain(16383);
        pcm.Push(input.data(), 600);
        sink.Pull(300);
        Check(sink.popped.empty(), "alternate sink still respects the first-picture gate");
        pcm.GateOpen();
        sink.Pull(300); sink.Pull(300); sink.Pull(300);
        Check(sink.popped.size() == 1200 && owner->StateOwner()->Count().frames == 600,
              "alternate sink pops the accepted FIFO exactly once");
        owner.reset();
        Check(sink.unbinds == 1 && !sink.attached, "owner detaches from its sink before dying");
    };
    RecordingSink alternate;
    run([](FmvPcmInput& pcm, SpuCore& core, RecordingSink& sink) {
        return std::make_unique<ps2r::app::MovieSound>(true, pcm, core, ps2r::app::MovieSound::Sink{
            [&sink](PcmSource* source) {
                if (source) { ++sink.binds; sink.attached = source; }
                else { ++sink.unbinds; sink.attached = nullptr; }
            }});
    }, alternate);
    Check(std::equal(alternate.popped.begin(), alternate.popped.end(), input.begin()) &&
          std::all_of(alternate.gains.begin(), alternate.gains.end(),
                      [](float g) { return g == 16383.0f / 32767.0f; }),
          "alternate sink receives the source PCM bytes and the quantized source gain unchanged");
    {
        FmvPcmInput pcm(1024);
        SpuCore core;
        bool refused = false;
        try { ps2r::app::MovieSound none(true, pcm, core, ps2r::app::MovieSound::Sink{}); }
        catch (const std::invalid_argument&) { refused = true; }
        Check(refused && !core.FmvInput(), "a selected owner without a sink refuses instead of falling back");
        ps2r::app::MovieSound retained(false, pcm, core, ps2r::app::MovieSound::Sink{});
        Check(core.FmvInput() == &pcm, "the retained arm ignores the sink and keeps the model input");
    }
    return checks;
}

// The owned output (AudioMixer, hle.audio_mix) seats the Movie owner through
// AudioMixer::SetMovieSource -- the same PcmSource contract as the common
// Mixer's SetPcmSource, reached from the root by SoundOutput::BindMovie. Same
// input on both seats: byte-identical mixed output, one accepted gain intent
// visible as Gains() on the seat, a refused intent reaching no copy, the
// retained core never pumped, detach before the owner dies.
struct OwnedOutput {
    std::vector<float> samples;
    uint64_t sink_frames = 0, audible_frames = 0, model_pops = 0, core_mix_calls = 0;
};
OwnedOutput RunOwnedOutput(bool owned_output) {
    using namespace ps2r::sound;
    std::vector<int16_t> input(2400 * 2);
    for (size_t i = 0; i < input.size(); ++i) input[i] = int16_t(int(i * 113 % 65536) - 32768);
    FmvPcmInput pcm(4096);
    SpuCore core;
    core.SdInit();
    core.SetMasterVolume(1.0f);
    MixerVoiceSource voices;
    voices.silent = true;                 // the seam under test is the Movie PCM, not the voices
    AudioMixer output(voices);
    std::unique_ptr<Mixer> mixer = owned_output ? std::make_unique<Mixer>(output) : std::make_unique<Mixer>(core);
    if (!owned_output) mixer->SetVoiceSource(&voices);
    OwnedOutput trace;
    {
        ps2r::app::MovieSound sound(true, pcm, core, owned_output ?
            ps2r::app::MovieSound::Sink{[&output](PcmSource* source) { output.SetMovieSource(source); }} :
            ps2r::app::MovieSound::Sink{[&m = *mixer](PcmSource* source) { m.SetPcmSource(source); }});
        Check(!core.FmvInput() && (!owned_output || output.MovieSource() == sound.StateOwner()),
              "the owned output seats the Movie owner itself: no FIFO copy, no second consumer");
        const auto pump = [&](int frames) {
            mixer->Pump(frames);
            trace.samples.insert(trace.samples.end(), mixer->Output(), mixer->Output() + mixer->OutputFrames() * 2);
        };
        for (const uint16_t gain : {uint16_t{0}, uint16_t{16383}, uint16_t{32767}}) {
            pcm.Flush();
            sound.SetGain(gain);
            if (owned_output)
                Check(output.MovieSource()->Gains() == std::array<float, 2>{gain / 32767.0f, gain / 32767.0f},
                      "one accepted gain intent is read from the owner by the owned output, never copied");
            pcm.Push(input.data(), 2400);
            pump(128);
            Check(pcm.Queued() == 2400, "the owned output respects the first-picture gate");
            pcm.GateOpen();
            for (int frames : {800, 800, 800, 128}) pump(frames);
            Check(!pcm.Queued(), "the owned output consumes the accepted FIFO exactly once");
        }
        bool refused = false;
        try { sound.SetGain(65535); } catch (const std::runtime_error&) { refused = true; }
        Check(refused && sound.StateOwner()->Gain() == 32767 &&
              (!owned_output || output.MovieSource()->Gains()[0] == 1.0f),
              "a refused gain intent reaches no seat");
        trace.sink_frames = owned_output ? output.Count().movie_frames : core.FmvFramesMixed();
        trace.audible_frames = owned_output ? output.Count().audible_movie_frames : core.FmvFramesMixedAudible();
        Check(sound.StateOwner()->Count().frames == 7200 && trace.sink_frames == 7200 && trace.audible_frames == 4800,
              "the seat's frame counts are the owner's popped frames");
    }
    trace.model_pops = core.FmvModelPops();
    trace.core_mix_calls = core.MixCalls();
    Check(!owned_output || output.MovieSource() == nullptr, "the owner detaches from the owned output before dying");
    pcm.Push(input.data(), 1);
    pcm.GateOpen();
    mixer->Pump(1);
    Check(pcm.Queued() == 1, "nothing pops the FIFO after the owner detached");
    return trace;
}
void OwnedOutputSeam() {
    const auto common = RunOwnedOutput(false), owned = RunOwnedOutput(true);
    Check(common.samples == owned.samples,
          "the owned output mixes the Movie owner byte-identically to the common Mixer on the same input");
    Check(std::any_of(owned.samples.begin(), owned.samples.end(), [](float v) { return v != 0; }),
          "owned-output comparison is audible");
    Check(!common.model_pops && !owned.model_pops && common.core_mix_calls > 0 && owned.core_mix_calls == 0,
          "the retained core is never pumped for Movie PCM; under the owned output it is not pumped at all");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::string(argv[1]) != "--both-paths") return 2;
    const auto model = Run(false), selected = Run(true);
    Check(model.samples == selected.samples && model.cursor == selected.cursor,
          "all mixed PCM bytes and FIFO/cut/gate observations match actual retained output");
    Check(std::any_of(model.samples.begin(), model.samples.end(), [](float v) { return v != 0; }),
          "mixed-output comparison is audible");
    Check(std::any_of(model.samples.begin(), model.samples.end(), [](float v) { return v == 1 || v == -1; }),
          "comparison exercises the common final clamp");
    Check(model.model_pops > 0 && model.model_gain_sets == 24 && !selected.model_pops &&
          !selected.model_gain_sets && selected.owned_pops > 0 && selected.owned_frames == 12288,
          "actual model positive controls and zero selected calls accompany positive owned PCM");
    SinkSeam();
    OwnedOutputSeam();
    std::printf("MovieAudio: %u checks, %zu stereo output frames, 12288 Movie frames; actual retained PCM/gain versus owned; alternate sink and owned-output (AudioMixer::SetMovieSource) seams.\n",
                checks, selected.samples.size() / 2);
}
