// PadReadinessHook implementation; the hook is described in the header.
#include "games/slus_20909_crash_twinsanity/hle/pad_readiness_hook.h"

#include "engine/pad/hle/pad_protocol.h"

PadReadinessHook::PadReadinessHook(PadProtocol& protocol, uint32_t ctrl_sm_entry, uint32_t ctrl_object_ptr,
                                   ReadWord read, Now now)
    : protocol_(protocol), ctrl_sm_entry_(ctrl_sm_entry), ctrl_object_ptr_(ctrl_object_ptr), read_(std::move(read)),
      now_(std::move(now)) {}

bool PadReadinessHook::Install(const CaptureFn& capture, const InstallFn& install) {
    if (ctrl_sm_entry_ == 0 || !capture || !install) return false;
    Body original = capture(ctrl_sm_entry_);
    if (!original) return false;
    return install(ctrl_sm_entry_, [this, original](CpuContext& ctx, uint32_t addr) {
        Observe();
        original(ctx, addr);
    });
}

int PadReadinessHook::Observe() {
    ++entries_;
    const uint32_t ctrl = read_(ctrl_object_ptr_);
    if (!IsRam(ctrl)) return -1;
    const int sel = static_cast<int>((read_(ctrl + 4) >> kCaseShift) & kCaseMask);
    if (sel == static_cast<int>(kReadinessCase) && !raised_) {
        raised_ = true;
        protocol_.NoteReadinessEvent(now_ ? now_() : 0);
    }
    return sel;
}
