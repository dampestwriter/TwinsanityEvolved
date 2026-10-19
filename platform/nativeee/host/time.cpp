// platform/nativeee (host side) -- time.h: Platform::Time over the engine's pacer, and the frame boundary.
//
// The clock is the engine's: an EmuDomain (the one game-visible tick), DerivedClocks (T0 at BUSCLK/256 = 9600 counts
// per 60 Hz field = Platform::Time::TicksPerSecond 576000 -- a pure function of the tick, no wall value), a TickSource
// (Deterministic: a field per boundary, no thread; Play: a host pacer thread notes one tick per 1/60 s wall edge and
// the boundary parks for it -- BoundaryProtocol::ParkForPacerTick, the wall-locked boundary) and a TickSync (the
// tick body runs ONLY on the game thread). The pacer thread is the engine's FieldPacer (TickThread's cadence as a
// library class: anchor + k * period, re-anchor when more than a period behind).
//
// Platform::Time::Ticks() therefore moves only at boundaries (9600 per field): the same shape the recomp's title sees
// under the engine (DerivedClocks: "the wall term does not exist here"), so the game's per-frame time budget loops
// (GameTimeHasTimeLeft) always find time left within a frame and finish their work by its own termination.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "engine/ee/timing/boundary_protocol.h"
#include "engine/platform/host/platform.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/field_pacer.h"
#include "engine/ee/timing/tick_source.h"
#include "engine/ee/timing/tick_sync.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_weak.h"
#include "te_services.h"

// The graphics side's phases of the last Present (graphics.cpp), for the slow-tick line; weak: a build without the
// graphics bodies (the tests) has no such symbol (te_weak.h: on COFF an absent body resolves to an empty one)
TE_WEAK_REF_VOID(te_graphics_last_present_phases_ms, (double* submit, double* present, double* pump))

namespace te {

struct Clock::Impl {
    EmuDomain domain;
    DerivedClocks clocks{domain};
    TickSource source;
    TickSync ticks;
    BoundaryProtocol protocol{source, ticks};
    bool play = false;
    uint32_t field_hz = 60;
    uint64_t fields = 0;
    // The RTC's epoch (seconds since 1970, in LOCAL civil time already) at construction.
    uint64_t rtc_epoch = DerivedClocks::kRtcFixedEpochS;
    // The pacer thread (Play only): the engine's FieldPacer (engine/ee/timing/field_pacer.h) -- the TickThread cadence
    // (anchor + k * 1e9/hz, re-anchor when more than a period behind) as a library class, consumed since the pin
    // nativeee twinsanityevolved@fd5273dc.
    std::unique_ptr<FieldPacer> pacer;
    // The ticks (Census): the game thread's work between boundaries, by the host's steady clock
    using Steady = std::chrono::steady_clock;
    Steady::time_point released{};
    bool have_release = false;
    uint64_t ticks_measured = 0, ticks_over_field = 0;
    double tick_max_ms = 0, tick_sum_ms = 0, tick_log_ms = -1;   // tick_log_ms < 0: TE_TICK_LOG_MS unset, no per-tick log
    uint64_t released_cpu_ns = 0;                                  // the game thread's CPU clock at the release (the EE's own cost)
    double tick_cpu_sum_ms = 0;

    void StartPacer() {
        // The pacer thread at the interactive quality of service (with the game and render threads): a pacer the
        // host starves (default QoS under load) misses edges -- the clock census's `late` and `timeouts`
        if (!pacer) pacer.reset(new FieldPacer(ticks, protocol, field_hz, [] {
            // the engine's per-platform arm (macOS QoS, Windows SetThreadPriority, Linux the name)
            if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-pacer", ps2r::platform::host::ThreadRole::Pacer);
        }));
        if (!pacer->Running()) pacer->Start();
    }
    void StopPacer() {
        if (pacer) pacer->Stop();
    }
};

Clock::Clock(const Settings& settings) : impl_(new Impl) {
    Impl& i = *impl_;
    i.play = !settings.deterministic;
    i.field_hz = settings.field_hz == 0 ? 60u : settings.field_hz;
    i.source.SetPolicy(i.play ? TickSource::Policy::Play : TickSource::Policy::Deterministic);
    i.source.SetMode(TickSource::Mode::Realtime);
    if (const char* ms = std::getenv("TE_TICK_LOG_MS")) i.tick_log_ms = std::atof(ms);
    // Deterministic profile: the quantum pinned at 1 (a perfect console); Play: the pacer release (every noted tick
    // is one elapsed wall field, rides 1:1).
    i.domain.SetRegimePin(1);
    i.domain.SetPacerRelease(i.play);
    i.clocks.SetRtcFixed(true);
    if (i.play) {
        // The host's local civil time at construction as the RTC epoch (then advanced by the tick only).
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        // Re-encode the LOCAL fields as if UTC so EpochToCivil gives them back.
        const int y = local.tm_year + 1900, m = local.tm_mon + 1, d = local.tm_mday;
        auto days_from_civil = [](int64_t y, unsigned m, unsigned d) -> int64_t {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        };
        i.rtc_epoch = static_cast<uint64_t>(days_from_civil(y, m, d)) * 86400u + local.tm_hour * 3600u +
                      local.tm_min * 60u + local.tm_sec;
    }
    // The tick body: the ONE place a field happens (BoundarySequencer::InstallPhase3Steps's body without the INTC).
    i.ticks.SetBody([this](TickSync::Cause cause) -> uint64_t {
        Impl& im = *impl_;
        const uint64_t raw = im.source.Tick();
        if (cause == TickSync::Cause::Deferred) im.domain.OnDeferredTick(raw);
        else im.domain.OnRawTick(raw);
        ++im.fields;
        return raw;
    });
    i.domain.ReAnchor(0);
    // Through the registry's current instance (not `this`): Services::ResetForTest rebuilds the services, the
    // section registration is once per process.
    savestate::Register(savestate::kTagClock, "te::Clock", false,
                        [](std::vector<uint8_t>& out) { Services::Get().clock().Serialize(out); },
                        [](const savestate::Blob& blob, std::string* err) { return Services::Get().clock().Restore(blob, err); });
}

// The clock's state at a boundary: the boundary/field counts, the Start offset, the RTC epoch, the raw tick and the
// EmuDomain's logical snapshot (its detector included). The pacer (Play) is wall-side and is not state: a loaded
// clock paces from its next boundary.
void Clock::Serialize(std::vector<uint8_t>& out) const {
    const Impl& i = *impl_;
    const EmuDomain::Snapshot d = i.domain.Capture();
    savestate::Put(out, uint32_t(1));
    savestate::Put(out, boundaries_);
    savestate::Put(out, i.fields);
    savestate::Put(out, uint8_t(started_ ? 1 : 0));
    savestate::Put(out, start_counts_);
    savestate::Put(out, i.rtc_epoch);
    savestate::Put(out, i.source.RawCount());
    savestate::Put(out, d.tick); savestate::Put(out, d.base); savestate::Put(out, d.boundary_raw);
    savestate::Put(out, d.arrival_raw); savestate::Put(out, d.boundaries); savestate::Put(out, d.lead);
    savestate::Put(out, d.regime); savestate::Put(out, d.pin);
    savestate::Put(out, uint8_t(d.have_boundary)); savestate::Put(out, uint8_t(d.bypass)); savestate::Put(out, uint8_t(d.pacer_release));
    savestate::Put(out, d.detector.n); savestate::Put(out, d.detector.samples); savestate::Put(out, d.detector.over);
    savestate::Put(out, d.detector.under); savestate::Put(out, uint8_t(d.detector.fast));
    savestate::Put(out, uint32_t(i.play ? 1 : 0));
    savestate::Put(out, i.field_hz);
}

bool Clock::Restore(const savestate::Blob& blob, std::string* err) {
    Impl& i = *impl_;
    savestate::PayloadReader in(blob.payload);
    uint32_t version = 0;
    uint64_t raw = 0;
    uint8_t started = 0, have_boundary = 0, bypass = 0, pacer_release = 0, fast = 0;
    EmuDomain::Snapshot d;
    uint32_t play = 0, hz = 60;
    auto fail = [&](const char* why) { if (err) *err = std::string("te::Clock: ") + why; return false; };
    if (!in.Get(&version) || version != 1) return fail("payload version");
    if (!in.Get(&boundaries_) || !in.Get(&i.fields) || !in.Get(&started) || !in.Get(&start_counts_) ||
        !in.Get(&i.rtc_epoch) || !in.Get(&raw) || !in.Get(&d.tick) || !in.Get(&d.base) || !in.Get(&d.boundary_raw) ||
        !in.Get(&d.arrival_raw) || !in.Get(&d.boundaries) || !in.Get(&d.lead) || !in.Get(&d.regime) || !in.Get(&d.pin) ||
        !in.Get(&have_boundary) || !in.Get(&bypass) || !in.Get(&pacer_release) || !in.Get(&d.detector.n) ||
        !in.Get(&d.detector.samples) || !in.Get(&d.detector.over) || !in.Get(&d.detector.under) || !in.Get(&fast) ||
        !in.Get(&play) || !in.Get(&hz))
        return fail("payload truncated");
    d.have_boundary = have_boundary != 0;
    d.bypass = bypass != 0;
    d.pacer_release = i.play;   // the loading run's policy, not the saved one
    d.detector.fast = fast != 0;
    started_ = started != 0;
    in_frame_ = 0;   // a state is a boundary's
    i.source.ResetForRestore(raw);
    i.have_release = false;   // the tick after a load is not a frame's work
    if (!EmuDomain::Validate(d, raw) || !i.domain.Restore(d, raw)) return fail("EmuDomain snapshot rejected");
    if ((play != 0) != i.play || hz != i.field_hz)
        std::fprintf(stderr, "[twinsanityevolved] clock: state saved under %s %u Hz, loading under %s %u Hz\n",
                     play ? "play" : "deterministic", hz, i.play ? "play" : "deterministic", i.field_hz);
    if (started_ && i.play) i.StartPacer();
    return true;
}

Clock::~Clock() { impl_->StopPacer(); }

void Clock::Start() {
    if (started_) return;
    started_ = true;
    start_counts_ = T0Counts();
    if (impl_->play) impl_->StartPacer();
}

bool Clock::LatestFieldEdge(int64_t* edge_ns, uint64_t* field) const {
    const Impl& i = *impl_;
    if (!i.play || !i.pacer || i.fields == 0) return false;
    if (!i.pacer->EdgeOfTickNs(i.fields, edge_ns)) return false;
    if (field) *field = i.fields;
    return true;
}

double Clock::MsSinceRelease() const {
    const Impl& i = *impl_;
    if (!i.have_release) return 0.0;
    return std::chrono::duration<double, std::milli>(Impl::Steady::now() - i.released).count();
}

uint64_t Clock::T0Counts() const { return impl_->clocks.T0Counts(); }
uint64_t Clock::DomainTick() const { return impl_->domain.DomainTick(); }
uint64_t Clock::Ticks() const { return T0Counts() - start_counts_ + in_frame_; }
uint64_t Clock::TicksPerField() const { return 576000u / (impl_->field_hz ? impl_->field_hz : 60u); }

void Clock::ChargeInFrame(uint64_t ticks) {
    // The cap is the GAME's frame budget, not the field's: the PAL game folds its headroom modulo 576000 / 50 = 11520
    // ticks (GameTimeHasTimeLeft's frameTicks) while the port's fields are 9600 at 60 Hz -- a charge capped at the
    // field could never expire that budget (the spin at boot frame 68 on the first drive-paced read). 11520 covers
    // either disc's frame; the NTSC game's loops expire at 9600 before reaching it.
    in_frame_ = std::min(kChargeCapTicks, in_frame_ + ticks);
}

void Clock::Boundary() {
    Impl& i = *impl_;
    in_frame_ = 0;   // the frame's charges end with it (the next frame's Ticks() start at the field's T0)
    if (i.have_release) {
        // This frame's work: from the previous boundary's release (after its park) to now
        const double ms = std::chrono::duration<double, std::milli>(Impl::Steady::now() - i.released).count();
        i.ticks_measured++;
        i.tick_sum_ms += ms;
        const uint64_t cpu = ps2r::host::ThreadCpuTimeNs();
        if (cpu && i.released_cpu_ns) i.tick_cpu_sum_ms += double(cpu - i.released_cpu_ns) / 1e6;
        if (ms > i.tick_max_ms) i.tick_max_ms = ms;
        if (ms > 1000.0 / double(i.field_hz ? i.field_hz : 60u)) i.ticks_over_field++;
        if (i.tick_log_ms >= 0 && ms > i.tick_log_ms) {
            // The phases: the draws' submission and the present/hand-over on this thread, the window's event pump, the rest
            double submit = 0, present = 0, pump = 0;
            if (TE_WEAK_PRESENT(te_graphics_last_present_phases_ms)) te_graphics_last_present_phases_ms(&submit, &present, &pump);
            std::fprintf(stderr, "[twinsanityevolved] tick: boundary %llu took %.1f ms (draw submit %.1f, present %.1f, pump %.1f, game %.1f)\n",
                         (unsigned long long)(boundaries_ + 1), ms, submit, present, pump, std::max(0.0, ms - submit - present - pump));
        }
    }
    if (i.play && !(i.pacer && i.pacer->Running())) i.StartPacer();   // a boundary before Time::Initialise (the pad setup's WaitVSync)
    i.domain.SetBypass(i.source.Bypass());
    i.domain.NoteArrival(i.source.RawCount());
    if (i.play) {
        // Wall-locked: park for the pacer's edge (an early frame), then fire every noted tick as one elapsed field; a
        // late frame finds its tick(s) noted and does not park. A stopped pacer (timeout) still gets its one field.
        i.protocol.ParkForPacerTick();
        if (i.ticks.FirePending() == 0) i.ticks.FireInline(TickSync::Cause::Boundary);
    } else {
        i.ticks.FireInline(TickSync::Cause::Boundary);
        i.ticks.FirePending();
    }
    i.domain.OnBoundary(i.source.RawCount());
    const uint64_t boundary = ++boundaries_;
    {
        // The hooks: a copy, so a hook may add/remove hooks. Destroyed before the checkpoint on purpose.
        const auto hooks = hooks_;
        for (const Entry& e : hooks) e.hook(boundary);
    }
    i.released = Impl::Steady::now();   // the next tick is measured from here (a save at the checkpoint counts to it)
    i.released_cpu_ns = ps2r::host::ThreadCpuTimeNs();
    i.have_release = true;
    // THE save-state checkpoint (savestate.h): the game fiber may be suspended inside this call and resumed in another
    // process. Nothing of this frame is used after it returns -- keep it the last statement.
    savestate::Checkpoint(boundary);
}

int Clock::AddBoundaryHook(const std::string& name, Hook hook) {
    const int id = next_hook_++;
    hooks_.push_back({id, name, std::move(hook)});
    return id;
}

void Clock::RemoveBoundaryHook(int id) {
    for (size_t k = 0; k < hooks_.size(); ++k) {
        if (hooks_[k].id == id) { hooks_.erase(hooks_.begin() + static_cast<long>(k)); return; }
    }
}

Clock::Civil Clock::LocalTime() const {
    const Impl& i = *impl_;
    // The epoch advanced by whole emulated seconds (60 fields each: the field rate the console's clock keeps).
    const uint64_t seconds = i.rtc_epoch + i.domain.DomainTick() / 60u;
    const DerivedClocks::RtcTime t = DerivedClocks::EpochToCivil(seconds);
    return Civil{t.year, t.month, t.day, t.hour, t.minute, t.second};
}

Clock::Census Clock::GetCensus() const {
    const Impl& i = *impl_;
    return Census{boundaries_, i.fields, i.protocol.PaceParks(), i.protocol.PaceLate(), i.protocol.PaceTimeouts(),
                  i.protocol.PaceSleptUs(), i.play, i.field_hz, i.ticks_measured, i.ticks_over_field, i.tick_max_ms,
                  i.tick_sum_ms, i.protocol.PaceCatchUp(), i.protocol.PaceDroppedFields(), i.tick_cpu_sum_ms};
}

}  // namespace te

// ---------------------------------------------------------------------------------------------------------------------
// The seam bodies (time.h)
// ---------------------------------------------------------------------------------------------------------------------
extern "C" {

void ps2e_time_initialise(void) { te::Services::Get().clock().Start(); }

ps2e_u64 ps2e_time_ticks(void) { return te::Services::Get().clock().Ticks(); }

}  // extern "C"
