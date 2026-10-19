#include "games/slus_20909_crash_twinsanity/tests/services/bank_pacing_oracle_case.h"

#include <algorithm>
#include <array>
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

namespace {
// The complete adjacent bank intents of the stage11 model run's bounded RPC
// head at boundary 113: tests/services/fixtures/stage11_banks.tsv
// (bank id, byte offset in CRASH6/CRASH.BD, bytes). Read-only evidence; the
// table is the same prefix the recorded deadline diagnostic consumed.
constexpr std::array<std::array<uint32_t, 3>, 15> kRecorded{{
    {{32768, 3878226, 5168}}, {{32769, 3883394, 3072}}, {{32770, 3886466, 3072}},
    {{14, 3018936, 6096}}, {{19, 3025032, 8224}}, {{20, 3033256, 6432}},
    {{21, 3039688, 10208}}, {{22, 3049896, 17648}}, {{23, 3067544, 13904}},
    {{26, 3081448, 11696}}, {{27, 3093144, 8608}}, {{28, 3101752, 3024}},
    {{51, 3104776, 12304}}, {{52, 3117080, 16160}}, {{98, 3133240, 5072}},
}};
constexpr uint32_t kArms = 3;   // model; selected without service time; selected with service time
constexpr uint64_t kStartTick = 100;
constexpr uint32_t kFrameBound = 4096;   // a runaway pacing loop fails instead of hanging

struct ArmResult {
    uint64_t frame_ends = 0;
    std::array<uint64_t, kRecorded.size()> frame_ends_after{};
    std::array<double, kRecorded.size()> deadline_ms{};
    std::array<uint64_t, kRecorded.size()> replies{};
    std::array<std::vector<uint8_t>, kRecorded.size()> bytes;
    uint64_t charged_us = 0;
};
}  // namespace

int RunBankPacingOracle(CpuContext& ctx, ps2r::iop::IopModuleTable& table,
    ps2r::title::stream_irx::StreamIrxPlugin& plugin, ps2r::sound::SpuCore& spu,
    ps2r::title::stream_irx::StreamRpcServer& face, const LoaderFileIo::Deps& original_deps) {
    int failures = 0;
    auto check = [&](bool value, const char* name) {
        if (!value) { ++failures; std::printf("FAIL bank pacing %s\n", name); }
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
    std::array<ArmResult, kArms> results;
    const auto saved_service_us = table.Rpc().ServiceUs();
    table.Rpc().SetServiceUs(160);   // the production round-trip charge of the retained service
    for (uint32_t arm = 0; arm < kArms; ++arm) {
        const bool selected = arm != 0;
        const bool service_time = arm == 2;
        auto& result = results[arm];
        LoaderFileIo::ResetState();
        check(StreamLibrary::Install(capture, install, true), "file owner selected in every arm");
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
        uint64_t tick = kStartTick;
        auto deps = original_deps;
        deps.model = &model;
        deps.now_tick = [&] { return tick; };
        deps.park_frame = [&] { ++tick; };
        LoaderFileIo::Bind(deps);
        auto& clock = table.Kernel().Clock();
        auto* files = StreamLibrary::Scheduling();
        check(files != nullptr, "owned file scheduler present");
        if (!files) break;
        // The root's ServiceTimeline observer shape: the remaining owner's
        // in-frame offset mapped onto the game's frame base. Read-only.
        check(files->SetServiceTime(service_time, [&] {
            return static_cast<double>(tick) * ReadTiming::kTickMs +
                   static_cast<double>(clock.OffsetInFrameUs()) / 1000;
        }), "service-time selection per arm");
        uint32_t charges = 0;
        plugin.Cdvdman().SetCompletionDue([&](uint32_t lba, uint32_t sectors, uint64_t now_us) {
            const auto base_us = clock.Frames() * ps2r::iop::IopClock::kFrameUs;
            const auto offset = now_us > base_us ? now_us - base_us : 0;
            const auto frame_ms = static_cast<double>(tick) * ReadTiming::kTickMs;
            const auto end = meter.ChargeReadAtMs(frame_ms + static_cast<double>(offset) / 1000, lba, sectors);
            ++charges;
            return base_us + static_cast<uint64_t>(std::max(end - frame_ms, 0.0) * 1000);
        });
        // The loader's headroom rule as the root pacing seam reads it: owned
        // settled time when selected, else the remaining service's in-frame
        // clock. A lift ends the frame: the game tick, the remaining clock's
        // frame base and owned boundary delivery all advance together.
        const auto headroom = [&] {
            return service_time ? files->ServiceHasHeadroom() : clock.NowUs() < clock.FrameCeilingUs();
        };
        const auto pace = [&] {
            uint32_t ends = 0;
            while (!headroom() && ends < kFrameBound) {
                ++tick; ++ends; ++result.frame_ends;
                clock.OnFrameBoundary();
                StreamLibrary::AdvanceBoundary(tick);
            }
            check(ends < kFrameBound, "pacing loop terminates");
        };
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
        // Every arm starts at the same frame base with the setup round trips
        // absorbed: a fresh frame, clean read head, no carried service time.
        clock.RebaseToFrame(clock.NowUs() / ps2r::iop::IopClock::kFrameUs + 4);
        check(meter.Timing()->Restore({}), "clean shared read timeline");
        check(headroom(), "arms begin with loader headroom");
        const auto charged_before = table.Rpc().ChargedUs();
        for (uint32_t request = 0; request < kRecorded.size(); ++request) {
            const auto bank = kRecorded[request][0], offset = kRecorded[request][1], bytes = kRecorded[request][2];
            const auto reply_before = table.Rpc().Completed();
            memory::write<uint32_t>(object, 0x30001);
            const auto before_cd = plugin.Cdvdman().ReadsIssued();
            const auto before_transfer = plugin.Libsd().VoiceTransBytes();
            std::memset(spu.SpuRam() + 0x90000, 0x5a, bytes);
            call(0x2ada68, {object, bank, offset, bytes});
            check(call(0x2adaf0, {object}) != 0, "bank queued pending");
            const auto before_rpc = face.Calls();
            for (uint32_t lap = 0; lap < 64 && call(0x2adaf0, {object}) != 0; ++lap)
                local(LoaderFileIo::WaitReadComplete, object);
            check(call(0x2adaf0, {object}) == 0, "wait reaches completion");
            call(0x1e8278, {bank});
            call(0x1e2958, {0});
            if (!selected) {
                for (uint32_t lap = 0; lap < 64 &&
                    plugin.Libsd().VoiceTransBytes() - before_transfer < bytes; ++lap) deps.pump_module(ctx);
                check(plugin.Libsd().VoiceTransBytes() - before_transfer >= bytes,
                      "comparison completes every sample transfer");
                check(charges && plugin.Cdvdman().ReadsIssued() > before_cd && face.Calls() > before_rpc,
                      "comparison really ran CD and service commands");
                spu.ApplyQueued();
                auto& iop = ps2r::title::StubSurface::Memory();
                const auto entries = iop.Read<uint32_t>(0x12e5c);
                const auto registry = iop.Read<uint32_t>(0x133e0);
                uint32_t asset = 0;
                for (uint32_t index = 0; index < entries && index < 256; ++index)
                    if (iop.Read<uint32_t>(registry + index * 16) == bank)
                        asset = iop.Read<uint32_t>(registry + index * 16 + 4);
                check(asset && asset <= ps2r::sound::kSpuRamSize - bytes, "model publishes the bank allocation");
                if (asset && asset <= ps2r::sound::kSpuRamSize - bytes)
                    result.bytes[request].assign(spu.SpuRam() + asset, spu.SpuRam() + asset + bytes);
            } else {
                check(charges == 0 && plugin.Cdvdman().ReadsIssued() == before_cd &&
                      plugin.Libsd().VoiceTransBytes() == before_transfer && model.Enqueued() == 0,
                      "selected read executes no model CD, sample transfer or loader queue");
                check(audio.HasSound(bank), "selected bank published");
                if (audio.HasSound(bank)) result.bytes[request] = *audio.Inspect().samples.at(bank).bytes;
                check(result.bytes[request] == results[0].bytes[request], "same complete bank bytes as the model");
                const auto settled = files->ServiceInstantMs();
                check(!service_time || settled >= meter.Timing()->Capture().last_end_ms,
                      "owned wait settles at least the accepted read deadline");
                check(service_time || files->ServiceSequence() == 0, "unselected owner settles nothing");
            }
            result.replies[request] = table.Rpc().Completed() - reply_before;
            result.deadline_ms[request] = meter.BusyUntilMs() - kStartTick * ReadTiming::kTickMs;
            // The loader lap after this bank: lift while the service instant lies
            // beyond the slice, then the next bank issues on the resumed frame.
            pace();
            result.frame_ends_after[request] = result.frame_ends;
            std::printf("BankPacing: arm=%u request=%u bank=%u bytes=%u replies=%llu deadline_ms=%.6f "
                        "iop_offset_us=%llu owned_ms=%.6f frame_ends=%llu tick=%llu\n",
                arm, request, bank, bytes, static_cast<unsigned long long>(result.replies[request]),
                result.deadline_ms[request], static_cast<unsigned long long>(clock.OffsetInFrameUs()),
                files->ServiceInstantMs() - kStartTick * ReadTiming::kTickMs,
                static_cast<unsigned long long>(result.frame_ends), static_cast<unsigned long long>(tick));
        }
        result.charged_us = table.Rpc().ChargedUs() - charged_before;
        audio.Reset();
        local(LoaderFileIo::Close, 3);
        call(0x1e75d8, {0x8000});
        call(0x1e2958, {0});
        check(title::SoundControls::Reset() &&
              title::SoundControls::Install(capture, install, nullptr), "control teardown");
        check(title::SoundBanks::Install(capture, install, nullptr), "bank teardown");
        plugin.Cdvdman().SetCompletionDue({});
        check(files->SetServiceTime(false), "detach service observer");
        LoaderFileIo::ResetState();
    }
    table.Rpc().SetServiceUs(saved_service_us);
    LoaderFileIo::Bind(original_deps);
    const auto& model = results[0];
    const auto& plain = results[1];
    const auto& settled = results[2];
    // Attribution contract: the model charges loading frames for bank service;
    // an owned bank wait that settles no service time charges none of them,
    // and the settled owner recovers them up to the retired round-trip cost.
    check(model.frame_ends > 0, "model bank service consumes loading frames");
    check(plain.frame_ends < model.frame_ends, "unsettled owned banks consume fewer frames than the model");
    check(settled.frame_ends > plain.frame_ends, "settled service time restores loading frames");
    check(settled.frame_ends <= model.frame_ends, "settled owner never exceeds the model's frames");
    check(settled.charged_us <= model.charged_us, "owned arm charges no more round trips than the model");
    const double residual_ms = static_cast<double>(model.charged_us - settled.charged_us) / 1000.0;
    const double deadline_gap = model.deadline_ms.back() - settled.deadline_ms.back();
    check(deadline_gap >= 0 && deadline_gap <= residual_ms + 1e-6,
          "final deadline gap is bounded by the retired round-trip charge");
    std::printf("BankPacingSelfOracle: banks=%zu bytes=%u model_frames=%llu owned_frames=%llu settled_frames=%llu "
                "model_rpc_us=%llu owned_rpc_us=%llu settled_rpc_us=%llu model_deadline_ms=%.6f settled_deadline_ms=%.6f "
                "owned_deadline_ms=%.6f failures=%d\n",
        kRecorded.size(), [] { uint32_t total = 0; for (const auto& row : kRecorded) total += row[2]; return total; }(),
        static_cast<unsigned long long>(model.frame_ends), static_cast<unsigned long long>(plain.frame_ends),
        static_cast<unsigned long long>(settled.frame_ends), static_cast<unsigned long long>(model.charged_us),
        static_cast<unsigned long long>(plain.charged_us), static_cast<unsigned long long>(settled.charged_us),
        model.deadline_ms.back(), settled.deadline_ms.back(), plain.deadline_ms.back(), failures);
    return failures;
}
