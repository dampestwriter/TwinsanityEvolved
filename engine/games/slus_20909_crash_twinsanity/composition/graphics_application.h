#pragma once

#include "games/slus_20909_crash_twinsanity/composition/graphics_movie.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_frame.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_import.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_runtime.h"
#include "engine/gs/hle/present/aspect_selection.h"
#include "engine/render/texture_filter_selection.h"

#include <memory>

namespace ps2r::ipu { class LibmpegBindings; }
namespace title { class ScreenAspect; }
namespace ps2r::savestate { class Quiescence; }
namespace ps2r::app {

// video.aspect's enum onto the GS layer's shape (the same order; the GS layer
// includes no Config).
gs::AspectSelection::Shape ShapeOf(AspectSel selection);
// video.texture_filter's enum onto the render layer's mode (the same order; the
// render layer includes no Config).
render::TextureFilter FilterOf(TextureFilterSel selection);

// Complete application composition: selection, capture/install order, shared
// output, texture state and Movie's existing picture/audio provider.
class GraphicsApplication {
public:
    // The whole Config: the hle.* selection, and (A4) video.texture_pack under
    // paths.texture_packs (TexturePackPath) with the --dump-textures request.
    // `aspect` (video.aspect) is the leg's one display shape;
    // `has_window` says whether `auto` has a session window to follow. When the
    // selection is live (AspectSelection::Live) Install composes the title's
    // aspect model (ScreenAspect) over the four projection/culling/HUD-fit
    // entries and the Movie picture takes the ratio; a headless 4:3/auto leg
    // composes nothing -- the identity.
    GraphicsApplication(MainMemory&, ipu::GuestMemory&, ::title::MovieDecoder&, ipu::LibmpegBindings&,
                        GraphicsOutput&, VideoStack&, const Config&, std::function<void()> stop_producers,
                        std::shared_ptr<gs::AspectSelection> aspect = nullptr, bool has_window = false);
    ~GraphicsApplication();
    bool Install();
    // The Movie player owners (Start/WaitFrame/Stop) install after every Movie
    // service they depend on, including MovieStorage's two buffer loans. A
    // refusal is printed with its reason; the root never exits 2 silently.
    bool InstallMoviePlayer();
    // Host leg frame clock (e.g. VerifyHost::Boundaries) stamped on every
    // refusal/fault attribution of the Graphics families. Optional; may be set
    // before or after Install.
    void SetFrameClock(std::function<uint64_t()> host_frame);
    // The camera transform source: the title's world->camera
    // matrix after every RenderView::Update, on the game thread. Delivered only
    // while the aspect model is composed (a windowed session); a headless leg
    // has no presenter to feed. Call after Install.
    void SetCameraObserver(std::function<void(const float* to_camera16, const float* camera_to_screen16)> observer);
    // The explicit legacy import (GraphicsImport): a frozen anchor of the
    // title's inventory restores with the selected producers, its texture
    // owner derived from its own memory (never fabricated). Call after
    // Install and before RegisterState with the title's
    // restore_mhash_expected.tsv and the --load-state path; without a call
    // no legacy image is admitted (every Graphics section stays required).
    void ApproveLegacyImport(const std::string& inventory, const std::string& load_file);
    bool RegisterState(StateRestorer&);
    void RegisterConditions(savestate::Quiescence&);
    bool CanRestore(std::string* reason);
    void Record(verify::RunMeta&) const;
    void StopAfterQuiescence();

private:
    MainMemory& memory_;
    ipu::GuestMemory& guest_;
    ::title::MovieDecoder& decoder_;
    ipu::LibmpegBindings& mpeg_;
    GraphicsOutput& output_;
    VideoStack& video_;
    const HleConfig selection_;
    std::string pack_dir_, dump_dir_;
    std::function<void()> stop_producers_;
    std::function<uint64_t()> frame_clock_;
    bool stopped_ = false;
    std::unique_ptr<GraphicsRuntime> runtime_;
    std::unique_ptr<GraphicsImport> import_;
    std::unique_ptr<GraphicsMovie> movie_;
    std::unique_ptr<GraphicsFrame> frame_;
    std::shared_ptr<gs::AspectSelection> aspect_;
    bool aspect_live_ = false;
    std::unique_ptr<::title::ScreenAspect> screen_aspect_;
};

}  // namespace ps2r::app
