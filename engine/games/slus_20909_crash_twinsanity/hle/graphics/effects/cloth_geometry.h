#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

namespace title {

// Complete cloth geometry: authored positions deform once before every
// instance's guard/clipping/projection, with owned source wave values.
class ClothGeometry {
public:
    enum class Kind { Single = 0x17, Three = 0x1a };
    struct Waves {
        Kind kind = Kind::Single;
        std::array<float, 3> amplitude{};
        std::array<std::array<float, 3>, 16> rows{};
    };
    using Result = MeshGeometry::Result;
    static Result Build(const MeshGeometry::Mesh&, const Waves&,
                        const std::vector<MeshGeometry::View>&,
                        const ps2r::gs::DrawIr& material, std::vector<ps2r::gs::DrawIr>& out);
};

}  // namespace title
