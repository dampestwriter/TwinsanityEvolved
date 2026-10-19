#include "games/slus_20909_crash_twinsanity/tests/services/service_time_case.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/stream/file_schedule.h"

int RunServiceTimeCase() {
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL service time %s\n", name); }
    };
    ps2r::CdvdConfig config;
    config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    auto timing = std::make_shared<ReadTiming>(config);
    auto budget = std::make_shared<ReadTiming::Budget>();
    FileSchedule files;
    uint64_t tick = 100;
    const double frame_start = tick * ReadTiming::kTickMs;
    double observed = frame_start;
    unsigned parks = 0;
    std::vector<uint32_t> delivered;
    files.Bind(timing, budget, [&] { return tick; }, [&](const auto& p) {
        // OLD P6/P7: clock settlement, bytes, then completion observation.
        // OLD P6/P7 pin for the settled owner; the unsettled arm below is the
        // named exception and is checked at its wait instead.
        check(!files.UsesServiceTime() || files.ServiceInstantMs() >= timing->Capture().last_end_ms,
              "blocking delivery sees settled deadline");
        delivered.push_back(p.object);
    });
    files.SetParkFrame([&] { ++tick; ++parks; });
    check(files.SetServiceTime(true, [&] { return observed; }), "bind observed remaining owner once");
    FileSchedule::Payload bank;
    bank.object = 0x10000; bank.handle = 3;
    bank.kind = FileSchedule::Destination::Sound;
    bank.sound = 0x8000; bank.generation = 1; bank.sequence = 1;
    bank.bytes.assign(5168, 0x5a);
    check(files.Submit(bank, 0x1ffff, 4, tick, true), "queue source-sized bank");
    check(files.ServiceSequence() == 0 && files.ServiceHasHeadroom(), "queue consumes no service time");
    check(!files.SetServiceTime(false), "cannot relabel outstanding ownership");
    const auto accepted_at = files.ServiceInstantMs();
    // Captured Send accepts the prefix at entry. Later retained work cannot
    // move that already accepted input to the end of a callback.
    observed += 7;
    files.AcceptSounds(files.LastOrder(), accepted_at);
    const double expected = frame_start + ReadTiming::kLongSeekMs + 4 / ReadTiming::kSectorsPerMs;
    check(timing->Capture().last_end_ms == expected, "acceptance uses captured logical instant");
    check(files.Wait(bank.object) && tick == 100 && parks == 0, "bank wait preserves game tick");
    check(files.ServiceInstantMs() == expected && !files.ServiceHasHeadroom(),
          "settled bank consumes loader headroom without an IOP write");
    check(observed == frame_start + 7, "remaining owner clock is untouched");
    FileSchedule::State saved;
    check(files.Capture(saved) && saved.service_time && saved.service.sequence == 1,
          "capture owned service position");
    files.CancelObject(bank.object);
    // A later read starts after the bank, even when it uses the same file and
    // therefore has no seek. Advancing the read head alone cannot express an
    // unrelated service's later acceptance instant.
    observed = expected + 13;
    bank.sequence = 2;
    check(files.Submit(bank, 0x20003, 1, tick, true), "next bank staged");
    files.AcceptSounds(files.LastOrder());
    const double second_end = observed + 1 / ReadTiming::kSectorsPerMs;
    check(timing->Capture().last_end_ms == second_end && files.Wait(bank.object),
          "later service input orders next read");
    check(delivered.size() == 2 && files.ServiceInstantMs() == second_end,
          "one delivery per accepted bank");
    files.Reset();
    observed = frame_start + 7;
    check(files.Restore(saved) && files.ServiceInstantMs() == expected &&
          files.ServiceSequence() == 1 && delivered.size() == 2,
          "restore position without recharging or delivering");
    check(files.Wait(saved.reads.begin()->first) && delivered.size() == 2,
          "restored consumed request does not publish twice");
    auto invalid = saved;
    invalid.service.settled_ms = std::numeric_limits<double>::quiet_NaN();
    check(!files.Validate(invalid), "reject invalid service instant before mutation");
    invalid = saved; invalid.service_time = false;
    check(!files.Validate(invalid), "selection identity cannot silently drop service position");
    tick = static_cast<uint64_t>(std::ceil(expected / ReadTiming::kTickMs));
    check(files.ServiceHasHeadroom(), "normal game ticks regain headroom");
    check(files.ServiceInstantMs() == tick * ReadTiming::kTickMs, "clock floor follows logical ticks");
    files.Reset();
    check(files.SetServiceTime(false) && files.ServiceSequence() == 0, "teardown detaches observer");
    // The stage11 gap, named: without owned service time the same bank wait
    // publishes past its accepted deadline (an overrun) and leaves the loader
    // headroom untouched, so the game yields no loading frame for it.
    const auto overruns_before = files.Count().overruns;
    bank.sequence = 3;
    check(files.Submit(bank, 0x3ffff, 4, tick, true), "unsettled bank staged");
    files.AcceptSounds(files.LastOrder());
    check(timing->Capture().last_end_ms >= ReadTiming::FrameEndMs(tick), "deadline lies past the frame");
    check(files.Wait(bank.object) && files.Count().overruns == overruns_before + 1 &&
          files.ServiceHasHeadroom() && files.ServiceSequence() == 0 && delivered.size() == 3,
          "unsettled bank wait is an overrun that charges no loader headroom");
    files.Reset();
    failures += RunLoaderChainOracle(checks);
    std::printf("ServiceTime: %d checks, %d failures\n", checks, failures);
    return failures;
}

// The parent's three-arm gate shape (gates/m4-service-time, ticks 131-189):
// after a bank wait settles, the loader reads a file in a chain of blocking
// windows while the game tick advances by wait parks only. The remaining IOP
// clock's frame base does not move during those parked ticks, so its in-frame
// offset is stale. Same inputs through two arms: the comparison arm (service
// time off) and the owned arm (on, observing the stale offset). The ordinary
// chain must charge, park and deliver identically; only the bank settles.
namespace {
struct ChainArm {
    std::vector<double> end_ms;
    std::vector<uint64_t> wait_tick;
    uint64_t parks = 0, sequence = 0, sequence_after_bank = 0;
    bool headroom_after_bank = true;
};
ChainArm RunChainArm(bool service_time, int& checks, int& failures) {
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL loader chain %s\n", name); }
    };
    ps2r::CdvdConfig config;
    config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    auto timing = std::make_shared<ReadTiming>(config);
    auto budget = std::make_shared<ReadTiming::Budget>();
    FileSchedule files;
    ChainArm arm;
    uint64_t tick = 124;
    constexpr double kStaleOffsetMs = 7.74;   // the gate's service_us - service_frame_us
    files.Bind(timing, budget, [&] { return tick; }, [](const auto&) {});
    files.SetParkFrame([&] { ++tick; ++arm.parks; });
    check(files.SetServiceTime(service_time, [&] {
        return static_cast<double>(tick) * ReadTiming::kTickMs + kStaleOffsetMs;
    }), "arm selection");
    // One bank at the module's read instant (the remaining clock's in-frame
    // offset). The comparison arm is the module path: the shared read position
    // is charged directly and the remaining clock is forced to the deadline.
    // The owned arm accepts and waits through the file owner and settles.
    const double module_instant = static_cast<double>(tick) * ReadTiming::kTickMs + kStaleOffsetMs;
    double bank_end = 0;
    if (!service_time) {
        bank_end = timing->ChargeOtherRead(module_instant, 476200, 3);
    } else {
        FileSchedule::Payload bank;
        bank.object = 0x20000; bank.handle = 3;
        bank.kind = FileSchedule::Destination::Sound;
        bank.sound = 0x8000; bank.generation = 1; bank.sequence = 1;
        bank.bytes.assign(5168, 0x5a);
        check(files.Submit(bank, 476200, 3, tick, true), "bank staged");
        check(files.ServiceInstantMs() == module_instant, "service instant is the module instant");
        files.AcceptSounds(files.LastOrder());
        bank_end = timing->Capture().last_end_ms;
        check(files.Wait(bank.object) && tick == 124, "bank wait keeps the game tick");
    }
    arm.sequence_after_bank = files.ServiceSequence();
    arm.headroom_after_bank = files.ServiceHasHeadroom();
    // Both arms lift frames until the bank deadline lies inside the slice: the
    // comparison arm through the forced remaining clock, the owned arm through
    // its settled instant.
    while (ReadTiming::FrameEndMs(tick) <= bank_end) {
        check(!service_time || !files.ServiceHasHeadroom(), "settled bank denies headroom while unpaid");
        ++tick;
    }
    check(files.ServiceHasHeadroom(), "paid bank restores headroom");
    // The gate's chain: 60.19 ms windows (a short seek and thirteen sectors),
    // each waited for before the next is issued.
    uint32_t lba = 476310;
    for (uint32_t window = 0; window < 15; ++window) {
        FileSchedule::Payload read;
        read.object = 0x30000 + window * 0x100; read.handle = 1;
        read.destination = 0x1000000; read.bytes.assign(13 * 2048, 0x11);
        check(files.Submit(read, lba, 13, tick), "window issued");
        arm.end_ms.push_back(timing->Capture().last_end_ms);
        check(files.Wait(read.object), "window waited");
        arm.wait_tick.push_back(tick);
        lba += 13 + 40;   // the next window seeks short, as the recorded chain does
    }
    arm.sequence = files.ServiceSequence();
    return arm;
}
}  // namespace

int RunLoaderChainOracle(int& checks) {
    int failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL loader chain %s\n", name); }
    };
    const ChainArm model = RunChainArm(false, checks, failures);
    const ChainArm owned = RunChainArm(true, checks, failures);
    check(model.sequence_after_bank == 0 && model.headroom_after_bank, "comparison arm settles nothing");
    check(owned.sequence_after_bank == 1 && !owned.headroom_after_bank, "owned bank wait settles once");
    check(owned.sequence == 1, "ordinary waits add no service sequence");
    for (size_t i = 0; i < model.end_ms.size() && i < owned.end_ms.size(); ++i)
        if (model.end_ms[i] != owned.end_ms[i] || model.wait_tick[i] != owned.wait_tick[i])
            std::printf("loader chain window %zu: model end=%.6f tick=%llu owned end=%.6f tick=%llu\n", i,
                model.end_ms[i], static_cast<unsigned long long>(model.wait_tick[i]),
                owned.end_ms[i], static_cast<unsigned long long>(owned.wait_tick[i]));
    check(model.end_ms == owned.end_ms, "ordinary windows charge from the same frame start");
    check(model.wait_tick == owned.wait_tick, "ordinary waits park the same game ticks");
    check(model.parks == owned.parks, "same wait park count");
    bool alternates = model.wait_tick.size() > 2;
    for (size_t i = 2; i < model.wait_tick.size(); ++i)
        alternates = alternates && model.wait_tick[i] - model.wait_tick[i - 1] >= 3 &&
                     model.wait_tick[i] - model.wait_tick[i - 1] <= 4;
    check(alternates, "60 ms windows park three or four ticks, never a stale offset's uniform four");
    std::printf("LoaderChainSelfOracle: windows=%zu model_parks=%llu owned_parks=%llu owned_sequence=%llu "
                "last_wait_tick=%llu/%llu failures=%d\n",
        model.wait_tick.size(), static_cast<unsigned long long>(model.parks),
        static_cast<unsigned long long>(owned.parks), static_cast<unsigned long long>(owned.sequence),
        static_cast<unsigned long long>(model.wait_tick.back()),
        static_cast<unsigned long long>(owned.wait_tick.back()), failures);
    return failures;
}
