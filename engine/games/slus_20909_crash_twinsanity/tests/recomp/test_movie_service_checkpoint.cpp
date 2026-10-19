// Composed OLD SOUND_P5/P8 pins: closed Movie pacing and audible owned Audio
// resume together; either live Movie loan forbids capture/restore. E2_SAVE:
// valid-CRC mismatches reject before any owner tears down or flushes PCM.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/composition/service_clock.h"
#include "app/host/session_clock.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/core/config/config.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/fmv_sector_source_adapter.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "engine/sound/fmv_pcm_input.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_checkpoint_graphics.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_checkpoint_factory.h"

namespace {
using namespace ps2r::savestate;
using namespace ps2r::app;
using Movie = title::MovieDecoder;
using Buffers = title::StreamBuffers;
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Remove(std::vector<Blob>& blobs, uint32_t tag) {
    blobs.erase(std::remove_if(blobs.begin(), blobs.end(), [tag](const auto& b) { return b.tag == tag; }),
                blobs.end());
}
Blob& Find(std::vector<Blob>& blobs, uint32_t tag) {
    auto it = std::find_if(blobs.begin(), blobs.end(), [tag](const auto& b) { return b.tag == tag; });
    Check(it != blobs.end(), "required owner found");
    return *it;
}

class Fixture {
public:
    static constexpr uint32_t kDecoder = 0x10000;
    MainMemory ram;
    Scratchpad spad;
    AddressMap map{ram, spad};
    MmioSink sink;
    MmioRouter router{sink};
    FastMem fast{ram, map, router};
    EmuDomain domain;
    TickSource source;
    TickSync ticks;
    SessionClock clock{domain, source, ticks, "Movie:files:Audio:music:buffers"};
    ps2r::CdvdConfig config;
    DeliveryMeter meter{config};
    DeliveryBudget budget{4096};
    LoaderModel retained{meter, budget};
    title::Audio audio;
    Buffers buffers{2u << 20};
    ps2r::ipu::GuestMemoryAdapter memory{[this](uint32_t a, size_t n) { return map.Translate(a, n); }};
    ps2r::ipu::FmvSectorSourceAdapter sectors{[](uint32_t, uint8_t*) { return 0; }};
    ps2r::sound::FmvPcmInput pcm{1024};
    title::MovieAudio movie_audio{pcm};
    ps2r::ipu::IpuSettings settings;
    bool fail_context = false;
    unsigned factory_copies = 0, factory_live = 0;
    ps2r::ipu::MpegApi mpeg{memory, FmvContextLayoutData::CrashTwinsanityUS(), sectors, pcm,
        MovieCheckpointFactory{fail_context, factory_copies, factory_live}, settings};
    title::MovieStream stream{memory, [](uint64_t, size_t, uint8_t*) -> int64_t { return 0; }};
    Movie movie{memory, {
        [](CpuContext&) {}, [](CpuContext&) {}, [](CpuContext&) {}, [](CpuContext&) {},
        [this] { mpeg.Init(); }, [this](uint32_t a) { mpeg.Reset(a); },
        [](CpuContext&, uint32_t) {}, [](uint32_t) { return false; }, {}
    }};
    MovieCheckpoint movie_state{movie, stream, mpeg, [this] { return int64_t(domain.DomainTick()); }, &movie_audio};
    SessionServices services{true, &audio};
    ServiceClock join{clock, services};
    MemorySections memories{ram, spad, nullptr, 0};
    StateRestorer restorer{memories};
    MovieCheckpointGraphics graphics;
    std::map<uint32_t, Movie::Body> entries;
    Movie::Capture capture = [this](uint32_t address) -> Movie::Body {
        if (entries.count(address)) return entries.at(address);
        // Explicit accepted Send boundary; this is not a model-output oracle.
        if (address == 0x1e2958) return [](CpuContext& c, uint32_t) { c.cpuRegs.GPR.r[2].SD[0] = 0; };
        return [](CpuContext&, uint32_t) { Check(false, "no generated service body may execute"); };
    };
    Movie::Installer install = [this](uint32_t address, Movie::Body body) {
        entries[address] = std::move(body); return true;
    };
    std::string error;
    unsigned teardown = 0;

    Fixture() {
        RecompAbi::BindMemory(&fast);
        Check(StreamLibrary::Install(capture, install, true), "selected file provider installed");
        LoaderFileIo::Deps deps;
        deps.model = &retained;
        deps.now_tick = [this] { return domain.DomainTick(); };
        LoaderFileIo::Bind(deps, {});
        Check(title::SoundBanks::Install(capture, install, &audio) &&
              title::SoundControls::Install(capture, install, &audio, [](uint32_t, uint16_t, uint16_t) {}) &&
              title::MusicLibrary::Install(capture, install, &audio) &&
              title::MusicBuffers::Install(capture, install, &buffers), "same file/Audio/music/pool composition as root");
        Check(movie.Install(capture, install, true) == 7 && stream.Install(capture, install, true) == 3,
              "Movie owners installed");
        memory.Write32(Movie::kDecoderPointer, kDecoder);
        CpuContext context{};
        movie.Initialise(context);
        mpeg.Reset(kDecoder);
        Check(restorer.Registry().Register(clock.Section()) &&
              restorer.Registry().Register(services.Section()) &&
              movie_state.Register(restorer.Registry()) && join.Register(restorer) &&
              movie_state.RegisterPreparation(restorer) && graphics.Register(restorer),
              "clock, services, Movie and held Graphics register with pure joins and staged resources");
        restorer.RequireSectionOwners();
        restorer.SetImageValidation([this](const auto& meta, const auto& sections, auto* why) {
            std::string reason;
            const bool valid = clock.ValidateImage(meta, sections, reason);
            if (!valid && why) *why = reason;
            return valid;
        });
        restorer.SetPreRestore([this](std::string* why) {
            if (!movie_state.CanCheckpoint(why) || !services.CanCheckpoint(why) ||
                !graphics.checkpoint.CanCapture(why)) return false;
            ++teardown; domain.ReAnchor(source.RawCount()); return true;
        });
    }
    ~Fixture() {
        Check(title::SoundControls::Reset(), "discard old producer commands");
        title::MusicLibrary::Reset();
        Check(title::MusicBuffers::Reset() && title::MusicBuffers::Install(capture, install, nullptr),
              "both Movie loans returned before pool destruction");
        StreamLibrary::Reset();
        title::SoundBanks::Reset();
        Check(title::MusicLibrary::Install(capture, install, nullptr) &&
              title::SoundControls::Install(capture, install, nullptr) &&
              title::SoundBanks::Install(capture, install, nullptr) &&
              StreamLibrary::Install(capture, install, false), "providers detach before their captured owners die");
        LoaderFileIo::Bind({}, {});
        RecompAbi::BindMemory(nullptr);
    }
    void Advance(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            domain.NoteArrival(source.RawCount());
            domain.OnRawTick(source.Tick()); domain.OnRawTick(source.Tick());
            domain.OnBoundary(source.RawCount());
        }
    }
    std::vector<uint8_t> Image(const std::vector<Blob>& blobs) {
        FileMeta meta;
        for (const auto& blob : blobs) if (blob.tag == SessionClock::kTag) {
            SessionClock::Snapshot state;
            Check(SessionClock::Decode(blob, state, error) &&
                  SessionClock::SetFrame(meta, state.domain.boundaries, error), "source metadata matches saved GCLK");
        }
        return SerializeStateFile(meta, blobs);
    }
    bool Restore(const std::vector<Blob>& blobs) {
        const auto image = Image(blobs);
        return restorer.RestoreImage(image.data(), image.size(), &error);
    }
};

void Run() {
    Fixture f;
    f.Advance(17);
    std::vector<uint8_t> adpcm(17 * 16);
    for (size_t block = 0; block < 17; ++block) {
        adpcm[block * 16] = uint8_t((block % 5) * 16 + 2);
        for (size_t i = 2; i < 16; ++i) adpcm[block * 16 + i] = uint8_t(11 + i * 21 + block);
    }
    adpcm[1] = 4; adpcm[16 * 16 + 1] = 3;
    f.audio.ReserveSounds(8);
    Check(f.audio.StoreSound(0x8000, adpcm) && f.audio.SoundBankLoaded(0x8000) &&
          f.audio.PlaySound(0x8000, 2, 8000, 19000, 0x1743, 3, 17, 0) == 0,
          "actual owned Audio has an audible looping voice to preserve");
    f.audio.MixTick();
    Check(f.graphics.Populate(0x803751b9) && f.movie_audio.SetGain(8191), "held image and closed Movie gain are owned");
    MovieCheckpoint::Snapshot movie;
    Check(f.movie_state.Capture(movie, &f.error), "capture closed Movie");
    movie.contexts.front().authored_fps = 29.97;
    movie.contexts.front().clock = {true, -24, 2, 4, 7};
    Check(f.mpeg.RestoreIdle(movie.contexts, int64_t(f.domain.DomainTick()), &f.error),
          "seed carried closed software deadline before application capture");
    auto saved = f.restorer.Registry().Capture();
    Check(f.join.Validate(saved, &f.error), "saved service tick matches saved GCLK");
    const auto expected = f.audio.MixTick();
    const auto expected_pixels = f.graphics.Pixels();
    Check(std::any_of(expected.begin(), expected.end(), [](float v) { return v != 0; }), "reference PCM is audible");
    f.Advance(200);
    f.audio.MixTick();
    Check(f.graphics.Populate(0x80791aee) && f.movie_audio.SetGain(32767), "live owner differs from incoming image");
    const auto live_pixels = f.graphics.Pixels();
    const auto image = f.Image(saved);
    const auto flushes_before = f.pcm.Flushes(), gains_before = f.movie_audio.Count().gain_changes;
    const auto live_tick = f.domain.DomainTick();
    const auto live_factories = f.factory_live;
    f.fail_context = true;
    Check(!f.restorer.PrepareImage(image.data(), image.size(), &f.error) && !f.restorer.HasPrepared() &&
          !f.movie_state.HasPrepared() && f.factory_live == live_factories,
          "actual MPEG context construction failure releases unpublished state");
    Check(f.teardown == 0 && f.domain.DomainTick() == live_tick && f.movie_audio.Gain() == 32767 &&
          f.pcm.Flushes() == flushes_before && f.graphics.Pixels() == live_pixels,
          "MPEG allocation failure leaves every live owner usable before teardown");
    f.fail_context = false;
    f.graphics.backend.fail_create = true;
    Check(!f.restorer.PrepareImage(image.data(), image.size(), &f.error) && !f.restorer.HasPrepared(),
          "real Graphics allocation failure refuses before destructive owner unwind");
    Check(!f.movie_state.HasPrepared() && f.factory_live == live_factories &&
          f.teardown == 0 && f.domain.DomainTick() == live_tick && f.movie_audio.Gain() == 32767 &&
          f.pcm.Flushes() == flushes_before && f.graphics.Pixels() == live_pixels,
          "failed staging preserves live clock, closed PCM gain and held picture");
    f.graphics.backend.fail_create = false;
    Check(f.restorer.PrepareImage(image.data(), image.size(), &f.error) && f.restorer.HasPrepared(),
          "prepare all actual incoming owners before application unwind");
    Check(f.movie_state.HasPrepared() && f.factory_live == live_factories + 1,
          "MPEG context is already allocated while the live owner remains intact");
    f.restorer.DiscardPrepared();
    Check(!f.restorer.HasPrepared() && !f.movie_state.HasPrepared() && f.factory_live == live_factories &&
          f.graphics.Pixels() == live_pixels && f.teardown == 0,
          "cancelling application preparation balances unpublished MPEG ownership");
    Check(f.restorer.PrepareImage(image.data(), image.size(), &f.error), "cancelled image can be prepared anew");
    Check(f.teardown == 0 && f.graphics.Pixels() == live_pixels && f.movie_audio.Gain() == 32767,
          "staging publishes no incoming Movie/Graphics state");
    const auto staged = f.graphics.ordered;
    const auto staged_allocations = f.graphics.backend.create_attempts;
    const auto staged_factories = f.factory_copies;
    auto changed = image; changed.back() ^= 1;
    Check(!f.restorer.RestorePrepared(changed.data(), changed.size(), &f.error) && f.teardown == 0 &&
          f.graphics.ordered == staged, "prepared owner image is immutable before teardown");
    Check(f.restorer.RestorePrepared(image.data(), image.size(), &f.error) && !f.restorer.HasPrepared(),
          "actual prepared application restore runs clock before services, Movie and held pictures");
    Check(f.graphics.backend.create_attempts == staged_allocations && f.factory_copies == staged_factories &&
          f.factory_live == live_factories && !f.movie_state.HasPrepared(),
          "applying MPEG and Graphics reuses prepared resources and releases the previous owners");
    Check(f.audio.MixTick() == expected, "audible Audio continues exactly after closed Movie restore");
    Check(f.graphics.Pixels() == expected_pixels, "held picture bytes restore exactly");
    Check(f.movie_audio.Gain() == 8191 && f.movie_audio.Count().gain_changes == gains_before,
          "closed PCM gain restores without executing a new volume intent");
    Check(f.pcm.Flushes() == flushes_before, "closed MPEG reconstruction executes no new PCM flush");
    Check(f.movie_state.Capture(movie, &f.error) && movie.contexts.front().clock.ticks_to_next == -24,
          "relative Movie deadline uses the same restored domain as Audio and files");
    const auto before = f.restorer.Registry().Capture();
    const auto flushes = f.pcm.Flushes();
    const auto teardowns = f.teardown;
    for (unsigned fault = 0; fault < 4; ++fault) {
        auto bad = saved;
        if (fault == 0) Remove(bad, SessionClock::kTag);
        if (fault == 1) Remove(bad, SessionServices::kTag);
        if (fault == 2) {
            SessionClock::Snapshot wrong = f.clock.Capture();
            ++wrong.domain.tick; ++wrong.raw_tick;
            Check(SessionClock::Encode(wrong, Find(bad, SessionClock::kTag).payload, f.error),
                  "different saved service clock is structurally valid");
        }
        if (fault == 3) Find(bad, SessionServices::kTag).flags = 0;
        Check(!f.restorer.Registry().Validate(bad, &f.error) && !f.Restore(bad),
              "clock/owner mismatch refuses in both interactive registry and actual restore paths");
        Check(f.teardown == teardowns && f.pcm.Flushes() == flushes &&
              SerializeStateFile({}, f.restorer.Registry().Capture()) == SerializeStateFile({}, before),
              "CRC-valid mismatch cannot mutate Movie, PCM, Audio or file ownership");
    }
    SessionServices inactive(false, nullptr);
    ServiceClock inactive_join(f.clock, inactive);
    StateRestorer inactive_restorer(f.memories);
    Check(inactive_restorer.Registry().Register(f.clock.Section()) &&
          f.movie_state.Register(inactive_restorer.Registry()) && inactive_join.Register(inactive_restorer),
          "unselected service composition registers the same admission policy");
    auto optional = saved;
    Find(optional, SessionServices::kTag).flags = 0;
    const auto optional_image = SerializeStateFile({}, optional);
    unsigned inactive_teardown = 0;
    inactive_restorer.SetPreRestore([&](std::string*) { ++inactive_teardown; return true; });
    Check(!inactive_restorer.RestoreImage(optional_image.data(), optional_image.size(), &f.error) &&
          inactive_teardown == 0, "unfiltered admission rejects an optional SVCS hidden by owner filtering");

    CpuContext context{};
    const auto sound = title::MusicBuffers::BorrowMovie(context, 0, Buffers::MoviePart::SoundRing);
    const auto disc = title::MusicBuffers::BorrowMovie(context, 0, Buffers::MoviePart::DiscStream);
    Check(sound && disc && f.buffers.ActiveLoans() == 2, "both actual Movie provider ranges are lent");
    Check(!f.services.CanCheckpoint(&f.error) && !f.Restore(saved) && f.teardown == teardowns,
          "closed MPEG alone cannot authorize capture or restore while either buffer consumer is live");
    Check(title::MusicBuffers::ReturnMovie(sound) && !f.services.CanCheckpoint(&f.error),
          "returning only the sound ring leaves the disc loan protected");
    Check(title::MusicBuffers::ReturnMovie(disc) && f.services.CanCheckpoint(&f.error) &&
          f.Restore(saved), "both returns permit the next closed Movie/service restore");
    Check(f.audio.MixTick() == expected && f.buffers.ActiveLoans() == 0,
          "repeated restore preserves audible output and never invents renewed Movie loans");

    // Admission can change between preparation and the joined apply. A
    // newly live PCM owner must still refuse before any saved state publishes,
    // while both preparation handles are released for the next request.
    Check(f.restorer.PrepareImage(image.data(), image.size(), &f.error) && f.movie_state.HasPrepared(),
          "prepare software state before final current-owner admission");
    const auto prior_teardown = f.teardown, prior_live = f.factory_live;
    const int16_t late_pcm[]{912, -73};
    f.pcm.Push(late_pcm, 1);
    Check(!f.restorer.RestorePrepared(image.data(), image.size(), &f.error) && !f.restorer.HasPrepared() &&
          !f.movie_state.HasPrepared() && f.teardown == prior_teardown && f.pcm.Queued() == 1 &&
          f.factory_live + 1 == prior_live && f.graphics.Pixels() == expected_pixels,
          "changed live owner refuses apply, releases prepared MPEG and preserves live PCM/picture");
    f.pcm.Flush(); // explicit source-close input; failed restore did not discard it
    Check(f.Restore(saved) && !f.movie_state.HasPrepared(), "a later closed restore can prepare and apply again");
}
}  // namespace

int main() {
    Run();
    std::printf("MovieServiceCheckpoint: %u checks passed (real owners, audible continuation, clock join, both loans).\n",
                checks);
}
