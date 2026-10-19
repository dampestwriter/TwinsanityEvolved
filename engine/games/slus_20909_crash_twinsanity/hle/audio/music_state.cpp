#include "games/slus_20909_crash_twinsanity/hle/audio/music_state.h"

#include <set>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
constexpr uint64_t kMaxAssets = 64 * 1024 * 1024, kMaxState = kMaxAssets + 2 * 1024 * 1024;
// Version 2 adds the stream's buffer-fill extent and charged refill position.
constexpr uint32_t kVersion = 2;
bool Fail(std::string* error, const char* text) { if (error) *error = text; return false; }
bool Active(MusicLibrary::Phase phase) {
    return phase == MusicLibrary::Phase::Reading || phase == MusicLibrary::Phase::Prepared ||
           phase == MusicLibrary::Phase::Playing;
}
void PutPlay(std::vector<uint8_t>& out, const MusicRequest& r) {
    for (const auto value : {static_cast<uint32_t>(r.kind), r.stream, r.parent, r.channel, r.group, r.track,
                            static_cast<uint32_t>(r.file), r.offset, r.size, r.block, r.end})
        StateBytes::Put(out, value);
    for (const auto value : {r.pitch, r.left, r.right, r.raw_left, r.raw_right}) StateBytes::Put(out, value);
    StateBytes::Put(out, r.attack); StateBytes::Put(out, r.release); StateBytes::Put(out, r.looping);
}
bool GetPlay(StateBytes& in, MusicRequest& r) {
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
void PutStream(std::vector<uint8_t>& out, const MusicLibrary::Stream& s) {
    PutPlay(out, s.play);
    StateBytes::Put(out, s.window.offset); StateBytes::Put(out, s.window.size);
    StateBytes::Put64(out, s.generation); StateBytes::Put64(out, s.sequence); StateBytes::Put64(out, s.start_tick);
    for (const auto value : {s.parent, s.child, s.block, s.end, s.track, static_cast<uint32_t>(s.phase)})
        StateBytes::Put(out, value);
    StateBytes::Put(out, s.owned); StateBytes::Put(out, s.issued); StateBytes::Put(out, s.disabled);
    StateBytes::Put(out, s.buffer); StateBytes::Put64(out, s.refilled);
    StateBytes::Put(out, s.data.count); StateBytes::Put(out, s.data.header_bytes);
    StateBytes::Put(out, s.data.source_rate); StateBytes::Put64(out, s.data.source_bytes);
    for (const auto& asset : s.data.tracks) {
        StateBytes::Put(out, asset ? static_cast<uint32_t>(asset->size()) : 0);
        if (asset) out.insert(out.end(), asset->begin(), asset->end());
    }
}
bool GetStream(StateBytes& in, MusicLibrary::Stream& s, uint64_t& assets, uint32_t version) {
    uint32_t phase = 0;
    if (!GetPlay(in, s.play) || !in.Get(s.window.offset) || !in.Get(s.window.size) ||
        !in.Get64(s.generation) || !in.Get64(s.sequence) || !in.Get64(s.start_tick) ||
        !in.Get(s.parent) || !in.Get(s.child) || !in.Get(s.block) || !in.Get(s.end) ||
        !in.Get(s.track) || !in.Get(phase) || phase > static_cast<uint32_t>(MusicLibrary::Phase::Failed) ||
        !in.GetBool(s.owned) || !in.GetBool(s.issued) || !in.GetBool(s.disabled) ||
        (version >= 2 && (!in.Get(s.buffer) || !in.Get64(s.refilled))) ||
        !in.Get(s.data.count) || !in.Get(s.data.header_bytes) || !in.Get(s.data.source_rate) ||
        !in.Get64(s.data.source_bytes)) return false;
    s.phase = static_cast<MusicLibrary::Phase>(phase);
    for (auto& asset : s.data.tracks) {
        uint32_t size = 0;
        if (!in.Get(size) || size % 16 || (assets += size) > kMaxAssets) return false;
        if (!size) continue;
        std::vector<uint8_t> bytes;
        if (!in.GetBytes(size, bytes)) return false;
        asset = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    }
    return true;
}
}
bool MusicLibrary::ValidateData(const State& state, std::string* error) {
    if (state.generation > UINT64_MAX - Audio::kVoices || state.sequence == UINT64_MAX ||
        state.tick > (uint64_t{1} << 52) || state.windows.size() > 65536)
        return Fail(error, "music state exceeds bounds");
    for (const auto& window : state.windows)
        if (window.first < 0 || uint64_t{window.second.offset} + window.second.size > UINT32_MAX)
            return Fail(error, "invalid music file window");
    uint64_t bytes = 0;
    std::set<uint64_t> generations;
    for (uint32_t i = 0; i < Audio::kVoices; ++i) {
        const auto& s = state.streams[i];
        if (s.phase > Phase::Failed || state.requested[i] > 65536 ||
            (state.projected[i] != 0 && state.projected[i] != 2 && state.projected[i] != 3) ||
            (state.requested[i] && (!state.selected[i] || !state.projected[i])))
            return Fail(error, "invalid music readiness projection");
        if (!s.owned) {
            if (s.generation || s.sequence || Active(s.phase) || s.issued || s.data.count ||
                s.data.tracks[0] || s.data.tracks[1]) return Fail(error, "unowned music stream has live state");
            continue;
        }
        if (!state.selected[i] || !s.generation || s.generation > state.generation ||
            !generations.insert(s.generation).second || s.sequence > state.sequence ||
            s.play.stream != i || s.play.channel >= Audio::kVoices || s.play.group > Audio::kGroups ||
            s.play.file < 0 || s.play.attack > 127 || s.play.release > 31 ||
            (s.play.kind != MusicRequest::Kind::Play && s.play.kind != MusicRequest::Kind::Child) ||
            s.parent >= Audio::kVoices || (s.child != UINT32_MAX && s.child >= Audio::kVoices) ||
            s.track > 1 || s.block % 16 || s.end % 16 || s.start_tick > state.tick ||
            (s.issued && (s.parent != i || !s.sequence)) ||
            (s.buffer && s.buffer >= s.window.size) || s.refilled > (uint64_t{1} << 48) ||
            (!s.play.looping && s.issued && s.refilled > s.window.size))
            return Fail(error, "invalid music stream identity or extent");
        if (Active(s.phase)) {
            const auto& parent = state.streams[s.parent];
            if (!parent.owned || parent.parent != s.parent || (s.parent != i && parent.child != i))
                return Fail(error, "orphaned active music child");
            if (s.child < Audio::kVoices) {
                const auto& child = state.streams[s.child];
                if (s.parent != i || !child.owned || child.parent != i || child.child != UINT32_MAX ||
                    child.track == s.track || child.play.channel == s.play.channel || child.phase != s.phase)
                    return Fail(error, "invalid paired music relationship");
            }
            if (s.parent == i && (s.phase == Phase::Prepared || s.phase == Phase::Playing) &&
                s.data.count != (s.child < Audio::kVoices ? 2u : 1u))
                return Fail(error, "ready music has no complete asset");
        }
        if (s.data.count > 2 || (s.parent != i && s.data.count) ||
            (s.data.header_bytes != 0 && s.data.header_bytes != 48) ||
            s.data.source_bytes > UINT32_MAX)
            return Fail(error, "invalid music asset metadata");
        for (size_t side = 0; side < s.data.tracks.size(); ++side) {
            const auto& asset = s.data.tracks[side];
            if (static_cast<bool>(asset) != (side < s.data.count)) return Fail(error, "missing music track");
            if (!asset) continue;
            SampleVoice player;
            if (asset->empty() || (bytes += asset->size()) > kMaxAssets || !player.Start(asset, {}))
                return Fail(error, "invalid music track samples");
        }
    }
    return true;
}
bool MusicState::Encode(const MusicLibrary::State& state, std::vector<uint8_t>& bytes, std::string* error) {
    if (!MusicLibrary::ValidateData(state, error)) return false;
    std::vector<uint8_t> out;
    StateBytes::Put(out, kVersion); StateBytes::Put64(out, state.generation);
    StateBytes::Put64(out, state.sequence); StateBytes::Put64(out, state.tick);
    for (auto value : state.selected) StateBytes::Put(out, value);
    for (auto value : state.requested) StateBytes::Put(out, value);
    for (auto value : state.projected) StateBytes::Put(out, value);
    StateBytes::Put(out, static_cast<uint32_t>(state.windows.size()));
    for (const auto& window : state.windows) {
        StateBytes::Put(out, static_cast<uint32_t>(window.first));
        StateBytes::Put(out, window.second.offset); StateBytes::Put(out, window.second.size);
    }
    for (const auto& stream : state.streams) PutStream(out, stream);
    if (out.size() > kMaxState) return Fail(error, "music snapshot exceeds bound");
    bytes = std::move(out);
    return true;
}
bool MusicState::Decode(const std::vector<uint8_t>& bytes, MusicLibrary::State& state, std::string* error) {
    if (bytes.size() > kMaxState) return Fail(error, "music snapshot exceeds bound");
    StateBytes in(bytes);
    MusicLibrary::State out;
    uint32_t version = 0, count = 0;
    if (!in.Get(version) || version < 1 || version > kVersion || !in.Get64(out.generation) ||
        !in.Get64(out.sequence) || !in.Get64(out.tick)) return Fail(error, "invalid music header");
    for (auto& value : out.selected) if (!in.GetBool(value)) return Fail(error, "invalid music ownership");
    for (auto& value : out.requested) if (!in.Get(value)) return Fail(error, "invalid music readiness");
    for (auto& value : out.projected) if (!in.Get8(value)) return Fail(error, "invalid music projection");
    if (!in.Get(count) || count > 65536) return Fail(error, "invalid music window count");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t file = 0;
        MusicLibrary::Window window;
        if (!in.Get(file) || file > INT32_MAX || !in.Get(window.offset) || !in.Get(window.size) ||
            !out.windows.emplace(static_cast<int32_t>(file), window).second) return Fail(error, "invalid music window");
    }
    uint64_t assets = 0;
    for (auto& stream : out.streams) // a version-1 stream charged its whole window (buffer 0)
        if (!GetStream(in, stream, assets, version)) return Fail(error, "invalid music stream");
    if (in.Remaining() || !MusicLibrary::ValidateData(out, error)) return Fail(error, "invalid music closure");
    state = std::move(out);
    return true;
}
} // namespace title
