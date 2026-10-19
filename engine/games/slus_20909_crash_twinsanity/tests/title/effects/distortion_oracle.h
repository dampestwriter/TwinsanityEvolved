#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_geometry.h"

// Unchanged original NTSC type18 image on the current NEW interpreter.
// Geometry expansion and vertex assembly in this arm are independent of the
// replacement. The original image is a static executable fixture, not a capture.
class DistortionOracle {
public:
    explicit DistortionOracle(std::vector<uint8_t> program);
    std::vector<ps2r::gs::DrawIr> Run(const std::vector<title::DistortionGeometry::Block>&,
        const title::DistortionGeometry::Look&, const title::DistortionGeometry::View&,
        const ps2r::gs::DrawIr& material) const;
private:
    std::vector<uint8_t> program_;
};
