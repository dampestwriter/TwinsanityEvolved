#pragma once
// vs_indexed_displace: the indexed-palette displacement tristrip emitter.
// Two parameterizations of one body: broadcast (the payload's .w scales
// every lane; gate link 0x335) and componentwise (QW[node+3].xyz scales per
// lane; gate link 0x336).
//
// The first 0x1720 call of an MSCAL (the dispatcher's QW9.x beacon is set)
// runs the pre-pass over every vertex: scaled = pos.xyz * I (I ~ 1/2681),
// idx.L = low4(low16(bits(scaled.L))) per lane, gather.L = QW[node+3+idx.L].L,
// pos.xyz = pos + gather * scale (one-deep pending store: result j-1 lands
// after gather j); then clears the beacon. Per mesh (list QW2.x, sentinel
// QW[mesh0].y): object rows QW[m+1..4]; the gate flag QW[m].x selects the
// clip gate over rows QW[m+z..z+4]; per vertex T = obj * pos, own Q,
// ST = (aux.xyz + payload) * Q, RGBAQ = aux raw, XYZ2 = {FTOI0(T*Q),
// sext16((low16(pos.w) & flag) | low16(aux.w))}; node packet then main kick;
// QW1 rotation. No registers reproduced.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class IndexedDisplace {
public:
    // The two registered parameterizations.
    static bool RunBroadcast(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunComponentwise(const Vu1HleInput& in, Vu1HleOutput& out);

    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out, bool componentwise, vu1lib::u32 gate_link);

    // The QW9.x-gated pre-pass.
    static void RunIndexPrepass(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, vu1lib::u32 nverts,
                                bool componentwise);
    // One 0x1720 emitter call (one dispatch node); false = decline.
    static bool RunIdxCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, bool componentwise,
                           vu1lib::u32 gate_link, Vu1HleOutput& out, ClipGate::GateRegs* gr = nullptr);
};
