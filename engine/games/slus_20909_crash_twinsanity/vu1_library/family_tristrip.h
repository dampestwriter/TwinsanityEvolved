#pragma once
// vs_family_tristrip: the family fast-path tristrip emitter (the shared
// per-material subroutine at the variant base of the base image family).
// The dispatcher walks the node list at QW3.x; each emitter call walks the
// mesh list at QW2.x; per mesh: object rows QW[m+1..4], clip rows
// QW[m+z..z+4], the clip gate, then every vertex (QW[TOP+3+4i]) is
// transformed, perspective-divided, z-negated, FTOI0'd and stored as
// {RGBAQ = node colour, XYZ2} into the pre-built packet at QW1.x with a
// screen-space cross-product backface test forcing ADC on culled vertices;
// the packet is kicked and QW1 is rotated. No registers are reproduced.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class FamilyTristrip {
public:
    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out);

    // One emitter invocation (one dispatch node); false = decline.
    static bool RunEmitterCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, Vu1HleOutput& out,
                               ClipGate::GateRegs* gr = nullptr);
};
