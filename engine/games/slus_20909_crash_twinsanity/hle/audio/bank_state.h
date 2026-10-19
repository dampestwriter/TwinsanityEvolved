#pragma once

#include "games/slus_20909_crash_twinsanity/hle/audio/audio_state.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

namespace title {

// File/Audio component of a service checkpoint, including accepted and
// not-yet-sent bank loads. Root still owns RAM, clock, control batch, Music,
// Movie and output queue composition. This does not relax their guards.
class BankState {
public:
    struct State {
        LoaderFileIo::State files;
        Audio::Snapshot audio;
    };
    static bool Capture(const Audio& audio, State& state, std::string* error = nullptr);
    static bool Validate(const Audio& audio, const State& state, std::string* error = nullptr);
    static bool Encode(const Audio& audio, const State& state, std::vector<uint8_t>& bytes,
                       std::string* error = nullptr);
    static bool Decode(const Audio& audio, const std::vector<uint8_t>& bytes, State& state,
                       std::string* error = nullptr);
    // Game execution owner only, outside callbacks/mixing. Restore RAM and
    // DomainTick first. Old file cancellations finish BEFORE the renewed
    // Audio epoch is installed; no request is charged or delivered here.
    static bool Restore(Audio& audio, const State& state, std::string* error = nullptr);
};
} // namespace title
