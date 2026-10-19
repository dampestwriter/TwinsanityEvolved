#pragma once

#include <string>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

namespace title {

// Audio's portion of a composed checkpoint. Assets referenced only by a
// playing voice survive bank unload. Pending tickets require the matching
// FileSchedule payload; this codec alone does not authorize a root checkpoint.
class AudioState {
public:
    static bool Encode(const Audio::Snapshot& state, std::vector<uint8_t>& bytes,
                       std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, Audio::Snapshot& state,
                       std::string* error = nullptr);
    // Queued plays own assets even before a mixer channel starts. This uses
    // the same voice grammar and deduplicated asset table as active playback.
    static bool EncodeLeases(const std::vector<Audio::Voice>& leases, std::vector<uint8_t>& bytes,
                             std::string* error = nullptr);
    static bool DecodeLeases(const std::vector<uint8_t>& bytes, std::vector<Audio::Voice>& leases,
                             std::string* error = nullptr);
};
} // namespace title
