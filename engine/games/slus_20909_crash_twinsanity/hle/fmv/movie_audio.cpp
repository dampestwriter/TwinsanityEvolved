#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"

#include <algorithm>

namespace title {
bool MovieAudio::SetGain(uint16_t gain) {
    if (gain > 32767) return false;
    gain_ = gain;
    ++count_.gain_changes;
    if (gain) ++count_.nonzero_changes;
    count_.peak = std::max(count_.peak, gain);
    return true;
}

std::array<float, 2> MovieAudio::Gains() const {
    // NTSC SetSoundVolume quantizes the source float to 0..32767 first.
    // Preserve the retained common mixer's linear gain arithmetic exactly.
    const float gain = static_cast<float>(gain_) / 32767.0f;
    return {gain, gain};
}

int MovieAudio::Pop(int16_t* stereo, int frames) {
    ++count_.pops;
    const int popped = input_.Pop(stereo, frames);
    count_.frames += static_cast<uint64_t>(popped);
    if (gain_) count_.audible_frames += static_cast<uint64_t>(popped);
    return popped;
}

bool MovieAudio::CanCheckpoint(std::string* error) const {
    if (!input_.Queued() && !input_.GateIsOpen()) return true;
    if (error) *error = "Movie PCM still requires source Close";
    return false;
}

bool MovieAudio::RestoreClosed(uint16_t gain, std::string* error) {
    if (gain > 32767) {
        if (error) *error = "Movie PCM gain is outside the source range";
        return false;
    }
    if (!CanCheckpoint(error)) return false;
    gain_ = gain;
    return true;
}
}  // namespace title
