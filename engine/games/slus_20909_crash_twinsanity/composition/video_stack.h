// app/host/video_stack.h -- VideoStack: the live video stack of the composition
// root -- the host window, the GL 4.1 context, the GlDevice backend and the
// Phase-6 scene objects (VramImage, the title's RendererProfile, ScalePolicy,
// RenderTargetCache, TargetRefresher, TextureResolver, ScanoutComposer,
// Presenter, ClassRegistry, SequenceRecipe, DrawPipeline), built in the corpus
// replay driver's order with the LIVE rules:
//   * no OnSetWindow seed from the window size (the live window comes from the
//     first DISPLAY job; the seed is the replay tool's quirk);
//   * the scanout base starts UNSET (the original runtime's 0xFFFFFFFF sentinel):
//     DrawScanout presents nothing until the first DISPFB job set the base, while
//     the display grab still falls back to the most recently drawn target;
//   * the backend's render-size fallback stays at the pipeline's native default.
// Create() returns false when no window / GL context / device is available (the
// root then keeps the stub present and reports video=stub); nothing here fails a
// leg. Shutdown order: the profile releases its backend resources BEFORE the
// device shuts down, then the context, then the window.
//
// The GL context is current on one host thread at a time; every guest thread may
// reach the boundary, so EnsureCurrent() re-binds the context when the calling
// thread changed (counted).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <thread>

#include "engine/core/config/config.h"
#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/present/presenter.h"
#include "engine/gs/hle/present/scanout_composer.h"
#include "engine/gs/hle/targets/render_target_cache.h"
#include "engine/gs/hle/targets/scale_policy.h"
#include "engine/gs/hle/targets/target_refresher.h"
#include "engine/gs/hle/texsrc/texture_resolver.h"
#include "engine/gs/vram/vram_image.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "engine/render/review/frame_review.h"
#include "games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_intent.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/display_layout.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"

namespace title {
class GraphicsSurfaces;
class GraphicsResources;
class GraphicsRenderer;
class GraphicsScene;
class GraphicsPresentation;
}

namespace ps2r::app {

class VideoStack {
public:
    VideoStack();
    ~VideoStack();
    VideoStack(const VideoStack&) = delete;
    VideoStack& operator=(const VideoStack&) = delete;

    // Window (hidden when headless) -> GL context -> swap interval from
    // video.vsync -> GlDevice -> the scene at ScaleSettings{video.scale} over
    // `vram`: the GS's local-memory image (the one its transfer engine writes),
    // which the decode tiers, the target cache and the refresher read -- one
    // image, as the console has one local memory. The caller keeps it alive
    // past Shutdown(). False = no GL available (nothing was built).
    bool Create(const VideoConfig& video, bool headless, const char* title, gs::VramImage& vram);
    const gs::VramImage* Vram() const { return vram_; }
    void Shutdown();
    bool Active() const { return pipe_ != nullptr; }

    gs::DrawPipeline* Pipe() { return pipe_.get(); }
    const char* Renderer() const { return ctx_.Renderer(); }
    const char* Version() const { return ctx_.Version(); }
    int Scale() const { return scale_factor_; }
    // A live internal-scale change (video.scale from the Display page
    // A2): on the backend owner between two frames (the GS consumer's ordered
    // slot in play). The scale policy takes the factor; the target cache
    // re-creates each target at its next use (content rescued first); the owned
    // display images re-allocate. Returns the factor applied (clamped 1..4).
    int ApplyScale(int scale);
    uint64_t ContextRebinds() const { return rebinds_; }
    // The player's texture-filtering enhancement (video.texture_filter /
    // video.anisotropy): the GL device samples the owned textures
    // through this selection from the next draw on; the Display page changes its
    // fields live. Set once after Create (any time before the first draw).
    void SetTextureFilter(std::shared_ptr<const render::TextureFilterSelection> selection) {
        be_.SetTextureFilter(std::move(selection));
    }
    // run.meta video_texture_filter=: the selection in force, the driver's
    // anisotropy limit and the executor's owned / enhanced / hardware-LOD draws.
    std::string TextureFilterCensus() const;
    int WarmedPrograms() const { return warmed_; }

    // Before any backend work from the calling thread.
    void EnsureCurrent();
    void ReleaseCurrent();
    // Main installs once before guest execution. Game output is copied after
    // the renderer's display readback; the main presenter owns its own context.
    // `main_post` (optional): the same handoff WITHOUT a wait, for the game
    // thread's per-boundary path (ApplyPendingWindowResize) -- main may be inside
    // a display swap for up to a second. Without it the
    // boundary falls back to main_dispatch (a bootstrap leg: inline).
    void SetSessionOutput(std::function<void(std::vector<uint8_t>, int, int)> output,
                          std::function<bool(std::function<void()>)> main_dispatch,
                          std::function<bool(std::function<void()>)> main_post = {});
    // A SetWindow job changed the window size: the device viewport (and the
    // visible window when not headless) follow it. From the thread that owns
    // the process's windows (the main thread, where the EE runs).
    void OnWindowResized(int w, int h);
    // The same from the DrawIR consumer thread: the device viewport applies now
    // (plain backend state, the context owner's); the OS window resize is held
    // -- SDL/Cocoa window geometry calls are main-thread only -- and applied by
    // ApplyPendingWindowResize() at the frame boundary. NEVER a synchronous
    // call into the main thread from here: the main thread may be waiting for
    // the consumer in DrawIrThreadQueue::Sync() (a restore's
    // SettleOwnedForCapture), and the round trip would be a deadlock. Counted
    // in DeferredResizes(); successive calls before the boundary coalesce.
    void OnWindowResizedOffMain(int w, int h);
    // The frame boundary, on the game's host thread (the dispatch reaches the
    // main thread when that is another thread; it is pumping at a boundary).
    void ApplyPendingWindowResize();
    uint64_t DeferredResizes() const { return deferred_resizes_.load(); }

    // Typed output uses the SAME backend and scene allocations as Pipe().
    // Call on the ordered backend owner after preceding retained work drains.
    ::title::GraphicsResources* Resources() { return resources_.get(); }
    ::title::GraphicsRenderer* GraphicsRenderer() { return graphics_renderer_.get(); }
    ::title::GraphicsScene* Scene() { return graphics_scene_.get(); }
    ::title::GraphicsPresentation* MoviePresentation() { return graphics_presentation_.get(); }
    ::title::GraphicsDisplay* Display() { return graphics_display_.get(); }
    // Backend-owner calls, ordered with source frame submission.
    bool SelectMovieFrame(const ::title::FrameEnvironment::SceneFrame&);
    void ClearMovieFrame();
    // Read only after ordered output has drained. The presentation lease is a
    // separate state owner; surface persistence alone must not lose it.
    bool HasSelectedFrame() const;
    // GraphicsDisplay::Resolve at the ordered slot: Ok / Refused (slot released) / Failed (fault).
    ::title::GraphicsDisplay::Resolution ResolveDisplay(const ::title::FrameIntent::Output&,const ::title::FrameEnvironment::SceneFrame&);
    bool ConfigureDisplay(const ::title::DisplayLayout::Output&);
    bool DisplayMatches(const ::title::FrameEnvironment::SceneFrame&) const;
    uint64_t DisplayCopies() const;
    bool PresentOwnedFrame(bool ee_frame_end, gs::DrawPipeline::Frame* capture);
    // The display-rate re-view (video.render_rate=display): the
    // Backend proxy under the allocation layer that records a frame's commands
    // and re-issues them with the camera interpolated (FrameReview). Null until
    // the scene is built. Backend owner for every mutation; its atomics for the
    // presenter.
    render::FrameReview* Review() { return review_.get(); }
    // The window readback's census (run.meta hfr_review window_readback=): the
    // synchronous reads (the 60-rate path), the asynchronous PBO transfers queued
    // under the re-view, the hand-overs that had to wait for the GPU, and the
    // transfers whose buffer could not be mapped (no picture that edge).
    struct ReadbackCensus {
        uint64_t sync = 0, async = 0, waited = 0, lost = 0;
        uint64_t queue_us = 0;     // the consumer's time queuing a transfer (orphan + glReadPixels + fence)
        uint64_t deliver_us = 0;   // ... and handing one over (fence poll / wait + map + copy)
    };
    ReadbackCensus WindowReadbackCensus() const { return readback_census_; }

private:
    bool CreateContext(const VideoConfig& video, bool headless, const char* title);
    void BuildScene(int scale, gs::VramImage& vram);
    void PresentWindow();
    // The window readback under the display-rate re-view: PresentWindow queues
    // the back buffer's copy into a pixel-pack buffer (the GPU transfers it; the
    // consumer thread does not wait for the frame's draws to finish) and hands
    // the PREVIOUS present's picture over to the session -- its transfer had a
    // whole present period. Two buffers alternate; a hand-over whose transfer
    // has not completed waits for it (counted). The 60-rate path (the review not
    // recording) keeps the synchronous read, after draining any pending transfer.
    struct WindowReadback {
        uint32_t pbo = 0;          // GL buffer name
        void* fence = nullptr;     // GLsync of the queued transfer
        int w = 0, h = 0;
        bool pending = false;
    };
    void QueueWindowReadback(WindowReadback& slot, int w, int h);
    void DeliverWindowReadback(WindowReadback& slot);
    void ReleaseWindowReadbacks();

    platform::Window win_;
    platform::GlContext ctx_;
    render::GlDevice be_;
    std::unique_ptr<render::FrameReview> review_;
    std::unique_ptr<::title::GraphicsSurfaces> surfaces_;
    std::unique_ptr<::title::GraphicsResources> resources_;
    std::unique_ptr<::title::GraphicsRenderer> graphics_renderer_;
    std::unique_ptr<::title::GraphicsScene> graphics_scene_;
    std::unique_ptr<::title::GraphicsPresentation> graphics_presentation_;
    std::unique_ptr<::title::GraphicsDisplay> graphics_display_;
    gs::VramImage* vram_ = nullptr;   // the GS's image (not owned)
    std::unique_ptr<title::RendererProfile> profile_;
    std::unique_ptr<gs::ScalePolicy> scale_;
    std::unique_ptr<gs::RenderTargetCache> cache_;
    std::unique_ptr<gs::TargetRefresher> refresher_;
    std::unique_ptr<gs::TextureResolver> resolver_;
    std::unique_ptr<gs::ScanoutComposer> scanout_;
    std::unique_ptr<gs::Presenter> presenter_;
    std::unique_ptr<gs::ClassRegistry> registry_;
    std::unique_ptr<gs::SequenceRecipe> sequence_;
    std::unique_ptr<gs::DrawPipeline> pipe_;
    std::thread::id current_thread_;
    std::atomic<uint64_t> pending_resize_{0};   // (w << 32 | h) held for the main thread; 0 = none
    std::atomic<uint64_t> deferred_resizes_{0};
    uint64_t rebinds_ = 0;
    int warmed_ = 0;
    int scale_factor_ = 1;
    std::function<void(std::vector<uint8_t>, int, int)> session_output_;
    std::function<bool(std::function<void()>)> main_dispatch_;
    std::function<bool(std::function<void()>)> main_post_;
    WindowReadback readbacks_[2];
    int readback_next_ = 0;
    ReadbackCensus readback_census_;
};

}  // namespace ps2r::app
