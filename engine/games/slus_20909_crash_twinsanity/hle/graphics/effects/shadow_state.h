#pragma once

#include "engine/core/savestate/savestate_format.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_library.h"

namespace title {

// One selection-bearing section: asset bytes cannot be detached from the
// selected owner by stripping a separate marker. All checks precede RAM writes.
class ShadowState {
public:
    static constexpr uint32_t kTag = 0x57444853; // SHDW
    static constexpr uint32_t kVersion = 1;
    static bool Encode(const ShadowLibrary::Snapshot&, std::vector<uint8_t>&);
    static bool Decode(const std::vector<uint8_t>&, ShadowLibrary::Snapshot&);
    // Takes the UNFILTERED image. Reject duplicate/invalid sections, selection
    // differences and selected state without its required payload. Model-only
    // historical files may omit SHDW; this never imports a selected owner.
    static bool Preflight(const std::vector<ps2r::savestate::Blob>&, bool selected,
                          ShadowLibrary::Snapshot&, MaterialState::Read& incoming,
                          std::string* error = nullptr);
};

}  // namespace title
