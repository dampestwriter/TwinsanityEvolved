#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_draw.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"

namespace title {

// Source shader/target settings for immediate 2D producers. Owns every active
// draw field; does not consume generated settings packets or inherited uniforms.
class PrimitiveMaterial {
public:
    static constexpr uint32_t kScreenWidth = 0x309744, kScreenHeight = 0x309748;
    using Read = MaterialState::Read;
    enum class Kind { Sprite, Text };
    enum class Result { Ok, InvalidMemory, UnsupportedMaterial, UnsupportedTarget, MissingTexture };
    struct Output {
        uint32_t bucket = 0, shader = 0;
        ps2r::gs::DrawIr draw;
        SpriteDraw::Target target;
        TextureBindings::Material sampling;
    };
    // The read callback and texture binding must observe the SAME stable RAM.
    // Context 0, source scene target, no inherited fog, complete asset sampling.
    // Scene-copy/inherited settings remain their named pass producer's owner.
    static Result Capture(const Read& read, const TextureBindings& textures, uint32_t material,
                          const MaterialState::Environment& environment, Output& out,
                          Kind kind = Kind::Sprite);
};

}  // namespace title
