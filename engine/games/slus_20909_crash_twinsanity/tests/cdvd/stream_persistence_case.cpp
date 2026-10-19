#include "tests/cdvd/stream_persistence_case.h"
#include "tests/cdvd/stream_library_fixture.h"

#include <array>
#include <cstdio>
#include <map>
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

using namespace stream_fixture;
#define CHECK(condition, name) do { \
    if (!(condition)) { std::printf("FAIL %s\n", name); ++failures; } \
    else std::printf("PASS %s\n", name); \
} while (false)

int RunStreamPersistenceCase(uint64_t& tick, DeliveryMeter& meter,
    LoaderModel& model, const std::vector<uint32_t>& calls, int32_t reentrant_file,
    const StreamPersistenceActions& actions) {
    int failures = 0;
    const auto& window = actions.window;
    const auto& wait = actions.wait;
    const auto& raw = actions.raw;
    window(200, 100, kDest, false);
    std::vector<uint8_t> checkpoint{0x77};
    std::string error;
    CHECK(StreamState::Capture(checkpoint, &error) && StreamState::Validate(checkpoint, &error),
          "state_capture_owns_pending_payload_and_logical_schedule");
    const auto pending_tick = tick;
    const auto pending_clock = meter.BusyUntilMs();
    ps2r::app::SessionServices session(true, nullptr);
    ps2r::savestate::SectionRegistry sections;
    CHECK(sections.Register(session.Section()), "file_application_owner_registers");
    const auto image_state = sections.Capture();
    CHECK(session.CanCheckpoint(&error) && session.ValidateImage(image_state, tick, &error) &&
              !session.ValidateImage(image_state, tick + 1, &error) &&
              !session.ValidateImage({}, tick, &error),
          "file_application_owner_validates_required_state_and_logical_clock_without_pumping");
    std::array<uint8_t, 0x48> pending_object;
    std::array<uint8_t, 20> pending_reader;
    for (size_t i = 0; i < pending_object.size(); ++i) pending_object[i] = memory::read<uint8_t>(kFs + i);
    for (size_t i = 0; i < pending_reader.size(); ++i) pending_reader[i] = memory::read<uint8_t>(kCb + i);
    wait(kFs);
    CHECK(Bytes(kDest, 200, 100), "first_pending_future_delivers_ring_bytes");
    raw(kFs, 2500, 64, kDest + 256);
    tick = pending_tick;
    for (size_t i = 0; i < pending_object.size(); ++i) memory::write<uint8_t>(kFs + i, pending_object[i]);
    for (size_t i = 0; i < pending_reader.size(); ++i) memory::write<uint8_t>(kCb + i, pending_reader[i]);
    Fill(kDest, 512); Fill(kRing, 256);
    CHECK(sections.Restore(image_state, &error) && meter.BusyUntilMs() == pending_clock,
          "pending_restore_replaces_future_generation_without_recharging");
    wait(kFs);
    CHECK(Bytes(kDest, 200, 100) && memory::read<uint8_t>(kDest + 256) == 0xCE,
          "restored_pending_applies_owned_bytes_and_not_discarded_future");
    CHECK(StreamState::Capture(checkpoint, &error) && StreamState::Validate(checkpoint, &error),
          "state_captures_complete_file_identities_ownership_and_ring_window");
    LoaderFileIo::State decoded;
    CHECK(StreamState::Decode(checkpoint, decoded, &error) && decoded.files.count(reentrant_file) &&
              decoded.streams.files.count(reentrant_file), "state_payload_owns_open_handles");
    const auto saved_windows = decoded.windows.size();
    const auto saved_sectors = decoded.streams.schedule.reads.at(kFs).sectors;
    decoded.streams.schedule.reads.at(kFs).sectors++;
    CHECK(!LoaderFileIo::RestoreState(decoded, &error) && StreamLibrary::FileOwned(reentrant_file),
          "state_rejects_payload_and_timing_extent_disagreement");
    decoded.streams.schedule.reads.at(kFs).sectors = saved_sectors;
    auto bad = checkpoint;
    bad.push_back(0);
    CHECK(!StreamState::Restore(bad, &error) && StreamLibrary::FileOwned(reentrant_file),
          "state_rejects_trailing_data_before_mutation");
    for (size_t cut = 0; cut < checkpoint.size(); ++cut) {
        const std::vector<uint8_t> truncated(checkpoint.begin(), checkpoint.begin() + cut);
        if (StreamState::Validate(truncated)) {
            CHECK(false, "state_rejects_every_truncation");
            break;
        }
    }
    decoded.streams.files.at(reentrant_file).sample = true;
    decoded.streams.files.at(reentrant_file).registered = true;
    CHECK(!LoaderFileIo::RestoreState(decoded, &error) && StreamLibrary::FileOwned(reentrant_file),
          "state_rejects_changed_sample_classification_before_mutation");
    CHECK(StreamState::Decode(checkpoint, decoded, &error), "state_decodes_after_invalid_restore");
    decoded.files.at(reentrant_file).size++;
    CHECK(!LoaderFileIo::RestoreState(decoded, &error) && StreamLibrary::FileOwned(reentrant_file),
          "state_rejects_mismatched_media_before_mutation");
    LoaderFileIo::ResetState();
    CHECK(!StreamLibrary::FileOwned(reentrant_file), "state_reset_releases_file_ownership");
    CHECK(StreamState::Restore(checkpoint, &error) && StreamLibrary::FileOwned(reentrant_file),
          "state_restores_file_ownership_without_open_rpc");
    LoaderFileIo::State roundtrip;
    CHECK(LoaderFileIo::CaptureState(roundtrip) && roundtrip.windows.size() == saved_windows,
          "state_restores_completed_window_tracking");
    const auto restored_reads = model.Enqueued();
    const auto restored_calls = calls.size();
    window(300, 80, kDest, false);
    CHECK(Bytes(kDest, 300, 80) && model.Enqueued() == restored_reads && calls.size() == restored_calls + 1,
          "restored_ring_hit_uses_original_bytes_without_disc_or_stream_work");
    window(1000, 400, kDest, false); wait(kFs);
    CHECK(Bytes(kDest, 1000, 400) && StreamLibrary::CanCheckpoint(),
          "restored_file_can_issue_and_deliver_a_fresh_generation");
    constexpr uint32_t system = 0x570000, links = 0x571000, streams = 0x572000;
    Fill(system, 0x1c, 0);
    memory::write<uint16_t>(system, 4);
    memory::write<uint16_t>(system + 4, 1);
    memory::write<uint32_t>(system + 8, links);
    memory::write<uint32_t>(system + 12, streams);
    for (uint32_t slot = 0; slot < 4; ++slot) {
        memory::write<int16_t>(links + slot * 2, slot == 2 ? -1 : -2);
        memory::write<uint32_t>(streams + slot * 4, slot == 2 ? kFs : 0xDEADBEEF);
    }
    Text(kPath, "data.bd");
    memory::write<uint32_t>(kFs + 0x1c, kPath);
    memory::write<uint32_t>(kFs + 0x20, 7);
    memory::write<uint32_t>(kFs + 0x24, 8);
    LoaderFileIo::State migrated;
    CHECK(StreamState::MigrateFiles(system, migrated, &error) &&
              migrated.files.at(reentrant_file).path == "data.bd" && migrated.files.size() == 1,
          "legacy_migration_uses_game_file_identity_and_ignores_free_slots");
    std::map<uint32_t, uint8_t> image;
    for (const auto& span : {std::make_pair(system, 0x1cu), std::make_pair(links, 8u),
                            std::make_pair(streams, 16u), std::make_pair(kFs, 0x48u),
                            std::make_pair(kPath, 8u)})
        for (uint32_t i = 0; i < span.second; ++i) image[span.first + i] = memory::read<uint8_t>(span.first + i);
    const StreamState::ReadMemory incoming = [&](uint32_t address, uint8_t* bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const auto found = image.find(address + static_cast<uint32_t>(i));
            if (found == image.end()) return false;
            bytes[i] = found->second;
        }
        return true;
    };
    memory::write<uint32_t>(kFs + 0x1c, 0);
    memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, -1);
    CHECK(StreamState::MigrateFiles(system, incoming, migrated, &error) &&
              migrated.files.at(reentrant_file).path == "data.bd" &&
              memory::read<uint32_t>(kFs + 0x1c) == 0 &&
              memory::read<int32_t>(kFs + LoaderFileIo::kFsHandle) == -1,
          "incoming_ram_preflight_uses_only_supplied_bytes_and_does_not_restore_live_ram");
    image.erase(kPath + 7);
    CHECK(!StreamState::MigrateFiles(system, incoming, migrated, &error) &&
              migrated.files.at(reentrant_file).path == "data.bd" && StreamLibrary::FileOwned(reentrant_file),
          "incomplete_incoming_ram_rejected_without_mutating_candidate_or_live_owner");
    image[kPath + 7] = 0;
    image[kFs + 0x24] = 7;
    CHECK(!StreamState::MigrateFiles(system, incoming, migrated, &error),
          "incoming_string_capacity_cannot_exclude_terminator");
    image[kFs + 0x24] = 8;
    CHECK(!StreamState::MigrateFiles(system, StreamState::ReadMemory{}, migrated, &error),
          "incoming_ram_provider_is_required");
    memory::write<uint32_t>(kFs + 0x1c, kPath);
    memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, reentrant_file);
    memory::write<uint8_t>(kFs + 1, 3);
    memory::write<int32_t>(kFs + 0x40, reentrant_file);
    Text(kPath, "DATA");
    memory::write<uint32_t>(kFs + 0x34, kPath);
    memory::write<uint32_t>(kFs + 0x38, 4);
    memory::write<uint32_t>(kFs + 0x3c, 5);
    CHECK(StreamState::MigrateFiles(system, migrated, &error) &&
              migrated.files.at(reentrant_file).path == "DATA.BD",
          "legacy_migration_resolves_archive_backing_file_not_inner_resource_path");
    memory::write<uint32_t>(kFs, memory::read<uint32_t>(kFs) | LoaderFileIo::kFlagReadPending);
    CHECK(!StreamState::MigrateFiles(system, migrated, &error) && StreamLibrary::FileOwned(reentrant_file),
          "legacy_migration_declines_unknown_pending_transport_without_mutation");
    memory::write<uint32_t>(kFs, memory::read<uint32_t>(kFs) & ~LoaderFileIo::kFlagReadPending);
    memory::write<uint16_t>(system + 4, 2);
    CHECK(!StreamState::MigrateFiles(system, migrated, &error), "legacy_migration_validates_active_pool_count");
    memory::write<uint16_t>(system + 4, 1);

    // A legacy ring window still in flight: the title's own staging words name
    // the fill the module was serving (fs+0xc position, cb base/capacity, the
    // staged fs+0x10/0x18 window). The import re-issues that fill through the
    // owned scheduler; the title's completion copy still lands the window.
    memory::write<uint8_t>(kFs + 1, 0);                      // ordinary open file, not an archive
    memory::write<uint32_t>(kFs + 0x1c, kPath); Text(kPath, "data.bd");
    memory::write<uint32_t>(kFs + 0x20, 7); memory::write<uint32_t>(kFs + 0x24, 8);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, 0);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsRingStart, 1024);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsDest, kDest + 64);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSize, 100);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsCbObj, kCb);
    memory::write<uint32_t>(kCb + LoaderFileIo::kCbBase, kRing);
    memory::write<uint32_t>(kCb + LoaderFileIo::kCbCursor, kRing);
    memory::write<uint32_t>(kCb + LoaderFileIo::kCbCap, 256);
    const uint32_t base_flags = memory::read<uint32_t>(kFs) & 0xffu;
    memory::write<uint32_t>(kFs, base_flags | LoaderFileIo::kFlagReadPending | LoaderFileIo::kFlagRingValid);
    std::vector<StreamState::PendingWindow> pending;
    CHECK(!StreamState::MigrateFiles(system, migrated, &error) && error == "legacy file read still pending",
          "legacy_migration_without_pending_consumer_still_refuses");
    LoaderFileIo::State imported;
    CHECK(StreamState::MigrateFiles(system, [](uint32_t address, uint8_t* bytes, size_t count) {
              for (size_t i = 0; i < count; ++i) bytes[i] = memory::read<uint8_t>(address + static_cast<uint32_t>(i));
              return true;
          }, imported, pending, &error) && pending.size() == 1 && pending[0].fs == kFs &&
              pending[0].handle == reentrant_file && pending[0].position == 1024 && pending[0].fill == 256 &&
              pending[0].ring_base == kRing && pending[0].staged_dest == kDest + 64 && pending[0].staged_size == 100 &&
              pending[0].offset == 1024 && imported.windows.count(kFs) &&
              imported.windows.at(kFs).dest == kDest + 64 && imported.windows.at(kFs).size == 100 &&
              imported.windows.at(kFs).offset == 1024,
          "legacy_pending_ring_window_recovered_from_title_staging_words");
    const auto read_disc = [](uint32_t, uint64_t byte_offset, uint32_t size, uint8_t* out) {
        for (uint32_t i = 0; i < size; ++i) out[i] = Byte(static_cast<uint32_t>(byte_offset) + i);
        return true;
    };
    LoaderFileIo::ResetState();
    Fill(kDest, 512); Fill(kRing, 256);
    CHECK(LoaderFileIo::RestoreState(imported, &error) && StreamLibrary::FileOwned(reentrant_file),
          "legacy_pending_state_restores_file_ownership_and_window_table");
    CHECK(!StreamState::ImportPending(pending, imported, [](uint32_t, uint64_t, uint32_t, uint8_t*) { return false; },
                                      tick, &error) && !StreamLibrary::Scheduling()->InFlight(kFs),
          "unreadable_medium_imports_nothing");
    CHECK(StreamState::ImportPending(pending, imported, read_disc, tick, &error) &&
              StreamLibrary::Scheduling()->InFlight(kFs) && memory::read<uint8_t>(kRing) == 0xCE,
          "pending_fill_reissued_on_owned_schedule_without_early_bytes");
    CHECK(!StreamState::ImportPending(pending, imported, read_disc, tick, &error),
          "pending_fill_cannot_be_imported_twice");
    wait(kFs);
    CHECK(Bytes(kRing, 1024, 256) && Bytes(kDest + 64, 1024, 100) &&
              memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0 &&
              !(memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending),
          "imported_fill_lands_in_ring_and_title_completion_copy_finishes_window");
    // A direct (unstaged) window has no EE-side record: explicit refusal, no mutation.
    memory::write<uint32_t>(kFs, base_flags | LoaderFileIo::kFlagReadPending);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsDest, 0);
    pending.clear();
    CHECK(!StreamState::MigrateFiles(system, [](uint32_t address, uint8_t* bytes, size_t count) {
              for (size_t i = 0; i < count; ++i) bytes[i] = memory::read<uint8_t>(address + static_cast<uint32_t>(i));
              return true;
          }, imported, pending, &error) && pending.empty() &&
              error == "legacy direct read record is held by the retained module only",
          "legacy_direct_pending_read_refused_by_name");
    // A staged size at or above the ring capacity is not a ring fill the module served.
    memory::write<uint32_t>(kFs, base_flags | LoaderFileIo::kFlagReadPending | LoaderFileIo::kFlagRingValid);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsDest, kDest + 64);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSize, 256);
    CHECK(!StreamState::MigrateFiles(system, [](uint32_t address, uint8_t* bytes, size_t count) {
              for (size_t i = 0; i < count; ++i) bytes[i] = memory::read<uint8_t>(address + static_cast<uint32_t>(i));
              return true;
          }, imported, pending, &error) && pending.empty(),
          "oversized_staging_is_not_a_recoverable_ring_window");
    memory::write<uint32_t>(kFs, base_flags);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsDest, 0);
    return failures;
}
