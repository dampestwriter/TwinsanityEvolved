#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace title {

// The game's controller contract, sampled only by the game execution owner.
// No DMA buffers, PADMAN requests, wall clock or firmware state are involved.
class Input {
public:
    static constexpr unsigned kPorts = 2, kSlots = 4;
    enum class State : int32_t { Disconnected = 0, Finding = 1, Busy = 5, Stable = 6 };
    enum class Request : int32_t { Complete = 0, Failed = 1, Busy = 2 };
    struct Sample {
        bool connected = false;
        uint16_t buttons = 0;  // active high, SELECT bit 0 ... SQUARE bit 15
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
        bool has_pressure = false;
        // right, left, up, down, triangle, circle, cross, square, L1,R1,L2,R2
        std::array<uint8_t, 12> pressure{};
    };
    using Report = std::array<uint8_t, 32>;
    struct Reading {
        Sample sample;
        Report report{};
        State connection = State::Disconnected;
        uint64_t boundary = 0, sequence = 0;
        bool readable = false;
    };
    struct Motors { uint8_t small = 0, big = 0; };
    // The root queues this to the host input thread. False means unsupported
    // or disconnected; never call SDL from the game execution thread.
    using MotorOutput = std::function<bool(unsigned, unsigned, Motors)>;
    enum class Command : uint8_t { None, Mode, Align, Pressure };
    struct PadState {
        bool open = false;
        State connection = State::Disconnected;
        bool analog = true, pressure = false, locked = false;
        uint64_t command_at = 0;
        Request request = Request::Busy;
        Command pending = Command::None;
        bool next_analog = true, next_lock = false;
        std::array<uint8_t, 6> alignment{{0, 1, 255, 255, 255, 255}};
        std::array<uint8_t, 6> next_alignment{{0, 1, 255, 255, 255, 255}};
        Sample sample;
        Motors motors;
    };
    struct Snapshot {
        uint32_t version = 2;
        uint64_t boundary = 0, sequence = 1;
        bool sampled = false, paused = false;
        std::array<PadState, kPorts * kSlots> pads{};
    };
    // Cold-boot attribution record (run.meta), not serialized state: the
    // accepted boundary at which a port was last opened, first became Stable
    // and first completed its pressure setup (the title's SetupDone for a
    // DualShock 2, TR src/game/pads.cpp:402-536). `reached` false = never.
    struct Milestone { bool reached = false; uint64_t boundary = 0; };
    struct Milestones { Milestone opened, stable, setup; };

    explicit Input(MotorOutput motors = {});
    int32_t Open(unsigned port, unsigned slot);
    int32_t Close(unsigned port, unsigned slot);
    // Samples include every slot. Repeating a boundary does not resample.
    // A lower boundary is rejected; Restore is the explicit clock rebase.
    bool SampleBoundary(uint64_t boundary, const std::array<Sample, kPorts * kSlots>& samples);
    State GetState(unsigned port, unsigned slot) const;
    Request GetRequest(unsigned port, unsigned slot) const;
    // An owned, coherent view of the accepted boundary input and the report
    // Read serves. No read-count hold: physical edges belong to the sampler.
    // Call on the game owner, just like SampleBoundary and Read.
    Reading Current(unsigned port, unsigned slot) const;
    int32_t Read(unsigned port, unsigned slot, Report& report) const;
    int32_t InfoMode(unsigned port, unsigned slot, int32_t info, int32_t index) const;
    int32_t InfoActuator(unsigned port, unsigned slot, int32_t actuator, int32_t term) const;
    int32_t SetMode(unsigned port, unsigned slot, int32_t mode, int32_t lock);
    int32_t SetAlignment(unsigned port, unsigned slot, const std::array<uint8_t, 6>& alignment);
    int32_t EnterPressure(unsigned port, unsigned slot);
    int32_t SetMotors(unsigned port, unsigned slot, const std::array<uint8_t, 6>& values);
    bool IsOpen(unsigned port, unsigned slot) const;
    void Pause();
    void Resume();
    void Reset();
    Snapshot Capture() const { return state_; }
    static bool ValidateSnapshot(const Snapshot& snapshot, std::string* error = nullptr);
    // Restore on the original saved clock without resampling an accepted key
    // or postponing setup. Desired motors are reissued only on the next sample.
    bool RestoreSaved(const Snapshot& snapshot);
    // Explicit import into a different clock. Setup remains busy at the
    // rebased boundary, then completes on the next accepted input sample.
    bool Restore(const Snapshot& snapshot, uint64_t new_boundary);
    uint64_t Sequence() const { return state_.sequence; }
    uint64_t Boundary() const { return state_.boundary; }
    Milestones MilestonesOf(unsigned port, unsigned slot) const;
    static Report MakeReport(const Sample& sample, bool analog, bool pressure);

private:
    PadState* Pad(unsigned port, unsigned slot);
    const PadState* Pad(unsigned port, unsigned slot) const;
    bool Accept(PadState* pad, Command command);
    void Stop(unsigned index, bool clear);
    Snapshot state_;
    std::array<Milestones, kPorts * kSlots> milestones_{};
    MotorOutput output_;
    bool restore_motors_ = false;
};

}  // namespace title
