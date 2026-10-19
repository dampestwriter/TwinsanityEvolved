#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// Type-0B shadow volumes. All data is load-time geometry or source instance
// values; no register file, packet, program image or hardware owner is needed.
class ShadowGeometry {
public:
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;
    struct Vertex {
        std::array<float, 3> position{};
        bool restart = false;
        // Source corner's orientation flag (bit 5 of its authored normal.x).
        // The clipped fan uses this flag; the surviving strip uses vertex parity.
        bool reverse_fan = false;
    };
    struct Strip { std::vector<Vertex> vertices; };
    struct Mesh { std::vector<Strip> strips; };
    struct View {
        Matrix screen{}, clip{};
        Vector guard{};
        std::array<float, 2> offset{};
    };
    enum class Result { Ok, InvalidAsset, InvalidGeometry, LimitExceeded };
    static constexpr size_t kMaxVertices = 65536;
    static constexpr size_t kMaxOutputVertices = 2 * 24 * kMaxVertices;
    // ModelRead's authored part record, decoded once while its bytes are owned.
    // Type 0B requires the normal stream even though it does not light vertices:
    // normal.x bit 5 selects the clipped fan's orientation. Failure is atomic.
    static Result Decode(const uint8_t*, size_t, uint32_t vertices, Mesh&);
    // Each source strip visits the dark node's instances, then the light
    // node's instances. Fans precede that node/instance's surviving strip.
    // Keep output groups intact: the pass counts each strip/fan only once.
    static Result Build(const Mesh&, const std::vector<View>&,
                        std::vector<ps2r::gs::DrawIr>& out);
};

}  // namespace title
