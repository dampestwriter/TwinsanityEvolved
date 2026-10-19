#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Source FinishSky: nearest half-size SkyColor -> SceneColor, writing zero
// scene depth. Complete DrawIR generation, not a packet rewrite or color copy.
class SkyIntent {
public:
    static constexpr uint32_t kAddress = 0x001b9f30;
    static constexpr uint32_t kBucket = 0;
    enum class Result { Ok, UnsupportedEntry, InvalidMemory, UnsupportedFrame };
    struct Output {
        ps2r::gs::DrawIr draw;
        uint32_t width = 0, height = 0;
    };
    // The environment belongs to the ordered source frame, never GsState.
    // FinishSky overrides clamp/dither, leaves scan mask and shared environment.
    // Inactive context-1 blend and unused mip bindings are canonical zero.
    // Bounds exclude wrapping retail surface layouts. Errors leave out intact.
    static Result Build(uint32_t width, uint32_t height,
                        const MaterialState::Environment& environment, Output& out);
    // Full entry only. Reads only the two contiguous display-size globals;
    // the source's Sky* argument is unused. No packet/cache/guest writes.
    static Result Capture(const MaterialState::Read& read, uint32_t entry,
                          const MaterialState::Environment& environment, Output& out);
};

}  // namespace title
