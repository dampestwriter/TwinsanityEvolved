#include <cmath>
#include <cstdio>

#include "games/slus_20909_crash_twinsanity/composition/service_timeline.h"
#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/iop/iop_clock.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL ServiceTimeline %s\n", name); }
    };
    EmuDomain domain;
    domain.SetBypass(true);
    for (uint64_t raw = 1; raw <= 100; ++raw) domain.OnRawTick(raw);
    ps2r::iop::IopClock remaining;
    remaining.RebaseToFrame(100);
    ps2r::CdvdConfig config;
    DeliveryMeter meter(config);
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    check(StreamLibrary::Install([](uint32_t) {
        return StreamLibrary::Body{[](CpuContext&, uint32_t) {}};
    }, [](uint32_t, StreamLibrary::Body) { return true; }, true), "selected file owner");
    StreamLibrary::Bind(&model);
    StreamLibrary::BindFiles([&] { return domain.DomainTick(); }, [](const auto&) {});
    unsigned completed = 0;
    check(StreamLibrary::BindSounds([&](const auto&) { ++completed; }, [](const auto&) {}),
          "typed bank consumer");
    auto* files = StreamLibrary::Scheduling();
    {
        ps2r::app::ServiceTimeline comparison(false, domain, &remaining);
        check(comparison.Install() && comparison.TimeLeft() && comparison.Sequence() == 0,
              "off arm retains old logical clock");
        remaining.AdvanceToUs(remaining.NowUs() + 320);
        const double original = domain.DomainTick() * ReadTiming::kTickMs + 0.320;
        check(comparison.ReadStartMs(remaining.NowUs()) == original, "off arm keeps original input arithmetic");
        ps2r::app::ServiceTimeline selected(true, domain, &remaining);
        check(selected.Install(), "root binds read-only observation");
        const auto before = remaining.NowUs();
        FileSchedule::Payload bank;
        bank.object = 0x10000; bank.handle = 1; bank.sound = 1;
        bank.kind = FileSchedule::Destination::Sound;
        bank.generation = bank.sequence = 1; bank.bytes.assign(5168, 1);
        check(files->Submit(bank, 0x20000, 4, domain.DomainTick(), true), "real typed request queued");
        files->AcceptSounds(files->LastOrder());
        check(files->Wait(bank.object) && completed == 1 && files->Count().delivered == 1,
              "owned wait delivers positive work");
        check(remaining.NowUs() == before && domain.DomainTick() == 100,
              "root repair advances neither IOP clock nor game tick");
        check(!selected.TimeLeft() && comparison.TimeLeft() && selected.Sequence() == 1,
              "headroom consumes the replaced service wait");
        check(selected.ReadStartMs(remaining.NowUs()) == meter.BusyUntilMs(),
              "remaining reader inherits the same settled input");
        const auto end = meter.BusyUntilMs();
        const auto next = meter.ChargeReadAtMs(selected.ReadStartMs(remaining.NowUs()), 0x20004, 1);
        check(next == end + 1 / ReadTiming::kSectorsPerMs, "shared reader cannot rewind behind bank wait");
        const auto due_tick = static_cast<uint64_t>(std::ceil(end / ReadTiming::kTickMs));
        for (uint64_t raw = 101; raw <= due_tick; ++raw) domain.OnRawTick(raw);
        // The remaining owner hasn't run. Its observation maps to the same
        // new game tick; no artificial charges are added by headroom queries.
        const auto sequence = selected.Sequence();
        for (unsigned query = 0; query < 50; ++query) check(selected.TimeLeft(), "logical tick regains headroom");
        check(selected.Sequence() == sequence && remaining.NowUs() == before,
              "time/headroom queries are observational");
    }
    check(!files->UsesServiceTime() && files->ServiceSequence() == 0 && !files->InFlight(0x10000),
          "root lifetime detaches observer and cancels requests");
    check(StreamLibrary::BindSounds({}, {}), "consumer detaches after scheduling owner");
    StreamLibrary::Reset();
    std::printf("ServiceTimeline: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
