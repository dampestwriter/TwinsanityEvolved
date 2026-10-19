#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_buckets.h"

namespace title {

// Joins owned title draws to EXISTING writer predecessors. No packet, tag,
// material-cache write, GsState mutation or hardware command is produced.
// Recording/sealing/tag observation have one producer owner; delivery runs on
// the ordered output owner. Counts may be read from either thread.
class GraphicsBridge {
    struct Batch;
    struct Impl;
public:
    static constexpr uint32_t kFrameBucketsAddress = 0x00323658;
    enum class Result {
        Ok, NoMatch, AlreadyQueued, InvalidState, InvalidInput, Unavailable,
        UnsupportedAnchor, AnchorChanged, MissingAdapter, DeliveryFailed,
        Unvisited, Pending, Cancelled, Discarded
    };
    struct Submission {
    private:
        friend class GraphicsBridge;
        std::shared_ptr<Batch> batch_;
    };
    struct Hooks {
        // Accept exactly one packet-free callback at the current stream
        // position, AFTER all preceding staged VIF work. True transfers
        // ownership, false guarantees no execution/retention. This is NOT
        // GifArbiter::submitDirectOwned: that API requires raw fallback bytes.
        std::function<bool(std::function<void()>)> enqueue;
        // On that slot's drain owner: finish preceding legacy assembly into
        // the SAME sink, including pending upload payload publication.
        std::function<bool()> flush_legacy;
        ps2r::gs::DrawIrSink* sink = nullptr;
        // Fence consumer use of draws AND their target/resource leases.
        std::function<bool()> drain;
    };
    struct Counters {
        uint64_t recorded = 0;
        uint64_t queued_groups = 0;
        uint64_t completed = 0;
        uint64_t failed_groups = 0;
        // Groups whose predecessor the title never kicked (its chain was reset
        // between the record and the walk -- TR movie.cpp:156/211,
        // context.cpp:244 ResetBuckets after WaitIdle): released at EndWalk by
        // Discard, their owned work cancelled, never delivered, never a fault.
        uint64_t discarded_groups = 0;
        uint64_t recorded_work = 0, completed_work = 0, refused_work = 0;
        std::array<uint64_t, Graphics::kBucketCount> replaced_bucket_jobs{};
    };

    explicit GraphicsBridge(Hooks hooks);
    ~GraphicsBridge();
    GraphicsBridge(const GraphicsBridge&) = delete;
    GraphicsBridge& operator=(const GraphicsBridge&) = delete;

    Result BeginFrame(uint32_t frame);
    // All geometry/material/target eligibility must be decided before this
    // call. Ok owns a copy: bypass the WHOLE Strip producer on Ok only.
    // Other results leave guest memory and the original fallback untouched.
    Result AppendStrip(uint32_t bucket, const GraphicsBuckets::Reader& reader,
                       const ps2r::gs::DrawIr& draw,
                       const std::vector<Graphics::Lease>& leases = {});
    // Complete owned resource/draw group at the SAME source predecessor.
    // Callback inputs must be immutable/owned; no deferred guest packet reads.
    // The callback's FALSE is a refusal AT THE SLOT (the owned scene declined
    // before drawing: nothing was published, the slot is released and the
    // caller attributes it); the group still completes and the walk never
    // faults on it (Counters::refused_work). A THROW after delivery started
    // faults the group (DeliveryFailed); neither is ever a model replay.
    // `cancel` (optional) runs on the producer owner if the group is DISCARDED
    // (its predecessor never walked): the work will not be delivered.
    Result AppendWork(uint32_t bucket, const GraphicsBuckets::Reader& reader,
                      std::function<bool()> work,
                      const std::vector<Graphics::Lease>& leases = {},
                      std::function<void()> cancel = {});
    // At LinkRenderBuckets(a0, a1), BEFORE its chain kick and StartFrameBuckets.
    // Validate existing anchors; only their link address may have changed.
    // Seal failure after an accepted producer is a hard integration error,
    // never permission to discard its draw and continue the legacy frame.
    Result Seal(uint32_t buckets, const GraphicsBuckets::Reader& reader,
                Submission* output);
    // Read-only selection for the generic chain boundary seam. Only selected
    // anchors require flushing the current staged VIF segment/opening a slot.
    bool WantsTag(const Submission& submission, uint32_t physical_address,
                  bool from_scratchpad = false) const;
    // Supply canonical physical RAM tag address AFTER its TTE/payload is fed
    // and BEFORE following legacy work. With staged VIF, flush its segment
    // before this call so its marker precedes the new slot; no worker wait is
    // required. Scratchpad is a separate identity.
    // A sealed submission is retained through any deferred tail continuation.
    Result AfterTag(const Submission& submission, uint32_t physical_address,
                    bool from_scratchpad = false);
    // EndWalk catches accepted draws whose predecessors were never visited;
    // Poll distinguishes queued output from completion. Neither waits.
    Result EndWalk(const Submission& submission) const;
    Result Poll(const Submission& submission) const;
    // The title's own resolution of Unvisited: the kicked chain did not carry
    // those predecessors (the renderer's DMA "starts over" at a movie start/
    // end), so the title never drew them. Every still-Recorded group of the
    // submission is Discarded (its items' cancel hooks run, nothing is
    // delivered); EndWalk/Poll then read Ok. Returns the number discarded.
    size_t Discard(const Submission& submission);

    // The OPEN recording (BeginFrame .. Seal): the frame's groups the title
    // has recorded but not yet Linked. At the title's Present (TR
    // renderer.cpp:436-440: Present -> SyncV, then StartNextFrame -> Link) this
    // is exactly the set a boundary capture cannot carry; the digest names it.
    struct Recording {
        uint32_t groups = 0;   // distinct predecessors
        uint32_t items = 0;    // draws + owned work recorded behind them
        std::array<uint32_t, Graphics::kBucketCount> bucket_items{};
    };
    Recording OpenRecording() const;

    // Full runtime reset only, after producers and ordered/consumer queues are
    // quiescent. Frame-buffer rotation uses Seal/BeginFrame, NEVER Reset.
    // Invalidates old submissions; retained callbacks cannot draw after reset.
    void Reset();
    Counters Counts() const;

private:
    Result Append(uint32_t bucket, const GraphicsBuckets::Reader& reader,
                  const ps2r::gs::DrawIr* draw, std::function<bool()> work,
                  const std::vector<Graphics::Lease>& leases, std::function<void()> cancel);
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
