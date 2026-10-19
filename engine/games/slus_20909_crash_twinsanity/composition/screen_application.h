#pragma once

#include <functional>
#include <memory>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_pass.h"

class MainMemory;
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Composition of the bucket-4 screen-effects owner (DrawScreenEffects
// 0x1b92d8). Model arm: the boundary census, every call forwarded whole.
// Selected arm (hle.graphics_screen): the call's parameters, palette and wave
// rows are read from the root's RAM at the boundary, the frame's bucket-4
// slot is reserved through the root and the owned pass (EffectsScene::Screen)
// draws it on the ordered render owner; the model body never runs for an
// owned frame. The pass keeps no state across frames: the checkpoint
// conditions are "no body executing" and "no reserved frame undelivered at a
// closed boundary" (the settle's open recording admits the reserved frame).
class ScreenApplication {
public:
    using Body = ::title::ScreenPass::Body;
    using Capture = ::title::ScreenPass::Capture;
    using Installer = ::title::ScreenPass::Installer;
    using Work = std::function<bool()>;
    struct Output {
        std::function<bool(::title::ScreenPass::Frame&)> frame; // checked bucket-4 scene frame
        std::function<bool(Work)> reserve;                      // bucket-4 source predecessor now
        std::function<bool(Work)> ordered;                      // synchronous accepted render-owner fence
        std::function<bool(std::string*)> checkpoint;           // registered shared surface owner (the settle)
        // The ordered output's capture-time settle found an OPEN recording at
        // this boundary (GraphicsOutput::SettleOwnedForCapture + UnlinkedAtBoundary):
        // this frame's reserved pass is one of its unlinked groups. Optional.
        std::function<bool()> unlinked_open;
    };
    ScreenApplication(MainMemory&, ::title::EffectsScene*, bool selected, Output);
    ~ScreenApplication();
    bool Install(const Capture&, const Installer&);
    bool Selected() const;
    // False while the boundary body is executing, or while a reserved frame
    // awaits its consumer at a CLOSED boundary (a checkpoint there would lose
    // the owned frame). The shared owner settles first: with an open recording
    // this frame's reserved slot is one of its unlinked items (GXPD) and the
    // boundary is admitted, as for the shadow and distortion passes.
    bool CanCheckpoint(std::string* error = nullptr) const;
    // After game producers stop and GraphicsOutput invalidates/cancels queued
    // work, before its render thread/context stops.
    bool StopAfterQuiescence(std::string* error = nullptr);
    ::title::ScreenPass::Counters Counts() const;
    void Record(verify::RunMeta&) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ps2r::app
