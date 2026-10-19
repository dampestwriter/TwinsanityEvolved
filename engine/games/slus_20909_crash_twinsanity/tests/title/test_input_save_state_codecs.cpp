#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"
#include "engine/core/savestate/section_registry.h"
#include "engine/core/savestate/quiescence.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace {
using title::Input;
using title::InputStateCodec;
using title::SaveBindings;
using title::SaveStateCodec;
using Store = title::SaveStorage;
using Op = Store::Operation;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

auto TemporaryDirectory() {
    const auto cleanup = [](fs::path* path) {
        std::error_code error;
        fs::remove_all(*path, error);
        delete path;
    };
    auto path = fs::temp_directory_path() / ("input-save-codec-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(path)) throw std::runtime_error("temporary directory collision");
    return std::unique_ptr<fs::path, decltype(cleanup)>(new fs::path(std::move(path)), cleanup);
}
std::vector<uint8_t> DiskBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}
Store::Request Request(Op operation, const char* file = "", uint32_t size = 0) {
    Store::Request result;
    result.operation = operation;
    result.file = file;
    result.size = size;
    return result;
}
void Finish(Store& store, uint64_t& tick) {
    for (unsigned i = 0; i < 12 && store.Busy(); ++i) store.Update(0, 0, ++tick);
    CHECK(!store.Busy());
}
void InputRoundTrip() {
    Input input;
    std::array<Input::Sample, 8> samples{};
    samples[0].connected = samples[7].connected = true;
    samples[0].buttons = 0x4018;
    samples[0].lx = 17;
    samples[0].ry = 203;
    samples[0].has_pressure = true;
    samples[0].pressure[6] = 91;
    samples[7].buttons = 0x8000;
    CHECK(input.Open(0, 0) && input.Open(1, 3));
    CHECK(input.SampleBoundary(8, samples));
    CHECK(input.EnterPressure(0, 0));
    CHECK(input.SampleBoundary(9, samples));
    CHECK(input.SetMode(0, 0, 0, 3)); // pending command survives rebased tick zero
    auto original = input.Capture();
    original.sequence = 0x1122334455667788ull;
    std::vector<uint8_t> bytes{0xee};
    std::string error;
    CHECK(InputStateCodec::Encode(original, bytes, &error));
    CHECK(bytes.size() == 486);
    const std::vector<uint8_t> header{'I', 'N', 'P', 'T', 2, 0, 0, 0,
        9, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    CHECK(std::equal(header.begin(), header.end(), bytes.begin()));
    CHECK(bytes[26] == 8 && bytes[27] == 0); // fixed slot count, no host sizeof
    Input::Snapshot decoded;
    CHECK(InputStateCodec::Decode(bytes, decoded, &error) && error.empty());
    std::vector<uint8_t> roundtrip;
    CHECK(InputStateCodec::Encode(decoded, roundtrip) && bytes == roundtrip);
    CHECK(decoded.pads[7].sample.buttons == 0x8000 && decoded.pads[0].sample.pressure[6] == 91);
    CHECK(decoded.pads[0].pending == Input::Command::Mode && decoded.pads[0].next_lock);
    CHECK(input.Restore(decoded, 0));
    CHECK(input.GetState(0, 0) == Input::State::Busy);
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.Current(0, 0).report[1] == 0x79);
    CHECK(input.Sequence() == original.sequence + 1);
    CHECK(input.SampleBoundary(1, samples));
    CHECK(input.Current(0, 0).report[1] == 0x41);
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    // OLD READY/sequence restore defects stay pinned after actual byte coding.
    CHECK(input.Current(1, 3).readable && input.Sequence() == original.sequence + 2);

    unsigned motor_calls = 0;
    Input motors([&](unsigned, unsigned, Input::Motors) { ++motor_calls; return true; });
    CHECK(motors.Open(0, 0));
    motors.SampleBoundary(8, samples);
    CHECK(motors.SetMotors(0, 0, {{1, 198, 0, 0, 0, 0}}));
    const auto held = motors.Capture();
    CHECK(InputStateCodec::Encode(held, bytes));
    CHECK(InputStateCodec::Decode(bytes, decoded));
    CHECK(motor_calls == 1); // codec never drives a device
    CHECK(decoded.pads[0].motors.small == 1 && decoded.pads[0].motors.big == 198);
    motors.Reset();
    CHECK(motor_calls == 2);
    CHECK(motors.Restore(decoded, 0) && motor_calls == 2);
    motors.SampleBoundary(0, samples);
    CHECK(motor_calls == 3);
}
void InputHostile() {
    Input::Snapshot original;
    std::vector<uint8_t> bytes;
    CHECK(InputStateCodec::Encode(original, bytes));
    Input::Snapshot sentinel;
    sentinel.sequence = 99;
    for (size_t length = 0; length < bytes.size(); ++length) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(length));
        CHECK(!InputStateCodec::Decode(truncated, sentinel) && sentinel.sequence == 99);
    }
    auto bad = bytes;
    bad.push_back(0);
    CHECK(!InputStateCodec::Decode(bad, sentinel));
    for (size_t offset : {size_t(0), size_t(4), size_t(24), size_t(26), size_t(46), size_t(50), size_t(85)}) {
        bad = bytes;
        bad[offset] = 254;
        CHECK(!InputStateCodec::Decode(bad, sentinel) && sentinel.sequence == 99);
    }
    bad = bytes;
    std::fill(bad.begin() + 16, bad.begin() + 24, 255); // exhausted sequence
    CHECK(!InputStateCodec::Validate(bad));
    original.sequence = 0;
    const auto preserved = bytes;
    CHECK(!InputStateCodec::Encode(original, bytes) && bytes == preserved);
    // Bounded hostile mutations: a successfully decoded canonical payload must
    // re-encode identically, and every failure leaves the destination alone.
    for (size_t offset = 0; offset < preserved.size(); ++offset) {
        bad = preserved;
        bad[offset] ^= 0x80;
        auto result = sentinel;
        if (InputStateCodec::Decode(bad, result)) {
            CHECK(Input::ValidateSnapshot(result));
            CHECK(InputStateCodec::Encode(result, bytes) && bytes == bad);
        } else {
            CHECK(result.sequence == sentinel.sequence);
        }
    }
}
void SaveRoundTripAndPolicy() {
    auto temporary = TemporaryDirectory();
    Store store({*temporary});
    uint64_t tick = 100;
    std::vector<uint8_t> ram(0x30000, 0xcd);
    unsigned writes = 0;
    SaveBindings::Hooks hooks;
    hooks.boundary = [&] { return tick; };
    hooks.read = [&](uint32_t address, void* data, size_t size) {
        address &= 0x1fffffffu;
        if (address > ram.size() || size > ram.size() - address) return false;
        std::memcpy(data, ram.data() + address, size);
        return true;
    };
    hooks.write = [&](uint32_t address, const void* data, size_t size) {
        address &= 0x1fffffffu;
        if (address > ram.size() || size > ram.size() - address) return false;
        ++writes;
        std::memcpy(ram.data() + address, data, size);
        return true;
    };
    SaveBindings bindings(store, hooks);
    CHECK(store.Initialise("BA", "SLUS-20909"));
    CHECK(store.Submit(Request(Op::Create), tick));
    Finish(store, tick);
    CHECK(store.TakeCompletion()->result.succeeded == 1);
    auto request = Request(Op::Write, "Bank0.bin", 3);
    request.bytes = {1, 2, 3};
    CHECK(store.Submit(request, tick));
    ps2r::savestate::Quiescence quiescence;
    quiescence.Register("SaveStorage", [&](std::string* reason) {
        if (store.CanCheckpoint()) return true;
        if (reason) *reason = "pending persistent mutation; retry at a later boundary";
        return false;
    });
    CHECK(!quiescence.Check() && !bindings.Capture());
    CHECK(store.Busy() && !fs::exists(*temporary / "mc0/BASLUS-20909/Bank0.bin"));
    // Refusing a runtime snapshot does not wait, cancel or complete game writes.
    CHECK(store.GetResult(Op::Write).succeeded == -1);
    Finish(store, tick);
    CHECK(quiescence.Check());
    const auto committed = bindings.Capture();
    CHECK(committed && committed->storage.completion->committed);
    std::vector<uint8_t> bytes;
    CHECK(SaveStateCodec::Encode(*committed, bytes));
    CHECK(bytes[0] == 'S' && bytes[1] == 'V' && bytes[2] == 'S' && bytes[3] == 'T');
    SaveBindings::Snapshot decoded;
    CHECK(SaveStateCodec::Decode(bytes, decoded) && writes == 0);
    std::vector<uint8_t> again;
    CHECK(SaveStateCodec::Encode(decoded, again) && again == bytes);
    CHECK(decoded.storage.completion->committed && decoded.storage.completion->result.step == 5);
    const fs::path file = *temporary / "mc0/BASLUS-20909/Bank0.bin";
    { std::ofstream changed(file, std::ios::binary | std::ios::trunc); changed.write("\x09\x08\x07", 3); }
    CHECK(bindings.Restore(decoded, 0));
    const auto generation = bindings.Capture()->storage.generation;
    CHECK(bindings.Restore(decoded, 0));
    CHECK(bindings.Capture()->storage.generation > generation);
    CpuContext context{};
    tick = 0;
    bindings.Invoke(0x2a6b68, context);
    CHECK(DiskBytes(file) == (std::vector<uint8_t>{9, 8, 7}) && writes == 0);
    CHECK(!store.TakeCompletion()); // delivered once, durable write not replayed
    std::memcpy(ram.data() + 0x10000, "", 1);
    std::memcpy(ram.data() + 0x10100, "Bank0.bin", 10);
    context.cpuRegs.GPR.r[6].UL[0] = 0x10000;
    context.cpuRegs.GPR.r[7].UL[0] = 0x10100;
    context.cpuRegs.GPR.r[8].UL[0] = 0x80020000; // a valid alias, preserved exactly
    context.cpuRegs.GPR.r[9].UL[0] = 16;
    bindings.Invoke(0x2a8df0, context);
    CHECK(context.cpuRegs.GPR.r[2].SL[0] == 1);
    const auto pending = bindings.Capture();
    CHECK(pending && pending->storage.pending.operation == Op::Read && quiescence.Check());
    CHECK(SaveStateCodec::Encode(*pending, bytes) && SaveStateCodec::Decode(bytes, decoded));
    CHECK(decoded.outputs.data == 0x80020000 && decoded.outputs.size == 16);
    CHECK(bindings.Restore(decoded, 0));
    CHECK(store.Update(0, 0, 0) == Op::Checking && writes == 0);
    Finish(store, tick); // completion queued, not yet published by the adapter
    const auto completed = bindings.Capture();
    CHECK(completed && completed->storage.completion->bytes == (std::vector<uint8_t>{9, 8, 7}));
    CHECK(SaveStateCodec::Encode(*completed, bytes) && SaveStateCodec::Decode(bytes, decoded));
    CHECK(writes == 0);
    fs::remove(file); // completed bytes are owned; decoding/restoring never reopens
    CHECK(bindings.Restore(decoded, 0));
    context.cpuRegs.GPR.r[6].UL[0] = 0;
    bindings.Invoke(0x2a6b68, context);
    CHECK(writes == 1 && ram[0x20000] == 9 && ram[0x20002] == 7 && ram[0x20003] == 0xcd);
    bindings.Invoke(0x2a6b68, context);
    CHECK(writes == 1 && !fs::exists(file));
}
void SaveHostile() {
    SaveBindings::Snapshot original;
    std::vector<uint8_t> bytes;
    CHECK(SaveStateCodec::Encode(original, bytes));
    SaveBindings::Snapshot sentinel;
    sentinel.storage.generation = 93;
    for (size_t length = 0; length < bytes.size(); ++length) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(length));
        CHECK(!SaveStateCodec::Decode(truncated, sentinel) && sentinel.storage.generation == 93);
    }
    auto bad = bytes;
    bad.push_back(0);
    CHECK(!SaveStateCodec::Validate(bad));
    bad = bytes;
    Put32(bad, 13, UINT32_MAX); // region length: reject before allocation
    CHECK(!SaveStateCodec::Decode(bad, sentinel) && sentinel.storage.generation == 93);
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), bytes.size() - 5}) {
        bad = bytes;
        bad[offset] = 255;
        CHECK(!SaveStateCodec::Decode(bad, sentinel));
    }
    bad.assign(SaveStateCodec::kMaxPayloadBytes + 1, 0);
    CHECK(!SaveStateCodec::Validate(bad));
    const auto unchanged = bytes;
    original.storage.initialised = true;
    original.storage.region = "BA";
    original.storage.product = "SLUS-20909";
    original.storage.sequence = 1;
    original.storage.active = Op::Checking;
    original.storage.pending = Request(Op::Write, "Bank0.bin", 3);
    original.storage.pending.bytes = {1, 2, 3};
    CHECK(!SaveStateCodec::Encode(original, bytes) && bytes == unchanged);
    original.storage.pending = Request(Op::Read, "Bank0.bin", 4);
    original.outputs = {0, 0, 0x1fffffe, 4};
    CHECK(!SaveStateCodec::Encode(original, bytes)); // restoring cannot cross RAM end
    original.outputs = {0, 0, 0x10000, 4};
    CHECK(SaveStateCodec::Encode(original, bytes));
    bad = bytes;
    Put32(bad, bad.size() - 4, 3); // valid span, wrong pending output length
    CHECK(!SaveStateCodec::Decode(bad, sentinel));
    original.outputs.value = 0x20000; // a read cannot publish a different intent's scalar
    CHECK(!SaveStateCodec::Encode(original, bad));

    for (Op operation : {Op::Create, Op::Format, Op::Write, Op::Find, Op::Measure, Op::Read}) {
        SaveBindings::Snapshot state;
        state.storage.initialised = true;
        state.storage.region = "BA";
        state.storage.product = "SLUS-20909";
        state.storage.sequence = 7;
        state.storage.completion.emplace();
        auto& completion = *state.storage.completion;
        completion.operation = operation;
        completion.generation = state.storage.generation;
        completion.sequence = state.storage.sequence;
        completion.result = {1, operation == Op::Write ? 5 : 1, 0};
        state.storage.results[static_cast<unsigned>(operation)] = completion.result;
        completion.committed = operation == Op::Write || operation == Op::Format || operation == Op::Create;
        if (operation == Op::Measure) state.outputs.value = 0x10000;
        if (operation == Op::Find) {
            completion.entry_present = true;
            completion.entry.size = 0x10203;
            completion.entry.attributes = 0x8497;
            completion.entry.created = {{1, 2, 3, 4, 5, 6, 7, 8}};
            std::memcpy(completion.entry.name.data(), "Bank0.bin", 10);
            state.outputs.entry = 0x10000;
        }
        if (operation == Op::Read) {
            completion.bytes.assign(Store::kMaxBytes, 0xab);
            state.outputs = {0, 0, 0x10000, Store::kMaxBytes};
        }
        CHECK(SaveStateCodec::Encode(state, bytes));
        CHECK(SaveStateCodec::Decode(bytes, sentinel));
        CHECK(SaveStateCodec::Encode(sentinel, bad) && bad == bytes);
        CHECK(sentinel.storage.completion->committed == completion.committed);
        if (operation == Op::Find) {
            CHECK(sentinel.storage.completion->entry.created == completion.entry.created);
            CHECK(sentinel.storage.completion->entry.attributes == 0x8497);
        }
        if (operation == Op::Read) CHECK(sentinel.storage.completion->bytes.size() == Store::kMaxBytes);
        // Error codes are signed, retained, and must agree with the published result.
        completion.bytes.clear();
        completion.entry_present = false;
        completion.result = {0, 1, Store::kIoError};
        state.storage.results[static_cast<unsigned>(operation)] = completion.result;
        CHECK(SaveStateCodec::Encode(state, bytes) && SaveStateCodec::Decode(bytes, sentinel));
        CHECK(sentinel.storage.completion->result.error == Store::kIoError);
    }
}
void SectionPreflight() {
    using namespace ps2r::savestate;
    SectionRegistry registry;
    unsigned mutations = 0;
    SectionSpec input;
    input.tag = InputStateCodec::kTag;
    input.version = InputStateCodec::kVersion;
    input.owner = "Input";
    input.serialize = [](std::vector<uint8_t>& out) { CHECK(InputStateCodec::Encode(Input::Snapshot{}, out)); };
    input.validate = [](const Blob& blob, std::string* error) {
        return blob.version == InputStateCodec::kVersion && InputStateCodec::Validate(blob.payload, error);
    };
    input.restore = [&](const Blob&, std::string*) { ++mutations; return true; };
    SectionSpec saves;
    saves.tag = SaveStateCodec::kTag;
    saves.version = SaveStateCodec::kVersion;
    saves.owner = "SaveStorage";
    saves.serialize = [](std::vector<uint8_t>& out) { CHECK(SaveStateCodec::Encode(SaveBindings::Snapshot{}, out)); };
    saves.validate = [](const Blob& blob, std::string* error) {
        return blob.version == SaveStateCodec::kVersion && SaveStateCodec::Validate(blob.payload, error);
    };
    saves.restore = [&](const Blob&, std::string*) { ++mutations; return true; };
    CHECK(registry.Register(input) && registry.Register(saves));
    auto captured = registry.Capture();
    CHECK(captured.size() == 2);
    auto bad = captured;
    bad.back().payload.pop_back();
    // OLD CRC-valid truncated MCHL class: the file envelope can be valid while
    // the owned payload is corrupt. Reject before either owner's restore.
    const auto image = SerializeStateFile({}, bad);
    std::vector<Blob> parsed;
    std::string error;
    CHECK(ParseStateFile(image.data(), image.size(), nullptr, &parsed, &error));
    CHECK(!registry.Validate(parsed, &error) && !registry.Restore(parsed, &error) && mutations == 0);
    bad = captured;
    bad.back().version = 2;
    CHECK(!registry.Restore(bad, &error) && mutations == 0);
    CHECK(registry.Restore(captured, &error) && mutations == 2);
    CHECK(!registry.Validate({captured.front()}, &error));
    // Selection checks MUST inspect the original list: dropping unregistered
    // blobs would conceal a state written with a now-disabled service.
    SectionRegistry disabled;
    CHECK(!disabled.Validate(captured, &error));
    CHECK(disabled.Validate({}, &error)); // demonstrates why filtering first is insufficient
}
}
int main() {
    InputRoundTrip();
    InputHostile();
    SaveRoundTripAndPolicy();
    SaveHostile();
    SectionPreflight();
    std::printf("InputSaveStateCodecs: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
