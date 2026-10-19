#include "games/slus_20909_crash_twinsanity/hle/stream/file_schedule.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace {
bool Extent(uint32_t address, size_t size) {
    return address >= 0x10000 && address < 0x02000000 && size <= 0x02000000 - address;
}
}

FileSchedule::FileSchedule()
    : timing_(std::make_shared<ReadTiming>(ps2r::CdvdConfig{})),
      budget_(std::make_shared<ReadTiming::Budget>()) {}
void FileSchedule::Bind(std::shared_ptr<ReadTiming> timing, std::shared_ptr<ReadTiming::Budget> budget,
                        Now now, Apply apply) {
    state_.deferred = timing != nullptr;
    if (timing) timing_ = std::move(timing);
    if (budget) budget_ = std::move(budget);
    now_ = std::move(now);
    apply_ = std::move(apply);
}
void FileSchedule::SetParkFrame(Park park) { park_ = std::move(park); }
bool FileSchedule::SetServiceTime(bool selected, TimeSource remaining) {
    if (!state_.reads.empty() || !CanCheckpoint()) return false;
    if (selected != service_time_) service_.Reset();
    service_time_ = selected;
    remaining_time_ = selected ? std::move(remaining) : TimeSource{};
    return true;
}
double FileSchedule::ServiceInstantMs() const {
    return service_time_ ? service_.Now(Tick(), remaining_time_ ? remaining_time_() : 0) :
                           static_cast<double>(Tick()) * ReadTiming::kTickMs;
}
bool FileSchedule::ServiceHasHeadroom() const {
    return !service_time_ || ServiceInstantMs() < ReadTiming::FrameEndMs(Tick());
}
void FileSchedule::SetCancel(Apply cancel) { cancel_ = std::move(cancel); }
uint64_t FileSchedule::Tick() const { return now_ ? now_() : 0; }
bool FileSchedule::Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool FileSchedule::ValidPayload(const Payload& payload) {
    if (payload.kind == Destination::Music)
        return payload.handle >= 0 && payload.stream < 48 && payload.object == MusicObject(payload.stream) &&
               payload.destination == 0 && payload.ring_base == 0 && payload.ring_cursor == 0 &&
               payload.sound == UINT32_MAX && payload.generation != 0 && payload.sequence != 0 &&
               !payload.bytes.empty() && payload.bytes.size() <= UINT32_MAX;
    if (payload.stream != UINT32_MAX) return false;
    if (payload.kind == Destination::Sound)
        return payload.handle >= 0 && Extent(payload.object, 0x48) &&
               payload.destination == 0 && payload.ring_base == 0 && payload.ring_cursor == 0 &&
               payload.sound != UINT32_MAX && payload.generation != 0 && payload.sequence != 0 &&
               !payload.bytes.empty() && payload.bytes.size() <= UINT32_MAX && payload.bytes.size() % 16 == 0;
    if (payload.kind != Destination::Memory || payload.sound != UINT32_MAX ||
        payload.generation || payload.sequence) return false;
    return payload.handle >= 0 && Extent(payload.object, 0x48) &&
           Extent(payload.destination, payload.bytes.size()) &&
           ((payload.ring_base == 0 && payload.ring_cursor == 0) ||
            (Extent(payload.ring_base, payload.bytes.size()) && Extent(payload.ring_cursor, 4)));
}
void FileSchedule::IssueDelivery(uint32_t object) {
    const auto order = state_.reads.at(object).order;
    tickets_[object] = deliveries_.Issue(0, [this, object, order](Scheduler::Result) {
        auto found = state_.reads.find(object);
        if (found == state_.reads.end() || found->second.order != order ||
            found->second.phase != Phase::Eligible) return;
        const auto payload = found->second.payload;
        const auto site = delivery_sites_.at(object);
        apply_(*payload); // owner may reset/cancel while observing delivery
        found = state_.reads.find(object);
        if (found == state_.reads.end() || found->second.order != order) return;
        found->second.phase = Phase::Delivered;
        ++counters_.delivered;
        if (site == Site::Poll) ++counters_.poll_delivered;
        else if (site == Site::Wait) ++counters_.wait_delivered;
        else ++counters_.boundary_delivered;
    });
}
bool FileSchedule::Submit(Payload payload, uint32_t lba, uint32_t sectors, uint64_t tick,
                          bool staged_sound) {
    if (!apply_ || !ValidPayload(payload) || InFlight(payload.object) ||
        state_.next_order == UINT64_MAX ||
        (staged_sound && payload.kind != Destination::Sound)) {
        ++counters_.rejected;
        return false;
    }
    const auto object = payload.object;
    Read read;
    read.payload = std::make_shared<const Payload>(std::move(payload));
    read.order = ++state_.next_order; read.lba = lba; read.sectors = sectors;
    read.accepted = false;
    state_.reads[object] = std::move(read);
    ++counters_.issued;
    // An ordinary memory read keeps the comparison arm's frame-start charge
    // (the loader's reads never ran on the service clock; the shared read
    // position already carries any settled bank). Only service-served
    // destinations (bank, music) accept at the owned service instant.
    if (!staged_sound) Accept(object, tick, AcceptanceMs(state_.reads.at(object).payload->kind, tick));
    return true;
}
double FileSchedule::AcceptanceMs(Destination kind, uint64_t tick) const {
    return kind == Destination::Memory ? static_cast<double>(tick) * ReadTiming::kTickMs : ServiceInstantMs();
}
void FileSchedule::Accept(uint32_t object, uint64_t tick, double accepted_ms) {
    auto& read = state_.reads.at(object);
    read.accepted = true;
    const auto order = read.order;
    read.due = !state_.deferred ? tick : service_time_ ?
        timing_->DueAtMs(tick, accepted_ms, read.lba, read.sectors) :
        timing_->DueTick(tick, read.lba, read.sectors);
    read.end_ms = timing_->Capture().last_end_ms;
    IssueDelivery(object);
    state_.arrivals.push_back(order);
    if (!state_.deferred) {
        RemoveArrival(order);
        Release(object, Site::Poll);
        deliveries_.Drain(0);
    }
}
uint64_t FileSchedule::LastOrder() const { return state_.next_order; }
double FileSchedule::ChargeRead(uint32_t lba, uint32_t sectors) {
    if (!state_.deferred || !sectors) return 0;
    return timing_->ChargeServiceRead(ServiceInstantMs(), lba, sectors);
}
void FileSchedule::AcceptSounds(uint64_t through, double accepted_ms) {
    if (accepted_ms < 0) accepted_ms = ServiceInstantMs();
    std::map<uint64_t, uint32_t> queued;
    for (const auto& pair : state_.reads)
        if (!pair.second.accepted && pair.second.order <= through)
            queued.emplace(pair.second.order, pair.first);
    for (const auto& pair : queued) {
        const auto found = state_.reads.find(pair.second);
        if (found == state_.reads.end() || found->second.order != pair.first) continue;
        if (state_.next_order == UINT64_MAX) { ++counters_.rejected; break; }
        // A memory read accepted while this bank was merely queued precedes
        // it on the shared timeline. Delivery order is acceptance order.
        found->second.order = ++state_.next_order;
        Accept(pair.second, Tick(), accepted_ms);
    }
}
bool FileSchedule::InFlight(uint32_t object) const {
    const auto found = state_.reads.find(object);
    return found != state_.reads.end() && found->second.phase < Phase::Delivered;
}
bool FileSchedule::Released(uint32_t object) const {
    const auto found = state_.reads.find(object);
    return found != state_.reads.end() && found->second.phase == Phase::Delivered;
}
bool FileSchedule::HasSoundReads() const {
    for (const auto& pair : state_.reads)
        if (pair.second.payload->kind == Destination::Sound && pair.second.phase < Phase::Delivered) return true;
    return false;
}
bool FileSchedule::HasMusicReads() const {
    for (const auto& pair : state_.reads)
        if (pair.second.payload->kind == Destination::Music && pair.second.phase < Phase::Delivered) return true;
    return false;
}
bool FileSchedule::PollVisibility() const { return timing_->PollVisibility(); }
uint32_t FileSchedule::ObjectOf(uint64_t order) const {
    for (const auto& pair : state_.reads) if (pair.second.order == order) return pair.first;
    return 0;
}
void FileSchedule::RemoveArrival(uint64_t order) {
    state_.arrivals.erase(std::remove(state_.arrivals.begin(), state_.arrivals.end(), order), state_.arrivals.end());
}
void FileSchedule::Release(uint32_t object, Site site) {
    auto& read = state_.reads.at(object);
    if (read.phase != Phase::Pending || !read.accepted) return;
    read.phase = Phase::Eligible;
    delivery_sites_[object] = site;
    deliveries_.Publish(tickets_.at(object), {});
}
void FileSchedule::Advance(uint64_t tick, bool polling) {
    if (polling && !PollVisibility()) return;
    while (!state_.arrivals.empty()) {
        const auto object = ObjectOf(state_.arrivals.front());
        if (!object) { state_.arrivals.pop_front(); continue; }
        auto& read = state_.reads.at(object);
        if (polling ? read.end_ms >= ReadTiming::FrameEndMs(tick) : read.due > tick) break;
        state_.arrivals.pop_front();
        if (!PollVisibility() && !ReadTiming::Admit(*budget_, read.due, static_cast<uint64_t>(read.sectors) * 2048).allowed) {
            ++counters_.deferred;
            ++read.due;
            state_.arrivals.push_back(read.order);
            continue;
        }
        Release(object, polling ? Site::Poll : Site::Boundary);
        deliveries_.Drain(0);
    }
}
FileSchedule::PollOutcome FileSchedule::Poll(uint32_t object) {
    Advance(Tick(), true);
    deliveries_.Drain(0);
    auto found = state_.reads.find(object);
    state_.note = PollOutcome::Progress; state_.noted_object = object;
    if (found != state_.reads.end()) {
        if (found->second.phase < Phase::Delivered) state_.note = PollOutcome::HeldInflight;
        else if (found->second.phase == Phase::Delivered) {
            found->second.phase = Phase::Consumed;
            ++counters_.consumed;
        }
    }
    return state_.note;
}
bool FileSchedule::Wait(uint32_t object) {
    auto found = state_.reads.find(object);
    if (found == state_.reads.end()) return false;
    const bool sound = found->second.payload->kind == Destination::Sound;
    if (sound && !found->second.accepted) {
        const auto payload = found->second.payload;
        AcceptSounds(LastOrder());
        found = state_.reads.find(object);
        if (found == state_.reads.end() || found->second.payload != payload) return false;
    }
    const auto target = found->second.order;
    // Bank waits settle service work without advancing DomainTick. Ordinary
    // file waits retain the existing game-park rule.
    while (!sound && InFlight(object) && PollVisibility() && park_ &&
           found->second.end_ms >= ReadTiming::FrameEndMs(Tick())) {
        const auto before = Tick();
        park_();
        const bool advanced = Tick() != before;
        if (advanced) ++counters_.wait_parks;
        found = state_.reads.find(object);
        if (found == state_.reads.end() || found->second.order != target) return false;
        if (!advanced) break;
    }
    if (InFlight(object) && PollVisibility() && found->second.end_ms >= ReadTiming::FrameEndMs(Tick()))
        ++counters_.overruns;
    // Only the bank wait settles: it stands in for the retired module round
    // trip. An ordinary wait already parked the game tick, as the comparison
    // arm does, and leaves the service position and its sequence untouched.
    if (sound && service_time_ && InFlight(object) && PollVisibility())
        service_.Settle(Tick(), found->second.end_ms, remaining_time_ ? remaining_time_() : 0);
    std::map<uint64_t, uint32_t> predecessors;
    for (const auto& pair : state_.reads)
        if (pair.second.accepted && pair.second.order <= target && pair.second.phase == Phase::Pending)
            predecessors.emplace(pair.second.order, pair.first);
    for (const auto& pair : predecessors) {
        const auto current = state_.reads.find(pair.second);
        if (current == state_.reads.end() || current->second.order != pair.first) continue;
        RemoveArrival(pair.first);
        Release(pair.second, Site::Wait);
        deliveries_.Drain(0);
    }
    found = state_.reads.find(object);
    return found != state_.reads.end() && found->second.order == target &&
           Poll(object) == PollOutcome::Progress;
}
void FileSchedule::AdvanceBoundary(uint64_t tick) {
    Advance(tick, false);
    deliveries_.Drain(0);
}
void FileSchedule::CancelObject(uint32_t object) {
    const auto found = state_.reads.find(object);
    if (found == state_.reads.end()) return;
    const auto payload = found->second.payload;
    const bool pending = found->second.phase < Phase::Delivered;
    const auto ticket = tickets_.find(object);
    if (ticket != tickets_.end()) deliveries_.Cancel(ticket->second);
    tickets_.erase(object); delivery_sites_.erase(object);
    RemoveArrival(found->second.order);
    state_.reads.erase(found);
    if (pending) {
        ++counters_.cancelled;
        NotifyCancelled({payload});
    }
    deliveries_.Drain(0);
}
void FileSchedule::CancelHandle(int32_t handle) {
    std::vector<std::shared_ptr<const Payload>> cancelled;
    for (auto it = state_.reads.begin(); it != state_.reads.end();) {
        if (it->second.payload->handle != handle) { ++it; continue; }
        if (it->second.phase < Phase::Delivered) {
            ++counters_.cancelled;
            cancelled.push_back(it->second.payload);
        }
        const auto ticket = tickets_.find(it->first);
        if (ticket != tickets_.end()) deliveries_.Cancel(ticket->second);
        tickets_.erase(it->first); delivery_sites_.erase(it->first);
        RemoveArrival(it->second.order);
        it = state_.reads.erase(it);
    }
    NotifyCancelled(cancelled);
    deliveries_.Drain(0);
}
void FileSchedule::Reset() {
    std::vector<std::shared_ptr<const Payload>> cancelled;
    for (const auto& pair : state_.reads)
        if (pair.second.phase < Phase::Delivered) cancelled.push_back(pair.second.payload);
    deliveries_.Reset();
    state_.reads.clear(); state_.arrivals.clear();
    tickets_.clear(); delivery_sites_.clear();
    state_.note = PollOutcome::None; state_.noted_object = 0;
    service_.Reset();
    NotifyCancelled(cancelled);
}
void FileSchedule::NotifyCancelled(const std::vector<std::shared_ptr<const Payload>>& payloads) {
    // Cancellation can enqueue new work or recurse into another close/reset.
    // A snapshot or provider rebind cannot observe this transient ownership.
    const auto callback = cancel_;
    if (!callback || payloads.empty()) return;
    ++cancellation_depth_;
    try {
        for (const auto& payload : payloads) callback(*payload);
    } catch (...) {
        --cancellation_depth_;
        throw;
    }
    --cancellation_depth_;
}
bool FileSchedule::ShouldYieldPollSlice(uint32_t address, uint32_t site, bool poll_ran) {
    if (address != site || !poll_ran || state_.note != PollOutcome::HeldInflight ||
        !InFlight(state_.noted_object)) return false;
    state_.note = PollOutcome::None;
    return true;
}
uint64_t FileSchedule::DeliveredAndConsumed() const { return counters_.delivered + counters_.consumed; }
bool FileSchedule::CanCheckpoint() const { return !deliveries_.Draining() && !cancellation_depth_; }
const FileSchedule::Counters& FileSchedule::Count() const { return counters_; }

bool FileSchedule::Capture(State& out, std::string* error) const {
    if (!CanCheckpoint()) return Fail(error, "file delivery is executing");
    State state = state_;
    state.service_time = service_time_; state.service = service_.Capture();
    state.tick = Tick(); state.timing = timing_->Capture(); state.budget = *budget_;
    state.mode = static_cast<uint32_t>(timing_->Mode());
    state.visibility = static_cast<uint32_t>(timing_->Visibility());
    state.scale = timing_->Scale(); state.poll_scale = timing_->PollScale();
    if (!Validate(state, error)) return false;
    out = std::move(state);
    return true;
}
bool FileSchedule::Validate(const State& state, std::string* error) const {
    constexpr uint64_t max_tick = uint64_t{1} << 52;
    if (state.deferred != state_.deferred || state.mode != static_cast<uint32_t>(timing_->Mode()) ||
        state.visibility != static_cast<uint32_t>(timing_->Visibility()) ||
        state.scale != timing_->Scale() || state.poll_scale != timing_->PollScale() ||
        state.service_time != service_time_ || !ServiceTime::Validate(state.service) ||
        (!state.service_time && (state.service.settled_ms != 0 || state.service.sequence != 0)) ||
        !ReadTiming::Validate(state.timing) || !ReadTiming::Validate(state.budget) ||
        state.budget.bytes != budget_->bytes || state.reads.size() > 65536 || state.tick > max_tick ||
        state.arrivals.size() > state.reads.size() || state.note > PollOutcome::HeldInflight)
        return Fail(error, "file scheduling configuration or position mismatch");
    std::map<uint64_t, uint32_t> pending;
    std::set<uint64_t> orders;
    uint64_t bytes = 0;
    for (const auto& pair : state.reads) {
        const auto& read = pair.second;
        if (!read.payload || !ValidPayload(*read.payload) || read.payload->object != pair.first ||
            !read.order || read.order > state.next_order || !orders.insert(read.order).second ||
            read.phase > Phase::Consumed || !std::isfinite(read.end_ms) || read.end_ms < 0 ||
            read.due > max_tick || read.end_ms > state.timing.busy_ms)
            return Fail(error, "invalid owned file request");
        bytes += read.payload->bytes.size();
        if (bytes > 64 * 1024 * 1024) return Fail(error, "owned file requests exceed snapshot bound");
        if (!read.accepted) {
            if (read.payload->kind != Destination::Sound || read.phase != Phase::Pending ||
                read.due != 0 || read.end_ms != 0)
                return Fail(error, "invalid unaccepted bank request");
        } else if (read.phase == Phase::Pending) pending.emplace(read.order, pair.first);
    }
    for (const auto order : state.arrivals)
        if (pending.erase(order) != 1) return Fail(error, "invalid file completion order");
    if (!pending.empty()) return Fail(error, "pending file request has no deadline");
    return true;
}
bool FileSchedule::Restore(const State& state, std::string* error) {
    if (!Validate(state, error) || !CanCheckpoint() || Tick() != state.tick)
        return Fail(error, "file clock must be restored before its requests");
    Reset();
    state_ = state;
    service_.Restore(state.service);
    timing_->Restore(state.timing); *budget_ = state.budget;
    std::map<uint64_t, uint32_t> ordered;
    for (const auto& pair : state_.reads)
        if (pair.second.accepted && pair.second.phase < Phase::Delivered)
            ordered.emplace(pair.second.order, pair.first);
    for (const auto& pair : ordered) {
        IssueDelivery(pair.second);
        if (state_.reads.at(pair.second).phase == Phase::Eligible) {
            delivery_sites_[pair.second] = Site::Boundary;
            deliveries_.Publish(tickets_.at(pair.second), {});
        }
    }
    return true;
}
