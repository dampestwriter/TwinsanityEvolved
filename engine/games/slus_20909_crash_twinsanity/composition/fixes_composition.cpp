#include "games/slus_20909_crash_twinsanity/composition/fixes_composition.h"

#include <utility>

#include "engine/core/config/config.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"

namespace ps2r::app {

FixesComposition::FixesComposition(const Config& cfg, bool play_mode)
    : fixes_(::title::RetailFixes::Selection{play_mode && cfg.fixes.look_rest_seconds, play_mode && cfg.fixes.skid_mark_init},
             [](uint32_t a, uint32_t v) { memory::write<uint32_t>(a, v); }) {
    refused_ = !fixes_.Install([](uint32_t a) { return DispatchTable::Capture(a); },
                               [](uint32_t a, ::title::RetailFixes::Body b) { return DispatchTable::Install(a, std::move(b)); });
}

}  // namespace ps2r::app
