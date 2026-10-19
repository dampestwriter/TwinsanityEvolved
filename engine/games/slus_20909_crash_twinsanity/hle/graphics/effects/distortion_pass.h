#pragma once

#include <atomic>
#include <map>
#include <memory>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_blocks.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_material.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"

namespace title {

// Complete source distortion producer for bucket 23. Each source hexagon
// block draw (DrawParticleList -> DrawHexagonParticleBlock) appends owned
// geometry to one material batch per frame; the batch becomes readable at the
// source ForgetMaterials boundary, where the original material writer would
// have been flushed. The consumer performs exactly one scene copy followed by
// every ordered hexagon batch (EffectsScene::Distortion).
class DistortionPass {
public:
    using Frame = FrameEnvironment::SceneFrame;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    static constexpr uint32_t kDraw = 0x001b03f0, kForget = 0x001c0410;
    static constexpr uint32_t kMaterial = 0x003703d8, kBucket = 23;
    static constexpr uint32_t kWriterFirst = 0x28, kTableCorners = 0x40, kTableSteps = 0x70;
    struct Batch {
        const Frame frame;
        const MaterialState::Environment environment;
        Batch(Frame value, MaterialState::Environment env) : frame(value), environment(env) {}
        bool Ready() const;
        bool Complete() const;
        bool Failed() const;
        const std::vector<ps2r::gs::DrawIr>& Draws() const;
        // Ordered consumer, exactly once, after Ready. Failure is sticky.
        bool Finish(bool success) const;
    private:
        friend class DistortionPass;
        enum class State { Building, Ready, Complete, Failed };
        mutable std::atomic<State> state_{State::Building};
        std::vector<ps2r::gs::DrawIr> draws_;
    };
    struct Hooks {
        bool enabled = false;
        MaterialState::Read read;
        // The settings version captured at the SOURCE settings call for THIS
        // shader object (DistortionSettings::Current); the constructor's later
        // filter/K/L writes never reached it. False: the live material was
        // never observed, so the selected pass refuses it.
        std::function<bool(uint32_t shader, DistortionMaterial::Settings&)> settings;
        std::function<bool(MaterialState::Environment&)> environment;
        // The source-frame owner for bucket 23, at the material's insertion.
        std::function<bool(Frame&)> frame;
        // Reserve bucket 23's exact source position now. The consumer checks
        // Ready, runs EffectsScene::Distortion(frame, environment, Draws()) and
        // calls Finish once. False/throw leaves the output queue unchanged.
        std::function<bool(std::shared_ptr<const Batch>)> reserve;
    };
    struct Counters {
        uint64_t calls = 0, replaced = 0, model_calls = 0, interior = 0;
        uint64_t forgets = 0, batches = 0, blocks = 0, draws = 0, completed = 0, faults = 0;
        // Selected frames refused to the model at their first draw, BEFORE any
        // reservation: the frame contract (bucket-23 frame/environment) was not
        // available, the model already owned the material's writer, the source
        // frame values/dimensions or settings version were not provable, or the
        // root declined the source-position reservation (RecordOwned false). Every
        // draw of such a frame runs the model (counted in model_calls) until
        // the source's ForgetMaterials. Attributable census, never a fault.
        uint64_t refused = 0;
    };
    DistortionPass(DistortionBlocks&, Hooks);
    bool Install(const Capture&, const Installer&);
    void Run(uint32_t boundary, CpuContext&, uint32_t entry);
    Counters Counts() const;
    // `unlinked_recording_open`: the ordered output's capture-time settle
    // admits this frame's closed pass (Ready batches behind their bucket-23
    // predecessor, not yet Linked); a closed boundary needs them Complete.
    bool CanCheckpoint(bool unlinked_recording_open = false) const;
    // Explicit abandoned-frame/reset after the root output fence; never an
    // absent-payload restore. Does not reset DistortionBlocks.
    void ResetAfterQuiescence();
private:
    DistortionBlocks& blocks_;
    Hooks hooks_;
    std::map<uint32_t, Body> originals_;
    std::shared_ptr<Batch> open_;
    ps2r::gs::DrawIr material_;
    std::array<uint32_t, 8> target_{};
    std::vector<std::shared_ptr<Batch>> pending_;
    Counters counts_;
    bool installed_ = false, faulted_ = false, refusing_ = false;
    // False: refused before any reservation (the caller runs the model).
    bool Draw(const CpuContext&);
    bool Open(uint32_t material);
    void Forget();
    void Collect();
    [[noreturn]] void Fault(const char*);
};

}  // namespace title
