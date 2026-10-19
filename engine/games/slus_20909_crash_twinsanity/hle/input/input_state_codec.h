#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/input/input.h"

namespace ps2r::savestate { struct Blob; }
namespace title {

class InputStateCodec {
public:
    static constexpr uint32_t kTag = 0x54504e49; // INPT, distinct from retired PADS
    static constexpr uint32_t kVersion = 2;
    static constexpr size_t kPayloadBytes = 30 + Input::kPorts * Input::kSlots * 57;
    // Pure value conversions. Success replaces out; failure leaves it intact.
    // Capture/Restore and motor delivery remain the composition owner's work.
    static bool Encode(const Input::Snapshot& snapshot, std::vector<uint8_t>& out, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, Input::Snapshot& out, std::string* error = nullptr);
    static bool Validate(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
    // Version 1 is a bounded value migration: preserve captured connection,
    // report and pending setup, discard only obsolete connection timers.
    // The next accepted host sample supplies availability.
    static bool DecodeSection(const ps2r::savestate::Blob&, Input::Snapshot&, std::string* error = nullptr);
};

}  // namespace title
