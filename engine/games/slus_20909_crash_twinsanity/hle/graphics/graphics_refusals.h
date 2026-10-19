#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace ps2r::verify { class RunMeta; }

namespace title {

// Counted, attributed refusals of the selected Graphics producers.
//
// A REFUSAL is a selected call that returned to the ORIGINAL body before the
// owned arm committed anything: no guest write, no queued item, no bypassed
// source side effect. The model then renders that draw; the family's own
// model_calls census rises, so the selection cannot flip a default -- but the
// run completes and every refusal is attributed (frame, source frame, producer
// entry, family-specific detail code) instead of aborting the process.
//
// A FAULT is a failure AFTER commitment (guest state written, work recorded,
// source producer already bypassed). It is attributed the same way and the
// caller still throws: a committed selected arm is never replayed by the model.
//
// Reasons: MissingResource = an asset/texture/model this owner never converted
// (the original loaded it, or no load was observed); UnsupportedState = a
// source object/material/stream/geometry shape this owner does not model;
// Ordering = the source frame/environment/predecessor/bracket is not in the
// state the producer needs (environment not established, interior entry,
// nested or unopened pass, reservation refused by the ordered output).
//
// Producer-time refusals are recorded on the source (EE) thread; a retained
// delivery at the ordered output slot is attributed by the drain owner (the
// EE thread or the GIF drain thread). The ledger serialises both.
class GraphicsRefusals {
public:
    enum class Family : uint8_t { Textures, Sprites, Text, Sky, Strips, Frame, Shadows, Meshes, Count };
    enum class Reason : uint8_t { MissingResource, UnsupportedState, Ordering, Count };
    // Family-side callback: reason, the producer entry (NTSC address) and the
    // family's detail code (its Result enum or a documented local code).
    using Sink = std::function<void(Reason, uint32_t producer, uint32_t detail)>;
    using Clock = std::function<uint64_t()>;
    struct Site {
        bool recorded = false;
        Reason reason = Reason::UnsupportedState;
        uint64_t ordinal = 0;      // 1-based count of this family's refusals (or faults)
        uint64_t frame = 0;        // host leg frame (VerifyHost boundary) when a clock is set
        uint64_t source_frame = 0; // source Link ordinal when a clock is set
        uint32_t producer = 0, detail = 0;
    };
    struct Counts {
        uint64_t total = 0, faults = 0;
        std::array<uint64_t, size_t(Reason::Count)> reasons{};
        Site first, last, first_fault;
    };

    // Either clock may stay empty; the matching site field then reads 0.
    void SetClocks(Clock host_frame, Clock source_frame);
    void Refuse(Family family, Reason reason, uint32_t producer, uint32_t detail);
    void Fault(Family family, Reason reason, uint32_t producer, uint32_t detail);
    Sink Refuser(Family family);
    Sink Faulter(Family family);
    Counts Of(Family family) const;
    // Owner swaps (restore publishes a fresh producer composition) keep the
    // process totals: add the retired owner's counts into this ledger.
    void Retire(const GraphicsRefusals& retired);
    // graphics_refusals_<family>=total=..,missing_resource=..,unsupported_state=..,
    // ordering=..,faults=..,first=<site>,last=<site>,first_fault=<site>
    // where <site> is none or frame:F/source_frame:S/n:N/producer:0xADDR/detail:D/reason:R.
    void Record(ps2r::verify::RunMeta& meta, std::initializer_list<Family> families) const;
    static const char* Name(Family family);
    static const char* Name(Reason reason);
    static std::string Describe(const Site& site);

private:
    Site Stamp(Reason reason, uint64_t ordinal, uint32_t producer, uint32_t detail) const;
    mutable std::mutex mutex_;
    std::array<Counts, size_t(Family::Count)> families_{};
    Clock host_frame_, source_frame_;
};

}  // namespace title
