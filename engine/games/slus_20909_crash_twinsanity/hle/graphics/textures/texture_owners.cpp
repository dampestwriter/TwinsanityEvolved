#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_owners.h"

namespace title {

const TextureOwners::Entry* TextureOwners::Find(uint32_t texture) {
    for (const auto& entry : kStartup)
        if (entry.texture == texture) return &entry;
    return nullptr;
}

}  // namespace title
