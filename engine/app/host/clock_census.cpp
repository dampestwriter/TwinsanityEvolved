#include "app/host/clock_census.h"

#include "engine/platform/host/platform.h"

namespace ps2r::app {

// Through the host arm (CLOCK_THREAD_CPUTIME_ID on POSIX, GetThreadTimes on Win32):
// this TU names no OS clock.
uint64_t ClockCensus::ThreadCpuNs() { return ps2r::host::ThreadCpuTimeNs(); }

void ClockCensus::RollLocked(Clock::time_point now) {
    // Close every whole second that elapsed (an idle stretch yields zero windows).
    while (now - window_start_ >= std::chrono::seconds(1)) {
        done_.push_back(current_);
        current_ = Window{};
        window_start_ += std::chrono::seconds(1);
    }
}

void ClockCensus::SetEdgeLookup(EdgeLookup lookup) {
    std::lock_guard<std::mutex> lk(mu_);
    edge_lookup_ = std::move(lookup);
}

void ClockCensus::NoteBoundaryAt(uint64_t domain_tick, Clock::time_point now) {
    NoteBoundaryAt(domain_tick, now, ThreadCpuNs());
}

void ClockCensus::NoteBoundaryAt(uint64_t domain_tick, Clock::time_point now, uint64_t thread_cpu_ns, uint64_t fast_forward_fields,
                                 uint64_t dropped_fields) {
    std::lock_guard<std::mutex> lk(mu_);
    // The pacer's tick: the domain tick less the fast-forward fields the pacer
    // never ticked, plus the pacer ticks the catch-up cap dropped (the domain
    // never ticked them) -- the key the edge lookup understands.
    const uint64_t pacer_tick = (domain_tick >= fast_forward_fields ? domain_tick - fast_forward_fields : 0) + dropped_fields;
    // The boundary's time: its first field's wall edge when the pacer knows it
    // (never later than now, never before the current window), else now.
    Clock::time_point at = now;
    if (edge_lookup_ && (!started_ || pacer_tick > last_tick_)) {
        Clock::time_point edge;
        const uint64_t first_field = started_ ? last_tick_ + 1 : pacer_tick;
        if (edge_lookup_(first_field, &edge) && edge <= now) at = edge;
    }
    if (!started_) {
        started_ = true;
        window_start_ = at;
        origin_ = at;
        last_tick_ = pacer_tick;
        last_domain_tick_ = domain_tick;
        last_dropped_ = dropped_fields;
        last_cpu_ns_ = thread_cpu_ns;
    }
    // The boundary's window is its edge's -- even one that has already CLOSED: a
    // boundary the lookup could not place (attributed by completion time) may have
    // rolled the window past edges still to be answered by the catch-up boundaries
    // after it (rule 3's single-field form); clamping those into the current window
    // printed a short window followed by a long one (56 / 64) for edges all answered.
    // The window counts the edges the game answered, whenever it answered them.
    Window* w = &current_;
    if (at < window_start_) {
        const auto idx = std::chrono::duration_cast<std::chrono::seconds>(at - origin_).count();
        if (at >= origin_ && idx >= 0 && static_cast<size_t>(idx) < done_.size()) w = &done_[static_cast<size_t>(idx)];
    } else {
        RollLocked(at);
    }
    w->boundaries++;
    // The edges this boundary's stall left unanswered belong to its window.
    if (dropped_fields > last_dropped_) w->dropped += static_cast<uint32_t>(dropped_fields - last_dropped_);
    last_dropped_ = dropped_fields;
    // The game thread's CPU since the previous boundary, credited to this window
    // in whole ms (the remainder carries, so the series sums to the total).
    if (thread_cpu_ns >= last_cpu_ns_) {
        const uint64_t delta = thread_cpu_ns - last_cpu_ns_ + cpu_carry_ns_;
        w->cpu_ms += static_cast<uint32_t>(delta / 1000000ull);
        cpu_carry_ns_ = delta % 1000000ull;
        cpu_total_ns_ += thread_cpu_ns - last_cpu_ns_;
    }
    last_cpu_ns_ = thread_cpu_ns;
    if (domain_tick > last_domain_tick_) w->fields += static_cast<uint32_t>(domain_tick - last_domain_tick_);
    last_domain_tick_ = domain_tick;
    if (pacer_tick > last_tick_) last_tick_ = pacer_tick;
}

void ClockCensus::NotePresentAt(Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return;   // presents before the first boundary belong to no window
    RollLocked(now);
    current_.presents++;
}

std::string ClockCensus::SeriesLocked(uint32_t Window::*field) const {
    std::string s;
    for (const Window& w : done_) {
        if (!s.empty()) s += ',';
        s += std::to_string(w.*field);
    }
    return s;
}

std::string ClockCensus::GameHzSeries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return SeriesLocked(&Window::boundaries);
}

std::string ClockCensus::FieldHzSeries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return SeriesLocked(&Window::fields);
}

std::string ClockCensus::RenderFpsSeries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return SeriesLocked(&Window::presents);
}

std::string ClockCensus::EeCpuMsSeries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return SeriesLocked(&Window::cpu_ms);
}

std::string ClockCensus::DroppedSeries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return SeriesLocked(&Window::dropped);
}

uint64_t ClockCensus::EeCpuTotalMs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cpu_total_ns_ / 1000000ull;
}

std::vector<ClockCensus::Window> ClockCensus::Windows() const {
    std::lock_guard<std::mutex> lk(mu_);
    return done_;
}

uint32_t ClockCensus::LatestGameHz() const {
    std::lock_guard<std::mutex> lk(mu_);
    return done_.empty() ? 0u : done_.back().boundaries;
}

uint32_t ClockCensus::LatestRenderFps() const {
    std::lock_guard<std::mutex> lk(mu_);
    return done_.empty() ? 0u : done_.back().presents;
}

}  // namespace ps2r::app
