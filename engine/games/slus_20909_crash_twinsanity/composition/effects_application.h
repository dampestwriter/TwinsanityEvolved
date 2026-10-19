#pragma once

#include "games/slus_20909_crash_twinsanity/composition/distortion_application.h"
#include "games/slus_20909_crash_twinsanity/composition/screen_application.h"
#include "games/slus_20909_crash_twinsanity/composition/shadow_application.h"

#include <array>
#include <atomic>
#include <memory>

namespace ps2r::app {

class VideoStack;

// One composition root for the effect applications (shadow bucket 21,
// distortion bucket 23, the bucket-4 screen effects): a single
// EffectsScene over the shared Graphics owner, the per-family default-off
// selections, and the install/register/checkpoint/census/stop sequence the
// bootstrap calls once. Services come from the root's
// GraphicsOutput and ordered consumer; nothing here owns a renderer or clock.
class EffectsApplication {
public:
    using Body = ::title::ShadowLibrary::Body;
    using Capture = ::title::ShadowLibrary::Capture;
    using Work = std::function<bool()>;
    // (address, body, selected): the root wraps unselected bodies with its
    // interior delegation before installing.
    using Installer = std::function<bool(uint32_t, Body, bool)>;
    struct Selection {
        bool shadows = false, distortion = false, screen = false;
    };
    struct Services {
        // Guest read over main RAM and the META arena (ShadowApplication::Output::read); empty = RAM only.
        ::title::MaterialState::Read read;
        std::function<bool(uint32_t, ::title::FrameEnvironment::SceneFrame&)> capture_scene; // checked bucket
        std::function<bool(::title::MaterialState::Environment&)> environment; // bucket-23 source environment
        std::function<bool(uint32_t, Work)> reserve; // bucket source predecessor now
        std::function<bool(Work)> ordered; // synchronous accepted render-owner fence
        std::function<bool(std::string*)> checkpoint; // registered shared surface owner
        // The ordered output's capture-time settle found an OPEN recording at
        // this boundary (GraphicsOutput::SettleOwnedForCapture + UnlinkedAtBoundary):
        // this frame's reserved pass is one of its unlinked groups. Optional.
        std::function<bool()> unlinked_open;
        // The dispatch's audited call-counting wrapper (DispatchTable::CountCalls):
        // the cloth producers' execution census (ClothProducers; run.meta
        // graphics_cloth_producer=). Optional: without it the row reads installed=0.
        std::function<Body(uint32_t, Body, std::shared_ptr<std::atomic<uint64_t>>)> count_calls;
    };
    using Preflight = ShadowApplication::Preflight;
    using AddPreflight = ShadowApplication::AddPreflight;
    // `video` may be null only when nothing is selected.
    EffectsApplication(MainMemory&, VideoStack* video, Selection, Services);
    ~EffectsApplication();
    bool Selected() const; // any family
    bool Install(const Capture&, const Installer&);
    bool Register(savestate::SectionRegistry&, const AddPreflight&);
    bool CanCheckpoint(std::string* error = nullptr) const;
    // Registers one quiescence condition per family through the callback.
    void RegisterConditions(const std::function<void(const char*, std::function<bool(std::string*)>)>&);
    void Record(verify::RunMeta&) const;
    bool StopAfterQuiescence(std::string* error = nullptr);
private:
    std::unique_ptr<::title::EffectsScene> scene_;
    std::unique_ptr<ShadowApplication> shadows_;
    std::unique_ptr<DistortionApplication> distortion_;
    std::unique_ptr<ScreenApplication> screen_;
    // The cloth producers' execution census (ClothProducers: 17 packet/update,
    // 1A packet/update) -- diagnostic counters, never a selection.
    std::function<Body(uint32_t, Body, std::shared_ptr<std::atomic<uint64_t>>)> count_calls_;
    std::array<std::shared_ptr<std::atomic<uint64_t>>, 4> cloth_calls_;
    bool cloth_census_installed_ = false;
};

}  // namespace ps2r::app
