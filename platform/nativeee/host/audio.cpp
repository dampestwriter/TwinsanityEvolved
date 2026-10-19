// platform/nativeee (host side) -- audio.h: the ps2e_audio_* imports over te::audio::AudioService (audio_service.h).
// Scalarisation (ps2e_seam.h): u16/s16/bool/u32 arguments arrive as ps2e_s32 and are narrowed back here.
#include <cstdint>

#include "audio_service.h"
#include "ps2e_seam.h"

namespace {
inline te::audio::AudioService& A() { return te::audio::Service(); }
inline int16_t S16(ps2e_s32 v) { return static_cast<int16_t>(v); }
inline uint16_t U16(ps2e_s32 v) { return static_cast<uint16_t>(v); }
inline uint32_t U32(ps2e_s32 v) { return static_cast<uint32_t>(v); }
}  // namespace

extern "C" {

void ps2e_audio_reset(void) { A().Reset(); }
void ps2e_audio_pause_all(void) { A().PauseAll(); }
void ps2e_audio_resume_all(void) { A().ResumeAll(); }
ps2e_s32 ps2e_audio_lend_to_movie(void) { return A().LendToMovie(); }
ps2e_s32 ps2e_audio_reclaim_from_movie(void) { return A().ReclaimFromMovie(); }
void ps2e_audio_compact_sound_memory(void) { A().CompactSoundMemory(); }
void ps2e_audio_set_group_volume(ps2e_s32 group, ps2e_s32 left, ps2e_s32 right) { A().SetGroupVolume(group, U32(left), U32(right)); }
ps2e_s32 ps2e_audio_set_voice_volume(ps2e_s32 voice, ps2e_s32 left, ps2e_s32 right) { return A().SetVoiceVolume(voice, S16(left), S16(right)); }
ps2e_s32 ps2e_audio_set_voice_pitch(ps2e_s32 voice, ps2e_s32 pitch) { return A().SetVoicePitch(voice, U16(pitch)); }
void ps2e_audio_set_voice_reverb(ps2e_s32 voice, ps2e_s32 on) { A().SetVoiceReverb(voice, on != 0); }
void ps2e_audio_release_voice(ps2e_s32 voice, ps2e_s32 rate) { A().ReleaseVoice(voice, U32(rate)); }
ps2e_s32 ps2e_audio_is_voice_free(ps2e_s32 voice) { return A().IsVoiceFree(voice); }
void ps2e_audio_set_reverb(ps2e_s32 core, ps2e_s32 mode, ps2e_s32 depthLeft, ps2e_s32 depthRight, ps2e_s32 delay, ps2e_s32 feedback) {
    A().SetReverb(core, mode, U16(depthLeft), U16(depthRight), U16(delay), U16(feedback));
}
void ps2e_audio_clear_reverb(ps2e_s32 core) { A().ClearReverb(core); }
void ps2e_audio_set_reverb_volume(ps2e_s32 core, ps2e_s32 left, ps2e_s32 right) { A().SetReverbVolume(core, S16(left), S16(right)); }
ps2e_s32 ps2e_audio_reserve_sound_memory(ps2e_s32 size) { return static_cast<ps2e_s32>(A().ReserveSoundMemory(U32(size))); }
void ps2e_audio_sound_bank_loaded(ps2e_s32 bank) { A().SoundBankLoaded(U16(bank)); }
void ps2e_audio_reserve_sounds(ps2e_s32 count) { A().ReserveSounds(U16(count)); }
void ps2e_audio_release_sound(ps2e_s32 sound) { A().ReleaseSound(U32(sound)); }
ps2e_s32 ps2e_audio_play_sound(ps2e_s32 sound, ps2e_s32 voiceAndGroup, ps2e_s32 left, ps2e_s32 right, ps2e_s32 pitch, ps2e_s32 attack, ps2e_s32 release, ps2e_s32 loops) {
    return A().PlaySound(U32(sound), U32(voiceAndGroup), S16(left), S16(right), U16(pitch), U32(attack), U32(release), U32(loops));
}
ps2e_s32 ps2e_audio_pitch_of_rate(ps2e_s32 rate) { return A().PitchOfRate(rate); }
ps2e_s32 ps2e_audio_channel_buffer_address(ps2e_s32 channel) { return static_cast<ps2e_s32>(A().ChannelBufferAddress(channel)); }
void ps2e_audio_read_music(ps2e_s32 file, ps2e_s32 offset, ps2e_s32 size) { A().ReadMusic(file, U32(offset), U32(size)); }
ps2e_s32 ps2e_audio_stream_music(ps2e_s32 file, ps2e_s32 channel, ps2e_s32 voiceAndGroup, ps2e_s32 pitch, ps2e_s32 once) {
    return A().StreamMusic(file, channel, U32(voiceAndGroup), U16(pitch), once != 0);
}
void ps2e_audio_interleave_music(ps2e_s32 channel, ps2e_s32 blockSize) { A().InterleaveMusic(channel, U32(blockSize)); }
void ps2e_audio_add_music_channel(ps2e_s32 child, ps2e_s32 parent, ps2e_s32 voiceAndGroup, ps2e_s32 soundAddress) {
    A().AddMusicChannel(child, parent, U32(voiceAndGroup), U32(soundAddress));
}
void ps2e_audio_set_music_end(ps2e_s32 channel, ps2e_s32 end) { A().SetMusicEnd(channel, U32(end)); }
void ps2e_audio_prepare_music(ps2e_s32 channel) { A().PrepareMusic(channel); }
ps2e_s32 ps2e_audio_is_music_ready(ps2e_s32 channel) { return A().IsMusicReady(channel) ? 1 : 0; }
ps2e_s32 ps2e_audio_play_music(ps2e_s32 channel) { return A().PlayMusic(channel); }
ps2e_s32 ps2e_audio_is_music_playing(ps2e_s32 channel) { return A().IsMusicPlaying(channel) ? 1 : 0; }
void ps2e_audio_stop_music(ps2e_s32 channel) { A().StopMusic(channel); }

}  // extern "C"
