#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class MathLibraryAnimationOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 8192> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
        void Write16(uint32_t address, uint16_t value);
    };
    static const char* SourceDigest();
    static void Invoke(CpuContext& context, Memory& memory);
};
