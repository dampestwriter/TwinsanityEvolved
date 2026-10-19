// engine/platform/ps2e/present_pacer.cpp -- PresentPacer. See the header.
#include "engine/platform/ps2e/present_pacer.h"

#include <cctype>
#include <cstring>

#include "engine/platform/host/platform.h"

namespace ps2r {
namespace ps2e {

namespace {
const char* const kNames[] = {"30", "60", "120", "144", "240", "uncapped"};
const uint32_t kHz[] = {30, 60, 120, 144, 240, 0};
constexpr int64_t kCoarseMarginNs = 1000000;   // sleep coarsely to one millisecond before the slot
constexpr int64_t kFineStepNs = 100000;        // then in 100 us steps (nanosleep / the waitable timer round up)
}  // namespace

const char* PresentPacer::Name(Rate rate) { return kNames[static_cast<unsigned>(rate) < 6 ? static_cast<unsigned>(rate) : 1]; }

bool PresentPacer::Parse(const char* text, Rate* out) {
    if (!text || !out) return false;
    char lower[16] = {};
    size_t n = 0;
    for (; text[n] && n < sizeof lower - 1; ++n) lower[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[n])));
    if (text[n]) return false;
    for (unsigned i = 0; i < 6; ++i) {
        if (std::strcmp(lower, kNames[i]) == 0) {
            *out = static_cast<Rate>(i);
            return true;
        }
    }
    return false;
}

uint32_t PresentPacer::Hz(Rate rate) { return kHz[static_cast<unsigned>(rate) < 6 ? static_cast<unsigned>(rate) : 1]; }

uint32_t PresentPacer::SlotsPerTick(Rate rate, uint32_t game_hz) {
    const uint32_t hz = Hz(rate);
    if (!game_hz || !hz || hz % game_hz) return 0;
    return hz / game_hz;
}

void PresentPacer::SetRate(Rate rate) {
    rate_ = rate;
    const uint32_t hz = Hz(rate);
    period_ns_ = hz ? static_cast<int64_t>(1000000000ull / hz) : 0;
    anchored_ = false;
    k_ = 0;
}

bool PresentPacer::ShowsPicture(uint64_t picture) const {
    if (rate_ == Rate::Rate30) return (picture & 1u) == 0;   // every other picture, the even ones: a steady cadence
    return true;
}

void PresentPacer::OnTickEdge(int64_t edge_ns) {
    if (!PacedBySlots(rate_) || rate_ == Rate::Uncapped) return;
    const uint32_t m = SlotsPerTick(rate_);
    if (m == 0 && anchored_) return;   // 144: free-running once anchored
    anchor_ns_ = edge_ns;
    // the next slot on the new train is the first one at least half a period after the last slot served (the old
    // train's slot on this very edge may already have been served: never two presents for one slot)
    k_ = 0;
    if (anchored_ && period_ns_ > 0) {
        while (anchor_ns_ + static_cast<int64_t>(k_) * period_ns_ < last_slot_ns_ + period_ns_ / 2) ++k_;
    }
    anchored_ = true;
    census_.reanchors++;
}

int64_t PresentPacer::NextSlotNs(int64_t now_ns) {
    if (rate_ == Rate::Uncapped || !PacedBySlots(rate_)) return now_ns;
    if (!anchored_) {
        anchor_ns_ = now_ns;
        k_ = 0;
        anchored_ = true;
        census_.reanchors++;
    }
    int64_t slot = anchor_ns_ + static_cast<int64_t>(k_) * period_ns_;
    if (slot + period_ns_ < now_ns) {
        // more than one slot behind (a stall, a long picture): re-anchor at now, no burst of catch-up presents
        anchor_ns_ = now_ns;
        k_ = 0;
        slot = now_ns;
        census_.reanchors++;
    }
    return slot;
}

void PresentPacer::SlotServed(int64_t slot_ns, int64_t now_ns) {
    census_.slots++;
    k_++;
    last_slot_ns_ = slot_ns;
    if (period_ns_ > 0 && now_ns > slot_ns + period_ns_) {
        census_.late++;
        const double late_ms = double(now_ns - slot_ns) / 1e6;
        if (late_ms > census_.late_ms_max) census_.late_ms_max = late_ms;
    }
}

int64_t PresentPacer::NowNs() { return static_cast<int64_t>(ps2r::host::MonotonicNowNs()); }

void PresentPacer::SleepUntilNs(int64_t deadline_ns) {
    for (;;) {
        const int64_t now = NowNs();
        if (now >= deadline_ns) return;
        const int64_t remaining = deadline_ns - now;
        if (remaining > kCoarseMarginNs) ps2r::host::SleepForNs(static_cast<uint64_t>(remaining - kCoarseMarginNs));
        else ps2r::host::SleepForNs(static_cast<uint64_t>(remaining < kFineStepNs ? remaining : kFineStepNs));
    }
}

}  // namespace ps2e
}  // namespace ps2r
