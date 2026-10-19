#pragma once

#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_library.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"

class MainMemory;
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Composition and persistence for the complete source distortion producer
// (bucket 23). Output callbacks use the shared graphics owner, never a second
// renderer or clock; the shape mirrors ShadowApplication.
class DistortionApplication {
public:
    using Body = ::title::DistortionLibrary::Body;
    using Capture = ::title::DistortionLibrary::Capture;
    using Installer = ::title::DistortionLibrary::Installer;
    using Work = std::function<bool()>;
    struct Output {
        std::function<bool(::title::MaterialState::Environment&)> environment; // source environment now
        std::function<bool(::title::DistortionPass::Frame&)> frame; // checked bucket 23
        std::function<bool(Work)> reserve; // bucket 23 source predecessor now
        std::function<bool(Work)> ordered; // synchronous accepted render-owner fence
        std::function<bool(std::string*)> checkpoint; // registered shared surface owner
        // The ordered output's capture-time settle found an OPEN recording at
        // this boundary (GraphicsOutput::SettleOwnedForCapture + UnlinkedAtBoundary):
        // this frame's reserved pass is one of its unlinked groups. Optional.
        std::function<bool()> unlinked_open;
    };
    using Preflight = std::function<bool(const std::vector<savestate::Blob>&, std::string*)>;
    using AddPreflight = std::function<void(Preflight)>;
    DistortionApplication(MainMemory&, ::title::EffectsScene*, bool selected, Output);
    ~DistortionApplication();
    bool Install(const Capture&, const Installer&);
    // The installer MUST run over the unfiltered image, after envelope/section
    // validation and before teardown/RAM writes. Root uses StateRestorer's hook.
    bool Register(savestate::SectionRegistry&, const AddPreflight&);
    bool CanCheckpoint(std::string* error = nullptr) const;
    bool Selected() const;
    // After game producers stop and GraphicsOutput invalidates/cancels queued
    // work, before its render thread/context stops.
    bool StopAfterQuiescence(std::string* error = nullptr);
    ::title::DistortionLibrary::Counters Counts() const;
    void Record(verify::RunMeta&) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ps2r::app
