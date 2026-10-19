#pragma once
// vs_lit_tristrip: the lit textured tristrip emitter -- per-vertex
// normal-matrix lighting modulating static vertex colours, texcoords from the
// vertex stream. Per mesh: object rows QW[m+1..4], normal rows QW[m+5..7],
// light rows QW[m+9..12]; the gate flag QW[m].x selects the clip gate; every
// vertex pairs with its own quotient: T = obj * pos, wn = nrm * n,
// lit = L3 + L0*max(wn.x,0) + L1*max(wn.y,0) + L2*max(wn.z,0),
// RGBA = FTOI0(MINI(ITOF0(col) * {lit, 1}, 255)), ST = (st.xyz + payload) * Q,
// XYZ2 = {FTOI0(T*Q).xyz, sext16((low16(pos.w) & flag) | low16(st.w))}.
// The mesh walk is data-driven (terminates on its own sentinel) under the
// ring safety bound. One image additionally carries an env-map second sub.
// RunLitCall is shared with the crystal programs, which carry a relocated
// copy of this body under a different gate link.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class LitTristrip {
public:
    static constexpr vu1lib::u32 kGateLink = 0x2F9;  // (gate JALR pc + 16) / 8 of this body

    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out);

    // One emitter call (one dispatch node); false = decline. `gate_link` is
    // the JALR link the body's gate call spills; `gr` = nullptr declines full
    // near clips.
    static bool RunLitCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, Vu1HleOutput& out,
                           vu1lib::u32 gate_link = kGateLink, ClipGate::GateRegs* gr = nullptr);
    // The env-map second material sub of the pinned image (static colour at
    // QW[4v+3]; gate link of its own).
    static bool RunEnv2Call(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, Vu1HleOutput& out);
};
