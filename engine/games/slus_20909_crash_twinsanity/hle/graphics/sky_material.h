#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_assets.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_setup.h"

namespace title {

class SkyMaterial {
public:
    static constexpr uint32_t kVtable = 0x002fa948;
    struct Output {
        ps2r::gs::DrawIr state;
        SkyAssets::TextureLease texture;
        std::array<float, 2> scroll{};
    };
    enum class Result { Ok, InvalidMemory, UnsupportedMaterial, MissingTexture };
    // One type-A shader in bucket 0, as enumerated in the authored sky assets.
    // Values come from software objects; register/packet pointers are ignored.
    static Result Capture(const MaterialState::Read& read, uint32_t material,
                          const SkySetup::Output& setup, const SkyAssets& assets, Output& out);
};

}  // namespace title
