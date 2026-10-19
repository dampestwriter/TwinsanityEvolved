#pragma once

#include "engine/gs/hle/ir/draw_ir.h"

class ScreenOracle {
public:
    static std::vector<ps2r::gs::DrawIr> Blur(uint32_t height);
    static std::vector<ps2r::gs::DrawIr> Wave(uint32_t width, uint32_t height);
    // CopyDepthBuffer's two halves (the depth as a Z16 texture into the 16-bit copy).
    static std::vector<ps2r::gs::DrawIr> DepthCopy(uint32_t height);
    static ps2r::gs::DrawIr Filter(uint32_t height, bool after_blur);
    static uint64_t Calls();
};
