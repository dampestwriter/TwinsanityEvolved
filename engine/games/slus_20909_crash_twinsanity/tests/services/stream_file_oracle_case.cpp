#include "games/slus_20909_crash_twinsanity/tests/services/stream_file_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/stream_oracle_helpers.h"

#include <array>
#include <cstdio>
#include <cstring>
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"

using namespace stream_oracle;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

int RunModelFileOracle(StreamOracleInputs& fixture, uint32_t kFs,
    const IsoFs::Entry& file, const std::vector<uint8_t>& oracle) {
    int g_fail = 0;
    auto& ctx = fixture.ctx;
    auto& face = fixture.face;
    auto& plugin = fixture.plugin;
    auto& table = fixture.table;
    auto& k = table.Kernel();
    auto& ee_sink = fixture.ee_sink;
    auto& ee_server = fixture.ee_server;
    auto& router = fixture.router;
    auto& model = fixture.model;
    auto& iop_mem = fixture.iop_mem;
    auto& pump_calls = fixture.pump_calls;
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


    return g_fail;
}

int RunOwnedFileOracle(StreamOracleInputs& fixture, uint32_t kFs, uint8_t kSlot,
    const IsoFs::Entry& file) {
    int g_fail = 0;
    auto& ctx = fixture.ctx;
    auto& face = fixture.face;
    auto& plugin = fixture.plugin;
    auto& ee_sink = fixture.ee_sink;
    auto& model = fixture.model;
    auto& meter = fixture.meter;
    const auto& deps = fixture.deps;
    const auto& layout = fixture.layout;
    auto& pump_calls = fixture.pump_calls;
    // Both arms now receive the identical file ID, path, byte offset, length,
    // object, destination and sentinel bytes on NEW. The disabled arm executes
    // the actual captured NTSC body and recompiled STREAM/CD/SIF path above.
    Prep(ctx); SetArg(ctx, 4, 3); LoaderFileIo::Close(ctx);
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, StreamLibrary::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    const std::array<std::pair<uint32_t, uint32_t>, 3> requests{{{0, 2048}, {13, 1027}, {2041, 3333}}};
    uint64_t compared_bytes = 0;
    for (const auto& request : requests) {
        const uint32_t offset = request.first, length = request.second;
        std::vector<uint8_t> model_bytes;
        for (const bool owned : {false, true}) {
            CHECK(StreamLibrary::Install(capture, install, owned), "oracle_selects_requested_arm");
            LoaderFileIo::Bind(deps, layout);
            const uint64_t rpc_before = face.Calls(), dma_before = ee_sink.Bytes();
            const uint64_t reads_before = plugin.Cdvdman().ReadsIssued(), pumps_before = pump_calls;
            const uint64_t enqueued_before = model.Enqueued(), scheduled_before = meter.LastScheduledId();
            memory::write<int32_t>(kHandleCounter, 3);
            Prep(ctx); SetArg(ctx, 4, 0x00500000); LoaderFileIo::Open(ctx);
            CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == 3, "oracle_same_file_handle");
            memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 3);
            Prep(ctx); SetArg(ctx, 4, kFs); LoaderFileIo::FileSize(ctx);
            CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == file.size, "oracle_same_file_extent");
            memory::write<uint32_t>(kFs, kSlot | kFlagRingOpen | LoaderFileIo::kFlagReadPending);
            memory::write<uint32_t>(kFs + LoaderFileIo::kFsDest, 0);
            for (uint32_t i = 0; i < length + 16; ++i) memory::write<uint8_t>(kDest + i, 0x5A);
            Prep(ctx); SetArg(ctx, 4, kFs); SetArg(ctx, 5, offset); SetArg(ctx, 6, length); SetArg(ctx, 7, kDest);
            DispatchTable::Invoke(0x2ad9a8u, ctx);
            CHECK(RangeIs(kDest, 0x5A, length) && TitlePoll(ctx, kFs) != 0,
                  "oracle_no_bytes_or_completion_before_delivery");
            Prep(ctx); SetArg(ctx, 4, kFs); LoaderFileIo::WaitReadComplete(ctx);
            CHECK(TitlePoll(ctx, kFs) == 0 && RangeIs(kDest + length, 0x5A, 16),
                  "oracle_complete_with_destination_guard");
            std::vector<uint8_t> result(length);
            for (uint32_t i = 0; i < length; ++i) result[i] = memory::read<uint8_t>(kDest + i);
            Prep(ctx); SetArg(ctx, 4, 3); LoaderFileIo::Close(ctx);
            if (!owned) {
                model_bytes = std::move(result);
                CHECK(face.Calls() > rpc_before && ee_sink.Bytes() > dma_before &&
                          plugin.Cdvdman().ReadsIssued() > reads_before && pump_calls > pumps_before,
                      "oracle_model_arm_really_executed_stream_iop_cd_and_dma");
            } else {
                CHECK(result == model_bytes, "oracle_owned_file_bytes_equal_actual_model_output");
                CHECK(face.Calls() == rpc_before && ee_sink.Bytes() == dma_before &&
                          plugin.Cdvdman().ReadsIssued() == reads_before && pump_calls == pumps_before &&
                          model.Enqueued() == enqueued_before && meter.LastScheduledId() == scheduled_before,
                      "oracle_owned_arm_executes_zero_stream_iop_cd_dma_and_model_queue_calls");
                compared_bytes += length;
            }
        }
    }
    std::printf("StreamFileSelfOracle: requests=%zu bytes=%llu failures=%d\n", requests.size(),
                static_cast<unsigned long long>(compared_bytes), g_fail);

    return g_fail;
}
