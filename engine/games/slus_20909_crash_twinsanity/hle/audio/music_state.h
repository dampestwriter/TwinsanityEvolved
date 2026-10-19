#pragma once

#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"

namespace title {

class MusicState {
public:
    static bool Encode(const MusicLibrary::State& state, std::vector<uint8_t>& bytes,
                       std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, MusicLibrary::State& state,
                       std::string* error = nullptr);
};
} // namespace title
