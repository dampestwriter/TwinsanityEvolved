// LoaderRingCache: the title's windowed reader (FUN_002aac80) serves its records
// from a 128 KB callback-stream ring filled once per window of the file. This
// pin replays the coldboot request list of the loader object (fixtures/
// loader_requests_coldboot121.tsv: 906 requests, 900 of them the Default.rm2
// records of boundary 112) through LoaderFileIo with loader.ring_cache on, over
// the real disc image, and asserts the console's observables:
//   * one read record per fill boundary -- the console's fill chain of the
//     Default.rm2 stream (fs+0xc = the BD-absolute start of each fill:
//     0xfd4e6, 0x11d4e6, 0x13d4e6, 0x1741fe, ..., 0x25b3de, 0x27b3de, 0x29b3de,
//     0x2bb3de, 0x2db3de, 0x328f18, 0x348f18) instead of one record per request;
//   * the ring's bytes once the +0x1ddef8 fill landed equal the console's
//     (pcsx2_plus90_ee.bin) and, after the EOF fill, the console's rec159 image
//     (file[+0x24ba32..) over the previous window's tail);
//   * the staged words of the first fill (fs+0x10 = dest, fs+0x18 = 0xc) and of
//     the +0x1ddef8 fill (fs+0x18 = 0x144) match the console's;
//   * every request's destination holds the file's bytes.
// With `--ring-cache off` the same assertions run against the per-record fill
// (906 records, fs+0xc never set): the RED side of the pin.
//
//   test_loader_ring_cache <disc.iso> <requests.tsv> [--console-dir <dir>] [--ring-cache on|off]
//
// SKIP (77) without the disc image; the console images are optional evidence
// (their checks run only when --console-dir names a directory holding them).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

// The title's layout (the console's and NEW's agree).
static constexpr uint32_t kGp = 0x00400000;
static constexpr uint32_t kPathAddr = 0x00500000;
static constexpr uint32_t kSp = 0x00700000;
static constexpr uint32_t kFs = 0xbb3560;        // the loader object
static constexpr uint32_t kCb = 0xbab588;        // its callback stream {vtbl, +8 base, +0xc cursor, +0x10 cap}
static constexpr uint32_t kVtbl = 0x306b50;      // the stream's vtable (+0x10 this-adjust, +0x14 copy-out)
static constexpr uint32_t kCopyOutFn = 0x2b5dd8; // FUN_002b5dd8
static constexpr uint32_t kRing = 0x948540;
static constexpr uint32_t kCap = 0x20000;
static constexpr uint32_t kRm2Base = 0xfd4e6, kRm2Size = 0x261a19;
static constexpr uint32_t kBhSize = 27376;

struct Req { uint32_t handle, off, size, dest, sync, pos; };

static std::vector<Req> LoadRequests(const char* path) {
    std::vector<Req> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        unsigned long tick, handle, off, size, dest, sync, pos;
        if (std::sscanf(line.c_str(), "%lu %lu %lx %lx %lx %lu %lx", &tick, &handle, &off, &size, &dest, &sync, &pos) != 7) continue;
        out.push_back(Req{static_cast<uint32_t>(handle), static_cast<uint32_t>(off), static_cast<uint32_t>(size),
                          static_cast<uint32_t>(dest), static_cast<uint32_t>(sync), static_cast<uint32_t>(pos)});
    }
    return out;
}

static bool ReadImageSlice(const std::string& path, uint32_t addr, uint32_t n, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.assign(n, 0);
    const bool ok = std::fseek(f, static_cast<long>(addr), SEEK_SET) == 0 && std::fread(out.data(), 1, n, f) == n;
    std::fclose(f);
    return ok;
}

static bool RamEquals(uint32_t addr, const std::vector<uint8_t>& ref, uint32_t lo, uint32_t hi, uint32_t* first_diff) {
    for (uint32_t i = lo; i < hi; ++i) {
        if (memory::read<uint8_t>(addr + i) != ref[i]) { if (first_diff) *first_diff = i; return false; }
    }
    return true;
}

static void Prep(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[28].UD[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UD[0] = kSp;
}

static int32_t OpenPath(CpuContext& ctx, const char* path) {
    for (uint32_t i = 0; i <= std::strlen(path); ++i) memory::write<uint8_t>(kPathAddr + i, static_cast<uint8_t>(path[i]));
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kPathAddr;
    LoaderFileIo::Open(ctx);
    return ctx.cpuRegs.GPR.r[2].SL[0];
}

int main(int argc, char** argv) {
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink sink;
    static MmioRouter mmio(sink);
    static FastMem fm(ram, map, mmio);
    RecompAbi::BindMemory(&fm);

    std::string console_dir;
    bool ring_cache = true;
    uint32_t verbose = 0;   // --verbose N: print the first N requests' words
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--console-dir") && i + 1 < argc) console_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--ring-cache") && i + 1 < argc) ring_cache = std::strcmp(argv[++i], "off") != 0;
        else if (!std::strcmp(argv[i], "--verbose") && i + 1 < argc) verbose = static_cast<uint32_t>(std::atoi(argv[++i]));
    }
    IsoFs fs;
    if (argc < 3 || !fs.Open(argv[1])) {
        std::printf("SKIP no disc image\n");
        return 77;
    }
    const std::vector<Req> reqs = LoadRequests(argv[2]);
    if (reqs.size() != 906) { std::printf("FAIL request list: %zu requests (expected 906)\n", reqs.size()); return 1; }
    ReadEngine engine(fs);

    // The seam: no LoaderModel (a delivery lands inside the issue), the title's two
    // bodies stood in -- the poll answers done, the copy-out does what FUN_002b5dd8 does.
    LoaderFileIo::Deps deps;
    deps.engine = &engine;
    deps.ring_cache = ring_cache;
    LoaderFileIo::Layout layout;
    layout.stream_open_cmd_addr = 0x00530000;
    layout.stream_batch_count_addr = 0x00530010;
    deps.call = [&layout](uint32_t fn, CpuContext& c) {
        if (fn == layout.fn_poll) { c.cpuRegs.GPR.r[2].UD[0] = 0; return; }
        if (fn == kCopyOutFn) {   // memcpy(dest, cursor, n); cursor += n
            const uint32_t cb = c.cpuRegs.GPR.r[4].UL[0], dest = c.cpuRegs.GPR.r[5].UL[0], n = c.cpuRegs.GPR.r[6].UL[0];
            const uint32_t cursor = memory::read<uint32_t>(cb + 0xc);
            for (uint32_t i = 0; i < n; ++i) memory::write<uint8_t>(dest + i, memory::read<uint8_t>(cursor + i));
            memory::write<uint32_t>(cb + 0xc, cursor + n);
            c.cpuRegs.GPR.r[2].UD[0] = n;
            return;
        }
        c.cpuRegs.GPR.r[2].UD[0] = 0;   // the STREAM client bodies (register / ship): nothing to do here
    };
    memory::write<int32_t>(layout.stream_open_cmd_addr, -1);
    memory::write<uint16_t>(layout.stream_batch_count_addr, 0);
    LoaderFileIo::Bind(deps, layout);
    LoaderFileIo::ResetForTest();

    CpuContext ctx{};
    memory::write<int32_t>(kGp - 0x74d8, 0);
    const int32_t h_bh = OpenPath(ctx, "crash6\\crash.bh");
    const int32_t h_bd = OpenPath(ctx, "crash6\\crash.bd");
    CHECK(h_bh == 0 && h_bd == 1, "handles_0_bh_1_bd");
    IsoFs::Entry bd;
    if (!fs.Find("CRASH6/CRASH.BD", bd)) { std::printf("FAIL CRASH.BD not on the disc\n"); return 1; }
    IsoFs::Entry bh;
    if (!fs.Find("CRASH6/CRASH.BH", bh)) { std::printf("FAIL CRASH.BH not on the disc\n"); return 1; }

    // The stream object and its vtable.
    memory::write<uint32_t>(kCb + 0x0, kVtbl);
    memory::write<uint32_t>(kCb + 0x8, kRing);
    memory::write<uint32_t>(kCb + 0xc, kRing);
    memory::write<uint32_t>(kCb + 0x10, kCap);
    memory::write<uint16_t>(kVtbl + 0x10, 0);
    memory::write<uint32_t>(kVtbl + 0x14, kCopyOutFn);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsCbObj, kCb);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x300);
    for (uint32_t i = 0; i < kCap; ++i) memory::write<uint8_t>(kRing + i, 0xEE);   // the previous occupant

    // The console's fill chain of the Default.rm2 stream (fs+0xc at each fill; the
    // measured values of the console trace, the intermediate ones from the same rule).
    static const uint32_t kConsoleChain[] = {0xfd4e6, 0x11d4e6, 0x13d4e6, 0x1741fe, 0x196d92, 0x1b6d92, 0x1d6d92, 0x1fb3de,
                                             0x21b3de, 0x23b3de, 0x25b3de, 0x27b3de, 0x29b3de, 0x2bb3de, 0x2db3de, 0x328f18,
                                             0x348f18};
    static const uint32_t kMeasured[] = {0xfd4e6, 0x11d4e6, 0x13d4e6, 0x1741fe, 0x25b3de, 0x27b3de, 0x29b3de, 0x2bb3de, 0x2db3de,
                                         0x328f18, 0x348f18};

    std::vector<uint8_t> plus90, rec159;
    const bool have_console = !console_dir.empty() &&
                              ReadImageSlice(console_dir + "/pcsx2_plus90_ee.bin", kRing, kCap, plus90) &&
                              ReadImageSlice(console_dir + "/pcsx2_rec159_ee.bin", kRing, kCap, rec159);
    if (!console_dir.empty() && !have_console) std::printf("note: console images not readable under %s\n", console_dir.c_str());

    std::vector<uint32_t> chain;             // fs+0xc after every request that changed it
    uint32_t prev_fill_start = 0;
    uint32_t cur_base = 0xffffffffu;
    bool first_rm2_staging_ok = false, plus90_staging_ok = false, plus90_ring_ok = true, dest_ok = true, status_ok = true;
    uint32_t dest_bad = 0, plus90_diff = 0;
    std::vector<uint8_t> file_bytes;
    uint32_t n = 0;
    for (const Req& r : reqs) {
        ++n;
        const uint32_t fbase = r.pos - r.off;
        if (fbase != cur_base) {
            // The title's stream reset between files (FUN_002acfe0: fs+0xc = 0, flags &= ~0x50000; cursor rewound).
            cur_base = fbase;
            memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, static_cast<int32_t>(r.handle));
            memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, fbase);
            memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, r.handle == 0 ? kBhSize : (fbase == kRm2Base ? kRm2Size : 0x100000u));
            memory::write<uint32_t>(kFs + LoaderFileIo::kFsRingStart, 0);
            memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, memory::read<uint32_t>(kFs) & ~0x50000u);
            memory::write<uint32_t>(kCb + 0xc, kRing);
            prev_fill_start = 0;
        }
        for (uint32_t i = 0; i < r.size; ++i) memory::write<uint8_t>(r.dest + i, 0xAB);
        Prep(ctx);
        ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = r.off; ctx.cpuRegs.GPR.r[6].UD[0] = r.size;
        ctx.cpuRegs.GPR.r[7].UD[0] = r.dest; ctx.cpuRegs.GPR.r[8].UD[0] = r.sync; ctx.cpuRegs.GPR.r[9].UD[0] = 0x00520000;
        LoaderFileIo::WindowedRead(ctx);
        if (memory::read<uint32_t>(0x00520000) != r.size) status_ok = false;
        const uint32_t fill_start = memory::read<uint32_t>(kFs + LoaderFileIo::kFsRingStart);
        if (n <= verbose)
            std::printf("  #%u off=0x%x size=0x%x pos=0x%x -> v0=%u flags=0x%x fs+0xc=0x%x cursor=+0x%x fs+0x10=0x%x fs+0x18=0x%x\n", n, r.off,
                        r.size, r.pos, ctx.cpuRegs.GPR.r[2].UL[0], memory::read<uint32_t>(kFs), fill_start,
                        memory::read<uint32_t>(kCb + 0xc) - kRing, memory::read<uint32_t>(kFs + 0x10), memory::read<uint32_t>(kFs + 0x18));
        if (fill_start != prev_fill_start) {
            chain.push_back(fill_start);
            prev_fill_start = fill_start;
            if (fill_start == kRm2Base)   // the console's +40 state: the first fill in flight, the window staged
                first_rm2_staging_ok = memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == 0x102cf00 &&
                                       memory::read<uint32_t>(kFs + LoaderFileIo::kFsSize) == 0xc &&
                                       (memory::read<uint32_t>(kFs) & (LoaderFileIo::kFlagRingValid | LoaderFileIo::kFlagReadPending)) ==
                                           (LoaderFileIo::kFlagRingValid | LoaderFileIo::kFlagReadPending);
            if (fill_start == 0x2db3de)   // the console's +70 trace, stop 7: the +0x1ddef8 fill staged with s6 = 0x144
                plus90_staging_ok = memory::read<uint32_t>(kFs + LoaderFileIo::kFsSize) == 0x144;
        }
        // The title waits for every asynchronous record (FUN_002ad1a0): the completion
        // copy lands the staged window and lowers the flag.
        if (r.sync == 0 && (memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending)) {
            Prep(ctx);
            ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
            LoaderFileIo::WaitReadComplete(ctx);
        }
        if (fill_start == 0x2db3de && have_console && plus90_ring_ok)   // the console's +90/+100 ring: file[+0x1ddef8 .. +0x1fdef8)
            plus90_ring_ok = RamEquals(kRing, plus90, 0, kCap, &plus90_diff);
        // The destination holds the file's bytes.
        const uint32_t lba = r.handle == 0 ? bh.lba : bd.lba;
        const uint64_t byte = static_cast<uint64_t>(lba) * IsoFs::kSectorSize + r.pos;
        const uint32_t first = static_cast<uint32_t>(byte / IsoFs::kSectorSize), skew = static_cast<uint32_t>(byte % IsoFs::kSectorSize);
        const uint32_t sectors = (skew + r.size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
        file_bytes.assign(static_cast<size_t>(sectors) * IsoFs::kSectorSize, 0);
        engine.ReadSectors(first, sectors, file_bytes.data());
        for (uint32_t i = 0; i < r.size && dest_ok; ++i)
            if (memory::read<uint8_t>(r.dest + i) != file_bytes[skew + i]) { dest_ok = false; dest_bad = n; }
    }
    const LoaderFileIo::Census& c = LoaderFileIo::Stats();
    const uint64_t records = c.async_issued + c.sync_reads;
    std::printf("ring_cache=%s requests=%zu records=%llu hits=%llu straddles=%llu misses=%llu fills=%llu direct=%llu\n",
                ring_cache ? "on" : "off", reqs.size(), static_cast<unsigned long long>(records),
                static_cast<unsigned long long>(c.ring_hits), static_cast<unsigned long long>(c.ring_straddles),
                static_cast<unsigned long long>(c.ring_misses), static_cast<unsigned long long>(c.ring_fills),
                static_cast<unsigned long long>(c.async_direct));
    std::printf("fs+0xc chain (%zu):", chain.size());
    for (uint32_t v : chain) std::printf(" 0x%x", v);
    std::printf("\n");

    CHECK(status_ok, "status_word_is_the_request_size");
    CHECK(dest_ok, "every_destination_holds_the_file_bytes");
    if (!dest_ok) std::printf("  first bad destination: request #%u\n", dest_bad);
    // One record per fill boundary: 19 fills (CRASH.BH, the 0x3b2ce0 segment, 17 Default.rm2 windows), no direct read.
    CHECK(records == 19, "one_record_per_fill_boundary_19_not_906");
    CHECK(c.ring_fills == 19 && c.async_direct == 0, "every_record_is_a_ring_fill");
    std::vector<uint32_t> rm2_chain;
    for (uint32_t v : chain) if (v >= kRm2Base && v < kRm2Base + kRm2Size) rm2_chain.push_back(v);
    bool chain_ok = rm2_chain.size() == sizeof(kConsoleChain) / sizeof(kConsoleChain[0]);
    for (size_t i = 0; chain_ok && i < rm2_chain.size(); ++i) chain_ok = rm2_chain[i] == kConsoleChain[i];
    CHECK(chain_ok, "default_rm2_fill_chain_is_the_console_s");
    bool measured_ok = true;   // the eleven values the console trace measured, in order
    size_t j = 0;
    for (uint32_t v : rm2_chain) if (j < sizeof(kMeasured) / sizeof(kMeasured[0]) && v == kMeasured[j]) ++j;
    measured_ok = j == sizeof(kMeasured) / sizeof(kMeasured[0]);
    CHECK(measured_ok, "the_eleven_measured_fs0xc_values_in_order");
    CHECK(first_rm2_staging_ok, "first_fill_staged_fs10_0x102cf00_fs18_0xc_flags_0x50000");
    CHECK(plus90_staging_ok, "plus90_fill_staged_fs18_0x144");
    // After the EOF fill: the ring is file[+0x24ba32 .. EOF) (0x15fe7 bytes) over the previous window's tail.
    {
        std::vector<uint8_t> rm2_tail(kCap, 0);
        const uint64_t byte = static_cast<uint64_t>(bd.lba) * IsoFs::kSectorSize + 0x348f18;
        const uint32_t first = static_cast<uint32_t>(byte / IsoFs::kSectorSize), skew = static_cast<uint32_t>(byte % IsoFs::kSectorSize);
        std::vector<uint8_t> raw((kCap / IsoFs::kSectorSize + 2) * IsoFs::kSectorSize, 0);
        engine.ReadSectors(first, static_cast<uint32_t>(raw.size() / IsoFs::kSectorSize), raw.data());
        std::memcpy(rm2_tail.data(), raw.data() + skew, kCap);
        uint32_t d = 0;
        const bool eof_fill_ok = RamEquals(kRing, rm2_tail, 0, 0x15fe7, &d);
        CHECK(eof_fill_ok, "eof_fill_0x15fe7_bytes_of_file_plus_0x24ba32_at_base");
        // Beyond the EOF fill the ring keeps the +0x22ba32 window's tail: file[+0x22ba32 + 0x15fee ..).
        std::vector<uint8_t> prev(kCap, 0);
        const uint64_t byte2 = static_cast<uint64_t>(bd.lba) * IsoFs::kSectorSize + 0x328f18;
        const uint32_t first2 = static_cast<uint32_t>(byte2 / IsoFs::kSectorSize), skew2 = static_cast<uint32_t>(byte2 % IsoFs::kSectorSize);
        engine.ReadSectors(first2, static_cast<uint32_t>(raw.size() / IsoFs::kSectorSize), raw.data());
        std::memcpy(prev.data(), raw.data() + skew2, kCap);
        CHECK(RamEquals(kRing, prev, 0x15fee, kCap, &d), "ring_past_the_eof_fill_keeps_the_previous_window");
    }
    if (have_console) {
        CHECK(plus90_ring_ok, "ring_after_the_plus_0x1ddef8_fill_equals_pcsx2_plus90");
        if (!plus90_ring_ok) std::printf("  first differing ring byte: +0x%x\n", plus90_diff);
        uint32_t d = 0;
        CHECK(RamEquals(kRing, rec159, 0, 0x15fe7, &d), "ring_eof_fill_equals_pcsx2_rec159_0x948540_0x95e527");
        CHECK(RamEquals(kRing, rec159, 0x15fee, kCap, &d), "ring_tail_equals_pcsx2_rec159_0x95e52e_0x968540");
    } else {
        std::printf("note: console-image checks not run (no --console-dir)\n");
    }
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
