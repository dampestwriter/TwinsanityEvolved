#pragma once

#include "games/slus_20909_crash_twinsanity/hle/audio/audio_state.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/control_state.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_state.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/buffer_state.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

namespace title {

// One service snapshot joins request producers, consumers, assets and clocks.
// Movie's separate owner must supply its complete loan references at restore.
class ServiceState {
public:
    struct State {
        LoaderFileIo::State files;
        Audio::Snapshot audio;
        SoundControls::State controls;
        MusicLibrary::State music;
        MusicBuffers::State buffers;
    };
    static bool Capture(const Audio& audio, State& state, std::string* error = nullptr);
    static bool Validate(const Audio& audio, const State& state, std::string* error = nullptr);
    static bool Encode(const Audio& audio, const State& state, std::vector<uint8_t>& bytes,
                       std::string* error = nullptr);
    static bool Decode(const Audio& audio, const std::vector<uint8_t>& bytes, State& state,
                       std::string* error = nullptr);
    static bool ValidateMovie(const State& state, const std::vector<StreamBuffers::Loan>& references,
                              std::string* error = nullptr);
    // Saved game RAM and DomainTick must already be installed. Existing Movie
    // views must have returned; returned renewed loans are reattached by M2.
    static bool Restore(Audio& audio, const State& state,
                        const std::vector<StreamBuffers::Loan>& references,
                        StreamBuffers::RenewedLoans& renewed, std::string* error = nullptr);
};
} // namespace title
