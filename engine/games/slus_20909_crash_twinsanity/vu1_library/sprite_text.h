#pragma once
// vs_sprite_text: the glyph / text typesetter entered directly at 0x1748
// (and re-entered through the effect-free 0x1980 MSCNT branch-back).
//
// The class-B zero-kick stub of the same image pre-loads vi13 = the
// double-buffered out-base pointer list, vi12 = the buffer phase, vf01 = the
// position offset and vf02 = the GIFtag template; this body consumes those
// ENTRY registers plus TOP data: QW[TOP] = RGBAQ template, QW[TOP+1].xy =
// glyph scale, low16(QW[TOP+1].z) = the outer entry count, list A at TOP+2
// = per-string {pos.xy, group count .z}, list B after it = glyph-index QWs
// (4 glyphs per QW, consumed lane by lane via MR32 + MTIR & 255). Per glyph
// (record QW[54+idx] = {u0, v0, size}): vertex A = the cursor (.z = R =
// 1.0, .w = MINI(1, 0) = +0), vertex B = cursor + size * scale, UV1 =
// {u0 + w, v0 - h}; five stores {UV0, RGBAQ, XYZ2 A (the same-bundle OLD
// vf09), UV1, XYZ2 B}; the cursor advances and the out pointer steps by the
// per-class stride QW[vi14 + (low16(u0 bits) & 3) + 5].x while the packet
// budget (20) drops by .y of that row. Budget exhausted or data consumed:
// GIFtag word0 = sext16(0x8000 | (20 - remaining)) into the ENTRY vf02,
// stored at the packet base, XGKICK, phase toggle, next base from
// QW[vi13 + phase].x. Full register file reproduced (has_regs; the phase and
// the patched vf02.x persist into the next MSCAL), MAC pushes in program
// order; TPC 0x1980.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class SpriteText {
public:
    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out);
};
