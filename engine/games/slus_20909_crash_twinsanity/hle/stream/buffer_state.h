#pragma once

#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"

namespace title {
class BufferState {
public:
    static bool Encode(const MusicBuffers::State& state, std::vector<uint8_t>& bytes,
                       std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, MusicBuffers::State& state,
                       std::string* error = nullptr);
    // Join the Movie section's successful Prepare/OpenStream provider tokens
    // to this section before teardown. Missing, duplicated or forged loans
    // fail; a Movie without a PCM provider lists only its disc token.
    static bool ValidateMovie(const MusicBuffers::State& state,
                              const std::vector<StreamBuffers::Loan>& loans,
                              std::string* error = nullptr);
};
} // namespace title
