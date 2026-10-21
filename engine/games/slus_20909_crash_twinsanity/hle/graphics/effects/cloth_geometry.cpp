#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_geometry.h"

#include <cmath>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include "engine/platform/host/platform.h"
#include "engine/vu/vu_float.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float Float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    if (exponent == 0x7f800000) return Float((bits & 0x80000000) | 0x7f7fffff);
    return value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
}
ClothGeometry::Result ClothGeometry::Build(const MeshGeometry::Mesh& original, const Waves& waves,
    const std::vector<MeshGeometry::View>& instances, const ps2r::gs::DrawIr& state,
    std::vector<ps2r::gs::DrawIr>& out) {
    if (waves.kind != Kind::Single && waves.kind != Kind::Three) return Result::UnsupportedState;
    for (float value : waves.amplitude) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (const auto& row : waves.rows)
        for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
    size_t count = 0;
    for (const auto& strip : original.strips) {
        if (strip.vertices.size() < 3 || strip.vertices.size() > 38) return Result::InvalidGeometry;
        count += strip.vertices.size();
        if (count > MeshGeometry::kMaxVertices) return Result::LimitExceeded;
        for (const auto& vertex : strip.vertices)
            for (float value : vertex.position) if (!std::isfinite(value)) return Result::InvalidGeometry;
    }
    RoundingGuard rounding;
    auto mesh = original;
    for (auto& strip : mesh.strips) {
        for (auto& vertex : strip.vertices) {
            for (size_t axis = 0; axis < 3; ++axis) {
                const float position = vertex.position[axis];
                const unsigned index = Bits(Product(position, Float(0x39c38f36))) & 15;
                // NTSC 17/1A use the same +3 lookup origin. 1A's added
                // amplitude row occupies index zero, followed by wave rows
                // 0..14. Do not silently shift it to the apparent intention.
                const float wave = waves.kind == Kind::Single ? waves.rows[index][axis] :
                    index ? waves.rows[index - 1][axis] : waves.amplitude[axis];
                const float amplitude = waves.amplitude[waves.kind == Kind::Single ? 0 : axis];
                const float offset = Product(wave, amplitude);
                // The product is committed before position*1 + offset. Fusing
                // wave*amplitude+position changes the original vertex bits.
                vertex.position[axis] = Finite(VuFloat::Fma(Finite(position), 1.f, offset));
            }
        }
    }
    return MeshGeometry::Build(mesh, instances, state, out);
}
}  // namespace title
