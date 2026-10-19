#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"

#include <array>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"

namespace title {
namespace {
constexpr uint32_t kWindow = 0x1e2ee0, kPlay = 0x1e1500, kInterleave = 0x1e2fd0,
    kChild = 0x1e30d0, kEnd = 0x1e7800, kPrepare = 0x1e7990, kStart = 0x1e78f0,
    kStop = 0x1e6c28, kStatus = 0x1e3308, kReply = 0x1e1f48;
constexpr std::array<uint32_t, 10> kEntries{
    kWindow, kPlay, kInterleave, kChild, kEnd, kPrepare, kStart, kStop, kStatus, kReply};
constexpr uint32_t kStreams = 48, kStates = 0x3b3d00, kActive = 0x3b3dc0;
using Window = MusicLibrary::Window;
using Stream = MusicLibrary::Stream;
Audio* g_audio = nullptr;
std::array<Stream, kStreams> g_streams;
std::array<bool, kStreams> g_selected{};
std::array<uint32_t, kStreams> g_requested{};
std::array<uint8_t, kStreams> g_projected{};
std::map<int32_t, Window> g_windows;
std::array<StreamLibrary::Body, kEntries.size()> g_originals;
StreamLibrary::Body g_first_free, g_free_in;
uint64_t g_generation = 0, g_sequence = 0, g_tick = 0;
bool g_installed = false;
MusicLibrary::Counters g_count;

uint32_t Arg(const CpuContext& ctx, unsigned reg) { return ctx.cpuRegs.GPR.r[reg].UL[0]; }
void Result(CpuContext& ctx, int32_t value) { ctx.cpuRegs.GPR.r[2].SD[0] = value; }
bool Selected(uint32_t stream) { return stream < kStreams && g_selected[stream]; }
StreamLibrary::Body& Original(uint32_t address) {
    for (size_t i = 0; i < kEntries.size(); ++i)
        if (kEntries[i] == address) return g_originals[i];
    throw std::logic_error("unknown music service entry");
}
uint32_t Parent(uint32_t stream) {
    if (stream >= kStreams) return kStreams;
    const auto parent = g_streams[stream].parent;
    return parent < kStreams && (parent == stream || g_streams[parent].child == stream) ? parent : stream;
}
bool CurrentVoice(uint32_t stream) {
    const auto& s = g_streams[stream];
    if (!g_audio || s.play.channel >= Audio::kVoices) return false;
    const auto& v = g_audio->Inspect().voices[s.play.channel];
    return v.music_stream == stream && v.music_generation == s.generation;
}
void Publish() {
    if (!g_audio) return;
    for (uint32_t stream = 0; stream < kStreams; ++stream) {
        auto& s = g_streams[stream];
        if (!s.owned) continue;
        if (s.phase == MusicLibrary::Phase::Playing &&
            (!CurrentVoice(stream) || g_audio->IsVoiceFree(s.play.channel)))
            s.phase = MusicLibrary::Phase::Stopped;
        const bool ready = s.phase == MusicLibrary::Phase::Prepared || s.phase == MusicLibrary::Phase::Playing;
        memory::write<uint8_t>(kStates + stream, g_requested[stream] ? g_projected[stream] :
            s.phase == MusicLibrary::Phase::Reading ? 5 : ready ? 1 : 0);
        memory::write<uint8_t>(kActive + stream, ready && !(g_requested[stream] && g_projected[stream] == 2) ? 1 : 0);
        memory::write<uint8_t>(0x3b3d60 + stream, s.play.looping ? 0 : 1);
        memory::write<uint8_t>(0x3b3d90 + stream, s.play.channel);
        memory::write<int32_t>(0x3b3b80 + 4 * stream, s.play.file);
        const auto& voice = g_audio->Inspect().voices[s.play.channel];
        memory::write<uint16_t>(0x2e7590 + 2 * stream, voice.player.Capture().parameters.pitch);
        memory::write<uint32_t>(0x3b3c40 + 4 * stream,
            s.phase == MusicLibrary::Phase::Playing ? static_cast<uint32_t>(g_tick - s.start_tick) : 0);
        memory::write<uint16_t>(0x3b3e20 + 2 * stream, CurrentVoice(stream) ?
            static_cast<uint16_t>(voice.player.Capture().envelope * 32767.0f) : 0);
        memory::write<uint32_t>(0x3b4160 + 4 * stream, CurrentVoice(stream) ?
            voice.player.Capture().block_offset : 0);
        // Buffer metadata belongs to MusicBuffers when composed, otherwise
        // the retained allocation service. No address is invented here.
    }
}
void Stop(uint32_t stream, MusicLibrary::Phase phase = MusicLibrary::Phase::Stopped) {
    const auto parent = Parent(stream);
    if (parent >= kStreams) return;
    const uint32_t child = g_streams[parent].child;
    for (const auto index : {parent, child}) {
        if (index >= kStreams) continue;
        auto& s = g_streams[index];
        if (g_audio) g_audio->StopMusicVoice(index, s.generation, s.play.channel);
        s.phase = phase;
        s.issued = false;
        s.data = {};
        // Invalidate before a cancellation callback can observe this owner.
        s.sequence = 0;
        if (auto* schedule = StreamLibrary::Scheduling())
            schedule->CancelObject(FileSchedule::MusicObject(index));
    }
}
// The buffer the pair streams through is the parent's: the title opens the
// paired second stream without a buffer of its own and only names the paired
// sound address on AddStreamChannel, so a child is reserved against its
// parent's owned storage, never against a slot of its own.
bool Reserve(uint32_t stream, uint32_t buffer_stream) {
    const auto& s = g_streams[stream];
    const auto& p = s.play;
    if (!MusicBuffers::CanPlay(buffer_stream)) { ++g_count.refused_buffers; return false; }
    if (!g_audio->ReserveMusicVoice(stream, s.generation, p.channel | (p.group << 16),
            p.pitch, p.left, p.right, p.raw_left, p.raw_right, p.attack, p.release)) {
        ++g_count.refused_voice;
        return false;
    }
    return true;
}
void Start(uint32_t parent) {
    auto& s = g_streams[parent];
    if (s.phase != MusicLibrary::Phase::Prepared || s.disabled) return;
    const auto child = s.child;
    if (!CurrentVoice(parent) || (child < kStreams && !CurrentVoice(child))) {
        ++g_count.errors; Stop(parent, MusicLibrary::Phase::Failed); return;
    }
    for (const auto index : {parent, child}) {
        if (index >= kStreams) continue;
        auto& member = g_streams[index];
        if (!g_audio->StartMusicVoice(index, member.generation, member.play.channel,
                                     s.data.tracks[member.track])) {
            ++g_count.errors; Stop(parent, MusicLibrary::Phase::Failed); return;
        }
        member.phase = MusicLibrary::Phase::Playing;
        member.start_tick = g_tick;
    }
    if (!g_count.starts) g_count.first_start_tick = g_tick;
    ++g_count.starts;
}
void Deliver(const FileSchedule::Payload& payload) {
    if (!g_audio || payload.stream >= kStreams) { ++g_count.stale; return; }
    auto& s = g_streams[payload.stream];
    if (s.generation != payload.generation || s.sequence != payload.sequence ||
        s.play.file != payload.handle || s.phase != MusicLibrary::Phase::Reading) {
        ++g_count.stale; return;
    }
    const MusicAsset::Layout layout{s.child < kStreams ? 2u : 1u, s.block, s.end, s.play.looping};
    if (!MusicAsset::Read(payload.bytes, layout, s.data)) {
        ++g_count.errors; Stop(payload.stream, MusicLibrary::Phase::Failed); Publish(); return;
    }
    s.phase = MusicLibrary::Phase::Prepared;
    if (s.child < kStreams) g_streams[s.child].phase = MusicLibrary::Phase::Prepared;
    ++g_count.delivered; g_count.bytes += payload.bytes.size();
    Start(payload.stream);
    Publish(); // data and both voices precede readiness
}
void Cancel(const FileSchedule::Payload& payload) {
    if (payload.stream >= kStreams) return;
    const auto& s = g_streams[payload.stream];
    if (s.generation != payload.generation || s.sequence != payload.sequence) return;
    Stop(payload.stream, MusicLibrary::Phase::Failed);
    Publish();
}
void Apply(const MusicRequest& request) {
    const uint32_t index = request.stream;
    if (request.kind == MusicRequest::Kind::Window) {
        g_windows[request.file] = {request.offset, request.size};
        ++g_count.windows;
        return;
    }
    if (request.kind == MusicRequest::Kind::Reset) {
        for (uint32_t i = 0; i < kStreams; ++i)
            if (g_streams[i].owned && g_streams[i].parent == i) Stop(i);
        return;
    }
    if (index >= kStreams) { ++g_count.errors; return; }
    auto& s = g_streams[index];
    switch (request.kind) {
    case MusicRequest::Kind::Play: {
        Stop(index);
        s = {};
        s.owned = true; s.parent = index; s.play = request; s.generation = ++g_generation;
        g_selected[index] = true;
        s.phase = MusicLibrary::Phase::Reading;
        if (g_requested[index]) --g_requested[index];
        const auto window = g_windows.find(request.file);
        if (window == g_windows.end()) ++g_count.refused_window;
        if (window == g_windows.end() || !Reserve(index, index)) {
            s.phase = MusicLibrary::Phase::Failed; ++g_count.errors;
        } else s.window = window->second;
        if (!g_count.plays) g_count.first_play_tick = g_tick;
        ++g_count.plays;
        break;
    }
    case MusicRequest::Kind::Child: {
        if (g_requested[index]) --g_requested[index];
        auto& parent = g_streams[request.parent];
        if (!parent.owned || parent.parent != request.parent || parent.issued ||
            parent.phase != MusicLibrary::Phase::Reading || parent.child != UINT32_MAX ||
            request.channel == parent.play.channel || request.track == parent.track) {
            ++g_count.errors; break;
        }
        Stop(index);
        s = {};
        s.owned = true; s.parent = request.parent; s.play = request; s.generation = ++g_generation;
        g_selected[index] = true;
        s.play.file = parent.play.file; s.play.pitch = parent.play.pitch;
        s.play.looping = parent.play.looping; s.play.attack = parent.play.attack; s.play.release = parent.play.release;
        s.track = request.track; s.phase = MusicLibrary::Phase::Reading;
        parent.child = index;
        if (!Reserve(index, request.parent)) { ++g_count.errors; Stop(request.parent, MusicLibrary::Phase::Failed); }
        ++g_count.children;
        break;
    }
    case MusicRequest::Kind::Interleave:
        if (s.issued) { ++g_count.errors; break; }
        s.block = request.block; s.track = request.track; ++g_count.interleaves; break;
    case MusicRequest::Kind::End:
        if (s.issued) { ++g_count.errors; break; }
        s.end = request.end; ++g_count.ends; break;
    case MusicRequest::Kind::Prepare: g_streams[Parent(index)].disabled = true; ++g_count.prepares; break;
    case MusicRequest::Kind::Start: {
        const auto parent = Parent(index);
        g_streams[parent].disabled = false; Start(parent); break;
    }
    case MusicRequest::Kind::Stop:
        if (g_requested[index]) --g_requested[index];
        Stop(index); ++g_count.stops; break;
    case MusicRequest::Kind::Window:
    case MusicRequest::Kind::Reset: break;
    }
}
void Finish() {
    // Interleave/end/child/prepare in this accepted batch are applied before
    // issuing the file window. No completion can expose a half-configured pair.
    for (uint32_t index = 0; index < kStreams; ++index) {
        auto& s = g_streams[index];
        if (!s.owned || s.parent != index || s.issued || s.phase != MusicLibrary::Phase::Reading) continue;
        if (s.block && s.child == UINT32_MAX) continue;
        s.sequence = ++g_sequence; s.issued = true;
        // Readiness at the stream's buffer fill, not the whole window: the
        // module keys the channel on once its buffers are full (TR
        // multistream.h MsAllowKeyOn), the rest streams in as refills. The
        // buffer is the owned slot's when a buffers owner is composed, else
        // two blocks of every track for an interleaved pair (TR
        // MsInterleavedBufferSize); otherwise the whole window is charged.
        s.buffer = MusicBuffers::BufferBytes(index);
        if (!s.buffer && s.block && s.child < kStreams) s.buffer = s.block * 2 * 2;
        if (s.buffer >= s.window.size) s.buffer = 0;
        s.refilled = s.buffer ? s.buffer : s.window.size;
        if (!LoaderFileIo::StartMusicRead(index, s.generation, s.sequence, s.play.file,
                                         s.window.offset, s.window.size, s.buffer)) {
            ++g_count.errors; Stop(index, MusicLibrary::Phase::Failed);
        } else { ++g_count.reads; if (s.buffer) ++g_count.fills; }
    }
    Publish();
}
// The drive time of a playing stream's refills: the module reads another
// half of the buffer each time the voice has consumed one, keeping the
// buffer ahead of the channel. Charged on the shared read position at the
// service instant (nothing is read: the window is owned); a looping stream
// keeps refilling from the window's start.
void Refill() {
    if (!g_audio) return;
    for (uint32_t index = 0; index < kStreams; ++index) {
        auto& s = g_streams[index];
        if (!s.owned || s.parent != index || s.phase != MusicLibrary::Phase::Playing || !s.buffer ||
            s.play.channel >= Audio::kVoices || !CurrentVoice(index) || s.data.count == 0) continue;
        const auto& voice = g_audio->Inspect().voices[s.play.channel].player.Capture();
        const auto& track = s.data.tracks[s.track < s.data.count ? s.track : 0];
        if (!track || track->empty()) continue;
        const uint64_t consumed = (voice.ends * track->size() + voice.block_offset) * s.data.count + s.data.header_bytes;
        const uint64_t half = std::max<uint64_t>(s.buffer / 2, 16);
        // Refill k lands when the voice has consumed k halves: the buffer
        // stays one fill ahead of the channel, half by half.
        const uint64_t ahead = s.buffer + (consumed / half) * half;
        for (unsigned laps = 0; laps < 8 && s.refilled < ahead; ++laps) {
            if (!s.play.looping && s.refilled >= s.window.size) break;
            const uint64_t at = s.refilled % s.window.size;
            const uint64_t bytes = std::min<uint64_t>(half, s.window.size - at);
            if (!LoaderFileIo::ChargeMusicRefill(s.play.file, uint64_t{s.window.offset} + at,
                                                static_cast<uint32_t>(bytes))) break;
            s.refilled += bytes;
            ++g_count.refills; g_count.refill_bytes += bytes;
        }
    }
}
void Enter(uint32_t address, CpuContext& ctx, uint32_t entry) {
    auto& original = Original(address);
    auto fallback = [&] { ++g_count.fallback; original(ctx, entry); };
    if (!g_audio || (entry != 0 && entry != address)) { fallback(); return; }
    const auto a = Arg(ctx, 4), b = Arg(ctx, 5), c = Arg(ctx, 6);
    if (address == kReply) { original(ctx, entry); Publish(); return; }
    if (address == kStatus) {
        if (Selected(a)) {
            if (auto* schedule = StreamLibrary::Scheduling())
                schedule->Poll(FileSchedule::MusicObject(Parent(a)));
            Publish();
            ++g_count.statuses;
        }
        original(ctx, entry); // pure client view; preserves the complete ABI
        return;
    }
    MusicRequest request;
    uint32_t words = 0;
    if (address == kWindow) {
        if (!StreamLibrary::MusicFileOwned(static_cast<int32_t>(a))) { fallback(); return; }
        request.kind = MusicRequest::Kind::Window; request.file = static_cast<int32_t>(a);
        request.offset = b; request.size = c; words = 8;
    } else if (address == kPlay) {
        if (!StreamLibrary::MusicFileOwned(static_cast<int32_t>(a))) { fallback(); return; }
        if (Arg(ctx, 10) > 1) { ++g_count.errors; Result(ctx, -1); return; }
        const auto count = memory::read<int32_t>(Arg(ctx, 28) - 0x794c);
        uint32_t chosen = b;
        if (b == 0x80) {
            auto query = ctx;
            Publish(); g_first_free(query, 0); chosen = Arg(query, 2);
        } else if (b == 0x81 || b == 0x82) {
            chosen = UINT32_MAX;
            for (int32_t first = 0; first < count && first < static_cast<int32_t>(kStreams);) {
                auto query = ctx;
                query.cpuRegs.GPR.r[4].SD[0] = first;
                query.cpuRegs.GPR.r[5].SD[0] = count - 1;
                Publish(); g_free_in(query, 0);
                const auto found = query.cpuRegs.GPR.r[2].SL[0];
                if (found < first || found >= count || found >= static_cast<int32_t>(kStreams)) break;
                if (memory::read<uint8_t>(0x2e7650 + found) == (b == 0x81 ? 1 : 0) || found == count - 1) {
                    chosen = static_cast<uint32_t>(found); break;
                }
                first = found + 1;
            }
        }
        if (chosen >= kStreams || static_cast<int32_t>(chosen) >= count || (c & 0xffff) >= Audio::kVoices) {
            Result(ctx, -1); return;
        }
        request.kind = MusicRequest::Kind::Play; request.stream = chosen; request.file = static_cast<int32_t>(a);
        request.channel = c & 0xffff; request.group = c >> 16;
        request.left = Arg(ctx, 7); request.right = Arg(ctx, 8); request.pitch = Arg(ctx, 9);
        request.looping = Arg(ctx, 10) == 0;
        const auto attack = Arg(ctx, 11), release = memory::read<uint32_t>(Arg(ctx, 29));
        if (attack > 127 || release > 31) { ++g_count.errors; Result(ctx, -1); return; }
        request.attack = attack; request.release = release; words = 10;
    } else {
        uint32_t stream = a;
        if (address == kStop && a >= 0x40 && a < 0x40 + Audio::kVoices)
            stream = memory::read<uint8_t>(0x2e79d8 + a - 0x40);
        if (address == kChild) {
            if (!Selected(b)) { fallback(); return; }
            if (stream >= kStreams || stream == b || (c & 0xffff) >= Audio::kVoices || Arg(ctx, 7) > 1) {
                ++g_count.errors; return;
            }
            request.kind = MusicRequest::Kind::Child; request.parent = b;
            request.channel = c & 0xffff; request.group = c >> 16; request.track = Arg(ctx, 7);
            request.left = Arg(ctx, 8); request.right = Arg(ctx, 9); words = 9;
        } else {
            if (!Selected(stream)) { fallback(); return; }
            if (address == kInterleave) {
                if (b > 1 || c == 0 || c % 16) { ++g_count.errors; return; }
                request.kind = MusicRequest::Kind::Interleave; request.track = b; request.block = c; words = 6;
            } else if (address == kEnd) {
                if (!b || b % 16) { Result(ctx, -1); return; }
                request.kind = MusicRequest::Kind::End; request.end = b; words = 5;
            } else {
                request.kind = address == kPrepare ? MusicRequest::Kind::Prepare :
                    address == kStart ? MusicRequest::Kind::Start : MusicRequest::Kind::Stop;
                words = 3;
            }
        }
        request.stream = stream;
    }
    if (!SoundControls::StageMusic(ctx, request, words)) { ++g_count.errors; Result(ctx, -1); return; }
    if (request.kind == MusicRequest::Kind::Play || request.kind == MusicRequest::Kind::Child) {
        g_selected[request.stream] = true; ++g_requested[request.stream];
        g_projected[request.stream] = 2;
        memory::write<uint8_t>(kStates + request.stream, 2);
        memory::write<uint8_t>(kActive + request.stream, 0);
        memory::write<uint8_t>(0x2e79d8 + request.channel, request.stream);
        memory::write<uint16_t>(0x2e75f0 + 2 * request.stream, 0x8080);
    } else if (request.kind == MusicRequest::Kind::Stop) {
        ++g_requested[request.stream]; g_projected[request.stream] = 3;
        memory::write<uint8_t>(kStates + request.stream, 3);
        if (a >= 0x40 && a < 0x40 + Audio::kVoices) memory::write<uint8_t>(0x2e79d8 + a - 0x40, 0xff);
    }
    if (address != kWindow && address != kInterleave && address != kChild)
        Result(ctx, address == kPlay ? static_cast<int32_t>(request.stream) : 0);
}
} // namespace

bool MusicLibrary::Install(const Capture& capture, const Installer& install, Audio* selected) {
    if (selected && (!StreamLibrary::Enabled() || !SoundControls::Enabled())) return false;
    if (selected == g_audio && g_installed) return true;
    for (const auto& s : g_streams)
        if (s.phase == Phase::Reading || s.phase == Phase::Prepared || s.phase == Phase::Playing) return false;
    if (!g_installed) {
        for (size_t i = 0; i < kEntries.size(); ++i) {
            g_originals[i] = capture(kEntries[i]);
            if (!g_originals[i]) return false;
        }
        g_first_free = capture(0x1e6aa0); g_free_in = capture(0x1e6af0);
        if (!g_first_free || !g_free_in) return false;
        for (size_t i = 0; i < kEntries.size(); ++i) {
            const auto address = kEntries[i];
            if (!install(address, [address](CpuContext& ctx, uint32_t entry) { Enter(address, ctx, entry); })) {
                for (size_t j = 0; j < i; ++j) install(kEntries[j], g_originals[j]);
                return false;
            }
        }
        g_installed = true;
    }
    if (!SoundControls::BindMusic(selected ? Apply : SoundControls::MusicApply{},
                                 selected ? Finish : std::function<void()>{})) return false;
    if (!StreamLibrary::BindMusic(selected ? Deliver : FileSchedule::Apply{},
                                  selected ? Cancel : FileSchedule::Apply{})) {
        SoundControls::BindMusic(g_audio ? Apply : SoundControls::MusicApply{},
                                 g_audio ? Finish : std::function<void()>{});
        return false;
    }
    g_audio = selected;
    return true;
}
bool MusicLibrary::Enabled() { return g_audio != nullptr; }
bool MusicLibrary::Owns(const Audio& audio) { return g_audio == &audio; }
bool MusicLibrary::CaptureState(State& state, std::string* error) {
    State snapshot{g_streams, g_selected, g_requested, g_projected, g_windows,
                   g_generation, g_sequence, g_tick};
    if (!ValidateState(snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool MusicLibrary::ValidateState(const State& state, std::string* error) {
    SoundControls::State controls;
    if (!g_audio || !SoundControls::Owns(*g_audio) || !SoundControls::CaptureState(controls, error) ||
        std::max(state.generation, g_generation) > UINT64_MAX - kStreams ||
        state.sequence == UINT64_MAX) {
        if (error) *error = "music owner unavailable, delivering, or generation exhausted";
        return false;
    }
    return ValidateData(state, error);
}
bool MusicLibrary::RestoreState(const State& state, Generations& renewed, std::string* error) {
    if (!ValidateState(state, error)) return false;
    SoundControls::State controls;
    const auto* schedule = StreamLibrary::Scheduling();
    if ((schedule && schedule->HasMusicReads()) ||
        !SoundControls::CaptureState(controls, error) || !controls.pending.empty()) {
        if (error) *error = "discard old music reads and controls before restore";
        return false;
    }
    auto snapshot = state;
    auto generation = std::max(state.generation, g_generation);
    Generations generations{};
    for (uint32_t i = 0; i < kStreams; ++i) if (snapshot.streams[i].owned) {
        snapshot.streams[i].generation = ++generation;
        generations[i] = generation;
    }
    g_streams = std::move(snapshot.streams); g_windows = std::move(snapshot.windows);
    g_selected = state.selected; g_requested = state.requested; g_projected = state.projected;
    g_generation = generation; g_sequence = std::max(g_sequence, state.sequence); g_tick = state.tick;
    renewed = generations;
    return true;
}
void MusicLibrary::Reset() {
    for (uint32_t i = 0; i < kStreams; ++i)
        if (g_streams[i].owned && g_streams[i].parent == i) Stop(i);
    g_streams = {}; g_selected.fill(false); g_requested.fill(0); g_projected.fill(0); g_windows.clear();
    g_tick = 0;
}
void MusicLibrary::CloseBuffer(uint32_t stream) {
    if (stream < kStreams && g_streams[stream].owned) Stop(stream);
    Publish();
}
bool MusicLibrary::CanCheckpoint() { return !Enabled(); }
void MusicLibrary::AdvanceBoundary(uint64_t tick) { g_tick = tick; Refill(); Publish(); }
MusicLibrary::Status MusicLibrary::Inspect(uint32_t stream) {
    if (stream >= kStreams) return {};
    const auto& s = g_streams[stream];
    return {s.phase, s.parent, s.play.channel, s.play.file, s.generation,
            g_streams[Parent(stream)].data.source_bytes};
}
MusicLibrary::Counters MusicLibrary::Count() { return g_count; }
} // namespace title
