#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Source frame.cpp WriteFrameHead: repeatable display conversion, captured
// when the source establishes it and executed at each presentation submission.
class FrameIntent {
public:
    static constexpr uint32_t kAddress = 0x0019aaa8;
    static constexpr uint32_t kSourceObject = 0x003d4400;
    enum class Result { Ok, UnsupportedEntry, InvalidMemory, UnsupportedFrame };
    struct Output {
        uint32_t width = 0, height = 0;
        ps2r::gs::DrawIr draw;
    };
    static Result Build(uint32_t width, uint32_t height,
                        const MaterialState::Environment&, Output&);
    static Result Capture(const MaterialState::Read&, uint32_t entry,
                          const MaterialState::Environment&, Output&);
};
} // namespace title
