// audio_service.h -- platform/nativeee's audio owner: Platform::Audio (audio.h) and Platform::Sound (sound.h) as a
// façade over the engine's title::Audio (the 48 SPU2 voices, 4 volume groups, sample registry, music voices --
// engine games/slus_20909_crash_twinsanity/hle/audio), mixed by the engine's sound::AudioMixer into the engine's
// AudioPullRing and played by its SdlAudioSink (engine/platform/sdl). Host-typed so tests drive it directly; the
// extern "C" ps2e_audio_* / ps2e_sound_* bodies in audio.cpp / sound.cpp are one-liners over Service().
//
// Clock: ONE logical tick per game boundary -- OnBoundary() runs as a hook of the services' te::Clock (the boundary
// Graphics::WaitVSync/Present stand on, te_services.h), registered at the game's first audio call; 48000 /
// BoundaryHz stereo frames (800 at 60 Hz, 960 at 50; te::Settings::field_hz). Nothing here reads wall time; the
// device thread only pulls the ring. Everything else runs on the game thread.
//
// What the PS2 had that is modelled only as bookkeeping: SPU RAM addresses (ReserveSoundMemory mints a cursor the
// game stores as the bank/music buffer "address"; a sound is keyed by its bank ID, never by an address) and the
// IOP-side music stream buffers (ChannelBufferAddress is the stream owner's opaque handle). A music / speech stream
// whose window begins with a VAGp/MSVp header keys on at the header's sample rate (STREAM.IRX 0x4c14's rule; the
// speech banks' .mh table names 32000 for clips that are 32 / 48 / 18 kHz), the game's later SetVoicePitch as sent.
// Reverb (SetReverb/
// ClearReverb/SetReverbVolume/SetVoiceReverb) is MISSING in the engine: recorded and counted, no effect.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/sound/audio_mixer.h"
#include "engine/sound/voice_source.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"

namespace ps2r::platform::sdl { class AudioPullRing; class SdlAudioSink; }

namespace te::audio {

class AudioService final : public ps2r::sound::VoiceSource {
public:
    static constexpr uint32_t kVoices = 48;
    static constexpr uint32_t kStreams = 48;        // music channels the game may name (stream.h channels)
    static constexpr uint32_t kSampleRate = 48000;

    AudioService();
    ~AudioService() override;

    // ----- device / clock ------------------------------------------------------------------------------------
    // Whether InitialiseRemote may open the SDL audio device (the runner's --no-audio and the tests say no).
    void Configure(bool open_device);
    // Platform::Sound::InitialiseRemote: connects to the "sound driver" = opens the device when configured.
    // Returns 0 (the SDRDRV init result the game expects), also when there is no device (sound then goes nowhere).
    // The PS2 platform called it from System::StartServices (the services owner's); Audio::Reset opens the device
    // too, so the game's first audio call suffices.
    int32_t InitialiseRemote();
    void CloseDevice();
    bool DeviceOpen() const;
    // 60 (default) or 50: frames per tick = 48000 / hz. Set by the clock/vsync owner before the first boundary.
    void SetBoundaryHz(uint32_t hz);
    uint32_t BoundaryHz() const { return hz_; }
    // One logical boundary: controls advance, one tick is mixed and pushed to the device ring. The clock's hook calls
    // it (HookClock); tests call it directly in place of a clock.
    void OnBoundary();
    // The same tick without a device (tests, offline hashing): the mixed stereo frames of one boundary.
    void MixTick(std::vector<float>& out);

    // ----- audio.h ---------------------------------------------------------------------------------------------
    void Reset();
    void PauseAll();
    void ResumeAll();
    int32_t LendToMovie();
    int32_t ReclaimFromMovie();
    void CompactSoundMemory();
    void SetGroupVolume(int32_t group, uint32_t left, uint32_t right);
    int32_t SetVoiceVolume(int32_t voice, int16_t left, int16_t right);
    int32_t SetVoicePitch(int32_t voice, uint16_t pitch);
    void SetVoiceReverb(int32_t voice, bool on);
    void ReleaseVoice(int32_t voice, uint32_t rate);
    int32_t IsVoiceFree(int32_t voice);
    void SetReverb(int32_t core, int32_t mode, uint16_t depthLeft, uint16_t depthRight, uint16_t delay, uint16_t feedback);
    void ClearReverb(int32_t core);
    void SetReverbVolume(int32_t core, int16_t left, int16_t right);
    uint32_t ReserveSoundMemory(uint32_t size);
    void SoundBankLoaded(uint16_t bank);
    void ReserveSounds(uint16_t count);
    void ReleaseSound(uint32_t sound);
    int32_t PlaySound(uint32_t sound, uint32_t voiceAndGroup, int16_t left, int16_t right, uint16_t pitch,
                      uint32_t attack, uint32_t release, uint32_t loops);
    int32_t PitchOfRate(int32_t rate);
    uint32_t ChannelBufferAddress(int32_t channel);
    void ReadMusic(int32_t file, uint32_t offset, uint32_t size);
    int32_t StreamMusic(int32_t file, int32_t channel, uint32_t voiceAndGroup, uint16_t pitch, bool once);
    void InterleaveMusic(int32_t channel, uint32_t blockSize);
    void AddMusicChannel(int32_t child, int32_t parent, uint32_t voiceAndGroup, uint32_t soundAddress);
    void SetMusicEnd(int32_t channel, uint32_t end);
    void PrepareMusic(int32_t channel);
    bool IsMusicReady(int32_t channel);
    int32_t PlayMusic(int32_t channel);
    bool IsMusicPlaying(int32_t channel);
    void StopMusic(int32_t channel);

    // ----- the stream owner's side (Platform::Stream::ReadSoundBank's delivery) -------------------------------
    // The bank's sample bytes (ADPCM blocks) as read from the disc: stored under the bank ID, ready after the
    // game's SoundBankLoaded. Returns false (counted) when the registry refuses them.
    bool StoreSoundBank(uint32_t bank, std::vector<uint8_t> bytes);
    // The current sound-memory cursor (where the next ReadSoundBank's bank "goes" on the PS2).
    uint32_t SoundMemoryCursor() const { return sound_memory_cursor_; }

    // ----- the movie owner's side (movie_player.cpp): the movie's PCM is the mixer's movie source ----------------
    void SetMovieSource(ps2r::sound::PcmSource* source) { mixer_.SetMovieSource(source); }
    // ----- inspection ------------------------------------------------------------------------------------------
    const title::Audio& Audio() const { return audio_; }
    title::Audio& Audio() { return audio_; }
    const ps2r::sound::AudioMixer& Mixer() const { return mixer_; }
    struct Counters {
        uint64_t ticks = 0, frames = 0, plays = 0, play_refused = 0, banks_stored = 0, banks_refused = 0,
                 bank_bytes = 0, music_plays = 0, music_starts = 0, music_stops = 0, music_reads = 0,
                 music_read_bytes = 0, music_errors = 0, reverb_calls = 0;
        double music_prepare_ms_max = 0.0;   // the longest PrepareMusic read + resolve on the game thread (a tick's cost)
        // The device ring's own counters (0 without a device).
        uint64_t ring_pushed = 0, ring_pulled = 0, ring_dropped = 0, ring_underruns = 0, ring_overruns = 0,
                 ring_padded = 0;
    };
    Counters Count() const;

    // Save state (savestate.h, kTagAudio): title::Audio's snapshot (AudioState codec: samples, voices, pending loads)
    // + this façade's music streams/windows and counters. Music windows are read from the disc again on restore.
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const std::vector<uint8_t>& payload, std::string* error);

    // VoiceSource (the mixer's view)
    void BeginMixTick() override;
    bool StepVoice(uint32_t channel, float& left, float& right) override;

private:
    enum class Phase : uint8_t { Idle, Reading, Prepared, Playing, Stopped, Failed };
    struct Window { uint32_t offset = 0, size = 0; bool set = false; };
    struct Stream {
        bool owned = false;
        uint32_t parent = UINT32_MAX, child = UINT32_MAX;   // a child names its parent; a parent its child
        int32_t file = -1;
        uint32_t voice_and_group = 0;
        uint16_t pitch = 0x1000;
        bool looping = true;
        uint32_t block = 0, end = 0, track = 0;
        uint32_t sound_address = 0;      // the child's (AddMusicChannel), bookkeeping only
        Window window;
        uint64_t generation = 0;
        bool key_on_disabled = false;    // PrepareMusic until PlayMusic
        bool issued = false;             // the file window was read
        Phase phase = Phase::Idle;
        title::MusicAsset::Data data;
    };
    void EnsureDevice();
    void HookClock();
    void IssueMusicRead(uint32_t parent);
    void StartMusic(uint32_t parent);
    void StopStream(uint32_t stream, Phase phase);
    uint32_t ParentOf(uint32_t stream) const;
    bool VoicePlaying(uint32_t voice) const;
    void RefreshMusicPhase(uint32_t parent);

    title::Audio audio_;
    ps2r::sound::AudioMixer mixer_{*this};
    std::unique_ptr<ps2r::platform::sdl::AudioPullRing> ring_;
    std::unique_ptr<ps2r::platform::sdl::SdlAudioSink> sink_;
    bool open_device_ = true, device_open_ = false;
    const void* hooked_clock_ = nullptr;   // the te::Clock the boundary hook is on
    uint32_t hz_ = 60;
    std::vector<float> tick_;
    uint32_t sound_memory_cursor_ = kSoundMemoryBase;
    std::array<Window, 256> windows_{};         // ReadMusic's window per file number (files are small ints)
    std::array<Stream, kStreams> streams_{};
    uint64_t generation_ = 0;
    Counters count_;
    static constexpr uint32_t kSoundMemoryBase = 0x5010;   // past the SPU2's reserved low 20 KB, as libsd leaves it
};

// The process-wide service the ps2e_* bodies use.
AudioService& Service();

}  // namespace te::audio
