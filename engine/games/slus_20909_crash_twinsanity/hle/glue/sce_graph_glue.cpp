// Title pass-through seam bodies (row 16): each body runs
// the hooked address's own generated body. Phase 6 registers real GS-priv
// bodies and deletes these lines. Hooked addresses: the common TU's
// `recompiled_functions[addr]` install sites for these names.
#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)

void hle_sceGsResetGraph(CpuContext& ctx) { FUN_002c1bd8(ctx, 0x002c1bd8); }
void hle_sceGsResetPath(CpuContext& ctx) { FUN_002c1ab0(ctx, 0x002c1ab0); }
void hle_sceGsSendImage(CpuContext& ctx) { FUN_002c1190(ctx, 0x002c1190); }
