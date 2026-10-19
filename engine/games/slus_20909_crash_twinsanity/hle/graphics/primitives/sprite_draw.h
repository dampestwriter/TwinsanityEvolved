#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_draw.h"

namespace title {

// Complete Sprite geometry: source rectangles/transform -> owned triangles.
// No packet production, graphics model, register state, or VU execution.
class SpriteDraw {
public:
    using Matrix = MathLibrary::Matrix;
    struct Rectangle { float x = 0, y = 0, width = 1, height = 1; };
    struct Target {
        StripDraw::Target coordinates;
        int32_t screen_width = 0, screen_height = 0;
    };
    struct Input {
        Rectangle texture;
        const Matrix* matrix = nullptr;
        bool turned = false;
        uint32_t colour = 0x80c0c0c0;
    };
    enum class Result { Ok, InvalidState, InvalidInput };
    // state carries the complete material/environment. Textured Sprite uses
    // STQ coordinates with Q=1. Fixed UV, inherited fog and nonfinite inputs
    // explicitly decline. All failures leave output intact; input may alias it.
    // Geometry preserves source operation order, subpixel truncation, signed
    // coordinate packing, newest-vertex colour and degenerate strip parity.
    static Result Build(const ps2r::gs::DrawIr& state, const Target& target,
                        const Input& input, ps2r::gs::DrawIr& out);

private:
    struct Rounding;
};

}  // namespace title
