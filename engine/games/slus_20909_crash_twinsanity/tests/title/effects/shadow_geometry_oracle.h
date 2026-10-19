#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"

class ShadowGeometryOracle {
public:
    static std::vector<ps2r::gs::DrawIr> Run(const title::ShadowGeometry::Mesh&,
                                            const std::vector<title::ShadowGeometry::View>&);
};
