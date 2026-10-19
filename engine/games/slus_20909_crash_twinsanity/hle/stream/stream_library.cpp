#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include "engine/cdvd/hle/loader_model.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/scheduler.h"

namespace {
struct File {
    bool sample = false;
    bool registered = false;
    uint64_t generation = 0;
};
struct Read {
    int32_t handle = -1;
    bool active = true;
    bool ready = false;
    Scheduler::Ticket ticket;
};
bool g_enabled = false;
bool g_installed = false;
LoaderModel* g_model = nullptr;
Scheduler g_completions;
FileSchedule g_file_schedule;
FileSchedule::Apply g_sound_apply, g_sound_cancel;
FileSchedule::Apply g_music_apply, g_music_cancel;
std::map<int32_t, File> g_files;
uint64_t g_file_generation = 0;
std::map<uint32_t, std::shared_ptr<Read>> g_reads;
std::map<uint32_t, int32_t> g_audio_reads;
std::atomic<uint64_t> g_file_opens{0}, g_sample_opens{0}, g_read_count{0}, g_deliveries{0},
    g_polls{0}, g_waits{0}, g_audio_count{0}, g_registrations{0}, g_cancelled{0},
    g_stale{0}, g_errors{0}, g_fallbacks{0};
// Per entry (StartRead, IsReading, WaitRead) x reason, plus the bank entry.
std::array<std::atomic<uint64_t>, 3> g_declined{}, g_unowned{};
std::atomic<uint64_t> g_bank_start{0}, g_bypassed{0};

int32_t Handle(uint32_t fs) {
    return memory::read<int32_t>(fs + LoaderFileIo::kFsHandle);
}
bool Samples(const std::string& path) {
    const size_t end = path.find(';');
    const std::string name = path.substr(0, end);
    const size_t n = name.size();
    return n >= 3 && name[n - 3] == '.' && (name[n - 2] == 'm' || name[n - 2] == 'M') &&
           (name[n - 1] == 'b' || name[n - 1] == 'B');
}
void Cancel(uint32_t fs) {
    const auto it = g_reads.find(fs);
    if (it == g_reads.end()) return;
    if (!it->second->ready) ++g_cancelled;
    it->second->active = false;
    g_completions.Cancel(it->second->ticket);
    g_reads.erase(it);
}
}

bool StreamLibrary::Install(const Capture& capture, const Installer& install, bool enabled) {
    if (g_installed) {
        if (g_enabled == enabled) return true;
        if (!g_files.empty() || !g_completions.CanCheckpoint()) return false;
        g_enabled = enabled;
        return true;
    }
    // NTSC bodies independently checked against TR game/filestream.cpp:
    // StartRead, IsReading, WaitRead, StartSoundBankRead.
    constexpr std::array<uint32_t, 4> addresses{0x2ad9a8, 0x2adaf0, 0x2ada30, 0x2ada68};
    std::array<Body, 4> originals;
    for (size_t i = 0; i < addresses.size(); ++i) {
        originals[i] = capture(addresses[i]);
        if (!originals[i]) return false;
    }
    for (size_t i = 0; i < addresses.size(); ++i) {
        const auto address = addresses[i];
        const auto original = originals[i];
        if (!install(address, [address, original, i](CpuContext& ctx, uint32_t entry) {
            if (g_enabled && (entry == 0 || entry == address)) {
                if (i == 0 && LoaderFileIo::StartRead(ctx)) return;
                if (i == 1 && TryIsReading(ctx)) return;
                if (i == 2 && TryWait(ctx)) return;
                if (i == 3) PrepareAudioRead(ctx);
                // The census of the original's execution, by entry and reason.
                if (i == 3) ++g_bank_start;
                else if (OwnsRead(ctx.cpuRegs.GPR.r[4].UL[0])) ++g_declined[i];
                else ++g_unowned[i];
            } else {
                ++g_bypassed;
            }
            ++g_fallbacks;
            original(ctx, entry);
        })) {
            for (size_t j = 0; j < i; ++j) install(addresses[j], originals[j]);
            return false;
        }
    }
    g_installed = true;
    g_enabled = enabled;
    return true;
}

bool StreamLibrary::Enabled() { return g_enabled; }
void StreamLibrary::Bind(LoaderModel* model) { g_model = model; }
void StreamLibrary::BindFiles(FileSchedule::Now now, FileSchedule::Apply apply, FileSchedule::Park park) {
    FileSchedule::Apply dispatch;
    if (apply) dispatch = [apply = std::move(apply)](const FileSchedule::Payload& payload) {
        if (payload.kind == FileSchedule::Destination::Sound) {
            if (g_sound_apply) g_sound_apply(payload);
        } else if (payload.kind == FileSchedule::Destination::Music) {
            if (g_music_apply) g_music_apply(payload);
        } else apply(payload);
    };
    g_file_schedule.Bind(g_model ? g_model->Timing() : nullptr, g_model ? g_model->TimingBudget() : nullptr,
                         std::move(now), std::move(dispatch));
    g_file_schedule.SetCancel([](const FileSchedule::Payload& payload) {
        if (payload.kind == FileSchedule::Destination::Sound && g_sound_cancel) g_sound_cancel(payload);
        if (payload.kind == FileSchedule::Destination::Music && g_music_cancel) g_music_cancel(payload);
    });
    g_file_schedule.SetParkFrame(std::move(park));
}
bool StreamLibrary::BindSounds(FileSchedule::Apply apply, FileSchedule::Apply cancel) {
    if (!g_file_schedule.CanCheckpoint() || g_file_schedule.HasSoundReads() ||
        static_cast<bool>(apply) != static_cast<bool>(cancel)) return false;
    g_sound_apply = std::move(apply);
    g_sound_cancel = std::move(cancel);
    return true;
}
bool StreamLibrary::HasSoundReads() { return g_file_schedule.HasSoundReads(); }
bool StreamLibrary::BindMusic(FileSchedule::Apply apply, FileSchedule::Apply cancel) {
    if (!g_file_schedule.CanCheckpoint() || g_file_schedule.HasMusicReads() ||
        static_cast<bool>(apply) != static_cast<bool>(cancel)) return false;
    if (static_cast<bool>(apply) != static_cast<bool>(g_music_apply))
        for (const auto& file : g_files) if (file.second.sample) return false;
    g_music_apply = std::move(apply); g_music_cancel = std::move(cancel);
    return true;
}
void StreamLibrary::SetParkFrame(FileSchedule::Park park) { g_file_schedule.SetParkFrame(std::move(park)); }
void StreamLibrary::AdvanceBoundary(uint64_t tick) { if (g_enabled) g_file_schedule.AdvanceBoundary(tick); }
FileSchedule* StreamLibrary::Scheduling() { return g_enabled ? &g_file_schedule : nullptr; }
bool StreamLibrary::SubmitFile(FileSchedule::Payload payload, uint32_t lba, uint32_t sectors, uint64_t tick) {
    const auto object = payload.object;
    const bool staged_sound = payload.kind == FileSchedule::Destination::Sound;
    if (payload.kind == FileSchedule::Destination::Sound && (!g_sound_apply || !g_sound_cancel)) return false;
    if (payload.kind == FileSchedule::Destination::Music && (!g_music_apply || !g_music_cancel)) return false;
    if (!FileOwned(payload.handle) ||
        !g_file_schedule.Submit(std::move(payload), lba, sectors, tick, staged_sound)) return false;
    Cancel(object);
    g_audio_reads.erase(object);
    ++g_read_count;
    return true;
}
void StreamLibrary::Reset() {
    for (const auto& entry : g_reads) entry.second->active = false;
    g_completions.Reset();
    g_reads.clear();
    g_audio_reads.clear();
    g_files.clear();
    g_file_schedule.Reset();
    g_model = nullptr;
}
bool StreamLibrary::CanCheckpoint() {
    return g_completions.CanCheckpoint() && (!g_enabled || g_file_schedule.CanCheckpoint());
}
bool StreamLibrary::CaptureState(State& out) {
    if (!CanCheckpoint()) return false;
    State state;
    state.enabled = g_enabled;
    for (const auto& file : g_files)
        state.files.emplace(file.first, FileState{file.second.sample, file.second.registered});
    for (const auto& read : g_reads) {
        if (read.second->active && !read.second->ready) return false;
        if (read.second->active) state.completed_reads.emplace(read.first, read.second->handle);
    }
    state.audio_reads = g_audio_reads;
    state.has_schedule = g_enabled;
    if (state.has_schedule && !g_file_schedule.Capture(state.schedule)) return false;
    out = std::move(state);
    return true;
}
bool StreamLibrary::ValidateState(const State& state) {
    if (state.enabled != g_enabled || state.files.size() > 65536 ||
        state.completed_reads.size() > 65536 || state.audio_reads.size() > 65536) return false;
    if (!state.enabled && (!state.files.empty() || !state.completed_reads.empty() ||
                           !state.audio_reads.empty())) return false;
    if (state.has_schedule && (!state.enabled || !g_file_schedule.Validate(state.schedule))) return false;
    if (state.has_schedule && (!g_sound_apply || !g_sound_cancel))
        for (const auto& read : state.schedule.reads)
            if (read.second.payload->kind == FileSchedule::Destination::Sound) return false;
    if (state.has_schedule && (!g_music_apply || !g_music_cancel))
        for (const auto& read : state.schedule.reads)
            if (read.second.payload->kind == FileSchedule::Destination::Music) return false;
    if (state.has_schedule)
        for (const auto& read : state.schedule.reads) {
            const auto& request = *read.second.payload;
            const auto file = state.files.find(request.handle);
            if (file == state.files.end() || (file->second.sample && !g_music_apply) ||
                (request.kind == FileSchedule::Destination::Music && !file->second.sample)) return false;
        }
    for (const auto& file : state.files)
        if (file.first < 0 || (file.second.sample && !file.second.registered && !g_music_apply)) return false;
    for (const auto& read : state.completed_reads) {
        const auto file = state.files.find(read.second);
        if (read.first < 0x10000 || read.first > 0x02000000 - 0x48 ||
            file == state.files.end() || (file->second.sample && !g_music_apply) ||
            state.audio_reads.count(read.first)) return false;
    }
    for (const auto& read : state.audio_reads)
        if (read.first < 0x10000 || read.first > 0x02000000 - 0x48 ||
            !state.files.count(read.second)) return false;
    return true;
}
bool StreamLibrary::RestoreState(const State& state) {
    if (!ValidateState(state) || state.files.size() > UINT64_MAX - g_file_generation) return false;
    std::map<int32_t, File> files;
    for (const auto& file : state.files)
        files.emplace(file.first, File{file.second.sample, file.second.registered, ++g_file_generation});
    if (state.has_schedule && !g_file_schedule.Restore(state.schedule)) return false;
    if (!state.has_schedule) g_file_schedule.Reset();
    for (const auto& read : g_reads) read.second->active = false;
    g_completions.Reset();
    g_reads.clear(); g_audio_reads.clear(); g_files = std::move(files);
    for (const auto& entry : state.completed_reads) {
        auto read = std::make_shared<Read>();
        read->handle = entry.second;
        read->ready = true;
        g_reads.emplace(entry.first, std::move(read));
    }
    g_audio_reads = state.audio_reads;
    return true;
}

bool StreamLibrary::Opened(int32_t handle, const std::string& path) {
    if (!g_enabled) return true;
    if (g_file_generation == UINT64_MAX) throw std::overflow_error("file generations exhausted");
    if (g_files.count(handle)) Closing(handle);
    const bool sample = Samples(path);
    const bool registered = sample && !g_music_apply;
    g_files[handle] = File{sample, registered, ++g_file_generation};
    if (sample) ++g_sample_opens; else ++g_file_opens;
    return registered;
}
bool StreamLibrary::IsSampleFile(const std::string& path) { return Samples(path); }
bool StreamLibrary::FileOwned(int32_t handle) {
    const auto it = g_files.find(handle);
    return g_enabled && it != g_files.end() && (!it->second.sample || g_music_apply);
}
uint64_t StreamLibrary::FileGeneration(int32_t handle) {
    const auto it = g_files.find(handle);
    return g_enabled && it != g_files.end() ? it->second.generation : 0;
}
bool StreamLibrary::MusicFileOwned(int32_t handle) {
    const auto it = g_files.find(handle);
    return g_enabled && g_music_apply && it != g_files.end() && it->second.sample;
}
bool StreamLibrary::FileRegistered(int32_t handle) {
    const auto it = g_files.find(handle);
    return !g_enabled || it == g_files.end() || it->second.registered;
}
void StreamLibrary::Registered(int32_t handle) {
    const auto it = g_files.find(handle);
    if (it == g_files.end() || it->second.registered) return;
    it->second.registered = true;
    ++g_registrations;
}
void StreamLibrary::Closing(int32_t handle) {
    if (!g_enabled) return;
    // Invalidate all old table entries before running any owner callback.
    // A callback's newly opened handle/read belongs to a different lifetime.
    g_files.erase(handle);
    std::vector<uint32_t> legacy_objects;
    for (auto it = g_reads.begin(); it != g_reads.end();) {
        const auto fs = it->first;
        const auto read = it->second;
        ++it;
        if (read->handle != handle) continue;
        legacy_objects.push_back(fs);
        Cancel(fs);
    }
    for (auto it = g_audio_reads.begin(); it != g_audio_reads.end();) {
        if (it->second == handle) it = g_audio_reads.erase(it); else ++it;
    }
    g_file_schedule.CancelHandle(handle);
    // Only the comparison-era Track adapter owns model closures. Never
    // complete a new callback-issued read which reused one of these keys.
    for (const auto fs : legacy_objects)
        if (g_model && !FileGeneration(handle) && !g_reads.count(fs)) g_model->CompleteNow(fs);
    g_completions.Drain(0);
}

StreamLibrary::Apply StreamLibrary::Track(uint32_t fs, int32_t handle, Apply apply) {
    if (!FileOwned(handle)) return apply;
    Cancel(fs);
    // A reset can leave an invalidated delivery in the unchanged model. Settle
    // it before reusing this object key; its lifetime guard suppresses the copy.
    if (g_model && g_model->InFlight(fs)) g_model->CompleteNow(fs);
    auto read = std::make_shared<Read>();
    read->handle = handle;
    g_reads[fs] = read;
    g_audio_reads.erase(fs);
    ++g_read_count;
    read->ticket = g_completions.Issue(0, [read, apply = std::move(apply)](Scheduler::Result) {
        if (!read->active || read->ready) {
            ++g_stale;
            return;
        }
        apply();
        read->ready = true; // completion is visible only after every byte
        ++g_deliveries;
    });
    return [read] {
        if (!g_completions.Publish(read->ticket, {})) {
            ++g_stale;
            return;
        }
        g_completions.Drain(0);
    };
}

bool StreamLibrary::OwnsRead(uint32_t fs) {
    if (fs < 0x10000 || fs > 0x02000000 - 0x48) return false;
    const auto handle = Handle(fs);
    if (!FileOwned(handle)) return false;
    const auto audio = g_audio_reads.find(fs);
    return audio == g_audio_reads.end() || audio->second != handle;
}
bool StreamLibrary::TryIsReading(CpuContext& ctx) {
    const uint32_t fs = ctx.cpuRegs.GPR.r[4].UL[0];
    if (!OwnsRead(fs)) return false;
    const auto handle = Handle(fs);
    const auto generation = FileGeneration(handle);
    ++g_polls;
    g_file_schedule.Poll(fs);
    if (g_reads.count(fs) && g_model) g_model->Poll(fs); // legacy Track adapter only
    g_completions.Drain(0);
    if (Handle(fs) != handle || FileGeneration(handle) != generation) {
        ++g_stale;
        ctx.cpuRegs.GPR.r[2].UD[0] = 1;
        return true; // an old poll cannot complete a replacement owner's read
    }
    const auto it = g_reads.find(fs);
    const bool pending = g_file_schedule.InFlight(fs) ||
                         (it != g_reads.end() && it->second->active && !it->second->ready);
    ctx.cpuRegs.GPR.r[2].UD[0] = pending ? 1 : 0;
    return true;
}
bool StreamLibrary::TryWait(CpuContext& ctx) {
    const uint32_t fs = ctx.cpuRegs.GPR.r[4].UL[0];
    if (!OwnsRead(fs)) return false;
    const auto handle = Handle(fs);
    const auto generation = FileGeneration(handle);
    ++g_waits;
    g_file_schedule.Wait(fs);
    if (Handle(fs) != handle || FileGeneration(handle) != generation) {
        ++g_stale;
        ctx.cpuRegs.GPR.r[2].UD[0] = 1;
        return true;
    }
    if (g_model && g_reads.count(fs)) {
        // A wait on a later request observes every earlier file completion.
        // In poll mode LoaderModel already preserves this rule; the explicit
        // order also closes its boundary-mode pull-forward exception.
        const auto target = g_reads.find(fs);
        if (!g_model->PollVisibility() && target != g_reads.end()) {
            std::map<uint64_t, uint32_t> preceding;
            for (const auto& entry : g_reads)
                if (entry.second->active && !entry.second->ready &&
                    entry.second->ticket.sequence <= target->second->ticket.sequence)
                    preceding.emplace(entry.second->ticket.sequence, entry.first);
            for (const auto& entry : preceding) g_model->CompleteNow(entry.second);
        }
        g_model->CompleteNow(fs);
    }
    g_completions.Drain(0);
    if (Handle(fs) != handle || FileGeneration(handle) != generation) {
        ++g_stale;
        ctx.cpuRegs.GPR.r[2].UD[0] = 1;
        return true;
    }
    return TryIsReading(ctx);
}
void StreamLibrary::PrepareAudioRead(CpuContext& ctx) {
    const uint32_t fs = ctx.cpuRegs.GPR.r[4].UL[0];
    const auto saved = ctx.cpuRegs;
    // FileStreamReadSoundBank raises this for the NEW bank before entering
    // StartSoundBankRead. Settling an earlier file request must not clear that
    // caller-owned intent: the captured start body does not raise it again.
    const auto pending = memory::read<uint32_t>(fs + LoaderFileIo::kFsFlags) &
                         LoaderFileIo::kFlagReadPending;
    if (OwnsRead(fs)) LoaderFileIo::WaitReadComplete(ctx);
    ctx.cpuRegs = saved;
    LoaderFileIo::RegisterForAudio(ctx);
    ctx.cpuRegs = saved;
    Cancel(fs);
    g_audio_reads[fs] = Handle(fs);
    memory::write<uint32_t>(fs + LoaderFileIo::kFsFlags,
        memory::read<uint32_t>(fs + LoaderFileIo::kFsFlags) | pending);
    ++g_audio_count;
}
void StreamLibrary::ReadError() { ++g_errors; }
StreamLibrary::Fallbacks StreamLibrary::CountFallbacks() {
    return {g_declined[0].load(), g_declined[1].load(), g_declined[2].load(),
            g_unowned[0].load(), g_unowned[1].load(), g_unowned[2].load(),
            g_bank_start.load(), g_bypassed.load()};
}
StreamLibrary::Counters StreamLibrary::Count() {
    return {g_file_opens.load(), g_sample_opens.load(), g_read_count.load(),
            g_deliveries.load() + g_file_schedule.Count().delivered,
            g_polls.load(), g_waits.load(), g_audio_count.load(), g_registrations.load(),
            g_cancelled.load() + g_file_schedule.Count().cancelled, g_stale.load(), g_errors.load(), g_fallbacks.load()};
}
