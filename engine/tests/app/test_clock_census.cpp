// tests/app/test_clock_census.cpp -- the clock contract's census (rule 5):
// boundaries, fields and presents fall into 1 s WALL windows opened at the first
// boundary; the series lists completed windows only; an idle second yields a zero
// window; a late tick's extra fields count in field_hz while game_hz counts the
// boundary once; presents on another thread land in the window of their time.
#include <chrono>
#include <cstdio>
#include <string>

#include "app/host/clock_census.h"

namespace {
int g_failures = 0;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) std::printf("  PASS %s\n", what);                         \
        else { std::printf("  FAIL %s\n", what); ++g_failures; }            \
    } while (0)
}  // namespace

int main() {
    using ps2r::app::ClockCensus;
    using namespace std::chrono;
    ClockCensus c;
    CHECK(c.GameHzSeries().empty() && c.LatestGameHz() == 0 && c.LatestRenderFps() == 0, "no window before the first boundary");
    const auto t0 = ClockCensus::Clock::time_point(seconds(1000));
    c.NotePresentAt(t0 - milliseconds(5));   // before the stream: belongs to no window
    // Second 1: 60 boundaries, one field each, 45 presents.
    uint64_t tick = 100;
    for (int i = 0; i < 60; ++i) c.NoteBoundaryAt(++tick, t0 + microseconds(16667) * i);
    for (int i = 0; i < 45; ++i) c.NotePresentAt(t0 + milliseconds(22) * i);
    CHECK(c.GameHzSeries().empty(), "the partial first window is not in the series yet");
    // Second 2: 58 boundaries, two of them late by a field (field_hz 60), 25 presents.
    const auto t1 = t0 + seconds(1);
    for (int i = 0; i < 58; ++i) {
        tick += (i == 10 || i == 40) ? 2 : 1;
        c.NoteBoundaryAt(tick, t1 + microseconds(17000) * i);
    }
    for (int i = 0; i < 25; ++i) c.NotePresentAt(t1 + milliseconds(40) * i);
    // Second 3 idle; a boundary at second 4 closes windows 2 and 3.
    c.NoteBoundaryAt(++tick, t0 + seconds(3) + milliseconds(1));
    CHECK(c.GameHzSeries() == "60,58,0", ("game_hz series 60,58,0: " + c.GameHzSeries()).c_str());
    CHECK(c.FieldHzSeries() == "59,60,0", ("field_hz series 59,60,0 (fields between boundaries; the late ticks' extra fields counted): " + c.FieldHzSeries()).c_str());
    CHECK(c.RenderFpsSeries() == "45,25,0", ("render_fps series 45,25,0: " + c.RenderFpsSeries()).c_str());
    CHECK(c.LatestGameHz() == 0 && c.LatestRenderFps() == 0, "latest = the last completed window (the idle one)");
    CHECK(c.Windows().size() == 3 && c.Windows()[1].boundaries == 58, "Windows() exposes the completed windows");
    // Edge attribution (review loop 1): with an edge lookup a boundary belongs to
    // the window of its first field's wall edge. Edges exactly 1/60 s apart from
    // e0; the game answers edge k at e_k + latency, where the latency of the first
    // boundary is 9 ms and every later one 0.5 ms -- by completion time the 60th
    // edge's boundary (at e0 + 1 s + 0.5 ms < window end e0 + 1 s + 9 ms) would
    // fall into window 1 (61) and window 2 would read 59; by edge it reads 60/60.
    // One late frame (edges 70 and 71 answered by one boundary) reads 59 with
    // field_hz 60 in its window; an evicted edge (lookup false) falls back to now.
    {
        ClockCensus e;
        const auto e0 = ClockCensus::Clock::time_point(seconds(2000));
        // The pacer's train: edge t at e0 + (t-1) * 1e9 / 60 ns (rational per edge).
        auto edge_of = [&](uint64_t t) { return e0 + nanoseconds((static_cast<int64_t>(t - 1) * 1000000000LL) / 60); };
        e.SetEdgeLookup([&](uint64_t t, ClockCensus::Clock::time_point* edge) {
            if (t > 500) return false;
            *edge = edge_of(t);
            return true;
        });
        uint64_t k = 0;
        auto answer = [&](uint64_t fields, microseconds latency) {
            k += fields;
            e.NoteBoundaryAt(k, edge_of(k) + latency);
        };
        answer(1, milliseconds(9));                            // edge 1, slow first answer
        for (int i = 1; i < 60; ++i) answer(1, microseconds(500));   // edges 2..60
        for (int i = 0; i < 9; ++i) answer(1, microseconds(500));    // edges 61..69
        answer(2, microseconds(500));                          // edges 70+71 in one boundary (late frame)
        for (int i = 0; i < 49; ++i) answer(1, microseconds(500));   // edges 72..120
        answer(1, microseconds(500));                          // edge 121 closes window 2
        CHECK(e.GameHzSeries() == "60,59", ("edge attribution: game_hz 60,59 (no 61/59 straddle; the late frame's window reads 59): " + e.GameHzSeries()).c_str());
        CHECK(e.FieldHzSeries() == "59,60", ("edge attribution: field_hz 59,60 (the first boundary opens the count): " + e.FieldHzSeries()).c_str());
        // Completion-time attribution of the same stream (no lookup) prints the straddle.
        ClockCensus n;
        k = 0;
        auto answer_n = [&](uint64_t fields, microseconds latency) {
            k += fields;
            n.NoteBoundaryAt(k, edge_of(k) + latency);
        };
        answer_n(1, milliseconds(9));
        for (int i = 1; i < 60; ++i) answer_n(1, microseconds(500));
        for (int i = 0; i < 9; ++i) answer_n(1, microseconds(500));
        answer_n(2, microseconds(500));
        for (int i = 0; i < 49; ++i) answer_n(1, microseconds(500));
        answer_n(1, microseconds(500));
        answer_n(1, microseconds(500));   // edge 122 closes the completion-time window 2
        CHECK(n.GameHzSeries() == "61,59", ("completion-time attribution of the same stream straddles (61 then 59): " + n.GameHzSeries()).c_str());
    }
    {
        // ee_cpu_ms: the game thread's CPU clock delta between boundaries credited to
        // the boundary's window in whole ms, the remainder carried; the total is exact.
        ClockCensus c;
        const auto t0 = steady_clock::now();
        uint64_t tick = 0, cpu = 5'000'000'000ull;   // 5 s of prior CPU: only deltas count
        c.NoteBoundaryAt(++tick, t0, cpu);
        for (int i = 1; i < 60; ++i) { cpu += 10'500'000ull; c.NoteBoundaryAt(++tick, t0 + microseconds(16667) * i, cpu); }   // 59 x 10.5 ms
        for (int i = 0; i < 60; ++i) { cpu += 4'000'000ull; c.NoteBoundaryAt(++tick, t0 + seconds(1) + microseconds(16667) * i, cpu); }   // 60 x 4 ms
        c.NoteBoundaryAt(++tick, t0 + seconds(2) + milliseconds(1), cpu);   // closes window 2
        CHECK(c.EeCpuMsSeries() == "619,240", ("ee_cpu_ms per window (59 x 10.5 = 619.5 -> 619 + carry; 60 x 4 + 0.5 carry = 240): " + c.EeCpuMsSeries()).c_str());
        CHECK(c.EeCpuTotalMs() == 859, "the total is the exact sum of the deltas (859.5 ms -> 859)");
        CHECK(c.GameHzSeries() == "60,60", "the boundaries themselves are unchanged by the CPU reading");
        // The default form reads the calling thread's CPU clock: monotone, never negative.
        ClockCensus d;
        d.NoteBoundary(1);
        volatile uint64_t sink = 0;
        for (uint64_t i = 0; i < 20'000'000ull; ++i) sink += i;   // some CPU
        d.NoteBoundary(2);
        CHECK(d.EeCpuTotalMs() < 60000, "the calling thread's CPU delta is a sane figure");
    }
    {
        // Fast-forward (review C2 loop 1): an engaged cutscene fast-forward adds
        // Deferred fields the pacer never ticked, so the DOMAIN tick runs ahead of
        // the pacer's. With the running fast-forward total passed in, the census
        // keys the edge lookup by the pacer tick: attribution stays by wall edge
        // through and after the stretch (60/60/60, no straddle), field_hz carries
        // the extra fields (60 + 3 x 20 in the engaged window). Without the
        // subtraction the lookup misses (tick > noted) from the first engaged
        // boundary on and the census falls to completion time for the rest of
        // the run -- the straddle the header warns about.
        const auto e0 = ClockCensus::Clock::time_point(seconds(4000));
        auto edge_of = [&](uint64_t t) { return e0 + nanoseconds((static_cast<int64_t>(t - 1) * 1000000000LL) / 60); };
        auto lookup = [&](uint64_t t, ClockCensus::Clock::time_point* edge) {
            if (t > 500) return false;
            *edge = edge_of(t);
            return true;
        };
        // The stream: 60 plain boundaries; 60 boundaries of which the first 20 are
        // fast-forwarded by 3 fields each (factor 4); 60 plain; one closing boundary.
        // Each answers its pacer edge 0.5 ms late, except pacer edge 120 (the last
        // of window 2) which completes 20 ms late -- past the window's end.
        auto drive = [&](ClockCensus& c, bool subtract) {
            uint64_t pacer = 0, ff = 0;
            auto answer = [&](uint64_t extra) {
                pacer += 1;
                ff += extra;
                const auto latency = pacer == 120 ? milliseconds(20) : milliseconds(0) + microseconds(500);
                c.NoteBoundaryAt(pacer + ff, edge_of(pacer) + latency, 0, subtract ? ff : 0);
            };
            for (int i = 0; i < 60; ++i) answer(0);
            for (int i = 0; i < 20; ++i) answer(3);
            for (int i = 0; i < 40; ++i) answer(0);
            for (int i = 0; i < 60; ++i) answer(0);
            answer(0);   // pacer edge 181 closes window 3
        };
        ClockCensus with;
        with.SetEdgeLookup(lookup);
        drive(with, true);
        CHECK(with.GameHzSeries() == "60,60,60", ("fast-forward subtracted: game_hz 60,60,60 by wall edge through the engaged stretch: " + with.GameHzSeries()).c_str());
        CHECK(with.FieldHzSeries() == "59,120,60", ("fast-forward subtracted: field_hz 59,120,60 (the 60 extra fields land in their window): " + with.FieldHzSeries()).c_str());
        ClockCensus without;
        without.SetEdgeLookup(lookup);
        drive(without, false);
        CHECK(without.GameHzSeries() == "60,59,61", ("fast-forward NOT subtracted: the lookup misses from the engaged stretch on and the late edge-120 answer straddles (60,59,61): " + without.GameHzSeries()).c_str());
    }
    {
        // Dropped fields (the clock contract's rule 3, single-field form): a stall
        // of 21 edges in window 2 delivers one field, keeps 8 as the backlog and
        // DROPS 12 -- the domain tick then lags the pacer's by 12. With the running
        // dropped total passed in, the boundary after the stall is attributed to its
        // own edge, the 12 dropped edges follow unanswered (window 2 reads 48 = 60 -
        // 12, pace_dropped_hz 12 there) and the 8 catch-up boundaries map to the
        // stall's last edges; every later boundary stays on its true edge (60,60).
        // Without the total the lookup runs 12 edges early from the drop on: every
        // later window borrows its successor's first 12 boundaries (60,60,60) -- the
        // stall is pushed into the unclosed tail and HIDDEN, the attribution shifted.
        const auto e0 = ClockCensus::Clock::time_point(seconds(5000));
        auto edge_of = [&](uint64_t t) { return e0 + nanoseconds((static_cast<int64_t>(t - 1) * 1000000000LL) / 60); };
        auto lookup = [&](uint64_t t, ClockCensus::Clock::time_point* edge) {
            if (t > 500) return false;
            *edge = edge_of(t);
            return true;
        };
        auto drive = [&](ClockCensus& c, bool add_dropped) {
            uint64_t pacer = 0, domain = 0, dropped = 0;
            auto answer = [&](uint64_t drop, ClockCensus::Clock::time_point at) {
                pacer += 1 + drop;
                domain += 1;
                dropped += drop;
                c.NoteBoundaryAt(domain, at, 0, 0, add_dropped ? dropped : 0);
            };
            for (int i = 0; i < 60; ++i) answer(0, edge_of(pacer + 1) + microseconds(500));          // window 1: 60 on time
            for (int i = 0; i < 20; ++i) answer(0, edge_of(pacer + 1) + microseconds(500));          // window 2: 20 on time, then
            const uint64_t stall_from = pacer + 1;                                                   // a 21-edge stall (350 ms)
            answer(12, edge_of(stall_from + 21));                                                     // 1 field, 12 dropped (edges 2..13)
            for (int i = 0; i < 8; ++i) answer(0, edge_of(stall_from + 21) + microseconds(500) * (i + 1));   // 8 catch-ups, at once
            for (int i = 0; i < 19; ++i) answer(0, edge_of(pacer + 1) + microseconds(500));          // the rest of window 2 on time
            for (int i = 0; i < 60; ++i) answer(0, edge_of(pacer + 1) + microseconds(500));          // window 3: 60 on time
            for (int i = 0; i < 13; ++i) answer(0, edge_of(pacer + 1) + microseconds(500));          // closes window 3 (on either keying)
        };
        ClockCensus with;
        with.SetEdgeLookup(lookup);
        drive(with, true);
        CHECK(with.GameHzSeries() == "60,48,60", ("dropped added back: game_hz 60,48,60 -- the stall's window reads 60 - 12 dropped, the later windows stay on their edges: " + with.GameHzSeries()).c_str());
        CHECK(with.FieldHzSeries() == "59,48,60", ("dropped added back: field_hz 59,48,60 (one field per boundary): " + with.FieldHzSeries()).c_str());
        CHECK(with.DroppedSeries() == "0,12,0", ("pace_dropped_hz 0,12,0 -- the dropped edges in the window of the stall: " + with.DroppedSeries()).c_str());
        ClockCensus without;
        without.SetEdgeLookup(lookup);
        drive(without, false);
        CHECK(without.GameHzSeries() == "60,60,60", ("dropped NOT added back: the lookup runs 12 edges early from the drop on -- the stall is hidden (60,60,60): " + without.GameHzSeries()).c_str());
        CHECK(without.DroppedSeries() == "0,0,0", "without the total nothing is counted dropped");
    }
    {
        // A boundary the lookup cannot place (its tick not yet published: the pacer
        // thread preempted between the wake and its count store) is attributed by its
        // completion time -- late, in the NEXT second -- and rolls the window; the
        // catch-up boundaries after it answer edges of the window just closed and are
        // credited THERE (60,60), not clamped into the current one (the 56/64 smear).
        const auto e0 = ClockCensus::Clock::time_point(seconds(6000));
        auto edge_of = [&](uint64_t t) { return e0 + nanoseconds((static_cast<int64_t>(t - 1) * 1000000000LL) / 60); };
        uint64_t unpublished = 0;   // the one tick the lookup does not know yet
        auto lookup = [&](uint64_t t, ClockCensus::Clock::time_point* edge) {
            if (t > 500 || t == unpublished) return false;
            *edge = edge_of(t);
            return true;
        };
        ClockCensus c;
        c.SetEdgeLookup(lookup);
        uint64_t tick = 0;
        for (int i = 0; i < 56; ++i) { ++tick; c.NoteBoundaryAt(tick, edge_of(tick) + microseconds(500)); }   // window 1: 56 on time
        // A 5-edge late stretch at the window's end: edges 57..61 answered after edge 61's time
        // (in window 2); edge 57's boundary is the unplaced one (completion time).
        const auto late_at = edge_of(62) + microseconds(500);
        ++tick; unpublished = tick; c.NoteBoundaryAt(tick, late_at);                      // 57: completion time -> window 2 rolls
        unpublished = 0;
        for (int i = 0; i < 4; ++i) { ++tick; c.NoteBoundaryAt(tick, late_at + microseconds(500) * (i + 1)); }   // 58..61: catch-ups, edges in window 1
        for (int i = 0; i < 60; ++i) { ++tick; c.NoteBoundaryAt(tick, edge_of(tick) + microseconds(500)); }   // 62..121: on time
        ++tick; c.NoteBoundaryAt(tick, edge_of(tick) + microseconds(500));                 // 122 closes window 2
        CHECK(c.GameHzSeries() == "59,61", ("closed-window credit: the unplaced boundary lands in window 2 by its completion time (59,61), the four catch-ups in window 1 by their edges -- not 56,64: " + c.GameHzSeries()).c_str());
        CHECK(c.FieldHzSeries() == "58,61", ("closed-window credit: fields follow the boundaries (58,61): " + c.FieldHzSeries()).c_str());
    }
    std::printf("ClockCensus: %s\n", g_failures ? "FAIL" : "ALL PASS");
    return g_failures ? 1 : 0;
}
