#include "games/slus_20909_crash_twinsanity/hle/audio/service_state.h"

#include <set>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
bool Fail(std::string* error, const char* text) { if (error) *error = text; return false; }
bool MusicActive(MusicLibrary::Phase phase) {
    return phase == MusicLibrary::Phase::Reading || phase == MusicLibrary::Phase::Prepared ||
           phase == MusicLibrary::Phase::Playing;
}
bool JoinMusic(const ServiceState::State& state, std::string* error) {
    std::array<uint32_t, Audio::kVoices> requested{};
    std::array<uint8_t, Audio::kVoices> projected{};
    for (const auto& command : state.controls.pending) {
        if (command.kind != SoundControls::Kind::Music) continue;
        const auto& r = command.music;
        if (r.kind == MusicRequest::Kind::Play || r.kind == MusicRequest::Kind::Child ||
            r.kind == MusicRequest::Kind::Stop) {
            ++requested[r.stream];
            projected[r.stream] = r.kind == MusicRequest::Kind::Stop ? 3 : 2;
        }
    }
    if (requested != state.music.requested) return Fail(error, "music readiness has no queued request");
    for (uint32_t i = 0; i < Audio::kVoices; ++i) {
        if (requested[i] && projected[i] != state.music.projected[i])
            return Fail(error, "music projected operation disagrees with pending batch");
        const auto& s = state.music.streams[i];
        if (!s.owned || !MusicActive(s.phase)) continue;
        const auto& voice = state.audio.playback.voices[s.play.channel];
        if (voice.music_stream != i || voice.music_generation != s.generation)
            return Fail(error, "active music stream lost its paired voice");
        if (state.controls.buffers) {
            // A child streams through its parent's buffer (the title opens
            // the paired stream without one); the pair is joined on the
            // parent's slot.
            const auto owner = s.parent < Audio::kVoices ? s.parent : i;
            const auto& slot = state.buffers.storage.slots[owner];
            const auto storage = state.buffers.storage.storage.find(slot.handle);
            if (!state.buffers.selected[owner] || !slot.allocated ||
                (slot.bytes && (storage == state.buffers.storage.storage.end() ||
                                !storage->second.owned || storage->second.loans)))
                return Fail(error, "active music stream lost its buffer ownership");
        }
    }
    for (const auto& voice : state.audio.playback.voices) {
        if (voice.music_stream == UINT32_MAX) continue;
        const auto& s = state.music.streams[voice.music_stream];
        if (!voice.music_generation || voice.music_generation > state.music.generation)
            return Fail(error, "music voice has a future generation");
        // A finished old channel retains silent ownership until handoff.
        // It must not be attached to the new stream that reused its index.
        if ((!s.owned || s.generation != voice.music_generation) && voice.player.Playing())
            return Fail(error, "audible music voice has no current stream");
    }
    return true;
}
bool JoinReads(const ServiceState::State& state, std::string* error) {
    auto banks = state.audio.pending_loads;
    std::set<uint32_t> music;
    for (const auto& entry : state.files.streams.schedule.reads) {
        const auto& read = entry.second;
        const auto& p = *read.payload;
        if (p.kind == FileSchedule::Destination::Sound) {
            if (p.generation > state.audio.load_generation || p.sequence > state.audio.load_sequence)
                return Fail(error, "bank payload has a future ticket");
            const auto ticket = state.audio.pending_loads.find(p.sound);
            const bool current = p.generation == state.audio.load_generation &&
                ticket != state.audio.pending_loads.end() && ticket->second == p.sequence;
            if (current && (read.phase >= FileSchedule::Phase::Delivered || !banks.erase(p.sound)))
                return Fail(error, "bank request/ticket closure mismatch");
        } else if (p.kind == FileSchedule::Destination::Music) {
            if (!state.controls.music) return Fail(error, "music payload has no selected owner");
            const auto& s = state.music.streams[p.stream];
            if (!s.owned || s.parent != p.stream || s.generation != p.generation ||
                s.sequence != p.sequence || s.play.file != p.handle || s.window.offset != p.file_offset ||
                s.window.size != p.bytes.size() || !s.issued || !music.insert(p.stream).second)
                return Fail(error, "music payload/stream identity mismatch");
            if ((read.phase < FileSchedule::Phase::Delivered) != (s.phase == MusicLibrary::Phase::Reading))
                return Fail(error, "music bytes and readiness disagree");
        }
    }
    if (!banks.empty()) return Fail(error, "pending bank ticket has no file payload");
    if (state.controls.music)
        for (uint32_t i = 0; i < Audio::kVoices; ++i) {
            const auto& s = state.music.streams[i];
            if (s.owned && s.parent == i && s.phase == MusicLibrary::Phase::Reading &&
                s.issued && !music.count(i)) return Fail(error, "pending music read lost its payload");
        }
    return true;
}
}
bool ServiceState::Capture(const Audio& audio, State& state, std::string* error) {
    State snapshot;
    if (!LoaderFileIo::CaptureState(snapshot.files) ||
        !SoundControls::CaptureState(snapshot.controls, error)) return Fail(error, "service owner is delivering");
    snapshot.audio = audio.CaptureLoads();
    if (snapshot.controls.music && !MusicLibrary::CaptureState(snapshot.music, error)) return false;
    if (snapshot.controls.buffers && !MusicBuffers::CaptureState(snapshot.buffers, error)) return false;
    if (!Validate(audio, snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool ServiceState::Validate(const Audio& audio, const State& state, std::string* error) {
    if (!SoundBanks::Owns(audio) || !SoundControls::Owns(audio) ||
        state.controls.music != MusicLibrary::Enabled() || state.controls.buffers != MusicBuffers::Enabled() ||
        (state.controls.music && !MusicLibrary::Owns(audio)) ||
        (state.controls.buffers && !state.controls.music) ||
        !state.files.streams.enabled || !state.files.streams.has_schedule)
        return Fail(error, "service selection or Audio ownership mismatch");
    if (!LoaderFileIo::ValidateState(state.files, error) || !audio.CanRestoreLoads(state.audio) ||
        !SoundControls::ValidateState(state.controls, error)) return Fail(error, "invalid file/Audio/control state");
    const auto tick = state.files.streams.schedule.tick;
    if ((state.controls.clocked && state.controls.tick > tick) ||
        (state.controls.music && state.music.tick > tick)) return Fail(error, "service clocks exceed saved game clock");
    if (state.controls.music) {
        if (!MusicLibrary::ValidateState(state.music, error) || !JoinMusic(state, error)) return false;
    } else {
        for (const auto& voice : state.audio.playback.voices)
            if (voice.music_stream != UINT32_MAX) return Fail(error, "music voice has no selected owner");
    }
    if (state.controls.buffers && !MusicBuffers::ValidateState(state.buffers, error)) return false;
    return JoinReads(state, error);
}
bool ServiceState::ValidateMovie(const State& state, const std::vector<StreamBuffers::Loan>& references,
                                std::string* error) {
    if (state.controls.buffers) return BufferState::ValidateMovie(state.buffers, references, error);
    return references.empty() || Fail(error, "Movie loan has no selected buffer owner");
}
bool ServiceState::Encode(const Audio& audio, const State& state, std::vector<uint8_t>& bytes, std::string* error) {
    if (!Validate(audio, state, error)) return false;
    std::array<std::vector<uint8_t>, 5> parts;
    if (!StreamState::Encode(state.files, parts[0], error) || !AudioState::Encode(state.audio, parts[1], error) ||
        !ControlState::Encode(state.controls, parts[2], error) ||
        (state.controls.music && !MusicState::Encode(state.music, parts[3], error)) ||
        (state.controls.buffers && !BufferState::Encode(state.buffers, parts[4], error))) return false;
    std::vector<uint8_t> out;
    StateBytes::Put(out, 1);
    for (const auto& part : parts) {
        StateBytes::Put(out, static_cast<uint32_t>(part.size()));
        out.insert(out.end(), part.begin(), part.end());
    }
    bytes = std::move(out);
    return true;
}
bool ServiceState::Decode(const Audio& audio, const std::vector<uint8_t>& bytes, State& state, std::string* error) {
    if (bytes.size() > 256 * 1024 * 1024) return Fail(error, "service state exceeds bound");
    StateBytes in(bytes);
    uint32_t version = 0, size = 0;
    if (!in.Get(version) || version != 1) return Fail(error, "invalid service version");
    std::array<std::vector<uint8_t>, 5> parts;
    for (auto& part : parts)
        if (!in.Get(size) || !in.GetBytes(size, part)) return Fail(error, "invalid service component extent");
    State out;
    if (in.Remaining() || !StreamState::Decode(parts[0], out.files, error) ||
        !AudioState::Decode(parts[1], out.audio, error) || !ControlState::Decode(parts[2], out.controls, error) ||
        (out.controls.music ? !MusicState::Decode(parts[3], out.music, error) : !parts[3].empty()) ||
        (out.controls.buffers ? !BufferState::Decode(parts[4], out.buffers, error) : !parts[4].empty()) ||
        !Validate(audio, out, error)) return Fail(error, "invalid composed service state");
    state = std::move(out);
    return true;
}
bool ServiceState::Restore(Audio& audio, const State& state, const std::vector<StreamBuffers::Loan>& references,
                          StreamBuffers::RenewedLoans& renewed, std::string* error) {
    if (!Validate(audio, state, error) || !ValidateMovie(state, references, error)) return false;
    FileSchedule::State current;
    auto* schedule = StreamLibrary::Scheduling();
    if (!schedule || !schedule->Capture(current, error) || current.tick != state.files.streams.schedule.tick)
        return Fail(error, "restore game clock before services");
    if (state.controls.buffers) {
        MusicBuffers::State buffers;
        if (!MusicBuffers::CaptureState(buffers, error) || !buffers.storage.loans.empty())
            return Fail(error, "return current Movie loans before services restore");
    }
    // Prepare all payload copies before invalidating the old world.
    auto files = state.files;
    auto playback = state.audio;
    std::vector<std::shared_ptr<FileSchedule::Payload>> payloads;
    for (auto& entry : files.streams.schedule.reads) {
        if (entry.second.payload->kind == FileSchedule::Destination::Memory) continue;
        auto copy = std::make_shared<FileSchedule::Payload>(*entry.second.payload);
        entry.second.payload = copy; payloads.push_back(std::move(copy));
    }
    if (!SoundControls::Reset()) return Fail(error, "sound delivery began during restore");
    schedule->Reset(); // old Audio/Music cancellation runs before new generations exist
    MusicLibrary::Generations music{};
    if (state.controls.music) {
        MusicLibrary::Reset();
        if (!MusicLibrary::RestoreState(state.music, music, error)) return false;
        for (auto& voice : playback.playback.voices)
            if (voice.music_stream < Audio::kVoices &&
                voice.music_generation == state.music.streams[voice.music_stream].generation)
                voice.music_generation = music[voice.music_stream];
    }
    uint64_t bank_generation = 0;
    if (!audio.RestoreLoads(playback, bank_generation)) return Fail(error, "Audio restore preflight changed");
    for (auto& p : payloads) {
        if (p->kind == FileSchedule::Destination::Sound && p->generation == state.audio.load_generation)
            p->generation = bank_generation;
        if (p->kind == FileSchedule::Destination::Music) p->generation = music[p->stream];
    }
    StreamBuffers::RenewedLoans loans;
    if (state.controls.buffers && !MusicBuffers::RestoreState(state.buffers, loans, error)) return false;
    if (!LoaderFileIo::RestoreState(files, error) || !SoundControls::RestoreState(state.controls, error)) return false;
    renewed = std::move(loans);
    return true;
}
} // namespace title
