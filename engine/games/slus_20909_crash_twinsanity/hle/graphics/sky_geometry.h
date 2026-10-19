#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

namespace title {

// Authored type-A sky geometry, converted once when the model is loaded.
// A draw owns values; it has no packet, data-memory image, or microcode entry.
class SkyGeometry {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    using Vertex = MeshGeometry::Vertex;
    using Strip = MeshGeometry::Strip;
    using Mesh = MeshGeometry::Mesh;
    struct View {
        Matrix screen;
        Matrix clip;
        Vector guard;
        std::array<float, 2> scroll{};
    };
    using Result = MeshGeometry::Result;
    static constexpr size_t kMaxVertices = 65536;
    static constexpr size_t kMaxOutputVertices = kMaxVertices * 24;

    // TR resources.cpp ModelRead's serialized submodel. The shipped sky family
    // has one fixed authored layout; this is not a general DMA/VIF interpreter.
    // The caller supplies its declared vertex count and bounds, at load time.
    // UV float mantissas also store colour bytes: both meanings are retained.
    static Result Decode(const uint8_t* bytes, size_t size, uint32_t vertices, Mesh& out);

    // The source shader's strip gate, six-plane clipping, interpolation and
    // screen projection. The full state comes from a source-owned material.
    // Clipped fans precede the surviving strip, as the source shader does.
    // Failure leaves out unchanged; success can produce no visible triangles.
    static Result Build(const Mesh& mesh, const View& view, const ps2r::gs::DrawIr& state,
                        std::vector<ps2r::gs::DrawIr>& out);
};

}  // namespace title
