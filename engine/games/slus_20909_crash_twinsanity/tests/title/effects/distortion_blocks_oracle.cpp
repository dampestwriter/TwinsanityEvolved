#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_blocks_oracle.h"

#include <array>
#include <cstring>
#include <stdexcept>

#include "engine/vu/vu0/lib_vu0_sin_cos.h"
#include "engine/vu/vu_chop_round_guard.h"

void DistortionBlocksOracle::Execute(CpuContext& context, uint32_t entry) {
    std::array<uint8_t, 0x110> code{};
    std::memcpy(code.data() + LibVu0SinCos::kEntryPc, LibVu0SinCos::kFingerprint,
                sizeof(LibVu0SinCos::kFingerprint));
    VuChopRoundGuard rounding;
    if (!LibVu0SinCos::TryExecute(code.data(), code.size(), context.vuRegs, entry))
        throw std::runtime_error("unexpected distortion CPU event microprogram");
}
