#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_bridge.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_intent.h"

namespace title {

// The verified NTSC flat Strip producer and its ordered frame continuation.
// Legacy producers keep their original code. This service does not write guest
// tags, advance packet pointers, update last2DMaterial or reproduce MMIO.
class GraphicsService {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    static constexpr uint32_t kStripAddress = GraphicsBuckets::kStripAddress;
    static constexpr uint32_t kLinkAddress = GraphicsBuckets::kSubmitAddress;
    static constexpr std::array<uint32_t, 4> kStripAddresses{
        StripIntent::kPlainAddress, StripIntent::kTintedAddress,
        StripIntent::kPlacedAddress, StripIntent::kPlacedTintedAddress};
    // retirement: Model/All select the two same-input self-oracle arms. Plain keeps
    // the previously accepted default until the full four-variant gate passes.
    // A selected Strip call whose source intent, environment or ordered target
    // is unavailable REFUSES: nothing was committed, so the ORIGINAL producer
    // draws that strip (counted as its model call and attributed through the
    // refusal sink). Faults (an owned capture that threw after acceptance, a
    // lost predecessor, a failed delivery) still throw and are never replayed.
    enum class StripSelection { Model, Plain, All };
    // Detail codes of a Strip refusal: kInterior/kEnvironment/kRecordRefused
    // or kCapture | StripIntent::Result.
    static constexpr uint32_t kInterior = 1, kEnvironment = 2, kRecordRefused = 3, kDiscarded = 4, kCapture = 0x100;
    static constexpr StripSelection kDefaultStripSelection = StripSelection::Plain;

    struct Hooks {
        StripSelection strip_selection = kDefaultStripSelection;
        GraphicsBuckets::Reader read;
        GraphicsBridge::Hooks delivery;
        // Source-owned frame environment. False means its complete state is
        // not established: All refuses; Plain retains its accepted fallback.
        // This MUST NOT snapshot GsState to fill missing source intent.
        std::function<bool(MaterialState::Environment&)> environment;
        // Called only at an accepted anchor, on the chain-walk owner, before
        // enqueue. Opens preceding staged work's ordered marker. Inline mode
        // may supply a no-op; this must not wait on the GIF drain owner.
        std::function<void()> flush_staged;
        // Required for All. Captures the complete source frame and records the
        // owned draw through RecordOwned. Accepted Plain keeps its old sink.
        std::function<bool(StripIntent::Output)> record_strip;
        // Attribution of refusals (Plain and All fallbacks) and faults.
        GraphicsRefusals::Sink refuse, fault;
        // Optional, at every complete Link before the original runs: the 28
        // buckets' `first` tag addresses (physical) of the chain being linked
        // (FrameBuckets[b].first, NTSC 0x323658 + 8 + b*0x38) -- the bucket
        // boundaries of the walk that follows, for census attribution only.
        std::function<void(const std::array<uint32_t, Graphics::kBucketCount>&)> linked_buckets;
    };
    struct VariantCounters {
        uint64_t calls = 0;  // complete entries
        uint64_t replaced = 0;
        uint64_t model_calls = 0;  // every generated call, including interiors
        uint64_t interior_fallback = 0;
    };
    struct Counters {
        uint64_t strip_calls = 0;
        uint64_t replaced_producers = 0;
        uint64_t empty_producers = 0; // complete source calls with no backend draw
        uint64_t owned_strip_producers = 0;
        uint64_t fallback = 0;
        uint64_t interior_fallback = 0;
        uint64_t linked_submissions = 0;
        uint64_t ordering_errors = 0;
        // Walks that ended with accepted predecessors unvisited: the title
        // kicked another chain (its ResetBuckets after a movie start/end
        // discards the frame's appended buckets); the groups were discarded,
        // attributed (ordering, the Link producer, detail kDiscarded).
        uint64_t discarded_walks = 0;
        uint64_t unsubmitted = 0;
        uint64_t pending_submissions = 0;
        uint64_t model_producers = 0;
        std::array<VariantCounters, 4> variants{};
        GraphicsBridge::Counters output;
    };

    explicit GraphicsService(Hooks hooks);
    ~GraphicsService();
    GraphicsService(const GraphicsService&) = delete;
    GraphicsService& operator=(const GraphicsService&) = delete;

    // Captures all four Strip bodies and Link before replacing any dispatch.
    // Failed installation restores every earlier successful binding in reverse
    // order. An installer returning false must leave that binding unchanged.
    // Missing dependencies decline installation. Model/Plain interior entries
    // retain their original continuation. All cannot inherit a model stack.
    bool Install(const Capture& capture, const Installer& installer);
    void Strip(CpuContext& context, uint32_t entry);
    void Link(CpuContext& context, uint32_t entry);
    // Producer adapter entry: all selection and input ownership checks precede
    // this call. Uses the same source bucket predecessor, Link and walk closure
    // as Strip; no new chain observer or source-frame lifecycle is installed.
    bool RecordOwned(uint32_t bucket, std::function<bool()> work,
                     const std::vector<Graphics::Lease>& leases = {},
                     std::function<void()> cancel = {});

    // Generic post-tag observer contract: first_tag is true exactly once per
    // WalkVifChain invocation, including a tail replay. Its address identifies
    // the root. Root selection uses the oldest unstarted sealed submission;
    // later tags use ONLY that submission. Never dispatch by tag address alone.
    void OnTag(uint32_t physical_address, bool from_scratchpad, bool first_tag);
    // Optional exact walk-end notification; otherwise the next first_tag
    // checks completion of the previous walk. No waiting occurs here.
    void EndWalk();

    // Host intents are not in the existing device snapshot format. Save only
    // when this guard succeeds AFTER the normal engine/renderer quiescence.
    // Restoring/resetting cancels old host intents only after that quiescence.
    bool CanCheckpoint() const;
    // The capture-time settle (the title's frame boundary, TR renderer.cpp:
    // 436-440: PresentFrame = Present -> SyncV -> StartNextFrame -> Link). At
    // the SyncV boundary every SEALED submission must have delivered (the
    // previous Link's walk, the worker's kicks: nothing in flight), while the
    // current frame's recording is OPEN: its groups are recorded, not Linked.
    // True admits the boundary with that open recording and reports it
    // (`unlinked`: the groups/items the image cannot carry; the restored
    // Link draws that one frame without them). False: faulted, or a sealed
    // group still in flight -- not settled.
    bool CanCheckpointUnlinked(GraphicsBridge::Recording* unlinked) const;
    // The restore-side attribution of an image's unlinked recording: counted
    // at the Link producer with detail kRestoredUnlinked, once per item.
    static constexpr uint32_t kRestoredUnlinked = 5;
    void ResetAfterQuiescence();
    uint32_t NextFrame() const;
    // Explicit checkpoint publication only; source and consumer are stopped.
    // Cannot discard accepted work, repair a fault or reset execution counts.
    bool RestoreFrame(uint32_t next);
    Counters Counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Returns 0 when the strip was taken, else the refusal detail code.
    uint32_t TryStrip(uint32_t producer, const CpuContext& context);
    void StripVariant(size_t variant, CpuContext& context, uint32_t entry);
    [[noreturn]] void Fault(const char* message, uint32_t producer = kLinkAddress, uint32_t detail = 0);
};

}  // namespace title
