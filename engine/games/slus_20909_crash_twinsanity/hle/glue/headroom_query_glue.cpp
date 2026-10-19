// HeadroomQueryGlue: the pass-through that puts the title's generated headroom
// predicate back at its own address (see the header). This
// TU is the only one outside the generated code that names FUN_00191bd0.
#include "headroom_query_glue.h"

#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)

void HeadroomQueryGlue::RunRecompiled(CpuContext& ctx, uint32_t addr) { FUN_00191bd0(ctx, addr); }

bool HeadroomQueryGlue::InstallRecompiled(const InstallFn& install) {
    if (!install) return false;
    return install(kAddress, [](CpuContext& ctx, uint32_t addr) { FUN_00191bd0(ctx, addr); });
}
