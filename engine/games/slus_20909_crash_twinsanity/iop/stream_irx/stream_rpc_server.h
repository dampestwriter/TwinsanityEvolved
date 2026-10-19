// games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h --
// StreamRpcServer: the RpcRouter face of STREAM.IRX's server 0x12345. An EE
// sceSifCallRpc becomes one body on the module's RPC-dispatch thread (the
// SifRpcBridge wakes tid 1 out of its SleepThread park and, under FRAME, drives
// the kernel until the body completed): the send buffer is copied EE -> IOP at
// kRpcSendBuffer, main_handler (kMainHandler) runs with $a0 = that buffer on
// the entry's stack/$gp, and kRpcResultBuffer is copied IOP -> EE into the
// caller's recv buffer (recv_size bytes). Same bytes, same order as the
// original's dispatch (send copy -> handler -> result copy).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/sif/hle/rpc_router.h"

namespace ps2r::iop {
class IopMemory;
class SifRpcBridge;
}

namespace ps2r::title::stream_irx {

class StreamRpcServer : public RpcRouter::Server {
public:
    // EE RAM access is injected (the title library holds no EE memory dependency).
    using EeRead = std::function<uint8_t(uint32_t addr)>;
    using EeWrite = std::function<void(uint32_t addr, uint8_t value)>;

    StreamRpcServer(iop::IopMemory& mem, iop::SifRpcBridge& bridge, EeRead read, EeWrite write);
    void Call(const RpcRouter::CallRequest& req) override;

    uint32_t Calls() const { return calls_; }
    uint32_t Stalls() const { return stalls_; }
    // The command ids served, in call order: each request is
    // {u16 header, u16 count, {u16 cmd, u16 nparams, u16 params[nparams]}...}
    // (the module's main handler's decode); bounded to kMaxRecordedCmds.
    const std::vector<uint16_t>& CommandIds() const { return cmd_ids_; }
    std::string CommandIdsCsv() const;
    static constexpr size_t kMaxRecordedCmds = 256;

    // The LAST kMaxTailCmds commands with their parameters, each tagged with
    // the owner's boundary count at the call (SetBoundary, called once per
    // frame boundary by the composition root): the census of what the title
    // asked the module late in a leg, after the first kMaxRecordedCmds ids.
    // Rendered as `b<boundary>:<cmd>[p0 p1 ...];...` (params beyond
    // kTailParams dropped). Data only; no guest state is read or written
    // beyond the request decode the module's handler performs anyway.
    void SetBoundary(uint64_t boundary) { boundary_ = boundary; }
    std::string CommandTailCsv() const;
    // The FIRST kMaxRecordedCmds commands in the same boundary-tagged form
    // (the ids-only `stream_rpc_cmds` stays for the existing pins).
    std::string CommandHeadCsv() const;
    // The ids of every command served at or after `boundary`, in call order,
    // bounded by the tail's depth (an empty string when none is recorded).
    std::string CommandIdsFromBoundaryCsv(uint64_t boundary) const;
    static constexpr size_t kMaxTailCmds = 512;
    static constexpr size_t kTailParams = 8;

private:
    void Dispatch(const RpcRouter::CallRequest& req);

    iop::IopMemory& mem_;
    iop::SifRpcBridge& bridge_;
    EeRead read_;
    EeWrite write_;
    void RecordCommandIds(const RpcRouter::CallRequest& req);

    struct TailCmd {
        uint64_t boundary = 0;
        uint16_t cmd = 0;
        uint16_t nparams = 0;
        uint16_t params[kTailParams] = {};
    };
    static std::string RenderCmds(const TailCmd* first, size_t count, size_t start, size_t ring);

    uint32_t calls_ = 0;
    uint32_t stalls_ = 0;
    std::vector<uint16_t> cmd_ids_;
    std::vector<TailCmd> head_;   // the first kMaxRecordedCmds commands with params
    uint64_t boundary_ = 0;
    std::vector<TailCmd> tail_;   // ring of the last kMaxTailCmds commands
    size_t tail_next_ = 0;
    size_t tail_count_ = 0;
};

}  // namespace ps2r::title::stream_irx
