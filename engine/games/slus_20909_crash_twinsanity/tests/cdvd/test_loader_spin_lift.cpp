// LoaderSpinLift pin test: the load-wait loop's lift fires ONLY when both
// guards hold and the model's rule holds (the noted poll was held in flight
// AND the read is still in flight now), at either engaged site:
//   lap 1 (ra 0x17d724): this lap's poll ran ($s0 != 0);
//   lap >= 2 (ra 0x17d75c): the query precedes the lap's poll ($s1 != 0,
//   $s0 == 0), the note is the previous lap's poll.
// A read that CAN land in-slice -- already delivered by the boundary
// (DeliverDue) or by the blocking wait (CompleteNow) before the query -- never
// lifts at either site, nor does a lap with no in-flight read. Any other ra
// (the lap-end query 0x17d87c) is not a decision (no decline counted). The
// original body's result stands whenever the lift declines. Rig: LoaderModel +
// DeliveryMeter (Instant), a fake "original" body that answers headroom = 1
// and counts its runs, the hook installed through Capture/Install lambdas over
// a one-entry table.
// Poll visibility (t14-t17, a Poll-mode rig with a fake IOP pump): a lap with
// no host read in flight runs the pump -- it ran something (an IOP thread, a
// drive completion, a beat; the owner decides -- a delivered reply alone does
// not count) -> no lift, the title's loop iterates; nothing due -> the lift
// (v0 = 0, the frame ends); a read the drive finished inside the frame lands at
// the lift-side poll -> iterate, no pump; a read past the frame's edge -> the
// (g4) yield, no pump. Boundary mode never calls the pump (t5/t12 above:
// declines). The module round trip's cost (t19-t21, a fake IOP instant): each
// round trip advances the IOP's in-frame clock by Layout::stream_rpc_service_us;
// a reply lap continues while the slice has time left (16,667 / 160 = 104 round
// trips, then the edge lift), the owner's pump-at-instant runs at every reply
// lap, a landed read iterates and a read past the edge yields before the reply
// rule, and boundary mode never takes the branch.
#include <cstdio>
#include <cstring>
#include <map>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_spin_lift.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

namespace {

ps2r::CdvdConfig MakeInstant() {
    ps2r::CdvdConfig c;
    c.delivery_visibility = ps2r::CdvdDeliveryVisibility::Boundary;   // this case tests the boundary model (the default is poll since gate S4)
    c.latency_model = ps2r::CdvdLatencyModel::Instant;
    return c;
}

ps2r::CdvdConfig MakePoll() {
    ps2r::CdvdConfig c;
    c.latency_model = ps2r::CdvdLatencyModel::Modelled;
    c.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    c.poll_latency_scale = 1.0;
    return c;
}

constexpr uint32_t kFs = 0x00bb35a8;   // the title's file object (any RAM address)
constexpr uint32_t kLapEndSiteRa = 0x17d87c;   // FUN_0017d6b8's lap-end headroom query (never engaged)

// The rig: data only (the free functions below drive it).
bool g_poll_rig = false;   // set before constructing a poll-visibility Rig (data-only rigs: no constructor)

struct Rig {
    ps2r::CdvdConfig cfg = g_poll_rig ? MakePoll() : MakeInstant();
    DeliveryMeter meter{cfg};
    DeliveryBudget budget{0};
    LoaderModel model{meter, budget};
    LoaderSpinLift::Layout layout{};
    // The seams' poll on the title's behalf: this rig has one file object.
    LoaderSpinLift lift{model, [this] { return model.Poll(kFs) == LoaderModel::PollOutcome::HeldInflight; }, layout};
    std::map<uint32_t, LoaderSpinLift::Body> table;   // the one-entry dispatch surface
    int original_runs = 0;
};

// The fake original body: the real predicate answering "headroom left" (v0 = 1).
void SeedOriginal(Rig& r) {
    r.table[r.layout.fn_headroom_query] = [&r](CpuContext& ctx, uint32_t) {
        ++r.original_runs;
        ctx.cpuRegs.GPR.r[2].UD[0] = 1;
    };
}

bool InstallHook(Rig& r) {
    SeedOriginal(r);
    return r.lift.Install([&r](uint32_t a) { return r.table.count(a) ? r.table[a] : LoaderSpinLift::Body{}; },
                          [&r](uint32_t a, LoaderSpinLift::Body b) {
                              if (!r.table.count(a)) return false;
                              r.table[a] = std::move(b);
                              return true;
                          });
}

// Enters the headroom query as the title does: ra = the call site's return
// address, $s0 = the lap's poll flag, $s1 = the lap counter (0 in lap 1),
// v0 poisoned so the answer is visible.
uint32_t Query(Rig& r, uint32_t ra, uint32_t poll_ran, uint32_t lap = 0) {
    CpuContext ctx{};
    std::memset(&ctx, 0, sizeof ctx);
    ctx.cpuRegs.GPR.r[31].UL[0] = ra;
    ctx.cpuRegs.GPR.r[r.layout.poll_ran_reg].UD[0] = poll_ran;
    ctx.cpuRegs.GPR.r[r.layout.lap_counter_reg].UD[0] = lap;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xdead;
    r.table[r.layout.fn_headroom_query](ctx, r.layout.fn_headroom_query);
    return ctx.cpuRegs.GPR.r[2].UL[0];
}

}  // namespace

int main() {
    {   // (1) both guards, the read in flight -> lifts once: v0 = 0, the original body did not run, the note consumed
        Rig r;
        CHECK(InstallHook(r) && r.lift.Installed(), "t1_hook_installs_over_the_headroom_query");
        int applied = 0;
        CHECK(r.model.Enqueue(kFs, 5000, 13, /*issue*/ 10, [&] { applied = 1; }) == 11 && r.model.InFlight(kFs),
              "t1_read_in_flight_until_the_boundary");
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 0, "t1_lift_answers_no_headroom_v0_0");
        CHECK(r.lift.Lifts() == 1 && r.lift.Declines() == 0, "t1_lift_counter_1_no_decline");
        CHECK(r.original_runs == 0, "t1_original_body_did_not_run");
        CHECK(r.model.LastPoll() == LoaderModel::PollOutcome::None && r.model.Yields() == 1, "t1_model_note_consumed_by_the_yield");
        CHECK(r.model.InFlight(kFs) && applied == 0, "t1_lift_touched_no_completion_state");
        // The next lap's FIRST query (the poll flag is 0 again): the real predicate runs.
        const uint32_t v0b = Query(r, r.layout.spin_site_ra, 0);
        CHECK(v0b == 1 && r.original_runs == 1 && r.lift.Lifts() == 1 && r.lift.Declines() == 1,
              "t1_next_lap_first_query_keeps_the_real_predicate");
    }
    {   // (2) the poll did not run this lap ($s0 == 0) -> no lift, decline counted, the original answer stands
        Rig r;
        InstallHook(r);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 0);
        CHECK(v0 == 1 && r.original_runs == 1, "t2_poll_not_run_original_result_untouched");
        CHECK(r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t2_decline_counted");
        CHECK(r.model.InFlight(kFs), "t2_read_still_in_flight");
    }
    {   // (3) any other query of the loop (the lap-end query, ra 0x17d87c): not a decision (no lift, no decline)
        Rig r;
        InstallHook(r);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        r.model.Poll(kFs);   // a held poll, as the lap's poll would leave it
        const uint32_t v0 = Query(r, kLapEndSiteRa, 1, 3);
        CHECK(v0 == 1 && r.original_runs == 1, "t3_other_site_runs_the_original");
        CHECK(r.lift.Lifts() == 0 && r.lift.Declines() == 0 && r.model.Declines() == 0, "t3_other_site_is_not_a_decision");
        CHECK(r.model.LastPoll() == LoaderModel::PollOutcome::HeldInflight, "t3_note_left_untouched");
    }
    {   // (4a) the read CAN land in-slice: the boundary delivered it before the query -> no lift (re-verified)
        Rig r;
        InstallHook(r);
        int applied = 0;
        const uint64_t due = r.model.Enqueue(kFs, 5000, 13, 10, [&] { applied = 1; });
        CHECK(r.model.Poll(kFs) == LoaderModel::PollOutcome::HeldInflight, "t4a_poll_before_the_boundary_is_held");
        CHECK(r.meter.DeliverDue(due) == 1 && applied == 1 && r.model.Released(kFs), "t4a_boundary_delivers_before_the_query");
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 1 && r.original_runs == 1, "t4a_delivered_read_does_not_lift");
        CHECK(r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t4a_decline_counted");
        CHECK(!r.model.InFlight(kFs) && !r.model.Released(kFs) && r.model.Consumed() == 1,
              "t4a_the_lift_side_poll_consumed_the_released_read");
    }
    {   // (4b) the blocking wait completed it (CompleteNow) before the query -> no lift
        Rig r;
        InstallHook(r);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        r.model.Poll(kFs);
        CHECK(r.model.CompleteNow(kFs) && !r.model.InFlight(kFs), "t4b_wait_completes_before_the_query");
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t4b_completed_read_does_not_lift");
    }
    {   // (4c) a stale held note with the read landed between the poll and the query (no lift-side poll) -> the model re-verifies
        Rig r;
        LoaderSpinLift bare(r.model, {}, r.layout);   // no poll producer: only the note the lap left
        const uint64_t due = r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        r.model.Poll(kFs);
        r.meter.DeliverDue(due);
        CpuContext ctx{};
        std::memset(&ctx, 0, sizeof ctx);
        ctx.cpuRegs.GPR.r[31].UL[0] = r.layout.spin_site_ra;
        ctx.cpuRegs.GPR.r[r.layout.poll_ran_reg].UD[0] = 1;
        CHECK(!bare.Decide(ctx) && bare.Declines() == 1 && bare.Lifts() == 0, "t4c_stale_held_note_re_verified_no_lift");
    }
    {   // (5) no in-flight read at all (the poll noted Progress) -> no lift
        Rig r;
        InstallHook(r);
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t5_no_in_flight_read_does_not_lift");
        CHECK(r.model.LastPoll() == LoaderModel::PollOutcome::Progress, "t5_poll_noted_progress");
    }
    {   // (7) lap >= 2 site, the previous lap's poll held AND the read still in flight -> lifts once (lap2 counter), v0 = 0
        Rig r;
        InstallHook(r);
        int applied = 0;
        r.model.Enqueue(kFs, 5000, 13, 10, [&] { applied = 1; });
        // Lap 1 ended with the lift (its note consumed); lap 2's query precedes its poll: $s1 = 1, $s0 = 0.
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 0 && r.lift.LiftsLap1() == 1, "t7_lap1_lift_first");
        const uint32_t v0 = Query(r, r.layout.lap2_site_ra, 0, 1);
        CHECK(v0 == 0, "t7_lap2_lift_answers_no_headroom_v0_0");
        CHECK(r.lift.LiftsLap2() == 1 && r.lift.Lifts() == 2 && r.lift.Declines() == 0 && r.original_runs == 0, "t7_lap2_counter_1_no_decline");
        CHECK(r.model.InFlight(kFs) && applied == 0 && r.model.Yields() == 2, "t7_lift_touched_no_completion_state");
        // Lap 36275 of the same slice: still in flight -> still the hardware's exit.
        CHECK(Query(r, r.layout.lap2_site_ra, 0, 36275) == 0 && r.lift.LiftsLap2() == 2, "t7_later_lap_same_slice_lifts");
    }
    {   // (8) lap >= 2 site with the lap's poll flag already set ($s0 != 0): not the gate's state -> no lift, decline counted
        Rig r;
        InstallHook(r);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        r.model.Poll(kFs);
        const uint32_t v0 = Query(r, r.layout.lap2_site_ra, 1, 2);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t8_lap2_with_poll_flag_set_declines");
    }
    {   // (9) lap-2 site ra but the lap counter says lap 1 ($s1 == 0): flags do not match -> no lift, decline counted
        Rig r;
        InstallHook(r);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        r.model.Poll(kFs);
        const uint32_t v0 = Query(r, r.layout.lap2_site_ra, 0, 0);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t9_lap2_site_in_lap1_declines");
    }
    {   // (10) lap >= 2, the read CAN land in-slice: delivered (DeliverDue) before the query -> no lift; the poll consumes it
        Rig r;
        InstallHook(r);
        int applied = 0;
        const uint64_t due = r.model.Enqueue(kFs, 5000, 13, 10, [&] { applied = 1; });
        CHECK(r.model.Poll(kFs) == LoaderModel::PollOutcome::HeldInflight, "t10_previous_lap_poll_was_held");
        CHECK(r.meter.DeliverDue(due) == 1 && applied == 1 && r.model.Released(kFs), "t10_boundary_delivers_before_the_query");
        const uint32_t v0 = Query(r, r.layout.lap2_site_ra, 0, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t10_lap2_delivered_read_does_not_lift");
        CHECK(!r.model.InFlight(kFs) && r.model.Consumed() == 1, "t10_the_lift_side_poll_consumed_the_released_read");
    }
    {   // (11) lap >= 2, the blocking wait completed it (CompleteNow) before the query -> no lift
        Rig r;
        InstallHook(r);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        r.model.Poll(kFs);
        CHECK(r.model.CompleteNow(kFs) && !r.model.InFlight(kFs), "t11_wait_completes_before_the_query");
        const uint32_t v0 = Query(r, r.layout.lap2_site_ra, 0, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t11_lap2_completed_read_does_not_lift");
    }
    {   // (12) lap >= 2, no in-flight read at all -> no lift
        Rig r;
        InstallHook(r);
        const uint32_t v0 = Query(r, r.layout.lap2_site_ra, 0, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && r.lift.Lifts() == 0 && r.lift.Declines() == 1, "t12_lap2_no_in_flight_read_does_not_lift");
    }
    {   // (13) lap2_site_ra = 0 disables the lap >= 2 site (the veto path): its query is not a decision; lap 1 unchanged
        Rig r;
        LoaderSpinLift::Layout l = r.layout;
        l.lap2_site_ra = 0;
        LoaderSpinLift lift(r.model, [&r] { return r.model.Poll(kFs) == LoaderModel::PollOutcome::HeldInflight; }, l);
        r.model.Enqueue(kFs, 5000, 13, 10, [] {});
        CpuContext ctx{};
        std::memset(&ctx, 0, sizeof ctx);
        ctx.cpuRegs.GPR.r[31].UL[0] = r.layout.lap2_site_ra;
        ctx.cpuRegs.GPR.r[l.lap_counter_reg].UD[0] = 1;
        CHECK(!lift.Decide(ctx) && lift.Declines() == 0 && lift.Lifts() == 0, "t13_lap2_site_disabled_is_not_a_decision");
        ctx.cpuRegs.GPR.r[31].UL[0] = l.spin_site_ra;
        ctx.cpuRegs.GPR.r[l.lap_counter_reg].UD[0] = 0;
        ctx.cpuRegs.GPR.r[l.poll_ran_reg].UD[0] = 1;
        CHECK(lift.Decide(ctx) && lift.LiftsLap1() == 1 && ctx.cpuRegs.GPR.r[2].UD[0] == 0, "t13_lap1_still_lifts");
    }
    {   // (6) nothing to install: address 0 or no captured body
        Rig r;
        LoaderSpinLift::Layout none = r.layout;
        none.fn_headroom_query = 0;
        LoaderSpinLift unbound(r.model, {}, none);
        CHECK(!unbound.Install([](uint32_t) { return LoaderSpinLift::Body{}; }, [](uint32_t, LoaderSpinLift::Body) { return true; }) &&
                  !unbound.Installed(),
              "t6_address_0_installs_nothing");
        LoaderSpinLift nobody(r.model, {}, r.layout);
        CHECK(!nobody.Install([](uint32_t) { return LoaderSpinLift::Body{}; }, [](uint32_t, LoaderSpinLift::Body) { return true; }),
              "t6_no_captured_body_installs_nothing");
    }
    {   // (14) poll visibility, nothing in flight, the IOP has nothing due inside the frame -> the lift (idle), v0 = 0
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        int pumps = 0;
        r.lift.SetPumpWithinFrame([&pumps](CpuContext&) { ++pumps; return false; });
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 0 && r.original_runs == 0, "t14_nothing_due_lifts_v0_0");
        CHECK(pumps == 1 && r.lift.LiftsIdle() == 1 && r.lift.IopPumps() == 0 && r.lift.Lifts() == 0 && r.lift.Declines() == 0,
              "t14_pump_asked_once_idle_counted");
        // The lap >= 2 site: the same rule (the frame ends; the SM resumes next frame).
        CHECK(Query(r, r.layout.lap2_site_ra, 0, 1) == 0 && pumps == 2 && r.lift.LiftsIdle() == 2, "t14_lap2_site_nothing_due_lifts");
        // Not an engaged site: not a decision, the pump is not asked.
        CHECK(Query(r, kLapEndSiteRa, 1, 3) == 1 && pumps == 2 && r.lift.Declines() == 0, "t14_other_site_never_pumps");
    }
    {   // (15) poll visibility, an RPC end-callback queued / module work due -> pumped at the poll, the loop iterates
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        int due = 1;   // one item of IOP work due inside the frame
        r.lift.SetPumpWithinFrame([&due](CpuContext&) { return due-- > 0; });
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 1 && r.original_runs == 1, "t15_pumped_work_keeps_headroom");
        CHECK(r.lift.IopPumps() == 1 && r.lift.Declines() == 1 && r.lift.LiftsIdle() == 0, "t15_iop_pump_counted_as_a_decline");
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 0 && r.lift.LiftsIdle() == 1, "t15_then_nothing_due_lifts");
    }
    {   // (16) poll visibility, a read the drive finishes inside the frame lands at the lift-side poll -> iterate, no pump
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        int pumps = 0;
        r.lift.SetPumpWithinFrame([&pumps](CpuContext&) { ++pumps; return false; });
        int applied = 0;
        r.model.Enqueue(kFs, 0, 2, tick, [&] { applied = 1; });   // contiguous 2 sectors from the head: ends inside frame 10
        CHECK(r.model.InFlight(kFs) && applied == 0, "t16_read_in_flight_before_the_query");
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && applied == 1, "t16_landed_read_keeps_headroom");
        CHECK(pumps == 0 && r.lift.Lifts() == 0 && r.lift.LiftsIdle() == 0 && r.lift.Declines() == 1, "t16_no_pump_no_lift");
        CHECK(!r.model.InFlight(kFs) && r.model.PollDelivered() == 1, "t16_delivered_at_the_poll");
    }
    {   // (17) poll visibility, a read in flight past the frame's edge -> the (g4) yield, the pump is not asked
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        int pumps = 0;
        r.lift.SetPumpWithinFrame([&pumps](CpuContext&) { ++pumps; return true; });
        r.model.Enqueue(kFs, 900000, 40, tick, [] {});   // a long seek + 40 sectors: past the edge
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 0 && r.lift.LiftsLap1() == 1 && r.lift.LiftsIdle() == 0 && pumps == 0, "t17_in_flight_past_the_edge_yields_without_pumping");
        CHECK(r.model.InFlight(kFs) && r.model.Yields() == 1, "t17_read_still_in_flight");
    }
    {   // (18) boundary mode with a pump bound: the branch is never taken (bit for bit the t5 behaviour)
        Rig r;
        InstallHook(r);
        int pumps = 0;
        r.lift.SetPumpWithinFrame([&pumps](CpuContext&) { ++pumps; return false; });
        const uint32_t v0 = Query(r, r.layout.spin_site_ra, 1);
        CHECK(v0 == 1 && r.original_runs == 1 && pumps == 0 && r.lift.Declines() == 1 && r.lift.LiftsIdle() == 0,
              "t18_boundary_mode_never_pumps_or_lifts_idle");
    }
    {   // (19) poll visibility, the module round trip costs S us of the frame: a frame of 16,667 us admits floor(16667/S)
        // reply laps (each lap ships one request, the IOP's clock advances by S), then the slice is consumed -> the lift (edge)
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        const uint64_t S = r.layout.stream_rpc_service_us;   // the title's data (160 on the reference)
        const uint64_t kFrameUs = 16667;
        uint64_t iop_now = 0, round_trips = 0;
        int at_instant = 0, within = 0;
        r.lift.SetIopInstant([&] { return LoaderSpinLift::IopInstant{round_trips, iop_now < kFrameUs}; });
        r.lift.SetPumpAtInstant([&at_instant](CpuContext&) { ++at_instant; return false; });
        r.lift.SetPumpWithinFrame([&within](CpuContext&) { ++within; return false; });
        auto ship = [&] { ++round_trips; iop_now += S; };   // the title's Ship(0): one round trip, charged by the bridge
        int laps = 0;
        for (;;) {
            ship();
            if (Query(r, r.layout.spin_site_ra, 1) == 0) break;
            ++laps;
            if (laps > 1000) break;
        }
        CHECK(S == 160 && laps == static_cast<int>(kFrameUs / S) && laps == 104,
              "t19_a_frame_admits_floor_16667_over_S_round_trips_then_yields");   // 104 x 160 = 16,640 < 16,667; the 105th crosses
        CHECK(r.lift.RpcLaps() == 104 && r.lift.LiftsEdge() == 1 && r.lift.LiftsIdle() == 0 && r.lift.Lifts() == 0,
              "t19_reply_laps_counted_then_one_edge_lift");
        CHECK(at_instant == 105 && within == 0 && r.original_runs == 104, "t19_each_reply_lap_pumped_at_the_instant_never_to_the_ceiling");
        // The boundary moved the base: the overshoot (< S) is the next frame's first instant; its laps continue.
        iop_now -= kFrameUs;
        ship();
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 1 && r.lift.RpcLaps() == 105, "t19_next_frame_continues_on_its_first_reply");
        // A lap with no round trip falls to the nothing-due rule (the pump to the ceiling, idle lift).
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 0 && within == 1 && r.lift.LiftsIdle() == 1, "t19_no_round_trip_lap_keeps_the_nothing_due_rule");
    }
    {   // (20) poll visibility, a drive completion due inside the frame (+9 ms) lands at the poll of a reply lap -> iterate
        // (landed wins, no edge); a read due past the edge -> the (g4) yield before the reply rule
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        uint64_t round_trips = 0;
        bool time_left = true;
        r.lift.SetIopInstant([&] { return LoaderSpinLift::IopInstant{round_trips, time_left}; });
        int at_instant = 0;
        r.lift.SetPumpAtInstant([&at_instant](CpuContext&) { ++at_instant; return false; });
        r.lift.SetPumpWithinFrame([](CpuContext&) { return false; });
        int applied = 0;
        r.model.Enqueue(kFs, 0, 9, tick, [&] { applied = 1; });   // 9 contiguous sectors from the head: ~8.3 ms, inside frame 10
        ++round_trips;
        time_left = false;   // even with the slice consumed...
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 1 && applied == 1 && r.lift.LiftsEdge() == 0 && at_instant == 0,
              "t20_a_read_landing_at_the_poll_iterates_before_the_reply_rule");
        time_left = true;
        r.model.Enqueue(kFs, 900000, 40, tick, [] {});   // a long seek + 40 sectors: past the edge
        ++round_trips;
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 0 && r.lift.LiftsLap1() == 1 && r.lift.RpcLaps() == 0 && at_instant == 0,
              "t20_a_read_past_the_edge_yields_g4_before_the_reply_rule");
        // The round trip of the lap (g4) ended belongs to that lap: once the blocking wait completed the read (no
        // host read in flight, nothing landing at this poll) the next lap without a round trip of its own keeps the
        // nothing-due rule (idle lift) -- it does not continue on the earlier reply.
        CHECK(r.model.CompleteNow(kFs) && !r.model.InFlight(kFs), "t20_the_wait_completes_the_read_past_the_edge");
        CHECK(Query(r, r.layout.spin_site_ra, 1) == 0 && r.lift.LiftsIdle() == 1 && r.lift.RpcLaps() == 0 && at_instant == 0,
              "t20_a_round_trip_of_the_g4_lap_is_not_the_next_laps");
    }
    {   // (21) the lap >= 2 site carries the same reply rule; boundary mode never sees a round trip
        g_poll_rig = true;
        Rig r;
        g_poll_rig = false;
        InstallHook(r);
        uint64_t tick = 10;
        r.model.SetNowTick([&tick] { return tick; });
        uint64_t round_trips = 1;
        bool time_left = true;
        r.lift.SetIopInstant([&] { return LoaderSpinLift::IopInstant{round_trips, time_left}; });
        r.lift.SetPumpAtInstant([](CpuContext&) { return false; });
        r.lift.SetPumpWithinFrame([](CpuContext&) { return false; });
        CHECK(Query(r, r.layout.lap2_site_ra, 0, 1) == 1 && r.lift.RpcLaps() == 1, "t21_lap2_site_continues_on_a_reply");
        time_left = false;
        ++round_trips;
        CHECK(Query(r, r.layout.lap2_site_ra, 0, 2) == 0 && r.lift.LiftsEdge() == 1 && r.lift.LiftsLap2() == 0, "t21_lap2_site_edge_lift");
        Rig b;   // boundary rig
        InstallHook(b);
        uint64_t rt = 5;
        b.lift.SetIopInstant([&] { return LoaderSpinLift::IopInstant{rt, false}; });
        int pumps = 0;
        b.lift.SetPumpAtInstant([&pumps](CpuContext&) { ++pumps; return true; });
        CHECK(Query(b, b.layout.spin_site_ra, 1) == 1 && pumps == 0 && b.lift.RpcLaps() == 0 && b.lift.LiftsEdge() == 0 && b.lift.Declines() == 1,
              "t21_boundary_mode_never_takes_the_reply_branch");
    }
    std::printf("%s\n", g_fail ? "FAILURES" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
