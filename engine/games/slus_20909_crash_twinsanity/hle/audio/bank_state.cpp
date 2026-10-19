#include "games/slus_20909_crash_twinsanity/hle/audio/bank_state.h"

#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
bool Fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
bool BankState::Capture(const Audio& audio, State& state, std::string* error) {
    State snapshot;
    if (!LoaderFileIo::CaptureState(snapshot.files)) return Fail(error, "file owner is delivering");
    snapshot.audio = audio.CaptureLoads();
    if (!Validate(audio, snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool BankState::Validate(const Audio& audio, const State& state, std::string* error) {
    if (!SoundBanks::Owns(audio) || !state.files.streams.enabled || !state.files.streams.has_schedule)
        return Fail(error, "bank snapshot requires selected file and Audio owners");
    if (!LoaderFileIo::ValidateState(state.files, error)) return false;
    if (!audio.CanRestoreLoads(state.audio)) return Fail(error, "invalid or exhausted Audio owner");
    for (const auto& voice : state.audio.playback.voices)
        if (voice.music_stream != UINT32_MAX) return Fail(error, "music voice requires Music state");
    auto pending = state.audio.pending_loads;
    for (const auto& entry : state.files.streams.schedule.reads) {
        const auto& read = entry.second;
        const auto& payload = *read.payload;
        if (payload.kind == FileSchedule::Destination::Music) return Fail(error, "music read requires Music state");
        if (payload.kind != FileSchedule::Destination::Sound) continue;
        if (payload.generation > state.audio.load_generation || payload.sequence > state.audio.load_sequence)
            return Fail(error, "bank request has a future Audio ticket");
        const auto ticket = state.audio.pending_loads.find(payload.sound);
        const bool current = payload.generation == state.audio.load_generation &&
            ticket != state.audio.pending_loads.end() && ticket->second == payload.sequence;
        if (current) {
            if (read.phase >= FileSchedule::Phase::Delivered) return Fail(error, "delivered bank has pending Audio ticket");
            if (!pending.erase(payload.sound)) return Fail(error, "duplicate pending Audio ticket");
        }
        // Freed/replaced tickets can still be in the ordered file schedule.
        // Preserve that delivery as stale; never resurrect its asset.
    }
    if (!pending.empty()) return Fail(error, "pending Audio ticket has no file payload");
    return true;
}
bool BankState::Encode(const Audio& audio, const State& state, std::vector<uint8_t>& bytes, std::string* error) {
    if (!Validate(audio, state, error)) return false;
    std::vector<uint8_t> files, samples, encoded;
    if (!StreamState::Encode(state.files, files, error) || !AudioState::Encode(state.audio, samples, error)) return false;
    StateBytes::Put(encoded, 1);
    StateBytes::Put(encoded, static_cast<uint32_t>(files.size()));
    encoded.insert(encoded.end(), files.begin(), files.end());
    StateBytes::Put(encoded, static_cast<uint32_t>(samples.size()));
    encoded.insert(encoded.end(), samples.begin(), samples.end());
    bytes = std::move(encoded);
    return true;
}
bool BankState::Decode(const Audio& audio, const std::vector<uint8_t>& bytes, State& state, std::string* error) {
    if (bytes.size() > 144 * 1024 * 1024 + 12) return Fail(error, "bank snapshot exceeds bound");
    StateBytes in(bytes);
    State snapshot;
    uint32_t version = 0, size = 0;
    std::vector<uint8_t> files, samples;
    if (!in.Get(version) || version != 1 || !in.Get(size) || !in.GetBytes(size, files) ||
        !in.Get(size) || !in.GetBytes(size, samples) || in.Remaining())
        return Fail(error, "invalid bank snapshot header");
    if (!StreamState::Decode(files, snapshot.files, error) || !AudioState::Decode(samples, snapshot.audio, error) ||
        !Validate(audio, snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool BankState::Restore(Audio& audio, const State& state, std::string* error) {
    if (!Validate(audio, state, error)) return false;
    FileSchedule::State current;
    auto* schedule = StreamLibrary::Scheduling();
    if (!schedule || !schedule->Capture(current, error) || current.tick != state.files.streams.schedule.tick)
        return Fail(error, "restore the game clock before bank requests");
    // Allocate all replacement payload copies before cancelling the old queue.
    auto files = state.files;
    std::vector<std::shared_ptr<FileSchedule::Payload>> renewed;
    for (auto& entry : files.streams.schedule.reads) {
        const auto& payload = *entry.second.payload;
        if (payload.kind != FileSchedule::Destination::Sound ||
            payload.generation != state.audio.load_generation) continue;
        auto copy = std::make_shared<FileSchedule::Payload>(payload);
        entry.second.payload = copy;
        renewed.push_back(std::move(copy));
    }
    schedule->Reset(); // cancels OLD Audio tickets, before new tickets exist
    uint64_t generation = 0;
    if (!audio.RestoreLoads(state.audio, generation)) return Fail(error, "Audio restore preflight changed");
    for (auto& payload : renewed) payload->generation = generation;
    return LoaderFileIo::RestoreState(files, error);
}
} // namespace title
