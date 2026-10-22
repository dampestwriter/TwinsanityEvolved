#pragma once
// vs_skinned_lit_strip: the skinned lit strip family (one program serving
// every entry pc of its images).
//
// Entry 0: the dispatcher node walk. Preprocessor subs rewrite the vertex
// stream at TOP in place: 0xcb0 = the int->float scale pass (RunScalePhase)
// fused with a 3-bone palette skin (weights QW[v+2].xyz, palette indices =
// low 9 bits of the weight lanes, influence count = low 2 bits of .w; bones
// at QW[idx+13..16]); 0x1040 = the float-input skin (different
// accumulation order, raw-bit stores); 0xb30 = the guard-band clip mark
// (rows QW[head+89..93], 3-vertex window OR into bit15 of QW[v+2].w). The
// 0x1720 material sub is a single lit strip (rows QW[f+78..88], per-vertex
// own quotient, RGBA = FTOI0(MINI(ITOF0(col) * lit, 255)), ST = (uv +
// payload) * Q, XYZ2 = {FTOI0(T*Q), nrm.w raw}); QW1 rotates BEFORE both
// (unconditional) kicks. One image carries the 0x1aa0 specular / env second
// sub. No registers reproduced.
// Entry 0x12a0: the scale pass alone (data writes only). Entries 0x1400 /
// 0x15a0: the shared-prefix displace passes (DisplacePrefix, has_regs).
// Entry 0x90: the E-bit stub -- registers pass through, TPC = family exit.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class SkinnedLitStrip {
public:
    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out);

    // The int->float scale pass (also the crystal programs' 0x12a0 entry).
    // False when the micro loop could not terminate (count 0 or not a
    // multiple of 4). `stale_w_out` (optional) receives the raw .w bits the
    // pipelined tail load leaves in vf19 at loop exit.
    static bool RunScalePhase(QwPtr m, vu1lib::u32 top, vu1lib::u32* stale_w_out);

    // One 0xcb0 fused scale + skin node.
    static bool RunSkinCb0(QwPtr m, vu1lib::u32 top);
    // One 0x1040 float-input skin node; `entry_vf19_w` = vf19.w at entry.
    static bool RunSkin1040(QwPtr m, vu1lib::u32 top, vu1lib::u32 entry_vf19_w);
    // One 0xb30 guard-band clip-mark node.
    static bool RunClipMark0B30(QwPtr m, vu1lib::u32 top);
    // One 0x1720 lit-strip emitter call.
    static bool RunSkinnedLitCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, Vu1HleOutput& out);
    // Its body alone: the strip packet written to the output list in memory, QW1 not rotated, nothing kicked (a caller
    // that reads the vertices back from memory)
    static bool RunSkinnedLitBody(QwPtr m, vu1lib::u32 node, vu1lib::u32 top);
    // One 0x1aa0 specular / env-mapped second-material call (pinned image).
    static bool RunSkinEnv1aa0(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, Vu1HleOutput& out);
};
