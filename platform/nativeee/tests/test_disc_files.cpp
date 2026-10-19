// test_disc_files.cpp -- disc.h and files.h over a generated ISO 9660 image: Initialise/SetMedia/WaitReady, the
// descriptors (Open by either slash and any case, Read into guest memory, Seek, Close, Write refused, Reset), the
// errors the PS2 fio layer gave (negative descriptors), and the no-disc behaviour.
#include <cstdio>

#include "engine/cdvd/iso_fs.h"
#include "ps2e_seam.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
constexpr uint32_t kPath = 0x1000, kBuf = 0x100000;
}

int main() {
    using namespace te_test;
    const std::vector<uint8_t> bh = Pattern(5000, 1), elf = Pattern(3000, 2), small = Pattern(10, 3);
    const std::string iso = WriteTestIso({{"CRASH6", "CRASH.BH", bh}, {"", "SLES_525.68", elf}, {"CRASH6", "TINY.BIN", small}});
    CHECK(!iso.empty());

    // --- no disc: Initialise 0, WaitReady NotReady, Open fails
    {
        te::Settings s = DefaultSettings();
        s.iso_path = "";
        Reset(s);
        CHECK_EQ(ps2e_disc_initialise(), 0);
        CHECK_EQ(ps2e_disc_wait_ready(), 6);
        WriteString(kPath, "Crash6\\Crash.bh");
        CHECK(ps2e_files_open(kPath, 1) < 0);
    }
    // --- the disc
    te::Settings s = DefaultSettings();
    s.iso_path = iso;
    Reset(s);
    CHECK_EQ(ps2e_disc_initialise(), 1);
    CHECK_EQ(ps2e_disc_set_media(2), 1);
    CHECK_EQ(ps2e_disc_set_media(9), 0);
    CHECK_EQ(ps2e_disc_wait_ready(), 2);
    CHECK_EQ(ps2e_disc_initialise(), 1);   // again: still open
    CHECK(te::Services::Get().disc().Iso().VolumeId() == "TWINSANITY_TEST");

    // --- files: the game's path forms
    ps2e_files_reset();
    WriteString(kPath, "Crash6\\Crash.bh");
    const int32_t fd = ps2e_files_open(kPath, 1);
    CHECK(fd >= 0);
    WriteString(kPath, "crash6/CRASH.BH");
    const int32_t fd2 = ps2e_files_open(kPath, 1);
    CHECK(fd2 >= 0 && fd2 != fd);
    WriteString(kPath, "Crash6\\Missing.bh");
    CHECK_EQ(ps2e_files_open(kPath, 1), -2);
    WriteString(kPath, "Crash6\\Crash.bh");
    CHECK_EQ(ps2e_files_open(kPath, 2), -30);   // write: refused (read-only disc)
    CHECK(ps2e_files_open(0, 1) < 0);           // null path

    // Read whole, in two parts, then EOF
    CHECK_EQ(ps2e_files_read(fd, kBuf, 2000), 2000);
    CHECK(std::memcmp(te::GuestPointer(kBuf, 2000), bh.data(), 2000) == 0);
    CHECK_EQ(ps2e_files_read(fd, kBuf + 2000, 10000), 3000);
    CHECK(std::memcmp(te::GuestPointer(kBuf, 5000), bh.data(), 5000) == 0);
    CHECK_EQ(ps2e_files_read(fd, kBuf, 100), 0);
    // Seek: set / current / end
    CHECK_EQ(ps2e_files_seek(fd, 100, 0), 100);
    CHECK_EQ(ps2e_files_read(fd, kBuf, 16), 16);
    CHECK(std::memcmp(te::GuestPointer(kBuf, 16), bh.data() + 100, 16) == 0);
    CHECK_EQ(ps2e_files_seek(fd, -16, 1), 100);
    CHECK_EQ(ps2e_files_seek(fd, 0, 2), 5000);
    CHECK(ps2e_files_seek(fd, 1, 2) < 0);
    CHECK(ps2e_files_seek(fd, 0, 7) < 0);
    // Read into unmapped memory
    CHECK_EQ(ps2e_files_seek(fd, 0, 0), 0);
    CHECK(ps2e_files_read(fd, 0x7FFFFFF0u, 16) < 0);
    CHECK_EQ(ps2e_files_read(fd, kBuf, -1), -22);
    // Write refused, Close, bad descriptors
    CHECK_EQ(ps2e_files_write(fd, kBuf, 16), -30);
    CHECK_EQ(ps2e_files_close(fd), 0);
    CHECK(ps2e_files_read(fd, kBuf, 16) < 0);
    CHECK(ps2e_files_close(fd) < 0);
    CHECK(ps2e_files_close(99) < 0);
    // The second descriptor still works; Reset forgets it
    CHECK_EQ(ps2e_files_read(fd2, kBuf, 8), 8);
    ps2e_files_reset();
    CHECK(ps2e_files_read(fd2, kBuf, 8) < 0);
    // The root file and a tiny file
    WriteString(kPath, "SLES_525.68");
    const int32_t fe = ps2e_files_open(kPath, 1);
    CHECK(fe >= 0);
    CHECK_EQ(ps2e_files_seek(fe, 0, 2), 3000);
    WriteString(kPath, "CRASH6/tiny.bin");
    const int32_t ft = ps2e_files_open(kPath, 1);
    CHECK(ft >= 0);
    CHECK_EQ(ps2e_files_read(ft, kBuf, 64), 10);
    CHECK(std::memcmp(te::GuestPointer(kBuf, 10), small.data(), 10) == 0);
    // A directory does not open as a file
    WriteString(kPath, "CRASH6");
    CHECK(ps2e_files_open(kPath, 1) < 0);

    te::Services::ResetForTest();
    std::remove(iso.c_str());
    return Finish("test_disc_files");
}
