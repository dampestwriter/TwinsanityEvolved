// Title seam bodies of the loader's five file-IO hooks (the generated code
// dispatches these addresses to the HLE_FUN_ names): each forwards to
// LoaderFileIo, the title-side glue over the engine's ReadEngine/LoaderModel.
#include "recompiled.h"   // the generated header (build dir; ps2r_title_recomp adds PS2R_RECOMP_GEN_DIR)
#include "../loader_sync/loader_file_io.h"

void HLE_FUN_002ad8f0(CpuContext& ctx, uint32_t) { LoaderFileIo::Open(ctx); }
void HLE_FUN_002adb18(CpuContext& ctx, uint32_t) { LoaderFileIo::FileSize(ctx); }
void HLE_FUN_002aac80(CpuContext& ctx, uint32_t) { LoaderFileIo::WindowedRead(ctx); }
void HLE_FUN_002adb40(CpuContext& ctx, uint32_t) { LoaderFileIo::Close(ctx); }
void HLE_FUN_002ad1a0(CpuContext& ctx, uint32_t) { LoaderFileIo::WaitReadComplete(ctx); }
