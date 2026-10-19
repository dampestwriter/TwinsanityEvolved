#pragma once

#include <cstdint>

#include "app/host/iop_arm.h"

class EmuDomain;
#if PS2R_IOP_ORACLE
namespace ps2r::iop { class IopClock; }
#endif
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Temporary composition adapter for remaining services. The owned file
// scheduler carries settled time; this adapter only reads the comparison
// owner's logical position and never advances its kernel/controller clock.
// `remaining` is the retained IOP arm's frame clock (the oracle binary) or
// null (the play binary): with no remaining services the clock sits at
// frame 0 / 0 us under the frame policy -- exactly the value the never-pumped
// kernel clock reports under hle.startup=native, so the play lanes are unchanged.
class ServiceTimeline {
public:
    static constexpr uint64_t kFrameUs = 16667;   // one NTSC frame quantum (IopClock::kFrameUs)
#if PS2R_IOP_ORACLE
    ServiceTimeline(bool selected, EmuDomain& domain, const iop::IopClock* remaining);
#else
    ServiceTimeline(bool selected, EmuDomain& domain);   // the play binary: no remaining services
#endif
    ~ServiceTimeline();
    bool Install();
    double ReadStartMs(uint64_t remaining_us) const;
    bool TimeLeft() const;
    uint64_t Sequence() const;
    void Record(verify::RunMeta& meta) const;

private:
    struct Remaining { uint64_t frames = 0, now_us = 0, ceiling_us = kFrameUs; bool frame_policy = true; };
    Remaining View() const;
    double ObservedMs(uint64_t remaining_us) const;
    bool selected_ = false, installed_ = false;
    EmuDomain& domain_;
#if PS2R_IOP_ORACLE
    const iop::IopClock* remaining_ = nullptr;
#endif
};
} // namespace ps2r::app
