#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_mesh.h"

#include <utility>

namespace title {
MeshMaterial::Result ClothMesh::Capture(const MaterialState::Read& read, uint32_t material,
    const MaterialState::Environment& environment, const MeshMaterial::Lookup& texture,
    MeshMaterial::Output& output, Geometry& geometry) {
    ClothMaterial::Output captured;
    const auto result = ClothMaterial::Capture(read, material, environment, texture, captured);
    if (result != MeshMaterial::Result::Ok) return result;
    Geometry candidate = [waves = captured.waves](const MeshGeometry::Mesh& mesh,
        const std::vector<MeshGeometry::View>& instances, const ps2r::gs::DrawIr& state,
        std::vector<ps2r::gs::DrawIr>& draws) {
        return ClothGeometry::Build(mesh, waves, instances, state, draws);
    };
    output = std::move(captured.material);
    geometry.swap(candidate);
    return MeshMaterial::Result::Ok;
}
}  // namespace title
