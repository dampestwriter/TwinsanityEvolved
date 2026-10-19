#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"
#include "engine/mc/hle/mc_host_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>

namespace {
using Store = title::SaveStorage;
using Op = Store::Operation;
namespace fs = std::filesystem;
int failures = 0, checks = 0, oracle_checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)
#define ORACLE(condition) do { ++oracle_checks; CHECK(condition); } while (0)
auto TemporaryDirectory() {
    const auto cleanup = [](fs::path* path) {
        std::error_code error;
        fs::remove_all(*path, error);
        delete path;
    };
    const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() / ("twinsanity-save-test-" + std::to_string(id));
    if (!fs::create_directory(path)) throw std::runtime_error("test directory collision");
    return std::unique_ptr<fs::path, decltype(cleanup)>(new fs::path(std::move(path)), cleanup);
}
std::vector<uint8_t> Bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
Store::Request Request(Op operation, std::string save = {}, std::string file = {}, uint32_t size = 0) {
    Store::Request request;
    request.operation = operation;
    request.save = std::move(save);
    request.file = std::move(file);
    request.size = size;
    return request;
}
Store::Completion Finish(Store& store, uint64_t& tick) {
    for (unsigned n = 0; n < 12 && store.Busy(); ++n) store.Update(0, 0, ++tick);
    CHECK(!store.Busy());
    auto completion = store.TakeCompletion();
    CHECK(completion.has_value());
    return completion ? std::move(*completion) : Store::Completion{};
}
Store::Completion Run(Store& store, Store::Request request, uint64_t& tick) {
    CHECK(store.Submit(std::move(request), tick));
    return Finish(store, tick);
}
void BasicAndSelfOracle() {
    auto temp = TemporaryDirectory();
    Store store({(*temp) / "service"});
    McHostStore current(((*temp) / "current").string());
    uint64_t tick = 0;
    CHECK(store.Initialise("BA", "SLUS-20909"));
    CHECK(store.StorageInfo(0, 0).type == 2 && store.StorageInfo(0, 0).formatted == 1);
    CHECK(!store.Initialise("../", "bad"));
    std::vector<McHostStore::DirEntry> directory;
    // R203-MC: missing directory is an error, not an empty successful listing.
    ORACLE(current.GetDir("/BASLUS-20909/*", 20, directory) == -4);
    auto found = Run(store, Request(Op::Find, "", "Bank0.bin"), tick);
    ORACLE(found.result.succeeded == 0 && found.result.error == -4);
    auto measured = Run(store, Request(Op::Measure), tick);
    CHECK(measured.result.succeeded == 1 && measured.value == -1);
    CHECK(Run(store, Request(Op::Read, "", "missing", 1), tick).result.error == Store::kMissing);
    auto missing_write = Request(Op::Write, "", "Bank0.bin", 3);
    missing_write.bytes = {1, 2, 3};
    CHECK(Run(store, missing_write, tick).result.error == -4);
    CHECK(!fs::exists((*temp) / "service/mc0/BASLUS-20909"));
    ORACLE(current.Mkdir("/BASLUS-20909") == 0);
    CHECK(Run(store, Request(Op::Create), tick).result.succeeded == 1);
    ORACLE(current.GetDir("/BASLUS-20909/*", 20, directory) == 2);
    const auto empty_measure = Run(store, Request(Op::Measure), tick);
    // Source accounting includes the two dot entries without exposing them.
    ORACLE(empty_measure.value == (static_cast<int>(directory.size()) - 1) / 2 + 2);
    auto missing_file = Run(store, Request(Op::Find, "", "Bank0.bin"), tick);
    CHECK(missing_file.result.succeeded == 1 && missing_file.value == -1 && !missing_file.entry_present);

    const std::vector<uint8_t> payload{0, 255, 17, 31, 64, 128, 9, 8, 7, 6, 5, 4, 3, 2, 1, 42, 99};
    auto write = Request(Op::Write, "", "Bank0.bin", static_cast<uint32_t>(payload.size()));
    write.bytes = payload;
    CHECK(store.Submit(write, tick));
    std::fill(write.bytes.begin(), write.bytes.end(), 0xee); // request owns its copy
    CHECK(!store.CanCheckpoint() && !store.Capture());
    CHECK(store.Update(1, 1, tick) == Op::Checking);
    CHECK(store.GetResult(Op::Write).succeeded == -1);
    // R202-MC: another slot's watchdog never destroys the pending write.
    CHECK(store.Update(1, 1, ++tick) == Op::Write);
    CHECK(!store.Submit(Request(Op::Find, "", "other"), tick));
    CHECK(store.LastSubmitError() == -200 && store.Active() == Op::Write);
    CHECK(!fs::exists((*temp) / "service/mc0/BASLUS-20909/Bank0.bin"));
    const auto written = Finish(store, tick);
    CHECK(written.result.succeeded == 1 && written.result.step == 5 && written.committed);
    CHECK(!store.TakeCompletion()); // once only
    CHECK(current.Chdir("/BASLUS-20909") == 0);
    int fd = current.Open("Bank0.bin", McHostStore::kModeWrite | McHostStore::kModeCreate);
    CHECK(fd >= 2);
    CHECK(current.Write(fd, payload.data(), static_cast<uint32_t>(payload.size())) == 17);
    CHECK(current.Close(fd) == 0);
    ORACLE(Bytes((*temp) / "service/mc0/BASLUS-20909/Bank0.bin") ==
           Bytes((*temp) / "current/mc0/BASLUS-20909/Bank0.bin"));
    // R200-MC: bytes survive a new owner; no fake successful write.
    Store fresh({(*temp) / "service"});
    CHECK(fresh.Initialise("BA", "SLUS-20909"));
    auto read = Run(fresh, Request(Op::Read, "", "Bank0.bin", 64), tick);
    fd = current.Open("Bank0.bin", McHostStore::kModeRead);
    std::vector<uint8_t> reference_bytes;
    CHECK(current.Read(fd, reference_bytes, 64) == 17 && current.Close(fd) == 0);
    ORACLE(read.bytes == reference_bytes && read.result.succeeded == 1);
    ORACLE(current.GetDir("/BASLUS-20909/Bank0.bin", 1, directory) == 1);
    found = Run(fresh, Request(Op::Find, "", "Bank0.bin"), tick);
    ORACLE(found.entry_present && found.entry.size == directory.front().size &&
           std::string(found.entry.name.data()) == directory.front().name && found.entry.attributes == 0x8497);
    CHECK(found.entry.created == (std::array<uint8_t, 8>{{0, 0, 0, 12, 15, 8, 234, 7}}));
    ORACLE(current.GetDir("/BASLUS-20909/*", 20, directory) == 3);
    uint32_t measured_reference = 0;
    for (const auto& entry : directory) measured_reference += (entry.size + 1023) / 1024;
    measured_reference += (static_cast<uint32_t>(directory.size()) - 1) / 2 + 2;
    ORACLE(Run(fresh, Request(Op::Measure), tick).value == static_cast<int32_t>(measured_reference));

    auto shorter = Request(Op::Write, "", "Bank0.bin", 2);
    shorter.bytes = {222, 111};
    CHECK(Run(fresh, shorter, tick).result.succeeded == 1);
    fd = current.Open("Bank0.bin", McHostStore::kModeWrite | McHostStore::kModeCreate);
    CHECK(current.Write(fd, shorter.bytes.data(), 2) == 2 && current.Close(fd) == 0);
    ORACLE(Bytes((*temp) / "service/mc0/BASLUS-20909/Bank0.bin") ==
           Bytes((*temp) / "current/mc0/BASLUS-20909/Bank0.bin"));
    CHECK(Run(fresh, Request(Op::Write, "", "Bank0.bin"), tick).result.succeeded == 1);
    CHECK(Bytes((*temp) / "service/mc0/BASLUS-20909/Bank0.bin").empty());
    CHECK(Run(fresh, Request(Op::Write, "", "Bank1.bin"), tick).result.succeeded == 1);
    const auto allocation = Run(fresh, Request(Op::Measure), tick).value;
    fresh.Update(0, 0, ++tick);
    CHECK(fresh.StorageInfo(0, 0).free_kilobytes == 0x1f00 - allocation);
}
void ErrorsAndAtomicity() {
    auto temp = TemporaryDirectory();
    int fault = -1;
    Store::Config config{(*temp)};
    config.write_failure = [&](Store::WritePoint point) {
        return static_cast<int>(point) == fault ? Store::kIoError : 0;
    };
    Store store(config);
    uint64_t tick = 0;
    CHECK(store.Initialise("BA", "SLUS-20909"));
    CHECK(Run(store, Request(Op::Create), tick).result.succeeded == 1);
    auto write = Request(Op::Write, "", "Bank0.bin", 3);
    write.bytes = {1, 2, 3};
    CHECK(Run(store, write, tick).result.succeeded == 1);
    write.bytes = {4, 5, 6};
    for (fault = 0; fault < 3; ++fault) {
        auto result = Run(store, write, tick);
        CHECK(result.result.succeeded == 0 && result.result.error == Store::kIoError && !result.committed);
        CHECK(Bytes((*temp) / "mc0/BASLUS-20909/Bank0.bin") == (std::vector<uint8_t>{1, 2, 3}));
        CHECK(std::distance(fs::directory_iterator((*temp) / "mc0/BASLUS-20909"), fs::directory_iterator{}) == 1);
    }
    fault = -1;
    for (const std::string& name : std::vector<std::string>{
             "../Bank0.bin", "/Bank0.bin", "a\\b", ".", "..", "a:b", "a*b",
             ".save-pending-hidden", std::string(32, 'b'), std::string("x\0y", 3)}) {
        auto invalid = write;
        invalid.file = name;
        CHECK(!store.Submit(invalid, tick) && store.LastSubmitError() == Store::kInvalid);
    }
    auto invalid = write;
    invalid.size = Store::kMaxBytes + 1;
    CHECK(!store.Submit(invalid, tick));
    invalid = write;
    invalid.operation = Op::Read;
    CHECK(!store.Submit(invalid, tick)); // reads cannot carry arbitrary write payloads
    invalid = write;
    invalid.bytes.clear();
    CHECK(!store.Submit(invalid, tick));
    invalid = write;
    invalid.slot = 2;
    CHECK(!store.Submit(invalid, tick));
    invalid = write;
    invalid.operation = static_cast<Op>(9);
    CHECK(!store.Submit(invalid, tick));
    CHECK(store.Submit(Request(Op::Read, "", "Bank0.bin", 3), tick));
    store.SetPresent(0, 0, false);
    CHECK(store.Update(0, 0, ++tick) == Op::NoStorage);
    auto absent = store.TakeCompletion();
    CHECK(absent && absent->result.error == Store::kNoStorage);
    CHECK(!store.Submit(write, tick));
    store.SetPresent(0, 0, true);
    CHECK(Run(store, write, tick).result.succeeded == 1);
    // Symlink directories/files must not redirect reads or writes out of a slot.
    std::error_code error;
    fs::create_directories((*temp) / "outside");
    fs::create_directory_symlink((*temp) / "outside", (*temp) / "mc0/BASLUS-20909link", error);
    CHECK(!error);
    auto escaped = write;
    escaped.save = "link";
    CHECK(Run(store, escaped, tick).result.succeeded == 0);
    CHECK(Run(store, Request(Op::Measure, "link"), tick).result.error == Store::kDenied);
    CHECK(Run(store, Request(Op::Create, "link"), tick).result.error == Store::kDenied);
    CHECK(!fs::exists((*temp) / "outside/Bank0.bin"));
    fs::create_symlink((*temp) / "outside/file", (*temp) / "mc0/BASLUS-20909/evil", error);
    CHECK(!error);
    CHECK(Run(store, write, tick).result.succeeded == 0);
    fs::remove((*temp) / "mc0/BASLUS-20909/evil");
    Store::Config tiny_config{(*temp) / "tiny"};
    tiny_config.capacity_kilobytes = 2;
    Store tiny(tiny_config);
    CHECK(tiny.Initialise("BA", "SLUS-20909"));
    CHECK(Run(tiny, Request(Op::Create), tick).result.succeeded == 1);
    CHECK(Run(tiny, write, tick).result.error == Store::kNoSpace);
    CHECK(!fs::exists((*temp) / "tiny/mc0/BASLUS-20909/Bank0.bin"));
    // Formats are real operations confined to the selected slot.
    auto other = Request(Op::Create);
    other.port = 1;
    CHECK(Run(store, other, tick).result.succeeded == 1);
    CHECK(Run(store, Request(Op::Format), tick).result.succeeded == 1);
    CHECK(fs::is_empty((*temp) / "mc0"));
    CHECK(fs::is_directory((*temp) / "mc1/BASLUS-20909"));
    // Reset during a pending write cancels the unpublished request, preserves
    // the preceding committed bytes, and invalidates prior generations.
    CHECK(Run(store, Request(Op::Create), tick).result.succeeded == 1);
    CHECK(Run(store, write, tick).result.succeeded == 1);
    const auto before_reset = store.Capture();
    CHECK(before_reset);
    write.bytes = {9, 9, 9};
    CHECK(store.Submit(write, tick));
    CHECK(!store.CanCheckpoint());
    store.Reset();
    CHECK(!store.Busy() && !store.TakeCompletion());
    CHECK(store.Capture()->generation > before_reset->generation);
    CHECK(store.Initialise("BA", "SLUS-20909"));
    CHECK(Bytes((*temp) / "mc0/BASLUS-20909/Bank0.bin") == (std::vector<uint8_t>{4, 5, 6}));
    // Corrupt/truncated payloads remain opaque; the unchanged game's reader
    // owns validation. Storage must not invent padding or a valid checksum.
    auto opaque = Request(Op::Write, "", "Broken.bin", 1);
    opaque.bytes = {0xff};
    CHECK(Run(store, opaque, tick).result.succeeded == 1);
    CHECK(Run(store, Request(Op::Read, "", "Broken.bin", 128), tick).bytes == opaque.bytes);
    CHECK(Store::FileKilobytes(UINT64_MAX) == UINT64_MAX / 1024 + 1);
    CHECK(Store::SaveBytes(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
    CHECK(Store::SaveBytes(0, 0) == 2048 && Store::SaveBytes(0, 2) == 3072);
}
void CheckpointsAndBindings() {
    auto temp = TemporaryDirectory();
    Store store({(*temp)});
    title::SaveBindings::Hooks hooks;
    uint64_t tick = 0;
    std::vector<uint8_t> ram(0x200000, 0xcd);
    // The title's main thread runs on the scratchpad (TR entry.cpp:8-9,83);
    // the fixture keeps its 16 KB apart from main RAM like the root does.
    std::vector<uint8_t> spad(0x4000, 0xcd);
    const auto bytes_at = [&](uint32_t a, size_t n) -> uint8_t* {
        if ((a & 0x70000000u) == 0x70000000u) {
            const uint32_t offset = a & 0x3fffu;
            return n <= spad.size() - offset ? spad.data() + offset : nullptr;
        }
        return a <= ram.size() && n <= ram.size() - a ? ram.data() + a : nullptr;
    };
    hooks.boundary = [&] { return tick; };
    hooks.read = [&](uint32_t a, void* b, size_t n) {
        const auto* at = bytes_at(a, n);
        if (!at) return false;
        std::memcpy(b, at, n); return true;
    };
    hooks.write = [&](uint32_t a, const void* b, size_t n) {
        auto* at = bytes_at(a, n);
        if (!at) return false;
        std::memcpy(at, b, n); return true;
    };
    title::SaveBindings bindings(store, hooks);
    std::map<uint32_t, title::SaveBindings::Body> bodies;
    CHECK(bindings.Install([&](uint32_t a, title::SaveBindings::Body body) {
        bodies[a] = std::move(body); return true;
    }));
    CHECK(bodies.size() == 10);
    CpuContext context{};
    context.cpuRegs.GPR.r[29].UL[0] = 0x10000;
    context.cpuRegs.GPR.r[31].UD[0] = 0xcafecafe;
    const auto call = [&](uint32_t f, std::initializer_list<uint32_t> args) -> int32_t {
        unsigned index = 4;
        for (uint32_t value : args) context.cpuRegs.GPR.r[index++].UL[0] = value;
        bodies.at(f)(context, 0);
        CHECK(context.cpuRegs.GPR.r[29].UL[0] == 0x10000 && context.cpuRegs.GPR.r[31].UD[0] == 0xcafecafe);
        return context.cpuRegs.GPR.r[2].SL[0];
    };
    const auto string = [&](uint32_t a, const char* value) { std::memcpy(ram.data() + a, value, std::strlen(value) + 1); };
    string(0x20000, "BA"); string(0x20100, "SLUS-20909");
    string(0x20200, ""); string(0x20300, "Bank0.bin");
    CHECK(call(0x2a8668, {0x20000, 0x20100}) == 1);
    // BUG-089: poisoned sp+0x10 points to a live manager header.
    uint32_t manager = 0x21000, header = 0xe004;
    std::memcpy(ram.data() + 0x10010, &manager, 4);
    std::memcpy(ram.data() + manager, &header, 4);
    CHECK(call(0x2a6b68, {0, 0, 0x22000}) == 0);
    uint32_t observed = 0;
    std::memcpy(&observed, ram.data() + manager, 4);
    CHECK(observed == 0xe004);
    Store::Info info;
    std::memcpy(&info, ram.data() + 0x22000, sizeof(info));
    CHECK(info.type == 2 && info.formatted == 1 && ram[0x2200c] == 0xcd);
    CHECK(call(0x2a8728, {0, 0, 0x20200}) == 1);
    while (store.Busy()) { ++tick; call(0x2a6b68, {0, 0, 0}); }
    ram[0x23000] = 17; ram[0x23001] = 99;
    CHECK(call(0x2a8c28, {0, 0, 0x20200, 0x20300, 0x23000, 2}) == 1);
    ram[0x23000] = 0;
    CHECK(!bindings.Capture());
    while (store.Busy()) { ++tick; call(0x2a6b68, {1, 1, 0}); }
    CHECK(Bytes((*temp) / "mc0/BASLUS-20909/Bank0.bin") == (std::vector<uint8_t>{17, 99}));
    CHECK(call(0x2a8a60, {0, 0, 0x20300, 0x20200, 0x24000, 0x24100}) == 1);
    while (store.Busy()) { ++tick; call(0x2a6b68, {0, 0, 0}); }
    std::memcpy(&observed, ram.data() + 0x24000, 4);
    CHECK(observed == 2 && ram[0x24004] == 0xcd && ram[0x24140] == 0xcd);
    CHECK(call(0x2a8df0, {0, 0, 0x20200, 0x20300, 0x25000, 4}) == 1);
    const auto checkpoint = bindings.Capture();
    CHECK(checkpoint);
    auto corrupt = *checkpoint;
    corrupt.outputs.size = 1; // pending read cannot overrun a restored output
    CHECK(!bindings.Restore(corrupt, 0));
    CHECK(bindings.Restore(*checkpoint, 0));
    tick = 0;
    CHECK(call(0x2a6b68, {0, 0, 0}) == 1 && ram[0x25000] == 0xcd);
    while (store.Busy()) { ++tick; call(0x2a6b68, {0, 0, 0}); }
    CHECK(ram[0x25000] == 17 && ram[0x25001] == 99 && ram[0x25002] == 0xcd);
    CHECK(call(0x2a8638, {10, 0x26000}) == 1);
    Store::Result result;
    std::memcpy(&result, ram.data() + 0x26000, sizeof(result));
    CHECK(result.succeeded == 1 && result.step == 1 && result.error == 0);
    CHECK(context.cpuRegs.GPR.r[2].UL[1] == 1 && context.cpuRegs.GPR.r[3].SD[0] == 0);
    // Poll 0x2a2430 hands GetResult its own stack slot as the output; the
    // stack is in the scratchpad. The result lands there (gate-5 abort
    // "game output no longer writable" at the title card probe), never in
    // main RAM, and a slot running past the scratchpad's end is refused.
    CHECK(title::SaveBindings::RamRange(0x70003ff0, sizeof(Store::Result)));
    CHECK(!title::SaveBindings::RamRange(0x70003ff8, sizeof(Store::Result)));
    CHECK(call(0x2a8638, {10, 0x70003ff0}) == 1);
    Store::Result stack_result;
    std::memcpy(&stack_result, spad.data() + 0x3ff0, sizeof(stack_result));
    CHECK(stack_result.succeeded == 1 && stack_result.step == 1 && stack_result.error == 0);
    CHECK(ram[0x3ff0] == 0xcd);
    Store::Info stack_info;
    CHECK(call(0x2a6b68, {0, 0, 0x70000010}) == 0);
    std::memcpy(&stack_info, spad.data() + 0x10, sizeof(stack_info));
    CHECK(stack_info.type == 2 && stack_info.formatted == 1 && spad[0x1c] == 0xcd);
    bool stack_refused = false;
    try { call(0x2a8638, {10, 0x70003ff8}); } catch (const std::runtime_error&) { stack_refused = true; }
    CHECK(stack_refused);
    // Completed durable write restore must not write it again.
    const auto idle = bindings.Capture();
    CHECK(idle);
    const auto committed = store.Committed();
    CHECK(bindings.Restore(*idle, 0));
    CHECK(call(0x2a6b68, {0, 0, 0}) == 0 && store.Committed() == committed);
    const auto live_generation = bindings.Capture()->storage.generation;
    CHECK(bindings.Restore(*idle, 0));
    CHECK(bindings.Capture()->storage.generation > live_generation); // stale token generation never reused
    auto invalid_snapshot = *idle;
    invalid_snapshot.storage.initialised = false;
    invalid_snapshot.storage.active = Op::Read;
    invalid_snapshot.storage.pending = Request(Op::Read, "", "Bank0.bin", 0);
    CHECK(!bindings.Restore(invalid_snapshot, 0));
    invalid_snapshot = *idle;
    invalid_snapshot.storage.generation = UINT64_MAX;
    CHECK(!bindings.Restore(invalid_snapshot, 0));
    CHECK(call(0x2a8df0, {0, 0, 0x20200, 0x20300, 0x1ffffff, 4}) == 0);
    bool refused = false;
    try { bodies.at(0x2a8df0)(context, 0x2a8df4); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);
    bindings.Reset();
    CHECK(!store.Busy() && store.GetResult(Op::Read).succeeded == -1);
}
}
int main() {
    BasicAndSelfOracle();
    ErrorsAndAtomicity();
    CheckpointsAndBindings();
    std::printf("SaveStorage: %d checks (%d SELF oracle), %d failures\n", checks, oracle_checks, failures);
    return failures ? 1 : 0;
}
