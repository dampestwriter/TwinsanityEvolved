// FieldPacer: the library-tier field ticker (engine/ee/timing/field_pacer.*).
//   edges   -- EdgeNs is the exact rational train: edge hz is exactly one second after edge 0 at 60 and 50 Hz, edges
//              are monotonic, the 60 Hz train matches TickThread's (1e9/60 per k, no accumulated rounding).
//   pacer   -- a started pacer notes ticks at the wall rate (TickSync::Pending grows, NotifyTick wakes a parked
//              boundary: ParkForPacerTick returns having parked), and a late boundary finds its ticks noted; Stop joins.
//   fields  -- the clock contract's rule 3 in the single-field form against the LIVE pacer (BoundaryProtocol::
//              DeliverField, the game-thread boundary a library consumer runs): every boundary delivers exactly ONE
//              field; a frame longer than several periods leaves a backlog the following boundaries deliver without
//              parking (catch-up), one each, until the game has run as many frames as the wall produced fields; a
//              stall beyond kCatchUpMaxFields is dropped and counted, never replayed as a burst.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "engine/ee/timing/boundary_protocol.h"
#include "engine/ee/timing/field_pacer.h"
#include "engine/ee/timing/tick_source.h"
#include "engine/ee/timing/tick_sync.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, name) \
    do { if (cond) { ++g_pass; } else { std::printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } } while (0)

static int Edges() {
    CHECK(FieldPacer::EdgeNs(0, 60, 60) == 1000000000LL, "60 Hz: edge 60 is one second");
    CHECK(FieldPacer::EdgeNs(0, 50, 50) == 1000000000LL, "50 Hz: edge 50 is one second");
    CHECK(FieldPacer::EdgeNs(0, 120, 120) == 1000000000LL, "120 Hz: edge 120 is one second");
    CHECK(FieldPacer::EdgeNs(1000, 1, 60) == 1000 + 16666666LL, "60 Hz: edge 1 is 16,666,666 ns after the anchor");
    CHECK(FieldPacer::EdgeNs(0, 3600, 60) == 60000000000LL, "60 Hz: edge 3600 is exactly one minute");
    int64_t last = -1;
    bool monotonic = true;
    for (uint64_t k = 0; k < 1000; ++k) {
        const int64_t e = FieldPacer::EdgeNs(0, k, 60);
        if (e <= last) monotonic = false;
        last = e;
    }
    CHECK(monotonic, "edges strictly increase");
    CHECK(FieldPacer::EdgeNs(0, 60, 0) == 1000000000LL, "hz 0 reads as 60");
    return g_fail ? 1 : 0;
}

static int Pacer() {
    TickSource source;
    source.SetPolicy(TickSource::Policy::Play);
    TickSync ticks;
    uint64_t fired = 0;
    ticks.SetBody([&](TickSync::Cause) -> uint64_t { return ++fired; });
    BoundaryProtocol protocol(source, ticks);
    FieldPacer pacer(ticks, protocol, 120);
    CHECK(!pacer.Running(), "not running before Start");
    CHECK(pacer.Hz() == 120, "hz kept");
    pacer.Start();
    CHECK(pacer.Running(), "running after Start");
    // An early boundary parks for the next edge and finds exactly one tick noted (within ~ a period)
    const auto t0 = std::chrono::steady_clock::now();
    const bool parked = protocol.ParkForPacerTick();
    const auto park_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(parked, "an early boundary parks");
    CHECK(park_us <= 2 * 8334 + 5000, "the park ends by the next edge (bounded by two periods)");
    CHECK(ticks.FirePending() >= 1, "the parked boundary fires the noted tick(s)");
    // A late boundary (work longer than a period) finds its ticks noted and does not park
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!protocol.ParkForPacerTick(), "a late boundary does not park");
    const int late_fired = ticks.FirePending();
    CHECK(late_fired >= 2, "the elapsed fields were noted (>= 2 in 30 ms at 120 Hz)");
    // The rate: ~120 ticks per second
    const uint64_t before = pacer.Ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t noted = pacer.Ticks() - before;
    CHECK(noted >= 20 && noted <= 40, "about 30 ticks in 250 ms at 120 Hz");
    // The edge clock a presenter reads: the latest tick's edge is in the past and within a period of now; the edges
    // of two consecutive ticks are one period apart; a tick before the first is refused only before any tick
    {
        const uint64_t n = pacer.Ticks();
        int64_t e_n = 0, e_prev = 0;
        CHECK(n >= 2 && pacer.EdgeOfTickNs(n, &e_n) && pacer.EdgeOfTickNs(n - 1, &e_prev), "edges of the two latest ticks are known");
        CHECK(e_n - e_prev == pacer.PeriodNs(), "consecutive ticks' edges are one period apart");
        CHECK(pacer.PeriodNs() == 8333333LL, "120 Hz: the period is 8,333,333 ns");
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        CHECK(e_n <= now_ns && now_ns - e_n < 4 * pacer.PeriodNs(), "the latest edge is in the recent past");
    }
    pacer.Stop();
    CHECK(!pacer.Running(), "stopped");
    const uint64_t after_stop = pacer.Ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(pacer.Ticks() == after_stop, "no ticks after Stop");
    ticks.FirePending();
    return g_fail ? 1 : 0;
}

static int Fields() {
    TickSource source;
    source.SetPolicy(TickSource::Policy::Play);
    TickSync ticks;
    uint64_t fields = 0;
    ticks.SetBody([&](TickSync::Cause) -> uint64_t { source.Tick(); return ++fields; });
    BoundaryProtocol protocol(source, ticks);
    FieldPacer pacer(ticks, protocol, 120);   // 8.33 ms fields: the test runs in well under a second
    pacer.Start();
    // On-time frames: each boundary parks for its edge and delivers one field.
    for (int i = 0; i < 4; ++i) {
        const BoundaryProtocol::Field f = protocol.DeliverField();
        CHECK(!f.catch_up && f.backlog == 0 && f.dropped == 0, "an on-time boundary: one field, no backlog");
    }
    CHECK(fields == 4 && protocol.PaceCatchUp() == 0, "4 on-time boundaries: 4 fields, no catch-up");
    // A long frame: ~4 periods of work. The boundary delivers ONE field (never a
    // multi-field jump) and leaves the elapsed fields as the backlog.
    std::this_thread::sleep_for(std::chrono::microseconds(8334 * 4 - 2000));
    const uint64_t before = fields;
    BoundaryProtocol::Field late = protocol.DeliverField();
    CHECK(fields - before == 1, "the late boundary delivers exactly one field");
    CHECK(late.late && !late.parked && !late.catch_up, "the late boundary found its tick noted and did not park");
    CHECK(late.backlog >= 2 && late.backlog <= 4, "the fields elapsed during the long frame are the backlog (2..4)");
    CHECK(late.dropped == 0, "a 4-period frame is within the catch-up cap: nothing dropped");
    // Catch-up: the following boundaries deliver the backlog one field each WITHOUT parking.
    uint64_t catch_ups = 0;
    uint64_t multi = 0;
    for (int i = 0; i < 16 && protocol.Backlog() != 0; ++i) {
        const uint64_t b0 = fields;
        const auto t0 = std::chrono::steady_clock::now();
        const BoundaryProtocol::Field f = protocol.DeliverField();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (fields - b0 != 1) ++multi;
        if (f.catch_up) {
            ++catch_ups;
            CHECK(us < 4000, "a catch-up boundary does not park (returns at once)");
        }
    }
    CHECK(multi == 0, "every boundary delivered exactly one field (no multi-field jump)");
    CHECK(catch_ups >= late.backlog && catch_ups == protocol.PaceCatchUp(), "the backlog was delivered by catch-up boundaries, one field each");
    CHECK(protocol.Backlog() == 0, "the backlog drained");
    // Wall speed kept: fields == the pacer's ticks (every wall field reached the game once, none dropped).
    {
        const uint64_t noted = pacer.Ticks();
        const uint64_t pending = ticks.Pending();
        CHECK(fields <= noted + 1 && fields + pending + 1 >= noted,
              "fields delivered == pacer ticks noted (less the ones not yet taken; +-1 for the pacer's note/count order)");
    }
    CHECK(protocol.PaceDroppedFields() == 0, "nothing dropped on a 4-period frame");
    // Bounded: a stall of ~(cap + 6) periods delivers one field, keeps the cap and drops the rest.
    std::this_thread::sleep_for(std::chrono::microseconds(8334 * static_cast<int64_t>(BoundaryProtocol::kCatchUpMaxFields + 6)));
    const uint64_t b1 = fields;
    const BoundaryProtocol::Field stall = protocol.DeliverField();
    CHECK(fields - b1 == 1, "the stalled boundary delivers exactly one field");
    CHECK(stall.backlog == BoundaryProtocol::kCatchUpMaxFields, "the backlog is capped at kCatchUpMaxFields");
    CHECK(stall.dropped >= 2 && protocol.PaceDroppedFields() == stall.dropped, "the excess fields were dropped and counted");
    pacer.Stop();
    while (protocol.Backlog() != 0) protocol.DeliverField();   // drain (the pacer stopped: pure catch-up)
    CHECK(protocol.PaceTimeouts() == 0, "no park timed out");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (!std::strcmp(which, "edges") || !std::strcmp(which, "all")) rc |= Edges();
    if (!std::strcmp(which, "pacer") || !std::strcmp(which, "all")) rc |= Pacer();
    if (!std::strcmp(which, "fields") || !std::strcmp(which, "all")) rc |= Fields();
    std::printf("%s: %d passed, %d failed\n", which, g_pass, g_fail);
    return rc;
}
