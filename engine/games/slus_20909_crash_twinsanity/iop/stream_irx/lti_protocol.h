// games/slus_20909_crash_twinsanity/iop/stream_irx/lti_protocol.h --
// LtiProtocol: the title's streaming RPC protocol between STREAM.IRX (server id
// 0x12345, main_handler 0x8D28) and the EE. It owns the body behind the
// generated TU's `hle_stream_cd_read_rpc` thunk = the module's sceSifCallRpc to
// the EE stream-event server: the request FUN_0000e278 fills at the IOP send
// buffer, answered in the IOP recv buffer per the recv->slot copy contract.
// With an EeRpcServer installed the call is bridged to the game's registered EE
// stream-event server (sid kEeServerId) -- request copied into its buffer, the
// worker parked until the EE serviced it at boundary slot 6, the reply copied
// back; without one, or when no EE server is registered yet / the reply
// deadline expired, the reply is synthesised here exactly as the original.
// The CD data itself flows through the CdvdReader (Phase 4D).
#pragma once

#include <cstdint>

#include "engine/iop/iop_context.h"

class EeRpcServer;

namespace ps2r::iop {
class IopMemory;
}

namespace ps2r::title::stream_irx {

class LtiProtocol {
public:
    static constexpr uint32_t kServerId = 0x12345;   // the id STREAM.IRX registers
    static constexpr uint32_t kEeServerId = 0x12344321;  // the EE stream-event server the module calls
    static constexpr uint32_t kSendBuffer = 0x14a00; // request (FUN_0000e278 fills)
    static constexpr uint32_t kRecvBuffer = 0x15310; // reply (FUN_0000e278 reads)
    static constexpr uint32_t kSendBytes = 0x28;
    static constexpr uint32_t kRecvBytes = 0x40;
    static constexpr uint32_t kCmdEnd = 2;           // notify stream end/pause -> finalize
    static constexpr uint32_t kFinalizerMarker = 0x7e;   // '~' group-B finalizer

    // send: +0x00 slot, +0x04 running byte offset, +0x08 size, +0x0c command
    // (0 req, 1 block, 2 end, 3 init), +0x10 destPtr, +0x14 counter, +0x18 EE descriptor.
    struct Request {
        uint32_t slot = 0, offset = 0, size = 0, cmd = 0, dest_ptr = 0, counter = 0, descriptor = 0;
    };

    explicit LtiProtocol(iop::IopMemory& mem) : mem_(mem) {}

    // The EE-side server of the module's calls (null = synthesise every reply).
    void SetEeServer(EeRpcServer* server) { ee_server_ = server; }

    // The hle_stream_cd_read_rpc / sceSifCallRpc body: $a1 = fno, $a3 = send
    // (0 -> kSendBuffer), [sp+0x10] = send size (0 -> kSendBytes), [sp+0x14] = recv
    // (0 -> kRecvBuffer), [sp+0x18] = recv size (0 -> kRecvBytes). Bridged to the EE
    // server when possible; otherwise clears the reply and writes either the busy
    // ack (recv+0 = 1) or, on cmd 2, group B: +0x18 = 1, +0x1c = '~', +0x20 = descriptor,
    // +0x24 = offset. $v0 = 0 (sceSifCallRpc success).
    void CdReadRpc(iop::IopContext& ctx);

    Request ReadRequest(uint32_t send) const;
    uint32_t Calls() const { return calls_; }
    uint32_t Finalizes() const { return finalizes_; }
    uint32_t Bridged() const { return bridged_; }   // replies produced by the EE server

private:
    iop::IopMemory& mem_;
    EeRpcServer* ee_server_ = nullptr;
    uint32_t calls_ = 0;
    uint32_t finalizes_ = 0;
    uint32_t bridged_ = 0;
};

}  // namespace ps2r::title::stream_irx
