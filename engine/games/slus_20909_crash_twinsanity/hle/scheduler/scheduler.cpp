#include "games/slus_20909_crash_twinsanity/hle/scheduler/scheduler.h"

#include <utility>

Scheduler::Scheduler(OwnerCheck owner) : owner_(std::move(owner)) {}

bool Scheduler::Owned() {
    if (!owner_ || owner_()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.wrong_owner;
    return false;
}

Scheduler::Ticket Scheduler::Issue(uint64_t due_tick, Delivery delivery) {
    if (!Owned() || !delivery) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    // Never wrap an identity into one which an old worker could still hold.
    if (sequence_ == UINT64_MAX) return {};
    const auto sequence = ++sequence_;
    Job job;
    job.due_tick = due_tick;
    job.delivery = std::move(delivery);
    jobs_.emplace(sequence, std::move(job));
    ++counters_.issued;
    return {generation_, sequence};
}

bool Scheduler::Publish(Ticket ticket, Result result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(ticket.sequence);
    if (ticket.generation != generation_ || it == jobs_.end() ||
        it->second.state != State::Pending) {
        ++counters_.stale;
        return false;
    }
    it->second.result = std::move(result);
    it->second.state = State::Ready;
    ++counters_.published;
    return true;
}

bool Scheduler::Cancel(Ticket ticket) {
    if (!Owned()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(ticket.sequence);
    if (ticket.generation != generation_ || it == jobs_.end() ||
        it->second.state == State::Cancelled) return false;
    it->second.state = State::Cancelled;
    it->second.delivery = {};
    it->second.result = {};
    ++counters_.cancelled;
    return true;
}

Scheduler::State Scheduler::Status(Ticket ticket) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ticket || ticket.generation != generation_) return State::Missing;
    const auto it = jobs_.find(ticket.sequence);
    if (it != jobs_.end()) return it->second.state;
    // Delivered tickets are useful to callers only until reset. Cancelled
    // tickets leave the queue at a drain and likewise become terminal here.
    return ticket.sequence <= completed_ ? State::Settled : State::Missing;
}

uint64_t Scheduler::Drain(uint64_t now_tick) {
    if (!Owned()) return 0;
    uint64_t epoch = 0, limit = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (draining_) return 0;
        draining_ = true;
        epoch = generation_;
        limit = sequence_;
    }
    uint64_t delivered = 0;
    try {
        for (;;) {
            Delivery delivery;
            Result result;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_ != epoch || jobs_.empty()) break;
                auto it = jobs_.begin();
                auto& job = it->second;
                if (it->first > limit) break;
                if (job.state != State::Cancelled &&
                    (job.state != State::Ready || job.due_tick > now_tick)) break;
                delivery = std::move(job.delivery);
                result = std::move(job.result);
                completed_ = it->first;
                jobs_.erase(it);
            }
            // No internal lock survives a game callback.
            if (delivery) {
                delivery(std::move(result));
                ++delivered;
                std::lock_guard<std::mutex> lock(mutex_);
                ++counters_.delivered;
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        draining_ = false;
        throw;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    draining_ = false;
    return delivered;
}

void Scheduler::Reset() {
    if (!Owned()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : jobs_)
        if (entry.second.state != State::Cancelled) ++counters_.cancelled;
    jobs_.clear();
    // Reset invalidates worker results even if the object/channel is reused.
    // sequence_ is never reused; wrap cannot resurrect an old ticket.
    if (generation_ != UINT64_MAX) ++generation_;
    completed_ = 0;
}

bool Scheduler::CanCheckpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.empty() && !draining_;
}
bool Scheduler::Draining() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return draining_;
}

uint64_t Scheduler::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

Scheduler::Counters Scheduler::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}
