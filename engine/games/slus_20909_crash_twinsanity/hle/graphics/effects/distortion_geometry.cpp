#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_geometry.h"

#include <cmath>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include "engine/platform/host/platform.h"
#include "engine/vu/vu_float.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Geometry = DistortionGeometry;
using Draw = ps2r::gs::DrawIr;
using Vertex = ps2r::gs::VertexRec;
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float Float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
float Finite(float value) {
    const auto bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    if (exponent == 0x7f800000) return Float((bits & 0x80000000) | 0x7f7fffff);
    return value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
float Sum(float a, float b) { return Finite(Finite(a) + Finite(b)); }
float Difference(float a, float b) { return Finite(Finite(a) - Finite(b)); }
float Fma(float a, float b, float c) { return Finite(VuFloat::Fma(Finite(a), Finite(b), Finite(c))); }
float Reciprocal(float value) {
    value = Finite(value);
    return value == 0 ? Float((Bits(value) & 0x80000000) | 0x7f7fffff) : Finite(1.f / value);
}
uint32_t Integer(float value) {
    if (value >= 2147483648.f) return 0x7fffffff;
    if (value < -2147483648.f) return 0x80000000;
    return uint32_t(int32_t(value));
}
float Minimum(float a, float b) {
    const auto x = Bits(a), y = Bits(b);
    if ((x & y) >> 31) return int32_t(x) > int32_t(y) ? a : b;
    return int32_t(x) < int32_t(y) ? a : b;
}
float Maximum(float a, float b) {
    const auto x = Bits(a), y = Bits(b);
    if ((x & y) >> 31) return int32_t(x) < int32_t(y) ? a : b;
    return int32_t(x) > int32_t(y) ? a : b;
}
uint32_t PowerOfTwo(uint32_t value) {
    uint32_t result = 1;
    while (result < value) result *= 2;
    return result;
}
bool Same(const Vertex& a, const Vertex& b) { return a.x == b.x && a.y == b.y; }
void Triangle(Draw& draw, const Vertex& a, const Vertex& b, const Vertex& c) {
    if (!Same(a, b) && !Same(b, c) && !Same(a, c))
        draw.verts.insert(draw.verts.end(), {a, b, c});
}
struct Projection {
    std::array<float, 4> centre{};
    std::array<float, 2> axis_a{}, axis_b{}, distort_a{}, distort_b{};
    std::array<float, 2> texture_scale{}, texture_corner{}, texture_limit{};
    std::array<uint8_t, 4> colour{};
    float reciprocal = 1;
};
Projection Prepare(const Geometry::Particle& particle, const Geometry::Look& look,
                   const Geometry::View& view, float age, float phase) {
    Projection out;
    std::array<float, 3> point;
    for (unsigned axis = 0; axis < 3; ++axis)
        point[axis] = Sum(particle.start[axis], Product(particle.velocity[axis], age));
    point[1] = Sum(point[1], Product(look.gravity, Product(age, age)));
    for (unsigned axis = 0; axis < 4; ++axis) {
        float value = Product(view.screen[0][axis], point[0]);
        value = Fma(view.screen[2][axis], point[2], value);
        value = Fma(view.screen[3][axis], 1, value);
        out.centre[axis] = Fma(view.screen[1][axis], point[1], value);
    }
    out.reciprocal = Reciprocal(out.centre[3]);
    const int32_t index = int32_t(Integer(phase));
    const auto& current = look.shapes[uint32_t(index) & 63];
    const auto& next = look.shapes[(uint32_t(index) & 63) + 1];
    const float fraction = Difference(phase, float(index)), remaining = Difference(1, fraction);
    std::array<float, 6> shape;
    for (unsigned i = 0; i < 4; ++i)
        shape[i] = Fma(current[i], remaining, Product(next[i], fraction));
    // The second shape row reverses which product is committed before the
    // multiply-add. Reassociating it changes distortion coordinates.
    for (unsigned i = 4; i < 6; ++i)
        shape[i] = Fma(next[i], fraction, Product(current[i], remaining));
    for (unsigned i = 0; i < 4; ++i) out.colour[i] = uint8_t(Bits(current[i]));
    const float fade = Maximum(Product(Difference(40, out.centre[3]), Reciprocal(40)), 0);
    const std::array<float, 2> distortion{Product(view.distortion[0], 0x1p-14f),
                                         Product(view.distortion[1], -0x1p-12f)};
    for (unsigned axis = 0; axis < 2; ++axis) {
        out.axis_a[axis] = shape[axis + 2];
        out.axis_b[axis] = Fma(-out.axis_a[axis], 1, Difference(shape[axis + 4], shape[axis]));
        out.distort_a[axis] = Product(Product(out.axis_a[axis], distortion[axis]), fade);
        out.distort_b[axis] = Product(Product(out.axis_b[axis], distortion[axis]), fade);
        const auto dimension = axis ? view.height : view.width;
        const float extent = float(PowerOfTwo(dimension));
        out.texture_scale[axis] = 1.f / (extent * 16.f);
        out.texture_corner[axis] = (float(dimension) * 8.f - 32768.f) / (extent * 16.f);
        out.texture_limit[axis] = (float(dimension) - 1.f) / extent;
    }
    return out;
}
Vertex Project(const Projection& values, const std::array<float, 2>& corner,
               bool centre, const Draw& material, bool restart) {
    Vertex result{};
    std::array<float*, 2> position{&result.x, &result.y}, texture{&result.s, &result.t};
    for (unsigned axis = 0; axis < 2; ++axis) {
        float point = values.centre[axis];
        if (!centre) {
            point = Fma(values.axis_a[axis], corner[0], Product(point, 1));
            point = Fma(values.axis_b[axis], corner[1], point);
        }
        const float projected = Minimum(Maximum(Product(point, values.reciprocal), 0),
                                        axis ? Float(0x470e1000) : Float(0x47140000));
        *position[axis] = float(Integer(projected) & 0xffff) / 16 - (axis ? material.off_y : material.off_x);
        float sample = Product(projected, values.texture_scale[axis]);
        if (!centre) {
            sample = Fma(-values.distort_a[axis], corner[0], sample);
            sample = Fma(-values.distort_b[axis], corner[1], sample);
        }
        sample = Fma(values.texture_corner[axis], 1, sample);
        *texture[axis] = Minimum(sample, values.texture_limit[axis]);
    }
    volatile uint32_t depth = Integer(Product(values.centre[2], values.reciprocal));
    result.z = float(depth);
    result.q = 1;
    result.r = values.colour[0]; result.g = values.colour[1];
    result.b = values.colour[2];
    // The centre keeps the current step's alpha; all six outer corners are
    // transparent. Both strips retain this radial interpolation.
    result.a = centre ? values.colour[3] : 0;
    result.draw_kick = !restart;
    return result;
}
void ExpandParticle(const Geometry::Particle& particle, const Geometry::Look& look,
              const Geometry::View& view, Draw& draw) {
    const float age = Difference(view.time, particle.spawn_time);
    const float phase = Product(particle.life_scale, age);
    // The original branches test sign bits of these separate subtractions.
    // Signed zero is significant; folding into a range comparison changes it.
    if (std::signbit(age) || std::signbit(Difference(Float(0x427b999a), phase))) return;
    const auto values = Prepare(particle, look, view, age, phase);
    if (std::signbit(Difference(values.centre[3], .5f))) return;
    for (unsigned half = 0; half < 2; ++half) {
        const unsigned first = half * 3;
        const auto a = Project(values, look.corners[first], false, draw, true);
        const auto b = Project(values, look.corners[first + 1], false, draw, true);
        const auto c = Project(values, {}, true, draw, false);
        const auto d = Project(values, look.corners[first + 2], false, draw, false);
        const auto e = Project(values, look.corners[(first + 3) % 6], false, draw, false);
        Triangle(draw, a, b, c); Triangle(draw, c, b, d); Triangle(draw, c, d, e);
    }
}
bool Valid(const std::vector<Geometry::Block>& blocks, const Geometry::Look& look, const Geometry::View& view) {
    if (!std::isfinite(look.gravity) || !std::isfinite(view.time)) return false;
    for (float value : view.distortion) if (!std::isfinite(value)) return false;
    for (const auto& row : view.screen) for (float value : row) if (!std::isfinite(value)) return false;
    for (const auto& row : look.corners) for (float value : row) if (!std::isfinite(value)) return false;
    for (const auto& row : look.shapes) for (float value : row) if (!std::isfinite(value)) return false;
    for (const auto& block : blocks) for (const auto& particle : block) {
        if (!std::isfinite(particle.spawn_time) || !std::isfinite(particle.life_scale) || particle.life_scale < 0)
            return false;
        for (float value : particle.start) if (!std::isfinite(value)) return false;
        for (float value : particle.velocity) if (!std::isfinite(value)) return false;
    }
    return true;
}
}
DistortionGeometry::Result DistortionGeometry::Build(const std::vector<Block>& blocks,
    const Look& look, const View& view, const Draw& material, std::vector<Draw>& out) {
    if (!view.width || !view.height || view.width > 2048 || view.height > 2048) return Result::InvalidDimensions;
    if (blocks.size() > kMaxBlocks) return Result::LimitExceeded;
    if (!Valid(blocks, look, view)) return Result::InvalidValues;
    if (material.prim_type != 4 || material.use_fst || !material.tex_enabled ||
        !std::isfinite(material.off_x) || !std::isfinite(material.off_y)) return Result::UnsupportedState;
    RoundingGuard rounding;
    std::vector<Draw> draws;
    for (const auto& block : blocks) for (unsigned group = 0; group < 4; ++group) {
        auto draw = material;
        draw.verts.clear();
        for (unsigned particle = 0; particle < 3; ++particle)
            ExpandParticle(block[group * 3 + particle], look, view, draw);
        if (draw.verts.empty()) continue;
        draw.vertex_count = uint32_t(draw.verts.size());
        draw.snapshot.verts_incl = 1;
        draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
        ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
        draws.push_back(std::move(draw));
    }
    out = std::move(draws);
    return Result::Ok;
}
}  // namespace title
