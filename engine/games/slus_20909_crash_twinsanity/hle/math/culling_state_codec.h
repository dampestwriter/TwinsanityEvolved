#pragma once

#include <cstddef>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/math/culling_state.h"

namespace title {

class CullingStateCodec {
public:
    static constexpr uint32_t kTag = 0x4c4c5543u;  // CULL
    static constexpr size_t kMaxBytes = 1024;
    // Raw float words, including NaN payloads and negative zero, are encoded
    // little endian. No pointers, callbacks, register files or transport data.
    // On malformed/version-mismatched input the output is left unchanged.
    static bool Encode(const CullingState::Snapshot& snapshot,
                       std::vector<uint8_t>& output, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes,
                       CullingState::Snapshot& output, std::string* error = nullptr);
    static bool Validate(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
};

}  // namespace title
