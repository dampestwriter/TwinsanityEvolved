// app/host/video_stack.cpp -- the live video stack (see video_stack.h).
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"

#include <SDL.h>
#include <chrono>
#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include "engine/core/log/log.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_presentation.h"

namespace ps2r::app {

namespace {
// The native display rectangle the visible window opens at (NTSC 640x448); the
// first DISPLAY job resizes it.
constexpr int kInitialWindowW = 640;
constexpr int kInitialWindowH = 448;
}  // namespace

VideoStack::VideoStack() = default;
VideoStack::~VideoStack() { Shutdown(); }

bool VideoStack::Create(const VideoConfig& video, bool headless, const char* title, gs::VramImage& vram) {
    if (Active()) return true;
    if (!CreateContext(video, headless, title)) {
        Shutdown();
        return false;
    }
    int scale = video.scale;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    BuildScene(scale, vram);
    return true;
}

bool VideoStack::CreateContext(const VideoConfig& video, bool headless, const char* title) {
    if (!win_.Create(headless, kInitialWindowW, kInitialWindowH, title)) {
        PS2R_LOG(HOST, Warn, "video: no window (stub present)");
        return false;
    }
    if (!ctx_.Create(win_)) {
        PS2R_LOG(HOST, Warn, "video: no GL 4.1 context (stub present)");
        return false;
    }
    ctx_.SetSwapInterval(video.vsync ? 1 : 0);
    if (!be_.Init([this] { PresentWindow(); })) {
        PS2R_LOG(HOST, Warn, "video: GlDevice::Init failed (stub present)");
        return false;
    }
    be_.SetWindowSize(win_.Width(), win_.Height());
    current_thread_ = std::this_thread::get_id();
    PS2R_LOG(HOST, Info, "video: GL %s / %s (%dx%d%s)", ctx_.Version(), ctx_.Renderer(), win_.Width(), win_.Height(),
             headless ? ", hidden" : "");
    return true;
}

void VideoStack::BuildScene(int scale, gs::VramImage& vram) {
    scale_factor_ = scale;
    vram_ = &vram;
    profile_ = std::make_unique<title::RendererProfile>();
    // The re-view proxy sits UNDER the allocation layer: every command of the
    // retained pipeline and of the typed Graphics reaches the device through it.
    review_ = std::make_unique<render::FrameReview>(be_);
    surfaces_ = std::make_unique<::title::GraphicsSurfaces>(*review_);
    auto& backend = *surfaces_;
    scale_ = std::make_unique<gs::ScalePolicy>(gs::ScaleSettings{scale}, &profile_->Vram());
    cache_ = std::make_unique<gs::RenderTargetCache>(backend, *vram_, *scale_);
    refresher_ = std::make_unique<gs::TargetRefresher>(backend, *vram_, *cache_);
    resolver_ = std::make_unique<gs::TextureResolver>(backend, *vram_, *cache_, *refresher_, profile_->Vram());
    scanout_ = std::make_unique<gs::ScanoutComposer>(backend, *cache_);
    presenter_ = std::make_unique<gs::Presenter>(*scanout_);
    registry_ = std::make_unique<gs::ClassRegistry>(profile_->ClassRules());
    sequence_ = std::make_unique<gs::SequenceRecipe>(profile_->Sequence());
    gs::DrawPipeline::Deps deps;
    deps.be = &backend;
    deps.vram = vram_;
    deps.cache = cache_.get();
    deps.refresher = refresher_.get();
    deps.resolver = resolver_.get();
    deps.scanout = scanout_.get();
    deps.presenter = presenter_.get();
    deps.layout = &profile_->Vram();
    deps.registry = registry_.get();
    deps.sequence = sequence_.get();
    pipe_ = std::make_unique<gs::DrawPipeline>(deps);
    // LIVE: the VRAM image is the console's local memory -- the resolver's
    // VRAM-decode tiers serve textured draws (the replay driver leaves this off).
    pipe_->SetLiveSnapshot(true);
    // LIVE: no window seed (the first DISPLAY job sets the window and content
    // dims); the scanout base is unset until the first DISPFB job, so the
    // display quad is skipped until then (the original's sentinel behaviour).
    scanout_->SetScanoutFbp(gs::DrawPipeline::kNoFbp);
    profile_->RegisterClassRenderers(*registry_, sequence_.get());
    warmed_ = registry_->WarmUpAll(*pipe_, nullptr);
    resources_ = std::make_unique<::title::GraphicsResources>(backend);
    graphics_renderer_ = std::make_unique<::title::GraphicsRenderer>(backend, *resources_);
    graphics_scene_ = std::make_unique<::title::GraphicsScene>(
        backend, *resources_, *graphics_renderer_, *pipe_);
    graphics_presentation_ = std::make_unique<::title::GraphicsPresentation>(
        *graphics_scene_, *resources_, *pipe_);
    graphics_display_ = std::make_unique<::title::GraphicsDisplay>(
        *graphics_scene_,*resources_,*graphics_renderer_,*pipe_,uint8_t(scale));
}

int VideoStack::ApplyScale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (!scale_ || scale == scale_factor_) return scale_factor_;
    scale_->SetFactor(scale);
    scale_factor_ = scale_->Factor();
    if (graphics_display_) graphics_display_->SetScale(static_cast<uint8_t>(scale_factor_));
    PS2R_LOG(HOST, Info, "video: internal scale %d (targets re-created at their next use)", scale_factor_);
    return scale_factor_;
}

std::string VideoStack::TextureFilterCensus() const {
    const render::TextureFilterSelection* sel = be_.TextureFilter();
    const render::GlDevice::TextureFilterCensus& c = be_.FilterCensus();
    return std::string("filter=") + render::TextureFilterSelection::Name(sel ? sel->Filter() : render::TextureFilter::Title) +
           ",anisotropy=" + std::to_string(sel ? sel->Anisotropy() : 1) +
           ",max_anisotropy=" + std::to_string(be_.MaxAnisotropy()) +
           ",owned_draws=" + std::to_string(c.owned_draws) +
           ",enhanced_draws=" + std::to_string(c.enhanced_draws) +
           ",hw_lod_draws=" + std::to_string(c.hw_lod_draws);
}

bool VideoStack::SelectMovieFrame(const ::title::FrameEnvironment::SceneFrame& frame) {
    return graphics_presentation_ && graphics_presentation_->Select(frame);
}

void VideoStack::ClearMovieFrame() {
    if (graphics_presentation_) graphics_presentation_->Clear();
}

bool VideoStack::HasSelectedFrame() const {
    return (graphics_presentation_ && graphics_presentation_->Active()) ||
        (graphics_display_ && graphics_display_->Active());
}

::title::GraphicsDisplay::Resolution VideoStack::ResolveDisplay(const ::title::FrameIntent::Output& intent,
                               const ::title::FrameEnvironment::SceneFrame& frame) {
    if (!graphics_display_) return ::title::GraphicsDisplay::Resolution::Refused;
    return graphics_display_->Resolve(intent,frame);
}
bool VideoStack::ConfigureDisplay(const ::title::DisplayLayout::Output& layout) {
    if (!graphics_scene_ || !pipe_) return false;
    const bool changed = pipe_->WindowW() != int(layout.width) || pipe_->WindowH() != int(layout.height);
    if (graphics_scene_->ConfigureDisplay(layout) != ::title::GraphicsScene::Result::Ok) return false;
    if (changed) OnWindowResizedOffMain(int(layout.width),int(layout.height));
    return true;
}
bool VideoStack::DisplayMatches(const ::title::FrameEnvironment::SceneFrame& frame) const {
    return graphics_display_ && graphics_display_->Matches(frame);
}
uint64_t VideoStack::DisplayCopies() const {
    return graphics_display_ ? graphics_display_->Copies() : 0;
}

bool VideoStack::PresentOwnedFrame(bool ee_frame_end, gs::DrawPipeline::Frame* capture) {
    if (graphics_display_ && graphics_display_->Active())
        return graphics_display_->Present(ee_frame_end,capture);
    return graphics_presentation_ && graphics_presentation_->Present(ee_frame_end, capture);
}

void VideoStack::EnsureCurrent() {
    if (!ctx_.IsCreated()) return;
    const std::thread::id me = std::this_thread::get_id();
    if (me == current_thread_ && SDL_GL_GetCurrentWindow() == win_.Handle()) return;
    if (ctx_.MakeCurrent(win_)) {
        current_thread_ = me;
        rebinds_++;
    }
}

void VideoStack::ReleaseCurrent() {
    if (SDL_GL_GetCurrentWindow() == win_.Handle()) SDL_GL_MakeCurrent(win_.Handle(), nullptr);
    current_thread_ = {};
}

void VideoStack::SetSessionOutput(std::function<void(std::vector<uint8_t>, int, int)> output,
                                  std::function<bool(std::function<void()>)> main_dispatch,
                                  std::function<bool(std::function<void()>)> main_post) {
    session_output_ = std::move(output);
    main_dispatch_ = std::move(main_dispatch);
    main_post_ = std::move(main_post);
    if (session_output_ && win_.Handle()) SDL_HideWindow(win_.Handle());
}

void VideoStack::PresentWindow() {
    if (!session_output_) { win_.Swap(); return; }
    // Under the display-rate re-view the window shows re-viewed frames only:
    // the tick's own present is held while a replay can show the frame (the
    // replay's present passes). Counted on the review either way.
    if (review_ && !review_->Replaying() && review_->TickPresentSuppressed()) return;
    // Preserve the renderer's composed display (crop, format conversion and
    // orientation), not a heuristic choice of an intermediate render target.
    // The game's regression grab has already completed before this callback.
    const int width = win_.Width(), height = win_.Height();
    if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * height * 4 > (64u << 20)) return;
    if (review_ && review_->Recording()) {
        // The re-view's presents (two per tick): the previous present's picture
        // is handed over now (its transfer had a present period on the GPU) and
        // this one's copy is queued -- the consumer does not wait for the GPU to
        // finish the frame's draws before it returns to the stream.
        WindowReadback& previous = readbacks_[readback_next_ ^ 1];
        if (previous.pending) DeliverWindowReadback(previous);
        QueueWindowReadback(readbacks_[readback_next_], width, height);
        readback_next_ ^= 1;
        return;
    }
    // The 60-rate path, unchanged: the window's back buffer read synchronously.
    // A transfer still queued from the re-view (the row just switched back to
    // 60) lands first, in order.
    for (WindowReadback& slot : readbacks_)
        if (slot.pending) DeliverWindowReadback(slot);
    ++readback_census_.sync;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    GLint read_buffer = 0;
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glReadBuffer(static_cast<GLenum>(read_buffer));
    session_output_(std::move(pixels), width, height);
}

void VideoStack::QueueWindowReadback(WindowReadback& slot, int w, int h) {
    const auto started = std::chrono::steady_clock::now();
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(w) * h * 4;
    if (!slot.pbo) glGenBuffers(1, &slot.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    // A fresh store every time (orphaning): the transfer never waits on the
    // previous hand-over's map of the same buffer.
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    GLint read_buffer = 0;
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);   // into the bound pack buffer
    glReadBuffer(static_cast<GLenum>(read_buffer));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);   // every later glReadPixels reads to client memory again
    if (slot.fence) glDeleteSync(static_cast<GLsync>(slot.fence));
    slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // No glFlush here: on the macOS GL driver it cost ~1.2 ms per present (a
    // partial sync -- smoke9 vs smoke10); the hand-over's wait carries the
    // flush bit, and the driver submits on its own as the queue fills.
    slot.w = w;
    slot.h = h;
    slot.pending = true;
    ++readback_census_.async;
    readback_census_.queue_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}

void VideoStack::DeliverWindowReadback(WindowReadback& slot) {
    const auto started = std::chrono::steady_clock::now();
    slot.pending = false;
    if (slot.fence) {
        // Issued a present period ago: normally signalled already. Otherwise the
        // hand-over waits (bounded, counted) rather than skipping the picture.
        GLenum state = glClientWaitSync(static_cast<GLsync>(slot.fence), 0, 0);
        if (state == GL_TIMEOUT_EXPIRED) {
            ++readback_census_.waited;
            state = glClientWaitSync(static_cast<GLsync>(slot.fence), GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
        }
        glDeleteSync(static_cast<GLsync>(slot.fence));
        slot.fence = nullptr;
        (void)state;
    }
    const size_t bytes = static_cast<size_t>(slot.w) * slot.h * 4;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
    if (!mapped) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        ++readback_census_.lost;
        return;
    }
    std::vector<uint8_t> pixels(static_cast<const uint8_t*>(mapped), static_cast<const uint8_t*>(mapped) + bytes);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    readback_census_.deliver_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    if (session_output_) session_output_(std::move(pixels), slot.w, slot.h);
}

void VideoStack::ReleaseWindowReadbacks() {
    for (WindowReadback& slot : readbacks_) {
        if (slot.fence) glDeleteSync(static_cast<GLsync>(slot.fence));
        if (slot.pbo) glDeleteBuffers(1, &slot.pbo);
        slot = WindowReadback{};
    }
    readback_next_ = 0;
}

void VideoStack::OnWindowResized(int w, int h) {
    if (!be_.IsInitialized() || w <= 0 || h <= 0) return;
    if (!win_.Headless()) {
        if (main_dispatch_) main_dispatch_([this, w, h] { win_.Resize(w, h); });
        else win_.Resize(w, h);
    }
    be_.SetWindowSize(w, h);
}

void VideoStack::OnWindowResizedOffMain(int w, int h) {
    if (!be_.IsInitialized() || w <= 0 || h <= 0) return;
    be_.SetWindowSize(w, h);
    if (win_.Headless()) return;
    // DEFER, never a round trip: the caller is the GS consumer thread, and the
    // main thread may at this moment be waiting for THAT thread in
    // DrawIrThreadQueue::Sync() (GraphicsOutput::SettleOwnedForCapture during
    // a restore). A synchronous main_dispatch_ here is the two-thread cycle --
    // the pipelined relaunch deadlock. The frame boundary applies the geometry.
    pending_resize_.store((static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) | static_cast<uint32_t>(h));
    deferred_resizes_++;
}

void VideoStack::ApplyPendingWindowResize() {
    const uint64_t packed = pending_resize_.exchange(0);
    if (packed == 0 || win_.Headless()) return;
    const int w = static_cast<int>(packed >> 32), h = static_cast<int>(packed & 0xFFFFFFFFu);
    // The boundary runs on the game's host thread: the process main thread in a
    // bootstrap leg (the dispatch then runs inline), the session's game worker
    // in play mode -- there the geometry is POSTED (main_post_), never waited
    // for: main may be inside a display swap for up to a second (a
    // display change never blocks the game thread), and this is the
    // boundary right after the swap's own window-size change.
    if (main_post_) main_post_([this, w, h] { win_.Resize(w, h); });
    else if (main_dispatch_) main_dispatch_([this, w, h] { win_.Resize(w, h); });
    else win_.Resize(w, h);
}

void VideoStack::Shutdown() {
    if (profile_ && be_.IsInitialized()) {
        EnsureCurrent();
        profile_->ReleaseBackendResources(surfaces_ ? static_cast<render::Backend&>(*surfaces_) : be_);
    }
    if (ctx_.IsCreated() && be_.IsInitialized()) {
        // The pack buffers go with the context. A transfer still in flight is
        // not handed over: the session that would receive it may already be
        // gone (the composition root drains output before video shutdown).
        EnsureCurrent();
        ReleaseWindowReadbacks();
    }
    // The composition root drains output and releases producer/queue leases
    // before video shutdown. Imported pins go before retained cache retirement.
    graphics_display_.reset();
    graphics_presentation_.reset();
    graphics_scene_.reset();
    graphics_renderer_.reset();
    resources_.reset();
    pipe_.reset();
    sequence_.reset();
    registry_.reset();
    presenter_.reset();
    scanout_.reset();
    resolver_.reset();
    refresher_.reset();
    cache_.reset();
    scale_.reset();
    profile_.reset();
    surfaces_.reset();
    review_.reset();
    vram_ = nullptr;
    session_output_ = {};
    main_dispatch_ = {};
    main_post_ = {};
    if (be_.IsInitialized()) be_.Shutdown();
    ctx_.Destroy();
    win_.Destroy();
}

}  // namespace ps2r::app
