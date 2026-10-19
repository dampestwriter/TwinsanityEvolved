#pragma once
// CutsceneFastForward -- the A5 fast-forward decision for the title's in-engine
// cutscenes (Config qol.fast_forward = N; 1 = off). While the hold
// button is down and the title's video controller is PLAYING a cutscene, the
// boundary delivers N fields of elapsed time instead of one: the title's own
// clock (TR src/game/clock.cpp GameTimeSetStamp: the T0 count delta) steps the
// cutscene N frames per boundary -- the mechanism a late frame already uses
//So the camera, the instances and the music all keep pace.
// The boundary itself still fires once per 60 Hz wall edge (rule 1: game_hz
// stays 60); nothing here touches the deterministic profile (play mode only,
// live pad only).
//
// The title's state word: G_VideoController (PAL D_00309ADC -> NTSC 0x30973C,
// verified in the recompiled StartGame at 0x100bf8: `sw v0, -0x68c4(0x310000)`
// right after ConstructVideoController FUN_00299f90, 0x1C past
// G_GameMovieController at 0x309720 as in PAL) -> VideoController::state at +0x8
// (TR include/game/controllers.h: StatePlaying = 3; Paused = 4 is not played).
// The hold button is R2 (raw PS2 bit 9, active high): GameController::Watching
// reads only CROSS (skip) and the pause buttons during a cutscene.
#include <cstdint>
#include <functional>
#include <string>

namespace title {

class CutsceneFastForward {
public:
    // Reads one guest RAM word; false when the address is not readable.
    using Read32 = std::function<bool(uint32_t addr, uint32_t* value)>;

    static constexpr uint32_t kVideoControllerPtr = 0x0030973cu;
    static constexpr uint32_t kStateOffset = 0x8;
    static constexpr uint32_t kStatePlaying = 3;
    static constexpr uint16_t kHoldButton = 1u << 9;   // R2
    static constexpr uint32_t kRamBytes = 32u << 20;
    static constexpr int kFactorMax = 16;

    // factor: qol.fast_forward (<= 1 = off; clamped to kFactorMax).
    CutsceneFastForward(int factor, Read32 read32);

    bool Enabled() const { return factor_ > 1 && static_cast<bool>(read32_); }
    int Factor() const { return factor_; }
    // The boundary's pad serve (active-high buttons, SELECT bit 0 .. SQUARE bit 15).
    void NoteButtons(uint16_t buttons_active_high) { held_ = (buttons_active_high & kHoldButton) != 0; }
    bool Held() const { return held_; }
    // The title's video controller is playing a cutscene now.
    bool Playing() const;
    // Extra fields for this boundary: factor-1 while held and playing, else 0.
    uint32_t ExtraFields();

    uint64_t Boundaries() const { return boundaries_; }   // boundaries fast-forwarded
    uint64_t Fields() const { return fields_; }           // extra fields delivered
    // run.meta row: `fast_forward=factor=N,boundaries=B,fields=F,controller=0x..,state=S`
    // (`off` when 1; controller/state = the title's words now, for the address pin).
    std::string Census() const;

private:
    int factor_ = 1;
    Read32 read32_;
    bool held_ = false;
    uint64_t boundaries_ = 0;
    uint64_t fields_ = 0;
};

}  // namespace title
