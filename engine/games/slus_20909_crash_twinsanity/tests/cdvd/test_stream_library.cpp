#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/core/config/config.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

#include "tests/cdvd/stream_library_fixture.h"
#include "tests/cdvd/stream_persistence_case.h"

using namespace stream_fixture;
namespace {
int failures = 0;
#define CHECK(condition, name) do { \
    if (!(condition)) { std::printf("FAIL %s\n", name); ++failures; } \
    else std::printf("PASS %s\n", name); \
} while (false)

}

int main() {
    MainMemory ram;
    Scratchpad spad;
    AddressMap map(ram, spad);
    MmioSink sink;
    MmioRouter mmio(sink);
    FastMem memory(ram, map, mmio);
    RecompAbi::BindMemory(&memory);
    LoaderFileIo::ResetForTest();
    std::map<uint32_t, StreamLibrary::Body> dispatch;
    std::vector<uint32_t> originals, calls;
    std::vector<int32_t> registrations;
    int module_pumps = 0;
    for (const uint32_t address : {kStart, kPoll, kWait, kBank})
        dispatch[address] = [&, address](CpuContext& ctx, uint32_t entry) {
            originals.push_back(address);
            if (address == kPoll) ctx.cpuRegs.GPR.r[2].UD[0] = 1;
            if (address == kBank) {
                CHECK(entry == kBank && ctx.cpuRegs.GPR.r[4].UL[0] == kFs &&
                          ctx.cpuRegs.GPR.r[5].UL[0] == 123 && ctx.cpuRegs.GPR.r[6].UL[0] == 456 &&
                          ctx.cpuRegs.GPR.r[7].UL[0] == 789, "audio_continuation_receives_original_arguments");
                CHECK(!registrations.empty() && StreamLibrary::FileRegistered(
                          memory::read<int32_t>(kFs + LoaderFileIo::kFsHandle)),
                      "audio_continuation_has_registered_backing_file");
            }
        };
    auto capture = [&](uint32_t address) { return dispatch[address]; };
    auto install = [&](uint32_t address, StreamLibrary::Body body) { dispatch[address] = std::move(body); return true; };
    CHECK(!StreamLibrary::Install([](uint32_t) { return StreamLibrary::Body{}; }, install),
          "missing_binding_fails_without_enabling_file_retirement");
    CHECK(!StreamLibrary::Install(capture, [&](uint32_t address, StreamLibrary::Body body) {
        if (address == kWait) return false;
        return install(address, std::move(body));
    }) && !StreamLibrary::Enabled(), "partial_install_rolls_back_and_keeps_registration");
    CHECK(StreamLibrary::Install(capture, install), "all_four_live_file_continuations_installed");
    CHECK(StreamLibrary::Install(capture, install, false) && !StreamLibrary::Enabled(),
          "startup_toggle_selects_model_in_same_binary");
    CpuContext toggle_ctx{};
    dispatch[kStart](toggle_ctx, kStart);
    CHECK(originals == std::vector<uint32_t>{kStart}, "disabled_adapter_runs_captured_model_body");
    originals.clear();
    CHECK(StreamLibrary::Install(capture, install, true) && StreamLibrary::Enabled(),
          "startup_toggle_selects_owned_file_service_in_same_binary");

    const auto path = Disc();
    IsoFs iso;
    if (!iso.Open(path)) { std::printf("FAIL fixture_disc\n"); unlink(path.c_str()); return 1; }
    ReadEngine engine(iso);
    ps2r::CdvdConfig cfg;
    cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    DeliveryMeter meter(cfg);
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    uint64_t tick = 10;
    model.SetNowTick([&] { return tick; });
    model.SetParkFrame([&] { ++tick; });
    LoaderFileIo::Layout layout;
    LoaderFileIo::Deps deps;
    deps.engine = &engine; deps.model = &model; deps.ring_cache = true;
    deps.now_tick = [&] { return tick; };
    deps.park_frame = [&] { ++tick; };
    deps.pump_module = [&](CpuContext&) { ++module_pumps; };
    deps.call = [&](uint32_t address, CpuContext& ctx) {
        calls.push_back(address);
        if (address == layout.fn_register_file) {
            registrations.push_back(ctx.cpuRegs.GPR.r[4].SL[0]);
            // Guest callees may clobber caller-saved arguments.
            ctx.cpuRegs.GPR.r[7].UD[0] = 0xBAD;
        } else if (address == kCopy) {
            const auto cb = ctx.cpuRegs.GPR.r[4].UL[0], dst = ctx.cpuRegs.GPR.r[5].UL[0],
                       count = ctx.cpuRegs.GPR.r[6].UL[0];
            const auto cursor = memory::read<uint32_t>(cb + LoaderFileIo::kCbCursor);
            for (uint32_t i = 0; i < count; ++i) memory::write<uint8_t>(dst + i, memory::read<uint8_t>(cursor + i));
            memory::write<uint32_t>(cb + LoaderFileIo::kCbCursor, cursor + count);
        } else if (dispatch.count(address)) dispatch[address](ctx, address);
    };
    LoaderFileIo::Bind(deps, layout);
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[28].UD[0] = 0x400000;
    ctx.cpuRegs.GPR.r[29].UD[0] = 0x700000;
    ctx.cpuRegs.GPR.r[31].UD[0] = 0x123456;
    memory::write<int32_t>(0x400000 + layout.counter_gp_offset, 100);
    auto open = [&](const char* name) {
        Text(kPath, name); ctx.cpuRegs.GPR.r[4].UD[0] = kPath; LoaderFileIo::Open(ctx);
        return ctx.cpuRegs.GPR.r[2].SL[0];
    };
    auto setup = [&](uint32_t fs, int32_t handle, bool ring) {
        Fill(fs, 0x48, 0);
        memory::write<uint32_t>(fs, fs == kFs ? 2 : 3);
        memory::write<int32_t>(fs + LoaderFileIo::kFsHandle, handle);
        memory::write<uint32_t>(fs + LoaderFileIo::kFsFileSize, kSize);
        memory::write<uint32_t>(fs + LoaderFileIo::kFsCbObj, ring ? kCb : 0);
        memory::write<uint32_t>(kCb, kVtbl);
        memory::write<uint32_t>(kVtbl + 0x14, kCopy);
        memory::write<uint16_t>(kVtbl + 0x10, 0);
        memory::write<uint32_t>(kCb + 8, kRing);
        memory::write<uint32_t>(kCb + 12, kRing);
        memory::write<uint32_t>(kCb + 16, 256);
    };
    auto raw = [&](uint32_t fs, uint32_t offset, uint32_t count, uint32_t dest) {
        ctx.cpuRegs.GPR.r[4].UD[0] = fs; ctx.cpuRegs.GPR.r[5].UD[0] = offset;
        ctx.cpuRegs.GPR.r[6].UD[0] = count; ctx.cpuRegs.GPR.r[7].UD[0] = dest;
        memory::write<uint32_t>(fs, memory::read<uint32_t>(fs) | LoaderFileIo::kFlagReadPending);
        dispatch[kStart](ctx, kStart);
    };
    auto wait = [&](uint32_t fs) {
        ctx.cpuRegs.GPR.r[4].UD[0] = fs; LoaderFileIo::WaitReadComplete(ctx);
    };
    auto window = [&](uint32_t offset, uint32_t count, uint32_t dest, bool sync) {
        ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = offset;
        ctx.cpuRegs.GPR.r[6].UD[0] = count; ctx.cpuRegs.GPR.r[7].UD[0] = dest;
        ctx.cpuRegs.GPR.r[8].UD[0] = sync; ctx.cpuRegs.GPR.r[9].UD[0] = kStatus;
        LoaderFileIo::WindowedRead(ctx);
    };
    const int32_t handle = open("data.bd");
    CHECK(handle == 100 && calls.empty(), "file_open_consumes_id_without_module_registration");
    CHECK(!StreamLibrary::Install(capture, install, false) && StreamLibrary::Enabled(),
          "toggle_cannot_abandon_live_file_ownership");
    setup(kFs, handle, true);
    memory::write<uint16_t>(layout.stream_batch_count_addr, 7);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; LoaderFileIo::FileSize(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == kSize && calls.empty(), "file_size_does_not_ship_unrelated_audio");
    Fill(kDest, 1024);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, 999);
    raw(kFs, 257, 100, kDest);
    CHECK(StreamLibrary::Scheduling()->InFlight(kFs) && memory::read<uint8_t>(kDest) == 0xCE,
          "raw_read_waits_for_logical_delivery");
    CHECK(StreamLibrary::CanCheckpoint() && model.Enqueued() == 0,
          "typed_pending_payload_can_checkpoint_without_model_callback");
    for (unsigned i = 0; i < 6; ++i) {
        memory::write<uint8_t>(layout.stream_slot_status_addr + 2, 0);
        ctx.cpuRegs.GPR.r[4].UD[0] = kFs; dispatch[kPoll](ctx, kPoll);
        CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1, "reply_aging_cannot_finish_owned_read");
    }
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; dispatch[kWait](ctx, kWait);
    CHECK(Bytes(kDest, 257, 100), "raw_offset_is_absolute_not_archive_offset_added_twice");
    CHECK(StreamLibrary::CanCheckpoint(), "checkpoint_ready_after_ordered_publication");
    wait(kFs);
    CHECK(calls.empty() && module_pumps == 0 && originals.empty(), "file_read_poll_wait_execute_no_stream_continuation");

    setup(kFs, handle, true); Fill(kDest, 1024); Fill(kRing, 256);
    window(0, 100, kDest, false);
    CHECK(memory::read<uint8_t>(kDest) == 0xCE && memory::read<uint8_t>(kRing) == 0xCE,
          "ring_issue_publishes_neither_destination_nor_ring_early");
    tick += 100;
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; dispatch[kPoll](ctx, kPoll);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0 && Bytes(kRing, 0, 256) &&
              memory::read<uint8_t>(kDest) == 0xCE, "delivery_precedes_ring_handover");
    wait(kFs);
    CHECK(Bytes(kDest, 0, 100) && memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0,
          "wait_copies_and_clears_staging_once");
    Fill(kDest, 100, 0xBA); wait(kFs);
    CHECK(memory::read<uint8_t>(kDest) == 0xBA, "repeated_wait_cannot_replay_stale_ring_copy");
    window(224, 64, kDest, false);
    CHECK(Bytes(kDest, 224, 32), "straddle_preserves_held_prefix");
    wait(kFs);
    CHECK(Bytes(kDest, 224, 64), "straddle_tail_arrives_before_completion");
    const auto cursor = memory::read<uint32_t>(kCb + 12);
    window(1000, 600, kDest, false); wait(kFs);
    CHECK(Bytes(kDest, 1000, 600) && memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0,
          "oversized_read_never_copies_past_callback_ring");
    CHECK(memory::read<uint32_t>(kCb + 12) == kRing || memory::read<uint32_t>(kCb + 12) == cursor,
          "direct_read_does_not_advance_ring_for_destination_length");

    setup(kFs, handle, false); setup(kFs2, handle, false);
    Fill(kDest, 1024);
    // Keep the drive busy beyond this frame. Otherwise the second request's
    // initial poll can legitimately deliver the first read before the wait,
    // and the wait would not exercise its ordering arm.
    meter.ChargeReadAtMs(DeliveryMeter::FrameEndMs(tick), 20, 100);
    raw(kFs, 100, 100, kDest); raw(kFs2, 400, 100, kDest + 256);
    wait(kFs2);
    CHECK(Bytes(kDest, 100, 100) && Bytes(kDest + 256, 400, 100) &&
              StreamLibrary::Scheduling()->Count().wait_delivered >= 2 && model.Enqueued() == 0,
          "waiting_later_read_delivers_predecessor_first");
    wait(kFs);
    raw(kFs, 700, 100, kDest); raw(kFs, 900, 100, kDest + 256);
    CHECK(Bytes(kDest, 700, 100) && StreamLibrary::Scheduling()->InFlight(kFs),
          "overlap_finishes_previous_request_before_reuse");
    wait(kFs);
    CHECK(Bytes(kDest + 256, 900, 100) && model.Rejected() == 0, "overlap_preserves_new_pending_request");

    // Source FileStreamReadSoundBank marks its new intent before the start
    // adapter. The file-to-bank ownership handoff must retain this flag.
    memory::write<uint32_t>(kFs, memory::read<uint32_t>(kFs) | LoaderFileIo::kFlagReadPending);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 123;
    ctx.cpuRegs.GPR.r[6].UD[0] = 456; ctx.cpuRegs.GPR.r[7].UD[0] = 789;
    dispatch[kBank](ctx, kBank);
    CHECK(registrations == std::vector<int32_t>{handle}, "archive_registered_before_first_sound_bank_use");
    CHECK(memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending,
          "file_to_bank_handoff_retains_new_caller_pending_intent");
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; dispatch[kPoll](ctx, kPoll);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1 && originals.back() == kPoll, "audio_read_keeps_module_completion_authority");
    // Simulate the retained module finishing before the caller starts an EE read.
    memory::write<uint32_t>(kFs, 2);
    raw(kFs, 1200, 40, kDest); wait(kFs);
    CHECK(Bytes(kDest, 1200, 40), "same_archive_returns_to_file_service_after_audio_read");
    memory::write<uint32_t>(kFs, memory::read<uint32_t>(kFs) | LoaderFileIo::kFlagReadPending);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 123;
    ctx.cpuRegs.GPR.r[6].UD[0] = 456; ctx.cpuRegs.GPR.r[7].UD[0] = 789;
    dispatch[kBank](ctx, kBank);
    CHECK(registrations.size() == 1, "archive_registration_is_not_repeated_per_bank_segment");
    CHECK(memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending,
          "repeated_file_to_bank_handoff_retains_new_pending_intent");
    const auto sample = open("mUsIc.Mb;1");
    CHECK(sample == 101 && registrations.back() == sample, "sample_file_keeps_registration_and_handle_sequence");
    const auto before_close = calls.size();
    ctx.cpuRegs.GPR.r[4].SD[0] = sample; LoaderFileIo::Close(ctx);
    CHECK(calls.size() == before_close + 2 && calls[before_close] == layout.fn_unregister_file &&
              calls.back() == layout.fn_ship, "sample_close_keeps_unregister_and_ship");

    const auto plain = open("data.bd");
    setup(kFs, plain, false); Fill(kDest, 100);
    raw(kFs, 2000, 100, kDest);
    const auto before_cancel = calls.size();
    ctx.cpuRegs.GPR.r[4].SD[0] = plain; LoaderFileIo::Close(ctx);
    tick += 100; meter.DeliverDue(tick); StreamLibrary::AdvanceBoundary(tick);
    CHECK(memory::read<uint8_t>(kDest) == 0xCE && calls.size() == before_cancel &&
              !model.InFlight(kFs), "close_cancels_copy_without_stream_or_late_guest_write");
    const auto reopened = open("data.bd"); setup(kFs, reopened, false);
    raw(kFs, 2200, 100, kDest); wait(kFs);
    CHECK(Bytes(kDest, 2200, 100), "reused_object_and_destination_receive_new_generation");

    raw(kFs, 2400, 100, kDest); Fill(kDest, 100);
    LoaderFileIo::ResetForTest(); LoaderFileIo::Bind(deps, layout);
    const auto reset_file = open("data.bd"); setup(kFs, reset_file, false);
    raw(kFs, 2600, 100, kDest); wait(kFs);
    tick += 100; meter.DeliverDue(tick); StreamLibrary::AdvanceBoundary(tick);
    CHECK(Bytes(kDest, 2600, 100) && model.Rejected() == 0,
          "reset_invalidates_old_delivery_before_new_object_generation");
    const auto issued = model.Enqueued();
    raw(kFs, 0xffffffffu, 100, kDest);
    CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == -1 && model.Enqueued() == issued,
          "overflowing_file_extent_returns_error_without_read");
    raw(kFs, 0, 100, 0x01fffff0);
    CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == -1 && model.Enqueued() == issued,
          "destination_overflow_returns_error_without_copy");
    const auto short_file = open("short.bh"); setup(kFs, short_file, false); Fill(kDest, 100);
    raw(kFs, 0, kSize, kDest);
    CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == -1 && memory::read<uint8_t>(kDest) == 0xCE &&
              model.Enqueued() == issued, "short_disc_read_never_publishes_partial_bytes");
    CHECK(open("absent.bh") == -1, "missing_file_reports_open_failure");
    setup(kFs, -1, false);
    const auto previous = originals.size();
    const auto census_before = StreamLibrary::CountFallbacks();
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; dispatch[kPoll](ctx, kPoll);
    dispatch[kStart](ctx, kStart + 0x40);
    CHECK(originals.size() == previous + 2, "unknown_handles_and_interior_entries_keep_original_body");
    // The per-entry census splits the original's execution by reason: the
    // unknown-handle poll is `unowned` (the retained arm's request), the
    // interior entry `bypassed`, the two bank starts above `bank_start`; no
    // owned request was declined by either.
    const auto census_after = StreamLibrary::CountFallbacks();
    CHECK(census_after.is_reading_unowned == census_before.is_reading_unowned + 1 &&
              census_after.bypassed == census_before.bypassed + 1 &&
              census_after.bank_start == 2 &&
              census_after.is_reading_declined == census_before.is_reading_declined &&
              census_after.start_read_declined == census_before.start_read_declined &&
              census_after.wait_declined == census_before.wait_declined &&
              census_after.start_read_unowned == census_before.start_read_unowned,
          "fallback_census_splits_unowned_bypassed_and_bank_start_per_entry");
    CHECK(census_after.start_read_declined + census_after.is_reading_declined + census_after.wait_declined +
              census_after.start_read_unowned + census_after.is_reading_unowned + census_after.wait_unowned +
              census_after.bank_start + census_after.bypassed == StreamLibrary::Count().fallbacks,
          "fallback_census_partitions_the_total");
    CHECK(StreamLibrary::Count().errors == 3 && StreamLibrary::Count().deliveries > 0 &&
              StreamLibrary::Count().audio_registrations == 1, "census_reports_actual_work_and_failures");

    // A delivery may enqueue an already-ready child while the scheduler is
    // draining its finite batch. The next poll must make it visible without
    // requiring another disc event or an IOP heartbeat.
    const auto reentrant_file = open("data.bd");
    setup(kFs, reentrant_file, false); setup(kFs2, reentrant_file, false);
    int reentrant_order = 0;
    auto parent_delivery = StreamLibrary::Track(kFs, reentrant_file, [&] {
        CHECK(reentrant_order++ == 0, "reentrant_parent_delivers_first");
        auto child = StreamLibrary::Track(kFs2, reentrant_file, [&] {
            CHECK(reentrant_order++ == 1, "reentrant_child_delivers_after_parent");
        });
        child();
    });
    parent_delivery();
    CHECK(reentrant_order == 1 && !StreamLibrary::CanCheckpoint(),
          "callback_issued_ready_job_waits_for_next_finite_batch");
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs2;
    dispatch[kPoll](ctx, kPoll);
    CHECK(reentrant_order == 2 && ctx.cpuRegs.GPR.r[2].UL[0] == 0 && StreamLibrary::CanCheckpoint(),
          "poll_drains_callback_issued_ready_job_without_disc_or_iop_work");

    setup(kFs, reentrant_file, true);
    failures += RunStreamPersistenceCase(tick, meter, model, calls,
        reentrant_file, {window, wait, raw});
    LoaderFileIo::ResetForTest();
    iso.Close(); unlink(path.c_str());
    std::printf("StreamLibrary: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
