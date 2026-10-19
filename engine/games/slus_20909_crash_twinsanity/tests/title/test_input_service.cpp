#include "games/slus_20909_crash_twinsanity/hle/input/input_bindings.h"
#include "engine/pad/hle/pad_protocol.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)
using title::Input;
using title::InputBindings;
void SelfOracle() {
    std::array<uint8_t, 256> old_area{};
    PadProtocol current([&](uint32_t address, uint32_t value, int size) {
        std::memcpy(old_area.data() + address - 0x10000, &value, static_cast<size_t>(size));
    });
    current.RegisterArea(0, 0x10000, 0);
    Input input;
    CHECK(input.Open(0, 0) == 1);
    std::array<Input::Sample, Input::kPorts * Input::kSlots> samples{};
    samples[0].connected = true;
    PadProtocol::Input reference;
    // Attributed intent difference: a real host sample is immediately ready;
    // retained PADMAN stages are not a host availability contract.
    unsigned connection_differences = 0;
    for (uint64_t tick = 0; tick <= 214; ++tick) {
        input.SampleBoundary(tick, samples);
        current.Stamp(reference, tick);
        CHECK(input.GetState(0, 0) == Input::State::Stable);
        if (static_cast<uint8_t>(input.GetState(0, 0)) != old_area[0x70]) ++connection_differences;
    }
    CHECK(connection_differences == 214);
    CHECK(input.EnterPressure(0, 0) == 1);
    input.SampleBoundary(215, samples);
    // All 16-bit button words, varying independent stick bytes; compare the
    // whole game-visible report, not just buttons or a checksum.
    for (uint32_t bits = 0; bits <= 0xffff; ++bits) {
        auto& sample = samples[0];
        sample.buttons = reference.buttons = static_cast<uint16_t>(bits);
        sample.lx = reference.lx = static_cast<uint8_t>(bits);
        sample.ly = reference.ly = static_cast<uint8_t>(bits >> 8);
        sample.rx = reference.rx = static_cast<uint8_t>(bits * 37 + 19);
        sample.ry = reference.ry = static_cast<uint8_t>((bits >> 4) * 113 + 7);
        input.SampleBoundary(bits + 216, samples);
        current.Stamp(reference, bits + 216);
        Input::Report report{};
        CHECK(input.Read(0, 0, report) == 32);
        CHECK(std::memcmp(report.data(), old_area.data(), report.size()) == 0);
    }
    std::printf("SELF Input: 65536 complete reports matched; 214 startup connection differences attributed to accepted host availability\n");
}
void CoherenceAndLifecycle() {
    // OLD R183 root2 / R186: the whole report and its connection state are
    // owned values. Sampling must never transiently publish zero/all-pressed.
    Input input;
    std::array<Input::Sample, 8> samples{};
    samples[0].connected = true;
    CHECK(input.Open(0, 0) == 1);
    CHECK(input.SampleBoundary(8, samples));
    CHECK(input.EnterPressure(0, 0) == 1);
    constexpr unsigned pressure_bits[] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
    for (unsigned bit = 0; bit < 16; ++bit) {
        samples[0].buttons = static_cast<uint16_t>(1u << bit);
        CHECK(input.SampleBoundary(9 + bit, samples));
        const auto served = input.Current(0, 0);
        CHECK(served.readable && served.sample.connected && served.connection == Input::State::Stable);
        CHECK(served.sample.buttons == samples[0].buttons);
        CHECK(static_cast<uint16_t>(served.report[2] | served.report[3] << 8) ==
              static_cast<uint16_t>(~samples[0].buttons));
        for (unsigned i = 0; i < 12; ++i) CHECK(served.report[8 + i] == (pressure_bits[i] == bit ? 255 : 0));
        Input::Report repeat{};
        for (unsigned read = 0; read < 3; ++read)
            CHECK(input.Read(0, 0, repeat) == 32 && repeat == served.report);
    }
    const auto previous = input.Current(0, 0);
    samples[0].buttons = 0;
    CHECK(input.SampleBoundary(25, samples));
    const auto released = input.Current(0, 0);
    CHECK(released.sample.buttons == 0 && released.report[2] == 255 && released.report[3] == 255);
    CHECK(previous.sample.buttons == 0x8000 && previous.report[15] == 255); // immutable copy
    CHECK(input.SampleBoundary(26, samples));
    CHECK(input.Sequence() == released.sequence + 1); // unchanged input still advances
    CHECK(input.SetMode(0, 0, 0, 2) == 1);
    const auto pending = input.Capture();
    CHECK(input.Restore(pending, 0));
    CHECK(input.GetRequest(0, 0) == Input::Request::Busy);
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.GetRequest(0, 0) == Input::Request::Busy); // not early after restore
    CHECK(input.SampleBoundary(1, samples));
    CHECK(input.Current(0, 0).report[1] == 0x41);
    samples[0].connected = false;
    CHECK(input.SampleBoundary(2, samples));
    CHECK(!input.Current(0, 0).sample.connected && !input.Current(0, 0).readable);
    input.Reset();
    CHECK(input.Sequence() == 1 && !input.IsOpen(0, 0));
    const auto cleared = input.MilestonesOf(0, 0);
    CHECK(!cleared.opened.reached && !cleared.stable.reached && !cleared.setup.reached);
    CHECK(input.Open(0, 0) == 1);
    CHECK(input.GetState(0, 0) == Input::State::Disconnected);
    samples[0].connected = true;
    CHECK(input.SampleBoundary(8, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    CHECK(input.SampleBoundary(9, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    CHECK(input.SampleBoundary(10, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    // Cold-boot attribution record: opened at the last accepted boundary before
    // PortOpen (0 after Reset), Stable at the first connected sample, setup at
    // the boundary the pressure request completes; kept by later samples.
    auto milestones = input.MilestonesOf(0, 0);
    CHECK(milestones.opened.reached && milestones.opened.boundary == 0);
    CHECK(milestones.stable.reached && milestones.stable.boundary == 8);
    CHECK(!milestones.setup.reached);
    CHECK(input.EnterPressure(0, 0) == 1);
    CHECK(input.SampleBoundary(11, samples));
    milestones = input.MilestonesOf(0, 0);
    CHECK(milestones.setup.reached && milestones.setup.boundary == 11);
    CHECK(input.SampleBoundary(12, samples));
    CHECK(input.MilestonesOf(0, 0).stable.boundary == 8 && input.MilestonesOf(0, 0).setup.boundary == 11);
    CHECK(!input.MilestonesOf(1, 0).opened.reached && !input.MilestonesOf(2, 0).opened.reached);
    CHECK(input.Open(0, 0) == 1);   // PortOpen restarts the record at the current boundary
    milestones = input.MilestonesOf(0, 0);
    CHECK(milestones.opened.reached && milestones.opened.boundary == 12 && !milestones.stable.reached);
}
}

int main() {
    std::vector<Input::Motors> motors;
    Input input([&](unsigned port, unsigned slot, Input::Motors value) {
        CHECK(port == 0 && slot == 0);
        motors.push_back(value);
        return true;
    });
    std::array<Input::Sample, Input::kPorts * Input::kSlots> samples{};
    samples[0].connected = true;
    samples[0].buttons = 0x4018; // Cross + Start + Up
    samples[0].lx = 17;
    samples[0].ry = 203;
    CHECK(input.Open(0, 0) == 1);
    CHECK(input.Open(2, 0) == 0);
    CHECK(input.GetState(0, 0) == Input::State::Disconnected);
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    CHECK(input.SampleBoundary(7, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    CHECK(input.SampleBoundary(8, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    CHECK(input.InfoMode(0, 0, 1, 0) == 7);
    CHECK(input.InfoMode(0, 0, 4, -1) == 2);
    CHECK(input.InfoMode(0, 0, 4, 1) == 7);
    CHECK(input.InfoActuator(0, 0, -1, 0) == 2);
    CHECK(input.EnterPressure(0, 0) == 1);
    CHECK(input.GetRequest(0, 0) == Input::Request::Busy);
    CHECK(input.GetState(0, 0) == Input::State::Busy);
    CHECK(input.EnterPressure(0, 0) == 0); // outstanding request is retained
    auto changed = samples;
    changed[0].buttons = 0;
    CHECK(!input.SampleBoundary(8, changed)); // no host-arrival resample
    CHECK(input.GetRequest(0, 0) == Input::Request::Busy);
    CHECK(input.SampleBoundary(9, samples));
    Input::Report report{};
    CHECK(input.Read(0, 0, report) == 32);
    CHECK(report[1] == 0x79 && report[2] == 0xe7 && report[3] == 0xbf);
    CHECK(report[6] == 17 && report[5] == 203);
    CHECK(report[10] == 255 && report[14] == 255 && report[8] == 0);
    samples[0].has_pressure = true;
    samples[0].pressure[6] = 91;
    CHECK(input.SampleBoundary(10, samples));
    CHECK(input.Read(0, 0, report) == 32 && report[14] == 91);
    CHECK(input.SetAlignment(0, 0, {{1, 0, 255, 255, 255, 255}}) == 1);
    CHECK(input.SampleBoundary(11, samples));
    CHECK(input.SetMotors(0, 0, {{123, 3, 0, 0, 0, 0}}) == 1);
    CHECK(motors.back().small == 1 && motors.back().big == 123);
    const auto snapshot = input.Capture();
    input.Pause();
    CHECK(motors.back().small == 0 && motors.back().big == 0);
    CHECK(input.Restore(snapshot, 0));
    CHECK(input.GetState(0, 0) == Input::State::Stable); // BUG-065
    CHECK(input.SampleBoundary(0, samples));
    CHECK(input.Sequence() == snapshot.sequence + 1); // BUG-074
    CHECK(motors.back().small == 1 && motors.back().big == 123);
    samples[0].connected = false;
    CHECK(input.SampleBoundary(1, samples));
    CHECK(input.GetState(0, 0) == Input::State::Disconnected);
    CHECK(input.Read(0, 0, report) == 0);
    CHECK(motors.back().small == 0 && motors.back().big == 0);
    samples[0].connected = true;
    CHECK(input.SampleBoundary(2, samples));
    CHECK(input.SampleBoundary(9, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    CHECK(input.SampleBoundary(10, samples));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    auto corrupt = input.Capture();
    corrupt.pads[0].pending = static_cast<Input::Command>(255);
    CHECK(!input.Restore(corrupt, 0));
    CHECK(input.Boundary() == 10);
    Input delayed;
    CHECK(delayed.Open(0, 0) == 1);
    CHECK(delayed.SampleBoundary(100, samples));
    CHECK(delayed.Restore(delayed.Capture(), 0));
    CHECK(delayed.SampleBoundary(113, samples));
    CHECK(delayed.GetState(0, 0) == Input::State::Stable);
    CHECK(delayed.SampleBoundary(114, samples));
    CHECK(delayed.GetState(0, 0) == Input::State::Stable);
    CHECK(delayed.SetMotors(0, 0, {{1, 2, 0, 0, 0, 0}}) == 0); // no fake rumble

    // Actual dispatch adapters: no RAM slots, SDK globals, or model required.
    std::vector<uint8_t> ram(4 * 1024 * 1024, 0xcc);
    InputBindings bindings(input, {
        [&](uint32_t a, void* b, size_t n) { if (a > ram.size() || n > ram.size() - a) return false;
            std::memcpy(b, ram.data() + a, n); return true; },
        [&](uint32_t a, const void* b, size_t n) { if (a > ram.size() || n > ram.size() - a) return false;
            std::memcpy(ram.data() + a, b, n); return true; },
    });
    std::map<uint32_t, InputBindings::Body> bodies;
    CHECK(bindings.Install([&](uint32_t a, InputBindings::Body body) { bodies[a] = std::move(body); return true; }));
    CHECK(bodies.size() == 17);
    CpuContext context{};
    context.cpuRegs.GPR.r[6].UL[0] = 0x10000;
    context.cpuRegs.GPR.r[29].UD[0] = 0x1f000;
    context.cpuRegs.GPR.r[31].UD[0] = 0xcafe;
    bodies.at(0x2c2bb8)(context, 0);
    CHECK(context.cpuRegs.GPR.r[2].SL[0] == 32);
    CHECK(ram[0x10002] == 0xe7 && ram[0x1000e] == 91);
    CHECK(ram[0x10020] == 0xcc && ram[0x3c6e90] == 0xcc);
    CHECK(context.cpuRegs.GPR.r[29].UD[0] == 0x1f000 && context.cpuRegs.GPR.r[31].UD[0] == 0xcafe);
    context.cpuRegs.GPR.r[6].UL[0] = 0x01fffff0;
    bodies.at(0x2c2bb8)(context, 0x2c2bb8);
    CHECK(context.cpuRegs.GPR.r[2].SL[0] == 0);
    bool rejected = false;
    try { bodies.at(0x2c2bb8)(context, 0x2c2bbc); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    CHECK(bodies.count(0x177168) == 0); // frontend body never observed or replaced
    // Validate before mutation: corrupt checkpoint fields cannot wrap clocks
    // or manufacture a pending command on a closed controller.
    corrupt = input.Capture();
    corrupt.sequence = UINT64_MAX;
    CHECK(!input.Restore(corrupt, 0));
    corrupt = input.Capture();
    corrupt.pads[0].alignment[0] = 19;
    CHECK(!input.Restore(corrupt, 0));
    corrupt = input.Capture();
    corrupt.pads[0].connection = static_cast<Input::State>(19);
    CHECK(!input.Restore(corrupt, 0));
    CHECK(input.GetState(0, 0) == Input::State::Stable);
    SelfOracle();
    CoherenceAndLifecycle();
    std::printf("InputService: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
