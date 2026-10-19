#include "games/slus_20909_crash_twinsanity/hle/input/input_bindings.h"

#include <stdexcept>
#include <utility>

#include "engine/ee/mem/scratchpad.h"

namespace title {
namespace {
// sdk_bindings.tsv and NTSC shard_029/shard_030. The title's platform
// wrappers use these SDK entries directly; source: platform/pads.h and
// game/pads.cpp. No PAL offset is applied at runtime.
constexpr uint32_t kEntries[] = {
    0x2c2428, 0x2c25b0, 0x2c27c0, 0x2c29a8, 0x2c2bb8, 0x2c2c30,
    0x2c2d68, 0x2c2dc8, 0x2c2ee8, 0x2c3020, 0x2c30d8, 0x2c31a0,
    0x2c3278, 0x2c3328, 0x2c33d8, 0x2c3438, 0x2c34a8,
};
uint32_t Arg(const CpuContext& c, unsigned i) { return c.cpuRegs.GPR.r[4 + i].UL[0]; }
void Return(CpuContext& c, int32_t value) { c.cpuRegs.GPR.r[2].SD[0] = value; }
}
InputBindings::InputBindings(Input& input, Memory memory) : input_(input), memory_(std::move(memory)) {}
// Game memory the entries read or stamp: main RAM in any segment, and the
// scratchpad -- the title's main thread runs on the scratchpad (TR
// src/platform/ps2/entry.cpp:8-9,83), so a stack buffer is a scratchpad
// address. Neither is a model of the hardware; both are where the game keeps
// the bytes it hands these entries.
bool InputBindings::RamRange(uint32_t address, size_t bytes) {
    if (address == 0) return false;
    if (Scratchpad::Contains(address)) return bytes <= Scratchpad::kSize - Scratchpad::Offset(address);
    const uint32_t physical = address & 0x1fffffffu;
    return physical < 0x02000000u && bytes <= 0x02000000u - physical;
}
bool InputBindings::ReadBytes(uint32_t address, void* out, size_t bytes) {
    if (RamRange(address, bytes) && memory_.read && memory_.read(address, out, bytes)) return true;
    ++counts_.bad_memory;
    return false;
}
bool InputBindings::WriteBytes(uint32_t address, const void* in, size_t bytes) {
    if (RamRange(address, bytes) && memory_.write && memory_.write(address, in, bytes)) return true;
    ++counts_.bad_memory;
    return false;
}
bool InputBindings::Install(const Installer& install) {
    if (!install || !memory_.read || !memory_.write) return false;
    for (uint32_t function : kEntries) {
        if (!install(function, [this, function](CpuContext& c, uint32_t entry) {
            // A saved interior SDK frame cannot be resumed under a different
            // owner. Reject explicitly instead of falling into pad firmware.
            if (entry != 0 && entry != function) throw std::runtime_error("Input: obsolete interior SDK continuation");
            Invoke(function, c);
        })) return false;
    }
    return true;
}
void InputBindings::Invoke(uint32_t function, CpuContext& c) {
    ++counts_.calls;
    const unsigned port = Arg(c, 0), slot = Arg(c, 1);
    int32_t result = 0;
    switch (function) {
        case 0x2c2428:  // pad initialise; no service module registration
        case 0x2c25b0: result = 1; break;
        case 0x2c27c0: // The caller owns driverMemory, but Input never stamps it.
            if (RamRange(Arg(c, 2), 256)) result = input_.Open(port, slot);
            break;
        case 0x2c29a8: result = input_.Close(port, slot); break;
        case 0x2c2bb8: {
            Input::Report report{};
            result = input_.Read(port, slot, report);
            if (result && !WriteBytes(Arg(c, 2), report.data(), report.size())) result = 0;
            if (result) ++counts_.reports;
            break;
        }
        case 0x2c2c30:
            result = input_.IsOpen(port, slot) ? static_cast<int32_t>(input_.GetState(port, slot)) : 0x63;
            break;
        case 0x2c2d68: result = static_cast<int32_t>(input_.GetRequest(port, slot)); break;
        case 0x2c2dc8:
            result = input_.InfoActuator(port, slot, static_cast<int32_t>(Arg(c, 2)), static_cast<int32_t>(Arg(c, 3)));
            break;
        case 0x2c2ee8:
            result = input_.InfoMode(port, slot, static_cast<int32_t>(Arg(c, 2)), static_cast<int32_t>(Arg(c, 3)));
            break;
        case 0x2c3020: result = input_.SetMode(port, slot, Arg(c, 2), Arg(c, 3)); break;
        case 0x2c30d8:
        case 0x2c31a0: {
            std::array<uint8_t, 6> data{};
            if (ReadBytes(Arg(c, 2), data.data(), data.size())) {
                if (function == 0x2c30d8) {
                    ++counts_.motor_calls;
                    result = input_.SetMotors(port, slot, data);
                } else result = input_.SetAlignment(port, slot, data);
            }
            break;
        }
        case 0x2c3278: result = input_.InfoMode(port, slot, 1, 0) ? 0x3ffff : 0; break;
        case 0x2c33d8: result = input_.InfoMode(port, slot, 1, 0) == 7 ? 1 : 0; break;
        case 0x2c3328: // SetButtonInfo is EnterPressureMode's complete request.
        case 0x2c3438: result = input_.EnterPressure(port, slot); break;
        case 0x2c34a8: result = 0x300; break;
        default: throw std::invalid_argument("Input: unregistered entry");
    }
    Return(c, result);
}

}  // namespace title
