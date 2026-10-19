// LoaderSeams: native bank-header lifecycle over a synthetic disc, and the
// loader file-IO bodies over the real disc: open -> size -> windowed read (sync,
// and async through the LoaderModel with delivery at the slot-4 boundary) ->
// wait -> close, with
// byte-exact results against the ReadEngine's own sector reads.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/core/config/config.h"
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

static constexpr uint32_t kGp = 0x00400000;        // a test $gp; the counter word sits at gp - 0x74d8
static constexpr uint32_t kPathAddr = 0x00500000;
static constexpr uint32_t kFs = 0x00510000;        // the loader's fs object
static constexpr uint32_t kCb = 0x00511000;        // its callback stream object {+8 base, +0xc cursor, +0x10 cap}
static constexpr uint32_t kRing = 0x00580000;      // the stream's ring buffer
static constexpr uint32_t kDest = 0x00600000;
static constexpr uint32_t kStatus = 0x00520000;
static constexpr uint32_t kSp = 0x00700000;

static void Prep(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[28].UD[0] = kGp;
    ctx.cpuRegs.GPR.r[29].UD[0] = kSp;
    ctx.cpuRegs.GPR.r[2].UD[0] = 0xDEADBEEFull;
}

static bool BytesEqual(uint32_t addr, const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); ++i)
        if (memory::read<uint8_t>(addr + static_cast<uint32_t>(i)) != bytes[i]) return false;
    return true;
}

static std::string GuestString(uint32_t addr) {
    std::string text;
    for (uint32_t i = 0; i < 300; ++i) {
        const char c = static_cast<char>(memory::read<uint8_t>(addr + i));
        if (c == 0) break;
        text += c;
    }
    return text;
}

static void WriteString(uint32_t addr, const std::string& text) {
    for (size_t i = 0; i <= text.size(); ++i)
        memory::write<uint8_t>(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(text.c_str()[i]));
}

static void IsoWord(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(value >> (i * 8));
        p[7 - i] = p[i];
    }
}

static size_t IsoRecord(uint8_t* p, const std::string& name, uint32_t lba, uint32_t size, bool directory = false) {
    const size_t n = (33 + name.size() + 1) & ~size_t(1);
    p[0] = static_cast<uint8_t>(n);
    IsoWord(p + 2, lba);
    IsoWord(p + 10, size);
    p[25] = directory ? 2 : 0;
    p[28] = p[31] = 1;
    p[32] = static_cast<uint8_t>(name.size());
    std::memcpy(p + 33, name.data(), name.size());
    return n;
}

// A two-sector header with a non-sector-aligned EOF, a samples file, missing
// and empty headers, and a truncated extent. No game image or IOP is needed.
static std::string MakeBankDisc(const std::vector<uint8_t>& header) {
    constexpr uint32_t sector = IsoFs::kSectorSize;
    std::vector<uint8_t> image(24 * sector, 0);
    uint8_t* pvd = image.data() + 16 * sector;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    IsoWord(pvd + 80, 24);
    IsoRecord(pvd + 156, std::string(1, '\0'), 18, sector, true);
    uint8_t* end = image.data() + 17 * sector;
    end[0] = 255;
    std::memcpy(end + 1, "CD001", 5);
    end[6] = 1;
    uint8_t* dir = image.data() + 18 * sector;
    size_t at = IsoRecord(dir, std::string(1, '\0'), 18, sector, true);
    at += IsoRecord(dir + at, std::string(1, '\1'), 18, sector, true);
    at += IsoRecord(dir + at, "BANK.MH;1", 20, static_cast<uint32_t>(header.size()));
    at += IsoRecord(dir + at, "BANK.MB;1", 22, 79);
    at += IsoRecord(dir + at, "MISSING.MB;1", 22, 79);
    at += IsoRecord(dir + at, "EMPTY.MH;1", 20, 0);
    at += IsoRecord(dir + at, "EMPTY.MB;1", 22, 79);
    at += IsoRecord(dir + at, "SHORT.MH;1", 23, static_cast<uint32_t>(header.size()));
    at += IsoRecord(dir + at, "SHORT.MB;1", 22, 79);
    at += IsoRecord(dir + at, "HUGE.MH;1", 20, 0x02000000u);
    at += IsoRecord(dir + at, "HUGE.MB;1", 22, 79);
    at += IsoRecord(dir + at, "DIRECTORY.MH;1", 18, sector, true);
    at += IsoRecord(dir + at, "DIRECTORY.MB;1", 22, 79);
    IsoRecord(dir + at, "HEADERONLY.MH;1", 20, static_cast<uint32_t>(header.size()));
    std::memcpy(image.data() + 20 * sector, header.data(), header.size());
    for (uint32_t i = 0; i < 79; ++i) image[22 * sector + i] = static_cast<uint8_t>(i + 17);

    std::string path = (std::filesystem::temp_directory_path() / "ps2r_bank_header_XXXXXX").string();
    const int fd = ::mkstemp(path.data());
    if (fd < 0) return {};
    FILE* file = ::fdopen(fd, "wb");
    if (!file) { ::close(fd); std::remove(path.c_str()); return {}; }
    const bool wrote = std::fwrite(image.data(), 1, image.size(), file) == image.size();
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) { std::remove(path.c_str()); return {}; }
    return path;
}

static void TestBankHeaders() {
    constexpr uint32_t bank = 0x00540000, heap = 0x00541000, old_header = 0x00542000;
    constexpr uint32_t first_header = 0x00610000, second_header = 0x00620000;
    std::vector<uint8_t> expected(IsoFs::kSectorSize + 37);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>((i * 29 + 7) ^ (i >> 3));
    const std::string image = MakeBankDisc(expected);
    IsoFs disc;
    const bool opened = !image.empty() && disc.Open(image);
    CHECK(opened, "bank_fixture_open");
    if (!opened) { if (!image.empty()) std::remove(image.c_str()); return; }
    ReadEngine engine(disc);
    LoaderFileIo::Layout layout;
    LoaderFileIo::Deps deps;
    deps.engine = &engine;
    uint32_t allocation = first_header, expected_published = first_header;
    int32_t old_samples = 7;
    std::string expected_wire = "cdrom0:\\BANK.MB";
    std::vector<uint32_t> calls, freed;
    std::vector<int32_t> registered;
    uint32_t allocation_calls = 0, module_pumps = 0;
    deps.pump_module = [&](CpuContext&) { ++module_pumps; };
    deps.call = [&](uint32_t fn, CpuContext& c) {
        calls.push_back(fn);
        uint32_t result = 0;
        if (fn == layout.fn_get_heap) {
            result = heap;
        } else if (fn == layout.fn_free) {
            CHECK(c.cpuRegs.GPR.r[4].UL[0] == heap, "bank_free_uses_guest_heap");
            freed.push_back(c.cpuRegs.GPR.r[5].UL[0]);
        } else if (fn == layout.fn_alloc_aligned) {
            ++allocation_calls;
            CHECK(memory::read<uint32_t>(bank) == 0, "bank_not_published_during_allocation");
            CHECK(c.cpuRegs.GPR.r[4].UL[0] == heap && c.cpuRegs.GPR.r[5].UL[0] == expected.size() &&
                      c.cpuRegs.GPR.r[6].UL[0] == 64, "bank_allocation_size_and_alignment");
            result = allocation;
        } else {
            CHECK(fn == layout.fn_unregister_file || fn == layout.fn_ship || fn == layout.fn_register_file,
                  "bank_only_sample_commands_reach_guest_stream");
            CHECK(memory::read<uint32_t>(bank) == expected_published, "bank_published_before_sample_operations");
            if (expected_published) CHECK(BytesEqual(expected_published, expected), "bank_published_bytes_complete");
            if (fn == layout.fn_unregister_file) {
                CHECK(c.cpuRegs.GPR.r[4].SL[0] == old_samples, "bank_closes_previous_sample_handle");
            } else if (fn == layout.fn_ship) {
                CHECK(c.cpuRegs.GPR.r[4].UL[0] == 0, "bank_sample_close_keeps_blocking_ship");
            } else if (fn == layout.fn_register_file) {
                CHECK(GuestString(c.cpuRegs.GPR.r[5].UL[0]) == expected_wire && c.cpuRegs.GPR.r[6].UL[0] == 0,
                      "bank_registers_only_mb_path");
                registered.push_back(c.cpuRegs.GPR.r[4].SL[0]);
            }
        }
        // A real nested call may destroy argument registers and use $ra.
        for (unsigned reg = 4; reg <= 9; ++reg) c.cpuRegs.GPR.r[reg].UD[0] = 0xBAD00000u + reg;
        c.cpuRegs.GPR.r[31].UD[0] = 0xBAD0001Fu;
        c.cpuRegs.GPR.r[2].UD[0] = result;
    };
    LoaderFileIo::Bind(deps, layout);
    LoaderFileIo::ResetForTest();
    memory::write<uint32_t>(bank - 4, 0xABCD1234);
    memory::write<uint32_t>(bank + 8, 0x5678ABCD);
    memory::write<uint32_t>(bank, old_header);
    memory::write<int32_t>(bank + 4, old_samples);
    memory::write<int32_t>(kGp + layout.counter_gp_offset, 40);
    auto load = [&](const char* name) {
        calls.clear();
        freed.clear();
        registered.clear();
        old_samples = memory::read<int32_t>(bank + 4);
        WriteString(kPathAddr, name);
        if (allocation >= 0x10000 && allocation <= 0x02000000u - expected.size() - 32)
            for (uint32_t i = 0; i < expected.size() + 32; ++i)
                memory::write<uint8_t>(allocation - 16 + i, 0xCE);
        CpuContext c{};
        Prep(c);
        c.cpuRegs.GPR.r[4].UD[0] = bank;
        c.cpuRegs.GPR.r[5].UD[0] = kPathAddr;
        c.cpuRegs.GPR.r[16].UD[0] = 0x123456789ABCDEF0ull;
        c.cpuRegs.GPR.r[31].UD[0] = 0x11223344u;
        LoaderFileIo::LoadSoundBank(c);
        CHECK(c.cpuRegs.GPR.r[29].UL[0] == kSp && c.cpuRegs.GPR.r[31].UD[0] == 0x11223344u &&
                  c.cpuRegs.GPR.r[16].UD[0] == 0x123456789ABCDEF0ull, "bank_preserves_callers_stack_and_saved_registers");
        CHECK(memory::read<uint32_t>(bank - 4) == 0xABCD1234 && memory::read<uint32_t>(bank + 8) == 0x5678ABCD,
              "bank_object_guards_intact");
    };

    load("bAnK");
    CHECK(BytesEqual(first_header, expected), "bank_header_exact_across_sector_and_partial_eof");
    CHECK(memory::read<uint8_t>(first_header - 1) == 0xCE &&
              memory::read<uint8_t>(first_header + static_cast<uint32_t>(expected.size())) == 0xCE, "bank_destination_guards_intact");
    CHECK(freed == std::vector<uint32_t>{old_header}, "bank_old_header_freed_once");
    CHECK((calls == std::vector<uint32_t>{layout.fn_get_heap, layout.fn_free, layout.fn_get_heap, layout.fn_alloc_aligned,
                                        layout.fn_unregister_file, layout.fn_ship, layout.fn_register_file}),
          "bank_lifecycle_free_allocate_then_sample_close_open");
    CHECK(registered == std::vector<int32_t>{41} && memory::read<int32_t>(bank + 4) == 41 &&
              memory::read<int32_t>(kGp + layout.counter_gp_offset) == 42, "bank_preserves_sample_file_number");
    auto count = LoaderFileIo::CountBankHeaders();
    CHECK(count.header_calls == 1 && count.sound_bank_headers == 1 && count.header_bytes == expected.size() &&
              count.header_misses == 0 && count.header_failures == 0, "bank_header_success_census");

    allocation = expected_published = second_header;
    load("bank");
    CHECK(freed == std::vector<uint32_t>{first_header} && BytesEqual(second_header, expected),
          "bank_replacement_frees_previous_header");
    CHECK(registered == std::vector<int32_t>{43} && memory::read<int32_t>(bank + 4) == 43,
          "bank_replacement_updates_sample_handle");

    expected_published = 0;
    expected_wire = "cdrom0:\\MISSING.MB";
    const uint32_t allocations_before_misses = allocation_calls;
    load("missing");
    CHECK(freed == std::vector<uint32_t>{second_header} && memory::read<uint32_t>(bank) == 0 &&
              memory::read<int32_t>(bank + 4) == 45, "bank_missing_header_still_replaces_samples");
    expected_wire = "cdrom0:\\EMPTY.MB";
    load("empty");
    CHECK(freed.empty() && allocation_calls == allocations_before_misses && memory::read<uint32_t>(bank) == 0,
          "bank_empty_and_missing_headers_do_not_allocate");
    count = LoaderFileIo::CountBankHeaders();
    CHECK(count.sound_bank_headers == 2 && count.header_misses == 2 && count.header_failures == 0,
          "bank_missing_headers_are_distinct_from_failures");

    allocation = 0;
    expected_wire = "cdrom0:\\BANK.MB";
    load("bank");
    CHECK(memory::read<uint32_t>(bank) == 0 && freed.empty() && memory::read<int32_t>(bank + 4) == 49,
          "bank_allocation_failure_keeps_null_header_and_sample_lifecycle");
    allocation = first_header;
    expected_wire = "cdrom0:\\SHORT.MB";
    load("short");
    CHECK(memory::read<uint32_t>(bank) == 0 && freed == std::vector<uint32_t>{first_header} &&
              memory::read<uint8_t>(first_header) == 0xCE, "bank_short_read_frees_allocation_without_publishing_or_copying");
    count = LoaderFileIo::CountBankHeaders();
    CHECK(count.header_calls == 6 && count.sound_bank_headers == 2 && count.header_bytes == 2 * expected.size() &&
              count.header_misses == 2 && count.header_failures == 2,
          "bank_failure_census_never_counts_partial_bytes");

    expected_published = first_header;
    expected_wire = "cdrom0:\\HEADERONLY.MB";
    load("headeronly");
    CHECK(BytesEqual(first_header, expected) && memory::read<int32_t>(bank + 4) == -1,
          "bank_missing_samples_keep_completed_header_and_open_failure");
    expected_wire = "cdrom0:\\BANK.MB";
    allocation = expected_published = second_header;
    load("bank");
    CHECK((calls == std::vector<uint32_t>{layout.fn_get_heap, layout.fn_free, layout.fn_get_heap, layout.fn_alloc_aligned,
                                        layout.fn_register_file}),
          "bank_no_previous_samples_skips_close_and_ship");
    CHECK(registered == std::vector<int32_t>{55} && memory::read<int32_t>(bank + 4) == 55,
          "bank_samples_open_after_missing_samples");
    CHECK(module_pumps == 0 && LoaderFileIo::Stats().waits == 0 && LoaderFileIo::Stats().waits_module == 0 &&
              engine.OpenFiles() == 0, "bank_header_has_no_stream_wait_or_iop_worker");
    count = LoaderFileIo::CountBankHeaders();
    CHECK(count.header_calls == 8 && count.sound_bank_headers == 4 && count.header_bytes == 4 * expected.size() &&
              count.header_misses == 2 && count.header_failures == 2,
          "bank_final_census");

    // Failed allocations/extents must not turn into visible partial headers.
    // Each case still consumes its header ID and runs the ordinary .mb lifecycle.
    expected_published = 0;
    allocation = first_header + 1;
    load("bank");
    CHECK((freed == std::vector<uint32_t>{second_header, allocation}) &&
              memory::read<uint32_t>(bank) == 0 && memory::read<uint8_t>(allocation) == 0xCE &&
              registered == std::vector<int32_t>{57}, "bank_misaligned_allocation_freed_without_copy_or_id_loss");
    allocation = 0x01ffffc0;
    memory::write<uint32_t>(allocation, 0x1234ABCD);
    load("bank");
    CHECK((freed == std::vector<uint32_t>{allocation}) && memory::read<uint32_t>(bank) == 0 &&
              memory::read<uint32_t>(allocation) == 0x1234ABCD &&
              registered == std::vector<int32_t>{59}, "bank_range_overflow_freed_without_partial_copy");
    const uint32_t allocations_before_invalid_extents = allocation_calls;
    expected_wire = "cdrom0:\\HUGE.MB";
    load("huge");
    CHECK(freed.empty() && allocation_calls == allocations_before_invalid_extents &&
              memory::read<uint32_t>(bank) == 0 && registered == std::vector<int32_t>{61},
          "bank_oversized_extent_rejected_before_allocation");
    expected_wire = "cdrom0:\\DIRECTORY.MB";
    load("directory");
    CHECK(freed.empty() && allocation_calls == allocations_before_invalid_extents &&
              memory::read<uint32_t>(bank) == 0 && registered == std::vector<int32_t>{63},
          "bank_directory_is_not_header_data");

    deps.engine = nullptr;
    LoaderFileIo::Bind(deps, layout);
    expected_wire = "cdrom0:\\BANK.MB";
    load("bank");
    CHECK(freed.empty() && allocation_calls == allocations_before_invalid_extents &&
              memory::read<uint32_t>(bank) == 0 && memory::read<int32_t>(bank + 4) == -1 &&
              registered == std::vector<int32_t>{65}, "bank_missing_disc_preserves_sample_failure_and_ids");
    CHECK(LoaderFileIo::Stats().opens == 13 && module_pumps == 0 && LoaderFileIo::Stats().waits == 0 &&
              engine.OpenFiles() == 0, "bank_only_sample_opens_use_loader_service");

    // Without a guest allocator/caller, fail before changing ownership or IDs.
    LoaderFileIo::Bind({});
    const uint32_t counter_before_unbound = memory::read<uint32_t>(kGp + layout.counter_gp_offset);
    memory::write<uint32_t>(bank, old_header);
    memory::write<int32_t>(bank + 4, 7);
    CpuContext unbound{};
    Prep(unbound);
    unbound.cpuRegs.GPR.r[4].UD[0] = bank;
    unbound.cpuRegs.GPR.r[5].UD[0] = kPathAddr;
    LoaderFileIo::LoadSoundBank(unbound);
    CHECK(memory::read<uint32_t>(bank) == old_header && memory::read<int32_t>(bank + 4) == 7 &&
              memory::read<uint32_t>(kGp + layout.counter_gp_offset) == counter_before_unbound &&
              unbound.cpuRegs.GPR.r[29].UL[0] == kSp, "bank_unbound_caller_preserves_existing_ownership");
    count = LoaderFileIo::CountBankHeaders();
    CHECK(count.header_calls == 14 && count.sound_bank_headers == 4 && count.header_bytes == 4 * expected.size() &&
              count.header_misses == 4 && count.header_failures == 6, "bank_rejected_requests_never_count_bytes");
    LoaderFileIo::ResetForTest();
    disc.Close();
    std::remove(image.c_str());
}

int main(int argc, char** argv) {
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink sink;
    static MmioRouter mmio(sink);
    static FastMem fm(ram, map, mmio);
    RecompAbi::BindMemory(&fm);
    TestBankHeaders();

    IsoFs fs;
    if (argc < 2 || !fs.Open(argv[1])) {
        std::printf("SKIP real-disc loader cases (no disc image); native bank tests %s\n", g_fail ? "FAILED" : "PASSED");
        return g_fail ? 1 : 0;
    }
    ReadEngine engine(fs);
    ps2r::CdvdConfig cdvd_cfg;
    cdvd_cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Boundary;   // this case tests the boundary model (the default is poll since gate S4)
    DeliveryMeter meter(cdvd_cfg);
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    uint64_t tick = 10;
    std::vector<uint32_t> calls;
    LoaderFileIo::Deps deps;
    deps.engine = &engine;
    deps.model = &model;
    deps.ring_cache = false;   // this case tests the per-record seam (loader.ring_cache=off; the Config default is on since (b) attempt 2 was KEPT; 2026-10-06)
    deps.now_tick = [&tick] { return tick; };
    deps.call = [&calls](uint32_t fn, CpuContext& c) {   // the recompiled STREAM/poll bodies stand in
        calls.push_back(fn);
        c.cpuRegs.GPR.r[2].UD[0] = 0;   // the poll answers "not pending"
    };
    LoaderFileIo::Layout layout;
    layout.stream_open_cmd_addr = 0x00530000;
    layout.stream_batch_count_addr = 0x00530010;
    memory::write<int32_t>(layout.stream_open_cmd_addr, -1);
    memory::write<uint16_t>(layout.stream_batch_count_addr, 0);
    LoaderFileIo::Bind(deps, layout);
    LoaderFileIo::ResetForTest();

    // The oracle: STREAM.IRX's extent read straight off the image.
    IsoFs::Entry irx;
    CHECK(fs.Find("CRASH6/SYS/STREAM.IRX", irx), "oracle_stream_irx_on_disc");
    if (irx.size == 0) { std::printf("FAILED (oracle missing)\n"); return 1; }
    std::vector<uint8_t> oracle((irx.size + 2047) / 2048 * 2048);
    engine.ReadSectors(irx.lba, static_cast<uint32_t>(oracle.size() / 2048), oracle.data());

    CpuContext ctx{};
    // open
    const char* path = "crash6\\sys\\stream.irx";
    for (uint32_t i = 0; i <= std::strlen(path); ++i) memory::write<uint8_t>(kPathAddr + i, static_cast<uint8_t>(path[i]));
    memory::write<int32_t>(kGp - 0x74d8, 7);
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kPathAddr;
    LoaderFileIo::Open(ctx);
    const int32_t handle = ctx.cpuRegs.GPR.r[2].SL[0];
    CHECK(handle == 7, "open_returns_the_counter_handle");
    CHECK(memory::read<int32_t>(kGp - 0x74d8) == 8, "open_advances_the_counter");
    CHECK(calls.size() == 1 && calls[0] == layout.fn_register_file, "open_registers_with_stream_cmd31");
    CHECK(ctx.cpuRegs.GPR.r[29].UL[0] == kSp, "open_restores_sp_after_the_stream_call");
    {   // the wire string stood in the frame below sp: "cdrom0:\CRASH6SYS\STREAM.IRX"
        const uint32_t buf = ((kSp - 0x120) & ~0xFu) + 0x10;
        std::string wire;
        for (uint32_t i = 0; i < 40; ++i) { char c = static_cast<char>(memory::read<uint8_t>(buf + i)); if (!c) break; wire += c; }
        CHECK(wire == "cdrom0:\\CRASH6\\SYS\\STREAM.IRX", "open_wire_name_is_cdrom0_upper");
    }
    // size
    memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, handle);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, irx.size);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsSeek, 0);
    // The title's fs objects carry a callback stream at +0x14 (a 64 KB ring); the
    // async window below its capacity is the one that stages dest/size for the
    // completion copy (a stream-less or oversized window goes straight to dest).
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsCbObj, kCb);
    memory::write<uint32_t>(kCb + 0x8, kRing);
    memory::write<uint32_t>(kCb + 0xc, kRing);
    memory::write<uint32_t>(kCb + 0x10, 0x10000);
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
    LoaderFileIo::FileSize(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == irx.size, "size_is_the_disc_extent_size");
    CHECK(calls.size() == 1, "size_does_not_ship_with_nothing_staged");

    // sync windowed read: 0x1000 bytes at offset 0x800 (a whole-sector window)
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 0x800; ctx.cpuRegs.GPR.r[6].UD[0] = 0x1000;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 1; ctx.cpuRegs.GPR.r[9].UD[0] = kStatus;
    LoaderFileIo::WindowedRead(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1, "sync_read_returns_1");
    CHECK(memory::read<uint32_t>(kStatus) == 0x1000, "sync_read_writes_status_size");
    bool exact = true;
    for (uint32_t i = 0; i < 0x1000; ++i) if (memory::read<uint8_t>(kDest + i) != oracle[0x800 + i]) { exact = false; break; }
    CHECK(exact, "sync_read_bytes_exact");

    // sync read with an unaligned window and an EOF clamp
    const uint32_t tail_off = irx.size - 0x333;
    for (uint32_t i = 0; i < 0x800; ++i) memory::write<uint8_t>(kDest + i, 0xEE);
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = tail_off; ctx.cpuRegs.GPR.r[6].UD[0] = 0x800;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 1; ctx.cpuRegs.GPR.r[9].UD[0] = 0;
    LoaderFileIo::WindowedRead(ctx);
    exact = true;
    for (uint32_t i = 0; i < 0x333; ++i) if (memory::read<uint8_t>(kDest + i) != oracle[tail_off + i]) { exact = false; break; }
    CHECK(exact && memory::read<uint8_t>(kDest + 0x333) == 0xEE, "sync_read_unaligned_window_clamped_at_eof");

    // async: issue -> in flight (0) -> boundary delivers -> re-ask answers 1
    for (uint32_t i = 0; i < 0x1000; ++i) memory::write<uint8_t>(kDest + i, 0);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFlags, 0x5);
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 0x1000; ctx.cpuRegs.GPR.r[6].UD[0] = 0x1000;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 0; ctx.cpuRegs.GPR.r[9].UD[0] = kStatus;
    LoaderFileIo::WindowedRead(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "async_issue_returns_0");
    CHECK(memory::read<uint32_t>(kFs) == (0x5 | LoaderFileIo::kFlagReadPending), "async_issue_raises_the_pending_flag");
    CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsDest) == kDest && memory::read<uint32_t>(kFs + LoaderFileIo::kFsSize) == 0x1000,
          "async_issue_records_dest_and_size");
    CHECK(model.InFlight(kFs) && meter.Pending() == 1, "async_issue_is_in_flight_on_the_meter");
    CHECK(memory::read<uint8_t>(kDest) == 0 && memory::read<uint8_t>(kDest + 1) == 0, "async_bytes_not_visible_before_the_boundary");
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 0x1000; ctx.cpuRegs.GPR.r[6].UD[0] = 0x1000;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 0; ctx.cpuRegs.GPR.r[9].UD[0] = kStatus;
    LoaderFileIo::WindowedRead(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "async_reask_while_in_flight_returns_0");
    CHECK(meter.DeliverDue(tick + 1) == 1, "boundary_slot4_delivers_the_read");
    exact = true;
    for (uint32_t i = 0; i < 0x1000; ++i) if (memory::read<uint8_t>(kDest + i) != oracle[0x1000 + i]) { exact = false; break; }
    CHECK(exact, "async_bytes_exact_after_delivery");
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 0x1000; ctx.cpuRegs.GPR.r[6].UD[0] = 0x1000;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 0; ctx.cpuRegs.GPR.r[9].UD[0] = kStatus;
    LoaderFileIo::WindowedRead(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 1, "async_reask_after_delivery_returns_1");
    CHECK(model.Consumed() == 1 && !model.InFlight(kFs), "async_completion_consumed");

    // wait: an in-flight read is completed inside the call; the flag drops on the poll's word
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 0x2000; ctx.cpuRegs.GPR.r[6].UD[0] = 0x800;
    ctx.cpuRegs.GPR.r[7].UD[0] = kDest; ctx.cpuRegs.GPR.r[8].UD[0] = 0; ctx.cpuRegs.GPR.r[9].UD[0] = 0;
    LoaderFileIo::WindowedRead(ctx);
    calls.clear();
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
    LoaderFileIo::WaitReadComplete(ctx);
    CHECK(!model.InFlight(kFs) && model.Forced() == 1, "wait_completes_the_in_flight_read_now");
    CHECK(calls.size() == 1 && calls[0] == layout.fn_poll, "wait_asks_the_title_poll");
    CHECK((memory::read<uint32_t>(kFs) & LoaderFileIo::kFlagReadPending) == 0, "wait_lowers_the_pending_flag");
    exact = true;
    for (uint32_t i = 0; i < 0x800; ++i) if (memory::read<uint8_t>(kDest + i) != oracle[0x2000 + i]) { exact = false; break; }
    CHECK(exact, "wait_bytes_exact");
    CHECK(meter.DeliverDue(tick + 1) == 1 && model.Delivered() == 2, "stale_boundary_closure_is_a_no_op");

    // close: cmd 32 then ship
    calls.clear();
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = static_cast<uint32_t>(handle);
    LoaderFileIo::Close(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "close_returns_0");
    CHECK(calls.size() == 2 && calls[0] == layout.fn_unregister_file && calls[1] == layout.fn_ship, "close_unregisters_then_ships");
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
    LoaderFileIo::FileSize(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == 0, "size_after_close_is_0");

    // a name that is not on the disc opens as -1 and counts as an archive miss
    const char* miss = "levels\\earth\\hub\\beach";
    for (uint32_t i = 0; i <= std::strlen(miss); ++i) memory::write<uint8_t>(kPathAddr + i, static_cast<uint8_t>(miss[i]));
    Prep(ctx);
    ctx.cpuRegs.GPR.r[4].UD[0] = kPathAddr;
    LoaderFileIo::Open(ctx);
    CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == -1, "archive_relative_name_opens_as_minus_1");
    CHECK(LoaderFileIo::Stats().archive_misses == 1 && LoaderFileIo::Stats().opens == 2, "census");

    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
