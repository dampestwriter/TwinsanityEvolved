// tests/iop/test_iopm_section.cpp -- IopmSection (ctest IopmSection): the IOPM
// codec over IopMemory + the kernel statics (32 threads / 64 semas). Pinned: the
// payload size and round trip (serialize -> validate -> parse -> re-serialize
// byte-identical); malformed payloads rejected (truncation, wrong RAM size,
// trailing garbage); cross-policy compatibility (a state mirrored from a FRAME
// kernel restores into a fresh FRAME kernel identically -- sema counts, sleep
// flags, the clock rebase -- and into a REALTIME kernel with the same statics);
// the SectionRegistry spec; and the frozen OLD anchor states as a read-only
// layout oracle (their IOPM sections parse and re-serialize byte-identical; SKIP
// when OLD is absent); and the boot-symmetric re-park against a real anchor: the
// plugin boots STREAM.IRX, the anchor's IOPM section restores through its deps, and
// the streaming worker the image records as started is parked on its semaphore
// with the image's token count (the title created it after module init; the loader
// never did -- the plugin's re-park must), and the heartbeat hard timer the image's
// descriptor records as armed beats after the restore (its first beat hands off to
// the parked worker through iSignalSema).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "engine/core/savestate/savestate_format.h"
#include "engine/core/savestate/section_registry.h"
#include "engine/iop/iop_module_table.h"
#include "engine/iop/savestate/iopm_section.h"
#include "engine/platform/host/platform.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"

using ps2r::iop::IopClock;
using ps2r::iop::IopContext;
using ps2r::iop::IopKernel;
using ps2r::iop::IopMemory;
using ps2r::iop::IopModuleTable;
using ps2r::iop::IopmSection;

static int g_failures = 0;
#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) std::printf("PASS: %s\n", name);               \
        else { std::printf("FAIL: %s (line %d)\n", name, __LINE__); g_failures++; } \
    } while (0)

static void FillPattern(IopMemory& ram, uint32_t seed) {
    uint8_t* p = ram.Data();
    uint32_t x = seed;
    for (size_t i = 0; i < ram.Size(); i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

static IopmSection::Statics SampleStatics() {
    IopmSection::Statics s{};
    s.ctx = IopContext::ForEntry(0x001FFC00, 0x13328);
    s.ctx.gpr[2] = 0xDEADBEEF;
    s.heap_ptr = 0x20C80;
    s.next_thread_id = 5;
    s.next_sema_id = 7;
    s.cd_callback_addr = 0x1234;
    s.sys_time = 987654321ull;
    for (int i = 1; i < 5; i++) {
        s.threads[i].entry = 0x1000 * i;
        s.threads[i].stack_size = 0x800;
        s.threads[i].priority = 20 + i;
        s.threads[i].arg = i;
        s.threads[i].started = true;
        s.threads[i].active = true;
        s.threads[i].sleeping = (i % 2) == 1;
    }
    s.threads[4].terminated = true;
    for (int i = 0; i < 6; i++) {
        s.semas[i].active = true;
        s.semas[i].count = i;
        s.semas[i].max_count = 255;
    }
    return s;
}

static bool StaticsEqual(const IopmSection::Statics& a, const IopmSection::Statics& b) {
    if (std::memcmp(&a.ctx, &b.ctx, sizeof(IopContext)) != 0) return false;
    if (a.heap_ptr != b.heap_ptr || a.next_thread_id != b.next_thread_id || a.next_sema_id != b.next_sema_id ||
        a.cd_callback_addr != b.cd_callback_addr || a.sys_time != b.sys_time)
        return false;
    for (int i = 0; i < IopmSection::kThreads; i++) {
        const auto& x = a.threads[i];
        const auto& y = b.threads[i];
        if (x.entry != y.entry || x.stack_size != y.stack_size || x.priority != y.priority || x.arg != y.arg ||
            x.active != y.active || x.started != y.started || x.sleeping != y.sleeping || x.terminated != y.terminated)
            return false;
    }
    for (int i = 0; i < IopmSection::kSemas; i++) {
        if (a.semas[i].count != b.semas[i].count || a.semas[i].max_count != b.semas[i].max_count ||
            a.semas[i].active != b.semas[i].active)
            return false;
    }
    return true;
}

static void TestRoundTrip() {
    IopMemory ram;
    FillPattern(ram, 42);
    const IopmSection::Statics s = SampleStatics();
    std::vector<uint8_t> payload;
    IopmSection::Serialize(payload, ram, s);
    std::printf("IOPM payload: %zu bytes (kPayloadBytes %zu)\n", payload.size(), IopmSection::kPayloadBytes);
    CHECK(payload.size() == IopmSection::kPayloadBytes, "payload size = the frozen layout");
    CHECK(payload.size() == 4 + 2097152 + 140 + 4 + 4 + 4 + 4 + 8 + 4 + 32 * 17 + 4 + 64 * 9, "layout arithmetic (32 threads x 17 B, 64 semas x 9 B)");
    CHECK(IopmSection::Validate(payload), "validate accepts the payload");
    IopMemory ram2;
    IopmSection::Statics s2{};
    CHECK(IopmSection::Parse(payload, ram2, s2), "parse succeeds");
    CHECK(std::memcmp(ram.Data(), ram2.Data(), IopMemory::kSize) == 0, "IOP RAM restored byte-identical");
    CHECK(StaticsEqual(s, s2), "statics restored field-identical");
    std::vector<uint8_t> again;
    IopmSection::Serialize(again, ram2, s2);
    CHECK(again == payload, "re-serialization is byte-identical");

    // Malformed payloads.
    std::vector<uint8_t> trunc(payload.begin(), payload.end() - 1);
    CHECK(!IopmSection::Validate(trunc), "truncated payload rejected");
    std::vector<uint8_t> garbage = payload;
    garbage.push_back(0);
    CHECK(!IopmSection::Validate(garbage), "trailing garbage rejected");
    std::vector<uint8_t> bad_size = payload;
    bad_size[0] ^= 1;
    CHECK(!IopmSection::Validate(bad_size), "wrong RAM size rejected");
    std::vector<uint8_t> bad_threads = payload;
    bad_threads[4 + IopMemory::kSize + 140 + 24] ^= 1;   // the kThreads word
    CHECK(!IopmSection::Validate(bad_threads), "wrong thread-table count rejected");
    IopMemory ram3;
    FillPattern(ram3, 7);
    IopmSection::Statics s3{};
    CHECK(!IopmSection::Parse(trunc, ram3, s3) && ram3.Data()[0] == [] { IopMemory r; FillPattern(r, 7); return r.Data()[0]; }(),
          "a failed parse writes nothing");
}

// Boot-symmetric kernel: 3 threads (1: heartbeat-like sleeper, 2: sema waiter, 3: sleeper), 2 semas.
static void BootKernel(IopKernel& k, std::vector<std::string>* fx) {
    k.RegisterSema(0, 0, 255);
    k.RegisterSema(1, 2, 255);
    for (int tid = 1; tid <= 3; tid++) k.RegisterThread(tid, 10 + tid);
    k.StartThread(1, [&k, fx] { for (;;) { k.SleepThread(); if (fx) fx->push_back("t1-woke"); } });
    k.StartThread(2, [&k, fx] { for (;;) { k.WaitSema(0); if (fx) fx->push_back("t2-sema"); } });
    k.StartThread(3, [&k, fx] { for (;;) { k.SleepThread(); if (fx) fx->push_back("t3-woke"); } });
    k.Dispatch();   // run each to its first park
}

static void FillThreadPods(IopmSection::Statics& s) {
    for (int tid = 1; tid <= 3; tid++) {
        s.threads[tid].entry = 0x2000 * tid;
        s.threads[tid].stack_size = 0x1000;
        s.threads[tid].priority = 10 + tid;
        s.threads[tid].started = true;
    }
    s.semas[0].active = true; s.semas[0].max_count = 255;
    s.semas[1].active = true; s.semas[1].max_count = 255;
}

static void TestCrossPolicy() {
    // Saver: FRAME kernel, some frames in; sema 0 signalled once (direct handoff to
    // the waiter, count stays 0), sema 1 signalled three times (no waiter: 2 -> 5).
    IopModuleTable a(IopClock::Policy::Frame);
    std::vector<std::string> fx_a;
    BootKernel(a.Kernel(), &fx_a);
    a.OnModuleLoaded(1);
    for (int i = 0; i < 10; i++) a.PumpAtBoundary();
    a.Kernel().SignalSema(0);
    for (int i = 0; i < 3; i++) a.Kernel().SignalSema(1);
    a.PumpAfterEeService();   // t2 runs once, re-parks on sema 0
    CHECK(fx_a.size() == 1 && fx_a[0] == "t2-sema" && a.Kernel().SemaCount(0) == 0 && a.Kernel().SemaCount(1) == 5,
          "saver kernel state as scripted");
    IopMemory ram_a;
    FillPattern(ram_a, 99);
    IopmSection::Statics st_a{};
    FillThreadPods(st_a);
    st_a.sys_time = 5000;
    IopmSection::MirrorKernel(a.Kernel(), st_a);
    CHECK(st_a.threads[1].active && st_a.threads[1].sleeping && st_a.threads[2].active && !st_a.threads[2].sleeping &&
              st_a.threads[3].sleeping && st_a.semas[0].count == 0 && st_a.semas[1].count == 5,
          "MirrorKernel: thread active/sleeping flags and sema counts from the live kernel");
    std::vector<uint8_t> payload;
    IopmSection::Serialize(payload, ram_a, st_a);

    // Loader 1: a fresh FRAME kernel, boot-symmetric (same threads parked at boot), restored at vf=600.
    IopModuleTable b(IopClock::Policy::Frame);
    std::vector<std::string> fx_b;
    BootKernel(b.Kernel(), &fx_b);
    b.OnModuleLoaded(1);
    IopMemory ram_b;
    IopmSection::Statics st_b{};
    CHECK(IopmSection::Parse(payload, ram_b, st_b), "FRAME -> FRAME: parse");
    IopmSection::ReconcileKernel(b.Kernel(), st_b, 600);
    CHECK(b.Kernel().SemaCount(0) == 0 && b.Kernel().SemaCount(1) == 5, "restored sema counts pushed into the kernel");
    CHECK(b.Kernel().Clock().NowUs() == 600ull * IopClock::kFrameUs, "FRAME clock rebased to restored_vf x quantum");
    b.PumpAfterEeService();
    CHECK(fx_b.empty() && b.Kernel().ThreadWaitKind(2) == IopKernel::WaitKind::Sema,
          "boot-symmetric parkees stay parked: a restored count of 0 releases no waiter");
    IopmSection::Statics st_b2{};
    FillThreadPods(st_b2);
    st_b2 = st_b;
    IopmSection::MirrorKernel(b.Kernel(), st_b2);
    std::vector<uint8_t> payload_b;
    IopmSection::Serialize(payload_b, ram_b, st_b2);
    CHECK(payload_b == payload, "FRAME -> FRAME: a re-mirrored, re-serialized state is byte-identical to the saved one");

    // Loader 2: a REALTIME kernel (never started) accepts the same payload with identical statics.
    IopModuleTable c(IopClock::Policy::Realtime);
    BootKernel(c.Kernel(), nullptr);
    IopMemory ram_c;
    IopmSection::Statics st_c{};
    CHECK(IopmSection::Parse(payload, ram_c, st_c) && StaticsEqual(st_c, st_b), "FRAME -> REALTIME: same statics (layout is policy-independent)");
    IopmSection::ReconcileKernel(c.Kernel(), st_c, 600);
    CHECK(c.Kernel().SemaCount(0) == 0 && c.Kernel().SemaCount(1) == 5, "REALTIME kernel reconciled (counts)");
    std::vector<uint8_t> payload_c;
    IopmSection::MirrorKernel(c.Kernel(), st_c);
    IopmSection::Serialize(payload_c, ram_c, st_c);
    CHECK(payload_c == payload, "REALTIME re-serialization equals the FRAME-saved bytes");
}

static void TestRegistrySpec() {
    IopMemory ram;
    FillPattern(ram, 3);
    IopmSection::Statics st = SampleStatics();
    st.threads[1].sleeping = false; st.threads[3].sleeping = false;   // no kernel here: PODs only
    ps2r::savestate::SectionRegistry reg;
    IopmSection::Deps deps;
    deps.ram = &ram;
    deps.statics = &st;
    deps.restored_vf = [] { return 0ull; };
    CHECK(reg.Register(IopmSection::MakeSpec(deps)), "spec registers");
    CHECK(reg.Has(ps2r::savestate::TAG_IOPM), "TAG_IOPM known");
    std::vector<ps2r::savestate::Blob> blobs = reg.Capture();
    CHECK(blobs.size() == 1 && blobs[0].tag == ps2r::savestate::TAG_IOPM && blobs[0].version == 1 &&
              blobs[0].payload.size() == IopmSection::kPayloadBytes,
          "Capture emits the IOPM v1 blob");
    std::string err;
    CHECK(reg.Validate(blobs, &err), "registry Validate passes");
    IopMemory ram2;
    IopmSection::Statics st2{};
    st2.sys_time = 1;   // older loader clock: the restored (larger) value wins
    ps2r::savestate::SectionRegistry reg2;
    IopmSection::Deps d2 = deps;
    d2.ram = &ram2;
    d2.statics = &st2;
    int after = 0;
    d2.after_restore = [&] { after++; };
    reg2.Register(IopmSection::MakeSpec(d2));
    CHECK(reg2.Restore(blobs, &err) && after == 1, "registry Restore applies the section and runs after_restore");
    CHECK(std::memcmp(ram.Data(), ram2.Data(), IopMemory::kSize) == 0 && StaticsEqual(st, st2), "restored RAM + statics equal the captured ones");
    // Optional section: absent -> Restore still succeeds (live values kept).
    std::vector<ps2r::savestate::Blob> none;
    CHECK(reg2.Restore(none, &err), "IOPM is optional: a file without it restores (live values kept)");
    // A bad version is rejected by validate.
    blobs[0].version = 2;
    CHECK(!reg2.Validate(blobs, &err), "unsupported IOPM version rejected");
}

static const char* const kAnchors[] = {
    "artdet_f37152.ps2st",
    "ps2state_slot0_f36935.ps2st",
    "ps2state_slot0_f39807.ps2st",
    "ps2state_slot0_f40391.ps2st",
    "ps2state_slot0_f44111.ps2st",
    "ps2state_slot0_f46203_red_crystal.ps2st",
    "ps2state_slot0_f46949_yellow_crystal.ps2st",
    "ps2state_slot0_f57292.ps2st",
    "ps2state_slot0_f59991.ps2st",
    "slowspot_f37092_s1789428268.ps2st",
    "slowspot_f40298_s1789693059.ps2st",
    "ps2state_slot0.ps2st",
};

static void TestOldAnchorsOracle(const std::string& og_root) {
    const std::string dir = og_root + "/save_states/";
    struct stat sb;
    if (og_root.empty() || stat((dir + kAnchors[0]).c_str(), &sb) != 0) {
        std::printf("anchor IOPM oracle: SKIP (OLD save_states not found under '%s')\n", og_root.c_str());
        return;
    }
    int ok = 0, total = 0;
    for (const char* name : kAnchors) {
        total++;
        ps2r::savestate::FileMeta meta;
        std::vector<ps2r::savestate::Blob> blobs;
        std::string err;
        if (!ps2r::savestate::ReadStateFile(dir + name, &meta, &blobs, &err)) {
            std::printf("  %s: read failed: %s\n", name, err.c_str());
            continue;
        }
        const ps2r::savestate::Blob* iopm = nullptr;
        for (const auto& b : blobs) if (b.tag == ps2r::savestate::TAG_IOPM) iopm = &b;
        if (!iopm) { std::printf("  %s: no IOPM section\n", name); continue; }
        IopMemory ram;
        IopmSection::Statics st{};
        const bool valid = IopmSection::Validate(iopm->payload);
        const bool parsed = valid && IopmSection::Parse(iopm->payload, ram, st);
        std::vector<uint8_t> again;
        if (parsed) IopmSection::Serialize(again, ram, st);
        const bool same = parsed && again == iopm->payload;
        std::printf("  %s: vf=%u IOPM %zu B valid=%d parsed=%d identical=%d heap_ptr=0x%x next_tid=%d next_sid=%d\n", name,
                    meta.vsync_frame, iopm->payload.size(), valid ? 1 : 0, parsed ? 1 : 0, same ? 1 : 0, st.heap_ptr,
                    st.next_thread_id, st.next_sema_id);
        if (same) ok++;
    }
    std::printf("anchor IOPM oracle: %d/%d identical\n", ok, total);
    CHECK(ok == total, "every OLD anchor's IOPM section parses and re-serializes byte-identical");
}

// The boot-symmetric re-park against a REAL anchor: the plugin boots STREAM.IRX to
// its first block (tid 1 only -- the streaming worker is created by the title's
// stream-open RPC, which the loader process never issues), then the anchor's IOPM
// section restores through the plugin's deps. The image records the worker (tid 2,
// entry 0x30d4, prio 40, started) parked on sema 0 with the image's token count; the
// restored kernel must show exactly that -- not a Dormant tid 2 with an unconsumed
// token. The kernel trace is quoted as the lifecycle evidence.
static const char* StateName(IopKernel::State s) {
    switch (s) {
    case IopKernel::State::Dormant: return "Dormant";
    case IopKernel::State::Ready: return "Ready";
    case IopKernel::State::Run: return "Run";
    case IopKernel::State::Wait: return "Wait";
    }
    return "?";
}
static const char* WaitName(IopKernel::WaitKind w) {
    switch (w) {
    case IopKernel::WaitKind::None: return "None";
    case IopKernel::WaitKind::Sleep: return "Sleep";
    case IopKernel::WaitKind::Delay: return "Delay";
    case IopKernel::WaitKind::Sema: return "Sema";
    case IopKernel::WaitKind::EventFlag: return "EventFlag";
    case IopKernel::WaitKind::Mbx: return "Mbx";
    case IopKernel::WaitKind::Rpc: return "Rpc";
    }
    return "?";
}
static std::string ThreadCensus(IopKernel& k) {
    std::string out;
    char buf[64];
    for (int t = 1; t < IopKernel::kMaxThreads; t++) {
        if (!k.ThreadRegistered(t)) continue;
        std::snprintf(buf, sizeof buf, "%s%d:%s/%s", out.empty() ? "" : ",", t, StateName(k.ThreadState(t)),
                      WaitName(k.ThreadWaitKind(t)));
        out += buf;
    }
    return out;
}

static void TestAnchorRepark(const std::string& og_root) {
    using ps2r::title::stream_irx::ModuleLayout;
    using ps2r::title::stream_irx::StreamIrxPlugin;
    const std::string irx_path = og_root + "/game_files/STREAM.IRX";
    const std::string anchor_path = og_root + "/save_states/" + kAnchors[0];
    struct stat sb;
    if (og_root.empty() || stat(irx_path.c_str(), &sb) != 0 || stat(anchor_path.c_str(), &sb) != 0) {
        std::printf("anchor re-park oracle: SKIP (STREAM.IRX or %s not found under '%s')\n", kAnchors[0], og_root.c_str());
        return;
    }
    std::ifstream in(irx_path, std::ios::binary);
    std::vector<uint8_t> irx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ps2r::savestate::FileMeta meta;
    std::vector<ps2r::savestate::Blob> blobs;
    std::string err;
    CHECK(ps2r::savestate::ReadStateFile(anchor_path, &meta, &blobs, &err), "anchor state file reads");
    const ps2r::savestate::Blob* iopm = nullptr;
    for (const auto& b : blobs) if (b.tag == ps2r::savestate::TAG_IOPM) iopm = &b;
    CHECK(iopm != nullptr, "anchor carries an IOPM section");
    if (!iopm) return;
    IopMemory image_ram;
    IopmSection::Statics image{};
    CHECK(IopmSection::Parse(iopm->payload, image_ram, image), "anchor IOPM parses");
    const IopmSection::ThreadRec& wrec = image.threads[2];
    std::printf("image: thr[2] entry=0x%x prio=%u flags=%s%s%s%s; s0=%d/%d; [0x12f14]=%u [0x12f18]=%u\n", wrec.entry,
                wrec.priority, wrec.active ? "active" : "", wrec.started ? "+started" : "", wrec.sleeping ? "+sleeping" : "",
                wrec.terminated ? "+terminated" : "", image.semas[0].count, image.semas[0].max_count,
                image_ram.Read<uint32_t>(0x12f14), image_ram.Read<uint32_t>(0x12f18));
    CHECK(wrec.entry != 0 && wrec.active && wrec.started && !wrec.sleeping && !wrec.terminated,
          "the image records the worker thread (tid 2) as active+started (parked on its semaphore, not sleeping)");

    IopModuleTable table(IopClock::Policy::Frame);
    IopMemory mem;
    ps2r::sound::SpuCore spu;
    StreamIrxPlugin plugin(table, mem, spu, nullptr, nullptr);
    std::vector<std::string> trace;
    table.Kernel().SetTraceHook([&trace](const char* evt, int64_t a, int64_t b, uint64_t t_us) {
        char line[128];
        std::snprintf(line, sizeof line, "[IOPSCHED] %s %lld %lld t=%llu", evt, (long long)a, (long long)b,
                      (unsigned long long)t_us);
        trace.push_back(line);
    });
    const StreamIrxPlugin::BootReport rep = plugin.Boot(irx.data(), irx.size());
    CHECK(rep.sections.ok && rep.entry_ran && rep.rpc_tid == 1, "the module boots to its first block (tid 1)");
    IopKernel& k = table.Kernel();
    std::printf("boot park: iop_threads=%s (tid 2 registered=%d) s0 active=%d\n", ThreadCensus(k).c_str(),
                k.ThreadRegistered(2) ? 1 : 0, k.SemaActive(0) ? 1 : 0);
    CHECK(!k.ThreadRegistered(2), "the boot path never creates the worker (the title's stream-open RPC does)");
    const size_t boot_lines = trace.size();

    IopmSection::Deps deps = plugin.IopmDeps([&meta] { return static_cast<uint64_t>(meta.vsync_frame); });
    ps2r::savestate::SectionSpec spec = IopmSection::MakeSpec(deps);
    CHECK(spec.restore(*iopm, &err), "anchor IOPM restores through the plugin's deps");
    for (size_t i = boot_lines; i < trace.size(); i++) std::printf("  restore %s\n", trace[i].c_str());
    std::printf("after restore: iop_threads=%s s0=%d unparked=%d\n", ThreadCensus(k).c_str(), k.SemaCount(0), k.UnparkedCount());
    CHECK(k.ThreadRegistered(2) && k.ThreadPriority(2) == wrec.priority, "the worker exists after the restore with the image's priority");
    CHECK(k.ThreadState(2) == IopKernel::State::Wait && k.ThreadWaitKind(2) == IopKernel::WaitKind::Sema,
          "the restored worker is parked waiting on its semaphore (not Dormant)");
    CHECK(k.ThreadState(1) == IopKernel::State::Wait && k.ThreadWaitKind(1) == IopKernel::WaitKind::Sleep,
          "the boot-path RPC thread keeps its park (Wait/Sleep)");
    CHECK(k.SemaActive(0) && k.SemaCount(0) == image.semas[0].count, "sema 0's token count equals the image's (no unconsumed token)");
    CHECK(k.UnparkedCount() == 0, "every thread is parked after the re-park");
    CHECK(mem.Read<uint32_t>(0x12e50) == image_ram.Read<uint32_t>(0x12e50) && mem.Read<uint32_t>(0x12e54) == image_ram.Read<uint32_t>(0x12e54),
          "the worker's run to its first park left the image's busy/waiter words unchanged");
    // The heartbeat hard timer: the image's descriptor {0, id, compare, 0} says the
    // title's init armed it; after the restore it beats (OLD's kernel kept beating
    // across its load -- it had armed the timer in frame 1 before the load applied).
    const uint32_t desc_id = image_ram.Read<uint32_t>(ModuleLayout::kTimerDescIdWord);
    const uint32_t desc_compare = image_ram.Read<uint32_t>(ModuleLayout::kTimerDescCompareWord);
    std::printf("image: timer desc [0x%x]={%u,%u,%u,%u} -> period %llu us; after restore Running(%u)=%d kernel_timer=%d [0x12f1c]=%u\n",
                ModuleLayout::kTimerDesc, image_ram.Read<uint32_t>(ModuleLayout::kTimerDesc), desc_id, desc_compare,
                image_ram.Read<uint32_t>(ModuleLayout::kTimerDesc + 12),
                (unsigned long long)ps2r::iop::hle::TimrmanModule::PeriodUsec(desc_compare), desc_id,
                plugin.Timrman().Running(static_cast<int>(desc_id)) ? 1 : 0, plugin.Timrman().KernelTimerId(static_cast<int>(desc_id)),
                mem.Read<uint32_t>(0x12f1c));
    CHECK(desc_id == 1 && desc_compare != 0, "the image's descriptor records the heartbeat timer (id 1) armed with a non-zero compare");
    CHECK(plugin.Timrman().Running(1) && plugin.Timrman().KernelTimerId(1) >= 0, "the heartbeat hard timer is running after the restore");
    bool timer_started = false;
    for (size_t i = boot_lines; i < trace.size(); i++) timer_started = timer_started || trace[i].rfind("[IOPSCHED] timer_start ", 0) == 0;
    CHECK(timer_started, "the restore armed a kernel periodic timer (timer_start in the trace)");
    // A signal from the RPC face hands the token straight to the parked worker.
    const size_t before_signal = trace.size();
    k.WithDispatchLock([&k] { k.SignalSema(0); });
    bool handoff = false;
    for (size_t i = before_signal; i < trace.size(); i++) handoff = handoff || trace[i].rfind("[IOPSCHED] sema_handoff 0 2", 0) == 0;
    CHECK(handoff && k.ThreadState(2) == IopKernel::State::Ready, "a SignalSema on the worker's semaphore hands off to the parked worker");
    // One period later the beat fires: the handler sets the tick-pending word and
    // signals the worker's semaphore (a handoff to a parked worker, or a token for
    // the worker the signal above readied); the kernel re-arms the beat and every
    // thread parks again (this harness has no disc: the worker's pass ends in the
    // module's own retry delay, not an unparked thread).
    const size_t before_beat = trace.size();
    const int ktimer = plugin.Timrman().KernelTimerId(1);
    char beat_tag[48];
    std::snprintf(beat_tag, sizeof beat_tag, "[IOPSCHED] hb_beat %d ", ktimer);
    k.Clock().AdvanceToUs(k.Clock().NowUs() + ps2r::iop::hle::TimrmanModule::PeriodUsec(desc_compare));
    k.Dispatch();
    bool beat = false, beat_signal = false;
    for (size_t i = before_beat; i < trace.size(); i++) {
        beat = beat || trace[i].rfind(beat_tag, 0) == 0;
        beat_signal = beat_signal || trace[i].rfind("[IOPSCHED] sema_handoff 0 2", 0) == 0 || trace[i].rfind("[IOPSCHED] sema_signal 0 ", 0) == 0;
        std::printf("  beat %s\n", trace[i].c_str());
    }
    std::printf("after beat: iop_threads=%s s0=%d unparked=%d Running(1)=%d\n", ThreadCensus(k).c_str(), k.SemaCount(0), k.UnparkedCount(),
                plugin.Timrman().Running(1) ? 1 : 0);
    CHECK(beat && beat_signal, "the first beat after the restore fires the handler, whose iSignalSema reaches the worker's semaphore");
    CHECK(plugin.Timrman().Running(1), "the heartbeat re-arms after its beat (the handler returns its compare)");
    CHECK(k.UnparkedCount() == 0, "every thread is parked again after the heartbeat pass");
}

static void TestRestoredPriorityOrder() {
    IopKernel saver;
    saver.Clock().SetPolicy(IopClock::Policy::Frame);
    for (int tid = 1; tid <= 2; ++tid) {
        saver.RegisterThread(tid, static_cast<uint32_t>(tid * 10));
        saver.StartThread(tid, [&] { saver.SleepThread(); });
    }
    saver.Dispatch();
    CHECK(saver.ChangePriority(2, 5) == 0, "saver changes a sleeping thread priority");
    IopmSection::Statics saved{};
    for (int tid = 1; tid <= 2; ++tid) saved.threads[tid].started = true;
    IopmSection::MirrorKernel(saver, saved);
    IopMemory ram;
    std::vector<uint8_t> payload;
    IopmSection::Serialize(payload, ram, saved);
    IopmSection::Statics loaded{};
    CHECK(IopmSection::Parse(payload, ram, loaded), "effective priorities round trip through the existing IOPM layout");

    IopKernel restored;
    restored.Clock().SetPolicy(IopClock::Policy::Frame);
    std::vector<int> order;
    for (int tid = 1; tid <= 2; ++tid) {
        restored.RegisterThread(tid, static_cast<uint32_t>(tid * 10));
        restored.StartThread(tid, [&, tid] { restored.SleepThread(); order.push_back(tid); });
    }
    restored.Dispatch();
    IopmSection::ReconcileKernel(restored, loaded, 0);
    restored.iWakeupThread(1);
    restored.iWakeupThread(2);
    restored.Dispatch();
    CHECK((order == std::vector<int>{2, 1}), "restored sleepers wake in saved effective priority order");
}

int main(int argc, char** argv) {
    ps2r::platform::host::RegisterSelf();
    TestRoundTrip();
    TestCrossPolicy();
    TestRestoredPriorityOrder();
    TestRegistrySpec();
    TestOldAnchorsOracle(argc > 1 ? argv[1] : "");
    TestAnchorRepark(argc > 1 ? argv[1] : "");
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
