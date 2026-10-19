#pragma once

#include <cstdint>

class LoaderSpinLift;
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Samples diagnostic counters before the IOP boundary pump advances its clock.
// This owner never reads or writes game state.
class FrameCensus {
public:
    uint64_t BeginFrame();
    void BindLift(const LoaderSpinLift& lift);
    void ObserveRpc(uint64_t calls);
    void ObserveCompletions(uint64_t count);
    void ObservePolls(uint64_t count);
    void ObserveInFrame(uint64_t offset);
    void Record(verify::RunMeta& meta, uint64_t service_us, uint64_t charged_us) const;

private:
    static uint64_t Bump(uint64_t now, uint64_t& last, uint64_t& maximum, uint64_t& sum);
    uint64_t frames_ = 0;
    uint64_t last_rpc_ = 0, rpc_max_ = 0, rpc_sum_ = 0;
    uint64_t inframe_max_ = 0, inframe_sum_ = 0;
    uint64_t last_done_ = 0, done_max_ = 0, done_sum_ = 0;
    uint64_t last_polls_ = 0, polls_max_ = 0, polls_sum_ = 0;
    const LoaderSpinLift* lift_ = nullptr;
    uint64_t last_rule_ = 0, lift_frames_ = 0, lift_rpc_max_ = 0, lift_rpc_sum_ = 0;
    uint64_t laps_max_ = 0, laps_sum_ = 0;
};

}  // namespace ps2r::app
