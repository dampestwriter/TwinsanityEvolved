// games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.cpp --
// StubSurface: the census table and the 74 iop_stubs:: forwarders.
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.h"

#include <cstring>

#include "engine/iop/hle/intrman/intrman_module.h"
#include "engine/iop/hle/ioman/ioman_module.h"
#include "engine/iop/hle/sifcmd/sifcmd_module.h"
#include "engine/iop/hle/sifman/sifman_module.h"
#include "engine/iop/hle/stdio/stdio_module.h"
#include "engine/iop/hle/sysclib/sysclib_module.h"
#include "engine/iop/hle/sysmem/sysmem_module.h"
#include "engine/iop/hle/thbase/thbase_module.h"
#include "engine/iop/hle/thsemap/thsemap_module.h"
#include "engine/iop/hle/timrman/timrman_module.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/host_app/iop_stubs.h"

namespace ps2r::title {

iop::IopMemory* StubSurface::memory_ = nullptr;
StubSurface::Modules StubSurface::modules_{};
StubSurface::DeviceProvider StubSurface::device_{};

// Row order = iop_stubs.h order. Device-facing rows (libsd 16 + cdvdman 9 = 25)
// start at index 49 and are indexed relative to that base by the forwarders.
const StubSurface::CensusRow StubSurface::kCensus[StubSurface::kRows] = {
    {"AllocSysMemory", "sysmem", true},        {"FreeSysMemory", "sysmem", true},
    {"QueryMaxFreeMemSize", "sysmem", true},   {"Kprintf", "sysmem", true},
    {"CpuSuspendIntr", "intrman", true},       {"CpuResumeIntr", "intrman", true},
    {"iop_open", "ioman", true},               {"iop_close", "ioman", true},
    {"iop_read", "ioman", true},               {"iop_lseek", "ioman", true},
    {"sceSifInitRpc", "sifcmd", true},         {"sceSifBindRpc", "sifcmd", true},
    {"sceSifCallRpc", "sifcmd", true},         {"hle_stream_cd_read_rpc", "hle", true},
    {"sceSifRegisterRpc", "sifcmd", true},     {"sceSifSetRpcQueue", "sifcmd", true},
    {"sceSifRpcLoop", "sifcmd", true},
    {"sceSifInit", "sifman", true},            {"sceSifSetDma", "sifman", true},
    {"sceSifDmaStat", "sifman", true},         {"sceSifCheckInit", "sifman", true},
    {"stdio_printf", "stdio", true},
    {"iop_memcpy", "sysclib", true},           {"iop_memset", "sysclib", true},
    {"iop_strcat", "sysclib", true},           {"iop_strcpy", "sysclib", true},
    {"iop_strlen", "sysclib", true},           {"iop_strncmp", "sysclib", true},
    {"iop_strstr", "sysclib", true},
    {"CreateThread", "thbase", true},          {"DeleteThread", "thbase", true},
    {"StartThread", "thbase", true},           {"TerminateThread", "thbase", true},
    {"ChangeThreadPriority", "thbase", true},  {"GetThreadId", "thbase", true},
    {"SleepThread", "thbase", true},           {"WakeupThread", "thbase", true},
    {"DelayThread", "thbase", true},           {"USec2SysClock", "thbase", true},
    {"CreateSema", "thsemap", true},           {"DeleteSema", "thsemap", true},
    {"SignalSema", "thsemap", true},           {"iSignalSema", "thsemap", true},
    {"WaitSema", "thsemap", true},
    {"AllocHardTimer", "timrman", true},       {"FreeHardTimer", "timrman", true},
    {"SetTimerHandler", "timrman", true},      {"SetupHardTimer", "timrman", true},
    {"StartHardTimer", "timrman", true},
    {"libsd_stub_0", "libsd", false},  {"libsd_stub_1", "libsd", false},  {"libsd_stub_2", "libsd", false},
    {"libsd_stub_3", "libsd", false},  {"libsd_stub_4", "libsd", false},  {"libsd_stub_5", "libsd", false},
    {"libsd_stub_6", "libsd", false},  {"libsd_stub_7", "libsd", false},  {"libsd_stub_8", "libsd", false},
    {"libsd_stub_9", "libsd", false},  {"libsd_stub_10", "libsd", false}, {"libsd_stub_11", "libsd", false},
    {"libsd_stub_12", "libsd", false}, {"libsd_stub_13", "libsd", false}, {"libsd_stub_14", "libsd", false},
    {"libsd_stub_15", "libsd", false},
    {"sceCdInit", "cdvdman", false},        {"sceCdRead", "cdvdman", false},
    {"sceCdGetError", "cdvdman", false},    {"sceCdSearchFile", "cdvdman", false},
    {"sceCdSync", "cdvdman", false},        {"sceCdGetDiskType", "cdvdman", false},
    {"sceCdDiskReady", "cdvdman", false},   {"sceCdCallback", "cdvdman", false},
    {"sceCdMmode", "cdvdman", false},
};

namespace {
constexpr size_t kDeviceBase = 49;   // first device-facing census row
}  // namespace

void StubSurface::Unbind() {
    memory_ = nullptr;
    modules_ = Modules{};
    device_ = DeviceProvider{};
}

size_t StubSurface::KernelFacingCount() {
    size_t n = 0;
    for (const CensusRow& r : kCensus) n += r.kernel_facing ? 1 : 0;
    return n;
}

size_t StubSurface::DeviceFacingCount() { return kRows - KernelFacingCount(); }

int StubSurface::FindRow(const char* name) {
    for (size_t i = 0; i < kRows; ++i)
        if (std::strcmp(kCensus[i].name, name) == 0) return static_cast<int>(i);
    return -1;
}

void StubSurface::DeviceCall(size_t row, iop::IopContext& ctx) {
    if (device_) {
        device_(row, ctx);
        return;
    }
    ctx.gpr[2] = 0;   // idle provider: the call succeeds and does nothing
}

}  // namespace ps2r::title

// ---- the 74 forwarders ------------------------------------------------------
namespace iop_stubs {

using ps2r::title::StubSurface;

#define PS2R_FORWARD(module, method) \
    void method(IopContext& ctx) { StubSurface::Bound().module->method(ctx); }

PS2R_FORWARD(sysmem, AllocSysMemory)
PS2R_FORWARD(sysmem, FreeSysMemory)
PS2R_FORWARD(sysmem, QueryMaxFreeMemSize)
PS2R_FORWARD(sysmem, Kprintf)

PS2R_FORWARD(intrman, CpuSuspendIntr)
PS2R_FORWARD(intrman, CpuResumeIntr)

PS2R_FORWARD(ioman, iop_open)
PS2R_FORWARD(ioman, iop_close)
PS2R_FORWARD(ioman, iop_read)
PS2R_FORWARD(ioman, iop_lseek)

PS2R_FORWARD(sifcmd, sceSifInitRpc)
PS2R_FORWARD(sifcmd, sceSifBindRpc)
PS2R_FORWARD(sifcmd, sceSifCallRpc)
PS2R_FORWARD(sifcmd, hle_stream_cd_read_rpc)
PS2R_FORWARD(sifcmd, sceSifRegisterRpc)
PS2R_FORWARD(sifcmd, sceSifSetRpcQueue)
PS2R_FORWARD(sifcmd, sceSifRpcLoop)

PS2R_FORWARD(sifman, sceSifInit)
PS2R_FORWARD(sifman, sceSifSetDma)
PS2R_FORWARD(sifman, sceSifDmaStat)
PS2R_FORWARD(sifman, sceSifCheckInit)

void stdio_printf(IopContext& ctx) { StubSurface::Bound().stdio->printf(ctx); }

void iop_memcpy(IopContext& ctx) { StubSurface::Bound().sysclib->memcpy(ctx); }
void iop_memset(IopContext& ctx) { StubSurface::Bound().sysclib->memset(ctx); }
void iop_strcat(IopContext& ctx) { StubSurface::Bound().sysclib->strcat(ctx); }
void iop_strcpy(IopContext& ctx) { StubSurface::Bound().sysclib->strcpy(ctx); }
void iop_strlen(IopContext& ctx) { StubSurface::Bound().sysclib->strlen(ctx); }
void iop_strncmp(IopContext& ctx) { StubSurface::Bound().sysclib->strncmp(ctx); }
void iop_strstr(IopContext& ctx) { StubSurface::Bound().sysclib->strstr(ctx); }

PS2R_FORWARD(thbase, CreateThread)
PS2R_FORWARD(thbase, DeleteThread)
PS2R_FORWARD(thbase, StartThread)
PS2R_FORWARD(thbase, TerminateThread)
PS2R_FORWARD(thbase, ChangeThreadPriority)
PS2R_FORWARD(thbase, GetThreadId)
PS2R_FORWARD(thbase, SleepThread)
PS2R_FORWARD(thbase, WakeupThread)
PS2R_FORWARD(thbase, DelayThread)
PS2R_FORWARD(thbase, USec2SysClock)

PS2R_FORWARD(thsemap, CreateSema)
PS2R_FORWARD(thsemap, DeleteSema)
PS2R_FORWARD(thsemap, SignalSema)
PS2R_FORWARD(thsemap, iSignalSema)
PS2R_FORWARD(thsemap, WaitSema)

PS2R_FORWARD(timrman, AllocHardTimer)
PS2R_FORWARD(timrman, FreeHardTimer)
PS2R_FORWARD(timrman, SetTimerHandler)
PS2R_FORWARD(timrman, SetupHardTimer)
PS2R_FORWARD(timrman, StartHardTimer)

#undef PS2R_FORWARD

#define PS2R_DEVICE(name, row) \
    void name(IopContext& ctx) { StubSurface::DeviceCall(ps2r::title::kDeviceBase + (row), ctx); }

PS2R_DEVICE(libsd_stub_0, 0)   PS2R_DEVICE(libsd_stub_1, 1)   PS2R_DEVICE(libsd_stub_2, 2)
PS2R_DEVICE(libsd_stub_3, 3)   PS2R_DEVICE(libsd_stub_4, 4)   PS2R_DEVICE(libsd_stub_5, 5)
PS2R_DEVICE(libsd_stub_6, 6)   PS2R_DEVICE(libsd_stub_7, 7)   PS2R_DEVICE(libsd_stub_8, 8)
PS2R_DEVICE(libsd_stub_9, 9)   PS2R_DEVICE(libsd_stub_10, 10) PS2R_DEVICE(libsd_stub_11, 11)
PS2R_DEVICE(libsd_stub_12, 12) PS2R_DEVICE(libsd_stub_13, 13) PS2R_DEVICE(libsd_stub_14, 14)
PS2R_DEVICE(libsd_stub_15, 15)
PS2R_DEVICE(sceCdInit, 16)       PS2R_DEVICE(sceCdRead, 17)     PS2R_DEVICE(sceCdGetError, 18)
PS2R_DEVICE(sceCdSearchFile, 19) PS2R_DEVICE(sceCdSync, 20)     PS2R_DEVICE(sceCdGetDiskType, 21)
PS2R_DEVICE(sceCdDiskReady, 22)  PS2R_DEVICE(sceCdCallback, 23) PS2R_DEVICE(sceCdMmode, 24)

#undef PS2R_DEVICE

}  // namespace iop_stubs
