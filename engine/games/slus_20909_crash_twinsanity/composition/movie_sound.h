#pragma once

#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"

namespace ps2r::sound { class SpuCore; class Mixer; }
namespace ps2r::verify { class RunMeta; }
namespace ps2r::app {
// Select one PCM input owner over the existing common mixer. The decoder
// retains the single FIFO and first-picture/Close gate.
class MovieSound {
public:
    // Attach or detach the owned PcmSource on exactly one sink. The sink pops
    // and scales; gain and PCM lifetime stay with the Movie owner (MOVI). The
    // root's sink is SoundOutput::BindMovie, which seats the owner either in
    // the common Mixer (SetPcmSource) or in the owned AudioMixer
    // (SetMovieSource); both read Gains() from the owner and store no copy.
    using Sink = std::function<void(sound::PcmSource*)>;
    MovieSound(bool selected, sound::FmvPcmInput&, sound::SpuCore&, sound::Mixer&);
    MovieSound(bool selected, sound::FmvPcmInput&, sound::SpuCore&, Sink sink);
    // One owner, one attach/detach: never copied or moved.
    MovieSound(const MovieSound&) = delete;
    MovieSound& operator=(const MovieSound&) = delete;
    ~MovieSound();
    void SetGain(uint16_t gain);
    ::title::MovieAudio* StateOwner() { return selected_ ? &audio_ : nullptr; }
    void Record(verify::RunMeta&) const;

private:
    bool selected_;
    sound::FmvPcmInput& input_;
    sound::SpuCore& retained_;
    Sink sink_;
    ::title::MovieAudio audio_;
};
}  // namespace ps2r::app
