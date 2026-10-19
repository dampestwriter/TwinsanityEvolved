#include "games/slus_20909_crash_twinsanity/composition/display_rows.h"

#include <algorithm>
#include <array>
#include <memory>

#include "games/slus_20909_crash_twinsanity/composition/graphics_application.h"
#include "app/host/session_runtime.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/core/verify/run_meta.h"
#include "engine/gs/hle/ir/gsdl_format.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/hle/queue/draw_ir_thread_queue.h"

namespace ps2r::app {

namespace {
using Clock = std::chrono::steady_clock;

// The title's camera-projected VU1 program families (games/<t>/vu1_library:
// the world-space geometry the programs project through the view's toScreen):
// the strips, the skinned and lit meshes, the displaced meshes, the crystal
// programs and the particle / billboard emitters. The typesetter
// (vs_sprite_text) is screen-space and stays Fixed, as does every unstamped
// draw (the owned screen-space passes and the model/raw route).
constexpr std::array<const char*, 11> kReviewedPrograms{
    "vs_family_tristrip", "vs_textured_tristrip", "vs_poscolor_strip", "vs_lit_tristrip",
    "vs_skinned_lit_strip", "vs_indexed_displace", "vs_crys_dual", "vs_crys_displace",
    "vs_zero_kick", "vs_zero_kick_cont", "vs_zero_kick_a_cont"};

const std::array<uint32_t, kReviewedPrograms.size()>& ReviewedHandlers() {
    static const std::array<uint32_t, kReviewedPrograms.size()> ids = [] {
        std::array<uint32_t, kReviewedPrograms.size()> out{};
        for (size_t i = 0; i < kReviewedPrograms.size(); ++i) out[i] = gs::GsdlFormat::HandlerId(kReviewedPrograms[i]);
        return out;
    }();
    return ids;
}
}  // namespace

render::FrameReview::Space DisplayRows::Classify(const gs::DrawIr& draw) {
    if (draw.identity.hle_handler == 0) return render::FrameReview::Space::Fixed;
    for (uint32_t id : ReviewedHandlers())
        if (id == draw.identity.hle_handler) return render::FrameReview::Space::Screen;
    return render::FrameReview::Space::Fixed;
}

DisplayRows::DisplayRows(SessionRuntime& session, VideoStack& video, gs::DrawIrThreadQueue* queue,
                         gs::DrawIrQueueConsumer* consumer, std::string camera_trace_path)
    : session_(session), video_(video), queue_(queue), consumer_(consumer) {
    if (!camera_trace_path.empty()) {
        const size_t slash = camera_trace_path.find_last_of('/');
        trace_path_ = (slash == std::string::npos ? std::string() : camera_trace_path.substr(0, slash + 1)) + "present.trace";
        camera_.SetTrace(std::move(camera_trace_path));
    }
    if (render::FrameReview* review = video_.Review()) review->SetClassifier(&DisplayRows::Classify);
}

DisplayRows::~DisplayRows() {
    std::lock_guard<std::mutex> lock(trace_mu_);
    if (trace_) std::fclose(trace_);
}

void DisplayRows::Trace(int kind, uint64_t tick, double phase, const double position[3], Clock::time_point now) {
    if (trace_path_.empty()) return;
    std::lock_guard<std::mutex> lock(trace_mu_);
    if (!trace_ && !trace_failed_) {
        trace_ = std::fopen(trace_path_.c_str(), "w");
        if (!trace_) trace_failed_ = true;
        else std::fprintf(trace_, "# wall_ns kind tick phase x y z   (kind 1 = re-viewed frame, 0 = the tick's frame)\n");
    }
    if (!trace_) return;
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::fprintf(trace_, "%lld %d %llu %.4f %.4f %.4f %.4f\n", static_cast<long long>(wall_ns), kind,
                 static_cast<unsigned long long>(tick), phase, position[0], position[1], position[2]);
    std::fflush(trace_);   // a leg may end by exit(): every present on disk
}

void DisplayRows::OnBoundary(GraphicsApplication* graphics) {
    if (!camera_bound_ && graphics) {
        graphics->SetCameraObserver([this](const float* to_camera, const float* camera_to_screen) {
            camera_.NoteCamera(to_camera);
            // The review takes the samples the track takes: a singular matrix (a
            // view not yet made, a menu's) carries no camera.
            float position[3];
            if (!to_camera || !camera_to_screen || !CameraTrack::CameraPosition(to_camera, position)) return;
            std::lock_guard<std::mutex> lock(camera_mu_);
            std::copy(to_camera, to_camera + 16, pending_to_camera_);
            std::copy(camera_to_screen, camera_to_screen + 16, pending_camera_to_screen_);
            pending_valid_ = true;
        });
        camera_bound_ = true;
    }
    const Clock::time_point now = Clock::now();
    camera_.NoteBoundaryAt(now);   // the frame's camera sample, stamped at the boundary
    if (render::FrameReview* review = video_.Review()) {
        // The re-view follows the render-rate row; it needs the consumer thread
        // (the GL context's owner; the game thread never draws for the presenter).
        const bool want = session_.RenderRateSetting() != 0 && queue_ != nullptr;
        if (want != recording_) {
            recording_ = want;
            ++recording_changes_;
            if (queue_) queue_->RunOrderedAsync([review, want] { review->SetRecording(want); return true; });
        }
        render::FrameReview::Camera camera;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(camera_mu_);
            if (pending_valid_) {
                have = true;
                pending_valid_ = false;
                for (int i = 0; i < 16; ++i) {
                    camera.to_camera[i] = pending_to_camera_[i];
                    camera.camera_to_screen[i] = pending_camera_to_screen_[i];
                }
            }
        }
        // Behind the frame's present in stream order (slot 0 pushed the frame
        // record): the review attaches it to the frame just completed.
        if (have && recording_ && queue_) {
            camera.valid = true;
            camera.at = now;
            ++camera_jobs_;
            queue_->RunOrderedAsync([review, camera] { review->NoteCamera(camera); return true; });
        }
    }
    const int scale = session_.TakeScaleRequest();
    if (scale <= 0 || (scale == last_applied_ && scale == video_.Scale())) return;
    ++scale_requests_;
    last_applied_ = scale;
    // The apply on the backend owner between frames: the GS consumer's ordered
    // slot (shared surfaces drained, the GL context current) without the game
    // thread waiting; inline when the consumer runs on this thread.
    auto apply = [this, scale] {
        const auto body = [this, scale] {
            video_.EnsureCurrent();
            video_.ApplyScale(scale);
            if (consumer_ && consumer_->FrameHashScale() != 0) consumer_->SetFrameHashScale(scale);
            scale_applied_.fetch_add(1, std::memory_order_relaxed);
            return true;
        };
        return consumer_ ? consumer_->RunOrdered(body) : body();
    };
    if (queue_) {
        ++scale_async_;
        queue_->RunOrderedAsync(apply);
    } else {
        ++scale_inline_;
        apply();
    }
}

void DisplayRows::OnPresent(Clock::time_point now) {
    const CameraTrack::Presented presented = camera_.Present(now);
    render::FrameReview* review = video_.Review();
    if (review && queue_ && session_.RenderRateSetting() != 0 && review->Armed()) {
        // ONE re-view job per edge; while the previous one has not run the edge
        // shows the previous picture (never a queue behind the consumer).
        if (replay_pending_.exchange(true, std::memory_order_acq_rel)) {
            edges_busy_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (queue_->Depth() > kReplayBacklogLimit && !queue_->ConsumerIdleWithin(kReplayIdleWithin)) {
            replay_pending_.store(false, std::memory_order_release);
            edges_behind_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // The phase is read at THIS edge's instant -- the presenter's regular
        // clock -- not at the consumer's run time: the job's service jitter (one
        // record's work) would otherwise be jitter in the shown camera, and a
        // lookahead to the landing edge clamps at the tick's camera for the part
        // of every tick before the next camera arrives (a duplicate picture).
        // The picture lands two edges on (the replay's present queues the window
        // readback; the next present hands it over): the camera shown at wall
        // time w is the game's at w - one tick - two presents. A clamp at 1.0
        // now means the camera is genuinely late (the consumer behind the stream
        // or the EE's tick late), not the clock's geometry.
        // The presenter's side entry (main is not the stream's producer): the job
        // runs on the consumer inside its ordered slot (the GL context current,
        // the pending upload drained), behind every record queued so far.
        const bool queued = queue_->PostPresenterWork([this, review, now] {
            // Whatever the slot does -- a replay, a refusal, a thrown RunOrdered
            // (ThreadMain swallows it) -- the next edge may post again.
            const auto clear = [](std::atomic<bool>* flag) { flag->store(false, std::memory_order_release); };
            const std::unique_ptr<std::atomic<bool>, decltype(clear)> reset(&replay_pending_, clear);
            const auto body = [this, review, now] {
                const render::FrameReview::Replayed r = review->Replay(now);
                if (r.presented) {
                    replay_presented_.fetch_add(1, std::memory_order_relaxed);
                    replay_us_sum_.fetch_add(r.micros, std::memory_order_relaxed);
                    uint64_t seen = replay_us_max_.load(std::memory_order_relaxed);
                    while (r.micros > seen && !replay_us_max_.compare_exchange_weak(seen, r.micros)) {}
                    Trace(1, r.tick, r.phase, r.position, Clock::now());
                } else {
                    replay_refused_.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            };
            return consumer_ ? consumer_->RunOrdered(body) : body();
        });
        if (queued) replay_jobs_.fetch_add(1, std::memory_order_relaxed);
        else replay_pending_.store(false, std::memory_order_release);
        return;
    }
    edges_tick_.fetch_add(1, std::memory_order_relaxed);
    if (presented.valid) {
        const double position[3] = {presented.latest[0], presented.latest[1], presented.latest[2]};
        Trace(0, presented.ordinal, presented.phase, position, now);
    }
}

void DisplayRows::Record(verify::RunMeta& meta) const {
    // grab=WxH: the consumer's latest display grab -- the frame the presenter
    // shows at the end of the leg (512x448 at scale 1, 1024x896 at scale 2 ...):
    // the Root pin's evidence that a live scale change reached the presented frame.
    std::string grab = "none";
    if (consumer_) {
        const gs::DrawPipeline::Frame header = consumer_->LastFrameHeaderCopy();
        if (header.grabbed && header.w > 0) grab = std::to_string(header.w) + "x" + std::to_string(header.h);
    }
    meta.Set("display_rows", "scale_setting=" + std::to_string(session_.ScaleSetting()) +
                                 ",scale=" + std::to_string(video_.Scale()) +
                                 ",scale_requests=" + std::to_string(scale_requests_) +
                                 ",scale_applied=" + std::to_string(scale_applied_.load()) +
                                 ",scale_async=" + std::to_string(scale_async_) + ",scale_inline=" + std::to_string(scale_inline_) +
                                 ",grab=" + grab +
                                 ",vsync=" + (session_.VSyncState() ? "1" : "0") +
                                 ",fullscreen=" + std::to_string(session_.DisplayModeState()) +
                                 ",aspect=" + std::to_string(session_.AspectSetting()) +
                                 ",render_rate=" + (session_.RenderRateSetting() ? "display" : "60") +
                                 ",present_hz=" + std::to_string(session_.PresentHz()) +
                                 ",presents=" + std::to_string(session_.Presents()));
    meta.Set("hfr_camera", camera_.Census());
    // hfr_review=: the re-view's census (the consumer's counters read after the
    // queue quiesced) and this binding's edges: jobs queued / presented / refused,
    // edges that found a job pending (busy) or showed the tick's frame.
    std::string row = "recording=" + std::string(recording_ ? "1" : "0") + ",changes=" + std::to_string(recording_changes_) +
                      ",camera_jobs=" + std::to_string(camera_jobs_) +
                      ",replay_jobs=" + std::to_string(replay_jobs_.load()) +
                      ",replay_presented=" + std::to_string(replay_presented_.load()) +
                      ",replay_refused=" + std::to_string(replay_refused_.load()) +
                      ",edges_busy=" + std::to_string(edges_busy_.load()) + ",edges_behind=" + std::to_string(edges_behind_.load()) +
                      ",edges_tick=" + std::to_string(edges_tick_.load());
    const uint64_t presented = replay_presented_.load();
    row += ",replay_us_mean=" + std::to_string(presented ? replay_us_sum_.load() / presented : 0) +
           ",replay_us_max=" + std::to_string(replay_us_max_.load());
    if (const render::FrameReview* review = video_.Review()) {
        const render::FrameReview::Census c = review->Count();
        row += ",armed=" + std::string(review->Armed() ? "1" : "0") +
               ",frames=" + std::to_string(c.frames) + ",frames_uncamera=" + std::to_string(c.frames_uncamera) +
               ",empty_presents=" + std::to_string(c.empty_presents) +
               ",cameras=" + std::to_string(c.cameras) + ",cameras_unframed=" + std::to_string(c.cameras_unframed) +
               ",replays=" + std::to_string(c.replays) + ",refused=" + std::to_string(c.replays_refused) +
               ",cuts=" + std::to_string(c.cuts) +
               ",reviewed_draws=" + std::to_string(c.reviewed_draws) + ",fixed_draws=" + std::to_string(c.fixed_draws) +
               ",stale=" + std::to_string(c.stale_commands) + ",unprojectable=" + std::to_string(c.unprojectable_draws) +
               ",tick_presents=" + std::to_string(c.tick_presents) + ",tick_suppressed=" + std::to_string(c.tick_suppressed) +
               ",stale_cameras=" + std::to_string(c.stale_cameras) +
               ",faults=" + std::to_string(c.faults) +
               ",live_draw_us_mean=" + std::to_string(c.frames ? c.live_draw_us_sum / c.frames : 0) +
               ",live_draw_us_max=" + std::to_string(c.live_draw_us_max) +
               ",frame_span_us_mean=" + std::to_string(c.frames ? c.frame_span_us_sum / c.frames : 0) +
               ",frame_span_us_max=" + std::to_string(c.frame_span_us_max) +
               ",live_present_us_mean=" + std::to_string(c.live_presents ? c.live_present_us_sum / c.live_presents : 0) +
               ",replay_present_us_mean=" + std::to_string(c.replays ? c.replay_present_us_sum / c.replays : 0) +
               ",commands_last=" + std::to_string(c.commands_last) + ",shadows=" + std::to_string(c.shadows) +
               ",trace=" + (trace_ ? "on" : trace_failed_ ? "failed" : "off");
    }
    // The window readback: the 60-rate path's synchronous reads, the re-view's
    // asynchronous transfers, the hand-overs that waited for the GPU, the lost,
    // and the consumer's mean time queuing / handing over a transfer.
    const VideoStack::ReadbackCensus rb = video_.WindowReadbackCensus();
    row += ",readback_sync=" + std::to_string(rb.sync) + ",readback_async=" + std::to_string(rb.async) +
           ",readback_waited=" + std::to_string(rb.waited) + ",readback_lost=" + std::to_string(rb.lost) +
           ",readback_queue_us_mean=" + std::to_string(rb.async ? rb.queue_us / rb.async : 0) +
           ",readback_deliver_us_mean=" + std::to_string(rb.async ? rb.deliver_us / rb.async : 0);
    meta.Set("hfr_review", row);
}

}  // namespace ps2r::app
