// platform/nativeee (host side) -- sound.h: Platform::Sound::InitialiseRemote, the game's connection to the
// sound driver (the PS2's SDRDRV over SIF). Here it is the moment the audio owner opens the output device
// (te::audio::AudioService::InitialiseRemote); the game then drives the sound through audio.h.
#include "audio_service.h"
#include "ps2e_seam.h"

extern "C" {

ps2e_s32 ps2e_sound_initialise_remote(void) { return te::audio::Service().InitialiseRemote(); }

}  // extern "C"
