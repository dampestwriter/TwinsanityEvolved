#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/file_schedule.h"

class LoaderModel;

// File-service ownership at the game's FileStream boundary. Audio still owns
// sample files and sound-bank transfers through the existing STREAM adapter.
class StreamLibrary {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    using Apply = std::function<void()>;

    // Startup selection for the same-binary self-oracle. Changing arms with
    // open files is rejected; Reset must invalidate the old service first.
    static bool Install(const Capture& capture, const Installer& install, bool enabled = true);
    static bool Enabled();
    static void Bind(LoaderModel* model);
    static void BindFiles(FileSchedule::Now now, FileSchedule::Apply apply, FileSchedule::Park park = {});
    static bool BindSounds(FileSchedule::Apply apply, FileSchedule::Apply cancel);
    // Startup-only music selection. Existing sample-file handles cannot
    // change owners in place; reset/restore composition owns that migration.
    static bool BindMusic(FileSchedule::Apply apply, FileSchedule::Apply cancel);
    static bool HasSoundReads();
    static void SetParkFrame(FileSchedule::Park park);
    static void AdvanceBoundary(uint64_t tick);
    static FileSchedule* Scheduling();
    static bool SubmitFile(FileSchedule::Payload payload, uint32_t lba, uint32_t sectors, uint64_t tick);
    // Invalidates deliveries without dereferencing the previous dependency.
    // The composition owner calls this before replacing the loader/model.
    static void Reset();
    // Typed pending files are capturable, but never during a delivery callback.
    // The legacy Track adapter must settle; CaptureState is the full preflight.
    static bool CanCheckpoint();
    struct FileState { bool sample = false, registered = false; };
    struct State {
        bool enabled = false;
        std::map<int32_t, FileState> files;
        std::map<uint32_t, int32_t> completed_reads, audio_reads;
        bool has_schedule = false;
        FileSchedule::State schedule;
    };
    static bool CaptureState(State& out);
    static bool ValidateState(const State& state);
    // Caller validates the associated LoaderFileIo table before either owner
    // mutates. The current timing dependency stays bound; old tickets die.
    static bool RestoreState(const State& state);

    // Returns whether this open still needs the module's file registration.
    static bool Opened(int32_t handle, const std::string& path);
    static bool IsSampleFile(const std::string& path);
    static bool FileOwned(int32_t handle);
    // Runtime incarnation, renewed on open and restore. Never a media id or
    // serialized pointer; a waiter must recheck it after running callbacks.
    static uint64_t FileGeneration(int32_t handle);
    static bool MusicFileOwned(int32_t handle);
    static bool FileRegistered(int32_t handle);
    static void Registered(int32_t handle);
    static void Closing(int32_t handle);

    // Wraps a delivery with a lifetime check. Only the guest execution owner
    // invokes it, through LoaderModel's existing ordered delivery points.
    static Apply Track(uint32_t fs, int32_t handle, Apply apply);
    static bool OwnsRead(uint32_t fs);
    static bool TryIsReading(CpuContext& ctx);
    static bool TryWait(CpuContext& ctx);
    static void PrepareAudioRead(CpuContext& ctx);
    static void ReadError();

    struct Counters {
        uint64_t file_opens = 0, sample_opens = 0, reads = 0, deliveries = 0,
                 polls = 0, waits = 0, audio_reads = 0, audio_registrations = 0,
                 cancelled = 0, stale_deliveries = 0, errors = 0, fallbacks = 0;
    };
    static Counters Count();
    // The replaced model's execution, per bound entry and per reason, so the
    // file-jobs selection is decidable: `declined` = an ordinary request the
    // owner holds yet handed to the captured original (must be zero under
    // hle.stream_files); `unowned` = a request on a file the owner does not
    // hold (a sound bank under hle.sound_samples=off, or an fs registered for
    // audio) -- the retained arm's work by design; `bank_start` = the
    // StartSoundBankRead entry, which always runs the original after
    // PrepareAudioRead until hle.sound_samples owns the bank path.
    struct Fallbacks {
        uint64_t start_read_declined = 0, is_reading_declined = 0, wait_declined = 0;
        uint64_t start_read_unowned = 0, is_reading_unowned = 0, wait_unowned = 0;
        uint64_t bank_start = 0, bypassed = 0;   // bypassed: selection off, or an interior entry
    };
    static Fallbacks CountFallbacks();
};
