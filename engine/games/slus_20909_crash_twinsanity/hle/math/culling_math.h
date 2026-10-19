#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace title {

// Source culling operations, with the view/link lifetime explicit. These are
// ordinary math data; installing NTSC entries also requires the owning renderer
// to replace LoadCullingView/CullLoadLink and the consumers of CullResult.
class CullingMath {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;

    struct View {
        Matrix to_clip;
        Matrix to_screen;
        Vector camera;
    };
    struct Link {
        Matrix to_clip;
        Matrix to_screen;
        Vector camera;
    };
    struct Model {
        Matrix to_screen;
        int32_t distance;
    };
    struct PlaneColumns {
        std::array<Vector, 3> absolute_normals;
        std::array<Vector, 3> normals;
        Vector offsets;
    };
    struct Frustum {
        PlaneColumns sides;
        PlaneColumns depth;
        PlaneColumns far_sides;
    };
    struct Box {
        Vector minimum;
        Vector maximum;
    };
    struct Classification {
        uint32_t outside;
        uint32_t clipped;
    };
    struct BoxView {
        Classification visibility;
        int32_t distance;
        std::optional<Matrix> to_screen;
        std::optional<Matrix> to_clip;
    };

    // TR renderer/culling.cpp:449. The camera is transformed by object, while
    // the two view matrices are composed with chunk. Camera.w is unused.
    static Link LoadLink(const View& view, const Matrix& chunk, const Matrix& object);
    // TR renderer/culling.cpp:465, chunkview.cpp:190,258. Distance is the
    // saturating integer squared xyz distance, not its square root.
    static Model LoadModel(const Link& link, const Matrix& model);
    // TR renderer/culling.cpp:478, chunkview.cpp:195. The loaded columns hold
    // four side planes and the near/far pair. Nonzero masks mean out/partial.
    static Classification TestBox(const Frustum& frustum, const Box& box);
    // Source renderer/culling.cpp:492. Rejects far-plane intersections;
    // visible objects produce screen data, with clip data only when needed.
    static BoxView TestBoxAt(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model);

private:
    class Rounding;
    static float Finite(float value);
    static float Product(float a, float b);
    static float Add(float a, float b);
    static float Difference(float a, float b);
    static float MultiplyAdd(float a, float b, float sum);
    static Vector Transform(const Matrix& basis, const Vector& coordinates);
    static int32_t Distance(const Vector& camera, const Vector& position);
    static Classification Classify(
        const PlaneColumns& planes, const Vector& center, const Vector& half);
    static PlaneColumns TransformPlanes(const PlaneColumns& planes, const Matrix& model);
};

}  // namespace title
