#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"

namespace title {

// Converted owned bytes only: no renderer IDs, pointers, guest transfers,
// inferred VRAM contents, or diagnostic counters. Root owns section selection,
// incoming RAM validation, quiescence and publication of the complete restore.
class TextureStateCodec {
public:
    static constexpr uint32_t kTag = 0x53415854; // TXAS
    static constexpr uint32_t kVersion = 1;
    // Matches the existing state-file section envelope. Larger captures refuse.
    static constexpr size_t kMaxPayloadBytes = 64 * 1024 * 1024;
    static bool Encode(const TextureAssets::Snapshot& snapshot, std::vector<uint8_t>& out,
                       std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, TextureAssets::Snapshot& out,
                       std::string* error = nullptr);
    static bool Validate(const std::vector<uint8_t>& bytes, std::string* error = nullptr);

private:
    struct Reader;
};

}  // namespace title
