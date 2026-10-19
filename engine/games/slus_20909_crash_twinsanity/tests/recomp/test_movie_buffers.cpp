// Buffer ownership at the actual NTSC Prepare/OpenStream/CloseStream services.
// Both arms run the same filename, lending and setup fixtures; the replaced
// player bodies, CD stream, IOP heap and selected StreamBuffers are production.
// OLD pins: SOUND_P8_REPORT stop retains/close frees and duplicate allocation;
// SOUND_P5_IMPL EOF never flushes PCM, abort/reopen returns real ownership.
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/cdvd/hle/libcdvd_bindings.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/cdvd/stream_reader.h"
#include "engine/ee/cpu/guest_park.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/iop/sysmem_heap.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

namespace {
using Movie = title::MovieDecoder;
using Stream = title::MovieStream;
using Buffers = title::StreamBuffers;
unsigned checks = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
uint32_t R(const CpuContext& c, unsigned r) { return c.cpuRegs.GPR.r[r].UL[0]; }
struct Input {
    std::filesystem::path path;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(80 * 2048);
};
Input MakeInput() {
        Input input;
        for (size_t i = 0; i < input.bytes.size(); ++i) input.bytes[i] = uint8_t(i * 7 + (i >> 11));
        auto* p = input.bytes.data() + 16 * 2048;
        p[0] = 1; std::memcpy(p + 1, "CD001", 5); p[6] = 1;
        std::fill(p + 80, p + 88, 0); p[80] = 80; p[87] = 80;
        std::fill(p + 156, p + 190, 0);
        p[156] = 34; p[158] = 20; p[165] = 20;
        p[167] = 8; p[172] = 8; p[181] = 2; p[188] = 1;
        // A real ISO directory is shared by LibcdvdBindings::SearchFile and
        // the selected lookup. Neither arm supplies a canned lookup answer.
        std::fill(input.bytes.begin() + 20 * 2048, input.bytes.begin() + 22 * 2048, 0);
        const auto record = [](uint8_t* at, std::string_view name, uint32_t sector,
                               uint32_t bytes, bool directory) {
            const size_t length = (33 + name.size() + 1) & ~size_t(1);
            at[0] = uint8_t(length); at[25] = directory ? 2 : 0;
            at[32] = uint8_t(name.size());
            for (unsigned i = 0; i < 4; ++i) {
                at[2 + i] = uint8_t(sector >> (8 * i));
                at[9 - i] = at[2 + i];
                at[10 + i] = uint8_t(bytes >> (8 * i));
                at[17 - i] = at[10 + i];
            }
            std::memcpy(at + 33, name.data(), name.size());
        };
        record(input.bytes.data() + 20 * 2048, "MOVIES", 21, 2048, true);
        record(input.bytes.data() + 21 * 2048, "LOGO.PSS;1", 32, 5 * 2048 + 17, false);
        input.path = std::filesystem::temp_directory_path() /
            ("movie-buffers-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".iso");
        std::ofstream out(input.path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(input.bytes.data()), std::streamsize(input.bytes.size()));
        Check(bool(out), "real backing file created");
        return input;
}
struct TestMovieBuffers;
using Fixture = TestMovieBuffers;
Fixture* current = nullptr;

struct TestMovieBuffers {
    static constexpr uint32_t gp = 0x3114f0, player = 0x10000, stack = 0x1f00000;
    MainMemory ram;
    Scratchpad scratch;
    AddressMap map{ram, scratch};
    MmioSink sink;
    MmioRouter router{sink};
    FastMem fast{ram, map, router};
    ps2r::ipu::GuestMemoryAdapter mem{[this](uint32_t a, size_t n) { return map.Translate(a, n); }};
    IsoFs fs;
    ReadEngine files{fs};
    StreamReader retained{&files};
    ps2r::iop::SysmemHeap heap;
    Buffers buffers;
    Stream stream{mem, [this](uint64_t offset, size_t n, uint8_t* out) {
        return int64_t(fs.ReadBytes(offset, n, out));
    }};
    unsigned initializes = 0, destroys = 0, lends = 0, reclaims = 0, allocations = 0, frees = 0;
    std::vector<uint32_t> setup;
    std::map<uint32_t, unsigned> generated;
    std::map<uint32_t, Movie::Body> installed;
    Movie movie;
    CpuContext c{};
    uint32_t lender = 0;
    bool selected, found = true, fail_disc = false, short_view = false, fail_return = false;
    bool lookup_error = false, lookup_throws = false, invalid_location = false;
    bool service_buffers = false;
    std::vector<Buffers::Loan> loans;
    std::map<uint32_t, std::string> strings;

    Movie::Hooks Hooks() {
        Movie::Hooks h;
        h.create = [](CpuContext& x) { x.cpuRegs.GPR.r[2].SD[0] = 1; };
        h.reset = h.create; h.picture_rgb32 = h.create; h.picture_rgb16 = h.create;
        h.initialize = [this] { ++initializes; };
        h.destroy = [this](uint32_t) { ++destroys; };
        h.call = [this](CpuContext& x, uint32_t a) { Dispatch(a, x); };
        h.has_pending = [](uint32_t) { return false; };
        h.volume = [](uint16_t) {};
        return h;
    }
    TestMovieBuffers(const Input& input, bool use, bool borrowed, uint32_t capacity = 0x100000,
            bool install_buffers = true, bool audio_playback = true, bool install_lookup = true)
        : buffers(capacity), movie(mem, Hooks()), selected(use) {
        current = this;
        Check(fs.Open(input.path.string()), "ISO file provider opens");
        RecompAbi::BindMemory(&fast);
        LibcdvdBindings::Bind(SdkBindingManifest::Instance(), &files, nullptr, &retained);
        c.cpuRegs.GPR.r[28].UD[0] = gp; c.cpuRegs.GPR.r[29].UD[0] = stack;
        c.cpuRegs.GPR.r[31].UD[0] = 0x123456;
        mem.Write32(player + 0x34, 1);
        mem.Write32(0x309824, 0x400000); mem.Write32(0x30a760, 0x410000);
        *mem.Translate(gp - 0x74c0, 1) = 1;
        mem.Write32(gp - Movie::kFileLocationGp, UINT32_MAX);
        if (borrowed) {
            if (selected) {
                Check(buffers.Allocate(0, 0x40000, 0), "real music storage allocated");
                lender = buffers.Inspect(0).handle;
            } else lender = heap.Alloc(0x40000);
        }
        const auto capture = Capture();
        const auto install = Install();
        Check(movie.Install(capture, install, true) == 7, "decoder lifecycle installed");
        Check(movie.InstallAudio(capture, install) == 2, "PCM consumer installed");
        Check(movie.InstallAudioPlayback(capture, install, audio_playback) == 1, "PCM start/stop selection installed");
        Check(movie.InstallStream(capture, install, {
            [this](uint32_t l) { return stream.Seek(l); },
            [this](uint8_t* b, uint32_t n) { return stream.Read(b, n); }}) == 2, "decoder cursor installed");
        Check(movie.InstallInput(capture, install, [](uint32_t, uint32_t, int32_t) { return 0; }) == 1,
              "synchronous demux installed");
        Check(stream.Install(capture, install, selected) == 3, "stream selection installed");
        if (selected && install_buffers) Check(stream.InstallBuffers(capture, install, movie, Services()) == 5,
                            "both consumers and both returns installed as one transaction");
        if (install_lookup && (!selected || install_buffers))
            Check(stream.InstallLookup(capture, install, selected ? Lookup() : Stream::Lookup{}) == 4,
                  "lookup owner or real retained file dispatch census installed");
    }
    ~TestMovieBuffers() {
        stream.ResetAfterRestore();
        Check(buffers.ActiveLoans() == 0, "fixture leaves no provider loans");
        RecompAbi::BindMemory(nullptr); current = nullptr;
    }
    Stream::BufferServices Services() {
        return {
            [this](CpuContext& context, uint32_t handle, Stream::BufferPart part) {
                Stream::BufferLoan result;
                if (fail_disc && part == Stream::BufferPart::DiscStream) return result;
                const auto requested = part == Stream::BufferPart::SoundRing ?
                    Buffers::MoviePart::SoundRing : Buffers::MoviePart::DiscStream;
                const auto loan = service_buffers ? title::MusicBuffers::BorrowMovie(context, handle, requested) :
                    buffers.BorrowMovie(handle, requested);
                if (!loan) return result;
                loans.push_back(loan);
                const auto view = service_buffers ? title::MusicBuffers::Resolve(loan) : buffers.Resolve(loan);
                result.handle = loan.handle; result.bytes = short_view ? 1 : view.bytes;
                result.data = view.data;
                result.release = [this, loan] {
                    return !fail_return && (service_buffers ? title::MusicBuffers::ReturnMovie(loan) :
                                           buffers.ReturnMovie(loan));
                };
                return result;
            },
            [this](CpuContext& x, uint32_t a) { Dispatch(a, x); }
        };
    }
    Stream::Lookup Lookup() {
        return [this](std::string_view path, Stream::File& file) {
            Check(path == (found ? "MOVIES/LOGO.PSS" : "MOVIES/MISSING.PSS"),
                  "selected lookup receives the same source-built filename");
            if (lookup_throws) throw std::runtime_error("provider lookup failure");
            if (lookup_error) return Stream::LookupResult::Error;
            IsoFs::Entry entry;
            if (!files.Find(std::string(path), entry)) return Stream::LookupResult::Missing;
            file = {invalid_location ? UINT32_MAX : entry.lba, entry.size};
            return Stream::LookupResult::Found;
        };
    }
    Movie::Capture Capture() {
        return [this](uint32_t address) -> Movie::Body {
            return [this, address](CpuContext& x, uint32_t) { Original(address, x); };
        };
    }
    Movie::Installer Install() {
        return [this](uint32_t a, Movie::Body b) { installed[a] = std::move(b); return true; };
    }
    void Dispatch(uint32_t a, CpuContext& x) {
        const auto found_body = installed.find(a);
        if (found_body != installed.end()) found_body->second(x, a);
        else Original(a, x);
    }
    void Original(uint32_t a, CpuContext& x) {
        if (movie_oracle::RunGenerated(a, x)) return;
        switch (a) {
        case 0x1e01b0: ++lends; x.cpuRegs.GPR.r[2].UD[0] = lender; return;
        case 0x1e6230: ++reclaims; return; // same game decision fixture in both arms
        case 0x2d45e0: ++allocations; x.cpuRegs.GPR.r[2].UD[0] = heap.Alloc(R(x, 4)); return;
        case 0x2d46c8: ++frees; x.cpuRegs.GPR.r[2].UD[0] = heap.Free(R(x, 4)); return;
        case 0x2c4860: LibcdvdBindings::StInit(x); return;
        case 0x2c4890: LibcdvdBindings::StStart(x); return;
        case 0x2c4900: LibcdvdBindings::StStop(x); return;
        case 0x2c4610: LibcdvdBindings::Break(x); return;
        case 0x2c4358: LibcdvdBindings::DiskReady(x); return;
        case 0x2c3bf0: LibcdvdBindings::Sync(x); return;
        case 0x203dc8: strings[R(x, 4)] = "movies/"; Publish(R(x, 4)); return;
        case 0x204060:
            strings[R(x, 4)] += R(x, 5) == 0x30a048 ? (found ? "logo" : "missing") : ".pss";
            Publish(R(x, 4)); return;
        case 0x203ba8: strings.erase(R(x, 4)); return;
        case 0x2c6cb0:
            x.cpuRegs.GPR.r[2].UD[0] = R(x, 4) >= 'a' && R(x, 4) <= 'z' ? R(x, 4) - 32 : R(x, 4);
            return;
        case 0x2c3a78:
            Check(std::string(reinterpret_cast<char*>(mem.Translate(R(x, 5), 20))) ==
                  (found ? "MOVIES/LOGO.PSS" : "MOVIES/MISSING.PSS"),
                  "source filename is built and uppercased before lookup");
            LibcdvdBindings::SearchFile(x); return;
        case 0x2cf560: case 0x1818b0: case 0x17dbf8: case 0x2c1b50: case 0x17ff50:
            setup.push_back(a); return;
        default: Check(false, "unexpected retained service in bounded player oracle");
        }
    }
    void Publish(uint32_t descriptor) {
        const auto& str = strings.at(descriptor);
        mem.Write32(descriptor, 0x300000); mem.Write32(descriptor + 4, uint32_t(str.size()));
        std::memcpy(mem.Translate(0x300000, str.size() + 1), str.c_str(), str.size() + 1);
    }
    void Call(uint32_t a, uint32_t a0 = player, uint32_t caller = 0x123456) {
        c.cpuRegs.GPR.r[4].UD[0] = a0; c.cpuRegs.GPR.r[31].UD[0] = caller;
        const uint32_t sp = R(c, 29);
        Dispatch(a, c);
        Check(R(c, 29) == sp && R(c, 28) == gp && R(c, 31) == caller, "source public ABI callee registers preserved");
    }
    void Prepare() { Call(0x2ae1c8); }
    void Open() { Call(0x2ae860); }
    void Close() {
        Call(0x2af5d0);
        const uint32_t ring = mem.Read32(Movie::kSoundRing);
        if (ring) {
            const bool lent = *mem.Translate(gp - 0x6ba4, 1) != 0;
            Call(lent ? 0x1e6230 : 0x2d46c8, ring, lent ? 0x2ae704 : 0x2ae714);
        }
        mem.Write32(Movie::kSoundRing, 0); mem.Write32(Movie::kSoundRingSize, 0);
    }
    int32_t Read(uint8_t* out, uint32_t sectors) {
        if (stream.OwnsCursor()) return stream.Read(out, sectors);
        uint32_t error = 0;
        const auto n = retained.Read(sectors, out, &error);
        return error ? -1 : int32_t(n);
    }
};

struct Trace {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> setup;
    unsigned lends = 0, reclaims = 0;
    uint64_t cd = 0;
};
Trace Run(const Input& input, bool selected, bool borrowed) {
    Fixture f(input, selected, borrowed);
    Trace trace;
    const auto before = LibcdvdBindings::StreamCalls();
    const auto search_before = LibcdvdBindings::SearchCalls();
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        f.Prepare();
        Check(f.initializes == 1, "initialise stays once across close/reopen");
        Check(!selected || !f.stream.CanCheckpoint(), "Prepare preflight prevents checkpoint");
        if (selected) Check(f.buffers.ActiveLoans() == 1, "PCM Prepare borrows only sound");
        f.Open();
        Check(f.mem.Read32(Movie::kArenaNext) == 0x410000 &&
              f.mem.Read32(Movie::kArenaEnd) == 0x940000, "original renderer arena bounds retained");
        Check(f.mem.Read32(Fixture::gp - Movie::kFileLocationGp) == 32, "lookup publishes file location");
        Check(f.mem.Read32(Fixture::gp - Movie::kFileSizeGp) == 5 * 2048 + 17,
              "lookup publishes actual unrounded asset size");
        if (selected) {
            Check(f.buffers.ActiveLoans() == 2, "Open independently borrows disc");
            auto sound = f.buffers.Resolve(f.loans[f.loans.size() - 2]);
            auto disc = f.buffers.Resolve(f.loans.back());
            Check(sound && disc, "both real loan views resolve");
            std::fill_n(sound.data, sound.bytes, 0x5a);
            std::fill_n(disc.data, disc.bytes, 0xa5);
            Check(sound.data[0] == 0x5a && sound.data[sound.bytes - 1] == 0x5a,
                  "disc writes cannot alias the separately resolved sound range");
            Check(f.mem.Read32(Movie::kSoundRing) == f.loans[f.loans.size() - 2].handle &&
                  f.mem.Read32(Fixture::player + 0x41f8) == f.loans.back().handle,
                  "guest client identities use raw opaque handles without offset or alignment");
        }
        std::vector<uint8_t> output(2 * 2048);
        Check(f.Read(output.data(), 2) == 2, "callback consumes source bytes");
        trace.bytes.insert(trace.bytes.end(), output.begin(), output.end());
        if (selected) {
            Check(f.stream.Seek(80) == 1 && f.Read(output.data(), 1) == 0 &&
                  f.buffers.ActiveLoans() == 2, "EOF keeps both loans until source close, with no PCM flush");
        }
        f.Close();
        Check(f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(), "normal close returns both loans");
        Check(f.mem.Read32(Fixture::player + 0x41f8) == 0 &&
              f.mem.Read32(Fixture::gp - Movie::kFileLocationGp) == UINT32_MAX, "close publishes source empty fields");
        if (selected) Check(f.buffers.Used() == (borrowed ? 0x40000u : 0u), "close returns exact storage baseline");
    }
    trace.cd = LibcdvdBindings::StreamCalls() - before;
    trace.setup = f.setup; trace.lends = f.lends; trace.reclaims = f.reclaims;
    Check(trace.cd == (selected ? 0 : 9), "actual CD stream dispatch positive control versus selected zero");
    Check(LibcdvdBindings::SearchCalls() - search_before == (selected ? 0 : 3),
          "actual retained search dispatch 3 versus selected zero");
    Check(f.stream.Counts().file_original_calls == (selected ? std::array<uint64_t, 4>{} :
          std::array<uint64_t, 4>{3, 12, 3, 3}),
          "all seven real Movie readiness/search/sync calls per cycle counted versus selected zero");
    Check(f.stream.Counts().lookups == (selected ? 3 : 0) &&
          f.stream.Counts().lookup_errors == 0, "selected synchronous lookup has positive owned work");
    Check(f.allocations == (!selected && !borrowed ? 6u : 0u) &&
          f.frees == f.allocations, "actual standalone IOP heap allocation/free 6/6 versus selected 0/0");
    Check(selected || f.generated[0x2ae1c8] == 3, "retained Prepare body executed");
    Check(!selected || (f.generated[0x2ae1c8] == 0 && f.generated[0x2ae860] == 0 &&
          f.generated[0x2af5d0] == 0), "selected replaced player bodies never execute");
    return trace;
}

void LookupFailures(const Input& input) {
    for (unsigned error = 0; error != 3; ++error) {
        Fixture f(input, true, true);
        f.lookup_error = error == 0; f.lookup_throws = error == 1; f.invalid_location = error == 2;
        f.Prepare();
        bool threw = false;
        try { f.Open(); } catch (const std::runtime_error&) { threw = true; }
        Check(threw == f.lookup_throws && f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(),
              "lookup error/exception/invalid location balances sound and source lending");
        Check(f.strings.empty() && R(f.c, 29) == Fixture::stack,
              "lookup failures destroy source filename and preserve stack");
        Check(f.mem.Read32(Fixture::gp - Movie::kFileLocationGp) == UINT32_MAX &&
              f.stream.Counts().file_original_calls == std::array<uint64_t, 4>{} &&
              f.stream.Counts().lookup_errors == 1,
              "lookup failure publishes no file without a hidden disc fallback");
        f.lookup_error = f.lookup_throws = f.invalid_location = false;
        f.Prepare(); f.Open(); f.Close();
        Check(f.buffers.ActiveLoans() == 0, "lookup failure remains reopenable");
    }
    for (bool selected : {false, true}) {
        Fixture f(input, selected, false);
        f.found = false;
        f.Prepare(); f.Open();
        Check(f.mem.Read32(Fixture::gp - Movie::kFileLocationGp) == UINT32_MAX,
              "real missing asset yields same source no-file result");
        Check(!selected || (f.buffers.ActiveLoans() == 0 && f.stream.Counts().missing_files == 1),
              "missing file is distinguished from provider failure");
        f.Close();
    }
    Fixture f(input, true, false);
    for (const auto entry : {0x2c4610u, 0x2c4358u, 0x2c3a78u, 0x2c3bf0u}) {
        const uint32_t caller = entry == 0x2c4610 ? 0x2ae920 :
            entry == 0x2c4358 ? 0x2af5f0 : entry == 0x2c3a78 ? 0x2ae964 : 0x2af668;
        bool rejected = false;
        try { f.Call(entry, 1, caller); } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "owned file selection rejects obsolete generated continuation");
    }
    f.Call(0x2c4358, 1, 0x123456);
    Check(R(f.c, 2) == 2 && f.stream.Counts().file_original_calls == std::array<uint64_t, 4>{},
          "unrelated file user remains serviced without false Movie attribution");
}

void Errors(const Input& input) {
    for (const unsigned error : {0u, 1u, 2u, 3u}) {
        Fixture f(input, true, error != 3, error == 3 ? 64 : 0x100000);
        f.found = error != 0; f.fail_disc = error == 1; f.short_view = error == 2;
        f.Prepare(); f.Open();
        Check(f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(), "failed open/borrow returns both independent loans");
        Check(f.mem.Read32(Movie::kSoundRing) == 0 && f.mem.Read32(Fixture::player + 0x41f8) == 0,
              "partial-open failure cannot leave an opaque client pointer");
        Check(f.mem.Read32(Fixture::gp - Movie::kFileLocationGp) == UINT32_MAX,
              "failed open publishes no-file so decoder cannot read prior input");
        if (error != 3) {
            f.found = true; f.fail_disc = f.short_view = false;
            f.Prepare(); f.Open(); f.Close();
            Check(f.buffers.ActiveLoans() == 0, "reopen after each partial failure is reusable");
        }
    }
    Fixture f(input, true, true);
    f.Prepare(); f.Open();
    Check(f.buffers.Close(0) && f.buffers.ActiveLoans() == 2 && f.buffers.Used() == 0x40000,
          "queued music close keeps storage alive for both movie consumers");
    f.Call(0x2af5d0);
    Check(f.buffers.ActiveLoans() == 1 && f.buffers.Resolve(f.loans.front()) &&
          f.buffers.Used() == 0x40000 && !f.stream.CanCheckpoint(),
          "disc reclaim does not invalidate sound or weaken checkpoint guard");
    f.Call(0x1e6230, f.mem.Read32(Movie::kSoundRing), 0x2ae704);
    Check(f.buffers.ActiveLoans() == 0 && f.buffers.Used() == 0, "last sound return frees deferred music close");
    f.mem.Write32(Movie::kSoundRing, 0); f.mem.Write32(Movie::kSoundRingSize, 0);
    Check(f.buffers.Allocate(0, 0x40000, 0), "closed music buffer reopens");
    f.lender = f.buffers.Inspect(0).handle;
    f.Prepare(); f.Open();
    const auto ram = std::vector<uint8_t>(f.ram.Data(), f.ram.Data() + f.ram.Size());
    f.stream.ResetAfterRestore();
    Check(f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint() &&
          std::equal(ram.begin(), ram.end(), f.ram.Data()), "quiescent reset returns loans without editing restored RAM");
    f.Prepare(); f.Open(); f.Close();
}

void Guards(const Input& input) {
    Fixture f(input, true, false);
    f.mem.Write32(Fixture::player + 0x34, 0);
    f.Prepare(); f.Open();
    Check(f.buffers.ActiveLoans() == 1 && f.stream.Counts().sound_loans == 0 &&
          f.mem.Read32(Movie::kSoundRingSize) == 0, "no-PCM movie never borrows a sound ring");
    f.Close();
    Check(f.buffers.ActiveLoans() == 0 && f.buffers.Used() == 0, "no-PCM close returns only its disc reservation");
    f.mem.Write32(Fixture::player + 0x34, 1);
    f.Prepare(); f.Open();
    const uint32_t other = f.heap.Alloc(512);
    f.Call(0x2d46c8, other, 0x123456);
    Check(f.heap.LiveBlocks() == 0 && f.buffers.ActiveLoans() == 2,
          "unrelated heap cleanup retains its real service while movie loans stay owned");
    for (const auto address : {Movie::kCreate, Movie::kFeedAudio, Movie::kReadStream, Movie::kDemultiplex}) {
        bool rejected = false;
        try { f.installed.at(address)(f.c, address + 4); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && f.buffers.ActiveLoans() == 2, "continuation cannot replay a retained IOP consumer with owned handles");
    }
    bool rejected = false;
    try { f.Call(Stream::kPrepare, 0x50, 0x2ae9c4); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "raw stream init cannot align an opaque disc handle");
    rejected = false;
    try { f.Call(Movie::kFeedAudio, Movie::kPlayerFeedSound); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "invalid selected PCM descriptor refuses the retained ring consumer");
    rejected = false;
    f.fail_return = true;
    try { f.Close(); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected && f.buffers.ActiveLoans() == 2 && !f.stream.CanCheckpoint(),
          "failed provider return retains tokens and checkpoint refusal");
    f.fail_return = false; f.Close();
    Check(f.buffers.ActiveLoans() == 0, "return error can be resolved without losing loan identity");
    f.Prepare(); f.Open();
    // A real decoder Start argument validation failure, before software create,
    // must release both preflight loans and preserve its existing zero result.
    for (unsigned i = 0; i < 8; ++i) f.c.cpuRegs.GPR.r[4 + i].UD[0] = 0;
    f.c.cpuRegs.GPR.r[6].UD[0] = 1; f.c.cpuRegs.GPR.r[7].UD[0] = 2;
    f.movie.Start(f.c);
    Check(R(f.c, 2) == 0 && f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(),
          "failed decoder Start balances the already open player loans");
    f.Prepare(); f.Open();
    // Reach the decoder's later transactional failure, not only its argument
    // validator: the deliberately exhausted software arena cannot configure.
    f.c.cpuRegs.GPR.r[4].UD[0] = 1;
    f.c.cpuRegs.GPR.r[5].UD[0] = 0x50000;
    f.c.cpuRegs.GPR.r[6].UD[0] = 1;
    f.c.cpuRegs.GPR.r[7].UD[0] = 2;
    f.c.cpuRegs.GPR.r[8].UD[0] = 1;
    f.mem.Write32(Fixture::stack + 24, 0x100);
    f.mem.Write32(Movie::kArenaEnd, f.mem.Read32(Movie::kArenaNext));
    f.movie.Start(f.c);
    Check(R(f.c, 2) == 0 && f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint() &&
          f.destroys == 1, "software decoder configuration failure returns both loans once");
}

void Installation(const Input& input) {
    {
        Fixture f(input, true, false, 0x100000, false, false);
        Check(f.stream.InstallBuffers(f.Capture(), f.Install(), f.movie, f.Services()) == 0,
              "loan selection refuses retained audio playback consumer");
        Check(f.stream.InstallLookup(f.Capture(), f.Install(), f.Lookup()) == 0,
              "owned lookup refuses installation without both buffer consumers");
    }
    {
        Fixture f(input, true, false, 0x100000, true, true, false);
        const auto install = f.Install();
        unsigned count = 0;
        Check(f.stream.InstallLookup(f.Capture(), [&](uint32_t a, Movie::Body b) {
            return ++count == 3 ? false : install(a, std::move(b));
        }, f.Lookup()) == 0, "lookup wrapper installation failure rolls back prior entries");
        f.Prepare(); f.Open(); f.Close();
        Check(f.stream.Counts().lookups == 0 && f.buffers.ActiveLoans() == 0,
              "failed lookup install preserves complete prior buffer service path");
        Check(f.stream.InstallLookup(f.Capture(), install, f.Lookup()) == 4,
              "lookup install can be retried after rollback");
    }
    Fixture f(input, true, false, 0x100000, false);
    const auto install = f.Install();
    unsigned count = 0;
    Check(f.stream.InstallBuffers(f.Capture(), [&](uint32_t a, Movie::Body b) {
        if (++count == 3) return false;
        return install(a, std::move(b));
    }, f.movie, f.Services()) == 0, "partial binding installation rolls back both consumers");
    // No selected music handles exist in this failed installation. The
    // restored actual Prepare is executable and makes real standalone IOP storage.
    f.Prepare();
    Check(f.generated[0x2ae1c8] == 1 && f.allocations == 1 && f.buffers.ActiveLoans() == 0,
          "failed install restored the actual source body, not a no-op");
    f.heap.Free(f.mem.Read32(Movie::kSoundRing));
}

// Application contract: both Movie consumers use the actual ordered music
// service and its pool. Send failure is an explicit provider fault here; the
// retained player/CD/heap output comparison remains Run(), above.
void ComposedMusicProviders(const Input& input) {
    Fixture f(input, true, false);
    title::Audio audio;
    bool send_busy = false;
    unsigned sends = 0;
    const auto capture = [&](uint32_t address) -> Movie::Body {
        const auto existing = f.installed.find(address);
        if (existing != f.installed.end()) return existing->second;
        if (address == 0x1e2958) return [&](CpuContext& c, uint32_t) {
            ++sends; c.cpuRegs.GPR.r[2].SD[0] = send_busy ? -1 : 0;
        };
        return [](CpuContext&, uint32_t) { Check(false, "unexpected retained music consumer"); };
    };
    const auto install = f.Install();
    Check(StreamLibrary::Install(capture, install, true) &&
          title::SoundControls::Install(capture, install, &audio, [](uint32_t, uint16_t, uint16_t) {}) &&
          title::MusicLibrary::Install(capture, install, &audio) &&
          title::MusicBuffers::Install(capture, install, &f.buffers),
          "application's file/control/music/pool provider order composes");
    f.service_buffers = true;
    const auto before = title::MusicBuffers::Count();
    const auto allocate = [&] {
        f.c.cpuRegs.GPR.r[5].UD[0] = 0x18000;
        f.c.cpuRegs.GPR.r[6].UD[0] = 0x40000;
        f.Call(0x1e1ca8, 2);
        Check(title::SoundControls::Flush(f.c), "accept actual queued music storage allocation");
        f.lender = f.buffers.Inspect(2).handle;
        Check(f.lender && f.mem.Read32(0x3b3fe0 + 8) == f.lender,
              "Movie lender is the published opaque service handle");
    };
    allocate();
    {
        // Round-2 gate 3 attribution pin (hle.music_buffers: music silent after
        // the movies, music_library errors=2 at Play, closes=0): a Movie
        // lend/return cycle with no service Close must leave the lent music
        // stream playable again. MusicLibrary::Reserve's only buffers-dependent
        // predicate is MusicBuffers::CanPlay.
        Check(title::MusicBuffers::CanPlay(2), "an owned, unlent music stream is playable");
        title::MusicRequest stop;
        stop.kind = title::MusicRequest::Kind::Stop; stop.stream = 2;
        Check(title::SoundControls::StageMusic(f.c, stop, 3), "source stop prefix queued before lending");
        f.Prepare();
        Check(!title::MusicBuffers::CanPlay(2), "a stream whose storage holds the Movie sound view is not playable");
        f.Open();
        Check(!title::MusicBuffers::CanPlay(2) && f.buffers.ActiveLoans() == 2, "nor while both Movie views are held");
        f.Close();
        Check(f.buffers.ActiveLoans() == 0 && f.buffers.Inspect(2).allocated && f.buffers.Used() == 0x40000 &&
              title::MusicBuffers::CanPlay(2),
              "after the Movie returns both views the stream is playable again without a service Close");
        const auto scratch = title::MusicBuffers::BorrowMovie(f.c, 0, title::StreamBuffers::MoviePart::SoundRing);
        Check(scratch.handle && scratch.handle != f.lender && f.buffers.ActiveLoans() == 1 && title::MusicBuffers::CanPlay(2),
              "a no-lender scratch loan never blocks the music streams");
        Check(title::MusicBuffers::ReturnMovie(scratch) && f.buffers.ActiveLoans() == 0 && f.buffers.Used() == 0x40000,
              "the scratch loan returns its own storage only");
    }
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        title::MusicRequest stop;
        stop.kind = title::MusicRequest::Kind::Stop; stop.stream = 2;
        Check(title::SoundControls::StageMusic(f.c, stop, 3), "source stop prefix queued before lending");
        const auto stops = title::MusicLibrary::Count().stops;
        f.Prepare();
        Check(title::MusicLibrary::Count().stops == stops + 1 && f.buffers.ActiveLoans() == 1,
              "sound loan accepts pending Stop before ownership transfer");
        f.Open();
        Check(f.buffers.ActiveLoans() == 2 && !f.stream.CanCheckpoint(),
              "both production providers own independent views");
        f.Call(0x1e6ea8, 2);
        Check(title::SoundControls::Flush(f.c) && f.buffers.Used() == 0x40000,
              "queued service Close defers physical release until Movie returns both views");
        f.Close();
        Check(f.buffers.ActiveLoans() == 0 && f.buffers.Used() == 0 && f.stream.CanCheckpoint(),
              "every composed close returns both tokens and real capacity");
        allocate();
    }
    send_busy = true;
    f.Prepare(); f.Open();
    Check(f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(),
          "busy acceptance cannot publish a Movie view or strand source lending");
    send_busy = false;
    f.Prepare();
    f.fail_disc = true;
    f.Open();
    Check(f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(),
          "second-provider failure returns the first actual MusicBuffers token");
    f.fail_disc = false;
    f.Prepare(); f.Open();
    const auto loan_count = f.buffers.ActiveLoans();
    f.stream.ResetAfterRestore();
    Check(loan_count == 2 && f.buffers.ActiveLoans() == 0 && f.stream.CanCheckpoint(),
          "supervisor teardown returns both actual provider closures");
    const auto after = title::MusicBuffers::Count();
    Check(after.sound_loans > before.sound_loans && after.disc_loans > before.disc_loans &&
          after.returns - before.returns ==
              after.sound_loans - before.sound_loans + after.disc_loans - before.disc_loans &&
          sends && f.heap.LiveBlocks() == 0,
          "positive production loans balance exactly without any Movie IOP allocation");
    Check(title::SoundControls::Reset(), "discard control producers before detach");
    title::MusicLibrary::Reset();
    Check(title::MusicBuffers::Reset() && title::MusicBuffers::Install(capture, install, nullptr),
          "empty provider pool detaches after Movie consumers");
    Check(title::MusicLibrary::Install(capture, install, nullptr) &&
          title::SoundControls::Install(capture, install, nullptr),
          "music and control callbacks detach before Audio lifetime ends");
    StreamLibrary::Reset();
    Check(StreamLibrary::Install(capture, install, false), "file provider detaches");
}
}  // namespace

namespace movie_oracle {
std::ostringstream g_logFile;
void Read(uint32_t a, void* out, size_t n) {
    auto* p = current->mem.Translate(a, n); Check(p != nullptr, "generated read in RAM"); std::memcpy(out, p, n);
}
void Write(uint32_t a, const void* in, size_t n) {
    auto* p = current->mem.Translate(a, n); Check(p != nullptr, "generated write in RAM"); std::memcpy(p, in, n);
}
void Dispatch(uint32_t a, CpuContext& c) { current->Dispatch(a, c); }
void ObserveGenerated(uint32_t a) { ++current->generated[a]; }
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t) { return nullptr; }
}  // namespace movie_oracle

int main(int argc, char** argv) {
    if (argc != 2 || std::string(argv[1]) != "--both-paths") return 2;
    GuestPark::SetSink([](const ParkRecord& r) { throw std::runtime_error(r.reason); });
    const Input input = MakeInput();
    for (const bool borrowed : {false, true}) {
        const auto model = Run(input, false, borrowed), selected = Run(input, true, borrowed);
        Check(model.bytes == selected.bytes && model.setup == selected.setup &&
              model.lends == selected.lends && model.reclaims == selected.reclaims,
              "same NEW input: file bytes and source setup/lend/reclaim decisions exact");
    }
    Errors(input);
    LookupFailures(input);
    Guards(input);
    Installation(input);
    ComposedMusicProviders(input);
    GuestPark::SetSink({});
    std::error_code error;
    std::filesystem::remove(input.path, error);
    std::printf("MovieBuffers: %u checks; retained player/CD/heap versus two owned loans; default unchanged\n", checks);
}
