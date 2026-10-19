// Title pass-through seam bodies (row 22): the 12 libc/allocator hook points
// run their own generated bodies (the allocator HLE is retired, INDEX :654;
// the meta-arena reroute is a separate allocator observer, 3E).
#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)

void HLE_002c5080(CpuContext& ctx) { FUN_002c5080(ctx, 0x002c5080); }
void HLE_002c5820(CpuContext& ctx) { FUN_002c5820(ctx, 0x002c5820); }
void HLE_002c5940(CpuContext& ctx) { FUN_002c5940(ctx, 0x002c5940); }
void HLE_002c6ce0(CpuContext& ctx) { FUN_002c6ce0(ctx, 0x002c6ce0); }
void HLE_002c6d8c(CpuContext& ctx) { FUN_002c6d8c(ctx, 0x002c6d8c); }
void HLE_002c6e88(CpuContext& ctx) { FUN_002c6e88(ctx, 0x002c6e88); }
void HLE_002c7070(CpuContext& ctx) { FUN_002c7070(ctx, 0x002c7070); }
void HLE_002c71bc(CpuContext& ctx) { FUN_002c71bc(ctx, 0x002c71bc); }
void HLE_002c7418(CpuContext& ctx) { FUN_002c7418(ctx, 0x002c7418); }
void HLE_002c75e8(CpuContext& ctx) { FUN_002c75e8(ctx, 0x002c75e8); }
void HLE_002ccba0(CpuContext& ctx) { FUN_002ccba0(ctx, 0x002ccba0); }
void HLE_002cf930(CpuContext& ctx) { FUN_002cf930(ctx, 0x002cf930); }
