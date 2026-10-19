// LoaderCompletionOrder: the title's own async-read completion predicate
// (the REAL recompiled FUN_002adaf0 -> FUN_001e3308 = STREAM slot status, run
// through the dispatch table) over a host-served LoaderFileIo::WindowedRead:
// it must read PENDING from the issue until the DeliveryMeter delivers the
// bytes into the destination and the callback ring, and DONE only after --
// never "done" over a stale ring. Skips (77) without the disc image.
//
// Slot aging: the title's STREAM client rewrites its slot table on every
// status reply (the REAL recompiled FUN_001e1f48, the reply processor): an issued
// slot the module never lists goes 2 -> 5 and then counts down to 0 = done. A
// host-served window is never listed, so under poll visibility -- where the
// modelled drive keeps a read in flight across replies -- the issue must leave
// a byte the processor does not retire: the title's poll stays pending until the
// delivery, and reads done after it. Boundary mode's byte sequence is unchanged.
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
static constexpr uint32_t kCb = 0x00511000;        // its callback stream object {+8 base, +0xc cursor, +0x10 cap}
static constexpr uint32_t kRing = 0x00580000;      // the ring buffer
static constexpr uint32_t kDest = 0x00600000;
static constexpr uint32_t kSp = 0x00700000;
static constexpr uint8_t kSlot = 5;
static constexpr uint32_t kWindow = 0x1000;
static constexpr uint32_t kReplyBuf = 0x003b3000;      // the STREAM client's reply buffer FUN_001e1f48 reads (byte 0 = listed count)
static constexpr uint32_t kReplyCountdown = 0x003b3d30; // the client's per-slot countdown table (slot byte 5 ages through it)
static constexpr uint32_t kReplyPendingFlag = kGp - 0x78bc;   // the processor's "reply pending" word (2 = fresh)
static constexpr uint32_t kReplyLastSeq = kGp - 0x791c;        // the last processed reply's sequence word


static void Prep(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[28].UD[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UD[0] = kSp;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xDEADBEEFull;
}

// One status reply with NO slot listed, processed by the REAL FUN_001e1f48: the
// sequence word differs from the last one so the processor runs its passes.
static void ProcessUnlistedReply(CpuContext& ctx, uint32_t seq) {
    Prep(ctx);
    memory::write<uint8_t>(kReplyBuf, 0);            // zero entries listed
    memory::write<uint32_t>(kReplyBuf + 4, seq);     // entry 0's slot: the sequence word for count 0
    memory::write<uint32_t>(kReplyPendingFlag, 2);
    DispatchTable::Invoke(0x1e1f48u, ctx);
}

// The title's poll exactly as WaitReadComplete asks it: FUN_002adaf0(fs) -> pending (v0).
static uint32_t TitlePoll(CpuContext& ctx) {
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
    DispatchTable::Invoke(0x2adaf0u, ctx);
    return ctx.cpuRegs.GPR.r[2].UL[0];
}

static void IssueAsync(CpuContext& ctx, uint32_t offset) {
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = offset; ctx.cpuRegs.GPR.r[6].UD[0] = kWindow;
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
    // The poll is the REAL recompiled body; the STREAM record stagers (cmd 31/32,
    // ship) are stood in for -- the read is host-served, no record is shipped.
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
    if (irx.size == 0) { std::printf("FAILED (oracle missing)\n"); return 1; }
    std::vector<uint8_t> oracle((irx.size + 2047) / 2048 * 2048);
    engine.ReadSectors(irx.lba, static_cast<uint32_t>(oracle.size() / 2048), oracle.data());

    // The title's objects: fs {slot byte, handle, sizes, cb}, the cb ring with a
    // previous read's bytes at base and a cursor past it, the client's slot count.
    memory::write<uint32_t>(kSlotMaxAddr, 0x30);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 3);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, irx.size);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, 0);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsCbObj, kCb);
    memory::write<uint32_t>(kCb + 0x8, kRing);
    memory::write<uint32_t>(kCb + 0xc, kRing + 0x40);
    memory::write<uint32_t>(kCb + 0x10, 0x10000);
    for (uint32_t i = 0; i < kWindow; ++i) memory::write<uint8_t>(kRing + i, 0xAB);   // the stale ring
    for (uint32_t i = 0; i < kWindow; ++i) memory::write<uint8_t>(kDest + i, 0);
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
    // Before any read the title's predicate says done (slot idle, flag down).
    CHECK(TitlePoll(ctx) == 0, "idle_slot_polls_done");

    // 1. issue: pending from the issue on; nothing moved yet.
    IssueAsync(ctx, 0);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0 && model.InFlight(kFs), "async_issue_in_flight");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotIssued, "slot_byte_reads_issued_after_issue");
    CHECK(TitlePoll(ctx) != 0, "title_poll_pending_while_in_flight");
    CHECK(RangeIs(kRing, 0xAB, kWindow), "ring_base_still_holds_previous_bytes");
    CHECK(RangeIs(kDest, 0x00, kWindow), "dest_untouched_before_delivery");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing + 0x40, "cursor_unchanged_before_delivery");

    // 2. delivery at the next boundary: bytes in ring (at base) and dest, cursor
    // reset to base, THEN the slot reads done and the title's poll says 0.
    CHECK(meter.DeliverDue(tick + 1) == 1, "boundary_delivers");
    CHECK(TitlePoll(ctx) == 0, "title_poll_done_after_delivery");
    CHECK(RangeEquals(kRing, oracle, 0, kWindow), "ring_base_holds_file_bytes");
    CHECK(RangeEquals(kDest, oracle, 0, kWindow), "dest_holds_file_bytes");
    CHECK(memory::read<uint32_t>(kCb + 0xc) == kRing, "cursor_equals_base");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "slot_byte_reads_done");
    {   // consume through the seam's re-ask (the title's FUN_002ad110 completes on its own).
        IssueAsync(ctx, 0);
        CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1 && !model.InFlight(kFs), "reask_consumes_the_delivery");
    }

    // 3. the blocking wait: CompleteNow pulls bytes + status forward and the REAL
    // poll sees done through the modelled status; the flag drops.
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    for (uint32_t i = 0; i < kWindow; ++i) memory::write<uint8_t>(kRing + i, 0xCD);
    IssueAsync(ctx, 0x2000);
    CHECK(TitlePoll(ctx) != 0, "wait_path_pending_before_wait");
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
    LoaderFileIo::WaitReadComplete(ctx);
    CHECK(!model.InFlight(kFs) && model.Forced() == 1, "wait_completes_now");
    CHECK((memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending) == 0, "wait_lowers_flag_through_the_real_poll");
    CHECK(RangeEquals(kRing, oracle, 0x2000, kWindow) && RangeEquals(kDest, oracle, 0x2000, kWindow), "wait_bytes_exact");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "wait_slot_done");
    CHECK(meter.DeliverDue(tick + 1) == 1, "stale_closure_no_op");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "stale_closure_leaves_status");

    // 4. discrimination: the predicate depends on the slot byte alone -- with the
    // "issued" write undone, the title says done over a stale ring.
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    for (uint32_t i = 0; i < kWindow; ++i) memory::write<uint8_t>(kRing + i, 0xEF);
    IssueAsync(ctx, 0x3000);
    CHECK(TitlePoll(ctx) != 0, "discriminator_pending_with_status_written");
    memory::write<uint8_t>(layout.stream_slot_status_addr + kSlot, LoaderFileIo::kSlotDone);   // what a seam without the model leaves
    CHECK(model.InFlight(kFs) && TitlePoll(ctx) == 0 && RangeIs(kRing, 0xEF, kWindow),
          "discriminator_without_status_title_says_done_over_stale_ring");
    CHECK(meter.DeliverDue(tick + 1) == 1 && RangeEquals(kRing, oracle, 0x3000, kWindow), "discriminator_delivery_still_lands");

    const LoaderFileIo::Census& c = LoaderFileIo::Stats();
    CHECK(c.async_issued == 3 && c.slot_issued == 3 && c.slot_done == 3, "census_slot_status_writes");

    // 5. slot aging, boundary mode (the frozen lanes): the issue writes "issued"
    // (2) and the REAL reply processor ages it 2 -> 5 -> countdown -> 0 = done
    // after four unlisted replies -- the title's client's own timeout, left as it
    // is in boundary mode (a window there is delivered before it can fire).
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    memory::write<uint8_t>(kReplyCountdown + kSlot, 3);   // a previous module-served record's 1 -> 0 left the countdown at 3
    memory::write<uint32_t>(kReplyLastSeq, 0);
    IssueAsync(ctx, 0x4000);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotIssued, "aging_boundary_issue_writes_issued");
    ProcessUnlistedReply(ctx, 1);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotQueued && TitlePoll(ctx) != 0, "aging_boundary_first_reply_queued_pending");
    ProcessUnlistedReply(ctx, 2);
    ProcessUnlistedReply(ctx, 3);
    CHECK(TitlePoll(ctx) != 0, "aging_boundary_third_reply_still_pending");
    ProcessUnlistedReply(ctx, 4);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone && model.InFlight(kFs) && TitlePoll(ctx) == 0,
          "aging_boundary_fourth_reply_retires_the_slot_while_in_flight");
    CHECK(meter.DeliverDue(tick + 1) == 1 && !model.InFlight(kFs), "aging_boundary_delivery_still_lands");
    {   // consume the delivery so the object is free
        IssueAsync(ctx, 0x4000);
        CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1, "aging_boundary_reask_consumes");
    }

    // 6. slot aging, poll visibility: the modelled drive keeps the read in flight
    // across the replies; the issue marks the slot served (outside the client's
    // state set), the processor's passes leave it, the title's poll stays pending
    // until the poll that lands the read, and reads done after it.
    ps2r::CdvdConfig poll_cfg;
    poll_cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    DeliveryMeter poll_meter(poll_cfg);
    LoaderModel poll_model(poll_meter, budget);
    uint64_t poll_tick = 100;
    poll_model.SetNowTick([&poll_tick] { return poll_tick; });
    deps.model = &poll_model;
    LoaderFileIo::Bind(deps, layout);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5 | kSlot);
    memory::write<uint8_t>(kReplyCountdown + kSlot, 3);
    memory::write<uint8_t>(layout.stream_slot_status_addr + kSlot, 0);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, 0);
    tick = poll_tick;
    IssueAsync(ctx, 0x5000);   // a long seek from the drive's head: several frames of drive time
    CHECK(poll_model.InFlight(kFs), "aging_poll_issue_in_flight");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotServed, "aging_poll_issue_writes_served");
    CHECK(TitlePoll(ctx) != 0, "aging_poll_pending_after_issue");
    for (uint32_t r = 1; r <= 6; ++r) ProcessUnlistedReply(ctx, 10 + r);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotServed, "aging_poll_six_replies_leave_the_served_byte");
    CHECK(memory::read<uint8_t>(kReplyCountdown + kSlot) == 3, "aging_poll_countdown_untouched");
    CHECK(poll_model.InFlight(kFs) && TitlePoll(ctx) != 0, "aging_poll_still_pending_while_in_flight");
    CHECK(poll_model.Poll(kFs) == LoaderModel::PollOutcome::HeldInflight, "aging_poll_model_holds_before_the_drive_end");
    // The frame that contains the drive's end: the poll lands it, then the byte reads done.
    while (poll_model.InFlight(kFs) && poll_tick < 100 + 600) {
        ++poll_tick;
        if (poll_model.Poll(kFs) == LoaderModel::PollOutcome::Progress) break;
    }
    CHECK(!poll_model.InFlight(kFs) && poll_tick > 100, "aging_poll_read_lands_at_a_later_frame");
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "aging_poll_delivery_writes_done");
    CHECK(TitlePoll(ctx) == 0, "aging_poll_title_poll_done_after_delivery");
    CHECK(RangeEquals(kDest, oracle, 0x5000, kWindow), "aging_poll_dest_holds_file_bytes");
    ProcessUnlistedReply(ctx, 20);
    CHECK(LoaderFileIo::SlotStatus(kFs) == LoaderFileIo::kSlotDone, "aging_poll_done_stays_done");
    deps.model = &model;
    LoaderFileIo::Bind(deps, layout);
    std::printf("census: async_issued=%llu slot_issued=%llu slot_done=%llu\n", (unsigned long long)c.async_issued,
                (unsigned long long)c.slot_issued, (unsigned long long)c.slot_done);
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
