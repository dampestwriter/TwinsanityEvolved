#pragma once
// HeadroomQueryGlue -- installs the title's RECOMPILED headroom predicate
// FUN_00191bd0 over the generated TU's hook at the same address.
//
// What the title does (SLUS_209.09, FUN_00191bd0(obj, f12 seconds) -> v0):
//   ticks   = (int)(seconds * obj->tick_rate[+0x70])            // T0 ticks asked for
//   quantum = obj->tick_rate / obj->divisor[+0x68]               // 576000 / 60 = 9600
//   now     = FUN_00191920()                                     // T0 COUNT (BUSCLK/256)
//                                                                // widened by the overflow word
//   elapsed = (now - obj->anchor[+0x80]) folded modulo `quantum` (minus obj+0x78)
//   v0      = (ticks < quantum - elapsed) ? 1 : 0               // headroom left this frame?
// It is a QUERY of the T0 clock against the frame-top anchor: on hardware it
// returns at once and never waits.
//
// The frozen recompiler emitted a hook here (recompiled.cpp: `recompiled_
// functions[0x191bd0] = hle_TimerWaitMicroseconds`) that modelled the query as
// a WAIT of `seconds`; in this engine that wait is answered by one Demand tick
// of the emu clock and, with T0 paced by the tick, costs T0 a whole field per
// call (484 extra fields in 60 coldboot frames, 5 x 9600 per
// anchor frame). The generated TU is never edited, so the re-binding happens
// through the dispatch install primitive at composition time: this glue puts
// the generated body back, BEFORE LoaderSpinLift wraps the address (the lift
// captures whatever is installed and must wrap the predicate, not the wait).
//
// Over the slice-frozen T0 (DerivedClocks::TimerCounts is a pure function of
// the field count by design -- BUG-014/031/032) the predicate is constant
// inside a slice: it answers 1 while the frame's quantum has headroom and
// never advances the clock. The load-wait loop's exits inside a slice are the
// LoaderSpinLift's (its two load-poll sites) and `$s0 == 0` (nothing pending);
// the maintenance loops' other sites are bounded by their own work. After this
// install KernlTimer::TimerWaitMicroseconds is unreached on this title; the
// class stays as the row-18 seam provider.
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"

class HeadroomQueryGlue {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using InstallFn = std::function<bool(uint32_t addr, Body body)>;

    // The hooked address (the loader_sync Layout's fn_headroom_query names the
    // same site; the composition root checks they agree).
    static constexpr uint32_t kAddress = 0x191bd0;

    // Installs the generated predicate body at kAddress through `install`
    // (DispatchTable::Install in the root); returns the install's answer.
    static bool InstallRecompiled(const InstallFn& install);

    // The generated body itself (the unit pin drives it directly).
    static void RunRecompiled(CpuContext& ctx, uint32_t addr);
};
