// test_time.cpp -- time.h over te::Clock: ticks are a pure function of boundaries (9600 per field at 60 Hz =
// TicksPerSecond 576000), Initialise starts them from 0 once, hooks run in order, the Play pacer delivers fields at
// the wall rate, LocalTime is the emulated RTC.
#include <chrono>
#include <thread>

#include "ps2e_seam.h"
#include "te_test.h"

int main() {
    using namespace te_test;
    // --- deterministic: one field per boundary, nothing else moves the clock
    Reset(DefaultSettings());
    {
        te::Clock& clock = te::Services::Get().clock();
        CHECK_EQ(clock.T0Counts(), 0u);
        ps2e_time_initialise();
        CHECK_EQ(ps2e_time_ticks(), 0u);
        clock.Boundary();
        CHECK_EQ(ps2e_time_ticks(), 9600u);
        for (int i = 0; i < 59; ++i) clock.Boundary();
        CHECK_EQ(ps2e_time_ticks(), 576000u);   // 60 fields = one second = Platform::Time::TicksPerSecond
        CHECK_EQ(clock.BoundaryCount(), 60u);
        // Initialise again does not restart the count
        ps2e_time_initialise();
        CHECK_EQ(ps2e_time_ticks(), 576000u);
        // No wall time passes into the clock
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK_EQ(ps2e_time_ticks(), 576000u);
    }
    // --- Initialise after boundaries counts from 0 at that point (the game's clock starts after the pad setup frames)
    Reset(DefaultSettings());
    {
        te::Clock& clock = te::Services::Get().clock();
        for (int i = 0; i < 5; ++i) clock.Boundary();
        CHECK_EQ(clock.T0Counts(), 5u * 9600u);
        ps2e_time_initialise();
        CHECK_EQ(ps2e_time_ticks(), 0u);
        clock.Boundary();
        CHECK_EQ(ps2e_time_ticks(), 9600u);
    }
    // --- hooks: registration order, the boundary number, removal
    Reset(DefaultSettings());
    {
        te::Clock& clock = te::Services::Get().clock();
        std::vector<int> order;
        uint64_t seen = 0;
        const int a = clock.AddBoundaryHook("a", [&](uint64_t b) { order.push_back(1); seen = b; });
        clock.AddBoundaryHook("b", [&](uint64_t) { order.push_back(2); });
        clock.Boundary();
        CHECK(order.size() == 2 && order[0] == 1 && order[1] == 2);
        CHECK_EQ(seen, 1u);
        clock.RemoveBoundaryHook(a);
        clock.Boundary();
        CHECK(order.size() == 3 && order[2] == 2);
    }
    // --- the RTC: fixed 2005-06-15 12:00:00 deterministic, advanced one second per 60 fields
    Reset(DefaultSettings());
    {
        te::Clock& clock = te::Services::Get().clock();
        te::Clock::Civil t = clock.LocalTime();
        CHECK_EQ(t.year, 2005); CHECK_EQ(t.month, 6); CHECK_EQ(t.day, 15); CHECK_EQ(t.hour, 12); CHECK_EQ(t.minute, 0); CHECK_EQ(t.second, 0);
        for (int i = 0; i < 61; ++i) clock.Boundary();
        t = clock.LocalTime();
        CHECK_EQ(t.second, 1);
        // the seam body: DateTime{u16 year; u8 month, day, hour, minute, second}
        ps2e_system_local_time(0x1000);
        CHECK_EQ(ReadValue<uint16_t>(0x1000), 2005);
        CHECK_EQ(te::GuestPointer(0x1000, 8)[1 + 1], 6);
        CHECK_EQ(te::GuestPointer(0x1000, 8)[6], 1);
    }
    // --- Play: the pacer paces boundaries at the field rate (wall-locked); 12 boundaries at 120 Hz ~ 100 ms, and
    //     every boundary delivers at least one field; a slow frame (sleep 2 periods) delivers its elapsed fields.
    {
        te::Settings s = DefaultSettings();
        s.deterministic = false;
        s.field_hz = 120;
        Reset(s);
        te::Clock& clock = te::Services::Get().clock();
        ps2e_time_initialise();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 12; ++i) clock.Boundary();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 70 && ms <= 400);   // ~100 ms of 120 Hz edges (generous for a loaded host)
        const uint64_t before = clock.DomainTick();
        CHECK(before >= 12);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));   // ~3.6 periods of frame work
        clock.Boundary();
        CHECK(clock.DomainTick() - before >= 3);   // the elapsed fields reach the title's T0 (clock contract rule 3)
        te::Clock::Census c = clock.GetCensus();
        CHECK(c.play);
        CHECK_EQ(c.field_hz, 120u);
        CHECK(c.fields >= c.boundaries);
        CHECK(c.late >= 1);
    }
    te::Services::ResetForTest();
    return Finish("test_time");
}
