#include "games/slus_20909_crash_twinsanity/tests/services/bank_timing_oracle_case.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.h"

int RunBankTimingOracle(CpuContext& ctx, ps2r::iop::IopModuleTable& table,
    ps2r::title::stream_irx::StreamIrxPlugin& plugin, ps2r::sound::SpuCore& spu,
    ps2r::title::stream_irx::StreamRpcServer& face, const LoaderFileIo::Deps& original_deps) {
    int failures = 0;
    auto check = [&](bool value, const char* name) {
        if (!value) { ++failures; std::printf("FAIL bank timing %s\n", name); }
    };
    constexpr uint32_t gp = 0x3114f0, object = 0x512000, path = 0x500000;
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, StreamLibrary::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    auto call = [&](uint32_t address, std::initializer_list<uint32_t> arguments) {
        ctx.cpuRegs.GPR.r[28].UD[0] = gp; ctx.cpuRegs.GPR.r[29].UD[0] = 0x700000;
        ctx.cpuRegs.GPR.r[31].UD[0] = 0x100100;
        unsigned reg = 4;
        for (const auto argument : arguments) ctx.cpuRegs.GPR.r[reg++].UD[0] = argument;
        DispatchTable::Invoke(address, ctx);
        return ctx.cpuRegs.GPR.r[2].SL[0];
    };
    auto local = [&](void (*body)(CpuContext&), uint32_t argument) {
        ctx.cpuRegs.GPR.r[4].UD[0] = argument;
        body(ctx);
        return ctx.cpuRegs.GPR.r[2].SL[0];
    };
    title::Audio audio;
    uint64_t model_wait_ticks = 0;
    double model_busy = 0;
    uint32_t model_head = 0;
    std::vector<uint8_t> model_bytes;
    const auto saved_service_us = table.Rpc().ServiceUs();
    // Keep file ownership selected in BOTH arms, as in the production pair.
    // The model bank uses the actual captured service + STREAM/CD worker.
    for (uint32_t arm = 0; arm < 24; ++arm) {
        const uint32_t scenario = arm % 12 / 2;
        const bool service_time = arm >= 12;
        const bool selected = (arm & 1) != 0;
        table.Rpc().SetServiceUs(scenario < 3 ? 0 : 160); // production round-trip charge
        LoaderFileIo::ResetState();
        check(StreamLibrary::Install(capture, install, true), "file owner selected");
        check(title::SoundBanks::Install(capture, install, selected ? &audio : nullptr), "bank selection");
        check(title::SoundControls::Install(capture, install, selected ? &audio : nullptr,
            [&spu](uint32_t channel, uint16_t left, uint16_t right) {
                const auto entry = channel / 24 | (channel % 24) << 1;
                spu.SetParam(entry, left); spu.SetParam(entry | 0x100, right);
            }), "control selection");
        ps2r::CdvdConfig config;
        config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
        DeliveryMeter meter(config);
        DeliveryBudget budget(0);
        LoaderModel model(meter, budget);
        uint64_t tick = 100;
        auto deps = original_deps;
        deps.model = &model;
        deps.now_tick = [&] { return tick; };
        deps.park_frame = [&] { ++tick; };
        LoaderFileIo::Bind(deps);
        auto& clock = table.Kernel().Clock();
        uint32_t charges = 0;
        plugin.Cdvdman().SetCompletionDue([&](uint32_t lba, uint32_t sectors, uint64_t now_us) {
            const auto base_us = clock.Frames() * ps2r::iop::IopClock::kFrameUs;
            const auto offset = now_us > base_us ? now_us - base_us : 0;
            const auto frame_ms = static_cast<double>(tick) * ReadTiming::kTickMs;
            const auto end = meter.ChargeReadAtMs(frame_ms + static_cast<double>(offset) / 1000, lba, sectors);
            ++charges;
            return base_us + static_cast<uint64_t>(std::max(end - frame_ms, 0.0) * 1000);
        });
        // The source file-reader ring is 64 KiB. Its remaining allocation is
        // setup only; no CD or file work has been issued yet.
        call(0x1e6e60, {});
        call(0x1e1ca8, {1, 1, 0x10000});
        call(0x1e7540, {256});
        const char name[] = "crash6\\crash.bd";
        for (size_t i = 0; i < sizeof(name); ++i) memory::write<uint8_t>(path + i, name[i]);
        memory::write<int32_t>(gp - 0x74d8, 3);
        check(local(LoaderFileIo::Open, path) == 3, "same file");
        memory::write<uint32_t>(object, 0x20001);
        memory::write<int32_t>(object + LoaderFileIo::kFsHandle, 3);
        memory::write<uint32_t>(object + LoaderFileIo::kFsDest, 0);
        memory::write<uint32_t>(object + LoaderFileIo::kFsCbObj, 0);
        memory::write<uint32_t>(0x309ad8, 0x90000);
        local(LoaderFileIo::FileSize, object);
        call(0x1e2958, {0});
        clock.RebaseToFrame(clock.NowUs() / ps2r::iop::IopClock::kFrameUs + 4);
        check(meter.Timing()->Restore({}), "clean shared read timeline");
        check(StreamLibrary::Scheduling()->SetServiceTime(service_time, [&] {
            return static_cast<double>(tick) * ReadTiming::kTickMs +
                   static_cast<double>(clock.OffsetInFrameUs()) / 1000;
        }), "service timeline selection");
        memory::write<uint32_t>(object, 0x30001);
        const auto before_rpc = face.Calls(), before_cd = plugin.Cdvdman().ReadsIssued();
        const auto before_transfer = plugin.Libsd().VoiceTransBytes();
        std::memset(spu.SpuRam() + 0x90000, 0x5a, 5168);
        call(0x2ada68, {object, 0x8000, 0x3b2ed2, 5168});
        check(memory::read<uint32_t>(object) & LoaderFileIo::kFlagReadPending,
              "file-to-bank adapter preserves caller's new pending flag");
        check(call(0x2adaf0, {object}) != 0 && meter.BusyUntilMs() == 0,
              "issue queues without charging or publishing");
        // Source ReadSoundBank queues work. Advancing a clock cannot accept it.
        if (scenario % 3 == 0) tick += 20;
        StreamLibrary::AdvanceBoundary(tick);
        check(call(0x2adaf0, {object}) != 0, "unsent bank remains pending across boundaries");
        check(!audio.HasSound(0x8000), "unsent bank cannot publish bytes");
        const auto wait_start = tick;
        if (scenario % 3 == 2) check(call(0x1e2958, {0}) >= 0, "explicit send accepts the queued bank");
        const auto wait_service = clock.NowUs();
        for (uint32_t lap = 0; lap < 64 && call(0x2adaf0, {object}) != 0; ++lap)
            local(LoaderFileIo::WaitReadComplete, object);
        if (selected && service_time)
            check(clock.NowUs() == wait_service, "owned wait does not advance retained clock");
        const auto wait_ticks = tick - wait_start;
        check(call(0x2adaf0, {object}) == 0, "accepted wait reaches completion");
        call(0x1e8278, {0x8000});
        call(0x1e2958, {0});
        if (!selected) {
            for (uint32_t lap = 0; lap < 64 &&
                plugin.Libsd().VoiceTransBytes() - before_transfer < 5168; ++lap) deps.pump_module(ctx);
            check(plugin.Libsd().VoiceTransBytes() - before_transfer >= 5168,
                  "comparison completes every sample transfer");
            check(charges && plugin.Cdvdman().ReadsIssued() > before_cd &&
                  face.Calls() > before_rpc, "comparison really ran CD and service commands");
            model_wait_ticks = wait_ticks;
            model_busy = meter.BusyUntilMs();
            model_head = meter.Timing()->Capture().head_lba;
            spu.ApplyQueued();
            auto& iop = ps2r::title::StubSurface::Memory();
            const auto entries = iop.Read<uint32_t>(0x12e5c);
            const auto registry = iop.Read<uint32_t>(0x133e0);
            uint32_t asset = 0;
            // Observe the bank table consumed by the model's voice lookup.
            // STREAM's allocator can replace a requested upload address.
            // The owned path has no hardware address to compare.
            for (uint32_t index = 0; index < entries && index < 256; ++index)
                if (iop.Read<uint32_t>(registry + index * 16) == 0x8000)
                    asset = iop.Read<uint32_t>(registry + index * 16 + 4);
            check(asset && asset <= ps2r::sound::kSpuRamSize - 5168,
                  "model publishes the bank allocation consumed by voices");
            if (asset && asset <= ps2r::sound::kSpuRamSize - 5168)
                model_bytes.assign(spu.SpuRam() + asset, spu.SpuRam() + asset + 5168);
        } else {
            check(charges == 0 && plugin.Cdvdman().ReadsIssued() == before_cd &&
                  plugin.Libsd().VoiceTransBytes() == before_transfer && model.Enqueued() == 0,
                  "selected read executes no model CD, sample transfer or loader queue");
            check(wait_ticks == model_wait_ticks, "wait preserves logical tick accounting");
            check(std::abs(meter.BusyUntilMs() - model_busy) < 0.000001 &&
                  meter.Timing()->Capture().head_lba == model_head,
                  "same shared read deadline and position after wait");
            check(audio.HasSound(0x8000) && *audio.Inspect().samples.at(0x8000).bytes == model_bytes,
                  "timed wait publishes the same complete bank bytes");
            if (service_time) {
                check(StreamLibrary::Scheduling()->ServiceInstantMs() >= meter.BusyUntilMs() &&
                      !StreamLibrary::Scheduling()->ServiceHasHeadroom(),
                      "actual accepted bank settles later caller headroom");
            }
        }
        std::printf("BankTimingSelfOracle: scenario=%u selected=%d service_time=%d wait_ticks=%llu charge_count=%u busy=%f failures=%d\n",
            scenario, selected, service_time, static_cast<unsigned long long>(wait_ticks), charges,
            meter.BusyUntilMs() - 100 * ReadTiming::kTickMs, failures);
        local(LoaderFileIo::Close, 3);
        call(0x1e75d8, {0x8000});
        call(0x1e2958, {0});
        check(title::SoundControls::Reset() &&
              title::SoundControls::Install(capture, install, nullptr), "control teardown");
        check(title::SoundBanks::Install(capture, install, nullptr), "bank teardown");
        plugin.Cdvdman().SetCompletionDue({});
        LoaderFileIo::ResetState();
        check(StreamLibrary::Scheduling()->SetServiceTime(false), "detach bounded fixture timeline");
    }
    table.Rpc().SetServiceUs(saved_service_us);
    LoaderFileIo::Bind(original_deps);
    return failures;
}
