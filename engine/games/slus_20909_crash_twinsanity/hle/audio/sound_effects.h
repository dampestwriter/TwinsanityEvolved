#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace title {

// The effects / global-controls owner: the title's MultiStream client sets
// the two SPU2 cores' reverb (mode, depth, delay, feedback: TR commands.cpp
// MsSetEffect / MsClearEffect / MsSetEffectVolume) and routes voices to it
// (MsEffectSendOn/Off), and resets the sound service (MsResetSound). The
// retained arm's libsd (engine/iop/hle/libsd) answered SdSetEffectAttr with
// nothing -- no reverb was ever mixed under this port -- so the owned output is
// unchanged: this owner records the title's effect settings as its state (the
// census and a future host effects bus read them) and owns the reset of that
// state. It never fabricates an audible effect it does not mix.
class SoundEffects {
public:
    static constexpr uint32_t kCores = 2, kVoices = 48;
    struct Core {
        int32_t mode = 0;          // 0 off .. 9 (the SPU2's reverb modes)
        uint16_t depth_left = 0, depth_right = 0, delay = 0, feedback = 0;
        uint16_t volume_left = 0, volume_right = 0;
    };
    struct State {
        std::array<Core, kCores> cores{};
        std::array<bool, kVoices> send{};   // per voice: routed to the core's effect
    };

    static bool Install(bool selected);
    static bool Enabled();
    static void Reset();   // MsResetSound (0x7): every setting cleared, every send off

    // The commands, as the batch carries them (word arguments).
    static bool SetEffect(uint32_t core, int32_t mode, uint16_t depth_left, uint16_t depth_right,
                          uint16_t delay, uint16_t feedback);
    static bool ClearEffect(uint32_t core);
    static bool SetVolume(uint32_t core, uint16_t left, uint16_t right);
    static bool SetSend(uint32_t voice, bool on);

    static const State& Inspect();
    struct Counters {
        uint64_t sets = 0, clears = 0, volumes = 0, sends = 0, resets = 0, errors = 0;
    };
    static Counters Count();
    // `installed=1,bus=none,sets=..,clears=..,volumes=..,sends=..,resets=..,errors=..,core0=<mode>/<l>/<r>,core1=...`
    static std::string Census();
};

} // namespace title
