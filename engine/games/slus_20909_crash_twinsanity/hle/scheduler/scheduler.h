#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

// Ordered service completions on the game's logical clock. A worker may
// Publish an owned result; only the game execution owner may Drain it. Neither
// publication nor cancellation executes a callback or changes guest memory.
class Scheduler {
public:
    struct Ticket {
        uint64_t generation = 0;
        uint64_t sequence = 0;
        explicit operator bool() const { return generation != 0 && sequence != 0; }
    };
    struct Result {
        int32_t error = 0;
        std::vector<uint8_t> bytes;
    };
    using Delivery = std::function<void(Result)>;
    using OwnerCheck = std::function<bool()>;
    enum class State { Missing, Pending, Ready, Settled, Cancelled };
    struct Counters {
        uint64_t issued = 0, published = 0, delivered = 0, cancelled = 0,
                 stale = 0, wrong_owner = 0;
    };

    // An empty check means the caller supplies the execution-owner contract.
    // This permits cooperative EE threads sharing a run token, rather than
    // incorrectly equating "game owner" with one OS thread identity.
    explicit Scheduler(OwnerCheck owner = {});
    Ticket Issue(uint64_t due_tick, Delivery delivery);
    bool Publish(Ticket ticket, Result result);
    bool Cancel(Ticket ticket);
    // Settled includes cancelled entries removed by Drain. Success/error is
    // reported only by the delivery callback, never inferred from this status.
    State Status(Ticket ticket) const;
    // All ready predecessors run in issue order before a later completion.
    // A callback can issue/cancel/reset or recursively drain without deadlock.
    // Jobs issued by callbacks are left for the next drain's finite batch.
    uint64_t Drain(uint64_t now_tick);
    void Reset();
    bool CanCheckpoint() const;
    bool Draining() const;
    uint64_t Pending() const;
    Counters Count() const;

private:
    struct Job {
        uint64_t due_tick = 0;
        Delivery delivery;
        Result result;
        State state = State::Pending;
    };
    bool Owned();
    mutable std::mutex mutex_;
    OwnerCheck owner_;
    std::map<uint64_t, Job> jobs_;
    uint64_t generation_ = 1, sequence_ = 0, completed_ = 0;
    bool draining_ = false;
    Counters counters_;
};
