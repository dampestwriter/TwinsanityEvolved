#pragma once

#include <array>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class VuUnit;

class MathLibraryJointAnimateOracle {
public:
    // 4 KiB of low memory plus the 1 KiB window holding the source default
    // scale constants at 0x3234f0, mapped through every KSEG alias.
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        static constexpr uint32_t kLow = 0x1000, kWindow = 0x323400, kWindowSize = 0x400;
        std::array<uint8_t, kLow + kWindowSize> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };
    // Actual NEW body executions, actual callee executions and actual original
    // joint program calls.
    struct Counters {
        uint64_t generated_bodies, decoder_calls, scale_calls, translate_calls;
        uint64_t euler_programs, turn_programs, interpolate_programs;
    };
    static void Invoke(CpuContext& context, Memory& memory, VuUnit& control, uint32_t entry = 0);
    // The unchanged NEW ReadJointAngle body: the selected adapter's delegation
    // target in this isolated oracle (the live table holds the installed binding).
    static void InvokeDecoder(CpuContext& context, Memory& memory, uint32_t entry);
    static Counters Counts();
    static const char* SourceDigest();
};
