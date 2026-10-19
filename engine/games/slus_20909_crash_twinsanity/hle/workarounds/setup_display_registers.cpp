// HOOK section 2 workaround row 0x0019aff0 (row 19): Phase-3 form is a
// pass-through to the generated body; the ledger row records retirement.
#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)

void hle_SetupDisplayRegisters(CpuContext& ctx) { FUN_0019aff0(ctx, 0x0019aff0); }
