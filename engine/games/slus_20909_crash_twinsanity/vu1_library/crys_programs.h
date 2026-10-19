#pragma once
// The crystal programs: the dual-material dispatcher image and the direct
// displace entries of its sibling images.
//
// vs_crys_dual (entry 0): the dispatcher node walk over two material subs.
// The 0x1720 sub (RunCrysEnv1720) is a lit strip whose ST channel is
// screen-space env-mapped: scr = MULq(Obj * pos), st = (scr.xy - P3.xy) *
// P4.xy, stp = StMtx * nrm (rows QW[mesh+clip_off..+2], MULAz/MADDAy/MADDx),
// st = CF(st*1) + stp * P3.z, st = MAXx(MINIw(st, 1), 0), st.z = 1, st.xy +=
// P2.xy, st = MULq(st) with P2/P3/P4 = QW[node+2..4]; RGBA from the two-matrix
// lighting chain; XYZ2 = {FTOI0(scr), sext16((low16(pos.w) & flag) |
// low16(attr.w))}; node header kick at node+5 when node.w != 0, then the
// strip; QW1 rotates. Its clip gate spills link 0x306 and its near clipper
// dispatches to the crystal fan (NearClipper::EmitCrysClipFan). The 0x21b0
// sub is the relocated lit body (LitTristrip::RunLitCall under gate link
// 0x44B, the relocated lit fan). The mesh walk is data-driven (terminates
// on its own sentinel) under the ring safety bound; a gate-flagged mesh runs
// the lifted gate (no pre-scan decline). No registers reproduced.
//
// vs_crys_displace: 0x12a0 = the shared-prefix scale pass
// (SkinnedLitStrip::RunScalePhase, data writes only); 0x1400 / 0x15a0 = the
// shared-prefix displace passes (DisplacePrefix, has_regs); else decline.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class CrysPrograms {
public:
    static bool RunDual(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunDisplace(const Vu1HleInput& in, Vu1HleOutput& out);

    // One 0x1720 env-lit emitter call (one dispatch node); false = decline.
    // `gr` threads the gate-persistent register file across the walk and
    // receives this call's loop-end leftovers (vf16 = P3, vf18 = QW[top],
    // vf27 = the last vertex's stp chain); nullptr declines full near clips.
    static bool RunCrysEnv1720(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, Vu1HleOutput& out,
                               ClipGate::GateRegs* gr = nullptr);
};
