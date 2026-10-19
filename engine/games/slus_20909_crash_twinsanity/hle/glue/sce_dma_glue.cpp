// Title seam bodies of the two DMA-idle waits the generated TU hooks by name
// (row 17): the engine bodies are the libdma.a manifest rows `sceDmaSync` /
// `sceGifSyncDma` (LibdmaBindings over the Dmac: flush the parked VIF1 kick,
// return 0), reached through the manifest's row table so this TU links
// without the DMAC library. A rig with no libdma instance registered runs the
// hooked address's own generated body instead (it polls DMAC registers that
// read idle and returns at once).
#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)
#include "engine/ee/hle/sdk_binding_manifest.h"

namespace {
bool RunLibdmaRow(const char* name, CpuContext& ctx) {
    const SdkBindingManifest::EngineRow* row = SdkBindingManifest::Instance().Find("libdma.a", name);
    if (row == nullptr || row->handler == nullptr) return false;
    row->handler(ctx);
    return true;
}
}  // namespace

void hle_sceGifSyncDma(CpuContext& ctx) {
    if (!RunLibdmaRow("sceGifSyncDma", ctx)) FUN_002c12e0(ctx, 0x002c12e0);
}
void hle_sceDmaSync(CpuContext& ctx) {
    if (!RunLibdmaRow("sceDmaSync", ctx)) FUN_002c1030(ctx, 0x002c1030);
}
