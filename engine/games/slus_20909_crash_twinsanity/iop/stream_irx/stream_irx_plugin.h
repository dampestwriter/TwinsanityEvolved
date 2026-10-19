// games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h --
// StreamIrxPlugin: hosts the recompiled STREAM.IRX under the IOP kernel. It owns
// the HLE module instances, binds the StubSurface behind the generated TU,
// resolves guest addresses through iop_recompiled_functions, runs the loader
// (sections -> entry -> OnModuleLoaded: module init to its first block), emits
// the module-table rows of the post-reboot IOPRP set + the six idle disc IRXs,
// installs the LTI protocol as the CD-read RPC body, and gives the savestate
// its boot-symmetric re-park (IOPM after_restore).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "engine/iop/hle/cdvdman/cdvdman_module.h"
#include "engine/iop/hle/intrman/intrman_module.h"
#include "engine/iop/hle/ioman/ioman_module.h"
#include "engine/iop/hle/libsd/libsd_module.h"
#include "engine/sound/spu_core.h"
#include "engine/iop/hle/sifcmd/sifcmd_module.h"
#include "engine/iop/hle/sifman/sifman_module.h"
#include "engine/iop/hle/stdio/stdio_module.h"
#include "engine/iop/hle/sysclib/sysclib_module.h"
#include "engine/iop/hle/sysmem/sysmem_module.h"
#include "engine/iop/hle/thbase/thbase_module.h"
#include "engine/iop/hle/thsemap/thsemap_module.h"
#include "engine/iop/hle/timrman/timrman_module.h"
#include "engine/iop/iop_loader.h"
#include "engine/iop/iop_memory.h"
#include "engine/iop/iop_module_table.h"
#include "engine/iop/savestate/iopm_section.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/lti_protocol.h"

namespace ps2r::title::stream_irx {

class StreamIrxPlugin {
public:
    struct BootReport {
        iop::IopLoader::SectionReport sections;
        bool entry_ran = false;
        int rpc_tid = 0;
        std::string error;
    };

    // `table` outlives the plugin; `reader`/`sink` may be null until 4D/4C bind them.
    StreamIrxPlugin(iop::IopModuleTable& table, iop::IopMemory& mem, sound::SpuCore& spu,
                    cdvd::CdvdReader* reader, iop::hle::EeRamSink* sink);
    ~StreamIrxPlugin();
    StreamIrxPlugin(const StreamIrxPlugin&) = delete;
    StreamIrxPlugin& operator=(const StreamIrxPlugin&) = delete;

    // Sections -> entry -> module init to its first block (OnModuleLoaded).
    BootReport Boot(const uint8_t* irx, size_t size);

    // The IOPM section's deps: heap_ptr -> sysmem, libsd pending sequence reset, the
    // CD callback, the image's thread table re-created over the boot-path park
    // (a thread the image records as started is running: re-armed from its entry and
    // dispatched to its first park before the EE continues), and the heartbeat hard
    // timer re-armed from the module's descriptor words (a timer the image records
    // as started beats after the restore).
    iop::IopmSection::Deps IopmDeps(std::function<uint64_t()> restored_vf);
    void BootSymmetricRepark(const iop::IopmSection::Statics& s);
    iop::IopmSection::Statics& Statics() { return statics_; }

    static iop::IopLoader::GuestFunction Resolve(uint32_t addr);
    static constexpr size_t kModuleRows = 22 + 1 + 6 + 1;   // IOPRP300 set + libsd + the idle disc IRXs + STREAM.IRX

    iop::hle::SysmemModule& Sysmem() { return sysmem_; }
    iop::hle::LibsdModule& Libsd() { return libsd_; }
    iop::hle::CdvdmanModule& Cdvdman() { return cdvdman_; }
    iop::hle::TimrmanModule& Timrman() { return timrman_; }
    const iop::hle::ThbaseModule& Thbase() const { return thbase_; }
    iop::hle::SifcmdModule& Sifcmd() { return sifcmd_; }
    LtiProtocol& Protocol() { return protocol_; }
    iop::IopLoader& Loader() { return loader_; }

private:
    void RegisterModuleRows();
    void DeviceCall(size_t row, iop::IopContext& ctx);
    void RestoreHeartbeat();   // the hard timer from the restored RAM's descriptor

    iop::IopModuleTable& table_;
    iop::IopMemory& mem_;
    iop::IopLoader loader_;
    iop::hle::SysmemModule sysmem_;
    iop::hle::IntrmanModule intrman_;
    iop::hle::StdioModule stdio_;
    iop::hle::SysclibModule sysclib_;
    iop::hle::IomanModule ioman_;
    iop::hle::SifcmdModule sifcmd_;
    iop::hle::SifmanModule sifman_;
    iop::hle::ThbaseModule thbase_;
    iop::hle::ThsemapModule thsemap_;
    iop::hle::TimrmanModule timrman_;
    iop::hle::LibsdModule libsd_;
    iop::hle::CdvdmanModule cdvdman_;
    LtiProtocol protocol_;
    iop::IopmSection::Statics statics_{};   // the IOPM section's mirror of the kernel statics
    bool booted_ = false;
};

}  // namespace ps2r::title::stream_irx
