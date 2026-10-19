#include "games/slus_20909_crash_twinsanity/hle/workarounds/free_list_0x181560.h"
#include "engine/ee/timing/tick_profile.h"

#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

namespace free_list_0x181560 {

namespace {
constexpr uint32_t kPhysMask = 0x1FFFFFFFu;

void ReturnToCaller(CpuContext& ctx) { ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0]; }  // jr $ra
}  // namespace

Body AllocHook(MetaArena& arena, Body orig) {
    return [&arena, orig](CpuContext& ctx, uint32_t ep) {
        ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Allocator);
        ps2r::ee::TickProfile::Mark();   // the overrun caller census (hot title path)
        const uint32_t size = ctx.cpuRegs.GPR.r[4].UL[0];
        const uint32_t ra = ctx.cpuRegs.GPR.r[31].UL[0] & kPhysMask;
        if (MetaArena::RoutesAllocation(size, ra)) {
            if (const uint32_t block = arena.Alloc()) {
                ctx.cpuRegs.GPR.r[2].UD[0] = block;  // v0 = arena payload
                ReturnToCaller(ctx);
                return;
            }
        }
        if (orig) orig(ctx, ep);  // exhausted arena or unrouted call: the slab
    };
}

Body FreeHook(MetaArena& arena, Body orig) {
    return [&arena, orig](CpuContext& ctx, uint32_t ep) {
        ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Allocator);
        ps2r::ee::TickProfile::Mark();   // the overrun caller census (hot title path)
        const uint32_t ptr = ctx.cpuRegs.GPR.r[4].UL[0] & kPhysMask;
        if (MetaArena::Contains(ptr)) {
            arena.Free(ptr);
            ReturnToCaller(ctx);
            return;
        }
        if (orig) orig(ctx, ep);
    };
}

int InstallHooks(MetaArena& arena, const Capture& capture, const Install& install, bool route_allocations) {
    int n = 0;
    if (route_allocations && install(MetaArena::kAllocSite, AllocHook(arena, capture(MetaArena::kAllocSite)))) ++n;
    if (install(MetaArena::kFreeSite, FreeHook(arena, capture(MetaArena::kFreeSite)))) ++n;
    return n;
}

}  // namespace free_list_0x181560
