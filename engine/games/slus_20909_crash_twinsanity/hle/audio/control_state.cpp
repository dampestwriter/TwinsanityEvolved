#include "games/slus_20909_crash_twinsanity/hle/audio/control_state.h"

#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio_state.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
bool Fail(std::string* error, const char* text) { if (error) *error = text; return false; }
bool ValidMusic(const MusicRequest& r, uint32_t words) {
    if (r.kind > MusicRequest::Kind::Reset || r.stream >= Audio::kVoices ||
        r.parent >= Audio::kVoices || r.channel >= Audio::kVoices || r.group > Audio::kGroups ||
        r.track > 1 || r.attack > 127 || r.release > 31) return false;
    constexpr uint32_t sizes[]{8, 10, 6, 9, 5, 3, 3, 3, 0};
    if (words != sizes[static_cast<unsigned>(r.kind)]) return false;
    if (r.kind == MusicRequest::Kind::Window || r.kind == MusicRequest::Kind::Play)
        return r.file >= 0;
    if (r.kind == MusicRequest::Kind::Child) return r.parent != r.stream;
    if (r.kind == MusicRequest::Kind::Interleave) return r.block && r.block % 16 == 0;
    if (r.kind == MusicRequest::Kind::End) return r.end && r.end % 16 == 0;
    return true;
}
bool ValidBuffer(const BufferRequest& r, uint32_t words) {
    if (r.kind > BufferRequest::Kind::Query || r.stream >= Audio::kVoices) return false;
    constexpr uint32_t sizes[]{7, 5, 3, 2};
    if (words != sizes[static_cast<unsigned>(r.kind)]) return false;
    return r.kind != BufferRequest::Kind::Query ||
        (r.free_word >= 0x10000 && r.free_word <= 0x02000000 - 4 && r.free_word % 4 == 0);
}
void PutMusic(std::vector<uint8_t>& out, const MusicRequest& r) {
    for (const auto value : {static_cast<uint32_t>(r.kind), r.stream, r.parent, r.channel, r.group, r.track,
                            static_cast<uint32_t>(r.file), r.offset, r.size, r.block, r.end})
        StateBytes::Put(out, value);
    for (const auto value : {r.pitch, r.left, r.right, r.raw_left, r.raw_right}) StateBytes::Put(out, value);
    StateBytes::Put(out, r.attack); StateBytes::Put(out, r.release); StateBytes::Put(out, r.looping);
}
bool GetMusic(StateBytes& in, MusicRequest& r) {
    uint32_t kind = 0, file = 0;
    if (!in.Get(kind) || kind > static_cast<uint32_t>(MusicRequest::Kind::Reset) ||
        !in.Get(r.stream) || !in.Get(r.parent) || !in.Get(r.channel) || !in.Get(r.group) ||
        !in.Get(r.track) || !in.Get(file) || !in.Get(r.offset) || !in.Get(r.size) ||
        !in.Get(r.block) || !in.Get(r.end) || !in.Get16(r.pitch) || !in.Get16(r.left) ||
        !in.Get16(r.right) || !in.Get16(r.raw_left) || !in.Get16(r.raw_right) ||
        !in.Get8(r.attack) || !in.Get8(r.release) || !in.GetBool(r.looping)) return false;
    r.kind = static_cast<MusicRequest::Kind>(kind);
    r.file = file <= INT32_MAX ? static_cast<int32_t>(file) :
        static_cast<int32_t>(static_cast<int64_t>(file) - (int64_t{1} << 32));
    return true;
}
void PutBuffer(std::vector<uint8_t>& out, const BufferRequest& r) {
    for (const auto value : {static_cast<uint32_t>(r.kind), r.stream, r.size, r.sound_address, r.free_word})
        StateBytes::Put(out, value);
}
bool GetBuffer(StateBytes& in, BufferRequest& r) {
    uint32_t kind = 0;
    if (!in.Get(kind) || kind > static_cast<uint32_t>(BufferRequest::Kind::Query) ||
        !in.Get(r.stream) || !in.Get(r.size) || !in.Get(r.sound_address) || !in.Get(r.free_word)) return false;
    r.kind = static_cast<BufferRequest::Kind>(kind);
    return true;
}
}
bool SoundControls::ValidateData(const State& state, std::string* error) {
    if (state.pending.size() > 65536 || state.words > 0x400 || state.tick > (uint64_t{1} << 52))
        return Fail(error, "sound command snapshot exceeds bounds");
    uint64_t words = 0;
    std::array<uint32_t, Audio::kVoices> requested{};
    for (const auto& c : state.pending) {
        if (c.kind > Kind::Buffer || c.channel >= Audio::kVoices || (words += c.words) > 0x400)
            return Fail(error, "invalid sound command kind, channel or budget");
        uint32_t expected = 0;
        switch (c.kind) {
        case Kind::Play:
            expected = 10; ++requested[c.channel];
            if (!Audio::Validate(c.voice) || c.voice.sound == UINT32_MAX ||
                c.voice.music_stream != UINT32_MAX || !c.voice.player.Capture().samples)
                return Fail(error, "queued sample play lost its asset lease");
            break;
        case Kind::Volume: expected = 5; break;
        case Kind::Pitch: expected = 4; if (c.value > 0xffff) return Fail(error, "invalid pitch"); break;
        case Kind::Release: expected = 4; if (c.value > 31) return Fail(error, "invalid release"); break;
        case Kind::KeyOff: expected = 3; break;
        case Kind::Group:
            if ((c.value & 0xffff) || !(c.value >> 16) || (c.value >> 16) > Audio::kGroups ||
                c.left > 0x1000 || c.right > 0x1000) return Fail(error, "invalid group control");
            break;
        case Kind::ChannelGroup:
        case Kind::Pause: case Kind::Resume: case Kind::Stop: case Kind::HandOff: break;
        case Kind::RemainingVolume: expected = 5; break;
        case Kind::Music:
            if (!state.music || !ValidMusic(c.music, c.words)) return Fail(error, "invalid queued music request");
            expected = c.words;
            if (c.music.kind == MusicRequest::Kind::Play || c.music.kind == MusicRequest::Kind::Child) {
                if (c.channel != c.music.channel) return Fail(error, "music channel projection mismatch");
                ++requested[c.music.channel];
            }
            break;
        case Kind::Buffer:
            if (!state.buffers || !ValidBuffer(c.buffer, c.words)) return Fail(error, "invalid queued buffer request");
            expected = c.words;
            break;
        }
        if (c.words != expected) return Fail(error, "sound command accounting mismatch");
    }
    if (words != state.words || requested != state.requested)
        return Fail(error, "sound batch budget or readiness projection mismatch");
    return true;
}
bool ControlState::Encode(const SoundControls::State& state, std::vector<uint8_t>& bytes, std::string* error) {
    if (!SoundControls::ValidateData(state, error)) return false;
    std::vector<Audio::Voice> voices;
    for (const auto& command : state.pending)
        if (command.kind == SoundControls::Kind::Play) voices.push_back(command.voice);
    std::vector<uint8_t> leases, out;
    if (!AudioState::EncodeLeases(voices, leases, error)) return false;
    StateBytes::Put(out, 1); StateBytes::Put(out, state.words); StateBytes::Put64(out, state.tick);
    StateBytes::Put(out, state.clocked); StateBytes::Put(out, state.music); StateBytes::Put(out, state.buffers);
    for (const auto value : state.owned) StateBytes::Put(out, value);
    for (const auto value : state.requested) StateBytes::Put(out, value);
    StateBytes::Put(out, static_cast<uint32_t>(leases.size()));
    out.insert(out.end(), leases.begin(), leases.end());
    StateBytes::Put(out, static_cast<uint32_t>(state.pending.size()));
    for (const auto& c : state.pending) {
        StateBytes::Put(out, static_cast<uint32_t>(c.kind));
        StateBytes::Put(out, c.channel); StateBytes::Put(out, c.value); StateBytes::Put(out, c.words);
        for (auto value : {c.left, c.right, c.raw_left, c.raw_right}) StateBytes::Put(out, value);
        StateBytes::Put(out, c.smooth);
        if (c.kind == SoundControls::Kind::Music) PutMusic(out, c.music);
        if (c.kind == SoundControls::Kind::Buffer) PutBuffer(out, c.buffer);
    }
    bytes = std::move(out);
    return true;
}
bool ControlState::Decode(const std::vector<uint8_t>& bytes, SoundControls::State& state, std::string* error) {
    if (bytes.size() > 80 * 1024 * 1024) return Fail(error, "sound commands exceed snapshot bound");
    StateBytes in(bytes);
    SoundControls::State out;
    uint32_t version = 0, size = 0, count = 0;
    if (!in.Get(version) || version != 1 || !in.Get(out.words) || !in.Get64(out.tick) ||
        !in.GetBool(out.clocked) || !in.GetBool(out.music) || !in.GetBool(out.buffers))
        return Fail(error, "invalid sound command header");
    for (auto& value : out.owned) if (!in.GetBool(value)) return Fail(error, "invalid sound ownership");
    for (auto& value : out.requested) if (!in.Get(value)) return Fail(error, "invalid sound readiness");
    std::vector<uint8_t> leases;
    std::vector<Audio::Voice> voices;
    if (!in.Get(size) || !in.GetBytes(size, leases) || !AudioState::DecodeLeases(leases, voices, error) ||
        !in.Get(count) || count > 65536) return Fail(error, "invalid sound command leases/count");
    size_t voice = 0;
    for (uint32_t i = 0; i < count; ++i) {
        SoundControls::Command c;
        uint32_t kind = 0;
        if (!in.Get(kind) || kind > static_cast<uint32_t>(SoundControls::Kind::Buffer) ||
            !in.Get(c.channel) || !in.Get(c.value) || !in.Get(c.words) ||
            !in.Get16(c.left) || !in.Get16(c.right) || !in.Get16(c.raw_left) || !in.Get16(c.raw_right) ||
            !in.GetBool(c.smooth)) return Fail(error, "invalid sound command");
        c.kind = static_cast<SoundControls::Kind>(kind);
        if (c.kind == SoundControls::Kind::Play) {
            if (voice >= voices.size()) return Fail(error, "missing queued voice");
            c.voice = std::move(voices[voice++]);
        }
        if (c.kind == SoundControls::Kind::Music && !GetMusic(in, c.music)) return Fail(error, "invalid music command");
        if (c.kind == SoundControls::Kind::Buffer && !GetBuffer(in, c.buffer)) return Fail(error, "invalid buffer command");
        out.pending.push_back(std::move(c));
    }
    if (in.Remaining() || voice != voices.size() || !SoundControls::ValidateData(out, error))
        return Fail(error, "invalid sound command closure or trailing bytes");
    state = std::move(out);
    return true;
}
} // namespace title
