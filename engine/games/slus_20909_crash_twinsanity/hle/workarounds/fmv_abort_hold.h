#pragma once
// FmvAbortHold -- HOOK_RECLASSIFICATION section 3.1 row "FMV abort hold"
// (EMULATION-BUG-WORKAROUND, behavioural wrapper on the title's scene updater).
//
// The rule: the title's abort flag is a reaction to an impossible-on-hardware
// instant end of file. Its load-loop pump (FUN_002b0ae0) hands the movie file
// to sceMpegDemuxPss as fast as the refill callback delivers it; under instant
// CD delivery the callback reaches end of file while the player is still
// priming, returns < 0, and the pump latches the SceMpeg context's abort word
// (FmvContextLayout::abort_flag = 1). The scene updater (FUN_002af0d8) reads
// that word through FUN_002b0f68 on its next tick and tears the movie down
// after two served pictures. On retail the refill cannot outrun the ring the
// demux fills, so the end of file arrives with the last picture. While the
// engine still holds undelivered pictures (MpegApi::ShouldHoldAbort: a started
// movie that has not genuinely finished) the flag is cleared before the
// updater reads it -- the OLD observable the frozen baseline carries (its
// scene-updater wrapper cleared the same word each tick under the same
// predicate). Once the movie genuinely ends the predicate is false, the
// flag stands, and the real end-of-movie teardown happens.
//
// Guards (all must hold, each tick of the updater): the context handle word
// names a main-RAM context; the player mode word is the RAW8 movie path
// (layout.mode == layout.mode_raw8); the abort word is set; the engine says
// the movie has pictures left. Every clear is counted (run.meta
// fmv_abort_holds=).
//
// RETIREMENT GATE: a paced CD-delivery model (DeliveryMeter Modelled) that
// keeps the refill callback from reaching end of file before the movie ends;
// retire when the coldboot leg serves 201/346 pictures with the hold inert
// (fmv_abort_holds=0).
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/fmv_context_layout.h"
#include "engine/ipu/hle/guest_memory.h"

class FmvAbortHold {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t addr)>;          // the generated body at addr
    using Install = std::function<bool(uint32_t addr, Body body)>;
    // MpegApi::ShouldHoldAbort over the SceMpeg context address.
    using HoldPredicate = std::function<bool(uint32_t mpeg_addr)>;

    // Title data: the scene updater the hold wraps (every tick of the FMV
    // scene passes through it before the abort word is read) and the global
    // word holding the movie player's SceMpeg context handle.
    static constexpr uint32_t kSceneUpdater = 0x002AF0D8u;
    static constexpr uint32_t kContextHandleWord = 0x0030A700u;
    // The handle names a context only inside main RAM above the kernel area.
    static constexpr uint32_t kHandleMin = 0x00010000u;
    static constexpr uint32_t kHandleMax = 0x02000000u;

    FmvAbortHold(ps2r::ipu::GuestMemory& mem, const ps2r::ipu::FmvContextLayout& layout,
                 HoldPredicate should_hold);

    // One scene-updater tick: clears the abort word when every guard holds.
    // Returns true when a clear happened.
    bool Tick();

    // Wraps the generated scene-updater body (Tick before it) and installs the
    // wrapper; returns whether the install succeeded.
    bool InstallHook(const Capture& capture, const Install& install);
    // The wrapper body itself (exposed for the unit test): `orig` is the
    // generated body.
    Body Hook(Body orig);

    bool Installed() const { return installed_; }
    uint64_t Holds() const { return holds_; }      // abort words cleared
    uint64_t Ticks() const { return ticks_; }      // updater entries seen

private:
    ps2r::ipu::GuestMemory& mem_;
    ps2r::ipu::FmvContextLayout layout_;
    HoldPredicate should_hold_;
    bool installed_ = false;
    uint64_t holds_ = 0;
    uint64_t ticks_ = 0;
};
