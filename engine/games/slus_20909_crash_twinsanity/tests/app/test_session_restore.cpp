#include "app/host/session_restore.h"
#include "app/host/session_checkpoint.h"
#include "app/host/session_files.h"
#include "app/host/session_workers.h"
#include "engine/core/config/config.h"
#include "engine/core/verify/verify_host.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/cpu/shadow_call_stack.h"
#include "engine/ee/hle/thread_manager.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ee/sched/ee_run_token.h"
#include "engine/ee/timing/boundary_sequencer.h"
#include "engine/ee/timing/game_boundary.h"
#include "generated_stack.h"
#include "games/slus_20909_crash_twinsanity/recomp/continuations.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"
#include "games/slus_20909_crash_twinsanity/tests/app/restore_owner_fixture.h"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <unistd.h>

// Generated dispatch ABI fixture only. No game body is linked/executed.
std::map<uint32_t, DispatchTable::Body> recompiled_functions;
extern const uint32_t g_recomp_fn_count = 1;
extern const uint32_t g_recomp_fn_addrs[] = {0x100008};
bool g_recomp_fn_hooked[] = {false};
extern "C" { bool crashring_enabled = false; }
void initialize_recompiled_functions() {}
uint32_t recomp_fn_id_for(uint32_t fn) { return fn == 0x100008 ? 0 : UINT32_MAX; }
bool update_function_impl(uint32_t fn, const DispatchTable::Body& body) {
    if (!recompiled_functions.count(fn)) return false;
    recompiled_functions[fn] = DispatchTable::ObserveInstall(fn, body);
    return true;
}

namespace {
using namespace ps2r::app;
using namespace ps2r::savestate;
using Action = SessionControls::Action;
using Phase = SessionControls::Phase;
unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); } } while (false)
bool Await(const std::function<bool()>& ready) {
    const auto deadline = ps2r::Park::NowNs() + 20000000000ull;
    while (!ready()) {
        if (ps2r::Park::NowNs() >= deadline) return false;
        SDL_Delay(1);
    }
    return true;
}
bool Done(const SessionControls& controls, uint64_t id, Phase phase) {
    for (const auto& result : controls.Read().recent)
        if (result.request.id == id) return result.phase == phase;
    return false;
}
std::string Read(const std::string& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int main() {
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    char directory[] = "/tmp/product_restore_composition_XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    const std::string root = directory, path = root + "/reproduction.ps2st", bad = root + "/missing-clock.ps2st";
    auto& threads = ThreadManager::Instance();
    auto& token = EeRunToken::Instance();
    threads.ResetForTest(); token.ResetForTest(); threads.SetStackBytes(2u << 20);
    MainMemory ram;
    Scratchpad scratch;
    MemorySections memory(ram, scratch, nullptr, 0);
    StateRestorer restorer(memory);
    restorer.RequireSectionOwners();
    EmuDomain domain;
    TickSource source;
    BoundarySequencer sequence;
    SessionClock clock(domain, source, sequence.Ticks(), "composed-product-fixture");
    CHECK(restorer.Registry().Register(clock.Section()));
    RestoreOwnerFixture owners(ram, domain, source, root + "/game-saves");
    CHECK(owners.Register(restorer.Registry(), clock));
    ThreadSection thread_state(threads);
    thread_state.Register(restorer.Registry());
    GameBoundary boundary(threads, sequence);
    CHECK(boundary.Attached());
    SessionRuntime runtime(nullptr);
    std::string error;
    CHECK(runtime.Open(100, false, true, root + "/recording/input.padlog", error, root));
    if (!error.empty()) return 2;
    ps2r::Config config;
    config.harness.out_dir = root + "/recording";
    config.harness.record_input = true;
    ps2r::verify::VerifyHost output;
    CHECK(output.Arm(config, "fixture", "fixture", &error));
    SessionFiles files(root + "/frames");
    std::atomic<bool> running{true};
    std::atomic<unsigned> joins{0}, publications{0}, stages{0}, resumed{0}, instructions{0};
    uint64_t expected_frames = 0;
    std::atomic<bool> allow_staging{true};
    SessionWorkers workers(threads, running, [] {}, [] {});
    SessionCheckpoint checkpoint(restorer.Registry(), {
        [&] { return token.CanCaptureCurrentThread(); },
        [&](std::string& why) { return owners.CanCheckpoint(why); },
        [&](auto& records, auto& why) { return boundary.FreshThreads(records, why); },
        DispatchTable::CanResumeAt
    });
    restorer.SetImageValidation([&](const auto& meta, const auto& blobs, auto* why) {
        std::string reason;
        const auto ok = clock.ValidateImage(meta, blobs, reason);
        if (!ok && why) *why = reason;
        return ok;
    });
    restorer.AddImagePreflight([&](const auto&, std::string* why) {
        ++stages;
        if (!allow_staging && why) *why = "fixture resource allocation refused";
        return allow_staging.load();
    });
    restorer.SetPreRestore([&](std::string*) {
        ++publications;
        return !threads.CurrentIncarnation() && joins > resumed;
    });
    SessionRestore::Lifecycle lifecycle;
    lifecycle.inspect = [&](std::string& why) { return owners.CanCheckpoint(why); };
    lifecycle.prepare = [](std::string&) { return true; };
    lifecycle.joined = [&](std::string&) { checkpoint.Join(); ++joins; return true; };
    lifecycle.resumed = [&](uint32_t frame, std::string&) {
        if (domain.BoundaryCount() != frame || ram.Data()[17] != static_cast<uint8_t>(frame) ||
            !owners.Restored()) return false;
        ++resumed; running.store(true); return true;
    };
    SessionRestore restore(runtime, workers, restorer, clock, boundary, output, files, std::move(lifecycle));
    SessionRuntime::Hooks hooks;
    hooks.stop = [&] { workers.Cancel(); };
    hooks.pause = [](bool) {};
    hooks.alive = [&] { return workers.BoundaryAlive(); };
    hooks.stopped = [&] { workers.Join(); checkpoint.Join(); output.Finalize(); };
    hooks.capture = [&](const auto& request, uint64_t frame) {
        FileMeta meta;
        std::string why;
        if (!SessionClock::SetFrame(meta, frame, why))
            return SessionRuntime::CaptureResult{Phase::Refused, why};
        const auto result = checkpoint.Capture(request.id, request.path, meta);
        return SessionRuntime::CaptureResult{result.phase, result.detail};
    };
    hooks.poll_capture = [&]() -> std::optional<SessionRuntime::CaptureCompletion> {
        const auto result = checkpoint.Poll();
        if (!result) return {};
        return SessionRuntime::CaptureCompletion{result->request, {result->phase, result->detail}, result->path};
    };
    CHECK(restore.Bind(hooks, false) && !hooks.restore);
    CHECK(restore.Bind(hooks, true) && bool(hooks.restore));
    runtime.SetHooks(std::move(hooks));
    boundary.SetConsumer([&](uint64_t frame) { if (!runtime.Boundary(frame)) throw ThreadExitUnwind{}; });
    sequence.SetStep(BoundarySequencer::Step::TickAdvance, [&](CpuContext&) {
        domain.NoteArrival(source.RawCount());
        domain.OnRawTick(source.Tick());
        domain.OnBoundary(source.RawCount());
        ram.Data()[17] = static_cast<uint8_t>(domain.BoundaryCount());
        return true;
    });
    sequence.SetStep(BoundarySequencer::Step::MaintDuties, [&](CpuContext&) {
        owners.Step();
        return true;
    });
    std::array<float, 1600> silence{};
    sequence.SetStep(BoundarySequencer::Step::Verify, [&](CpuContext&) {
        files.Select(output.LegDir(), output.OutputSegment());
        const auto key = output.OnBoundary(silence.data(), 800, {});
        output.OnPadServe(static_cast<uint32_t>(domain.DomainTick()), 0xffff, 128, 128, 128, 128);
        output.OnPresent(output.PresentKey(0, key), nullptr, 0, 0, 1, "fixture");
        files.WriteHashes(key, key, key);
        return true;
    });
    constexpr uint32_t function = 0x100008, service = 0x2c1d78, continuation = 0x1000b8;
    const auto body = DispatchTable::GeneratedBody(function, [&](CpuContext& ctx, uint32_t) {
        while (running && !runtime.Closing()) {
            ++instructions;
            ctx.cpuRegs.GPR.r[31].UL[0] = continuation;
            DispatchTable::Invoke(service, ctx);
        }
    }, recomp_continuations(function));
    recompiled_functions[function] = DispatchTable::TraceBody(function, body,
        [body](CpuContext& ctx, uint32_t entry) { EeCallFrameGuard frame(function, 0); body(ctx, entry); });
    recompiled_functions[service] = DispatchTable::ObserveInstall(service, [&](CpuContext& ctx, uint32_t) {
        if (!sequence.RunBoundary(ctx) || !sequence.DeferServiceReturn(ctx, domain.BoundaryCount()))
            throw std::runtime_error("fixture boundary was not completed");
    });
    auto& controls = runtime.Controls();
    const auto pause = controls.Submit(Action::TogglePause);
    bool sequence_ok = true;
    std::thread director([&] {
        auto require = [&](bool ok) {
            if (!ok) {
                sequence_ok = false;
                const auto view = controls.Read();
                std::fprintf(stderr, "fixture stopped: frame=%llu instructions=%u joins=%u publications=%u stages=%u resumed=%u\n",
                    static_cast<unsigned long long>(view.runtime.frame), instructions.load(), joins.load(),
                    publications.load(), stages.load(), resumed.load());
                for (const auto& item : view.recent)
                    std::fprintf(stderr, "request=%llu phase=%d detail=%s\n",
                        static_cast<unsigned long long>(item.request.id), static_cast<int>(item.phase), item.message.c_str());
                runtime.RequestClose();
            }
            return ok;
        };
        if (!require(Await([&] { return Done(controls, pause, Phase::Succeeded); }))) return;
        owners.QueueSetup();
        auto step = controls.Submit(Action::Step);
        if (!require(Await([&] { return Done(controls, step, Phase::Succeeded); }))) return;
        const auto save = controls.Submit(Action::SaveReproduction, 0, path);
        if (!require(Await([&] { return Done(controls, save, Phase::Succeeded); }))) return;
        FileMeta meta; std::vector<Blob> blobs; std::string why;
        if (!require(ReadStateFile(path, &meta, &blobs, &why))) return;
        const auto saved_frame = meta.vsync_frame;
        expected_frames = saved_frame;
        for (const auto missing : {SessionClock::kTag, ::title::InputStateCodec::kTag,
                                  ::title::SaveStateCodec::kTag}) {
            auto incomplete = blobs;
            incomplete.erase(std::remove_if(incomplete.begin(), incomplete.end(),
                [missing](const auto& blob) { return blob.tag == missing; }), incomplete.end());
            if (!require(WriteStateFile(bad, meta, incomplete, &why))) return;
            const auto refused = controls.Submit(Action::RestoreReproduction, 0, bad);
            if (!require(Await([&] { return Done(controls, refused, Phase::Refused); }))) return;
            if (!require(joins == 0 && stages == 0 && publications == 0)) return;
        }
        allow_staging = false;
        auto refused = controls.Submit(Action::RestoreReproduction, 0, path);
        if (!require(Await([&] { return Done(controls, refused, Phase::Refused); }))) return;
        if (!require(joins == 0 && stages == 1 && publications == 0)) return;
        allow_staging = true;
        owners.QueueWrite();
        step = controls.Submit(Action::Step);
        if (!require(Await([&] { return Done(controls, step, Phase::Succeeded); }))) return;
        ++expected_frames;
        refused = controls.Submit(Action::RestoreReproduction, 0, path);
        if (!require(Await([&] { return Done(controls, refused, Phase::Refused); }))) return;
        if (!require(joins == 0 && stages == 1 && publications == 0 && !owners.WriteDone())) return;
        const auto pending_save = root + "/pending-mutation.ps2st";
        refused = controls.Submit(Action::SaveReproduction, 0, pending_save);
        if (!require(Await([&] { return Done(controls, refused, Phase::Refused); }))) return;
        if (!require(!std::filesystem::exists(pending_save))) return;
        // The persistent mutation must actually finish, then its newer disk
        // bytes must survive both reconstructions of older logical save state.
        for (unsigned i = 0; i < 16 && !owners.WriteDone(); ++i) {
            step = controls.Submit(Action::Step);
            if (!require(Await([&] { return Done(controls, step, Phase::Succeeded); }))) return;
            ++expected_frames;
        }
        if (!require(owners.WriteDone() && owners.Commits() == 2 && Read(owners.File()) == std::string(1, 71))) return;
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            const auto step = controls.Submit(Action::Step);
            if (!require(Await([&] { return Done(controls, step, Phase::Succeeded); }))) return;
            ++expected_frames;
            const auto before_instructions = instructions.load();
            const auto request = controls.Submit(Action::RestoreReproduction, 0, path);
            if (!require(Await([&] { return Done(controls, request, Phase::Succeeded); }))) return;
            if (!require(instructions == before_instructions && controls.Read().runtime.paused &&
                    controls.Read().runtime.frame == saved_frame && joins == iteration + 1 &&
                    publications == joins && resumed == joins && stages == iteration + 2)) return;
            if (!require(owners.Commits() == 2 && Read(owners.File()) == std::string(1, 71))) return;
        }
        controls.Submit(Action::Close);
    });
    const int result = runtime.Run([&] {
        CpuContext context{};
        auto info = threads.AdoptCurrentThread(1, 64); info->ctx = &context;
        token.GuestEnter(1, 64);
        if (!owners.Start()) throw std::runtime_error("Fixture game-save startup failed.");
        try { DispatchTable::Invoke(function, context); } catch (const ThreadExitUnwind&) {}
        std::string why;
        if (!threads.ReleaseCurrentThread(&why)) return 2;
        return 0;
    }, 30);
    director.join();
    CHECK(result == 0 && sequence_ok && joins == 2 && publications == 2);
    CHECK(output.OutputSegment() == 2 && output.Boundaries() == expected_frames);
    CHECK(owners.Commits() == 2 && Read(owners.File()) == std::string(1, 71));
    CHECK(std::filesystem::exists(root + "/recording/output-000001/input.padlog"));
    CHECK(std::filesystem::exists(root + "/recording/output-000002/frames.fhash"));
    CHECK(Read(root + "/recording/input.padlog").find("restore_tick") == std::string::npos);
    CHECK(Read(root + "/recording/output-000001/input.padlog").find("restore_tick=") != std::string::npos);
    CHECK(Read(root + "/recording/output-000002/run.meta").find("awaiting_resume") != std::string::npos);
    CHECK(!restorer.HasPrepared() && !token.CanCaptureCurrentThread() && !threads.CurrentIncarnation());
    std::filesystem::remove_all(root);
    std::printf("SessionRestoreComposition: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
