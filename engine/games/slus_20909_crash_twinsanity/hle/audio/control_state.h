#pragma once

#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"

namespace title {

// Portable ordered requests, including assets leased by plays that have not
// been accepted. Provider functions and captured original bodies are never
// serialized. Service composition validates their owning selections.
class ControlState {
public:
    static bool Encode(const SoundControls::State& state, std::vector<uint8_t>& bytes,
                       std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, SoundControls::State& state,
                       std::string* error = nullptr);
};
} // namespace title
