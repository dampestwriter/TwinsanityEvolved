#pragma once

#include "engine/gs/hle/ir/draw_ir.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_layout.h"

namespace title {

class TextDraw {
public:
    enum class Result { Ok, InvalidState, InvalidInput };
    // Source page 1..3; source frame extent determines the screen corner,
    // while state.off_* carries the actual destination's origin.
    // Preserves 20-glyph/group draw boundaries and every cursor advance.
    static Result Build(const ps2r::gs::DrawIr& state, const TextLayout::Font& font,
                        const TextLayout::Output& layout, uint8_t page,
                        int32_t frame_width, int32_t frame_height,
                        std::vector<ps2r::gs::DrawIr>& out);

private:
    struct Rounding;
};

}  // namespace title
