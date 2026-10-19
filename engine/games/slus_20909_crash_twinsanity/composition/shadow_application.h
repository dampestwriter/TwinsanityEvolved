#pragma once

#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_library.h"

class MainMemory;
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Composition and persistence for the complete source shadow producer. Output
// callbacks use the shared graphics owner, never a second renderer or clock.
class ShadowApplication {
public:
    using Body = ::title::ShadowLibrary::Body;
    using Capture = ::title::ShadowLibrary::Capture;
    using Installer = ::title::ShadowLibrary::Installer;
    using Work = std::function<bool()>;
    struct Output {
        // Guest read serving main RAM AND the META arena (the title's DiskManager
        // nodes live there under hle.meta_arena routing; a model's loaded bytes
        // are found through its node). Empty = main RAM only (MainMemory).
        ::title::MaterialState::Read read;
        std::function<bool(::title::ShadowPass::Frame&)> frame; // checked bucket 21
        std::function<bool(Work)> reserve; // bucket 21 source predecessor now
        std::function<bool(Work)> ordered; // synchronous accepted render-owner fence
        std::function<bool(std::string*)> checkpoint; // registered shared surface owner
        // The ordered output's capture-time settle found an OPEN recording at
        // this boundary (GraphicsOutput::SettleOwnedForCapture + UnlinkedAtBoundary):
        // this frame's reserved pass is one of its unlinked groups. Optional.
        std::function<bool()> unlinked_open;
    };
    using Preflight = std::function<bool(const std::vector<savestate::Blob>&, std::string*)>;
    using AddPreflight = std::function<void(Preflight)>;
    ShadowApplication(MainMemory&, ::title::EffectsScene*, bool selected, Output);
    ~ShadowApplication();
    bool Install(const Capture&, const Installer&);
    // The installer MUST run over the unfiltered image, after envelope/section
    // validation and before teardown/RAM writes. Root uses StateRestorer's hook.
    bool Register(savestate::SectionRegistry&, const AddPreflight&);
    bool CanCheckpoint(std::string* error = nullptr) const;
    bool Selected() const;
    // After game producers stop and GraphicsOutput invalidates/cancels queued
    // work, before its render thread/context stops. Never reports cancelled
    // batches as completed work.
    bool StopAfterQuiescence(std::string* error = nullptr);
    ::title::ShadowLibrary::Counters Counts() const;
    // The shared scene's shadow publication counts (passes drawn, passes with
    // no volume that published nothing); zero when unselected.
    ::title::EffectsScene::Counters SceneCounts() const;
    void Record(verify::RunMeta&) const;
    const ::title::GraphicsRefusals& Refusals() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ps2r::app
