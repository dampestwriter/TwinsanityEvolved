#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_start.h"

#include <stdexcept>

#include "engine/core/verify/run_meta.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
MovieStart::MovieStart(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder, MovieGraphics& pictures,
                       MovieDecoder::Body call)
    : memory_(memory), decoder_(decoder), pictures_(pictures), call_(std::move(call)) {}

bool MovieStart::Install(const MovieDecoder::Capture& capture,
                         const MovieDecoder::Installer& install, bool selected, bool direct_presentation) {
    // No opaque loan may reach a retained buffer or PCM consumer. The full
    // service mask includes both loans, shared input and owned picture output.
    if (installed_ || !capture || !install || (direct_presentation && !selected) ||
        (selected && (!call_ || !pictures_.Selected() || decoder_.Services() != 127u))) return false;
    auto original = capture(kAddress);
    if (!original || !install(kAddress, [this](auto& c, auto at) { Run(c, at); })) return false;
    original_ = std::move(original);
    selected_ = selected;
    direct_presentation_ = direct_presentation;
    installed_ = true;
    return true;
}

[[noreturn]] void MovieStart::Fail(const char* why) {
    failed_ = true;
    throw std::runtime_error(why);
}

uint32_t MovieStart::Call(CpuContext& c, uint32_t address, uint32_t caller,
                          std::initializer_list<uint32_t> args) {
    const auto saved = c.cpuRegs.GPR;
    unsigned reg = 4;
    for (uint32_t arg : args) c.cpuRegs.GPR.r[reg++].SD[0] = int32_t(arg);
    c.cpuRegs.GPR.r[31].UL[0] = caller;
    try { call_(c, address); }
    catch (...) { c.cpuRegs.GPR = saved; throw; }
    const uint32_t result = c.cpuRegs.GPR.r[2].UL[0];
    c.cpuRegs.GPR = saved;
    return result;
}

bool MovieStart::Open(CpuContext& c, uint32_t player) {
    // Selected WaitFrame already calls the complete game presentation.
    // Remove its dormant interrupt registration only with the paired Stop
    // owner; the earlier Start-only selection keeps its original dependency.
    if (!direct_presentation_) {
        Call(c, 0x2d4dc0, 0x2ae33c, {});
        Call(c, 0x2cfc68, 0x2ae344, {2});
        Call(c, 0x2c1e10, 0x2ae350, {0x2af568});
        Call(c, 0x2cfcd0, 0x2ae358, {2});
        Call(c, 0x2d4e10, 0x2ae360, {});
    }
    memory_.Write32(player + 0x40, 0);
    memory_.Write32(player + 0x1c, memory_.Read32(0x30a780) << 5);
    // The audited Player vtable's LendSound operation stays a game decision.
    Call(c, 0x2af9b8, 0x2ae388, {player});
    memory_.Write32(player + 0x2c, 1);
    memory_.Write32(player + 0x30, 2);
    memory_.Write32(player + 0x34, 1);
    if (!Call(c, 0x2ae1c8, 0x2ae3a0, {player})) return false;
    Call(c, 0x2ae860, 0x2ae3b0, {player});
    const uint32_t sp = c.cpuRegs.GPR.r[29].UL[0];
    memory_.Write32(sp, 1);
    memory_.Write32(sp + 8, memory_.Read32(player + 4) >> 6);
    memory_.Write32(sp + 16, 0x100);
    const uint32_t opened = Call(c, 0x2b0dc8, 0x2ae3f8,
        {player + 0x80, 1, 2, 1, memory_.Read32(player) & 0x3f,
         MovieDecoder::kSeekStream, MovieDecoder::kReadStream, memory_.Read32(player + 0xc)});
    memory_.Write32(player + 0x1e0, opened);
    if (opened != 1) return false;
    for (uint32_t caller : {0x2ae458u, 0x2ae528u}) {
        c.cpuRegs.GPR.r[4].UD[0] = player + 0x44;
        c.cpuRegs.GPR.r[5].UD[0] = memory_.Read32(player + 0x1c);
        c.cpuRegs.GPR.r[31].UL[0] = caller;
        pictures_.DecodeAndUpload(c);
        ++primed_;
        if (caller == 0x2ae458) pictures_.QueueDecoded(player);
    }
    for (;;) {
        c.cpuRegs.GPR.r[31].UL[0] = 0x2ae584;
        decoder_.Finished(c);
        if (c.cpuRegs.GPR.r[2].UL[0]) break;
        const auto mode = memory_.Read32(MovieDecoder::kSoundMode);
        if (!mode || (mode == 1 &&
            int32_t(memory_.Read32(0x30a948)) >= int32_t(memory_.Read32(MovieDecoder::kSoundRingSize)))) break;
        if (mode != 1) Fail("Movie Start encountered a foreign PCM mode");
        Call(c, 0x2b0fe0, 0x2ae570, {});
        c.cpuRegs.GPR.r[4].UD[0] = MovieDecoder::kPlayerFeedSound;
        c.cpuRegs.GPR.r[5].UD[0] = 0;
        c.cpuRegs.GPR.r[31].UL[0] = 0x2ae57c;
        decoder_.FeedAudio(c);
    }
    Call(c, 0x1e60a8, 0x2ae5cc, {3});
    if (memory_.Read32(MovieDecoder::kSoundMode) == 1) {
        const float volume = c.fpuRegs.fpr[0].f;
        const auto ring = memory_.Read32(MovieDecoder::kSoundRing);
        const int32_t bytes = int32_t(memory_.Read32(MovieDecoder::kSoundRingSize));
        if (bytes < 0) Fail("Movie Start has a negative owned ring size");
        Call(c, MovieDecoder::kSoundRemote, 0x2ae620,
             {1, 0x80e0, 0, 0x13, ring, uint32_t(bytes) & ~1023u, ring});
        c.fpuRegs.fpr[12].f = volume;
        c.cpuRegs.GPR.r[31].UL[0] = 0x2ae628;
        decoder_.SetVolume(c);
        memory_.Write32(MovieDecoder::kSoundStarted, 1);
    }
    return true;
}

void MovieStart::Run(CpuContext& c, uint32_t entry) {
    if (!selected_) { ++original_calls_; original_(c, entry); return; }
    if (active_ || failed_ || (entry && entry != kAddress))
        Fail("Movie Start requires a complete, nonrecursive service boundary");
    const auto player = c.cpuRegs.GPR.r[4].UL[0], stack = c.cpuRegs.GPR.r[29].UL[0];
    if (c.cpuRegs.GPR.r[28].UL[0] != 0x3114f0 ||
        (direct_presentation_ && !(c.cpuRegs.CP0.n.Status & 1)) || !player ||
        player > UINT32_MAX - 0x4200 || !memory_.Translate(player, 0x4200) ||
        stack < 0x50 || !memory_.Translate(stack - 0x50, 0x50) ||
        memory_.Read32(player + 0x18) != 0x3068e0 ||
        memory_.Read32(0x306920) != 0 || memory_.Read32(0x306924) != 0x2af9b8 ||
        !decoder_.CanCheckpoint() || pictures_.Active())
        Fail("Movie Start has no matching closed NTSC player and service ABI");
    const auto saved = c.cpuRegs.GPR;
    const auto f20 = c.fpuRegs.fpr[20];
    active_ = true;
    c.cpuRegs.GPR.r[29].UD[0] = stack - 0x50;
    c.cpuRegs.GPR.r[16].UD[0] = player;
    c.cpuRegs.GPR.r[17].UD[0] = 1;
    bool opened = false;
    try { ++starts_; opened = Open(c, player); }
    catch (...) {
        c.cpuRegs.GPR = saved; c.fpuRegs.fpr[20] = f20;
        active_ = false; failed_ = true;
        throw;
    }
    if (opened) ++opened_; else ++open_failed_;
    c.cpuRegs.GPR = saved; c.fpuRegs.fpr[20] = f20;
    c.cpuRegs.GPR.r[2].SD[0] = opened ? 1 : 0;
    active_ = false;
}

bool MovieStart::CanCheckpoint(std::string* reason) const {
    if (!active_ && !failed_) return true;
    if (reason) *reason = "Movie Start is active or has failed";
    return false;
}
std::string MovieStart::Census() const {
    return "selected=" + std::to_string(selected_) + ",direct_presentation=" + std::to_string(direct_presentation_) +
        ",original_calls=" + std::to_string(original_calls_) +
        ",starts=" + std::to_string(starts_) + ",opened=" + std::to_string(opened_) +
        ",open_failed=" + std::to_string(open_failed_) + ",primes=" + std::to_string(primed_) +
        ",failed=" + std::to_string(failed_);
}
void MovieStart::Record(ps2r::verify::RunMeta& meta) const { meta.Set("movie_start", Census()); }
}  // namespace title
