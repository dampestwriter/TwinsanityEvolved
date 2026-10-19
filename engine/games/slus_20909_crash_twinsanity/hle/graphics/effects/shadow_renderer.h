#pragma once

#include <array>
#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_draw.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"

namespace title {

// Executes a whole source shadow pass at one ordered Graphics output position.
// Every intermediate is a private allocation; none is a VRAM alias or a source
// for another producer. The source scene is supplied as exact allocation leases.
class ShadowRenderer {
public:
    enum class Result { Ok, InvalidInput, InvalidScene, ResourceFailure,
                        BackendFailure, PublicationFailure, Faulted };
    struct Scene {
        // color, ordinary scene depth, destination copy
        std::array<Graphics::Lease, 3> images;
        // Called once after the final scene draw. Owns the scene's write history
        // and destination-copy invalidation. Required, even in isolated tools.
        std::function<bool(const ps2r::gs::DrawIr&, const ps2r::render::DrawResult&)> publish;
    };
    struct Counters {
        uint64_t passes = 0, strips = 0, triangles = 0, allocations = 0, failures = 0;
    };

    ShadowRenderer(ps2r::render::Backend&, GraphicsResources&);
    // The caller fences the backend before destruction/reset. All entry points
    // run on that backend's owner; recording passes never calls this object.
    ~ShadowRenderer();
    Result Draw(const ShadowDraw::Frame&, const std::vector<ps2r::gs::DrawIr>&, const Scene&);
    void ResetAfterQuiescence();
    Counters Counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
