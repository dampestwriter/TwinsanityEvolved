#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

class MathLibraryPlanesOracle {
public:
    class Memory {
    public:
        Memory();
        uint8_t* Translate(uint32_t address, size_t size);
        std::vector<uint8_t> bytes;
    };
    static void Invoke(uint32_t address, CpuContext& context, Memory& memory);
    static std::array<uint64_t, 8> Counts();
    static const char* SourceDigest();
};
