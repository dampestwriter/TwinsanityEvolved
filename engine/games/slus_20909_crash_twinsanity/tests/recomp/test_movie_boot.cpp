// OLD E2_SAVE_REVIEW: CRC-valid semantic failures reject before teardown.
// SOUND_P5_IMPL: refuse live Movie ownership before load; EOF is not Close.
// Closed reconstruction preserves PCM ownership and executes no new flush.
// This is the production Movie/InputSave/SessionClock boot composition, without
// a game process, generated bodies, device output or a historical save input.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>

#include "games/slus_20909_crash_twinsanity/composition/movie_boot.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"
#include "app/host/session_clock.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/fmv_sector_source_adapter.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "engine/sound/fmv_pcm_input.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"

// Input/Save's importer inspects owned records; no generated stack executes.
extern "C" uint32_t ee_get_call_stack_depth() { return 0; }
extern "C" bool ee_get_call_stack_top(uint32_t, uint32_t*, uint32_t*) { return false; }
extern "C" bool ee_call_stack_push(uint32_t, uint32_t) { return false; }
extern "C" void ee_call_stack_pop() {}

namespace {
using namespace ps2r::savestate;
using ps2r::app::InputSave;
using ps2r::app::MovieBoot;
using ps2r::app::MovieCheckpoint;
using ps2r::app::SessionClock;
using Movie = title::MovieDecoder;
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Remove(std::vector<Blob>& blobs, uint32_t tag) {
    blobs.erase(std::remove_if(blobs.begin(), blobs.end(), [tag](const Blob& b) { return b.tag == tag; }),
                blobs.end());
}
const Blob& Find(const std::vector<Blob>& blobs, uint32_t tag) {
    const auto it = std::find_if(blobs.begin(), blobs.end(), [tag](const Blob& b) { return b.tag == tag; });
    Check(it != blobs.end(), "expected owner is present");
    return *it;
}

class Fixture {
public:
    static constexpr uint32_t kDecoder = 0x10000;
    MainMemory ram;
    Scratchpad spad;
    std::array<uint8_t, 16> arena{};
    EmuDomain domain;
    TickSource source;
    TickSync ticks;
    SessionClock clock{domain, source, ticks, "movie-boot:services-v1"};
    ps2r::ipu::GuestMemoryAdapter memory{[this](uint32_t a, size_t n) {
        return uint64_t(a) + n <= ram.Size() ? ram.Data() + a : nullptr;
    }};
    ps2r::ipu::FmvSectorSourceAdapter sectors{[](uint32_t, uint8_t*) { return 0; }};
    ps2r::sound::FmvPcmInput pcm{1024};
    title::MovieAudio audio{pcm};
    ps2r::ipu::IpuSettings settings;
    ps2r::ipu::MpegApi mpeg{memory, FmvContextLayoutData::CrashTwinsanityUS(), sectors, pcm,
        [] { return std::make_unique<ps2r::ipu::FfmpegVideoDecoder>(); }, settings};
    title::MovieStream stream{memory, [](uint64_t, size_t, uint8_t*) -> int64_t { return 0; }};
    Movie movie{memory, {
        [](CpuContext&) {}, [](CpuContext&) {}, [](CpuContext&) {}, [](CpuContext&) {},
        [this] { mpeg.Init(); }, [this](uint32_t a) { mpeg.Reset(a); },
        [](CpuContext&, uint32_t) {}, [](uint32_t) { return false; }, {}
    }};
    MovieCheckpoint movie_state;
    InputSave input;
    MemorySections memory_state{ram, spad, arena.data(), arena.size()};
    StateRestorer restorer{memory_state};
    MovieBoot boot{movie_state, input, clock};
    std::string path, error;
    uint64_t approved_hash = 0;
    unsigned teardown = 0;

    Fixture(const std::string& directory, uint32_t input_selection = 0, bool lifecycle = true)
        : movie_state(movie, stream, mpeg, [this] { return int64_t(domain.DomainTick()); },
                      lifecycle ? &audio : nullptr),
          input(input_selection, InputSave::RamAccess(ram, spad), {directory},
                [this] { return domain.BoundaryCount(); }, {}),
          path(directory + "/movie.ps2st") {
        const Movie::Capture capture = [](uint32_t) -> Movie::Body {
            return [](CpuContext&, uint32_t) { Check(false, "no generated continuation executes"); };
        };
        const Movie::Installer install = [](uint32_t, Movie::Body) { return true; };
        Check(movie.Install(capture, install, lifecycle) == (lifecycle ? 7 : 3), "Movie providers installed");
        if (lifecycle) Check(stream.Install(capture, install, true) == 3, "stream provider installed");
        memory.Write32(Movie::kDecoderPointer, kDecoder);
        CpuContext context{};
        if (lifecycle) movie.Initialise(context);
        mpeg.Reset(kDecoder);
        Check(restorer.Registry().Register(clock.Section()), "game clock registers before Movie deadlines");
        Check(input.Register(restorer.Registry()), "Input/Save selection owner registered");
        approved_hash = restorer.RestoredMemoryHash();
        // Use the same basename/inventory policy as root, over this fixture's
        // own NEW input. No old savestate is needed or substituted for output.
        const std::string inventory = directory + "/approved.txt";
        { std::ofstream file(inventory); file << "movie.ps2st " << std::hex << approved_hash << '\n'; }
        Check(movie_state.RegisterBoot(restorer.Registry(), inventory, path) &&
              movie_state.RegisterPreparation(restorer), "strict boot classifier and software preflight registered");
        restorer.RequireSectionOwners();
        restorer.SetPreRestore([this](std::string* why) {
            if (!movie_state.CanCheckpoint(why)) return false;
            ++teardown;
            domain.ReAnchor(source.RawCount());
            return true;
        });
    }
    void Advance(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            domain.NoteArrival(source.RawCount());
            domain.OnRawTick(source.Tick());
            domain.OnRawTick(source.Tick());
            domain.OnBoundary(source.RawCount());
        }
    }
    FileMeta Meta() const {
        FileMeta meta;
        meta.vsync_frame = uint32_t(domain.BoundaryCount());
        return meta;
    }
    std::vector<Blob> Capture() { return restorer.Registry().Capture(); }
    void Write(const std::vector<Blob>& blobs) {
        Check(WriteStateFile(path, Meta(), blobs, &error), "write fixture image with valid CRCs");
    }
};

void NativeClock(const std::string& directory) {
    Fixture f(directory);
    f.Advance(17);
    MovieCheckpoint::Snapshot movie;
    Check(f.movie_state.Capture(movie, &f.error) && movie.contexts.size() == 1, "closed software context captured");
    movie.contexts[0].authored_fps = 29.97;
    movie.contexts[0].clock = {true, -24, 2, 4, 7};
    Check(f.mpeg.RestoreIdle(movie.contexts, int64_t(f.domain.DomainTick()), &f.error),
          "seed actual closed software deadline before capture");
    const auto original = f.Capture();
    f.Write(original);
    f.Advance(200);
    f.movie.ResetAfterRestore();
    const auto before = f.Capture();
    const auto flushes = f.pcm.Flushes();
    Check(f.boot.PrepareFile(f.path, {}, f.error), "prepare new Movie/InputSave/Clock file");
    Check(SerializeStateFile({}, f.Capture()) == SerializeStateFile({}, before) &&
          f.pcm.Flushes() == flushes && f.teardown == 0, "preparation never mutates owners or tears down");
    Check(f.boot.Meta().vsync_frame == 17, "source frame available before restoring other providers");
    Check(f.boot.Restore(f.restorer, f.error), "composed owner restore succeeds");
    const auto restored = f.Capture();
    Check(Find(restored, SessionClock::kTag).payload == Find(original, SessionClock::kTag).payload &&
          Find(restored, MovieCheckpoint::kTag).payload == Find(original, MovieCheckpoint::kTag).payload,
          "clock restores before relative Movie deadline with no later initializer reset");
    Check(f.pcm.Flushes() == flushes && !f.pcm.Queued() && !f.pcm.GateIsOpen() &&
          f.movie_state.Migrated() == 0 && f.teardown == 1,
          "successful closed restore preserves closed PCM and is not a migration");
    Check(!f.boot.Restore(f.restorer, f.error) && f.teardown == 1, "prepared boot applies once");

    for (const auto tag : {MovieCheckpoint::kTag, SessionClock::kTag, InputSave::Checkpoint::kTag}) {
        auto broken = original;
        Remove(broken, tag);
        f.Write(broken);
        InputSave::Checkpoint::LegacyPolicy approved{true, InputSave::Checkpoint::Clock::OldSyncV, 0};
        Check(!f.boot.PrepareFile(f.path, approved, f.error), "partial new owner image never becomes legacy");
        Check(!f.boot.Restore(f.restorer, f.error) && f.teardown == 1,
              "failed preparation cannot reuse previously approved bytes");
        Check(SerializeStateFile({}, f.Capture()) == SerializeStateFile({}, restored),
              "CRC-valid missing owner leaves live Movie, input and clock intact");
    }
    auto mismatched = original;
    auto& clock_blob = *std::find_if(mismatched.begin(), mismatched.end(),
        [](const Blob& b) { return b.tag == SessionClock::kTag; });
    clock_blob.payload[8] ^= 1; // provider identity, still a structurally valid payload
    f.Write(mismatched);
    Check(!f.boot.PrepareFile(f.path, {}, f.error) && f.teardown == 1,
          "provider identity mismatch rejects before any live state change");
}

void LegacyImport(const std::string& directory, uint32_t input_selection, bool lifecycle) {
    Fixture f(directory, input_selection, lifecycle);
    auto original = f.Capture();
    Remove(original, SessionClock::kTag);
    Remove(original, MovieCheckpoint::kTag);
    Remove(original, InputSave::Checkpoint::kTag);
    Remove(original, title::InputStateCodec::kTag);
    if (input_selection) {
        original.push_back({TAG_PADS, 1, kSectionRequired, std::vector<uint8_t>(166)});
        original.push_back({TAG_CNTR, 1, 0, std::vector<uint8_t>(40)});
    }
    f.Write(original);
    InputSave::Checkpoint::LegacyPolicy approved{true, InputSave::Checkpoint::Clock::OldSyncV, 0};
    std::vector<Blob> input_only;
    Check(f.input.PrepareBoot(original, approved, input_only, f.error),
          "Input/Save alone adds its native selection marker");
    Check(!f.movie_state.ValidateImage(input_only, &f.error),
          "regression positive control: transformed image lost original Movie provenance");
    const auto flushes = f.pcm.Flushes();
    Check(f.boot.PrepareFile(f.path, approved, f.error), "composition classifies original before transforming it");
    Check(f.movie_state.Migrated() == 0 && f.pcm.Flushes() == flushes && f.teardown == 0,
          "prepared legacy input is not yet an applied migration");
    Check(f.boot.Restore(f.restorer, f.error), "closed Movie import coexists with Input/Save migration");
    Check(f.movie_state.Migrated() == 1 && f.movie_state.Restored() == 1 && f.teardown == 1 &&
          f.pcm.Flushes() == flushes && !f.pcm.Queued() && !f.pcm.GateIsOpen(),
          "only successful real MOVI apply counts import and preserves the already closed PCM owner");
    Check(f.restorer.RestoredMemoryHash() == f.approved_hash, "boot migration preserves original memory bytes");
    MovieCheckpoint::Snapshot state;
    Check(f.movie_state.Capture(state, &f.error) && state.contexts.empty() && !state.stream.owned &&
          state.stopped_decoder == (lifecycle ? Fixture::kDecoder : 0) && state.pcm_gain == 0,
          "approved import has explicit closed ownership and selected stopped identity");
    const auto& applied = f.restorer.LastSections();
    Check(Find(applied, MovieCheckpoint::kTag).flags == kSectionRequired &&
          Find(applied, InputSave::Checkpoint::kTag).flags == kSectionRequired,
          "converted input carries both required native owners");
    if (input_selection) {
        Check(Find(applied, title::InputStateCodec::kTag).flags == kSectionRequired,
              "selected Input is restored alongside Movie");
        Check(std::none_of(applied.begin(), applied.end(), [](const Blob& b) { return b.tag == TAG_PADS; }),
              "converted Input has no competing retired owner");
    }
    // Capturing the imported session produces a complete new image. It must
    // restore as new state and may never inherit an absence permission.
    f.Write(f.Capture());
    Check(f.boot.PrepareFile(f.path, {}, f.error) && f.boot.Restore(f.restorer, f.error) &&
          f.movie_state.Migrated() == 1, "recaptured imported session restores without legacy approval");
    auto corrupt = original;
    corrupt.front().payload[0] ^= 1;
    f.Write(corrupt);
    Check(!f.boot.PrepareFile(f.path, approved, f.error), "same filename with different memory rejects");
    Check(!f.boot.Restore(f.restorer, f.error) && f.movie_state.Migrated() == 1,
          "failed legacy preparation never reuses prior prepared state or adds a migration");
}
}  // namespace

int main() {
    char directory[] = "/tmp/movie_boot_XXXXXX";
    Check(mkdtemp(directory) != nullptr, "temporary input directory created");
    NativeClock(directory);
    LegacyImport(directory, 0, false);
    LegacyImport(directory, 0, true);
    LegacyImport(directory, InputSave::Checkpoint::kInput, true);
    std::filesystem::remove_all(directory);
    std::printf("MovieBoot: %u checks passed (original provenance, composed migration, clock order, atomic refusal).\n",
                checks);
}
