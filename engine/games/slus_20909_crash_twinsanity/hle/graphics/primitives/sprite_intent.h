#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/primitive_material.h"

namespace title {

class SpriteIntent {
public:
    static constexpr uint32_t kPlain = 0x1a8550, kColoured = 0x1a87a0;
    static constexpr uint32_t kPlaced = 0x1abeb0, kPlacedColoured = 0x1a89e0;
    enum class Result { Ok, UnsupportedEntry, InvalidMemory, UnsupportedOwner, InvalidMaterial, InvalidGeometry, Empty };
    struct Output {
        uint32_t bucket = 0;
        ps2r::gs::DrawIr draw;
        TextureBindings::Material sampling;
    };
    // Exact complete Sprite::Draw boundary; a0 Sprite, a1 colour or matrix,
    // a2 colour for PlacedColoured. The source default colour is read through
    // its actual table. No guest writes, allocations, or instruction execution.
    // Empty publishes the same complete material/texture capture with no
    // triangles, so the root can validate its source target without GPU work.
    static Result Capture(const PrimitiveMaterial::Read& read, const TextureBindings& textures,
                          uint32_t entry, uint32_t a0, uint32_t a1, uint32_t a2,
                          const MaterialState::Environment& environment, Output& out);
};

}  // namespace title
