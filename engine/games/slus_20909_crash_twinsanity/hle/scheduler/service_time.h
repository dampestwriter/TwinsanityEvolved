#pragma once

#include <cstdint>

// Logical position of owned service work on the game's existing 60 Hz clock.
// Waiting settles an accepted deadline. It never invents per-call CPU cost,
// advances the game tick, sleeps, or writes another owner's clock.
class ServiceTime {
public:
    struct State {
        double settled_ms = 0;
        uint64_t sequence = 0;
    };
    double Now(uint64_t tick, double observed_ms = 0) const;
    bool HasHeadroom(uint64_t tick, double observed_ms = 0) const;
    void Settle(uint64_t tick, double deadline_ms, double observed_ms = 0);
    State Capture() const { return state_; }
    static bool Validate(const State& state);
    bool Restore(const State& state);
    void Reset() { state_ = {}; }

private:
    State state_;
};
