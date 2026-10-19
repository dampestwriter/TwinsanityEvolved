#pragma once

#include "engine/iop/iop_module_table.h"
#include "games/slus_20909_crash_twinsanity/tests/services/music_oracle_case.h"

int RunBankTimingOracle(CpuContext& context, ps2r::iop::IopModuleTable& table,
    ps2r::title::stream_irx::StreamIrxPlugin& plugin,
    ps2r::sound::SpuCore& spu, ps2r::title::stream_irx::StreamRpcServer& face,
    const LoaderFileIo::Deps& deps);
