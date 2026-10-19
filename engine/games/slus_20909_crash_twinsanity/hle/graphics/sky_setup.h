#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_geometry.h"

namespace title {

// StartSkyBuffer's software output and half-size target intent. No clear: the
// source retains the target's contents until geometry writes them.
class SkySetup {
public:
    static constexpr uint32_t kAddress = 0x001b9cf8;
    static constexpr uint32_t kRenderView = 0x00309840;
    struct Output {
        uint32_t width = 0, height = 0; // full display; attachments are >> 1
        SkyGeometry::Matrix to_screen{};
        ps2r::gs::DrawIr state;
    };
    enum class Result { Ok, InvalidMemory, UnsupportedFrame, InvalidMatrix };
    // Reads source objects only. Caller stores to_screen at Sky+0x10 AFTER
    // selecting the entire pass. The retained DrawSky consumes that matrix.
    static Result Capture(const MaterialState::Read& read,
                          const MaterialState::Environment& environment, Output& out);
};

}  // namespace title
