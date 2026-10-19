#pragma once

#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"

namespace title {

// Completed source boundaries and submission order. This section contains no
// graphics model registers, packets, target pixels or diagnostic call totals.
class FrameState {
public:
    static constexpr uint32_t kTag = 0x4d524647; // GFRM
    static constexpr uint32_t kVersion = 1;
    static bool Validate(const FrameEnvironment::Snapshot&, std::string* error = nullptr);
    static bool Encode(const FrameEnvironment::Snapshot&, std::vector<uint8_t>&, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>&, FrameEnvironment::Snapshot&, std::string* error = nullptr);
};

}  // namespace title
