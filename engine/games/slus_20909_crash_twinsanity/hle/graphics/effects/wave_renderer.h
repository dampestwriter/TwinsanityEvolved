#pragma once

#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/wave_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"

namespace title {

class WaveRenderer {
public:
    enum class Result { Ok, InvalidInput, InvalidScene, ResourceFailure,
                        BackendFailure, PublicationFailure, Faulted };
    struct Scene {
        std::array<Graphics::Lease, 3> images; // color, depth, destination copy
        std::function<bool(const ps2r::gs::DrawIr&, const ps2r::render::DrawResult&)> publish;
    };
    struct Counters { uint64_t passes = 0, strips = 0, triangles = 0, allocations = 0, failures = 0; };

    WaveRenderer(ps2r::render::Backend&, GraphicsResources&);
    ~WaveRenderer();
    // Ordered backend owner, with source grid/table captured before delivery.
    // Copy and deformation finish as one operation; no source address lookup.
    Result Draw(uint32_t frame, const WaveGeometry::Grid&, const WaveGeometry::Waves&, const Scene&);
    void ResetAfterQuiescence();
    Counters Counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
