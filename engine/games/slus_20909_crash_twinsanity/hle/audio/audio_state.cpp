#include "games/slus_20909_crash_twinsanity/hle/audio/audio_state.h"

#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
// Version 2 adds the per-voice pause hold. A version-1 image is read with the
// hold it meant: every playing voice held while the service was paused.
constexpr uint32_t kVersion = 2, kFirstVersion = 1, kMaxBytes = 72 * 1024 * 1024, kAssetBytes = 64 * 1024 * 1024;
using Assets = std::vector<SampleVoice::Samples>;
using AssetIds = std::map<const std::vector<uint8_t>*, uint32_t>;
bool Fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
void PutRamp(std::vector<uint8_t>& out, const VolumeRamp& ramp) {
    const auto& s = ramp.Capture();
    StateBytes::Put(out, s.target); StateBytes::Put(out, s.direction + 1);
    StateBytes::Put(out, s.active); StateBytes::Put(out, s.crossing_zero);
    StateBytes::PutFloat(out, s.step);
}
bool GetRamp(StateBytes& in, VolumeRamp& ramp) {
    VolumeRamp::State state;
    uint32_t direction = 0;
    if (!in.Get16(state.target) || !in.Get(direction) || direction > 2 ||
        !in.GetBool(state.active) || !in.GetBool(state.crossing_zero) || !in.GetFloat(state.step)) return false;
    state.direction = static_cast<int8_t>(static_cast<int>(direction) - 1);
    return ramp.Restore(state);
}
void PutVoice(std::vector<uint8_t>& out, const Audio::Voice& voice, const AssetIds& ids) {
    const auto put = [&](uint32_t value) { StateBytes::Put(out, value); };
    put(voice.sound); put(voice.group); put(voice.left); put(voice.right); put(voice.repeats);
    put(voice.music_stream); StateBytes::Put64(out, voice.music_generation); put(voice.music_started);
    put(voice.held);
    const auto& s = voice.player.Capture();
    put(s.samples ? ids.at(s.samples.get()) : 0);
    put(s.parameters.pitch); put(s.parameters.left); put(s.parameters.right);
    put(s.parameters.attack); put(s.parameters.decay); put(s.parameters.sustain); put(s.parameters.release);
    put(static_cast<uint32_t>(s.phase));
    put(s.block_offset); put(s.loop_offset); put(s.fraction); put(s.sample_index);
    put(static_cast<uint16_t>(s.history.h1)); put(static_cast<uint16_t>(s.history.h2));
    for (auto sample : s.block) put(static_cast<uint16_t>(sample));
    put(s.flags); put(static_cast<uint16_t>(s.previous));
    StateBytes::PutFloat(out, s.envelope); StateBytes::PutFloat(out, s.left); StateBytes::PutFloat(out, s.right);
    PutRamp(out, s.left_ramp); PutRamp(out, s.right_ramp); StateBytes::Put64(out, s.ends);
}
bool GetVoice(StateBytes& in, Audio::Voice& voice, const Assets& assets, uint32_t version, bool paused) {
    SampleVoice::State s;
    uint32_t asset = 0, phase = 0;
    uint16_t h1 = 0, h2 = 0, previous = 0;
    if (!in.Get(voice.sound) || !in.Get(voice.group) || !in.Get16(voice.left) || !in.Get16(voice.right) ||
        !in.Get(voice.repeats) || !in.Get(voice.music_stream) || !in.Get64(voice.music_generation) ||
        !in.GetBool(voice.music_started) || (version >= 2 && !in.GetBool(voice.held)) ||
        !in.Get(asset) || asset >= assets.size() ||
        !in.Get16(s.parameters.pitch) || !in.Get16(s.parameters.left) || !in.Get16(s.parameters.right) ||
        !in.Get8(s.parameters.attack) || !in.Get8(s.parameters.decay) || !in.Get8(s.parameters.sustain) ||
        !in.Get8(s.parameters.release) || !in.Get(phase) || phase > 4 || !in.Get(s.block_offset) ||
        !in.Get(s.loop_offset) || !in.Get(s.fraction) || !in.Get(s.sample_index) ||
        !in.Get16(h1) || !in.Get16(h2)) return false;
    s.samples = assets[asset];
    s.phase = static_cast<SampleVoice::Phase>(phase);
    // Defined arithmetic for the signed 16-bit sample domain.
    const auto signed16 = [](uint16_t value) { return value < 0x8000 ? int{value} : int{value} - 0x10000; };
    s.history.h1 = signed16(h1); s.history.h2 = signed16(h2);
    for (auto& sample : s.block) {
        uint16_t value = 0;
        if (!in.Get16(value)) return false;
        sample = static_cast<int16_t>(signed16(value));
    }
    if (!in.Get8(s.flags) || !in.Get16(previous) || !in.GetFloat(s.envelope) ||
        !in.GetFloat(s.left) || !in.GetFloat(s.right) || !GetRamp(in, s.left_ramp) ||
        !GetRamp(in, s.right_ramp) || !in.Get64(s.ends)) return false;
    s.previous = static_cast<int16_t>(signed16(previous));
    if (!voice.player.Restore(s)) return false;
    if (version < 2) voice.held = paused && voice.player.Playing();
    return true;
}
bool GetAssets(StateBytes& in, Assets& assets) {
    uint32_t count = 0;
    if (!in.Get(count) || count > 0xffff + Audio::kVoices) return false;
    uint64_t total = 0;
    assets.push_back({});
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = 0;
        if (!in.Get(size) || !size || size % 16 || (total += size) > kAssetBytes) return false;
        std::vector<uint8_t> bytes;
        if (!in.GetBytes(size, bytes)) return false;
        assets.push_back(std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
    }
    return true;
}
} // namespace
bool AudioState::Encode(const Audio::Snapshot& state, std::vector<uint8_t>& bytes, std::string* error) {
    if (!Audio::Validate(state)) return Fail(error, "invalid Audio state");
    Assets assets;
    AssetIds ids;
    uint64_t total = 0;
    const auto asset = [&](const SampleVoice::Samples& data) {
        if (!data || ids.count(data.get())) return true;
        if (data->empty() || data->size() % 16 || (total += data->size()) > kAssetBytes) return false;
        assets.push_back(data);
        ids.emplace(data.get(), static_cast<uint32_t>(assets.size()));
        return true;
    };
    for (const auto& sample : state.playback.samples)
        if (!asset(sample.second.bytes)) return Fail(error, "Audio assets exceed snapshot bound");
    for (const auto& voice : state.playback.voices)
        if (!asset(voice.player.Capture().samples)) return Fail(error, "Audio leases exceed snapshot bound");
    std::vector<uint8_t> out;
    StateBytes::Put(out, kVersion);
    StateBytes::Put(out, static_cast<uint32_t>(assets.size()));
    for (const auto& data : assets) {
        StateBytes::Put(out, static_cast<uint32_t>(data->size()));
        out.insert(out.end(), data->begin(), data->end());
    }
    const auto& playback = state.playback;
    StateBytes::Put64(out, playback.generation); StateBytes::Put64(out, playback.ticks);
    StateBytes::Put(out, playback.sample_limit);
    StateBytes::Put(out, playback.paused); StateBytes::Put(out, playback.movie_owned);
    StateBytes::Put64(out, state.load_generation); StateBytes::Put64(out, state.load_sequence);
    StateBytes::Put(out, static_cast<uint32_t>(playback.samples.size()));
    for (const auto& entry : playback.samples) {
        StateBytes::Put(out, entry.first); StateBytes::Put64(out, entry.second.generation);
        StateBytes::Put64(out, entry.second.load_sequence); StateBytes::Put(out, entry.second.ready);
        StateBytes::Put(out, ids.at(entry.second.bytes.get()));
    }
    StateBytes::Put(out, static_cast<uint32_t>(state.pending_loads.size()));
    for (const auto& entry : state.pending_loads) {
        StateBytes::Put(out, entry.first); StateBytes::Put64(out, entry.second);
    }
    for (const auto volume : playback.group_left) StateBytes::Put(out, volume);
    for (const auto volume : playback.group_right) StateBytes::Put(out, volume);
    for (const auto& voice : playback.voices) PutVoice(out, voice, ids);
    if (out.size() > kMaxBytes) return Fail(error, "Audio state exceeds snapshot bound");
    bytes = std::move(out);
    return true;
}
bool AudioState::Decode(const std::vector<uint8_t>& bytes, Audio::Snapshot& state, std::string* error) {
    if (bytes.size() > kMaxBytes) return Fail(error, "Audio state exceeds snapshot bound");
    StateBytes in(bytes);
    uint32_t version = 0, count = 0;
    Assets assets;
    Audio::Snapshot decoded;
    auto& p = decoded.playback;
    if (!in.Get(version) || version < kFirstVersion || version > kVersion || !GetAssets(in, assets) ||
        !in.Get64(p.generation) || !in.Get64(p.ticks) || !in.Get(p.sample_limit) ||
        !in.GetBool(p.paused) || !in.GetBool(p.movie_owned) ||
        !in.Get64(decoded.load_generation) || !in.Get64(decoded.load_sequence) ||
        !in.Get(count) || count > 0xffff) return Fail(error, "invalid Audio header");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sound = 0, asset = 0;
        Audio::Sample sample;
        if (!in.Get(sound) || !in.Get64(sample.generation) || !in.Get64(sample.load_sequence) ||
            !in.GetBool(sample.ready) || !in.Get(asset) || !asset || asset >= assets.size())
            return Fail(error, "invalid Audio sample");
        sample.bytes = assets[asset];
        if (!p.samples.emplace(sound, std::move(sample)).second) return Fail(error, "duplicate Audio sample");
    }
    if (!in.Get(count) || count > 0xffff) return Fail(error, "invalid Audio ticket count");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sound = 0;
        uint64_t sequence = 0;
        if (!in.Get(sound) || !in.Get64(sequence) ||
            !decoded.pending_loads.emplace(sound, sequence).second) return Fail(error, "invalid Audio ticket");
    }
    for (auto& volume : p.group_left) if (!in.Get16(volume)) return Fail(error, "invalid Audio group volume");
    for (auto& volume : p.group_right) if (!in.Get16(volume)) return Fail(error, "invalid Audio group volume");
    for (auto& voice : p.voices)
        if (!GetVoice(in, voice, assets, version, p.paused)) return Fail(error, "invalid Audio voice");
    if (in.Remaining() || !Audio::Validate(decoded)) return Fail(error, "invalid Audio state or trailing bytes");
    state = std::move(decoded);
    return true;
}
bool AudioState::EncodeLeases(const std::vector<Audio::Voice>& leases, std::vector<uint8_t>& bytes,
                              std::string* error) {
    if (leases.size() > 65536) return Fail(error, "too many queued voice leases");
    Assets assets;
    AssetIds ids;
    uint64_t total = 0;
    for (const auto& voice : leases) {
        if (!Audio::Validate(voice)) return Fail(error, "invalid queued voice lease");
        const auto& data = voice.player.Capture().samples;
        if (!data || ids.count(data.get())) continue;
        if ((total += data->size()) > kAssetBytes) return Fail(error, "queued assets exceed bound");
        assets.push_back(data); ids.emplace(data.get(), static_cast<uint32_t>(assets.size()));
    }
    std::vector<uint8_t> out;
    StateBytes::Put(out, kVersion); StateBytes::Put(out, static_cast<uint32_t>(assets.size()));
    for (const auto& data : assets) {
        StateBytes::Put(out, static_cast<uint32_t>(data->size()));
        out.insert(out.end(), data->begin(), data->end());
    }
    StateBytes::Put(out, static_cast<uint32_t>(leases.size()));
    for (const auto& voice : leases) PutVoice(out, voice, ids);
    if (out.size() > kMaxBytes) return Fail(error, "queued voice state exceeds bound");
    bytes = std::move(out);
    return true;
}
bool AudioState::DecodeLeases(const std::vector<uint8_t>& bytes, std::vector<Audio::Voice>& leases,
                              std::string* error) {
    if (bytes.size() > kMaxBytes) return Fail(error, "queued voice state exceeds bound");
    StateBytes in(bytes);
    Assets assets;
    uint32_t version = 0, count = 0;
    if (!in.Get(version) || version < kFirstVersion || version > kVersion || !GetAssets(in, assets) ||
        !in.Get(count) || count > 65536) return Fail(error, "invalid queued voice header");
    std::vector<Audio::Voice> out(count);
    for (auto& voice : out) // a queued lease is not yet keyed: never held
        if (!GetVoice(in, voice, assets, version, false) || !Audio::Validate(voice)) return Fail(error, "invalid queued voice");
    if (in.Remaining()) return Fail(error, "trailing queued voice bytes");
    leases = std::move(out);
    return true;
}
} // namespace title
