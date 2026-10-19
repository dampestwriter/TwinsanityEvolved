#pragma once
// Generated-code ABI: the 74 `iop_stubs::` names the recompiled STREAM.IRX TU
// calls -- the 73 import stubs the frozen recompiler registered plus the one
// HLE thunk it emitted (hle_stream_cd_read_rpc). Each takes the R3000 register
// file: arguments in $a0..$a3, result in $v0. The bodies are StubSurface's
// forwarders (stub_surface.cpp); this header is the contract the census test
// pins (StreamIrxStubSurface).
#include "../host_app/iop_context.h"

namespace iop_stubs {

// sysmem (4)
void AllocSysMemory(IopContext& ctx);
void FreeSysMemory(IopContext& ctx);
void QueryMaxFreeMemSize(IopContext& ctx);
void Kprintf(IopContext& ctx);

// intrman (2)
void CpuSuspendIntr(IopContext& ctx);
void CpuResumeIntr(IopContext& ctx);

// ioman (4)
void iop_open(IopContext& ctx);
void iop_close(IopContext& ctx);
void iop_read(IopContext& ctx);
void iop_lseek(IopContext& ctx);

// sifcmd (6) + the CD-read RPC thunk
void sceSifInitRpc(IopContext& ctx);
void sceSifBindRpc(IopContext& ctx);
void sceSifCallRpc(IopContext& ctx);
void hle_stream_cd_read_rpc(IopContext& ctx);
void sceSifRegisterRpc(IopContext& ctx);
void sceSifSetRpcQueue(IopContext& ctx);
void sceSifRpcLoop(IopContext& ctx);

// sifman (4)
void sceSifInit(IopContext& ctx);
void sceSifSetDma(IopContext& ctx);
void sceSifDmaStat(IopContext& ctx);
void sceSifCheckInit(IopContext& ctx);

// stdio (1)
void stdio_printf(IopContext& ctx);

// sysclib (7): ordinals 12, 14, 20, 23, 27, 29, 34
void iop_memcpy(IopContext& ctx);
void iop_memset(IopContext& ctx);
void iop_strcat(IopContext& ctx);
void iop_strcpy(IopContext& ctx);
void iop_strlen(IopContext& ctx);
void iop_strncmp(IopContext& ctx);
void iop_strstr(IopContext& ctx);

// thbase (10)
void CreateThread(IopContext& ctx);
void DeleteThread(IopContext& ctx);
void StartThread(IopContext& ctx);
void TerminateThread(IopContext& ctx);
void ChangeThreadPriority(IopContext& ctx);
void GetThreadId(IopContext& ctx);
void SleepThread(IopContext& ctx);
void WakeupThread(IopContext& ctx);
void DelayThread(IopContext& ctx);
void USec2SysClock(IopContext& ctx);

// thsemap (5)
void CreateSema(IopContext& ctx);
void DeleteSema(IopContext& ctx);
void SignalSema(IopContext& ctx);
void iSignalSema(IopContext& ctx);
void WaitSema(IopContext& ctx);

// timrman (5)
void AllocHardTimer(IopContext& ctx);
void FreeHardTimer(IopContext& ctx);
void SetTimerHandler(IopContext& ctx);
void SetupHardTimer(IopContext& ctx);
void StartHardTimer(IopContext& ctx);

// libsd (16): device-facing, provider = the 4B'' libsd module
void libsd_stub_0(IopContext& ctx);
void libsd_stub_1(IopContext& ctx);
void libsd_stub_2(IopContext& ctx);
void libsd_stub_3(IopContext& ctx);
void libsd_stub_4(IopContext& ctx);
void libsd_stub_5(IopContext& ctx);
void libsd_stub_6(IopContext& ctx);
void libsd_stub_7(IopContext& ctx);
void libsd_stub_8(IopContext& ctx);
void libsd_stub_9(IopContext& ctx);
void libsd_stub_10(IopContext& ctx);
void libsd_stub_11(IopContext& ctx);
void libsd_stub_12(IopContext& ctx);
void libsd_stub_13(IopContext& ctx);
void libsd_stub_14(IopContext& ctx);
void libsd_stub_15(IopContext& ctx);

// cdvdman (9): device-facing, provider = the 4B'' cdvdman module
void sceCdInit(IopContext& ctx);
void sceCdRead(IopContext& ctx);
void sceCdGetError(IopContext& ctx);
void sceCdSearchFile(IopContext& ctx);
void sceCdSync(IopContext& ctx);
void sceCdGetDiskType(IopContext& ctx);
void sceCdDiskReady(IopContext& ctx);
void sceCdCallback(IopContext& ctx);
void sceCdMmode(IopContext& ctx);

}  // namespace iop_stubs
