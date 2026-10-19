#include "games/slus_20909_crash_twinsanity/composition/movie_sound.h"

#include <sstream>
#include <stdexcept>

#include "engine/core/verify/run_meta.h"
#include "engine/sound/mixer.h"
#include "engine/sound/spu_core.h"

namespace ps2r::app {
namespace {
std::string Hex(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
}  // namespace

MovieSound::MovieSound(bool selected, sound::FmvPcmInput& input, sound::SpuCore& retained, sound::Mixer& mixer)
    : MovieSound(selected, input, retained,
                 Sink{[&mixer](sound::PcmSource* source) { mixer.SetPcmSource(source); }}) {}

MovieSound::MovieSound(bool selected, sound::FmvPcmInput& input, sound::SpuCore& retained, Sink sink)
    : selected_(selected), input_(input), retained_(retained), sink_(std::move(sink)), audio_(input) {
    if (selected_) {
        if (!sink_) throw std::invalid_argument("selected Movie PCM requires exactly one sink");
        sink_(&audio_);
    } else {
        retained_.SetFmvInput(&input_);
    }
}

MovieSound::~MovieSound() {
    if (selected_) sink_(nullptr);
    else retained_.SetFmvInput(nullptr);
}

void MovieSound::SetGain(uint16_t gain) {
    if (selected_) {
        if (!audio_.SetGain(gain)) throw std::runtime_error("Movie gain exceeds the source's 15-bit range");
    } else {
        retained_.SetParam(0xf80, gain);
        retained_.SetParam(0x1080, gain);
    }
}

void MovieSound::Record(verify::RunMeta& meta) const {
    const auto& owned = audio_.Count();
    meta.Set("movie_audio", "selected=" + std::to_string(selected_) +
        ",gain_changes=" + std::to_string(owned.gain_changes) +
        ",nonzero_changes=" + std::to_string(owned.nonzero_changes) +
        ",gain=" + std::to_string(audio_.Gain()) + ",peak=" + std::to_string(owned.peak) +
        ",pops=" + std::to_string(owned.pops) + ",frames=" + std::to_string(owned.frames) +
        ",audible_frames=" + std::to_string(owned.audible_frames) +
        ",model_pops=" + std::to_string(retained_.FmvModelPops()));
    // Keep old register programming counters honest. Selected audio publishes
    // its own gain evidence above instead of pretending it programmed BVOL.
    meta.Set("fmv_pcm", "pushed=" + std::to_string(input_.Pushed()) +
        ",pushed_nonzero=" + std::to_string(input_.PushedNonZero()) +
        ",mixed=" + std::to_string(retained_.FmvFramesMixed()) +
        ",dropped=" + std::to_string(input_.Dropped()) + ",queued=" + std::to_string(input_.Queued()) +
        ",gate=" + (input_.GateIsOpen() ? "open" : "closed") +
        ",gate_opens=" + std::to_string(input_.GateOpens()) + ",flushes=" + std::to_string(input_.Flushes()) +
        ",bvol=" + Hex(retained_.CoreAttr(0, sound::SpuCore::kBvoll)) + "/" +
            Hex(retained_.CoreAttr(0, sound::SpuCore::kBvolr)) +
        ",bvol_sets=" + std::to_string(retained_.BvolSets()) +
        ",bvol_nonzero_sets=" + std::to_string(retained_.BvolSetsNonZero()) +
        ",bvol_peak=" + Hex(retained_.BvolPeak()) +
        ",mixed_audible=" + std::to_string(retained_.FmvFramesMixedAudible()) + ",sink=mixer");
}
}  // namespace ps2r::app
