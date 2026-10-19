#pragma once

#include "games/slus_20909_crash_twinsanity/tests/services/bank_timing_oracle_case.h"

// Loading-phase pacing self-oracle on the recorded stage11 bank intents.
// Three arms share identical bank inputs and the shared logical read
// position; each is paced by the loader's headroom rule the way the
// composition root pacing seam observes it (ServiceTimeline::TimeLeft).
// Reports and bounds the frame ends each arm charges to the game.
int RunBankPacingOracle(CpuContext& context, ps2r::iop::IopModuleTable& table,
    ps2r::title::stream_irx::StreamIrxPlugin& plugin,
    ps2r::sound::SpuCore& spu, ps2r::title::stream_irx::StreamRpcServer& face,
    const LoaderFileIo::Deps& deps);
