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
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/sif_init_statics.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h"

using namespace ps2r::iop;
using ps2r::title::stream_irx::ModuleLayout;
using ps2r::title::stream_irx::StreamIrxPlugin;
using ps2r::title::stream_irx::StreamRpcServer;

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

static constexpr uint32_t kGp = 0x003114f0;              // the title's $gp (the STREAM client statics are $gp-relative)
static constexpr uint32_t kSlotMaxAddr = kGp - 0x794c;   // the client's slot count word FUN_001e3308 bounds the slot by
static constexpr uint32_t kHandleCounter = kGp - 0x74d8; // the loader's handle counter word
static constexpr uint32_t kClientRecord = 0x003b27c0;    // the STREAM client's sceSifBindRpc record (FUN_001e11e8)
static constexpr uint32_t kStreamManagerPtr = 0x0030a748; // the title's stream-manager pointer (FUN_002ad560 stores it; FUN_002ab160 loads it)
static constexpr uint32_t kRingSizeTable = 0x002e7740;   // the client's per-slot ring size words FUN_001e1ca8 fills (slot * 4)
static constexpr uint32_t kFnStreamManagerCtor = 0x2add88;   // FUN_002add88(mgr, capacity)
static constexpr uint32_t kFnStreamOpen = 0x2ad650;      // FUN_002ad650(mgr, ring_size, 0, 0) -> fs
static constexpr uint32_t kStreamManagerCapacity = 9;    // FUN_002ad560's argument
static constexpr uint32_t kStreamManagerBytes = 0x1c;    // FUN_002ad560: alloc(0x1c) then the ctor
static constexpr uint32_t kRingSize = 0x800;             // FUN_002ab160: FUN_002ad650(mgr, 0x800, 0, 0)
static constexpr uint32_t kFlagRingOpen = 0x00020000u;   // FUN_002ad7c8 sets it once the ring stream is staged
static constexpr uint32_t kCmdRingStreamOpen = 13;       // STREAM cmd 13 = open a ring stream for the slot
static constexpr uint32_t kCmdRegisterFile = 31;         // STREAM cmd 31 = register a file handle (the seam's Open)
static constexpr uint32_t kBatchRecords = kGp - 0x78f8;  // the client's staged-record count FUN_001e13b0 bumps on commit
static constexpr uint32_t kBatchBuffer = 0x003b17c0;     // the client's batch buffer {u16 hdr, u16 count, records...}
static constexpr uint32_t kFsControl = 0x00512000;       // a second fs object with nothing staged
static constexpr uint32_t kDest = 0x00600000;
static constexpr uint32_t kSp = 0x00700000;
static constexpr uint32_t kRigHeap = 0x00800000;         // the test-local allocator's region (the title heap stands in)
static constexpr uint32_t kRigHeapEnd = 0x00900000;
static constexpr uint8_t kControlSlot = 7;
static constexpr uint32_t kReadBytes = 0x800;            // one sector of the file: the module's DMA chunk

static void Prep(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[28].UD[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UD[0] = kSp;
    ctx.cpuRegs.GPR.r[31].UD[0] = 0x00100100;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xDEADBEEFull;
}
static void SetArg(CpuContext& ctx, int reg, uint32_t v) { ctx.cpuRegs.GPR.r[reg].UD[0] = v; }

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

// The title's poll exactly as WaitReadComplete asks it: FUN_002adaf0(fs) -> pending (v0).
static uint32_t TitlePoll(CpuContext& ctx, uint32_t fs) {
    Prep(ctx);
    SetArg(ctx, 4, fs);
    DispatchTable::Invoke(0x2adaf0u, ctx);
    return ctx.cpuRegs.GPR.r[2].UL[0];
}

static bool RangeEquals(uint32_t addr, const std::vector<uint8_t>& oracle, uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (memory::read<uint8_t>(addr + i) != oracle[off + i]) return false;
    return true;
}
static bool RangeIs(uint32_t addr, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (memory::read<uint8_t>(addr + i) != v) return false;
    return true;
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
    IopKernel& k = table.Kernel();

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

    // Open through the seam: handle 3 -> the file's extent, cmd 31 staged behind cmd 13.
    {
        const char* path = "crash6\\sys\\stream.irx";
        const uint32_t path_addr = 0x00500000;
        for (uint32_t i = 0; i <= std::strlen(path); ++i) memory::write<uint8_t>(path_addr + i, static_cast<uint8_t>(path[i]));
        memory::write<int32_t>(kHandleCounter, 3);
        Prep(ctx);
        SetArg(ctx, 4, path_addr);
        LoaderFileIo::Open(ctx);
        CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == 3, "open_handle_3");
        // The retail cadence: the size query ships the staged records (cmd 13,
        // then cmd 31) before any read record is staged, so the module has the
        // ring stream and knows the handle.
        memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 3);
        Prep(ctx);
        SetArg(ctx, 4, kFs);
        LoaderFileIo::FileSize(ctx);
        CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == file.size && face.Calls() == 2, "file_size_ships_the_open_records_first");
        // The shipped batch is {13, 31, 9}: the ring-stream open, the file
        // registration, then the status record Ship(0) appends to every call.
        const std::vector<uint16_t>& ids = face.CommandIds();
        std::printf("stream cmds served: %s\n", face.CommandIdsCsv().c_str());
        size_t at13 = ids.size(), at31 = ids.size();
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == kCmdRingStreamOpen) at13 = i;
            if (ids[i] == kCmdRegisterFile) at31 = i;
        }
        CHECK(at13 < ids.size() && at31 == at13 + 1 && memory::read<uint32_t>(kBatchRecords) == 0,
              "module_served_cmd_13_then_cmd_31_the_retail_order");
    }
    // The fs objects: the real one keeps its slot/flags; the file size as the
    // title stores it (fs+0x30); the control fs has its own idle slot.
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, file.size);
    memory::write<uint32_t>(kFsControl + LoaderFileIo::kFsFlags, kControlSlot);
    memory::write<int32_t>(kFsControl + LoaderFileIo::kFsHandle, 3);
    memory::write<uint32_t>(kFsControl + LoaderFileIo::kFsCbObj, 0);
    for (uint32_t i = 0; i < kReadBytes + 16; ++i) memory::write<uint8_t>(kDest + i, 0x5A);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone && TitlePoll(ctx, kFs) == 0, "idle_slot_polls_done");

    // The title stages the record itself: FUN_002ad9a8(fs, pos 0, size, dest) --
    // FlushCache, FUN_001e2ee0(handle, pos, size), FUN_001e69a8(0x7e, slot, handle, dest);
    // the client marks the slot "issued" (2). No host read is in flight.
    Prep(ctx);
    SetArg(ctx, 4, kFs); SetArg(ctx, 5, 0); SetArg(ctx, 6, kReadBytes); SetArg(ctx, 7, kDest);
    DispatchTable::Invoke(0x2ad9a8u, ctx);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotIssued, "title_staged_record_marks_slot_issued");
    CHECK(!model.InFlight(kFs), "no_host_read_in_flight");
    CHECK(LoaderFileIo::SlotModuleOwned(kFs), "slot_is_module_owned");
    CHECK(TitlePoll(ctx, kFs) != 0, "title_poll_pending_before_wait");
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, memory::read<uint32_t>(kFs) | LoaderFileIo::kFlagReadPending);
    const uint64_t face_calls_before = face.Calls();

    // The wait: the seam ships the staged records, yields to the IOP in bounded
    // laps, and the title's own poll answers done once the module's reply
    // retired the slot through the client's end-callback.
    Prep(ctx);
    SetArg(ctx, 4, kFs);
    LoaderFileIo::WaitReadComplete(ctx);
    const LoaderFileIo::Census& c = LoaderFileIo::Stats();
    std::printf("wait: waits=%llu waits_module=%llu module_laps=%llu pump_calls=%u face_calls=%u->%u bridge submitted=%llu completed=%llu stalls=%llu sifman bytes=%llu\n",
                (unsigned long long)c.waits, (unsigned long long)c.waits_module, (unsigned long long)c.module_laps, pump_calls,
                (unsigned)face_calls_before, face.Calls(), (unsigned long long)table.Rpc().Submitted(),
                (unsigned long long)table.Rpc().Completed(), (unsigned long long)table.Rpc().Stalls(),
                (unsigned long long)ee_sink.Bytes());
    std::printf("slot byte after wait: %d; tid1 %d/%d tid2 %d/%d tid3 %d/%d tid4 %d/%d; cdvdman reads=%u sectors=%llu callbacks=%u search_hits=%u; protocol calls=%u bridged=%u; dest[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                LoaderFileIo::SlotStatus(kFs), (int)k.ThreadState(1), (int)k.ThreadWaitKind(1), (int)k.ThreadState(2), (int)k.ThreadWaitKind(2),
                (int)k.ThreadState(3), (int)k.ThreadWaitKind(3), (int)k.ThreadState(4), (int)k.ThreadWaitKind(4),
                plugin.Cdvdman().ReadsIssued(), (unsigned long long)plugin.Cdvdman().SectorsRead(), plugin.Cdvdman().CallbacksFired(),
                plugin.Cdvdman().SearchHits(), plugin.Protocol().Calls(), plugin.Protocol().Bridged(),
                memory::read<uint8_t>(kDest), memory::read<uint8_t>(kDest + 1), memory::read<uint8_t>(kDest + 2), memory::read<uint8_t>(kDest + 3),
                memory::read<uint8_t>(kDest + 4), memory::read<uint8_t>(kDest + 5), memory::read<uint8_t>(kDest + 6), memory::read<uint8_t>(kDest + 7));
    CHECK(c.waits_module == 1 && c.module_laps >= 1, "wait_yielded_to_the_module_at_least_one_lap");
    if (LoaderFileIo::SlotStatus(kFs) != LoaderFileIo::kSlotDone) {
        // Diagnostic (a failing pin): does the module start the job at frame boundaries instead?
        for (int i = 0; i < 12; ++i) {
            table.PumpAtBoundary();
            ee_server.Service(ctx);
            table.PumpAfterEeService();
            router.CompletePending(ctx, [](uint32_t addr, CpuContext& cc) { DispatchTable::Invoke(addr, cc); },
                                   [](uint32_t a) { return memory::read<uint32_t>(a); });
            Prep(ctx);
            SetArg(ctx, 4, kFs);
            LoaderFileIo::WaitReadComplete(ctx);
            std::printf("  boundary %d: reads=%u callbacks=%u sifman bytes=%llu slot=%d tid1 %d/%d tid2 %d/%d protocol calls=%u face calls=%u dest[0]=%02x\n", i,
                        plugin.Cdvdman().ReadsIssued(), plugin.Cdvdman().CallbacksFired(), (unsigned long long)ee_sink.Bytes(),
                        LoaderFileIo::SlotStatus(kFs), (int)k.ThreadState(1), (int)k.ThreadWaitKind(1), (int)k.ThreadState(2),
                        (int)k.ThreadWaitKind(2), plugin.Protocol().Calls(), face.Calls(), memory::read<uint8_t>(kDest));
        }
    }
    CHECK(pump_calls == c.module_laps, "every_lap_ran_the_bound_pump");
    CHECK(face.Calls() > face_calls_before, "the_seam_shipped_the_staged_records_to_the_module");
    CHECK(table.Rpc().Stalls() == 0, "no_bridge_drive_stall");
    CHECK(RangeEquals(kDest, oracle, 0, kReadBytes), "dest_holds_the_file_bytes_through_sif_dma");
    CHECK(RangeIs(kDest + kReadBytes, 0x5A, 16), "dest_guard_intact");
    CHECK(ee_sink.Bytes() >= kReadBytes, "sifman_dma_landed_the_bytes_in_ee_ram");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "slot_byte_retired_by_the_client_end_callback");
    CHECK(TitlePoll(ctx, kFs) == 0, "real_poll_answers_done");
    CHECK((memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending) == 0, "wait_lowered_the_flag_through_the_real_poll");
    CHECK(model.Forced() == 0, "no_host_completion_was_forced");
    CHECK(k.UnparkedCount() == 0, "module_threads_parked_after_the_wait");

    // Control: a wait with the flag up and nothing staged on an idle slot -- the
    // poll answers done at once, no lap.
    const uint64_t laps_before = c.module_laps, waits_module_before = c.waits_module;
    memory::write<uint32_t>(kFsControl + LoaderFileIo::kFsFlags, kControlSlot | LoaderFileIo::kFlagReadPending);
    Prep(ctx);
    SetArg(ctx, 4, kFsControl);
    LoaderFileIo::WaitReadComplete(ctx);
    CHECK(c.module_laps == laps_before && c.waits_module == waits_module_before, "control_without_a_record_takes_no_lap");
    CHECK((memory::read<uint32_t>(kFsControl) & LoaderFileIo::kFlagReadPending) == 0, "control_flag_lowered_by_the_poll");
    CHECK(iop_mem.UnroutedCount() == 0, "no_iop_access_outside_iop_ram");

    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    g_service_ctx = nullptr;
    return g_fail ? 1 : 0;
}
