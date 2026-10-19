#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

// Test-only data memory and entry metadata. Expected arithmetic is extracted
// from NEW's generated functions at build time, never from a golden dump or HLE.
class MathLibraryOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 4096> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };

    struct Entry {
        const char* name;
        uint32_t address;
        unsigned argument_layout;
    };

    static constexpr unsigned kCasesPerEntry = 4000;
    static constexpr unsigned kEntryCount = 13;

    // Defined by the generated oracle translation unit.
    static const std::array<Entry, kEntryCount>& Entries();
    static const char* SourceDigest();
    static void Invoke(uint32_t function, CpuContext& context, Memory& memory);
};
