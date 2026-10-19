// Complete generated game callers, actual Input bindings, bounded allocator/
// libc fixture. This does not launch gameplay or execute a pad/card module.
#include "games/slus_20909_crash_twinsanity/tests/recomp/input_caller_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using title::Input;
unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "Input caller line %d: %s\n", __LINE__, #value); } } while (false)
constexpr uint32_t kInfo = 0x10000, kGp = 0x3114f0, kStack = 0x7f0000;
constexpr uint32_t kCreate = 0x2b3890, kDestroy = 0x2b3858, kUpdate = 0x2b2558, kMotors = 0x2b3998;
// NTSC ELF 8d114879...40a38f, data at 0x306a30 (36 bytes),
// sha256 28a2c2fb92119edf3c8c9528bd433e65000ac6626488335cb1814f72b2a12380.
// Same internal targets are explicitly recovered in the generated caller.
constexpr std::array<uint32_t, 9> kSetup = {
    0x2b2688, 0x2b2738, 0x2b27a8, 0x2b27d4, 0x2b2810, 0x2b2854, 0x2b2894, 0x2b2920, 0x2b2944};
struct Rig {
    std::vector<uint8_t> ram = std::vector<uint8_t>(8u << 20);
    std::map<uint32_t, title::InputBindings::Body> bindings;
    std::map<uint32_t, unsigned> calls;
    bool allocated = false, motor_supported = true;
    Input::Motors motors;
    unsigned motor_calls = 0;
    Input input{[this](unsigned port, unsigned slot, Input::Motors value) {
        CHECK(port == 0 && slot == 0);
        ++motor_calls;
        if (!motor_supported) return false;
        motors = value;
        return true;
    }};
    title::InputBindings adapter{input, {
        [this](uint32_t at, void* out, size_t size) { return Access(at, out, size, false); },
        [this](uint32_t at, const void* in, size_t size) { return Access(at, const_cast<void*>(in), size, true); }}};
    CpuContext context{};
    std::array<Input::Sample, 8> samples{};
    uint64_t tick = 0;

    Rig() {
        CHECK(adapter.Install([this](uint32_t at, auto body) {
            return bindings.emplace(at, std::move(body)).second;
        }));
        CHECK(bindings.size() == 17 && bindings.count(0x177168) == 0);
        CHECK(Access(0x306a30, const_cast<uint32_t*>(kSetup.data()), sizeof(kSetup), true));
        context.cpuRegs.GPR.r[28].UL[0] = kGp;
        context.cpuRegs.GPR.r[29].UL[0] = kStack;
        context.cpuRegs.GPR.r[31].UD[0] = 0x1122334455667788ull;
        for (unsigned i = 16; i <= 18; ++i) context.cpuRegs.GPR.r[i].UD[0] = 0x88776655ull + i;
        samples[0].connected = true;
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
    int32_t Call(uint32_t function, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {
        context.cpuRegs.GPR.r[4].UL[0] = a0;
        context.cpuRegs.GPR.r[5].UL[0] = a1;
        context.cpuRegs.GPR.r[6].UL[0] = a2;
        const auto before = context.cpuRegs.GPR;
        CHECK(input_callers::Run(function, context));
        CHECK(context.cpuRegs.GPR.r[29].UL[0] == kStack);
        CHECK(context.cpuRegs.GPR.r[31].UD[0] == before.r[31].UD[0]);
        for (unsigned i = 16; i <= 18; ++i)
            CHECK(context.cpuRegs.GPR.r[i].UD[0] == before.r[i].UD[0]);
        return context.cpuRegs.GPR.r[2].SL[0];
    }
    int32_t Boundary() {
        CHECK(input.SampleBoundary(tick++, samples));
        return Call(kUpdate, kInfo);
    }
};
Rig* current = nullptr;
}

namespace input_callers {
std::ostringstream g_logFile;
void Read(uint32_t address, void* out, size_t size) {
    if (!current || !current->Access(address, out, size, false)) throw std::runtime_error("Input caller read");
}
void Write(uint32_t address, const void* in, size_t size) {
    if (!current || !current->Access(address, const_cast<void*>(in), size, true))
        throw std::runtime_error("Input caller write");
}
void Dispatch(uint32_t address, CpuContext& context) {
    auto& rig = *current;
    ++rig.calls[address];
    if (const auto found = rig.bindings.find(address); found != rig.bindings.end()) {
        found->second(context, address);
        return;
    }
    const uint32_t first = context.cpuRegs.GPR.r[4].UL[0];
    if (address == 0x1815c0) {
        CHECK(first == 0xda0 && !rig.allocated);
        rig.allocated = true;
        context.cpuRegs.GPR.r[2].UL[0] = kInfo;
    } else if (address == 0x181590) {
        CHECK(first == kInfo && rig.allocated);
        rig.allocated = false;
    } else if (address == 0x2c6e88) {
        const auto size = context.cpuRegs.GPR.r[6].UL[0];
        CHECK(first == kInfo && size == 0x5a0);
        std::memset(rig.ram.data() + first, context.cpuRegs.GPR.r[5].UL[0] & 255, size);
        context.cpuRegs.GPR.r[2].UL[0] = first;
    } else throw std::runtime_error("Unexpected Input caller dependency " + std::to_string(address));
}
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t) {
    throw std::runtime_error("Controller setup escaped its source jump table");
}
namespace ps2_threading { [[noreturn]] void exitThread() { throw std::runtime_error("Controller caller exited"); } }
}

namespace {
void SetupAndReports() {
    Rig rig;
    current = &rig;
    CHECK(rig.Call(kCreate, 0) == static_cast<int32_t>(kInfo));
    CHECK(rig.input.IsOpen(0, 0) && rig.allocated);
    CHECK(rig.input.GetState(0, 0) == Input::State::Disconnected);
    for (unsigned i = 0; i < 16 && rig.adapter.Counts().reports == 0; ++i) rig.Boundary();
    CHECK(rig.adapter.Counts().reports > 0);
    CHECK(rig.calls[0x2c31a0] == 1 && rig.calls[0x2c3438] == 1);
    CHECK(rig.input.Capture().pads[0].pressure);
    CHECK(rig.Word(kInfo + 0x508) == 9); // unchanged game completed its own setup
    const auto report_count = rig.adapter.Counts().reports;
    for (unsigned bit = 0; bit < 16; ++bit) {
        rig.samples[0].buttons = static_cast<uint16_t>(1u << bit);
        CHECK(rig.Boundary() != 0);
        CHECK(rig.adapter.Counts().reports == report_count + bit + 1);
        // GamePadInformation numbers the report's first button byte high
        // (TR platform/pads.h ReportButtonsHigh=2), unlike the host bit word.
        const auto game_buttons = 1u << ((bit + 8) & 15);
        CHECK(rig.Word(kInfo + 0x560) == game_buttons);
        CHECK(rig.Word(kInfo + 0x564) == game_buttons);
        const auto report = rig.input.Current(0, 0).report;
        CHECK(std::equal(report.begin(), report.end(), rig.ram.begin() + kInfo + 0x574));
    }
    rig.samples[0].buttons = 0;
    rig.Boundary();
    CHECK(rig.Word(kInfo + 0x560) == 0);
    rig.Call(kMotors, kInfo, 1, 193);
    CHECK(rig.motor_calls && rig.motors.small == 1 && rig.motors.big == 193);
    rig.motor_supported = false;
    rig.Call(kMotors, kInfo, 0, 17);
    CHECK(rig.input.Capture().pads[0].motors.big == 193); // failed provider cannot replace intent
    rig.motor_supported = true;
    const auto captured = rig.input.Capture();
    const auto game = rig.ram;
    rig.samples[0].connected = false;
    CHECK(rig.Boundary() == 0 && rig.input.GetState(0, 0) == Input::State::Disconnected);
    CHECK(rig.Word(kGp - 0x6ddc) == 60 && rig.motors.big == 0);
    rig.samples[0].connected = true;
    rig.Boundary();
    CHECK(rig.Word(kGp - 0x6ddc) == 59); // game owns the reconnect delay
    // Reconstruct actual game bytes and service state, no restart of setup.
    rig.ram = game;
    std::vector<uint8_t> bytes;
    CHECK(title::InputStateCodec::Encode(captured, bytes));
    Input::Snapshot decoded;
    CHECK(title::InputStateCodec::Decode(bytes, decoded) && rig.input.Restore(decoded, 0));
    rig.tick = 0;
    const auto alignments = rig.calls[0x2c31a0], pressure_requests = rig.calls[0x2c3438];
    CHECK(rig.Boundary() != 0);
    CHECK(rig.calls[0x2c31a0] == alignments && rig.calls[0x2c3438] == pressure_requests);
    CHECK(rig.input.Sequence() == captured.sequence + 1);
    rig.Call(kDestroy, kInfo);
    CHECK(!rig.allocated && !rig.input.IsOpen(0, 0) && rig.motors.big == 0);
    CHECK(rig.calls.count(0x177168) == 0);
}
void PendingSetupAndReconnect() {
    Rig rig;
    current = &rig;
    CHECK(rig.Call(kCreate, 0) == static_cast<int32_t>(kInfo));
    rig.Boundary(); // mode identification
    rig.Boundary(); // actual caller submits actuator alignment
    CHECK(rig.input.Capture().pads[0].pending == Input::Command::Align);
    CHECK(rig.Word(kInfo + 0x508) == 5);
    const auto state = rig.input.Capture();
    const auto game = rig.ram;
    rig.Boundary();
    CHECK(rig.input.GetRequest(0, 0) == Input::Request::Complete);
    rig.ram = game;
    CHECK(rig.input.Restore(state, 0));
    rig.tick = 0;
    rig.Boundary();
    CHECK(rig.Word(kInfo + 0x508) == 5 && rig.input.GetRequest(0, 0) == Input::Request::Busy);
    for (unsigned i = 0; i < 16 && rig.adapter.Counts().reports == 0; ++i) rig.Boundary();
    CHECK(rig.adapter.Counts().reports > 0 && rig.calls[0x2c31a0] == 1);
    rig.samples[0].connected = false;
    CHECK(rig.Boundary() == 0 && rig.Word(kGp - 0x6ddc) == 60);
    const auto before = rig.adapter.Counts().reports;
    rig.samples[0].connected = true;
    for (unsigned i = 0; i < 60; ++i) {
        CHECK(rig.Boundary() == 0);
        CHECK(rig.adapter.Counts().reports == before);
    }
    CHECK(rig.Word(kGp - 0x6ddc) == 0);
    for (unsigned i = 0; i < 16 && rig.adapter.Counts().reports == before; ++i) rig.Boundary();
    CHECK(rig.adapter.Counts().reports > before);
    CHECK(rig.calls[0x2c31a0] == 2 && rig.calls[0x2c3438] == 2);
    rig.Call(kDestroy, kInfo);
}
}
int main() {
    const int rounding = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    SetupAndReports();
    PendingSetupAndReconnect();
    std::fesetround(rounding);
    std::printf("InputCallers: %u checks, %u failures; four unchanged generated callers\n", checks, failures);
    return failures ? 1 : 0;
}
