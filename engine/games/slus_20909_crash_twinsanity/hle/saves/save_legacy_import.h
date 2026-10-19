#pragma once

#include <string>
#include <vector>

#include "engine/core/savestate/savestate_format.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"

namespace title {

class SaveLegacyImport {
public:
    // Pure NTSC idle-only migration from the original unfiltered image.
    // Requires RAM0, MCHL, SIFS and a complete THRD owner/continuation census.
    // Caller must establish legacy producer/title provenance first.
    // Rejects pending effects, ambiguous observations and absent owner state.
    // No callbacks, model execution, filesystem I/O or output change on failure.
    static bool Decode(const std::vector<ps2r::savestate::Blob>& original,
                       SaveBindings::Snapshot& out, std::string* error = nullptr);
};

}  // namespace title
