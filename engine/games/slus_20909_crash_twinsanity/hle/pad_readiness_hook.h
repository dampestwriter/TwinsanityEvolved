#pragma once
// PadReadinessHook -- the title hook that raises PadProtocol's readiness event:
// the boot UI's controller state machine (TitleProfile::Pad::ctrl_sm_entry)
// enters case 2 to wait for the pad's ready bit, and PADMAN's READY comes two
// vsyncs after that entry (PadProtocol::kReadyDelayAfterEvent). The case is
// bits 12..17 of the word at +4 of the controller object
// (*TitleProfile::Pad::ctrl_object_ptr); the first case-2 entry is the event,
// later entries are not.
//
// Installed by the composition root over the generated body (Capture /
// Install, as the workaround hooks are); the original body always runs.
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"

class PadProtocol;

class PadReadinessHook {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using CaptureFn = std::function<Body(uint32_t addr)>;
    using InstallFn = std::function<bool(uint32_t addr, Body body)>;
    using ReadWord = std::function<uint32_t(uint32_t addr)>;
    using Now = std::function<uint64_t()>;

    static constexpr uint32_t kCaseShift = 12;
    static constexpr uint32_t kCaseMask = 0x3F;
    static constexpr uint32_t kReadinessCase = 2;

    PadReadinessHook(PadProtocol& protocol, uint32_t ctrl_sm_entry, uint32_t ctrl_object_ptr, ReadWord read, Now now);

    // Wraps the body at ctrl_sm_entry; false when nothing is installed
    // (address 0 or no captured body).
    bool Install(const CaptureFn& capture, const InstallFn& install);

    // The hook's own step (exposed for the test): inspects the object and
    // raises the event on the first case-2 entry. Returns the case seen, or
    // -1 when the object pointer is not a RAM address.
    int Observe();

    bool EventRaised() const { return raised_; }
    uint64_t Entries() const { return entries_; }

private:
    static bool IsRam(uint32_t a) { return a >= 0x10000u && a < 0x02000000u; }

    PadProtocol& protocol_;
    uint32_t ctrl_sm_entry_;
    uint32_t ctrl_object_ptr_;
    ReadWord read_;
    Now now_;
    bool raised_ = false;
    uint64_t entries_ = 0;
};
