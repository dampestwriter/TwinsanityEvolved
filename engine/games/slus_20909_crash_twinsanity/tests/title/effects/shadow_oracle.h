#pragma once

#include <cstdint>
#include <array>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

// Actual current NEW StartShadows/ApplyShadows bodies and vertex assembly,
// operating on isolated synthetic RAM. No original recording or PCSX2 input.
class ShadowOracle {
public:
    using Matrix = std::array<std::array<float, 4>, 4>;
    struct CpuEffects {
        std::array<Matrix, 4> begin; // saved screen, view screen, shadow camera, shadow screen
        Matrix end;
    };
    static std::vector<ps2r::gs::DrawIr> Passes(uint32_t width, uint32_t height);
    static CpuEffects Matrices(uint32_t width, uint32_t height,
                               const std::array<int32_t, 8>& target,
                               const Matrix& camera, const Matrix& screen);
    static uint64_t Calls();
};
