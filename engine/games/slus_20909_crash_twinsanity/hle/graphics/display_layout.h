#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// The source display-settings object at SetupPCRTC. Accepted intent is the
// ordinary complete CT16 image in both circuits, with neutral display alpha.
// Other circuit/blend/position policies need their own owned presentation.
class DisplayLayout {
public:
    static constexpr uint32_t kAddress = 0x0019aff0, kSettings = 0x003d4ae0;
    struct Output { uint32_t width = 0, height = 0; };
    enum class Result { Ok, UnsupportedEntry, InvalidMemory, UnsupportedLayout };
    static bool Validate(const Output&);
    static Result Capture(const MaterialState::Read&, uint32_t entry, Output&);
};
} // namespace title
