#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"

#include <cstdio>

#include "engine/core/verify/run_meta.h"

namespace title {

const char* GraphicsRefusals::Name(Family family) {
    switch (family) {
        case Family::Textures: return "textures";
        case Family::Sprites: return "sprites";
        case Family::Text: return "text";
        case Family::Sky: return "sky";
        case Family::Strips: return "strips";
        case Family::Frame: return "frame";
        case Family::Shadows: return "shadows";
        case Family::Meshes: return "meshes";
        case Family::Count: break;
    }
    return "unknown";
}
const char* GraphicsRefusals::Name(Reason reason) {
    switch (reason) {
        case Reason::MissingResource: return "missing_resource";
        case Reason::UnsupportedState: return "unsupported_state";
        case Reason::Ordering: return "ordering";
        case Reason::Count: break;
    }
    return "unknown";
}

void GraphicsRefusals::SetClocks(Clock host_frame, Clock source_frame) {
    host_frame_ = std::move(host_frame);
    source_frame_ = std::move(source_frame);
}

GraphicsRefusals::Site GraphicsRefusals::Stamp(Reason reason, uint64_t ordinal,
                                               uint32_t producer, uint32_t detail) const {
    Site site;
    site.recorded = true;
    site.reason = reason;
    site.ordinal = ordinal;
    site.producer = producer;
    site.detail = detail;
    // A clock that throws must not turn an attributed refusal into an abort.
    try { if (host_frame_) site.frame = host_frame_(); } catch (...) { site.frame = 0; }
    try { if (source_frame_) site.source_frame = source_frame_(); } catch (...) { site.source_frame = 0; }
    return site;
}

GraphicsRefusals::Counts GraphicsRefusals::Of(Family family) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return families_[size_t(family)];
}

void GraphicsRefusals::Refuse(Family family, Reason reason, uint32_t producer, uint32_t detail) {
    if (family >= Family::Count || reason >= Reason::Count) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& counts = families_[size_t(family)];
    ++counts.total;
    ++counts.reasons[size_t(reason)];
    const auto site = Stamp(reason, counts.total, producer, detail);
    if (!counts.first.recorded) counts.first = site;
    counts.last = site;
}

void GraphicsRefusals::Fault(Family family, Reason reason, uint32_t producer, uint32_t detail) {
    if (family >= Family::Count || reason >= Reason::Count) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& counts = families_[size_t(family)];
    ++counts.faults;
    if (!counts.first_fault.recorded) counts.first_fault = Stamp(reason, counts.faults, producer, detail);
}

GraphicsRefusals::Sink GraphicsRefusals::Refuser(Family family) {
    return [this, family](Reason reason, uint32_t producer, uint32_t detail) {
        Refuse(family, reason, producer, detail);
    };
}
GraphicsRefusals::Sink GraphicsRefusals::Faulter(Family family) {
    return [this, family](Reason reason, uint32_t producer, uint32_t detail) {
        Fault(family, reason, producer, detail);
    };
}

void GraphicsRefusals::Retire(const GraphicsRefusals& retired) {
    if (&retired == this) return;
    std::scoped_lock lock(mutex_, retired.mutex_);
    for (size_t f = 0; f < families_.size(); ++f) {
        auto& mine = families_[f];
        const auto& old = retired.families_[f];
        mine.total += old.total;
        mine.faults += old.faults;
        for (size_t r = 0; r < mine.reasons.size(); ++r) mine.reasons[r] += old.reasons[r];
        if (!mine.first.recorded) mine.first = old.first;
        if (old.last.recorded) mine.last = old.last;
        if (!mine.first_fault.recorded) mine.first_fault = old.first_fault;
    }
}

std::string GraphicsRefusals::Describe(const Site& site) {
    if (!site.recorded) return "none";
    char producer[16];
    std::snprintf(producer, sizeof(producer), "0x%06x", unsigned(site.producer));
    return "frame:" + std::to_string(site.frame) + "/source_frame:" + std::to_string(site.source_frame) +
           "/n:" + std::to_string(site.ordinal) + "/producer:" + producer +
           "/detail:" + std::to_string(site.detail) + "/reason:" + Name(site.reason);
}

void GraphicsRefusals::Record(ps2r::verify::RunMeta& meta, std::initializer_list<Family> families) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto family : families) {
        if (family >= Family::Count) continue;
        const auto& counts = families_[size_t(family)];
        std::string row = "total=" + std::to_string(counts.total);
        for (size_t r = 0; r < counts.reasons.size(); ++r)
            row += std::string(",") + Name(Reason(r)) + "=" + std::to_string(counts.reasons[r]);
        row += ",faults=" + std::to_string(counts.faults);
        row += ",first=" + Describe(counts.first) + ",last=" + Describe(counts.last) +
               ",first_fault=" + Describe(counts.first_fault);
        meta.Set(std::string("graphics_refusals_") + Name(family), row);
    }
}

}  // namespace title
