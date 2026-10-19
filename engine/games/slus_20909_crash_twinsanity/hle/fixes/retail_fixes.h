#pragma once
// RetailFixes -- optional patches of retail defects the decomp documents
//Each fix is one Config toggle,
// DEFAULT OFF (the title as shipped), listed on the launcher's 'Fixes' page
// with its citation. A fix is an owned body installed over the title's own
// function (DispatchTable): the title's body runs to completion unchanged,
// then the words the retail code forgot to initialise are written -- never a
// hook on gameplay, never a change to what the title's body computes.
//
//   look_rest_seconds  LookController::Construct (PAL FUN_00147310 -> NTSC
//                      FUN_001473a0, verified: +0 vtable 0x2f3230, +4 agent,
//                      +0x3c/+0x40/+0x48/+0x4c the pitch/yaw limits, Reset
//                      0x147410, +0x44 carryPitchMax) never stores restSeconds
//                      (+0x60): the new look rests for the heap's leftover
//                      float. Fix: restSeconds = 0.0f after the constructor.
//   skid_mark_init     ConstructSkidMarks (PAL FUN_00161ac0 -> NTSC
//                      FUN_00161b50, verified: memset(+0x38, 0x190), +0x1c8
//                      next, +0 active, +0x1d0 length, +0x1cc idleFrames)
//                      never sets lastPoint (+0x10) or edgeColour/lowColour
//                      (+0x30/+0x34): the first strip is coloured from heap
//                      leftovers and measured from a garbage point. Fix:
//                      lastPoint = (0,0,0,1), edgeColour = 0x80FFFFFF (white),
//                      lowColour = 0x00FFFFFF (the values a strip leaves).
#include <cstdint>
#include <functional>
#include <string>

#include "engine/ee/cpu/cpu_state.h"

namespace title {

class RetailFixes {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t addr)>;
    using Installer = std::function<bool(uint32_t addr, Body body)>;
    using Write32 = std::function<void(uint32_t addr, uint32_t value)>;

    struct Selection {
        bool look_rest_seconds = false;
        bool skid_mark_init = false;
    };
    struct Counts {
        uint64_t look_constructs = 0;   // fixes applied
        uint64_t skid_constructs = 0;
        int installed = 0;
    };

    static constexpr uint32_t kLookControllerConstruct = 0x001473a0u;
    static constexpr uint32_t kConstructSkidMarks = 0x00161b50u;
    static constexpr uint32_t kRestSecondsOffset = 0x60;
    static constexpr uint32_t kLastPointOffset = 0x10;
    static constexpr uint32_t kEdgeColourOffset = 0x30;
    static constexpr uint32_t kLowColourOffset = 0x34;

    RetailFixes(Selection selection, Write32 write32);
    // Installs the selected fixes; false when a selected install was refused.
    // Nothing selected = nothing installed (true).
    bool Install(const Capture& capture, const Installer& install);
    const Counts& Count() const { return counts_; }
    // run.meta row: `retail_fixes=look_rest_seconds=on,applied=N;skid_mark_init=off`.
    std::string Census() const;

private:
    Selection selection_;
    Write32 write32_;
    Counts counts_;
};

}  // namespace title
