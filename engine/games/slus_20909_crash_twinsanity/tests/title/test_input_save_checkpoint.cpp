#include <algorithm>
#include <cstdio>
#include <utility>

#include "engine/core/savestate/section_registry.h"
#include "engine/pad/hle/pad_protocol.h"
#include "engine/pad/savestate/pads_section.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_save_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"

namespace {
using title::Input;
using title::InputStateCodec;
using title::SaveBindings;
using title::SaveStateCodec;
using Checkpoint = title::InputSaveCheckpoint;
using namespace ps2r::savestate;
int checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::fprintf(stderr, "%d: %s\n", __LINE__, #condition); } } while (false)
void Put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}
Blob Marker(uint32_t mask) {
    Blob blob{Checkpoint::kTag, Checkpoint::kVersion, kSectionRequired, {}};
    CHECK(Checkpoint::EncodeSelection(mask, blob.payload));
    return blob;
}
std::vector<Blob> ServiceFile(uint32_t mask) {
    std::vector<Blob> blobs{Marker(mask)};
    if (mask & Checkpoint::kInput) {
        Input input;
        input.Open(0, 0);
        std::array<Input::Sample, 8> samples{};
        samples[0].connected = true;
        samples[0].buttons = 0x8009;
        input.SampleBoundary(214, samples);
        input.EnterPressure(0, 0); // pending command must survive pure preparation
        Blob blob{InputStateCodec::kTag, InputStateCodec::kVersion, kSectionRequired, {}};
        CHECK(InputStateCodec::Encode(input.Capture(), blob.payload));
        blobs.push_back(std::move(blob));
    }
    if (mask & Checkpoint::kSaveStorage) {
        SaveBindings::Snapshot snapshot;
        snapshot.storage.initialised = true;
        snapshot.storage.region = "BA";
        snapshot.storage.product = "SLUS-20909";
        snapshot.storage.results[10] = {0, 1, -4}; // preserve a failed read, not default success
        Blob blob{SaveStateCodec::kTag, 1, kSectionRequired, {}};
        CHECK(SaveStateCodec::Encode(snapshot, blob.payload));
        blobs.push_back(std::move(blob));
    }
    return blobs;
}
Checkpoint::Plan Sentinel() {
    Checkpoint::Plan plan;
    plan.selection = 3;
    plan.legacy_file = plan.imported_input = plan.imported_saves = true;
    plan.input.emplace();
    plan.input->sequence = 7171;
    plan.saves.emplace();
    plan.saves->storage.generation = 8181;
    return plan;
}
std::vector<uint8_t> PlanBytes(const Checkpoint::Plan& plan) {
    std::vector<uint8_t> bytes{static_cast<uint8_t>(plan.selection), uint8_t(plan.legacy_file),
        uint8_t(plan.imported_input), uint8_t(plan.imported_saves),
        uint8_t(plan.input.has_value()), uint8_t(plan.saves.has_value())};
    std::vector<uint8_t> payload;
    if (plan.input) {
        CHECK(InputStateCodec::Encode(*plan.input, payload));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    if (plan.saves) {
        CHECK(SaveStateCodec::Encode(*plan.saves, payload));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    return bytes;
}
std::vector<Blob> ParseValidFile(const std::vector<Blob>& blobs) {
    FileMeta meta;
    meta.vsync_frame = 9999; // deliberately not OLD's saved SyncV clock
    const auto image = SerializeStateFile(meta, blobs);
    std::vector<Blob> parsed;
    std::string error;
    CHECK(ParseStateFile(image.data(), image.size(), nullptr, &parsed, &error));
    CHECK(error.empty());
    return parsed;
}
void Rejected(const std::vector<Blob>& blobs, uint32_t mask, const char* reason,
              const Checkpoint::LegacyPolicy& legacy = {}) {
    const auto parsed = ParseValidFile(blobs); // including intentionally corrupt section semantics
    auto plan = Sentinel();
    const auto before = PlanBytes(plan);
    std::string error;
    CHECK(!Checkpoint::Prepare(parsed, mask, legacy, plan, &error));
    if (error.find(reason) == std::string::npos)
        std::fprintf(stderr, "expected '%s', got '%s'\n", reason, error.c_str());
    CHECK(error.find(reason) != std::string::npos);
    CHECK(PlanBytes(plan) == before);
}
void MarkerEncoding() {
    const auto marker = Marker(3);
    CHECK(marker.payload == std::vector<uint8_t>({'I', 'S', 'C', 'P', 1, 0, 0, 0, 3, 0, 0, 0}));
    uint32_t mask = 0;
    CHECK(Checkpoint::DecodeSelection(marker, mask) && mask == 3);
    std::vector<uint8_t> unchanged{0xaa, 0xbb};
    CHECK(!Checkpoint::EncodeSelection(4, unchanged));
    CHECK(unchanged == std::vector<uint8_t>({0xaa, 0xbb}));
    const auto reject = [&](const Blob& blob) {
        mask = 12345;
        std::string error;
        CHECK(!Checkpoint::DecodeSelection(blob, mask, &error));
        CHECK(mask == 12345 && !error.empty());
    };
    for (size_t size = 0; size < marker.payload.size(); ++size) {
        auto bad = marker;
        bad.payload.resize(size);
        reject(bad);
    }
    auto bad = marker; bad.payload.push_back(0); reject(bad);
    bad = marker; bad.flags = 0; reject(bad);
    bad = marker; bad.flags = 3; reject(bad);
    bad = marker; bad.tag ^= 1; reject(bad);
    bad = marker; bad.version = 2; reject(bad);
    bad = marker; bad.payload[0] ^= 1; reject(bad);
    bad = marker; Put32(bad.payload, 4, 2); reject(bad);
    bad = marker; Put32(bad.payload, 8, 4); reject(bad);
}
void SelectionMatrix() {
    for (uint32_t saved = 0; saved <= 3; ++saved) {
        const auto blobs = ServiceFile(saved);
        const auto parsed = ParseValidFile(blobs);
        for (uint32_t selected = 0; selected <= 3; ++selected) {
            auto plan = Sentinel();
            const auto before = PlanBytes(plan);
            std::string error;
            const bool accepted = Checkpoint::Prepare(parsed, selected, {}, plan, &error);
            CHECK(accepted == (saved == selected));
            if (!accepted) {
                CHECK(error.find("selections differ") != std::string::npos);
                CHECK(PlanBytes(plan) == before);
                continue;
            }
            CHECK(error.empty() && plan.selection == selected);
            CHECK(!plan.legacy_file && !plan.imported_input && !plan.imported_saves);
            CHECK(plan.input.has_value() == bool(selected & Checkpoint::kInput));
            CHECK(plan.saves.has_value() == bool(selected & Checkpoint::kSaveStorage));
            if (plan.input) {
                CHECK(plan.input->pads[0].pending == Input::Command::Pressure);
                CHECK(plan.input->pads[0].sample.buttons == 0x8009);
            }
            if (plan.saves) CHECK(plan.saves->storage.results[10].error == -4);
            auto reordered = parsed;
            std::reverse(reordered.begin(), reordered.end());
            Checkpoint::Plan other;
            CHECK(Checkpoint::Prepare(reordered, selected, {}, other));
            CHECK(PlanBytes(plan) == PlanBytes(other));
        }
    }
    Rejected(ServiceFile(0), 4, "unknown");
}
void MissingAndAmbiguous() {
    Checkpoint::LegacyPolicy approved{true, Checkpoint::Clock::OldSyncV, 0};
    auto blobs = ServiceFile(3);
    blobs.erase(blobs.begin() + 1); // INPT removed; recomputed CRCs still parse
    Rejected(blobs, 3, "INPT missing", approved);
    blobs = ServiceFile(3);
    blobs.pop_back();
    Rejected(blobs, 3, "SVST missing", approved);
    blobs = ServiceFile(3);
    blobs.erase(blobs.begin());
    Rejected(blobs, 3, "without required ISCP", approved);
    Rejected({}, 3, "provenance");
    Rejected({}, 0, "provenance"); // both-off files also need a marker or explicit legacy provenance
    Rejected({}, 3, "RAM0, MCHL, SIFS and THRD are all required", approved);
    Rejected({Marker(3)}, 3, "INPT missing", approved); // approved never downgrades a marked file

    blobs = ServiceFile(1); blobs.front() = Marker(0);
    blobs[1].flags = 0; // optional unregistered data must also reject when deselected
    Rejected(blobs, 0, "INPT present while Input is deselected");
    blobs = ServiceFile(2); blobs.front() = Marker(0); blobs[1].flags = 0;
    Rejected(blobs, 0, "SVST present while SaveStorage is deselected");
    blobs = ServiceFile(1); blobs[1].flags = 0;
    Rejected(blobs, 1, "REQUIRED");
    blobs = ServiceFile(2); blobs[1].version = 2;
    Rejected(blobs, 2, "version 1");
    blobs = ServiceFile(1); blobs[0].flags = 0;
    Rejected(blobs, 1, "ISCP requires");
    blobs = ServiceFile(3); blobs.back().payload.resize(17);
    Rejected(blobs, 3, "SaveStorage:"); // first successful decode never leaks into output Plan
    blobs = ServiceFile(3); blobs[1].payload.pop_back();
    Rejected(blobs, 3, "Input:");
    blobs = ServiceFile(3); blobs.push_back(blobs.front());
    Rejected(blobs, 3, "duplicate");
    blobs = ServiceFile(1); blobs.push_back(blobs.back());
    Rejected(blobs, 1, "duplicate");
    blobs = ServiceFile(1); blobs.push_back({TAG_PADS, 1, 0, std::vector<uint8_t>(166)});
    Rejected(blobs, 1, "retired owner");
    blobs = ServiceFile(2); blobs.push_back({TAG_MCHL, 1, 0, std::vector<uint8_t>(12)});
    Rejected(blobs, 2, "retired owner");
    blobs = ServiceFile(0);
    blobs.push_back({Tag('T', 'E', 'S', 'T'), 1, 0, {}});
    blobs.push_back(blobs.back());
    Rejected(blobs, 0, "duplicate");
}
std::vector<Blob> LegacyFile() {
    std::vector<uint8_t> ram(32 * 1024 * 1024);
    constexpr uint32_t area = 0x100000, table = 0x3c6e90;
    Put32(ram, table, area);
    Put32(ram, table + 4, 0x3c6f80);
    Put32(ram, table + 0x10, 1);
    PadProtocol protocol([&](uint32_t address, uint32_t value, int size) {
        for (int i = 0; i < size; ++i) ram.at(address + i) = static_cast<uint8_t>(value >> (i * 8));
    });
    protocol.RegisterArea(0, area, 256);
    protocol.Stamp({}, 264); // still finding; wrong clock changes/rejects staging
    std::vector<uint8_t> pads;
    PadsSection(protocol, {}).Serialize(pads);
    std::vector<uint8_t> counters(40);
    Put32(counters, 0, 7777); // present counter, not the pad clock
    Put32(counters, 28, 264);
    return {{TAG_RAM, 1, kSectionRequired, std::move(ram)},
            {TAG_PADS, 1, kSectionRequired, std::move(pads)},
            {TAG_CNTR, 1, kSectionRequired, std::move(counters)}};
}
void LegacyPolicyAndClocks() {
    auto blobs = LegacyFile();
    const auto parsed = ParseValidFile(blobs);
    Checkpoint::LegacyPolicy policy{true, Checkpoint::Clock::OldSyncV, 8888};
    Checkpoint::Plan plan;
    CHECK(Checkpoint::Prepare(parsed, 1, policy, plan));
    CHECK(plan.legacy_file && plan.imported_input && plan.input && !plan.saves);
    CHECK(plan.input->boundary == 264);
    CHECK(plan.input->pads[0].connection == Input::State::Finding);
    CHECK(Checkpoint::Prepare(parsed, 0, policy, plan));
    CHECK(plan.legacy_file && !plan.imported_input && !plan.input && !plan.saves);
    policy.clock = Checkpoint::Clock::NewBoundary;
    policy.boundary = 265;
    CHECK(Checkpoint::Prepare(parsed, 1, policy, plan));
    CHECK(plan.input->boundary == 265);
    policy.clock = Checkpoint::Clock::Unavailable;
    Rejected(blobs, 1, "clock is not established", policy);
    policy.clock = Checkpoint::Clock::OldSyncV;
    blobs[2].payload.resize(39);
    Rejected(blobs, 1, "CNTR v1", policy);
    blobs[2].payload.resize(40);
    blobs[2].version = 2;
    Rejected(blobs, 1, "CNTR v1", policy);
    blobs.pop_back();
    Rejected(blobs, 1, "CNTR v1", policy);
    policy.clock = Checkpoint::Clock::NewBoundary;
    policy.boundary = 264;
    CHECK(Checkpoint::Prepare(blobs, 1, policy, plan)); // explicit NEW clock needs no CNTR
    policy.boundary = uint64_t{UINT32_MAX} + 1;
    Rejected(blobs, 1, "source clock exceeds", policy);
    policy.boundary = 264;
    blobs[0].version = 2;
    Rejected(blobs, 1, "RAM0 version", policy);
    blobs[0].version = 1;
    blobs[1].version = 2;
    Rejected(blobs, 1, "PADS v1", policy);
    blobs.pop_back();
    Rejected(blobs, 1, "PADS and RAM0", policy);
    blobs.clear();
    blobs.push_back({TAG_MCHL, 1, kSectionRequired, {6, 0, 0, 0, 32, 0, 0, 0, 1, 0, 0, 0}});
    Rejected(blobs, 2, "RAM0, MCHL, SIFS and THRD are all required", policy);
}
void PreflightBeforeCallbacks() {
    // OLD E2_SAVE: semantic failures with valid CRCs must stop before teardown
    // or any absent/reset handler. Registry setup intentionally cannot prove
    // selection on its own; this is the parent's required call ordering.
    unsigned teardown = 0, restored = 0, absent_reset = 0;
    SectionRegistry registry;
    for (uint32_t tag : {Checkpoint::kTag, InputStateCodec::kTag, SaveStateCodec::kTag}) {
        SectionSpec spec;
        spec.tag = tag; spec.required = false; spec.owner = "test composition";
        spec.restore = [&](const Blob&, std::string*) { ++restored; return true; };
        spec.absent = [&] { ++absent_reset; };
        registry.Register(spec);
    }
    const auto load = [&](const std::vector<Blob>& blobs, uint32_t selected) {
        const auto raw = ParseValidFile(blobs);
        Checkpoint::Plan plan;
        std::string error;
        if (!Checkpoint::Prepare(raw, selected, {}, plan, &error) || !registry.Validate(raw, &error)) return false;
        ++teardown;
        return registry.Restore(raw, &error);
    };
    auto missing = ServiceFile(3); missing.pop_back();
    CHECK(registry.Validate(missing, nullptr)); // optional specs alone would accept missing SVST
    CHECK(!load(missing, 3));
    CHECK(!load(ServiceFile(3), 0));
    auto corrupt = ServiceFile(3); corrupt.back().payload.resize(1);
    CHECK(!load(corrupt, 3));
    CHECK(teardown == 0 && restored == 0 && absent_reset == 0);
    // Other tags are preserved for the strict owner check; the helper cannot
    // know another service's ownership/codec.
    auto unknown = ServiceFile(3);
    unknown.push_back({Tag('N', 'E', 'W', '!'), 1, kSectionRequired, {}});
    Checkpoint::Plan plan;
    CHECK(Checkpoint::Prepare(unknown, 3, {}, plan));
    CHECK(!load(unknown, 3));
    CHECK(teardown == 0 && restored == 0 && absent_reset == 0);
    CHECK(load(ServiceFile(3), 3));
    CHECK(teardown == 1 && restored == 3 && absent_reset == 0);
}
}

int main() {
    MarkerEncoding();
    SelectionMatrix();
    MissingAndAmbiguous();
    LegacyPolicyAndClocks();
    PreflightBeforeCallbacks();
    std::printf("InputSaveCheckpoint: %d checks, %d failures (CRC-valid selection and migration preflight)\n",
                checks, failures);
    return failures ? 1 : 0;
}
