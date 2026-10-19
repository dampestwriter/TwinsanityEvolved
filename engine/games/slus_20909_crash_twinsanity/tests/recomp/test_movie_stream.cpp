#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "engine/cdvd/hle/libcdvd_bindings.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/cdvd/stream_reader.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"

namespace {
using Stream = title::MovieStream;
using Movie = title::MovieDecoder;
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// A real backing file with independent sector identities, read by both actual
// ReadEngine/StreamReader and the selected stateless file service. No asset or
// model result is replaced by an expected-return stub.
struct Input {
    std::filesystem::path path;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(80 * Stream::kSectorBytes);
    Input() {
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = uint8_t((i * 29 + (i / Stream::kSectorBytes) * 17 + (i >> 7)) ^ (i >> 3));
        auto* pvd = bytes.data() + 16 * Stream::kSectorBytes;
        pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
        std::fill(pvd + 80, pvd + 88, 0); pvd[80] = 80; pvd[87] = 80;
        std::fill(pvd + 156, pvd + 190, 0);
        pvd[156] = 34; pvd[158] = 20; pvd[165] = 20;
        pvd[167] = 8; pvd[172] = 8; pvd[181] = 2; pvd[188] = 1;
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("movie-stream-" + std::to_string(id) + ".iso");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        Check(bool(file), "write the shared synthetic backing file");
    }
    ~Input() { std::error_code error; std::filesystem::remove(path, error); }
};

struct Fixture {
    static constexpr uint32_t kGp = 0x3114f0, kPlayer = 0x10000, kBuffer = 0x150031;
    MainMemory ram;
    Scratchpad scratch;
    AddressMap map{ram, scratch};
    MmioSink sink;
    MmioRouter router{sink};
    FastMem fast{ram, map, router};
    ps2r::ipu::GuestMemoryAdapter memory{[this](uint32_t a, size_t n) { return map.Translate(a, n); }};
    IsoFs fs;
    ReadEngine files{fs};
    StreamReader model{&files};
    int provider_error = 0;
    unsigned provider_calls = 0;
    Stream stream{memory, [this](uint64_t offset, size_t bytes, uint8_t* destination) -> int64_t {
        ++provider_calls;
        if (provider_error == 1) return -1;
        if (provider_error == 2) return int64_t(bytes) + 1;
        return int64_t(fs.ReadBytes(offset, bytes, destination));
    }};
    std::map<uint32_t, Movie::Body> installed;
    CpuContext context{};

    explicit Fixture(const Input& input, bool selected) {
        Check(fs.Open(input.path.string()), "real ISO file provider opens the input");
        RecompAbi::BindMemory(&fast);
        LibcdvdBindings::Bind(SdkBindingManifest::Instance(), &files, nullptr, &model);
        Check(stream.Install(Capture(), Install(), selected) == 3, "three production control bindings installed");
        context.cpuRegs.GPR.r[28].UD[0] = kGp;
        context.cpuRegs.GPR.r[17].UD[0] = kPlayer;
        memory.Write32(kPlayer + 0x41f8, kBuffer);
    }
    ~Fixture() { RecompAbi::BindMemory(nullptr); }
    Movie::Capture Capture() {
        return [](uint32_t address) -> Movie::Body {
            if (address == Stream::kPrepare) return [](CpuContext& c, uint32_t) { LibcdvdBindings::StInit(c); };
            if (address == Stream::kOpen) return [](CpuContext& c, uint32_t) { LibcdvdBindings::StStart(c); };
            if (address == Stream::kClose) return [](CpuContext& c, uint32_t) { LibcdvdBindings::StStop(c); };
            return {};
        };
    }
    Movie::Installer Install() {
        return [this](uint32_t address, Movie::Body body) { installed[address] = std::move(body); return true; };
    }
    void Call(uint32_t address, uint32_t caller, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0) {
        context.cpuRegs.GPR.r[4].UD[0] = a0; context.cpuRegs.GPR.r[5].UD[0] = a1;
        context.cpuRegs.GPR.r[6].UD[0] = a2; context.cpuRegs.GPR.r[31].UD[0] = caller;
        installed.at(address)(context, address);
        Check(context.cpuRegs.GPR.r[2].SD[0] == 1, "retained and selected control acknowledgement matches");
    }
    void Open(uint32_t location) {
        memory.Write32(kGp - Movie::kFileLocationGp, location);
        memory.Write32(kGp - Movie::kFileSizeGp, 3 * Stream::kSectorBytes + 17);
        Call(Stream::kPrepare, 0x2ae9c4, 0x50, 5, (kBuffer + 63) & ~63u);
        Check(!stream.OwnsCursor() || !stream.CanCheckpoint(),
              "prepared cursor refuses a checkpoint before decoder Start owns playback");
        Call(Stream::kOpen, 0x2ae9ec, location, kGp - 0x74b8);
    }
    void Close() { Call(Stream::kClose, 0x2af608); }
    int32_t Read(uint8_t* destination, uint32_t sectors) {
        if (stream.OwnsCursor()) return stream.Read(destination, sectors);
        uint32_t error = 0;
        const uint32_t got = model.Read(sectors, destination, &error);
        return error ? -1 : int32_t(got);
    }
    int32_t Seek(uint32_t location) {
        return stream.OwnsCursor() ? stream.Seek(location) : int32_t(model.Seek(location));
    }
};

struct Trace {
    std::vector<int32_t> results;
    std::vector<uint8_t> bytes;
    uint64_t dispatches = 0;
};

Trace Run(const Input& input, bool selected) {
    Fixture f(input, selected);
    const auto before = LibcdvdBindings::StreamCalls();
    Trace trace;
    const auto read = [&](uint32_t count, uint32_t expected_location, int32_t expected_count) {
        std::vector<uint8_t> bytes(size_t(count) * Stream::kSectorBytes + 16, 0xa5);
        const int32_t got = f.Read(bytes.data(), count);
        trace.results.push_back(got);
        Check(got == expected_count, "shared cursor returns the authored number of sectors");
        if (got > 0) {
            const size_t n = size_t(got) * Stream::kSectorBytes;
            Check(std::equal(bytes.begin(), bytes.begin() + n,
                             input.bytes.begin() + size_t(expected_location) * Stream::kSectorBytes),
                  "callback/read-ahead bytes come from their one advancing cursor");
            trace.bytes.insert(trace.bytes.end(), bytes.begin(), bytes.begin() + n);
        }
        Check(std::all_of(bytes.end() - 16, bytes.end(), [](uint8_t b) { return b == 0xa5; }),
              "file service preserves the output buffer guard");
    };
    for (const uint32_t location : {32u, 48u, 32u}) {
        f.Open(location);
        Check(f.model.Active() != selected, "only the retained arm actually starts the stream model");
        Check(!selected || f.stream.Active(), "selected file lifetime is active");
        read(2, location, 2);       // MPEG read-ahead
        read(1, location + 2, 1);   // movie callback shares the cursor
        read(2, location + 3, 2);   // includes unchanged backing-sector padding
        Check(f.Seek(location) == 1, "source seek rewinds that same cursor");
        read(1, location, 1);
        Check(f.Seek(79) == 1, "position the file service near physical EOF");
        read(3, 79, 1); read(2, 80, 0);
        Check(!selected || f.stream.Active(), "EOF does not close movie ownership or queued playback");
        f.Close();
        read(1, 0, -1);
        Check(!f.model.Active() && !f.stream.Active(), "Close ends the active reader in each arm");
        Check(f.stream.CanCheckpoint(), "closed cursor has no pending file ownership to save");
    }
    trace.dispatches = LibcdvdBindings::StreamCalls() - before;
    Check(trace.dispatches == (selected ? 0u : 9u),
          "actual LibcdvdBindings stream dispatch counter is positive control / selected zero");
    if (selected) {
        const auto counts = f.stream.Counts();
        Check(counts.prepared == 3 && counts.opened == 3 && counts.closed == 3 &&
              counts.sectors == 21 && counts.original_calls == std::array<uint64_t, 3>{},
              "all source controls and completed selected reads belong to MovieStream");
    }
    return trace;
}

void Failures(const Input& input) {
    Fixture f(input, true);
    f.Open(32);
    std::vector<uint8_t> bytes(Stream::kSectorBytes, 0);
    f.provider_error = 1;
    Check(f.Read(bytes.data(), 1) == -1 && f.stream.Active(), "provider error preserves owned playback for source cleanup");
    f.provider_error = 2;
    Check(f.Read(bytes.data(), 1) == -1, "invalid provider byte count rejected");
    f.provider_error = 0;
    Check(f.Read(bytes.data(), 1) == 1 &&
          std::equal(bytes.begin(), bytes.end(), input.bytes.begin() + 32 * Stream::kSectorBytes),
          "errors do not advance the shared cursor");
    Check(f.Read(nullptr, 1) == -1 && f.Read(bytes.data(), UINT32_MAX) == -1,
          "null destination and unrepresentable count rejected before provider call");
    const auto calls = f.provider_calls;
    Check(f.Read(bytes.data(), 0) == 0 && f.provider_calls == calls, "zero read never calls provider or advances cursor");

    const auto before = LibcdvdBindings::StreamCalls();
    f.Call(Stream::kOpen, 0x1234, 48);
    Check(!f.stream.OwnsCursor() && f.model.Active(), "foreign start hands the file service to the actual retained owner");
    Check(f.Read(bytes.data(), 1) == 1 &&
          std::equal(bytes.begin(), bytes.end(), input.bytes.begin() + 48 * Stream::kSectorBytes),
          "foreign owner keeps its real read cursor");
    f.Close();
    Check(!f.model.Active() && LibcdvdBindings::StreamCalls() - before == 2,
          "known movie close still stops a foreign model start");
    f.Open(32);
    const std::vector<uint8_t> guest_before(f.ram.Data(), f.ram.Data() + f.ram.Size());
    f.stream.ResetAfterRestore();
    Check(!f.stream.OwnsCursor() && !f.stream.Active() && f.stream.CanCheckpoint() &&
          std::equal(guest_before.begin(), guest_before.end(), f.ram.Data()),
          "restore drops transient cursor without touching restored guest words");
    f.Open(48);
    Check(f.Read(bytes.data(), 1) == 1 &&
          std::equal(bytes.begin(), bytes.end(), input.bytes.begin() + 48 * Stream::kSectorBytes),
          "post-restore reopen cannot consume the previous movie");
    f.Close();

    Stream missing(f.memory, {});
    Check(missing.Install(f.Capture(), f.Install(), true) == 0, "selected install requires the real file provider");
    Stream transaction(f.memory, [](uint64_t, size_t, uint8_t*) -> int64_t { return 0; });
    unsigned installs = 0;
    const auto installer = f.Install();
    Check(transaction.Install(f.Capture(), [&](uint32_t a, Movie::Body b) {
        if (++installs == 2) return false;
        return installer(a, std::move(b));
    }, true) == 0, "partial selection fails atomically");
    const auto rollback = LibcdvdBindings::StreamCalls();
    f.Call(Stream::kPrepare, 0x2ae9c4, 0x50, 5, (Fixture::kBuffer + 63) & ~63u);
    Check(LibcdvdBindings::StreamCalls() == rollback + 1,
          "failed transaction restores the actual first SDK body");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--both-paths")) return 2;
    Input input;
    const auto model = Run(input, false), selected = Run(input, true);
    Check(model.results == selected.results && model.bytes == selected.bytes && !selected.bytes.empty(),
          "same NEW inputs: actual retained service and selected file bytes/results exact");
    Failures(input);
    std::printf("MovieStream: %u checks, %zu output bytes exact; actual CD stream dispatches %llu/%llu\n",
                checks, selected.bytes.size(), static_cast<unsigned long long>(model.dispatches),
                static_cast<unsigned long long>(selected.dispatches));
    return 0;
}
