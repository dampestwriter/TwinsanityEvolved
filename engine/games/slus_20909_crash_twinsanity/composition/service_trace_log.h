#pragma once

#include <fstream>
#include <memory>
#include <string>

#include "app/host/iop_arm.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_trace.h"

class DeliveryMeter;
class EmuDomain;
#if PS2R_IOP_ORACLE
namespace ps2r::iop { class IopModuleTable; }
#endif
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Bounded harness evidence over the already selected service owners. This
// does not choose a service, advance a clock, or keep a second request queue.
// `table` is the retained IOP arm's module table (the oracle binary) or null
// (the play binary): the position's service_us / service_frame_us /
// replies are then 0 -- the values the never-pumped kernel reports under
// hle.startup=native, so a play leg's services.log is unchanged.
class ServiceTraceLog {
public:
    ~ServiceTraceLog();
#if PS2R_IOP_ORACLE
    bool Install(const std::string& path, EmuDomain& domain,
                 iop::IopModuleTable* table, DeliveryMeter& reads);
#else
    bool Install(const std::string& path, EmuDomain& domain, DeliveryMeter& reads);
#endif
    void Record(verify::RunMeta& meta);

private:
    std::ofstream log_;
    std::unique_ptr<title::ServiceTrace> trace_;
    uint64_t lines_ = 0, write_failures_ = 0;
};
} // namespace ps2r::app
