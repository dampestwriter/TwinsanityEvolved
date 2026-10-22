#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "engine/vu/vu_float.h"
#include <limits>

namespace title {

// The Vita: the operations as the CPU does them, unclamped and in its own rounding -- the exact form (operands and
// results clamped through memory, the VU's rounding) cost the game thread milliseconds a frame there; elsewhere exact.
#if defined(__vita__)
class CullingMath::Rounding { public: Rounding() {} };
#else
class CullingMath::Rounding {
public:
    Rounding() : previous_(std::fegetround()) {
        if (previous_ != FE_TOWARDZERO) std::fesetround(FE_TOWARDZERO);
    }
    ~Rounding() {
        if (previous_ != FE_TOWARDZERO) std::fesetround(previous_);
    }
private:
    int previous_;
};
#endif

float CullingMath::Finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t magnitude = bits & 0x7fffffff;
    if (magnitude >= 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    else if (magnitude < 0x00800000) bits &= 0x80000000;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

#if defined(__vita__)
float CullingMath::Product(float a, float b) { return a * b; }
float CullingMath::Add(float a, float b) { return a + b; }
float CullingMath::Difference(float a, float b) { return a - b; }
float CullingMath::MultiplyAdd(float a, float b, float sum) { return a * b + sum; }
#else
float CullingMath::Product(float a, float b) {
    volatile float left = Finite(a), right = Finite(b);
    volatile float result = left * right;
    return Finite(result);
}

float CullingMath::Add(float a, float b) {
    volatile float left = Finite(a), right = Finite(b);
    volatile float result = left + right;
    return Finite(result);
}

float CullingMath::Difference(float a, float b) {
    volatile float left = Finite(a), right = Finite(b);
    volatile float result = left - right;
    return Finite(result);
}

float CullingMath::MultiplyAdd(float a, float b, float sum) {
    volatile float left = Finite(a), right = Finite(b), accumulated = Finite(sum);
    // VuFloat::Fma: std::fma's result (the Vita: its product and sum rounded apart; newlib's fmaf is software)
    return Finite(::VuFloat::Fma(left, right, accumulated));
}
#endif

CullingMath::Vector CullingMath::Transform(const Matrix& basis, const Vector& coordinates) {
    Vector result;
    for (size_t lane = 0; lane < result.size(); ++lane) {
        float sum = Product(basis[0][lane], coordinates[0]);
        sum = MultiplyAdd(basis[1][lane], coordinates[1], sum);
        sum = MultiplyAdd(basis[2][lane], coordinates[2], sum);
        result[lane] = MultiplyAdd(basis[3][lane], coordinates[3], sum);
    }
    return result;
}

CullingMath::Link CullingMath::LoadLink(
        const View& view, const Matrix& chunk, const Matrix& object) {
    Rounding rounding;
    Link link;
    for (size_t row = 0; row < chunk.size(); ++row) {
        link.to_clip[row] = Transform(view.to_clip, chunk[row]);
        link.to_screen[row] = Transform(view.to_screen, chunk[row]);
    }
    // Source camera composition begins with translation, unlike Transform.
    for (size_t lane = 0; lane < link.camera.size(); ++lane) {
        float sum = Product(object[3][lane], 1.0f);
        sum = MultiplyAdd(object[0][lane], view.camera[0], sum);
        sum = MultiplyAdd(object[1][lane], view.camera[1], sum);
        link.camera[lane] = MultiplyAdd(object[2][lane], view.camera[2], sum);
    }
    return link;
}

int32_t CullingMath::Distance(const Vector& camera, const Vector& position) {
    Vector squared;
    for (size_t lane = 0; lane < 3; ++lane) {
        const float delta = Difference(camera[lane], position[lane]);
        squared[lane] = Product(delta, delta);
    }
    const float sum = Add(Add(squared[0], squared[1]), squared[2]);
    if (sum >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(sum);
}

CullingMath::Model CullingMath::LoadModel(const Link& link, const Matrix& model) {
    Rounding rounding;
    Model result;
    for (size_t row = 0; row < model.size(); ++row)
        result.to_screen[row] = Transform(link.to_screen, model[row]);
    result.distance = Distance(link.camera, model[3]);
    return result;
}

CullingMath::Classification CullingMath::Classify(
        const PlaneColumns& planes, const Vector& center, const Vector& half) {
    Classification result{};
    for (size_t plane = 0; plane < 4; ++plane) {
        float radius = Product(planes.absolute_normals[0][plane], half[0]);
        radius = MultiplyAdd(planes.absolute_normals[1][plane], half[1], radius);
        radius = MultiplyAdd(planes.absolute_normals[2][plane], half[2], radius);
        float distance = Product(planes.normals[0][plane], center[0]);
        distance = MultiplyAdd(planes.normals[1][plane], center[1], distance);
        distance = MultiplyAdd(planes.normals[2][plane], center[2], distance);
        distance = MultiplyAdd(planes.offsets[plane], 1.0f, distance);
        const bool high_negative = std::signbit(MultiplyAdd(radius, 1.0f, distance));
        const bool low_negative = std::signbit(MultiplyAdd(-Finite(radius), 1.0f, distance));
        // Source CullResult exposes these nonzero plane masks. Sign, including
        // negative zero, is the criterion; replacing it with < 0 loses a case.
        const uint32_t mask = 0x80u >> plane;
        if (high_negative && low_negative) result.outside |= mask;
        if (high_negative || low_negative) result.clipped |= mask;
    }
    return result;
}

CullingMath::Classification CullingMath::TestBox(const Frustum& frustum, const Box& box) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto sides = Classify(frustum.sides, center, half);
    const auto depth = Classify(frustum.depth, center, half);
    return {sides.outside | depth.outside, sides.clipped | depth.clipped};
}

CullingMath::PlaneColumns CullingMath::TransformPlanes(
        const PlaneColumns& planes, const Matrix& model) {
    PlaneColumns result;
    for (size_t plane = 0; plane < 4; ++plane) {
        for (size_t axis = 0; axis < 3; ++axis) {
            float sum = Product(planes.normals[0][plane], model[axis][0]);
            sum = MultiplyAdd(planes.normals[1][plane], model[axis][1], sum);
            sum = MultiplyAdd(planes.normals[2][plane], model[axis][2], sum);
            result.normals[axis][plane] = sum;
            result.absolute_normals[axis][plane] = std::fabs(sum);
        }
        float sum = Product(planes.normals[0][plane], model[3][0]);
        sum = MultiplyAdd(planes.normals[1][plane], model[3][1], sum);
        sum = MultiplyAdd(planes.normals[2][plane], model[3][2], sum);
        result.offsets[plane] = MultiplyAdd(planes.offsets[plane], 1.0f, sum);
    }
    return result;
}

CullingMath::BoxView CullingMath::TestBoxAt(
        const Frustum& frustum, const Link& link, const Box& box, const Matrix& model) {
    Rounding rounding;
    Vector center{}, half{};
    for (size_t axis = 0; axis < 3; ++axis) {
        half[axis] = Product(Difference(box.maximum[axis], box.minimum[axis]), 0.5f);
        center[axis] = MultiplyAdd(box.maximum[axis], 0.5f, Product(box.minimum[axis], 0.5f));
    }
    const auto depth = Classify(TransformPlanes(frustum.depth, model), center, half);
    const auto sides = Classify(TransformPlanes(frustum.sides, model), center, half);
    BoxView result{};
    result.distance = Distance(link.camera, model[3]);
    result.visibility.clipped = sides.outside | (depth.clipped & 0x40u);
    result.visibility.outside = depth.outside | result.visibility.clipped;
    if (result.visibility.outside) return result;

    const auto far = Classify(TransformPlanes(frustum.far_sides, model), center, half);
    result.visibility.clipped = far.clipped | depth.clipped;
    Matrix screen;
    for (size_t row = 0; row < 4; ++row) screen[row] = Transform(link.to_screen, model[row]);
    result.to_screen = screen;
    if (result.visibility.clipped) {
        Matrix clip;
        for (size_t row = 0; row < 4; ++row) clip[row] = Transform(link.to_clip, model[row]);
        result.to_clip = clip;
    }
    return result;
}

}  // namespace title
