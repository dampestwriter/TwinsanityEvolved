#pragma once
// vs_poscolor_strip: the flat pos+colour tristrip emitter -- ONE strip per
// dispatch node (no mesh loop). The matrix base is the FIRST node of the
// dispatch list (QW3.x): object rows QW[base+78..81], clip rows
// QW[base+82..86]; the clip gate runs unconditionally; every vertex pairs
// with its OWN quotient; ADC = sext16(low16(pos.w post-gate) | low16(colour.w));
// output stride 3 {ST = (colour.xyz + payload.xyz) * Q, RGBAQ = colour raw,
// XYZ2 = {FTOI0(T*Q).xyz, ADC}} (no z-negate). Memory-op order per iteration
// is the microcode's (loads of v_{i+2} first, then XYZ2 v_i / ST v_i /
// RGBAQ v_{i+2}) so any buffer aliasing behaves identically. A dispatch into
// the zero-filled tail past the image extent (a stale node) ends the walk.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class PoscolorStrip {
public:
    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out);

    // One emitter call (one dispatch node); false = decline.
    static bool RunPosColorCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, ClipGate::GateRegs& gr,
                                Vu1HleOutput& out);
};
