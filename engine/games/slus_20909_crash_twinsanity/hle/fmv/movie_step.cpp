#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_step.h"

#include <stdexcept>

#include "engine/core/verify/run_meta.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
MovieStep::MovieStep(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder, MovieGraphics& pictures)
    : memory_(memory), decoder_(decoder), pictures_(pictures) {}

bool MovieStep::Install(const MovieDecoder::Capture& capture,
                        const MovieDecoder::Installer& install, bool selected) {
    if (installed_ || !capture || !install ||
        (selected && (!pictures_.Selected() || (decoder_.Services() & 9u) != 9u))) return false;
    auto original = capture(kAddress);
    if (!original || !install(kAddress, [this](auto& c, auto at) { Run(c, at); })) return false;
    original_ = std::move(original);
    selected_ = selected;
    installed_ = true;
    return true;
}

[[noreturn]] void MovieStep::Fail(const char* why) {
    failed_ = true;
    throw std::runtime_error(why);
}

void MovieStep::Run(CpuContext& context, uint32_t entry) {
    if (!selected_) {
        ++original_calls_;
        original_(context, entry);
        return;
    }
    if (failed_ || active_ || (entry && entry != kAddress))
        Fail("Movie Step requires a complete, nonrecursive source service boundary");
    const uint32_t player = context.cpuRegs.GPR.r[4].UL[0];
    if (!player || player > UINT32_MAX - 0x1e8 || !memory_.Translate(player, 0x1e8) ||
        memory_.Read32(MovieDecoder::kDecoderPointer) != player + 0x80 ||
        memory_.Read32(player + 0x80 + 0x148) != 2 ||
        memory_.Read32(player + 0x80 + 0x154) != 0x100)
        Fail("Movie Step has no matching software RGB32 player");

    const auto saved = context.cpuRegs.GPR;
    active_ = true;
    int32_t result = 0;
    try {
        ++steps_;
        context.cpuRegs.GPR.r[31].UL[0] = 0x2aea1c;
        decoder_.Finished(context);
        if (context.cpuRegs.GPR.r[2].UL[0]) {
            ++finished_;
        } else {
            // Validate the source's already software-fed PCM arm before
            // publishing a queue change. Never delegate opaque picture IDs
            // or a nonempty foreign audio ring to retained transport.
            const uint32_t decoder = player + 0x80;
            const auto audio = memory_.Read32(decoder + 0x14c);
            if (audio && (audio != 1 || memory_.Read32(MovieDecoder::kSoundMode) != 1 ||
                memory_.Read32(decoder + 0x110) != 0 || !memory_.Read32(decoder + 0x108) ||
                memory_.Read32(decoder + 0x108) > 0x7fffffff ||
                memory_.Read32(decoder + 0x10c) >= memory_.Read32(decoder + 0x108)))
                Fail("Movie Step requires its software PCM consumer");
            if (pictures_.QueueDecoded(player)) {
                context.cpuRegs.GPR.r[4].UD[0] = player + 0x44;
                context.cpuRegs.GPR.r[5].UD[0] = memory_.Read32(player + 0x1c);
                context.cpuRegs.GPR.r[31].UL[0] = 0x2aead0;
                pictures_.DecodeAndUpload(context);
                context.cpuRegs.GPR.r[4].UD[0] = MovieDecoder::kPlayerFeedSound;
                context.cpuRegs.GPR.r[5].UD[0] = 0;
                context.cpuRegs.GPR.r[31].UL[0] = 0x2aeb10;
                decoder_.FeedAudio(context);
                memory_.Write32(player + 0x1e4, memory_.Read32(player + 0x1e4) + 1u);
                ++advanced_;
            } else {
                ++full_; // no decoded picture or both source queue slots busy
            }
            result = 1;
        }
    } catch (...) {
        context.cpuRegs.GPR = saved;
        active_ = false;
        failed_ = true;
        throw;
    }
    context.cpuRegs.GPR = saved;
    context.cpuRegs.GPR.r[2].SD[0] = result;
    active_ = false;
}

bool MovieStep::CanCheckpoint(std::string* reason) const {
    if (!active_ && !failed_) return true;
    if (reason) *reason = "Movie Step is active or has failed";
    return false;
}

std::string MovieStep::Census() const {
    return "selected=" + std::to_string(selected_) + ",original_calls=" + std::to_string(original_calls_) +
        ",steps=" + std::to_string(steps_) + ",advanced=" + std::to_string(advanced_) +
        ",waiting=" + std::to_string(full_) + ",finished=" + std::to_string(finished_) +
        ",failed=" + std::to_string(failed_);
}

void MovieStep::Record(ps2r::verify::RunMeta& meta) const {
    meta.Set("movie_step", Census());
}
}  // namespace title
