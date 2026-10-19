#pragma once

#include <array>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// The source distortion particle intent, including its complete hexagon
// expansion. All values are captured before submission; no packet or model
// state is consulted while building geometry.
class DistortionGeometry {
public:
    struct Particle {
        std::array<float, 3> start{};
        float spawn_time = 0;
        std::array<float, 3> velocity{};
        float life_scale = 0;
    };
    struct Look {
        float gravity = 0;
        std::array<std::array<float, 2>, 6> corners{};
        // Source WriteParticleRenderTable overlays RGBA in the low byte of
        // shape[0..3]. These are the resulting authored float values, including
        // those bits. Colours are taken from the current step without blending.
        std::array<std::array<float, 6>, 64> shapes{};
    };
    struct View {
        std::array<std::array<float, 4>, 4> screen{};
        float time = 0;
        std::array<float, 2> distortion{};
        uint32_t width = 512, height = 448;
    };
    using Block = std::array<Particle, 12>;
    enum class Result { Ok, InvalidDimensions, InvalidValues, UnsupportedState, LimitExceeded };
    static constexpr size_t kMaxBlocks = 4096;
    // Source order: blocks, four batches of three particles, then the two
    // restarted strips per particle. Invalid/expired particles emit nothing.
    // Errors leave output unchanged; an empty successful result is actual
    // source culling, not a successful rendering placeholder.
    static Result Build(const std::vector<Block>&, const Look&, const View&,
                        const ps2r::gs::DrawIr& material, std::vector<ps2r::gs::DrawIr>& out);
};

}  // namespace title
