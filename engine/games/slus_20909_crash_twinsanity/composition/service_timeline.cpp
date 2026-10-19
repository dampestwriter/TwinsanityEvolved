#include "games/slus_20909_crash_twinsanity/composition/service_timeline.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "engine/core/verify/run_meta.h"
#include "engine/ee/timing/emu_domain.h"
#if PS2R_IOP_ORACLE
#include "engine/iop/iop_clock.h"
#endif
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace ps2r::app {
#if PS2R_IOP_ORACLE
ServiceTimeline::ServiceTimeline(bool selected, EmuDomain& domain, const iop::IopClock* remaining)
    : selected_(selected), domain_(domain), remaining_(remaining) {}
#else
ServiceTimeline::ServiceTimeline(bool selected, EmuDomain& domain) : selected_(selected), domain_(domain) {}
#endif
ServiceTimeline::Remaining ServiceTimeline::View() const {
    Remaining r;
#if PS2R_IOP_ORACLE
    if (remaining_) {
        static_assert(iop::IopClock::kFrameUs == kFrameUs);
        r.frames = remaining_->Frames();
        r.now_us = remaining_->NowUs();
        r.ceiling_us = remaining_->FrameCeilingUs();
        r.frame_policy = remaining_->GetPolicy() == iop::IopClock::Policy::Frame;
    }
#endif
    return r;
}
ServiceTimeline::~ServiceTimeline() {
    if (installed_) if (auto* files = StreamLibrary::Scheduling()) {
        files->Reset(); // workers joined; consumers still alive to cancel old tickets
        files->SetServiceTime(false); // detach this observer before its clocks die
    }
}
double ServiceTimeline::ObservedMs(uint64_t remaining_us) const {
    const auto base = View().frames * kFrameUs;
    const auto offset = remaining_us > base ? remaining_us - base : 0;
    const double frame_ms = static_cast<double>(domain_.DomainTick()) * ReadTiming::kTickMs;
    return frame_ms + static_cast<double>(offset) / 1000.0;
}
bool ServiceTimeline::Install() {
    if (!selected_) return true;
    auto* files = StreamLibrary::Scheduling();
    if (installed_ || !files || !View().frame_policy) {
        std::fprintf(stderr, "service_time requires selected files and logical remaining services\n");
        return false;
    }
    installed_ = files->SetServiceTime(true, [this] { return ObservedMs(View().now_us); });
    return installed_;
}
double ServiceTimeline::ReadStartMs(uint64_t remaining_us) const {
    const auto observed = ObservedMs(remaining_us);
    const auto* files = installed_ ? StreamLibrary::Scheduling() : nullptr;
    return files ? std::max(observed, files->ServiceInstantMs()) : observed;
}
bool ServiceTimeline::TimeLeft() const {
    const auto* files = installed_ ? StreamLibrary::Scheduling() : nullptr;
    const Remaining r = View();
    return files ? files->ServiceHasHeadroom() : r.now_us < r.ceiling_us;
}
uint64_t ServiceTimeline::Sequence() const {
    const auto* files = installed_ ? StreamLibrary::Scheduling() : nullptr;
    return files ? files->ServiceSequence() : 0;
}
void ServiceTimeline::Record(verify::RunMeta& meta) const {
    const auto* files = installed_ ? StreamLibrary::Scheduling() : nullptr;
    meta.Set("service_time_binding", selected_ ? "on" : "off");
    meta.Set("service_timeline", "installed=" + std::to_string(installed_) +
        ",now_ms=" + std::to_string(files ? files->ServiceInstantMs() : ObservedMs(View().now_us)) +
        ",sequence=" + std::to_string(Sequence()) + ",headroom=" + std::to_string(TimeLeft()));
}
} // namespace ps2r::app
