#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

namespace title {

// A track's owned ADPCM sides. File interleave and transport loop markers are
// resolved when loading; playback only sees a contiguous immutable asset.
class MusicAsset {
public:
    struct Layout {
        uint32_t tracks = 1;
        uint32_t block_bytes = 0;
        uint32_t final_bytes = 0;
        bool looping = false;
    };
    struct Data {
        std::array<SampleVoice::Samples, 2> tracks;
        uint32_t count = 0, header_bytes = 0, source_rate = 0;
        uint64_t source_bytes = 0;
    };
    static bool Read(const std::vector<uint8_t>& bytes, Layout layout,
                     Data& out, std::string* error = nullptr);
    static uint32_t HeaderBytes(const uint8_t* bytes, size_t size);
};
} // namespace title
