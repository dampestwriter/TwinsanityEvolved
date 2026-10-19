// games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h --
// ModuleLayout: the title data of STREAM.IRX (Traveller's Tales' lti_streamer)
// that the engine's IOP kernel, loader and HLE modules take as parameters. Every
// address here is a fact of this IRX image (Ghidra / the original's boot code),
// never engine policy: engine/ holds no title constant (LintEnginePurity).
#pragma once

#include <cstdint>

#include "engine/iop/hle/thbase/thbase_module.h"
#include "engine/iop/hle/timrman/timrman_module.h"
#include "engine/iop/iop_loader.h"

namespace ps2r::title::stream_irx {

struct ModuleLayout {
    // Where the image lives on the disc (ISO LBA, byte size) -- the 4D disc reader's key.
    static constexpr uint32_t kIsoLba = 1019227;
    static constexpr uint32_t kImageBytes = 116156;

    static constexpr uint32_t kGp = 0x13328;             // the module's $gp
    static constexpr uint32_t kEntry = 0x13C;            // _start
    static constexpr uint32_t kStackTop = 0x100000;      // entry $sp = kStackTop - 0x10
    static constexpr uint32_t kStackStride = 0x4000;     // thread tid's $sp = kStackTop - tid*stride - 0x10
    static constexpr uint32_t kRpcTidAddr = 0x13328;     // the entry stores its RPC-dispatch tid here
    static constexpr uint32_t kTimerHandlerSp = 0x1FFC00;   // scratch stack of the hard-timer beat
    // The heartbeat descriptor the module's init RPC fills: {0, hard-timer id, compare
    // (sysclock ticks), 0}; its shutdown frees the timer and zeroes the id word. The
    // handler is the module's iSignalSema beat on the streaming worker's semaphore.
    static constexpr uint32_t kTimerDesc = 0x14530;
    static constexpr uint32_t kTimerDescIdWord = kTimerDesc + 4;
    static constexpr uint32_t kTimerDescCompareWord = kTimerDesc + 8;
    static constexpr uint32_t kTimerHandler = 0x136C;
    static constexpr uint32_t kRpcServerId = 0x12345;    // sceSifRegisterRpc server id (the EE binds it)
    static constexpr uint32_t kMainHandler = 0x8D28;     // the RPC dispatch function (main_handler)
    static constexpr uint32_t kRpcSendBuffer = 0x1344C;  // main_handler's request buffer ($a0)
    static constexpr uint32_t kRpcResultBuffer = 0x14A90;   // build_result_buffer's output (the reply)
    static constexpr uint32_t kRpcDispatchSp = kStackTop - 0x10;   // main_handler runs on the entry's stack

    static constexpr iop::IopLoader::ModuleLayout Loader() {
        return iop::IopLoader::ModuleLayout{kGp, kEntry, kStackTop, kStackStride, kRpcTidAddr};
    }
    static constexpr iop::hle::ThbaseModule::ThreadLayout Threads() {
        return iop::hle::ThbaseModule::ThreadLayout{kStackTop, kStackStride, kGp};
    }
    static constexpr iop::hle::TimrmanModule::TimerLayout Timer() {
        return iop::hle::TimrmanModule::TimerLayout{kTimerHandlerSp, kGp};
    }
};

}  // namespace ps2r::title::stream_irx
