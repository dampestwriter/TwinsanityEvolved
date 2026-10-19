// tests/iop/test_stream_irx_loads_and_parks.cpp -- StreamIrxLoadsAndParks: the
// plugin loads the real STREAM.IRX bytes (OLD game_files/, read-only), runs the
// entry under the FRAME-policy kernel through module init to its first block, and
// reports the park (tid / wait kind / heartbeat armed). Mode `repark`: the IOPM
// boot-symmetric re-park oracle -- serialize the booted state's section, restore it
// through the plugin's deps, and check kernel thread states/wait kinds and sema
// counts equal the boot-path park.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "engine/iop/iop_kernel.h"
#include "engine/iop/iop_memory.h"
#include "engine/iop/iop_module_table.h"
#include "engine/iop/savestate/iopm_section.h"
#include "engine/platform/host/platform.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"

using namespace ps2r::iop;
using ps2r::title::stream_irx::ModuleLayout;
using ps2r::title::stream_irx::StreamIrxPlugin;

static int g_fail = 0;
#define CHECK(c, msg)                                                          \
    do {                                                                       \
        if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); g_fail++; } \
    } while (0)

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

struct Park {
    IopKernel::State state[IopKernel::kMaxThreads];
    IopKernel::WaitKind wait[IopKernel::kMaxThreads];
    bool registered[IopKernel::kMaxThreads];
    int32_t sema[IopKernel::kMaxSemas];
    bool sema_active[IopKernel::kMaxSemas];
};
static Park Snapshot(IopKernel& k) {
    Park p{};
    for (int t = 0; t < IopKernel::kMaxThreads; ++t) {
        p.registered[t] = k.ThreadRegistered(t);
        p.state[t] = k.ThreadState(t);
        p.wait[t] = k.ThreadWaitKind(t);
    }
    for (int s = 0; s < IopKernel::kMaxSemas; ++s) {
        p.sema_active[s] = k.SemaActive(s);
        p.sema[s] = p.sema_active[s] ? k.SemaCount(s) : 0;
    }
    return p;
}
static std::string Quote(IopKernel& k, int rpc_tid) {
    std::string q;
    char buf[160];
    for (int t = 1; t < IopKernel::kMaxThreads; ++t) {
        if (!k.ThreadRegistered(t)) continue;
        std::snprintf(buf, sizeof buf, "tid %d %s/%s prio %u%s; ", t, StateName(k.ThreadState(t)),
                      WaitName(k.ThreadWaitKind(t)), k.ThreadPriority(t), t == rpc_tid ? " (rpc)" : "");
        q += buf;
    }
    return q;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <OLD root> [repark]\n", argv[0]);
        return 2;
    }
    ps2r::platform::host::RegisterSelf();   // the fiber layer behind the kernel coroutines
    const bool repark = argc > 2 && std::strcmp(argv[2], "repark") == 0;
    const std::string path = std::string(argv[1]) + "/game_files/STREAM.IRX";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("SKIP: %s not present (OLD game_files read-only source)\n", path.c_str());
        return 0;
    }
    std::vector<uint8_t> irx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(irx.size() == ModuleLayout::kImageBytes, "STREAM.IRX is the 116156-byte image");

    IopModuleTable table(IopClock::Policy::Frame);
    IopMemory mem;
    ps2r::sound::SpuCore spu;
    StreamIrxPlugin plugin(table, mem, spu, nullptr, nullptr);
    const StreamIrxPlugin::BootReport rep = plugin.Boot(irx.data(), irx.size());
    std::printf("sections: text %u data %u rodata %u bss %u; %s\n", rep.sections.text_bytes, rep.sections.data_bytes,
                rep.sections.rodata_bytes, rep.sections.bss_bytes, rep.sections.ok ? "ok" : rep.sections.error.c_str());
    CHECK(rep.sections.ok && rep.entry_ran, "the module loads and its entry runs");
    CHECK(table.Booted(), "OnModuleLoaded booted the table (module init ran to its first block)");
    CHECK(table.ModuleCount() == StreamIrxPlugin::kModuleRows, "22 IOPRP rows + libsd + 6 idle disc IRXs + STREAM");
    CHECK(mem.UnroutedCount() == 0, "no access fell outside IOP RAM during init");

    IopKernel& k = table.Kernel();
    const int rpc_tid = rep.rpc_tid;
    std::printf("first block: rpc tid %d ([0x%X] = %u); %s\n", rpc_tid, ModuleLayout::kRpcTidAddr,
                mem.Read<uint32_t>(ModuleLayout::kRpcTidAddr), Quote(k, rpc_tid).c_str());
    std::printf("kernel: unparked %d; timers: hard timer 1 %s; libsd calls: init %u setparam %u; cd init %u\n",
                k.UnparkedCount(), plugin.Timrman().Running(1) ? "RUNNING" : "idle", plugin.Libsd().Calls(0),
                plugin.Libsd().Calls(1), plugin.Cdvdman().CallbackAddr());
    CHECK(k.UnparkedCount() == 0, "every started thread is parked after init (the first block)");
    // The observed first block of the boot path: the entry creates and starts ONE thread
    // (tid 1, priority 30, the module's main thread) and stores its tid at [0x13328]; that
    // thread runs its init prologue and parks in SleepThread (Wait::Sleep) BEFORE SdInit,
    // the hard-timer heartbeat and the RPC service loop -- those follow the EE's wakeup
    // (sceSifBindRpc/CallRpc from the game, Phase 4C), so at this park the timer is idle
    // and no libsd/cdvdman call has happened yet.
    CHECK(rpc_tid == 1 && k.ThreadRegistered(1) && k.ThreadPriority(1) == 30, "tid 1 (prio 30) is the module's main thread");
    CHECK(k.ThreadState(1) == IopKernel::State::Wait && k.ThreadWaitKind(1) == IopKernel::WaitKind::Sleep,
          "the first block is tid 1 in SleepThread (Wait::Sleep)");
    CHECK(!plugin.Timrman().Running(1), "the heartbeat hard timer is not armed before the EE wakes tid 1 (4C)");
    CHECK(plugin.Libsd().Calls(0) == 0 && plugin.Cdvdman().CallbackAddr() == 0, "no device call before the wakeup");
    CHECK(plugin.Protocol().Calls() == 0, "no CD-read RPC before the EE arms the stream");

    if (repark) {
        const Park boot = Snapshot(k);
        IopmSection::Deps deps = plugin.IopmDeps([] { return 0ull; });
        ps2r::savestate::SectionSpec spec = IopmSection::MakeSpec(deps);
        std::vector<uint8_t> payload;
        spec.serialize(payload);
        CHECK(payload.size() == IopmSection::kPayloadBytes, "the booted state serializes into an IOPM section");
        // Perturb what the restore must put back: the heap pointer and a libsd sequence.
        plugin.Sysmem().RestoreHeapPtr(plugin.Statics().heap_ptr + 0x1000);
        IopContext scratch = IopContext::ForEntry(ModuleLayout::kStackTop - 0x10, ModuleLayout::kGp);
        scratch.gpr[4] = 0;
        mem.Write<uint32_t>(scratch.gpr[29] + 0x10, 64);
        plugin.Libsd().Call(9, scratch);
        CHECK(plugin.Libsd().PendingSequence(0) == 1, "a voice transfer is pending before the restore");
        ps2r::savestate::Blob blob;
        blob.payload = payload;
        std::string err;
        CHECK(spec.restore(blob, &err), err.c_str());
        const Park after = Snapshot(k);
        bool same = true;
        for (int t = 0; t < IopKernel::kMaxThreads; ++t)
            same = same && boot.registered[t] == after.registered[t] && boot.state[t] == after.state[t] &&
                   boot.wait[t] == after.wait[t];
        for (int s = 0; s < IopKernel::kMaxSemas; ++s)
            same = same && boot.sema_active[s] == after.sema_active[s] && boot.sema[s] == after.sema[s];
        std::printf("re-park: %s\n", Quote(k, rpc_tid).c_str());
        CHECK(same, "boot-symmetric re-park: thread states/wait kinds and sema counts equal the boot-path park");
        CHECK(k.UnparkedCount() == 0, "nothing unparked by the restore");
        CHECK(plugin.Libsd().PendingSequence(0) == 0, "the libsd pending sequence was reset by after_restore");
        CHECK(plugin.Sysmem().Heap().HeapPtr() == plugin.Statics().heap_ptr, "heap_ptr restored into the sysmem arena");
    }
    std::printf("%s: %d failure(s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
