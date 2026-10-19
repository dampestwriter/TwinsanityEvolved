#pragma once

#include <cstdint>

namespace title {
// Immutable service request, accepted in the same queue as music controls.
struct BufferRequest {
    enum class Kind : uint8_t { Allocate, Resize, Close, Query };
    Kind kind = Kind::Allocate;
    uint32_t stream = 0, size = 0, sound_address = 0, free_word = 0;
};
} // namespace title
