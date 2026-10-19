// OLD E2_SAVE and SOUND_P5: invalid incoming state rejects before teardown;
// closed playback may restore, active playback/loans do not disappear at EOF.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/core/verify/memory_checkpointer.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/fmv_sector_source_adapter.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "engine/sound/fmv_pcm_input.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"

namespace {
using Checkpoint = ps2r::app::MovieCheckpoint;
using Movie = title::MovieDecoder;
using Stream = title::MovieStream;
using ps2r::savestate::Blob;
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Store(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i) bytes.at(offset + i) = uint8_t(value >> (i * 8));
}

class Fixture {
public:
    static constexpr uint32_t kDecoder = 0x10000, kGp = 0x3114f0, kPlayer = 0x20000;
    MainMemory ram;
    Scratchpad spad;
    ps2r::ipu::GuestMemoryAdapter memory{[this](uint32_t a, size_t n) {
        return uint64_t(a) + n <= ram.Size() ? ram.Data() + a : nullptr;
    }};
    ps2r::ipu::FmvSectorSourceAdapter sectors{[](uint32_t, uint8_t*) { return 0; }};
    ps2r::sound::FmvPcmInput pcm{1024};
    title::MovieAudio audio{pcm};
    ps2r::ipu::IpuSettings settings;
    ps2r::ipu::FmvContextLayout layout = FmvContextLayoutData::CrashTwinsanityUS();
    ps2r::ipu::MpegApi mpeg{memory, layout, sectors, pcm,
        [] { return std::make_unique<ps2r::ipu::FfmpegVideoDecoder>(); }, settings};
    Stream stream{memory, [](uint64_t, size_t, uint8_t*) -> int64_t { return 0; }};
    Movie movie{memory, {
        [](CpuContext&) {}, [](CpuContext&) {}, [](CpuContext&) {}, [](CpuContext&) {},
        [this] { mpeg.Init(); }, [this](uint32_t address) { mpeg.Reset(address); },
        [](CpuContext&, uint32_t) {}, [](uint32_t) { return false; }, {}
    }};
    int64_t tick = 1000;
    Checkpoint checkpoint;
    MemorySections memories{ram, spad, nullptr, 0};
    StateRestorer restorer{memories};
    std::map<uint32_t, Movie::Body> installed;
    unsigned teardown = 0;
    bool approve_legacy = false;
    CpuContext context{};

    explicit Fixture(bool owned_audio = true)
        : checkpoint(movie, stream, mpeg, [this] { return tick; }, owned_audio ? &audio : nullptr) {
        Movie::Capture capture = [](uint32_t) -> Movie::Body {
            return [](CpuContext&, uint32_t) { Check(false, "checkpoint fixture never executes an old body"); };
        };
        Movie::Installer install = [this](uint32_t address, Movie::Body body) {
            installed[address] = std::move(body); return true;
        };
        Check(movie.Install(capture, install, true) == 7 && stream.Install(capture, install, true) == 3,
              "production Movie owners installed");
        context.cpuRegs.GPR.r[28].UD[0] = kGp;
        context.cpuRegs.GPR.r[17].UD[0] = kPlayer;
        memory.Write32(kPlayer + 0x41f8, 0x500000);
        memory.Write32(Movie::kDecoderPointer, kDecoder);
        movie.Initialise(context);
        mpeg.Reset(kDecoder);
        Check(checkpoint.Register(restorer.Registry(), [this](const auto&, std::string* error) {
            if (!approve_legacy && error) *error = "Fixture boot import was not explicitly approved";
            return approve_legacy;
        }), "Movie registers required-on-write state and its pure preflight");
        restorer.RequireSectionOwners();
        restorer.SetPreRestore([this](std::string* error) {
            if (!checkpoint.CanCheckpoint(error)) return false;
            ++teardown;
            return true;
        });
    }
    void Call(uint32_t entry, uint32_t caller, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0) {
        context.cpuRegs.GPR.r[4].UD[0] = a0; context.cpuRegs.GPR.r[5].UD[0] = a1;
        context.cpuRegs.GPR.r[6].UD[0] = a2; context.cpuRegs.GPR.r[31].UD[0] = caller;
        installed.at(entry)(context, entry);
    }
    void Open() {
        memory.Write32(kGp - Movie::kFileLocationGp, 17);
        Call(Stream::kPrepare, 0x2ae9c4, 0x50, 5, 0x500000);
        Call(Stream::kOpen, 0x2ae9ec, 17, kGp - 0x74b8);
    }
    void Close() { Call(Stream::kClose, 0x2af608); }
    std::vector<Blob> Capture() { return restorer.Registry().Capture(); }
};

void TestComposedRestore() {
    Fixture f;
    std::string error;
    f.Open();
    Check(!f.checkpoint.CanCheckpoint(&error), "prepared/open file refuses even before decoder work");
    Check(f.stream.Read(f.ram.Data(), 1) == 0 && !f.checkpoint.CanCheckpoint(&error),
          "file EOF does not make playback captureable");
    f.Close();
    Check(f.audio.SetGain(8191), "closed software gain records the source intent");
    Check(f.checkpoint.CanCheckpoint(&error), "source Close releases the file guard");
    Checkpoint::Snapshot state;
    Check(f.checkpoint.Capture(state, &error) && state.stream.owned && state.contexts.size() == 1 &&
          state.pcm_gain == 8191 && (state.decoder_services & 128),
          "capture includes closed file identity and software context");
    state.contexts[0].authored_fps = 29.97;
    state.contexts[0].clock = {true, -24, 2, 4, 7};
    Check(f.checkpoint.Restore(state, &error), "restore installs a carried closed clock");
    const auto blobs = f.Capture();
    const auto& saved = blobs.back();
    Check(saved.tag == Checkpoint::kTag && saved.flags == ps2r::savestate::kSectionRequired,
          "new captures require a Movie owner");
    Checkpoint::Snapshot parsed;
    Check(Checkpoint::Decode(saved, parsed, &error) &&
          parsed.contexts[0].clock.ticks_to_next == -24 && parsed.contexts[0].clock.reanchors == 7,
          "portable signed clock and policy survive encoding");
    std::vector<uint8_t> again;
    Check(Checkpoint::Encode(parsed, again, &error) && again == saved.payload,
          "Movie section is byte-exact after decoding and encoding");
    f.movie.ResetAfterRestore();
    f.stream.ResetAfterRestore();
    Check(f.audio.SetGain(32767), "another live gain differs from saved gain");
    const auto gain_changes = f.audio.Count().gain_changes;
    f.tick = 0;
    f.memory.Write32(0x9000, 0x12345678);
    const auto image = ps2r::savestate::SerializeStateFile({}, blobs);
    Check(f.restorer.RestoreImage(image.data(), image.size(), &error), "actual owner restore succeeds");
    Check(f.memory.Read32(0x9000) == 0 && f.teardown == 1 && f.checkpoint.Restored() == 2,
          "actual orchestrator restores memory before the Movie owner");
    Check(f.audio.Gain() == 8191 && f.audio.Count().gain_changes == gain_changes,
          "closed gain restores without counterfeiting an executed volume change");
    Check(f.stream.OwnsCursor() && !f.stream.Active() && f.stream.Read(f.ram.Data(), 1) == -1,
          "closed owned cursor cannot fall through to stale model input");
    Check(f.checkpoint.Capture(parsed, &error) && parsed.contexts[0].clock.ticks_to_next == -24,
          "closed deadline rebases at the new origin without arming a new clock");
    f.Open(); f.Close();
    Check(f.checkpoint.CanCheckpoint(), "restored file service reopens and closes normally");
}

void TestPreflight() {
    Fixture f;
    std::string error;
    auto blobs = f.Capture();
    const auto original = blobs.back();
    const uint64_t flushes = f.pcm.Flushes();
    const auto check_unchanged = [&] {
        Check(f.teardown == 0 && f.checkpoint.Restored() == 0 && f.pcm.Flushes() == flushes &&
              f.memory.Read32(Movie::kDecoderPointer) == Fixture::kDecoder,
              "invalid incoming state never reaches teardown, PCM flush or RAM mutation");
    };
    for (size_t length = 0; length < original.payload.size(); ++length) {
        Blob short_blob = original;
        short_blob.payload.resize(length);
        Checkpoint::Snapshot out;
        out.stopped_decoder = 0xdead;
        Check(!Checkpoint::Decode(short_blob, out, &error) && out.stopped_decoder == 0xdead,
              "every truncated Movie record rejects without publishing partial state");
    }
    for (unsigned corruption = 0; corruption != 15; ++corruption) {
        auto broken = original;
        switch (corruption) {
        case 0: broken.version++; break;
        case 1: broken.flags = 0; break;
        case 2: broken.payload.push_back(0); break;
        case 3: Store(broken.payload, 8, 0); break;  // provider mismatch
        case 4: Store(broken.payload, 20, 2); break; // noncanonical bool
        case 5: Store(broken.payload, 40, UINT32_MAX); break;
        case 6: Store(broken.payload, 44, 32768); break; // out-of-range PCM gain
        case 7: Store(broken.payload, 48, 0); break;
        case 8: Store(broken.payload, 52, Fixture::kDecoder + 4); break;
        case 9: Store(broken.payload, 64, UINT64_MAX, 8); break; // NaN
        case 10: Store(broken.payload, 72, 2); break;
        case 11: Store(broken.payload, 76, 1); break;
        case 12: Store(broken.payload, 96, UINT64_MAX, 8); break; // negative policy
        case 13: Store(broken.payload, 16, Fixture::kDecoder + 16); break;
        case 14: Store(broken.payload, 16, 0); break;
        }
        blobs.back() = broken;
        Check(!f.restorer.Registry().Validate(blobs, &error), "semantic corrupt Movie image fails pure preflight");
        if (corruption == 0 || corruption == 3 || corruption == 13) {
            // Recompute valid section/envelope CRCs: these defects must still
            // reject before StateRestorer's pre-restore callback (OLD E2_SAVE).
            const auto image = ps2r::savestate::SerializeStateFile({}, blobs);
            Check(!f.restorer.RestoreImage(image.data(), image.size(), &error),
                  "CRC-valid corrupt owner image rejects before reconstruction");
        }
        check_unchanged();
    }
    blobs.back() = original;
    Store(blobs.front().payload, Movie::kDecoderPointer, Fixture::kDecoder + 4);
    Check(!f.restorer.Registry().Validate(blobs, &error), "incoming RAM mismatch refuses despite valid live RAM");
    check_unchanged();
    Store(blobs.front().payload, Movie::kDecoderPointer, Fixture::kDecoder);
    const auto hash = f.restorer.RestoredMemoryHash();
    f.memory.Write32(Movie::kDecoderPointer, Fixture::kDecoder + 4);
    Check(f.restorer.Registry().Validate(blobs, &error), "preflight reads incoming RAM, not the previous live image");
    f.memory.Write32(Movie::kDecoderPointer, Fixture::kDecoder);
    Check(f.restorer.RestoredMemoryHash() == hash, "preflight leaves all live bytes untouched");
    blobs.pop_back();
    Check(!f.restorer.Registry().Validate(blobs, &error), "missing selected owner is not an implicit legacy reset");
    check_unchanged();
    f.approve_legacy = true;
    const auto legacy = ps2r::savestate::SerializeStateFile({}, blobs);
    Check(f.restorer.RestoreImage(legacy.data(), legacy.size(), &error) &&
          f.checkpoint.Migrated() == 1 && f.checkpoint.Restored() == 0,
          "explicit historical import is counted separately from exact owner restore");
    Checkpoint::Snapshot cleared;
    Check(f.checkpoint.Capture(cleared, &error) && cleared.contexts.empty() && !cleared.stream.owned,
          "documented historical import discards only the unrecorded closed host state");
}

void TestActiveRefusal() {
    Fixture f;
    const auto image = f.restorer.SaveImage({});
    f.Open();
    std::string error;
    Check(!f.restorer.RestoreImage(image.data(), image.size(), &error) && f.teardown == 0 && f.stream.Active(),
          "live file ownership refuses restore without silently closing it");
    bool refused = false;
    try { (void)f.restorer.SaveImage({}); } catch (const std::runtime_error&) { refused = true; }
    Check(refused && f.stream.Active(), "unguarded direct capture cannot drop active ownership either");
    f.Close();
    Check(f.restorer.RestoreImage(image.data(), image.size(), &error), "source Close permits later restore");
}

void TestFrozenMigrationPolicy() {
    using namespace ps2r::savestate;
    std::vector<Blob> blobs{{TAG_RAM, 1, kSectionRequired, {1, 2, 3}},
                            {TAG_SPAD, 1, kSectionRequired, {4, 5}},
                            {TAG_META, 1, kSectionRequired, {6}},
                            {TAG_SNDC, 1, 0, {7}}};
    // MemorySections owns exact extents; this classifier only names the
    // frozen schema and proves the already validated incoming memory identity.
    std::vector<ps2r::verify::MemorySpan> spans;
    for (size_t i = 0; i < 3; ++i) spans.push_back({blobs[i].payload.data(), blobs[i].payload.size()});
    const uint64_t expected = ps2r::verify::MemoryCheckpointer::Hash(spans);
    std::string error;
    Check(Checkpoint::ValidateLegacy(blobs, expected, &error), "approved frozen schema and memory match");
    Check(!Checkpoint::ValidateLegacy(blobs, 0, &error), "absence of a known signature never approves migration");
    blobs[0].payload[0]++;
    Check(!Checkpoint::ValidateLegacy(blobs, expected, &error), "same name with different memory rejects migration");
    blobs[0].payload[0]--;
    blobs.push_back({Tag('G', 'C', 'L', 'K'), 1, kSectionRequired, {0}});
    Check(!Checkpoint::ValidateLegacy(blobs, expected, &error),
          "a new logical-clock owner prevents missing Movie from being imported as historical");
    blobs.pop_back();
    blobs.push_back(blobs.back());
    Check(!Checkpoint::ValidateLegacy(blobs, expected, &error), "duplicate frozen owner rejects migration");
    blobs.pop_back();
    blobs.erase(blobs.begin() + 2);
    Check(!Checkpoint::ValidateLegacy(blobs, expected, &error), "missing frozen memory section rejects migration");
}

void TestPcmAndPriorVersion() {
    Fixture f;
    const auto image = f.restorer.SaveImage({});
    const int16_t input[] = {123, -456};
    f.pcm.Push(input, 1);
    std::string error;
    Check(!f.checkpoint.CanCheckpoint(&error), "unserved queued PCM refuses even with a closed decoder");
    f.pcm.GateOpen();
    int16_t output[2]{};
    Check(f.pcm.Pop(output, 1) == 1 && !f.checkpoint.CanCheckpoint(&error),
          "an empty open gate still requires source Close");
    const auto flushes = f.pcm.Flushes();
    Check(!f.restorer.RestoreImage(image.data(), image.size(), &error) && f.teardown == 0 &&
          f.pcm.Flushes() == flushes, "restore cannot flush an unclosed software PCM owner");
    f.pcm.Flush();
    Check(f.restorer.RestoreImage(image.data(), image.size(), &error), "closed PCM owner admits restore");

    Fixture retained(false);
    auto prior = retained.Capture().back();
    prior.version = 1;
    Store(prior.payload, 4, 1);
    Checkpoint::Snapshot state;
    Check(Checkpoint::Decode(prior, state, &error) && retained.checkpoint.Validate(state, &error) &&
          !state.pcm_gain && !(state.decoder_services & 128),
          "MOVI v1 remains readable when the composition has no owned PCM provider");
    auto prior_sections = retained.Capture();
    prior_sections.back() = prior;
    const auto prior_image = ps2r::savestate::SerializeStateFile({}, prior_sections);
    Check(retained.restorer.RestoreImage(prior_image.data(), prior_image.size(), &error) &&
          retained.checkpoint.Restored() == 1,
          "the actual registry and restorer accept the supported v1 composition");
    Check(!f.checkpoint.Validate(state, &error),
          "v1 cannot silently invent the selected software gain owner");
    Store(prior.payload, 44, 1);
    Check(!Checkpoint::Decode(prior, state, &error), "v1 reserved bytes cannot masquerade as v2 gain state");
}
}  // namespace

int main() {
    TestComposedRestore();
    TestPreflight();
    TestActiveRefusal();
    TestFrozenMigrationPolicy();
    TestPcmAndPriorVersion();
    std::printf("MovieCheckpoint: %u checks passed (closed owners, clocks, incoming RAM, atomic preflight, migration).\n",
                checks);
}
