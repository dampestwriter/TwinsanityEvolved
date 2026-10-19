// Actual unchanged NEW callers over a bounded game-object/RAM fixture and the
// real SaveBindings/SaveStorage. No title process or memory-card module.
#include "games/slus_20909_crash_twinsanity/tests/recomp/save_caller_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "Save caller line %d: %s\n", __LINE__, #value); } } while (false)
constexpr uint32_t kDevice = 0x10000, kFiles = 0x11000, kFile = 0x11100, kStream = 0x11200;
constexpr uint32_t kVtable = 0x12000, kData = 0x20000, kStack = 0x7f0000;
// Reviewed NTSC ELF sha256 8d114879...74038f, 64 bytes at 0x305e50:
// sha256 690f2c3acd64a3958b540160fd001ad33fba4b85a3c52327c23cdef3f9320f22.
constexpr std::array<uint32_t, 16> kTables = {
    0x2a24a0, 0x2a24a8, 0x2a24b0, 0x2a24c0, 0x2a24c8, 0x2a24b8, 0x2a24c0, 0x2a24c8,
    0x2a2588, 0x2a259c, 0x2a25b8, 0x2a2614, 0x2a2668, 0x2a25d0, 0x2a2614, 0x2a2668};
struct Rig {
    std::vector<uint8_t> ram = std::vector<uint8_t>(8u << 20);
    uint64_t tick = 0;
    int write_error = 0;
    std::map<uint32_t, title::SaveBindings::Body> entries;
    std::map<uint32_t, unsigned> calls;
    title::SaveStorage store;
    title::SaveBindings binding;
    CpuContext context{};

    explicit Rig(const std::string& path)
        : store({path, 0x1f00, {{true, true, true, true}}, [&](auto) { return write_error; }}),
          binding(store, {
            [this](uint32_t at, void* out, size_t size) { return Access(at, out, size, false); },
            [this](uint32_t at, const void* in, size_t size) { return Access(at, const_cast<void*>(in), size, true); },
            [this] { return tick; }}) {
        CHECK(binding.Install([this](uint32_t at, auto body) {
            return entries.emplace(at, std::move(body)).second;
        }));
        CHECK(entries.size() == 10);
        std::memcpy(ram.data() + 0x305e50, kTables.data(), sizeof(kTables));
        Word(kDevice + 0x1c, kFiles); Word(kFiles, kFile);
        Word(kDevice + 0x20, kVtable); Word(kVtable + 0x3c, 0x2a8510);
        Word(kDevice + 0x38, 0x4020); Word(kFile, 0x4040);
        Word(kFile + 0x1c, kStream); Word(kFile + 0x20, 4); Word(kStream + 8, kData);
        std::memcpy(ram.data() + 0x4000, "BA", 3);
        std::memcpy(ram.data() + 0x4008, "SLUS-20909", 11);
        std::memcpy(ram.data() + 0x4020, "CRASH", 6);
        std::memcpy(ram.data() + 0x4040, "Bank0.bin", 10);
        context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
        context.cpuRegs.GPR.r[29].UL[0] = kStack;
        context.cpuRegs.GPR.r[31].UD[0] = 0x8877665544332211ull;
        for (unsigned r = 16; r <= 19; ++r) context.cpuRegs.GPR.r[r].UD[0] = 0x11223344ull + r;
    }
    bool Access(uint32_t at, void* bytes, size_t size, bool write) {
        at &= 0x1fffffffu;
        if (at > ram.size() || size > ram.size() - at) return false;
        if (write) std::memcpy(ram.data() + at, bytes, size);
        else std::memcpy(bytes, ram.data() + at, size);
        return true;
    }
    uint32_t Word(uint32_t at) const {
        uint32_t value;
        std::memcpy(&value, ram.data() + at, 4);
        return value;
    }
    void Word(uint32_t at, uint32_t value) { std::memcpy(ram.data() + at, &value, 4); }
    int32_t Call(uint32_t address, std::initializer_list<uint32_t> args) {
        unsigned r = 4;
        for (const auto arg : args) context.cpuRegs.GPR.r[r++].UL[0] = arg;
        const auto before = context.cpuRegs.GPR;
        if (!save_callers::Run(address, context)) save_callers::Dispatch(address, context);
        CHECK(context.cpuRegs.GPR.r[29].UL[0] == kStack);
        CHECK(context.cpuRegs.GPR.r[31].UD[0] == before.r[31].UD[0]);
        for (unsigned i = 16; i <= 19; ++i)
            CHECK(context.cpuRegs.GPR.r[i].UD[0] == before.r[i].UD[0]);
        return context.cpuRegs.GPR.r[2].SL[0];
    }
    void Update(bool advance = true) {
        if (advance) ++tick;
        Call(0x2a8558, {kDevice});
    }
    int32_t Request(uint32_t operation, uint32_t next = 24) {
        const auto step = Call(0x2a2548, {kDevice, operation, next});
        // The unchanged Request returns the next step to its game caller.
        Word(kDevice, (Word(kDevice) & ~(0x3fu << 20)) | (uint32_t(step) << 20));
        return step;
    }
    int32_t Poll(uint32_t operation, uint32_t waited = 1) {
        return Call(0x2a2430, {kDevice, operation, 25, waited});
    }
    int32_t Finish(uint32_t operation) {
        for (unsigned i = 0; i < 16; ++i) {
            Update();
            const auto result = Poll(operation);
            if (result != 24) return result;
        }
        return -99;
    }
};
Rig* current = nullptr;
std::vector<uint8_t> FileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
}

namespace save_callers {
std::ostringstream g_logFile;
void Read(uint32_t address, void* out, size_t size) {
    if (!current || !current->Access(address, out, size, false)) throw std::runtime_error("Save caller read");
}
void Write(uint32_t address, const void* in, size_t size) {
    if (!current || !current->Access(address, const_cast<void*>(in), size, true))
        throw std::runtime_error("Save caller write");
}
void Dispatch(uint32_t address, CpuContext& context) {
    ++current->calls[address];
    const auto body = current->entries.find(address);
    if (body == current->entries.end()) throw std::runtime_error("Unexpected save callee");
    body->second(context, address);
}
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t address) {
    if (address != 0x2a8510) throw std::runtime_error("Unexpected save virtual target");
    static const std::function<void(CpuContext&, uint32_t)> presence = [](CpuContext& context, uint32_t entry) {
        if (!Run(entry, context)) throw std::runtime_error("Missing complete device-presence body");
    };
    return &presence;
}
namespace ps2_threading { void exitThread() { throw std::runtime_error("Save caller escaped fixture"); } }
}

int main() {
    char directory[] = "/tmp/save_callers_XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    Rig rig(directory);
    current = &rig;
    CHECK(rig.Call(0x2c5080, {}) == 0 && rig.Call(0x2a8668, {0x4000, 0x4008}) == 1);
    rig.Update();
    CHECK(rig.Word(kDevice + 0x48) == 2);
    CHECK(rig.Request(4) == 24 && rig.Poll(4) == 24); // accepted is still pending
    rig.Update(false);
    CHECK(rig.Poll(4) == 24 && rig.Finish(4) == 25);
    const auto path = std::filesystem::path(directory) / "mc0/BASLUS-20909CRASH/Bank0.bin";
    const std::vector<uint8_t> expected{17, 29, 42, 60};
    std::copy(expected.begin(), expected.end(), rig.ram.begin() + kData);
    for (const auto operation : {5u, 8u}) {
        CHECK(rig.Request(operation) == 24 && rig.Poll(operation, 0) == 24);
        CHECK(rig.Finish(operation) == 25 && FileBytes(path) == expected);
    }
    CHECK(rig.Request(7) == 24 && rig.Finish(7) == 25);
    CHECK(rig.Word(kDevice + 0x58) == 4 && rig.Word(kDevice + 0x80 + 16) == 4);
    CHECK(rig.Request(3) == 24 && rig.Finish(3) == 25 && rig.Word(kDevice + 0x54) > 0);
    for (const auto operation : {6u, 9u}) {
        std::memset(rig.ram.data() + kData, 0, expected.size());
        CHECK(rig.Request(operation) == 24 && rig.Poll(operation) == 24);
        CHECK(rig.ram[kData] == 0);
        CHECK(rig.Finish(operation) == 25);
        CHECK(std::equal(expected.begin(), expected.end(), rig.ram.begin() + kData));
    }
    // OLD E2/U4/R200: a completed failure must stay a failure in the actual
    // game's poll, and must not replace durable bytes or a pending request.
    rig.write_error = title::SaveStorage::kIoError;
    rig.ram[kData] = 99;
    CHECK(rig.Request(8) == 24 && rig.Poll(8) == 24);
    const auto submitted = rig.binding.Counts().submitted;
    CHECK(rig.Request(9) == 1 && rig.binding.Counts().submitted == submitted);
    rig.Word(kDevice, 24u << 20); // the first caller's waiting state
    CHECK(rig.Finish(8) == 1 && ((rig.Word(kDevice) >> 16) & 15) == 1);
    CHECK(FileBytes(path) == expected && rig.store.GetResult(title::SaveStorage::Operation::Write).error ==
        title::SaveStorage::kIoError);
    rig.write_error = 0;
    const auto results = rig.calls[0x2a8638];
    rig.store.SetPresent(0, 0, false);
    rig.Update();
    CHECK(rig.Poll(8, 0) == 1 && rig.calls[0x2a8638] == results);
    CHECK(rig.Request(4) == 1);
    rig.store.SetPresent(0, 0, true);
    rig.Update();
    CHECK(rig.Request(2) == 24 && rig.Finish(2) == 25 && !std::filesystem::exists(path));
    CHECK(rig.Request(0) == 1 && rig.Request(10) == 1);
    CHECK(rig.binding.Counts().published > 0 && rig.store.Committed() > 0);
    CHECK(save_callers::g_logFile.str().empty());
    std::filesystem::remove_all(directory);
    std::printf("SaveCallers: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
