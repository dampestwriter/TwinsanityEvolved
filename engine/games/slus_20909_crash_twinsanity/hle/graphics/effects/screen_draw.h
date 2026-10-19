#pragma once

#include "engine/gs/hle/ir/draw_ir.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Source screen-effect geometry and material state. Attachment identity and
// sampled images are supplied by the screen pass owner, never these metadata.
class ScreenDraw {
public:
    enum class Result { Ok, InvalidHeight };
    // The source screen effects always draw 16 strips across 512 pixels.
    static Result WaveCopy(uint32_t height, uint32_t frame, ps2r::gs::DrawIr& out);
    // Source type18's fixed 256-square RGB scene copy, in the second context.
    static Result DistortionCopy(uint32_t height, uint32_t frame, const MaterialState::Environment&,
                                 ps2r::gs::DrawIr& out);
    // Ordered shrink, stretch, depth-tested composite.
    static Result Blur(uint32_t height, uint32_t frame, std::vector<ps2r::gs::DrawIr>& out);
    // CopyDepth establishes nearest sampling. A preceding Blur changes the
    // same source context to bilinear; FilterColours intentionally inherits it.
    static Result Filter(uint32_t height, uint32_t frame, bool after_blur, ps2r::gs::DrawIr& out);
};

}  // namespace title
