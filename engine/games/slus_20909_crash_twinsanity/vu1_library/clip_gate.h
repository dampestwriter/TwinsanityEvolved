#pragma once
// The family's shared clip gate (the subroutine every emitter body JALRs
// before its vertex loop) and the register state the near clipper's spill
// needs from the calling body.
//
// Gate rule: for v in [0, nverts): clip-space p = clipR3 + R0*x + R1*y + R2*z,
// guard g = R4*p; the CLIP judgements of the trailing vertex triple combine --
// strict AND != 0 marks the vertex ADC (the whole triangle is off one plane);
// guard OR != 0 runs the near clipper (the vertex is ADC-marked by the call's
// delay slot either way); otherwise QW[v].w takes QW[v+1].w raw. The gate
// spills {node, dispatcher link, its own link} to QW10 first. A near clip on
// an image whose fan body is not identity-verified (GateRegs.valid false)
// DECLINES the whole run.
#include <cstdint>

#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class ClipGate {
public:
    // Values the near-clipper spill needs that are not derivable from data
    // memory at the JALR site: the pass-through vf16/vf18, the gate's
    // rotating window registers vf27..vf30 (persist across gate calls), the
    // body-owned vi09/vi13, the current object rows, and the one fan emitter
    // entry this body accepts.
    struct GateRegs {
        vu1lib::V4 vf16{}, vf18{};
        vu1lib::V4 vf27{}, vf28{}, vf29{}, vf30{};
        vu1lib::u16 vi09 = 0, vi13 = 0;
        vu1lib::V4 obj[4] = {};
        vu1lib::u32 fan_pc = 0;
        // Which fan emitter body serves fan_pc: 0 = the textured/pos-color
        // fan, 1 = the lit fan, 2 = the family fan, 3 = the crystal env fan.
        int fan_kind = 0;
        bool valid = false;
    };

    static GateRegs SeedGateRegs(const Vu1HleInput& in, vu1lib::u32 fan_pc, bool image_ok);

    // Largest v <= limit with v % 3 == c and v >= minv; -1 when none.
    static int LatestOfClass(int limit, int c, int minv);

    static void UpdateGateRegsAtEnd(QwPtr m, vu1lib::u32 top, const vu1lib::V4 clipR[5], vu1lib::u32 nverts,
                                    GateRegs& gr);

    // link_vi12 = the JALR link the caller's gate call leaves in vi12.
    static bool ClipGateB0(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, vu1lib::u32 link_vi12,
                           const vu1lib::V4 clipR[5], vu1lib::u32 nverts, GateRegs* gr = nullptr,
                           Vu1HleOutput* out = nullptr);
};
