#pragma once

#include <string>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_startup.h"

namespace ps2r::verify { class RunMeta; }
namespace ps2r::app {

// The startup owner's reading of the services owner's contract
// (`title::ServiceStartup`, the startup ownership
// contract): which of the title's retained IOP modules and the IOP heap still
// have a consumer under the composed selection. It decides nothing by itself.
// A retained module keeps executing exactly as today; the two booleans say
// when the startup owner may stop executing a load or InitIopHeap, and the
// consumer list is the explainable refusal a live restore names meanwhile.
class StartupOwnership {
public:
    using Report = ::title::ServiceStartup::Report;
    using Module = ::title::ServiceStartup::Module;

    explicit StartupOwnership(Report report);

    const Report& Contract() const { return report_; }
    // STREAM.IRX, CDVDSTM.IRX and the IOP heap have no consumer.
    bool StreamOwned() const { return report_.StreamStartupOwned(); }
    // LIBSD.IRX and SDRDRV.IRX have no consumer.
    bool SoundOwned() const { return report_.SoundStartupOwned(); }
    // Service-only startup: every module and the heap released.
    bool Owned() const { return StreamOwned() && SoundOwned(); }

    // The live-restore refusal while startup is still retained: the retained
    // modules and every consumer with its title root, one line. Empty when
    // Owned(). Never a generic sentence.
    std::string Refusal() const;
    // run.meta `service_startup=` (stream/sound native or retained, the retained
    // module names, the consumer count): selection-aware, read by the parent's
    // pair next to `irx_boot`.
    std::string Census() const;
    void Record(verify::RunMeta& meta) const;

private:
    Report report_;
};

}  // namespace ps2r::app
