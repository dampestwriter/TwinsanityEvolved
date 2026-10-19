#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "engine/ee/hle/kernel_codes.h"
#include "engine/ee/savestate/thread_section.h"
#include "engine/mc/hle/mc_command_model.h"
#include "engine/mc/savestate/mchl_section.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_save_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_legacy_import.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"

namespace {
using namespace ps2r::savestate;
using Import = title::SaveLegacyImport;
using Codec = title::SaveStateCodec;
using Bindings = title::SaveBindings;
using Store = title::SaveStorage;
using Checkpoint = title::InputSaveCheckpoint;
namespace fs = std::filesystem;
int checks = 0, failures = 0, oracle_checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::fprintf(stderr, "%d: %s\n", __LINE__, #condition); } } while (false)
#define ORACLE(condition) do { ++oracle_checks; CHECK(condition); } while (false)
void Put(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}
uint32_t Get(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at + 1)) << 8) |
        (uint32_t(bytes.at(at + 2)) << 16) | (uint32_t(bytes.at(at + 3)) << 24);
}
std::vector<uint8_t> Encoded(const Bindings::Snapshot& snapshot) {
    std::vector<uint8_t> result;
    CHECK(Codec::Encode(snapshot, result));
    return result;
}
ThreadSection::Record Owner() {
    ThreadSection::Record record;
    record.tid = 1; record.entry = 0x100008; record.gp = 0x3114f0;
    record.status = THS_RUN;
    record.started = record.has_ctx = record.is_boundary = true;
    record.ctx.cpuRegs.pc = 0x17d000;
    record.ctx.cpuRegs.GPR.r[31].UL[0] = 0x100080;
    record.chain = {{0x17ce50, 0x100080}, {0x100008, 0}};
    return record;
}
void Threads(std::vector<Blob>& blobs, const std::vector<ThreadSection::Record>& records) {
    blobs[3].payload.clear();
    ThreadSection::Emit(records, blobs[3].payload);
}
std::vector<Blob> Image(int seen = -1) {
    std::vector<Blob> blobs{{TAG_RAM, 1, kSectionRequired, std::vector<uint8_t>(32 * 1024 * 1024)},
        {TAG_MCHL, 1, 0, {}}, {TAG_SIFS, 1, 0, std::vector<uint8_t>(40)}, {TAG_THRD, 1, kSectionRequired, {}}};
    auto& ram = blobs[0].payload;
    Put(ram, 0x309fb4, 1); Put(ram, 0x309fc0, -1); Put(ram, 0x309fcc, -1);
    for (unsigned i = 0; i < 12; ++i) {
        Put(ram, 0x3c6100 + i * 12, -1);
        Put(ram, 0x3c6104 + i * 12, -1);
    }
    for (const auto at : {0x3c61a0, 0x3064e0}) {
        Put(ram, at, -1); Put(ram, at + 4, -1);
    }
    const char region[] = "BA", product[] = "SLUS-20909";
    std::memcpy(ram.data() + 0x30a6f8, region, sizeof(region));
    std::memcpy(ram.data() + 0x3c6190, product, sizeof(product));
    for (unsigned i = 0; i < 3; ++i) Put(ram, 0x2e7d68 + i * 4, -1);
    Put(ram, 0x3c85a4, 0x80000400);
    Put(ram, 0x2e9ccc, 11); Put(ram, 0x2e9cd0, 12); Put(ram, 0x3c8588, 13);
    McCommandModel current(0);
    current.SetBound(true);
    if (seen >= 0) {
        const auto info = current.GetInfo(seen / 2, seen % 2);
        Put(ram, 0x2e7d68, info.type); Put(ram, 0x2e7d6c, info.free); Put(ram, 0x2e7d70, info.format);
    }
    MchlSection(current).Serialize(blobs[1].payload);
    Threads(blobs, {Owner()});
    return blobs;
}
void Rejected(const std::vector<Blob>& blobs, const char* reason) {
    Bindings::Snapshot out;
    out.storage.generation = 1717;
    const auto before = Encoded(out);
    std::string error;
    CHECK(!Import::Decode(blobs, out, &error));
    if (error.find(reason) == std::string::npos)
        std::fprintf(stderr, "expected '%s', got '%s'\n", reason, error.c_str());
    CHECK(error.find(reason) != std::string::npos);
    CHECK(Encoded(out) == before);
}
void SourceAndCurrentOracle() {
    auto blobs = Image();
    Bindings::Snapshot imported;
    CHECK(Import::Decode(blobs, imported));
    CHECK(imported.storage.initialised && imported.storage.region == "BA" &&
          imported.storage.product == "SLUS-20909");
    CHECK(imported.storage.generation == 1 && imported.storage.sequence == 0 &&
          imported.storage.active == Store::Operation::None && !imported.storage.completion);
    for (const auto& info : imported.storage.info)
        CHECK(info.type == -1 && info.free_kilobytes == -1 && info.formatted == -1);
    for (int slot = 0; slot < 4; ++slot) {
        blobs = Image(slot);
        // Preserve every retained result, including unsupported historical op
        // result slots. Never reinterpret errors as a new completion.
        for (unsigned i = 0; i < 12; ++i) {
            Put(blobs[0].payload, 0x3c6100 + i * 12, i % 2);
            Put(blobs[0].payload, 0x3c6104 + i * 12, i % 6);
            Put(blobs[0].payload, 0x3c6108 + i * 12, static_cast<uint32_t>(-100 - int(i)));
        }
        std::string error = "old";
        CHECK(Import::Decode(blobs, imported, &error) && error.empty());
        for (unsigned i = 0; i < 12; ++i) {
            const auto& result = imported.storage.results[i];
            CHECK(result.succeeded == int(i % 2) && result.step == int(i % 6) && result.error == -100 - int(i));
        }
        McCommandModel current(0);
        current.SetBound(true);
        const auto expected = current.GetInfo(slot / 2, slot % 2);
        for (int i = 0; i < 4; ++i) {
            const auto& info = imported.storage.info[i];
            if (i == slot) {
                ORACLE(info.type == expected.type && info.formatted == expected.format &&
                       info.free_kilobytes == expected.free);
            } else CHECK(info.type == -1 && info.formatted == -1 && info.free_kilobytes == -1);
        }
        const auto bytes = Encoded(imported);
        Bindings::Snapshot decoded;
        CHECK(Codec::Decode(bytes, decoded) && Encoded(decoded) == bytes);
        // BUG-089: the saved format word is data, not an implicit successful 1.
        Put(blobs[0].payload, 0x2e7d70, -1);
        CHECK(Import::Decode(blobs, imported));
        CHECK(imported.storage.info[slot].formatted == -1);
    }
    blobs = Image(0);
    McCommandModel pending(0);
    pending.SetBound(true);
    pending.SetSeen(0, 0, true);
    pending.Issue(McCommandModel::kCmdWrite, 32);
    pending.Issue(McCommandModel::kCmdGetInfo, 0);
    ORACLE(pending.PendingCmd() == McCommandModel::kCmdWrite && pending.PendingResult() == 32 &&
           pending.DroppedGetInfoKicks() == 1); // OLD R202 watchdog cannot consume a write.
    blobs[1].payload.clear(); MchlSection(pending).Serialize(blobs[1].payload);
    Rejected(blobs, "pending command"); // existing manager idle does not permit dropping the completion
    ORACLE(pending.Sync(0).cmd == McCommandModel::kCmdWrite);
    blobs[1].payload.clear(); MchlSection(pending).Serialize(blobs[1].payload);
    CHECK(Import::Decode(blobs, imported));
    ORACLE(Get(blobs[1].payload, 0) == 0 && Get(blobs[1].payload, 4) == 0);
    pending.Issue(McCommandModel::kCmdGetInfo, -1);
    blobs[1].payload.clear(); MchlSection(pending).Serialize(blobs[1].payload);
    Rejected(blobs, "pending command"); // even watchdog-only pending state needs a real migration
}
void ManagerAndObservations() {
    auto blobs = Image();
    auto& ram = blobs[0].payload;
    const auto bad_word = [&](uint32_t at, uint32_t value, const char* reason) {
        auto& data = blobs[0].payload;
        const auto previous = Get(data, at);
        Put(data, at, value); Rejected(blobs, reason); Put(data, at, previous);
    };
    bad_word(0x309fb4, 0, "initialisation");
    bad_word(0x309fb4, 2, "initialisation");
    for (unsigned op = 1; op <= 12; ++op) bad_word(0x309fb8, op, "active operation");
    for (unsigned step = 1; step <= 6; ++step) bad_word(0x309fc4, step, "active operation");
    bad_word(0x309fc0, 0, "next operation");
    bad_word(0x309fbc, 12, "last operation");
    bad_word(0x309fcc, 2, "descriptor");
    for (unsigned i = 0; i < 0x9c; ++i) {
        ram[0x3c61a0 + i] ^= 1; Rejected(blobs, "context"); ram[0x3c61a0 + i] ^= 1;
        ram[0x3064e0 + i] ^= 1; Rejected(blobs, "template"); ram[0x3064e0 + i] ^= 1;
    }
    for (unsigned i = 0; i < 4; ++i) {
        blobs[1].payload[8 + i] = 2; Rejected(blobs, "canonical"); blobs[1].payload[8 + i] = 0;
    }
    bad_word(0x2e7d68, 2, "without a seen slot");
    bad_word(0x3c6108, -4, "without any card observation");
    ram[0x30a6f8] = '/'; Rejected(blobs, "identity"); ram[0x30a6f8] = 'B';
    for (unsigned i = 0; i < 8; ++i) ram[0x30a6f8 + i] = 'A';
    Rejected(blobs, "unterminated");
    blobs = Image(0);
    blobs[1].payload[9] = 1; Rejected(blobs, "multiple seen"); blobs[1].payload[9] = 0;
    bad_word(0x2e7d68, -1, "no storage observation");
    bad_word(0x2e7d68, 1, "storage information");
    bad_word(0x2e7d6c, -2, "storage information");
    bad_word(0x2e7d70, 2, "storage information");
    bad_word(0x3c6100, 2, "invalid result");
    bad_word(0x3c6104, 6, "invalid result");
}
void OwnerContinuations() {
    auto blobs = Image();
    const auto reject_thread = [&](ThreadSection::Record record, const char* reason) {
        Threads(blobs, {record}); Rejected(blobs, reason);
    };
    auto record = Owner(); record.tid = 0; reject_thread(record, "thread ids");
    record = Owner(); record.status = 3; reject_thread(record, "thread status");
    record = Owner(); record.wait_type = 4; reject_thread(record, "wait kind");
    record = Owner(); record.wait_type = TSW_SEMA; reject_thread(record, "wait kind");
    record = Owner(); record.is_boundary = false; reject_thread(record, "exactly one");
    record = Owner(); record.started = false; reject_thread(record, "complete context");
    record = Owner(); record.has_ctx = false; reject_thread(record, "complete context");
    record = Owner(); record.chain.clear(); reject_thread(record, "complete context");
    record = Owner(); record.status = THS_DORMANT; reject_thread(record, "owner is not live");
    for (uint32_t pc : {0u, 0x17d001u, 0x2a2970u, 0x2a6b68u, 0x2a8668u, 0x2c5080u,
                       0x2c54e0u, 0x2c58e8u, 0x2c5c10u, 0xa02c5820u}) {
        record = Owner(); record.entry = pc; reject_thread(record, "entry");
        record = Owner(); record.ctx.cpuRegs.pc = pc; reject_thread(record, "continuation");
        record = Owner(); record.chain[0].func_addr = pc; reject_thread(record, "continuation");
        if (pc) {
            record = Owner(); record.ctx.cpuRegs.GPR.r[31].UL[0] = pc; reject_thread(record, "continuation");
            record = Owner(); record.chain[0].ra = pc; reject_thread(record, "continuation");
        }
    }
    for (int id : {11, 12, 13}) {
        record = Owner(); record.status = THS_WAIT; record.wait_type = TSW_SEMA; record.wait_id = id;
        reject_thread(record, "semaphore waiter");
    }
    record.wait_id = 99; Threads(blobs, {record});
    Bindings::Snapshot out;
    CHECK(Import::Decode(blobs, out)); // other owners still require root validation
    record = Owner(); record.tid = 2;
    Threads(blobs, {Owner(), record}); Rejected(blobs, "exactly one");
    record.is_boundary = false; record.tid = 1;
    Threads(blobs, {Owner(), record}); Rejected(blobs, "thread ids");
    record.tid = 2; record.status = THS_DORMANT; record.started = record.has_ctx = false;
    record.chain.clear(); Threads(blobs, {Owner(), record});
    CHECK(Import::Decode(blobs, out));
    record.chain.push_back({0x100008, 0}); Threads(blobs, {Owner(), record});
    Rejected(blobs, "call chain has no context");
    Threads(blobs, {}); Rejected(blobs, "no incoming owner");
    Threads(blobs, {Owner()});
    for (unsigned offset : {60, 61, 62, 63}) {
        auto previous = blobs[3].payload[4 + offset];
        blobs[3].payload[4 + offset] = offset == 63 ? 1 : 2;
        Rejected(blobs, "canonical"); blobs[3].payload[4 + offset] = previous;
    }
    for (size_t size : {size_t{0}, size_t{4}, size_t{83}, size_t{100}, blobs[3].payload.size() - 1}) {
        auto full = blobs[3].payload; blobs[3].payload.resize(size); Rejected(blobs, "THRD");
        blobs[3].payload = std::move(full);
    }
}
void RpcAndContainer() {
    auto blobs = Image();
    auto& ram = blobs[0].payload;
    for (const auto offset : {4, 0x10, 0x18, 0x1c, 0x20}) {
        Put(ram, 0x3c8580 + offset, 1);
        Rejected(blobs, offset < 0x1c ? "queued work" : "ambiguous end callback");
        Put(ram, 0x3c8580 + offset, 0);
    }
    Put(ram, 0x3c85a4, 0); Rejected(blobs, "bound MC"); Put(ram, 0x3c85a4, 0x80000400);
    const auto rpc = [&](uint32_t client, uint8_t active) {
        blobs[2].payload.resize(49);
        Put(blobs[2].payload, 36, 1); Put(blobs[2].payload, 40, client);
        Put(blobs[2].payload, 44, 42); blobs[2].payload[48] = active;
    };
    rpc(0x3c8580, 1); Rejected(blobs, "active MC RPC");
    rpc(0xa03c8580, 1); Rejected(blobs, "active MC RPC");
    rpc(0x200000, 1); Put(ram, 0x200024, 0x80000400); Rejected(blobs, "active MC RPC");
    Put(ram, 0x200024, 0x80000999);
    Bindings::Snapshot out;
    CHECK(Import::Decode(blobs, out));
    rpc(0x200000, 0); Put(ram, 0x20001c, 0x2c54e0); Rejected(blobs, "Save callback");
    Put(ram, 0x20001c, 0);
    rpc(0x3c8580, 0); CHECK(Import::Decode(blobs, out));
    for (uint32_t client : {0u, 0x200001u, 0x1ffffdcu}) {
        rpc(client, 1); Rejected(blobs, "invalid pending client");
    }
    rpc(0x200000, 2); Rejected(blobs, "invalid pending client");
    blobs[2].payload.resize(39); Rejected(blobs, "header is truncated");
    blobs[2].payload.resize(40); Put(blobs[2].payload, 36, 4097); Rejected(blobs, "count or length");
    Put(blobs[2].payload, 36, 0); blobs[2].payload.push_back(0); Rejected(blobs, "count or length");
    blobs = Image();
    for (size_t i = 0; i < 4; ++i) {
        auto removed = std::move(blobs[i]);
        blobs.erase(blobs.begin() + i); Rejected(blobs, "all required");
        blobs.insert(blobs.begin() + i, std::move(removed));
        blobs[i].version = 2; Rejected(blobs, "version"); blobs[i].version = 1;
        const auto flags = blobs[i].flags;
        blobs[i].flags = 2; Rejected(blobs, "flags"); blobs[i].flags = flags;
    }
    blobs[0].payload.pop_back(); Rejected(blobs, "32 MiB"); blobs[0].payload.push_back(0);
    for (size_t size = 0; size < 12; ++size) {
        blobs[1].payload.resize(size); Rejected(blobs, "12 bytes"); blobs[1].payload.resize(12);
    }
    Put(blobs[1].payload, 4, -4); Rejected(blobs, "undelivered result"); Put(blobs[1].payload, 4, 0);
    for (auto tag : {Tag('I', 'S', 'C', 'P'), Tag('I', 'N', 'P', 'T'), Codec::kTag}) {
        blobs.push_back({tag, 1, kSectionRequired, {}});
        Rejected(blobs, "service checkpoint"); blobs.pop_back();
    }
    blobs.push_back(blobs[1]); Rejected(blobs, "duplicate"); blobs.pop_back();
}
auto TemporaryDirectory() {
    const auto cleanup = [](fs::path* path) { std::error_code error; fs::remove_all(*path, error); delete path; };
    auto path = fs::temp_directory_path() / ("save-import-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(path)) throw std::runtime_error("test directory collision");
    return std::unique_ptr<fs::path, decltype(cleanup)>(new fs::path(std::move(path)), cleanup);
}
void InstallWithoutEffects() {
    auto blobs = Image(0);
    Put(blobs[0].payload, 0x3c6178, 0); // result 10: failed read at step 1
    Put(blobs[0].payload, 0x3c617c, 1); Put(blobs[0].payload, 0x3c6180, -4);
    Bindings::Snapshot imported;
    CHECK(Import::Decode(blobs, imported));
    auto temp = TemporaryDirectory();
    const auto directory = *temp / "cards";
    // Root supplies the real host store. Restore itself must not create it.
    Store store({directory});
    auto& ram = blobs[0].payload;
    unsigned writes = 0;
    uint64_t boundary = 800;
    Bindings bindings(store, {
        [&](uint32_t address, void* out, size_t size) {
            if (address > ram.size() || size > ram.size() - address) return false;
            std::memcpy(out, ram.data() + address, size); return true;
        },
        [&](uint32_t address, const void* in, size_t size) {
            if (address > ram.size() || size > ram.size() - address) return false;
            ++writes; std::memcpy(ram.data() + address, in, size); return true;
        }, [&] { return boundary; }});
    CHECK(bindings.Restore(imported, boundary));
    CHECK(writes == 0 && !fs::exists(directory) && store.Completed() == 0 && store.Committed() == 0);
    const auto state = bindings.Capture();
    CHECK(state && state->storage.generation == 2 && state->storage.last_boundary == boundary);
    CHECK(!store.TakeCompletion());
    CHECK(store.GetResult(Store::Operation::Read).error == -4);
    CHECK(store.Update(0, 0, boundary) == Store::Operation::None); // idle; absence is the Info result
    CHECK(store.StorageInfo(0, 0).type == 0 && !fs::exists(directory));
    for (const char* slot : {"mc0", "mc0_slot1", "mc1", "mc1_slot1"}) fs::create_directories(directory / slot);
    for (unsigned slot = 0; slot < 4; ++slot) {
        std::fill(ram.begin() + 0x200000, ram.begin() + 0x200020, 0xab);
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = slot / 2;
        context.cpuRegs.GPR.r[5].UL[0] = slot % 2;
        context.cpuRegs.GPR.r[6].UL[0] = 0x200004;
        bindings.Invoke(0x2a6b68, context);
        CHECK(Get(ram, 0x200004) == 2 && Get(ram, 0x20000c) == 1);
        CHECK(Get(ram, 0x200000) == 0xabababab && Get(ram, 0x200010) == 0xabababab);
        CHECK(context.cpuRegs.GPR.r[2].SD[0] == 0);
    }
    CHECK(writes == 4 && bindings.Counts().published == 0 && store.Committed() == 0);
}
std::vector<Blob> Parsed(const std::vector<Blob>& blobs) {
    const auto bytes = SerializeStateFile({}, blobs);
    std::vector<Blob> out;
    CHECK(ParseStateFile(bytes.data(), bytes.size(), nullptr, &out, nullptr));
    return out;
}
void CheckpointSelection() {
    auto blobs = Image(0);
    blobs.push_back({TAG_PADS, 1, kSectionRequired, std::vector<uint8_t>(166)});
    blobs.push_back({TAG_CNTR, 1, kSectionRequired, std::vector<uint8_t>(40)});
    Checkpoint::LegacyPolicy policy{true, Checkpoint::Clock::OldSyncV, 0};
    auto parsed = Parsed(blobs);
    for (uint32_t mask : {2u, 3u}) {
        Checkpoint::Plan plan;
        CHECK(Checkpoint::Prepare(parsed, mask, policy, plan));
        CHECK(plan.legacy_file && plan.imported_saves && plan.saves && !plan.saves->storage.completion);
        CHECK(plan.imported_input == (mask == 3) && plan.input.has_value() == (mask == 3));
    }
    Checkpoint::Plan unchanged;
    unchanged.selection = 17; unchanged.imported_saves = true;
    std::string error;
    CHECK(!Checkpoint::Prepare(parsed, 2, {}, unchanged, &error));
    CHECK(unchanged.selection == 17 && unchanged.imported_saves && !unchanged.saves);
    // OLD E2_SAVE: valid file CRC cannot make truncated semantic state safe.
    blobs[1].payload.resize(4); parsed = Parsed(blobs);
    CHECK(!Checkpoint::Prepare(parsed, 2, policy, unchanged, &error));
    CHECK(error.find("12 bytes") != std::string::npos && unchanged.selection == 17 && !unchanged.saves);
    blobs[1].payload.resize(12); blobs[1].payload[8] = 1;
    blobs[4].payload.resize(1); // Save import succeeds locally, Input then fails: publish neither.
    CHECK(!Checkpoint::Prepare(blobs, 3, policy, unchanged, &error));
    CHECK(unchanged.selection == 17 && !unchanged.saves && !unchanged.input);
    blobs[4].payload.resize(166);
    blobs.push_back({Checkpoint::kTag, 1, kSectionRequired, {}});
    CHECK(Checkpoint::EncodeSelection(2, blobs.back().payload));
    parsed = Parsed(blobs);
    CHECK(!Checkpoint::Prepare(parsed, 2, policy, unchanged, &error));
    CHECK(error.find("SVST missing") != std::string::npos);
}
}

int main() {
    SourceAndCurrentOracle();
    ManagerAndObservations();
    OwnerContinuations();
    RpcAndContainer();
    InstallWithoutEffects();
    CheckpointSelection();
    std::printf("SaveLegacyImport: %d checks, %d failures, %d current NEW SELF oracle checks\n",
                checks, failures, oracle_checks);
    return failures ? 1 : 0;
}
