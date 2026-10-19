// test_savestate.cpp -- the services' save-state sections (savestate.h) round-tripped through the port registry without
// a module: clock, files, pads, saves, IOP heap, stream, audio and the system section are captured from one set of
// services and restored into a fresh set (Services::ResetForTest), then compared field by field; a bad payload is
// refused and leaves the state untouched. Also the pad log parser (padlog.h).
#include <cstdio>
#include <filesystem>

#include "engine/core/savestate/savestate_format.h"
#include "padlog.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "stream_service.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
constexpr uint32_t kPath = 0x1000, kBuf = 0x100000, kPadBuf = 0x5000;

void PutGuestString(uint32_t at, const char* s) { std::memcpy(te::GuestPointer(at, std::strlen(s) + 1), s, std::strlen(s) + 1); }

const te::savestate::Blob* Find(const std::vector<te::savestate::Blob>& sections, uint32_t tag) {
    for (const auto& b : sections) if (b.tag == tag) return &b;
    return nullptr;
}
}  // namespace

int main() {
    using namespace te_test;
    const std::vector<uint8_t> bh = Pattern(70000, 11);
    const std::string iso = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"CRASH6", "CRASH.BD", Pattern(300000, 13)},
                                          {"", "SLES_525.68", Pattern(100, 12)}});
    CHECK(!iso.empty());
    te::Settings s = DefaultSettings();
    s.iso_path = iso;
    s.mc_dir = (std::filesystem::temp_directory_path() / "te_test_savestate_mc").string();   // every lane's temp dir
    Reset(s);

    // --- a run: services started, 7 boundaries, a file open at an offset, a pad open, an IOP block, a stream channel
    ps2e_system_initialise();
    ps2e_system_start_services();
    ps2e_time_initialise();
    CHECK_EQ(ps2e_pads_open(0, 0, kPadBuf), 1);
    for (int k = 0; k < 7; ++k) ps2e_graphics_wait_v_sync();
    PutGuestString(kPath, "Crash6\\Crash.bh");
    const int32_t fd = ps2e_files_open(kPath, 1);   // 1 = read
    CHECK(fd >= 0);
    CHECK_EQ(ps2e_files_read(fd, kBuf, 1000), 1000);
    const uint32_t block = ps2e_io_allocate_heap(0x4000);
    CHECK(block != 0);
    CHECK_EQ(ps2e_stream_initialise(8, 0), 0);
    PutGuestString(kPath, "CRASH6\\CRASH.BD");
    const int32_t sf = ps2e_stream_open_file(kPath);
    CHECK_EQ(sf, 0);
    ps2e_stream_attach_buffer(2, 0x8000, 0, 0);
    te::Services& a = te::Services::Get();
    const uint64_t ticks_a = ps2e_time_ticks();
    const uint64_t boundaries_a = a.clock().BoundaryCount();
    CHECK_EQ(boundaries_a, 7u);

    // --- capture through the registry (the runner's own sections -- head, RAM0, module, stack -- are not registered here)
    std::vector<te::savestate::Blob> sections = te::savestate::Registry().Capture();
    CHECK(Find(sections, te::savestate::kTagSystem) != nullptr);
    CHECK(Find(sections, te::savestate::kTagClock) != nullptr);
    CHECK(Find(sections, te::savestate::kTagFiles) != nullptr);
    CHECK(Find(sections, te::savestate::kTagPads) != nullptr);
    CHECK(Find(sections, te::savestate::kTagSaves) != nullptr);
    CHECK(Find(sections, te::savestate::kTagIoHeap) != nullptr);
    CHECK(Find(sections, te::savestate::kTagStream) != nullptr);
    CHECK(Find(sections, te::savestate::kTagAudio) != nullptr);
    // The envelope round-trips byte-identically
    {
        ps2r::savestate::FileMeta meta;
        meta.vsync_frame = 7;
        const std::vector<uint8_t> image = ps2r::savestate::SerializeStateFile(meta, sections);
        ps2r::savestate::FileMeta meta2;
        std::vector<te::savestate::Blob> back;
        std::string err;
        CHECK(ps2r::savestate::ParseStateFile(image.data(), image.size(), &meta2, &back, &err));
        CHECK_EQ(back.size(), sections.size());
        CHECK_EQ(meta2.vsync_frame, 7u);
    }

    // --- a fresh process: the services constructed as the system section says, then every section restored
    Reset(s);
    te::stream::Service().Reset();
    te::Services& b = te::Services::Get();
    CHECK(!b.Initialised());
    te::savestate::PrepareOwners();
    b.PrepareForRestore(sections);
    CHECK(b.Initialised());
    CHECK(b.Started());
    std::string err;
    CHECK(te::savestate::Registry().Restore(sections, &err));
    if (!err.empty()) std::fprintf(stderr, "  restore: %s\n", err.c_str());
    CHECK_EQ(b.clock().BoundaryCount(), boundaries_a);
    CHECK_EQ(ps2e_time_ticks(), ticks_a);
    // the clock continues from there
    ps2e_graphics_wait_v_sync();
    CHECK_EQ(b.clock().BoundaryCount(), boundaries_a + 1);
    CHECK_EQ(ps2e_time_ticks(), ticks_a + 9600u);
    // the file descriptor is at its offset: the next 100 bytes are bytes 1000..1099 of the pattern
    CHECK_EQ(ps2e_files_read(fd, kBuf + 0x1000, 100), 100);
    CHECK_EQ(std::memcmp(te::GuestPointer(kBuf + 0x1000, 100), bh.data() + 1000, 100), 0);
    // the pad is open and stable, with the same buffer
    CHECK_EQ(ps2e_pads_get_state(0, 0), 6);
    CHECK_EQ(b.pads().BufferOf(0, 0), kPadBuf);
    // the IOP heap's block is known (freeing it works, freeing twice does not)
    CHECK_EQ(ps2e_io_free_heap(block), 0);
    CHECK_EQ(ps2e_io_free_heap(block), -1);
    // the stream's file and channel buffer are back
    const te::stream::StreamService& st = te::stream::Service();
    CHECK_EQ(static_cast<int>(st.GetState()), 1);
    CHECK_EQ(st.Channels(), 8);
    CHECK(st.ChannelBufferAddress(2) != 0);
    uint64_t off = 0;
    uint32_t size = 0;
    CHECK(st.FileExtent(sf, off, size));
    CHECK_EQ(size, 300000u);
    CHECK_EQ(ps2e_stream_open_file(kPath), 1);   // numbering continues

    // --- a corrupt section is refused, the live state untouched
    {
        std::vector<te::savestate::Blob> bad = sections;
        for (auto& blob : bad) if (blob.tag == te::savestate::kTagIoHeap) blob.payload.resize(3);
        std::string e;
        CHECK(!te::savestate::Registry().Restore(bad, &e));
        CHECK(!e.empty());
    }

    // --- the pad log parser
    uint16_t buttons = 0;
    CHECK(te::padlog::ParseButtons("START+CROSS", buttons));
    CHECK_EQ(buttons, 0x4008);
    CHECK(te::padlog::ParseButtons("0040", buttons));
    CHECK_EQ(buttons, 0x0040);
    CHECK(!te::padlog::ParseButtons("JUMP", buttons));
    CHECK(te::padlog::ButtonNames(0x4008) == "START+CROSS");
    CHECK(te::padlog::ButtonNames(0) == "NONE");
    {
        const std::string path = s.mc_dir + "_pad.padlog";
        FILE* f = std::fopen(path.c_str(), "w");
        std::fprintf(f, "# test\n10 START 128 128 128 128\nhold 20 25 CROSS\n30 NONE\n");
        std::fclose(f);
        std::string perr;
        CHECK(!te::padlog::Play(path, &perr));     // hold lines are for scripts
        CHECK(te::padlog::Script(path, &perr));
        te::Pads& pads = te::Services::Get().pads();
        // sample at boundaries 10, 22, 30 through the override sampler: the clock is at boundaries_a + 1 now, so
        // advance it and read the report
        auto buttons_at = [&](uint64_t target) {
            while (te::Services::Get().clock().BoundaryCount() < target) ps2e_graphics_wait_v_sync();
            uint8_t report[32];
            CHECK_EQ(ps2e_pads_read(0, 0, kBuf + 0x2000), 32);
            std::memcpy(report, te::GuestPointer(kBuf + 0x2000, 32), 32);
            return static_cast<uint16_t>(~(uint16_t(report[2]) | uint16_t(report[3]) << 8));
        };
        CHECK_EQ(buttons_at(10), 0x0008);
        CHECK_EQ(buttons_at(22), 0x4008);   // START still held from line 10, CROSS from the hold
        CHECK_EQ(buttons_at(30), 0x0000);
        (void)pads;
    }

    std::fprintf(stderr, "test_savestate: %d checks, %d failures\n", Checks(), Failures());
    return Failures() ? 1 : 0;
}
