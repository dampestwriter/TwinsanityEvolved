// audio_service.cpp -- see audio_service.h.
#include "audio_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "engine/platform/platform.h"
#include "engine/platform/sdl/audio_pull_ring.h"
#include "engine/platform/sdl/sdl_audio_sink.h"
#include "engine/sound/vag_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio_state.h"
#include "te_harness.h"
#include "movie_player.h"
#include "savestate.h"
#include "stream_service.h"
#include "te_audio.h"
#include "te_services.h"
#if defined(_WIN32)
#include <io.h>
#define te_write_fd _write
#elif defined(__vita__)
// The device has no stderr descriptor a reader sees (newlib's fd 2 is the kernel TTY); the census goes where every
// other line goes -- the runner's redirected stderr stream (runner/te_vita.cpp).
#include <cstdio>
static inline int te_write_fd(int, const char* buf, unsigned n) { return (int)std::fwrite(buf, 1, n, stderr); }
#else
#include <unistd.h>
#define te_write_fd write
#endif

namespace te::audio {
namespace {
constexpr uint32_t kGroupMax = 4;
void Once(const char* what, bool& said) {
    if (!said) { said = true; std::fprintf(stderr, "[twinsanityevolved] audio: %s\n", what); }
}
}  // namespace

AudioService::AudioService()
    : ring_(std::make_unique<ps2r::platform::sdl::AudioPullRing>()),
      sink_(std::make_unique<ps2r::platform::sdl::SdlAudioSink>()) {
    // The title's InitSpu intent: both cores at the same fixed output gain (engine app/host/sound_output.cpp).
    constexpr float gain = 16383.0f / 32767.0f;
    for (uint32_t group = 0; group < ps2r::sound::AudioMixer::kGroups; ++group) mixer_.SetGroup(group, gain, gain);
    // The clock contract's rule 4: a late game thread is padded with the previous tail, never a gap or a skip.
    ring_->SetPadWithTail(true);
    // TE_AUDIO_TAP=FILE: every buffer the device plays, appended as raw 48 kHz stereo float (diagnosis: what the speakers
    // got -- the pads of a late game thread included -- on any platform's device)
    if (const char* tap = std::getenv("TE_AUDIO_TAP")) {
        if (FILE* f = std::fopen(tap, "wb")) {
            sink_->SetTap([](void* user, const float* interleaved, int frames) {
                std::fwrite(interleaved, sizeof(float) * 2, size_t(frames), static_cast<FILE*>(user));
            }, f);
        }
    }
}

AudioService::~AudioService() { CloseDevice(); }

// ----- device / clock ------------------------------------------------------------------------------------------
void AudioService::Configure(bool open_device) { open_device_ = open_device; }

int32_t AudioService::InitialiseRemote() {
    EnsureDevice();
    return 0;   // sceSdRemoteInit's success
}

void AudioService::HookClock() {
    // The clock owns the boundary (te::Clock::Boundary from Graphics::WaitVSync/Present): one audio tick per boundary
    // runs as its hook. The services may be rebuilt (tests' ResetForTest): a new Clock gets the hook again.
    te::Clock& clock = te::Services::Get().clock();
    if (&clock == hooked_clock_) return;
    hooked_clock_ = &clock;
    SetBoundaryHz(te::GetSettings().field_hz);
    clock.AddBoundaryHook("audio mix", [this](uint64_t) { OnBoundary(); });
}

void AudioService::EnsureDevice() {
    HookClock();
    if (open_device_ && !device_open_) {
        ps2r::platform::AudioSpec spec;
        spec.sample_rate = static_cast<int>(kSampleRate);
        spec.channels = 2;
        spec.frames = static_cast<int>(kSampleRate / hz_);
        ring_->Prime();
        device_open_ = sink_->Open(spec, *ring_);
        if (!device_open_) std::fprintf(stderr, "[twinsanityevolved] audio: no device -- the mix goes nowhere\n");
    }
}

void AudioService::CloseDevice() {
    if (device_open_) { sink_->Close(); device_open_ = false; }
}

bool AudioService::DeviceOpen() const { return device_open_; }

void AudioService::SetBoundaryHz(uint32_t hz) {
    if (hz == 50 || hz == 60) hz_ = hz;
}

void AudioService::MixTick(std::vector<float>& out) {
    const uint32_t frames = kSampleRate / hz_;
    out.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
    audio_.AdvanceControls();   // the boundary's control pulse (volume ramps), once per tick before mixing
    for (uint32_t parent = 0; parent < kStreams; ++parent) RefreshMusicPhase(parent);
    mixer_.MixInto(out.data(), static_cast<int>(frames));
    ++count_.ticks;
    count_.frames += frames;
}

void AudioService::OnBoundary() {
    MixTick(tick_);
    te::harness::OnAudioTick(tick_.data(), static_cast<int>(tick_.size() / 2));   // the harness leg's audio lane (te_harness.h)
    if (device_open_) ring_->Push(tick_.data(), static_cast<int>(tick_.size() / 2));
}

// VoiceSource
void AudioService::BeginMixTick() { audio_.BeginMixTick(); }
bool AudioService::StepVoice(uint32_t channel, float& left, float& right) { return audio_.StepVoice(channel, left, right); }

// ----- audio.h -------------------------------------------------------------------------------------------------
void AudioService::Reset() {
    // The game's first audio call (ConstructMusic -> ResetSound): the device is open from here even when the
    // services owner's StartServices did not connect the sound driver (Platform::Sound::InitialiseRemote).
    EnsureDevice();
    // Every voice and setting as when the sound processor started; the registry (banks) stays, as the IOP's
    // sound table did across MsResetSound (the game reloads what it needs afterwards anyway).
    for (uint32_t stream = 0; stream < kStreams; ++stream) StopStream(stream, Phase::Idle);
    audio_.StopVoices();
    for (uint32_t group = 1; group <= kGroupMax; ++group) audio_.StoreGroupVolume(group << 16, 0x1000, 0x1000);
}

void AudioService::PauseAll() { audio_.PauseAll(); }
void AudioService::ResumeAll() { audio_.ResumeAll(); }

int32_t AudioService::LendToMovie() { audio_.LendToMovie(); return 0; }
int32_t AudioService::ReclaimFromMovie() { audio_.ReclaimFromMovie(); return 0; }

void AudioService::CompactSoundMemory() {
    // No sound memory to compact: samples are owned assets keyed by bank.
}

void AudioService::SetGroupVolume(int32_t group, uint32_t left, uint32_t right) {
    // The game gives the group as number << GroupShift and 4.12 volumes (0x1000 = 1).
    const int32_t result = audio_.SetGroupVolume(static_cast<uint32_t>(group), std::min<uint32_t>(left, 0x1000),
                                                 std::min<uint32_t>(right, 0x1000));
    if (result) { static bool said; Once("SetGroupVolume refused (group not 1..4)", said); }
}

int32_t AudioService::SetVoiceVolume(int32_t voice, int16_t left, int16_t right) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return -1;
    // Past MaxVolume the other phase (InvertedVolumeBase - size); the engine's voice takes the same 15-bit field.
    return audio_.SetVoiceVolume(static_cast<uint32_t>(voice), left, right) ? -1 : 0;
}

int32_t AudioService::SetVoicePitch(int32_t voice, uint16_t pitch) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return -1;
    return audio_.SetVoicePitch(static_cast<uint32_t>(voice), pitch) ? -1 : 0;
}

void AudioService::SetVoiceReverb(int32_t voice, bool on) {
    (void)voice; (void)on;
    ++count_.reverb_calls;   // reverb MISSING in the engine: the send is recorded, not applied
}

void AudioService::ReleaseVoice(int32_t voice, uint32_t rate) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return;
    audio_.ReleaseVoice(static_cast<uint32_t>(voice), std::min<uint32_t>(rate, 31));
}

int32_t AudioService::IsVoiceFree(int32_t voice) {
    if (voice < 0 || voice >= static_cast<int32_t>(kVoices)) return -1;
    return audio_.IsVoiceFree(static_cast<uint32_t>(voice));
}

void AudioService::SetReverb(int32_t core, int32_t mode, uint16_t depthLeft, uint16_t depthRight, uint16_t delay, uint16_t feedback) {
    (void)core; (void)mode; (void)depthLeft; (void)depthRight; (void)delay; (void)feedback;
    ++count_.reverb_calls;
    static bool said; Once("reverb is not implemented (SetReverb recorded only)", said);
}

void AudioService::ClearReverb(int32_t core) { (void)core; ++count_.reverb_calls; }

void AudioService::SetReverbVolume(int32_t core, int16_t left, int16_t right) {
    (void)core; (void)left; (void)right;
    ++count_.reverb_calls;
}

uint32_t AudioService::ReserveSoundMemory(uint32_t size) {
    // The PS2's allocation cursor: where the bank's samples would go; the game keeps the number (music buffers).
    const uint32_t location = sound_memory_cursor_;
    sound_memory_cursor_ += (size + 15u) & ~15u;
    return location;
}

void AudioService::SoundBankLoaded(uint16_t bank) {
    if (!audio_.SoundBankLoaded(bank)) {
        ++count_.banks_refused;
        std::fprintf(stderr, "[twinsanityevolved] audio: SoundBankLoaded(%u) before its bytes arrived\n", bank);
    }
}

void AudioService::ReserveSounds(uint16_t count) { audio_.ReserveSounds(count); }

void AudioService::ReleaseSound(uint32_t sound) { audio_.ReleaseSound(sound); }

int32_t AudioService::PlaySound(uint32_t sound, uint32_t voiceAndGroup, int16_t left, int16_t right, uint16_t pitch,
                                uint32_t attack, uint32_t release, uint32_t loops) {
    const int32_t result = audio_.PlaySound(sound, voiceAndGroup, left, right, pitch, std::min<uint32_t>(attack, 127),
                                            std::min<uint32_t>(release, 31), loops);
    if (result == 0) { ++count_.plays; return 0; }
    ++count_.play_refused;
    if (result == -3) {
        static bool said; Once("PlaySound of a sound whose bank is not loaded (refused)", said);
        return -1;   // the header's "no such voice" is the only refusal the game knows besides -2
    }
    return result == -2 ? -2 : -1;
}

int32_t AudioService::PitchOfRate(int32_t rate) { return title::Audio::PitchOfRate(rate); }

uint32_t AudioService::ChannelBufferAddress(int32_t channel) {
    return te::stream::Service().ChannelBufferAddress(channel);
}

bool AudioService::StoreSoundBank(uint32_t bank, std::vector<uint8_t> bytes) {
    // The bank's samples are whole 16-byte ADPCM blocks; a size the disc table rounded otherwise is cut to blocks.
    const std::size_t blocks = bytes.size() / VagDecoder::kBlockBytes * VagDecoder::kBlockBytes;
    if (blocks != bytes.size()) {
        std::fprintf(stderr, "[twinsanityevolved] audio: bank %u is %zu bytes, not whole ADPCM blocks (cut to %zu)\n",
                     bank, bytes.size(), blocks);
        bytes.resize(blocks);
    }
    count_.bank_bytes += bytes.size();
    if (bytes.empty() || !audio_.StoreSound(bank, std::move(bytes))) {
        ++count_.banks_refused;
        std::fprintf(stderr, "[twinsanityevolved] audio: bank %u refused by the sample registry\n", bank);
        return false;
    }
    ++count_.banks_stored;
    return true;
}

// ----- music ---------------------------------------------------------------------------------------------------
void AudioService::ReadMusic(int32_t file, uint32_t offset, uint32_t size) {
    if (file < 0 || file >= static_cast<int32_t>(windows_.size())) { ++count_.music_errors; return; }
    windows_[static_cast<std::size_t>(file)] = Window{offset, size, true};
}

uint32_t AudioService::ParentOf(uint32_t stream) const {
    if (stream >= kStreams || !streams_[stream].owned) return UINT32_MAX;
    return streams_[stream].parent == UINT32_MAX ? stream : streams_[stream].parent;
}

bool AudioService::VoicePlaying(uint32_t voice) const {
    return voice < kVoices && audio_.Inspect().voices[voice].player.Playing();
}

void AudioService::StopStream(uint32_t stream, Phase phase) {
    const uint32_t parent = ParentOf(stream);
    if (parent == UINT32_MAX) return;
    const uint32_t child = streams_[parent].child;
    for (const uint32_t index : {parent, child}) {
        if (index >= kStreams) continue;
        auto& s = streams_[index];
        audio_.StopMusicVoice(index, s.generation, s.voice_and_group & 0xffff);
        if (phase == Phase::Idle) audio_.HandOffVoice(s.voice_and_group & 0xffff);
        s = Stream{};
        s.phase = phase;
        s.owned = phase != Phase::Idle;
    }
}

int32_t AudioService::StreamMusic(int32_t file, int32_t channel, uint32_t voiceAndGroup, uint16_t pitch, bool once) {
    if (channel < 0 || channel >= static_cast<int32_t>(kStreams) || (voiceAndGroup & 0xffff) >= kVoices ||
        (voiceAndGroup >> 16) > kGroupMax || file < 0 || file >= static_cast<int32_t>(windows_.size())) {
        ++count_.music_errors;
        return -1;
    }
    const auto stream = static_cast<uint32_t>(channel);
    StopStream(stream, Phase::Idle);
    auto& s = streams_[stream];
    s = Stream{};
    s.owned = true;
    s.file = file;
    s.voice_and_group = voiceAndGroup;
    s.pitch = pitch;
    s.looping = !once;
    s.generation = ++generation_;
    s.window = windows_[static_cast<std::size_t>(file)];
    s.phase = Phase::Reading;
    ++count_.music_plays;
    if (!s.window.set) { ++count_.music_errors; s.phase = Phase::Failed; return -1; }
    // The voice is the stream's from here (MsPlayStream): the game's volumes come after PlayMusic.
    audio_.HandOffVoice(voiceAndGroup & 0xffff);
    if (!audio_.ReserveMusicVoice(stream, s.generation, voiceAndGroup, pitch, 0, 0, 0, 0, 0, 0)) {
        ++count_.music_errors; s.phase = Phase::Failed; return -1;
    }
    return channel;
}

void AudioService::InterleaveMusic(int32_t channel, uint32_t blockSize) {
    if (channel < 0 || channel >= static_cast<int32_t>(kStreams) || blockSize == 0 || blockSize % 16) { ++count_.music_errors; return; }
    auto& s = streams_[static_cast<std::size_t>(channel)];
    if (!s.owned || s.issued) { ++count_.music_errors; return; }
    s.block = blockSize;
    s.track = 0;   // the first track is the parent's (the file-reading channel)
}

void AudioService::AddMusicChannel(int32_t child, int32_t parent, uint32_t voiceAndGroup, uint32_t soundAddress) {
    if (child < 0 || child >= static_cast<int32_t>(kStreams) || parent < 0 || parent >= static_cast<int32_t>(kStreams) ||
        child == parent || (voiceAndGroup & 0xffff) >= kVoices) { ++count_.music_errors; return; }
    auto& p = streams_[static_cast<std::size_t>(parent)];
    if (!p.owned || p.parent != UINT32_MAX || p.issued || p.child != UINT32_MAX || p.phase != Phase::Reading) {
        ++count_.music_errors; return;
    }
    StopStream(static_cast<uint32_t>(child), Phase::Idle);
    auto& c = streams_[static_cast<std::size_t>(child)];
    c = Stream{};
    c.owned = true;
    c.parent = static_cast<uint32_t>(parent);
    c.file = p.file;
    c.voice_and_group = voiceAndGroup;
    c.pitch = p.pitch;
    c.looping = p.looping;
    c.generation = p.generation;
    c.track = 1;   // the second track
    c.sound_address = soundAddress;
    c.phase = Phase::Reading;
    p.child = static_cast<uint32_t>(child);
    audio_.HandOffVoice(voiceAndGroup & 0xffff);
    if (!audio_.ReserveMusicVoice(static_cast<uint32_t>(child), c.generation, voiceAndGroup, p.pitch, 0, 0, 0, 0, 0, 0)) {
        ++count_.music_errors;
        StopStream(static_cast<uint32_t>(parent), Phase::Failed);
    }
}

void AudioService::SetMusicEnd(int32_t channel, uint32_t end) {
    if (channel < 0 || channel >= static_cast<int32_t>(kStreams) || end == 0 || end % 16) { ++count_.music_errors; return; }
    auto& s = streams_[static_cast<std::size_t>(channel)];
    if (!s.owned || s.issued) { ++count_.music_errors; return; }
    s.end = end;
}

void AudioService::IssueMusicRead(uint32_t parent) {
    auto& s = streams_[parent];
    if (!s.owned || s.parent != UINT32_MAX || s.issued || s.phase != Phase::Reading) return;
    s.issued = true;
    std::vector<uint8_t> bytes;
    const auto t0 = std::chrono::steady_clock::now();
    // The window the game named (ReadMusic): read whole, the track's interleave and markers resolved by the
    // engine's MusicAsset. The PS2 streamed it block by block into IOP buffers; the owned asset plays from memory.
    if (!te::stream::Service().ReadFileBytes(s.file, s.window.offset, s.window.size, bytes) || bytes.empty()) {
        ++count_.music_errors;
        StopStream(parent, Phase::Failed);
        return;
    }
    ++count_.music_reads;
    count_.music_read_bytes += bytes.size();
    const title::MusicAsset::Layout layout{s.child < kStreams ? 2u : 1u, s.block, s.end, s.looping};
    std::string error;
    if (!title::MusicAsset::Read(bytes, layout, s.data, &error)) {
        ++count_.music_errors;
        std::fprintf(stderr, "[twinsanityevolved] audio: music window (file %d +0x%X, %u bytes) refused: %s\n",
                     s.file, s.window.offset, s.window.size, error.c_str());
        StopStream(parent, Phase::Failed);
        return;
    }
    s.phase = Phase::Prepared;
    if (s.child < kStreams) streams_[s.child].phase = Phase::Prepared;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    count_.music_prepare_ms_max = std::max(count_.music_prepare_ms_max, ms);
    if (std::getenv("TE_STREAM_TRACE"))
        std::fprintf(stderr, "[audio] music window file %d +0x%X %u bytes read + resolved in %.2f ms (%u tracks)\n", s.file,
                     s.window.offset, s.window.size, ms, s.data.count);
    if (!s.key_on_disabled) StartMusic(parent);
}

void AudioService::StartMusic(uint32_t parent) {
    auto& s = streams_[parent];
    if (s.phase != Phase::Prepared) return;
    // A stream whose data begins with a VAGp/MSVp header keys on at the HEADER's sample rate, not at the pitch the game
    // passed to StreamMusic: STREAM.IRX reads the header's rate when the first block lands and programs the voice's pitch
    // from it (module code 0x4c14: pitch = rate * 0x1000 / 48000, written over the channel's requested pitch and the
    // voice shadow for a stream played with the default flags, as every Platform::Audio::StreamMusic is). The game's
    // pitch comes from the bank's .mh table, which the speech banks fill with 32000 for every line while the clips
    // themselves are 32000 / 48000 / 18000 Hz (Crash6\<language>.mb: 3 of the 69 English lines are 48 kHz, 57 of the
    // French / German / Italian / Spanish ones) -- played at the table's pitch a 48 kHz line runs at two thirds speed,
    // a fifth lower. An explicit SetVoicePitch after the key-on is applied as sent, as the module applies the EE's
    // pitch command. Interleaved music is headerless (source_rate 0): the requested pitch stands.
    if (s.data.header_bytes && s.data.source_rate) {
        const int32_t pitch = title::Audio::PitchOfRate(static_cast<int32_t>(s.data.source_rate));
        if (pitch > 0 && pitch <= 0x3fff) {
            for (const uint32_t index : {parent, s.child}) {
                if (index >= kStreams) continue;
                auto& member = streams_[index];
                member.pitch = static_cast<uint16_t>(pitch);
                audio_.SetVoicePitch(member.voice_and_group & 0xffff, member.pitch);
            }
            if (std::getenv("TE_STREAM_TRACE"))
                std::fprintf(stderr, "[audio] music window file %d +0x%X keys on at its header's %u Hz (pitch 0x%X)\n", s.file,
                             s.window.offset, s.data.source_rate, static_cast<unsigned>(pitch));
        }
    }
    for (const uint32_t index : {parent, s.child}) {
        if (index >= kStreams) continue;
        auto& member = streams_[index];
        const uint32_t track = member.track < s.data.count ? member.track : 0;
        if (!audio_.StartMusicVoice(index, member.generation, member.voice_and_group & 0xffff, s.data.tracks[track])) {
            ++count_.music_errors;
            StopStream(parent, Phase::Failed);
            return;
        }
        member.phase = Phase::Playing;
    }
    ++count_.music_starts;
}

void AudioService::RefreshMusicPhase(uint32_t parent) {
    auto& s = streams_[parent];
    if (!s.owned || s.parent != UINT32_MAX || s.phase != Phase::Playing) return;
    // A once-through stream ends when its voice stops; a looping one plays on.
    if (!VoicePlaying(s.voice_and_group & 0xffff)) {
        s.phase = Phase::Stopped;
        if (s.child < kStreams) streams_[s.child].phase = Phase::Stopped;
    }
}

void AudioService::PrepareMusic(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) { ++count_.music_errors; return; }
    streams_[parent].key_on_disabled = true;
    IssueMusicRead(parent);   // preload without starting (MsDisableKeyOn, then the batch's send)
}

bool AudioService::IsMusicReady(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) return false;
    IssueMusicRead(parent);
    RefreshMusicPhase(parent);
    const Phase phase = streams_[parent].phase;
    return phase == Phase::Prepared || phase == Phase::Playing;
}

int32_t AudioService::PlayMusic(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) { ++count_.music_errors; return -1; }
    streams_[parent].key_on_disabled = false;
    IssueMusicRead(parent);
    StartMusic(parent);
    return streams_[parent].phase == Phase::Failed ? -1 : 0;
}

bool AudioService::IsMusicPlaying(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) return false;
    RefreshMusicPhase(parent);
    const Phase phase = streams_[parent].phase;
    // The module's stream state is "off" only when nothing is reading, prepared or playing.
    return phase == Phase::Reading || phase == Phase::Prepared || phase == Phase::Playing;
}

void AudioService::StopMusic(int32_t channel) {
    const uint32_t parent = channel < 0 ? UINT32_MAX : ParentOf(static_cast<uint32_t>(channel));
    if (parent == UINT32_MAX) return;
    ++count_.music_stops;
    StopStream(parent, Phase::Idle);
}

AudioService::Counters AudioService::Count() const {
    Counters count = count_;
    count.ring_pushed = ring_->pushed();
    count.ring_pulled = ring_->pulled();
    count.ring_dropped = ring_->dropped();
    count.ring_underruns = ring_->underruns();
    count.ring_overruns = ring_->overruns();
    count.ring_padded = ring_->padded();
    return count;
}

// ----- save state --------------------------------------------------------------------------------------------------
void AudioService::Serialize(std::vector<uint8_t>& out) const {
    std::vector<uint8_t> audio;
    std::string error;
    if (!title::AudioState::Encode(audio_.CaptureLoads(), audio, &error))
        std::fprintf(stderr, "[twinsanityevolved] audio: state not captured: %s\n", error.c_str());
    savestate::Put(out, uint32_t(1));
    savestate::Put(out, hz_);
    savestate::Put(out, sound_memory_cursor_);
    savestate::Put(out, generation_);
    savestate::Put(out, uint32_t(windows_.size()));
    for (const Window& w : windows_) { savestate::Put(out, w.offset); savestate::Put(out, w.size); savestate::Put(out, uint8_t(w.set)); }
    savestate::Put(out, uint32_t(streams_.size()));
    for (const Stream& s : streams_) {
        savestate::Put(out, uint8_t(s.owned)); savestate::Put(out, s.parent); savestate::Put(out, s.child);
        savestate::Put(out, s.file); savestate::Put(out, s.voice_and_group); savestate::Put(out, s.pitch);
        savestate::Put(out, uint8_t(s.looping)); savestate::Put(out, s.block); savestate::Put(out, s.end); savestate::Put(out, s.track);
        savestate::Put(out, s.sound_address);
        savestate::Put(out, s.window.offset); savestate::Put(out, s.window.size); savestate::Put(out, uint8_t(s.window.set));
        savestate::Put(out, s.generation); savestate::Put(out, uint8_t(s.key_on_disabled)); savestate::Put(out, uint8_t(s.issued));
        savestate::Put(out, uint8_t(s.phase));
    }
    savestate::PutBytes(out, &count_, sizeof count_);
    savestate::Put(out, uint32_t(audio.size()));
    savestate::PutBytes(out, audio.data(), audio.size());
}

bool AudioService::Restore(const std::vector<uint8_t>& payload, std::string* error) {
    savestate::PayloadReader in(payload);
    auto fail = [&](const std::string& why) { if (error) *error = "te::audio: " + why; return false; };
    uint32_t version = 0, nwindows = 0, nstreams = 0;
    uint32_t hz = 60, cursor = 0;
    uint64_t generation = 0;
    if (!in.Get(&version) || version != 1 || !in.Get(&hz) || !in.Get(&cursor) || !in.Get(&generation) ||
        !in.Get(&nwindows) || nwindows != windows_.size())
        return fail("payload");
    std::array<Window, 256> windows{};
    for (Window& w : windows) {
        uint8_t set = 0;
        if (!in.Get(&w.offset) || !in.Get(&w.size) || !in.Get(&set)) return fail("payload truncated (windows)");
        w.set = set != 0;
    }
    if (!in.Get(&nstreams) || nstreams != streams_.size()) return fail("payload (streams)");
    std::array<Stream, kStreams> streams{};
    for (Stream& s : streams) {
        uint8_t owned = 0, looping = 0, set = 0, key_on_disabled = 0, issued = 0, phase = 0;
        if (!in.Get(&owned) || !in.Get(&s.parent) || !in.Get(&s.child) || !in.Get(&s.file) || !in.Get(&s.voice_and_group) ||
            !in.Get(&s.pitch) || !in.Get(&looping) || !in.Get(&s.block) || !in.Get(&s.end) || !in.Get(&s.track) ||
            !in.Get(&s.sound_address) || !in.Get(&s.window.offset) || !in.Get(&s.window.size) || !in.Get(&set) ||
            !in.Get(&s.generation) || !in.Get(&key_on_disabled) || !in.Get(&issued) || !in.Get(&phase) || phase > uint8_t(Phase::Failed))
            return fail("payload truncated (streams)");
        s.owned = owned != 0; s.looping = looping != 0; s.window.set = set != 0;
        s.key_on_disabled = key_on_disabled != 0; s.issued = issued != 0; s.phase = static_cast<Phase>(phase);
    }
    Counters counters{};
    std::vector<uint8_t> audio;
    if (!in.GetBytes(&counters, sizeof counters) || !savestate::GetVector(in, audio, 80u << 20)) return fail("payload truncated (audio)");
    if (!audio.empty()) {
        title::Audio::Snapshot snapshot;
        std::string err;
        uint64_t load_generation = 0;
        if (!title::AudioState::Decode(audio, snapshot, &err)) return fail("title::Audio snapshot: " + err);
        if (!audio_.RestoreLoads(snapshot, load_generation)) return fail("title::Audio rejected the snapshot");
    }
    hz_ = hz;
    sound_memory_cursor_ = cursor;
    generation_ = generation;
    windows_ = windows;
    streams_ = streams;
    count_ = counters;
    // The music windows' decoded data: a Playing stream's tracks are the samples its voices play (the title::Audio
    // snapshot carries them, deduplicated); a Prepared one (window read, not started) is read from the disc again.
    for (uint32_t parent = 0; parent < kStreams; ++parent) {
        Stream& s = streams_[parent];
        if (!s.owned || s.parent != UINT32_MAX || !s.issued || (s.phase != Phase::Prepared && s.phase != Phase::Playing)) continue;
        s.data = {};
        s.data.count = s.child < kStreams ? 2u : 1u;
        bool have = false;
        if (s.phase == Phase::Playing) {
            const auto& voices = audio_.Inspect().voices;
            for (const uint32_t index : {parent, s.child}) {
                if (index >= kStreams) continue;
                const Stream& member = streams_[index];
                const uint32_t voice = member.voice_and_group & 0xffff;
                const uint32_t track = member.track < s.data.count ? member.track : 0;
                if (voice < voices.size() && voices[voice].music_stream == index && voices[voice].player.Capture().samples) {
                    s.data.tracks[track] = voices[voice].player.Capture().samples;
                    have = true;
                }
            }
        }
        if (!have) {
            std::vector<uint8_t> bytes;
            const title::MusicAsset::Layout layout{s.data.count, s.block, s.end, s.looping};
            std::string err;
            if (!te::stream::Service().ReadFileBytes(s.file, s.window.offset, s.window.size, bytes) || bytes.empty() ||
                !title::MusicAsset::Read(bytes, layout, s.data, &err)) {
                if (te::GetSettings().verbose)
                    std::fprintf(stderr, "[twinsanityevolved] audio: music window of stream %u not re-read after load (%s)\n", parent,
                                 err.empty() ? "the file is closed" : err.c_str());
                s.data = {};
            }
        }
    }
    HookClock();
    return true;
}

AudioService& Service() {
    static AudioService service;
    static const bool registered = [] {
        savestate::Register(savestate::kTagAudio, "te::audio::AudioService", false,
            [](std::vector<uint8_t>& out) { Service().Serialize(out); },
            [](const savestate::Blob& blob, std::string* err) { return Service().Restore(blob.payload, err); });
        return true;
    }();
    (void)registered;
    return service;
}

}  // namespace te::audio

// ----- te_audio.h (the light hooks) ----------------------------------------------------------------------------
namespace te::audio {
void Configure(bool open_device) { Service().Configure(open_device); }
void SetBoundaryHz(uint32_t hz) { Service().SetBoundaryHz(hz); }
void OnBoundary() { Service().OnBoundary(); }
RingHealth Health() {
    const auto c = Service().Count();
    RingHealth h;
    h.pushed = c.ring_pushed; h.pulled = c.ring_pulled; h.dropped = c.ring_dropped; h.underruns = c.ring_underruns;
    h.overruns = c.ring_overruns; h.padded = c.ring_padded; h.device = Service().DeviceOpen();
    return h;
}
void Report() {
    char line[512];
    const auto a = Service().Count();
    const auto s = te::stream::Service().Count();
    const auto m = te::movie::Service().Count();
    int n = std::snprintf(line, sizeof line,
        "[twinsanityevolved] audio: device=%d hz=%u ticks=%llu frames=%llu pushed=%llu pulled=%llu dropped=%llu underruns=%llu overruns=%llu padded=%llu\n",
        Service().DeviceOpen() ? 1 : 0, Service().BoundaryHz(), (unsigned long long)a.ticks, (unsigned long long)a.frames,
        (unsigned long long)a.ring_pushed, (unsigned long long)a.ring_pulled, (unsigned long long)a.ring_dropped,
        (unsigned long long)a.ring_underruns, (unsigned long long)a.ring_overruns, (unsigned long long)a.ring_padded);
    if (n > 0) (void)!te_write_fd(2, line, static_cast<unsigned>(n));
    n = std::snprintf(line, sizeof line,
        "[twinsanityevolved] audio: banks stored=%llu refused=%llu bytes=%llu; plays=%llu refused=%llu; music plays=%llu starts=%llu stops=%llu reads=%llu bytes=%llu errors=%llu prepare_max_ms=%.1f; reverb calls=%llu\n",
        (unsigned long long)a.banks_stored, (unsigned long long)a.banks_refused, (unsigned long long)a.bank_bytes,
        (unsigned long long)a.plays, (unsigned long long)a.play_refused, (unsigned long long)a.music_plays,
        (unsigned long long)a.music_starts, (unsigned long long)a.music_stops, (unsigned long long)a.music_reads,
        (unsigned long long)a.music_read_bytes, (unsigned long long)a.music_errors, a.music_prepare_ms_max, (unsigned long long)a.reverb_calls);
    if (n > 0) (void)!te_write_fd(2, line, static_cast<unsigned>(n));
    n = std::snprintf(line, sizeof line,
        "[twinsanityevolved] stream: opens=%llu missing=%llu closes=%llu reads=%llu bank_reads=%llu delivered=%llu bytes=%llu short=%llu polls=%llu waits=%llu updates=%llu attaches=%llu detaches=%llu refused=%llu bad_channel=%llu bad_file=%llu\n",
        (unsigned long long)s.opens, (unsigned long long)s.opens_missing, (unsigned long long)s.closes, (unsigned long long)s.reads,
        (unsigned long long)s.bank_reads, (unsigned long long)s.delivered, (unsigned long long)s.delivered_bytes,
        (unsigned long long)s.short_reads, (unsigned long long)s.polls, (unsigned long long)s.waits, (unsigned long long)s.updates,
        (unsigned long long)s.attaches, (unsigned long long)s.detaches, (unsigned long long)s.attach_refused,
        (unsigned long long)s.bad_channel, (unsigned long long)s.bad_file);
    if (n > 0) (void)!te_write_fd(2, line, static_cast<unsigned>(n));
    n = std::snprintf(line, sizeof line,
        "[twinsanityevolved] movie: constructs=%llu opens=%llu failed=%llu skipped=%llu closes=%llu unknown=%llu; pictures decoded=%llu queued=%llu uploaded=%llu presents=%llu draws=%llu fields=%llu bytes=%llu; pcm frames=%llu silence=%llu%s\n",
        (unsigned long long)m.constructs, (unsigned long long)m.opens, (unsigned long long)m.open_failed, (unsigned long long)m.skipped,
        (unsigned long long)m.closes, (unsigned long long)m.unknown_player, (unsigned long long)m.pictures_decoded,
        (unsigned long long)m.pictures_queued, (unsigned long long)m.pictures_uploaded, (unsigned long long)m.presents,
        (unsigned long long)m.draws, (unsigned long long)m.fields, (unsigned long long)m.bytes_read, (unsigned long long)m.pcm_frames,
        (unsigned long long)m.pcm_silence_frames, te::movie::Service().SkipMovies() ? " (skip movies)" : "");
    if (n > 0) (void)!te_write_fd(2, line, static_cast<unsigned>(n));
}
}  // namespace te::audio
