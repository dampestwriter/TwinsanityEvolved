#pragma once

#include <cstdint>

namespace title {

// Immutable title service input, held in the same ordered batch as sample
// controls. No command-buffer address or IOP/SPU storage is retained.
struct MusicRequest {
    enum class Kind : uint8_t { Window, Play, Interleave, Child, End, Prepare, Start, Stop, Reset };
    Kind kind = Kind::Window;
    uint32_t stream = 0, parent = 0, channel = 0, group = 0, track = 0;
    int32_t file = -1;
    uint32_t offset = 0, size = 0, block = 0, end = 0;
    uint16_t pitch = 0, left = 0, right = 0, raw_left = 0, raw_right = 0;
    uint8_t attack = 0, release = 0;
    bool looping = false;
};
} // namespace title
