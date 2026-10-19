#pragma once
// The two shared-prefix displacement passes every family image carries in
// its byte-identical prefix ([0x1400, 0x1598) and [0x15a0, 0x1720)), entered
// directly by MSCAL from the skinned and crystal programs' images.
//
// 0x1400 (int vertices): vi13 = TOP; QW7.x = the displacement bank cursor
// (MR32-rotated and stored back); per stride-4 vertex group the ITOF0'd
// vertex pair is scaled by QW[TOP+2].xy broadcast, the ITOF0'd bank row is
// scaled by QW[bank].xyz then .w and added to the first vertex; E-bit at
// 0x1588 -> TPC 0x1598.
// 0x15a0 (float vertices): vi13 inherited (no XTOP), only the bank row is
// converted and scaled; one vertex per group displaced; E-bit at 0x1710 ->
// TPC 0x1720.
// Both: zero kicks, the full register file reproduced (has_regs) with the
// MAC / STATUS chain applied per FMAC in program order, data writes = the
// working-copy diff.
#include "engine/vu/hle/vu1_hle_contract.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/library_common.h"

class DisplacePrefix {
public:
    static constexpr vu1lib::u32 kIntEntryPc = 0x1400;
    static constexpr vu1lib::u32 kFloatEntryPc = 0x15a0;

    static bool RunDisplaceInt(const Vu1HleInput& in, Vu1HleOutput& out);
    static bool RunDisplaceFloat(const Vu1HleInput& in, Vu1HleOutput& out);
};
