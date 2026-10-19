#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "engine/core/savestate/savestate_format.h"

#include <algorithm>
#include <cstdio>

namespace {
using title::Input;
using title::InputStateCodec;
using ps2r::savestate::Blob;
unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (false)
void Put64(std::vector<uint8_t>& bytes, size_t at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (8 * i));
}
// Frozen INPT v1 wire layout, assembled from explicit old offsets. Only
// connection timers disappear; the new body retains every report/setup value.
std::vector<uint8_t> VersionOne(const Input::Snapshot& snapshot) {
    std::vector<uint8_t> current;
    CHECK(InputStateCodec::Encode(snapshot, current));
    std::vector<uint8_t> old(615);
    std::copy_n(current.begin(), 24, old.begin());
    old[4] = 1;
    old[40] = snapshot.sampled; old[42] = snapshot.paused; old[43] = 8;
    for (unsigned i = 0; i < 8; ++i) {
        const size_t from = 30 + 57 * i, to = 47 + 71 * i;
        const auto& pad = snapshot.pads[i];
        old[to] = pad.open;
        old[to + 1] = pad.connection == Input::State::Stable || pad.connection == Input::State::Busy;
        old[to + 2] = pad.sample.connected;
        std::copy_n(current.begin() + from + 5, 3, old.begin() + to + 3);
        std::copy_n(current.begin() + from + 8, 8, old.begin() + to + 14);
        std::copy_n(current.begin() + from + 16, 41, old.begin() + to + 30);
    }
    return old;
}
void PendingAndReports() {
    Input input;
    std::array<Input::Sample, 8> samples{};
    samples[0].connected = true;
    samples[0].buttons = 0xa05a;
    samples[0].lx = 17; samples[0].ry = 201;
    samples[0].has_pressure = true;
    samples[0].pressure = {{1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23}};
    CHECK(input.Open(0, 0) && input.SampleBoundary(100, samples));
    CHECK(input.EnterPressure(0, 0) && input.SampleBoundary(101, samples));
    CHECK(input.SetMode(0, 0, 0, 3));
    auto captured = input.Capture();
    captured.sequence = 71328;
    captured.pads[0].motors = {1, 213};
    std::vector<uint8_t> encoded;
    CHECK(InputStateCodec::Encode(captured, encoded));
    auto old = VersionOne(captured);
    Input::Snapshot decoded;
    std::string error;
    Blob section{InputStateCodec::kTag, 1, ps2r::savestate::kSectionRequired, old};
    CHECK(InputStateCodec::DecodeSection(section, decoded, &error));
    std::vector<uint8_t> migrated;
    CHECK(InputStateCodec::Encode(decoded, migrated) && migrated == encoded);
    CHECK(input.Restore(decoded, 0));
    CHECK(input.GetState(0, 0) == Input::State::Busy && input.Sequence() == captured.sequence);
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.GetRequest(0, 0) == Input::Request::Busy && input.Current(0, 0).report[1] == 0x79);
    CHECK(input.SampleBoundary(1, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable && input.Current(0, 0).report[1] == 0x41);
    section.version = 2; // outer and inner version cannot disagree
    decoded.sequence = 9182;
    CHECK(!InputStateCodec::DecodeSection(section, decoded, &error) && decoded.sequence == 9182);
    section.version = 1; section.payload = encoded;
    CHECK(!InputStateCodec::DecodeSection(section, decoded, &error) && decoded.sequence == 9182);
    section.payload = old; section.flags = 0;
    CHECK(!InputStateCodec::DecodeSection(section, decoded, &error));
    Put64(old, 32, UINT64_MAX); // hostile legacy readiness clock/bias
    CHECK(!InputStateCodec::Decode(old, decoded, &error) && decoded.sequence == 9182);
}
void OpeningMigration() {
    Input input;
    std::array<Input::Sample, 8> samples{};
    samples[0].connected = true;
    input.Open(0, 0);
    input.SampleBoundary(100, samples);
    auto state = input.Capture();
    state.pads[0].connection = Input::State::Finding;
    state.pads[0].request = Input::Request::Busy;
    auto old = VersionOne(state);
    Input::Snapshot decoded;
    CHECK(InputStateCodec::Decode(old, decoded));
    CHECK(input.Restore(decoded, 0) && input.GetState(0, 0) == Input::State::Finding);
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable); // accepted availability, no old timer
    Put64(old, 47 + 6, 96); // four boundaries since open in the old image
    CHECK(InputStateCodec::Decode(old, decoded));
    CHECK(input.Restore(decoded, 0) && input.GetState(0, 0) == Input::State::Disconnected);
    samples[0].connected = false;
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.GetState(0, 0) == Input::State::Disconnected); // migration never invents a device
    Put64(old, 47 + 22, UINT64_MAX);
    decoded.sequence = 4321;
    CHECK(!InputStateCodec::Decode(old, decoded) && decoded.sequence == 4321);
}
}
int main() {
    PendingAndReports();
    OpeningMigration();
    std::printf("InputVersion: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
