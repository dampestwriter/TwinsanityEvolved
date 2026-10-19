#pragma once

#include "engine/ee/cpu/cpu_state.h"

// Actual NEW inline macro scale blend of AnimateJoint (block 294184):
// reads VF30 (now), VF29 (next), VF31.w (share) and VF0, writes ACC and VF29.
class MathLibraryJointLerpOracle {
public:
    static void Invoke(CpuContext& context);
    static const char* SourceDigest();
};
