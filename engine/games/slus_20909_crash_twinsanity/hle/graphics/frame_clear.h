#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Source SetUpFrame / StartFrame rectangle. The target's computed GS origin
// is read at the public NTSC entry; no chain or register state is inspected.
class FrameClear {
public:
    static constexpr uint32_t kAddress = 0x0019f1c8;
    struct Input {
        uint32_t x = 0, y = 0, width = 0, height = 0;
        uint32_t screen_width = 0, screen_height = 0;
        uint8_t red = 0, green = 0, blue = 0;
        bool color = false, depth = false;
    };
    struct Output {
        uint32_t width = 0, height = 0;
        ps2r::gs::DrawIr draw;
    };
    enum class Result { Ok, UnsupportedEntry, InvalidMemory, UnsupportedFrame };
    static Result Build(const Input&, Output&);
    static Result Capture(const MaterialState::Read&, const CpuContext&, uint32_t entry, Output&);
};
} // namespace title
