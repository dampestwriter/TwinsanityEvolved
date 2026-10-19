#include "games/slus_20909_crash_twinsanity/composition/frame_census.h"

#include <cstdio>
#include <string>

#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_spin_lift.h"

namespace ps2r::app {

uint64_t FrameCensus::BeginFrame() { return ++frames_; }
void FrameCensus::BindLift(const LoaderSpinLift& lift) { lift_ = &lift; }

uint64_t FrameCensus::Bump(uint64_t now, uint64_t& last, uint64_t& maximum, uint64_t& sum) {
    const uint64_t delta = now >= last ? now - last : 0;
    last = now;
    if (delta > maximum) maximum = delta;
    sum += delta;
    return delta;
}

void FrameCensus::ObserveRpc(uint64_t calls) {
    const uint64_t rpc_this_frame = Bump(calls, last_rpc_, rpc_max_, rpc_sum_);
    if (!lift_) return;
    const uint64_t rule = lift_->RpcLaps() + lift_->LiftsEdge();
    const uint64_t delta = rule >= last_rule_ ? rule - last_rule_ : 0;
    last_rule_ = rule;
    if (delta == 0) return;
    ++lift_frames_;
    if (rpc_this_frame > lift_rpc_max_) lift_rpc_max_ = rpc_this_frame;
    lift_rpc_sum_ += rpc_this_frame;
    if (delta > laps_max_) laps_max_ = delta;
    laps_sum_ += delta;
}

void FrameCensus::ObserveCompletions(uint64_t count) { Bump(count, last_done_, done_max_, done_sum_); }
void FrameCensus::ObservePolls(uint64_t count) { Bump(count, last_polls_, polls_max_, polls_sum_); }
void FrameCensus::ObserveInFrame(uint64_t offset) {
    if (offset > inframe_max_) inframe_max_ = offset;
    inframe_sum_ += offset;
}

void FrameCensus::Record(verify::RunMeta& meta, uint64_t service_us, uint64_t charged_us) const {
    auto mean = [](uint64_t maximum, uint64_t sum, uint64_t count) {
        char buffer[64];
        const double value = count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0;
        std::snprintf(buffer, sizeof buffer, "%llu,%.2f", static_cast<unsigned long long>(maximum), value);
        return std::string(buffer);
    };
    meta.Set("stream_rpc_per_frame", mean(rpc_max_, rpc_sum_, frames_));
    meta.Set("stream_rpc_per_lift_frame",
             mean(lift_rpc_max_, lift_rpc_sum_, lift_frames_) + "," + std::to_string(lift_frames_));
    meta.Set("stream_reply_laps_per_frame", mean(laps_max_, laps_sum_, lift_frames_));
    meta.Set("iop_inframe_us", mean(inframe_max_, inframe_sum_, frames_));
    meta.Set("cdvd_completions_per_frame", mean(done_max_, done_sum_, frames_));
    meta.Set("loader_polls_per_frame", mean(polls_max_, polls_sum_, frames_));
    meta.Set("stream_rpc_service", std::to_string(service_us) + "us," + std::to_string(charged_us));
}

}  // namespace ps2r::app
