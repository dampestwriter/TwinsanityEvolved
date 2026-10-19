#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/cpu/shadow_call_stack.h"
#include "app/host/session_checkpoint.h"
#include "engine/ee/hle/thread_manager.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/recomp_abi/host_app/gs_hle.h"
#include "engine/ee/savestate/thread_relauncher.h"
#include "engine/ee/sched/ee_run_token.h"
#include "engine/ee/timing/boundary_sequencer.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/game_boundary.h"
#include "engine/gs/hle/sce/gs_sync.h"
// Actual generated ABI extracted from the common TU; no game body is linked.
#include "generated_stack.h"
#include "games/slus_20909_crash_twinsanity/recomp/continuations.h"

#if PS2R_PRODUCT_BOUNDARY_UI_TESTS
#include <SDL.h>
#include "app/host/session_runtime.h"
#endif

std::map<uint32_t, DispatchTable::Body> recompiled_functions;
extern const uint32_t g_recomp_fn_count = 1;
extern const uint32_t g_recomp_fn_addrs[] = {0x100008};
bool g_recomp_fn_hooked[] = {true};
extern "C" { bool crashring_enabled = false; }
void initialize_recompiled_functions() {}
// Registry population is not part of this fixture; the real SyncV adapter is.
void SdkBindingManifest::Register(const std::string&, const std::string&, Handler) {
    throw std::logic_error("fixture must not install SDK bindings");
}
uint32_t recomp_fn_id_for(uint32_t fn) { return fn == 0x100008 ? 0 : UINT32_MAX; }
bool update_function_impl(uint32_t fn, const DispatchTable::Body& body) {
    auto it = recompiled_functions.find(fn);
    if (it == recompiled_functions.end()) return false;
    it->second = DispatchTable::ObserveInstall(fn, body);
    return true;
}

namespace {
int checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr); } } while (false)

void InstalledProof() {
    constexpr uint32_t fn = 0x100008;
    int hits = 0;
    uint32_t entered = 0;
    const auto original = DispatchTable::GeneratedBody(fn,
        [&](CpuContext&, uint32_t entry) { ++hits; entered = entry; }, recomp_continuations(fn));
    recompiled_functions[fn] = original;
    CpuContext ctx{};
    CHECK(DispatchTable::IsHooked(fn)); // Classic all-hooked says nothing.
    CHECK(DispatchTable::CanResumeAt(fn, 0));
    CHECK(DispatchTable::CanResumeAt(fn, fn));
    CHECK(DispatchTable::CanResumeAt(fn, 0x1000b8));
    CHECK(!DispatchTable::CanResumeAt(fn, 0x1000bc)); // Contained but no case.
    CHECK(!DispatchTable::CanResumeAt(fn, 0x1000b9));
    CHECK(!DispatchTable::CanResumeAt(fn + 4, fn));
    CHECK(DispatchTable::InvokeAt(fn, 0x1000b8, ctx) && hits == 1 && entered == 0x1000b8);
    CHECK(!DispatchTable::InvokeAt(fn, 0x1000bc, ctx) && hits == 1);
    CHECK(DispatchTable::Install(fn, [&](CpuContext&, uint32_t) { ++hits; }));
    CHECK(!DispatchTable::CanResumeAt(fn, 0x1000b8));
    CHECK(!DispatchTable::InvokeAt(fn, 0, ctx) && hits == 1);
    CHECK(update_function_impl(fn, original)); // Exact captured body restores proof.
    CHECK(DispatchTable::CanResumeAt(fn, 0x1000b8));
    CHECK(update_function_impl(fn, [original](CpuContext& state, uint32_t entry) { original(state, entry); }));
    CHECK(!DispatchTable::CanResumeAt(fn, 0x1000b8)); // A wrapper needs independent proof.
    recompiled_functions[fn] = [](CpuContext&, uint32_t) {};
    CHECK(!DispatchTable::CanResumeAt(fn, fn)); // No stale address registry.
    recompiled_functions[fn] = DispatchTable::TraceBody(fn, original,
        [original](CpuContext& state, uint32_t entry) {
            EeCallFrameGuard frame(0x100008, state.cpuRegs.GPR.r[31].UL[0]);
            original(state, entry);
        });
    CHECK(DispatchTable::CanResumeAt(fn, 0x1000b8));
    recompiled_functions[fn + 4] = original; // Proof for a different callable address.
    CHECK(!DispatchTable::CanResumeAt(fn + 4, fn));
    for (const auto& pair : std::vector<std::pair<uint32_t, int>>{
             {0x2cf200, 1}, {0x2cf320, 2}, {0x2d65c8, 3}}) {
        recompiled_functions[pair.first] = DispatchTable::GeneratedBody(pair.first,
            [](CpuContext&, uint32_t) {}, recomp_continuations(pair.first));
        CHECK(DispatchTable::CanRestartWait(pair.first, pair.second));
        CHECK(!DispatchTable::CanRestartWait(pair.first, pair.second + 1));
        update_function_impl(pair.first, [](CpuContext&, uint32_t) {});
        CHECK(!DispatchTable::CanRestartWait(pair.first, pair.second));
    }
    CHECK(!DispatchTable::CanRestartWait(fn, 1));
    CHECK(!DispatchTable::CanRestartWait(fn, 0));
}

void SaturationAndOwners() {
    CHECK(ShadowCallStack::Complete());
    {
        std::vector<std::unique_ptr<EeCallFrameGuard>> guards;
        for (unsigned i = 0; i < 128; ++i)
            guards.push_back(std::make_unique<EeCallFrameGuard>(0x1000 + 4 * i, 0));
        CHECK(ShadowCallStack::Depth() == 128 && ShadowCallStack::Complete());
        {
            EeCallFrameGuard overflow(0x9998, 0), overflow_again(0x999c, 0);
            CHECK(ShadowCallStack::Depth() == 128 && !ShadowCallStack::Complete());
        }
        guards.pop_back();
        CHECK(ShadowCallStack::Depth() == 127 && !ShadowCallStack::Complete());
        bool foreign_complete = false;
        std::thread other([&] { foreign_complete = ShadowCallStack::Complete(); });
        other.join();
        CHECK(foreign_complete); // Loss belongs to this owner, not all workers.
        while (!guards.empty()) guards.pop_back();
    }
    CHECK(ShadowCallStack::Depth() == 0 && ShadowCallStack::Complete());
    for (unsigned i = 0; i < 128; ++i) CHECK(ShadowCallStack::Push(0x1000, 0));
    CHECK(!ShadowCallStack::Push(0x2000, 0)); // Legacy failed-push caller skips pop.
    ShadowCallStack::Pop();
    CHECK(!ShadowCallStack::Complete());
    while (ShadowCallStack::Depth()) ShadowCallStack::Pop();
    CHECK(ShadowCallStack::Complete());
}

void PostReturnAndOpaqueFrames() {
    CpuContext ctx{}, other{};
    int notified = 0;
    CHECK(!DispatchTable::AfterCurrentReturn(ctx, [](CpuContext&) {}));
    recompiled_functions[0x2200] = DispatchTable::ObserveInstall(0x2200,
        [&](CpuContext& state, uint32_t) {
            CHECK(!ShadowCallStack::Complete());
            CHECK(!DispatchTable::AfterCurrentReturn(other, [](CpuContext&) {}));
            CHECK(DispatchTable::AfterCurrentReturn(state, [&](CpuContext& completed) {
                ++notified;
                CHECK(completed.cpuRegs.GPR.r[2].UL[0] == 42);
                CHECK(ShadowCallStack::Complete());
                CHECK(ShadowCallStack::Depth() == 1);
            }));
            CHECK(!DispatchTable::AfterCurrentReturn(state, [](CpuContext&) {}));
            state.cpuRegs.GPR.r[2].UL[0] = 42; // Wrapper write AFTER service notification.
        });
    {
        EeCallFrameGuard caller(0x1100, 0);
        DispatchTable::Invoke(0x2200, ctx);
        CHECK(notified == 1);
    }
    recompiled_functions[0x3300] = DispatchTable::ObserveInstall(0x3300,
        [&](CpuContext& state, uint32_t) {
            CHECK(DispatchTable::AfterCurrentReturn(state, [&](CpuContext&) { ++notified; }));
            throw std::runtime_error("load unwind");
        });
    try { DispatchTable::Invoke(0x3300, ctx); } catch (const std::runtime_error&) {}
    CHECK(notified == 1 && ShadowCallStack::Complete());
    recompiled_functions[0x4400] = DispatchTable::ObserveInstall(0x4400,
        [&](CpuContext& state, uint32_t) {
            CHECK(DispatchTable::AfterCurrentReturn(state, [&](CpuContext&) {
                CHECK(!ShadowCallStack::Complete()); // Unknown outer wrapper still live.
                ++notified;
            }));
        });
    recompiled_functions[0x5500] = DispatchTable::ObserveInstall(0x5500,
        [](CpuContext& state, uint32_t) { DispatchTable::Invoke(0x4400, state); });
    DispatchTable::Invoke(0x5500, ctx);
    CHECK(notified == 2 && ShadowCallStack::Complete());
}

void BindFixture(uint32_t fn, DispatchTable::Body body) {
    auto original = DispatchTable::GeneratedBody(fn, std::move(body), recomp_continuations(fn));
    recompiled_functions[fn] = DispatchTable::TraceBody(fn, original,
        [fn, original](CpuContext& ctx, uint32_t entry) {
            EeCallFrameGuard guard(fn, ctx.cpuRegs.GPR.r[31].UL[0]);
            original(ctx, entry);
        });
}

void StructuralInteriorProofs() {
    constexpr uint32_t fn = 0x100008, interior = 0x1000b8;
    int full_calls = 0, tails = 0, replacements = 0;
    bool complete_in_body = false;
    BindFixture(fn, [&](CpuContext& ctx, uint32_t entry) {
        complete_in_body = ShadowCallStack::Complete();
        if (!entry || entry == fn) {
            ++full_calls;
            ctx.cpuRegs.GPR.r[2].UL[0] = 100;
        } else {
            CHECK(entry == interior);
            ++tails;
            ctx.cpuRegs.GPR.r[2].UL[0] += 7;
        }
    });
    const auto original = DispatchTable::Capture(fn);
    const auto delegated = DispatchTable::DelegateInterior(fn, original,
        [&](CpuContext& ctx, uint32_t entry) {
            ++replacements;
            CHECK(!ShadowCallStack::Complete());
            original(ctx, entry); // Even this fallback has an unrepresented outer frame.
        });
    CHECK(DispatchTable::Install(fn, delegated));
    CHECK(DispatchTable::CanResumeAt(fn, interior));
    CHECK(!DispatchTable::CanResumeAt(fn, 0) && !DispatchTable::CanResumeAt(fn, fn));
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[2].UL[0] = 10;
    CHECK(DispatchTable::InvokeAt(fn, interior, ctx));
    CHECK(tails == 1 && full_calls == 0 && replacements == 0 && complete_in_body);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 17); // Resume tail, never replay entry work.
    CHECK(!DispatchTable::InvokeAt(fn, 0x1000bc, ctx) && tails == 1); // Containment is insufficient.
    CHECK(!DispatchTable::InvokeAt(fn, 0, ctx) && replacements == 0);
    DispatchTable::Invoke(fn, ctx); // Normal full-entry calls remain callable.
    CHECK(full_calls == 1 && replacements == 1 && !complete_in_body);
    CHECK(ShadowCallStack::Complete() && ShadowCallStack::Depth() == 0);
    const auto counted = DispatchTable::CountCalls(fn, delegated,
        std::make_shared<std::atomic<uint64_t>>(0));
    recompiled_functions[fn] = DispatchTable::TraceBody(fn, counted,
        [counted](CpuContext& state, uint32_t entry) { counted(state, entry); });
    CHECK(DispatchTable::CanResumeAt(fn, interior) && !DispatchTable::CanResumeAt(fn, fn));
    CHECK(!DispatchTable::CanRestartWait(fn, 1)); // Neither adapter nor trace widens proof.
    CHECK(DispatchTable::Install(fn, [delegated](CpuContext& state, uint32_t entry) {
        delegated(state, entry);
    }));
    CHECK(!DispatchTable::CanResumeAt(fn, interior)); // Arbitrary outer wrapper revokes it.
    const auto opaque = DispatchTable::Capture(fn);
    CHECK(DispatchTable::Install(fn,
        DispatchTable::DelegateInterior(fn, opaque, [](CpuContext&, uint32_t) {})));
    CHECK(!DispatchTable::CanResumeAt(fn, interior)); // Cannot launder a captured opaque layer.
    CHECK(DispatchTable::Install(fn,
        DispatchTable::DelegateInterior(fn + 4, original, [](CpuContext&, uint32_t) {})));
    CHECK(!DispatchTable::CanResumeAt(fn, interior)); // Identity is the exact function.
    CHECK(DispatchTable::Install(fn, delegated));
    CHECK(DispatchTable::CanResumeAt(fn, interior) && !DispatchTable::CanResumeAt(fn, 0));
    CHECK(!DispatchTable::DelegateInterior(fn, {}, original));
    CHECK(!DispatchTable::DelegateInterior(fn, original, {}));
    recompiled_functions.erase(fn);
}

void DiagnosticCounterProofs() {
    constexpr uint32_t fn = 0x100008;
    int entered = 0;
    const auto original = DispatchTable::GeneratedBody(fn,
        [&](CpuContext& ctx, uint32_t entry) { ++entered; ctx.cpuRegs.pc = entry; },
        recomp_continuations(fn));
    using Counters = std::array<std::atomic<uint64_t>, 2>;
    std::weak_ptr<Counters> lifetime;
    {
        auto cells = std::make_shared<Counters>();
        (*cells)[0].store(0);
        (*cells)[1].store(0);
        lifetime = cells;
        auto enabled = std::make_shared<std::atomic<bool>>(false);
        auto each = std::shared_ptr<std::atomic<uint64_t>>(cells, &(*cells)[0]);
        auto address = std::shared_ptr<std::atomic<uint64_t>>(cells, &(*cells)[1]);
        auto counted = DispatchTable::CountCalls(fn, original, each,
            DispatchTable::CountMode::EveryCall, enabled);
        counted = DispatchTable::CountCalls(fn, std::move(counted), address,
            DispatchTable::CountMode::FunctionAddress);
        recompiled_functions[fn] = counted;
        CHECK(DispatchTable::CanResumeAt(fn, 0) && DispatchTable::CanResumeAt(fn, 0x1000b8));
        CpuContext ctx{};
        CHECK(DispatchTable::InvokeAt(fn, 0x1000b8, ctx) && ctx.cpuRegs.pc == 0x1000b8);
        CHECK(entered == 1 && each->load() == 0 && address->load() == 0);
        enabled->store(true);
        CHECK(DispatchTable::InvokeAt(fn, 0, ctx) && ctx.cpuRegs.pc == 0);
        CHECK(each->load() == 1 && address->load() == 0); // SDK zero is not its full address.
        DispatchTable::Invoke(fn, ctx);
        CHECK(entered == 3 && each->load() == 2 && address->load() == 1);
        enabled->store(false); // Inactive/rollback wrapper still owns and executes its original.
        CHECK(DispatchTable::InvokeAt(fn, 0x1000b8, ctx));
        CHECK(entered == 4 && each->load() == 2 && address->load() == 1);
        CHECK(!DispatchTable::CountCalls(fn, original, {}));
        const auto wrong = DispatchTable::CountCalls(fn + 4, original, each);
        recompiled_functions[fn + 4] = wrong;
        CHECK(!DispatchTable::CanResumeAt(fn + 4, fn + 4));
        recompiled_functions.erase(fn + 4);
    }
    CHECK(!lifetime.expired()); // Installed callbacks own aliased counters and their enable flag.
    recompiled_functions.erase(fn);
    CHECK(lifetime.expired());
    recompiled_functions[fn] = DispatchTable::CountCalls(fn,
        [original](CpuContext& ctx, uint32_t entry) { original(ctx, entry); },
        std::make_shared<std::atomic<uint64_t>>(0));
    CHECK(!DispatchTable::CanResumeAt(fn, 0x1000b8)); // Counter cannot recover erased identity.
    recompiled_functions.erase(fn);
}

void AdapterWaitProofs() {
    for (const auto& [fn, wait] : std::array<std::pair<uint32_t, int>, 3>{{
             {0x2cf200, 1}, {0x2cf320, 2}, {0x2d65c8, 3}}}) {
        const auto original = DispatchTable::GeneratedBody(fn,
            [](CpuContext&, uint32_t) {}, recomp_continuations(fn));
        recompiled_functions[fn] = original;
        CHECK(DispatchTable::CanRestartWait(fn, wait));
        CHECK(DispatchTable::Install(fn, DispatchTable::DelegateInterior(fn, original,
            [](CpuContext&, uint32_t) {})));
        CHECK(!DispatchTable::CanResumeAt(fn, 0) && !DispatchTable::CanRestartWait(fn, wait));
        const auto counted = DispatchTable::CountCalls(fn, original,
            std::make_shared<std::atomic<uint64_t>>(0));
        CHECK(DispatchTable::Install(fn, counted));
        CHECK(DispatchTable::CanResumeAt(fn, 0) && !DispatchTable::CanRestartWait(fn, wait));
        recompiled_functions[fn] = DispatchTable::TraceBody(fn, counted,
            [counted](CpuContext& ctx, uint32_t entry) { counted(ctx, entry); });
        CHECK(!DispatchTable::CanRestartWait(fn, wait)); // No proof reappears through tracing.
        CHECK(DispatchTable::Install(fn, original) && DispatchTable::CanRestartWait(fn, wait));
        recompiled_functions.erase(fn);
    }
}

void GameBoundaryCaptureAndRestart() {
    using Phase = ps2r::app::SessionControls::Phase;
    auto& threads = ThreadManager::Instance();
    auto& token = EeRunToken::Instance();
    threads.ResetForTest();
    token.ResetForTest();
    threads.SetStackBytes(2u << 20);
    BoundarySequencer sequence;
    EmuDomain domain;
    DerivedClocks clocks(domain);
    uint64_t raw = 0;
    sequence.SetStep(BoundarySequencer::Step::TickAdvance, [&](CpuContext&) {
        domain.OnRawTick(++raw);
        domain.OnBoundary(raw);
        return true;
    });
    GsSync::Bind(&sequence, &clocks);
    GameBoundary boundary(threads, sequence);
    CHECK(boundary.Attached());
    CHECK(!sequence.AttachCompletedService(&domain, [](CpuContext&, uint64_t) {}));
    ThreadSection section(threads);
    ps2r::savestate::SectionRegistry registry;
    section.Register(registry);
    bool movie_quiescent = false;
    ps2r::app::SessionCheckpoint checkpoint(registry, {
        [&] { return token.CanCaptureCurrentThread(); },
        [&](std::string& reason) {
            if (!movie_quiescent) reason = "Movie still owns active buffers.";
            return movie_quiescent;
        },
        [&](auto& records, std::string& error) { return boundary.FreshThreads(records, error); },
        DispatchTable::CanResumeAt
    });
    char directory[] = "/tmp/product_boundary_XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fresh.ps2st";
    ThreadCapture::Records fresh, restored;
    std::string error;
    CHECK(!boundary.FreshThreads(fresh, error));
    int completed_callbacks = 0, caller_tail = 0, root_tail = 0;
    bool resumed = false;
    bool reset_before_resume = false;
    boundary.SetConsumer([&](uint64_t frame) {
        ++completed_callbacks;
        CHECK(sequence.InFlight() == 0 && sequence.Completed() >= frame);
        CHECK(g_vsync_frame.load() == frame && ShadowCallStack::Complete());
        CHECK(boundary.FreshThreads(fresh, error));
        CHECK(fresh.size() == 1 && fresh[0].chain.size() == 2);
        CHECK(fresh[0].ctx.cpuRegs.GPR.r[2].UL[0] == 0xfeed); // After-wrapper write.
        CHECK(fresh[0].ctx.cpuRegs.pc == 0x1a01a4);
        CHECK(fresh[0].chain[0].func_addr == 0x1a0150 && fresh[0].chain[1].func_addr == 0x100008);
        CHECK(fresh[0].chain.back().ra == 0);
        bool foreign_refused = false;
        std::thread other([&] {
            ThreadCapture::Records output;
            std::string reason;
            foreign_refused = !boundary.FreshThreads(output, reason) && output.empty();
        });
        other.join();
        CHECK(foreign_refused);
        if (!resumed) {
            const auto waiting = checkpoint.Capture(1, path, {});
            CHECK(waiting.phase == Phase::Waiting && waiting.detail == "Movie still owns active buffers.");
            CHECK(!std::filesystem::exists(path) && !checkpoint.Busy());
            movie_quiescent = true; // The root owner closes/releases movie buffers.
            CHECK(checkpoint.Capture(2, path, {}).phase == Phase::Working);
        } else {
            CHECK(reset_before_resume);
            CHECK(fresh[0].chain[1].ra == restored[0].chain[1].ra); // Deferred outer tail.
        }
    });
    BindFixture(0x100008, [&](CpuContext& ctx, uint32_t entry) {
        if (!entry || entry == 0x100008) {
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1000d8;
            DispatchTable::Invoke(0x1a0150, ctx);
        }
        ++root_tail;
    });
    BindFixture(0x1a0150, [&](CpuContext& ctx, uint32_t) {
        // A second boundary during relaunch proves deferred logical callers
        // remain capturable even though only this frame is on the host stack.
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1a01a4;
        DispatchTable::Invoke(0x2c1d78, ctx);
        ++caller_tail; // OLD arena-flip pin: never skip the innermost caller.
    });
    recompiled_functions[0x2c1d78] = DispatchTable::ObserveInstall(0x2c1d78,
        [](CpuContext& ctx, uint32_t) {
            GsSync::SyncV(ctx);
            ctx.cpuRegs.GPR.r[2].UL[0] = 0xfeed;
            ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];
        });
    CpuContext ctx{};
    auto info = threads.AdoptCurrentThread(1, 64);
    info->ctx = &ctx;
    token.GuestEnter(1, 64);
    DispatchTable::Invoke(0x100008, ctx);
    CHECK(completed_callbacks == 1 && caller_tail == 1 && root_tail == 1);
    CHECK(!boundary.FreshThreads(fresh, error) && fresh.empty()); // Scope expired.
    CHECK(sequence.RejectedServiceReturns() == 0);
    checkpoint.Join();
    const auto published = checkpoint.Poll();
    CHECK(published && published->phase == Phase::Succeeded);
    ps2r::savestate::FileMeta meta;
    std::vector<ps2r::savestate::Blob> blobs;
    CHECK(ps2r::savestate::ReadStateFile(path, &meta, &blobs, &error));
    CHECK(registry.Validate(blobs, &error));
    CHECK(ThreadSection::Parse(blobs[0].payload, restored, &error));
    CHECK(threads.ReleaseCurrentThread(&error));
    threads.TerminateAll();
    threads.JoinAll();
    CHECK(threads.RestartAfterJoin(&error));
    CHECK(registry.Restore(blobs, &error));
    boundary.ResetAfterRestore();
    // Root's actual GameLibraries/Input reset remains root-owned.
    reset_before_resume = true;
    resumed = true;
    int replacement_entries = 0;
    const auto resumed_calls = std::make_shared<std::atomic<uint64_t>>(0);
    const auto caller = DispatchTable::CountCalls(0x1a0150,
        DispatchTable::Capture(0x1a0150), resumed_calls);
    CHECK(DispatchTable::Install(0x1a0150, DispatchTable::DelegateInterior(0x1a0150, caller,
        [&](CpuContext&, uint32_t) { ++replacement_entries; })));
    CHECK(!DispatchTable::CanResumeAt(0x1a0150, 0));
    ThreadRelauncher relaunch(threads, DispatchTable::InvokeAt);
    CHECK(relaunch.Relaunch(restored, &error) == 1);
    threads.JoinAll();
    CHECK(completed_callbacks == 2 && caller_tail == 2 && root_tail == 2);
    CHECK(resumed_calls->load() == 1 && replacement_entries == 0);
    CHECK(fresh[0].chain.size() == 2 && fresh[0].has_ctx);
    threads.TerminateAll();
    CHECK(threads.RestartAfterJoin(&error));
    GsSync::ResetForTest();
    std::filesystem::remove_all(directory);
}

void CompletionFailures() {
    BoundarySequencer sequence;
    CpuContext ctx{}, other{};
    int delivered = 0;
    CHECK(sequence.AttachCompletedService(&sequence,
        [&](CpuContext&, uint64_t) { ++delivered; }, DispatchTable::AfterCurrentReturn));
    sequence.SetStep(BoundarySequencer::Step::Verify, [](CpuContext&) { return false; });
    CHECK(!sequence.RunBoundary(ctx) && sequence.Stopped() == 1);
    CHECK(!sequence.CompleteServiceReturn(ctx, 1) && !sequence.DeferServiceReturn(ctx, 1));
    sequence.SetStep(BoundarySequencer::Step::Verify, [](CpuContext&) -> bool { throw 7; });
    try { sequence.RunBoundary(ctx); } catch (int) {}
    CHECK(!sequence.CompleteServiceReturn(ctx, 1) && sequence.InFlight() == 0);
    sequence.SetStep(BoundarySequencer::Step::Verify, {});
    CHECK(sequence.RunBoundary(ctx));
    CHECK(!sequence.CompleteServiceReturn(other, 1));
    CHECK(sequence.CompleteServiceReturn(ctx, 1));
    CHECK(!sequence.CompleteServiceReturn(ctx, 1) && delivered == 1);
    recompiled_functions[0x6600] = DispatchTable::ObserveInstall(0x6600,
        [&](CpuContext& state, uint32_t) {
            CHECK(sequence.RunBoundary(state));
            CHECK(sequence.DeferServiceReturn(state, 2));
            sequence.ResetServiceCompletion(); // Restore invalidates queued old return.
        });
    DispatchTable::Invoke(0x6600, ctx);
    CHECK(delivered == 1);
    sequence.DetachCompletedService(&sequence);
}

void CompletionConsumerUnwind() {
    auto& threads = ThreadManager::Instance();
    auto& token = EeRunToken::Instance();
    threads.ResetForTest();
    token.ResetForTest();
    BoundarySequencer sequence;
    GameBoundary boundary(threads, sequence);
    CHECK(boundary.Attached());
    ThreadCapture::Records fresh;
    std::string error;
    int delivered = 0, caller_tail = 0;
    boundary.SetConsumer([&](uint64_t frame) {
        ++delivered;
        CHECK(frame == 1 && sequence.InFlight() == 0);
        CHECK(boundary.FreshThreads(fresh, error) && fresh.size() == 1);
        // Root must translate SessionRuntime::Boundary(false) into its normal
        // unwind. A void consumer silently dropping false would run this tail.
        throw ThreadExitUnwind{};
    });
    BindFixture(0x100008, [&](CpuContext& ctx, uint32_t) {
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1000d8;
        DispatchTable::Invoke(0x7700, ctx);
        ++caller_tail;
    });
    recompiled_functions[0x7700] = DispatchTable::ObserveInstall(0x7700,
        [&](CpuContext& ctx, uint32_t) {
            CHECK(sequence.RunBoundary(ctx));
            CHECK(sequence.DeferServiceReturn(ctx, 1));
        });
    CpuContext ctx{};
    auto info = threads.AdoptCurrentThread(1, 64);
    info->ctx = &ctx;
    token.GuestEnter(1, 64);
    bool unwound = false;
    try { DispatchTable::Invoke(0x100008, ctx); }
    catch (const ThreadExitUnwind&) { unwound = true; }
    CHECK(unwound && delivered == 1 && caller_tail == 0);
    CHECK(ShadowCallStack::Depth() == 0 && ShadowCallStack::Complete());
    CHECK(!DispatchTable::AfterCurrentReturn(ctx, [](CpuContext&) {}));
    CHECK(!boundary.FreshThreads(fresh, error) && fresh.empty());
    CHECK(!sequence.CompleteServiceReturn(ctx, 1) && delivered == 1);
    CHECK(threads.ReleaseCurrentThread(&error));
    threads.TerminateAll();
    threads.JoinAll();
    CHECK(threads.RestartAfterJoin(&error));
}
#if PS2R_PRODUCT_BOUNDARY_UI_TESTS
void SessionBoundaryCapture() {
    using Controls = ps2r::app::SessionControls;
    using Runtime = ps2r::app::SessionRuntime;
    using Action = Controls::Action;
    using Phase = Controls::Phase;
    using namespace ps2r::savestate;
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    char directory[] = "/tmp/product_session_boundary_XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/fresh.ps2st";
    {
        auto& threads = ThreadManager::Instance();
        auto& token = EeRunToken::Instance();
        threads.ResetForTest();
        token.ResetForTest();
        threads.SetStackBytes(2u << 20);
        BoundarySequencer sequence;
        GameBoundary boundary(threads, sequence);
        CHECK(boundary.Attached());
        ThreadSection section(threads);
        SectionRegistry registry;
        section.Register(registry);
        // The second owner represents mutable game data. Restoring must replay
        // the innermost post-call work once against these captured bytes.
        uint8_t caller_tail = 0, root_tail = 0;
        SectionSpec data;
        data.tag = Tag('T', 'E', 'S', 'T');
        data.owner = "Session fixture game data";
        data.serialize = [&](std::vector<uint8_t>& out) { out = {caller_tail, root_tail}; };
        data.validate = [](const Blob& blob, std::string*) { return blob.payload.size() == 2; };
        data.restore = [&](const Blob& blob, std::string*) {
            caller_tail = blob.payload[0];
            root_tail = blob.payload[1];
            return true;
        };
        CHECK(registry.Register(data));
        Runtime runtime(nullptr);
        std::string error;
        CHECK(runtime.Open(100, false, false, {}, error));
        if (!error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return;
        }
        const auto main_thread = std::this_thread::get_id();
        std::atomic<bool> movie_closed{false}, allow_publication{false};
        std::atomic<uint64_t> admitted{0};
        std::atomic<Uint32> window_id{0};
        uint64_t saved_frame = 0;
        uint8_t saved_caller_tail = 0, saved_root_tail = 0;
        bool owner_callbacks = true, writer_joined = false;
        int captures = 0;
        ps2r::app::SessionCheckpoint checkpoint(registry, {
            [&] { return token.CanCaptureCurrentThread(); },
            [&](std::string& reason) {
                if (!movie_closed.load()) reason = "Movie still owns active buffers.";
                return movie_closed.load();
            },
            [&](auto& records, std::string& reason) { return boundary.FreshThreads(records, reason); },
            DispatchTable::CanResumeAt
        });
        Runtime::Hooks hooks;
        hooks.stop = [&] { threads.TerminateAll(); };
        hooks.pause = [&](bool) { owner_callbacks &= token.CanCaptureCurrentThread(); };
        hooks.capture = [&](const Controls::Request& request, uint64_t frame) {
            ++captures;
            owner_callbacks &= token.CanCaptureCurrentThread() && sequence.InFlight() == 0;
            FileMeta meta;
            meta.vsync_frame = static_cast<uint32_t>(frame);
            const auto result = checkpoint.Capture(request.id, path, meta);
            if (result.phase == Phase::Working) {
                saved_frame = frame;
                saved_caller_tail = caller_tail;
                saved_root_tail = root_tail;
                admitted.store(request.id);
            }
            return Runtime::CaptureResult{result.phase, result.detail};
        };
        hooks.poll_capture = [&]() -> std::optional<Runtime::CaptureCompletion> {
            // This fixture delays reporting to separate admission from success.
            if (!allow_publication.load()) return {};
            if (std::this_thread::get_id() != main_thread) std::abort();
            const auto result = checkpoint.Poll();
            if (!result) return {};
            return Runtime::CaptureCompletion{
                result->request, {result->phase, result->detail}, result->path};
        };
        hooks.stopped = [&] { checkpoint.Join(); threads.JoinAll(); writer_joined = true; };
        runtime.SetHooks(std::move(hooks));
        boundary.SetConsumer([&](uint64_t frame) {
            if (!runtime.Boundary(frame)) throw ThreadExitUnwind{};
        });
        uint64_t service_frame = 10405;
        BindFixture(0x100008, [&](CpuContext& ctx, uint32_t entry) {
            if (entry && entry != 0x100008) {
                ++root_tail; // Deferred outer continuation during relaunch.
                return;
            }
            while (!runtime.Closing()) {
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1000d8;
                DispatchTable::Invoke(0x1a0150, ctx);
                ++root_tail;
            }
        });
        BindFixture(0x1a0150, [&](CpuContext& ctx, uint32_t entry) {
            if (!entry || entry == 0x1a0150) {
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1a01a4;
                DispatchTable::Invoke(0x2c1d78, ctx);
            }
            ++caller_tail; // OLD E2 arena flip: cannot skip this saved caller.
        });
        recompiled_functions[0x2c1d78] = DispatchTable::ObserveInstall(0x2c1d78,
            [&](CpuContext& ctx, uint32_t) {
                if (!sequence.RunBoundary(ctx) || !sequence.DeferServiceReturn(ctx, ++service_frame))
                    std::abort();
                // Both final writes must precede the owned completion callback.
                ctx.cpuRegs.GPR.r[2].UL[0] = 0xfeed;
                ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];
            });
        auto& controls = runtime.Controls();
        const auto pause_id = controls.Submit(Action::TogglePause);
        auto completed = [&](uint64_t id, Phase phase) {
            for (const auto& result : controls.Read().recent)
                if (result.request.id == id) return result.phase == phase;
            return false;
        };
        auto await = [](const std::function<bool()>& ready) {
            const auto deadline = ps2r::Park::NowNs() + 3000000000ull;
            while (!ready()) {
                if (ps2r::Park::NowNs() >= deadline) return false;
                SDL_Delay(1);
            }
            return true;
        };
        auto key = [&](SDL_Keycode code) {
            SDL_Event event{};
            event.type = SDL_KEYDOWN;
            event.key.windowID = window_id.load();
            event.key.keysym.sym = code;
            return SDL_PushEvent(&event) == 1;
        };
        bool sequence_ok = true;
        uint64_t refused_id = 0;
        std::thread director([&] {
            auto require = [&](bool ok) {
                if (!ok) { sequence_ok = false; runtime.RequestClose(); }
                return ok;
            };
            if (!require(await([&] {
                return completed(pause_id, Phase::Succeeded) && controls.Read().runtime.paused;
            }))) return;
            const auto paused_frame = controls.Read().runtime.frame;
            if (!require(key(SDLK_F5)) || !require(await([&] {
                for (const auto& result : controls.Read().recent)
                    if (result.request.action == Action::SaveReproduction && result.phase == Phase::Refused) {
                        refused_id = result.request.id;
                        return result.message == "Movie still owns active buffers.";
                    }
                return false;
            }))) return;
            if (!require(!std::filesystem::exists(path))) return;
            movie_closed.store(true); // EOF alone never changes the owner gate.
            if (!require(key(SDLK_F5)) || !require(await([&] { return admitted.load() != 0; }))) return;
            const auto save_id = admitted.load();
            if (!require(!completed(save_id, Phase::Succeeded) &&
                         controls.Read().runtime.restore_path.empty())) return;
            bool main_serviced = false;
            if (!require(runtime.OnMain([&] { main_serviced = std::this_thread::get_id() == main_thread; }) &&
                         main_serviced)) return;
            allow_publication.store(true);
            if (!require(await([&] {
                return completed(save_id, Phase::Succeeded) && controls.Read().runtime.restore_path == path;
            }))) return;
            if (!require(controls.Read().runtime.paused && controls.Read().runtime.frame == paused_frame)) return;
            const auto restore = controls.Submit(Action::RestoreReproduction);
            if (!require(completed(restore, Phase::Refused) && !controls.Read().capabilities.restore_reproduction))
                return;
            if (!require(key(SDLK_F10)) || !require(await([&] {
                for (const auto& result : controls.Read().recent)
                    if (result.request.action == Action::Step && result.phase == Phase::Succeeded)
                        return controls.Read().runtime.frame == paused_frame + 1;
                return false;
            }))) return;
            SDL_Event close{};
            close.type = SDL_WINDOWEVENT;
            close.window.windowID = window_id.load();
            close.window.event = SDL_WINDOWEVENT_CLOSE;
            if (!require(SDL_PushEvent(&close) == 1)) return;
            require(await([&] { return runtime.Closing(); }));
        });
        bool unwound = false, released = false;
        const int run_result = runtime.Run([&] {
            CpuContext ctx{};
            auto info = threads.AdoptCurrentThread(1, 64);
            info->ctx = &ctx;
            token.GuestEnter(1, 64);
            // Process the queued pause before entering the first fixture body.
            runtime.OnMain([&] { window_id.store(SDL_GetWindowID(SDL_GL_GetCurrentWindow())); });
            try { DispatchTable::Invoke(0x100008, ctx); }
            catch (const ThreadExitUnwind&) { unwound = true; }
            released = threads.ReleaseCurrentThread(&error);
            return 0;
        }, 3);
        director.join();
        CHECK(run_result == 0 && sequence_ok && unwound && released && writer_joined && owner_callbacks);
        CHECK(captures == 2 && refused_id && completed(admitted.load(), Phase::Succeeded));
        CHECK(controls.Read().pending.empty() && !controls.Read().capabilities.restore_reproduction);
        CHECK(sequence.RejectedServiceReturns() == 0 && sequence.Completed() == 2);
        CHECK(caller_tail == saved_caller_tail + 1 && root_tail == saved_root_tail + 1);
        FileMeta meta;
        std::vector<Blob> blobs;
        CHECK(ReadStateFile(path, &meta, &blobs, &error) && registry.Validate(blobs, &error));
        CHECK(meta.vsync_frame == saved_frame && saved_frame == 10406);
        ThreadCapture::Records records;
        for (const auto& blob : blobs)
            if (blob.tag == TAG_THRD) CHECK(ThreadSection::Parse(blob.payload, records, &error));
        CHECK(records.size() == 1 && records[0].chain.size() == 2);
        if (records.size() == 1 && records[0].chain.size() == 2) {
            CHECK(records[0].ctx.cpuRegs.GPR.r[2].UL[0] == 0xfeed &&
                  records[0].ctx.cpuRegs.pc == 0x1a01a4);
            CHECK(records[0].chain[0].func_addr == 0x1a0150 &&
                  records[0].chain[1].func_addr == 0x100008 && records[0].chain.back().ra == 0);
            CHECK(threads.RestartAfterJoin(&error));
            CHECK(registry.Restore(blobs, &error));
            CHECK(caller_tail == saved_caller_tail && root_tail == saved_root_tail);
            boundary.ResetAfterRestore();
            ThreadRelauncher relaunch(threads, DispatchTable::InvokeAt);
            CHECK(relaunch.Relaunch(records, &error) == 1);
            threads.JoinAll();
            CHECK(caller_tail == saved_caller_tail + 1 && root_tail == saved_root_tail + 1);
            CHECK(sequence.Completed() == 2); // Resume never replays the saved service.
        }
        threads.TerminateAll();
        threads.JoinAll();
        CHECK(threads.RestartAfterJoin(&error));
        for (const auto fn : {0x100008u, 0x1a0150u, 0x2c1d78u}) recompiled_functions.erase(fn);
    }
    std::filesystem::remove_all(directory);
    SDL_Quit();
}
#endif

} // namespace

int main(int argc, char** argv) {
#if PS2R_PRODUCT_BOUNDARY_UI_TESTS
    if (argc == 2 && std::string(argv[1]) == "--session-boundary") {
        SessionBoundaryCapture();
        std::printf("%s SessionBoundaryCapture: %d checks\n", failures ? "FAIL" : "PASS", checks);
        return failures ? 1 : 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    InstalledProof();
    SaturationAndOwners();
    PostReturnAndOpaqueFrames();
    StructuralInteriorProofs();
    DiagnosticCounterProofs();
    AdapterWaitProofs();
    GameBoundaryCaptureAndRestart();
    CompletionFailures();
    CompletionConsumerUnwind();
    std::printf("%s ContinuationProofs: %d checks\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
