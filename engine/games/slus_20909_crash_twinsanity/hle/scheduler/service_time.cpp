#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_time.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "engine/services/read_timing.h"

double ServiceTime::Now(uint64_t tick, double observed_ms) const {
    if (!std::isfinite(observed_ms) || observed_ms < 0 || observed_ms > 1e15)
        throw std::logic_error("invalid service observation");
    return std::max({static_cast<double>(tick) * ReadTiming::kTickMs, state_.settled_ms, observed_ms});
}
bool ServiceTime::HasHeadroom(uint64_t tick, double observed_ms) const {
    return Now(tick, observed_ms) < ReadTiming::FrameEndMs(tick);
}
void ServiceTime::Settle(uint64_t tick, double deadline_ms, double observed_ms) {
    if (!std::isfinite(deadline_ms) || deadline_ms < 0 || deadline_ms > 1e15)
        throw std::logic_error("invalid service deadline");
    const double settled = std::max(Now(tick, observed_ms), deadline_ms);
    if (settled <= state_.settled_ms) return;
    if (state_.sequence == UINT64_MAX) throw std::overflow_error("service sequence exhausted");
    state_.settled_ms = settled;
    ++state_.sequence;
}
bool ServiceTime::Validate(const State& state) {
    return std::isfinite(state.settled_ms) && state.settled_ms >= 0 && state.settled_ms <= 1e15 &&
        (state.sequence != 0 || state.settled_ms == 0) && state.sequence != UINT64_MAX;
}
bool ServiceTime::Restore(const State& state) {
    if (!Validate(state)) return false;
    state_ = state;
    return true;
}
