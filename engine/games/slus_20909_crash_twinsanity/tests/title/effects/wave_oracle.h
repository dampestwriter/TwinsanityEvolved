#pragma once

#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/wave_geometry.h"

// Actual NEW interpreter over the statically identified retail type-1C bytes.
// Its result goes through NEW's UV/RGBA/vertex assembly before comparison.
class WaveOracle {
public:
    explicit WaveOracle(std::vector<uint8_t> program);
    std::vector<ps2r::gs::DrawIr> Run(const title::WaveGeometry::Grid&,
                                    const title::WaveGeometry::Waves&) const;
private:
    std::vector<uint8_t> program_;
};
