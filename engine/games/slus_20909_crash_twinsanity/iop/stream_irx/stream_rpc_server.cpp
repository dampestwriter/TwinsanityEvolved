#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h"

#include <string>

#include "engine/iop/iop_context.h"
#include "engine/iop/iop_memory.h"
#include "engine/iop/sif_rpc_bridge.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"

namespace ps2r::title::stream_irx {

StreamRpcServer::StreamRpcServer(iop::IopMemory& mem, iop::SifRpcBridge& bridge, EeRead read, EeWrite write)
    : mem_(mem), bridge_(bridge), read_(std::move(read)), write_(std::move(write)) {}

void StreamRpcServer::Call(const RpcRouter::CallRequest& req) {
    ++calls_;
    RecordCommandIds(req);
    const RpcRouter::CallRequest copy = req;
    if (!bridge_.RunOnRpcThread([this, copy] { Dispatch(copy); })) ++stalls_;
}

void StreamRpcServer::Dispatch(const RpcRouter::CallRequest& req) {
    for (uint32_t i = 0; i < req.send_size; ++i)
        mem_.Write<uint8_t>(ModuleLayout::kRpcSendBuffer + i, read_(req.send + i));
    iop::IopContext ctx = iop::IopContext::ForEntry(ModuleLayout::kRpcDispatchSp, ModuleLayout::kGp);
    ctx.gpr[4] = ModuleLayout::kRpcSendBuffer;
    ctx.pc = ModuleLayout::kMainHandler;
    if (auto fn = StreamIrxPlugin::Resolve(ModuleLayout::kMainHandler)) fn(ctx);
    for (uint32_t i = 0; i < req.recv_size; ++i)
        write_(req.recv + i, mem_.Read<uint8_t>(ModuleLayout::kRpcResultBuffer + i));
}

}  // namespace ps2r::title::stream_irx

namespace ps2r::title::stream_irx {

// The request's command records are decoded from EE RAM at call time (the
// same walk the module's main handler does): header, count, then count x
// {cmd, nparams, params[nparams]}, bounded by the send size.
void StreamRpcServer::RecordCommandIds(const RpcRouter::CallRequest& req) {
    auto rd16 = [this](uint32_t addr) -> uint16_t {
        return static_cast<uint16_t>(read_(addr) | (static_cast<uint16_t>(read_(addr + 1)) << 8));
    };
    if (req.send_size < 4) return;
    const uint32_t end = req.send + req.send_size;
    const uint16_t count = rd16(req.send + 2);
    uint32_t ptr = req.send + 4;
    for (uint16_t i = 0; i < count && ptr + 4 <= end; ++i) {
        const uint16_t cmd = rd16(ptr);
        const uint16_t nparams = rd16(ptr + 2);
        ptr += 4;
        if (ptr + static_cast<uint32_t>(nparams) * 2 > end) break;
        TailCmd t;
        t.boundary = boundary_;
        t.cmd = cmd;
        t.nparams = nparams;
        for (uint16_t p = 0; p < nparams && p < kTailParams; ++p) t.params[p] = rd16(ptr + p * 2);
        if (tail_.size() < kMaxTailCmds) tail_.push_back(t);
        else tail_[tail_next_] = t;
        tail_next_ = (tail_next_ + 1) % kMaxTailCmds;
        if (tail_count_ < kMaxTailCmds) ++tail_count_;
        ptr += static_cast<uint32_t>(nparams) * 2;
        if (cmd_ids_.size() < kMaxRecordedCmds) {
            cmd_ids_.push_back(cmd);
            head_.push_back(t);
        }
    }
}

std::string StreamRpcServer::CommandIdsCsv() const {
    std::string out;
    for (size_t i = 0; i < cmd_ids_.size(); ++i) {
        if (i) out += ',';
        out += std::to_string(cmd_ids_[i]);
    }
    return out;
}

// Oldest first: the ring's start is tail_next_ once it wrapped, 0 before.
std::string StreamRpcServer::CommandTailCsv() const {
    const size_t start = tail_count_ < kMaxTailCmds ? 0 : tail_next_;
    return RenderCmds(tail_.data(), tail_count_, start, kMaxTailCmds);
}

std::string StreamRpcServer::CommandHeadCsv() const { return RenderCmds(head_.data(), head_.size(), 0, kMaxRecordedCmds); }

std::string StreamRpcServer::RenderCmds(const TailCmd* first, size_t count, size_t start, size_t ring) {
    std::string out;
    uint64_t open_boundary = 0;
    bool open = false;
    for (size_t n = 0; n < count; ++n) {
        const TailCmd& t = first[(start + n) % ring];
        if (!open || t.boundary != open_boundary) {
            if (open) out += ';';
            out += 'b' + std::to_string(t.boundary) + ':';
            open_boundary = t.boundary;
            open = true;
        } else {
            out += ',';
        }
        out += std::to_string(t.cmd);
        if (t.nparams) {
            out += '[';
            for (uint16_t p = 0; p < t.nparams && p < kTailParams; ++p) {
                if (p) out += ' ';
                out += std::to_string(t.params[p]);
            }
            out += ']';
        }
    }
    return out;
}

std::string StreamRpcServer::CommandIdsFromBoundaryCsv(uint64_t boundary) const {
    std::string out;
    const size_t start = tail_count_ < kMaxTailCmds ? 0 : tail_next_;
    for (size_t n = 0; n < tail_count_; ++n) {
        const TailCmd& t = tail_[(start + n) % kMaxTailCmds];
        if (t.boundary < boundary) continue;
        if (!out.empty()) out += ',';
        out += std::to_string(t.cmd);
    }
    return out;
}

}  // namespace ps2r::title::stream_irx
