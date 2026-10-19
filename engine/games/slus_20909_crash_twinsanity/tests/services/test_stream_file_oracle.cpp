// Same-input file self-oracle: the actual NEW STREAM.IRX model and StreamLibrary.
// Fixture setup derived from the existing LoaderCompletionModule test; no PCSX2 data.
// LoaderCompletionModule: a STREAM record the TITLE stages itself (the issue
// primitive FUN_002ad9a8 -> FUN_001e69a8, type 0x7e "load file to EE dest") is
// served by the REAL recompiled STREAM.IRX under the cooperative IOP kernel,
// and the blocking wait (LoaderFileIo::WaitReadComplete = the title's
// FUN_002ad1a0) yields to that kernel through Deps::pump_module -- bound here
// exactly as the composition root binds it -- until the title's OWN completion
// poll (the REAL FUN_002adaf0 -> FUN_001e3308 over the client's slot byte)
// answers done. The rig follows the whole-file loader FUN_002ab160 step by
// step: the stream object comes from the REAL FUN_002ad650(manager, 0x800, 0, 0)
// (allocates the fs, takes a slot from the stream manager, and stages the
// ring-stream open -- STREAM cmd 13 {slot, 0, 0x800} through FUN_001e1ca8 --
// before the file is registered), the file is opened and sized through the seam
// (cmd 31 staged, then both shipped: the retail `[cmd=13][cmd=31]` order), and
// the record is staged by the REAL FUN_002ad9a8. The stream manager is built by
// its REAL constructor FUN_002add88(mgr, 9) (the title's FUN_002ad560 argument);
// the title's heap (FUN_00181560/0x1815c0 new, 0x181590/0x1815f0 delete) is a
// test-local bump allocator over a scratch region -- the rig has no title heap.
// Asserted: the file bytes land in the destination through sifman's SIF DMA
// (sceSifSetDma into the EE RAM sink), the slot byte retires (2 -> 0) through
// the client's end-callback, the flag drops through the real poll,
// waits_module == 1 with >= 1 lap; a control wait with no staged record takes
// 0 laps. Red-without-fix (scratch-build runs recorded in INTEGRATION_IMPL): with
// sifman's sceSifDmaStat constant at 0 the module's CD worker never leaves its
// post-DMA poll; with the seam's pump_module unbound the wait takes no lap --
// in both the completion assertions fail. Skips (77) without the disc image.
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/core/config/config.h"
#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/iop/iop_kernel.h"
#include "engine/iop/iop_memory.h"
#include "engine/iop/iop_module_table.h"
#include "engine/platform/host/platform.h"
#include "engine/sif/hle/ee_rpc_server.h"
#include "engine/sif/hle/main_memory_ee_ram_sink.h"
#include "engine/sif/hle/rpc_router.h"
#include "engine/sif/hle/sce_sif.h"
#include "engine/sif/sif_state.h"
#include "engine/sound/mixer.h"
#include "engine/sound/voice_source.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/hle/sif_init_statics.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h"
#include "games/slus_20909_crash_twinsanity/tests/services/music_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/stream_oracle_helpers.h"
#include "games/slus_20909_crash_twinsanity/tests/services/stream_file_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/bank_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/bank_timing_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/bank_pacing_oracle_case.h"

using namespace ps2r::iop;
using namespace stream_oracle;
using ps2r::title::stream_irx::ModuleLayout;
using ps2r::title::stream_irx::StreamIrxPlugin;
using ps2r::title::stream_irx::StreamRpcServer;

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

// The title's four allocator entry points (FUN_00181560 new, FUN_001815c0 new[],
// FUN_00181590 delete, FUN_001815f0 delete[]) over the title heap, which this rig
// does not build: a bump allocator over a scratch region answers them (16-byte
// aligned, zero-filled; frees are no-ops). Installed through the dispatch table
// the way the manifest installs its rows, so the REAL callers (FUN_002add88,
// FUN_002ad650) run unchanged.
static uint32_t g_rig_heap_next = kRigHeap;
static uint32_t g_rig_allocs = 0;
static uint32_t RigAlloc(uint32_t size) {
    const uint32_t at = g_rig_heap_next;
    const uint32_t rounded = (size + 15u) & ~15u;
    if (at + rounded > kRigHeapEnd) return 0;
    g_rig_heap_next = at + rounded;
    for (uint32_t i = 0; i < rounded; ++i) memory::write<uint8_t>(at + i, 0);
    ++g_rig_allocs;
    return at;
}
static int InstallRigAllocator() {
    int installed = 0;
    const uint32_t allocs[] = {0x181560u, 0x1815c0u};
    const uint32_t frees[] = {0x181590u, 0x1815f0u};
    for (uint32_t a : allocs) {
        installed += DispatchTable::Install(a, [](CpuContext& c, uint32_t) {
            c.cpuRegs.GPR.r[2].UD[0] = RigAlloc(c.cpuRegs.GPR.r[4].UL[0]);
        }) ? 1 : 0;
    }
    for (uint32_t f : frees) {
        installed += DispatchTable::Install(f, [](CpuContext& c, uint32_t) { c.cpuRegs.GPR.r[2].UD[0] = 0; }) ? 1 : 0;
    }
    return installed;
}

static CpuContext* g_service_ctx = nullptr;   // the EE context the bridge's service poll runs on (the root: the current guest thread's)

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("SKIP usage: %s <disc image> <OLD root> <sdk_bindings.tsv>\n", argv[0]);
        return 77;
    }
    ps2r::platform::host::RegisterSelf();
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink mmio_sink;
    static MmioRouter mmio(mmio_sink);
    static FastMem fm(ram, map, mmio);
    RecompAbi::BindMemory(&fm);
    DispatchTable::Initialize();

    IsoFs fs;
    if (!fs.Open(argv[1])) {
        std::printf("SKIP no disc image\n");
        return 77;
    }
    const std::string irx_path = std::string(argv[2]) + "/game_files/STREAM.IRX";
    std::ifstream in(irx_path, std::ios::binary);
    if (!in) {
        std::printf("SKIP %s not present\n", irx_path.c_str());
        return 77;
    }
    std::vector<uint8_t> irx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ReadEngine engine(fs);

    // The IOP: the module table (FRAME clock), the real STREAM.IRX over the disc
    // reader and the EE-RAM sink of IOP->EE SIF DMA bound to main RAM (the root's shape).
    IopModuleTable table(IopClock::Policy::Frame);
    IopMemory iop_mem;
    MainMemoryEeRamSink ee_sink(ram);
    ps2r::sound::SpuCore spu;
    StreamIrxPlugin plugin(table, iop_mem, spu, &engine, &ee_sink);
    const StreamIrxPlugin::BootReport rep = plugin.Boot(irx.data(), irx.size());
    CHECK(rep.sections.ok && rep.entry_ran && table.Booted() && rep.rpc_tid == 1, "module_boots_to_first_block");

    // The EE side: the sceSif rows (bound into the manifest and installed into the
    // dispatch table under the title's addresses), the STREAM face, the EE RPC server.
    RpcRouter router;
    SifState sif;
    SdkBindingManifest& manifest = SdkBindingManifest::Instance();
    manifest.ResetForTest();
    manifest.RegisterBuiltins();
    SceSif::IopHeap heap;
    heap.alloc = [&plugin](uint32_t size) { return plugin.Sysmem().Heap().Alloc(size); };
    heap.free = [&plugin](uint32_t addr) { plugin.Sysmem().Heap().Free(addr); };
    SceSif::Bind(manifest, router, sif, std::move(heap), [](uint32_t handle) {
        if ((handle & 0xFFFF0000u) != 0xDEAD0000u) return 0u;
        const uint32_t low = handle & 0xFFFFu;
        if (low == (ModuleLayout::kRpcServerId & 0xFFFFu)) return ModuleLayout::kRpcServerId;
        return low ? (0x80000000u | low) : 0u;
    });
    SceSif::SetInitRpcStatics(SifInitStatics::CrashTwinsanityUS());
    StreamRpcServer face(
        iop_mem, table.Rpc(), [](uint32_t a) { return memory::read<uint8_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); });
    router.RegisterServer(ModuleLayout::kRpcServerId, face, "STREAM");
    EeRpcServer ee_server(
        SceSif::RegisteredEeServer(), iop_mem, table.Rpc(), [](uint32_t a) { return memory::read<uint8_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); },
        [](uint32_t addr, CpuContext& c) { DispatchTable::Invoke(addr, c); });
    plugin.Protocol().SetEeServer(&ee_server);
    table.Rpc().SetEeServicePoll([&ee_server] { if (g_service_ctx) ee_server.Service(*g_service_ctx); });
    {
        std::vector<SdkBindingManifest::TitleRow> rows;
        std::string err;
        const bool loaded = SdkBindingManifest::LoadTitleManifest(argv[3], rows, &err);
        CHECK(loaded, ("manifest_loads " + err).c_str());
        if (!loaded) return 1;
        int traps = 0, unresolved = 0;
        const std::vector<SdkBindingManifest::Resolved> resolved = manifest.Resolve(rows);
        const int installed = manifest.Install(
            resolved, [](uint32_t a, SdkBindingManifest::Body b) { return DispatchTable::Install(a, b); },
            [](uint32_t a) { return DispatchTable::Capture(a); }, &traps, &unresolved);
        std::printf("manifest: %d rows installed (%d traps, %d not function starts)\n", installed, traps, unresolved);
        CHECK(installed > 0, "manifest_rows_installed");
    }

    // The loader seam bound as the root binds it: the disc, the completion model,
    // the dispatch table for the recompiled bodies, and the module pump lap.
    ps2r::CdvdConfig cdvd_cfg;
    cdvd_cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Boundary;   // this case tests the boundary model (the default is poll since gate S4)
    DeliveryMeter meter(cdvd_cfg);
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    uint64_t tick = 10;
    LoaderFileIo::Deps deps;
    deps.engine = &engine;
    deps.model = &model;
    deps.ring_cache = false;   // this case tests the per-record seam (loader.ring_cache=off; the Config default is on since (b) attempt 2 was KEPT; 2026-10-06)
    deps.now_tick = [&tick] { return tick; };
    deps.park_frame = [&tick] { ++tick; };
    deps.call = [](uint32_t fn, CpuContext& c) { DispatchTable::Invoke(fn, c); };
    uint32_t pump_calls = 0;
    deps.pump_module = [&table, &router, &ee_server, &pump_calls](CpuContext& c) {
        ++pump_calls;
        table.PumpFromParkLoop();
        ee_server.Service(c);
        table.PumpAfterEeService();
        router.CompletePending(
            c, [](uint32_t addr, CpuContext& cc) { DispatchTable::Invoke(addr, cc); },
            [](uint32_t a) { return memory::read<uint32_t>(a); });
    };
    LoaderFileIo::Layout layout;   // the title's real addresses
    LoaderFileIo::Bind(deps, layout);
    LoaderFileIo::ResetForTest();

    // The oracle bytes.
    IsoFs::Entry file;
    CHECK(fs.Find("CRASH6/SYS/STREAM.IRX", file), "oracle_file_on_disc");
    if (file.size == 0) return 1;
    std::vector<uint8_t> oracle((file.size + 2047) / 2048 * 2048);
    engine.ReadSectors(file.lba, static_cast<uint32_t>(oracle.size() / 2048), oracle.data());

    // The title's STREAM client: the REAL init (memset of the 0x3b0000 statics,
    // sceSifInitRpc, sceSifBindRpc(0x3b27c0, 0x12345)); the open-record latch of
    // the .data image (-1 = none staged) and the client's slot count word.
    CpuContext ctx{};
    g_service_ctx = &ctx;
    memory::write<int32_t>(layout.stream_open_cmd_addr, -1);
    memory::write<uint32_t>(kSlotMaxAddr, 0x30);
    Prep(ctx);
    DispatchTable::Invoke(0x1e11e8u, ctx);
    const RpcRouter::Binding* b = router.FindByClient(kClientRecord);
    CHECK(b && b->server_id == ModuleLayout::kRpcServerId, "client_init_binds_0x12345_at_0x3b27c0");
    // The module's init batch as the title ships it (the original runtime's RPC
    // trace of the retail boot, OLD logs/runtime_log.txt:3374): hdr 0x48, nine
    // commands -- 21 {0,0x7fff,0x7fff} x2, 42 {2}, 42 {5}, CD_READ_INIT 12 {0,0x24,0,0x28},
    // 16 {9}, 81 {0,0}, 81 {1,1}, 9 {0xffff,0} -- through the bound client.
    {
        constexpr uint32_t kInitSend = 0x00204000, kInitRecv = 0x00205000, kInitRecvBytes = 0x40;
        const uint16_t init_words[] = {0x0048, 0x0009,
                                       21, 3, 0x0000, 0x7fff, 0x7fff,   21, 3, 0x0000, 0x7fff, 0x7fff,
                                       42, 1, 0x0002,   42, 1, 0x0005,
                                       12, 4, 0x0000, 0x0024, 0x0000, 0x0028,
                                       16, 1, 0x0009,   81, 2, 0x0000, 0x0000,   81, 2, 0x0001, 0x0001,
                                       9, 2, 0xffff, 0x0000};
        for (uint32_t i = 0; i < sizeof(init_words) / 2; ++i) memory::write<uint16_t>(kInitSend + i * 2, init_words[i]);
        Prep(ctx);
        memory::write<uint32_t>(kSp, 0);
        SetArg(ctx, 4, kClientRecord); SetArg(ctx, 5, 0); SetArg(ctx, 6, 0); SetArg(ctx, 7, kInitSend);
        SetArg(ctx, 8, sizeof(init_words)); SetArg(ctx, 9, kInitRecv); SetArg(ctx, 10, kInitRecvBytes); SetArg(ctx, 11, 0);
        SceSif::CallRpc(ctx);
        CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0 && plugin.Timrman().Running(1), "cd_read_init_served_heartbeat_armed");
    }

    // The title's stream manager (FUN_002ad560: alloc(0x1c), the REAL ctor
    // FUN_002add88(mgr, 9), ready flag at +0x14, the pointer at 0x30a748), over the
    // rig's allocator; then the whole-file loader's first step (FUN_002ab160):
    // the REAL FUN_002ad650(mgr, 0x800, 0, 0) -> the fs object -- allocated, given
    // a slot by the manager, the ring-stream open (cmd 13 {slot, 0, 0x800}) staged
    // on the client's batch BEFORE the file is registered.
    CHECK(InstallRigAllocator() == 4, "rig_allocator_installed_at_the_title_allocator_entries");
    const uint32_t mgr = RigAlloc(kStreamManagerBytes);
    Prep(ctx);
    SetArg(ctx, 4, mgr); SetArg(ctx, 5, kStreamManagerCapacity);
    DispatchTable::Invoke(kFnStreamManagerCtor, ctx);
    memory::write<uint32_t>(mgr + 0x14, 1);
    memory::write<uint32_t>(kStreamManagerPtr, mgr);
    CHECK(static_cast<int16_t>(memory::read<uint16_t>(mgr)) >= static_cast<int16_t>(kStreamManagerCapacity) &&
              memory::read<uint32_t>(mgr + 0x8) != 0 && memory::read<uint32_t>(mgr + 0xc) != 0,
          "stream_manager_built_by_the_real_ctor");
    Prep(ctx);
    SetArg(ctx, 4, memory::read<uint32_t>(kStreamManagerPtr)); SetArg(ctx, 5, kRingSize); SetArg(ctx, 6, 0); SetArg(ctx, 7, 0);
    DispatchTable::Invoke(kFnStreamOpen, ctx);
    const uint32_t fs_obj = ctx.cpuRegs.GPR.r[2].UL[0];
    CHECK(fs_obj >= kRigHeap && fs_obj < kRigHeapEnd, "real_stream_open_returns_a_heap_fs");
    if (!(fs_obj >= kRigHeap && fs_obj < kRigHeapEnd)) return 1;
    const uint32_t kFs = fs_obj;
    const uint8_t kSlot = memory::read<uint8_t>(kFs);
    std::printf("stream open: fs=0x%x slot=%u flags=0x%x handle=%d manager used=%d latch=%d ring[slot]=0x%x allocs=%u\n", kFs, kSlot,
                memory::read<uint32_t>(kFs), memory::read<int32_t>(kFs + LoaderFileIo::kFsHandle),
                static_cast<int16_t>(memory::read<uint16_t>(mgr + 4)), memory::read<int32_t>(layout.stream_open_cmd_addr),
                memory::read<uint32_t>(kRingSizeTable + kSlot * 4), g_rig_allocs);
    CHECK(kSlot < layout.stream_slot_count && memory::read<uint32_t>(memory::read<uint32_t>(mgr + 0xc) + kSlot * 4) == kFs,
          "manager_assigned_the_slot_and_tabled_the_fs");
    CHECK((memory::read<uint32_t>(kFs) & kFlagRingOpen) != 0, "ring_stream_open_flag_set_on_fs");
    // FUN_001e1ca8 stages cmd 13 {slot, mode_hi, mode_lo, size_hi, size_lo} and
    // FUN_001e13b0 commits it to the client's batch (record count +1, the latch
    // back to -1): the batch holds one record, cmd 13, five params, for this slot.
    CHECK(memory::read<int32_t>(layout.stream_open_cmd_addr) == -1 && memory::read<uint32_t>(kBatchRecords) == 1 &&
              memory::read<uint16_t>(kBatchBuffer + 4) == kCmdRingStreamOpen && memory::read<uint16_t>(kBatchBuffer + 6) == 5 &&
              memory::read<uint16_t>(kBatchBuffer + 8) == kSlot && memory::read<uint16_t>(kBatchBuffer + 16) == kRingSize &&
              memory::read<uint32_t>(kRingSizeTable + kSlot * 4) == kRingSize,
          "ring_stream_open_cmd_13_staged_for_the_slot");
    CHECK(memory::read<int32_t>(kFs + LoaderFileIo::kFsHandle) == -1, "fresh_fs_has_no_handle");

    StreamOracleInputs fixture{ctx, table, iop_mem, spu, plugin, ee_sink, face,
        ee_server, router, model, meter, deps, layout, tick, pump_calls, mgr};
    g_fail += RunModelFileOracle(fixture, kFs, file, oracle);
    g_fail += RunOwnedFileOracle(fixture, kFs, kSlot, file);
    g_fail += RunBankOracle(fixture);

    // Production Poll mode processes a blocking send's reply before return.
    // The earlier Boundary-mode fixtures intentionally keep deferred replies.
    SceSif::SetBlockingEndCallbackRunner([&router](CpuContext& call) {
        router.CompletePending(call, [](uint32_t address, CpuContext& reply) {
            DispatchTable::Invoke(address, reply);
        }, [](uint32_t address) { return memory::read<uint32_t>(address); });
    });
    g_fail += RunBankTimingOracle(ctx, table, plugin, spu, face, deps);
    // Loading-phase pacing attribution on the recorded stage11 bank intents.
    g_fail += RunBankPacingOracle(ctx, table, plugin, spu, face, deps);
    // Keep production inline replies for the shared-control/music comparison,
    // including the retained service's existing logical round-trip charge.
    const auto saved_service_us = table.Rpc().ServiceUs();
    table.Rpc().SetServiceUs(160);
    g_fail += RunMusicOracle(ctx, plugin, spu, iop_mem, face, deps, tick, fs, engine);
    table.Rpc().SetServiceUs(saved_service_us);
    SceSif::SetBlockingEndCallbackRunner({});
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    g_service_ctx = nullptr;
    return g_fail ? 1 : 0;
}
