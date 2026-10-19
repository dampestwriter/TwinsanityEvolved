#pragma once
// vs_textured_tristrip: the textured tristrip emitter with per-vertex colour,
// perspective STQ and a conditional node-embedded state packet. Per mesh the
// gate flag QW[m].x selects the GATED path (clip gate, then the emitter; the
// ADC word is the gate-rewritten pos.w) or the NO-GATE path (ADC word =
// colour.w raw; two short DIV->MULq windows read the PREVIOUS quotient).
// Output stride 3 {ST, RGBAQ, XYZ2}: STQ = (colour.xyz + payload.xyz) * Q,
// RGBAQ = colour raw, XYZ2 = {FTOI0(T*Q).xyz, ADC} (no z-negate); a node
// packet is kicked before the main packet when node.w != 0. One image
// additionally carries an env-map second material sub (RunEnvCall).
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class TexturedTristrip {
public:
    static bool Run(const Vu1HleInput& in, Vu1HleOutput& out);

    // One emitter call (one dispatch node); false = decline.
    static bool RunTexCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, ClipGate::GateRegs& gr, Vu1HleOutput& out);
    // The env-map second material sub of the pinned image.
    static bool RunEnvCall(QwPtr m, vu1lib::u32 node, vu1lib::u32 top, ClipGate::GateRegs& gr, Vu1HleOutput& out);
};
