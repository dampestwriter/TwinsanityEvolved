#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.h"

#include <cstring>
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"
#include <stdexcept>
#include <utility>

#include "engine/core/verify/run_meta.h"
#include "engine/dmac/chain_walker.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gs/hle/queue/draw_ir_thread_queue.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_report.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

namespace ps2r::app {

struct GraphicsOutput::OwnedBatch {
    enum class State { Pending, Complete, Refused, Failed, Cancelled };
    OwnedWork work;
    RetainableWork retainable; // exactly one of work/retainable is set
    std::atomic<State> state{State::Pending};
    bool immediate = false;    // EnqueueOwned: an ordered slot, never a recorded group
    uint32_t bucket = 0;       // RecordOwned/RecordOwnedRetainable: the recorded bucket
};

GraphicsOutput::GraphicsOutput(MainMemory& memory, GifParser& parser, GifArbiter& arbiter,
                               gs::GsState& state, ChainWalker& walker,
                               gs::DrawIrThreadQueue* consumer, GraphicsStrips strips)
    : state_(state), walker_(walker), consumer_(consumer),
      environment_([&memory](uint32_t address, void* out, size_t size) {
          const auto* bytes = memory.Pointer(address, size);
          if (!bytes || !out) return false;
          std::memcpy(out, bytes, size);
          return true;
      }), arbiter_(arbiter), parser_(parser), strips_(strips) {
    ::title::GraphicsService::Hooks hooks;
    using Selection = ::title::GraphicsService::StripSelection;
    hooks.strip_selection = strips == GraphicsStrips::All ? Selection::All :
                            strips == GraphicsStrips::Plain ? Selection::Plain : Selection::Model;
    hooks.read = [&memory](uint32_t address, uint8_t* out, size_t size) {
        const auto* bytes = memory.Pointer(address, size);
        if (!bytes || !out) return false;
        std::memcpy(out, bytes, size);
        return true;
    };
    hooks.delivery.enqueue = [&arbiter](std::function<void()> work) {
        arbiter.submitOrdered(std::move(work));
        return true;
    };
    hooks.delivery.flush_legacy = [&parser, &state] {
        if (!parser.CanAcceptDirect()) return false;
        // Keep the preceding legacy strip's assembly history: publishing a
        // complete title draw is not a GS state change.
        state.Flush().Flush(gs::DrawFlush::FlushKind::GifEop);
        return true;
    };
    hooks.delivery.sink = this;
    // The bridge's fence after a delivered group. Every input the consumer uses
    // is an owned copy at the push (the DrawIr with its vertices is copied into
    // the ring; an owned batch travels as a shared_ptr whose capture the consumer
    // clears when the work ran; textures resolve from the pipeline's own VRAM
    // image, which the stream's VramOps records keep current in record order),
    // so the fence the group needs IS its stream position (PERF_S4's barrier
    // model). The clock contract (rule 2): in play (frame skip on) the game
    // thread therefore does not wait here -- the one Sync() per group that
    // serialized the EE on the renderer (10,697 groups = 10,694 syncs = 17.9 s
    // of a 112 s s3 replay). The deterministic profile keeps the drain (the
    // lockstep/pipelined identity is pinned on it).
    hooks.delivery.drain = [this] {
        if (!consumer_) return true;
        if (consumer_->FrameSkip()) { ++drains_stream_ordered_; return true; }
        ++drains_waited_;
        consumer_->Sync();
        return true;
    };
    hooks.environment = [this](::title::MaterialState::Environment& out) { return environment_.Read(out); };
    hooks.flush_staged = [&walker] { walker.FlushVif1StagedFeed(); };
    strip_output_ = std::make_unique<GraphicsStrip>(
        [this](auto& frame) { return CaptureScene(24,frame); },
        [this](uint32_t bucket,GraphicsStrip::Work work) {
            return RecordOwnedRetainable(bucket,[work=std::move(work)](VideoStack& video,OwnedRetained& retained) {
                if (!video.Scene()) return OwnedOutcome::Failed;
                return work(*video.Scene(),retained);
            });
        },
        [this](uint32_t producer,uint32_t detail) {
            refusals_.Refuse(::title::GraphicsRefusals::Family::Strips,
                             ::title::GraphicsRefusals::Reason::UnsupportedState,producer,detail);
        });
    hooks.record_strip = [this](auto intent) { return strip_output_->Submit(std::move(intent)); };
    hooks.refuse = refusals_.Refuser(::title::GraphicsRefusals::Family::Strips);
    hooks.fault = refusals_.Faulter(::title::GraphicsRefusals::Family::Strips);
    hooks.linked_buckets = [this](const std::array<uint32_t, ::title::Graphics::kBucketCount>& firsts) {
        if (!bucket_hint_) return;
        if (linked_.size() >= 8) linked_.pop_front();
        linked_.push_back(LinkedBuckets{firsts});
    };
    service_ = std::make_unique<::title::GraphicsService>(std::move(hooks));
    refusals_.SetClocks({}, [this] { return SourceFrame(); });
    walker_.SetVif1TagObserver(
        [this](uint32_t address, bool scratchpad, bool first_tag) {
            if (upload_tags_) upload_tags_(address, scratchpad, first_tag);
            if (bucket_hint_) TrackBucket(address, scratchpad, first_tag);
            service_->OnTag(address, scratchpad, first_tag);
        }, [this] { service_->EndWalk(); });
}

void GraphicsOutput::SetBucketHint(std::function<void(int)> hint) { bucket_hint_ = std::move(hint); }

void GraphicsOutput::TrackBucket(uint32_t address, bool scratchpad, bool first_tag) {
    int bucket = bucket_;
    if (first_tag) {
        walking_ = false;
        bucket = -1;
        if (!scratchpad) {
            // The walk's root is bucket 0's first tag of one Link's snapshot
            // (copied: a later Link may pop the deque's front mid-walk).
            for (auto it = linked_.begin(); it != linked_.end(); ++it) {
                if (it->first[0] == address) {
                    walking_buckets_ = *it;
                    walking_ = true;
                    // Snapshots before this one belong to chains already walked.
                    linked_.erase(linked_.begin(), it + 1);
                    break;
                }
            }
        }
    }
    if (walking_ && !scratchpad) {
        for (size_t b = 0; b < walking_buckets_.first.size(); ++b)
            if (walking_buckets_.first[b] == address) bucket = int(b);
    }
    if (bucket != bucket_) {
        bucket_ = bucket;
        // The hint travels IN STREAM ORDER, like a texture bind: the programs
        // the fed tags kicked run after the walk (the inline VU1 runs at the
        // MSCAL but its PATH1 packets queue in the arbiter; the MTVU worker
        // runs them later still), so a hint applied at the walk would attribute
        // every kick of the frame to the last bucket linked. Flushing the staged
        // feed first seals the records fed so far into a marker BEFORE this
        // slot; the drain owner applies the hint when it reaches the slot.
        walker_.FlushVif1StagedFeed();
        arbiter_.submitOrdered([this, bucket] { bucket_hint_(bucket); });
    }
}

void GraphicsOutput::SetUploadTagListener(UploadTagListener listener) { upload_tags_ = std::move(listener); }

bool GraphicsOutput::RunOnConsumer(std::function<void(gs::DrawIrQueueConsumer&)> work, bool wait) {
    if (!retained_) { ++texture_no_consumer_; return false; }
    auto run = [this, work = std::move(work)] {
        return retained_->RunOrdered([&] { work(*retained_); return true; });
    };
    if (!consumer_) { run(); return true; }
    if (wait) consumer_->RunOrdered(run); else consumer_->RunOrderedAsync(run);
    return true;
}

void GraphicsOutput::DeliverTextureBind(uint32_t base, gs::OwnedTextureSlots::Texture&& texture) {
    ++texture_binds_;
    auto t = std::make_shared<gs::OwnedTextureSlots::Texture>(std::move(texture));
    // The records staged before this upload are sealed into a marker ahead of
    // the bind's slot: a draw fed before the bind samples the slot's previous
    // image, not this one (the MTVU staged feed flushes per 32 records/32 KiB).
    walker_.FlushVif1StagedFeed();
    arbiter_.submitOrdered([this, base, t] {
        // The transfer's observation boundary in this stream position: the pending
        // draws flushed, the destination pages stamped, the InvalidateTextures job
        // (TextureResolver::OnCpuWrite retires the decoded textures and the older
        // bind under it) -- then the bind itself, right behind that job.
        state_.Xfer().NoteOwnedUpload(base, t->footprint);
        RunOnConsumer([base, t](gs::DrawIrQueueConsumer& consumer) {
            consumer.Pipeline().Resolver().Owned().Bind(base, gs::OwnedTextureSlots::Texture(*t));
        }, false);
        ++texture_delivered_;
    });
}

void GraphicsOutput::DeliverTextureRelease(uint64_t key) {
    ++texture_releases_;
    walker_.FlushVif1StagedFeed();
    arbiter_.submitOrdered([this, key] {
        RunOnConsumer([key](gs::DrawIrQueueConsumer& consumer) {
            consumer.Pipeline().Resolver().Owned().Release(key);
        }, false);
    });
}

void GraphicsOutput::ResetOwnedTextures() {
    ++texture_resets_;
    RunOnConsumer([](gs::DrawIrQueueConsumer& consumer) { consumer.Pipeline().Resolver().Owned().Reset(); }, true);
}

GraphicsOutput::OwnedTextureCounts GraphicsOutput::OwnedTextures() const {
    return {texture_binds_.load(), texture_releases_.load(), texture_delivered_.load(), texture_resets_.load(),
            texture_no_consumer_.load()};
}

GraphicsOutput::~GraphicsOutput() { walker_.SetVif1TagObserver({}); }

uint64_t GraphicsOutput::SourceFrame() const { return service_->NextFrame(); }
void GraphicsOutput::SetFrameClock(::title::GraphicsRefusals::Clock host_frame) {
    frame_clock_ = host_frame;
    refusals_.SetClocks(std::move(host_frame), [this] { return SourceFrame(); });
}

bool GraphicsOutput::AttachVideo(VideoStack& video, gs::DrawIrQueueConsumer& retained) {
    if (video_ || !video.Active() || !video.Resources() || !video.Scene()) return false;
    video_ = &video;
    retained_ = &retained;
    return true;
}

bool GraphicsOutput::CaptureScene(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& out) const {
    return environment_.CaptureScene(bucket, out);
}

bool GraphicsOutput::CaptureSubmittedScene(::title::FrameEnvironment::SceneFrame& out) const {
    return environment_.CaptureSubmittedScene(out);
}

bool GraphicsOutput::SceneLinked() const { return environment_.Linked(); }

bool GraphicsOutput::CaptureSetup(::title::FrameEnvironment::SceneFrame& out) const {
    return environment_.CaptureSetup(out);
}

bool GraphicsOutput::CaptureUi(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& frame,
                               ::title::MaterialState::Environment& environment) const {
    ::title::FrameEnvironment::SceneFrame target;
    ::title::MaterialState::Environment state;
    if ((bucket != 24 && bucket != 26) || !environment_.CaptureScene(bucket, target) ||
        !environment_.Read(state)) return false;
    frame = target;
    environment = state;
    return true;
}

bool GraphicsOutput::CaptureSky(::title::FrameEnvironment::SceneFrame& frame,
                                ::title::MaterialState::Environment& environment) const {
    return environment_.CaptureSky(frame, environment);
}

bool GraphicsOutput::CaptureEffects(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& frame,
                                    ::title::MaterialState::Environment& environment) const {
    return environment_.CaptureEffects(bucket, frame, environment);
}

bool GraphicsOutput::CaptureMesh(uint32_t bucket, ::title::FrameEnvironment::SceneFrame& frame,
                                 ::title::MaterialState::Environment& environment) const {
    return environment_.CaptureMesh(bucket, frame, environment);
}

std::shared_ptr<GraphicsOutput::OwnedBatch> GraphicsOutput::MakeOwned(OwnedWork work, RetainableWork retainable) {
    // Only a FAILED delivery (a throw after it started) closes the owned
    // output for the run; a refused or retained slot never does.
    if (stopped_ || !video_ || (!work && !retainable) || owned_failed_.load()) return {};
    for (auto it = owned_.begin(); it != owned_.end();) {
        const auto state = (*it)->state.load();
        if (state == OwnedBatch::State::Complete || state == OwnedBatch::State::Refused ||
            state == OwnedBatch::State::Cancelled) it = owned_.erase(it);
        else ++it;
    }
    auto batch = std::make_shared<OwnedBatch>();
    batch->work = std::move(work);
    batch->retainable = std::move(retainable);
    return batch;
}

GraphicsOutput::OwnedOutcome GraphicsOutput::DeliverOwned(const std::shared_ptr<OwnedBatch>& batch) {
    switch (batch->state.load()) {
        case OwnedBatch::State::Pending: break;
        case OwnedBatch::State::Complete: return OwnedOutcome::Completed;
        case OwnedBatch::State::Refused: return OwnedOutcome::Refused;
        default: return OwnedOutcome::Failed;
    }
    // Written by the ordered backend callback, read here after RunOrdered
    // returned (the queue's future hands it over).
    auto outcome = OwnedOutcome::Failed;
    OwnedRetained retained;
    // Section 8a, deferred by one slot: an asynchronous delivery that failed on
    // the consumer faults the next delivery on the game thread.
    if (async_failed_.load(std::memory_order_acquire)) {
        ++owned_failed_;
        batch->state.store(OwnedBatch::State::Failed);
        --owned_pending_;
        return OwnedOutcome::Failed;
    }
    try {
        // A slot the GIF cannot take direct work at is not this group's doing:
        // the group is refused (released), never failed.
        if (!parser_.CanAcceptDirect()) {
            outcome = OwnedOutcome::Refused;
        } else if (async_owned_ && consumer_ && retained_ && consumer_->FrameSkip()) {
            // The clock contract (rule 2): no wait. The work runs on the consumer in
            // stream order; the slot is credited now, the outcome counted there.
            state_.Flush().Flush(gs::DrawFlush::FlushKind::GifEop);
            ++async_delivered_;
            consumer_->RunOrderedAsync([this, batch] {
                auto oc = OwnedOutcome::Failed;
                OwnedRetained unused;
                try {
                    // The same consumer boundary the synchronous path runs under
                    // (review loop 1, item 2): Enter() binds the context and
                    // drains the pending upload before the owned group.
                    retained_->RunOrdered([&] {
                        if (batch->retainable) oc = batch->retainable(*video_, unused);
                        else oc = batch->work(*video_) ? OwnedOutcome::Completed : OwnedOutcome::Refused;
                        if (oc == OwnedOutcome::Completed) video_->Scene()->CollectAfterQuiescence();
                        return oc != OwnedOutcome::Failed;
                    });
                } catch (...) { oc = OwnedOutcome::Failed; }
                batch->work = {};        // the capture's lease ends when the work ran
                batch->retainable = {};
                ++async_ran_;
                if (oc != OwnedOutcome::Completed) ++async_not_completed_;
                if (oc == OwnedOutcome::Failed) async_failed_.store(true, std::memory_order_release);
                return oc != OwnedOutcome::Failed;
            });
            outcome = OwnedOutcome::Completed;
        } else {
            state_.Flush().Flush(gs::DrawFlush::FlushKind::GifEop);
            const auto deliver = [&] {
                return retained_->RunOrdered([&] {
                    video_->EnsureCurrent();
                    if (batch->retainable) outcome = batch->retainable(*video_, retained);
                    else outcome = batch->work(*video_) ? OwnedOutcome::Completed : OwnedOutcome::Refused;
                    if (outcome == OwnedOutcome::Completed) video_->Scene()->CollectAfterQuiescence();
                    return outcome != OwnedOutcome::Failed;
                });
            };
            const bool ran = consumer_ ? consumer_->RunOrdered(deliver) : deliver();
            if (!ran) outcome = OwnedOutcome::Failed;
        }
    } catch (...) { outcome = OwnedOutcome::Failed; }
    if (outcome == OwnedOutcome::Retained) {
        // The owned scene refused this slot before drawing. This IS the Plain
        // control's route at this very slot (GraphicsBridge delivered the legacy
        // flush already): the same captured draw, same order, identical pixels.
        if (!retained.draw.verts.empty()) state_.SubmitDrawIr(retained.draw);
        ++owned_retained_;
        if (retained.attribute) retained.attribute();
    }
    switch (outcome) {
        case OwnedOutcome::Completed:
        case OwnedOutcome::Retained:
            // Backend borrowing ended in RunOrdered. Completed source captures
            // must not keep texture generations leased until the next game draw
            // (an asynchronous delivery clears its capture on the consumer, when
            // the work ran).
            if (!(async_owned_ && consumer_ && retained_ && consumer_->FrameSkip())) batch->work = {};
            ++owned_completed_;
            batch->state.store(OwnedBatch::State::Complete);
            break;
        case OwnedOutcome::Refused:
            // Nothing was published: the slot is the caller's model/plain
            // route's. The capture is released; the run's owned output is open.
            batch->work = {};
            batch->retainable = {};
            ++owned_refused_;
            batch->state.store(OwnedBatch::State::Refused);
            break;
        case OwnedOutcome::Failed:
            ++owned_failed_;
            batch->state.store(OwnedBatch::State::Failed);
            break;
    }
    --owned_pending_;
    return outcome;
}

bool GraphicsOutput::DeliverBridged(const std::shared_ptr<OwnedBatch>& batch) {
    switch (DeliverOwned(batch)) {
        case OwnedOutcome::Completed:
        case OwnedOutcome::Retained: return true;
        case OwnedOutcome::Refused: return false;
        default: break;
    }
    // Section 8a: a failure after delivery started is the walk's fault.
    throw std::runtime_error("Graphics owned output failed at its ordered slot");
}

bool GraphicsOutput::RecordOwnedRetainable(uint32_t bucket, RetainableWork work) {
    auto batch = MakeOwned({}, std::move(work));
    if (!batch) return false;
    batch->bucket = bucket;
    owned_.push_back(batch);
    if (!service_->RecordOwned(bucket, [this,batch] { return DeliverBridged(batch); }, {},
                               [this,batch] { CancelOwned(batch); })) {
        owned_.pop_back();
        return false;
    }
    ++owned_pending_;
    ++owned_queued_;
    return true;
}

void GraphicsOutput::CancelOwned(const std::shared_ptr<OwnedBatch>& batch) {
    // The title never kicked this group's predecessor (GraphicsBridge::Discard):
    // the capture is released unexecuted; no completion credit, no fault.
    auto expected = OwnedBatch::State::Pending;
    if (!batch->state.compare_exchange_strong(expected, OwnedBatch::State::Cancelled)) return;
    batch->work = {};
    batch->retainable = {};
    --owned_pending_;
    ++owned_cancelled_;
}

bool GraphicsOutput::RecordOwned(uint32_t bucket, OwnedWork work) {
    auto batch = MakeOwned(std::move(work));
    if (!batch) return false;
    batch->bucket = bucket;
    // Allocate tracking before the source group accepts the callback.
    owned_.push_back(batch);
    if (!service_->RecordOwned(bucket, [this,batch] { return DeliverBridged(batch); }, {},
                               [this,batch] { CancelOwned(batch); })) {
        owned_.pop_back();
        return false;
    }
    ++owned_pending_;
    ++owned_queued_;
    return true;
}

bool GraphicsOutput::EnqueueOwned(OwnedWork work) {
    auto batch = MakeOwned(std::move(work));
    if (!batch) return false;
    batch->immediate = true;
    owned_.push_back(batch); // failed borrowed work stays alive until quiescence
    ++owned_pending_;
    ++owned_queued_;
    try {
        walker_.FlushVif1StagedFeed();
        arbiter_.submitOrdered([this, batch] { DeliverOwned(batch); });
    } catch (...) {
        if (batch->state.exchange(OwnedBatch::State::Failed) == OwnedBatch::State::Pending) {
            ++owned_failed_;
            --owned_pending_;
        }
        return false;
    }
    // Inline ordered delivery resolves the state here: Complete accepts,
    // Refused hands the slot back to the caller's own route, Failed is the
    // run's fault (owned_failed_). A deferred slot (still Pending) is accepted.
    const auto state = batch->state.load();
    return state != OwnedBatch::State::Failed && state != OwnedBatch::State::Refused;
}

bool GraphicsOutput::SubmitOwned(std::shared_ptr<::title::Graphics> work) {
    if (!work || work->CurrentState() != ::title::Graphics::State::Recording) return false;
    return EnqueueOwned([work = std::move(work)](VideoStack& video) {
        ::title::Graphics::Hooks hooks;
        hooks.upload = [&video](const ::title::Graphics::Upload& upload) {
            return video.Resources()->Upload(upload) == ::title::GraphicsResources::Result::Ok;
        };
        hooks.draw = [&video](const gs::DrawIr& draw, const std::vector<::title::Graphics::Lease>& leases) {
            return video.Scene()->Draw(draw, leases) == ::title::GraphicsScene::Result::Ok;
        };
        // Backend methods finish borrowing the submitted vectors on return.
        // This callback runs on the same owner as retained backend work.
        hooks.drain = [] { return true; };
        const bool ok = work->Submit(hooks) == ::title::Graphics::Result::Ok;
        if (!ok && work->CurrentState() == ::title::Graphics::State::Faulted)
            work->Recover(hooks); // release only after drain; never replay
        return ok;
    });
}

bool GraphicsOutput::Install() {
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, ::title::GraphicsService::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    installed_ = environment_.Install(capture, install) && service_->Install(capture, install);
    return installed_;
}

bool GraphicsOutput::SettleOwnedForCapture(std::string* reason) {
    ++settles_;
    // Drain the already submitted host work first (the ordered consumer; the
    // MTVU worker's kicks are the `mtvu` predicate's, which runs before this).
    if (consumer_) consumer_->Sync();
    const auto refuse = [&](std::string why) {
        if (reason) *reason = std::move(why);
        return false;
    };
    if (owned_failed_.load())
        return refuse("Graphics owned failed=" + std::to_string(owned_failed_.load()));
    uint64_t immediate = 0, recorded = 0;
    for (const auto& batch : owned_) {
        if (batch->state.load() != OwnedBatch::State::Pending) continue;
        if (batch->immediate) ++immediate; else ++recorded;
    }
    if (immediate)
        return refuse("Graphics owned immediate slots in flight=" + std::to_string(immediate));
    ::title::GraphicsBridge::Recording open;
    if (!service_->CanCheckpointUnlinked(&open)) {
        // A sealed group of the previous Link still delivering (or a fault):
        // the walk has not settled; the boundary is not admissible yet.
        const auto counts = service_->Counts();
        return refuse("Graphics owned groups in flight: pending_submissions=" +
                      std::to_string(counts.pending_submissions) +
                      ",unsubmitted=" + std::to_string(counts.unsubmitted) +
                      ",open_items=" + std::to_string(open.items) +
                      ",ordering_errors=" + std::to_string(counts.ordering_errors) +
                      " (source_frame=" + std::to_string(service_->NextFrame()) +
                      ",host_frame=" + (frame_clock_ ? std::to_string(frame_clock_()) : std::string("none")) + ")");
    }
    if (recorded != owned_pending_.load() || recorded > open.items)
        return refuse("Graphics owned pending set is inconsistent: pending=" + std::to_string(owned_pending_.load()) +
                      ",recorded=" + std::to_string(recorded) + ",open_items=" + std::to_string(open.items));
    // Settled. The open recording (TR renderer.cpp:436-440: recorded before
    // Present, Linked by StartNextFrame after SyncV) is the unlinked digest.
    unlinked_ = Unlinked{};
    unlinked_.source_frame = service_->NextFrame();
    unlinked_.groups = open.groups;
    unlinked_.items = open.items;
    unlinked_.owned_items = uint32_t(recorded);
    unlinked_.bucket_items = open.bucket_items;
    if (open.items) ++settled_unlinked_;
    return true;
}

bool GraphicsOutput::CanCheckpoint(std::string* reason) {
    if (!SettleOwnedForCapture(reason)) return false;
    if (!source_state_registered_) return true;
    ::title::FrameEnvironment::Snapshot source;
    if (!environment_.CaptureState(source, reason)) return false;
    if (source.next_frame == service_->NextFrame()) return true;
    if (reason) *reason = "Graphics source frame and submission ordinals differ";
    return false;
}

bool GraphicsOutput::RequireClosedRecording(std::string* error) const {
    if (service_->CanCheckpoint()) return true;
    if (error) *error = "Graphics restore needs a closed recording on the restoring session: unlinked items=" +
                        std::to_string(service_->Counts().unsubmitted);
    return false;
}

void GraphicsOutput::ResetAfterQuiescence() {
    if (owned_pending_.load())
        throw std::logic_error("Graphics reset before owned output drained");
    // Recovery must drain a faulted Graphics queue before its final owner dies.
    // Failed selected output is fatal to that run, not an automatic replay.
    if (owned_failed_.load())
        throw std::logic_error("Graphics selected output failed; explicit recovery required");
    owned_.clear();
    service_->ResetAfterQuiescence();
    environment_.ResetAfterQuiescence();
}

uint64_t GraphicsOutput::ResetForRestore() {
    if (owned_failed_.load())
        throw std::logic_error("Graphics selected output failed; explicit recovery required");
    // The settle proved every Pending owned batch is an item of the bridge's
    // OPEN recording (SettleOwnedForCapture); a live restore drops that recording
    // exactly as a fresh --load-state process never had it: the batches are
    // cancelled (no credit, no fault; producer tickets observe the cancellation
    // through the emptied work), then the service and the environment reset.
    // The output stays usable: the restored title's first Link records into it.
    if (consumer_) consumer_->Sync();
    uint64_t cancelled = 0;
    for (auto& batch : owned_) {
        if (batch->state.load() == OwnedBatch::State::Pending) {
            batch->state.store(OwnedBatch::State::Cancelled);
            --owned_pending_;
            ++owned_cancelled_;
            ++cancelled;
        }
        batch->work = {};
    }
    owned_.clear();
    service_->ResetAfterQuiescence();
    environment_.ResetAfterQuiescence();
    live_restore_cancelled_ += cancelled;
    ++live_restores_;
    return cancelled;
}

void GraphicsOutput::SetLegacyImport(std::function<bool()> prepared) {
    if (!source_state_registered_) legacy_ = std::move(prepared);
}

bool GraphicsOutput::RegisterSourceState(StateRestorer& restorer) {
    if (!installed_ || source_state_registered_ || !video_ || !retained_) return false;
    using State = ::title::FrameState;
    using Snapshot = ::title::FrameEnvironment::Snapshot;
    savestate::SectionSpec spec;
    spec.tag = State::kTag;
    spec.version = State::kVersion;
    spec.owner = "Graphics source frame/order";
    spec.required = !legacy_;
    spec.capture_required = true;
    if (legacy_) {
        // A frozen image: no source operation has been observed on this
        // timeline; the ordinal and the environment start as at boot and the
        // title's next SetUpFrame/Link re-establish them.
        spec.absent = [this] {
            std::string error;
            if (!legacy_()) throw std::runtime_error("Graphics source frame section is missing without an admitted legacy import");
            if (!CanCheckpoint(&error) || !RequireClosedRecording(&error))
                throw std::runtime_error("Graphics source frame legacy reset: " + error);
            if (!service_->RestoreFrame(0)) throw std::logic_error("Graphics source frame changed during quiescent publication");
            environment_.ResetAfterQuiescence();
        };
    }
    spec.serialize = [this](std::vector<uint8_t>& bytes) {
        Snapshot snapshot;
        std::string error;
        if (!CanCheckpoint(&error) || !environment_.CaptureState(snapshot, &error))
            throw std::runtime_error("Graphics source frame capture: " + error);
        if (snapshot.next_frame != service_->NextFrame())
            throw std::runtime_error("Graphics source frame and submission ordinals differ");
        if (!State::Encode(snapshot, bytes, &error))
            throw std::runtime_error("Graphics source frame capture: " + error);
    };
    spec.validate = [](const savestate::Blob& blob, std::string* error) {
        Snapshot snapshot;
        return blob.version == State::kVersion && State::Decode(blob.payload, snapshot, error);
    };
    spec.restore = [this](const savestate::Blob& blob, std::string* error) {
        Snapshot snapshot, current;
        if (!State::Decode(blob.payload, snapshot, error) || !CanCheckpoint(error) ||
            !RequireClosedRecording(error) || !environment_.CaptureState(current, error)) return false;
        if (!service_->RestoreFrame(snapshot.next_frame) || !environment_.RestoreState(snapshot, error))
            throw std::logic_error("Graphics source frame changed during quiescent publication");
        return true;
    };
    if (!restorer.Registry().Register(spec)) return false;
    if (!RegisterUnlinkedState(restorer)) return false;
    checkpoint_ = std::make_unique<GraphicsCheckpoint>(*video_->Scene(),
        [this](std::function<bool()> work) {
            if (!CanCheckpoint(nullptr)) return false;
            const auto run = [&] {
                return retained_->RunOrdered([&] {
                    video_->EnsureCurrent();
                    return work();
                });
            };
            return consumer_ ? consumer_->RunOrdered(run) : run();
        },video_->MoviePresentation(),video_->Display(),[this](uint32_t width,uint32_t height) {
            video_->OnWindowResizedOffMain(int(width),int(height));
        });
    checkpoint_->SetLegacyImport(legacy_);
    if (!checkpoint_->Register(restorer)) return false;
    source_state_registered_ = true;
    return true;
}

bool GraphicsOutput::CanCheckpointScene(std::string* reason) {
    if (!CanCheckpoint(reason)) return false;
    if (checkpoint_) return checkpoint_->CanCapture(reason);
    if (reason) *reason = "Graphics surface state is not composed";
    return false;
}

void GraphicsOutput::StopAfterQuiescence() {
    if (stopped_) return;
    if (consumer_) consumer_->Sync();
    stopped_ = true;
    service_->ResetAfterQuiescence(); // invalidates deferred bridge callbacks first
    for (auto& batch : owned_) {
        if (batch->state.load() == OwnedBatch::State::Pending) {
            batch->state.store(OwnedBatch::State::Cancelled);
            --owned_pending_;
        }
        batch->work = {}; // producer tickets observe cancellation while still alive
    }
    owned_.clear();
    environment_.ResetAfterQuiescence();
    if (video_ && retained_) {
        const auto release = [this] {
            return retained_->RunOrdered([this] {
                video_->EnsureCurrent();
                video_->ClearMovieFrame();
                return true;
            });
        };
        if (consumer_) consumer_->RunOrdered(release); else release();
    }
}

void GraphicsOutput::OnDraw(const gs::DrawIr& draw) {
    // The bridge's Plain sink. A selected (All) Strip never arrives here: its
    // retained-at-slot route is DeliverOwned's explicit SubmitDrawIr.
    if (strips_ == GraphicsStrips::All)
        throw std::logic_error("Graphics selected Strip reached retained draw publication");
    state_.SubmitDrawIr(draw);
}
void GraphicsOutput::OnJob(const gs::JobRec&) {
    throw std::logic_error("Graphics Strip delivery cannot publish a resource job");
}
void GraphicsOutput::OnFrame(const gs::JobRec&) {
    throw std::logic_error("Graphics Strip delivery cannot publish a frame boundary");
}

void GraphicsOutput::Record(verify::RunMeta& meta) const {
    const auto counts = service_->Counts();
    const auto& output = counts.output;
    // The census of a faulted run is the attribution its owner reads
    // (`graphics_refusals_*.first_fault`, `graphics_strip_output.failed`).
    // Publish it; the fault itself has already been raised at the walk that
    // observed it (GraphicsService::Fault) and the leg reports that failure.
    const bool faulted = counts.ordering_errors || output.failed_groups || owned_failed_.load();
    meta.Set("graphics_output_faulted", faulted ? "1" : "0");
    meta.Set("graphics_owned_async", std::string("on=") + (async_owned_ ? "1" : "0") +
                                         ",delivered=" + std::to_string(async_delivered_.load()) +
                                         ",ran=" + std::to_string(async_ran_.load()) +
                                         ",not_completed=" + std::to_string(async_not_completed_.load()) +
                                         ",failed=" + (async_failed_.load() ? "1" : "0"));
    meta.Set("graphics_bridge_drain", "stream_ordered=" + std::to_string(drains_stream_ordered_.load()) +
                                          ",waited=" + std::to_string(drains_waited_.load()));
    meta.Set("graphics_owned_output", "queued=" + std::to_string(owned_queued_.load()) +
             ",completed=" + std::to_string(owned_completed_.load()) +
             ",pending=" + std::to_string(owned_pending_.load()) +
             ",failed=" + std::to_string(owned_failed_.load()) +
             ",retained=" + std::to_string(owned_retained_.load()) +
             ",refused=" + std::to_string(owned_refused_.load()) +
             ",cancelled=" + std::to_string(owned_cancelled_.load()));
    meta.Set("graphics_service_installed", installed_ ? "1" : "0");
    // Textures at load: the slot binds submitted at the walk, delivered at their
    // GIF stream slot, the releases, the restore resets (and binds that found no
    // attached consumer -- the module-only compositions).
    meta.Set("graphics_texture_binds", "submitted=" + std::to_string(texture_binds_.load()) +
             ",delivered=" + std::to_string(texture_delivered_.load()) +
             ",releases=" + std::to_string(texture_releases_.load()) +
             ",resets=" + std::to_string(texture_resets_.load()) +
             ",no_consumer=" + std::to_string(texture_no_consumer_.load()));
    // The live F8 path: resets of the open recording on the running
    // session and the owned batches each dropped (they were the GXPD digest's items).
    meta.Set("graphics_live_restore", "resets=" + std::to_string(live_restores_) +
                                          ",cancelled=" + std::to_string(live_restore_cancelled_));
    const auto digest = [](const Unlinked& u) {
        std::string buckets;
        for (size_t i = 0; i < u.bucket_items.size(); ++i) {
            if (!u.bucket_items[i]) continue;
            if (!buckets.empty()) buckets += '/';
            buckets += std::to_string(i) + ":" + std::to_string(u.bucket_items[i]);
        }
        return "source_frame=" + std::to_string(u.source_frame) + ",groups=" + std::to_string(u.groups) +
               ",items=" + std::to_string(u.items) + ",owned=" + std::to_string(u.owned_items) +
               ",buckets=" + (buckets.empty() ? std::string("none") : buckets);
    };
    // The capture-time settle: how many boundary settles ran, how many found
    // an open (unlinked) recording, the latest digest, and the restored image's.
    meta.Set("graphics_capture_settle", "settles=" + std::to_string(settles_) +
             ",unlinked=" + std::to_string(settled_unlinked_) + "," + digest(unlinked_));
    meta.Set("graphics_restore_unlinked", digest(restored_unlinked_));
    meta.Set("graphics_strip_selection", strips_ == GraphicsStrips::All ? "all" :
             strips_ == GraphicsStrips::Plain ? "plain" : "model");
    const auto strip = strip_output_->Counts();
    meta.Set("graphics_strip_output","captured=" + std::to_string(strip.captured) +
        ",completed=" + std::to_string(strip.completed) + ",failed=" + std::to_string(strip.failures) +
        ",retained=" + std::to_string(strip.retained) + ",first_refusal=" + std::to_string(strip.first_refusal));
    meta.Set("graphics_service",
             "strip_calls=" + std::to_string(counts.strip_calls) +
             ",replaced=" + std::to_string(counts.replaced_producers) +
             ",empty=" + std::to_string(counts.empty_producers) +
             ",owned_strips=" + std::to_string(counts.owned_strip_producers) +
             ",model_producers=" + std::to_string(counts.model_producers) +
             ",fallback=" + std::to_string(counts.fallback) +
             ",interior=" + std::to_string(counts.interior_fallback) +
             ",linked=" + std::to_string(counts.linked_submissions) +
             ",unsubmitted=" + std::to_string(counts.unsubmitted) +
             ",pending=" + std::to_string(counts.pending_submissions) +
             ",queued_groups=" + std::to_string(output.queued_groups) +
             ",completed=" + std::to_string(output.completed) +
             ",recorded_work=" + std::to_string(output.recorded_work) +
             ",completed_work=" + std::to_string(output.completed_work) +
             ",refused_work=" + std::to_string(output.refused_work) +
             ",ordering_errors=" + std::to_string(counts.ordering_errors) +
             ",failed_groups=" + std::to_string(output.failed_groups) +
             ",discarded_groups=" + std::to_string(output.discarded_groups) +
             ",discarded_walks=" + std::to_string(counts.discarded_walks));
    ::title::GraphicsReport::Strips(meta, counts);
    refusals_.Record(meta, {::title::GraphicsRefusals::Family::Strips});
    std::string buckets;
    for (size_t i = 0; i < output.replaced_bucket_jobs.size(); ++i) {
        if (i) buckets += ',';
        buckets += std::to_string(i) + ":" + std::to_string(output.replaced_bucket_jobs[i]);
    }
    meta.Set("graphics_replaced_buckets", buckets);
    const auto environment = environment_.Observed();
    meta.Set("graphics_frame_environment",
             "frames=" + std::to_string(environment.completed_frame_boundaries) +
             ",initializations=" + std::to_string(environment.complete_display_initializations) +
             ",interior=" + std::to_string(environment.interior_entries));
}

}  // namespace ps2r::app
