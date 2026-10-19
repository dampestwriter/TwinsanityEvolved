#include <algorithm>
#include <cstdio>
#include <vector>

#include "engine/pad/hle/pad_protocol.h"
#include "engine/pad/savestate/pads_section.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_legacy_import.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"

namespace {
using title::Input;
using title::InputLegacyImport;
int checks = 0, failures = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    ++failures; std::fprintf(stderr, "%d: %s\n", __LINE__, #expression); } } while (false)
constexpr uint32_t kArea = 0x100000, kTable = 0x3c6e90, kStatic = 0x3c6f80;
void Put(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
void OpenRam(std::vector<uint8_t>& ram, unsigned port, uint32_t area) {
    const uint32_t entry = kTable + port * 0x70;
    Put(ram, entry, area);
    Put(ram, entry + 4, kStatic + port * 0x80);
    Put(ram, entry + 0x10, 1);
}
std::vector<uint8_t> Payload(PadProtocol& protocol) {
    std::vector<uint8_t> bytes;
    PadsSection(protocol, {}).Serialize(bytes);
    return bytes;
}
PadProtocol MakeProtocol(std::vector<uint8_t>& ram) {
    return PadProtocol([&ram](uint32_t address, uint32_t value, int size) {
        for (int i = 0; i < size; ++i)
            ram[(address & 0x1fffffff) + i] = static_cast<uint8_t>(value >> (8 * i));
    });
}
void AllReportsAndClocks() {
    std::vector<uint8_t> ram(32 * 1024 * 1024);
    auto protocol = MakeProtocol(ram);
    protocol.RegisterArea(0, kArea, 0);
    OpenRam(ram, 0, kArea);
    PadProtocol::Input sample;
    sample.lx = 23; sample.ly = 48; sample.rx = 156; sample.ry = 222;
    std::string error;
    Input::Snapshot snapshot;
    for (uint64_t boundary = 1; boundary <= 215; ++boundary) {
        protocol.Stamp(sample, boundary);
        CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, boundary, snapshot, &error));
        Input input;
        CHECK(input.Restore(snapshot, 0));
        const auto expected = boundary < 8 ? Input::State::Disconnected :
            (boundary < 214 ? Input::State::Finding : Input::State::Stable);
        CHECK(input.GetState(0, 0) == expected);
        std::array<Input::Sample, 8> samples{};
        samples[0].connected = true;
        CHECK(input.SampleBoundary(1, samples));
        // Import preserves the recorded observation. Future availability
        // comes from the host provider, not an imported firmware timer.
        CHECK(input.GetState(0, 0) == Input::State::Stable);
    }
    for (uint32_t buttons = 0; buttons <= 0xffff; ++buttons) {
        sample.buttons = static_cast<uint16_t>(buttons);
        protocol.Stamp(sample, 216 + buttons);
        CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 216 + buttons, snapshot, &error));
        const auto report = Input::MakeReport(snapshot.pads[0].sample, snapshot.pads[0].analog,
                                              snapshot.pads[0].pressure);
        CHECK(std::equal(report.begin(), report.end(), ram.begin() + kArea + 128));
    }
    // OLD SELECT_IMPL: preserve actual fractional pressures as well as the
    // digital-derived current NEW reports; all 12 bytes are one report.
    for (unsigned i = 0; i < 12; ++i)
        ram[kArea + 8 + i] = ram[kArea + 136 + i] = static_cast<uint8_t>(17 + i * 13);
    CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 65751, snapshot, &error));
    for (unsigned i = 0; i < 12; ++i) CHECK(snapshot.pads[0].sample.pressure[i] == 17 + i * 13);

    // BUG-065/074: READY survives a source timestamp ahead of a rebased
    // boundary, and sequence derives from slot zero, not process boot.
    auto pads = Payload(protocol);
    Put(pads, 8, 123456);
    CHECK(InputLegacyImport::Decode(1, pads, ram, 0, snapshot, &error));
    Input restored;
    CHECK(restored.Restore(snapshot, 0));
    CHECK(restored.GetState(0, 0) == Input::State::Stable);
    CHECK(restored.Sequence() == protocol.Seq());
    // Obsolete OLD latch bytes must never introduce a two-read button hold.
    std::fill(pads.begin() + 26, pads.end(), 0xff);
    CHECK(InputLegacyImport::Decode(1, pads, ram, 0, snapshot, &error));
    CHECK(restored.Restore(snapshot, 0));
    std::array<Input::Sample, 8> released{};
    released[0].connected = true;
    CHECK(restored.SampleBoundary(1, released));
    Input::Report report;
    CHECK(restored.Read(0, 0, report) == 32);
    CHECK(report[2] == 0xff && report[3] == 0xff);
    CHECK(restored.Read(0, 0, report) == 32 && report[2] == 0xff);
}
void ReadinessMotorsAndSignedCounter() {
    std::vector<uint8_t> ram(32 * 1024 * 1024);
    auto protocol = MakeProtocol(ram);
    protocol.RegisterArea(0, kArea, 100);
    OpenRam(ram, 0, kArea);
    protocol.NoteReadinessEvent(109);
    protocol.Stamp({}, 110);
    Input::Snapshot snapshot;
    CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 110, snapshot));
    Input input;
    CHECK(input.Restore(snapshot, 0));
    std::array<Input::Sample, 8> samples{};
    samples[0].connected = true;
    CHECK(input.SampleBoundary(1, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);

    protocol.Stamp({}, 111);
    Put(ram, kStatic + 4, 1);
    Put(ram, kStatic + 8, 6);
    ram[kStatic + 12] = 1; ram[kStatic + 13] = 203;
    CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 111, snapshot));
    CHECK(snapshot.pads[0].motors.small == 1 && snapshot.pads[0].motors.big == 203);
    unsigned motor_calls = 0;
    Input::Motors last;
    Input with_motors([&](unsigned port, unsigned slot, Input::Motors motors) {
        CHECK(port == 0 && slot == 0); ++motor_calls; last = motors; return true;
    });
    CHECK(with_motors.Restore(snapshot, 0));
    CHECK(motor_calls == 0);
    CHECK(with_motors.SampleBoundary(1, samples));
    CHECK(motor_calls == 1 && last.small == 1 && last.big == 203);
    samples[0].connected = false;
    CHECK(with_motors.SampleBoundary(2, samples));
    CHECK(last.small == 0 && last.big == 0);
    samples[0].connected = true;
    CHECK(with_motors.SampleBoundary(3, samples));
    CHECK(with_motors.GetState(0, 0) == Input::State::Stable);

    for (uint32_t seq : {0x7fffffffu, 0xffffffffu}) {
        protocol.SetSeq(seq - 1);
        protocol.Stamp({}, 112);
        CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 112, snapshot));
        CHECK(snapshot.sequence == seq);
        CHECK(Input::ValidateSnapshot(snapshot));
    }
    protocol.RegisterArea(1, kArea + 256, 0);
    OpenRam(ram, 1, kArea + 256);
    protocol.SetSeq(1400);
    protocol.Stamp({}, 214);
    CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 214, snapshot));
    CHECK(snapshot.pads[0].connection == Input::State::Stable &&
          snapshot.pads[4].connection == Input::State::Stable && snapshot.sequence == 1401);
}
void Refusals() {
    std::vector<uint8_t> ram(32 * 1024 * 1024);
    auto protocol = MakeProtocol(ram);
    Input::Snapshot snapshot;
    CHECK(InputLegacyImport::Decode(1, Payload(protocol), ram, 0, snapshot)); // proven closed boot state
    protocol.RegisterArea(0, kArea, 0);
    OpenRam(ram, 0, kArea);
    protocol.Stamp({}, 214);
    const auto pads = Payload(protocol);
    std::string error;
    snapshot.sequence = 7654321;
    const auto reject = [&](uint32_t version, const auto& payload, const auto& memory, uint64_t clock) {
        CHECK(!InputLegacyImport::Decode(version, payload, memory, clock, snapshot, &error));
        CHECK(!error.empty() && snapshot.sequence == 7654321);
    };
    for (size_t size = 0; size < pads.size(); ++size)
        reject(1, std::vector<uint8_t>(pads.begin(), pads.begin() + size), ram, 214);
    auto invalid = pads;
    invalid.push_back(0); reject(1, invalid, ram, 214);
    reject(2, pads, ram, 214);
    reject(1, pads, std::vector<uint8_t>(1024), 214);
    reject(1, pads, ram, uint64_t{UINT32_MAX} + 1);
    invalid = pads; invalid[16] = 3; reject(1, invalid, ram, 214);
    invalid = pads; invalid[20] = 2; reject(1, invalid, ram, 214);
    invalid = pads; Put(invalid, 0, 0x1ffff80); reject(1, invalid, ram, 214);
    invalid = pads; Put(invalid, 0, kArea + 1); reject(1, invalid, ram, 214);
    Put(ram, kTable + 0x1c + 0x10, 1); reject(1, pads, ram, 214);
    Put(ram, kTable + 0x1c + 0x10, 0);
    ram[kArea + 128 + 0x71] = 2; reject(1, pads, ram, 214);
    ram[kArea + 128 + 0x71] = 0;
    ram[kArea + 2] ^= 1; reject(1, pads, ram, 214); // OLD torn-read phantom pin
    ram[kArea + 2] ^= 1;
    Put(ram, kArea + 0xd8, 87); reject(1, pads, ram, 214);
    protocol.Stamp({}, 215);
    Put(ram, kStatic + 4, 3); reject(1, pads, ram, 215);
    Put(ram, kStatic + 4, 0);
    protocol.SetSeq(UINT32_MAX);
    protocol.Stamp({}, 216); reject(1, pads, ram, 216); // zero seq cannot be represented
    protocol.RegisterArea(0, kArea, 100);
    protocol.Stamp({}, 108);
    reject(1, Payload(protocol), ram, 0); // no guessed opening age after a clock mismatch
}
}

int main() {
    AllReportsAndClocks();
    ReadinessMotorsAndSignedCounter();
    Refusals();
    std::printf("InputLegacyImport: %d checks (%d failures); SELF current NEW PADS/RAM reports\n", checks, failures);
    return failures ? 1 : 0;
}
