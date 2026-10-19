#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/services/read_timing.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/scheduler.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_time.h"

// Typed file requests and logical visibility. Payloads, destination intent,
// deadlines and ordering live here, never in a LoaderModel callback.
class FileSchedule {
public:
    enum class Destination : uint8_t { Memory, Sound, Music };
    struct Payload {
        uint32_t object = 0, destination = 0, ring_base = 0, ring_cursor = 0;
        int32_t handle = -1;
        uint64_t file_offset = 0;
        std::vector<uint8_t> bytes;
        Destination kind = Destination::Memory;
        uint32_t sound = UINT32_MAX;
        uint32_t stream = UINT32_MAX;
        uint64_t generation = 0, sequence = 0;
    };
    enum class Phase : uint8_t { Pending, Eligible, Delivered, Consumed };
    enum class PollOutcome : uint8_t { None, Progress, HeldInflight };
    struct Read {
        std::shared_ptr<const Payload> payload;
        uint32_t lba = 0, sectors = 0;
        uint64_t order = 0, due = 0;
        double end_ms = 0;
        Phase phase = Phase::Pending;
        // A bank start queues an immutable request; send/wait accepts it.
        // Ordinary file and music requests are already accepted at submit.
        bool accepted = true;
    };
    struct State {
        bool deferred = true;
        uint32_t mode = 0, visibility = 0;
        double scale = 0, poll_scale = 0;
        uint64_t tick = 0, next_order = 0;
        bool service_time = false;
        ServiceTime::State service;
        ReadTiming::Position timing;
        ReadTiming::Budget budget;
        std::map<uint32_t, Read> reads;
        std::deque<uint64_t> arrivals;
        PollOutcome note = PollOutcome::None;
        uint32_t noted_object = 0;
    };
    struct Counters {
        uint64_t issued = 0, delivered = 0, consumed = 0, cancelled = 0, deferred = 0,
                 poll_delivered = 0, boundary_delivered = 0, wait_delivered = 0,
                 wait_parks = 0, overruns = 0, rejected = 0;
    };
    using Now = std::function<uint64_t()>;
    using Park = std::function<void()>;
    using Apply = std::function<void(const Payload&)>;
    using TimeSource = std::function<double()>;

    FileSchedule();
    void Bind(std::shared_ptr<ReadTiming> timing, std::shared_ptr<ReadTiming::Budget> budget,
              Now now, Apply apply);
    void SetParkFrame(Park park);
    // Bind before requests exist. The optional observer supplies the current
    // logical instant of remaining service owners, read-only during migration.
    bool SetServiceTime(bool selected, TimeSource remaining = {});
    bool UsesServiceTime() const { return service_time_; }
    double ServiceInstantMs() const;
    bool ServiceHasHeadroom() const;
    uint64_t ServiceSequence() const { return service_.Capture().sequence; }
    // Cancellation observers run on the owner after removing the request.
    // They may release a typed asset ticket; they cannot publish file bytes.
    void SetCancel(Apply cancel);
    bool Submit(Payload payload, uint32_t lba, uint32_t sectors, uint64_t issue_tick,
                bool staged_sound = false);
    // Snapshot the finite prefix before calling the remaining send service.
    // A failed send must not call AcceptSounds. Callback-created requests have
    // higher orders and remain staged for the next acceptance boundary.
    uint64_t LastOrder() const;
    // A drive extent a service consumes without a delivery of its own (a
    // music stream's buffer refill: the bytes are already owned, the drive
    // time is not). Charged at the current service instant on the shared
    // read position, so later requests see the drive busy. Returns the
    // charged end (ms), 0 when the schedule charges nothing (instant model).
    double ChargeRead(uint32_t lba, uint32_t sectors);
    void AcceptSounds(uint64_t through, double accepted_ms = -1);
    PollOutcome Poll(uint32_t object);
    bool Wait(uint32_t object);
    void AdvanceBoundary(uint64_t tick);
    void CancelHandle(int32_t handle);
    void CancelObject(uint32_t object);
    void Reset();
    bool InFlight(uint32_t object) const;
    bool Released(uint32_t object) const;
    bool HasSoundReads() const;
    bool HasMusicReads() const;
    // Logical music keys occupy 1..48, disjoint from checked EE object keys.
    static constexpr uint32_t MusicObject(uint32_t stream) { return stream + 1; }
    bool PollVisibility() const;
    bool ShouldYieldPollSlice(uint32_t address, uint32_t site, bool poll_ran);
    uint64_t DeliveredAndConsumed() const;
    bool CanCheckpoint() const;
    bool Capture(State& out, std::string* error = nullptr) const;
    bool Validate(const State& state, std::string* error = nullptr) const;
    bool Restore(const State& state, std::string* error = nullptr);
    const Counters& Count() const;

private:
    enum class Site { Poll, Boundary, Wait };
    void IssueDelivery(uint32_t object);
    void Accept(uint32_t object, uint64_t tick, double accepted_ms);
    double AcceptanceMs(Destination kind, uint64_t tick) const;
    void Release(uint32_t object, Site site);
    void Advance(uint64_t tick, bool polling);
    void RemoveArrival(uint64_t order);
    void NotifyCancelled(const std::vector<std::shared_ptr<const Payload>>& payloads);
    uint32_t ObjectOf(uint64_t order) const;
    uint64_t Tick() const;
    static bool ValidPayload(const Payload& payload);
    static bool Fail(std::string* error, const char* message);
    std::shared_ptr<ReadTiming> timing_;
    std::shared_ptr<ReadTiming::Budget> budget_;
    Now now_;
    Park park_;
    TimeSource remaining_time_;
    ServiceTime service_;
    bool service_time_ = false;
    Apply apply_;
    Apply cancel_;
    Scheduler deliveries_;
    State state_;
    std::map<uint32_t, Scheduler::Ticket> tickets_;
    std::map<uint32_t, Site> delivery_sites_;
    Counters counters_;
    uint32_t cancellation_depth_ = 0;
};
