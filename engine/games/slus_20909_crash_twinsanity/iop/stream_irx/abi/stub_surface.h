// games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.h --
// StubSurface: the process-global binding behind the generated STREAM.IRX
// TU's `iop_stubs::` / `iop_memory::` calls. The 74 free-function forwarders
// (abi/host_app/iop_stubs.h) route each name to the bound HLE module
// instance; libsd/cdvdman rows route to the device-facing provider hook (the
// 4B'' modules) and idle to $v0 = 0 until one is bound. The census table lists
// every name with its library so the StreamIrxStubSurface test can prove
// referenced == provided. That test also references the generated function map,
// which links iop_recompiled.o in, so an unlisted extern fails its link. Text
// census 74 = the ABI declarations; link census 72: Kprintf and the
// hle_stream_cd_read_rpc call sites are statically unreachable in the TU, so the
// object never imports them (nm -u) -- both keep their forwarders as ABI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "engine/iop/iop_context.h"
#include "engine/iop/iop_memory.h"

namespace ps2r::iop::hle {
class SysmemModule;
class IntrmanModule;
class StdioModule;
class SysclibModule;
class IomanModule;
class SifcmdModule;
class SifmanModule;
class ThbaseModule;
class ThsemapModule;
class TimrmanModule;
}  // namespace ps2r::iop::hle

namespace ps2r::title {

class StubSurface {
public:
    struct Modules {
        iop::hle::SysmemModule* sysmem = nullptr;
        iop::hle::IntrmanModule* intrman = nullptr;
        iop::hle::StdioModule* stdio = nullptr;
        iop::hle::SysclibModule* sysclib = nullptr;
        iop::hle::IomanModule* ioman = nullptr;
        iop::hle::SifcmdModule* sifcmd = nullptr;
        iop::hle::SifmanModule* sifman = nullptr;
        iop::hle::ThbaseModule* thbase = nullptr;
        iop::hle::ThsemapModule* thsemap = nullptr;
        iop::hle::TimrmanModule* timrman = nullptr;
    };

    // The device-facing provider: (census row index, ctx) for libsd/cdvdman names.
    using DeviceProvider = std::function<void(size_t row, iop::IopContext& ctx)>;

    struct CensusRow {
        const char* name;      // the iop_stubs:: name
        const char* library;   // the IOP library it imports from ("hle" for the thunk)
        bool kernel_facing;    // routed to a bound module (true) or the device provider (false)
    };

    static constexpr size_t kRows = 74;

    static void BindMemory(iop::IopMemory& mem) { memory_ = &mem; }
    static void BindModules(const Modules& m) { modules_ = m; }
    static void BindDeviceProvider(DeviceProvider p) { device_ = std::move(p); }
    static void Unbind();

    static iop::IopMemory& Memory() { return *memory_; }
    static const Modules& Bound() { return modules_; }

    static const CensusRow* Census() { return kCensus; }
    static size_t KernelFacingCount();
    static size_t DeviceFacingCount();
    static int FindRow(const char* name);   // -1 when unlisted

    // Device-facing dispatch used by the libsd/cdvdman forwarders.
    static void DeviceCall(size_t row, iop::IopContext& ctx);

private:
    static iop::IopMemory* memory_;
    static Modules modules_;
    static DeviceProvider device_;
    static const CensusRow kCensus[kRows];
};

}  // namespace ps2r::title
