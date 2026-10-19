#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"

namespace title {

// Source view-plane preparation for ordinary chunks, portals and particles.
// The six rows are near, right, left, top, bottom, far.
class CullingPlanes {
public:
    using Planes = std::array<CullingMath::Vector, 6>;
    static CullingMath::Vector Transform(
        const CullingMath::Vector& plane, const CullingMath::Matrix& place);
    static CullingMath::Frustum InChunk(
        const Planes& view, const Planes& far_view, const CullingMath::Matrix& place);
    // Portal sides already belong to the chunk. Only the view's near plane
    // and wider side planes are transformed; the far-depth column is zero.
    static CullingMath::Frustum ThroughPortal(
        const Planes& view, const Planes& far_view,
        const CullingMath::Matrix& place, const Planes& portal);

private:
    static void Column(CullingMath::PlaneColumns& columns,
                       const CullingMath::Vector& plane, size_t column);
    static CullingMath::PlaneColumns Sides(
        const Planes& planes, const CullingMath::Matrix* place);
};

}  // namespace title
