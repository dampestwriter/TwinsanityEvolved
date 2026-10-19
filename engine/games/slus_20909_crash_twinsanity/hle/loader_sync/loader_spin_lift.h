#pragma once
// LoaderSpinLift -- the title's load-wait loop (FUN_0017d6b8) bound to the
// engine's LoaderModel sync-wait rule (LoaderModel::ShouldYieldPollSlice) at
// the loop's headroom query FUN_00191bd0, at its two load-poll sites.
//
// What the title does (SLUS_209.09, FUN_0017d6b8; $s1 = the lap counter):
//   lap 1 (0x17d710..0x17d73c):
//     while (FUN_00191bd0(timer, f12 = 1.7 ms))   // ra = 0x17d724
//         { if (!FUN_002aa8d0(mgr, 0, &out)) break;  $s0 = 1; }
//   lap >= 2 (0x17d748..0x17d774):
//     if (FUN_00191bd0(timer, f12 = 1.0 ms))       // ra = 0x17d75c
//         { $s0 = 0; if (FUN_002aa8d0(mgr, 0, &out)) $s0 = 1; }
//   then one step of two other managers (each behind its own headroom query),
//   and at 0x17d860: `$s0 == 0` -> leave the wait loop; else if
//   FUN_00191bd0(timer, 1.0 ms) (ra 0x17d87c) -> next lap, else leave.
// FUN_00191bd0(obj, seconds) is a HEADROOM QUERY: it reads the T0 clock
// (FUN_00191920 -> T0 COUNT, BUSCLK/256), folds the time since the frame-top
// anchor at obj+0x80 modulo the frame quantum (obj+0x70 / obj+0x68 = 9600
// ticks) and answers 1 while the slice still has >= `seconds` of headroom.
// FUN_002aa8d0 polls the one in-flight asynchronous read through the title's
// own completion predicate (FUN_002ad110 -> FUN_002adaf0 -> FUN_001e3308,
// the STREAM slot status LoaderFileIo models) and answers nonzero while it is
// pending; $s0 records "this lap still has pending work".
//
// The PS2 rule: on hardware T0 advances in real time while the EE spins, so
// the load-wait loop exits on quantum exhaustion (a headroom query turns false
// by itself: lap 1 leaves its poll spin, a lap >= 2 skips its poll with
// $s0 == 0 and 0x17d860 leaves the loop) and the read completes on a later
// frame. In this engine T0 is a pure function of the emu tick and is frozen
// inside a slice by design (the wall sub-tick term was the nondeterminism
// source: BUG-014, BUG-031, BUG-032), so inside a slice no headroom query can
// turn false on its own; and the in-flight read is delivered by the boundary
// model only (LoaderModel, boundary slot 4), so it cannot land inside the
// slice either. When both hold, every remaining poll of this slice is
// outcome-invariant (it answers "pending" and nothing else changes) and this
// seam answers "no headroom" (v0 = 0) -- the exit the hardware's clock would
// have produced. Completion status, the slot table and guest memory are never
// touched: the loop leaves through the title's own headroom = 0 path, the
// frame ends, the boundary delivers the read, the next frame's poll consumes it.
//
// Guards (ALL must hold, else the original body runs untouched):
//   (g2) ra is one of the two engaged sites: Layout::spin_site_ra (lap 1) or
//        Layout::lap2_site_ra (lap >= 2). Any other ra -- including the loop's
//        other manager queries and the lap-end query 0x17d87c -- is not a
//        decision at all (not counted) and keeps the real predicate.
//   (g3) the lap flags match the site:
//        lap 1: GPR[Layout::poll_ran_reg] ($s0) != 0 -- this lap's poll ran
//               and answered pending (set in the bnel delay slot at 0x17d73c),
//               so the note below is this lap's own;
//        lap >= 2: GPR[Layout::lap_counter_reg] ($s1) != 0 AND $s0 == 0 -- the
//               query PRECEDES the lap's single poll (0x17d6e8 zeroed $s0), so
//               the poll whose outcome the note reflects is the PREVIOUS lap's
//               (or lap 1's spin). No boundary passes inside the spin, so a
//               read in flight now was in flight at that poll: the note the
//               seam produces on the title's behalf is that poll's outcome.
//   (g4) LoaderModel::ShouldYieldPollSlice: the last poll noted on the model
//        was HeldInflight AND the read is still in flight now (the model's own
//        re-verification -- a read already delivered can be consumed by the
//        very next poll and must not be skipped). The title's poll reads the
//        modelled slot byte directly, not through a seam, so the seam polls the
//        model on its behalf here (Deps::poll_in_flight) before the rule decides.
// Anything short of all three declines (counted) and the recompiled query
// runs as written. Counters per site: LiftsLap1(), LiftsLap2(), Declines()
// -> run.meta loader_spin_lifts=lap1=<n>,lap2=<n>,declines=<m>.
//
// Poll visibility (LoaderModel::PollVisibility(), cdvd.delivery_visibility =
// poll) adds the IOP's half of the hardware rule at the same two sites, after
// (g2)/(g3) hold and (g4) did not yield. A lap whose poll found NO host-served
// read in flight (the title's loop still asks: its loader state machine is
// busy on a record the STREAM module serves, or on nothing the drive owes) is
// waiting on the IOP, which on hardware runs on its own clock while the EE
// spins: the seam runs the IOP's work due inside this frame NOW through the
// owner's PumpWithinFrame (the kernel drained to the frame ceiling without a
// frame-base advance, the queued SIF RPC end-callbacks delivered) and answers
//   "headroom" (the original predicate runs: the title's next poll sees what
//   the IOP did) when that pump reports the IOP RAN (a thread, a completion,
//   a beat -- the owner decides what counts; a delivered reply alone does not,
//   or a status round trip the title re-asks every lap would hold the frame
//   forever while the module's read is due past the edge), else
//   "no headroom" (v0 = 0, the lift) when nothing is due inside the frame:
//   the lap exits through the title's own path, the frame ends at the
//   boundary, the state machine resumes next frame.
// A read that landed at this poll (the model consumed it) is never lifted
// over: the next iteration consumes it. Boundary mode never reaches this
// branch. Counters: LiftsIdle() (nothing due), IopPumps() (ran) ->
// run.meta loader_spin_lifts=...,idle=<n>,iop_pumps=<n>.
//
// The module round trip's cost (poll visibility; the reference measurement:
// on the console the STREAM reply processor FUN_001e1f48 is entered in bursts
// of replies 48k-57k EE cycles apart, about 160-190 us = one SIF RPC round
// trip -- the send DMA, the module's handler, the reply DMA -- and about a
// hundred of them fit a frame; the title issues the next request as soon as
// a reply lets it). The EE's instant inside the frame is the IOP's in-frame
// clock, the one sub-frame quantity the engine models: every round trip the
// title issues (Ship(0) at 0x1e2958 -> sceSifCallRpc -> the module) advances
// it by Layout::stream_rpc_service_us (the bridge charges it; the root binds
// the value under poll only). At the two sites a round trip completed since
// the previous decision is noted at every decision (it belongs to the lap that
// issued it, whether (g4), a landed read or the reply rule ends that lap);
// when (g4) did not yield, nothing landed or is in flight and the lap had one:
//   the IOP's work due at or before its instant runs (the owner's
//   PumpAtInstant: the module's drive completion if the instant passed it,
//   the EE-side service, the queued end-callbacks -- the reply is visible to
//   the title's next poll), then
//   "headroom" while the instant lies inside the slice (a reply IS progress:
//   the title issues its next request), else
//   "no headroom" (v0 = 0, the lift): the frame's modelled time is consumed;
//   the frame ends at the boundary, the IOP's clock carries the overshoot.
// The round trips per frame therefore fall out of the slice and the service
// time (about 16667 / 160), with the drive's gaps in between -- no per-frame
// count of ours. A lap with no round trip keeps the rule above (the IOP's
// dues inside the frame; nothing due = the frame ends). Counters: RpcLaps()
// (continued on a reply), LiftsEdge() (the slice consumed) ->
// run.meta loader_spin_lifts=...,rpc=<n>,edge=<n>.
//
// Why the lap >= 2 site is engaged here although OLD's guard refused it: OLD
// delivered reads on a wall timer inside the frame, so skipping a lap's poll
// could defer a consume by a whole frame (a liveness cost). With boundary-only
// delivery the skipped poll could only have answered "pending".
//
// Installed by the composition root over the generated body of the headroom
// query (Capture / Install, as PadReadinessHook is). Title data (addresses,
// the flag registers) lives in Layout here, never in engine/.
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"

class LoaderModel;

class LoaderSpinLift {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using CaptureFn = std::function<Body(uint32_t addr)>;
    using InstallFn = std::function<bool(uint32_t addr, Body body)>;
    // Polls the model for the reads in flight on the title's behalf (the note
    // ShouldYieldPollSlice consumes); returns whether one is still in flight.
    using PollInFlightFn = std::function<bool()>;
    // Runs the IOP's work due inside the current frame (no frame-base advance)
    // and the queued RPC end-callbacks on `ctx`; true when anything ran.
    using PumpWithinFrameFn = std::function<bool(CpuContext&)>;
    // The IOP's instant as the owner sees it: the round trips completed so far
    // (the bridge's completed tickets) and whether the IOP's in-frame clock
    // still lies inside the slice (its instant before the frame ceiling).
    struct IopInstant {
        uint64_t round_trips = 0;
        bool time_left = true;
    };
    using IopInstantFn = std::function<IopInstant()>;

    struct Layout {
        uint32_t fn_headroom_query = 0x191bd0;   // FUN_00191bd0(obj, f12 seconds) -> v0 = 1 while headroom >= f12
        uint32_t spin_site_ra = 0x17d724;        // lap 1: the poll spin's return address (jal at 0x17d71c)
        uint32_t lap2_site_ra = 0x17d75c;        // lap >= 2: the single-poll gate's return address (jal at 0x17d754); 0 = not engaged
        uint32_t poll_ran_reg = 16;              // $s0: 1 once a poll of this lap answered pending
        uint32_t lap_counter_reg = 17;           // $s1: the lap counter (0 in lap 1)
        // One STREAM.IRX RPC round trip's IOP time (microseconds): the reference
        // console runs the title's status replies 48.0k-56.8k EE cycles apart
        // inside a burst (294.912 MHz: 163-193 us); the low end is the round trip
        // itself, the rest the title's own work between them. Hardware data, not a
        // rate of ours: 0 disables the charge.
        uint64_t stream_rpc_service_us = 160;
    };

    LoaderSpinLift(LoaderModel& model, PollInFlightFn poll_in_flight);
    LoaderSpinLift(LoaderModel& model, PollInFlightFn poll_in_flight, const Layout& layout);

    // Wraps the body at Layout::fn_headroom_query; false when nothing is
    // installed (address 0 or no captured body).
    bool Install(const CaptureFn& capture, const InstallFn& install);
    // Binds the IOP pump the poll-visibility branch uses (unbound: that branch
    // declines, as boundary mode does).
    void SetPumpWithinFrame(PumpWithinFrameFn pump) { pump_within_frame_ = std::move(pump); }
    // Binds the IOP's instant and the pump that runs its work due at or before
    // that instant (the reply-lap branch; unbound: a round trip is not seen and
    // the lap falls to the nothing-due rule).
    void SetIopInstant(IopInstantFn instant) { iop_instant_ = std::move(instant); }
    void SetPumpAtInstant(PumpWithinFrameFn pump) { pump_at_instant_ = std::move(pump); }

    // The seam's own step (exposed for the test): decides for the query
    // entered with `ctx`. True = lifted (v0 := 0, the original body must not
    // run); false = the original body runs.
    bool Decide(CpuContext& ctx);

    bool Installed() const { return installed_; }
    uint64_t Lifts() const { return lifts_lap1_ + lifts_lap2_; }
    uint64_t LiftsLap1() const { return lifts_lap1_; }
    uint64_t LiftsLap2() const { return lifts_lap2_; }
    uint64_t Declines() const { return declines_; }
    uint64_t LiftsIdle() const { return lifts_idle_; }   // poll visibility: nothing due inside the frame
    uint64_t IopPumps() const { return iop_pumps_; }     // poll visibility: the IOP ran at the poll
    uint64_t RpcLaps() const { return rpc_laps_; }       // poll visibility: a round trip completed, time left -> the lap continued
    uint64_t LiftsEdge() const { return lifts_edge_; }   // poll visibility: a round trip consumed the slice -> the frame ends
    const Layout& GetLayout() const { return layout_; }

private:
    LoaderModel& model_;
    PollInFlightFn poll_in_flight_;
    PumpWithinFrameFn pump_within_frame_;
    PumpWithinFrameFn pump_at_instant_;
    IopInstantFn iop_instant_;
    Layout layout_;
    bool installed_ = false;
    uint64_t lifts_lap1_ = 0;
    uint64_t lifts_lap2_ = 0;
    uint64_t declines_ = 0;
    uint64_t lifts_idle_ = 0;
    uint64_t iop_pumps_ = 0;
    uint64_t rpc_laps_ = 0;
    uint64_t lifts_edge_ = 0;
    uint64_t last_round_trips_ = 0;
};
