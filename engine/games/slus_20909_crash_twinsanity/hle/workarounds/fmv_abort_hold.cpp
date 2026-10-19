#include "games/slus_20909_crash_twinsanity/hle/workarounds/fmv_abort_hold.h"

#include <utility>

FmvAbortHold::FmvAbortHold(ps2r::ipu::GuestMemory& mem, const ps2r::ipu::FmvContextLayout& layout,
                           HoldPredicate should_hold)
    : mem_(mem), layout_(layout), should_hold_(std::move(should_hold)) {}

bool FmvAbortHold::Tick() {
    ++ticks_;
    const uint32_t gctx = mem_.Read32(kContextHandleWord);
    if (gctx < kHandleMin || gctx >= kHandleMax) return false;          // no movie player context
    if (mem_.Read32(gctx + layout_.mode) != layout_.mode_raw8) return false;  // not the movie path
    if (mem_.Read32(gctx + layout_.abort_flag) == 0) return false;      // nothing to hold
    if (!should_hold_ || !should_hold_(gctx)) return false;             // the movie genuinely ended
    mem_.Write32(gctx + layout_.abort_flag, 0);
    ++holds_;
    return true;
}

FmvAbortHold::Body FmvAbortHold::Hook(Body orig) {
    return [this, orig](CpuContext& ctx, uint32_t ep) {
        Tick();   // before the updater reads the abort word
        if (orig) orig(ctx, ep);
    };
}

bool FmvAbortHold::InstallHook(const Capture& capture, const Install& install) {
    installed_ = install(kSceneUpdater, Hook(capture(kSceneUpdater)));
    return installed_;
}
