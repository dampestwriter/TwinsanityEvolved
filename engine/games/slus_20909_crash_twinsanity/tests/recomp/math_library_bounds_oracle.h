#pragma once

#include <array>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class MathLibraryBoundsOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 4096> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };
    static void Invoke(uint32_t entry, CpuContext& context, Memory& memory, uint32_t dispatch = 0);
    static std::array<uint64_t, 3> Counts();
    static const char* SourceDigest();
};
