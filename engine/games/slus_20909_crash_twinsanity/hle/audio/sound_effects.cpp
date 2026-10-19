#include "games/slus_20909_crash_twinsanity/hle/audio/sound_effects.h"

namespace title {
namespace {
bool g_installed = false;
SoundEffects::State g_state;
SoundEffects::Counters g_count;
constexpr int32_t kMaxMode = 9;   // the SPU2's reverb modes, TR commands.cpp MsSetEffect "below 10"
} // namespace

bool SoundEffects::Install(bool selected) {
    g_installed = selected;
    g_state = State{};
    g_count = Counters{};
    return true;
}
bool SoundEffects::Enabled() { return g_installed; }
void SoundEffects::Reset() {
    g_state = State{};
    ++g_count.resets;
}
bool SoundEffects::SetEffect(uint32_t core, int32_t mode, uint16_t depth_left, uint16_t depth_right,
                             uint16_t delay, uint16_t feedback) {
    if (core >= kCores || mode < 0 || mode > kMaxMode) { ++g_count.errors; return false; }
    auto& c = g_state.cores[core];
    c.mode = mode; c.depth_left = depth_left; c.depth_right = depth_right; c.delay = delay; c.feedback = feedback;
    ++g_count.sets;
    return true;
}
bool SoundEffects::ClearEffect(uint32_t core) {
    if (core >= kCores) { ++g_count.errors; return false; }
    auto& c = g_state.cores[core];
    c.mode = 0; c.depth_left = c.depth_right = c.delay = c.feedback = 0;
    ++g_count.clears;
    return true;
}
bool SoundEffects::SetVolume(uint32_t core, uint16_t left, uint16_t right) {
    if (core >= kCores) { ++g_count.errors; return false; }
    g_state.cores[core].volume_left = left;
    g_state.cores[core].volume_right = right;
    ++g_count.volumes;
    return true;
}
bool SoundEffects::SetSend(uint32_t voice, bool on) {
    if (voice >= kVoices) { ++g_count.errors; return false; }
    g_state.send[voice] = on;
    ++g_count.sends;
    return true;
}
const SoundEffects::State& SoundEffects::Inspect() { return g_state; }
SoundEffects::Counters SoundEffects::Count() { return g_count; }
std::string SoundEffects::Census() {
    std::string s = std::string("installed=") + (g_installed ? "1" : "0") + ",bus=none";
    s += ",sets=" + std::to_string(g_count.sets) + ",clears=" + std::to_string(g_count.clears);
    s += ",volumes=" + std::to_string(g_count.volumes) + ",sends=" + std::to_string(g_count.sends);
    s += ",resets=" + std::to_string(g_count.resets) + ",errors=" + std::to_string(g_count.errors);
    for (uint32_t core = 0; core < kCores; ++core) {
        const auto& c = g_state.cores[core];
        s += ",core" + std::to_string(core) + "=" + std::to_string(c.mode) + "/" + std::to_string(c.volume_left) + "/" +
             std::to_string(c.volume_right);
    }
    return s;
}

} // namespace title
