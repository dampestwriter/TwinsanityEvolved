#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// The bucket-4 screen-effects source boundary: DrawScreenEffects (NTSC
// 0x1b92d8, TR renderer/effects.cpp:268), called once per scene from
// FinishScene(effects). It reads the three effect switches, frees the texture
// slots (keepTime 1), copies the depth buffer and runs Blur, FilterColours and
// WaveFrame in that order.
//
// Model arm: every call is forwarded whole and counted. Selected arm: the
// complete call parameters, the filter palette FilterColours would upload and
// the wave shader's rows are read at the boundary, the frame's bucket-4 slot
// is reserved through the root (RecordOwned), the source's keepTime-1
// FreeTextureSlots is preserved by forwarding that captured body with the
// source arguments, and the model body never runs. A call that cannot be
// proved before anything is committed (unreadable parameters, an unsupported
// palette, unreadable waves, no completed scene frame, a declined reservation,
// a faulted consumer) REFUSES to the model: the original body runs whole and
// `model_calls`/`refused` attribute it.
class ScreenPass {
public:
    using Read = MaterialState::Read;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    using Frame = FrameEnvironment::SceneFrame;
    static constexpr uint32_t kDraw = 0x001b92d8;
    // The title's constant $gp; the effect globals are gp-relative in NTSC.
    static constexpr uint32_t kGlobalPointer = 0x003114f0;
    static constexpr uint32_t kBlurSwitch = 0x003098b0, kFilterSwitch = 0x003098b1, kWaveSwitch = 0x003098b2;
    static constexpr uint32_t kFilterBlends = 0x00309892, kFilterAmount = 0x00309894;
    static constexpr uint32_t kFilterPalette = 0x00309898, kFilterSecond = 0x0030989c;
    // g_ScreenWidth/g_ScreenHeight (TR effects.cpp), distinct from the display extent.
    static constexpr uint32_t kScreenWidth = 0x00309744, kScreenHeight = 0x00309748;
    static constexpr uint32_t kTextureContext = 0x0030a4a0;
    static constexpr uint32_t kFreeTextureSlots = 0x001c0778, kCopyDepth = 0x001af788;
    static constexpr uint32_t kBlur = 0x001afca8, kFilter = 0x001ae698, kWave = 0x001aef48;
    // g_ParticleWaveShader (type 1C, NTSC; TR renderer.h WaveShader): the
    // waves pointer at +0x170 (16 quadwords of phase rows) and the amplitude
    // at +0x178, the values WaveFrame hands VU1 by REF.
    static constexpr uint32_t kWaveShader = 0x00332040;
    static constexpr uint32_t kWaveRows = 0x170, kWaveAmplitude = 0x178;
    struct Parameters {
        bool blur = false, filter = false, wave = false;
        uint32_t width = 0, height = 0;
        uint32_t base = 0; // half-size buffer block base: (width*height*10) >> 8
        uint32_t palette = 0, second = 0;
        bool blends = false;
        float amount = 0;
        bool Any() const { return blur || filter || wave; }
    };
    enum class Result { Ok, InvalidMemory, UnsupportedSource };
    // Source values at the call. Refuses a foreign $gp or an extent the source
    // screen effects cannot draw (they always address 512 pixels across).
    static Result ReadParameters(const Read&, const CpuContext&, Parameters&);
    static Result ReadWaves(const Read&, WaveGeometry::Waves&);
    // One reserved owned frame. The consumer checks Ready(), runs
    // EffectsScene::Screen(frame, work) and calls Finish exactly once.
    //
    // Unlike a shadow bracket there is no Building half: the call's parameters
    // are complete at the boundary, so the batch is Ready the moment it is
    // reserved and stays so until the ordered consumer delivers it (Finish) or
    // the root releases the reservation unexecuted (Cancel).
    class Batch {
    public:
        Frame frame;
        EffectsScene::ScreenFrame work;
        bool Ready() const { return state_.load(std::memory_order_acquire) == State::Ready; }
        bool Complete() const { return !Ready(); }
        bool Failed() const { return state_.load(std::memory_order_acquire) == State::Failed; }
        // Ordered consumer, exactly once. Failure is sticky on the pass.
        bool Finish(bool success) const;
        // Released unexecuted (GraphicsOutput::CancelOwned after a
        // GraphicsBridge::Discard dropped the closure): no credit, no fault; the
        // batch is complete for the checkpoint. A no-op after Finish.
        bool Cancel() const;
    private:
        friend class ScreenPass;
        enum class State : int { Ready, Complete, Failed, Cancelled };
        mutable std::atomic<State> state_{State::Ready};
        std::shared_ptr<std::atomic<uint64_t>> failed_;
    };
    enum class Refusal : uint8_t { None, Unreadable, Palette, Waves, Frame, Extent, Reserve, Faulted };
    struct Hooks {
        bool enabled = false;
        std::function<bool(Frame&)> frame;                           // checked bucket-4 scene frame
        std::function<bool(std::shared_ptr<const Batch>)> reserve;  // bucket-4 source predecessor now
    };
    struct Counters {
        uint64_t calls = 0, empty = 0, effects = 0, model_calls = 0, interior = 0;
        uint64_t blur = 0, filter = 0, wave = 0, unreadable = 0;
        // Selected arm: frames the owned pass drew (or that draw nothing
        // visible: filter switched on but g_ColourFilterOn clear), refusals to
        // the model by class, failed deliveries.
        uint64_t replaced = 0, filter_off = 0, refused = 0, faults = 0;
        Refusal last_refusal = Refusal::None;
        // The last owned frame's source values (the filter palette index the
        // chunk selected, the blend flag and the screen height).
        uint32_t last_palette = 0, last_height = 0;
        bool last_blends = false;
    };
    static const char* Name(Refusal);
    explicit ScreenPass(Read);
    ScreenPass(Read, Hooks);
    bool Install(const Capture&, const Installer&);
    void Run(CpuContext&, uint32_t entry);
    bool Selected() const { return hooks_.enabled; }
    Counters Counts() const;
    // `unlinked_recording_open`: the ordered output's capture-time settle
    // (Graphics INTERFACES section 12) found an OPEN recording at this
    // boundary -- the title's frame boundary is Present -> SyncV -> StartNextFrame
    // -> Link, and at SyncV this frame's reserved bucket-4 slot is one of the
    // recording's unlinked items (GXPD), delivered once the pass completes.
    // Such a boundary admits every reserved frame (mirrors ShadowPass /
    // DistortionPass::CanCheckpoint's Ready arm). A closed boundary (false)
    // needs them Complete. Never while a body is executing.
    bool CanCheckpoint(bool unlinked_recording_open = false) const;
    // Explicit abandoned-frame/reset after the root output fence, never an
    // absent-payload restore operation (the pass has no state section).
    void ResetAfterQuiescence();
private:
    void Refuse(CpuContext&, uint32_t entry, Refusal);
    void Collect();
    Read read_;
    Hooks hooks_;
    Body original_, free_slots_;
    Counters counts_;
    uint32_t active_ = 0;
    bool installed_ = false;
    // Reserved frames not yet seen Complete (delivered, failed or cancelled).
    std::vector<std::shared_ptr<Batch>> pending_;
    // Failed deliveries (Batch::Finish(false) on the consumer); nonzero makes
    // every later call refuse to the model.
    std::shared_ptr<std::atomic<uint64_t>> failed_ = std::make_shared<std::atomic<uint64_t>>(0);
};

}  // namespace title
