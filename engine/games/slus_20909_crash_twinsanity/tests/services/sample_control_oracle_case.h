#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

int RunSampleControlOracle(CpuContext& ctx, ps2r::sound::SpuCore& spu,
    title::Audio& audio, uint32_t sample);
