#pragma once
// FixesComposition -- the composition root's install of the RETAIL_BUGS fixes
//The title RetailFixes selection from Config fixes.* (play
// mode only; every toggle off = nothing captured or installed), bound to the
// DispatchTable and main memory. One object in InstallDispatch; the census row
// `retail_fixes=` is its.
#include "games/slus_20909_crash_twinsanity/hle/fixes/retail_fixes.h"

namespace ps2r { class Config; }

namespace ps2r::app {

class FixesComposition {
public:
    FixesComposition(const Config& cfg, bool play_mode);
    bool Refused() const { return refused_; }
    ::title::RetailFixes* Fixes() { return &fixes_; }

private:
    ::title::RetailFixes fixes_;
    bool refused_ = false;
};

}  // namespace ps2r::app
