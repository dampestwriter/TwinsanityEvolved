#pragma once

#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"

namespace title {

class DistortionRenderer {
public:
    struct Frame { uint32_t source_frame = 0, width = 512, height = 448; };
    struct Scene {
        std::array<Graphics::Lease, 3> images; // color, depth, destination copy
        std::function<bool(const ps2r::gs::DrawIr&, const ps2r::render::DrawResult&)> publish;
    };
    struct Counters {
        uint64_t passes = 0, copies = 0, batches = 0, triangles = 0, allocations = 0, failures = 0;
    };
    enum class Result { Ok, InvalidInput, InvalidScene, ResourceFailure,
                        BackendFailure, PublicationFailure, Faulted };
    DistortionRenderer(ps2r::render::Backend&, GraphicsResources&);
    ~DistortionRenderer();
    // A whole source material pass: exactly one captured scene copy, followed
    // by every ordered hexagon batch. No image is resolved by source address.
    static bool Valid(const Frame&, const std::vector<ps2r::gs::DrawIr>&);
    Result Draw(const Frame&, const MaterialState::Environment&,
                const std::vector<ps2r::gs::DrawIr>&, const Scene&);
    void ResetAfterQuiescence();
    Counters Counts() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
