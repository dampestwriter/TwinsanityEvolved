#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

namespace title {

// Complete source intent boundaries in models.cpp. The game's visibility,
// camera, and lighting decisions remain with their callers.
class MeshIntent {
public:
    static constexpr uint32_t kRigid = 0x001bd000, kPlaced = 0x001be7f0;
    static constexpr uint32_t kDefault = 0x001be9f8;
    static constexpr uint32_t kClose = 0x0019c638, kFlushMaterial = 0x001ba3a8;
    static constexpr uint32_t kRenderView = 0x00309840;
    enum class Kind { Rigid, Placed };
    struct Output {
        uint32_t object = 0, geometry = 0;
        std::vector<uint32_t> materials;
        MeshGeometry::View view;
        Kind kind = Kind::Placed;
        bool direct = false;
        // Software batching units from models.cpp. They determine instance
        // grouping and therefore draw order; no device buffer is allocated.
        uint32_t size = 0;
    };
    enum class Result { Ok, InvalidMemory, Unsupported, InvalidGeometry };
    // Declines billboards and eye-dependent instances; their game transforms
    // require separate source families. Type01 does not consume rigid lights.
    // No source object, allocation, packet, or register file is modified.
    static Result Capture(const MaterialState::Read& read, uint32_t boundary,
                          const CpuContext& context, Output& out);
};

}  // namespace title
