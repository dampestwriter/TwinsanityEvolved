#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets.h"

namespace title {

// Pure value payload for the eventual effects participant. This magic is an inner
// format marker, not a separately registered savestate section or selection.
class ShadowAssetsCodec {
public:
    static constexpr uint32_t kMagic = 0x53414853; // SHAS
    static constexpr size_t kMaxBytes = 20 + ShadowAssets::kMaxCurrent * (24 + 4 * 1024 * 1024);
    static bool Encode(const ShadowAssets::Snapshot&, std::vector<uint8_t>&);
    static bool Decode(const std::vector<uint8_t>&, ShadowAssets::Snapshot&);
};

}  // namespace title
