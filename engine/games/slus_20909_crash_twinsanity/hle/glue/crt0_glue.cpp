// Title pass-through seam body (row 21): the game's own global-constructor
// walk IS the boot step; no HLE reimplementation.
#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)

void hle_DoGlobalConstructors(CpuContext& ctx) { FUN_002d69a0(ctx, 0x002d69a0); }
