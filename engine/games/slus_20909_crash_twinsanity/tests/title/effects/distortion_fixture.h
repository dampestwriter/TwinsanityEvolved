#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_material.h"

class DistortionFixture {
public:
    using Geometry = title::DistortionGeometry;
    Geometry::Look look;
    Geometry::View view;
    std::vector<Geometry::Block> blocks;
    ps2r::gs::DrawIr material;
    std::vector<ps2r::gs::DrawIr> draws;
    title::MaterialState::Environment environment;
    explicit DistortionFixture(uint32_t height = 448, uint32_t source_frame = 7);
    void Build();
};
