#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/input/input.h"

namespace title {

class InputLegacyImport {
public:
    // One-time NTSC migration, before any live restore. RAM0 is the original
    // 32 MiB payload, not live RAM. The caller must prove source_boundary:
    // OLD PADS uses CNTR's SyncV u32 at +0x1c; current NEW uses the saved
    // domain boundary. A presentation frame is not interchangeable.
    // No model, I/O, callback, or destination mutation on failure.
    static bool Decode(uint32_t pads_version, const std::vector<uint8_t>& pads,
                       const std::vector<uint8_t>& ram0, uint64_t source_boundary,
                       Input::Snapshot& out, std::string* error = nullptr);
};

}  // namespace title
