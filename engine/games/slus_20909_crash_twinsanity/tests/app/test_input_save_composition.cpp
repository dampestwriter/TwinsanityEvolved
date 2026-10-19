#include "games/slus_20909_crash_twinsanity/composition/input_save.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>

#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#if PS2R_PRODUCT_SAMPLING_TESTS
#include "games/slus_20909_crash_twinsanity/composition/input_sample.h"
#include "engine/pad/hle/pad_stamp_duty.h"
#include "engine/core/triage/input_log.h"
#endif

// Legacy import parses records; this fixture never executes generated bodies
// or claims a live caller capture.
extern "C" uint32_t ee_get_call_stack_depth() { return 0; }
extern "C" bool ee_get_call_stack_top(uint32_t, uint32_t*, uint32_t*) { return false; }
extern "C" bool ee_call_stack_push(uint32_t, uint32_t) { return false; }
extern "C" void ee_call_stack_pop() {}

using ps2r::app::InputSave;
using namespace ps2r::savestate;
static unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); } } while (false)

static std::vector<uint8_t> Bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

int main() {
    char directory[] = "/tmp/input_save_composition_XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    std::vector<uint8_t> memory(32u << 20);
    InputSave::Memory access{
        [&](uint32_t at, void* out, size_t size) {
            at &= 0x1fffffffu;
            if (at > memory.size() || size > memory.size() - at) return false;
            std::memcpy(out, memory.data() + at, size); return true;
        }, [&](uint32_t at, const void* in, size_t size) {
            at &= 0x1fffffffu;
            if (at > memory.size() || size > memory.size() - at) return false;
            std::memcpy(memory.data() + at, in, size); return true;
        }};
    uint64_t boundary = 0;
    unsigned motors = 0;
    bool motor_supported = false;
    int fail_write = 0;
    ::title::SaveStorage::Config config{directory};
    config.write_failure = [&](auto) { return fail_write; };
    InputSave services(InputSave::Checkpoint::kAll, access, config, [&] { return boundary; },
        [&](unsigned port, unsigned slot, auto) {
            CHECK(port == 0 && slot == 0); ++motors; return motor_supported;
        });
    std::map<uint32_t, ::title::InputBindings::Body> entries;
    CHECK(services.Install([&](uint32_t address, auto body) {
        CHECK(address != 0x177168); // no frontend/game-function readiness hook
        return entries.emplace(address, std::move(body)).second;
    }));
    CHECK(entries.size() == 27);
    auto call = [&](uint32_t address, std::initializer_list<uint32_t> args) {
        CpuContext context{};
        unsigned reg = 4;
        for (uint32_t arg : args) context.cpuRegs.GPR.r[reg++].UL[0] = arg;
        entries.at(address)(context, 0);
        return context.cpuRegs.GPR.r[2].SD[0];
    };
    // Main calls startup before it supplies the game save region/product.
    // This must never require the removed MCSERV registration.
    CHECK(call(0x2c5080, {}) == 0);
    CHECK(std::filesystem::is_directory(std::filesystem::path(directory) / "mc0"));
    CHECK(std::filesystem::is_directory(std::filesystem::path(directory) / "mc1_slot1"));
    CHECK(call(0x2c27c0, {0, 0, 0x1000}) == 1);
    CHECK(call(0x2c2c30, {0, 0}) == 0); // open alone never invents a device
    ::title::Input::Sample sample;
    sample.connected = true; sample.buttons = 0x8001; sample.rx = 91;
    boundary = 0;
    CHECK(services.NeedsSample(boundary) && services.Sample(boundary, sample));
    CHECK(call(0x2c2bb8, {0, 0, 0x3000}) == 32);
    CHECK(memory[0x3002] == 0xfe && memory[0x3003] == 0x7f && memory[0x3004] == 91);
    sample.buttons = 0;
    CHECK(!services.NeedsSample(boundary) && services.Sample(boundary, sample));
    CHECK(services.Current().sample.buttons == 0x8001); // no resampling same clock key
    memory[0x3100] = 1; memory[0x3101] = 127;
    CHECK(call(0x2c30d8, {0, 0, 0x3100}) == 0); // no fake rumble capability
    motor_supported = true;
    CHECK(call(0x2c30d8, {0, 0, 0x3100}) == 1);
    CHECK(motors >= 2);

    std::memcpy(memory.data() + 0x4000, "BA", 3);
    std::memcpy(memory.data() + 0x4020, "SLUS-20909", 11);
    std::memcpy(memory.data() + 0x4060, "Bank0.bin", 10);
    memory[0x4100] = 17; memory[0x4101] = 29;
    CHECK(call(0x2a8668, {0x4000, 0x4020}) == 1);
    auto finish = [&] {
        for (unsigned i = 0; i < 16; ++i) {
            ++boundary;
            if (call(0x2a6b68, {0, 0, 0x4200}) == 0) return true;
        }
        return false;
    };
    CHECK(call(0x2a8728, {0, 0, 0x4040}) == 1);
    std::string error;
    CHECK(!services.CanCheckpoint(&error)); // pending durable mutation
    CHECK(finish() && services.CanCheckpoint(&error));
    CHECK(call(0x2a8c28, {0, 0, 0x4040, 0x4060, 0x4100, 2}) == 1);
    CHECK(!services.CanCheckpoint(&error) && finish());
    const auto save = std::filesystem::path(directory) / "mc0/BASLUS-20909/Bank0.bin";
    CHECK(Bytes(save) == std::vector<uint8_t>({17, 29}));
    CHECK(call(0x2c5080, {}) == 0 && Bytes(save) == std::vector<uint8_t>({17, 29}));

    SectionRegistry registry;
    CHECK(services.Register(registry));
    const auto captured = registry.Capture();
    CHECK(registry.Validate(captured, &error));
    const auto before = SerializeStateFile({}, registry.Capture());
    auto missing = captured;
    missing.erase(std::remove_if(missing.begin(), missing.end(), [](const Blob& blob) {
        return blob.tag == ::title::SaveStateCodec::kTag;
    }), missing.end());
    CHECK(!registry.Restore(missing, &error));
    CHECK(before == SerializeStateFile({}, registry.Capture()));
    auto damaged = captured;
    damaged.back().payload.pop_back();
    CHECK(!registry.Restore(damaged, &error));
    CHECK(before == SerializeStateFile({}, registry.Capture()));
    auto dual = captured;
    dual.push_back({TAG_PADS, 1, 0, {}});
    CHECK(!registry.Restore(dual, &error));
    CHECK(before == SerializeStateFile({}, registry.Capture()));
    fail_write = ::title::SaveStorage::kIoError;
    memory[0x4100] = 99;
    CHECK(call(0x2a8c28, {0, 0, 0x4040, 0x4060, 0x4100, 2}) == 1);
    CHECK(finish());
    call(0x2a8638, {11, 0x4300});
    int32_t result[3]{};
    access.read(0x4300, result, sizeof(result));
    CHECK(result[0] == 0 && result[2] == ::title::SaveStorage::kIoError);
    CHECK(Bytes(save) == std::vector<uint8_t>({17, 29}));
    fail_write = 0;
    // Durable files remain current when an older reproduction is restored.
    { std::ofstream file(save, std::ios::binary); file.put(71); }
    boundary = 100;
    CHECK(registry.Restore(captured, &error));
    CHECK(Bytes(save) == std::vector<uint8_t>({71}));
    CHECK(services.Current().sample.buttons == 0x8001);
    CHECK(services.Current().connection == ::title::Input::State::Stable);
    sample.connected = false;
    CHECK(services.Sample(++boundary, sample));
    CHECK(services.Current().connection == ::title::Input::State::Disconnected);
    CHECK(call(0x2c30d8, {0, 0, 0x3100}) == 0);

    std::vector<Blob> output{{Tag('K','E','E','P'), 1, 0, {77}}};
    CHECK(!services.PrepareBoot({}, {}, output, error));
    CHECK(output.front().payload == std::vector<uint8_t>{77});
    InputSave model(0, access, config, [&] { return boundary; }, {});
    InputSave::Checkpoint::LegacyPolicy legacy;
    CHECK(!model.PrepareBoot({}, legacy, output, error));
    legacy.approved = true;
    CHECK(model.PrepareBoot({}, legacy, output, error));
    CHECK(output.size() == 1 && output.front().tag == InputSave::Checkpoint::kTag);
    CHECK(!services.PrepareBoot(output, legacy, output, error)); // marked selection never imports
    const std::string reproduction = std::string(directory) + "/owned.ps2st";
    FileMeta saved_meta; saved_meta.vsync_frame = 214;
    CHECK(WriteStateFile(reproduction, saved_meta, captured, &error));
    FileMeta imported_meta;
    imported_meta.vsync_frame = 71;
    std::vector<uint8_t> imported_bytes{42};
    CHECK(!services.PrepareBootFile(reproduction, {}, imported_meta, imported_bytes, error, {}));
    CHECK(!services.PrepareBootFile(reproduction, {}, imported_meta, imported_bytes, error,
        [&](const auto& meta, const auto& blobs, bool legacy_clock, std::string& why) {
            CHECK(meta.vsync_frame == 214 && blobs.size() == captured.size() && !legacy_clock);
            why = "Clock-envelope mismatch."; return false;
        }));
    CHECK(imported_meta.vsync_frame == 71 && imported_bytes == std::vector<uint8_t>{42});
    CHECK(services.PrepareBootFile(reproduction, {}, imported_meta, imported_bytes, error,
        [&](const auto&, const auto&, bool legacy_clock, std::string&) { CHECK(!legacy_clock); return true; }));
    CHECK(imported_meta.vsync_frame == 214);
    std::vector<Blob> imported;
    CHECK(ParseStateFile(imported_bytes.data(), imported_bytes.size(), nullptr, &imported, &error));
    CHECK(registry.Validate(imported, &error));
    CHECK(WriteStateFile(reproduction, saved_meta, {}, &error));
    CHECK(model.PrepareBootFile(reproduction, legacy, imported_meta, imported_bytes, error,
        [&](const auto&, const auto& blobs, bool legacy_clock, std::string&) {
            CHECK(blobs.empty() && legacy_clock); return true;
        }));
    MainMemory ram;
    Scratchpad spad;
    const auto mapped = InputSave::RamAccess(ram, spad);
    uint32_t word = 0x12345678, read = 0;
    CHECK(mapped.write(0x80000100, &word, sizeof(word)));
    CHECK(mapped.read(0x100, &read, sizeof(read)) && read == word);
    CHECK(!mapped.write(0x01ffffff, &word, sizeof(word)));
    // The title's main thread runs on the scratchpad (TR entry.cpp:8-9,83):
    // a stack buffer handed to an owned entry is a scratchpad address, mapped
    // to the scratchpad bytes, never aliased into main RAM, bounded at 16 KB.
    uint32_t spad_word = 0xa1b2c3d4, spad_read = 0;
    CHECK(mapped.write(0x70003f00, &spad_word, sizeof(spad_word)));
    CHECK(mapped.read(0x70003f00, &spad_read, sizeof(spad_read)) && spad_read == spad_word);
    CHECK(std::memcmp(spad.Data() + 0x3f00, &spad_word, sizeof(spad_word)) == 0);
    CHECK(std::memcmp(ram.Data() + 0x3f00, &spad_word, sizeof(spad_word)) != 0);
    CHECK(!mapped.write(0x70003ffe, &spad_word, sizeof(spad_word)));
    CHECK(!mapped.read(0x70003ffd, &spad_read, sizeof(spad_read)));   // crosses the 16 KB end
    CHECK(::title::SaveBindings::RamRange(0x70003ff4, 12) && !::title::SaveBindings::RamRange(0x70003ff8, 12));
    CHECK(::title::InputBindings::RamRange(0x70000000, 32) && !::title::InputBindings::RamRange(0, 1));
    const auto blocked = std::filesystem::path(directory) / "blocked";
    { std::ofstream file(blocked); file << "occupied by a file"; }
    ::title::SaveStorage failed_store({blocked});
    ::title::SaveBindings failed_start(failed_store, {access.read, access.write, [&] { return boundary; }});
    CpuContext startup{};
    failed_start.Invoke(0x2c5080, startup);
    CHECK(startup.cpuRegs.GPR.r[2].SD[0] < 0 && failed_start.Counts().start_failures == 1);
    CHECK(!failed_store.Capture()->initialised);
    CHECK(!std::filesystem::exists(blocked / "mc0"));
#if PS2R_PRODUCT_SAMPLING_TESTS
    const std::string input_path = std::string(directory) + "/input.padlog";
    { std::ofstream file(input_path); file << "400-400:START\n"; }
    ps2r::triage::InputLog replay;
    CHECK(replay.LoadFile(input_path, &error));
    uint64_t protocol_writes = 0;
    PadProtocol protocol([&](uint32_t, uint32_t, int) { ++protocol_writes; });
    protocol.RegisterArea(0, 0x1800, 0); protocol.NoteReadinessEvent(0);
    PadStampDuty stamp(protocol, &replay, nullptr);
    ::title::Input::Sample selected, reference;
    CHECK(ps2r::app::SampleInput(services, nullptr, protocol, stamp, replay, 400, 400, selected));
    CHECK(protocol.Stamps() == 0 && protocol_writes == 0 && selected.buttons == 8);
    CHECK(ps2r::app::SampleInput(model, nullptr, protocol, stamp, replay, 400, 400, reference));
    CHECK(protocol.Stamps() == 1 && protocol_writes > 0);
    CHECK(selected.buttons == reference.buttons && selected.lx == reference.lx && selected.rx == reference.rx);
    replay = {};
    CHECK(ps2r::app::SampleInput(services, nullptr, protocol, stamp, replay, 400, 400, selected));
    CHECK(selected.buttons == 8 && protocol.Stamps() == 1); // same key never consumes replacement input
    CHECK(!ps2r::app::SampleInput(services, nullptr, protocol, stamp, replay, 399, 399, selected));
#endif
    std::filesystem::remove_all(directory);
    std::printf("InputSaveComposition: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
