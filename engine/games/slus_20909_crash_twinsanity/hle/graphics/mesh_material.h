#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

class MeshMaterial {
public:
    static constexpr uint32_t kVtable = 0x002faec8;
    // Supplied by the load-time texture owner (TextureAssets). No texture
    // conversion, source-slot cache, or address-based lookup lives here.
    struct Texture {
        Graphics::Handle handle;
        Graphics::Lease lease;
        uint32_t width = 0, height = 0, buffer_width = 0;
        uint8_t levels = 0, source_format = 0, function = 0;
        bool alpha = false;
    };
    using Lookup = std::function<bool(uint32_t object, Texture&)>;
    struct Output {
        ps2r::gs::DrawIr state;
        Texture texture;
        std::array<float, 2> scroll{};
        uint32_t bucket = 0;
    };
    enum class Result { Ok, InvalidMemory, UnsupportedMaterial, UnsupportedTarget, MissingTexture };
    // Full single type01, textured STQ/Gouraud, standard scene target.
    // Called at FlushMaterial (or a direct material's first source draw), not
    // when its instances are collected: scrolling/settings can change later.
    static Result Capture(const MaterialState::Read& read, uint32_t material,
                          const MaterialState::Environment& environment,
                          const Lookup& texture, Output& out);
};
}  // namespace title
