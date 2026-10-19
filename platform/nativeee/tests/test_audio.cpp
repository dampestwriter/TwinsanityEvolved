// test_audio.cpp -- audio.h's façade (te::audio::AudioService): sounds from banks on voices, groups, pause/resume,
// release, music streams (mono and interleaved pair) read from a synthetic disc through the stream owner,
// the logical tick (MixTick / OnBoundary) and the device-less ring health, the ps2e_audio_* bodies.
#include <cmath>
#include <cstring>

#include "audio_service.h"
#include "ps2e_seam.h"
#include "stream_service.h"
#include "te_audio.h"
#include "te_test.h"
#include "te_test_adpcm.h"
#include "te_test_iso.h"

namespace {
using te::audio::AudioService;

double Energy(const std::vector<float>& frames) {
    double e = 0;
    for (float f : frames) e += double(f) * f;
    return e;
}

// A fresh audio owner (the services, the stream owner and the ISO stay: the music tests read from the disc).
AudioService& Fresh() {
    auto& a = te::audio::Service();
    a.Configure(false);
    a.Reset();
    return a;
}

void LoadBank(AudioService& a, uint32_t id, std::size_t blocks, uint8_t nibble, bool loop) {
    CHECK(a.StoreSoundBank(id, te_test::AdpcmBlocks(blocks, nibble, loop)));
    a.SoundBankLoaded(static_cast<uint16_t>(id));
}

void TestSamples() {
    auto& a = Fresh();
    a.ReserveSounds(0x100);
    LoadBank(a, 0x8000, 40, 0x3, false);   // 40 blocks = 1120 samples at 48 kHz ~ 23 ms, once
    LoadBank(a, 0x8001, 4, 0x5, true);     // a looping 4-block tone
    CHECK_EQ(a.IsVoiceFree(5), 1);
    CHECK_EQ(a.IsVoiceFree(48), -1);
    CHECK_EQ(a.IsVoiceFree(-1), -1);

    // Play on voice 5 of group 1 at full volume: the tick carries energy, the voice is busy, then ends.
    CHECK_EQ(a.PlaySound(0x8000, 5 | (1u << 16), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1), 0);
    CHECK_EQ(a.IsVoiceFree(5), 0);
    std::vector<float> tick;
    a.MixTick(tick);
    CHECK_EQ(tick.size(), 1600u);          // 800 stereo frames at 60 Hz
    CHECK(Energy(tick) > 0.0);
    a.MixTick(tick);                       // 1600 frames > 1120 samples: finished
    CHECK_EQ(a.IsVoiceFree(5), 1);
    a.MixTick(tick);
    CHECK_EQ(Energy(tick), 0.0);
    CHECK_EQ(a.Count().plays, 1u);
    CHECK_EQ(a.Count().ticks, 3u);
    CHECK_EQ(a.Count().frames, 2400u);

    // Refusals: no such voice (-1), too many loops (-2), unknown sound (-1, counted).
    CHECK_EQ(a.PlaySound(0x8000, 48, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1), -1);
    CHECK_EQ(a.PlaySound(0x8000, 1, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 0x10000), -2);
    CHECK_EQ(a.PlaySound(0x1234, 1, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 1), -1);
    CHECK_EQ(a.Count().play_refused, 3u);

    // A looping sound plays until released: ReleaseVoice fades it out at the rate.
    CHECK_EQ(a.PlaySound(0x8001, 7, 0x3FFF, 0x3FFF, 0x1000, 0, 0, 0), 0);
    for (int i = 0; i < 5; ++i) a.MixTick(tick);
    CHECK(Energy(tick) > 0.0);
    CHECK_EQ(a.IsVoiceFree(7), 0);
    a.ReleaseVoice(7, 0);                   // rate 0: the fastest release
    a.MixTick(tick);
    a.MixTick(tick);
    CHECK_EQ(a.IsVoiceFree(7), 1);

    // Volume 0 is silent, pitch halves the rate, group volume scales.
    CHECK_EQ(a.PlaySound(0x8001, 8 | (2u << 16), 0, 0, 0x1000, 0, 0, 0), 0);
    a.MixTick(tick);
    CHECK_EQ(Energy(tick), 0.0);
    CHECK_EQ(a.SetVoiceVolume(8, 0x3FFF, 0x3FFF), 0);
    for (int i = 0; i < 3; ++i) a.MixTick(tick);   // ramps settle
    const double full = Energy(tick);
    CHECK(full > 0.0);
    a.SetGroupVolume(2 << 16, 0x800, 0x800);        // half
    for (int i = 0; i < 3; ++i) a.MixTick(tick);
    const double half = Energy(tick);
    CHECK(half > 0.0 && half < full * 0.6);
    CHECK_EQ(a.SetVoicePitch(8, 0x800), 0);
    CHECK_EQ(a.SetVoiceVolume(48, 1, 1), -1);
    CHECK_EQ(a.SetVoicePitch(48, 1), -1);

    // Pause holds the playing voice (silence), resume brings it back at its stored volume.
    a.PauseAll();
    a.MixTick(tick);
    CHECK_EQ(Energy(tick), 0.0);
    CHECK_EQ(a.IsVoiceFree(8), 0);
    a.ResumeAll();
    a.MixTick(tick);
    CHECK(Energy(tick) > 0.0);

    // Reverb is recorded, never fails.
    a.SetVoiceReverb(8, true);
    a.SetReverb(0, 3, 0x1000, 0x1000, 0, 0);
    a.SetReverbVolume(0, 0x1000, 0x1000);
    a.ClearReverb(0);
    CHECK_EQ(a.Count().reverb_calls, 4u);
    a.CompactSoundMemory();
    CHECK_EQ(a.LendToMovie(), 0);
    CHECK_EQ(a.ReclaimFromMovie(), 0);

    // Reset: every voice off, banks kept.
    a.Reset();
    CHECK_EQ(a.IsVoiceFree(8), 1);
    CHECK(a.Audio().HasSound(0x8001));
    a.ReleaseSound(0x8001);
    CHECK(!a.Audio().HasSound(0x8001));
    CHECK_EQ(a.PitchOfRate(48000), 0x1000);
    CHECK_EQ(a.PitchOfRate(24000), 0x800);
    CHECK_EQ(a.PitchOfRate(44100), 0xEB3);
}

// The title's music sequence (game/src/game/sound.cpp StartMusic/PlayMusic/StopMusic) over the stream owner.
void TestMusicMono(int32_t file, uint32_t offset, uint32_t size) {
    auto& a = Fresh();
    a.ReadMusic(file, offset, size);
    const uint32_t vg = 20 | (3u << 16);
    CHECK_EQ(a.StreamMusic(file, 4, vg, a.PitchOfRate(48000), false), 4);
    CHECK_EQ(a.IsVoiceFree(20), 0);          // reserved for the stream
    a.PrepareMusic(4);                         // preload without starting
    CHECK(a.IsMusicReady(4));
    CHECK(a.IsMusicPlaying(4));                // the stream is on (prepared), as MsGetStreamStatus's state
    std::vector<float> tick;
    a.MixTick(tick);
    CHECK_EQ(Energy(tick), 0.0);               // not keyed on yet
    CHECK_EQ(a.PlayMusic(4), 0);
    CHECK_EQ(a.SetVoiceVolume(20, 0x3FFF, 0x3FFF), 0);
    for (int i = 0; i < 3; ++i) a.MixTick(tick);
    CHECK(Energy(tick) > 0.0);
    CHECK(a.IsMusicPlaying(4));
    CHECK_EQ(a.Count().music_plays, 1u);
    CHECK_EQ(a.Count().music_reads, 1u);
    CHECK_EQ(a.Count().music_read_bytes, size);
    CHECK_EQ(a.Count().music_starts, 1u);
    // Looping: plays on well past its length (320 bytes = 560 samples).
    for (int i = 0; i < 10; ++i) a.MixTick(tick);
    CHECK(Energy(tick) > 0.0);
    CHECK(a.IsMusicPlaying(4));
    a.StopMusic(4);
    CHECK(!a.IsMusicPlaying(4));
    CHECK_EQ(a.IsVoiceFree(20), 1);
    a.MixTick(tick);
    CHECK_EQ(Energy(tick), 0.0);
    CHECK_EQ(a.Count().music_stops, 1u);

    // Once-through: ends on its own; IsMusicPlaying then says so.
    CHECK_EQ(a.StreamMusic(file, 4, vg, 0x1000, true), 4);
    a.PrepareMusic(4);
    CHECK_EQ(a.PlayMusic(4), 0);
    a.SetVoiceVolume(20, 0x3FFF, 0x3FFF);
    CHECK(a.IsMusicPlaying(4));
    CHECK_EQ(a.IsVoiceFree(20), 0);
    a.MixTick(tick);                           // 800 frames > 560 samples: over within the tick
    CHECK(Energy(tick) > 0.0);
    CHECK(!a.IsMusicPlaying(4));
    CHECK_EQ(a.IsVoiceFree(20), 1);
    a.StopMusic(4);

    // Without a window the stream fails cleanly.
    CHECK_EQ(a.StreamMusic(200, 5, 21, 0x1000, false), -1);
    CHECK(!a.IsMusicReady(5));
    CHECK(a.Count().music_errors >= 1);
    // Bad arguments.
    CHECK_EQ(a.StreamMusic(file, 48, 21, 0x1000, false), -1);
    CHECK_EQ(a.StreamMusic(file, 6, 48, 0x1000, false), -1);
    a.StopMusic(99);
    CHECK(!a.IsMusicPlaying(-1));
}

// A speech line: an MSVp-headed clip keys on at the HEADER's sample rate, not at the pitch the game passed (the speech
// banks' .mh table says 32000 for every line; the clips are 32 / 48 / 18 kHz) -- STREAM.IRX 0x4c14's rule. The game's
// explicit SetVoicePitch after the key-on is applied as sent; a headerless (interleaved) stream keeps the requested pitch.
void TestMusicHeaderRate(int32_t file, uint32_t offset48, uint32_t size48, uint32_t offset24, uint32_t size24) {
    auto& a = Fresh();
    const uint32_t vg = 24 | (3u << 16);
    const uint16_t table_pitch = static_cast<uint16_t>(a.PitchOfRate(32000));   // 0xAAA, the .mh table's rate
    std::vector<float> tick;
    // 48 kHz clip asked for at the table's 32 kHz pitch: keys on at 0x1000.
    a.ReadMusic(file, offset48, size48);
    CHECK_EQ(a.StreamMusic(file, 6, vg, table_pitch, true), 6);
    a.PrepareMusic(6);
    CHECK_EQ(a.Audio().Inspect().voices[24].player.Capture().parameters.pitch, table_pitch);   // reserved at the request
    CHECK_EQ(a.PlayMusic(6), 0);
    CHECK_EQ(a.Audio().Inspect().voices[24].player.Capture().parameters.pitch, uint16_t(0x1000));
    a.SetVoiceVolume(24, 0x3FFF, 0x3FFF);
    // 40 blocks = 1120 samples at 48 kHz: over inside two ticks (1600 frames); at the table's pitch it would run 1680.
    a.MixTick(tick);
    CHECK(Energy(tick) > 0.0);
    CHECK(a.IsMusicPlaying(6));
    a.MixTick(tick);
    CHECK(!a.IsMusicPlaying(6));
    a.StopMusic(6);
    // 24 kHz clip: keys on at 0x800 and lasts twice as long (1120 samples = 2240 frames: three ticks).
    a.ReadMusic(file, offset24, size24);
    CHECK_EQ(a.StreamMusic(file, 6, vg, table_pitch, true), 6);
    a.PrepareMusic(6);
    CHECK_EQ(a.PlayMusic(6), 0);
    CHECK_EQ(a.Audio().Inspect().voices[24].player.Capture().parameters.pitch, uint16_t(0x800));
    a.SetVoiceVolume(24, 0x3FFF, 0x3FFF);
    a.MixTick(tick); a.MixTick(tick);
    CHECK(a.IsMusicPlaying(6));
    // The game's explicit pitch after the key-on stands (MsSetChannelPitch applies the EE's value).
    CHECK_EQ(a.SetVoicePitch(24, 0x1000), 0);
    CHECK_EQ(a.Audio().Inspect().voices[24].player.Capture().parameters.pitch, uint16_t(0x1000));
    a.MixTick(tick);
    CHECK(!a.IsMusicPlaying(6));
    a.StopMusic(6);
}

void TestMusicInterleaved(int32_t file, uint32_t offset, uint32_t size, uint32_t block, uint32_t end) {
    auto& a = Fresh();
    const auto before = a.Count();   // counters are the process's, not reset by the game's Audio::Reset
    a.ReadMusic(file, offset, size);
    const uint32_t parent_vg = 22 | (3u << 16), child_vg = 23 | (3u << 16);
    CHECK_EQ(a.StreamMusic(file, 4, parent_vg, 0x1000, false), 4);
    a.InterleaveMusic(4, block);
    a.AddMusicChannel(5, 4, child_vg, 0x9010);
    a.SetMusicEnd(4, end);
    a.PrepareMusic(4);
    CHECK(a.IsMusicReady(4));
    CHECK_EQ(a.IsVoiceFree(22), 0);
    CHECK_EQ(a.IsVoiceFree(23), 0);
    CHECK_EQ(a.PlayMusic(4), 0);
    // The game pans the pair: first voice left, second right.
    CHECK_EQ(a.SetVoiceVolume(22, 0x3FFF, 0), 0);
    CHECK_EQ(a.SetVoiceVolume(23, 0, 0x3FFF), 0);
    std::vector<float> tick;
    for (int i = 0; i < 3; ++i) a.MixTick(tick);
    double left = 0, right = 0;
    for (std::size_t i = 0; i < tick.size(); i += 2) { left += std::fabs(tick[i]); right += std::fabs(tick[i + 1]); }
    CHECK(left > 0.0);
    CHECK(right > 0.0);
    CHECK(a.IsMusicPlaying(4));
    CHECK(a.IsMusicPlaying(5));                // the child reports its parent's state
    CHECK_EQ(a.Count().music_starts, before.music_starts + 1);
    // Stopping the pair frees both voices.
    a.StopMusic(4);
    CHECK(!a.IsMusicPlaying(4));
    CHECK_EQ(a.IsVoiceFree(22), 1);
    CHECK_EQ(a.IsVoiceFree(23), 1);
    // An interleave whose blocks do not divide the window fails cleanly (counted), both voices freed.
    CHECK_EQ(a.StreamMusic(file, 4, parent_vg, 0x1000, false), 4);
    a.InterleaveMusic(4, block + 16);
    a.AddMusicChannel(5, 4, child_vg, 0x9010);
    a.PrepareMusic(4);
    CHECK(!a.IsMusicReady(4));
    CHECK(!a.IsMusicPlaying(4));
    CHECK(a.Count().music_errors > before.music_errors);
    a.StopMusic(4);
}

void TestBoundaryAndHealth() {
    auto& a = Fresh();
    const auto before = a.Count();
    // No device: OnBoundary mixes and counts, the ring is untouched (nothing pushed, dropped or underrun).
    a.SetBoundaryHz(60);
    for (int i = 0; i < 5; ++i) te::audio::OnBoundary();
    CHECK_EQ(a.Count().ticks, before.ticks + 5);
    CHECK_EQ(a.Count().frames, before.frames + 4000);
    const auto h = te::audio::Health();
    CHECK(!h.device);
    CHECK_EQ(h.pushed, 0u);
    CHECK_EQ(h.dropped, 0u);
    CHECK_EQ(h.underruns, 0u);
    // 50 Hz: 960 frames per tick; other rates are refused.
    a.SetBoundaryHz(50);
    std::vector<float> tick;
    a.MixTick(tick);
    CHECK_EQ(tick.size(), 1920u);
    a.SetBoundaryHz(30);
    CHECK_EQ(a.BoundaryHz(), 50u);
    a.SetBoundaryHz(60);
    // InitialiseRemote without a device configured: 0, no device.
    CHECK_EQ(a.InitialiseRemote(), 0);
    CHECK(!a.DeviceOpen());
    CHECK_EQ(ps2e_sound_initialise_remote(), 0);
}

void TestSeamBodies() {
    auto& a = Fresh();
    ps2e_audio_reserve_sounds(0x100);
    LoadBank(a, 0x8002, 8, 0x4, true);
    CHECK_EQ(ps2e_audio_is_voice_free(9), 1);
    CHECK_EQ(ps2e_audio_play_sound(0x8002, 9 | (1 << 16), 0x3FFF, 0x3FFF, 0x1000, 0, 0, 0), 0);
    CHECK_EQ(ps2e_audio_is_voice_free(9), 0);
    CHECK_EQ(ps2e_audio_set_voice_volume(9, 0x2000, 0x2000), 0);
    CHECK_EQ(ps2e_audio_set_voice_pitch(9, 0x1000), 0);
    ps2e_audio_set_group_volume(1 << 16, 0x1000, 0x1000);
    ps2e_audio_pause_all();
    ps2e_audio_resume_all();
    ps2e_audio_release_voice(9, 0);
    std::vector<float> tick;
    a.MixTick(tick); a.MixTick(tick);
    CHECK_EQ(ps2e_audio_is_voice_free(9), 1);
    CHECK_EQ(ps2e_audio_pitch_of_rate(48000), 0x1000);
    CHECK_EQ(ps2e_audio_reserve_sound_memory(16) + 16, ps2e_audio_reserve_sound_memory(16));
    ps2e_audio_release_sound(0x8002);
    CHECK(!a.Audio().HasSound(0x8002));
    ps2e_audio_reset();
    // Scalarised u16 pitch / s16 volumes narrow back (0xFFFF pitch stays 0xFFFF, not -1).
    LoadBank(a, 0x8003, 8, 0x4, true);
    CHECK_EQ(ps2e_audio_play_sound(0x8003, 10, 0x3FFF, 0x3FFF, 0xFFFF, 0, 0, 0), 0);
    CHECK_EQ(a.Audio().Inspect().voices[10].player.Capture().parameters.pitch, 0xFFFF);
    ps2e_audio_stop_music(99);
    CHECK_EQ(ps2e_audio_is_music_playing(99), 0);
}
}  // namespace

int main() {
    // The disc: a music file with a mono track (20 blocks, a VAGp header in front as the title's .mb tracks have)
    // and an interleaved pair (2 tracks x 2 pairs of 64-byte blocks, the last block 48 bytes long).
    std::vector<uint8_t> music;
    std::vector<uint8_t> header(48, 0);
    std::memcpy(header.data(), "VAGp", 4);
    const std::vector<uint8_t> mono = te_test::AdpcmBlocks(20, 0x3, false);
    const uint32_t mono_offset = 0;
    music.insert(music.end(), header.begin(), header.end());
    music.insert(music.end(), mono.begin(), mono.end());
    const uint32_t mono_size = static_cast<uint32_t>(music.size());
    const uint32_t block = 64, end = 48;
    const uint32_t pair_offset = mono_size;
    const std::vector<uint8_t> left = te_test::AdpcmBlocks(8, 0x3, false), right = te_test::AdpcmBlocks(8, 0x5, false);
    for (uint32_t pair = 0; pair < 2; ++pair) {
        music.insert(music.end(), left.begin() + pair * block, left.begin() + (pair + 1) * block);
        music.insert(music.end(), right.begin() + pair * block, right.begin() + (pair + 1) * block);
    }
    const uint32_t pair_size = static_cast<uint32_t>(music.size()) - pair_offset;
    // Two speech-like clips with an MSVp header naming their sample rate (big-endian at +16): 48000 and 24000 Hz.
    auto headed = [&](uint32_t rate) {
        std::vector<uint8_t> h(48, 0);
        std::memcpy(h.data(), "MSVp", 4);
        h[16] = uint8_t(rate >> 24); h[17] = uint8_t(rate >> 16); h[18] = uint8_t(rate >> 8); h[19] = uint8_t(rate);
        const std::vector<uint8_t> blocks = te_test::AdpcmBlocks(40, 0x3, false);
        const uint32_t at = static_cast<uint32_t>(music.size());
        music.insert(music.end(), h.begin(), h.end());
        music.insert(music.end(), blocks.begin(), blocks.end());
        return std::pair<uint32_t, uint32_t>{at, static_cast<uint32_t>(music.size()) - at};
    };
    const auto clip48 = headed(48000), clip24 = headed(24000);
    const std::string iso = te_test::WriteTestIso({{"CRASH6", "MUSIC.MB", music}});
    if (iso.empty()) { std::fprintf(stderr, "cannot write the test ISO\n"); return 2; }
    te::Settings settings = te_test::DefaultSettings();
    settings.iso_path = iso;
    te_test::Reset(settings);
    auto& s = te::stream::Service();
    s.Reset();
    s.Initialise(9, 0);
    const int32_t file = s.OpenFile("Crash6/Music.mb");
    CHECK_EQ(s.FileSize(), static_cast<uint32_t>(music.size()));

    TestSamples();
    TestMusicMono(file, mono_offset, mono_size);
    TestMusicInterleaved(file, pair_offset, pair_size, block, end);
    TestMusicHeaderRate(file, clip48.first, clip48.second, clip24.first, clip24.second);
    TestBoundaryAndHealth();
    TestSeamBodies();

    s.CloseFile(file);
    std::remove(iso.c_str());
    return te_test::Finish("test_audio");
}
