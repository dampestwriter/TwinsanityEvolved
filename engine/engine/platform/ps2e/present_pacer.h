// engine/platform/ps2e/present_pacer.h -- PresentPacer: the cadence a producer that paces its own presents keeps (the
// typed-draw surface's consumer: a decompiled title's native arm whose game runs at 60 Hz on the engine's FieldPacer and
// whose render thread decides WHEN to show).
//
// The rates are video.frame_rate's (engine/core/config: 30 | 60 | 120 | 144 | 240 | uncapped). The game's tick is never
// touched; this class answers two questions for the presenter:
//
//   * which PICTURES to show when the cadence is the game's own -- 30 shows every other picture (steady: the even
//     ones), 60 every picture (ShowsPicture);
//   * when the next presentation SLOT is when the cadence is faster than the game -- 120 / 144 / 240 as a slot train
//     slot_k = anchor + k * 1e9 / rate (ns, the steady clock FieldPacer stands on), re-anchored on every game-tick edge
//     when the rate is a whole multiple of 60 (so the slots of 120 and 240 fall exactly on the ticks and halfway /
//     quarter-way between them), free-running at 144 (12 slots per 5 ticks: its slots cannot all meet the ticks); and
//     uncapped = the next slot is now (no wait, the producer runs the renderer flat out).
//
// Waiting is the host arm's finest timer (ps2r::host::SleepForNs: the Windows 1 ms period + high-resolution waitable
// timer, nanosleep on POSIX -- never a scheduler-quantum Sleep), coarse to one millisecond before the slot and then in
// fine steps to it. The census counts slots served and slots served LATE (more than one slot period after their time).
#pragma once

#include <cstdint>

namespace ps2r {
namespace ps2e {

class PresentPacer {
public:
    enum class Rate : uint8_t { Rate30 = 0, Rate60, Rate120, Rate144, Rate240, Uncapped };
    struct Census {
        uint64_t slots = 0;          // slots the producer served (NextSlot .. SlotServed)
        uint64_t late = 0;           // slots served more than one slot period after their time
        double late_ms_max = 0;      // the worst lateness
        uint64_t reanchors = 0;      // slot trains re-anchored (a tick edge, or the train fell behind)
    };

    PresentPacer() = default;

    // The rate's ini / command-line spelling ("30" .. "240", "uncapped"); Parse accepts them case-insensitively.
    static const char* Name(Rate rate);
    static bool Parse(const char* text, Rate* out);
    // Presents per second; 0 = uncapped.
    static uint32_t Hz(Rate rate);
    // The cadence is a slot train (120 / 144 / 240 / uncapped) rather than the game's own pictures (30 / 60).
    static bool PacedBySlots(Rate rate) { return rate != Rate::Rate30 && rate != Rate::Rate60; }
    // Slots per game tick when the rate is a whole multiple of the game's 60 Hz (2 at 120, 4 at 240), else 0.
    static uint32_t SlotsPerTick(Rate rate, uint32_t game_hz = 60);

    void SetRate(Rate rate);
    Rate GetRate() const { return rate_; }

    // The game's own cadence (30 / 60): whether picture number `picture` (1-based, counted as they arrive) is shown.
    bool ShowsPicture(uint64_t picture) const;

    // The slot train. OnTickEdge: a game tick's edge (the FieldPacer's EdgeOfTickNs) -- at a whole-multiple rate the
    // train is re-anchored on it so the next slots are edge + j * period / m; at 144 the train is anchored only the
    // first time (or after it fell behind). NextSlotNs: the first slot at or after `now_ns`, consuming the slots before
    // it (a train that fell more than one slot behind re-anchors at now, counted). Uncapped: now.
    void OnTickEdge(int64_t edge_ns);
    int64_t NextSlotNs(int64_t now_ns);
    // The producer served the slot NextSlotNs returned, at `now_ns` (its present started): the census.
    void SlotServed(int64_t slot_ns, int64_t now_ns);
    // The slot period in ns (0 when uncapped).
    int64_t SlotPeriodNs() const { return period_ns_; }

    // Sleep until the steady-clock instant `deadline_ns`: coarse (SleepForNs of the remaining time less one
    // millisecond) then fine (SleepForNs of the remainder in steps) -- returns at or just after the deadline.
    static void SleepUntilNs(int64_t deadline_ns);
    // The steady clock in ns (the one the FieldPacer's edges are on).
    static int64_t NowNs();

    Census Count() const { return census_; }

private:
    Rate rate_ = Rate::Rate60;
    int64_t period_ns_ = 16666666;
    bool anchored_ = false;
    int64_t anchor_ns_ = 0;
    uint64_t k_ = 0;   // the next slot's index on the current train
    int64_t last_slot_ns_ = -(1LL << 62);   // the last slot served (a re-anchored train starts after it)
    Census census_;
};

}  // namespace ps2e
}  // namespace ps2r
