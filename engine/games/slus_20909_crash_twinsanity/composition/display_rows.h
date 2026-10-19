#pragma once
// DisplayRows -- the composition root's binding of the title's Display page
// rows to the live video stack: the internal scale the user
// asks for in the menu is applied on the backend owner BETWEEN two frames (the
// GS consumer's ordered slot in play -- never a wait on the game thread, the
// clock contract's rule 2), the frames lane's hash scale follows, and the
// camera transform source (CameraTrack) is fed by the Graphics application's
// projection entry while the presenter renders at the display's rate.
//
// video.render_rate=display (A3): the video stack's FrameReview records each
// frame's backend commands; this binding carries the tick's camera (toCamera,
// cameraToScreen, the boundary's wall instant) to the review in stream order
// behind the frame's present (one ordered job at the boundary), switches the
// recording on/off with the row, and at every presenter edge enqueues ONE
// re-view job on the GS consumer (dropped, never queued behind, while the
// previous one has not run): the review re-issues the previous tick's frame
// with the camera interpolated at the EDGE's instant (the presenter's regular
// clock, not the consumer's service time) and presents it; the present's window
// readback is an asynchronous pack-buffer transfer handed over at the next
// present (VideoStack::PresentWindow), so the consumer never waits for the GPU
// inside a re-view. The game thread never waits; the tick's own present is held
// while the review can show the frame. present.trace (beside camera.trace,
// harness runs): one line per picture the window received -- `wall_ns kind tick
// phase x y z` with kind 1 = a re-viewed frame, 0 = the tick's frame (the review
// not armed) -- the RootHighFrameRate judder metric reads it.
//
// Composed in play mode only (a session exists); a headless leg constructs none
// of it. OnBoundary runs on the game thread at the frame boundary (MaintDuties).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include "app/host/camera_track.h"
#include "engine/render/review/frame_review.h"

namespace ps2r::gs { class DrawIrThreadQueue; class DrawIrQueueConsumer; }
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

class SessionRuntime;
class VideoStack;
class GraphicsApplication;

class DisplayRows {
public:
    // `queue` null = the inline consumer (the apply runs on the calling thread;
    // the re-view needs the consumer thread and stays off).
    DisplayRows(SessionRuntime& session, VideoStack& video, gs::DrawIrThreadQueue* queue,
                gs::DrawIrQueueConsumer* consumer, std::string camera_trace_path);
    ~DisplayRows();
    // Game thread, every frame boundary: a pending scale request becomes one
    // ordered job on the backend owner; the camera observer attaches once the
    // Graphics application exists (`graphics` may be null until then); the
    // tick's camera travels to the review; the recording follows the row.
    void OnBoundary(GraphicsApplication* graphics);
    // The presenter's per-present hook (SessionRuntime::SetPresentObserver).
    void OnPresent(std::chrono::steady_clock::time_point now);
    CameraTrack& Camera() { return camera_; }
    void Record(verify::RunMeta& meta) const;

    // The draws the review re-projects: the title's camera-projected VU1
    // program families (the world-space geometry); everything else -- the
    // typesetter, the owned screen-space passes, the model/raw route -- draws
    // at the tick (Space::Fixed).
    static render::FrameReview::Space Classify(const gs::DrawIr& draw);
    // The re-view's budget: an edge posts a re-view only while the consumer
    // keeps up with the stream -- it was idle within the last tick and a half,
    // or its backlog is below kReplayBacklogLimit records (a frame's records
    // arrive in a burst at the boundary: depth alone over-reads lateness). A
    // consumer busy for longer is behind: the live frame comes first (a re-view
    // ahead of a backlog would make the stream drop frames -- rule 2 -- and the
    // dropped frame disarms the review).
    static constexpr size_t kReplayBacklogLimit = 400;
    static constexpr std::chrono::nanoseconds kReplayIdleWithin{25000000};

private:
    void Trace(int kind, uint64_t tick, double phase, const double position[3],
               std::chrono::steady_clock::time_point now);

    SessionRuntime& session_;
    VideoStack& video_;
    gs::DrawIrThreadQueue* queue_;
    gs::DrawIrQueueConsumer* consumer_;
    CameraTrack camera_;
    bool camera_bound_ = false;
    uint64_t scale_requests_ = 0, scale_async_ = 0, scale_inline_ = 0;
    std::atomic<uint64_t> scale_applied_{0};   // counted on the backend owner
    int last_applied_ = 0;
    // The re-view (A3).
    std::mutex camera_mu_;                      // the pending sample: game thread writes, boundary reads
    bool pending_valid_ = false;
    float pending_to_camera_[16] = {}, pending_camera_to_screen_[16] = {};
    bool recording_ = false;                    // the state last sent to the review
    std::atomic<bool> replay_pending_{false};   // a re-view job is queued and has not run
    uint64_t camera_jobs_ = 0, recording_changes_ = 0;
    std::atomic<uint64_t> replay_jobs_{0}, replay_presented_{0}, replay_refused_{0}, edges_busy_{0}, edges_tick_{0},
        edges_behind_{0};
    std::atomic<uint64_t> replay_us_sum_{0}, replay_us_max_{0};
    std::string trace_path_;
    std::mutex trace_mu_;
    std::FILE* trace_ = nullptr;
    bool trace_failed_ = false;
};

}  // namespace ps2r::app
