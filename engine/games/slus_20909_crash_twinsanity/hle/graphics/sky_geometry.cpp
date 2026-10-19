#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_geometry.h"

namespace title {
SkyGeometry::Result SkyGeometry::Decode(const uint8_t* bytes, size_t size,
    uint32_t vertices, Mesh& out) {
    return MeshGeometry::Decode(bytes, size, vertices, out, MeshGeometry::Layout::Sky);
}
SkyGeometry::Result SkyGeometry::Build(const Mesh& mesh, const View& view,
    const ps2r::gs::DrawIr& state, std::vector<ps2r::gs::DrawIr>& out) {
    MeshGeometry::View instance{view.screen, view.clip, view.guard, view.scroll, true};
    return MeshGeometry::Build(mesh, {instance}, state, out);
}
}  // namespace title
