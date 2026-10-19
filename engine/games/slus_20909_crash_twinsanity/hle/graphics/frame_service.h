#pragma once

#include <memory>

#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_intent.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_clear.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/display_layout.h"

namespace title {

// Captured repeatable source display intent and its two submission boundaries.
// Other GIF send clients retain their own owner; source-object identity is
// matched without reading or rewriting a packet.
// A selected boundary whose source plan/target/environment is unavailable or
// whose owned recording the root refuses REFUSES: nothing was committed, the
// ORIGINAL boundary body runs (counted as its model call) and the refusal is
// attributed (detail codes below). A recording that THREW after the root took
// it is a fault: thrown, never replayed.
class FrameService {
public:
    using Body = FrameEnvironment::Body;
    using Capture = FrameEnvironment::Capture;
    using Installer = FrameEnvironment::Installer;
    static constexpr uint32_t kSend = 0x181968, kSendInterrupt = 0x1819e8;
    struct Hooks {
        bool enabled = false;
        MaterialState::Read read;
        std::function<bool(FrameEnvironment::SceneFrame&)> submitted;
        std::function<bool(std::shared_ptr<const FrameIntent::Output>,
                           const FrameEnvironment::SceneFrame&)> record;
        std::function<bool(FrameEnvironment::SceneFrame&)> setup = {};
        std::function<bool(std::shared_ptr<const FrameClear::Output>,
                           const FrameEnvironment::SceneFrame&)> clear = {};
        std::function<bool(const DisplayLayout::Output&)> layout = {};
        // Whether any primary Link has completed since reset (FrameEnvironment::
        // Linked). Optional: without it a Send with no linked scene refuses.
        std::function<bool()> linked = {};
        GraphicsRefusals::Sink refuse = {}, fault = {};
    };
    static constexpr uint32_t kInterior = 1, kLayoutUnsupported = 2, kLayoutRefused = 3,
                              kClearTarget = 4, kClearRefused = 5, kSetupUnsupported = 6,
                              kSendFrame = 7, kSendRefused = 8;
    struct Count { uint64_t calls = 0, replaced = 0, model_calls = 0; };
    struct Counters {
        Count setup, send, interrupt, clear, layout;
        uint64_t foreign_sends = 0, interior_calls = 0, failures = 0, refused = 0;
        // Sends completed as the owned display of NO scene: TR renderer.cpp:436-440
        // Present precedes StartNextFrame's SubmitBuckets (Link) in every
        // PresentFrame, so the title's first Send copies a drawn buffer no
        // scene was ever linked into; the owned display is already that reset state.
        uint64_t empty_sends = 0;
    };
    explicit FrameService(Hooks);
    bool Install(const Capture&, const Installer&);
    Counters Counts() const;
    // Portable source plan dimensions; no transport bytes or GPU handles.
    bool CapturePlan(uint32_t& width,uint32_t& height) const;
    bool RestorePlan(uint32_t width,uint32_t height);

private:
    void Run(size_t index,CpuContext&,uint32_t entry);
    void Refuse(GraphicsRefusals::Reason,uint32_t boundary,uint32_t detail);
    Hooks hooks_;
    std::array<Body,5> originals_;
    std::shared_ptr<const FrameIntent::Output> plan_;
    Counters counts_;
    bool installed_ = false, failed_ = false;
};
} // namespace title
