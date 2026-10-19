// tests/iop/test_stream_irx_serves_first_rpc.cpp -- StreamIrxServesFirstRpc: the
// first EE -> STREAM.IRX round trip through the real module. The plugin boots the
// real STREAM.IRX bytes (OLD game_files/, read-only) to its first block (tid 1 in
// SleepThread), an EE client is bound through SceSif::BindRpc(0x12345) and one
// sceSifCallRpc is driven through SceSif::CallRpc against the StreamRpcServer
// over a test EE RAM. Asserted, in order: the bridge body is submitted and
// completed on the module's RPC thread (tid 1 left its SleepThread park and ran
// the service loop), main_handler ran on the request (the send bytes at
// kRpcSendBuffer, the reply = kRpcResultBuffer byte for byte in the EE recv
// buffer), tid 1 re-parked in the service loop, the client record completed
// (next == cbuff, command bit 0 clear), the sema is untouched (-1), the call
// returned 0. A zeroed request is NOT the module's init command: SdInit and the
// hard-timer heartbeat stay idle (the pin). The second case sends the module's
// init command, CD_READ_INIT (cmd 12), and asserts what follows it: the sound
// core set up through libsd (SdSetSwitch/SdGetSwitch/SdGetAddr -- never ordinal
// 0, SdInit) and the hard-timer heartbeat armed (timrman hard timer 1 running)
// and live over the next boundaries. The EE-side server (EeRpcServer) is installed so a bridged
// stream-event call would be served; neither call issues one (Bridged() == 0
// after the first).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/iop/iop_kernel.h"
#include "engine/iop/iop_memory.h"
#include "engine/iop/iop_module_table.h"
#include "engine/platform/host/platform.h"
#include "engine/sif/hle/ee_rpc_server.h"
#include "engine/sif/hle/rpc_router.h"
#include "engine/sif/hle/sce_sif.h"
#include "engine/sif/sif_state.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h"

using namespace ps2r::iop;
using ps2r::title::stream_irx::ModuleLayout;
using ps2r::title::stream_irx::StreamIrxPlugin;
using ps2r::title::stream_irx::StreamRpcServer;

static int g_fail = 0;
#define CHECK(c, msg)                                                          \
    do {                                                                       \
        if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); g_fail++; } \
    } while (0)

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
static const char* StateName(IopKernel::State s) {
    switch (s) {
    case IopKernel::State::Dormant: return "Dormant";
    case IopKernel::State::Ready: return "Ready";
    case IopKernel::State::Run: return "Run";
    case IopKernel::State::Wait: return "Wait";
    }
    return "?";
}

// The test EE RAM behind memory::read/write (what SceSif's rows use).
static void BindEeRam() {
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink sink;
    static MmioRouter mmio(sink);
    static FastMem fm(ram, map, mmio);
    RecompAbi::BindMemory(&fm);
}

static void SetArg(CpuContext& ctx, int reg, uint32_t v) { ctx.cpuRegs.GPR.r[reg].UD[0] = v; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <OLD root>\n", argv[0]);
        return 2;
    }
    ps2r::platform::host::RegisterSelf();
    const std::string path = std::string(argv[1]) + "/game_files/STREAM.IRX";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("SKIP: %s not present (OLD game_files read-only source)\n", path.c_str());
        return 0;
    }
    std::vector<uint8_t> irx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    BindEeRam();

    IopModuleTable table(IopClock::Policy::Frame);
    IopMemory mem;
    ps2r::sound::SpuCore spu;
    StreamIrxPlugin plugin(table, mem, spu, nullptr, nullptr);
    const StreamIrxPlugin::BootReport rep = plugin.Boot(irx.data(), irx.size());
    CHECK(rep.sections.ok && rep.entry_ran && table.Booted(), "the module loads, its entry runs, the table booted");
    IopKernel& k = table.Kernel();
    const int rpc_tid = rep.rpc_tid;
    CHECK(rpc_tid == 1 && k.ThreadWaitKind(1) == IopKernel::WaitKind::Sleep, "first block: tid 1 in SleepThread");
    CHECK(!plugin.Timrman().Running(1) && plugin.Libsd().Calls(0) == 0, "no SdInit / heartbeat before the first RPC");

    // The EE side: router + the sceSif rows' collaborators + the module's face.
    RpcRouter router;
    SifState sif;
    SceSif::Bind(SdkBindingManifest::Instance(), router, sif, SceSif::IopHeap{}, [](uint32_t handle) { return handle & 0xFFFFu; });
    StreamRpcServer face(
        mem, table.Rpc(), [](uint32_t a) { return memory::read<uint8_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); });
    router.RegisterServer(ModuleLayout::kRpcServerId, face, "STREAM");
    EeRpcServer ee_server(
        SceSif::RegisteredEeServer(), mem, table.Rpc(), [](uint32_t a) { return memory::read<uint8_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); }, [](uint32_t, CpuContext&) {});
    plugin.Protocol().SetEeServer(&ee_server);

    // sceSifBindRpc(client, 0x12345, 0).
    constexpr uint32_t kClient = 0x00200000, kSp = 0x00201000, kSend = 0x00202000, kRecv = 0x00203000;
    CpuContext ctx{};
    SetArg(ctx, 29, kSp);
    SetArg(ctx, 4, kClient);
    SetArg(ctx, 5, ModuleLayout::kRpcServerId);
    SetArg(ctx, 6, 0);
    SceSif::BindRpc(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "sceSifBindRpc returns 0");
    CHECK(memory::read<uint32_t>(kClient + SceSif::kClientServer) == SceSif::ServerHandle(ModuleLayout::kRpcServerId),
          "the client record's server word is the bound handle");
    const RpcRouter::Binding* b = router.FindByClient(kClient);
    CHECK(b && b->server_id == ModuleLayout::kRpcServerId, "the router routes the client to server 0x12345");
    CHECK(router.BoundCensus().find("12345") != std::string::npos, "sif_bound= names the server");

    // The request: a zeroed 0x28-byte send buffer with a recognisable stamp in
    // its trailing words (the reply is whatever main_handler builds for it).
    constexpr uint32_t kSendBytes = 0x28, kRecvBytes = 0x40;
    for (uint32_t o = 0; o < kSendBytes; o += 4) memory::write<uint32_t>(kSend + o, 0);
    for (uint32_t o = 0; o < kRecvBytes; o += 4) memory::write<uint32_t>(kRecv + o, 0xCAFE0000u | o);
    memory::write<uint32_t>(kSp, 0);   // end_param
    // sceSifCallRpc(client, fno 0, mode 0, send, send_size, recv, recv_size, end_func 0).
    SetArg(ctx, 4, kClient);
    SetArg(ctx, 5, 0);
    SetArg(ctx, 6, 0);
    SetArg(ctx, 7, kSend);
    SetArg(ctx, 8, kSendBytes);
    SetArg(ctx, 9, kRecv);
    SetArg(ctx, 10, kRecvBytes);
    SetArg(ctx, 11, 0);
    SetArg(ctx, 31, 0x00100100);
    SceSif::CallRpc(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "sceSifCallRpc returns 0");

    // The observed sequence on the module side.
    std::printf("first RPC: face calls %u stalls %u; bridge submitted %llu completed %llu stalls %llu\n", face.Calls(),
                face.Stalls(), (unsigned long long)table.Rpc().Submitted(), (unsigned long long)table.Rpc().Completed(),
                (unsigned long long)table.Rpc().Stalls());
    std::printf("tid 1: %s/%s; libsd init %u setparam %u; hard timer 1 %s; unparked %d; protocol calls %u bridged %u\n",
                StateName(k.ThreadState(1)), WaitName(k.ThreadWaitKind(1)), plugin.Libsd().Calls(0),
                plugin.Libsd().Calls(1), plugin.Timrman().Running(1) ? "RUNNING" : "idle", k.UnparkedCount(),
                plugin.Protocol().Calls(), plugin.Protocol().Bridged());
    CHECK(face.Calls() == 1 && face.Stalls() == 0, "the STREAM face served one call without a stall");
    CHECK(table.Rpc().Submitted() == 1 && table.Rpc().Completed() == 1, "one bridge body submitted and completed");
    CHECK(plugin.Libsd().Calls(0) == 0 && !plugin.Timrman().Running(1),
          "a zeroed request is not the init command: SdInit / the heartbeat stay idle");
    CHECK(k.ThreadState(1) == IopKernel::State::Wait && k.ThreadWaitKind(1) == IopKernel::WaitKind::Sleep,
          "tid 1 parks again in the RPC service loop (sceSifRpcLoop = SleepThread)");
    CHECK(k.UnparkedCount() == 0, "every module thread is parked after the call");
    CHECK(plugin.Protocol().Bridged() == 0 && ee_server.Served() == 0, "the first RPC issues no EE stream-event call");

    // The reply: kRpcResultBuffer's bytes, as the module left them, in the EE recv buffer.
    bool same = true;
    for (uint32_t i = 0; i < kRecvBytes; ++i) same = same && memory::read<uint8_t>(kRecv + i) == mem.Read<uint8_t>(ModuleLayout::kRpcResultBuffer + i);
    CHECK(same, "the reply in the EE recv buffer is kRpcResultBuffer byte for byte");
    bool overwritten = false;
    for (uint32_t o = 0; o < kRecvBytes; o += 4) overwritten = overwritten || memory::read<uint32_t>(kRecv + o) != (0xCAFE0000u | o);
    CHECK(overwritten, "the recv buffer was written (the stamp is gone)");
    std::printf("reply: %08x %08x %08x %08x ... (recv[0..15])\n", memory::read<uint32_t>(kRecv), memory::read<uint32_t>(kRecv + 4),
                memory::read<uint32_t>(kRecv + 8), memory::read<uint32_t>(kRecv + 12));
    bool send_copied = true;
    for (uint32_t i = 0; i < kSendBytes; ++i) send_copied = send_copied && mem.Read<uint8_t>(ModuleLayout::kRpcSendBuffer + i) == memory::read<uint8_t>(kSend + i);
    CHECK(send_copied, "the send buffer reached kRpcSendBuffer byte for byte");

    // The client record's completion.
    CHECK(memory::read<uint32_t>(kClient + SceSif::kClientNext) == memory::read<uint32_t>(kClient + SceSif::kClientCbuff),
          "completed: next == cbuff");
    CHECK((memory::read<uint32_t>(kClient + SceSif::kClientCommand) & 1u) == 0, "completed: command bit 0 clear");
    CHECK(memory::read<int32_t>(kClient + SceSif::kClientSema) == -1, "no completion sema on the record");
    CHECK(router.PendingCount() == 0, "no end callback queued (end_func 0)");
    CHECK(mem.UnroutedCount() == 0, "no IOP access fell outside IOP RAM");

    // ---- The module's INIT command: CD_READ_INIT (cmd 12) ---------------------
    // Request layout (the parser at main_handler 0x8D28, as the original's HLE
    // read it -- sif_rpc_handlers.cpp:5271-5310, :5459-5479): u16 header, u16
    // command_count, then per command u16 cmd_type, u16 param_count, u16
    // params[]. Cmd 12 carries {config, p1, sectors, flags}. This is the
    // command that initialises the streaming engine: the sound-core setup
    // (libsd SdSetSwitch/SdGetSwitch/SdGetAddr -- never SdInit, ordinal 0) and
    // the hard-timer heartbeat follow it, not "any first call".
    constexpr uint32_t kInitSend = 0x00204000, kInitRecv = 0x00205000, kInitRecvBytes = 0x40;
    const uint16_t init_words[] = {0x0000, 0x0001, 0x000C, 0x0004, 0x0001, 0x0000, 0x0010, 0x0000};
    constexpr uint32_t kInitSendBytes = sizeof(init_words);
    for (uint32_t i = 0; i < kInitSendBytes / 2; ++i) memory::write<uint16_t>(kInitSend + i * 2, init_words[i]);
    for (uint32_t o = 0; o < kInitRecvBytes; o += 4) memory::write<uint32_t>(kInitRecv + o, 0xCAFE0000u | o);
    SetArg(ctx, 4, kClient);
    SetArg(ctx, 5, 0);
    SetArg(ctx, 6, 0);
    SetArg(ctx, 7, kInitSend);
    SetArg(ctx, 8, kInitSendBytes);
    SetArg(ctx, 9, kInitRecv);
    SetArg(ctx, 10, kInitRecvBytes);
    SetArg(ctx, 11, 0);
    SetArg(ctx, 31, 0x00100100);
    SceSif::CallRpc(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "init command: sceSifCallRpc returns 0");
    std::printf("init command (cmd 12): face calls %u stalls %u; bridge submitted %llu completed %llu\n", face.Calls(), face.Stalls(),
                (unsigned long long)table.Rpc().Submitted(), (unsigned long long)table.Rpc().Completed());
    std::printf("tid 1: %s/%s; libsd init %u setparam %u; hard timer 1 %s; unparked %d; protocol calls %u bridged %u\n",
                StateName(k.ThreadState(1)), WaitName(k.ThreadWaitKind(1)), plugin.Libsd().Calls(0), plugin.Libsd().Calls(1),
                plugin.Timrman().Running(1) ? "RUNNING" : "idle", k.UnparkedCount(), plugin.Protocol().Calls(),
                plugin.Protocol().Bridged());
    std::printf("init reply: %08x %08x %08x %08x ... (recv[0..15])\n", memory::read<uint32_t>(kInitRecv),
                memory::read<uint32_t>(kInitRecv + 4), memory::read<uint32_t>(kInitRecv + 8), memory::read<uint32_t>(kInitRecv + 12));
    CHECK(face.Calls() == 2 && face.Stalls() == 0, "the STREAM face served the init command without a stall");
    CHECK(table.Rpc().Submitted() == 2 && table.Rpc().Completed() == 2, "the second bridge body completed");
    // The command census: the ids-only list and the boundary-tagged forms with
    // parameters (the first request carried no command: count 0 -> nothing recorded;
    // the init request = cmd 12 {1, 0, 16, 0} tagged with the boundary set before it).
    face.SetBoundary(7);
    CHECK(face.CommandIdsCsv() == "12", "stream_rpc_cmds: the init command's id only (the zeroed request recorded nothing)");
    CHECK(face.CommandHeadCsv() == "b0:12[1 0 16 0]", "stream_rpc_cmds_head: boundary-tagged with the parameters (the boundary at the call)");
    CHECK(face.CommandTailCsv() == face.CommandHeadCsv(), "stream_rpc_cmds_tail equals the head while fewer than the ring's depth were served");
    CHECK(face.CommandIdsFromBoundaryCsv(1).empty() && face.CommandIdsFromBoundaryCsv(0) == "12",
          "the from-boundary census selects by the tag");
    // The module's sound init on cmd 12 is voice/core setup through libsd
    // SdSetSwitch (3) / SdGetSwitch (4) / SdGetAddr (6) (+ ordinals 11, 15) --
    // libsd ordinal 0 (SdInit) is never called by the real module (recorded:
    // P4-CDINIT-SOUND-SHAPE); the census line is the oracle quote.
    std::printf("libsd ordinals after cmd 12:");
    for (int o = 0; o < 16; ++o) std::printf(" %u", plugin.Libsd().Calls(o));
    std::printf("\n");
    CHECK(plugin.Libsd().Calls(3) == 2 && plugin.Libsd().Calls(4) == 2 && plugin.Libsd().Calls(6) == 48,
          "CD_READ_INIT: sound init = SdSetSwitch x2, SdGetSwitch x2, SdGetAddr x48 on the idle core");
    CHECK(plugin.Libsd().Calls(0) == 0, "libsd ordinal 0 (SdInit) is not what the module's init command calls");
    // The armed heartbeat is live: over the next boundaries it polls the sound
    // core (SdGetParam (2) / SdGetAddr (6)) while tid 1 stays parked.
    for (int i = 0; i < 8; ++i) table.PumpAtBoundary();
    std::printf("libsd ordinals after 8 boundaries:");
    for (int o = 0; o < 16; ++o) std::printf(" %u", plugin.Libsd().Calls(o));
    std::printf("; tid 1 %s/%s; hard timer 1 %s; unparked %d\n", StateName(k.ThreadState(1)), WaitName(k.ThreadWaitKind(1)),
                plugin.Timrman().Running(1) ? "RUNNING" : "idle", k.UnparkedCount());
    CHECK(plugin.Libsd().Calls(2) > 0 && plugin.Libsd().Calls(6) > 48, "the armed heartbeat polls the sound core at every boundary");
    CHECK(plugin.Timrman().Running(1) && k.ThreadWaitKind(1) == IopKernel::WaitKind::Sleep, "the heartbeat keeps running; tid 1 stays parked");
    CHECK(plugin.Timrman().Running(1), "CD_READ_INIT: the hard-timer heartbeat is armed (timrman hard timer 1 running)");
    CHECK(k.ThreadState(1) == IopKernel::State::Wait && k.ThreadWaitKind(1) == IopKernel::WaitKind::Sleep,
          "tid 1 parks again in the RPC service loop after the init command");
    same = true;
    for (uint32_t i = 0; i < kInitRecvBytes; ++i) same = same && memory::read<uint8_t>(kInitRecv + i) == mem.Read<uint8_t>(ModuleLayout::kRpcResultBuffer + i);
    CHECK(same, "the init reply in the EE recv buffer is kRpcResultBuffer byte for byte");
    CHECK(mem.UnroutedCount() == 0, "no IOP access fell outside IOP RAM during the init command");

    std::printf("%s: %d failure(s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
