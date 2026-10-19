// games/slus_20909_crash_twinsanity/iop/stream_irx/lti_protocol.cpp -- the synthesised
// EE stream-event reply (the recv->slot copy contract of FUN_0000e278).
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/lti_protocol.h"

#include "engine/iop/iop_memory.h"
#include "engine/sif/hle/ee_rpc_server.h"

namespace ps2r::title::stream_irx {

LtiProtocol::Request LtiProtocol::ReadRequest(uint32_t send) const {
    Request r;
    r.slot = mem_.Read<uint32_t>(send + 0x00);
    r.offset = mem_.Read<uint32_t>(send + 0x04);
    r.size = mem_.Read<uint32_t>(send + 0x08);
    r.cmd = mem_.Read<uint32_t>(send + 0x0c);
    r.dest_ptr = mem_.Read<uint32_t>(send + 0x10);
    r.counter = mem_.Read<uint32_t>(send + 0x14);
    r.descriptor = mem_.Read<uint32_t>(send + 0x18);
    return r;
}

void LtiProtocol::CdReadRpc(iop::IopContext& ctx) {
    calls_++;
    const uint32_t send_arg = ctx.gpr[7];                                  // $a3
    const uint32_t send_size = mem_.Read<uint32_t>(ctx.gpr[29] + 0x10);    // sp+0x10
    const uint32_t recv_arg = mem_.Read<uint32_t>(ctx.gpr[29] + 0x14);     // sp+0x14
    const uint32_t recv_size = mem_.Read<uint32_t>(ctx.gpr[29] + 0x18);    // sp+0x18
    const uint32_t send = send_arg ? send_arg : kSendBuffer;
    const uint32_t recv = recv_arg ? recv_arg : kRecvBuffer;
    if (ee_server_ && ee_server_->RequestAndWait(kEeServerId, ctx.gpr[5], send, send_size ? send_size : kSendBytes,
                                                 recv, recv_size ? recv_size : kRecvBytes)) {
        bridged_++;
        ctx.gpr[2] = 0;   // the EE server's reply is already in place
        ctx.FixZero();
        return;
    }
    const Request q = ReadRequest(send);

    for (uint32_t o = 0; o < kRecvBytes; o += 4) mem_.Write<uint32_t>(recv + o, 0);
    if (q.cmd == kCmdEnd) {
        finalizes_++;
        mem_.Write<uint32_t>(recv + 0x00, 0);                 // busy clear
        mem_.Write<uint32_t>(recv + 0x18, 1);                 // group-B valid
        mem_.Write<uint32_t>(recv + 0x1c, kFinalizerMarker);  // '~' finalizer marker type
        mem_.Write<uint32_t>(recv + 0x20, q.descriptor);      // EE buffer descriptor
        mem_.Write<uint32_t>(recv + 0x24, q.offset);          // file position (running offset)
    } else {
        mem_.Write<uint32_t>(recv + 0x00, 1);                 // busy = 1: block delivered, keep alive
    }
    ctx.gpr[2] = 0;
    ctx.FixZero();
}

}  // namespace ps2r::title::stream_irx
