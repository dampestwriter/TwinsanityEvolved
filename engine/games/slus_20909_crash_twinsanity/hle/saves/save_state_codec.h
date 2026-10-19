#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"

namespace title {

class SaveStateCodec {
public:
    static constexpr uint32_t kTag = 0x54535653; // SVST, distinct from retired MCHL
    static constexpr uint32_t kVersion = 1;
    static constexpr size_t kMaxPayloadBytes = SaveStorage::kMaxBytes + 1024;
    // Encode only a successful SaveBindings::Capture. Pending mutations defer
    // capture; no codec function waits, commits, opens files or calls the game.
    // Success replaces out; rejection leaves it intact.
    static bool Encode(const SaveBindings::Snapshot& snapshot, std::vector<uint8_t>& out, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, SaveBindings::Snapshot& out, std::string* error = nullptr);
    static bool Validate(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
};

}  // namespace title
