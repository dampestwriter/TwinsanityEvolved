#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stop.h"

#include <cstring>
#include <stdexcept>

#include "engine/core/verify/run_meta.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
MovieStop::MovieStop(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder, MovieGraphics& pictures,
                     MovieDecoder::Body call)
    : memory_(memory), decoder_(decoder), pictures_(pictures), call_(std::move(call)) {}

bool MovieStop::Install(const MovieDecoder::Capture& capture,
                        const MovieDecoder::Installer& install, bool selected) {
    if (installed_ || !capture || !install ||
        (selected && (!call_ || !pictures_.Selected() || decoder_.Services() != 127u))) return false;
    original_ = capture(kAddress); callback_ = capture(kCallback);
    if (!original_ || !callback_) return false;
    capture_ = capture;
    selected_ = selected;
    if (!install(kAddress, [this](auto& c, auto at) { Run(c, at); })) return false;
    if (!install(kCallback, [this](auto& c, auto at) {
            if (selected_) Fail("Movie direct presentation cannot execute a retained VBlank callback");
            ++callback_calls_;
            callback_(c, at);
        })) {
        install(kAddress, original_);
        return false;
    }
    installed_ = true;
    return true;
}

[[noreturn]] void MovieStop::Fail(const char* why) {
    failed_ = true;
    throw std::runtime_error(why);
}

void MovieStop::Call(CpuContext& c, uint32_t address, uint32_t caller,
                     std::initializer_list<uint32_t> args) {
    const auto saved = c.cpuRegs.GPR;
    unsigned reg = 4;
    for (uint32_t arg : args) c.cpuRegs.GPR.r[reg++].SD[0] = int32_t(arg);
    c.cpuRegs.GPR.r[31].UL[0] = caller;
    try { call_(c, address); }
    catch (...) { c.cpuRegs.GPR = saved; throw; }
    c.cpuRegs.GPR = saved;
}

void MovieStop::CheckVirtual(uint32_t object, uint32_t table_offset, uint32_t slot) {
    if (!object || object > UINT32_MAX - table_offset - 4 ||
        !memory_.Translate(object, table_offset + 4))
        Fail("Movie Stop has an unmapped source object");
    const auto table = memory_.Read32(object + table_offset);
    if (!table || table > UINT32_MAX - slot - 8 || !memory_.Translate(table + slot, 8))
        Fail("Movie Stop has an unmapped source vtable");
    int16_t adjust;
    std::memcpy(&adjust, memory_.Translate(table + slot, 2), 2);
    const int64_t adjusted = int64_t(object) + adjust;
    const auto address = memory_.Read32(table + slot + 4);
    if (adjusted <= 0 || adjusted > UINT32_MAX || !memory_.Translate(uint32_t(adjusted), 4) ||
        address < 0x100008 || address >= 0x300000 || !capture_(address))
        Fail("Movie Stop has an invalid complete source virtual call");
}

void MovieStop::Virtual(CpuContext& c, uint32_t object, uint32_t table_offset,
                        uint32_t slot, uint32_t caller, std::initializer_list<uint32_t> args) {
    CheckVirtual(object, table_offset, slot);
    const auto table = memory_.Read32(object + table_offset);
    int16_t adjust;
    std::memcpy(&adjust, memory_.Translate(table + slot, 2), 2);
    const auto saved = c.cpuRegs.GPR;
    c.cpuRegs.GPR.r[4].SD[0] = int32_t(object + adjust);
    unsigned reg = 5;
    for (uint32_t arg : args) c.cpuRegs.GPR.r[reg++].SD[0] = int32_t(arg);
    c.cpuRegs.GPR.r[31].UL[0] = caller;
    try { call_(c, memory_.Read32(table + slot + 4)); }
    catch (...) { c.cpuRegs.GPR = saved; throw; }
    c.cpuRegs.GPR = saved;
}

void MovieStop::Close(CpuContext& c, uint32_t player) {
    Call(c, 0x2af5d0, 0x2ae6bc, {player});
    c.fpuRegs.fpr[12].f = 0;
    Call(c, MovieDecoder::kSetVolume, 0x2ae6c8, {});
    Call(c, MovieDecoder::kSoundRemote, 0x2ae6e4, {1, 0x80e0, 0, 2, 0, 0});
    const auto ring = memory_.Read32(MovieDecoder::kSoundRing);
    if (ring) {
        const auto* lent = memory_.Translate(0x3114f0 - 0x6ba4, 1);
        if (!lent) Fail("Movie Stop has no source sound loan state");
        Call(c, *lent ? 0x1e6230 : 0x2d46c8, *lent ? 0x2ae704 : 0x2ae714, {ring});
    }
    memory_.Write32(MovieDecoder::kSoundRing, 0);
    memory_.Write32(MovieDecoder::kSoundRingSize, 0);
    if (memory_.Read32(player + 0x1e0) == 1)
        Call(c, MovieDecoder::kClose, 0x2ae734, {});
}

void MovieStop::Recover(CpuContext& c, uint32_t player, uint32_t renderer, uint32_t target) {
    const auto color = memory_.Read32(target + 0x14);
    const auto stack = c.cpuRegs.GPR.r[29].UL[0];
    Call(c, 0x11bf10, 0x2ae740, {stack, 8}); // source ColourBlack lookup
    memory_.Write32(target + 0x14, memory_.Read32(stack));
    Call(c, 0x19f1c8, 0x2ae758, {target, 1, 1});
    Virtual(c, renderer, 0x1c, 0x50, 0x2ae770, {0});
    Virtual(c, renderer, 0x1c, 0x70, 0x2ae788, {1});
    Call(c, 0x19f1c8, 0x2ae798, {target, 1, 1});
    Virtual(c, renderer, 0x1c, 0x50, 0x2ae7b0, {0});
    Virtual(c, renderer, 0x1c, 0x68, 0x2ae7c4, {});
    // Keep the source's existing WaitIdle/ResetBuckets order. The graphics owner owns
    // replacement of those graphics service boundaries, including completion.
    Call(c, 0x2cf560, 0x2ae7cc, {0});
    Call(c, 0x1818b0, 0x2ae7d4, {});
    Call(c, 0x17dbf8, 0x2ae7dc, {});
    Call(c, 0x2c1b50, 0x2ae7e8, {0, 0});
    *memory_.Translate(0x3114f0 - 0x7d44, 1) = 0;
    Call(c, 0x17ff50, 0x2ae7f4, {0x323658});
    Virtual(c, renderer, 0x1c, 0x50, 0x2ae80c, {0});
    Virtual(c, renderer, 0x1c, 0x68, 0x2ae820, {});
    Virtual(c, player, 0x18, 0x48, 0x2ae834, {});
    memory_.Write32(target + 0x14, color);
    ++recoveries_;
}

void MovieStop::Run(CpuContext& c, uint32_t entry) {
    if (!selected_) { ++original_calls_; original_(c, entry); return; }
    if (active_ || failed_ || (entry && entry != kAddress))
        Fail("Movie Stop requires a complete nonrecursive source service boundary");
    const auto player = c.cpuRegs.GPR.r[4].UL[0], stack = c.cpuRegs.GPR.r[29].UL[0];
    if (c.cpuRegs.GPR.r[28].UL[0] != 0x3114f0 || !(c.cpuRegs.CP0.n.Status & 1) ||
        !player || player > UINT32_MAX - 0x4200 || !memory_.Translate(player, 0x4200) ||
        stack < 0x40 || !memory_.Translate(stack - 0x40, 0x40) ||
        memory_.Read32(player + 0x18) != 0x3068e0 || memory_.Read32(0x306928) != 0 ||
        memory_.Read32(0x30692c) != 0x2afa00 || memory_.Read32(player + 0x1e0) != 1 ||
        memory_.Read32(MovieDecoder::kDecoderPointer) != player + 0x80 ||
        memory_.Read32(player + 0x80 + 0x148) != 2 ||
        memory_.Read32(player + 0x80 + 0x154) != 0x100 ||
        memory_.Read32(player + 0x80 + 0x14c) != 1 ||
        memory_.Read32(player + 0x80 + 0x110) != 0 ||
        memory_.Read32(MovieDecoder::kSoundMode) != 1 || decoder_.CanCheckpoint())
        Fail("Movie Stop has no matching NTSC player and enabled source caller");
    const auto renderer = memory_.Read32(0x309728), state = memory_.Read32(0x309740);
    if (!state || state > UINT32_MAX - 12 || !memory_.Translate(state, 12))
        Fail("Movie Stop has no source render target owner");
    const auto target = memory_.Read32(state + 8);
    if (!target || target > UINT32_MAX - 0x18 || !memory_.Translate(target, 0x18))
        Fail("Movie Stop has no mapped source render target");
    for (uint32_t slot : {0x50u, 0x68u, 0x70u}) CheckVirtual(renderer, 0x1c, slot);
    CheckVirtual(player, 0x18, 0x48);
    const auto saved = c.cpuRegs.GPR;
    c.cpuRegs.GPR.r[29].UD[0] = stack - 0x40;
    c.cpuRegs.GPR.r[18].UD[0] = player;
    active_ = true;
    try {
        ++stops_;
        Close(c, player);
        Recover(c, player, renderer, target);
    } catch (...) {
        c.cpuRegs.GPR = saved; active_ = false; failed_ = true;
        throw;
    }
    c.cpuRegs.GPR = saved;
    c.cpuRegs.GPR.r[2].SD[0] = 1;
    active_ = false;
}

bool MovieStop::CanCheckpoint(std::string* reason) const {
    if (!active_ && !failed_) return true;
    if (reason) *reason = "Movie Stop is active or has failed";
    return false;
}
std::string MovieStop::Census() const {
    return "selected=" + std::to_string(selected_) + ",original_calls=" + std::to_string(original_calls_) +
        ",callback_calls=" + std::to_string(callback_calls_) + ",stops=" + std::to_string(stops_) +
        ",recoveries=" + std::to_string(recoveries_) + ",failed=" + std::to_string(failed_);
}
void MovieStop::Record(ps2r::verify::RunMeta& meta) const { meta.Set("movie_stop", Census()); }
} // namespace title
