#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_assets.h"

namespace title {

// Portable saved converted geometry/images. No software pointers other than
// source object identity, runtime handles, retained packets or GPU allocations.
class SkyState {
public:
    static constexpr uint32_t kTag = 0x41594b53; // SKYA
    static constexpr uint32_t kVersion = 1;
    static constexpr size_t kMaxBytes = 64 * 1024 * 1024;
    static bool Encode(const SkyAssets::Snapshot&, std::vector<uint8_t>&, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>&, SkyAssets::Snapshot&, std::string* error = nullptr);
    static bool Validate(const std::vector<uint8_t>&, std::string* error = nullptr);

private:
    struct Reader;
};

}  // namespace title
