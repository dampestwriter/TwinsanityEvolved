// tests/core/test_pacer_release.cpp -- the deferred-tick / pacer-release pins on the timing pieces:
//
//  * EmuDomain: under the deterministic policy (pacer release OFF) a Deferred tick
//    is exactly OnRawTick -- the same absorbed / released counts as the quantum
//    rule gives today; with the pacer release ON (Play with a pacer bound) a
//    Deferred tick rides 1:1 while OnRawTick still absorbs.
//  * BoundarySequencer: unbound (deterministic) the TickAdvance slot fires exactly
//    one Boundary tick per boundary; bound to a pacer under Play it fires NO
//    Boundary tick of its own -- the pacer's noted tick is the boundary's tick
//    (Deferred), a late boundary (tick already noted) does not park, a boundary
//    with nothing noted parks and consumes the pacer's next tick; a LATE boundary
//    (several ticks noted during the frame's work) delivers exactly ONE field --
//    the clock contract rules 1 and 3 in the SINGLE-FIELD form: the
//    further noted ticks are the backlog the following boundaries deliver one
//    each without parking (catch-up, pace_catchup), bounded by
//    BoundaryProtocol::kCatchUpMaxFields (the excess dropped, pace_dropped_fields);
//    the title's T0 steps one field per boundary, never a multi-field jump; the
//    boundary after the backlog parks for the next edge; no quantum top-up under
//    the release; a stopped pacer is bounded (kBoundaryParkPeriods) and the field
//    still happens. The deterministic profile is unchanged.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/intc/intc.h"
#include "engine/ee/timing/boundary_protocol.h"
#include "engine/ee/timing/boundary_sequencer.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/frame_rendezvous.h"
#include "engine/ee/timing/tick_source.h"

namespace {
int g_failures = 0;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) std::printf("  PASS %s\n", what);                         \
        else { std::printf("  FAIL %s\n", what); ++g_failures; }            \
    } while (0)

// Six raw ticks inside the loop under an exhausted quantum, as Deferred ticks:
// returns the emu advance.
uint64_t DeferredAdvance(bool release) {
    EmuDomain d;
    d.SetRegimePin(1);
    d.SetPacerRelease(release);
    d.ReAnchor(0);
    d.NoteArrival(0);
    d.OnRawTick(1);
    d.OnBoundary(1);   // in the loop from here
    const uint64_t before = d.DomainTick();
    for (uint64_t raw = 2; raw <= 5; ++raw) d.OnDeferredTick(raw);
    return d.DomainTick() - before;
}

void DomainPins() {
    {
        EmuDomain d;
        d.SetRegimePin(1);
        d.ReAnchor(0);
        d.NoteArrival(0);
        d.OnRawTick(1);
        d.OnBoundary(1);
        d.OnRawTick(2);   // the frame's one field
        d.OnRawTick(3);   // absorbed
        d.OnRawTick(4);   // absorbed
        CHECK(d.DomainTick() == 2 && d.Absorbed() == 2, "deterministic: quantum absorbs raw ticks past the regime (2 absorbed)");
        EmuDomain e;
        e.SetRegimePin(1);
        e.ReAnchor(0);
        e.NoteArrival(0);
        e.OnRawTick(1);
        e.OnBoundary(1);
        e.OnDeferredTick(2);
        e.OnDeferredTick(3);
        e.OnDeferredTick(4);
        CHECK(e.DomainTick() == d.DomainTick() && e.Absorbed() == d.Absorbed(),
              "deterministic (release off): OnDeferredTick == OnRawTick (same tick and absorbed counts)");
    }
    CHECK(DeferredAdvance(false) == 1, "release off: 4 deferred ticks under the quantum advance 1 (3 absorbed)");
    CHECK(DeferredAdvance(true) == 4, "release on (Play + pacer): 4 deferred ticks advance 4 (a pace park of N ticks costs N fields)");
    {
        EmuDomain d;
        d.SetRegimePin(1);
        d.SetPacerRelease(true);
        d.ReAnchor(0);
        d.NoteArrival(0);
        d.OnRawTick(1);
        d.OnBoundary(1);
        d.OnRawTick(2);
        d.OnRawTick(3);
        CHECK(d.DomainTick() == 2 && d.Absorbed() == 1, "release on: a plain OnRawTick still absorbs (the release is the Deferred form only)");
        d.OnDeferredTick(4);
        d.OnDeferredTick(5);
        d.NoteArrival(5);
        d.OnBoundary(5);
        CHECK(d.DomainTick() == 4 && d.LastBoundaryTick() == 4, "release on: the boundary settle keeps the ridden ticks (no regression, no burst)");
    }
}

struct TestPacerRelease {  // the boundary rig (primary type of this file)
    TickSource source;
    EmuDomain domain;
    FrameRendezvous rendezvous;
    Intc& intc = Intc::Instance();
    std::atomic<bool> running{true};
    BoundarySequencer seq;
    BoundaryProtocol protocol;
    CpuContext ctx{};
    explicit TestPacerRelease(TickSource::Policy p) : protocol(source, seq.Ticks()) {
        intc.ResetForTest();
        source.SetPolicy(p);
        source.SetMode(TickSource::Mode::Realtime);
        domain.SetRegimePin(1);
        BoundarySequencer::Phase3Deps deps;
        deps.source = &source;
        deps.domain = &domain;
        deps.rendezvous = &rendezvous;
        deps.intc = &intc;
        deps.running = &running;
        seq.InstallPhase3Steps(deps);
    }
};

void SequencerPins() {
    {
        TestPacerRelease r(TickSource::Policy::Deterministic);
        for (int i = 0; i < 3; ++i) r.seq.RunBoundary(r.ctx);
        CHECK(r.seq.Ticks().FiredByCause(TickSync::Cause::Boundary) == 3 && r.source.RawCount() == 3 &&
                  r.seq.Ticks().FiredByCause(TickSync::Cause::Deferred) == 0,
              "deterministic, unbound: exactly one Boundary tick per boundary, no Deferred");
        CHECK(!r.seq.WallLocked() && !r.domain.PacerRelease(), "deterministic: not wall-locked, pacer release off");
    }
    {
        TestPacerRelease r(TickSource::Policy::Play);   // Play policy WITHOUT a pacer bound: unchanged slot
        for (int i = 0; i < 2; ++i) r.seq.RunBoundary(r.ctx);
        CHECK(r.seq.Ticks().FiredByCause(TickSync::Cause::Boundary) == 2 && r.protocol.PaceParks() == 0,
              "Play without a bound pacer: the slot fires its own Boundary tick (no park)");
    }
    {
        TestPacerRelease r(TickSource::Policy::Play);
        r.seq.BindPacer(&r.protocol);
        CHECK(r.seq.WallLocked() && r.domain.PacerRelease(), "BindPacer: wall-locked, pacer release on");
        // Late boundary: the pacer ticked during the frame's work -> no park.
        r.seq.Ticks().NotePending(TickSync::Cause::Demand);
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.seq.Ticks().FiredByCause(TickSync::Cause::Boundary) == 0 &&
                  r.seq.Ticks().FiredByCause(TickSync::Cause::Deferred) == 1 && r.protocol.PaceLate() == 1 &&
                  r.protocol.PaceParks() == 0 && r.source.RawCount() == 1,
              "wall-locked, late: the noted tick is the boundary's tick (Deferred), no own Boundary tick, pace_late=1");
        // Parked boundary: nothing noted -> park until the pacer's tick.
        std::thread pacer([&r] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            r.seq.Ticks().NotePending(TickSync::Cause::Demand);
            r.protocol.NotifyTick();
        });
        const auto t0 = std::chrono::steady_clock::now();
        r.seq.RunBoundary(r.ctx);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        pacer.join();
        CHECK(r.protocol.PaceParks() == 1 && r.protocol.PaceTimeouts() == 0 && r.source.RawCount() == 2 &&
                  r.seq.Ticks().FiredByCause(TickSync::Cause::Deferred) == 2 && ms < 60,
              "wall-locked, parked: the boundary waits for the pacer's tick and fires it (pace_parks=1, no timeout)");
        CHECK(r.domain.DomainTick() == 2, "wall-locked: one T0 field per pacer tick (DomainTick == pacer ticks)");
        // Stopped pacer: bounded park, the field still happens (Boundary fallback).
        const auto t1 = std::chrono::steady_clock::now();
        r.seq.RunBoundary(r.ctx);
        const auto ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
        CHECK(r.protocol.PaceTimeouts() == 1 && r.seq.Ticks().FiredByCause(TickSync::Cause::Boundary) == 1 &&
                  r.source.RawCount() == 3 && ms1 >= 25 && ms1 < 500,
              "wall-locked, pacer stopped: the park is bounded (2 periods) and the boundary falls back to one Boundary tick");
    }
    {
        // The clock contract, rule 3 in the SINGLE-FIELD form: a LATE
        // boundary -- the pacer noted 2 ticks while the EE was busy with the frame
        // (a 33 ms frame) -- delivers ONE field: the title's T0 (DerivedClocks:
        // 9600 counts per field) advances by one field, so its per-frame delta
        // (TR src/game/clock.cpp GameTimeSetStamp over Platform::Time::Ticks)
        // stays ONE field -- never a multi-field jump. The second noted tick is the
        // backlog: the NEXT boundary does not park (catch-up) and delivers it, so
        // the game runs two frames for the two fields the wall produced and keeps
        // wall speed; the boundary after that parks for the next edge again.
        // Nothing is absorbed, no regime top-up adds a field.
        TestPacerRelease r(TickSource::Policy::Play);
        r.seq.BindPacer(&r.protocol);
        DerivedClocks clocks(r.domain);
        for (int i = 0; i < 2; ++i) {   // two on-time boundaries (one noted tick each): in the loop
            r.seq.Ticks().NotePending(TickSync::Cause::Demand);
            r.protocol.NotifyTick();
            r.seq.RunBoundary(r.ctx);
        }
        const uint64_t tick0 = r.domain.DomainTick();
        const uint64_t t0_before = clocks.T0Counts();
        CHECK(r.seq.PaceLateFields() == 0 && r.domain.Absorbed() == 0 && tick0 == 2 && r.protocol.PaceCatchUp() == 0,
              "two boundaries with one noted tick each: no late fields, no catch-up, nothing absorbed, 2 fields");
        const uint64_t one_field_counts = t0_before / 2;
        for (int i = 0; i < 2; ++i) r.seq.Ticks().NotePending(TickSync::Cause::Demand);   // a 33 ms frame: 2 edges passed
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.protocol.PaceLate() == 3 && r.seq.Ticks().FiredByCause(TickSync::Cause::Deferred) == 3 &&
                  r.seq.Ticks().FiredByCause(TickSync::Cause::Boundary) == 0 && r.seq.PaceLateFields() == 1 &&
                  r.protocol.Backlog() == 1 && r.seq.Ticks().Pending() == 1,
              "rule 3, late with 2 noted: ONE fired as Deferred (no own Boundary tick), pace_late_fields=1, backlog 1 left noted");
        CHECK(r.domain.DomainTick() - tick0 == 1 && r.domain.Absorbed() == 0 && r.domain.Regime() == 1,
              "rule 3: a 33 ms tick reports ONE vsync (DomainTick +1), nothing absorbed, no regime top-up -- never a multi-field jump");
        CHECK(clocks.T0Counts() - t0_before == one_field_counts && one_field_counts == 9600,
              "rule 3: the title's T0 delta over the late frame is 1 x 9600 counts -- its GameTime frame delta stays one field");
        // Catch-up: the next boundary finds the backlog and does NOT park -- it
        // delivers the second field at once (the game runs the frame the wall
        // produced), counted pace_catchup, not late.
        const uint64_t parks_before = r.protocol.PaceParks();
        const uint64_t late_before = r.protocol.PaceLate();
        const auto tc0 = std::chrono::steady_clock::now();
        r.seq.RunBoundary(r.ctx);
        const auto tc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tc0).count();
        CHECK(r.protocol.PaceCatchUp() == 1 && r.protocol.PaceParks() == parks_before && r.protocol.PaceLate() == late_before &&
                  r.domain.DomainTick() - tick0 == 2 && r.protocol.Backlog() == 0 && tc_ms < 10,
              "rule 3 catch-up: the boundary after a late frame delivers the backlog field without parking (pace_catchup=1, DomainTick +2 over the two boundaries)");
        CHECK(clocks.T0Counts() - t0_before == 2 * one_field_counts,
              "rule 3 catch-up: over the late frame and its catch-up boundary the T0 advanced 2 fields, one per boundary");
        // Rule 1: after the backlog is gone the next boundary finds nothing noted and
        // PARKS for the next edge (no burst repayment), the park counted as slept time.
        std::thread pacer([&r] {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            r.seq.Ticks().NotePending(TickSync::Cause::Demand);
            r.protocol.NotifyTick();
        });
        const uint64_t parks0 = r.protocol.PaceParks();
        r.seq.RunBoundary(r.ctx);
        pacer.join();
        CHECK(r.protocol.PaceParks() == parks0 + 1 && r.domain.DomainTick() - tick0 == 3 && r.protocol.PaceSleptUs() >= 5000,
              "rule 1: the boundary after the backlog parks for the next edge (pace_parks +1, one field, pace_slept_ms counted) -- never faster than the wall");
        // Bounded catch-up: a stall of kCatchUpMaxFields + 5 fields (a debugger
        // pause) is not replayed as a burst -- the boundary delivers one field, keeps
        // kCatchUpMaxFields of backlog and DROPS the excess (counted); the backlog
        // then drains one field per boundary.
        const uint64_t tick_s = r.domain.DomainTick();
        const uint64_t late_fields0 = r.seq.PaceLateFields();
        const uint64_t stall = BoundaryProtocol::kCatchUpMaxFields + 5;
        for (uint64_t i = 0; i < stall + 1; ++i) r.seq.Ticks().NotePending(TickSync::Cause::Demand);   // the boundary's own + the stall
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.domain.DomainTick() - tick_s == 1 && r.protocol.Backlog() == BoundaryProtocol::kCatchUpMaxFields &&
                  r.protocol.PaceDroppedFields() == 5 && r.seq.Ticks().Dropped() == 5 &&
                  r.seq.PaceLateFields() - late_fields0 == stall,
              "rule 3 bounded: a long stall delivers one field, keeps kCatchUpMaxFields of backlog, drops the excess (pace_dropped_fields=5)");
        uint64_t catchups = 0;
        for (uint64_t i = 0; i < BoundaryProtocol::kCatchUpMaxFields; ++i) {
            const uint64_t c0 = r.protocol.PaceCatchUp();
            r.seq.RunBoundary(r.ctx);
            catchups += r.protocol.PaceCatchUp() - c0;
        }
        CHECK(catchups == BoundaryProtocol::kCatchUpMaxFields && r.protocol.Backlog() == 0 &&
                  r.domain.DomainTick() - tick_s == 1 + BoundaryProtocol::kCatchUpMaxFields && r.seq.Ticks().Pending() == 0,
              "rule 3 bounded: the capped backlog drains one field per catch-up boundary, then nothing is noted");
        // The waiter path is untouched: a Deferred tick the guest
        // WAITED for at a kernel timer wait rides 1:1.
        const uint64_t tick1 = r.domain.DomainTick();
        for (int i = 0; i < 2; ++i) r.seq.Ticks().NotePending(TickSync::Cause::Demand);
        r.protocol.NotifyTick();
        r.protocol.Wait(0);   // Play: ONE pending tick fires on the waiter (no guest predicate set); the waiter re-reads its clock
        CHECK(r.domain.DomainTick() - tick1 == 1 && r.domain.Absorbed() == 0 && r.seq.Ticks().Pending() == 1,
              "timer waiter under the wall lock: a wake fires ONE waited-for Deferred tick (single-field), the second stays noted");
        const auto tw0 = std::chrono::steady_clock::now();
        r.protocol.Wait(0);   // a tick already noted ends the wait at once
        const auto tw_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tw0).count();
        CHECK(r.domain.DomainTick() - tick1 == 2 && r.seq.Ticks().Pending() == 0 && tw_ms < 10,
              "timer waiter: the next wait returns at once on the noted tick and fires it (2 waits -> 2 fields, none absorbed)");
        // A backlog the timer waiter consumed mid-frame is not caught up by the
        // boundary: it finds nothing noted and parks (no stale catch-up).
        for (int i = 0; i < 2; ++i) r.seq.Ticks().NotePending(TickSync::Cause::Demand);   // a late frame: backlog 1
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.protocol.Backlog() == 1, "late frame leaves a backlog of 1");
        r.protocol.Wait(0);   // the title's timer wait inside the frame takes it
        std::thread pacer2([&r] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            r.seq.Ticks().NotePending(TickSync::Cause::Demand);
            r.protocol.NotifyTick();
        });
        const uint64_t parks1 = r.protocol.PaceParks();
        const uint64_t catchup1 = r.protocol.PaceCatchUp();
        r.seq.RunBoundary(r.ctx);
        pacer2.join();
        CHECK(r.protocol.PaceParks() == parks1 + 1 && r.protocol.PaceCatchUp() == catchup1 && r.protocol.Backlog() == 0,
              "a backlog the timer waiter consumed is not caught up by the boundary: it parks");
        // Fast-forward: the extra-fields provider adds N-1 fields to
        // ONE boundary (one noted tick): the title's T0 sees N fields, the boundary
        // count is still one per edge, nothing is counted as late; a provider
        // answering 0 changes nothing; the cap bounds a runaway answer.
        const uint64_t tick2 = r.domain.DomainTick();
        const uint64_t late0 = r.seq.PaceLateFields();
        const uint64_t t0_ff = clocks.T0Counts();
        uint32_t extra = 3;
        r.seq.SetExtraFields([&extra] { return extra; });
        r.seq.Ticks().NotePending(TickSync::Cause::Demand);
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.domain.DomainTick() - tick2 == 4 && r.seq.FastForwardFields() == 3 && r.seq.PaceLateFields() == late0 &&
                  clocks.T0Counts() - t0_ff == 4 * one_field_counts,
              "fast-forward x4: one boundary delivers 1 noted + 3 extra fields (T0 +4 fields), none counted late");
        extra = 0;
        r.seq.Ticks().NotePending(TickSync::Cause::Demand);
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.domain.DomainTick() - tick2 == 5 && r.seq.FastForwardFields() == 3, "fast-forward provider 0: a plain one-field boundary");
        extra = 1000;
        r.seq.Ticks().NotePending(TickSync::Cause::Demand);
        r.protocol.NotifyTick();
        r.seq.RunBoundary(r.ctx);
        CHECK(r.domain.DomainTick() - tick2 == 6 + BoundarySequencer::kExtraFieldsMax &&
                  r.seq.FastForwardFields() == 3 + BoundarySequencer::kExtraFieldsMax,
              "fast-forward: a runaway answer is capped at kExtraFieldsMax");
        r.seq.SetExtraFields({});
    }
    {
        // The deterministic profile is UNCHANGED by the contract: the regime detector's
        // top-up still applies with the release off (OnBoundary's quantum settle).
        EmuDomain d;
        d.SetRegimePin(2);
        d.ReAnchor(0);
        d.NoteArrival(0);
        d.OnRawTick(1);
        d.OnBoundary(1);
        d.OnRawTick(2);
        d.NoteArrival(2);
        d.OnBoundary(2);
        CHECK(d.DomainTick() == 3, "deterministic (release off), regime 2: the boundary tops the quantum up (1 raw tick -> 2 fields)");
        EmuDomain e;
        e.SetRegimePin(2);
        e.SetPacerRelease(true);
        e.ReAnchor(0);
        e.NoteArrival(0);
        e.OnDeferredTick(1);
        e.OnBoundary(1);
        e.OnDeferredTick(2);
        e.NoteArrival(2);
        e.OnBoundary(2);
        CHECK(e.DomainTick() == 2, "rule 1 (release on), regime pinned 2: NO quantum top-up -- the fields are the wall's alone");
    }
}
}  // namespace

int main(int argc, char** argv) {
    const char* section = argc > 1 ? argv[1] : "all";
    const bool all = std::string(section) == "all";
    if (all || std::string(section) == "domain") DomainPins();
    if (all || std::string(section) == "sequencer") SequencerPins();
    std::printf("PacerRelease(%s): %s\n", section, g_failures ? "FAIL" : "ALL PASS");
    return g_failures ? 1 : 0;
}
