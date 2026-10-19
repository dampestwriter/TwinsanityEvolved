#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/scheduler.h"

namespace {
int failures = 0, checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
}

int main() {
    const auto owner = std::this_thread::get_id();
    Scheduler scheduler([&] { return std::this_thread::get_id() == owner; });
    std::vector<int> values;
    const auto a = scheduler.Issue(10, [&](Scheduler::Result r) {
        CHECK(r.error == 0 && r.bytes == std::vector<uint8_t>({1, 2, 3}));
        values.push_back(1);
    });
    const auto b = scheduler.Issue(10, [&](Scheduler::Result r) {
        CHECK(r.error == -7 && r.bytes.empty());
        values.push_back(2);
    });
    // OLD S57 / BUG-001/026/032/033/041: scheduling order, data-before-done,
    // and the thread delivering results cannot depend on host completion speed.
    std::thread worker([&] {
        CHECK(scheduler.Publish(b, {-7, {}}));
        CHECK(!scheduler.Issue(0, [](Scheduler::Result) {}));
        CHECK(scheduler.Drain(100) == 0);
    });
    worker.join();
    CHECK(values.empty());
    CHECK(scheduler.Drain(100) == 0);
    CHECK(scheduler.Publish(a, {0, {1, 2, 3}}));
    CHECK(scheduler.Drain(9) == 0);
    CHECK(!scheduler.CanCheckpoint());
    CHECK(scheduler.Drain(10) == 2);
    CHECK(values == std::vector<int>({1, 2}));
    CHECK(scheduler.Status(a) == Scheduler::State::Settled);
    CHECK(!scheduler.Publish(a, {}));
    CHECK(scheduler.CanCheckpoint());
    CHECK(scheduler.Count().wrong_owner == 2);

    // OLD SOUND_P8/P9: stop/reopen before a worker finishes cannot kill the
    // fresh generation. Cancelling a head must release its ready successor.
    const auto stale = scheduler.Issue(20, [&](Scheduler::Result) { values.push_back(99); });
    const auto next = scheduler.Issue(20, [&](Scheduler::Result) { values.push_back(3); });
    CHECK(scheduler.Publish(next, {}));
    CHECK(scheduler.Cancel(stale));
    CHECK(!scheduler.Publish(stale, {}));
    CHECK(scheduler.Drain(20) == 1 && values.back() == 3);
    const auto old = scheduler.Issue(21, [&](Scheduler::Result) { values.push_back(99); });
    scheduler.Reset();
    const auto fresh = scheduler.Issue(21, [&](Scheduler::Result) { values.push_back(4); });
    CHECK(old.generation != fresh.generation);
    CHECK(!scheduler.Publish(old, {}));
    CHECK(scheduler.Publish(fresh, {}));
    CHECK(scheduler.Drain(21) == 1 && values.back() == 4);

    // OLD callback-slot-zero / BUG-038: no lock across a callback, and a
    // callback may request work whose producer is this same execution owner.
    Scheduler::Ticket child;
    const auto parent = scheduler.Issue(22, [&](Scheduler::Result) {
        child = scheduler.Issue(22, [&](Scheduler::Result) { values.push_back(6); });
        CHECK(scheduler.Publish(child, {}));
        CHECK(scheduler.Drain(22) == 0);
        CHECK(!scheduler.CanCheckpoint());
        values.push_back(5);
    });
    CHECK(scheduler.Publish(parent, {}));
    CHECK(scheduler.Drain(22) == 1 && values.back() == 5);
    CHECK(scheduler.Drain(22) == 1 && values.back() == 6);

    // Reset from a callback invalidates the rest of that batch.
    const auto reset = scheduler.Issue(23, [&](Scheduler::Result) {
        scheduler.Reset();
        child = scheduler.Issue(23, [&](Scheduler::Result) { values.push_back(7); });
        CHECK(scheduler.Publish(child, {}));
    });
    const auto dead = scheduler.Issue(23, [&](Scheduler::Result) { values.push_back(99); });
    CHECK(scheduler.Publish(reset, {}) && scheduler.Publish(dead, {}));
    CHECK(scheduler.Drain(23) == 1 && values.back() == 6);
    CHECK(scheduler.Drain(23) == 1 && values.back() == 7);
    CHECK(scheduler.Pending() == 0);

    const auto throwing = scheduler.Issue(24, [](Scheduler::Result) { throw std::runtime_error("fixture"); });
    CHECK(scheduler.Publish(throwing, {}));
    bool caught = false;
    try { scheduler.Drain(24); } catch (const std::runtime_error&) { caught = true; }
    CHECK(caught && scheduler.CanCheckpoint());
    std::printf("Scheduler: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
