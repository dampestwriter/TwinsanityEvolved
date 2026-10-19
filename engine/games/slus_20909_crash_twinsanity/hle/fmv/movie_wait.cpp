#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_wait.h"

#include <stdexcept>

#include "engine/core/verify/run_meta.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
MovieWait::MovieWait(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder, MovieGraphics& pictures,
                     MovieDecoder::Body call)
    : memory_(memory), decoder_(decoder), pictures_(pictures), call_(std::move(call)) {}

bool MovieWait::Install(const MovieDecoder::Capture& capture,
                        const MovieDecoder::Installer& install, bool selected) {
    if (installed_ || !capture || !install ||
        (selected && (!call_ || !pictures_.Selected() || decoder_.Services() != 127u ||
                      !capture(0x2b0fe0) || !capture(0x19fcb8)))) return false;
    original_ = capture(kAddress); tail_ = capture(kTail);
    if (!original_ || !tail_) return false;
    selected_ = selected;
    if (!install(kAddress, [this](auto& c, auto at) { Run(c, at); })) return false;
    if (!install(kTail, [this](auto& c, auto at) {
            if (selected_) Fail("Movie WaitFrame cannot resume the retained spin continuation");
            ++tail_calls_;
            tail_(c, at);
        })) {
        install(kAddress, original_);
        return false;
    }
    installed_ = true;
    return true;
}

[[noreturn]] void MovieWait::Fail(const char* why) {
    failed_ = true;
    throw std::runtime_error(why);
}

void MovieWait::Call(CpuContext& c, uint32_t address, uint32_t caller, bool presenting) {
    const auto a0 = c.cpuRegs.GPR.r[4], ra = c.cpuRegs.GPR.r[31];
    if (presenting) c.cpuRegs.GPR.r[4].UD[0] = 0;
    c.cpuRegs.GPR.r[31].SD[0] = int32_t(caller);
    try { call_(c, address); }
    catch (...) {
        c.cpuRegs.GPR.r[4] = a0; c.cpuRegs.GPR.r[31] = ra;
        throw;
    }
    c.cpuRegs.GPR.r[4] = a0; c.cpuRegs.GPR.r[31] = ra;
}

void MovieWait::Run(CpuContext& c, uint32_t entry) {
    if (!selected_) { ++original_calls_; original_(c, entry); return; }
    if (active_ || failed_ || (entry && entry != kAddress))
        Fail("Movie WaitFrame requires a complete nonrecursive service boundary");
    const auto player = c.cpuRegs.GPR.r[4].UL[0];
    if (c.cpuRegs.GPR.r[28].UL[0] != 0x3114f0 || !player ||
        player > UINT32_MAX - 0x1e8 || !memory_.Translate(player, 0x1e8) ||
        memory_.Read32(MovieDecoder::kDecoderPointer) != player + 0x80 ||
        memory_.Read32(player + 0x80 + 0x148) != 2 ||
        memory_.Read32(player + 0x80 + 0x154) != 0x100 || memory_.Read32(player + 0x40) > 2)
        Fail("Movie WaitFrame has no matching software RGB32 player");
    active_ = true;
    try {
        ++waits_;
        for (unsigned reads = 0; reads < 2 && memory_.Read32(player + 0x40) < 2; ++reads) {
            Call(c, 0x2b0fe0, 0x2af940, false);
            ++refills_;
        }
        // Keep the complete game present/next-frame/queue decisions. The graphics owner owns
        // their ordered output transport; this call does not invent a frame,
        // a clock tick, an interrupt flag or a successful output completion.
        Call(c, 0x19fcb8, 0x2af588, true);
        ++presents_;
    } catch (...) {
        active_ = false; failed_ = true;
        throw;
    }
    c.cpuRegs.GPR.r[2].SD[0] = 1;
    c.cpuRegs.pc = c.cpuRegs.GPR.r[31].UL[0];
    active_ = false;
}

bool MovieWait::CanCheckpoint(std::string* reason) const {
    if (!active_ && !failed_) return true;
    if (reason) *reason = "Movie WaitFrame is active or has failed";
    return false;
}
std::string MovieWait::Census() const {
    return "selected=" + std::to_string(selected_) + ",original_calls=" + std::to_string(original_calls_) +
        ",tail_calls=" + std::to_string(tail_calls_) + ",waits=" + std::to_string(waits_) +
        ",refills=" + std::to_string(refills_) + ",presents=" + std::to_string(presents_) +
        ",failed=" + std::to_string(failed_);
}
void MovieWait::Record(ps2r::verify::RunMeta& meta) const { meta.Set("movie_wait", Census()); }
} // namespace title
