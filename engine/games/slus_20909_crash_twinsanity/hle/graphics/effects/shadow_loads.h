#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"

namespace title {

// Composable source load/destroy observers, independent of the mesh owner's model cache.
// They do not replace game allocation/stream operations or intercept devices.
class ShadowLoads {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    struct Counters {
        uint64_t reads = 0, destroys = 0, converted = 0, unselected = 0, failed = 0, interior = 0;
        // Source models this owner could not convert at load (ShadowAssets::Result
        // other than Ok/NotSelected): REFUSED, not faulted -- nothing was
        // committed (the original read ran), the model stays unowned and is
        // attributed (section 8a); a later owned mesh of it is that pass's own
        // refusal/fault.
        uint64_t refused = 0;
    };
    // Attribution sink of a load refusal: (reason, ShadowAssets::kRead, Result).
    using Refuse = std::function<void(GraphicsRefusals::Reason, uint32_t, uint32_t)>;
    ShadowLoads(ShadowAssets&, MaterialState::Read, Refuse refuse = {});
    bool Install(const Capture&, const Installer&);
    Counters Counts() const { return counts_; }
    // Source reads may yield inside the preserved file/stream body. Their
    // post-load conversion has not happened yet; never checkpoint that gap.
    bool CanCheckpoint() const { return active_ == 0 && !counts_.failed && !counts_.interior; }
private:
    ShadowAssets& assets_;
    MaterialState::Read read_;
    Refuse refuse_;
    Body read_body_, destroy_body_;
    Counters counts_;
    uint32_t active_ = 0;
    bool installed_ = false;
    void Run(bool load, CpuContext&, uint32_t entry);
};

}  // namespace title
