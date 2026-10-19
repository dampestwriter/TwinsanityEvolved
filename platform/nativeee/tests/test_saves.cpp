// test_saves.cpp -- saves.h over title::SaveStorage in a temp memory-card directory: Initialise, the one-operation
// state machine advanced by Update on boundaries (Checking -> operation -> None), CreateSave / Write / FindFile /
// Read / MeasureSave round trips with the game's outputs written to guest memory, the no-card and busy refusals,
// FileKilobytes/SaveBytes.
#include <cstdio>
#include <algorithm>
#include <filesystem>
#if defined(_WIN32)
#include <process.h>
#define TE_TEST_GETPID _getpid
#else
#include <unistd.h>
#define TE_TEST_GETPID getpid
#endif

#include "ps2e_seam.h"
#include "te_test.h"

namespace {
constexpr uint32_t kRegion = 0x1000, kProduct = 0x1020, kSave = 0x1040, kFile = 0x1080, kInfo = 0x1100, kResult = 0x1120;
constexpr uint32_t kValue = 0x1140, kEntry = 0x1180, kData = 0x2000, kBack = 0x3000;
// Platform::Saves::Operation
constexpr int32_t OpNoStorage = -1, OpNone = 0, OpChecking = 1, OpCreate = 4, OpMeasure = 7, OpFind = 8, OpRead = 10, OpWrite = 11;

// Runs Update on boundaries until the operation is None (or `limit` boundaries pass); returns the operations seen.
std::vector<int32_t> Drive(te::Clock& clock, int limit = 64) {
    std::vector<int32_t> seen;
    for (int i = 0; i < limit; ++i) {
        clock.Boundary();
        const int32_t op = ps2e_saves_update(0, 0, kInfo);
        seen.push_back(op);
        if (op == OpNone || op == OpNoStorage) break;
    }
    return seen;
}
}  // namespace

int main() {
    using namespace te_test;
    namespace fs = std::filesystem;
    const fs::path mc = fs::temp_directory_path() / ("te_test_mc_" + std::to_string(::TE_TEST_GETPID()));
    fs::remove_all(mc);

    // --- no card directory: Initialise fails
    {
        te::Settings s = DefaultSettings();
        s.mc_dir = "";
        Reset(s);
        WriteString(kRegion, "BE"); WriteString(kProduct, "SLES-52568");
        CHECK_EQ(ps2e_saves_initialise(kRegion, kProduct), 0);
    }
    te::Settings s = DefaultSettings();
    s.mc_dir = mc.string();
    Reset(s);
    te::Clock& clock = te::Services::Get().clock();
    WriteString(kRegion, "BE"); WriteString(kProduct, "SLES-52568");
    CHECK_EQ(ps2e_saves_initialise(kRegion, kProduct), 1);
    CHECK(fs::is_directory(mc / "mc0"));
    // Idle Update: None, the card's info (type 2 memory card, formatted, free kilobytes)
    CHECK_EQ(ps2e_saves_update(0, 0, kInfo), OpNone);
    CHECK_EQ(ReadValue<int32_t>(kInfo), 2);
    CHECK_EQ(ReadValue<int32_t>(kInfo + 8), 1);
    CHECK(ReadValue<int32_t>(kInfo + 4) > 0);
    // Static helpers (the retail numbers: a 1 KB cluster, a directory entry per file + 2)
    CHECK_EQ(ps2e_saves_file_kilobytes(1), 1);
    CHECK_EQ(ps2e_saves_file_kilobytes(1024), 1);
    CHECK_EQ(ps2e_saves_file_kilobytes(1025), 2);
    CHECK(ps2e_saves_save_bytes(3, 2) >= 3 * 1024);

    // --- CreateSave: Checking first, then Create, then None; GetResult succeeded
    WriteString(kSave, "CRASH");   // the directory is region + product + save: /BESLES-52568CRASH
    CHECK_EQ(ps2e_saves_create_save(0, 0, kSave), 1);
    CHECK_EQ(ps2e_saves_create_save(0, 0, kSave), 0);   // one at a time
    std::vector<int32_t> seen = Drive(clock);
    CHECK(seen.size() >= 2);
    CHECK(seen.front() == OpChecking || seen.front() == OpCreate);   // Checking is seen when polled in the Submit's boundary
    CHECK(std::find(seen.begin(), seen.end(), OpCreate) != seen.end());
    CHECK_EQ(seen.back(), OpNone);
    ps2e_saves_get_result(kResult, OpCreate);
    CHECK_EQ(ReadValue<int32_t>(kResult), 1);
    CHECK(fs::is_directory(mc / "mc0" / "BESLES-52568CRASH"));
    // Repeated Update within one boundary does not advance: a second Create stays Checking until a boundary passes
    CHECK_EQ(ps2e_saves_create_save(0, 0, kSave), 1);
    CHECK_EQ(ps2e_saves_update(0, 0, 0), OpChecking);
    CHECK_EQ(ps2e_saves_update(0, 0, 0), OpChecking);
    Drive(clock);
    ps2e_saves_get_result(kResult, OpCreate);
    CHECK_EQ(ReadValue<int32_t>(kResult), 1);   // an existing save directory: Create succeeds

    // --- Write a file, Find it (size + the 64-byte entry), Read it back, Measure the save
    const std::vector<uint8_t> payload = [] { std::vector<uint8_t> v(1500); for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(i * 7); return v; }();
    std::memcpy(te::GuestPointer(kData, payload.size()), payload.data(), payload.size());
    WriteString(kFile, "CRASH.DAT");
    CHECK_EQ(ps2e_saves_write(0, 0, kSave, kFile, kData, int32_t(payload.size())), 1);
    seen = Drive(clock);
    CHECK(std::find(seen.begin(), seen.end(), OpWrite) != seen.end());
    ps2e_saves_get_result(kResult, OpWrite);
    CHECK_EQ(ReadValue<int32_t>(kResult), 1);
    CHECK(fs::file_size(mc / "mc0" / "BESLES-52568CRASH" / "CRASH.DAT") == payload.size());

    WriteValue<int32_t>(kValue, -7);
    CHECK_EQ(ps2e_saves_find_file(0, 0, kFile, kSave, kValue, kEntry), 1);
    CHECK_EQ(ReadValue<int32_t>(kValue), -7);   // not before the completion
    seen = Drive(clock);
    CHECK(std::find(seen.begin(), seen.end(), OpFind) != seen.end());
    CHECK_EQ(ReadValue<int32_t>(kValue), int32_t(payload.size()));
    CHECK_EQ(ReadValue<uint32_t>(kEntry + 16), uint32_t(payload.size()));   // FileEntry::size
    CHECK(std::strncmp(reinterpret_cast<const char*>(te::GuestPointer(kEntry + 32, 32)), "CRASH.DAT", 32) == 0);
    ps2e_saves_get_result(kResult, OpFind);
    CHECK_EQ(ReadValue<int32_t>(kResult), 1);

    CHECK_EQ(ps2e_saves_read(0, 0, kSave, kFile, kBack, int32_t(payload.size())), 1);
    seen = Drive(clock);
    CHECK(std::find(seen.begin(), seen.end(), OpRead) != seen.end());
    CHECK(std::memcmp(te::GuestPointer(kBack, payload.size()), payload.data(), payload.size()) == 0);
    ps2e_saves_get_result(kResult, OpRead);
    CHECK_EQ(ReadValue<int32_t>(kResult), 1);

    WriteValue<int32_t>(kValue, 0);
    CHECK_EQ(ps2e_saves_measure_save(0, 0, kSave, kValue), 1);
    seen = Drive(clock);
    CHECK(std::find(seen.begin(), seen.end(), OpMeasure) != seen.end());
    CHECK(ReadValue<int32_t>(kValue) >= 2);   // the file's kilobytes + the directory

    // --- A missing file: Find reports -1 size / failed result; a missing save: Measure -1
    WriteString(kFile, "NOPE.DAT");
    WriteValue<int32_t>(kValue, 55);
    CHECK_EQ(ps2e_saves_find_file(0, 0, kFile, kSave, kValue, 0), 1);
    seen = Drive(clock);
    ps2e_saves_get_result(kResult, OpFind);
    CHECK(ReadValue<int32_t>(kResult) != 1 || ReadValue<int32_t>(kValue) == -1);

    // --- Invalid inputs are refused at submit: a name too long, a null buffer with a size
    WriteString(kSave, "THIS_SAVE_NAME_IS_FAR_TOO_LONG_FOR_A_MEMORY_CARD");
    CHECK_EQ(ps2e_saves_create_save(0, 0, kSave), 0);
    WriteString(kSave, "CRASH");   // the directory is region + product + save: /BESLES-52568CRASH
    WriteString(kFile, "CRASH.DAT");
    CHECK_EQ(ps2e_saves_read(0, 0, kSave, kFile, 0x7FFFFFF0u, 16), 0);

    // --- A multitap slot has no card: an operation there is refused at once ("there's no memory card (it then fails
    //     at once)") and its storage info says no card
    CHECK_EQ(ps2e_saves_create_save(0, 1, kSave), 0);
    clock.Boundary();
    CHECK_EQ(ps2e_saves_update(0, 1, kInfo), OpNone);
    CHECK(ReadValue<int32_t>(kInfo) != 2);
    // Port 1 (the second card) is present
    CHECK_EQ(ps2e_saves_update(1, 0, kInfo), OpNone);
    CHECK_EQ(ReadValue<int32_t>(kInfo), 2);
    CHECK(fs::is_directory(mc / "mc1"));

    te::Services::ResetForTest();
    fs::remove_all(mc);
    return Finish("test_saves");
}
