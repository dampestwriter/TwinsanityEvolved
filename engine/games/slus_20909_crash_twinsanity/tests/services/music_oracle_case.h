#pragma once

#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/iop/iop_memory.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h"

int RunMusicOracle(CpuContext& context, ps2r::title::stream_irx::StreamIrxPlugin& plugin,
    ps2r::sound::SpuCore& spu, ps2r::iop::IopMemory& memory,
    ps2r::title::stream_irx::StreamRpcServer& face,
    const LoaderFileIo::Deps& deps, uint64_t& tick, IsoFs& iso, ReadEngine& reader);
