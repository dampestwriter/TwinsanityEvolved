#pragma once

#include <cstdint>
#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/buffer_request.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_request.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace title {

// Full-entry adapters for the title's MultiStream sound service. Gameplay
// still selects assets/channels/parameters. Owned requests replace the
// sample command producer and module consumer; they are never wire packets.
class SoundControls {
public:
    using Capture = StreamLibrary::Capture;
    using Installer = StreamLibrary::Installer;
    // Temporary provider for immediate group-volume changes on channels
    // still owned by music. Called on the game owner at accepted batch send.
    // It is deliberately counted separately from retired sample commands.
    using RemainingVolume = std::function<void(uint32_t, uint16_t, uint16_t)>;
    static bool Install(const Capture& capture, const Installer& install,
                        Audio* selected, RemainingVolume remaining_volume = {});
    static bool Enabled();
    static bool Owns(const Audio& audio);
    enum class Kind : uint8_t { Play, Volume, Pitch, Release, KeyOff, Group, ChannelGroup,
                               Pause, Resume, Stop, HandOff, RemainingVolume, Music, Buffer };
    struct Command {
        Kind kind = Kind::Play;
        uint32_t channel = 0, value = 0, words = 0;
        uint16_t left = 0, right = 0, raw_left = 0, raw_right = 0;
        bool smooth = false;
        Audio::Voice voice;
        MusicRequest music;
        BufferRequest buffer;
        // Index into the plays head while this play is pending (0 = unrecorded).
        size_t record = 0;
    };
    struct State {
        std::vector<Command> pending;
        std::array<bool, Audio::kVoices> owned{};
        std::array<uint32_t, Audio::kVoices> requested{};
        uint32_t words = 0;
        bool clocked = false, music = false, buffers = false;
        uint64_t tick = 0;
    };
    // Owner-local component state: no send, callback or status publication at
    // capture/restore. Audio, files, music and game RAM must be composed by
    // the session owner before execution resumes.
    static bool CaptureState(State& state, std::string* error = nullptr);
    static bool ValidateState(const State& state, std::string* error = nullptr);
    static bool RestoreState(const State& state, std::string* error = nullptr);
    static bool ValidateData(const State& state, std::string* error = nullptr);
    // Teardown/reset only. Discards pending commands/leases before Audio is
    // reset, and clears the clock anchor. Never call during command delivery.
    static bool Reset();
    static bool CanCheckpoint();
    // The music service shares acceptance/ordering with sample controls.
    // Bindings are composition dependencies; queued work remains typed data.
    using MusicApply = std::function<void(const MusicRequest&)>;
    static bool BindMusic(MusicApply apply, std::function<void()> finish_batch);
    static bool StageMusic(CpuContext& ctx, MusicRequest request, uint32_t words);
    using BufferApply = std::function<void(const BufferRequest&)>;
    static bool BindBuffers(BufferApply apply);
    static bool StageBuffer(CpuContext& ctx, BufferRequest request, uint32_t words);
    // Service ownership barrier (Movie loans): accept the finite pending
    // prefix using the existing send contract; preserve the caller's ABI.
    static bool Flush(CpuContext& ctx);
    // Exactly once per logical boundary, before mixing. An idempotent pulse,
    // not a host clock, not an IOP pump. A restore installs the saved State
    // after its saved clock; Reset is reserved for teardown/new sessions.
    static bool AdvanceBoundary(uint64_t tick);
    struct Counters {
        uint64_t staged = 0, applied = 0, plays = 0, volume = 0, pitch = 0,
                 release = 0, key_off = 0, batches = 0, busy = 0, replies = 0,
                 handoffs = 0, shared = 0, remaining_volumes = 0,
                 fallback = 0, errors = 0, boundaries = 0;
    };
    static Counters Count();
    // The first accepted sample plays, as the title requested them and as the
    // owned voice started: a census row, so a silent play is attributable from
    // run.meta without a trace (which argument the owned voice took).
    struct PlayRecord {
        uint64_t boundary = 0;
        uint32_t sound = 0, channel = 0, group = 0, loops = 0, bytes = 0;
        uint16_t raw_left = 0, raw_right = 0, left = 0, right = 0, pitch = 0;
        uint8_t attack = 0, release = 0;
        bool started = false;
    };
    static constexpr size_t kPlayHead = 8;
    static std::vector<PlayRecord> PlayHead();
};
} // namespace title
