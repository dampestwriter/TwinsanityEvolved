#pragma once
// The family near clipper (the full interpolating path the clip gate JALRs
// when a guard-band hit has the vertex's bit-15 clear) and the four
// body-variant fan emitters it dispatches to. Every memory side effect is
// reproduced in program order: the register spill to QW11-13 / QW14-37, the
// 6-plane Sutherland-Hodgman clip in the ping-pong buffers QW39-58 / QW59-78,
// the prelude kick, the fan packet at QW1.y with its NLOOP patch, the fan
// kick. Registers themselves are restored by the microcode and never
// compared. The fan entry is data-driven (QW[node].y) and must be the one
// the calling body accepts, else the run DECLINES.
#include <cstdint>

#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class NearClipper {
public:
    static constexpr vu1lib::u32 kEntryPc = 0x440;  // the family clipper's pc (QW5.y * 8)

    // j / s_hist / g_hist = the gate-loop state at the JALR site (vertex j's
    // .w is not yet finalised in memory).
    static bool Run(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, const vu1lib::V4 clipR[5], vu1lib::u32 j,
                    vu1lib::u32 nverts, const vu1lib::u32 s_hist[3], const vu1lib::u32 g_hist[3],
                    const ClipGate::GateRegs& gr, Vu1HleOutput& out);

    // fan_kind 0: the textured / pos-color fan (interpolated colour + STQ).
    static bool EmitClipFan(QwPtr m, vu1lib::u32 node, vu1lib::u32 vi06, vu1lib::u32 fan_beg, vu1lib::u32 fan_end,
                            vu1lib::u32 stale_w, const ClipGate::GateRegs& gr, Vu1HleOutput& out);
    // fan_kind 1: the lit fan (corner colours recomputed by the lighting chain).
    static bool EmitLitClipFan(QwPtr m, vu1lib::u32 node, vu1lib::u32 vi06, vu1lib::u32 fan_beg,
                               vu1lib::u32 fan_end, const ClipGate::GateRegs& gr, Vu1HleOutput& out);
    // fan_kind 2: the family fan (backface prefilter + flat node colour).
    static bool EmitFamClipFan(QwPtr m, vu1lib::u32 node, vu1lib::u32 vi06, vu1lib::u32 fan_beg,
                               vu1lib::u32 fan_end, const ClipGate::GateRegs& gr, Vu1HleOutput& out);
    // fan_kind 3: the crystal env-lit fan.
    static bool EmitCrysClipFan(QwPtr m, vu1lib::u32 node, vu1lib::u32 vi06, vu1lib::u32 fan_beg,
                                vu1lib::u32 fan_end, const ClipGate::GateRegs& gr, Vu1HleOutput& out);
};
