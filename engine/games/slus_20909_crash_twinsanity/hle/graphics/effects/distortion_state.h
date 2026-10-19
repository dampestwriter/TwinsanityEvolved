#pragma once

#include "engine/core/savestate/savestate_format.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_library.h"

namespace title {

// One selection-bearing section for the distortion owner's value state
// (block-list ownership and the settings version). All checks precede RAM
// writes; a model-only historical file may omit it, never a selected one.
class DistortionState {
public:
    static constexpr uint32_t kTag = 0x4e545344; // DSTN
    static constexpr uint32_t kVersion = 1;
    static bool Encode(const DistortionLibrary::Snapshot&, std::vector<uint8_t>&);
    static bool Decode(const std::vector<uint8_t>&, DistortionLibrary::Snapshot&);
    // Takes the UNFILTERED image. Rejects duplicate/invalid sections, selection
    // differences and selected state without its required payload; binds the
    // incoming RAM0 reader the library's Stage validates against.
    static bool Preflight(const std::vector<ps2r::savestate::Blob>&, bool selected,
                          DistortionLibrary::Snapshot&, MaterialState::Read& incoming,
                          std::string* error = nullptr);
};

}  // namespace title
