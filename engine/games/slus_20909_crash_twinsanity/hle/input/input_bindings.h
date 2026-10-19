#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input.h"

namespace title {

// NTSC entry adapters for the calls made by UpdatePad (0x2b2558),
// CreateGamePadInfo (0x2b3890), DestroyGamePadInfo (0x2b3858), and
// SetPadMotors (0x2b3998). These replace the service at the game's actual
// imported boundary; they never call the SDK body or PadProtocol.
class InputBindings {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    struct Memory {
        std::function<bool(uint32_t, void*, size_t)> read;
        std::function<bool(uint32_t, const void*, size_t)> write;
    };
    struct Counters {
        uint64_t calls = 0, reports = 0, motor_calls = 0, bad_memory = 0;
    };
    InputBindings(Input& input, Memory memory);
    // Install AFTER the SDK manifest. False may leave an installed prefix;
    // the root must treat it as startup failure. Object lifetime >= dispatch.
    bool Install(const Installer& install);
    void Invoke(uint32_t function, CpuContext& context);
    Counters Counts() const { return counts_; }
    // Game memory an entry may read or stamp: main RAM (any segment) or the
    // scratchpad, where the title's main thread keeps its stack.
    static bool RamRange(uint32_t address, size_t bytes);

private:
    bool ReadBytes(uint32_t address, void* out, size_t bytes);
    bool WriteBytes(uint32_t address, const void* in, size_t bytes);
    Input& input_;
    Memory memory_;
    Counters counts_;
};

}  // namespace title
