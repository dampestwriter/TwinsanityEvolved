#pragma once

#include <functional>
#include <initializer_list>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "engine/ee/cpu/cpu_state.h"

int RunControlStateCase(CpuContext& context, title::Audio& audio,
    const std::function<int32_t(uint32_t, std::initializer_list<uint32_t>)>& call, bool& busy);
