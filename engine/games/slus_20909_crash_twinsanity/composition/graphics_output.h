#pragma once

#include "games/slus_20909_crash_twinsanity/composition/graphics_strip.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/config/config.h"
#include "engine/gs/hle/ir/draw_ir_sink.h"
#include "engine/gs/hle/texsrc/owned_texture_slots.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"

class ChainWalker;
class GifArbiter;
class GifParser;
class MainMemory;
class StateRestorer;
namespace ps2r::gs { class GsState; class DrawIrThreadQueue; class DrawIrQueueConsumer; }
namespace ps2r::verify { class RunMeta; }
namespace title { class GraphicsService; class Graphics; }

namespace ps2r::app {
class VideoStack;
class GraphicsCheckpoint;

// Joins title draws to the retained producers' ordered output and resource
// lifetime. Construct before worker owners so their shutdown precedes this
// object's destruction. No packet construction or register-state capture.
class GraphicsOutput final : private gs::DrawIrSink {
public:
    GraphicsOutput(MainMemory& memory, GifParser& parser, GifArbiter& arbiter,
                   gs::GsState& state, ChainWalker& walker,
                   gs::DrawIrThreadQueue* consumer, GraphicsStrips strips);
    ~GraphicsOutput();
    bool Install();
    // The boundary capture predicate: SettleOwnedForCapture, then the source
    // frame/order consistency (registered source state only).
    bool CanCheckpoint(std::string* reason);
    // The capture-time SETTLE (Graphics INTERFACES section 13). The title's
    // frame boundary is Present -> SyncV -> StartNextFrame -> Link (TR
    // renderer.cpp:436-440): at the SyncV boundary the PREVIOUS Link's groups
    // have delivered and the CURRENT frame's owned groups are recorded behind
    // their predecessors but not yet Linked (host closures; no image can carry
    // them). Settled = the consumer drained, no owned immediate slot or sealed
    // group in flight, no owned fault, and every pending owned batch is an
    // item of the bridge's OPEN recording. Then the boundary is admitted and
    // the open recording is published as the unlinked digest (GXPD): the
    // restored Link draws that one frame without those groups, attributed.
    bool SettleOwnedForCapture(std::string* reason);
    struct Unlinked {
        uint32_t source_frame = 0;  // the Link ordinal the groups belong to
        uint32_t groups = 0, items = 0, owned_items = 0;
        std::array<uint32_t, ::title::Graphics::kBucketCount> bucket_items{};
    };
    // The latest settle's digest (zero when the recording was closed).
    const Unlinked& UnlinkedAtBoundary() const { return unlinked_; }
    // The restored image's digest (zero without a GXPD section).
    const Unlinked& RestoredUnlinked() const { return restored_unlinked_; }
    static constexpr uint32_t kUnlinkedTag = 0x44505847; // GXPD
    static constexpr uint32_t kUnlinkedVersion = 1;
    static bool EncodeUnlinked(const Unlinked&, std::vector<uint8_t>&, std::string* error = nullptr);
    static bool DecodeUnlinked(const std::vector<uint8_t>&, Unlinked&, std::string* error = nullptr);
    void ResetAfterQuiescence();
    // The LIVE restore's form: the settle admitted the boundary with
    // the bridge's recording OPEN (its owned batches Pending). Cancel those
    // batches, reset the service and the environment, keep the output usable --
    // the state a fresh --load-state process has before its image applies, on
    // the running session. Returns the batches cancelled. Throws on owned failure.
    uint64_t ResetForRestore();
    // Before RegisterSourceState: the legacy-import predicate (true while the
    // root's GraphicsImport holds a prepared frozen image). With it the source
    // frame/order, shared-surface and presentation sections are optional for
    // THAT image only; their absence publishes the observational owners' boot
    // state (nothing seen yet), as the retained arm restores the same anchors.
    void SetLegacyImport(std::function<bool()> prepared);
    // Required with selected owned producers. Source boundary/order state is
    // restored explicitly; it never certifies missing scene-surface content.
    bool RegisterSourceState(StateRestorer&);
    bool CanCheckpointScene(std::string* reason);
    // Source execution and MTVU must have stopped. Drain already submitted
    // host work, cancel unsubmitted source groups, then release their captures.
    // Cancellation earns no completion credit and is never a checkpoint reset.
    void StopAfterQuiescence();
    void Record(verify::RunMeta& meta) const;
    // The clock contract (rule 2), play mode only: owned deliveries run on the GS
    // consumer in stream order WITHOUT the game thread waiting for them -- the
    // slot is credited Completed at once; the consumer's actual outcome is
    // counted when it runs (graphics_owned_async=), a Failed outcome faults the
    // next delivery (section 8a, one slot later), a Refused/Retained outcome is
    // counted as lost (the retained draw cannot be re-submitted after the fact).
    // Off (the default, the deterministic profile): the waiting RunOrdered.
    void SetAsyncOwnedDelivery(bool on) { async_owned_ = on; }
    bool AsyncOwnedDelivery() const { return async_owned_; }
    // Source Link ordinal (the FrameState `next_frame`): the source-frame clock
    // of every refusal attribution. Host leg frame clock is optional.
    uint64_t SourceFrame() const;
    void SetFrameClock(::title::GraphicsRefusals::Clock host_frame);
    const ::title::GraphicsRefusals& Refusals() const { return refusals_; }
    // Call once after VideoStack::Create, before selecting any owned producer.
    // Root keeps VideoStack alive through producer shutdown and output drain.
    bool AttachVideo(VideoStack& video, gs::DrawIrQueueConsumer& retained);
    // Producer-side owned snapshot, before RecordOwned/EnqueueOwned. No later
    // source/global/GS read is needed to bind the captured scene.
    bool CaptureScene(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& out) const;
    bool CaptureSubmittedScene(::title::FrameEnvironment::SceneFrame& out) const;
    // FrameEnvironment::Linked: whether any primary Link has completed since reset.
    bool SceneLinked() const;
    bool CaptureSetup(::title::FrameEnvironment::SceneFrame& out) const;
    // Ordinary source overlay policy, only buckets 24/26. Capture the target
    // and environment together before any producer accepts owned work.
    bool CaptureUi(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& frame,
                   ::title::MaterialState::Environment& environment) const;
    bool CaptureSky(::title::FrameEnvironment::SceneFrame& frame,
                    ::title::MaterialState::Environment& environment) const;
    // The effect producers (buckets 4/21/23): the frame's completed scene tuple
    // plus the frame-wide environment at the producer's insertion
    // (FrameEnvironment::CaptureEffects; fog is the effect material's own).
    bool CaptureEffects(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& frame,
                        ::title::MaterialState::Environment& environment) const;
    // The owned mesh bucket (opaque 2/3, blended 6-19) -- the
    // frame's completed scene tuple plus the frame-wide environment
    // (FrameEnvironment::CaptureMesh).
    bool CaptureMesh(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& frame,
                     ::title::MaterialState::Environment& environment) const;
    // Called at the EXISTING source-order output slot (e.g. accepted bucket
    // predecessor), never merely at a gameplay producer's call time. Captures
    // own their inputs and must not read guest pointers when delivered.
    // The callback runs after prior assembly/uploads/consumer work drains.
    // Use VideoStack::Scene()->Bind before recording a group into named targets.
    //
    // Group lifecycle (the ordered-group contract every owned family shares):
    //   captured  -> the producer's call recorded the group at its slot;
    //   completed -> the owned scene drew it;
    //   retained  -> the scene refused BEFORE drawing and the same captured
    //                draw took the retained route at that slot (Retainable);
    //   refused   -> the scene refused before drawing and the slot is RELEASED
    //                (the callback returned false): nothing was published, the
    //                caller attributes it and its model/plain route stands
    //                (EnqueueOwned returns false at once; a bridge group
    //                completes with the item counted refused_work);
    //   failed    -> the callback THREW after delivery started: that run's
    //                owned output is faulted (graphics_output_faulted=1), a
    //                bridge group faults its walk, later owned work refuses;
    //   cancelled -> the title never kicked the group's predecessor (its
    //                renderer reset its buckets: GraphicsBridge::Discard); the
    //                capture is released unexecuted, no credit, no fault.
    // A refusal never latches: the groups behind it (the Movie presentation
    // release, the next Link walk) are delivered as if it had completed.
    using OwnedWork = std::function<bool(VideoStack&)>;
    bool EnqueueOwned(OwnedWork work);
    // Preferred source producer seam: validates/captures the bucket predecessor
    // now, delivers the owned callback through the existing Link/tag observer.
    // False leaves the source producer unselected; success must bypass it whole.
    bool RecordOwned(uint32_t bucket, OwnedWork work);
    // RecordOwned for work that may REFUSE at the ordered slot before committing
    // (a retained-pipeline state readable only there). On Retained the delivering
    // owner submits `Retained::draw` — the same captured draw — through the
    // retained route at that slot (the Plain control's path, same order) and
    // runs `Retained::attribute` on its own thread; the group completes, never
    // faults. Failed stays a fault. Counted graphics_owned_output.retained.
    using OwnedOutcome = GraphicsStrip::Outcome;
    using OwnedRetained = GraphicsStrip::Retained;
    using RetainableWork = std::function<OwnedOutcome(VideoStack&, OwnedRetained&)>;
    bool RecordOwnedRetainable(uint32_t bucket, RetainableWork work);
    // Convenience for an already bound, exclusively handed-off typed group.
    // After acceptance the producer must not mutate work until it completes.
    bool SubmitOwned(std::shared_ptr<::title::Graphics> work);

    // Owned texture uploads (textures at load; the title's WriteTextureUpload
    // adapter TextureUploads). The listener runs in the VIF1 walk observer for
    // every walked tag BEFORE the bridge's own observer (address = physical RAM
    // / scratchpad offset; `first_tag` = the walk's first): the adapter takes
    // its empty tag there and calls DeliverTextureBind, which submits the bind
    // at the CURRENT GIF stream position (GifArbiter::submitOrdered -- after
    // every preceding VIF packet of the chain, in the slot the texture's IMAGE
    // transfer held). At that slot, on the GS stream owner: the transfer's
    // marks and InvalidateTextures job without bytes (TransferEngine::
    // NoteOwnedUpload), then the bind travels to the GS consumer in stream order
    // (RunOrderedAsync / inline) into TextureResolver::Owned(). A release (the
    // owner destroyed the asset) travels the same way; a reset (restore) is
    // synchronous at a quiescent boundary.
    using UploadTagListener = std::function<void(uint32_t address, bool scratchpad, bool first_tag)>;
    void SetUploadTagListener(UploadTagListener listener);
    void DeliverTextureBind(uint32_t base, gs::OwnedTextureSlots::Texture&& texture);
    void DeliverTextureRelease(uint64_t key);
    void ResetOwnedTextures();
    struct OwnedTextureCounts { uint64_t binds = 0, releases = 0, delivered = 0, resets = 0, no_consumer = 0; };
    OwnedTextureCounts OwnedTextures() const;

    // Census attribution of the walked chain to the title's render buckets
    //At every Link the service hands over the 28
    // buckets' `first` tags; the VIF1 walk observer then reports, through
    // `hint`, the bucket whose `first` the walk passed last (-1 = unknown: a
    // walk no Link snapshot matches). The hint is an ordered arbiter op (the
    // staged feed flushed ahead of it), applied on the drain owner at the
    // stream position the bucket's first tag held: exact under the inline and
    // the MTVU staged feed alike. Observation only; nothing is delivered or
    // reordered. `BucketHintExact()` stays true unless a run ever reported
    // an unordered hint.
    void SetBucketHint(std::function<void(int)> hint);
    bool BucketHintExact() const { return bucket_hint_exact_; }

private:
    void OnDraw(const gs::DrawIr& draw) override;
    void OnJob(const gs::JobRec& job) override;
    void OnFrame(const gs::JobRec& frame) override;

    gs::GsState& state_;
    ChainWalker& walker_;
    gs::DrawIrThreadQueue* consumer_;
    ::title::FrameEnvironment environment_;
    ::title::GraphicsRefusals refusals_; // Strips family; declared before the service that sinks into it
    std::unique_ptr<::title::GraphicsService> service_;
    GifArbiter& arbiter_;
    GifParser& parser_;
    VideoStack* video_ = nullptr;
    gs::DrawIrQueueConsumer* retained_ = nullptr;
    struct OwnedBatch;
    std::shared_ptr<OwnedBatch> MakeOwned(OwnedWork work, RetainableWork retainable = {});
    // Registers the GXPD section (optional; absent = nothing was unlinked).
    bool RegisterUnlinkedState(StateRestorer&);
    // The restore side needs the restoring session's recording CLOSED (its
    // own open groups would be orphaned by the source-frame reset).
    bool RequireClosedRecording(std::string* error) const;
    // Delivers at the slot; returns the batch's terminal outcome (Completed /
    // Retained are complete; Refused released the slot; Failed faulted).
    OwnedOutcome DeliverOwned(const std::shared_ptr<OwnedBatch>& batch);
    // The bridge-group form of DeliverOwned: false = refused (slot released),
    // throw = failed (the group faults its walk).
    bool DeliverBridged(const std::shared_ptr<OwnedBatch>& batch);
    // GraphicsBridge::Discard's cancel hook: the predecessor was never kicked.
    void CancelOwned(const std::shared_ptr<OwnedBatch>& batch);
    std::vector<std::shared_ptr<OwnedBatch>> owned_;
    std::atomic<uint64_t> owned_queued_{0}, owned_completed_{0}, owned_failed_{0}, owned_pending_{0};
    std::atomic<uint64_t> owned_retained_{0}, owned_refused_{0}, owned_cancelled_{0};
    bool async_owned_ = false;
    std::atomic<uint64_t> async_delivered_{0}, async_ran_{0}, async_not_completed_{0};
    std::atomic<bool> async_failed_{false};
    // The bridge's per-group fence (delivery.drain): groups fenced by their stream
    // position (play, frame skip on) vs. groups the game thread waited for (Sync).
    std::atomic<uint64_t> drains_stream_ordered_{0}, drains_waited_{0};
    bool installed_ = false;
    bool stopped_ = false;
    bool source_state_registered_ = false;
    std::unique_ptr<GraphicsCheckpoint> checkpoint_;
    std::function<bool()> legacy_;
    ::title::GraphicsRefusals::Clock frame_clock_;
    GraphicsStrips strips_;
    std::unique_ptr<GraphicsStrip> strip_output_;
    Unlinked unlinked_, restored_unlinked_;
    uint64_t settles_ = 0, settled_unlinked_ = 0; // capture settles; those with an open recording
    uint64_t live_restores_ = 0, live_restore_cancelled_ = 0; // ResetForRestore calls; batches it cancelled
    UploadTagListener upload_tags_;
    std::function<void(int)> bucket_hint_;
    struct LinkedBuckets { std::array<uint32_t, ::title::Graphics::kBucketCount> first{}; };
    std::deque<LinkedBuckets> linked_;     // the last few Links' snapshots (producer owner)
    LinkedBuckets walking_buckets_{};      // the snapshot of the chain being walked
    bool walking_ = false;
    int bucket_ = -1;
    bool bucket_hint_exact_ = true;
    void TrackBucket(uint32_t address, bool scratchpad, bool first_tag);
    // Runs `work` on the GS consumer in stream order (the pipelined queue's
    // ordered work; inline without a consumer thread) under the consumer's
    // boundary (DrawIrQueueConsumer::RunOrdered). False = no consumer attached.
    bool RunOnConsumer(std::function<void(gs::DrawIrQueueConsumer&)> work, bool wait);
    std::atomic<uint64_t> texture_binds_{0}, texture_releases_{0}, texture_delivered_{0}, texture_resets_{0},
        texture_no_consumer_{0};
};

}  // namespace ps2r::app
