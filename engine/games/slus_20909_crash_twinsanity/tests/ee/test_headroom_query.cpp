// HeadroomQueryRecompiled -- the pin for the headroom re-binding: the title's headroom
// query FUN_00191bd0 runs its RECOMPILED body (HeadroomQueryGlue installed over
// the generated TU's hle_TimerWaitMicroseconds hook through the dispatch
// install primitive) and advances NO tick: TickSync's Demand count and
// KernlTimer::LastDeadline() are unchanged by a call, while the answer is the
// predicate's -- v0 = 1 with headroom (anchor = now), v0 = 0 when the frame-top
// anchor is exactly one quantum old. The same call BEFORE the install (the
// generated hook as emitted) fires one Demand tick: that is what the pin fails
// on when the root forgets the re-binding.
//
// Rig: real guest memory (MainMemory + MmioRouter) with a settable T0 COUNT at
// 0x10000000 (the query's FUN_00191920 reads it and widens it by the title's
// overflow word at $gp-0x7d20), a timer object {tick_rate 576000 at +0x70,
// divisor 60 at +0x68, anchor at +0x80}, f12 = 1.0 ms. KernlTimer's clock /
// wait hook count what the wait seam would do.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/hle/kernl_timer.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_handler.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ee/timing/tick_sync.h"
#include "games/slus_20909_crash_twinsanity/hle/glue/headroom_query_glue.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_spin_lift.h"

namespace {
int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

// The test rig (the file's one primary type): T0 COUNT (0x10000000, 16 bits on
// the EE) as a settable word; the other timer registers read 0.
class TestHeadroomQuery : public MmioHandler {
public:
    uint32_t count = 0;
    uint64_t Read(uint32_t addr, unsigned) override { return addr == 0x10000000u ? (count & 0xFFFFu) : 0; }
    void Write(uint32_t, unsigned, uint64_t) override {}
};

constexpr uint32_t kTimerObj = 0x00300000;   // the title's timer object (any RAM address)
constexpr uint32_t kGp = 0x00400000;         // $gp: the query's overflow word lives at gp-0x7d20
constexpr uint32_t kSp = 0x00500000;
constexpr uint32_t kTickRate = 576000;       // BUSCLK/256
constexpr uint32_t kDivisor = 60;            // one frame quantum = 9600 ticks
constexpr uint32_t kQuantum = kTickRate / kDivisor;
constexpr float kSeconds = 0.001f;           // the lap >= 2 query's f12 (576 ticks)

TestHeadroomQuery g_t0;

void BindMemory() {
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink sink;
    static MmioRouter mmio(sink);
    static bool registered = false;
    if (!registered) {
        mmio.Register(0x10000000u, 0x10u, g_t0, "T0");
        registered = true;
    }
    static FastMem fm(ram, map, mmio);
    RecompAbi::BindMemory(&fm);
}

void SeedTimerObject(uint64_t anchor) {
    memory::write<uint32_t>(kTimerObj + 0x68, kDivisor);
    memory::write<uint32_t>(kTimerObj + 0x70, kTickRate);
    memory::write<uint64_t>(kTimerObj + 0x78, 0);
    memory::write<uint64_t>(kTimerObj + 0x80, anchor);
    memory::write<uint64_t>(kGp - 0x7d20, 0);   // no T0 overflow so far
    memory::write<uint8_t>(kGp - 0x7d17, 0);
}

uint32_t Query(uint32_t addr) {
    CpuContext ctx{};
    std::memset(&ctx, 0, sizeof ctx);
    ctx.cpuRegs.GPR.r[4].UL[0] = kTimerObj;
    ctx.cpuRegs.GPR.r[28].UL[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UL[0] = kSp;
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x0017d75c;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xdead;
    std::memcpy(&ctx.fpuRegs.fpr[12].UL, &kSeconds, sizeof(kSeconds));
    DispatchTable::Invoke(addr, ctx);
    return ctx.cpuRegs.GPR.r[2].UL[0];
}
}  // namespace

int main() {
    BindMemory();
    DispatchTable::Initialize();
    const uint32_t addr = HeadroomQueryGlue::kAddress;
    CHECK(addr == LoaderSpinLift::Layout{}.fn_headroom_query, "the glue's address is the loader_sync Layout's fn_headroom_query");
    CHECK(DispatchTable::IsHooked(addr), "as emitted, 0x191bd0 is a hook (hle_TimerWaitMicroseconds), not the generated body");

    // The emitted hook is the Phase-3 WAIT seam (hle_TimerWaitMicroseconds): a real
    // wait on KernlTimer's clock. It is NOT invoked here -- a test clock that does
    // not satisfy the wait hangs the process (the 2026-10-01 six-hour ctest hang).
    // The pin is the recompiled-predicate half below; the hook's presence is the
    // precondition asserted above.
    TickSync ticks;
    ticks.SetBody([](TickSync::Cause) { return 1ull; });
    uint64_t clock = 100000;
    KernlTimer::ResetForTest();
    KernlTimer::SetClock([&clock] { return clock; }, [&](uint64_t) {
        ticks.FireInline(TickSync::Cause::Demand);
        clock += kQuantum;
    });
    g_t0.count = 0x1000;
    SeedTimerObject(/*anchor=*/g_t0.count);

    // The re-binding: the generated predicate through the install primitive.
    ticks.ResetCounters();
    const uint64_t deadline_before = KernlTimer::LastDeadline();
    CHECK(HeadroomQueryGlue::InstallRecompiled([](uint32_t a, HeadroomQueryGlue::Body b) { return DispatchTable::Install(a, b); }),
          "HeadroomQueryGlue::InstallRecompiled installs through DispatchTable::Install");

    SeedTimerObject(/*anchor=*/g_t0.count);
    const uint32_t v_now = Query(addr);
    CHECK(v_now == 1, "recompiled predicate: anchor = now -> headroom -> v0 = 1");
    CHECK(ticks.FiredByCause(TickSync::Cause::Demand) == 0 && ticks.Fired() == 0, "recompiled predicate fires NO tick");
    CHECK(KernlTimer::LastDeadline() == deadline_before, "recompiled predicate leaves KernlTimer::LastDeadline() unchanged");
    CHECK(memory::read<uint8_t>(kGp - 0x7d17) == 1, "the query's T0 read ran (FUN_00191920 latched its first-read flag)");

    // "One quantum old" = the clock advanced one quantum PAST the anchor. Never
    // retreat the anchor below the count: the predicate's elapsed = now - anchor is
    // u64 arithmetic, and a wrapped elapsed sends its period-fold loop through
    // ~2^64 / 9600 iterations (the 2026-10-01 six-hour hang).
    g_t0.count = 0x1000 + kQuantum;
    SeedTimerObject(/*anchor=*/0x1000);
    const uint32_t v_old = Query(addr);
    CHECK(v_old == 0, "recompiled predicate: anchor one quantum old -> no headroom -> v0 = 0");
    CHECK(ticks.Fired() == 0 && KernlTimer::LastDeadline() == deadline_before, "still no tick, no deadline");

    // Headroom shrinks with the elapsed time, never by the call itself: 9000 of
    // 9600 ticks elapsed leaves 600 >= 576 (v0 = 1); 9100 leaves 500 (v0 = 0).
    g_t0.count = 0x1000 + 9000;   // 9000 ticks past the anchor 0x1000
    SeedTimerObject(/*anchor=*/0x1000);
    CHECK(Query(addr) == 1, "9000 elapsed: 600 ticks of headroom >= 576 -> 1");
    g_t0.count = 0x1000 + 9100;   // 9100 ticks past the anchor
    SeedTimerObject(/*anchor=*/0x1000);
    CHECK(Query(addr) == 0, "9100 elapsed: 500 ticks of headroom < 576 -> 0");
    CHECK(ticks.Fired() == 0, "four predicate calls, zero ticks");
    CHECK(g_t0.count == 0x1000 + 9100, "T0 COUNT itself was never written by the query (only the test advanced it)");

    std::printf("%s\n", g_fail ? "HeadroomQueryRecompiled: FAIL" : "HeadroomQueryRecompiled: PASS");
    return g_fail ? 1 : 0;
}
