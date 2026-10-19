#include "games/slus_20909_crash_twinsanity/tests/services/stream_oracle_helpers.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"

namespace stream_oracle {
void Prep(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[28].UD[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UD[0] = kSp;
    ctx.cpuRegs.GPR.r[31].UD[0] = 0x00100100;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xDEADBEEFull;
}
void SetArg(CpuContext& ctx, int reg, uint32_t v) { ctx.cpuRegs.GPR.r[reg].UD[0] = v; }

// The title's poll exactly as WaitReadComplete asks it: FUN_002adaf0(fs) -> pending (v0).
uint32_t TitlePoll(CpuContext& ctx, uint32_t fs) {
    Prep(ctx);
    SetArg(ctx, 4, fs);
    DispatchTable::Invoke(0x2adaf0u, ctx);
    return ctx.cpuRegs.GPR.r[2].UL[0];
}

bool RangeEquals(uint32_t addr, const std::vector<uint8_t>& oracle, uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (memory::read<uint8_t>(addr + i) != oracle[off + i]) return false;
    return true;
}
bool RangeIs(uint32_t addr, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (memory::read<uint8_t>(addr + i) != v) return false;
    return true;
}

}  // namespace stream_oracle
