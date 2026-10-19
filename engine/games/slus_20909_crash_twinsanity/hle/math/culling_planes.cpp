#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"

#include <cmath>

#include "engine/ee/cpu/ps2_float.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {
namespace {
float Product(float a, float b) {
    volatile float product = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(product);
}
float Add(float a, float b) {
    volatile float sum = ps2_float_clamp(a) + ps2_float_clamp(b);
    return ps2_float_clamp(sum);
}
float Difference(float a, float b) {
    volatile float result = ps2_float_clamp(a) - ps2_float_clamp(b);
    return ps2_float_clamp(result);
}
}

CullingMath::Vector CullingPlanes::Transform(
        const CullingMath::Vector& plane, const CullingMath::Matrix& place) {
    // Source game/math.cpp:539. This is macro rotation followed by scalar
    // separately rounded dot/subtraction, not the fused micro plane operation.
    auto result = MathLibrary::EvaluateRotation(place, plane).output;
    const float x = Product(result[0], place[3][0]);
    const float y = Product(result[1], place[3][1]);
    const float z = Product(result[2], place[3][2]);
    result[3] = Difference(result[3], Add(Add(x, y), z));
    return result;
}

void CullingPlanes::Column(
        CullingMath::PlaneColumns& columns, const CullingMath::Vector& plane, size_t column) {
    for (size_t axis = 0; axis < 3; ++axis) {
        columns.normals[axis][column] = plane[axis];
        columns.absolute_normals[axis][column] = std::fabs(plane[axis]);
    }
    columns.offsets[column] = plane[3];
}

CullingMath::PlaneColumns CullingPlanes::Sides(
        const Planes& planes, const CullingMath::Matrix* place) {
    CullingMath::PlaneColumns result{};
    for (size_t side = 0; side < 4; ++side) {
        const auto& plane = planes[side + 1];
        Column(result, place ? Transform(plane, *place) : plane, side);
    }
    return result;
}

CullingMath::Frustum CullingPlanes::InChunk(
        const Planes& view, const Planes& far_view, const CullingMath::Matrix& place) {
    CullingMath::Frustum result{};
    result.sides = Sides(view, &place);
    result.far_sides = Sides(far_view, &place);
    Column(result.depth, Transform(view[0], place), 0);
    Column(result.depth, Transform(view[5], place), 1);
    return result;
}

CullingMath::Frustum CullingPlanes::ThroughPortal(
        const Planes& view, const Planes& far_view,
        const CullingMath::Matrix& place, const Planes& portal) {
    CullingMath::Frustum result{};
    result.sides = Sides(portal, nullptr);
    result.far_sides = Sides(far_view, &place);
    Column(result.depth, Transform(view[0], place), 0);
    return result;
}

}  // namespace title
