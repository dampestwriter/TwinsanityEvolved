#pragma once

#include <array>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class VuUnit;

class MathLibraryJointTransformOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 4096> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };
    // Actual NEW body executions and actual original joint program calls.
    struct Counters {
        uint64_t generated_bodies, actual_bases, actual_scaled_bases, actual_parent_products;
    };
    static void Invoke(CpuContext& context, Memory& memory, VuUnit& control, uint32_t entry = 0);
    static Counters Counts();
    static const char* SourceDigest();
};
