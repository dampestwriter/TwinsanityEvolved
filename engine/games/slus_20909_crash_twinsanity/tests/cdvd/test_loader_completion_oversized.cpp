// LoaderCompletionOversized: the title's own completion (the REAL recompiled
// FUN_002ad110 -> FUN_002adaf0 -> FUN_001e3308 -> cb vtbl+0x14 = FUN_002b5dd8, run
// through the dispatch table) over a host-served LoaderFileIo::WindowedRead whose
// window is at least the callback ring's capacity. Retail FUN_002aac80 DMAs such a
// window straight to dest and stages no ring copy (fs+0x10 stays 0), so after the
// completion dest holds the file bytes and the ring/cursor are untouched; a window
// below the capacity still goes through the ring (fs+0x10/+0x18 staged, cursor
// rewound) and the completion copy advances the cursor by size.
// Skips (77) without the disc image.
#include <cstdio>
#include <cstring>
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
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

static constexpr uint32_t kGp = 0x003114f0;        // the title's $gp (the STREAM client statics are $gp-relative)
static constexpr uint32_t kSlotMaxAddr = kGp - 0x794c;   // the client's slot count word FUN_001e3308 bounds the slot by
static constexpr uint32_t kFs = 0x00510000;        // the loader's fs object
static constexpr uint32_t kCb = 0x00511000;        // its callback stream object {+0 vtable, +8 base, +0xc cursor, +0x10 cap}
static constexpr uint32_t kVtable = 0x00306b50;    // the stream class's vtable (retail .data; +0x10 adj, +0x14 copy-out)
static constexpr uint32_t kCopyOut = 0x002b5dd8;   // FUN_002b5dd8 = memcpy(dest, cursor, n); cursor += n
static constexpr uint32_t kCompletion = 0x002ad110;
static constexpr uint32_t kRing = 0x00580000;      // the ring buffer
static constexpr uint32_t kCap = 0x1000;           // its capacity
static constexpr uint32_t kDest = 0x00600000;
static constexpr uint32_t kSp = 0x00700000;
static constexpr uint8_t kSlot = 5;
static constexpr uint32_t kBig = 0x3000;           // a window three rings long
static constexpr uint32_t kSmall = 0x800;          // a window below the capacity

static void Prep(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[28].UD[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UD[0] = kSp;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xDEADBEEFull;
}

// The title's completion exactly as its job poll asks it: FUN_002ad110(fs) -> fs+2 & 1.
static uint32_t TitleComplete(CpuContext& ctx) {
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
    DispatchTable::Invoke(kCompletion, ctx);
    return ctx.cpuRegs.GPR.r[2].UL[0];
}

static void IssueAsync(CpuContext& ctx, uint32_t offset, uint32_t size) {
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = offset; ctx.cpuRegs.GPR.r[6].UD[0] = size;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 0; ctx.cpuRegs.GPR.r[9].UD[0] = 0;
    LoaderFileIo::WindowedRead(ctx);
}

static bool RangeIs(uint32_t addr, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (memory::read<uint8_t>(addr + i) != v) return false;
    return true;
}
static bool RangeEquals(uint32_t addr, const std::vector<uint8_t>& oracle, uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (memory::read<uint8_t>(addr + i) != oracle[off + i]) return false;
    return true;
}
static void Fill(uint32_t addr, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) memory::write<uint8_t>(addr + i, v);
}

int main(int argc, char** argv) {
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink sink;
    static MmioRouter mmio(sink);
    static FastMem fm(ram, map, mmio);
    RecompAbi::BindMemory(&fm);
    DispatchTable::Initialize();

    IsoFs fs;
    if (argc < 2 || !fs.Open(argv[1])) {
        std::printf("SKIP no disc image\n");
        return 77;
    }
    ReadEngine engine(fs);
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
    LoaderFileIo::Layout layout;
    deps.call = [&layout](uint32_t fn, CpuContext& c) {
        if (fn == layout.fn_poll) DispatchTable::Invoke(fn, c);
        else c.cpuRegs.GPR.r[2].UD[0] = 0;
    };
    layout.stream_open_cmd_addr = 0x00530000;
    layout.stream_batch_count_addr = 0x00530010;
    memory::write<int32_t>(layout.stream_open_cmd_addr, -1);
    memory::write<uint16_t>(layout.stream_batch_count_addr, 0);
    LoaderFileIo::Bind(deps, layout);
    LoaderFileIo::ResetForTest();

    IsoFs::Entry irx;
    CHECK(fs.Find("CRASH6/SYS/STREAM.IRX", irx), "oracle_file_on_disc");
    if (irx.size < 0x4000) { std::printf("FAILED (oracle missing or too small)\n"); return 1; }
    std::vector<uint8_t> oracle((irx.size + 2047) / 2048 * 2048);
    engine.ReadSectors(irx.lba, static_cast<uint32_t>(oracle.size() / 2048), oracle.data());

    // The title's objects: fs {slot byte, handle, sizes, cb}, the stream object
    // over a ring with a previous read's bytes and a cursor past base, the stream
    // class's vtable words the completion dereferences, the client's slot count.
    memory::write<uint32_t>(kSlotMaxAddr, 0x30);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 3);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, irx.size);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, 0);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsCbObj, kCb);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsDest, 0);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSize, 0);
    memory::write<uint32_t>(kCb + 0x0, kVtable);
    memory::write<uint32_t>(kCb + 0x8, kRing);
    memory::write<uint32_t>(kCb + 0xc, kRing + 0x40);
    memory::write<uint32_t>(kCb + 0x10, kCap);
    memory::write<uint16_t>(kVtable + 0x10, 0);
    memory::write<uint32_t>(kVtable + 0x14, kCopyOut);
    Fill(kRing, 0xAB, kCap);            // the stale ring
    Fill(kRing + kCap, 0xCD, kBig);     // what lies past the ring's end
    Fill(kDest, 0, kBig);
    memory::write<uint8_t>(layout.stream_slot_status_addr + kSlot, 0);
    {   // Open through the seam so handle 3 maps to the file's extent.
        const char* path = "crash6\\sys\\stream.irx";
        const uint32_t path_addr = 0x00500000;
        for (uint32_t i = 0; i <= std::strlen(path); ++i) memory::write<uint8_t>(path_addr + i, static_cast<uint8_t>(path[i]));
        memory::write<int32_t>(kGp - 0x74d8, 3);
        CpuContext o{};
        Prep(o);
        o.cpuRegs.GPR.r[4].UD[0] = path_addr;
        LoaderFileIo::Open(o);
        CHECK(o.cpuRegs.GPR.r[2].SL[0] == 3, "open_handle_3");
    }

    CpuContext ctx{};

    // 1. an oversized window (size >= cap): issued straight to dest -- nothing staged
    // for the completion copy, the ring and its cursor untouched, the slot issued.
    IssueAsync(ctx, 0, kBig);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0 && model.InFlight(kFs), "big_issue_in_flight");
    CHECK((memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending) != 0, "big_flag_raised");
    CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0, "big_no_copy_dest_staged");
    CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsSize) == 0, "big_no_copy_size_staged");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotIssued, "big_slot_issued");
    CHECK(TitleComplete(ctx) == 1, "big_title_completion_pending_while_in_flight");
    CHECK(RangeIs(kDest, 0, kBig), "big_dest_untouched_before_delivery");

    // 2. delivery: dest holds all the file bytes; the ring still holds the previous
    // read, the cursor did not move, the slot reads done.
    CHECK(meter.DeliverDue(tick + 1) == 1, "big_boundary_delivers");
    CHECK(RangeEquals(kDest, oracle, 0, kBig), "big_dest_holds_file_bytes");
    CHECK(RangeIs(kRing, 0xAB, kCap) && RangeIs(kRing + kCap, 0xCD, kBig), "big_ring_untouched");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing + 0x40, "big_cursor_untouched");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "big_slot_done");

    // 3. the title's own completion: done -> no ring copy (fs+0x10 == 0) -> the flag
    // drops; dest is STILL the file bytes (a ring copy of kBig would have put the
    // 0xCD bytes past the ring's end into dest+kCap..), the cursor still where it was.
    CHECK(TitleComplete(ctx) == 0, "big_title_completion_done");
    CHECK((memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending) == 0, "big_completion_lowers_flag");
    CHECK(RangeEquals(kDest, oracle, 0, kBig), "big_dest_still_file_bytes_after_completion");
    CHECK(!RangeIs(kDest + kCap, 0xCD, 16), "big_no_ring_tail_in_dest");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing + 0x40, "big_cursor_untouched_after_completion");
    {   // the seam's re-ask consumes the delivered window
        IssueAsync(ctx, 0, kBig);
        CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1 && !model.InFlight(kFs), "big_reask_consumes");
    }

    // 4. a window below the capacity still goes through the ring: staged copy, ring
    // at base + cursor rewound at delivery, and the REAL completion copies size bytes
    // from the cursor into dest and advances it by size.
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    Fill(kRing, 0xAB, kCap);
    Fill(kDest, 0, kBig);
    IssueAsync(ctx, 0x2000, kSmall);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0 && model.InFlight(kFs), "small_issue_in_flight");
    CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == kDest && memory::read<uint32_t>(kFs + LoaderFileIo::kFsSize) == kSmall,
          "small_copy_staged");
    CHECK(TitleComplete(ctx) == 1, "small_title_completion_pending_while_in_flight");
    CHECK(meter.DeliverDue(tick + 1) == 1, "small_boundary_delivers");
    CHECK(RangeEquals(kRing, oracle, 0x2000, kSmall) && RangeIs(kRing + kSmall, 0xAB, kCap - kSmall), "small_ring_base_holds_window");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing, "small_cursor_rewound");
    CHECK(TitleComplete(ctx) == 0, "small_title_completion_done");
    CHECK(RangeEquals(kDest, oracle, 0x2000, kSmall) && RangeIs(kDest + kSmall, 0, kBig - kSmall), "small_dest_holds_window_only");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing + kSmall, "small_cursor_advanced_by_size");
    CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0, "small_completion_clears_staged_dest");
    CHECK((memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending) == 0, "small_completion_lowers_flag");

    // 5. a window exactly the capacity is oversized on retail (size < cap is false).
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSize, 0);
    IssueAsync(ctx, 0x1000, kCap);
    CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0 && memory::read<uint32_t>(kFs + LoaderFileIo::kFsSize) == 0,
          "equal_to_cap_goes_direct");
    CHECK(meter.DeliverDue(tick + 1) == 1 && RangeEquals(kDest, oracle, 0x1000, kCap), "equal_to_cap_dest_holds_file_bytes");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing + kSmall, "equal_to_cap_cursor_untouched");

    const LoaderFileIo::Census& c = LoaderFileIo::Stats();
    CHECK(c.async_issued == 3 && c.async_direct == 2 && c.slot_issued == 3 && c.slot_done == 3, "census_direct_windows");
    std::printf("census: async_issued=%llu async_direct=%llu slot_issued=%llu slot_done=%llu\n", (unsigned long long)c.async_issued,
                (unsigned long long)c.async_direct, (unsigned long long)c.slot_issued, (unsigned long long)c.slot_done);
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
