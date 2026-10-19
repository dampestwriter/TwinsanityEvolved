#pragma once

#include "engine/ee/cpu/cpu_state.h"

class DistortionBlocksOracle {
public:
    // The unchanged current NEW math service used by the wall-bounce callee.
    // This does not supply replacement block/list decisions.
    static void Execute(CpuContext&, uint32_t entry);
};
