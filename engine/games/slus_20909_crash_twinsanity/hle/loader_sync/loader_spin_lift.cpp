// LoaderSpinLift implementation; the seam and its guards are described in the header.
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_spin_lift.h"

#include "engine/cdvd/hle/loader_model.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

LoaderSpinLift::LoaderSpinLift(LoaderModel& model, PollInFlightFn poll_in_flight)
    : LoaderSpinLift(model, std::move(poll_in_flight), Layout{}) {}

LoaderSpinLift::LoaderSpinLift(LoaderModel& model, PollInFlightFn poll_in_flight, const Layout& layout)
    : model_(model), poll_in_flight_(std::move(poll_in_flight)), layout_(layout) {}

bool LoaderSpinLift::Install(const CaptureFn& capture, const InstallFn& install) {
    if (layout_.fn_headroom_query == 0 || !capture || !install) return false;
    Body original = capture(layout_.fn_headroom_query);
    if (!original) return false;
    installed_ = install(layout_.fn_headroom_query, [this, original](CpuContext& ctx, uint32_t addr) {
        if (Decide(ctx)) return;   // v0 = 0 already: the loop leaves through the title's own path
        original(ctx, addr);       // every other query keeps the real headroom predicate
    });
    return installed_;
}

bool LoaderSpinLift::Decide(CpuContext& ctx) {
    const uint32_t ra = ctx.cpuRegs.GPR.r[31].UL[0];
    const bool lap1 = ra == layout_.spin_site_ra;
    const bool lap2 = layout_.lap2_site_ra != 0 && ra == layout_.lap2_site_ra;
    if (!lap1 && !lap2) return false;   // (g2) neither engaged site: not a decision
    const bool poll_ran = ctx.cpuRegs.GPR.r[layout_.poll_ran_reg & 31].UL[0] != 0;
    const bool later_lap = ctx.cpuRegs.GPR.r[layout_.lap_counter_reg & 31].UL[0] != 0;
    // (g3) lap 1: this lap's poll ran (pending). lap >= 2: the query precedes
    // the lap's poll, so the note reflects the previous lap's poll.
    const bool flags_match = lap1 ? poll_ran : (later_lap && !poll_ran);
    // The poll answered pending from the modelled slot byte; note that on the
    // model (HeldInflight while the read is in flight, Progress otherwise). A
    // read the drive finished inside the frame lands here (poll visibility).
    auto* files = StreamLibrary::Scheduling();
    const auto completed = [&] { return files ? files->DeliveredAndConsumed() : model_.Delivered() + model_.Consumed(); };
    const bool poll_visibility = files ? files->PollVisibility() : model_.PollVisibility();
    const uint64_t landed_before = completed();
    bool in_flight = false;
    if (flags_match && poll_in_flight_) in_flight = poll_in_flight_();
    const uint32_t site = lap1 ? layout_.spin_site_ra : layout_.lap2_site_ra;   // the engaged site the model checks ra against
    // Poll visibility: did a module round trip complete since the previous
    // decision (the title shipped a request inside this lap and the IOP's
    // clock carries its service)? Noted at EVERY decision, before (g4): a round
    // trip belongs to the lap that issued it, whatever ends that lap.
    const bool reply_rule = flags_match && poll_visibility && iop_instant_;
    bool round_trip = false;
    if (reply_rule) {
        const uint64_t round_trips = iop_instant_().round_trips;
        round_trip = round_trips != last_round_trips_;
        last_round_trips_ = round_trips;
    }
    if (files ? files->ShouldYieldPollSlice(ra, site, flags_match) :
                model_.ShouldYieldPollSlice(ra, site, flags_match)) {
        if (lap1) ++lifts_lap1_; else ++lifts_lap2_;
        ctx.cpuRegs.GPR.r[2].UD[0] = 0;   // "no headroom": the exit the hardware's clock would have produced
        return true;
    }
    // A round trip this lap, nothing landed or in flight on the host side: the
    // IOP's work due at or before its instant runs and the reply is visible; the
    // lap continues while the slice has modelled time left, else the frame ends.
    const bool landed = completed() != landed_before;
    if (reply_rule) {
        if (round_trip && !landed && !in_flight) {
            if (pump_at_instant_) pump_at_instant_(ctx);
            if (iop_instant_().time_left) {
                ++rpc_laps_;
                ++declines_;   // "headroom": a reply is progress, the title issues its next request
                return false;
            }
            ++lifts_edge_;
            ctx.cpuRegs.GPR.r[2].UD[0] = 0;   // the frame's modelled time is consumed: the frame ends at the boundary
            return true;
        }
    }
    // Poll visibility, no host-served read in flight and none landed at this
    // poll: the lap waits on the IOP. Its work due inside the frame runs now;
    // nothing due = the frame ends.
    if (flags_match && !in_flight && !landed && pump_within_frame_ && poll_visibility) {
        if (pump_within_frame_(ctx)) {
            ++iop_pumps_;
            ++declines_;   // "headroom": the title's next poll observes the IOP's progress
            return false;
        }
        ++lifts_idle_;
        ctx.cpuRegs.GPR.r[2].UD[0] = 0;   // nothing due inside this frame: the frame ends at the boundary
        return true;
    }
    ++declines_;
    return false;
}
