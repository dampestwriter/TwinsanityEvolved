#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_material.h"

namespace title {

// Adapter for the mesh owner's material reservation. The returned geometry
// function closes over owned wave values only, never a shader/memory pointer.
class ClothMesh {
public:
    using Geometry = std::function<MeshGeometry::Result(
        const MeshGeometry::Mesh&, const std::vector<MeshGeometry::View>&,
        const ps2r::gs::DrawIr&, std::vector<ps2r::gs::DrawIr>&)>;
    static MeshMaterial::Result Capture(const MaterialState::Read&, uint32_t material,
        const MaterialState::Environment&, const MeshMaterial::Lookup&,
        MeshMaterial::Output&, Geometry&);
};

}  // namespace title
