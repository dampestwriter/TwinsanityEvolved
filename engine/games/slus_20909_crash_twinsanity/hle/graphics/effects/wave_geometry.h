#pragma once

#include <array>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// Source MakeWaveGrid + statically identified type-1C geometry. The wave table
// is owned material data captured at submission, not a data-memory image.
class WaveGeometry {
public:
    struct Vertex {
        std::array<float, 2> position{};
        std::array<uint16_t, 2> texture{};
        uint32_t depth = 0;
        bool restart = false;
    };
    struct Strip { std::vector<Vertex> vertices; };
    struct Grid {
        uint32_t width = 0, height = 0;
        std::vector<Strip> strips;
    };
    struct Waves {
        std::array<std::array<float, 3>, 16> rows{};
        float amplitude = 0;
    };
    enum class Result { Ok, InvalidDimensions, InvalidGeometry, InvalidWaves, LimitExceeded };
    // The source always makes 32 columns, then the upper/lower strips in that
    // order. It uses height>>5, with the lower strips starting at row 15.
    static Result MakeGrid(uint32_t width, uint32_t height, Grid& out);
    // One source material instance. RGB/A=127 and depth/ADC/UV come from the
    // source program, not from a generic screen-copy approximation.
    static Result Build(const Grid&, const Waves&, uint32_t frame,
                        std::vector<ps2r::gs::DrawIr>& out);
};

}  // namespace title
