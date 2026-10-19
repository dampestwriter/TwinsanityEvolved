#include "games/slus_20909_crash_twinsanity/hle/input/input.h"

#include <algorithm>
#include <utility>

namespace title {
namespace {
constexpr unsigned kPressureBits[] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
bool Ready(const Input::PadState& pad) {
    return pad.connection == Input::State::Stable || pad.connection == Input::State::Busy;
}
void Mark(Input::Milestone& milestone, uint64_t boundary) {
    if (milestone.reached) return;
    milestone.reached = true;
    milestone.boundary = boundary;
}
}

Input::Input(MotorOutput motors) : output_(std::move(motors)) {}
Input::PadState* Input::Pad(unsigned port, unsigned slot) {
    return port < kPorts && slot < kSlots ? &state_.pads[port * kSlots + slot] : nullptr;
}
const Input::PadState* Input::Pad(unsigned port, unsigned slot) const {
    return port < kPorts && slot < kSlots ? &state_.pads[port * kSlots + slot] : nullptr;
}
bool Input::IsOpen(unsigned port, unsigned slot) const {
    const auto* pad = Pad(port, slot);
    return pad && pad->open;
}
int32_t Input::Open(unsigned port, unsigned slot) {
    auto* pad = Pad(port, slot);
    if (!pad) return 0;
    Stop(port * kSlots + slot, true);
    *pad = PadState{};
    pad->open = true;
    auto& milestones = milestones_[port * kSlots + slot];
    milestones = Milestones{};
    Mark(milestones.opened, state_.boundary);
    return static_cast<int32_t>(port * 2 + slot + 1);
}
int32_t Input::Close(unsigned port, unsigned slot) {
    auto* pad = Pad(port, slot);
    if (!pad || !pad->open) return 0;
    Stop(port * kSlots + slot, true);
    *pad = PadState{};
    return 1;
}
bool Input::SampleBoundary(uint64_t boundary, const std::array<Sample, kPorts * kSlots>& samples) {
    if ((state_.sampled && boundary <= state_.boundary) || state_.sequence == UINT64_MAX) return false;
    state_.boundary = boundary;
    state_.sampled = true;
    ++state_.sequence;
    for (unsigned i = 0; i < state_.pads.size(); ++i) {
        auto& pad = state_.pads[i];
        if (!pad.open) continue;
        pad.sample = samples[i];
        if (!pad.sample.connected) {
            Stop(i, true);
            pad.connection = State::Disconnected;
            pad.pending = Command::None;
            pad.request = Request::Failed;
            continue;
        }
        // A coherent accepted host sample proves availability. The game owns
        // reconnect/setup delays (game/pads.cpp); no frontend hook or firmware
        // connection timer belongs here. Restored setup stays pending.
        if (!Ready(pad)) {
            pad.connection = State::Stable;
            pad.request = Request::Complete;
            Mark(milestones_[i].stable, boundary);
        }
        if (pad.pending != Command::None && boundary > pad.command_at) {
            if (pad.pending == Command::Mode) {
                pad.analog = pad.next_analog;
                pad.locked = pad.next_lock;
            } else if (pad.pending == Command::Align) {
                pad.alignment = pad.next_alignment;
            } else {
                pad.pressure = true;
                Mark(milestones_[i].setup, boundary);
            }
            pad.pending = Command::None;
            pad.request = Request::Complete;
            pad.connection = State::Stable;
        }
        if (restore_motors_ && Ready(pad) && !state_.paused && output_)
            output_(i / kSlots, i % kSlots, pad.motors);
    }
    restore_motors_ = false;
    return true;
}
Input::Milestones Input::MilestonesOf(unsigned port, unsigned slot) const {
    return port < kPorts && slot < kSlots ? milestones_[port * kSlots + slot] : Milestones{};
}
Input::State Input::GetState(unsigned port, unsigned slot) const {
    const auto* pad = Pad(port, slot);
    return pad && pad->open ? pad->connection : State::Disconnected;
}
Input::Request Input::GetRequest(unsigned port, unsigned slot) const {
    const auto* pad = Pad(port, slot);
    return pad && pad->open ? pad->request : Request::Failed;
}
Input::Report Input::MakeReport(const Sample& sample, bool analog, bool pressure) {
    Report out{};
    out[1] = analog ? (pressure ? 0x79 : 0x73) : 0x41;
    const uint16_t buttons = static_cast<uint16_t>(~sample.buttons);
    out[2] = static_cast<uint8_t>(buttons);
    out[3] = static_cast<uint8_t>(buttons >> 8);
    out[4] = analog ? sample.rx : 128;
    out[5] = analog ? sample.ry : 128;
    out[6] = analog ? sample.lx : 128;
    out[7] = analog ? sample.ly : 128;
    if (analog && pressure) {
        for (unsigned i = 0; i < 12; ++i)
            out[8 + i] = sample.has_pressure ? sample.pressure[i] :
                ((sample.buttons & (1u << kPressureBits[i])) ? 255 : 0);
    }
    return out;
}
int32_t Input::Read(unsigned port, unsigned slot, Report& report) const {
    const auto reading = Current(port, slot);
    if (!reading.readable) return 0;
    report = reading.report;
    return static_cast<int32_t>(report.size());
}
Input::Reading Input::Current(unsigned port, unsigned slot) const {
    Reading reading;
    reading.boundary = state_.boundary;
    reading.sequence = state_.sequence;
    reading.connection = GetState(port, slot);
    const auto* pad = Pad(port, slot);
    if (!pad || !pad->open) return reading;
    reading.sample = pad->sample;
    reading.readable = Ready(*pad) && pad->sample.connected;
    if (reading.readable) reading.report = MakeReport(pad->sample, pad->analog, pad->pressure);
    return reading;
}
int32_t Input::InfoMode(unsigned port, unsigned slot, int32_t info, int32_t index) const {
    const auto* pad = Pad(port, slot);
    if (!pad || !Ready(*pad) || !pad->sample.connected) return 0;
    switch (info) {
        case 1: return pad->analog ? 7 : 4;
        case 2: return pad->analog ? 7 : 4;
        case 3: return pad->analog ? 1 : 0;
        case 4: return index == -1 ? 2 : (index == 0 ? 4 : (index == 1 ? 7 : 0));
        default: return 0;
    }
}
int32_t Input::InfoActuator(unsigned port, unsigned slot, int32_t actuator, int32_t term) const {
    const auto* pad = Pad(port, slot);
    if (!pad || !Ready(*pad) || !pad->sample.connected) return 0;
    if (actuator == -1) return 2;
    if (actuator < 0 || actuator > 1 || term < 1 || term > 4) return 0;
    constexpr int32_t descriptors[2][4] = {{0, 1, 1, 255}, {1, 1, 1, 255}};
    return descriptors[actuator][term - 1];
}
bool Input::Accept(PadState* pad, Command command) {
    if (!pad || !pad->open || !Ready(*pad) || !pad->sample.connected || pad->pending != Command::None) return false;
    pad->pending = command;
    pad->command_at = state_.boundary;
    pad->request = Request::Busy;
    pad->connection = State::Busy;
    return true;
}
int32_t Input::SetMode(unsigned port, unsigned slot, int32_t mode, int32_t lock) {
    if ((mode != 0 && mode != 1) || (lock != 2 && lock != 3)) return 0;
    auto* pad = Pad(port, slot);
    if (!Accept(pad, Command::Mode)) return 0;
    pad->next_analog = mode == 1;
    pad->next_lock = lock == 3;
    return 1;
}
int32_t Input::SetAlignment(unsigned port, unsigned slot, const std::array<uint8_t, 6>& alignment) {
    for (uint8_t value : alignment) if (value != 0 && value != 1 && value != 255) return 0;
    auto* pad = Pad(port, slot);
    if (!Accept(pad, Command::Align)) return 0;
    pad->next_alignment = alignment;
    return 1;
}
int32_t Input::EnterPressure(unsigned port, unsigned slot) {
    return Accept(Pad(port, slot), Command::Pressure) ? 1 : 0;
}
int32_t Input::SetMotors(unsigned port, unsigned slot, const std::array<uint8_t, 6>& values) {
    auto* pad = Pad(port, slot);
    if (!pad || !pad->open || !Ready(*pad) || !pad->sample.connected || !output_) return 0;
    Motors motors;
    for (unsigned i = 0; i < values.size(); ++i) {
        if (pad->alignment[i] == 0) motors.small = values[i] != 0;
        if (pad->alignment[i] == 1) motors.big = values[i];
    }
    if (!output_(port, slot, state_.paused ? Motors{} : motors)) return 0;
    pad->motors = motors;
    return 1;
}
void Input::Stop(unsigned index, bool clear) {
    auto& pad = state_.pads[index];
    if ((pad.motors.small || pad.motors.big) && output_) output_(index / kSlots, index % kSlots, {});
    if (clear) pad.motors = {};
}
void Input::Pause() {
    state_.paused = true;
    for (unsigned i = 0; i < state_.pads.size(); ++i) Stop(i, true);
}
void Input::Resume() { state_.paused = false; }
void Input::Reset() {
    for (unsigned i = 0; i < state_.pads.size(); ++i) Stop(i, true);
    state_ = Snapshot{};
    milestones_ = {};
    restore_motors_ = false;
}
bool Input::ValidateSnapshot(const Snapshot& snapshot, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (snapshot.version != 2 || snapshot.sequence == 0 || snapshot.sequence == UINT64_MAX)
        return fail("Input: invalid snapshot version or clock");
    for (const auto& pad : snapshot.pads) {
        if (static_cast<unsigned>(pad.pending) > static_cast<unsigned>(Command::Pressure) ||
            static_cast<unsigned>(pad.request) > static_cast<unsigned>(Request::Busy))
            return fail("Input: invalid pending command or request state");
        if (pad.connection != State::Disconnected && pad.connection != State::Finding &&
            pad.connection != State::Busy && pad.connection != State::Stable)
            return fail("Input: invalid connection state");
        if ((pad.connection != State::Disconnected && (!pad.open || !pad.sample.connected)) ||
            (pad.pending != Command::None && (pad.connection != State::Busy || pad.request != Request::Busy)) ||
            (pad.connection == State::Busy && pad.pending == Command::None) ||
            (pad.connection == State::Stable && pad.request != Request::Complete) ||
            pad.motors.small > 1 || (pad.command_at > snapshot.boundary && pad.pending != Command::None))
            return fail("Input: inconsistent device state or pending command");
        for (uint8_t value : pad.alignment)
            if (value != 0 && value != 1 && value != 255) return fail("Input: invalid motor alignment");
        for (uint8_t value : pad.next_alignment)
            if (value != 0 && value != 1 && value != 255) return fail("Input: invalid pending motor alignment");
    }
    if (error) error->clear();
    return true;
}
bool Input::RestoreSaved(const Snapshot& snapshot) {
    if (!ValidateSnapshot(snapshot)) return false;
    for (unsigned i = 0; i < state_.pads.size(); ++i) Stop(i, true);
    state_ = snapshot;
    restore_motors_ = true;
    return true;
}
bool Input::Restore(const Snapshot& snapshot, uint64_t new_boundary) {
    if (!RestoreSaved(snapshot)) return false;
    state_.boundary = new_boundary;
    state_.sampled = false;
    for (auto& pad : state_.pads) {
        pad.command_at = new_boundary;  // still busy at restored boundary
    }
    return true;
}

}  // namespace title
