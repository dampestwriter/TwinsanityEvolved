#pragma once

#include <cstdint>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_request.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace title {

// Music service ABI, composed after SoundControls and before opening .mb
// files. The game still selects tracks, streams, voices and fades.
class MusicLibrary {
public:
    using Capture = StreamLibrary::Capture;
    using Installer = StreamLibrary::Installer;
    enum class Phase : uint8_t { Stopped, Reading, Prepared, Playing, Failed };
    struct Window { uint32_t offset = 0, size = 0; };
    struct Stream {
        MusicRequest play;
        Window window;
        MusicAsset::Data data;
        uint64_t generation = 0, sequence = 0, start_tick = 0;
        uint32_t parent = UINT32_MAX, child = UINT32_MAX, block = 0, end = 0, track = 0;
        Phase phase = Phase::Stopped;
        bool owned = false, issued = false, disabled = false;
        // The stream's IOP buffer (the title's AttachBuffer size) and the
        // window bytes whose drive time has been charged: the fill before
        // readiness, then half-buffer refills as the voice consumes
        // (TR multistream.h: the stream keys on once its buffers are full).
        // buffer 0 = the whole window was charged at acceptance.
        uint32_t buffer = 0;
        uint64_t refilled = 0;
    };
    struct State {
        std::array<Stream, Audio::kVoices> streams;
        std::array<bool, Audio::kVoices> selected{};
        std::array<uint32_t, Audio::kVoices> requested{};
        std::array<uint8_t, Audio::kVoices> projected{};
        std::map<int32_t, Window> windows;
        uint64_t generation = 0, sequence = 0, tick = 0;
    };
    using Generations = std::array<uint64_t, Audio::kVoices>;
    static bool Install(const Capture& capture, const Installer& install, Audio* selected);
    static bool Enabled();
    static bool Owns(const Audio& audio);
    static bool CaptureState(State& state, std::string* error = nullptr);
    static bool ValidateData(const State& state, std::string* error = nullptr);
    static bool ValidateState(const State& state, std::string* error = nullptr);
    // Call after cancelling the discarded world's reads and control batch.
    // Renewed generations must be joined to saved Audio voices/file payloads
    // before execution resumes. No status, callback or new read is produced.
    static bool RestoreState(const State& state, Generations& renewed, std::string* error = nullptr);
    // Discard SoundControls' pending batch first. Invalidate reads before
    // destroying the Audio owner. Reset never invokes a guest continuation.
    static void Reset();
    // Accepted stream-buffer close invalidates its pending read and paired
    // voices before freeing the storage. It does not close the music file.
    static void CloseBuffer(uint32_t stream);
    static bool CanCheckpoint();
    // After StreamLibrary's boundary delivery, before Audio mixing. Status
    // inspection also publishes completed voices, without advancing time.
    static void AdvanceBoundary(uint64_t tick);
    struct Status {
        Phase phase = Phase::Stopped;
        uint32_t parent = UINT32_MAX, channel = UINT32_MAX;
        int32_t file = -1;
        uint64_t generation = 0, bytes = 0;
    };
    static Status Inspect(uint32_t stream);
    struct Counters {
        uint64_t windows = 0, plays = 0, children = 0, interleaves = 0, ends = 0,
                 prepares = 0, starts = 0, stops = 0, reads = 0, delivered = 0,
                 bytes = 0, stale = 0, errors = 0, fallback = 0, statuses = 0;
        // Drive time charged after readiness: half-buffer refills and their
        // bytes; `fills` counts reads charged at buffer-fill granularity.
        uint64_t fills = 0, refills = 0, refill_bytes = 0;
        // The boundary ticks of the first accepted Play and the first Start
        // (0 = none yet): the title's PlayStream and when its pair keyed on.
        uint64_t first_play_tick = 0, first_start_tick = 0;
        // The Play/Child refusal clauses (each also counted in `errors`):
        // the file has no owned window, the buffers owner refuses CanPlay
        // for the pair's buffer (the parent's slot, also for a child: not
        // allocated through the owned path, storage not owned or on loan),
        // or the Audio owner refuses the music voice.
        uint64_t refused_window = 0, refused_buffers = 0, refused_voice = 0;
    };
    static Counters Count();
};
} // namespace title
