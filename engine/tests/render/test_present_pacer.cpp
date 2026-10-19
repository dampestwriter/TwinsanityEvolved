// tests/render/test_present_pacer.cpp -- PresentPacer (engine/platform/ps2e/present_pacer.h): the names and rates, the
// 30 Hz picture rule, the slot trains (120 / 240 re-anchored on the tick edges with no double slot, 144 free-running,
// uncapped = now), the late census, and the timer sleeping to a deadline. No window, no GL.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/platform/ps2e/present_pacer.h"

using ps2r::ps2e::PresentPacer;
using Rate = PresentPacer::Rate;

static int g_fail = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) { std::printf("FAIL: %s\n", name); g_fail = 1; }       \
        else std::printf("ok: %s\n", name);                                \
    } while (0)

int main() {
    // names and rates
    Rate r = Rate::Rate60;
    CHECK(PresentPacer::Parse("120", &r) && r == Rate::Rate120 && PresentPacer::Hz(r) == 120, "parse 120");
    CHECK(PresentPacer::Parse("UNCAPPED", &r) && r == Rate::Uncapped && PresentPacer::Hz(r) == 0, "parse uncapped (any case)");
    CHECK(PresentPacer::Parse("30", &r) && r == Rate::Rate30 && !PresentPacer::PacedBySlots(r), "30 is the game's own cadence");
    CHECK(!PresentPacer::Parse("90", &r) && !PresentPacer::Parse("", &r) && !PresentPacer::Parse(nullptr, &r), "unknown rates refused");
    CHECK(std::strcmp(PresentPacer::Name(Rate::Rate144), "144") == 0 && std::strcmp(PresentPacer::Name(Rate::Uncapped), "uncapped") == 0, "names");
    CHECK(PresentPacer::SlotsPerTick(Rate::Rate120) == 2 && PresentPacer::SlotsPerTick(Rate::Rate240) == 4 &&
              PresentPacer::SlotsPerTick(Rate::Rate144) == 0 && PresentPacer::SlotsPerTick(Rate::Uncapped) == 0,
          "slots per tick: 2 at 120, 4 at 240, none at 144 / uncapped");

    // 30: every other picture, the even ones
    {
        PresentPacer p;
        p.SetRate(Rate::Rate30);
        int shown = 0;
        for (uint64_t i = 1; i <= 100; ++i) shown += p.ShowsPicture(i) ? 1 : 0;
        CHECK(shown == 50 && p.ShowsPicture(2) && !p.ShowsPicture(1), "30 shows 50 of 100 pictures, the even ones");
        p.SetRate(Rate::Rate60);
        CHECK(p.ShowsPicture(1) && p.ShowsPicture(2), "60 shows every picture");
    }

    // 120: two slots per tick on the tick edges; a re-anchor never doubles a slot
    {
        PresentPacer p;
        p.SetRate(Rate::Rate120);
        CHECK(p.SlotPeriodNs() == 8333333, "120: the slot period");
        const int64_t e0 = 1000000000;   // tick edge n
        p.OnTickEdge(e0);
        const int64_t s0 = p.NextSlotNs(e0 + 5000000);   // the picture arrived 5 ms after its edge
        CHECK(s0 == e0, "the first slot is the edge itself (served late by 5 ms, within a period)");
        p.SlotServed(s0, e0 + 5000000);
        const int64_t s1 = p.NextSlotNs(e0 + 5100000);
        CHECK(s1 == e0 + 8333333, "the second slot is halfway to the next tick");
        p.SlotServed(s1, s1 + 200000);
        const int64_t e1 = e0 + 16666666;   // the next tick's edge
        const int64_t s2 = p.NextSlotNs(e1 - 100000);
        CHECK(s2 == e0 + 2 * 8333333, "the old train's third slot sits on the next edge (within rounding)");
        p.SlotServed(s2, e1 + 100000);
        p.OnTickEdge(e1);   // the next picture arrives: the train re-anchored on its edge
        const int64_t s3 = p.NextSlotNs(e1 + 5000000);
        CHECK(s3 == e1 + 8333333, "after the re-anchor the next slot is the half-way slot, not the edge again (no double present)");
        p.SlotServed(s3, s3);
        CHECK(p.Count().slots == 4 && p.Count().late == 0 && p.Count().reanchors == 2, "census: 4 slots, none late, 2 anchors");
    }

    // a stall: more than one slot behind re-anchors at now (no burst)
    {
        PresentPacer p;
        p.SetRate(Rate::Rate240);
        p.OnTickEdge(0);
        const int64_t s = p.NextSlotNs(0);
        p.SlotServed(s, 0);
        const int64_t late = p.NextSlotNs(50000000);   // 50 ms later
        CHECK(late == 50000000 && p.Count().reanchors == 2, "a 50 ms stall re-anchors the train at now");
        p.SlotServed(late, 50000000 + 6000000);
        CHECK(p.Count().late == 1 && p.Count().late_ms_max >= 5.9, "a slot served 6 ms after its time at 240 Hz counts as late");
    }

    // 144: free-running after the first anchor; 12 slots per 5 ticks
    {
        PresentPacer p;
        p.SetRate(Rate::Rate144);
        p.OnTickEdge(0);
        int64_t now = 0;
        int slots = 0;
        for (int tick = 0; tick < 5; ++tick) {
            p.OnTickEdge(tick * 16666666LL);   // ignored once anchored
            for (;;) {
                const int64_t s = p.NextSlotNs(now);
                if (s >= (tick + 1) * 16666666LL - 1000) break;   // the train's period rounds down: 2 ns a slot of drift
                p.SlotServed(s, s);
                now = s + 1;
                slots++;
            }
        }
        CHECK(slots == 12 && p.Count().reanchors == 1, "144: twelve slots over five ticks on one free-running train");
    }

    // uncapped: the next slot is now
    {
        PresentPacer p;
        p.SetRate(Rate::Uncapped);
        CHECK(p.NextSlotNs(123) == 123 && p.SlotPeriodNs() == 0, "uncapped: now, no period");
        p.OnTickEdge(999);
        CHECK(p.NextSlotNs(456) == 456 && p.Count().late == 0, "uncapped: edges change nothing");
    }

    // the timer: sleeping to a deadline 3 ms away returns at or after it, and within a millisecond of it
    {
        const int64_t t0 = PresentPacer::NowNs();
        PresentPacer::SleepUntilNs(t0 + 3000000);
        const int64_t t1 = PresentPacer::NowNs();
        CHECK(t1 >= t0 + 3000000, "SleepUntilNs never returns early");
        CHECK(t1 - t0 < 3000000 + 2000000, "SleepUntilNs returns within 2 ms of the deadline (a loaded host allowed)");
        const int64_t t2 = PresentPacer::NowNs();
        PresentPacer::SleepUntilNs(t2 - 1);
        CHECK(PresentPacer::NowNs() - t2 < 1000000, "a past deadline returns at once");
    }
    return g_fail;
}
