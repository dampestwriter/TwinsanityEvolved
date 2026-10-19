#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class MathLibraryCollisionOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 16384> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };

    struct Entry {
        const char* name;
        uint32_t address;
    };
    static constexpr unsigned kEntryCount = 8;
    static constexpr unsigned kCasesPerEntry = 4000;
    static const std::array<Entry, kEntryCount>& Entries();
    static const char* SourceDigest();
    static void Invoke(uint32_t address, CpuContext& context, Memory& memory);
};
