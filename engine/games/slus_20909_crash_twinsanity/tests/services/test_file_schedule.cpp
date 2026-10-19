#include <cstdio>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/file_schedule.h"
#include "games/slus_20909_crash_twinsanity/tests/services/service_time_case.h"

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(value, name) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %s\n", name); } } while (false)
using Event = std::tuple<uint64_t, uint32_t, std::vector<uint8_t>>;
FileSchedule::Payload Payload(uint32_t object = 0x10000, int32_t handle = 1) {
    return {object, 0x20000, 0, 0, handle, 13, {1, 5, 0, 255, 128}};
}
void Oracle(ps2r::CdvdDeliveryVisibility visibility, uint64_t limit, double scale) {
    ps2r::CdvdConfig config;
    config.delivery_visibility = visibility;
    config.latency_model = ps2r::CdvdLatencyModel::Modelled;
    config.latency_scale = scale;
    DeliveryMeter meter(config), shared(config);
    DeliveryBudget budget(limit), other_budget(limit);
    LoaderModel model(meter, budget);
    FileSchedule files;
    uint64_t tick = 0;
    std::vector<Event> expected, actual;
    model.SetNowTick([&] { return tick; });
    files.Bind(shared.Timing(), other_budget.TimingBudget(), [&] { return tick; },
               [&](const auto& payload) { actual.emplace_back(tick, payload.object, payload.bytes); });
    for (tick = 0; tick < 200; ++tick) {
        if (tick == 2 || tick == 51 || tick == 102) {
            // Remaining music reads share only the logical timing position.
            CHECK(meter.ChargeReadAtMs(tick * ReadTiming::kTickMs, 20, 3) ==
                      shared.ChargeReadAtMs(tick * ReadTiming::kTickMs, 20, 3), "other_client_charge");
            for (uint32_t i = 0; i < 3; ++i) {
                auto payload = Payload(0x10000 + i * 0x100);
                const uint32_t lba = i == 1 ? 0x7000 : 21, sectors = i + 2;
                model.Enqueue(payload.object, lba, sectors, tick,
                    [&, payload] { expected.emplace_back(tick, payload.object, payload.bytes); });
                CHECK(files.Submit(payload, lba, sectors, tick), "owned_submit");
            }
        }
        meter.DeliverDue(tick); files.AdvanceBoundary(tick);
        for (uint32_t i = 0; i < 3; ++i) {
            const auto object = 0x10000 + i * 0x100;
            CHECK((model.Poll(object) == LoaderModel::PollOutcome::HeldInflight) ==
                      (files.Poll(object) == FileSchedule::PollOutcome::HeldInflight), "poll_self_oracle");
        }
        CHECK(expected == actual, "ordered_delivery_tick_and_bytes_self_oracle");
        CHECK(meter.BusyUntilMs() == shared.BusyUntilMs() && budget.Remaining() == other_budget.Remaining(),
              "clock_and_budget_self_oracle");
    }
    CHECK(actual.size() == 9 && files.Count().delivered == 9 && shared.Pending() == 0 &&
              shared.Delivered() == 0, "owned_jobs_never_enter_model_queue");
}
void WaitOracle() {
    ps2r::CdvdConfig config;
    config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    DeliveryMeter meter(config), shared(config);
    DeliveryBudget budget(0), other_budget(0);
    LoaderModel model(meter, budget);
    FileSchedule files;
    uint64_t model_tick = 0, file_tick = 0;
    std::vector<Event> expected, actual;
    model.SetNowTick([&] { return model_tick; });
    model.SetParkFrame([&] { ++model_tick; });
    files.Bind(shared.Timing(), other_budget.TimingBudget(), [&] { return file_tick; },
               [&](const auto& payload) { actual.emplace_back(file_tick, payload.object, payload.bytes); });
    files.SetParkFrame([&] { ++file_tick; });
    for (uint32_t i = 0; i < 3; ++i) {
        auto payload = Payload(0x10000 + i * 0x100);
        model.Enqueue(payload.object, 0x7000 + i, 100, 0,
            [&, payload] { expected.emplace_back(model_tick, payload.object, payload.bytes); });
        CHECK(files.Submit(payload, 0x7000 + i, 100, 0), "wait_submit");
    }
    CHECK(model.CompleteNow(0x10200) && files.Wait(0x10200), "blocking_wait_finishes");
    CHECK(expected == actual && model_tick == file_tick && file_tick > 0,
          "logical_wait_and_predecessor_bytes_self_oracle");
    CHECK(files.Count().overruns == 0 && model.WaitOverruns() == 0, "wait_no_host_time_or_overrun");
}
void Persistence() {
    ps2r::CdvdConfig config;
    config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    auto timing = std::make_shared<ReadTiming>(config);
    auto budget = std::make_shared<ReadTiming::Budget>();
    FileSchedule files;
    uint64_t tick = 12;
    std::vector<Event> events;
    files.Bind(timing, budget, [&] { return tick; },
               [&](const auto& payload) { events.emplace_back(tick, payload.object, payload.bytes); });
    CHECK(files.Submit(Payload(), 0x7000, 100, tick), "pending_request");
    FileSchedule::State saved;
    CHECK(files.Capture(saved) && saved.reads.at(0x10000).phase == FileSchedule::Phase::Pending,
          "capture_owns_pending_bytes_and_deadline");
    const auto due = saved.reads.at(0x10000).due;
    tick = due; files.AdvanceBoundary(tick);
    CHECK(events.size() == 1, "first_future_delivers");
    auto next = Payload(); next.bytes[0] = 42;
    CHECK(files.Submit(next, 20, 20, tick), "future_generation");
    CHECK(!files.Restore(saved), "restore_requires_matching_logical_clock");
    tick = saved.tick; events.clear();
    CHECK(files.Restore(saved) && timing->Capture().busy_ms == saved.timing.busy_ms,
          "restore_reconstructs_payload_without_recharging");
    FileSchedule::State again;
    CHECK(files.Capture(again) && again.reads.at(0x10000).due == due &&
              again.next_order == saved.next_order, "restored_order_and_deadline_exact");
    tick = due; files.AdvanceBoundary(tick);
    CHECK(events.size() == 1 && std::get<2>(events.front()) == Payload().bytes,
          "restored_pending_replaces_future_generation");
    CHECK(files.Capture(again) && again.reads.at(0x10000).phase == FileSchedule::Phase::Delivered,
          "delivered_before_consumption_captured");
    CHECK(files.Restore(again), "restore_delivered_not_consumed");
    files.AdvanceBoundary(tick + 100);
    CHECK(files.Poll(0x10000) == FileSchedule::PollOutcome::Progress && events.size() == 1,
          "restored_delivered_payload_not_copied_twice");
    auto corrupt = saved;
    corrupt.reads.at(0x10000).end_ms = std::numeric_limits<double>::quiet_NaN();
    CHECK(!files.Restore(corrupt) && !files.InFlight(0x10000), "bad_clock_rejected_before_mutation");
    CHECK(files.Submit(Payload(), 25, 100, tick), "close_pending_request");
    files.CancelHandle(1); files.AdvanceBoundary(tick + 1000);
    CHECK(events.size() == 1 && !files.InFlight(0x10000), "close_invalidates_late_delivery");
}
void ServiceReadCharge() {
    // A music refill: drive time on the shared position, no delivery; a
    // later request is due after it. Nothing is charged under the instant
    // boundary model, exactly as DueAtMs charges nothing there.
    for (const auto visibility : {ps2r::CdvdDeliveryVisibility::Poll, ps2r::CdvdDeliveryVisibility::Boundary}) {
        ps2r::CdvdConfig config;
        config.delivery_visibility = visibility;
        auto timing = std::make_shared<ReadTiming>(config);
        FileSchedule files;
        uint64_t tick = 30;
        std::vector<Event> events;
        files.Bind(timing, {}, [&] { return tick; },
                   [&](const auto& payload) { events.emplace_back(tick, payload.object, payload.bytes); });
        const auto before = timing->Capture().busy_ms;
        const auto end = files.ChargeRead(0x9000, 160);
        const bool charges = visibility == ps2r::CdvdDeliveryVisibility::Poll;
        CHECK(charges ? end > before && timing->Capture().busy_ms == end : end == 0 && timing->Capture().busy_ms == before,
              "service_read_charges_shared_position_under_the_drive_model");
        CHECK(events.empty() && files.Count().issued == 0, "service_read_delivers_nothing");
        CHECK(files.Submit(Payload(), 0x9000 + 160, 10, tick), "request_after_service_read");
        FileSchedule::State saved;
        CHECK(files.Capture(saved) && (!charges || saved.reads.at(0x10000).end_ms >= end),
              "later_request_waits_behind_the_service_read");
        CHECK(files.ChargeRead(0x9000, 0) == 0, "empty_service_read_charges_nothing");
    }
}
void Reentry() {
    FileSchedule files;
    uint32_t copies = 0;
    bool snapshot_declined = false;
    files.Bind({}, {}, {}, [&](const auto&) {
        ++copies;
        FileSchedule::State saved;
        snapshot_declined = !files.Capture(saved);
        if (copies == 1) CHECK(files.Submit(Payload(0x10100), 20, 1, 0), "callback_submits_owned_job");
    });
    CHECK(files.Submit(Payload(), 20, 1, 0), "immediate_request");
    CHECK(copies == 1 && snapshot_declined, "callback_batch_is_finite_snapshot_not_mid_copy");
    CHECK(files.Poll(0x10100) == FileSchedule::PollOutcome::Progress && copies == 2,
          "next_poll_drains_callback_issued_job");
    files.Reset();
    files.Bind({}, {}, {}, [&](const auto&) { ++copies; files.Reset(); });
    CHECK(files.Submit(Payload(), 20, 1, 0) && !files.InFlight(0x10000),
          "reset_inside_delivery_has_no_stale_iterator_write");
}
void SoundDestinations() {
    FileSchedule files;
    ps2r::CdvdConfig config;
    auto timing = std::make_shared<ReadTiming>(config);
    uint64_t tick = 0;
    std::vector<uint32_t> delivered, cancelled;
    files.Bind(timing, {}, [&] { return tick; },
               [&](const auto& payload) { delivered.push_back(payload.object); });
    files.SetCancel([&](const auto& payload) {
        CHECK(!files.InFlight(payload.object), "cancel_observer_runs_after_removal");
        cancelled.push_back(payload.sound);
    });
    auto sound = Payload(0x10100);
    sound.kind = FileSchedule::Destination::Sound; sound.destination = 0;
    sound.sound = 0x8000; sound.generation = 7; sound.sequence = 9; sound.bytes.resize(16);
    auto invalid = sound; invalid.destination = 0x20000;
    CHECK(!files.Submit(invalid, 20, 1, tick), "sound_cannot_masquerade_as_memory");
    invalid = sound; invalid.sequence = 0;
    CHECK(!files.Submit(invalid, 20, 1, tick), "sound_requires_lifetime_ticket");
    CHECK(files.Submit(Payload(), 20, 1, tick) && files.Submit(sound, 21, 1, tick),
          "memory_and_sound_share_the_ordered_queue");
    CHECK(files.HasSoundReads(), "pending_sound_owns_bound_delivery");
    CHECK(files.Wait(sound.object), "sound_wait_finishes_file_predecessor");
    CHECK(delivered == std::vector<uint32_t>({0x10000, 0x10100}) && !files.HasSoundReads(),
          "typed_destinations_keep_submission_order");
    files.CancelHandle(1);
    CHECK(cancelled.empty(), "already_delivered_asset_not_cancelled");
    CHECK(files.Submit(sound, 21, 1, tick), "pending_sound_before_close");
    files.CancelHandle(1);
    CHECK(cancelled == std::vector<uint32_t>({0x8000}), "close_releases_pending_asset_ticket");
    CHECK(files.Submit(sound, 21, 1, tick), "pending_sound_before_reset");
    files.Reset();
    CHECK(cancelled.size() == 2 && !files.HasSoundReads(), "reset_releases_pending_asset_ticket");
    tick += 1000;
    files.AdvanceBoundary(tick);
    CHECK(delivered.size() == 2, "cancelled_bank_has_no_late_delivery");
}
void BankAcceptance() {
    ps2r::CdvdConfig config;
    config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    auto timing = std::make_shared<ReadTiming>(config);
    FileSchedule files;
    uint64_t tick = 100, parks = 0;
    std::vector<uint32_t> delivered, cancelled;
    files.Bind(timing, {}, [&] { return tick; },
        [&](const auto& payload) { delivered.push_back(payload.object); });
    files.SetParkFrame([&] { ++parks; ++tick; });
    files.SetCancel([&](const auto& payload) { cancelled.push_back(payload.object); });
    auto sound = Payload(0x10100);
    sound.kind = FileSchedule::Destination::Sound; sound.destination = 0;
    sound.sound = 0x8000; sound.generation = 1; sound.sequence = 1; sound.bytes.resize(16);
    CHECK(files.Submit(sound, 0x7000, 4, tick, true), "bank_queues_immutable_request");
    const auto prefix = files.LastOrder();
    const auto uncharged = timing->Capture();
    tick += 20;
    files.AdvanceBoundary(tick);
    CHECK(files.Poll(sound.object) == FileSchedule::PollOutcome::HeldInflight &&
          delivered.empty() && timing->Capture().busy_ms == uncharged.busy_ms,
          "queued_bank_has_no_clock_charge_or_early_completion");
    FileSchedule::State pending;
    CHECK(files.Capture(pending) && !pending.reads.at(sound.object).accepted &&
          pending.arrivals.empty() && files.Restore(pending), "typed_capture_keeps_unaccepted_bank");
    cancelled.clear(); // Restore invalidates the old generation before import.
    auto later = sound; later.object = 0x10200; later.sequence = 2;
    CHECK(files.Submit(later, 0x7004, 1, tick, true), "next_batch_bank_staged");
    CHECK(files.Submit(Payload(), 20, 1, tick), "file_accepted_while_bank_is_queued");
    files.AcceptSounds(prefix);
    FileSchedule::State accepted;
    CHECK(files.Capture(accepted) && accepted.reads.at(sound.object).accepted &&
          !accepted.reads.at(later.object).accepted &&
          accepted.reads.at(0x10000).order < accepted.reads.at(sound.object).order,
          "accepts_only_finite_prefix_after_already_accepted_files");
    const auto charged = timing->Capture();
    files.AcceptSounds(prefix);
    CHECK(timing->Capture().busy_ms == charged.busy_ms, "accepted_bank_is_never_charged_twice");
    const auto before_wait = tick;
    CHECK(files.Wait(sound.object) && tick == before_wait && parks == 0 &&
          timing->Capture().busy_ms == charged.busy_ms &&
          timing->Capture().head_lba == charged.head_lba,
          "bank_wait_settles_service_without_game_tick_or_debt_loss");
    CHECK(delivered == std::vector<uint32_t>({0x10000, sound.object}) &&
          files.InFlight(later.object), "unaccepted_later_batch_is_not_delivered_by_earlier_wait");
    files.CancelObject(later.object);
    tick += 1000;
    files.AcceptSounds(files.LastOrder()); files.AdvanceBoundary(tick);
    CHECK(delivered.size() == 2 && cancelled == std::vector<uint32_t>{later.object},
          "cancelled_unaccepted_bank_never_acquires_a_deadline");

    // An immediate provider can call back during acceptance. New work is
    // immutable and belongs to the next finite batch, never this iteration.
    files.Reset();
    files.Bind({}, {}, [&] { return tick; }, [&](const auto& payload) {
        delivered.push_back(payload.object);
        if (payload.object == sound.object)
            CHECK(files.Submit(later, 0x7004, 1, tick, true), "callback_stages_next_bank");
    });
    CHECK(files.Submit(sound, 0x7000, 4, tick, true), "immediate_provider_still_requires_acceptance");
    files.AcceptSounds(files.LastOrder());
    CHECK(files.Released(sound.object) && files.InFlight(later.object),
          "acceptance_does_not_swallow_callback_issued_bank");
    files.AcceptSounds(files.LastOrder());
    CHECK(files.Released(later.object), "later_acceptance_delivers_callback_bank");
}

void CancellationBoundary() {
    FileSchedule files;
    ps2r::CdvdConfig config;
    auto timing = std::make_shared<ReadTiming>(config);
    files.Bind(timing, {}, {}, [](const auto&) {});
    CHECK(files.Submit(Payload(), 20, 1, 0), "cancel_boundary_request");
    FileSchedule::State saved;
    CHECK(files.Capture(saved), "cancel_boundary_saved");
    unsigned cancelled = 0;
    files.SetCancel([&](const auto&) {
        ++cancelled;
        FileSchedule::State partial;
        CHECK(!files.Capture(partial), "cancel_callback_is_not_a_snapshot_boundary");
        CHECK(!files.Restore(saved), "cancel_callback_cannot_restore_over_its_owner");
        CHECK(!files.SetServiceTime(true), "cancel_callback_cannot_reselect_clock");
        CHECK(files.Submit(Payload(0x10100, 2), 21, 1, 0), "cancel_callback_can_issue_new_job");
        files.SetCancel([](const auto&) { throw std::runtime_error("cancelled provider"); });
    });
    files.CancelHandle(1);
    CHECK(cancelled == 1 && files.InFlight(0x10100) && files.CanCheckpoint(),
          "cancel_prefix_preserves_new_owner_work");
    bool threw = false;
    try { files.CancelHandle(2); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && files.CanCheckpoint() && !files.InFlight(0x10100),
          "cancel_exception_releases_boundary_guard");
}
}
int main() {
    for (const auto visibility : {ps2r::CdvdDeliveryVisibility::Poll, ps2r::CdvdDeliveryVisibility::Boundary})
        for (const uint64_t budget : {0, 2048, 8192})
            for (const double scale : {0., .5, 1.}) Oracle(visibility, budget, scale);
    WaitOracle(); Persistence(); ServiceReadCharge(); Reentry(); SoundDestinations(); BankAcceptance(); CancellationBoundary();
    failures += RunServiceTimeCase();
    std::printf("FileSchedule: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
