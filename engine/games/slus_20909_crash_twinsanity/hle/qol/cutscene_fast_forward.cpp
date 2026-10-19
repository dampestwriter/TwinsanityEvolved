#include "games/slus_20909_crash_twinsanity/hle/qol/cutscene_fast_forward.h"

#include <cstdio>
#include <utility>

namespace title {

CutsceneFastForward::CutsceneFastForward(int factor, Read32 read32) : read32_(std::move(read32)) {
    factor_ = factor < 1 ? 1 : factor > kFactorMax ? kFactorMax : factor;
}

bool CutsceneFastForward::Playing() const {
    if (!read32_) return false;
    uint32_t controller = 0, state = 0;
    if (!read32_(kVideoControllerPtr, &controller)) return false;
    if (controller == 0 || controller + kStateOffset + 4 > kRamBytes) return false;   // not constructed yet / not RAM
    if (!read32_(controller + kStateOffset, &state)) return false;
    return state == kStatePlaying;
}

uint32_t CutsceneFastForward::ExtraFields() {
    if (!Enabled() || !held_ || !Playing()) return 0;
    const uint32_t extra = static_cast<uint32_t>(factor_ - 1);
    ++boundaries_;
    fields_ += extra;
    return extra;
}

std::string CutsceneFastForward::Census() const {
    if (factor_ <= 1) return "off";
    uint32_t controller = 0, state = 0;
    char words[48];
    if (read32_ && read32_(kVideoControllerPtr, &controller) && controller && controller + kStateOffset + 4 <= kRamBytes &&
        read32_(controller + kStateOffset, &state))
        std::snprintf(words, sizeof words, ",controller=0x%x,state=%u", controller, state);
    else
        std::snprintf(words, sizeof words, ",controller=0x%x,state=-", controller);
    return "factor=" + std::to_string(factor_) + ",boundaries=" + std::to_string(boundaries_) + ",fields=" + std::to_string(fields_) + words;
}

}  // namespace title
